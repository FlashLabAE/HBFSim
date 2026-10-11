#include "physical/simulation_session.hpp"

#include <cmath>
#include <limits>

namespace hbfsim::physical {

void SimulationSession::peer_event(double at, std::function<void()> action) {
    if (!std::isfinite(at) || at < peer_clock_ns_)
        throw std::runtime_error("peer controller event moved backwards in time");
    peer_events_.push({at, peer_sequence_++, std::move(action)});
}

void SimulationSession::issue_peer_io(host::PeerIo request, double at,
        host::PeerMemoryController::Done done) {
    peer_event(at, [this, request, at, done=std::move(done)]() mutable {
        using host::PeerIoTarget;
        if (request.background && request.target == PeerIoTarget::Hbf &&
            hbf_->execution_stats().finish_ns > at) {
            issue_peer_io(request,hbf_->execution_stats().finish_ns,std::move(done));
            return;
        }
        const auto id = "peer/io/" + std::to_string(peer_sequence_++);
        if (request.target == PeerIoTarget::Hbm) {
            const auto ticket = hbm_->enqueue({.id=id, .tier=Tier::HBM, .op=request.op,
                .address_space=AddressSpace::Physical, .trace=config_.trace, .arrival_ns=at,
                .addr=request.address, .bytes=request.bytes});
            peer_hbm_.emplace(ticket, std::move(done));
            return;
        }
        double finish = at;
        if (request.target == PeerIoTarget::Hbf) {
            const auto completion = hbf_->issue({.id=id, .tier=Tier::HBF, .op=request.op,
                .address_space=AddressSpace::Logical, .trace=config_.trace, .arrival_ns=at,
                .addr=request.address, .bytes=request.bytes,
                .heatmap_source=request.background ? HeatmapTrafficSource::Maintenance : HeatmapTrafficSource::Direct});
            finish = completion.finish_ns;
            if (request.op == Op::Read) {
                ++peer_hbf_read_requests_;
                const auto page = config_.hbf.device.page_size_bytes;
                peer_hbf_read_pages_ += (request.address%page+request.bytes+page-1)/page;
            }
        } else if (request.target == PeerIoTarget::Invalidate) {
            const auto page = config_.hbf.device.page_size_bytes;
            finish = hbf_->invalidate_logical_pages(request.address/page, request.bytes/page, at,
                host::HbfInvalidationOrder::CompletedRange).completion.finish_ns;
        } else {
            const auto page = config_.hbf.device.page_size_bytes;
            std::vector<std::uint64_t> counts(config_.hbf.device.stacks);
            for (auto cursor=request.address; cursor<request.address+request.bytes;) {
                const auto take=std::min(page-cursor%page, request.address+request.bytes-cursor);
                counts.at(hbf_->stack_for_logical_page(cursor/page)) += take;
                cursor += take;
            }
            for (std::size_t stack=0; stack<counts.size(); ++stack) {
                if (!counts[stack]) continue;
                finish=std::max(finish,base_die_links_[stack].issue(id+"/"+std::to_string(stack),
                    request.op,at,counts[stack],config_.trace).finish_ns);
            }
        }
        issued_work_frontier_ns_=std::max(issued_work_frontier_ns_,finish);
        peer_event(finish,[done=std::move(done),finish] { done(finish); });
    });
}

void SimulationSession::finish_peer_hbm(std::uint64_t ticket, const PhysicalCompletion& completion) {
    auto done=std::move(peer_hbm_.at(ticket));
    peer_hbm_.erase(ticket);
    issued_work_frontier_ns_=std::max(issued_work_frontier_ns_,completion.finish_ns);
    peer_event(completion.finish_ns,[done=std::move(done),finish=completion.finish_ns] { done(finish); });
}

// Command boundaries finish already-started copies, without starting a new
// background drain or treating still-live HBM data as persistent flash data.
void SimulationSession::drive_peer(const std::function<bool()>& finished) {
    while (!finished()) {
        for (auto& [ticket, completion] : hbm_->take_completions()) finish_peer_hbm(ticket,completion);
        if (finished()) break;
        const auto next=peer_events_.empty() ? std::numeric_limits<double>::infinity() : peer_events_.top().at;
        if (!peer_hbm_.empty() && hbm_->service_before(next)) continue;
        if (peer_events_.empty()) throw std::runtime_error("peer operation lost its physical completion");
        auto event=peer_events_.top(); peer_events_.pop();
        peer_clock_ns_=event.at;
        hbm_->advance_buffer_frontier(event.at);
        event.action();
    }
}

void SimulationSession::configure_peer_memory(host::PeerMemoryLayout layout) {
    if (peer_ || next_sequence_ || failed_ || drained_ || !hbm_ || !hbf_ ||
        config_.initial_hbf_logical_pages || config_.initial_hbf_persistent_image ||
        layout.hbm_bytes>hbm_->application_capacity_bytes() || config_.hbf.device.page_size_bytes!=4096)
        throw SimulationInputError("peer configuration requires a fresh HBM/HBF session and its application budget");
    // Construct before mutating native media so malformed layouts are retryable.
    std::unique_ptr<host::PeerMemoryController> controller;
    try {
        controller=std::make_unique<host::PeerMemoryController>(std::move(layout),config_.hbf.host.peer_writeback,
            hbf_logical_capacity_pages_*config_.hbf.device.page_size_bytes,
            [this](host::PeerIo io,double at,host::PeerMemoryController::Done done) {
                issue_peer_io(io,at,std::move(done));
            });
    } catch (const std::runtime_error& error) { throw SimulationInputError(error.what()); }
    peer_=std::move(controller);
    const auto pages=peer_->initial_hbf_bytes()/4096;
    if (pages) hbf_->prepopulate_mutable_logical_page_range(0,pages);
    // Unwritten pieces of a coarse initial home are not valid flash pages.
    for (const auto& [address,bytes] : peer_->initial_hbf_holes()) {
        const auto result=hbf_->invalidate_logical_pages(address/4096,bytes/4096,issued_work_frontier_ns_);
        issued_work_frontier_ns_=std::max(issued_work_frontier_ns_,result.completion.finish_ns);
    }
    completed_frontier_ns_=issued_work_frontier_ns_;
    peer_clock_ns_=completed_frontier_ns_;
}

double SimulationSession::release_peer_memory(std::uint64_t address,std::uint64_t bytes) {
    if (!peer_ || failed_ || drained_) throw SimulationInputError("peer release needs an active peer controller");
    try { peer_->validate_access(Op::Write,address,bytes); }
    catch (const std::runtime_error& error) { throw SimulationInputError(error.what()); }
    if (address%4096 || bytes%4096) throw SimulationInputError("peer release is not page aligned");
    bool completed=false;
    double finish=completed_frontier_ns_;
    peer_event(completed_frontier_ns_,[&,this] {
        peer_->release(address,bytes,completed_frontier_ns_,[&](double end) { finish=end; completed=true; });
    });
    drive_peer([&] { return completed; });
    completed_frontier_ns_=std::max(completed_frontier_ns_,finish);
    issued_work_frontier_ns_=std::max(issued_work_frontier_ns_,finish);
    return finish;
}
} // namespace hbfsim::physical

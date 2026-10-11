#include "host/peer_memory.hpp"

#include <algorithm>
#include <cmath>
#include <iomanip>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace hbfsim::host {
using physical::Op;

std::uint64_t PeerMemoryController::size(const std::vector<Range>& ranges) {
    std::uint64_t bytes = 0;
    for (const auto& [begin, end] : ranges) bytes += end - begin;
    return bytes;
}

void PeerMemoryController::add_range(std::vector<Range>& ranges, Range range) {
    if (range.first >= range.second) return;
    ranges.push_back(range);
    std::sort(ranges.begin(), ranges.end());
    std::size_t count = 0;
    for (const auto& current : ranges) {
        if (count && current.first <= ranges[count-1].second)
            ranges[count-1].second = std::max(ranges[count-1].second, current.second);
        else ranges[count++] = current;
    }
    ranges.resize(count);
}

bool PeerMemoryController::covers(const std::vector<Range>& ranges, Range range) {
    return std::any_of(ranges.begin(), ranges.end(), [&](const Range& valid) {
        return valid.first <= range.first && valid.second >= range.second;
    });
}

PeerMemoryController::PeerMemoryController(PeerMemoryLayout layout, PeerWritebackConfig policy,
        std::uint64_t hbf_bytes, Issue issue)
    : layout_(std::move(layout)), policy_(policy), issue_(std::move(issue)) {
    if (!layout_.unit_bytes || layout_.unit_bytes % 4096 || !layout_.transfer_bytes ||
        layout_.transfer_bytes % 4096 || layout_.unit_bytes % layout_.transfer_bytes ||
        !layout_.hbm_bytes || layout_.hbm_bytes % layout_.unit_bytes ||
        layout_.kv_begin % layout_.unit_bytes || layout_.kv_begin > layout_.logical_bytes ||
        !std::isfinite(policy_.cold_ns) || policy_.cold_ns < 0 ||
        !std::isfinite(policy_.high_watermark) || !std::isfinite(policy_.low_watermark) ||
        policy_.low_watermark < 0 || policy_.low_watermark >= policy_.high_watermark ||
        policy_.high_watermark > 1)
        throw std::runtime_error("invalid peer memory layout or writeback policy");
    hbm_slots_ = layout_.hbm_bytes / layout_.unit_bytes;
    hbf_slots_ = hbf_bytes / layout_.unit_bytes;
    for (const auto& [begin, end] : layout_.initialized) {
        if (begin >= end || end > layout_.logical_bytes)
            throw std::runtime_error("peer initialization escapes logical memory");
        for (auto cursor = begin; cursor < end;) {
            const auto id = cursor / layout_.unit_bytes;
            const auto base = id * layout_.unit_bytes;
            const auto stop = std::min(end, base + layout_.unit_bytes);
            auto& unit = units_[id];
            if (!unit) {
                unit = std::make_shared<Unit>();
                unit->id = id;
                unit->weight = base < layout_.kv_begin;
            }
            add_range(unit->valid, {cursor-base, stop-base});
            cursor = stop;
        }
    }
    if (units_.size() > hbm_slots_ + hbf_slots_)
        throw std::runtime_error("initialized peer data exceeds HBM plus usable HBF capacity");
    // Only produced data claims capacity. The priority class takes HBM first;
    // the other class immediately uses all the space it leaves.
    for (const bool preferred : {true, false}) {
        for (const auto& [id, unit] : units_) {
            (void)id;
            if ((unit->weight != layout_.kv_first) != preferred) continue;
            const bool hbm = has_slot(true);
            home(unit, hbm, take_slot(hbm));
            stats_.live_bytes += size(unit->valid);
        }
    }
}

bool PeerMemoryController::has_slot(bool hbm) const {
    return hbm ? (!free_hbm_.empty() || next_hbm_ < hbm_slots_)
               : (!free_hbf_.empty() || next_hbf_ < hbf_slots_);
}

std::uint64_t PeerMemoryController::take_slot(bool hbm) {
    auto& free = hbm ? free_hbm_ : free_hbf_;
    if (!free.empty()) { const auto slot = free.back(); free.pop_back(); return slot; }
    auto& next = hbm ? next_hbm_ : next_hbf_;
    if (next >= (hbm ? hbm_slots_ : hbf_slots_))
        throw std::runtime_error("peer memory has no physical slot for live data");
    return next++;
}

void PeerMemoryController::home(const std::shared_ptr<Unit>& unit, bool hbm, std::uint64_t slot) {
    unit->hbm = hbm; unit->slot = slot; unit->homed = true;
    if (hbm) {
        auto& lru = unit->weight ? weight_lru_ : kv_lru_;
        lru.push_back(unit->id); unit->lru = std::prev(lru.end());
        ++stats_.hbm_units;
        (unit->weight ? stats_.hbm_weight_bytes : stats_.hbm_kv_bytes) += size(unit->valid);
    } else ++stats_.hbf_units;
}

void PeerMemoryController::unhome(const std::shared_ptr<Unit>& unit) {
    if (!unit->homed) return;
    if (unit->hbm) {
        (unit->weight ? weight_lru_ : kv_lru_).erase(unit->lru);
        free_hbm_.push_back(unit->slot); --stats_.hbm_units;
        (unit->weight ? stats_.hbm_weight_bytes : stats_.hbm_kv_bytes) -= size(unit->valid);
    } else { free_hbf_.push_back(unit->slot); --stats_.hbf_units; }
    unit->homed = false;
}

void PeerMemoryController::touch(const std::shared_ptr<Unit>& unit, double at) {
    unit->touched = std::max(unit->touched, at);
    if (unit->homed && unit->hbm) {
        auto& lru = unit->weight ? weight_lru_ : kv_lru_;
        lru.splice(lru.end(), lru, unit->lru);
    }
}

std::shared_ptr<PeerMemoryController::Unit> PeerMemoryController::victim(
        const std::list<std::uint64_t>& lru) const {
    for (const auto id : lru) {
        const auto& unit = units_.at(id);
        if (!unit->busy && unit->work.empty()) return unit;
    }
    return {};
}

void PeerMemoryController::validate_access(Op op, std::uint64_t address, std::uint64_t bytes) const {
    if (!bytes || address >= layout_.logical_bytes || bytes > layout_.logical_bytes-address ||
        (op != Op::Read && op != Op::Write) || (op == Op::Write && address < layout_.kv_begin))
        throw std::runtime_error("peer access escapes data or mutates immutable weights");
}

void PeerMemoryController::access(Op op, std::uint64_t address, std::uint64_t bytes,
        double at, Done done) {
    validate_access(op, address, bytes);
    struct Group { std::size_t left; double finish; Done done; };
    auto group = std::make_shared<Group>(Group{
        static_cast<std::size_t>((address % layout_.unit_bytes + bytes-1) / layout_.unit_bytes + 1), at, std::move(done)});
    for (auto cursor = address; cursor < address+bytes;) {
        const auto id = cursor / layout_.unit_bytes, base = id * layout_.unit_bytes;
        const auto stop = std::min(address+bytes, base+layout_.unit_bytes);
        auto& unit = units_[id];
        if (!unit) {
            unit = std::make_shared<Unit>(); unit->id = id; unit->weight = base < layout_.kv_begin;
        }
        touch(unit, at);
        unit->work.push_back({op, cursor-base, stop-base, at, [group](double finish) {
            group->finish = std::max(group->finish, finish);
            if (!--group->left) group->done(group->finish);
        }});
        pump(unit, at);
        cursor = stop;
    }
}

void PeerMemoryController::pump(const std::shared_ptr<Unit>& unit, double at) {
    if (unit->busy || unit->work.empty()) return;
    const auto work = unit->work.front();
    at = std::max(at, work.at);
    if (work.release) {
        unit->busy = true;
        auto finish = [this, unit, work](double end) {
            const auto before = size(unit->valid);
            std::vector<Range> kept;
            for (const auto& [begin, stop] : unit->valid) {
                if (begin < work.begin) add_range(kept, {begin, std::min(stop, work.begin)});
                if (stop > work.end) add_range(kept, {std::max(begin, work.end), stop});
            }
            const auto removed = before - size(kept);
            stats_.released_bytes += removed; stats_.live_bytes -= removed;
            if (unit->homed && unit->hbm) {
                stats_.released_hbm_bytes += removed;
                (unit->weight ? stats_.hbm_weight_bytes : stats_.hbm_kv_bytes) -= removed;
            }
            unit->valid = std::move(kept);
            if (unit->valid.empty()) unhome(unit);
            unit->work.pop_front(); unit->busy = false;
            work.done(end); pump(unit, end); wake(end);
            if (!unit->homed && unit->work.empty()) units_.erase(unit->id);
        };
        if (unit->homed && !unit->hbm)
            issue_({PeerIoTarget::Invalidate, Op::Write, unit->slot*layout_.unit_bytes+work.begin,
                    work.end-work.begin}, at, std::move(finish));
        else finish(at);
        return;
    }
    if (work.op == Op::Read && !covers(unit->valid, {work.begin, work.end}))
        throw std::runtime_error("peer read of uninitialized or released data");
    if (!unit->homed) {
        if (has_slot(true)) home(unit, true, take_slot(true));
        else if (!layout_.kv_first && kv_lru_.empty()) home(unit, false, take_slot(false));
        else {
            if (!unit->waiting) { waiting_.push_back(unit); unit->waiting = true; }
            if (waiting_.size() <= pending_hbm_frees_) return;
            auto old = layout_.kv_first ? victim(weight_lru_) : nullptr;
            if (!old) old = victim(kv_lru_);
            if (old && has_slot(false)) move(old, false, false, at);
            else if (!has_slot(false) && !stats_.migrating_units)
                throw std::runtime_error("peer live data exhausts physical memory");
            return;
        }
    } else if (!unit->hbm && has_slot(true)) {
        // A real access can use spare HBM. There is no speculative workload
        // hotset, and no forced promotion that displaces another live unit.
        move(unit, true, false, at);
        return;
    }
    touch(unit, at); unit->busy = true;
    const auto count = work.end-work.begin;
    if (unit->weight) (unit->hbm ? stats_.weight_hbm_read_bytes : stats_.weight_hbf_read_bytes) += count;
    else if (work.op == Op::Read) (unit->hbm ? stats_.kv_hbm_read_bytes : stats_.kv_hbf_read_bytes) += count;
    else (unit->hbm ? stats_.kv_hbm_write_bytes : stats_.kv_hbf_write_bytes) += count;
    issue_({unit->hbm ? PeerIoTarget::Hbm : PeerIoTarget::Hbf, work.op,
            unit->slot*layout_.unit_bytes+work.begin, count}, at,
        [this, unit, work](double finish) {
            if (work.op == Op::Write) {
                const auto before = size(unit->valid);
                add_range(unit->valid, {work.begin, work.end});
                const auto added = size(unit->valid)-before;
                stats_.live_bytes += added;
                if (unit->hbm) stats_.hbm_kv_bytes += added;
            }
            unit->work.pop_front(); unit->busy = false;
            work.done(finish); pump(unit, finish); wake(finish);
        });
}

void PeerMemoryController::wake(double at) {
    if (waking_) return;
    waking_ = true;
    auto pending = std::move(waiting_); waiting_.clear();
    for (const auto& unit : pending) { unit->waiting = false; pump(unit, at); }
    waking_ = false;
}

void PeerMemoryController::copy(const std::shared_ptr<Unit>& unit, bool to_hbm, bool background,
        std::uint64_t destination, double at, Done done) {
    // Copy only initialized bytes, in bounded transfer fragments. The native
    // session charges the source, actual stack links, destination, and FTL.
    struct Copy { std::vector<Range> parts; std::size_t next = 0; Done done; };
    auto state = std::make_shared<Copy>(); state->done = std::move(done);
    for (const auto& [begin, end] : unit->valid)
        for (auto cursor = begin; cursor < end;) {
            const auto stop = std::min(end, cursor+layout_.transfer_bytes);
            state->parts.emplace_back(cursor, stop); cursor = stop;
        }
    auto step = std::make_shared<Done>();
    std::weak_ptr<Done> weak_step = step;
    *step = [this, unit, to_hbm, background, destination, state, weak_step](double time) {
        if (state->next == state->parts.size()) {
            state->done(time);
            return;
        }
        const auto next = weak_step.lock();
        const auto [begin, end] = state->parts[state->next++];
        const auto source = unit->slot*layout_.unit_bytes+begin;
        const auto target = destination*layout_.unit_bytes+begin;
        issue_({to_hbm ? PeerIoTarget::Hbf : PeerIoTarget::Hbm, Op::Read, source, end-begin, background}, time,
            [this, to_hbm, background, source, target, begin, end, next](double read) {
                issue_({to_hbm ? PeerIoTarget::ToHbm : PeerIoTarget::ToHbf,
                        to_hbm ? Op::Read : Op::Write, to_hbm ? source : target, end-begin, background}, read,
                    [this, to_hbm, background, target, begin, end, next](double transferred) {
                        issue_({to_hbm ? PeerIoTarget::Hbm : PeerIoTarget::Hbf, Op::Write,
                                target, end-begin, background}, transferred,
                            [next](double finish) { (*next)(finish); });
                    });
            });
    };
    (*step)(at);
}

void PeerMemoryController::move(const std::shared_ptr<Unit>& unit, bool to_hbm, bool background,
        double at, Done done) {
    const auto destination = take_slot(to_hbm);
    unit->busy = true; ++stats_.migrating_units;
    if (!to_hbm) ++pending_hbm_frees_;
    const auto bytes = size(unit->valid);
    if (to_hbm) { ++stats_.promotions; stats_.promoted_bytes += bytes; }
    else if (background) {
        ++background_active_; ++stats_.background_demotions; stats_.background_bytes += bytes;
        ++stats_.idle_starts; stats_.last_background_start_ns = at;
        stats_.last_background_address = unit->id*layout_.unit_bytes;
    } else { ++stats_.forced_demotions; stats_.forced_bytes += bytes; }
    copy(unit, to_hbm, background, destination, at,
        [this, unit, to_hbm, background, destination, done=std::move(done)](double finish) {
            auto commit = [this, unit, to_hbm, background, destination, done](double time) {
                unhome(unit); home(unit, to_hbm, destination);
                --stats_.migrating_units; if (background) --background_active_;
                if (!to_hbm) --pending_hbm_frees_;
                unit->busy = false;
                if (done) done(time);
                pump(unit, time); wake(time);
            };
            if (to_hbm)
                issue_({PeerIoTarget::Invalidate, Op::Write, unit->slot*layout_.unit_bytes,
                        layout_.unit_bytes}, finish, std::move(commit));
            else commit(finish);
        });
}

std::optional<double> PeerMemoryController::background_ready(double at) {
    if (!policy_.enabled || background_active_ || !has_slot(false)) return {};
    // Pressure is measured against the KV budget. Spare HBM occupied by
    // opportunistic weights is not evidence that KV needs to be written out.
    const auto budget=layout_.kv_first ? hbm_slots_ : hbm_slots_-weight_lru_.size();
    if (!budget) return {};
    const auto used=static_cast<double>(kv_lru_.size())/budget;
    if (used <= policy_.low_watermark) { background_pressure_=false; return {}; }
    if (used < policy_.high_watermark && !background_pressure_) return {};
    if (layout_.kv_first && !weight_lru_.empty()) return {};
    const auto old = victim(kv_lru_);
    if (!old) return {};
    return std::max(at, old->touched+policy_.cold_ns);
}

bool PeerMemoryController::background(double at) {
    const auto ready = background_ready(at);
    if (!ready || *ready > at) return false;
    background_pressure_ = true;
    move(victim(kv_lru_), false, true, at);
    return true;
}

void PeerMemoryController::release(std::uint64_t address, std::uint64_t bytes, double at, Done done) {
    if (address < layout_.kv_begin || address % 4096 || bytes % 4096)
        throw std::runtime_error("peer release must cover complete KV pages");
    validate_access(Op::Write, address, bytes);
    struct Group { std::size_t left; double finish; Done done; };
    auto group = std::make_shared<Group>(Group{
        static_cast<std::size_t>((address%layout_.unit_bytes+bytes-1)/layout_.unit_bytes+1), at, std::move(done)});
    for (auto cursor = address; cursor < address+bytes;) {
        const auto id = cursor/layout_.unit_bytes, base = id*layout_.unit_bytes;
        const auto stop = std::min(address+bytes, base+layout_.unit_bytes);
        auto complete = [group](double end) {
            group->finish = std::max(group->finish,end);
            if (!--group->left) group->done(group->finish);
        };
        const auto found = units_.find(id);
        if (found == units_.end()) complete(at);
        else {
            found->second->work.push_back({Op::Write,cursor-base,stop-base,at,std::move(complete),true});
            pump(found->second,at);
        }
        cursor = stop;
    }
}

std::vector<std::pair<std::uint64_t,std::uint64_t>> PeerMemoryController::initial_hbf_holes() const {
    std::vector<Range> holes;
    for (const auto& [id, unit] : units_) {
        (void)id;
        if (unit->hbm) continue;
        std::uint64_t cursor = 0;
        for (const auto& [begin,end] : unit->valid) {
            const auto first = begin/4096*4096;
            if (cursor < first) holes.emplace_back(unit->slot*layout_.unit_bytes+cursor, first-cursor);
            cursor = (end+4095)/4096*4096;
        }
        if (cursor < layout_.unit_bytes)
            holes.emplace_back(unit->slot*layout_.unit_bytes+cursor, layout_.unit_bytes-cursor);
    }
    return holes;
}

std::string PeerMemoryController::snapshot_json() const {
    std::ostringstream out; out << std::setprecision(17) << "{\"policy\":\""
        << (layout_.kv_first ? "kv_first" : "weights_first") << "\",\"unit_bytes\":" << layout_.unit_bytes
        << ",\"hbm_capacity_bytes\":" << layout_.hbm_bytes
        << ",\"hbm_reserved_units\":" << next_hbm_-free_hbm_.size()
        << ",\"hbf_reserved_units\":" << next_hbf_-free_hbf_.size()
        << ",\"background_enabled\":" << (policy_.enabled ? "true" : "false")
        << ",\"high_watermark\":" << policy_.high_watermark << ",\"low_watermark\":" << policy_.low_watermark
        << ",\"cold_ns\":" << policy_.cold_ns;
#define PEER_STAT(name) out << ",\"" #name "\":" << stats_.name
    PEER_STAT(hbm_units); PEER_STAT(hbf_units); PEER_STAT(migrating_units);
    PEER_STAT(hbm_weight_bytes); PEER_STAT(hbm_kv_bytes); PEER_STAT(live_bytes);
    PEER_STAT(background_demotions); PEER_STAT(forced_demotions); PEER_STAT(promotions);
    PEER_STAT(background_bytes); PEER_STAT(forced_bytes); PEER_STAT(promoted_bytes);
    PEER_STAT(released_bytes); PEER_STAT(released_hbm_bytes);
    PEER_STAT(weight_hbm_read_bytes); PEER_STAT(weight_hbf_read_bytes);
    PEER_STAT(kv_hbm_read_bytes); PEER_STAT(kv_hbf_read_bytes);
    PEER_STAT(kv_hbm_write_bytes); PEER_STAT(kv_hbf_write_bytes);
    PEER_STAT(idle_starts); PEER_STAT(last_background_start_ns); PEER_STAT(last_background_address);
#undef PEER_STAT
    out << '}'; return out.str();
}
} // namespace hbfsim::host

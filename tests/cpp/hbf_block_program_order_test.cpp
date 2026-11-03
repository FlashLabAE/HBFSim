#include "../../verification/probes/hbf_with_hbm.hpp"
#include "host/hbf_controller.hpp"
#include "physical/hbf/hbf_device.hpp"

#include <algorithm>
#include <cstdint>
#include <iostream>
#include <map>
#include <random>
#include <regex>
#include <stdexcept>
#include <string>

// OCP v0.7.0 sections 5.4.1 and 11.5.2.6: a NAND block is written page by
// page, and a page's write may not be initiated before its predecessor's.
// The controller handles requests in arrival order, yet a request can reach
// the media later than one handled after it (a read-modify-write, or a stall
// on garbage collection). A page allocated after another one in the same
// block must still program after it.

namespace {

using hbfsim::host::HbfConfig;
using hbfsim::physical::AddressSpace;
using hbfsim::physical::Op;
using hbfsim::physical::PhysicalCompletion;
using hbfsim::physical::PhysicalRequest;
using hbfsim::physical::Tier;
using hbfsim::physical::TraceConfig;
using hbfsim::physical::TraceMode;
using HbfController = hbfsim::verification::HbfWithHbm;

constexpr std::uint64_t kPage = 4096;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

HbfConfig geometry(std::uint32_t planes, std::uint32_t blocks_per_plane,
    std::uint32_t pages_per_block) {
    HbfConfig config;
    config.device.stacks = 1;
    config.device.channels_per_stack = 1;
    config.device.dies_per_channel = 1;
    config.device.planes_per_die = planes;
    config.device.blocks_per_plane = blocks_per_plane;
    config.device.pages_per_block = pages_per_block;
    config.device.page_size_bytes = kPage;
    config.device.oob_bytes_per_page = 0;
    config.host.mapping_entries_per_page = 512;
    config.host.write_coalescing_enabled = false;
    return config;
}

PhysicalRequest write(std::uint64_t addr, std::uint64_t bytes, double arrival_ns) {
    return PhysicalRequest{
        .id = "w",
        .tier = Tier::HBF,
        .op = Op::Write,
        .address_space = AddressSpace::Logical,
        .trace = TraceConfig{
            .mode = TraceMode::Off,
            .retain_completion_diagnostics = false,
        },
        .arrival_ns = arrival_ns,
        .addr = addr,
        .bytes = bytes,
    };
}

// The page a logical write programmed, from its "...->planeP/blockB/pageN"
// resource path. Blocks are plane-local, so the plane qualifies them.
struct Placement {
    std::uint64_t block = 0;
    std::uint64_t page = 0;
};

Placement placement(const PhysicalCompletion& completion) {
    static const std::regex pattern(R"(plane(\d+)/block(\d+)/page(\d+))");
    std::smatch match;
    require(std::regex_search(completion.resource_path, match, pattern),
        "write completion names no physical page: " + completion.resource_path);
    return {
        (std::stoull(match[1].str()) << 32) | std::stoull(match[2].str()),
        std::stoull(match[3].str()),
    };
}

// Four simultaneous writes alternate between two planes. The second rewrites
// the first one's LPN, so its page waits for that program to commit; the
// fourth takes the next page of the same block on the other plane, with no
// dependency of its own. Before the fix it programmed while that block was
// still to be erased for the delayed page zero.
void test_page_behind_delayed_predecessor_keeps_block_order() {
    const auto config = geometry(2, 16, 4);
    HbfController device(config);
    (void)device.issue(write(1 * kPage, kPage, 0.0));
    const auto delayed = device.issue(write(1 * kPage, kPage, 0.0));
    (void)device.issue(write(2 * kPage, kPage, 0.0));
    const auto trailing = device.issue(write(3 * kPage, kPage, 0.0));
    const auto earlier = placement(delayed);
    const auto later = placement(trailing);
    require(earlier.block == later.block && later.page == earlier.page + 1,
        "the delayed and trailing writes did not take consecutive pages of one block");
    require(trailing.finish_ns >
            delayed.finish_ns + 0.5 * config.device.t_program_page_ns,
        "a page programmed ahead of its predecessor in the same block");
    (void)device.drain_pending("drain", trailing.finish_ns);
    require(device.stats().accounting_verified && device.audit_snapshot().quiescent(),
        "the delayed-predecessor writes did not drain cleanly");
}

// Bursts of simultaneous overwrites at high occupancy: the first write of a
// burst that finds no free page waits for GC and opens the reclaimed block,
// and the rest of the burst, handled after it but issued earlier, writes the
// following pages. Every page of every block incarnation must still finish
// after the page before it, and never ahead of the block's erase.
void test_gc_stall_keeps_block_order_under_concurrency() {
    const auto config = geometry(2, 32, 16);
    HbfController device(config);
    const auto footprint = device.logical_capacity_pages() * 9 / 10;
    device.prepopulate_mutable_logical_page_range(0, footprint);
    std::mt19937_64 generator(7);
    struct Previous {
        std::uint64_t page = 0;
        double finish_ns = 0.0;
    };
    std::map<std::uint64_t, Previous> previous;
    std::uint64_t ordered_pairs = 0;
    double arrival_ns = 0.0;
    for (int burst = 0; burst < 400; ++burst) {
        double burst_finish_ns = arrival_ns;
        for (int index = 0; index < 8; ++index) {
            const auto lpn = generator() % footprint;
            const auto completion = device.issue(write(lpn * kPage, kPage, arrival_ns));
            const auto at = placement(completion);
            if (at.page != 0) {
                const auto found = previous.find(at.block);
                if (found != previous.end()) {
                    require(at.page == found->second.page + 1,
                        "a block was not allocated in page order");
                    require(completion.finish_ns > found->second.finish_ns,
                        "page " + std::to_string(at.page) + " of a block finished at " +
                        std::to_string(completion.finish_ns) + " ns, before page " +
                        std::to_string(found->second.page) + " at " +
                        std::to_string(found->second.finish_ns) + " ns");
                    ++ordered_pairs;
                }
            }
            previous[at.block] = {at.page, completion.finish_ns};
            burst_finish_ns = std::max(burst_finish_ns, completion.finish_ns);
        }
        arrival_ns = burst_finish_ns;
    }
    (void)device.drain_pending("drain", arrival_ns);
    const auto& stats = device.stats();
    require(stats.gc_runs > 0 && stats.gc_user_blocked_runs > 0,
        "the overwrite bursts never stalled on garbage collection");
    require(ordered_pairs > 0, "no block received consecutive pages");
    require(stats.accounting_verified && device.audit_snapshot().quiescent(),
        "the overwrite bursts did not drain cleanly");
    std::cout << "gc_stall gc_runs=" << stats.gc_runs
              << " blocked=" << stats.gc_user_blocked_runs
              << " ordered_pairs=" << ordered_pairs << '\n';
}

// The base die answers an out-of-order page with write-order status 0x6.
void test_device_rejects_page_programmed_before_predecessor() {
    hbfsim::physical::hbf::HbfDevice device(geometry(1, 4, 4).device);
    (void)device.prepare_program(0);
    device.complete_program(0, 1000.0);
    (void)device.prepare_program(1);
    bool rejected = false;
    try {
        device.complete_program(1, 900.0);
    } catch (const std::runtime_error& error) {
        rejected = std::string(error.what()).find("0x6") != std::string::npos;
    }
    require(rejected, "the device accepted a page programmed before its predecessor");
    device.complete_program(1, 1100.0);
}

} // namespace

int main() {
    try {
        test_page_behind_delayed_predecessor_keeps_block_order();
        test_gc_stall_keeps_block_order_under_concurrency();
        test_device_rejects_page_programmed_before_predecessor();
    } catch (const std::exception& error) {
        std::cerr << "hbf_block_program_order_test: " << error.what() << '\n';
        return 1;
    }
    std::cout << "hbf_block_program_order_test: ok\n";
    return 0;
}

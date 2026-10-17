// Opt-in in-flight same-page read sharing (hbf-read-buffer-coalescing):
// one array read per page burst, readiness, eviction, bank isolation and
// invalidation; the disabled option keeps the baseline miss path.
#include "../../verification/probes/hbf_with_hbm.hpp"
#include "physical/hbf/hbf_device.hpp"
#include <algorithm>
#include <cmath>
#include <iostream>
#include <stdexcept>

using namespace hbfsim;
using namespace physical;
using host::HbfConfig;
using verification::HbfWithHbm;

namespace {
void check(bool ok, const char* message) {
    if (!ok) throw std::runtime_error(message);
}
HbfConfig tiny() {
    HbfConfig c;
    c.device.stacks = c.device.channels_per_stack = c.device.dies_per_channel = 1;
    c.device.planes_per_die = 2;
    c.device.blocks_per_plane = 16; c.device.pages_per_block = 16;
    c.device.t_read_page_ns = 4000; c.device.t_program_page_ns = 75000;
    c.device.thermal_enabled = false;
    c.host.auto_gc_enabled = false; c.host.gc_reserved_free_blocks_per_plane = 0;
    c.host.mapping_mode = host::MappingMode::FullResident;
    c.host.read_buffer_coalescing_enabled = true;
    return c;
}
PhysicalRequest request(std::uint64_t address, double at = 0,
                        AddressSpace space = AddressSpace::Physical, std::uint64_t bytes = 64) {
    PhysicalRequest r;
    r.id = "inflight-page"; r.tier = Tier::HBF; r.op = Op::Read;
    r.address_space = space; r.addr = address; r.bytes = bytes; r.arrival_ns = at;
    r.trace.mode = TraceMode::Full;
    return r;
}

void pending_index_lifetime() {
    auto c = tiny(); hbf::HbfDevice d(c.device, true);
    check(!d.pending_cache_fill(0, 0), "absent page has a pending fill");
    d.cache_fill(0, 10); d.cache_fill(0, 30);
    check(d.pending_cache_fill(0, 0) == 10, "earliest fill not found");
    check(!d.cache_hit(0, 9), "pending fill became an immediate hit");
    check(d.pending_cache_fill(0, 10) == 30, "completed fill is still pending");
    d.advance_cache(10);
    check(d.pending_cache_fill(0, 11) == 30, "retiring old fill removed newer fill");
    d.cache_purge_page(0);
    check(!d.pending_cache_fill(0, 11), "invalidation reused the old in-flight page");
    d.advance_cache(31);
    check(!d.cache_hit(0, 31), "invalidated page resurrected");
    const auto other = std::uint64_t(c.device.blocks_per_plane) * c.device.pages_per_block;
    d.cache_fill(0, 40); d.cache_fill(other, 40);
    d.cache_purge_block(0);
    check(!d.pending_cache_fill(0, 32), "block purge left a joinable fill");
    check(d.pending_cache_fill(other, 32) == 40, "purge affected a different bank");
    d.advance_cache(40);
    check(!d.pending_cache_fill(other, 40) && d.cache_hit(other, 40),
        "completed fill was not retired into residency");

    hbf::HbfDevice protected_cache(c.device, true);
    protected_cache.cache_fill(0, 10); protected_cache.cache_fill(1, 20);
    check(protected_cache.cache_hit(0, 100), "failed to reserve future consumer");
    protected_cache.cache_fill(2, 30);
    check(!protected_cache.pending_cache_fill(2, 0), "bypassed fill entered pending index");
    protected_cache.advance_cache(100);
}

void same_page_burst(AddressSpace space, double spacing, bool local) {
    auto config = tiny();
    if (local) {
        // Device-local mapping records live in HBF device DRAM.
        config.host.mapping_placement = host::MappingPlacement::DeviceLocal;
        config.host.device_dram_capacity_denominator = 256;
    }
    HbfWithHbm h(config);
    if (space == AddressSpace::Logical) h.prepopulate_read_only_logical_page_range(0, 1);
    else h.reserve_static_physical_block_extent(0, 1);
    double first_done = 0, last_done = 0;
    double fill_ready = 0;
    unsigned waits = 0;
    for (unsigned i = 0; i < 64; ++i) {
        const auto done = h.issue(request(i * 64, i * spacing, space));
        if (i == 0) {
            first_done = done.finish_ns;
            for (const auto& span : done.spans)
                if (span.name.ends_with("/sram_stage_read")) fill_ready = span.end_ns;
            check(fill_ready > 4000, "no decoded fill completion diagnostic");
        } else {
            check(done.finish_ns >= fill_ready, "joined response preceded decoded data");
            check(done.breakdown.array_read_ns == 0 && done.breakdown.ecc_latency_ns == 0,
                "join charged another sense or ECC decode");
            for (const auto& span : done.spans)
                waits += span.name == "wait_inflight_read_buffer_fill";
        }
        last_done = std::max(last_done, done.finish_ns);
    }
    auto s = h.stats();
    check(s.read_requests == 64 && s.page_reads == 1 && s.ecc_decode_ops == 1,
        "cold same-page burst did not share one physical read");
    check(s.physical_read_bytes == 4096, "physical byte count changed");
    if (local) {
        // Device-local mapping requires device DRAM, whose read cache already
        // serves the followers of an in-flight page; nothing is left to join.
        check(s.read_buffer_misses + s.device_dram_read_hits == 64 &&
              s.read_buffer_coalesced_reads == 0 && s.accounting_verified,
            "device-DRAM path lost the shared read or its accounting");
        return;
    }
    check(s.read_buffer_coalesced_bytes == 63 * 64, "coalesced byte count changed");
    check(s.read_buffer_hits == 0 && s.read_buffer_misses == 64 && s.read_buffer_coalesced_reads == 63,
        "pending reads were mislabeled as immediate hits");
    check(last_done < first_done + 4000 && waits > 0 && s.accounting_verified,
        "pending wait or accounting was lost");
    (void)h.issue(request(0, last_done + 1, space));
    s = h.stats();
    check(s.page_reads == 1 && s.read_buffer_hits == 1,
        "completed page did not become an ordinary hit");
}

void eviction_and_bank_isolation() {
    auto c = tiny(); HbfWithHbm h(c);
    h.reserve_static_physical_block_extent(0, 1);
    double at = 0;
    const auto read = [&](std::uint64_t page) {
        at = h.issue(request(page * 4096, at)).finish_ns;
    };
    read(0); read(1); read(2); read(0);
    check(h.stats().page_reads == 4, "pending index bypassed two-page LRU eviction");
    const auto other = std::uint64_t(c.device.blocks_per_plane) * c.device.pages_per_block;
    auto a = h.issue(request(3 * 4096, at));
    auto b = h.issue(request(other * 4096, at));
    auto b2 = h.issue(request(other * 4096 + 64, at));
    check(h.stats().page_reads == 6 && h.stats().read_buffer_coalesced_reads == 1,
        "in-flight merging crossed physical pages or banks");
    at = std::max({a.finish_ns, b.finish_ns, b2.finish_ns});
    read(3);
    check(h.stats().page_reads == 6, "other bank evicted the local page");
}

void protect_actual_sram_consumption() {
    auto c = tiny();
    // Three fills occupy SRAM without a gap for the late 64-B consumer.
    // At its SRAM slot the first page has been evicted, so it must not join
    // merely because the first page existed at its original fill instant.
    c.device.logic_sram_bandwidth_GBps = 0.125;
    HbfWithHbm h(c); h.reserve_static_physical_block_extent(0, 1);
    (void)h.issue(request(0));
    (void)h.issue(request(4096));
    (void)h.issue(request(8192));
    (void)h.issue(request(64));
    check(h.stats().page_reads == 4 && h.stats().read_buffer_coalesced_reads == 0,
        "joined data that was unavailable at actual SRAM consumption");
}

void complete_page_program_invalidates() {
    auto c = tiny(); c.host.mapping_mode = host::MappingMode::RawPhysical;
    HbfWithHbm h(c);
    auto w = request(0, 0, AddressSpace::Physical, 4096); w.op = Op::Write;
    auto at = h.issue_channel_local(w).finish_ns;
    auto a = h.issue_channel_local(request(0, at));
    auto b = h.issue_channel_local(request(64, at));
    check(h.stats().page_reads == 1, "physical channel path did not coalesce");
    const auto invalidated = h.invalidate_zone(0, 0, 0, std::max(a.finish_ns, b.finish_ns));
    const auto reset = h.reset_zone(0, 0, 0, invalidated.finish_ns);
    w.arrival_ns = reset.finish_ns;
    at = h.issue_channel_local(w).finish_ns;
    auto r = h.issue_channel_local(request(0, at));
    check(h.stats().page_reads == 2 && r.finish_ns >= at + c.device.t_read_page_ns,
        "page-zero erase/program reused old decoded data");
}
}

int main() {
    try {
        pending_index_lifetime();
        auto off = tiny(); off.host.read_buffer_coalescing_enabled = false;
        HbfWithHbm baseline(off); baseline.reserve_static_physical_block_extent(0, 1);
        (void)baseline.issue(request(0)); (void)baseline.issue(request(64));
        check(baseline.stats().page_reads == 2 && baseline.stats().read_buffer_coalesced_reads == 0,
            "disabled option changed the baseline miss path");
        for (const auto space : {AddressSpace::Physical, AddressSpace::Logical})
            for (const auto spacing : {0.0, 1.0})
                for (const bool local : {false, true})
                    if (!local || space == AddressSpace::Logical)
                        same_page_burst(space, spacing, local);
        {
            auto c=tiny();HbfWithHbm h(c);h.prepopulate_read_only_logical_page_range(0,1);
            auto a=h.issue(request(0,0,AddressSpace::Logical,4096));
            auto b=h.issue(request(0,0,AddressSpace::Logical,4096));
            const auto st=h.stats();
            check(st.read_requests==2 && st.logical_read_bytes==8192 && st.page_reads==1 && st.read_buffer_coalesced_reads==1,
                "coarse requests failed shared-controller payload/accounting");
            check(b.finish_ns>=a.finish_ns && st.accounting_verified,"coarse response/readiness lost");
        }
        eviction_and_bank_isolation();
        protect_actual_sram_consumption();
        complete_page_program_invalidates();
        std::cout << "in-flight page coalescing, data readiness, eviction and invalidation passed\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

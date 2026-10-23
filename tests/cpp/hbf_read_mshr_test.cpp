// Opt-in page-level read MSHR at stack ingress (hbf-read-mshr-*): a same-page
// burst pays one issue, lookup and page-read credit; entries are bounded,
// retire with their fill, cap their targets and never survive invalidation.
#include "../../verification/probes/hbf_with_hbm.hpp"
#include <algorithm>
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
HbfConfig tiny(std::uint32_t entries) {
    HbfConfig c;
    c.device.stacks = c.device.channels_per_stack = c.device.dies_per_channel = 1;
    c.device.planes_per_die = 2;
    c.device.blocks_per_plane = 16; c.device.pages_per_block = 16;
    c.device.t_read_page_ns = 4000; c.device.t_program_page_ns = 75000;
    c.device.thermal_enabled = false;
    c.host.auto_gc_enabled = false; c.host.gc_reserved_free_blocks_per_plane = 0;
    c.host.mapping_mode = host::MappingMode::FullResident;
    c.host.read_buffer_coalescing_enabled = true;
    c.host.read_mshr_entries_per_stack = entries;
    return c;
}
PhysicalRequest read(std::uint64_t address, double at, std::uint64_t bytes = 64) {
    PhysicalRequest r;
    r.id = "mshr"; r.tier = Tier::HBF; r.op = Op::Read;
    r.address_space = AddressSpace::Logical; r.addr = address; r.bytes = bytes; r.arrival_ns = at;
    r.trace.mode = TraceMode::Full;
    return r;
}

// Returns the last finish of a 64 x 64 B burst over page 0 issued at t=0.
double burst(std::uint32_t entries, host::HbfStats& stats) {
    HbfWithHbm h(tiny(entries));
    h.prepopulate_read_only_logical_page_range(0, 1);
    double fill_ready = 0, last = 0;
    for (unsigned i = 0; i < 64; ++i) {
        const auto done = h.issue(read(i * 64, 0));
        if (i == 0) {
            for (const auto& span : done.spans)
                if (span.name.ends_with("/sram_stage_read")) fill_ready = span.end_ns;
            check(fill_ready > 4000, "no decoded fill completion diagnostic");
        } else {
            check(done.finish_ns >= fill_ready, "joined response preceded decoded data");
            check(done.breakdown.array_read_ns == 0 && done.breakdown.ecc_latency_ns == 0,
                "follower charged another sense or ECC decode");
            if (entries) {
                check(done.note == "read-mshr-join", "follower did not attach to the entry");
                check(done.breakdown.command_ns == tiny(entries).host.read_mshr_check_ns,
                    "follower paid more than the ingress check");
            }
        }
        last = std::max(last, done.finish_ns);
    }
    stats = h.stats();
    check(stats.accounting_verified, "MSHR burst lost accounting");
    return last;
}
} // namespace

int main() {
    try {
        host::HbfStats off, on;
        const auto off_last = burst(0, off);
        const auto on_last = burst(4, on);
        check(off.read_mshr_allocations == 0 && off.read_mshr_joined_reads == 0 &&
              off.page_read_admission_events == 64,
            "disabled MSHR changed the ordinary path");
        check(on.read_requests == 64 && on.logical_read_bytes == 4096 &&
              on.page_reads == 1 && on.physical_read_bytes == 4096,
            "MSHR burst changed request or media accounting");
        check(on.read_mshr_allocations == 1 && on.read_mshr_joined_reads == 63 &&
              on.read_mshr_joined_bytes == 63 * 64 && on.page_read_admission_events == 1,
            "same-page burst did not share one issue and credit");
        check(on_last <= off_last, "MSHR made the burst slower");

        {   // Capacity: one entry; the second page cannot allocate.
            HbfWithHbm h(tiny(1));
            h.prepopulate_read_only_logical_page_range(0, 2);
            for (const auto addr : {0u, 4096u, 64u, 4096u + 64u}) (void)h.issue(read(addr, 0));
            const auto s = h.stats();
            check(s.read_mshr_allocations == 1 && s.read_mshr_full_rejects == 1 &&
                  s.read_mshr_joined_reads == 1 && s.page_reads == 2 && s.accounting_verified,
                "entry capacity was not enforced");
        }
        {   // Targets: at most two followers attach to one entry.
            auto c = tiny(4); c.host.read_mshr_targets_per_entry = 2;
            HbfWithHbm h(c);
            h.prepopulate_read_only_logical_page_range(0, 1);
            for (unsigned i = 0; i < 4; ++i) (void)h.issue(read(i * 64, 0));
            const auto s = h.stats();
            check(s.read_mshr_joined_reads == 2 && s.read_buffer_coalesced_reads == 3 &&
                  s.page_reads == 1 && s.accounting_verified,
                "target limit was not enforced or overflow lost read sharing");
        }
        {   // Retirement: a read after the fill completes is an ordinary hit.
            HbfWithHbm h(tiny(4));
            h.prepopulate_read_only_logical_page_range(0, 1);
            const auto first = h.issue(read(0, 0));
            (void)h.issue(read(64, first.finish_ns + 1));
            const auto s = h.stats();
            check(s.read_mshr_joined_reads == 0 && s.read_buffer_hits == 1 && s.accounting_verified,
                "completed entry still accepted followers");
        }
        {   // Multi-page reads neither allocate nor attach.
            HbfWithHbm h(tiny(4));
            h.prepopulate_read_only_logical_page_range(0, 2);
            (void)h.issue(read(0, 0, 8192));
            (void)h.issue(read(0, 0, 8192));
            const auto s = h.stats();
            check(s.read_mshr_allocations == 0 && s.read_mshr_joined_reads == 0 && s.accounting_verified,
                "multi-page read used the single-page MSHR");
        }
        {   // Invalidation: a rewrite retires every entry.
            HbfWithHbm h(tiny(4));
            h.prepopulate_mutable_logical_page_range(0, 1);
            (void)h.issue(read(0, 0));
            auto write = read(0, 1, 4096); write.op = Op::Write;
            (void)h.issue(write);
            const auto after = h.issue(read(64, 2));
            const auto s = h.stats();
            // The later read re-reads the rewritten page and allocates anew.
            check(after.note != "read-mshr-join" && s.read_mshr_allocations == 2 &&
                  s.read_mshr_joined_reads == 0 && s.accounting_verified,
                "follower attached to a page rewritten after allocation");
        }
        {   // The MSHR attaches to pending fills, so it needs read sharing.
            auto c = tiny(4); c.host.read_buffer_coalescing_enabled = false;
            bool rejected = false;
            try { HbfWithHbm h(c); } catch (const std::exception&) { rejected = true; }
            check(rejected, "MSHR without read-buffer coalescing was accepted");
        }
        std::cout << "hbf read MSHR: same-page burst " << off_last << " -> " << on_last << " ns\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

// Opt-in ingress buffer tags (hbf-ingress-buffer-tags): with the read MSHR, a
// single-page read of a page still in the read buffer or covered by the write
// buffer, and a write to a page already in the write buffer, pay only the
// ingress check; stale tags are reclaimed before capacity eviction, and a write retires only its
// own page's entry.
#include "../../verification/probes/hbf_with_hbm.hpp"
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
HbfConfig tiny(bool tags, std::uint32_t entries = 4) {
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
    c.host.ingress_buffer_tags_enabled = tags;
    c.host.write_coalescing_enabled = true;
    return c;
}
PhysicalRequest req(Op op, std::uint64_t address, double at, std::uint64_t bytes = 64) {
    PhysicalRequest r;
    r.id = "tag"; r.tier = Tier::HBF; r.op = op;
    r.address_space = AddressSpace::Logical; r.addr = address; r.bytes = bytes; r.arrival_ns = at;
    r.trace.mode = TraceMode::Full;
    return r;
}
} // namespace

int main() {
    try {
        const double check_ns = tiny(true).host.read_mshr_check_ns;
        double off_hit = 0, on_hit = 0;
        for (const bool tags : {false, true}) {   // Read-buffer tag after the fill completed.
            HbfWithHbm h(tiny(tags));
            h.prepopulate_read_only_logical_page_range(0, 1);
            const auto first = h.issue(req(Op::Read, 0, 0));
            const auto hit = h.issue(req(Op::Read, 64, first.finish_ns + 1));
            const auto s = h.stats();
            check(s.page_reads == 1 && s.accounting_verified, "read-buffer hit re-read the page");
            if (tags) {
                check(hit.note == "ingress-tag-read-buffer" && hit.breakdown.command_ns == check_ns &&
                      s.ingress_tag_read_buffer_hits == 1 && s.mapping_lookup_ops == 1 &&
                      s.page_read_admission_events == 1,
                    "buffered page did not hit at the ingress check");
                on_hit = hit.finish_ns - hit.arrival_ns;
            } else {
                check(s.ingress_tag_read_buffer_hits == 0 && s.mapping_lookup_ops == 2 && s.read_buffer_hits == 1,
                    "tags off changed the ordinary read-buffer hit");
                off_hit = hit.finish_ns - hit.arrival_ns;
            }
        }
        check(on_hit < off_hit, "the ingress tag hit was not faster than the ordinary hit");
        {   // Eviction: one entry; the second page's fill evicts the first page's entry.
            HbfWithHbm h(tiny(true, 1));
            h.prepopulate_read_only_logical_page_range(0, 2);
            const auto a = h.issue(req(Op::Read, 0, 0));
            const auto b = h.issue(req(Op::Read, 4096, a.finish_ns + 1));
            const auto again = h.issue(req(Op::Read, 64, b.finish_ns + 1));
            const auto s = h.stats();
            check(s.read_mshr_evictions == 1 && s.read_mshr_full_rejects == 0 &&
                  again.note != "ingress-tag-read-buffer" && s.accounting_verified,
                "full tag table did not evict its oldest entry");
        }
        {   // Write-buffer tags: the second write and a covered read pay the check only.
            HbfWithHbm h(tiny(true));
            h.prepopulate_mutable_logical_page_range(0, 1);
            const auto w0 = h.issue(req(Op::Write, 0, 0));
            const auto w1 = h.issue(req(Op::Write, 64, w0.finish_ns + 1));
            const auto covered = h.issue(req(Op::Read, 0, w1.finish_ns + 1, 128));
            const auto partial = h.issue(req(Op::Read, 128, covered.finish_ns + 1));
            const auto s = h.stats();
            check(w1.breakdown.command_ns == check_ns && s.ingress_tag_writes == 1,
                "write to a buffered page paid the scheduler issue");
            check(covered.note == "ingress-tag-write-buffer" && covered.breakdown.command_ns == check_ns &&
                  s.ingress_tag_write_buffer_hits == 1, "covered read missed the write-buffer tag");
            check(partial.note != "ingress-tag-write-buffer" && s.write_buffer_read_hits >= 1 &&
                  s.accounting_verified, "uncovered read used the write-buffer tag");
        }
        {   // A write retires only its own page's entry.
            HbfWithHbm h(tiny(true));
            h.prepopulate_mutable_logical_page_range(0, 2);
            const auto a = h.issue(req(Op::Read, 4096, 0));
            const auto w = h.issue(req(Op::Write, 0, a.finish_ns + 1));
            const auto hit = h.issue(req(Op::Read, 4096 + 64, w.finish_ns + 1));
            const auto s = h.stats();
            check(hit.note == "ingress-tag-read-buffer" && s.accounting_verified,
                "a write to another page retired the entry");
        }
        {   // Pure residency probes neither expose future fills nor reorder LRU.
            auto c = tiny(true).device;
            physical::hbf::HbfDevice media(c, true);
            media.cache_fill(0, 100);
            check(!media.cache_resident(0, 99) && media.cache_resident(0, 100),
                "residency probe exposed a future fill");
            media.cache_fill(1, 110);
            check(media.cache_resident(0, 120), "probe lost the first resident page");
            media.cache_fill(2, 130);
            check(!media.cache_resident(0, 140) && media.cache_resident(1, 140),
                "pure residency probe changed replacement order");
        }
        {   // A backfilled eviction must not cancel a reserved future response.
            auto c = tiny(true).device;
            physical::hbf::HbfDevice media(c, true);
            media.cache_fill(0, 100);
            check(media.cache_hit(0, 300), "could not reserve future consumer");
            media.cache_fill(1, 150);
            media.cache_fill(2, 200); // Would evict 0 before its reserved touch: bypassed.
            check(media.cache_resident(0, 300) && !media.cache_resident(2, 300),
                "future consumer was invalidated by host-order backfill");
            media.advance_cache(300);
            check(media.cache_resident(0, 301), "frontier lost the reserved page");
        }
        {   // A bank holds two pages: old tags must not displace live tags.
            auto c = tiny(true, 3);
            c.device.planes_per_die = 1;
            HbfWithHbm h(c);
            h.prepopulate_read_only_logical_page_range(0, 4);
            double at = 0;
            for (unsigned page = 0; page < 4; ++page)
                at = h.issue(req(Op::Read, page * 4096, at)).finish_ns + 1;
            const auto hit = h.issue(req(Op::Read, 2 * 4096 + 64, at));
            const auto s = h.stats();
            check(s.read_mshr_evictions == 0 && hit.note == "ingress-tag-read-buffer" &&
                  s.page_reads == 4 && s.accounting_verified,
                "expired decoded-page tag consumed capacity or displaced a live tag");
        }
        {   // A probe of an evicted page must not pay a stale tag check.
            auto c = tiny(true, 4);
            c.device.planes_per_die = 1;
            HbfWithHbm h(c);
            h.prepopulate_read_only_logical_page_range(0, 3);
            double at = 0;
            for (unsigned page = 0; page < 3; ++page)
                at = h.issue(req(Op::Read, page * 4096, at)).finish_ns + 1;
            const auto before = h.stats();
            (void)h.issue(req(Op::Read, 0, at));
            const auto after = h.stats();
            check(after.read_mshr_join_fallbacks == before.read_mshr_join_fallbacks &&
                  after.page_reads == before.page_reads + 1 && after.accounting_verified,
                "an evicted page still entered the tag fast path");
        }
        {   // A same-page write invalidates the old read identity, then forwards new dirty bytes.
            HbfWithHbm h(tiny(true));
            h.prepopulate_mutable_logical_page_range(0, 1);
            const auto r = h.issue(req(Op::Read, 0, 0));
            const auto w = h.issue(req(Op::Write, 0, r.finish_ns + 1));
            const auto after = h.issue(req(Op::Read, 0, w.finish_ns + 1));
            const auto s = h.stats();
            check(after.note == "ingress-tag-write-buffer" && s.ingress_tag_read_buffer_hits == 0 &&
                  s.accounting_verified, "same-page write left a stale read-buffer identity");
        }
        {   // Repeated sectors are requests, not 64 unique positions in a page.
            HbfWithHbm h(tiny(true));
            h.prepopulate_read_only_logical_page_range(0, 1);
            for (unsigned i = 0; i < 70; ++i) (void)h.issue(req(Op::Read, 0, 0));
            const auto s = h.stats();
            check(s.read_requests == 70 && s.read_mshr_joined_reads == 64 &&
                  s.page_reads == 1 && s.accounting_verified,
                "target overflow lost requests or repeated the array read");
        }
        {   // Tags extend the read MSHR, so they need it.
            auto c = tiny(true, 0);
            bool rejected = false;
            try { HbfWithHbm h(c); } catch (const std::exception&) { rejected = true; }
            check(rejected, "ingress tags without the read MSHR were accepted");
        }
        std::cout << "hbf ingress tags: buffered hit " << off_hit << " -> " << on_hit << " ns\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

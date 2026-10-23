// Shared HBM data-port controls; serial issue remains the default legacy policy.
#include "../../verification/probes/hbf_with_hbm.hpp"
#include <cmath>
#include <iostream>
#include <stdexcept>
using namespace hbfsim;
using namespace physical;
using verification::HbfWithHbm;
namespace {
void check(bool ok, const char* message) { if (!ok) throw std::runtime_error(message); }
host::HbfConfig flash(bool shared) {
    host::HbfConfig c;
    c.device.stacks = c.device.channels_per_stack = c.device.dies_per_channel = 1;
    c.device.planes_per_die = 1;
    c.device.blocks_per_plane = 16; c.device.pages_per_block = 8;
    c.device.thermal_enabled = false;
    c.host.auto_gc_enabled = false; c.host.gc_reserved_free_blocks_per_plane = 0;
    c.host.mapping_mode = host::MappingMode::FullResident;
    c.host.mapping_control_compute_ns = 0;
    c.host.ctrl_dram_bytes = 32768;
    c.host.ctrl_dram_latency_ns = 100;
    c.host.ctrl_dram_issue_ns = 1000;
    c.host.write_coalescing_enabled = true;
    c.host.write_buffer_pages = 4;
    c.host.read_buffer_coalescing_enabled = true;
    c.host.read_mshr_entries_per_stack = 4;
    c.host.ingress_buffer_tags_enabled = true;
    c.host.write_buffer_shared_hbm = shared;
    return c;
}
hbm::HbmConfig memory(unsigned channels = 1) {
    hbm::HbmConfig c;
    c.device.capacity_bytes = 256 * 1024;
    c.device.stacks = 1; c.device.channels_per_stack = channels;
    c.device.pseudo_channels_per_channel = 1;
    c.device.bank_groups_per_pseudo_channel = c.device.banks_per_group = 1;
    c.controller.interleave_bytes = 64;
    c.controller.service_group_channels = 1;
    c.controller.service_quantum_bytes = 4096;
    c.timing.bandwidth_efficiency = 1;
    return c;
}
PhysicalRequest request(Op op, std::uint64_t addr, double at, std::uint64_t bytes = 64) {
    return {.id="buffer", .tier=Tier::HBF, .op=op, .address_space=AddressSpace::Logical,
        .trace={.mode=TraceMode::Full}, .arrival_ns=at, .addr=addr, .bytes=bytes};
}
double isolated(bool shared) {
    const auto c = flash(shared);
    HbfWithHbm h(c, nullptr, memory());
    h.prepopulate_mutable_logical_page_range(0, 2);
    const auto warm = h.issue(request(Op::Write, 0, 0));
    const double at = warm.finish_ns + 10000;
    const auto before = h.stats();
    const auto hbm_before = h.buffer_memory().stats();
    const auto done = h.issue(request(Op::Write, 64, at));
    const auto cheap_after = h.execution_stats();
    check(cheap_after.write_buffer_dram_issue_busy_ns - before.write_buffer_dram_issue_busy_ns ==
          (shared ? 0 : c.host.ctrl_dram_issue_ns), "batch snapshot missed data-port issue work");
    const auto after = h.stats();
    check(after.write_buffer_dram_issue_busy_ns == cheap_after.write_buffer_dram_issue_busy_ns,
          "full observation changed the cumulative data-port work");
    const auto hbm_after = h.buffer_memory().stats();
    check(after.mapping_lookup_ops == before.mapping_lookup_ops && after.ingress_tag_writes == before.ingress_tag_writes + 1,
        "isolated buffered write used mapping or missed its tag");
    check(after.write_buffer_dram_write_bytes - before.write_buffer_dram_write_bytes == 64 &&
          hbm_after.controller_buffer_write_bytes - hbm_before.controller_buffer_write_bytes == 64,
        "buffered write charged a duplicate or missing HBM transfer");
    check(after.write_buffer_dram_issue_busy_ns - before.write_buffer_dram_issue_busy_ns ==
          (shared ? 0 : c.host.ctrl_dram_issue_ns), "data-port issue work did not match its policy");
    check(done.finish_ns >= at + c.host.read_mshr_check_ns + c.host.ctrl_dram_latency_ns,
        "buffered response preceded its configured latency floor");
    const auto expected = c.host.read_mshr_check_ns + (shared ? c.host.ctrl_dram_latency_ns : c.host.ctrl_dram_issue_ns);
    check(std::abs((done.finish_ns - at) - expected) < 1e-6,
        "isolated write did not select the declared data-port model");
    return done.finish_ns - at;
}
double group_wait(unsigned channels) {
    auto c = flash(true);
    c.host.ctrl_dram_latency_ns = c.host.ctrl_dram_issue_ns = 0.001;
    HbfWithHbm h(c, nullptr, memory(channels));
    h.prepopulate_mutable_logical_page_range(0, 2);
    const auto a = h.issue(request(Op::Write, 0, 0));
    const auto b = h.issue(request(Op::Write, 4096, a.finish_ns + 1));
    const auto before = h.stats();
    const double at = b.finish_ns + 10000;
    (void)h.issue(request(Op::Read, 0, at));
    (void)h.issue(request(Op::Read, 4096, at));
    const auto after = h.stats();
    check(after.write_buffer_dram_read_bytes - before.write_buffer_dram_read_bytes == 128 && after.accounting_verified,
        "concurrent buffered reads lost bytes");
    return after.host_hbm_queue_wait_ns - before.host_hbm_queue_wait_ns;
}
void common_hbm_contention() {
    auto c = flash(true); c.host.ctrl_dram_latency_ns = c.host.ctrl_dram_issue_ns = 0.001;
    HbfWithHbm h(c, nullptr, memory());
    h.prepopulate_mutable_logical_page_range(0, 2);
    const auto warm = h.issue(request(Op::Write, 0, 0));
    const double at = warm.finish_ns + 10000;
    const auto before = h.stats(); const auto mem_before = h.buffer_memory().stats();
    (void)h.buffer_memory().issue(PhysicalRequest{.id="application", .tier=Tier::HBM, .op=Op::Read,
        .address_space=AddressSpace::Physical, .arrival_ns=at, .addr=0, .bytes=4096});
    (void)h.issue(request(Op::Read, 0, at)); // Buffered data, no mapping lookup.
    (void)h.issue(request(Op::Read, 4096, at)); // Mapping metadata and array path.
    const auto after = h.stats(); const auto mem_after = h.buffer_memory().stats();
    const auto reads = mem_after.read_bytes - mem_before.read_bytes;
    const auto writes = mem_after.write_bytes - mem_before.write_bytes;
    check(after.host_hbm_queue_wait_ns > before.host_hbm_queue_wait_ns &&
          after.mapping_lookup_ops > before.mapping_lookup_ops,
        "buffer data and mapping did not contend with application HBM");
    check(reads == 4096 + after.host_hbm_read_bytes - before.host_hbm_read_bytes &&
          writes == after.host_hbm_write_bytes - before.host_hbm_write_bytes,
        "common HBM traffic was duplicated or escaped accounting");
    const auto expected_work = (reads + writes) / memory().pseudo_channel_bandwidth_GBps();
    check(std::abs(mem_after.bus_busy_ns - mem_before.bus_busy_ns - expected_work) < 1e-6,
        "one HBM group provided extra bandwidth");
    check(after.accounting_verified, "shared HBM broke flash accounting");
}
}
int main() {
    try {
        const auto legacy = isolated(false), shared = isolated(true);
        const auto one = group_wait(1), two = group_wait(2);
        check(one > two, "independent HBM groups lost concurrency or same-group serialization");
        common_hbm_contention();
        auto incompatible = flash(true);
        incompatible.host.device_dram_capacity_denominator = 1;
        incompatible.host.write_buffer_pages = 0;
        bool rejected = false;
        try { HbfWithHbm h(incompatible, nullptr, memory()); }
        catch (const std::exception& e) { rejected = std::string(e.what()).find("shared-HBM") != std::string::npos; }
        check(rejected, "shared HBM and device DRAM accepted an ambiguous buffer placement");
        std::cout << "write buffer latency: legacy=" << legacy << " shared=" << shared
                  << "; group wait: one=" << one << " two=" << two << " ns\n";
    } catch (const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}

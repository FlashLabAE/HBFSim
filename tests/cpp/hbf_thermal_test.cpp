#include "../../verification/probes/hbf_with_hbm.hpp"
#include "host/hbf_controller.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

using hbfsim::physical::AddressSpace;
using hbfsim::physical::Op;
using hbfsim::physical::PhysicalCompletion;
using hbfsim::physical::PhysicalRequest;
using hbfsim::physical::Tier;
using hbfsim::physical::TraceConfig;
using hbfsim::physical::TraceMode;
using hbfsim::host::HbfConfig;
using HbfController = hbfsim::verification::HbfWithHbm;

constexpr std::uint64_t kPage = 4096;
// One 4 KiB page at 6 pJ/bit.
constexpr double kReadEnergyJ = 4096.0 * 8.0 * 6.0e-12;
// Explicit light-throttle budget used by the pacing scenarios: one page of
// read energy takes kReadEnergyJ / kPacingPowerW seconds to pace.
constexpr double kPacingPowerW = 0.001;
constexpr double kPacingSlotNs = kReadEnergyJ / kPacingPowerW * 1e9;

void require(bool condition, const std::string& message) {
    if (!condition) {
        throw std::runtime_error(message);
    }
}

void require_close(
    double actual,
    double expected,
    double tolerance,
    const std::string& context) {
    require(
        std::abs(actual - expected) <= tolerance,
        context + ": expected " + std::to_string(expected) + " +/- " +
            std::to_string(tolerance) + ", got " + std::to_string(actual));
}

void require_throws(
    const std::function<void()>& function,
    const std::string& context,
    const std::string& fragment = {}) {
    try {
        function();
    } catch (const std::runtime_error& error) {
        require(
            std::string(error.what()).find(fragment) != std::string::npos,
            context + ": unexpected message: " + error.what());
        return;
    }
    throw std::runtime_error(context);
}

HbfConfig scalar_config() {
    HbfConfig config;
    config.device.stacks = 1;
    config.device.channels_per_stack = 1;
    config.device.dies_per_channel = 1;
    config.device.planes_per_die = 1;
    config.device.blocks_per_plane = 16;
    config.device.pages_per_block = 64;
    config.device.page_size_bytes = kPage;
    config.device.oob_bytes_per_page = 128;
    config.device.media_lanes_per_plane = 4;

    config.device.page_buffer_banks_per_plane = 2;
    config.host.mapping_entries_per_page = 512;


    return config;
}

// Thermal fixture: idle 40 C (ambient 40, no static power), RTT 60, LTT 65,
// STT at the TTTEMP register maximum and an explicit 1 mW light budget so
// every paced 4 KiB read reserves exactly kPacingSlotNs. degrees_per_page
// sets the thermal mass: C = read energy / degrees_per_page, and R = 1 / C
// keeps tau = R * C = 1 s, so a burst sees no meaningful decay while ten
// idle seconds cool the node back to idle. The default 26 C per page lets
// one read engage light throttling from idle and two more reads stay below
// STT.
HbfConfig thermal_config(HbfConfig config, double degrees_per_page = 26.0) {
    config.device.thermal_enabled = true;
    config.device.thermal_ambient_c = 40.0;
    config.device.thermal_capacitance_j_per_c = kReadEnergyJ / degrees_per_page;
    config.device.thermal_resistance_c_per_w =
        1.0 / config.device.thermal_capacitance_j_per_c;
    config.device.thermal_release_c = 60.0;
    config.device.thermal_throttle_c = 65.0;
    config.device.thermal_severe_c = 123.5;
    config.device.thermal_shutdown_c = 1000.0;
    config.device.thermal_static_power_w = 0.0;
    config.device.thermal_read_energy_pj_per_bit = 6.0;
    config.device.thermal_program_energy_pj_per_bit = 60.0;
    config.device.thermal_erase_energy_uj_per_block = 150.0;
    config.device.thermal_throttle_power_w = kPacingPowerW;
    return config;
}

// Severe-throttle fixture: the study thresholds (RTT 60, LTT 85, STT 95,
// shutdown 105), a 1 W light budget that is far above the sustainable
// level, and 10.5 degrees per page read, so one read from the light ceiling
// lands at 95.5 C, past STT, with margin for the nanoseconds of decay
// between boot and the first admission. tau = 1 s, so the Figure 36
// crossings are closed forms the tests recompute: a halted stack leaves
// Severe when its 55.5 C excess over idle decays to the 45 C excess of LTT.
constexpr double kSevereDegreesPerPage = 10.5;
constexpr double kSeverePeakC = 85.0 + kSevereDegreesPerPage;
HbfConfig severe_config(HbfConfig config, double degrees_per_page = kSevereDegreesPerPage) {
    config = thermal_config(std::move(config), degrees_per_page);
    config.device.thermal_release_c = 60.0;
    config.device.thermal_throttle_c = 85.0;
    config.device.thermal_severe_c = 95.0;
    config.device.thermal_shutdown_c = 105.0;
    config.device.thermal_throttle_power_w = 1.0;
    config.device.thermal_start_at_ceiling = true;
    return config;
}

constexpr double kTauNs = 1e9;
// Time for a node tau = 1 s above idle 40 C to cool from `from_c` to `to_c`.
double cooling_ns(double from_c, double to_c) {
    return kTauNs * std::log((from_c - 40.0) / (to_c - 40.0));
}

PhysicalRequest logical_read(
    std::string id,
    std::uint64_t first_lpn,
    std::uint64_t pages,
    double arrival_ns = 0.0) {
    return PhysicalRequest{
        .id = std::move(id),
        .tier = Tier::HBF,
        .op = Op::Read,
        .address_space = AddressSpace::Logical,
        .trace = TraceConfig{
            .mode = TraceMode::Off,
            .retain_completion_diagnostics = false,
        },
        .arrival_ns = arrival_ns,
        .addr = first_lpn * kPage,
        .bytes = pages * kPage,
    };
}

void disabled_by_default_reports_no_thermal_state() {
    HbfController device(scalar_config());
    device.prepopulate_logical_pages({0, 1});
    (void)device.issue(logical_read("read-0", 0, 1));
    (void)device.issue(logical_read("read-1", 1, 1));
    const auto& stats = device.stats();
    require(!stats.thermal_enabled, "thermal must be disabled by default");
    require(
        stats.thermal_throttled_media_ops == 0 &&
            stats.thermal_throttle_engagements == 0 &&
            stats.thermal_throttle_wait_ns == 0.0 &&
            stats.thermal_pacing_busy_ns == 0.0 &&
            stats.thermal_throttled_span_ns == 0.0 &&
            stats.thermal_severe_engagements == 0 &&
            stats.thermal_severe_halted_media_ops == 0 &&
            stats.thermal_severe_wait_ns == 0.0 &&
            stats.thermal_severe_span_ns == 0.0 &&
            stats.thermal_media_energy_j == 0.0 &&
            stats.thermal_peak_temperature_c == 0.0 &&
            stats.thermal_final_temperature_c == 0.0 &&
            stats.thermal_throttled_stacks == 0 &&
            stats.thermal_severe_stacks == 0 &&
            stats.thermal_cattrip_stacks == 0,
        "disabled thermal model must leave every thermal stat at zero");
}

void cold_device_matches_disabled_timing() {
    auto cold = thermal_config(scalar_config());
    // Enormous thermal mass: the same energy moves the node by micro-kelvins
    // and the governor never engages, so timing must match a thermal-off
    // device exactly.
    cold.device.thermal_capacitance_j_per_c = 1e6;
    HbfController off_device(scalar_config());
    HbfController cold_device(cold);
    off_device.prepopulate_logical_pages({0, 1, 2});
    cold_device.prepopulate_logical_pages({0, 1, 2});
    for (std::uint64_t lpn = 0; lpn < 3; ++lpn) {
        const auto off = off_device.issue(
            logical_read("read-" + std::to_string(lpn), lpn, 1));
        const auto on = cold_device.issue(
            logical_read("read-" + std::to_string(lpn), lpn, 1));
        require(
            off.finish_ns == on.finish_ns,
            "a cold thermal governor must not perturb completion times");
    }
    const auto& stats = cold_device.stats();
    require(stats.thermal_enabled, "thermal stats must report enabled");
    require(
        stats.thermal_media_energy_j > 0.0,
        "media energy must accumulate while enabled");
    require(
        stats.thermal_throttle_engagements == 0 &&
            stats.thermal_throttled_media_ops == 0 &&
            stats.thermal_severe_engagements == 0,
        "a cold device must never engage the throttle");
    require(
        stats.thermal_peak_temperature_c < cold.device.thermal_throttle_c,
        "cold peak temperature must stay below the throttle threshold");
}

void throttle_engages_and_paces_scalar_reads() {
    HbfController device(thermal_config(scalar_config()));
    device.prepopulate_logical_pages({0, 1, 2});
    const auto first = device.issue(logical_read("read-0", 0, 1));
    {
        const auto stats = device.execution_stats();
        require(
            stats.thermal_throttle_engagements == 1,
            "first hot read must engage the throttle exactly once");
        require(
            stats.thermal_throttled_media_ops == 0,
            "the engaging read itself must not be paced");
        require_close(
            stats.thermal_media_energy_j,
            kReadEnergyJ,
            kReadEnergyJ * 1e-9,
            "deposited energy after one read");
    }
    const auto second = device.issue(logical_read("read-1", 1, 1));
    const auto third = device.issue(logical_read("read-2", 2, 1));
    const auto stats = device.execution_stats();
    require(
        stats.thermal_throttle_engagements == 1,
        "pacing must not re-engage an already throttled stack");
    require(
        stats.thermal_severe_engagements == 0,
        "three reads must stay below the severe threshold");
    require(
        stats.thermal_throttled_media_ops == 2,
        "both post-engagement reads must be paced");
    require_close(
        stats.thermal_pacing_busy_ns,
        2.0 * kPacingSlotNs,
        1.0,
        "pacing busy work for two paced pages");
    require(
        stats.thermal_throttle_wait_ns >= 0.9 * kPacingSlotNs,
        "the second paced read must wait behind the first pacing slot");
    // The first paced read occupies [0, slot); the second may start media
    // work only after that slot, so its completion trails by at least the
    // pacing interval minus pipeline overlap.
    require(
        third.finish_ns >= kPacingSlotNs,
        "paced completion must not beat the power budget");
    require(
        third.finish_ns > second.finish_ns + 0.5 * kPacingSlotNs,
        "consecutive paced reads must be rate-limited");

    // Determinism: an identical fresh device must reproduce the exact
    // completion frontier.
    HbfController twin(thermal_config(scalar_config()));
    twin.prepopulate_logical_pages({0, 1, 2});
    (void)twin.issue(logical_read("read-0", 0, 1));
    (void)twin.issue(logical_read("read-1", 1, 1));
    const auto twin_third = twin.issue(logical_read("read-2", 2, 1));
    require(
        twin_third.finish_ns == third.finish_ns,
        "thermal pacing must be deterministic");
    (void)first;
}

void governor_releases_after_idle_decay() {
    HbfController device(thermal_config(scalar_config()));
    device.prepopulate_logical_pages({0, 1, 2, 3});
    (void)device.issue(logical_read("read-0", 0, 1));
    (void)device.issue(logical_read("read-1", 1, 1));
    (void)device.issue(logical_read("read-2", 2, 1));
    // tau = R * C = 1 s; ten seconds of idle decay the node through the
    // release threshold and essentially to the idle steady state.
    (void)device.issue(logical_read("read-3", 3, 1, 1.0e10));
    const auto stats = device.execution_stats();
    require(
        stats.thermal_throttled_media_ops == 2,
        "a released governor must not pace the post-idle read");
    require(
        stats.thermal_throttle_engagements >= 2,
        "the post-idle read's own heat must re-engage the throttle");
    // Three 26 C reads leave the node at 118 C; it crosses RTT after
    // tau * ln(78 / 20) of idle. The burst itself adds at most two pacing
    // slots of light time and a few hundredths of a degree of decay, so the
    // span agrees with the closed form to well under a millisecond.
    require_close(
        stats.thermal_throttled_span_ns,
        cooling_ns(118.0, 60.0),
        1e6,
        "light span must end at the RTT crossing");
    const auto& full = device.stats();
    require(
        full.thermal_peak_temperature_c >= full.thermal_final_temperature_c,
        "final projected temperature cannot exceed the recorded peak");
    require(
        full.thermal_peak_temperature_c >=
            thermal_config(scalar_config()).device.thermal_throttle_c,
        "peak temperature must reach the throttle threshold");
}

void run_path_completion_respects_power_budget() {
    // Tiny deposits (a millikelvin per page) keep a 4096-page run inside
    // the light band; the light ceiling start makes pacing active from the
    // first operation.
    auto config = thermal_config(scalar_config(), 1e-3);
    config.device.thermal_start_at_ceiling = true;

    // Logical page runs require the production 64-planes-per-stack fabric
    // and at least 4096 pages; provision enough capacity for the
    // prepopulated image plus its mapping pages.
    config.device.channels_per_stack = 4;
    config.device.dies_per_channel = 4;
    config.device.planes_per_die = 4;
    config.device.blocks_per_plane = 8;

    HbfController device(config);
    device.prepopulate_mutable_logical_page_range(0, 6144);
    const auto warm = device.issue(logical_read("warm", 0, 8));
    const auto warm_stats = device.execution_stats();
    require(
        warm_stats.thermal_throttled_media_ops == 8,
        "a light-ceiling start paces the warm-up request");
    // The second read is a 4096-page run. Paced, its read energy at the
    // pacing power is a hard lower bound on completion, far beyond media
    // time (~4 ms of batch rounds versus ~805 ms of pacing budget).
    constexpr std::uint64_t kRunPages = 4096;
    const auto paced = device.issue(
        logical_read("paced", 8, kRunPages, warm.finish_ns));
    const double budget_ns =
        static_cast<double>(kRunPages) * kPacingSlotNs;
    require(
        paced.finish_ns >= budget_ns,
        "paced run completion must not beat the pacing power budget");
    const auto stats = device.execution_stats();
    require(
        stats.thermal_throttled_media_ops >= kRunPages,
        "every page of the paced run must be counted as throttled");
    require(
        stats.scalar_read_pages >= kRunPages,
        "the paced request must still execute all pages in the paced request");
    require(
        stats.thermal_severe_engagements == 0,
        "millikelvin deposits must not reach the severe threshold");
    (void)warm;
}

void ceiling_start_paces_from_the_first_operation() {
    auto config = thermal_config(scalar_config());
    config.device.thermal_start_at_ceiling = true;
    HbfController device(config);
    device.prepopulate_logical_pages({0, 1});
    {
        const auto& boot = device.stats();
        require(
            boot.thermal_boot_temperature_c == config.device.thermal_throttle_c,
            "ceiling boot must report the throttle threshold as boot state");
        require(
            boot.thermal_final_temperature_c == config.device.thermal_throttle_c &&
                boot.thermal_peak_temperature_c == config.device.thermal_throttle_c,
            "before any work the projection must sit at the boot ceiling");
        require(
            boot.thermal_throttled_stacks == 1 && boot.thermal_severe_stacks == 0 &&
                boot.thermal_cattrip_stacks == 0,
            "ceiling boot must report the stack in light throttling only");
    }
    (void)device.issue(logical_read("read-0", 0, 1));
    auto stats = device.execution_stats();
    require(
        stats.thermal_throttle_engagements == 0,
        "booting at the ceiling is an initial condition, not an engagement");
    require(
        stats.thermal_throttled_media_ops == 1,
        "the very first read must be paced when booted at the ceiling");
    // tau = 1 s: ten seconds of idle decay the node through the release
    // threshold, so the post-idle read is admitted unpaced and its own
    // heat re-engages as a counted transition.
    (void)device.issue(logical_read("read-1", 1, 1, 1.0e10));
    stats = device.execution_stats();
    require(
        stats.thermal_throttled_media_ops == 1,
        "a released ceiling-start governor must not pace the post-idle read");
    require(
        stats.thermal_throttle_engagements == 1,
        "post-idle heat must re-engage as a counted transition");
    require(
        stats.thermal_throttled_span_ns > 0.0,
        "ceiling-start throttled span must accumulate");
}

void scalar_request_paces_once_per_stack() {
    auto config = thermal_config(scalar_config(), 1e-3);
    config.device.thermal_start_at_ceiling = true;
    HbfController device(config);
    device.prepopulate_logical_pages({0, 1, 2, 3, 4, 5, 6, 7, 8});
    // One eight-page scalar request: a single pacing decision covers the
    // whole request, and its completion cannot beat the power budget for
    // eight pages of energy.
    const auto batch = device.issue(logical_read("batch", 0, 8));
    auto stats = device.execution_stats();
    require(
        stats.thermal_throttled_media_ops == 8,
        "the whole scalar request must be paced in one decision");
    require_close(
        stats.thermal_pacing_busy_ns,
        8.0 * kPacingSlotNs,
        1.0,
        "pacing busy work for one eight-page request");
    require(
        batch.finish_ns >= 8.0 * kPacingSlotNs,
        "paced request completion must not beat the power budget");
    // The next request queues behind the full eight-slot budget.
    const auto next = device.issue(logical_read("next", 8, 1));
    stats = device.execution_stats();
    require(
        stats.thermal_throttled_media_ops == 9,
        "the follow-up request must also be paced");
    require(
        next.finish_ns >= 9.0 * kPacingSlotNs,
        "consecutive paced requests must reserve distinct budget slots");
}

void neighbor_heat_raises_boundary_and_shrinks_derived_budget() {
    // Derived pacing budget = (throttle - ambient - neighbor) / R - static.
    // With R = 1 and static = 0: 45 W isolated versus 25 W with 20 C of
    // package cross-heating, directly observable as the pacing-slot work
    // for one paced page.
    const auto make = [](double neighbor_heat_c) {
        auto config = thermal_config(scalar_config());
        config.device.thermal_resistance_c_per_w = 1.0;
        // tau = R * C = 1 s so the ceiling boot survives the admission
        // pipeline instead of decaying through release within nanoseconds.
        config.device.thermal_capacitance_j_per_c = 1.0;
        config.device.thermal_throttle_power_w = 0.0;
        config.device.thermal_neighbor_heat_c = neighbor_heat_c;
        // Keep the 60 C boundary below the release threshold so the
        // cross-heated configuration stays constructible.
        config.device.thermal_throttle_c = 85.0;
        config.device.thermal_release_c = 75.0;
        config.device.thermal_start_at_ceiling = true;
        return config;
    };
    HbfController isolated(make(0.0));
    HbfController heated(make(20.0));
    isolated.prepopulate_logical_pages({0});
    heated.prepopulate_logical_pages({0});
    (void)isolated.issue(logical_read("read-0", 0, 1));
    (void)heated.issue(logical_read("read-0", 0, 1));
    const auto cold_stats = isolated.execution_stats();
    const auto hot_stats = heated.execution_stats();
    require(
        cold_stats.thermal_boundary_temperature_c == 40.0 &&
            hot_stats.thermal_boundary_temperature_c == 60.0,
        "boundary temperature must be ambient plus neighbor heat");
    require_close(
        cold_stats.thermal_pacing_busy_ns,
        kReadEnergyJ / 45.0 * 1e9,
        1e-3,
        "isolated derived budget must be 45 W");
    require_close(
        hot_stats.thermal_pacing_busy_ns,
        kReadEnergyJ / 25.0 * 1e9,
        1e-3,
        "cross-heated derived budget must shrink to 25 W");
}

void independent_banks_backfill_thermal_budget() {
    for (const std::uint64_t banks : {2, 256}) {
        HbfConfig config;
        config.device.stacks = 1;
        config.device.channels_per_stack = banks == 2 ? 1 : 16;
        config.device.planes_per_die = banks == 2 ? 2 : 16;
        config.device.blocks_per_plane = 16;
        config.device.thermal_enabled = true;
        config.device.thermal_start_at_ceiling = true;
        config.host.mapping_mode = hbfsim::host::MappingMode::RawPhysical;
        config.host.auto_gc_enabled = false;
        config.host.gc_reserved_free_blocks_per_plane = 0;
        HbfController device(config);
        std::vector<hbfsim::physical::TraceSpan> erases, budgets;
        double finish_ns = 0;
        for (std::uint64_t bank = 0; bank < banks; ++bank) {
            const auto done = device.issue(PhysicalRequest{
                .id = "bank-" + std::to_string(bank),
                .tier = Tier::HBF,
                .op = Op::Write,
                .address_space = AddressSpace::Physical,
                .trace = {.mode = TraceMode::Full},
                .arrival_ns = 0,
                .addr = bank * config.device.blocks_per_plane *
                        config.device.pages_per_block * kPage,
                .bytes = kPage,
            });
            finish_ns = std::max(finish_ns, done.finish_ns);
            for (const auto& span : done.spans) {
                if (span.name.ends_with("/block_erase")) erases.push_back(span);
                if (span.name == "thermal_pacing_budget") budgets.push_back(span);
            }
        }
        require(erases.size() == banks, "each independent bank must erase once");
        require(erases[1].start_ns < erases[0].end_ns,
            "a future program must not serialize another bank's erase");
        const double power_w = (config.device.thermal_throttle_c -
            config.device.thermal_ambient_c) / config.device.thermal_resistance_c_per_w -
            config.device.thermal_static_power_w;
        const double erase_budget_ns = config.device.thermal_erase_energy_uj_per_block *
            1e-6 / power_w * 1e9;
        require(erases[1].start_ns - erases[0].start_ns >= erase_budget_ns - 1,
            "overlapping bank erases must still pay their shared power budget");
        const double energy_j = banks * (config.device.thermal_erase_energy_uj_per_block * 1e-6 +
            kPage * 8 * config.device.thermal_program_energy_pj_per_bit * 1e-12);
        const double budget_ns = energy_j / power_w * 1e9;
        // Energy slots are indivisible: a future program can leave a gap
        // shorter than one erase slot. Allow that bounded fragmentation
        // per program, while ruling out a full erase latency per bank.
        require(finish_ns < budget_ns + banks * erase_budget_ns + config.device.t_erase_block_ns +
            config.device.t_program_page_ns + 10000,
            "parallel erase completion must respect the power/fragmentation bound: banks=" +
            std::to_string(banks) + ", finish_ns=" + std::to_string(finish_ns) +
            ", budget_ns=" + std::to_string(budget_ns));
        require(budgets.size() == banks * 2,
            "every erase and program must retain a pacing reservation");
        std::sort(budgets.begin(), budgets.end(),
            [](const auto& a, const auto& b) { return a.start_ns < b.start_ns; });
        double occupied_ns = 0;
        for (std::size_t i = 0; i < budgets.size(); ++i) {
            occupied_ns += budgets[i].duration_ns();
            if (i) require(budgets[i].start_ns >= budgets[i-1].end_ns,
                "backfilled thermal reservations must never overlap");
        }
        require_close(occupied_ns, budget_ns, 1e-4,
            "calendar slots must conserve energy divided by pacing power");
        const auto stats = device.execution_stats();
        require(stats.block_erases == banks && stats.page_programs == banks,
            "parallel scheduling must conserve physical media operations");
        require_close(stats.thermal_media_energy_j, energy_j, energy_j * 1e-10,
            "parallel scheduling must conserve media heat");
    }
}

// OCP HBF v0.7.0 section 9.2: a stack that reaches STT despite light
// throttling asserts CATTRIP, deasserts AXI Ready and gives no credits.
// Accepted commands complete; every later command waits until the node
// cools below LTT, where Figure 36 returns the stack to light throttling.
void severe_throttle_halts_commands_until_ltt() {
    HbfController device(severe_config(scalar_config()));
    device.prepopulate_logical_pages({0, 1, 2, 3});
    const auto first = device.issue(logical_read("read-0", 0, 1));
    {
        const auto stats = device.execution_stats();
        require(
            stats.thermal_throttled_media_ops == 1 &&
                stats.thermal_severe_halted_media_ops == 0,
            "the read admitted at the light ceiling is paced, not halted");
        require(
            stats.thermal_severe_engagements == 1,
            "a 10.5 C deposit from the 85 C light ceiling must reach STT");
        require(
            first.finish_ns < 1e6,
            "the command in flight at the severe entry must still complete");
    }
    // The second read arrives while the stack is halted: it waits for the
    // exact Figure 36 exit (95.5 C cooling to LTT 85 C), is then paced in
    // light mode, and its own heat re-enters severe throttling.
    const double exit_ns = cooling_ns(kSeverePeakC, 85.0);
    const auto second = device.issue(logical_read("read-1", 1, 1));
    {
        const auto stats = device.execution_stats();
        require(
            stats.thermal_severe_halted_media_ops == 1,
            "the second read must be counted as halted");
        require_close(
            stats.thermal_severe_wait_ns, exit_ns, 1e3,
            "halt wait must equal the LTT cool-down crossing");
        require(
            second.finish_ns >= exit_ns,
            "a halted command cannot complete before the halt lifts");
        require(
            stats.thermal_throttled_media_ops == 2,
            "the released command is paced in light mode");
        require(
            stats.thermal_severe_engagements == 2,
            "the released command's heat re-enters severe throttling");
    }
    const auto third = device.issue(logical_read("read-2", 2, 1));
    const auto stats = device.execution_stats();
    require(
        stats.thermal_severe_halted_media_ops == 2 &&
            stats.thermal_severe_engagements == 3,
        "every further command repeats the halt/release cycle");
    require_close(
        stats.thermal_severe_wait_ns, 3.0 * exit_ns, 1e3,
        "the third read waits two full cool-down crossings from its arrival");
    require_close(
        stats.thermal_severe_span_ns, 2.0 * exit_ns, 1e3,
        "severe span must sum both completed halts");
    require(
        third.finish_ns >= 2.0 * exit_ns,
        "the third read cannot complete before the second halt lifts");

    const auto& projected = device.stats();
    require(
        projected.thermal_severe_stacks == 1 &&
            projected.thermal_throttled_stacks == 0,
        "the stack must be reported in severe throttling at the frontier");
    require(
        projected.thermal_cattrip_stacks == 1,
        "TMON.CES must report the CATTRIP event");
    // The first admission trails boot by about a hundred nanoseconds of
    // decay, a few microkelvin at tau = 1 s.
    require_close(
        projected.thermal_peak_temperature_c, kSeverePeakC, 1e-3,
        "peak temperature must be the STT entry temperature");

    HbfController twin(severe_config(scalar_config()));
    twin.prepopulate_logical_pages({0, 1, 2, 3});
    (void)twin.issue(logical_read("read-0", 0, 1));
    (void)twin.issue(logical_read("read-1", 1, 1));
    const auto twin_third = twin.issue(logical_read("read-2", 2, 1));
    require(
        twin_third.finish_ns == third.finish_ns,
        "severe throttling must be deterministic");
}

// Figure 36 cool-down: Severe -> Light below LTT, Light -> Normal below
// RTT, with the exact RC crossings and the sticky CATTRIP event bit.
void cool_down_passes_through_light_before_normal() {
    HbfController device(severe_config(scalar_config()));
    device.prepopulate_logical_pages({0, 1, 2, 3});
    (void)device.issue(logical_read("read-0", 0, 1));
    // 0.1 s later the node is still above LTT (about 90 C): halted.
    const double exit_ns = cooling_ns(kSeverePeakC, 85.0);
    (void)device.issue(logical_read("read-1", 1, 1, 1e8));
    {
        const auto stats = device.execution_stats();
        require(
            stats.thermal_severe_halted_media_ops == 1,
            "a command arriving inside the halt must wait");
        require_close(
            stats.thermal_severe_wait_ns, exit_ns - 1e8, 1e3,
            "halt wait must run from arrival to the LTT crossing");
    }
    // At 1.0 s the second halt has ended (exit at 2 * exit_ns) and the
    // light decay from LTT toward RTT is still in progress: the read is
    // paced, not halted, and its 10.5 C stay below STT.
    const double light_start_ns = 2.0 * exit_ns;
    const double rtt_crossing_ns = light_start_ns + cooling_ns(85.0, 60.0);
    require(rtt_crossing_ns > 1e9, "fixture: light mode must last past 1 s");
    const double at_1s_c = 40.0 + 45.0 * std::exp(-(1e9 - light_start_ns) / kTauNs);
    (void)device.issue(logical_read("read-2", 2, 1, 1e9));
    {
        const auto stats = device.execution_stats();
        require(
            stats.thermal_severe_halted_media_ops == 1 &&
                stats.thermal_throttled_media_ops == 3 &&
                stats.thermal_severe_engagements == 2,
            "a read during light cool-down is paced and stays below STT");
        require_close(
            stats.thermal_severe_span_ns, 2.0 * exit_ns, 1e3,
            "severe span must cover both halts");
        require_close(
            stats.thermal_throttled_span_ns, 1e9 - light_start_ns, 1e3,
            "light span must cover the decay from the LTT crossing to 1 s");
    }
    // At 3.0 s the node crossed RTT (after 1 s + tau ln((at_1s + 10.5 - 40)
    // / 20)) and sits near idle: the read is admitted at full performance.
    const double normal_ns = 1e9 + cooling_ns(at_1s_c + kSevereDegreesPerPage, 60.0);
    (void)device.issue(logical_read("read-3", 3, 1, 3e9));
    const auto stats = device.execution_stats();
    require(
        stats.thermal_throttled_media_ops == 3 &&
            stats.thermal_severe_halted_media_ops == 1 &&
            stats.thermal_throttle_engagements == 0,
        "a read after the RTT crossing is neither paced nor halted");
    require_close(
        stats.thermal_throttled_span_ns,
        (1e9 - light_start_ns) + (normal_ns - 1e9),
        1e3,
        "light span must end at the exact RTT crossing");
    const auto& projected = device.stats();
    require(
        projected.thermal_throttled_stacks == 0 &&
            projected.thermal_severe_stacks == 0,
        "the stack must be back in normal mode");
    require(
        projected.thermal_cattrip_stacks == 1,
        "TMON.CES is sticky: cooling down never clears the CATTRIP event");
}

// Writes are host commands too: during the halt the device receives no
// page request, payload or program.
void write_commands_wait_for_the_severe_halt() {
    // 10.5 degrees per program (60 pJ/bit, ten reads' worth), no erase
    // heat, so the first program lifts the light ceiling past STT and the
    // second one waits.
    auto config = severe_config(scalar_config(), kSevereDegreesPerPage / 10.0);
    config.device.thermal_erase_energy_uj_per_block = 0.0;
    config.host.mapping_mode = hbfsim::host::MappingMode::RawPhysical;
    config.host.auto_gc_enabled = false;
    config.host.gc_reserved_free_blocks_per_plane = 0;
    HbfController device(config);
    const auto write = [&](std::string id, std::uint64_t page) {
        return device.issue(PhysicalRequest{
            .id = std::move(id),
            .tier = Tier::HBF,
            .op = Op::Write,
            .address_space = AddressSpace::Physical,
            .trace = {.mode = TraceMode::Full},
            .arrival_ns = 0,
            .addr = page * kPage,
            .bytes = kPage,
        });
    };
    const auto first = write("write-0", 0);
    require(
        device.execution_stats().thermal_severe_engagements == 1 &&
            device.execution_stats().thermal_severe_halted_media_ops == 0,
        "the first program reaches STT without waiting");
    const auto second = write("write-1", 1);
    const auto stats = device.execution_stats();
    const double exit_ns = cooling_ns(kSeverePeakC, 85.0);
    require(
        stats.thermal_severe_halted_media_ops == 1,
        "the second program must wait for the halt");
    require(
        stats.thermal_severe_wait_ns > 0.99 * exit_ns &&
            stats.thermal_severe_wait_ns <= exit_ns + 1e6,
        "program halt wait must be the LTT cool-down crossing");
    require(
        second.finish_ns >= first.finish_ns + 0.99 * exit_ns,
        "the halted program cannot complete before the halt lifts");
    const bool traced_halt = std::any_of(
        second.spans.begin(), second.spans.end(),
        [](const auto& span) { return span.name == "wait_thermal_severe_halt"; });
    require(traced_halt, "the halt must appear as a traced wait on the stack");
    require(
        stats.page_programs == 2,
        "both programs must still execute once the halt lifts");
}

// A command that had not been accepted when CATTRIP went high cannot be
// accepted before the halt lifted, whatever its issue time: neither its
// pacing slot nor its media work may be backfilled into the halt window.
void late_arrivals_cannot_backfill_a_halt_window() {
    // 3.5 degrees per read: three reads from the 85 C ceiling reach STT
    // (95.5 C). The read released at the LTT exit lands at 88.5 C and the
    // next one at 92 C, both in light throttling, so the fifth read meets a
    // stack that is no longer halted but whose halt window still lies after
    // its issue time.
    HbfController device(severe_config(scalar_config(), 3.5));
    device.prepopulate_logical_pages({0, 1, 2, 3, 4});
    for (std::uint64_t lpn = 0; lpn < 3; ++lpn) {
        (void)device.issue(logical_read("read-" + std::to_string(lpn), lpn, 1));
    }
    require(
        device.execution_stats().thermal_severe_engagements == 1,
        "fixture: the third read must enter severe throttling");
    const double exit_ns = cooling_ns(kSeverePeakC, 85.0);
    const auto released = device.issue(logical_read("read-3", 3, 1));
    const auto late = device.issue(logical_read("read-4", 4, 1));
    const auto stats = device.execution_stats();
    require(
        stats.thermal_severe_engagements == 1,
        "the reads after the halt must stay below STT");
    require(
        stats.thermal_severe_halted_media_ops == 2,
        "both reads issued before the halt lifted must count as halted");
    require_close(
        stats.thermal_severe_wait_ns, 2.0 * exit_ns, 1e3,
        "each late arrival waits from its issue time to the halt exit");
    // A completion's start_ns is the request's first activity (address
    // generation at issue), so the media bound is on completion: nothing
    // accepted after the halt can finish within one page read of its end.
    const double t_read_ns = device.config().device.t_read_page_ns;
    require(
        released.finish_ns >= exit_ns + t_read_ns &&
            late.finish_ns >= exit_ns + t_read_ns,
        "no late arrival may be served inside the halt window");
    const auto& projected = device.stats();
    require(
        projected.thermal_throttled_stacks == 1 && projected.thermal_severe_stacks == 0,
        "the stack must finish the burst in light throttling");
}

// Table 34: heat that keeps rising with CATTRIP asserted shuts the device
// down and the host sees a link error. The simulator fails closed.
void shutdown_fails_closed() {
    require_throws(
        [] {
            // Thirty degrees per read: 85 C + 30 C passes STT and the
            // 105 C shutdown point in one deposit.
            HbfController device(severe_config(scalar_config(), 30.0));
            device.prepopulate_logical_pages({0});
            (void)device.issue(logical_read("read-0", 0, 1));
        },
        "a deposit past the shutdown threshold must fail the simulation",
        "thermal shutdown");
    // A device that never reaches the shutdown point keeps running.
    HbfController device(severe_config(scalar_config(), 19.0));
    device.prepopulate_logical_pages({0, 1});
    (void)device.issue(logical_read("read-0", 0, 1));
    const auto& stats = device.stats();
    require_close(
        stats.thermal_peak_temperature_c, 104.0, 1e-3,
        "a 19 C deposit from the light ceiling peaks at 104 C");
    require(
        stats.thermal_severe_stacks == 1,
        "a 104 C peak stays below shutdown in severe throttling");
}

void thresholds_must_sit_on_the_tttemp_register_grid() {
    const auto construct = [](const std::function<void(HbfConfig&)>& edit) {
        auto config = thermal_config(scalar_config());
        edit(config);
        HbfController device(config);
    };
    // 60 C + n * 0.5 C constructs; anything else is rejected.
    construct([](HbfConfig& c) {
        c.device.thermal_release_c = 60.5;
        c.device.thermal_throttle_c = 85.5;
        c.device.thermal_severe_c = 123.5;
    });
    require_throws(
        [&] { construct([](HbfConfig& c) { c.device.thermal_throttle_c = 85.25; }); },
        "a quarter-degree LTT must be rejected", "TTTEMP");
    require_throws(
        [&] { construct([](HbfConfig& c) { c.device.thermal_release_c = 59.5; }); },
        "an RTT below the 60 C register floor must be rejected", "TTTEMP");
    require_throws(
        [&] { construct([](HbfConfig& c) { c.device.thermal_severe_c = 124.0; }); },
        "an STT above the 123.5 C register ceiling must be rejected", "TTTEMP");
    require_throws(
        [&] { construct([](HbfConfig& c) { c.device.thermal_severe_c = 65.0; }); },
        "STT at or below LTT must be rejected", "release (RTT) < throttle (LTT) < severe (STT)");
    require_throws(
        [&] { construct([](HbfConfig& c) { c.device.thermal_shutdown_c = 123.5; }); },
        "shutdown at or below STT must be rejected", "thermal_shutdown_c");
}

void invalid_thermal_configs_are_rejected() {
    require_throws(
        [] {
            auto config = scalar_config();
            config.device.thermal_start_at_ceiling = true;
            HbfController device(config);
        },
        "ceiling start without an enabled thermal model must be rejected");
    require_throws(
        [] {
            auto config = thermal_config(scalar_config());
            config.device.thermal_resistance_c_per_w = 1.0;
            // Boundary 40 + 25 = 65 C reaches the 60 C release threshold:
            // the neighborhood alone cooks the stack past its operating
            // point, so the configuration must fail closed.
            config.device.thermal_neighbor_heat_c = 25.0;
            HbfController device(config);
        },
        "a boundary at/above the release threshold must be rejected");
    require_throws(
        [] {
            auto config = thermal_config(scalar_config());
            config.device.thermal_neighbor_heat_c = -1.0;
            HbfController device(config);
        },
        "negative neighbor heat must be rejected");
    require_throws(
        [] {
            auto config = thermal_config(scalar_config());
            config.device.thermal_release_c = config.device.thermal_throttle_c;
            HbfController device(config);
        },
        "release threshold at/above throttle threshold must be rejected");
    require_throws(
        [] {
            auto config = thermal_config(scalar_config());
            config.device.thermal_resistance_c_per_w = 1.0;
            // Idle steady state 40 + 30 * 1.0 = 70 C sits above the 60 C
            // release threshold: the governor could never release.
            config.device.thermal_static_power_w = 30.0;
            HbfController device(config);
        },
        "idle steady state above the release threshold must be rejected");
    require_throws(
        [] {
            auto config = thermal_config(scalar_config());
            config.device.thermal_read_energy_pj_per_bit = 0.0;
            HbfController device(config);
        },
        "zero read energy must be rejected while thermal is enabled");
}

} // namespace

int main() {
    try {
        disabled_by_default_reports_no_thermal_state();
        cold_device_matches_disabled_timing();
        throttle_engages_and_paces_scalar_reads();
        governor_releases_after_idle_decay();
        run_path_completion_respects_power_budget();
        ceiling_start_paces_from_the_first_operation();
        scalar_request_paces_once_per_stack();
        neighbor_heat_raises_boundary_and_shrinks_derived_budget();
        independent_banks_backfill_thermal_budget();
        severe_throttle_halts_commands_until_ltt();
        cool_down_passes_through_light_before_normal();
        write_commands_wait_for_the_severe_halt();
        late_arrivals_cannot_backfill_a_halt_window();
        shutdown_fails_closed();
        thresholds_must_sit_on_the_tttemp_register_grid();
        invalid_thermal_configs_are_rejected();
    } catch (const std::exception& error) {
        std::cerr << "hbf_thermal_test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "hbf_thermal_test passed\n";
    return 0;
}

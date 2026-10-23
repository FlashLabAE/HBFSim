// Closed-form checks of the HBM thermal node and the package thermal
// network: energy identity of HBM traffic, the RC relaxation of an HBM
// stack, the boot of the coupled GPU/HBM/HBF network at the steady state of
// the linear system it represents and its return there after a disturbance,
// the ceiling start pinned at LTT, the zero-coupling identity, a package too
// hot for its HBF stacks (throttling from boot, or refused), the coolant
// declaration rules, and the cascaded-topology pass-through heat.
#include "host/hbf_controller.hpp"
#include "physical/hbm/hbm_device.hpp"
#include "physical/package_thermal.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

using hbfsim::host::HbfConfig;
using hbfsim::host::HbfController;
using hbfsim::physical::AddressSpace;
using hbfsim::physical::Op;
using hbfsim::physical::PackageThermal;
using hbfsim::physical::PackageThermalConfig;
using hbfsim::physical::PhysicalRequest;
using hbfsim::physical::Tier;
using hbfsim::physical::TraceConfig;
using hbfsim::physical::TraceMode;
using hbfsim::physical::hbm::HbmConfig;
using hbfsim::physical::hbm::HbmDevice;

constexpr std::uint64_t kPage = 4096;
constexpr double kAmbientC = 45.0;

void require(bool condition, const std::string& message) {
    if (!condition) throw std::runtime_error(message);
}

void require_close(double actual, double expected, double tolerance, const std::string& context) {
    require(std::abs(actual - expected) <= tolerance,
            context + ": expected " + std::to_string(expected) + " +/- " + std::to_string(tolerance) +
                ", got " + std::to_string(actual));
}

void require_throws(const std::function<void()>& action, const std::string& fragment, const std::string& context) {
    try {
        action();
    } catch (const std::exception& error) {
        require(std::string(error.what()).find(fragment) != std::string::npos,
                context + ": unexpected message: " + error.what());
        return;
    }
    throw std::runtime_error(context + ": expected an exception");
}

HbmConfig hbm_config() {
    HbmConfig config;
    config.device.stacks = 1;
    config.thermal.enabled = true;
    config.thermal.ambient_c = kAmbientC;
    config.thermal.resistance_c_per_w = 2.0;
    config.thermal.capacitance_j_per_c = 0.25;
    config.thermal.static_power_w = 3.0;
    config.thermal.read_energy_pj_per_bit = 3.4;
    config.thermal.write_energy_pj_per_bit = 3.4;
    return config;
}

HbfConfig hbf_config() {
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
    config.device.thermal_enabled = true;
    config.device.thermal_ambient_c = kAmbientC;
    config.device.thermal_resistance_c_per_w = 2.0;
    config.device.thermal_capacitance_j_per_c = 0.3;
    config.device.thermal_static_power_w = 2.0;
    config.device.thermal_read_energy_pj_per_bit = 8.0;
    config.device.thermal_program_energy_pj_per_bit = 150.0;
    config.device.thermal_erase_energy_uj_per_block = 131.0;
    config.device.thermal_release_c = 80.0;
    config.device.thermal_throttle_c = 85.0;
    config.device.thermal_severe_c = 95.0;
    config.device.thermal_shutdown_c = 105.0;
    return config;
}

PackageThermalConfig package_config() {
    PackageThermalConfig config;
    config.enabled = true;
    config.ambient_c = kAmbientC;
    config.update_interval_ns = 1.0e6;
    config.gpu_power_w = 400.0;
    config.gpu_resistance_c_per_w = 0.05;
    config.gpu_capacitance_j_per_c = 20.0;
    config.coupling_hbf_hbm_w_per_c = 0.1;
    config.coupling_hbf_gpu_w_per_c = 0.3;
    config.coupling_hbm_gpu_w_per_c = 0.3;
    config.coupling_hbf_hbf_w_per_c = 0.0;
    config.coupling_hbm_hbm_w_per_c = 0.0;
    return config;
}

PhysicalRequest hbm_request(std::string id, Op op, std::uint64_t addr, std::uint64_t bytes, double arrival_ns) {
    return PhysicalRequest{
        .id = std::move(id),
        .tier = Tier::HBM,
        .op = op,
        .address_space = AddressSpace::Physical,
        .trace = TraceConfig{.mode = TraceMode::Off, .retain_completion_diagnostics = false},
        .arrival_ns = arrival_ns,
        .addr = addr,
        .bytes = bytes,
    };
}

// Steady state of the three-node network (HBF f, HBM m, GPU g) with the
// package conductances, by elimination of the 3x3 linear system
//   (T_i - A) / R_i + sum_j g_ij (T_i - T_j) = P_i.
std::array<double, 3> steady_state(const PackageThermalConfig& p, double r_f, double p_f, double r_m, double p_m) {
    const double g_fm = p.coupling_hbf_hbm_w_per_c, g_fg = p.coupling_hbf_gpu_w_per_c,
                 g_mg = p.coupling_hbm_gpu_w_per_c;
    double a[3][4] = {
        {1.0 / r_f + g_fm + g_fg, -g_fm, -g_fg, p_f + p.ambient_c / r_f},
        {-g_fm, 1.0 / r_m + g_fm + g_mg, -g_mg, p_m + p.ambient_c / r_m},
        {-g_fg, -g_mg, 1.0 / p.gpu_resistance_c_per_w + g_fg + g_mg,
         p.gpu_power_w + p.ambient_c / p.gpu_resistance_c_per_w},
    };
    for (int col = 0; col < 3; ++col) {
        for (int row = col + 1; row < 3; ++row) {
            const double factor = a[row][col] / a[col][col];
            for (int k = col; k < 4; ++k) a[row][k] -= factor * a[col][k];
        }
    }
    std::array<double, 3> t{};
    for (int row = 2; row >= 0; --row) {
        double sum = a[row][3];
        for (int k = row + 1; k < 3; ++k) sum -= a[row][k] * t[k];
        t[row] = sum / a[row][row];
    }
    return t;
}

// The same network with the HBF node pinned at t_f: two unknowns (HBM m,
// GPU g) by Cramer's rule.
std::array<double, 2> steady_state_pinned(const PackageThermalConfig& p, double t_f, double r_m, double p_m) {
    const double g_fm = p.coupling_hbf_hbm_w_per_c, g_fg = p.coupling_hbf_gpu_w_per_c,
                 g_mg = p.coupling_hbm_gpu_w_per_c;
    const double a11 = 1.0 / r_m + g_fm + g_mg, a12 = -g_mg;
    const double a21 = -g_mg, a22 = 1.0 / p.gpu_resistance_c_per_w + g_fg + g_mg;
    const double b1 = p_m + p.ambient_c / r_m + g_fm * t_f;
    const double b2 = p.gpu_power_w + p.ambient_c / p.gpu_resistance_c_per_w + g_fg * t_f;
    const double det = a11 * a22 - a12 * a21;
    return {(b1 * a22 - a12 * b2) / det, (a11 * b2 - b1 * a21) / det};
}

void test_hbm_energy_identity() {
    HbmDevice hbm(hbm_config());
    const std::uint64_t bytes = 64 * 1024;
    (void)hbm.issue(hbm_request("w", Op::Write, 0, bytes, 0.0));
    (void)hbm.issue(hbm_request("r", Op::Read, 0, bytes, 1.0e6));
    const auto& stats = hbm.stats();
    require(stats.thermal_enabled, "HBM thermal telemetry enabled");
    const double expected = 2.0 * static_cast<double>(bytes) * 8.0 * 3.4e-12;
    require_close(stats.thermal_energy_j, expected, expected * 1e-9, "HBM energy equals bytes x pJ/bit");
    require_close(stats.thermal_imported_energy_j, 0.0, 1e-18, "no imported energy without a cascaded HBF");
    require(stats.thermal_peak_temperature_c > stats.thermal_boot_temperature_c,
            "served bytes raise the HBM node");
}

void test_hbm_node_relaxes_on_its_rc_curve() {
    const auto config = hbm_config();
    HbmDevice hbm(config);
    const std::uint64_t bytes = 1 << 20;
    const auto completion = hbm.issue(hbm_request("w", Op::Write, 0, bytes, 0.0));
    const double idle_c = kAmbientC + config.thermal.static_power_w * config.thermal.resistance_c_per_w;
    const double tau_ns = config.thermal.resistance_c_per_w * config.thermal.capacitance_j_per_c * 1e9;
    // Every deposit happens no later than the completion; the node at the
    // completion instant therefore holds the whole energy minus the decay
    // of the earliest deposits, which bounds it from both sides.
    const double energy = static_cast<double>(bytes) * 8.0 * 3.4e-12;
    const double rise = energy / config.thermal.capacitance_j_per_c;
    const double at_completion = hbm.thermal_temperature(0, completion.finish_ns);
    require(at_completion <= idle_c + rise + 1e-9 && at_completion > idle_c, "node holds the deposited energy");
    const double later_ns = completion.finish_ns + 3.0 * tau_ns;
    const double expected = idle_c + (at_completion - idle_c) * std::exp(-3.0);
    require_close(hbm.thermal_temperature(0, later_ns), expected, 1e-9, "RC relaxation toward idle");
}

void test_package_boots_at_the_steady_state_of_the_linear_network() {
    const auto hbf_cfg = hbf_config();
    const auto hbm_cfg = hbm_config();
    const auto package_cfg = package_config();
    HbmDevice hbm(hbm_cfg);
    HbfController hbf(hbf_cfg);
    hbf.attach_hbm_buffer(hbm);
    PackageThermal package(package_cfg, &hbm, &hbf);
    const auto expected = steady_state(package_cfg,
        hbf_cfg.device.thermal_resistance_c_per_w, hbf_cfg.device.thermal_static_power_w,
        hbm_cfg.thermal.resistance_c_per_w, hbm_cfg.thermal.static_power_w);
    // Boot: every node sits at the network's idle steady state, not at the
    // isolated idle of its own configuration.
    require_close(hbf.thermal_temperature(0, 0.0), expected[0], 1e-6, "HBF node boots at the network steady state");
    require_close(hbm.thermal_temperature(0, 0.0), expected[1], 1e-6, "HBM node boots at the network steady state");
    require_close(package.state(0.0).gpu_boot_temperature_c, expected[2], 1e-6, "GPU node boots at the network steady state");
    require(expected[0] > kAmbientC + hbf_cfg.device.thermal_static_power_w * hbf_cfg.device.thermal_resistance_c_per_w + 5.0,
            "a 400 W GPU puts the HBF stack well above its isolated idle");
    require_close(hbf.stats().thermal_boot_temperature_c, expected[0], 1e-6, "HBF reports the coupled boot temperature");
    require_close(hbm.stats().thermal_boot_temperature_c, expected[1], 1e-6, "HBM reports the coupled boot temperature");
    // The HBF boundary is the Thevenin temperature its neighbors impose.
    const double g_fm = package_cfg.coupling_hbf_hbm_w_per_c, g_fg = package_cfg.coupling_hbf_gpu_w_per_c;
    const double conductance = 1.0 / hbf_cfg.device.thermal_resistance_c_per_w + g_fm + g_fg;
    const double boundary = (package_cfg.ambient_c / hbf_cfg.device.thermal_resistance_c_per_w +
                             g_fm * expected[1] + g_fg * expected[2]) / conductance;
    require_close(hbf.thermal_boundary_c(0), boundary, 1e-6, "HBF effective boundary");
    require(boundary > kAmbientC + 5.0, "a 400 W GPU warms the HBF boundary well above the coolant");
    require(hbf.stats().thermal_effective_boundary_c > kAmbientC, "effective boundary is reported");
    // Steady means steady: a minute of updates moves nothing.
    const double horizon_ns = 60.0e9;
    package.advance_to(horizon_ns);
    require_close(hbf.thermal_temperature(0, horizon_ns), expected[0], 1e-6, "HBF node holds the steady state");
    require_close(hbm.thermal_temperature(0, horizon_ns), expected[1], 1e-6, "HBM node holds the steady state");
    const auto state = package.state(horizon_ns);
    require_close(state.gpu_final_temperature_c, expected[2], 1e-6, "GPU node holds the steady state");
    require_close(state.gpu_peak_temperature_c, expected[2], 1e-6, "an idle network records no peak above boot");
    // One update at boot, then one per interval.
    require(state.updates == 60001, "the network was updated at every interval");
}

void test_hbm_heat_reaches_the_hbf_through_the_network_and_decays() {
    const auto hbf_cfg = hbf_config();
    const auto hbm_cfg = hbm_config();
    const auto package_cfg = package_config();
    HbmDevice hbm(hbm_cfg);
    HbfController hbf(hbf_cfg);
    hbf.attach_hbm_buffer(hbm);
    PackageThermal package(package_cfg, &hbm, &hbf);
    const auto steady = steady_state(package_cfg,
        hbf_cfg.device.thermal_resistance_c_per_w, hbf_cfg.device.thermal_static_power_w,
        hbm_cfg.thermal.resistance_c_per_w, hbm_cfg.thermal.static_power_w);
    const double steady_boundary = hbf.thermal_boundary_c(0);
    // 2 J into the HBM node at one second: +8 C on a 0.25 J/C node.
    const double at_ns = 1.0e9;
    package.advance_to(at_ns);
    hbm.thermal_deposit_imported(0, at_ns, 2.0);
    require_close(hbm.thermal_temperature(0, at_ns), steady[1] + 2.0 / hbm_cfg.thermal.capacitance_j_per_c, 1e-6,
                  "the deposit raises the HBM node by energy over heat capacity");
    // The next update hands the hotter neighbor to the HBF stack as a
    // warmer boundary, and the HBF node follows it upward.
    package.advance_to(at_ns + package_cfg.update_interval_ns);
    require(hbf.thermal_boundary_c(0) > steady_boundary + 0.5, "HBM heat raises the HBF boundary");
    const double half_second_ns = at_ns + 0.5e9;
    package.advance_to(half_second_ns);
    require(hbf.thermal_temperature(0, half_second_ns) > steady[0] + 0.1, "the HBF node warms behind its HBM neighbor");
    require(hbm.thermal_temperature(0, half_second_ns) < steady[1] + 2.0 / hbm_cfg.thermal.capacitance_j_per_c,
            "the HBM node cools from its peak");
    // The disturbance decays through the stacks' own paths to the coolant:
    // a minute later the network is back at its steady state.
    const double later_ns = at_ns + 60.0e9;
    package.advance_to(later_ns);
    require_close(hbf.thermal_temperature(0, later_ns), steady[0], 1e-6, "HBF node returns to the steady state");
    require_close(hbm.thermal_temperature(0, later_ns), steady[1], 1e-6, "HBM node returns to the steady state");
    require_close(hbf.thermal_boundary_c(0), steady_boundary, 1e-6, "HBF boundary returns to the steady state");
    require_close(package.state(later_ns).gpu_final_temperature_c, steady[2], 1e-6, "GPU node returns to the steady state");
}

void test_ceiling_start_keeps_hbf_at_ltt_and_settles_the_rest_around_it() {
    auto hbf_cfg = hbf_config();
    hbf_cfg.device.thermal_start_at_ceiling = true;
    const auto hbm_cfg = hbm_config();
    const auto package_cfg = package_config();
    HbmDevice hbm(hbm_cfg);
    HbfController hbf(hbf_cfg);
    hbf.attach_hbm_buffer(hbm);
    PackageThermal package(package_cfg, &hbm, &hbf);
    const double ltt = hbf_cfg.device.thermal_throttle_c;
    require_close(hbf.thermal_temperature(0, 0.0), ltt, 1e-9, "a ceiling-start HBF stack keeps LTT");
    require(hbf.stats().thermal_throttled_stacks == 1, "a ceiling-start HBF stack boots in Light mode");
    const auto expected = steady_state_pinned(package_cfg, ltt,
        hbm_cfg.thermal.resistance_c_per_w, hbm_cfg.thermal.static_power_w);
    require_close(hbm.thermal_temperature(0, 0.0), expected[0], 1e-6, "HBM boots at the steady state around the pinned HBF");
    require_close(package.state(0.0).gpu_boot_temperature_c, expected[1], 1e-6, "GPU boots at the steady state around the pinned HBF");
    require(expected[0] > steady_state(package_cfg, hbf_cfg.device.thermal_resistance_c_per_w,
                                       hbf_cfg.device.thermal_static_power_w,
                                       hbm_cfg.thermal.resistance_c_per_w, hbm_cfg.thermal.static_power_w)[1],
            "an HBF stack held at LTT warms its HBM neighbor beyond the idle network");
}

void test_zero_coupling_leaves_the_configured_environment() {
    auto package_cfg = package_config();
    package_cfg.gpu_power_w = 0.0;
    package_cfg.coupling_hbf_hbm_w_per_c = 0.0;
    package_cfg.coupling_hbf_gpu_w_per_c = 0.0;
    package_cfg.coupling_hbm_gpu_w_per_c = 0.0;
    HbmDevice hbm(hbm_config());
    HbfController hbf(hbf_config());
    hbf.attach_hbm_buffer(hbm);
    PackageThermal package(package_cfg, &hbm, &hbf);
    package.advance_to(5.0e9);
    require_close(hbf.thermal_boundary_c(0), kAmbientC, 1e-9, "HBF boundary stays at the coolant");
    require_close(hbm.thermal_boundary_c(0), kAmbientC, 1e-9, "HBM boundary stays at the coolant");
    const double idle_c = kAmbientC + 2.0 * 2.0;
    require_close(hbf.thermal_temperature(0, 5.0e9), idle_c, 1e-9, "HBF node idles at its configured steady state");
    require_close(hbf.stats().thermal_boot_temperature_c, idle_c, 1e-9, "HBF boot temperature is the configured idle");
    require_close(hbm.thermal_temperature(0, 5.0e9), kAmbientC + 3.0 * 2.0, 1e-9, "HBM node idles at its configured steady state");
}

void test_a_hot_package_boots_the_hbf_throttling_or_is_refused() {
    const auto hbf_cfg = hbf_config();
    const auto hbm_cfg = hbm_config();
    // 2200 W behind the same conductances rests the HBF stack between LTT
    // and STT: it boots in Light mode with the floor budget, and the first
    // command is paced. The premise is asserted from the linear system so
    // the test states what it exercises.
    {
        auto package_cfg = package_config();
        package_cfg.gpu_power_w = 2200.0;
        const auto expected = steady_state(package_cfg,
            hbf_cfg.device.thermal_resistance_c_per_w, hbf_cfg.device.thermal_static_power_w,
            hbm_cfg.thermal.resistance_c_per_w, hbm_cfg.thermal.static_power_w);
        require(expected[0] >= hbf_cfg.device.thermal_throttle_c && expected[0] < hbf_cfg.device.thermal_severe_c,
                "premise: the idle HBF stack rests between LTT and STT");
        HbmDevice hbm(hbm_cfg);
        HbfController hbf(hbf_cfg);
        hbf.attach_hbm_buffer(hbm);
        PackageThermal package(package_cfg, &hbm, &hbf);
        require_close(hbf.thermal_temperature(0, 0.0), expected[0], 1e-6, "HBF boots at the hot steady state");
        require(hbf.stats().thermal_throttled_stacks == 1, "an HBF stack resting above LTT boots in Light mode");
        require(hbf.stats().thermal_throttle_engagements == 0, "a boot above LTT is not a heat-up engagement");
        hbf.prepopulate_logical_pages({0});
        const PhysicalRequest read{
            .id = "r",
            .tier = Tier::HBF,
            .op = Op::Read,
            .address_space = AddressSpace::Logical,
            .trace = TraceConfig{.mode = TraceMode::Off, .retain_completion_diagnostics = false},
            .arrival_ns = 0.0,
            .addr = 0,
            .bytes = kPage,
        };
        (void)hbf.issue(read);
        require(hbf.execution_stats().thermal_throttled_media_ops == 1, "the first read is paced");
        require(hbf.execution_stats().thermal_pacing_busy_ns > 0.0, "the paced read reserves budget time");
    }
    // 3000 W rests the HBF stack at or above STT: it would assert CATTRIP
    // before serving, so the package is refused at construction.
    {
        auto package_cfg = package_config();
        package_cfg.gpu_power_w = 3000.0;
        const auto expected = steady_state(package_cfg,
            hbf_cfg.device.thermal_resistance_c_per_w, hbf_cfg.device.thermal_static_power_w,
            hbm_cfg.thermal.resistance_c_per_w, hbm_cfg.thermal.static_power_w);
        require(expected[0] >= hbf_cfg.device.thermal_severe_c, "premise: the idle HBF stack rests at or above STT");
        HbmDevice hbm(hbm_cfg);
        HbfController hbf(hbf_cfg);
        hbf.attach_hbm_buffer(hbm);
        require_throws([&] { PackageThermal package(package_cfg, &hbm, &hbf); }, "STT",
                       "a package whose idle steady state reaches STT is refused");
    }
}

void test_coolant_declarations_must_agree_with_the_network() {
    const auto package_cfg = package_config();
    {
        auto hbf_cfg = hbf_config();
        hbf_cfg.device.thermal_neighbor_heat_c = 10.0;
        HbmDevice hbm(hbm_config());
        HbfController hbf(hbf_cfg);
        hbf.attach_hbm_buffer(hbm);
        require_throws([&] { PackageThermal package(package_cfg, &hbm, &hbf); }, "neighbor-heat",
                       "a declared neighbor heat is refused under the package network");
    }
    {
        auto hbf_cfg = hbf_config();
        hbf_cfg.device.thermal_ambient_c = kAmbientC + 10.0;
        HbmDevice hbm(hbm_config());
        HbfController hbf(hbf_cfg);
        hbf.attach_hbm_buffer(hbm);
        require_throws([&] { PackageThermal package(package_cfg, &hbm, &hbf); }, "hbf-thermal-ambient-c",
                       "an HBF ambient that differs from the package ambient is refused");
    }
    {
        auto hbm_cfg = hbm_config();
        hbm_cfg.thermal.ambient_c = kAmbientC - 5.0;
        HbmDevice hbm(hbm_cfg);
        HbfController hbf(hbf_config());
        hbf.attach_hbm_buffer(hbm);
        require_throws([&] { PackageThermal package(package_cfg, &hbm, &hbf); }, "hbm-thermal-ambient-c",
                       "an HBM ambient that differs from the package ambient is refused");
    }
    {
        // A served stack cannot be rebooted: the boot belongs to time zero.
        HbmDevice hbm(hbm_config());
        (void)hbm.issue(hbm_request("w", Op::Write, 0, kPage, 0.0));
        require_throws([&] { hbm.thermal_boot_at(0, 60.0); }, "already served",
                       "a boot after traffic is refused");
    }
}

void test_cascaded_topology_heats_the_hbm_base_die() {
    for (const bool cascaded : {false, true}) {
        auto package_cfg = package_config();
        package_cfg.gpu_power_w = 0.0;
        package_cfg.hbf_topology = cascaded ? PackageThermalConfig::HbfTopology::Cascaded
                                           : PackageThermalConfig::HbfTopology::Beside;
        package_cfg.hbm_passthrough_energy_pj_per_bit = 0.5;
        HbmDevice hbm(hbm_config());
        HbfController hbf(hbf_config());
        hbf.attach_hbm_buffer(hbm);
        PackageThermal package(package_cfg, &hbm, &hbf);
        const std::uint64_t pages = 8;
        const PhysicalRequest write{
            .id = "w",
            .tier = Tier::HBF,
            .op = Op::Write,
            .address_space = AddressSpace::Logical,
            .trace = TraceConfig{.mode = TraceMode::Off, .retain_completion_diagnostics = false},
            .arrival_ns = 0.0,
            .addr = 0,
            .bytes = pages * kPage,
        };
        const auto completion = hbf.issue(write);
        package.advance_to(completion.finish_ns + 1.0e6);
        const auto& stats = hbm.stats();
        const double expected = cascaded ? static_cast<double>(pages * kPage) * 8.0 * 0.5e-12 : 0.0;
        require_close(stats.thermal_imported_energy_j, expected, expected * 1e-9 + 1e-18,
                      cascaded ? "cascaded HBF programs deposit PHY energy in the HBM base die"
                               : "an HBF beside the GPU deposits nothing in HBM");
        require_close(package.state(completion.finish_ns).passthrough_bytes,
                      cascaded ? static_cast<double>(pages * kPage) : 0.0, 1e-9, "pass-through bytes");
    }
}

}  // namespace

int main() {
    try {
        test_hbm_energy_identity();
        test_hbm_node_relaxes_on_its_rc_curve();
        test_package_boots_at_the_steady_state_of_the_linear_network();
        test_hbm_heat_reaches_the_hbf_through_the_network_and_decays();
        test_ceiling_start_keeps_hbf_at_ltt_and_settles_the_rest_around_it();
        test_zero_coupling_leaves_the_configured_environment();
        test_a_hot_package_boots_the_hbf_throttling_or_is_refused();
        test_coolant_declarations_must_agree_with_the_network();
        test_cascaded_topology_heats_the_hbm_base_die();
    } catch (const std::exception& error) {
        std::cerr << "package thermal test failed: " << error.what() << '\n';
        return 1;
    }
    std::cout << "package thermal tests passed\n";
    return 0;
}

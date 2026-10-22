#pragma once
#include "host/hbf_controller.hpp"
#include "physical/hbm/hbm_device.hpp"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace hbfsim::physical {

// Package-level thermal coupling between the GPU die, the HBM stacks and
// the HBF stacks of one package. Every stack keeps its own lumped node
// (HbfController, HbmDevice); this module adds a GPU node and the
// conductances between node classes, and at each update instant replaces
// every stack's environment with the one the network imposes:
//
//   1 / R_eff,i = 1 / R_i + sum_j g_ij
//   T_eff,i     = R_eff,i * (T_amb / R_i + sum_j g_ij * T_j)
//
// so between updates each node still relaxes toward a fixed asymptote
// with a fixed time constant and the per-device closed forms stay exact.
// Conductances are per node pair and symmetric, so the network conserves
// energy; the pairing is complete within each class pair (every HBF stack
// couples to every HBM stack with the same conductance), a lid-spreading
// approximation that needs no floorplan.
//
// Two topologies follow FlashAccel: HBF beside the GPU (its traffic only
// heats the HBF stack) and HBF cascaded behind an HBM base die (every byte
// an HBF stack moves also crosses that HBM stack's PHY and deposits
// passthrough energy there).
struct PackageThermalConfig {
    bool enabled = false;
    double ambient_c = 45.0;
    double update_interval_ns = 1.0e6;
    double gpu_power_w = 0.0;
    double gpu_resistance_c_per_w = 0.05;
    double gpu_capacitance_j_per_c = 20.0;
    double coupling_hbf_hbm_w_per_c = 0.1;
    double coupling_hbf_gpu_w_per_c = 0.3;
    double coupling_hbm_gpu_w_per_c = 0.3;
    double coupling_hbf_hbf_w_per_c = 0.1;
    double coupling_hbm_hbm_w_per_c = 0.1;
    enum class HbfTopology { Beside, Cascaded } hbf_topology = HbfTopology::Beside;
    double hbm_passthrough_energy_pj_per_bit = 0.5;
};

struct PackageThermalState {
    bool enabled = false;
    std::uint64_t updates = 0;
    double gpu_power_w = 0.0;
    double gpu_boot_temperature_c = 0.0;
    double gpu_peak_temperature_c = 0.0;
    double gpu_final_temperature_c = 0.0;
    double gpu_boundary_c = 0.0;
    double passthrough_bytes = 0.0;
    std::vector<double> hbf_boundaries_c;
    std::vector<double> hbm_boundaries_c;
    std::vector<double> hbf_temperatures_c;
    std::vector<double> hbm_temperatures_c;
};

class PackageThermal {
public:
    // hbm or hbf may be null when that tier is absent; the GPU node and the
    // remaining couplings still apply.
    PackageThermal(PackageThermalConfig config, hbm::HbmDevice* hbm, host::HbfController* hbf);
    // Run every update instant up to t_ns. Monotone: a smaller t_ns than the
    // last one is a no-op.
    void advance_to(double t_ns);
    [[nodiscard]] PackageThermalState state(double at_ns) const;
    [[nodiscard]] const PackageThermalConfig& config() const { return config_; }
    [[nodiscard]] std::string json(double at_ns) const;

private:
    PackageThermalConfig config_;
    hbm::HbmDevice* hbm_ = nullptr;
    host::HbfController* hbf_ = nullptr;
    std::size_t hbf_stacks_ = 0;
    std::size_t hbm_stacks_ = 0;
    double hbf_resistance_ = 0.0;
    double hbm_resistance_ = 0.0;
    double gpu_temperature_c_ = 0.0;
    double gpu_clock_ns_ = 0.0;
    double gpu_idle_c_ = 0.0;
    double gpu_tau_ns_ = 0.0;
    double gpu_boundary_c_ = 0.0;
    double gpu_peak_c_ = 0.0;
    double gpu_boot_c_ = 0.0;
    double next_update_ns_ = 0.0;
    std::uint64_t updates_ = 0;
    double passthrough_bytes_ = 0.0;
    [[nodiscard]] double gpu_temperature(double at_ns) const;
    void update(double at_ns);
};

}  // namespace hbfsim::physical

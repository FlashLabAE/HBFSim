#include "physical/package_thermal.hpp"

#include <cmath>
#include <limits>
#include <sstream>
#include <stdexcept>

namespace hbfsim::physical {

namespace {
void require_finite_nonnegative(double value, const char* name) {
    if (!std::isfinite(value) || value < 0.0) {
        throw std::runtime_error(std::string("package thermal ") + name + " must be non-negative and finite");
    }
}
}  // namespace

PackageThermal::PackageThermal(PackageThermalConfig config, hbm::HbmDevice* hbm, host::HbfController* hbf)
    : config_(config), hbm_(hbm), hbf_(hbf) {
    if (!config_.enabled) {
        throw std::runtime_error("package thermal model constructed while disabled");
    }
    if (!std::isfinite(config_.ambient_c)) {
        throw std::runtime_error("package thermal ambient_c must be finite");
    }
    if (!std::isfinite(config_.update_interval_ns) || config_.update_interval_ns <= 0.0) {
        throw std::runtime_error("package thermal update_interval_ns must be positive and finite");
    }
    require_finite_nonnegative(config_.gpu_power_w, "gpu_power_w");
    if (!std::isfinite(config_.gpu_resistance_c_per_w) || config_.gpu_resistance_c_per_w <= 0.0 ||
        !std::isfinite(config_.gpu_capacitance_j_per_c) || config_.gpu_capacitance_j_per_c <= 0.0) {
        throw std::runtime_error("package thermal GPU resistance and capacitance must be positive and finite");
    }
    require_finite_nonnegative(config_.coupling_hbf_hbm_w_per_c, "coupling_hbf_hbm_w_per_c");
    require_finite_nonnegative(config_.coupling_hbf_gpu_w_per_c, "coupling_hbf_gpu_w_per_c");
    require_finite_nonnegative(config_.coupling_hbm_gpu_w_per_c, "coupling_hbm_gpu_w_per_c");
    require_finite_nonnegative(config_.coupling_hbf_hbf_w_per_c, "coupling_hbf_hbf_w_per_c");
    require_finite_nonnegative(config_.coupling_hbm_hbm_w_per_c, "coupling_hbm_hbm_w_per_c");
    require_finite_nonnegative(config_.hbm_passthrough_energy_pj_per_bit, "hbm_passthrough_energy_pj_per_bit");
    if (hbf_ != nullptr && hbf_->thermal_stack_count() != 0) {
        hbf_stacks_ = hbf_->thermal_stack_count();
        hbf_resistance_ = hbf_->config().device.thermal_resistance_c_per_w;
    }
    if (hbm_ != nullptr && hbm_->thermal_stack_count() != 0) {
        hbm_stacks_ = hbm_->thermal_stack_count();
        hbm_resistance_ = hbm_->config().thermal.resistance_c_per_w;
    }
    if (hbf_stacks_ == 0 && hbm_stacks_ == 0) {
        throw std::runtime_error(
            "package thermal model needs at least one thermally modeled stack "
            "(hbf-thermal-enable or hbm-thermal-enable)");
    }
    if (config_.hbf_topology == PackageThermalConfig::HbfTopology::Cascaded) {
        if (hbf_stacks_ == 0 || hbm_stacks_ == 0) {
            throw std::runtime_error(
                "package-hbf-topology=cascaded needs thermally modeled HBF and HBM stacks");
        }
        const double joule_per_byte = config_.hbm_passthrough_energy_pj_per_bit * 8.0 * 1e-12;
        hbf_->set_thermal_passthrough_sink(
            [this, joule_per_byte](std::size_t stack, double at_ns, std::uint64_t bytes) {
                passthrough_bytes_ += static_cast<double>(bytes);
                hbm_->thermal_deposit_imported(
                    stack % hbm_stacks_, at_ns, static_cast<double>(bytes) * joule_per_byte);
            });
    }
    // The GPU boots at the steady state of its configured power behind the
    // coolant: a package that has been serving, like the HBF ceiling start.
    gpu_boundary_c_ = config_.ambient_c;
    gpu_idle_c_ = config_.ambient_c + config_.gpu_power_w * config_.gpu_resistance_c_per_w;
    gpu_tau_ns_ = config_.gpu_resistance_c_per_w * config_.gpu_capacitance_j_per_c * 1e9;
    gpu_temperature_c_ = gpu_idle_c_;
    gpu_boot_c_ = gpu_idle_c_;
    gpu_peak_c_ = gpu_idle_c_;
    update(0.0);
    next_update_ns_ = config_.update_interval_ns;
}

double PackageThermal::gpu_temperature(double at_ns) const {
    const auto dt_ns = std::max(at_ns, gpu_clock_ns_) - gpu_clock_ns_;
    return gpu_idle_c_ + (gpu_temperature_c_ - gpu_idle_c_) * std::exp(-dt_ns / gpu_tau_ns_);
}

void PackageThermal::update(double at_ns) {
    // Project every node to the update instant under its current environment.
    std::vector<double> hbf_t(hbf_stacks_), hbm_t(hbm_stacks_);
    for (std::size_t i = 0; i < hbf_stacks_; ++i) hbf_t[i] = hbf_->thermal_temperature(i, at_ns);
    for (std::size_t j = 0; j < hbm_stacks_; ++j) hbm_t[j] = hbm_->thermal_temperature(j, at_ns);
    const double gpu_t = gpu_temperature(at_ns);
    gpu_temperature_c_ = gpu_t;
    gpu_clock_ns_ = std::max(gpu_clock_ns_, at_ns);
    gpu_peak_c_ = std::max(gpu_peak_c_, gpu_t);
    double hbf_sum = 0.0, hbm_sum = 0.0;
    for (const auto t : hbf_t) hbf_sum += t;
    for (const auto t : hbm_t) hbm_sum += t;
    const auto& c = config_;
    // HBF stacks.
    for (std::size_t i = 0; i < hbf_stacks_; ++i) {
        const double g_sum = c.coupling_hbf_hbm_w_per_c * static_cast<double>(hbm_stacks_) +
            c.coupling_hbf_gpu_w_per_c +
            c.coupling_hbf_hbf_w_per_c * static_cast<double>(hbf_stacks_ - 1);
        const double conductance = 1.0 / hbf_resistance_ + g_sum;
        const double weighted = c.ambient_c / hbf_resistance_ +
            c.coupling_hbf_hbm_w_per_c * hbm_sum +
            c.coupling_hbf_gpu_w_per_c * gpu_t +
            c.coupling_hbf_hbf_w_per_c * (hbf_sum - hbf_t[i]);
        hbf_->set_thermal_environment(i, weighted / conductance, 1.0 / conductance, at_ns);
    }
    // HBM stacks.
    for (std::size_t j = 0; j < hbm_stacks_; ++j) {
        const double g_sum = c.coupling_hbf_hbm_w_per_c * static_cast<double>(hbf_stacks_) +
            c.coupling_hbm_gpu_w_per_c +
            c.coupling_hbm_hbm_w_per_c * static_cast<double>(hbm_stacks_ - 1);
        const double conductance = 1.0 / hbm_resistance_ + g_sum;
        const double weighted = c.ambient_c / hbm_resistance_ +
            c.coupling_hbf_hbm_w_per_c * hbf_sum +
            c.coupling_hbm_gpu_w_per_c * gpu_t +
            c.coupling_hbm_hbm_w_per_c * (hbm_sum - hbm_t[j]);
        hbm_->set_thermal_environment(j, weighted / conductance, 1.0 / conductance, at_ns);
    }
    // GPU node: its own path to the coolant plus the stack couplings.
    {
        const double g_sum = c.coupling_hbf_gpu_w_per_c * static_cast<double>(hbf_stacks_) +
            c.coupling_hbm_gpu_w_per_c * static_cast<double>(hbm_stacks_);
        const double conductance = 1.0 / c.gpu_resistance_c_per_w + g_sum;
        const double weighted = c.ambient_c / c.gpu_resistance_c_per_w +
            c.coupling_hbf_gpu_w_per_c * hbf_sum + c.coupling_hbm_gpu_w_per_c * hbm_sum;
        gpu_boundary_c_ = weighted / conductance;
        gpu_idle_c_ = gpu_boundary_c_ + c.gpu_power_w / conductance;
        gpu_tau_ns_ = c.gpu_capacitance_j_per_c / conductance * 1e9;
    }
    ++updates_;
}

void PackageThermal::advance_to(double t_ns) {
    while (next_update_ns_ <= t_ns) {
        update(next_update_ns_);
        next_update_ns_ += config_.update_interval_ns;
    }
}

PackageThermalState PackageThermal::state(double at_ns) const {
    PackageThermalState out;
    out.enabled = true;
    out.updates = updates_;
    out.gpu_power_w = config_.gpu_power_w;
    out.gpu_boot_temperature_c = gpu_boot_c_;
    out.gpu_peak_temperature_c = std::max(gpu_peak_c_, gpu_temperature(at_ns));
    out.gpu_final_temperature_c = gpu_temperature(at_ns);
    out.gpu_boundary_c = gpu_boundary_c_;
    out.passthrough_bytes = passthrough_bytes_;
    for (std::size_t i = 0; i < hbf_stacks_; ++i) {
        out.hbf_boundaries_c.push_back(hbf_->thermal_boundary_c(i));
        out.hbf_temperatures_c.push_back(hbf_->thermal_temperature(i, at_ns));
    }
    for (std::size_t j = 0; j < hbm_stacks_; ++j) {
        out.hbm_boundaries_c.push_back(hbm_->thermal_boundary_c(j));
        out.hbm_temperatures_c.push_back(hbm_->thermal_temperature(j, at_ns));
    }
    return out;
}

std::string PackageThermal::json(double at_ns) const {
    const auto s = state(at_ns);
    std::ostringstream out;
    out.precision(17);
    const auto list = [&](const std::vector<double>& values) {
        out << '[';
        for (std::size_t i = 0; i < values.size(); ++i) out << (i ? "," : "") << values[i];
        out << ']';
    };
    out << "{\"enabled\":true,\"topology\":\""
        << (config_.hbf_topology == PackageThermalConfig::HbfTopology::Cascaded ? "cascaded" : "beside")
        << "\",\"ambient_c\":" << config_.ambient_c
        << ",\"update_interval_ns\":" << config_.update_interval_ns
        << ",\"updates\":" << s.updates
        << ",\"gpu_power_w\":" << s.gpu_power_w
        << ",\"gpu_boot_temperature_c\":" << s.gpu_boot_temperature_c
        << ",\"gpu_peak_temperature_c\":" << s.gpu_peak_temperature_c
        << ",\"gpu_final_temperature_c\":" << s.gpu_final_temperature_c
        << ",\"gpu_boundary_c\":" << s.gpu_boundary_c
        << ",\"passthrough_bytes\":" << s.passthrough_bytes
        << ",\"hbf_boundaries_c\":"; list(s.hbf_boundaries_c);
    out << ",\"hbf_temperatures_c\":"; list(s.hbf_temperatures_c);
    out << ",\"hbm_boundaries_c\":"; list(s.hbm_boundaries_c);
    out << ",\"hbm_temperatures_c\":"; list(s.hbm_temperatures_c);
    out << '}';
    return out.str();
}

}  // namespace hbfsim::physical

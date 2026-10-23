#pragma once
#include "physical/physical_types.hpp"
#include "physical/hbm/hbm_config.hpp"
#include "physical/resource_calendar.hpp"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace hbfsim::physical::hbm {

struct HbmConfig {
    // Versioned controller address map (see docs/reference/model.md). The
    // scheme id and the interleave size are recorded in every summary.
    static constexpr std::string_view address_mapping_scheme() {
        return "pch-interleave-bg-rotate-v2";
    }

    static constexpr std::string_view timing_model() { return "channel-aggregate-v2"; }

    static constexpr auto standard = kStandard;
    static constexpr std::uint64_t kDefaultInterleaveBytes = 256;
    HbmDeviceConfig device;
    HbmTimingConfig timing;
    HbmControllerConfig controller;
    HbmThermalConfig thermal;

    [[nodiscard]] std::uint64_t pseudo_channel_width_bits() const;
    [[nodiscard]] std::uint64_t row_size_bytes() const;
    [[nodiscard]] std::uint64_t burst_bytes() const;
    // The interleave the map actually uses: interleave_bytes when it is
    // explicit and legal (throws otherwise), else the auto default above.
    [[nodiscard]] std::uint64_t effective_interleave_bytes() const;
    [[nodiscard]] double channel_bandwidth_GBps() const;
    [[nodiscard]] double pseudo_channel_bandwidth_GBps() const;
    [[nodiscard]] double command_clock_period_ns() const;
    [[nodiscard]] double command_clock_MHz() const;
    [[nodiscard]] double burst_duration_ns() const;
    [[nodiscard]] std::uint64_t command_clock_cycles(double time_ns) const;
    [[nodiscard]] double command_clock_time_ns(std::uint64_t cycles) const;
    [[nodiscard]] double command_aligned_time_ns(double time_ns) const;
};

struct HbmAddress {
    std::uint32_t stack = 0;
    std::uint32_t channel = 0;
    std::uint32_t pseudo_channel = 0;
    std::uint32_t bank_group = 0;
    std::uint32_t bank = 0;
    std::uint64_t row = 0;
    std::uint64_t offset = 0;

    [[nodiscard]] std::string path() const;
};

struct HbmStats {
    std::uint64_t read_bytes = 0;
    std::uint64_t write_bytes = 0;
    std::uint64_t controller_buffer_read_bytes = 0;
    std::uint64_t controller_buffer_write_bytes = 0;
    std::uint64_t controller_buffer_transfers = 0;
    double controller_buffer_bus_busy_ns = 0.0;
    // Payload bus time is exact burst-rounded bytes / raw interface bandwidth.
    // Service time additionally includes the effective-bandwidth overhead.
    double bus_busy_ns = 0.0;
    double service_busy_ns = 0.0;
    // Executed service-group reservations, including controller DMA. A
    // coalesced reservation can represent multiple lane-service quanta.
    std::uint64_t channel_transfers = 0;
    // Group quanta represented, independent of safe coalescing.
    std::uint64_t service_quanta = 0;
    double first_arrival_ns = std::numeric_limits<double>::infinity();
    double finish_ns = 0.0;
    std::uint64_t pseudo_channels = 0;
    std::uint64_t active_pseudo_channels = 0;
    std::uint64_t max_pseudo_channel_accesses = 0;
    std::uint64_t max_queue_occupancy = 0;
    double max_pseudo_channel_busy_ns = 0.0;
    double avg_active_pseudo_channel_busy_ns = 0.0;
    std::uint64_t retained_bus_gaps = 0;
    // Sum over channel transfers; parallel channels may overlap in wall time.
    Breakdown stage_work;
    // Thermal telemetry of the per-stack RC nodes; all zero while disabled.
    bool thermal_enabled = false;
    // Energy deposited by this device's own reads and writes.
    double thermal_energy_j = 0.0;
    // Energy deposited by traffic that only passes through a stack's base
    // die (an HBF stack cascaded behind it).
    double thermal_imported_energy_j = 0.0;
    // Stack-time spent above the refresh-doubling and limit temperatures.
    double thermal_above_refresh_span_ns = 0.0;
    double thermal_above_limit_span_ns = 0.0;
    double thermal_boundary_temperature_c = 0.0;
    double thermal_boot_temperature_c = 0.0;
    // Projections to finish_ns, refreshed by stats().
    double thermal_peak_temperature_c = 0.0;
    double thermal_final_temperature_c = 0.0;
    // Hottest effective boundary a package model imposed, at finish_ns.
    double thermal_effective_boundary_c = 0.0;
    std::uint64_t thermal_stacks_above_refresh = 0;

    [[nodiscard]] double active_span_ns() const;
    [[nodiscard]] double utilization() const;
    [[nodiscard]] double bus_parallelism() const;
    [[nodiscard]] double pseudo_channel_busy_skew() const;
};

class HbmDevice {
public:
    explicit HbmDevice(HbmConfig config, AddressHeatmap* address_heatmap = nullptr);
    [[nodiscard]] HbmAddress decode(std::uint64_t addr) const;
    [[nodiscard]] std::uint64_t encode(const HbmAddress& addr) const;
    [[nodiscard]] PhysicalCompletion issue(const PhysicalRequest& request);
    void reserve_controller_buffer(std::uint64_t bytes);
    [[nodiscard]] std::uint64_t controller_buffer_bytes() const { return controller_buffer_bytes_; }
    [[nodiscard]] std::uint64_t application_capacity_bytes() const {
        return config_.device.capacity_bytes - controller_buffer_bytes_;
    }
    [[nodiscard]] PhysicalCompletion transfer_controller_buffer(const PhysicalRequest& request);
    // Only the joint system frontier may reclaim shared bus history.
    void advance_buffer_frontier(double at_ns);
    [[nodiscard]] std::uint64_t enqueue(const PhysicalRequest& request);
    [[nodiscard]] PhysicalCompletion pump(std::uint64_t ticket);
    // Start service strictly before the boundary. At most one quantum per
    // service group can cross it. Stop at a parent completion so newly unlocked
    // HBF/compute work can participate before further HBM service.
    [[nodiscard]] bool service_before(double arrival_ns);
    [[nodiscard]] std::vector<std::pair<std::uint64_t, PhysicalCompletion>> take_completions();
    void drain_queues();
    [[nodiscard]] const HbmStats& stats() const { refresh_parallel_stats(); return stats_; }
    [[nodiscard]] HbmStats execution_stats() const { return stats_; }
    [[nodiscard]] const HbmConfig& config() const { return config_; }
    // Per-stack thermal nodes (HbmThermalConfig). A package model reads
    // projected copies and replaces a stack's boundary and resistance at its
    // update instants; the node is advanced to that instant first, so the
    // environment switch never rewrites history.
    [[nodiscard]] std::size_t thermal_stack_count() const { return thermal_nodes_.size(); }
    [[nodiscard]] double thermal_temperature(std::size_t stack, double at_ns) const;
    [[nodiscard]] double thermal_boundary_c(std::size_t stack) const;
    void set_thermal_environment(
        std::size_t stack, double boundary_c, double resistance_c_per_w, double at_ns);
    // Package boot: an untouched stack takes the temperature the package's
    // idle steady state gives it. Fails closed once the stack has served.
    void thermal_boot_at(std::size_t stack, double temperature_c);
    // Heat that is not this device's traffic: the base-die PHY energy of
    // bytes an HBF stack moves through this stack in a cascaded package.
    void thermal_deposit_imported(std::size_t stack, double at_ns, double energy_j);
    void attach_address_heatmap(AddressHeatmap& address_heatmap) {
        if (last_enqueue_arrival_ns_ || stats_.controller_buffer_transfers) {
            throw std::runtime_error("HBM address heatmap must be attached before the first request");
        }
        address_heatmap_ = &address_heatmap;
    }

private:
    struct ChannelTransfer {
        std::uint64_t ticket = 0;
        std::uint64_t remaining_bursts = 0;
        std::uint64_t total_bursts = 0;
        std::uint64_t service_cycles = 0;
        std::optional<std::uint64_t> first_start;
    };
    struct ServiceGroupState {
        ResourceTimeline data_bus_cycles;
        std::deque<ChannelTransfer> queue;
        std::uint64_t floor_cycle = 0;
        std::optional<Op> last_op;
        std::uint64_t next_start = 0;
    };
    struct PendingRequest {
        PhysicalCompletion completion;
        TraceConfig trace;
        std::uint64_t ready_cycle = 0;
        std::size_t remaining_channels = 0;
        std::size_t critical_channel = std::numeric_limits<std::size_t>::max();
        bool admission_complete = false;
        std::vector<std::uint64_t> channel_bursts;
    };
    struct LocalAddress {
        std::uint32_t bank_group = 0;
        std::uint32_t bank = 0;
        std::uint64_t row = 0;
        std::uint64_t column_unit = 0;
    };
    HbmConfig config_;
    AddressHeatmap* address_heatmap_ = nullptr;
    std::uint64_t row_size_bytes_ = 0;
    std::uint64_t burst_bytes_ = 0;
    std::uint64_t units_per_row_ = 0;
    std::uint64_t stripe_bytes_ = 0;
    std::uint64_t total_pseudo_channels_ = 0;
    std::uint64_t pseudo_channels_per_stack_ = 0;
    double command_clock_period_ns_ = 0.0;
    std::uint64_t burst_cycles_ = 0;
    std::uint64_t quantum_bursts_ = 0;
    std::uint64_t quantum_cycles_ = 0;
    std::uint64_t read_latency_cycles_ = 0;
    std::uint64_t write_latency_cycles_ = 0;
    std::uint64_t read_to_write_cycles_ = 0;
    std::uint64_t write_to_read_cycles_ = 0;
    std::vector<ServiceGroupState> service_groups_;
    std::size_t service_group_width_ = 1;
    // Indexed tournament: exactly one candidate per service group. An
    // updated DMA preview replaces its leaf, without stale heap entries.
    static constexpr auto kNoChannel = std::numeric_limits<std::size_t>::max();
    std::size_t event_leaves_ = 1;
    std::vector<std::size_t> event_tree_;
    std::vector<std::uint64_t> pseudo_channel_accesses_;
    std::vector<std::uint64_t> pseudo_channel_bus_busy_cycles_;
    std::uint64_t bus_busy_cycles_ = 0;
    std::uint64_t service_busy_cycles_ = 0;
    mutable HbmStats stats_;
    std::uint64_t next_ticket_ = 0;
    std::uint64_t controller_buffer_bytes_ = 0;
    std::uint64_t buffer_frontier_cycle_ = 0;
    std::vector<std::uint64_t> grouped_bursts_;
    std::vector<std::size_t> grouped_channels_;
    std::vector<std::size_t> request_groups_;
    std::vector<std::uint64_t> request_group_bursts_;
    std::unordered_map<std::uint64_t, PendingRequest> pending_;
    std::unordered_map<std::uint64_t, PhysicalCompletion> completed_;
    std::optional<double> last_enqueue_arrival_ns_;
    struct ThermalNode {
        double temperature_c = 0.0;
        double clock_ns = 0.0;
        double peak_c = 0.0;
        double boundary_c = 0.0;
        double resistance_c_per_w = 0.0;
        double idle_c = 0.0;
        double tau_ns = 0.0;
    };
    std::vector<ThermalNode> thermal_nodes_;
    double thermal_read_energy_j_per_byte_ = 0.0;
    double thermal_write_energy_j_per_byte_ = 0.0;
    void thermal_advance(ThermalNode& node, double t_ns, bool record) const;
    void thermal_deposit(std::size_t stack, double at_ns, double energy_j, bool imported);

    [[nodiscard]] LocalAddress local_address(std::uint64_t unit) const;
    void assign_pseudo_channel(HbmAddress& addr, std::size_t pseudo_channel) const;
    [[nodiscard]] std::uint64_t byte_address(std::size_t pseudo_channel, std::uint64_t unit,
                                           std::uint64_t unit_offset) const;
    [[nodiscard]] double ns(std::uint64_t cycles) const;
    [[nodiscard]] std::uint64_t service_cycles(std::uint64_t bursts) const;
    [[nodiscard]] std::uint64_t latency_cycles(Op op) const;
    void validate_request(const PhysicalRequest& request, bool controller) const;
    void group_request(const PhysicalRequest& request);
    void record_heatmap(const PhysicalRequest& request, bool controller);
    void schedule_channel(std::size_t index);
    void update_event(std::size_t index);
    void advance_event_frontier();
    void service_channel(std::size_t index, double boundary_ns, bool sole_parent);
    void complete_parent(std::uint64_t ticket);
    void account_bus(std::size_t index, Op op, std::uint64_t bursts, std::uint64_t cycles, double at_ns);
    void refresh_parallel_stats() const;
};

} // namespace hbfsim::physical::hbm

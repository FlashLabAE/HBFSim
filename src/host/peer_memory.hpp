#pragma once

#include "physical/physical_types.hpp"

#include <cstdint>
#include <deque>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace hbfsim::host {

// A controller policy, independent of request traces and compute annotations.
struct PeerWritebackConfig {
    bool enabled = true;
    double high_watermark = 0.90;
    double low_watermark = 0.80;
    double cold_ns = 1000000.0;
};

struct PeerMemoryLayout {
    bool kv_first = false;
    std::uint64_t hbm_bytes = 0;
    std::uint64_t logical_bytes = 0;
    std::uint64_t kv_begin = 0;
    std::uint64_t unit_bytes = 0;
    std::uint64_t transfer_bytes = 0;
    // Initialized logical ranges only; unused virtual capacity owns no slots.
    std::vector<std::pair<std::uint64_t, std::uint64_t>> initialized;
};

enum class PeerIoTarget { Hbm, Hbf, ToHbf, ToHbm, Invalidate };
struct PeerIo {
    PeerIoTarget target;
    physical::Op op;
    std::uint64_t address;
    std::uint64_t bytes;
    bool background = false;
};

struct PeerMemoryStats {
    std::uint64_t hbm_units = 0, hbf_units = 0, migrating_units = 0;
    std::uint64_t hbm_weight_bytes = 0, hbm_kv_bytes = 0, live_bytes = 0;
    std::uint64_t background_demotions = 0, forced_demotions = 0, promotions = 0;
    std::uint64_t background_bytes = 0, forced_bytes = 0, promoted_bytes = 0;
    std::uint64_t released_bytes = 0, released_hbm_bytes = 0;
    std::uint64_t weight_hbm_read_bytes = 0, weight_hbf_read_bytes = 0;
    std::uint64_t kv_hbm_read_bytes = 0, kv_hbf_read_bytes = 0;
    std::uint64_t kv_hbm_write_bytes = 0, kv_hbf_write_bytes = 0;
    std::uint64_t idle_starts = 0;
    std::uint64_t last_background_address = 0;
    double last_background_start_ns = 0.0;
};

// The session supplies asynchronous physical IO and calls background() only
// when foreground dispatch and the native HBF controller are idle. Homes move
// at copy completion; a source slot is never reused while its copy is in flight.
class PeerMemoryController {
public:
    using Done = std::function<void(double)>;
    using Issue = std::function<void(PeerIo, double, Done)>;
    PeerMemoryController(PeerMemoryLayout layout, PeerWritebackConfig policy,
                         std::uint64_t hbf_bytes, Issue issue);
    void access(physical::Op op, std::uint64_t address, std::uint64_t bytes,
                double at, Done done);
    void release(std::uint64_t address, std::uint64_t bytes, double at, Done done);
    [[nodiscard]] std::optional<double> background_ready(double at);
    bool background(double at);
    [[nodiscard]] const PeerMemoryLayout& layout() const { return layout_; }
    [[nodiscard]] const PeerMemoryStats& stats() const { return stats_; }
    [[nodiscard]] std::uint64_t initial_hbf_bytes() const { return next_hbf_ * layout_.unit_bytes; }
    [[nodiscard]] std::vector<std::pair<std::uint64_t,std::uint64_t>> initial_hbf_holes() const;
    [[nodiscard]] bool active() const { return stats_.migrating_units != 0; }
    [[nodiscard]] std::string snapshot_json() const;
    void validate_access(physical::Op op, std::uint64_t address, std::uint64_t bytes) const;

private:
    using Range = std::pair<std::uint64_t, std::uint64_t>;
    struct Work {
        physical::Op op;
        std::uint64_t begin, end;
        double at;
        Done done;
        bool release = false;
    };
    struct Unit {
        std::uint64_t id = 0, slot = 0;
        bool weight = false, hbm = false, homed = false, busy = false, waiting = false;
        double touched = 0.0;
        std::vector<Range> valid;
        std::deque<Work> work;
        std::list<std::uint64_t>::iterator lru;
    };
    PeerMemoryLayout layout_;
    PeerWritebackConfig policy_;
    Issue issue_;
    PeerMemoryStats stats_;
    std::uint64_t hbm_slots_, hbf_slots_, next_hbm_ = 0, next_hbf_ = 0;
    std::vector<std::uint64_t> free_hbm_, free_hbf_;
    std::map<std::uint64_t, std::shared_ptr<Unit>> units_;
    std::list<std::uint64_t> kv_lru_, weight_lru_;
    std::deque<std::shared_ptr<Unit>> waiting_;
    bool background_pressure_ = false;
    std::uint64_t background_active_ = 0;
    std::uint64_t pending_hbm_frees_ = 0;
    bool waking_ = false;
    static void add_range(std::vector<Range>& ranges, Range range);
    static std::uint64_t size(const std::vector<Range>& ranges);
    static bool covers(const std::vector<Range>& ranges, Range range);
    std::uint64_t take_slot(bool hbm);
    bool has_slot(bool hbm) const;
    void touch(const std::shared_ptr<Unit>& unit, double at);
    void home(const std::shared_ptr<Unit>& unit, bool hbm, std::uint64_t slot);
    void unhome(const std::shared_ptr<Unit>& unit);
    std::shared_ptr<Unit> victim(const std::list<std::uint64_t>& lru) const;
    void pump(const std::shared_ptr<Unit>& unit, double at);
    void wake(double at);
    void move(const std::shared_ptr<Unit>& unit, bool to_hbm, bool background,
              double at, Done done = {});
    void copy(const std::shared_ptr<Unit>& unit, bool to_hbm, bool background,
              std::uint64_t destination, double at, Done done);
};

} // namespace hbfsim::host

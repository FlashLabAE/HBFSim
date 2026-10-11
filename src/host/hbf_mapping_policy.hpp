#pragma once

#include "host/hbf_config.hpp"
#include "physical/physical_types.hpp"
#include "physical/resource_calendar.hpp"
#include <map>
#include <optional>
#include <set>
#include <utility>
#include <vector>

namespace hbfsim::host {
class HbfController;
struct HbfLogicalInvalidationResult;

struct HbfMappingPolicyStats {
    std::uint64_t metadata_bytes = 0, peak_metadata_bytes = 0, reserved_bytes = 0;
    std::uint64_t data_programs = 0, copy_programs = 0, padding_programs = 0;
    std::uint64_t metadata_programs = 0, metadata_reads = 0, data_reads = 0, copy_reads = 0;
    std::uint64_t switch_merges = 0, partial_merges = 0, full_merges = 0, cleaned_segments = 0;
    // Log merges forced because the mapping memory could not hold another record.
    std::uint64_t memory_merges = 0;
    // Full pages a block organization kept in device DRAM until the pages before them were written.
    std::uint64_t prefix_holds = 0;
    std::uint64_t checkpoints = 0, buffer_flushes = 0, buffer_hits = 0;
    std::uint64_t object_creates = 0, object_seals = 0, object_deletes = 0;
    std::uint64_t extent_records = 0, active_logs = 0, memory_bytes = 0;
    std::uint64_t metadata_hbm_bytes = 0, lookups = 0, updates = 0;
    std::uint64_t reverse_scan_records = 0;
    double lookup_work_ns = 0;
};

// Ordered lossless LPN -> PPN runs, used as the authoritative primary index.
// Assign/erase split intersected runs; adjacent affine runs merge immediately.
class HbfExtentIndex {
public:
    struct Run { std::uint64_t pages, ppn; };
    [[nodiscard]] std::optional<std::uint64_t> lookup(std::uint64_t lpn) const;
    void assign(std::uint64_t lpn, std::uint64_t ppn);
    void erase(std::uint64_t first, std::uint64_t count);
    [[nodiscard]] const std::map<std::uint64_t, Run>& runs() const { return runs_; }
private:
    std::map<std::uint64_t, Run> runs_;
};

class HbfMappingPolicy {
public:
    // Resolve one implementation path: page policies retain the existing
    // engine; structural policies use its physical scheduler and own FTL.
    [[nodiscard]] static HbfConfig media_config(HbfConfig config);
    // The lane that position `lane` of allocation round `round` takes among
    // `lanes` single-plane lanes (or, inverse, the position of a lane).
    [[nodiscard]] static std::uint64_t shuffled_lane(std::uint64_t round, std::uint64_t lane,
                                                     std::uint64_t lanes, bool inverse);
    explicit HbfMappingPolicy(const HbfConfig& requested, const HbfConfig& media);
    [[nodiscard]] std::uint64_t capacity_pages() const { return logical_pages_; }
    [[nodiscard]] const HbfMappingPolicyStats& stats() const { return stats_; }
    [[nodiscard]] std::pair<std::uint64_t,std::uint64_t> allocated_pages() const;
    [[nodiscard]] std::size_t buffered_pages() const { return buffer_.size(); }
    [[nodiscard]] bool dirty() const { return dirty_ || !buffer_.empty(); }
    // Physical page holding lpn (superblock pages are striped over planes).
    [[nodiscard]] std::optional<std::uint64_t> lookup(std::uint64_t lpn) const;
    [[nodiscard]] std::optional<std::uint64_t> generation(std::uint64_t lpn) const;
    [[nodiscard]] physical::PhysicalCompletion issue(HbfController&, const physical::PhysicalRequest&);
    [[nodiscard]] physical::PhysicalCompletion checkpoint(HbfController&, std::string id,
        double arrival_ns, physical::TraceConfig trace);
    [[nodiscard]] HbfLogicalInvalidationResult invalidate(HbfController&,
        std::uint64_t first, std::uint64_t count, double at_ns);
    [[nodiscard]] physical::PhysicalCompletion object_command(HbfController&, std::string_view action,
        std::uint64_t id, std::uint64_t first, std::uint64_t count, double at_ns);
    [[nodiscard]] std::vector<std::uint64_t> snapshot() const;
    void restore(HbfController&, const std::vector<std::uint64_t>& words);
    void prepopulate(HbfController&, const std::vector<std::uint64_t>& lpns, bool read_only = false);
    void audit(const HbfController&) const;
private:
    using U = std::uint64_t;
    static constexpr U absent = ~U{0};
    enum class Purpose { Data, Copy, Padding, Metadata };
    struct Tag { U lpn = absent, generation = 0; };
    // appended_ns: when the latest program issued into this block had its
    // payload assembled; later slots start no earlier (NAND append order).
    struct Block { bool allocated = false; U owner = absent; std::vector<Tag> pages; double appended_ns = 0; };
    struct BlockMap { U base = absent, log = absent, touched = 0; std::vector<U> valid; std::map<U,U> updates; };
    struct Object { U first, count; bool sealed = false; };
    // stack: the write-buffer partition holding the page (stack_of).
    // ready_ns: when the complete page (payload merged over any old version)
    // is in controller memory; its program cannot start earlier.
    // written: the page's byte ranges the host wrote since it was buffered;
    // a device-DRAM buffer programs the page once they cover all of it.
    struct Buffered { U generation = 0; U touched = 0; std::size_t stack = 0; double ready_ns = 0;
        std::vector<std::pair<U,U>> written; };
    // A program the host has issued whose completion it has not yet applied:
    // the native page (and its payload generation) exists only once it has
    // completed, and a version superseded meanwhile is invalidated then.
    struct InFlight { double finish_ns; U generation; bool retire = false; };
    HbfConfig requested_;
    MappingOrganization organization_;
    // Blocks, pages and PPNs of this policy are superblock units: width_
    // physical blocks at one offset on consecutive allocation lanes. Page o
    // is row o / width_ of its lane (o % width_ + row_rotation) % width_: every
    // row puts one page on each lane, so each member block is programmed in
    // order, and the row's starting lane is mixed per superblock and row so
    // logical strides that are multiples of width_ (one layer's KV of every
    // slot) spread over all lanes. With width_ == 1 they are the physical
    // blocks and pages.
    U width_, physical_pages_per_block_;
    U pages_per_block_, total_blocks_, logical_pages_, metadata_blocks_, data_blocks_;
    U planes_, planes_per_stack_;
    // Common block state (per-block records and page validity bits), which
    // is reported but reserved outside the mapping budget.
    U block_state_bytes_;
    U open_ = absent, allocation_rank_cursor_ = 0, sequence_ = 0, checkpoint_generation_ = 0;
    U checkpoint_slot_ = absent, checkpoint_pages_ = 0;
    bool dirty_ = false, collecting_ = false, seeding_ = false, read_only_ = false;
    std::size_t memory_stack_ = 0;
    double ready_ns_ = 0, last_arrival_ns_ = 0;
    physical::TraceConfig trace_;
    std::vector<Block> blocks_;
    // One free index of allocation ranks: lanes interleave stacks, then planes
    // within a stack, then block offsets. Metadata blocks are absent ranks.
    std::set<U> free_;
    std::map<U,BlockMap> block_map_;
    U log_update_entries_ = 0, active_log_count_ = 0;
    HbfExtentIndex extents_;
    std::map<U,Object> objects_;
    std::map<U,U> object_ranges_;
    std::map<U,Buffered> buffer_;
    // Per stack, its buffered pages as (touched, lpn): the first is the page
    // written least recently.
    std::vector<std::set<std::pair<U,U>>> buffer_order_;
    // The host executes commands in order (ready_ns_); NAND work runs on its
    // planes. inflight_ is keyed by physical page, program_credits_ holds each
    // channel's issued-program completions (a min-heap bounded by the device's
    // outstanding-program pages per channel), and media_ns_/data_ns_ are the
    // current command's latest media and data-program completions.
    std::map<U,InFlight> inflight_;
    std::multimap<double,U> inflight_by_finish_;
    std::vector<std::vector<double>> program_credits_;
    // Released blocks whose programs are still in flight, with the finish of
    // their last program: they rejoin the free pool when it completes.
    std::map<U,double> releasing_;
    std::multimap<double,U> releasing_by_finish_;
    // Index lookups and updates use the same host resources as the page FTL:
    // a shared pool of mapping workers and one controller-memory issue slot
    // per stack. Issuing advances the ordered host stream; completion does not.
    std::vector<physical::ResourceTimeline> lookup_workers_, lookup_issue_;
    // update_ns_: latest posted index update of the current command.
    double media_ns_ = 0, data_ns_ = 0, update_ns_ = 0;
    HbfMappingPolicyStats stats_;
    [[nodiscard]] U memory_footprint() const;
    // Bytes charged to the mapping budget: the footprint without the common
    // block state, plus the write buffer and merge workspace.
    [[nodiscard]] U mapping_bytes() const;
    // Merges the least recently written logs other than logical block
    // keep's until the mapping budget can hold `bytes` more.
    void reclaim(HbfController&, U keep, U bytes, physical::PhysicalCompletion&);
    [[nodiscard]] U buffer_stack_pages(std::size_t stack) const;
    // Buffers the host bytes [begin, end) of lpn's page (offsets within the
    // page), new or already buffered, and returns whether the bytes written
    // since it was buffered now cover the whole page.
    bool buffer_write(HbfController&, U lpn, std::size_t stack, U generation, double ready_ns, U begin, U end);
    void buffer_drop(HbfController&, std::map<U,Buffered>::iterator);
    // Write-buffer pages live in HBF device DRAM (coalescing with device
    // DRAM enabled) on their LPN's stack, the buffer's partition.
    [[nodiscard]] bool buffer_on_device(const HbfController&) const;
    // The stack owning lpn's host-side state (write-buffer slot, index
    // memory, erased-page response): the page FTL's scrambled lane, so a
    // power-of-two stride (KV rows at even block offsets) spreads over the
    // stacks instead of pinning to lpn % stacks.
    [[nodiscard]] std::size_t stack_of(const HbfController&, U lpn) const;
    void update_footprint();
    void start(HbfController&, double arrival);
    void settle(HbfController&, double through_ns);
    void wait_until(double at_ns, physical::PhysicalCompletion&);
    void memory(HbfController&, U bytes, physical::Op, physical::PhysicalCompletion&);
    [[nodiscard]] double memory_at(HbfController&, U bytes, physical::Op, double at_ns,
        physical::Breakdown&, std::vector<physical::TraceSpan>*);
    [[nodiscard]] double merge_read(HbfController&, U page, double at_ns, physical::HeatmapTrafficSource,
        physical::Breakdown&, std::vector<physical::TraceSpan>*);
    void read_range(HbfController&, const physical::PhysicalRequest&, physical::PhysicalCompletion&);
    void read_page(HbfController&, U ppn, physical::PhysicalCompletion&);
    [[nodiscard]] double copy_read(HbfController&, U page, double at_ns, physical::Breakdown&,
        std::vector<physical::TraceSpan>*);
    void copy(HbfController&, U source, U destination, physical::PhysicalCompletion&);
    void program(HbfController&, U ppn, Tag, Purpose, physical::PhysicalCompletion&, double data_ready_ns = 0);
    void retire(HbfController&, U ppn);
    void release(HbfController&, U block, physical::PhysicalCompletion&);
    // Returns a released block, all of whose programs have finished, to the free pool.
    void reclaim_block(HbfController&, U block);
    [[nodiscard]] U lane_rank(U physical_block) const;
    [[nodiscard]] U block_at_lane_rank(U rank) const;
    [[nodiscard]] U allocation_rank(U block) const;
    [[nodiscard]] U block_at_allocation_rank(U rank) const;
    [[nodiscard]] U member(U block, U lane) const;
    [[nodiscard]] U row_rotation(U block, U row) const;
    [[nodiscard]] U physical(U ppn) const;
    [[nodiscard]] U member_prefix(U block, U pages, U lane) const;
    [[nodiscard]] U wear(const HbfController&, U block) const;
    [[nodiscard]] std::optional<U> locate(U lpn) const;
    [[nodiscard]] U allocate(HbfController&, physical::PhysicalCompletion&, bool reserve = false);
    void clean_segment(HbfController&, physical::PhysicalCompletion&);
    void merge(HbfController&, U logical_block, physical::PhysicalCompletion&);
    void store(HbfController&, U lpn, U generation, physical::PhysicalCompletion&, double data_ready_ns);
    void store_block(HbfController&, U first_lpn, physical::PhysicalCompletion&);
    void flush(HbfController&, U lpn, physical::PhysicalCompletion&);
    // Block organizations program a block's pages in order: a full page whose
    // block still has unwritten pages before it waits in device DRAM rather
    // than padding them with zeros (awaits_prefix), and is programmed once the
    // block's written prefix reaches it (flush_waiting).
    [[nodiscard]] bool awaits_prefix(U lpn) const;
    void flush_waiting(HbfController&, U logical_block, physical::PhysicalCompletion&);
    void evict(HbfController&, std::size_t stack, physical::PhysicalCompletion&);
    void flush_all(HbfController&, physical::PhysicalCompletion&);
    void lookup_work(HbfController&, U lpn, bool update, physical::PhysicalCompletion&);
    [[nodiscard]] double index_access(HbfController&, U lpn, bool update, physical::Breakdown&,
        std::vector<physical::TraceSpan>*, bool resolved);
    void lookup_compute(HbfController&, double work_ns, double& at_ns, physical::Breakdown&);
    [[nodiscard]] U index_entry(U lpn) const;
    [[nodiscard]] U object_for(U lpn) const;
    void finish(HbfController&, physical::PhysicalCompletion&, U before_bytes);
};
} // namespace hbfsim::host

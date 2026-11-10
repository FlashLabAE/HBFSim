// Plane placement of installed and live-written data under periodic strides.
//
// Installed images:
//
// A KV slot is 80 layers x 1 MiB (dense 70B) or 32 layers x 1 MiB (dense
// 8B), so the slot stride of 20480 or 8192 pages is a multiple of stacks x
// 256 pages at 2 and 4 HBF stacks with 256 planes per stack. A layout that
// put a stack's i-th page on plane i % 256 then read every layer's piece of
// every slot from the same 64 (4 stacks) or 128 (2 stacks) planes, and from
// ~128 at 6 stacks. With each stack-local round of pages starting on a mixed
// plane, a layer's pieces over 1,000 consecutive slots must reach every
// plane of every stack and load the busiest plane close to the mean.
//
// Live writes: a decode step appends one KV page per (sequence, layer) in the
// same order every step, so a stream returns to a stack after a fixed number
// of that stack's pages. With batch x layers a multiple of 256 (32 x 32,
// 64 x 32, 16 x 80) a bare per-stack round-robin put every page of a stream
// on one plane of each stack, and reading the stream back was serial. Live
// pages take the same rotated rounds as installed ones, so a stream's pages
// must spread over the planes of every stack.
#include "../../verification/probes/hbf_with_hbm.hpp"
#include <algorithm>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

using namespace hbfsim::host;
using namespace hbfsim::physical;
using Device = hbfsim::verification::HbfWithHbm;
using U = std::uint64_t;

static void require(bool ok, const std::string& message) {
    if (!ok) throw std::runtime_error(message);
}

int main() {
    try {
        constexpr U piece_pages = 256, slots = 1000;
        constexpr unsigned planes_per_stack = 256;
        for (const auto& [model, layers] : {std::pair{"dense70b", 80u}, std::pair{"dense8b", 32u}}) {
            const U slot_pages = layers * piece_pages;
            const U pages = slots * slot_pages;
            for (const unsigned stacks : {2u, 4u, 6u}) {
                HbfConfig c;
                c.device.stacks = stacks;
                c.device.channels_per_stack = 16;
                c.device.dies_per_channel = 1;
                c.device.planes_per_die = 16;
                c.device.pages_per_block = 256;
                // The image plus its mapping pages, the GC reserve and open frontiers.
                c.device.blocks_per_plane = static_cast<std::uint32_t>(
                    (pages / stacks + U{planes_per_stack} * 256 - 1) /
                    (U{planes_per_stack} * 256) + 8);
                c.host.auto_gc_enabled = false;
                Device d(c);
                d.prepopulate_mutable_logical_page_range(0, pages);
                const U pages_per_plane = U{c.device.blocks_per_plane} * c.device.pages_per_block;

                // Every plane of every stack received the same number of data
                // blocks to within one, as the unrotated round-robin did.
                std::vector<U> data_blocks(stacks * planes_per_stack, 0);
                for (const auto& block : d.audit_snapshot().blocks)
                    if (block.role == "data") ++data_blocks.at(block.block / c.device.blocks_per_plane);
                const auto [fewest, most] = std::minmax_element(data_blocks.begin(), data_blocks.end());
                require(*most > 0 && *most - *fewest <= 1,
                    std::string(model) + ": compact image unbalanced its planes");

                for (const unsigned layer : {0u, 1u, layers - 1}) {
                    std::vector<U> load(stacks * planes_per_stack, 0);
                    for (U slot = 0; slot < slots; ++slot) {
                        const U first = slot * slot_pages + layer * piece_pages;
                        for (U offset = 0; offset < piece_pages; ++offset) {
                            const auto ppn = d.mapped_physical_page(first + offset);
                            require(ppn.has_value(), "compact image did not map a KV page");
                            ++load.at(static_cast<std::size_t>(*ppn / pages_per_plane));
                        }
                    }
                    const double mean = static_cast<double>(slots * piece_pages) / stacks / planes_per_stack;
                    for (unsigned stack = 0; stack < stacks; ++stack) {
                        const auto first = load.begin() + stack * planes_per_stack;
                        const auto last = first + planes_per_stack;
                        const auto distinct = std::count_if(first, last, [](U n) { return n != 0; });
                        const auto busiest = *std::max_element(first, last);
                        std::cout << model << " stacks=" << stacks << " layer=" << layer
                            << " stack=" << stack << " distinct_planes=" << distinct
                            << " busiest_plane_pages=" << busiest << " mean=" << mean << '\n';
                        require(distinct == planes_per_stack,
                            std::string(model) + ": a layer's KV pieces alias onto a plane subset");
                        require(busiest <= 1.25 * mean,
                            std::string(model) + ": a layer's KV pieces overload one plane");
                    }
                }
                require(d.stats().accounting_verified, "compact image failed its accounting audit");
            }
        }

        // Live appends: `streams` = batch x layers pages per step, written in
        // the same order every step, with nothing installed before them.
        constexpr U steps = 192;
        for (const auto& [name, streams] : {std::pair{"32x32", U{1024}}, std::pair{"16x80", U{1280}},
                                             std::pair{"8x94", U{752}}}) {
            for (const unsigned stacks : {2u, 6u}) {
                HbfConfig c;
                c.device.stacks = stacks;
                c.device.channels_per_stack = 16;
                c.device.dies_per_channel = 1;
                c.device.planes_per_die = 16;
                c.device.pages_per_block = 256;
                c.device.blocks_per_plane = static_cast<std::uint32_t>(
                    (streams * steps / stacks + U{planes_per_stack} * 256 - 1) /
                    (U{planes_per_stack} * 256) + 8);
                c.host.auto_gc_enabled = false;
                Device d(c);
                const U page_bytes = c.device.page_size_bytes;
                const U pages_per_plane = U{c.device.blocks_per_plane} * c.device.pages_per_block;
                // Stream s owns the logical pages [s * steps, (s + 1) * steps).
                double at_ns = 0.0;
                for (U step = 0; step < steps; ++step) {
                    for (U stream = 0; stream < streams; ++stream) {
                        (void)d.issue(PhysicalRequest{
                            .id = "append",
                            .tier = Tier::HBF,
                            .op = Op::Write,
                            .address_space = AddressSpace::Logical,
                            .trace = TraceConfig{.mode = TraceMode::Off, .retain_completion_diagnostics = false},
                            .arrival_ns = at_ns,
                            .addr = (stream * steps + step) * page_bytes,
                            .bytes = page_bytes,
                        });
                        at_ns += 100.0;
                    }
                }
                (void)d.drain_pending("drain", d.execution_stats().finish_ns);

                // Planes that hold a stream's pages on each stack, for a few streams.
                for (const U stream : {U{0}, U{1}, streams / 2, streams - 1}) {
                    std::vector<std::map<U, U>> planes(stacks);
                    for (U step = 0; step < steps; ++step) {
                        const auto ppn = d.mapped_physical_page(stream * steps + step);
                        require(ppn.has_value(), "a live append is not mapped");
                        const U plane = *ppn / pages_per_plane;
                        ++planes.at(static_cast<std::size_t>(plane / planes_per_stack))[plane % planes_per_stack];
                    }
                    for (unsigned stack = 0; stack < stacks; ++stack) {
                        U pages = 0;
                        for (const auto& [plane, count] : planes[stack]) pages += count;
                        if (pages < 8) continue;
                        const U distinct = planes[stack].size();
                        std::cout << "live " << name << " stacks=" << stacks << " stream=" << stream
                            << " stack=" << stack << " pages=" << pages << " distinct_planes=" << distinct << '\n';
                        // Pages drawn over 256 planes almost never collide at
                        // these counts; one plane (or a 1/16 subset) is the defect.
                        require(distinct * 2 >= std::min(pages, U{planes_per_stack}),
                            std::string(name) + ": a live append stream aliases onto " +
                                std::to_string(distinct) + " planes for " + std::to_string(pages) + " pages");
                    }
                }
                require(d.stats().accounting_verified, "live appends failed the accounting audit");
            }
        }
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

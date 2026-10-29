// Compact-image plane placement under tensor/KV slot strides.
//
// A KV slot is 80 layers x 1 MiB (dense 70B) or 32 layers x 1 MiB (dense
// 8B), so the slot stride of 20480 or 8192 pages is a multiple of stacks x
// 256 pages at 2 and 4 HBF stacks with 256 planes per stack. A layout that
// put a stack's i-th page on plane i % 256 then read every layer's piece of
// every slot from the same 64 (4 stacks) or 128 (2 stacks) planes, and from
// ~128 at 6 stacks. With each stack-local round of pages starting on a mixed
// plane, a layer's pieces over 1,000 consecutive slots must reach every
// plane of every stack and load the busiest plane close to the mean.
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
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}

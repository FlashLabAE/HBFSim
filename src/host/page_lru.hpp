#pragma once
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <stdexcept>
#include <vector>

namespace hbfsim::host {

// Recency-ordered cache lines keyed by physical page, one ready time each.
// Lines live in one vector threaded by an intrusive list, and an
// open-addressing table maps pages to lines, so a hit, a fill and an
// eviction allocate nothing once the table has grown to its working size.
class PageLru {
public:
    [[nodiscard]] std::size_t size() const { return count_; }
    [[nodiscard]] bool empty() const { return count_ == 0; }
    [[nodiscard]] bool contains(std::uint64_t page) const { return locate(page) != kNone; }
    // The line's ready time without changing its recency.
    [[nodiscard]] std::optional<double> ready(std::uint64_t page) const {
        const auto line = locate(page);
        return line == kNone ? std::nullopt : std::optional<double>{lines_[line].ready_ns};
    }
    [[nodiscard]] double* ready_of(std::uint64_t page) {
        const auto line = locate(page);
        return line == kNone ? nullptr : &lines_[line].ready_ns;
    }
    // Makes the page the most recently used; false when it is not cached.
    bool touch(std::uint64_t page) {
        const auto line = locate(page);
        if (line == kNone) return false;
        unlink(line);
        link_front(line);
        return true;
    }
    // Inserts an absent page as the most recently used.
    void insert_front(std::uint64_t page, double ready_ns) {
        if (page == kEmptyPage) throw std::invalid_argument("page LRU key is reserved");
        if ((count_ + tombstones_ + 1) * 2 > table_.size()) rehash();
        std::uint32_t line;
        if (free_ != kNone) {
            line = free_;
            free_ = lines_[line].next;
        } else {
            line = static_cast<std::uint32_t>(lines_.size());
            lines_.push_back({});
        }
        lines_[line] = Line{.page = page, .ready_ns = ready_ns, .prev = kNone, .next = kNone};
        auto slot = probe(page);
        while (table_[slot] != kNone && table_[slot] != kTomb) slot = (slot + 1) & (table_.size() - 1);
        if (table_[slot] == kTomb) --tombstones_;
        table_[slot] = line;
        link_front(line);
        ++count_;
    }
    [[nodiscard]] std::uint64_t back() const {
        if (tail_ == kNone) throw std::logic_error("page LRU is empty");
        return lines_[tail_].page;
    }
    void pop_back() { erase(back()); }
    void erase(std::uint64_t page) {
        const auto slot = slot_of(page);
        if (slot == kNone) return;
        const auto line = table_[slot];
        table_[slot] = kTomb;
        ++tombstones_;
        unlink(line);
        lines_[line].next = free_;
        free_ = line;
        --count_;
    }

private:
    static constexpr std::uint32_t kNone = std::numeric_limits<std::uint32_t>::max();
    static constexpr std::uint32_t kTomb = kNone - 1;
    static constexpr std::uint64_t kEmptyPage = std::numeric_limits<std::uint64_t>::max();
    struct Line {
        std::uint64_t page = kEmptyPage;
        double ready_ns = 0.0;
        std::uint32_t prev = kNone, next = kNone;
    };
    [[nodiscard]] std::size_t probe(std::uint64_t page) const {
        return static_cast<std::size_t>((page * 0x9E3779B97F4A7C15ULL) >> shift_);
    }
    // The table slot holding the page's line, or kNone.
    [[nodiscard]] std::uint32_t slot_of(std::uint64_t page) const {
        if (table_.empty()) return kNone;
        auto slot = probe(page);
        while (table_[slot] != kNone) {
            if (table_[slot] != kTomb && lines_[table_[slot]].page == page)
                return static_cast<std::uint32_t>(slot);
            slot = (slot + 1) & (table_.size() - 1);
        }
        return kNone;
    }
    [[nodiscard]] std::uint32_t locate(std::uint64_t page) const {
        const auto slot = slot_of(page);
        return slot == kNone ? kNone : table_[slot];
    }
    void rehash() {
        std::size_t size = table_.empty() ? 64 : table_.size();
        while ((count_ + 1) * 2 > size) size *= 2;
        table_.assign(size, kNone);
        shift_ = 64 - static_cast<unsigned>(std::countr_zero(size));
        tombstones_ = 0;
        for (auto line = head_; line != kNone; line = lines_[line].next) {
            auto slot = probe(lines_[line].page);
            while (table_[slot] != kNone) slot = (slot + 1) & (size - 1);
            table_[slot] = line;
        }
    }
    void link_front(std::uint32_t line) {
        lines_[line].prev = kNone;
        lines_[line].next = head_;
        if (head_ != kNone) lines_[head_].prev = line;
        head_ = line;
        if (tail_ == kNone) tail_ = line;
    }
    void unlink(std::uint32_t line) {
        auto& entry = lines_[line];
        if (entry.prev != kNone) lines_[entry.prev].next = entry.next; else head_ = entry.next;
        if (entry.next != kNone) lines_[entry.next].prev = entry.prev; else tail_ = entry.prev;
        entry.prev = entry.next = kNone;
    }
    std::vector<Line> lines_;
    std::vector<std::uint32_t> table_;
    std::uint32_t head_ = kNone, tail_ = kNone, free_ = kNone;
    std::size_t count_ = 0, tombstones_ = 0;
    unsigned shift_ = 64;
};

}

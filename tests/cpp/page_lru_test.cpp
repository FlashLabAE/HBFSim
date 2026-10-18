// The page LRU keeps recency, ready times and membership exactly as a list
// plus a map would, through growth, eviction, erasure and reuse.
#include "host/page_lru.hpp"
#include <algorithm>
#include <list>
#include <random>
#include <stdexcept>
#include <unordered_map>
using hbfsim::host::PageLru;
namespace {
void check(bool condition, const char* why) { if (!condition) throw std::runtime_error(why); }
struct Oracle {
    std::list<std::uint64_t> order;  // front = most recently used
    std::unordered_map<std::uint64_t, double> ready;
    void insert_front(std::uint64_t page, double at) { order.push_front(page); ready[page] = at; }
    bool touch(std::uint64_t page) {
        const auto at = std::find(order.begin(), order.end(), page);
        if (at == order.end()) return false;
        order.splice(order.begin(), order, at);
        return true;
    }
    void erase(std::uint64_t page) {
        if (!ready.erase(page)) return;
        order.erase(std::find(order.begin(), order.end(), page));
    }
};
}
int main() {
    PageLru lru;
    Oracle oracle;
    std::mt19937_64 random(7);
    check(lru.empty() && !lru.contains(3), "a new LRU holds nothing");
    for (int step = 0; step < 200000; ++step) {
        const auto page = static_cast<std::uint64_t>(random() % 5000) * 97;
        switch (random() % 5) {
        case 0: case 1:
            if (!lru.contains(page)) {
                lru.insert_front(page, static_cast<double>(step));
                oracle.insert_front(page, static_cast<double>(step));
            }
            break;
        case 2:
            check(lru.touch(page) == oracle.touch(page), "touch disagrees about membership");
            break;
        case 3:
            if (!oracle.order.empty()) {
                check(lru.back() == oracle.order.back(), "least recently used page differs");
                lru.pop_back();
                oracle.erase(oracle.order.back());
            }
            break;
        default:
            lru.erase(page);
            oracle.erase(page);
            break;
        }
        check(lru.size() == oracle.ready.size(), "size differs");
        if (step % 997 == 0) {
            for (const auto& [held, at] : oracle.ready) {
                check(lru.contains(held) && lru.ready(held) == at, "a held page or its ready time is missing");
                check(*lru.ready_of(held) == at, "ready_of differs");
            }
            check(!lru.contains(page + 1), "an absent page is reported present");
        }
    }
    while (!oracle.order.empty()) {
        check(lru.back() == oracle.order.back(), "eviction order differs");
        lru.pop_back();
        oracle.erase(oracle.order.back());
    }
    check(lru.empty(), "the LRU does not empty");
    bool threw = false;
    try { (void)lru.back(); } catch (const std::logic_error&) { threw = true; }
    check(threw, "back() of an empty LRU must throw");
    return 0;
}

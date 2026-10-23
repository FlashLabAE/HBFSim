// Differential oracle: the pre-optimization vector/replay bank-cache rules
// from 6de8f0f. No snapshots or next-bank index are used by this oracle.
#include "physical/hbf/hbf_device.hpp"
#include <algorithm>
#include <iostream>
#include <limits>
#include <map>
#include <random>
#include <set>
#include <stdexcept>
#include <tuple>
using namespace hbfsim::physical::hbf;
namespace {
struct ReplayOracle {
    struct Event { double at; std::uint64_t seq, page; int action; };
    struct Bank {
        std::vector<std::uint64_t> resident;
        std::vector<Event> events;
        std::uint64_t consumers = 0;
        std::map<std::uint64_t, std::set<std::pair<double, std::uint64_t>>> pending;
    };
    std::map<std::uint64_t, Bank> banks;
    std::uint64_t sequence = 0;
    static bool apply(std::vector<std::uint64_t>& pages, const Event& event) {
        const auto found = std::find(pages.begin(), pages.end(), event.page);
        if (event.action < 0) { if (found != pages.end()) pages.erase(found); return true; }
        if (event.action == 0 && found == pages.end()) return false;
        if (found != pages.end()) pages.erase(found);
        pages.push_back(event.page);
        if (pages.size() > 2) pages.erase(pages.begin());
        return true;
    }
    static std::vector<std::uint64_t> project(const Bank& bank, double at, bool* valid = nullptr) {
        auto pages = bank.resident;
        if (valid) *valid = true;
        for (const auto& event : bank.events) {
            if (event.at > at) break;
            if (!apply(pages, event) && valid) *valid = false;
        }
        return pages;
    }
    static std::size_t insert(Bank& bank, Event event) {
        const auto at = std::lower_bound(bank.events.begin(), bank.events.end(), event,
            [](const Event& a, const Event& b) { return std::tie(a.at,a.seq) < std::tie(b.at,b.seq); });
        return static_cast<std::size_t>(bank.events.insert(at,event) - bank.events.begin());
    }
    bool resident(std::uint64_t page, double at) const {
        const auto found = banks.find(page / 32);
        if (found == banks.end()) return false;
        const auto pages = project(found->second, at);
        return std::find(pages.begin(),pages.end(),page) != pages.end();
    }
    bool hit(std::uint64_t page, double at) {
        if (!resident(page,at)) return false;
        auto& bank = banks[page / 32];
        const auto index = insert(bank,{at,sequence++,page,0});
        bool valid = true;
        (void)project(bank,std::numeric_limits<double>::infinity(),&valid);
        if (!valid) bank.events.erase(bank.events.begin()+static_cast<std::ptrdiff_t>(index));
        else ++bank.consumers;
        return valid;
    }
    void fill(std::uint64_t page, double at) {
        auto& bank = banks[page / 32];
        const auto seq = sequence++;
        const auto index = insert(bank,{at,seq,page,1});
        if (bank.consumers && index + 1 != bank.events.size()) {
            bool valid = true;
            (void)project(bank,std::numeric_limits<double>::infinity(),&valid);
            if (!valid) { bank.events.erase(bank.events.begin()+static_cast<std::ptrdiff_t>(index)); return; }
        }
        bank.pending[page].insert({at,seq});
    }
    std::optional<double> pending(std::uint64_t page, double at) const {
        const auto bank = banks.find(page / 32);
        if (bank == banks.end()) return std::nullopt;
        const auto found = bank->second.pending.find(page);
        if (found == bank->second.pending.end()) return std::nullopt;
        const auto next = found->second.upper_bound({at,std::numeric_limits<std::uint64_t>::max()});
        return next == found->second.end() ? std::nullopt : std::optional<double>(next->first);
    }
    void purge(std::uint64_t page) {
        const auto found = banks.find(page/32);
        if (found == banks.end()) return;
        auto& bank = found->second;
        bank.pending.erase(page);
        if (bank.events.empty()) std::erase(bank.resident,page);
        else (void)insert(bank,{bank.events.back().at,sequence++,page,-1});
    }
    void purge_block(std::uint64_t block) {
        const auto found = banks.find(block / 8);
        if (found == banks.end()) return;
        std::set<std::uint64_t> pages;
        for (auto page : found->second.resident) if (page/4 == block) pages.insert(page);
        for (const auto& event : found->second.events) if (event.page/4 == block) pages.insert(event.page);
        for (auto page : pages) purge(page);
    }
    void advance(double at) {
        for (auto& [index,bank] : banks) {
            (void)index;
            auto event = bank.events.begin();
            for (; event != bank.events.end() && event->at <= at; ++event) {
                if (!apply(bank.resident,*event)) throw std::runtime_error("oracle lost a future consumer");
                if (!event->action) --bank.consumers;
                if (event->action > 0) {
                    auto found = bank.pending.find(event->page);
                    if (found != bank.pending.end()) {
                        found->second.erase({event->at,event->seq});
                        if (found->second.empty()) bank.pending.erase(found);
                    }
                }
            }
            bank.events.erase(bank.events.begin(),event);
        }
    }
};
void check(bool condition,const char* message) { if (!condition) throw std::runtime_error(message); }
}
int main() {
    try {
        HbfDeviceConfig config;
        config.stacks = config.channels_per_stack = config.dies_per_channel = 1;
        config.planes_per_die = 4; config.blocks_per_plane = 8; config.pages_per_block = 4;
        HbfDevice actual(config,true);
        ReplayOracle expected;
        std::mt19937_64 random(20261023);
        double watermark = 0;
        for (unsigned step = 0; step < 2500; ++step) {
            const auto page = random()%128;
            const auto at = watermark + static_cast<double>(random()%40);
            switch (random()%6) {
            case 0: case 1: actual.cache_fill(page,at); expected.fill(page,at); break;
            case 2: check(actual.cache_hit(page,at) == expected.hit(page,at),"future hit admission differs"); break;
            case 3: actual.cache_purge_page(page); expected.purge(page); break;
            case 4: actual.cache_purge_block(page/4); expected.purge_block(page/4); break;
            default: watermark += static_cast<double>(random()%8);
                actual.advance_cache(watermark); expected.advance(watermark); break;
            }
            for (std::uint64_t probe = 0; probe < 128; ++probe)
                for (double time : {watermark,watermark+10,watermark+50}) {
                    check(actual.cache_resident(probe,time) == expected.resident(probe,time),"temporal residency differs");
                    check(actual.pending_cache_fill(probe,time) == expected.pending(probe,time),"pending fill differs");
                }
        }
        actual.advance_cache(watermark+100); expected.advance(watermark+100);
        for (std::uint64_t page=0;page<128;++page)
            check(actual.cache_resident(page,watermark+100) == expected.resident(page,watermark+100),"final frontier differs");
        std::cout << "2500 mixed temporal events: complete 128-page residency/pending comparisons passed\n";
    } catch (const std::exception& error) { std::cerr << error.what() << '\n'; return 1; }
}

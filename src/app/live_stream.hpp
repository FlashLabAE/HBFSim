#pragma once
#include "app/sha256.hpp"
#include "physical/simulation_session.hpp"
#include <istream>
#include <sstream>
#include <vector>
#include <limits>

namespace hbfsim::app {
// Unlike immutable FILE input, a malformed live stream is a terminal failure.
class LiveStream {
    std::istream& input_;
    std::uint64_t phase_,first_,count_=0;
    std::vector<unsigned char> buffer_;
    std::size_t cursor_=0;
    Sha256 digest_;
    static std::uint64_t number(const unsigned char* p,unsigned n) {
        std::uint64_t value=0;for(unsigned i=0;i<n;++i)value|=std::uint64_t(p[i])<<(8*i);return value;
    }
public:
    std::string sha;
    LiveStream(std::istream& input,std::uint64_t phase,std::uint64_t first)
        :input_(input),phase_(phase),first_(first) {}
    std::optional<physical::SimulationTransaction> next() {
        if(cursor_==buffer_.size()) {
            std::string line,command,extra;
            if(!std::getline(input_,line))throw std::runtime_error("live stream ended before seal");
            std::istringstream fields(line);std::uint64_t n=0;
            if(!(fields>>command>>n))throw std::runtime_error("invalid live chunk header");
            if(command=="END_LIVE") {
                if(!(fields>>sha) || fields>>extra || n!=count_ || sha!=digest_.finish_hex())
                    throw std::runtime_error("live stream seal count/digest mismatch");
                return std::nullopt;
            }
            if(command!="CHUNK" || fields>>extra || !n || n>65536 || n>25000000-count_)
                throw std::runtime_error("invalid live chunk size");
            buffer_.resize(20*n);cursor_=0;
            if(!input_.read(reinterpret_cast<char*>(buffer_.data()),buffer_.size()) || input_.get()!='\n')
                throw std::runtime_error("truncated live chunk");
            digest_.update(buffer_.data(),buffer_.size());
        }
        const auto* p=buffer_.data()+cursor_;cursor_+=20;
        if(p[0]>1 || p[1]>1 || number(p+2,2) || count_>=std::numeric_limits<std::uint64_t>::max()-first_)
            throw std::runtime_error("invalid live record fields/range");
        return physical::SimulationTransaction{
            .id="replay/"+std::to_string(phase_)+"/op/request/"+std::to_string(first_+count_++),
            .target=p[0]?physical::SimulationTarget::HbfLogical:physical::SimulationTarget::Hbm,
            .op=p[1]?physical::Op::Write:physical::Op::Read,.addr=number(p+4,8),.bytes=number(p+12,8)};
    }
};
}

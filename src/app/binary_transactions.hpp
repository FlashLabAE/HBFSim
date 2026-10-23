#pragma once
#include "app/sha256.hpp"
#include "physical/simulation_session.hpp"
#include <bit>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace hbfsim::app {
// Generic transaction wire encoding only. Execution/validation belongs to
// SimulationSession::run_batch, shared with the existing text protocol.
inline std::vector<physical::SimulationTransaction> read_binary_transactions(
    const std::filesystem::path& path, std::uint64_t offset, std::uint64_t length,
    const std::string& expected_sha256) {
    const auto status=std::filesystem::symlink_status(path);
    if (!path.is_absolute() || !std::filesystem::is_regular_file(status) ||
        length < 16 || length > 512ULL*1024*1024 ||
        offset > std::filesystem::file_size(path) ||
        length > std::filesystem::file_size(path)-offset) {
        throw std::runtime_error("binary transaction file bounds/type are invalid");
    }
    std::ifstream file(path,std::ios::binary);
    file.seekg(static_cast<std::streamoff>(offset));
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(length));
    if (!file.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(length)))
        throw std::runtime_error("truncated binary transaction payload");
    Sha256 digest;digest.update(bytes.data(),bytes.size());
    if (digest.finish_hex()!=expected_sha256)
        throw std::runtime_error("binary transaction payload digest mismatch");
    std::size_t cursor=0;
    const auto number=[&](unsigned width) {
        if (width>bytes.size()-cursor)throw std::runtime_error("truncated binary transaction record");
        std::uint64_t value=0;
        for(unsigned i=0;i<width;++i)value|=std::uint64_t(bytes[cursor++])<<(8*i);
        return value;
    };
    const auto identifier=[&](std::size_t size) {
        if (!size || size>4096 || size>bytes.size()-cursor)
            throw std::runtime_error("invalid binary transaction identifier size");
        std::string value(reinterpret_cast<const char*>(bytes.data()+cursor),size);cursor+=size;
        for(unsigned char ch:value)
            if (!((ch>='a'&&ch<='z')||(ch>='A'&&ch<='Z')||(ch>='0'&&ch<='9')||
                  ch=='_'||ch=='-'||ch=='.'||ch==':'||ch=='/'))
                throw std::runtime_error("unsafe binary transaction identifier");
        return value;
    };
    // A lossless shorthand for zero-issue serial memory phases. Expand the
    // same IDs and edges as the frontend; execute through the common run_batch.
    if (std::string(reinterpret_cast<const char*>(bytes.data()),8)=="HBFPH01\n") {
        cursor=8;
        const auto count=number(8), phase=number(8), first=number(8);
        if (count>=2000000 || count>(bytes.size()-cursor)/20 ||
            count*20!=bytes.size()-cursor ||
            count>std::numeric_limits<std::uint64_t>::max()-first)
            throw std::runtime_error("invalid binary phase count/range");
        const auto prefix="replay/"+std::to_string(phase);
        const auto previous=phase ? "replay/"+std::to_string(phase-1)+"/complete" : "";
        constexpr physical::SimulationTarget phase_targets[]={
            physical::SimulationTarget::Hbm, physical::SimulationTarget::HbfLogical,
            physical::SimulationTarget::HbfStatic, physical::SimulationTarget::HbfPhysical};
        std::vector<physical::SimulationTransaction> result;
        result.reserve(count+1);
        std::vector<std::string> waits;
        waits.reserve(count+bool(phase));
        for (std::uint64_t i=0; i<count; ++i) {
            const auto target=number(1), op=number(1), reserved=number(2);
            const auto address=number(8), size=number(8);
            if (target>=std::size(phase_targets) || op>1 || reserved || !size ||
                size>std::numeric_limits<std::uint64_t>::max()-address)
                throw std::runtime_error("invalid binary phase memory fields");
            auto id=prefix+"/op/request/"+std::to_string(first+i);
            waits.push_back(id);
            physical::SimulationTransaction tx{
                .id=std::move(id), .target=phase_targets[target],
                .op=op ? physical::Op::Write : physical::Op::Read,
                .addr=address, .bytes=size, .issue_ns=0, .duration_ns=0,
                .dependencies={}, .stack=0};
            if (phase) tx.dependencies.push_back(previous);
            result.push_back(std::move(tx));
        }
        if (phase) waits.push_back(previous);
        result.push_back(physical::SimulationTransaction{
            .id=prefix+"/complete", .target=physical::SimulationTarget::Barrier,
            .op=physical::Op::Read, .addr=0, .bytes=0, .issue_ns=0, .duration_ns=0,
            .dependencies=std::move(waits), .stack=0});
        return result;
    }
    if (std::string(reinterpret_cast<const char*>(bytes.data()),8)!="HBFTX01\n")
        throw std::runtime_error("unsupported binary transaction version");
    cursor=8;const auto count=number(8);
    if (count>2000000 || count>(bytes.size()-cursor)/49)
        throw std::runtime_error("binary transaction count exceeds payload/cap");
    constexpr physical::SimulationTarget targets[]={physical::SimulationTarget::Hbm,
        physical::SimulationTarget::HbfLogical,physical::SimulationTarget::HbfStatic,
        physical::SimulationTarget::HbfPhysical,physical::SimulationTarget::D2dHbfToHbm,
        physical::SimulationTarget::D2dHbmToHbf,physical::SimulationTarget::External,
        physical::SimulationTarget::HostDram,physical::SimulationTarget::DirectHbfToExternal,
        physical::SimulationTarget::DirectExternalToHbf,physical::SimulationTarget::Barrier};
    std::vector<physical::SimulationTransaction> result;result.reserve(count);
    for(std::uint64_t i=0;i<count;++i) {
        const auto id_bytes=number(2),target=number(1),op=number(1),stack=number(4);
        const auto address=number(8),size=number(8);
        const double issue=std::bit_cast<double>(number(8)),duration=std::bit_cast<double>(number(8));
        const auto deps=number(4),reserved=number(4);
        if(target>=std::size(targets) || reserved || deps>2000000 ||
           !std::isfinite(issue)||!std::isfinite(duration)||issue<0||duration<0||
           (target==10 ? op!=2 : op>1))
            throw std::runtime_error("invalid binary transaction fields");
        physical::SimulationTransaction tx{.id=identifier(id_bytes),.target=targets[target],
            .op=op==1?physical::Op::Write:physical::Op::Read,.addr=address,.bytes=size,
            .issue_ns=issue,.duration_ns=duration,.dependencies={},.stack=static_cast<std::uint32_t>(stack)};
        if (deps>(bytes.size()-cursor)/3)
            throw std::runtime_error("binary dependency count exceeds payload");
        tx.dependencies.reserve(deps);
        for(std::uint64_t d=0;d<deps;++d) {const auto n=number(2);tx.dependencies.push_back(identifier(n));}
        result.push_back(std::move(tx));
    }
    if(cursor!=bytes.size())throw std::runtime_error("trailing binary transaction payload");
    return result;
}
} // namespace hbfsim::app

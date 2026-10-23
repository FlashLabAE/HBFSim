#pragma once
#include "app/sha256.hpp"
#include "physical/simulation_session.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

namespace hbfsim::app {
// Private disk snapshot: complete hash/preflight validation without whole-file RAM.
class BinaryStream {
    std::unique_ptr<std::FILE, decltype(&std::fclose)> file_{nullptr,&std::fclose};
    std::vector<unsigned char> buffer_;
    std::size_t cursor_=0,available_=0;
    std::uint64_t index_=0;
    static std::uint64_t number(const unsigned char* p,unsigned n) {
        std::uint64_t v=0;for(unsigned i=0;i<n;++i)v|=std::uint64_t(p[i])<<(8*i);return v;
    }
public:
    std::uint64_t count=0,phase=0,first=0;
    BinaryStream(const std::filesystem::path& path,std::uint64_t offset,std::uint64_t length,
                 const std::string& sha,std::uint64_t window) {
        if (!window || window>65536 || !path.is_absolute() ||
            !std::filesystem::is_regular_file(std::filesystem::symlink_status(path)) ||
            length<32 || length>512ULL*1024*1024 || offset>std::filesystem::file_size(path) ||
            length>std::filesystem::file_size(path)-offset)
            throw std::runtime_error("invalid stream file bounds/window");
        buffer_.resize(20*window);
        file_.reset(std::tmpfile());if(!file_)throw std::runtime_error("cannot create stream snapshot");
        std::ifstream input(path,std::ios::binary);input.seekg(offset);
        std::array<unsigned char,65536> chunk{};Sha256 digest;
        for (auto left=length;left;) {
            const auto n=std::min<std::uint64_t>(left,chunk.size());
            if(!input.read(reinterpret_cast<char*>(chunk.data()),n) || std::fwrite(chunk.data(),1,n,file_.get())!=n)
                throw std::runtime_error("stream snapshot IO failure");
            digest.update(chunk.data(),n);left-=n;
        }
        if(digest.finish_hex()!=sha)throw std::runtime_error("stream snapshot digest mismatch");
        std::rewind(file_.get());std::array<unsigned char,32> head{};
        if(std::fread(head.data(),1,32,file_.get())!=32 ||
            std::string(reinterpret_cast<char*>(head.data()),8)!="HBFPH01\n")
            throw std::runtime_error("unsupported stream file format");
        count=number(head.data()+8,8);phase=number(head.data()+16,8);first=number(head.data()+24,8);
        if(count>25000000 || length!=32+20*count || count>UINT64_MAX-first)
            throw std::runtime_error("invalid stream count/range");
        rewind();
    }
    void rewind() {
        if(std::fseek(file_.get(),32,SEEK_SET))throw std::runtime_error("cannot rewind stream snapshot");
        cursor_=available_=0;index_=0;
    }
    physical::SimulationTransaction next() {
        if(index_>=count)throw std::runtime_error("stream read past end");
        if(cursor_==available_) {
            available_=std::min<std::uint64_t>(count-index_,buffer_.size()/20)*20;cursor_=0;
            if(std::fread(buffer_.data(),1,available_,file_.get())!=available_)
                throw std::runtime_error("truncated private stream snapshot");
        }
        const auto* p=buffer_.data()+cursor_;cursor_+=20;
        if(p[0]>1 || p[1]>1 || number(p+2,2))throw std::runtime_error("invalid stream record fields");
        return physical::SimulationTransaction{
            .id="replay/"+std::to_string(phase)+"/op/request/"+std::to_string(first+index_++),
            .target=p[0]?physical::SimulationTarget::HbfLogical:physical::SimulationTarget::Hbm,
            .op=p[1]?physical::Op::Write:physical::Op::Read,.addr=number(p+4,8),.bytes=number(p+12,8)};
    }
};
}

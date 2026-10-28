#include "badman_gpu_raster.hpp"
#include "badman_gpu_shader.hpp"
#include "badman_workers.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <cfenv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <limits>

namespace badman {
namespace {
constexpr std::uint32_t missing=std::numeric_limits<std::uint32_t>::max();
constexpr std::uint32_t packet_words=20;
}
GpuRaster::GpuRaster(psprecomp::GuestMemory &memory,std::string_view renderer):memory_(memory),data_(8) {
    diagnostics_=std::getenv("BADMAN_GPU_DIAGNOSTICS")!=nullptr;
    if(renderer=="d3d11")device_=make_d3d11_compute(gpu_shader_hlsl);
    else if(renderer=="vulkan")device_=make_vulkan_compute(gpu_shader_spirv);
    else if(renderer=="cpu")device_=make_cpu_compute(WorkerPool::default_helpers());
    else throw psprecomp::Error("Unknown GPU renderer: "+std::string(renderer));
}
bool GpuRaster::cached_transform(const GpuTransformKey &key,GpuTransform &result) const noexcept {
    if(std::fegetround()!=FE_TONEAREST||!transform_cache_||transform_cache_->key!=key)return false;
    result=transform_cache_->value;return true;
}
void GpuRaster::cache_transform(const GpuTransformKey &key,const GpuTransform &result) noexcept {
    if(std::fegetround()==FE_TONEAREST)transform_cache_=TransformEntry{key,result};
}
void GpuRaster::invalidate_textures(){textures_.clear();}
std::uint32_t GpuRaster::texture_data(const Texture &texture,const std::array<std::uint32_t,256>&r){
    const std::array<std::uint32_t,12> key{r[0xA0],r[0xA8],r[0xB8],r[0xC2],r[0xC3],r[0xC5],r[0xC6],r[0xC7],r[0xC8],r[0xC9],0,0};
    if(const auto it=textures_.find(key);it!=textures_.end())return it->second;
    auto snapshot=texture.gpu_snapshot();if(!snapshot)return missing;
    const auto offset=static_cast<std::uint32_t>(data_.size());
    data_.insert(data_.end(),snapshot->metadata.begin(),snapshot->metadata.end());
    data_.insert(data_.end(),snapshot->words.begin(),snapshot->words.end());
    textures_.emplace(key,offset);return offset;
}
bool GpuRaster::enqueue(std::span<const GpuSprite> sprites,const std::array<std::uint32_t,256>&r,const Texture *texture){
    const auto stride=r[0x9D]&65535u;
    auto target=psprecomp::GuestMemory::canonical((r[0x9C]&0xFFFFFFu)|((r[0x9D]&0xFF0000u)<<8));
    if(target>=psprecomp::GuestMemory::kVramPhysicalBase)target-=psprecomp::GuestMemory::kVramPhysicalBase;
    for(const auto &s:sprites)if(s.x1>stride || s.y1>1024)return false; // Aliased logical rows use scalar draw ordering.
    if(packet_count_&&(target_!=target||stride_!=stride||packet_count_+sprites.size()>8192||data_.size()>4*1024*1024))flush();
    target_=target;stride_=stride;
    auto texture_offset=missing;
    if(texture){texture_offset=texture_data(*texture,r);if(texture_offset==missing)return false;}
    const auto preserve=(r[0xE8]&0xFFFFFFu)|0xFF000000u;
    std::vector<std::uint32_t> coordinates;
    std::vector<std::array<std::uint32_t,packet_words>> staged;
    for(const auto &s:sprites){
        std::uint32_t u_offset=0,v_offset=0;
        if(texture){
            u_offset=static_cast<std::uint32_t>(data_.size()+coordinates.size());
            const auto start=s.rotated?s.y0:s.x0,end=s.rotated?s.y1:s.x1;
            for(auto p=start;p<end;++p){
                const auto f=(static_cast<double>(p*16+8)-(s.rotated?s.ay:s.ax))/(s.rotated?s.by-s.ay:s.bx-s.ax);
                const auto u=static_cast<float>(s.u0+(s.u1-s.u0)*f);
                coordinates.push_back(static_cast<std::uint32_t>(texture->fixed_u(u)));
            }
            v_offset=static_cast<std::uint32_t>(data_.size()+coordinates.size());
            const auto vstart=s.rotated?s.x0:s.y0,vend=s.rotated?s.x1:s.y1;
            for(auto p=vstart;p<vend;++p){
                const auto f=(static_cast<double>(p*16+8)-(s.rotated?s.ax:s.ay))/(s.rotated?s.bx-s.ax:s.by-s.ay);
                const auto v=static_cast<float>(s.v0+(s.v1-s.v0)*f);
                coordinates.push_back(static_cast<std::uint32_t>(texture->fixed_v(v)));
            }
        }
        std::array<std::uint32_t,packet_words> packet{s.x0,s.y0,s.x1,s.y1,s.color,preserve,
            r[0xDF]&0xFFFFFFu,r[0xE0]&0xFFFFFFu,r[0xE1]&0xFFFFFFu,
            unsigned((r[0x21]&0xFFFFFFu)!=0)|(unsigned(texture!=nullptr)<<1)|
            (unsigned((r[0xC9]&0xFFFFFFu)==0x100)<<2)|(unsigned((r[0x20]&1)!=0)<<3)|
            (unsigned(s.rotated)<<4)|(unsigned(s.minifying)<<5),texture_offset,u_offset,v_offset,
            r[0xE2],r[0xE3],r[0xE4],r[0xE5],0,0,0};
        staged.push_back(packet);
    }
    // Coordinate errors in a later pair must not submit an earlier pair from
    // this draw. Prepare the complete submission before mutating the queue.
    data_.insert(data_.end(),coordinates.begin(),coordinates.end());
    for(std::size_t i=0;i<sprites.size();++i){
        const auto &s=sprites[i];const auto &packet=staged[i];
        packets_.insert(packets_.end(),packet.begin(),packet.end());
        for(auto ty=s.y0/16;ty<=(s.y1-1)/16;++ty)for(auto tx=s.x0/16;tx<=(s.x1-1)/16;++tx)
            bins_[ty*64+tx].push_back(packet_count_);
        ++packet_count_;height_=std::max(height_,s.y1);
    }
    return true;
}
void GpuRaster::flush(){
    if(!packet_count_){data_.resize(8);textures_.clear();return;}
    auto &input=input_;
    input.assign(data_.begin(),data_.end());
    input[0]=target_/4;input[1]=stride_;input[2]=height_;
    input[3]=static_cast<std::uint32_t>(input.size());
    const auto bins_offset=input.size();input.resize(input.size()+4096*2);
    for(std::size_t i=0;i<bins_.size();++i){
        input[bins_offset+i*2]=static_cast<std::uint32_t>(input.size());
        input[bins_offset+i*2+1]=static_cast<std::uint32_t>(bins_[i].size());
        input.insert(input.end(),bins_[i].begin(),bins_[i].end());
    }
    input[4]=static_cast<std::uint32_t>(input.size());input[5]=packet_count_;
    input.insert(input.end(),packets_.begin(),packets_.end());
    auto &state=state_;
    const auto started=diagnostics_?std::chrono::steady_clock::now():std::chrono::steady_clock::time_point{};
    auto *vram=memory_.raw_pointer(psprecomp::GuestMemory::kVramPhysicalBase,psprecomp::GuestMemory::kVramSize);
    if(!vram||!device_->dispatch_in_place(input,{vram,psprecomp::GuestMemory::kVramSize})) {
        state.resize(psprecomp::GuestMemory::kVramSize/4); // Allocated and zeroed once, then overwritten whole.
        std::memcpy(state.data(),memory_.vram_bytes().data(),psprecomp::GuestMemory::kVramSize);
        device_->dispatch(input,state,(stride_*height_+63)/64);
        // No guest memory is modified until the entire GPU transaction completed.
        memory_.copy_in(psprecomp::GuestMemory::kVramPhysicalBase,
            {reinterpret_cast<const std::uint8_t*>(state.data()),psprecomp::GuestMemory::kVramSize});
    }
    if(diagnostics_){
        const auto now=std::chrono::steady_clock::now();
        transaction_ns_+=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now-started).count());
        ++batches_;submitted_sprites_+=packet_count_;uploaded_words_+=input.size()+state.size();
        const auto ns=static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count());
        if(ns>=next_report_ns_){
            std::cout<<"[gpu-batches] batches="<<batches_<<" sprites="<<submitted_sprites_
                <<" uploaded_words="<<uploaded_words_<<" transaction_ns="<<transaction_ns_
                <<" readback_words="<<batches_*state.size()<<std::endl;
            next_report_ns_=ns+5000000000ull;
        }
    }
    data_.resize(8);packets_.clear();textures_.clear();
    for(auto &bin:bins_)bin.clear();
    packet_count_=height_=0;
}
}

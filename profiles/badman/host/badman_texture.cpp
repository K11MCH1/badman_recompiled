#include "badman_texture.hpp"
#include "badman_clut.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <array>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <string>

namespace badman {
namespace {
int floor16(int value) { return value >> 4; } // Arithmetic shift: floor(value / 16) for negatives too.
std::uint32_t channel5(std::uint32_t value) { return (value << 3u) | (value >> 2u); }
std::uint32_t channel6(std::uint32_t value) { return (value << 2u) | (value >> 4u); }
std::uint32_t interpolate(std::uint32_t a,std::uint32_t b,unsigned fraction) {
    std::uint32_t output=0;
    for(unsigned shift=0;shift<32;shift+=8) {
        const auto first=(a>>shift)&255u,second=(b>>shift)&255u;
        output |= ((first*(16u-fraction)+second*fraction)>>4u)<<shift;
    }
    return output;
}
}

Texture::Texture(const psprecomp::GuestMemory &memory,const std::array<std::uint32_t,256> &commands,
                 const Clut *palette)
    :memory_(memory),palette_(palette) {
    auto reg=[&](unsigned command) { return commands[command]&0x00ffffffu; };
    auto reject=[](const std::string &message) { throw psprecomp::Error("GE texture: "+message); };
    const auto mode=reg(0xc2),format=reg(0xc3),filter=reg(0xc6),wrap=reg(0xc7),size=reg(0xb8),buffer=reg(0xa8);
    if(mode&~0x070101u) reject("unsupported texture-mode flags "+psprecomp::hex32(mode));
    if((mode>>16u)&7u) reject("mipmapped level chains are not implemented");
    if(format&~15u) reject("unsupported texture-format flags "+psprecomp::hex32(format));
    if(format>=6u) reject("T16/T32/compressed/reserved texture format is not implemented: "+std::to_string(format));
    if(format>=4u) {
        if(!palette_) reject("indexed texture requires latched CLUT-load state");
        palette_format_=reg(0xc5);
        Clut::validate_format(palette_format_);
    }
    if(size&~0x000f0fu || (size&15u)>9u || ((size>>8u)&15u)>9u) reject("invalid level-zero texture size");
    if(buffer&~0x0f07ffu || (buffer&0x7ffu)==0u || (buffer&0x7ffu)>1024u) reject("invalid texture buffer-width/address flags");
    if(reg(0xa0)&15u) reject("texture buffer address must be 16-byte aligned");
    if(filter&~0x000107u || (filter&7u)==2u || (filter&7u)==3u) reject("unsupported/reserved texture filter");
    if(wrap&~0x000101u) reject("unsupported texture wrap flags");
    if(reg(0xc8)!=0u) reject("nonzero texture LOD mode/bias is not implemented");
    format_=format;
    width_=1u<<(size&15u);height_=1u<<((size>>8u)&15u);
    const auto stride=buffer&0x7ffu;
    address_=reg(0xa0)|((buffer&0x0f0000u)<<8u);
    bytes_per_pixel_=format==3u?4u:(format>=4u?1u:2u);
    if(format==4u && (stride&1u)) reject("odd T4 buffer width is not implemented");
    row_bytes_=format==4u?stride/2u:stride*bytes_per_pixel_;
    swizzled_=(mode&1u)!=0u;
    clamp_u_=(wrap&1u)!=0u;clamp_v_=(wrap&0x100u)!=0u;
    linear_min_=(filter&1u)!=0u;linear_mag_=(filter&0x100u)!=0u;
    // Multiclut mode bit8 has no effect on a direct-color texture. Mipmap
    // filter codes4..7 reduce to the base-level nearest/linear filter when
    // maxmips=0; no level selection or missing mip content is fabricated.
    const auto visible_row_bytes=format==4u?(width_+1u)/2u:width_*bytes_per_pixel_;
    // TBW is an address pitch, not a logical width limit. Texels beyond a
    // smaller pitch continue into the following physical row/tile band.
    // Retain complete-pitch validation, and also cover the final logical row.
    std::uint64_t footprint=std::max(static_cast<std::uint64_t>(row_bytes_)*height_,
        static_cast<std::uint64_t>(height_-1u)*row_bytes_+visible_row_bytes);
    if(swizzled_) {
        if(row_bytes_<16u || (row_bytes_&15u)) reject("swizzled row pitch must be a positive multiple of 16 bytes");
        // Validate through the last addressed complete 16-byte x 8-row tile.
        // Merely multiplying pitch by height misses tiles when width > TBW.
        const auto last_tile_band=static_cast<std::uint64_t>((height_-1u)/8u)*row_bytes_*8u;
        const auto tile_band_end=last_tile_band+static_cast<std::uint64_t>((visible_row_bytes+15u)/16u)*128u;
        footprint=std::max(static_cast<std::uint64_t>(row_bytes_)*((height_+7u)&~7u),tile_band_end);
    }
    if(!memory_.contains(address_,static_cast<std::size_t>(footprint)))
        reject("complete texture footprint lies outside guest RAM/EDRAM at "+psprecomp::hex32(address_));
    // Keep the logical address/footprint rejection above: raw_pointer alone only
    // proves a contiguous physical backing, not the PSP logical VRAM boundary.
    contiguous_=memory_.raw_pointer(address_,static_cast<std::size_t>(footprint));
    footprint_=static_cast<std::size_t>(footprint);
}

std::optional<Texture::GpuSnapshot> Texture::gpu_snapshot() const {
    GpuSnapshot result;
    result.metadata={width_,height_,row_bytes_,format_,
        unsigned(swizzled_)|(unsigned(clamp_u_)<<1)|(unsigned(clamp_v_)<<2)|
        (unsigned(linear_min_)<<3)|(unsigned(linear_mag_)<<4),
        static_cast<std::uint32_t>((footprint_+3)/4)};
    result.words.resize((footprint_+3)/4+256);
    if(format_>=4u)
        for(unsigned i=0;i<(format_==4?16u:256u);++i)
            if(!palette_->try_lookup(i,palette_format_,result.words[(footprint_+3)/4+i])) return std::nullopt;
    for(std::size_t i=0;i<footprint_;++i)
        result.words[i/4]|=std::uint32_t(contiguous_?contiguous_[i]:memory_.load8(address_+static_cast<std::uint32_t>(i)))<<((i&3u)*8u);
    return result;
}

namespace {
std::uint64_t mix(std::uint64_t hash, std::uint64_t value) {
    hash ^= value + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2);
    return hash * 0xFF51AFD7ED558CCDull;
}
}
std::uint64_t Texture::layout_key() const {
    std::uint64_t hash=0x84222325CBF29CE4ull;
    for(const std::uint64_t value:{std::uint64_t(address_),std::uint64_t(width_),std::uint64_t(height_),std::uint64_t(row_bytes_),
            std::uint64_t(format_),std::uint64_t(swizzled_),std::uint64_t(palette_format_)})hash=mix(hash,value);
    return hash;
}
std::uint64_t Texture::texel_hash() const {
    // Four independent lanes over 8-byte words keep this near memory speed.
    std::array<std::uint64_t,4> lanes{0x9E3779B97F4A7C15ull,0xC2B2AE3D27D4EB4Full,0x165667B19E3779F9ull,0x27D4EB2F165667C5ull};
    std::size_t at=0;
    if(contiguous_) {
        for(;at+32<=footprint_;at+=32)
            for(unsigned lane=0;lane<4;++lane) {
                std::uint64_t word;std::memcpy(&word,contiguous_+at+lane*8,8);
                lanes[lane]=(lanes[lane]^word)*0x9FB21C651E98DF25ull;lanes[lane]^=lanes[lane]>>29;
            }
    }
    std::uint64_t hash=layout_key();
    for(const auto lane:lanes)hash=mix(hash,lane);
    for(;at<footprint_;++at)hash=mix(hash,contiguous_?contiguous_[at]:memory_.load8(address_+static_cast<std::uint32_t>(at)));
    return hash;
}
std::uint64_t Texture::palette_hash() const {
    std::uint64_t hash=palette_format_;
    if(format_>=4u)
        for(unsigned i=0;i<(format_==4u?16u:256u);++i) {
            std::uint32_t color=0;
            // 0x1'0000'0000 marks a palette entry that has not been loaded.
            hash=mix(hash,palette_->try_lookup(i,palette_format_,color)?std::uint64_t(color):0x1'0000'0000ull);
        }
    return hash;
}
std::vector<std::uint32_t> Texture::decode() const {
    std::vector<std::uint32_t> texels(static_cast<std::size_t>(width_)*height_);
    for(std::uint32_t y=0;y<height_;++y)
        for(std::uint32_t x=0;x<width_;++x) {
            // An unloaded palette entry stays transparent; the game never samples it.
            auto &out=texels[static_cast<std::size_t>(y)*width_+x];
            try{
                if(format_>=4u) {
                    int tx=static_cast<int>(x),ty=static_cast<int>(y);
                    const auto offset=texel_offset(tx,ty);
                    palette_->try_lookup(palette_index(tx,offset),palette_format_,out);
                } else out=texel(static_cast<int>(x),static_cast<int>(y));
            } catch(const psprecomp::Error &){}
        }
    return texels;
}

int Texture::index(int coordinate,std::uint32_t extent,bool clamp) {
    const auto size=static_cast<int>(extent);
    if(clamp) return std::clamp(coordinate,0,size-1);
    // Texture extents are powers of two; the mask equals the wrapped remainder.
    if((extent&(extent-1u))==0u) return coordinate&(size-1);
    const auto remainder=coordinate%size;
    return remainder<0?remainder+size:remainder;
}

int Texture::position(float coordinate,std::uint32_t extent,bool clamp) {
    if(!std::isfinite(coordinate)) throw psprecomp::Error("GE texture: nonfinite texture coordinate");
    // Hardware truncates to 1/16 texel toward zero before addressing/filtering.
    // Reduce/clamp in double precision before conversion, so finite float
    // extremes never overflow a host integer or a guest texture address.
    double fixed=std::trunc(static_cast<double>(coordinate)*16.0);
    const double period=static_cast<double>(extent)*16.0;
    if(!clamp && std::abs(fixed)<9.0e15) {
        // fixed and period are exact integers here; integer remainder keeps
        // fmod's sign and magnitude without its cost on every texel.
        return static_cast<int>(static_cast<std::int64_t>(fixed)%static_cast<std::int64_t>(period));
    }
    fixed=clamp?std::clamp(fixed,-16.0,period+16.0):std::fmod(fixed,period);
    return static_cast<int>(fixed);
}

std::uint32_t Texture::texel_offset(int &x,int &y) const {
    x=index(x,width_,clamp_u_);y=index(y,height_,clamp_v_);
    const auto byte_x=format_==4u?static_cast<std::uint32_t>(x)/2u:static_cast<std::uint32_t>(x)*bytes_per_pixel_;
    const auto row=static_cast<std::uint32_t>(y);
    if(swizzled_) return (row/8u)*(row_bytes_*8u)+(byte_x/16u)*128u+(row%8u)*16u+(byte_x%16u);
    return row*row_bytes_+byte_x;
}
std::uint32_t Texture::palette_index(int x,std::uint32_t offset) const {
    const auto packed=contiguous_?contiguous_[offset]:memory_.load8(address_+offset);
    return format_==4u?(packed>>((static_cast<unsigned>(x)&1u)*4u))&15u:packed;
}
std::uint32_t Texture::texel(int x,int y) const {
    const auto offset=texel_offset(x,y);
    const auto *bytes=contiguous_?contiguous_+offset:nullptr;
    if(format_>=4u) return palette_->lookup(palette_index(x,offset),palette_format_);
    if(format_==3u) return bytes?
        std::uint32_t(bytes[0])|(std::uint32_t(bytes[1])<<8u)|
        (std::uint32_t(bytes[2])<<16u)|(std::uint32_t(bytes[3])<<24u):memory_.load32(address_+offset);
    const auto value=bytes?std::uint32_t(bytes[0])|(std::uint32_t(bytes[1])<<8u):memory_.load16(address_+offset);
    if(format_==0u) return 0xff000000u|channel5(value&31u)|(channel6((value>>5u)&63u)<<8u)|(channel5((value>>11u)&31u)<<16u);
    if(format_==1u) return ((value&0x8000u)?0xff000000u:0u)|channel5(value&31u)|(channel5((value>>5u)&31u)<<8u)|(channel5((value>>10u)&31u)<<16u);
    return ((value&15u)*17u)|(((value>>4u)&15u)*17u<<8u)|(((value>>8u)&15u)*17u<<16u)|(((value>>12u)&15u)*17u<<24u);
}

std::uint32_t Texture::sample(float u,float v,bool minifying) const {
    return sample_fixed(position(u,width_,clamp_u_),position(v,height_,clamp_v_),minifying);
}
std::uint32_t Texture::sample_fixed(int x,int y,bool minifying) const {
    if(!(minifying?linear_min_:linear_mag_)) return texel(floor16(x),floor16(y));
    const auto left=floor16(x-8),top=floor16(y-8);
    const auto fx=static_cast<unsigned>(x-8-left*16),fy=static_cast<unsigned>(y-8-top*16);
    const auto first=interpolate(texel(left,top),texel(left+1,top),fx);
    const auto second=interpolate(texel(left,top+1),texel(left+1,top+1),fx);
    return interpolate(first,second,fy);
}
}

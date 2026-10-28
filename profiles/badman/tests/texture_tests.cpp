#include "badman_texture.hpp"
#include "badman_clut.hpp"
#include "psprecomp/common.hpp"
#include <array>
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>

namespace {
using psprecomp::GuestMemory;
using Registers=std::array<std::uint32_t,256>;
void require(bool value,const char *message) {if(!value)throw std::runtime_error(message);}
template<class F> void rejects(F operation,const char *message) {
    bool failed=false;try{operation();}catch(const psprecomp::Error &){failed=true;}
    require(failed,message);
}
Registers registers(std::uint32_t address=0x08800000u,unsigned format=3,unsigned width_exp=1,unsigned height_exp=1,unsigned stride=4) {
    Registers result{};result[0xa0]=address&0xffffffu;result[0xa8]=((address>>8u)&0x0f0000u)|stride;
    result[0xb8]=width_exp|(height_exp<<8u);result[0xc3]=format;result[0xc7]=0x101u;return result;
}
void row_pitch_nearest_and_addressing() {
    GuestMemory memory;constexpr std::uint32_t base=0x08800000;
    memory.store32(base,0xff00000au);memory.store32(base+4,0xff000014u);
    memory.store32(base+8,0xdeadbeefu);memory.store32(base+16,0xff00001eu);memory.store32(base+20,0xff000028u);
    auto regs=registers();badman::Texture clamp(memory,regs);
    require(clamp.width()==2&&clamp.height()==2&&clamp.format()==3,"Texture dimensions/format incorrect");
    require(clamp.sample(0,0)==0xff00000a&&clamp.sample(1,1)==0xff000028,"Row stride was replaced by visible width");
    require(clamp.sample(1.99f,0)==0xff000014,"Nearest sampling rounded to another texel");
    require(clamp.sample(-100,100)==0xff00001e,"Clamp addressing did not select edge texels");
    regs[0xc7]=0;badman::Texture repeat(memory,regs);
    require(repeat.sample(-0.1f,0)==0xff000014&&repeat.sample(2.2f,0)==0xff00000a,"Negative/positive repeat addressing wrong");
    require(repeat.sample(-0.01f,0)==0xff00000a,"Coordinates were not truncated to1/16 toward zero");
    require(clamp.sample(std::numeric_limits<float>::max(),std::numeric_limits<float>::max())==0xff000028,"Huge finite coordinates overflowed clamp");
    require(repeat.sample(std::numeric_limits<float>::max(),0)==0xff00000a,"Huge finite coordinate overflowed repeat");
    rejects([&]{clamp.sample(std::numeric_limits<float>::infinity(),0);},"Infinity coordinate accepted");
    rejects([&]{clamp.sample(0,std::numeric_limits<float>::quiet_NaN());},"NaN coordinate accepted");
}
void four_direct_color_formats() {
    GuestMemory memory;constexpr std::uint32_t base=0x08800000;
    auto regs=registers(base,0,1,0,2);
    memory.store16(base,0x001f);memory.store16(base+2,0xf800);
    badman::Texture rgb565(memory,regs);
    require(rgb565.sample(0,0)==0xff0000ff&&rgb565.sample(1,0)==0xffff0000,"565 BGR channels/opaque alpha wrong");
    memory.store16(base,0x07e0);require(rgb565.sample(0,0)==0xff00ff00,"565 six-bit green expansion wrong");
    regs[0xc3]=1;memory.store16(base,0x801f);memory.store16(base+2,0x7c00);badman::Texture rgba5551(memory,regs);
    require(rgba5551.sample(0,0)==0xff0000ff&&rgba5551.sample(1,0)==0x00ff0000,"5551 alpha/channel expansion wrong");
    regs[0xc3]=2;memory.store16(base,0xf321);badman::Texture rgba4444(memory,regs);
    require(rgba4444.sample(0,0)==0xff332211,"4444 nibble replication wrong");
    regs[0xc3]=3;memory.store32(base,0x12345678);badman::Texture rgba8888(memory,regs);
    require(rgba8888.sample(0,0)==0x12345678,"8888 texel was reordered");
}
void fixed_bilinear_and_filter_selection() {
    GuestMemory memory;constexpr std::uint32_t base=0x08800000;
    auto regs=registers(base,3,1,1,2);regs[0xc6]=0x100; // nearest min, linear mag
    memory.store32(base,0);memory.store32(base+4,0);memory.store32(base+8,0);memory.store32(base+12,0xffffffff);
    badman::Texture linear(memory,regs);
    require(linear.sample(0.5f,0.5f)==0,"Bilinear texel center shifted");
    require(linear.sample(1,1)==0x3f3f3f3f,"Bilinear did not truncate horizontal then vertical4-bit lerps");
    memory.store32(base+4,0xffffffff);
    require(linear.sample(0.5625f,0.5f)==0x0f0f0f0f,"Bilinear one-sixteenth fraction incorrect");
    require(linear.sample(0.55f,0.5f)==0,"Bilinear used float fraction instead of truncated4-bit weight");
    regs[0xc6]=7;badman::Texture observed(memory,regs);
    require(observed.sample(1,1)==0xffffffff,"Observed mag-nearest filter ignored");
    require(observed.sample(1,1,true)==0x7f7f7f7f,"Single-level mipfilter did not reduce to linear base-level minification");
    memory.store32(base,0xff00000a);memory.store32(base+4,0xff00001e);
    regs[0xc6]=0x100;regs[0xc7]=0;badman::Texture repeat(memory,regs);
    require(repeat.sample(0,0.5f)==0xff000014,"Bilinear repeat failed to blend across edge");
    regs[0xc7]=0x101;badman::Texture clamp(memory,regs);
    require(clamp.sample(0,0.5f)==0xff00000a,"Bilinear clamp sampled opposite edge");
}
void block_swizzle() {
    GuestMemory memory;constexpr std::uint32_t base=0x08800000;
    const std::array<std::uint32_t,4> colors{0xff0000ff,0xff00ff00,0xffff0000,0xffffffff};
    // Four independent128-byte tiles:2 horizontal blocks and2 vertical blocks.
    for(unsigned tile=0;tile<4;++tile)for(unsigned offset=0;offset<128;offset+=4)memory.store32(base+tile*128+offset,colors[tile]);
    auto regs=registers(base,3,3,4,8);regs[0xc2]=1;badman::Texture texture(memory,regs);
    require(texture.sample(0,0)==colors[0]&&texture.sample(7,7)==colors[1]&&
            texture.sample(0,8)==colors[2]&&texture.sample(7,15)==colors[3],"16-byte by8-row swizzle order wrong");
    regs=registers(base,3,1,0,2);regs[0xc2]=1;
    rejects([&]{badman::Texture invalid(memory,regs);},"Unsupported sub16-byte swizzle row pitch accepted");
    regs=registers(0x09ffffc0,3,1,0,4);regs[0xc2]=1;
    rejects([&]{badman::Texture invalid(memory,regs);},"Swizzle padded tile footprint not validated");
}
void bounds_and_unsupported_state() {
    GuestMemory memory;
    auto regs=registers(0x04001000,3,1,0,2);memory.store32(0x44001000,0x87654321);
    badman::Texture vram(memory,regs);require(vram.sample(0,0)==0x87654321,"VRAM cached alias data not sampled");
    regs=registers(0x09fffff0,3,1,0,4);badman::Texture last_ram(memory,regs);
    regs[0xa8]=(regs[0xa8]&0xf0000u)|8u;
    rejects([&]{badman::Texture invalid(memory,regs);},"Full row pitch outside RAM accepted");
    regs=registers(0x047ffff0,3,1,0,8);
    rejects([&]{badman::Texture invalid(memory,regs);},"Texture outside VRAM address span accepted");
    regs=registers(0x07000000);
    rejects([&]{badman::Texture invalid(memory,regs);},"Texture outside RAM/VRAM accepted");
    for(auto format:{4u,5u,6u,7u,8u,15u}) {
        regs=registers();regs[0xc3]=format;
        rejects([&]{badman::Texture invalid(memory,regs);},"Unsupported indexed/compressed format silently accepted");
    }
    const std::array<std::pair<unsigned,std::uint32_t>,10> bad_states{{{0xc2,0x10000},{0xc2,0x10},{0xc3,0x10003},
        {0xc6,2},{0xc6,0x200},{0xc7,2},{0xc8,1},{0xb8,0xa},{0xa8,0x0801},{0xa0,0x800004}}};
    for(const auto &[command,value]:bad_states) {
        regs=registers();regs[command]=value;
        rejects([&]{badman::Texture invalid(memory,regs);},"Malformed or unsupported texture flags accepted");
    }
}
void smaller_pitch_linear_and_swizzled_aliases() {
    GuestMemory memory;constexpr std::uint32_t base=0x08800000;
    auto regs=registers(base,3,2,1,2);
    for(unsigned texel=0;texel<6;++texel)memory.store32(base+texel*4,0xFF000000|texel);
    badman::Texture linear(memory,regs);
    require(linear.sample(2,0)==linear.sample(0,1)&&linear.sample(3,0)==linear.sample(1,1)&&
        linear.sample(3,1)==0xFF000005,"Smaller linear TBW did not alias subsequent physical rows");
    require(linear.sample(100,1)==0xFF000005,"Linear clamp used pitch instead of logical width");
    regs[0xc7]=0;badman::Texture repeat(memory,regs);
    require(repeat.sample(4,1)==0xFF000002&&repeat.sample(-1,0)==0xFF000003,
        "Linear repeat used pitch instead of logical width");
    regs=registers(base,3,3,4,4);regs[0xc2]=1;
    const std::array<std::uint32_t,3> colors{0xFF000000,0xFF0000FF,0xFF00FF00};
    for(unsigned tile=0;tile<colors.size();++tile)for(unsigned byte=0;byte<128;byte+=4)
        memory.store32(base+tile*128+byte,colors[tile]);
    badman::Texture swizzled(memory,regs);
    require(swizzled.sample(4,0)==colors[1]&&swizzled.sample(0,8)==colors[1]&&
        swizzled.sample(7,15)==colors[2],"Smaller swizzled TBW did not alias a horizontal tile with the following band");
    require(swizzled.sample(100,0)==colors[1]&&swizzled.sample(100,100)==colors[2],
        "Swizzled clamp lost the logical texture boundary");
    memory.store32(base+128,0x12345678);
    require(swizzled.sample(4,0)==0x12345678&&swizzled.sample(0,8)==0x12345678,
        "Aliased texels did not read the same actual guest bytes");
    memory.store32(base+128,colors[1]);
    regs[0xc6]=0x100;badman::Texture filtered(memory,regs);
    require(filtered.sample(4,0.5f)==0xFF00007F&&filtered.sample(4,8)==0xFF003F7F,
        "Bilinear samples across aliased tile/row boundaries blended the wrong guest texels");
    require(filtered.sample(8,0.5f)==colors[1],"Bilinear clamp wrapped at logical width");
    regs[0xc7]=0;badman::Texture filtered_repeat(memory,regs);
    require(filtered_repeat.sample(8,0.5f)==0xFF00007F,
        "Bilinear repeat did not blend logical last/first texels with smaller TBW");
}
void smaller_pitch_indexed_swizzle_and_observed_state() {
    GuestMemory memory;constexpr std::uint32_t base=0x08800000,palette_base=0x08810000;
    auto regs=registers(base,5,5,4,16);regs[0xc2]=1;regs[0xc5]=0xFF03;
    regs[0xb0]=palette_base&0xFFFFFF;regs[0xb1]=0x080000;
    const std::array<std::uint32_t,3> colors{0xFF000000,0xFF0000FF,0xFF00FF00};
    for(unsigned i=0;i<colors.size();++i)memory.store32(palette_base+i*4,colors[i]);
    badman::Clut palette;palette.load(memory,regs,1);
    for(unsigned tile=0;tile<3;++tile)for(unsigned byte=0;byte<128;++byte)
        memory.store8(base+tile*128+byte,static_cast<std::uint8_t>(tile));
    badman::Texture indexed(memory,regs,&palette);
    require(indexed.sample(16,0)==colors[1]&&indexed.sample(0,8)==colors[1]&&indexed.sample(31,15)==colors[2],
        "T8 small TBW used a fabricated padded row or wrong swizzled alias");
    memory.store8(base+128,2);
    require(indexed.sample(16,0)==colors[2]&&indexed.sample(0,8)==colors[2],"T8 alias did not observe actual shared index bytes");
    memory.store8(base+128,1);regs[0xc6]=0x100;badman::Texture filtered(memory,regs,&palette);
    require(filtered.sample(16,0.5f)==0xFF00007F&&filtered.sample(32,15.5f)==colors[2],
        "T8 alias boundary filtering/clamp did not interpolate expanded palette colors");
    regs[0xc7]=0;badman::Texture repeat(memory,regs,&palette);
    require(repeat.sample(32,0.5f)==0xFF00007F,"T8 repeat boundary used physical pitch instead of logical width");
    // Actual post-logo GE words: logical512x512, T8 pitch480, swizzled.
    regs={};regs[0xA0]=0xA029F880;regs[0xA8]=0xA80901E0;regs[0xB8]=0xB8000909;
    regs[0xC2]=0xC2000001;regs[0xC3]=0xC3000005;regs[0xC5]=0xC500FF03;regs[0xC7]=0xC7000101;
    memory.store8(0x0929F880+3840,1); // x480,y0 == x0,y8
    memory.store8(0x0929F880+246015,2); // Final logical texel of final aliased tile.
    badman::Texture observed(memory,regs,&palette);
    require(observed.width()==512&&observed.height()==512&&observed.sample(480,0)==colors[1]&&
        observed.sample(0,8)==colors[1]&&observed.sample(511,511)==colors[2],
        "Observed512/480 T8 GE state did not resolve real aliased guest indices");
}
void smaller_pitch_complete_footprint_guards() {
    GuestMemory memory;constexpr std::uint32_t ram_end=0x0A000000;
    // Linear width4/stride2/height4 accesses40 bytes, exceeding32-byte pitch allocation.
    auto regs=registers(ram_end-48,3,2,2,2);
    memory.store32(ram_end-12,0x12345678);badman::Texture fits(memory,regs);
    require(fits.sample(3,3)==0x12345678,"Last aliased linear texel did not fit its validated range");
    regs=registers(ram_end-32,3,2,2,2);
    rejects([&]{badman::Texture invalid(memory,regs);},"Linear smaller-pitch footprint overran RAM despite fitting pitch*height");
    // Three128-byte tiles are reachable even though pitch*height only spans256.
    regs=registers(ram_end-384,3,3,4,4);regs[0xC2]=1;
    memory.store32(ram_end-4,0x87654321);badman::Texture swizzle_fits(memory,regs);
    require(swizzle_fits.sample(7,15)==0x87654321,"Final aliased swizzle texel did not fit exact complete-tile guard");
    regs=registers(ram_end-368,3,3,4,4);regs[0xC2]=1;
    rejects([&]{badman::Texture invalid(memory,regs);},"Aliased swizzle tile exceeded RAM without rejection");
    auto indexed_regs=registers(0x08800000,5,5,4,16);indexed_regs[0xC5]=0xFF03;
    indexed_regs[0xB0]=0x810000;indexed_regs[0xB1]=0x080000;badman::Clut palette;palette.load(memory,indexed_regs,1);
    indexed_regs=registers(ram_end-384,5,5,4,16);indexed_regs[0xC2]=1;indexed_regs[0xC5]=0xFF03;
    memory.store8(ram_end-1,0);badman::Texture t8_fits(memory,indexed_regs,&palette);t8_fits.sample(31,15);
    indexed_regs=registers(ram_end-368,5,5,4,16);indexed_regs[0xC2]=1;indexed_regs[0xC5]=0xFF03;
    rejects([&]{badman::Texture invalid(memory,indexed_regs,&palette);},"T8 aliased swizzle tile exceeded RAM without rejection");
    constexpr auto vram_end=GuestMemory::kVramPhysicalBase+GuestMemory::kVramAddressSpan;
    regs=registers(vram_end-384,3,3,4,4);regs[0xC2]=1;badman::Texture mirror_fits(memory,regs);
    regs=registers(vram_end-368,3,3,4,4);regs[0xC2]=1;
    rejects([&]{badman::Texture invalid(memory,regs);},"Aliased swizzle footprint crossed the mapped VRAM mirror span");
}
void observed_command_word_state() {
    GuestMemory memory;Registers regs{};
    regs[0xa0]=0xa0d46250;regs[0xa8]=0xa8090008;regs[0xb8]=0xb8000303;
    regs[0xc2]=0xc2000000;regs[0xc3]=0xc3000003;regs[0xc6]=0xc6000007;regs[0xc7]=0xc7000101;regs[0xc8]=0xc8000000;
    memory.store32(0x09d46250,0x01020304);badman::Texture texture(memory,regs);
    require(texture.width()==8&&texture.height()==8&&texture.sample(0,0)==0x01020304,"Actual full GE commandword state was decoded incorrectly");
}
void indexed_nibbles_stride_latching_and_filter() {
    GuestMemory memory; constexpr std::uint32_t base=0x08800000,palette_base=0x08810000;
    const std::array<std::uint32_t,4> colors{0xFF0000FF,0xFF00FF00,0xFFFF0000,0xFFFFFFFF};
    auto regs=registers(base,4,1,1,4); regs[0xc5]=0xFF03;
    regs[0xb0]=palette_base&0xFFFFFF;regs[0xb1]=0x080000;
    for(unsigned i=0;i<colors.size();++i)memory.store32(palette_base+i*4,colors[i]);
    badman::Clut palette;palette.load(memory,regs,1);
    memory.store8(base,0x10);memory.store8(base+1,0xFF); // Padding must not become the next row.
    memory.store8(base+2,0x32);memory.store8(base+3,0xFF);
    badman::Texture t4(memory,regs,&palette);
    require(t4.sample(0,0)==colors[0]&&t4.sample(1,0)==colors[1]&&
            t4.sample(0,1)==colors[2]&&t4.sample(1,1)==colors[3],"T4 nibble order or texel-to-byte row pitch is wrong");
    memory.store32(palette_base,0);
    require(t4.sample(0,0)==colors[0],"Indexed texture bypassed the latched palette");
    regs[0xc6]=0x100;badman::Texture linear(memory,regs,&palette);
    require(linear.sample(1,1)==0xFF7F7F7F,"Indexed filtering interpolated indices instead of expanded palette colors");
    regs[0xc6]=0;regs[0xc3]=5;
    memory.store8(base,2);memory.store8(base+1,0);memory.store8(base+4,1);memory.store8(base+5,3);
    badman::Texture t8(memory,regs,&palette);
    require(t8.sample(0,0)==colors[2]&&t8.sample(1,1)==colors[3],"T8 index byte or padded row stride is wrong");
    memory.store8(base,8);
    rejects([&]{t8.sample(0,0);},"Indexed lookup fabricated an unloaded palette entry");
    regs[0xc3]=4;regs[0xa8]=0x080003;
    rejects([&]{badman::Texture invalid(memory,regs,&palette);},"Unsupported odd T4 row pitch accepted");
}
void indexed_swizzle_and_observed_state() {
    GuestMemory memory; constexpr std::uint32_t base=0x08800000,palette_base=0x08810000;
    auto regs=registers(base,4,6,4,64);regs[0xc2]=1;regs[0xc5]=0xFF03;
    regs[0xb0]=palette_base&0xFFFFFF;regs[0xb1]=0x080000;
    for(unsigned i=0;i<8;++i)memory.store32(palette_base+i*4,0xFF000000|i*0x010101);
    badman::Clut palette;palette.load(memory,regs,1);
    for(unsigned tile=0;tile<4;++tile)
        for(unsigned byte=0;byte<128;++byte)memory.store8(base+tile*128+byte,static_cast<std::uint8_t>(tile*2|((tile*2+1)<<4)));
    badman::Texture swizzled(memory,regs,&palette);
    require(swizzled.sample(0,0)==0xFF000000&&swizzled.sample(31,7)==0xFF010101&&
            swizzled.sample(32,0)==0xFF020202&&swizzled.sample(0,8)==0xFF040404&&
            swizzled.sample(63,15)==0xFF070707,"T4 swizzle used texels instead of16-byte blocks or wrong nibble parity");
    regs={};regs[0xa0]=0xA0879AD0;regs[0xa8]=0xA8090020;regs[0xb8]=0xB8000404;
    regs[0xc3]=0xC3000004;regs[0xc5]=0xC500FF03;regs[0xb0]=0xB0879BD0;regs[0xb1]=0xB1090000;
    memory.store8(0x09879AD0,0x21);memory.store32(0x09879BD4,0x12345678);memory.store32(0x09879BD8,0xABCDEF12);
    palette.load(memory,regs,0xC4000020);
    badman::Texture observed(memory,regs,&palette);
    require(observed.width()==16&&observed.height()==16&&observed.sample(0,0)==0x12345678&&
            observed.sample(1,0)==0xABCDEF12,"Captured T4/full commandword CLUT state was decoded incorrectly");
}

void live_backing_after_construction_and_copy() {
    // The Texture object may retain a backing pointer, but not texel contents.
    // Cached/uncached CPU aliases and physical EDRAM mirrors name live bytes.
    for(const auto size:{32u*1024u*1024u,64u*1024u*1024u}) {
        GuestMemory memory(size);
        for(const auto address:{0x08800000u,GuestMemory::kPhysicalBase+size-16u,
                                0x04001000u,0x04201000u,0x04601000u}) {
            auto regs=registers(address,3,1,0,2);
            memory.store32(address,0x11223344);memory.store32(address+4,0x55667788);
            badman::Texture texture(memory,regs);const auto copy=texture;
            require(texture.sample(0,0)==0x11223344&&copy.sample(1,0)==0x55667788,
                    "Constructed/copied Texture did not read independent initial literals");
            memory.store32(address|0x40000000u,0xA1B2C3D4);
            require(texture.sample(0,0)==0xA1B2C3D4&&copy.sample(0,0)==0xA1B2C3D4,
                    "Texture construction/copy latched texels instead of live CPU backing");
            if(address<GuestMemory::kPhysicalBase) {
                memory.store32(0x04001004u,0x98765432);
                require(texture.sample(1,0)==0x98765432&&copy.sample(1,0)==0x98765432,
                        "Texture retained stale bytes after a physical EDRAM mirror write");
            }
        }
    }
}
void physical_vram_wrap_retains_live_direct_and_indexed_reads() {
    GuestMemory memory;constexpr auto address=0x041FFFF0u;
    require(memory.contains(address,32)&&
            static_cast<const GuestMemory&>(memory).raw_pointer(address,32)==nullptr,
            "Physical wrap fixture must be legal guest space without contiguous host backing");
    auto regs=registers(address,3,2,1,4);
    memory.store32(address,0x11223344);memory.store32(0x04200000u,0xAABBCCDD);
    badman::Texture direct(memory,regs);const auto copy=direct;
    require(direct.sample(0,0)==0x11223344&&direct.sample(0,1)==0xAABBCCDD,
            "Direct Texture crossed the physical EDRAM endpoint incorrectly");
    memory.store32(0x44000000u,0x87654321);
    require(direct.sample(0,1)==0x87654321&&copy.sample(0,1)==0x87654321,
            "Wrapped direct/copy Texture ignored a live alias update");

    constexpr auto palette_base=0x08810000u;
    const std::array<std::uint32_t,4> colors{0xFF0000FF,0xFF00FF00,0xFFFF0000,0xFFFFFFFF};
    regs=registers(address,4,1,1,32);regs[0xc5]=0xFF03;
    regs[0xb0]=palette_base&0xFFFFFFu;regs[0xb1]=0x080000;
    for(unsigned i=0;i<colors.size();++i)memory.store32(palette_base+i*4,colors[i]);
    badman::Clut palette;palette.load(memory,regs,1);
    memory.store8(address,0x10);memory.store8(0x04200000u,0x32);
    badman::Texture indexed(memory,regs,&palette);
    require(indexed.sample(0,0)==colors[0]&&indexed.sample(1,0)==colors[1]&&
            indexed.sample(0,1)==colors[2]&&indexed.sample(1,1)==colors[3],
            "T4 physical wrapping lost low/high nibbles or row pitch");
    memory.store8(0x44000000u,0x01);
    require(indexed.sample(0,1)==colors[1]&&indexed.sample(1,1)==colors[0],
            "Wrapped indexed Texture latched its index byte instead of live backing");
}
void logical_vram_end_rejects_before_sampling() {
    GuestMemory memory;constexpr auto last=0x047FFFF0u;
    require(memory.contains(last,16)&&!memory.contains(last,32),
            "Logical EDRAM endpoint fixture does not distinguish complete footprint bounds");
    auto regs=registers(last,3,2,0,4);
    memory.store32(last,0x12345678);
    badman::Texture exact_end(memory,regs);
    require(exact_end.sample(0,0)==0x12345678,"Texture ending exactly at logical EDRAM limit was rejected");
    regs[0xb8]=2u|(1u<<8u); // Two 16-byte rows cross the logical 8 MiB window.
    bool failed=false;
    try {badman::Texture invalid(memory,regs);}
    catch(const psprecomp::Error &e) {
        failed=true;
        require(std::string(e.what())=="GE texture: complete texture footprint lies outside guest RAM/EDRAM at 0x047FFFF0",
                "Logical EDRAM end did not retain constructor error priority/text");
    }
    require(failed,"Texture accepted a physical mirror span beyond the logical EDRAM window");
    require(memory.load32(last)==0x12345678,"Rejected Texture mutated EDRAM");
}
}
int main() {
    try {
        row_pitch_nearest_and_addressing();four_direct_color_formats();fixed_bilinear_and_filter_selection();
        block_swizzle();bounds_and_unsupported_state();observed_command_word_state();
        indexed_nibbles_stride_latching_and_filter();indexed_swizzle_and_observed_state();
        smaller_pitch_linear_and_swizzled_aliases();smaller_pitch_indexed_swizzle_and_observed_state();
        smaller_pitch_complete_footprint_guards();
        live_backing_after_construction_and_copy();
        physical_vram_wrap_retains_live_direct_and_indexed_reads();
        logical_vram_end_rejects_before_sampling();
        std::cout<<"Texture tests passed\n";return 0;
    } catch(const std::exception &error) {std::cerr<<error.what()<<'\n';return 1;}
}

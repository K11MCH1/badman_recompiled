#include "badman_host.hpp"
#include "psprecomp/common.hpp"
#include <array>
#include <bit>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
using badman::Atrac;
using psprecomp::GuestMemory;
constexpr std::uint32_t input = 0x08820000, pcm = 0x08824000, metadata = 0x08828000;
void require(bool test, const char *message) { if (!test) throw std::runtime_error(message); }
template<class F> void rejects(F operation, const char *message) {
    bool rejected = false; try { operation(); } catch (const psprecomp::Error &) { rejected = true; }
    require(rejected, message);
}
void put16(std::vector<std::uint8_t> &data, std::size_t at, unsigned value) {
    data[at] = static_cast<std::uint8_t>(value); data[at+1] = static_cast<std::uint8_t>(value >> 8);
}
void put32(std::vector<std::uint8_t> &data, std::size_t at, std::uint32_t value) {
    put16(data, at, value); put16(data, at+2, value >> 16);
}
std::vector<std::uint8_t> fixture() {
    // Header-only metadata and four synthetic eight-byte packets, not encoded game audio.
    std::vector<std::uint8_t> data(128); std::memcpy(data.data(),"RIFF",4); put32(data,4,120);
    std::memcpy(data.data()+8,"WAVEfmt ",8); put32(data,16,52); put16(data,20,0xFFFE);
    put16(data,22,2); put32(data,24,44100); put16(data,32,8); put16(data,36,34);
    put16(data,38,2048); put32(data,40,3);
    const std::array<std::uint8_t,16> guid{0xBF,0xAA,0x23,0xE9,0x58,0xCB,0x71,0x44,0xA1,0x19,0xFF,0xFA,0x01,0xE4,0xCE,0x62};
    std::copy(guid.begin(),guid.end(),data.begin()+44);
    std::memcpy(data.data()+72,"fact",4); put32(data,76,8); put32(data,80,5000); put32(data,84,2048);
    std::memcpy(data.data()+88,"data",4); put32(data,92,32); return data;
}
std::int16_t sample(std::uint32_t frame, unsigned channel) {
    return std::bit_cast<std::int16_t>(static_cast<std::uint16_t>(frame * 3 + channel * 17001));
}
class Decoder final : public badman::AtracDecoder {
    unsigned &calls_;
public:
    explicit Decoder(unsigned &calls) : calls_(calls) {}
    std::vector<std::int16_t> decode(const badman::AtracRiff &info, std::span<const std::uint8_t> bytes) override {
        ++calls_; require(info.block_align == 8 && bytes.size() == 32,"HLE passed invalid decoder extent");
        std::vector<std::int16_t> result(8192*2);
        for(unsigned i=0;i<8192;++i)for(unsigned c=0;c<2;++c)result[i*2+c]=sample(i,c);
        return result;
    }
};
struct Test {
    psprecomp::Runtime runtime;
    std::ostringstream trace;
    badman::Host host{runtime,0x089F0000,0x089E0000,0x0891F420,trace};
    unsigned calls{};
    Test() {
        host.install({}); host.set_atrac_decoder_factory([this]{return std::make_unique<Decoder>(calls);});
        runtime.memory().copy_in(input,fixture());
    }
    std::uint32_t invoke(std::uint32_t nid,std::initializer_list<std::uint32_t> args) {
        auto &ctx=runtime.cpu();for(unsigned i=4;i<12;++i)ctx.gpr[i]=0;
        unsigned i=4;for(auto value:args)ctx.gpr[i++]=value;
        runtime.invoke_import("sceAtrac3plus",nid,ctx);return ctx.gpr[2];
    }
    void fill() {
        for(unsigned i=0;i<24;++i)runtime.memory().store8(metadata+i,0xA5);
        runtime.memory().store32(pcm,0x5A5A5A5A);
    }
    void untouched() {
        for(unsigned i=0;i<24;++i)require(runtime.memory().load8(metadata+i)==0xA5,"Invalid HLE call wrote metadata");
        require(runtime.memory().load32(pcm)==0x5A5A5A5A,"Invalid HLE call wrote PCM");
    }
};
void queries_and_validation() {
    Test test; auto &m=test.runtime.memory();
    rejects([&]{test.invoke(0x7A20E7AF,{input,0});},"Zero resident input size accepted");
    rejects([&]{test.invoke(0x7A20E7AF,{input+1,128});},"Unaligned resident input accepted");
    rejects([&]{test.invoke(0x7A20E7AF,{0x09FFFFFC,128});},"Cross-end resident input accepted");
    require(test.calls==0,"Invalid resident input reached decoder");
    require(test.invoke(0x7A20E7AF,{input,128})==0 && test.calls==1,"Resident HLE initialization failed");
    rejects([&]{test.host.set_atrac_decoder_factory({});},"Active decoder factory replaced");
    test.fill();require(test.invoke(0xD6A5F2F7,{0,metadata})==0&&m.load32(metadata)==2048,"Max-sample HLE query wrong");
    require(test.invoke(0x9AE849A7,{0,metadata+4})==0&&m.load32(metadata+4)==0xFFFFFFFF,"Resident remaining count wrong");
    require(test.invoke(0xA2BBA8BE,{0,metadata+8,metadata+12,metadata+16})==0&&m.load32(metadata+8)==4999&&
        m.load32(metadata+12)==0xFFFFFFFF&&m.load32(metadata+16)==0xFFFFFFFF,"Sound sample HLE layout wrong");
    require(test.invoke(0x868120B5,{0,0})==static_cast<std::uint32_t>(Atrac::no_loop),"Absent loop returned fake success");
    test.fill();require(test.invoke(0x83E85EA0,{0,metadata,metadata+4})==static_cast<std::uint32_t>(Atrac::no_second_buffer)&&
        m.load32(metadata)==0&&m.load32(metadata+4)==0&&m.load32(metadata+8)==0xA5A5A5A5,"Second buffer valid-error output rule wrong");
    test.fill();require(test.invoke(0x83E85EA0,{99,metadata,metadata+4})==static_cast<std::uint32_t>(Atrac::bad_id),"Invalid second-buffer ID wrong");test.untouched();
    rejects([&]{test.invoke(0x83E85EA0,{0,metadata,0});},"Null second buffer output accepted");test.untouched();
    rejects([&]{test.invoke(0x83E85EA0,{0,metadata,metadata|0x40000000});},"Aliased second-buffer outputs accepted");test.untouched();
    rejects([&]{test.invoke(0xA2BBA8BE,{0,metadata,metadata+4,0});},"Invalid third sound output accepted");test.untouched();
    rejects([&]{test.invoke(0xA2BBA8BE,{0,metadata,metadata+4,metadata|0x80000000});},"Aliased third sound output accepted");test.untouched();
    rejects([&]{test.invoke(0xD6A5F2F7,{0,metadata+1});},"Unaligned integer output accepted");test.untouched();
    rejects([&]{test.invoke(0x9AE849A7,{0,0x04000000});},"VRAM metadata output silently accepted");test.untouched();
    test.invoke(0x61EB33F5,{0});test.host.set_atrac_decoder_factory({});
    rejects([&]{test.invoke(0x7A20E7AF,{input,128});},"Missing decoder factory became silent HLE success");
}
void transactional_decode_and_abi() {
    Test test; auto &m=test.runtime.memory();require(test.invoke(0x7A20E7AF,{input,128})==0,"Decode fixture init failed");test.fill();
    const auto bad=[&](std::uint32_t output,std::uint32_t count,std::uint32_t end,std::uint32_t remain) {
        rejects([&]{test.invoke(0x6A8C3CD5,{0,output,count,end,remain});},"Invalid decode outputs accepted");test.untouched();
    };
    bad(pcm,metadata,metadata+4,0); // Argument five MUST be validated from r8.
    bad(pcm,metadata,metadata,metadata+8);
    bad(pcm,metadata,metadata+4,metadata|0x40000000);
    bad(metadata-2,metadata,metadata+4,metadata+8);
    bad(pcm+1,metadata,metadata+4,metadata+8);
    bad(0x09FFFFFE,metadata,metadata+4,metadata+8);
    bad(0x04000000,metadata,metadata+4,metadata+8);
    // Exact first output extent fits at RAM end, where a max8192-byte check would fail.
    constexpr auto end_pcm=GuestMemory::kPhysicalBase+32*1024*1024-1680*4;
    m.store16(end_pcm-2,0x5A5A);test.runtime.cpu().gpr[29]=0xFFFFFFFF;
    require(test.invoke(0x6A8C3CD5,{0,end_pcm,metadata,metadata+4,metadata+8})==0&&
        m.load32(metadata)==1680&&m.load32(metadata+4)==0&&m.load32(metadata+8)==0xFFFFFFFF,
        "First PCM decode changed cursor before failed validation or used o32 stack argument");
    require(m.load16(end_pcm-2)==0x5A5A,"PCM pre-guard overwritten");
    for(unsigned i=0;i<1680;++i)for(unsigned c=0;c<2;++c)
        require(m.load16(end_pcm+i*4+c*2)==std::bit_cast<std::uint16_t>(sample(2416+i,c)),"PSP PCM byte order/channel/position wrong");
    m.store32(pcm,0x5A5A5A5A);
    require(test.invoke(0x6A8C3CD5,{0,0,metadata,metadata+4,metadata+8})==0&&m.load32(metadata)==2048&&
        m.load32(metadata+4)==0&&m.load32(pcm)==0x5A5A5A5A,"Null PCM discard did not advance real state");
    m.store32(pcm+1272*4,0xA5A5A5A5);
    require(test.invoke(0x6A8C3CD5,{0,pcm|0x40000000,metadata,metadata+4,metadata+8})==0&&m.load32(metadata)==1272&&
        m.load32(metadata+4)==1&&m.load16(pcm)==std::bit_cast<std::uint16_t>(sample(6144,0))&&m.load32(pcm+1272*4)==0xA5A5A5A5,
        "Cached alias/final-frame extent or end flag wrong");
    test.fill();require(test.invoke(0x6A8C3CD5,{0,pcm,metadata,metadata+4,metadata+8})==static_cast<std::uint32_t>(Atrac::all_decoded),"End error missing");test.untouched();
    require(test.invoke(0x6A8C3CD5,{99,0,0,0,0})==static_cast<std::uint32_t>(Atrac::bad_id),"Invalid ID touched bad output pointers");
    require(test.invoke(0x61EB33F5,{0})==0&&test.invoke(0x61EB33F5,{0})==static_cast<std::uint32_t>(Atrac::bad_id),"Release HLE lifecycle wrong");
}
}
int main() {
    try { queries_and_validation();transactional_decode_and_abi();
        std::cout<<"ATRAC HLE ABI/transactional guest-memory tests passed; injected synthetic decoder, no device.\n";return 0;
    }catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}

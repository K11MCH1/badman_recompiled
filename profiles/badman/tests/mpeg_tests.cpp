#include "badman_mpeg.hpp"
#include "psprecomp/common.hpp"
#include <array>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
using namespace psprecomp;
constexpr std::uint32_t ring = 0x08804000, header = 0x08805000, pointer = 0x08806000;
constexpr std::uint32_t output = 0x08806100, au = 0x08806200, destination_pointer = 0x08806300;
constexpr std::uint32_t data = 0x08810000, workspace = 0x08820000, destination = 0x08900000;
void require(bool value, const char *reason) { if (!value) throw std::runtime_error(reason); }
template<class F> void rejects(F action, const char *reason) {
    bool rejected = false; try { action(); } catch (const Error &) { rejected = true; } require(rejected, reason);
}
void big(std::vector<std::uint8_t> &bytes, unsigned offset, std::uint32_t value) {
    for (unsigned i = 0; i < 4; ++i) bytes[offset + i] = static_cast<std::uint8_t>(value >> ((3 - i) * 8));
}
std::vector<std::uint8_t> psmf(std::uint32_t packets = 4) {
    std::vector<std::uint8_t> bytes(2048);
    const std::string magic = "PSMF0015"; std::copy(magic.begin(), magic.end(), bytes.begin());
    big(bytes, 8, 2048); big(bytes, 12, packets * 2048); bytes[0x81] = 1; bytes[0x82] = 0xE0;
    bytes[0x8E] = 30; bytes[0x8F] = 17; return bytes;
}
std::vector<std::uint8_t> packet(std::uint8_t value, std::uint64_t pts = 90000) {
    std::vector<std::uint8_t> bytes(2048, 0xFF);
    const std::array<std::uint8_t, 14> pack{0,0,1,0xBA,0x44,0,0,0,0,0,0,0,0,0};
    std::copy(pack.begin(), pack.end(), bytes.begin());
    const std::array<std::uint8_t, 14> pes{0,0,1,0xE0,0,9,0x80,0x80,5,
        static_cast<std::uint8_t>(0x21 | ((pts >> 29) & 14)), static_cast<std::uint8_t>(pts >> 22),
        static_cast<std::uint8_t>(((pts >> 14) & 254) | 1), static_cast<std::uint8_t>(pts >> 7),
        static_cast<std::uint8_t>(((pts << 1) & 254) | 1)};
    std::copy(pes.begin(), pes.end(), bytes.begin() + 14); bytes[28] = value;
    bytes[29]=0; bytes[30]=0; bytes[31]=1; bytes[32]=0xBE;
    const auto padding = 2048 - 35; bytes[33] = padding >> 8; bytes[34] = padding & 255;
    return bytes;
}
std::vector<std::uint8_t> audio_frame(std::uint8_t value) {
    std::vector<std::uint8_t> result{0x0F,0xD0,0x28,0,0,0,0,0};
    result.insert(result.end(),8,value);return result;
}
std::vector<std::uint8_t> mixed_packet(std::uint8_t video,std::span<const std::uint8_t> audio,
                                     unsigned first_frame=0,std::uint64_t audio_pts=85069) {
    auto result=packet(video);auto pes=packet(0,audio_pts);
    const auto size=12+audio.size();
    std::copy_n(pes.begin()+14,14,result.begin()+29);result[32]=0xBD;
    result[33]=static_cast<std::uint8_t>(size>>8);result[34]=static_cast<std::uint8_t>(size);
    result[43]=result[44]=0;result[45]=static_cast<std::uint8_t>(first_frame>>8);result[46]=static_cast<std::uint8_t>(first_frame);
    std::copy(audio.begin(),audio.end(),result.begin()+47);
    const auto padding=47+audio.size();require(padding+6<=result.size(),"Synthetic audio packet too large");
    result[padding]=0;result[padding+1]=0;result[padding+2]=1;result[padding+3]=0xBE;
    const auto tail=result.size()-padding-6;result[padding+4]=tail>>8;result[padding+5]=tail&255;
    return result;
}
class Decoder final : public badman::VideoDecoder {
    std::uint64_t bytes_{};
public:
    std::vector<badman::EncodedVideo> parse(std::span<const std::uint8_t> bytes, std::int64_t pts,
        std::int64_t dts, bool) override {
        bytes_ += bytes.size(); if (bytes.empty()) return {};
        return {{{bytes.begin(), bytes.end()}, bytes[0] == 0xEE ? badman::video_no_timestamp : pts, dts, bytes_}};
    }
    std::optional<badman::VideoFrame> decode(const badman::EncodedVideo &unit) override {
        badman::VideoFrame frame{480,272,unit.pts,{}}; frame.rgba.resize(480*272*4);
        for (std::size_t i = 0; i < frame.rgba.size(); i += 4) {
            frame.rgba[i] = unit.bytes[0]; frame.rgba[i+1] = 0x22; frame.rgba[i+2] = 0x33; frame.rgba[i+3] = 0xFF;
        }
        return frame;
    }
    std::optional<badman::VideoFrame> drain() override { return {}; }
    void reset() override { bytes_ = 0; }
};
class AudioDecoder final : public badman::MpegAudioDecoder {
    unsigned index_{};
public:
    std::vector<std::int16_t> decode(std::span<const std::uint8_t> bytes) override {
        std::vector<std::int16_t> result(4096);
        for (unsigned i=0;i<result.size();++i) result[i]=static_cast<std::int16_t>(bytes[0]+index_*256+i);
        ++index_;return result;
    }
    void reset() override { index_=0; }
};
struct Fixture {
    Runtime runtime;
    badman::Mpeg mpeg;
    std::optional<badman::Mpeg::RingCallback> call(std::uint32_t nid, std::initializer_list<std::uint32_t> arguments = {}) {
        auto &cpu = runtime.cpu(); for (unsigned i=4;i<12;++i) cpu.gpr[i]=0;
        unsigned i=4; for (auto value:arguments) cpu.gpr[i++]=value;
        cpu.gpr[28]=0x0891F420; return mpeg.invoke(nid,runtime,cpu);
    }
    std::uint32_t result() const { return runtime.cpu().gpr[2]; }
    explicit Fixture(unsigned capacity = 3, bool clock = false, bool audio = true)
        : mpeg([] { return std::make_unique<Decoder>(); }, audio ?
            badman::Mpeg::AudioDecoderFactory([] { return std::make_unique<AudioDecoder>(); }) : badman::Mpeg::AudioDecoderFactory{}) {
        auto metadata = psmf();
        if (clock) for (unsigned i = 0; i < 6; ++i) {
            metadata[0x54+i] = static_cast<std::uint8_t>(std::uint64_t(90000) >> ((5-i)*8));
            metadata[0x5A+i] = static_cast<std::uint8_t>(std::uint64_t(90000+4*3003) >> ((5-i)*8));
        }
        runtime.memory().copy_in(header,metadata);
        call(0x682A619B); require(result()==0&&mpeg.initialized(),"Real factory initialization was not performed");
        call(0xD7A29F46,{capacity}); require(result()==capacity*0x868,"Ringbuffer query did not use measured packet overhead");
        call(0x37295ED8,{ring,capacity,data,capacity*0x868,0x08808000,0x1234});
        call(0xD8C5F121,{pointer,workspace,0x10000,ring,512,0,0});
        require(result()==0&&runtime.memory().load32(pointer)==workspace,"MPEG workspace handle was not materialized");
        call(0x21FF80E4,{pointer,header,output}); require(runtime.memory().load32(output)==2048,"Stream offset wrong");
        call(0x42560F23,{pointer,0,0}); runtime.memory().store32(output+4,result());
        call(0xA780CF7E,{pointer}); require(result()==1,"AVC ES handle wrong");
        call(0x167AFD9E,{pointer,1,au}); runtime.memory().store32(destination_pointer,destination);
        runtime.memory().copy_in(destination,std::vector<std::uint8_t>(0x88000,0xA5));
    }
    void decode(std::uint8_t expected) {
        call(0xFE246728,{pointer,runtime.memory().load32(output+4),au,output+8}); require(result()==0,"Available AU not returned");
        call(0x0E3C2E9D,{pointer,au,512,destination_pointer,output+12});
        require(result()==0&&runtime.memory().load32(output+12)==1,"Decoded frame not reported");
        require(runtime.memory().load32(destination)==(0xFF332200u|expected),"MPEG output was not real decoder RGBA");
        require(runtime.memory().load32(destination+480*4)==0xA5A5A5A5,"MPEG decode overwrote framebuffer row padding");
    }
};
void parsing_and_rejection() {
    const auto h=psmf(); const auto parsed=badman::PsmfHeader::parse(h);
    require(parsed.stream_size==8192&&parsed.width==480&&parsed.height==272&&parsed.streams==1,"PSMF fields wrong");
    auto bad=h;bad[0]='X'; rejects([&]{badman::PsmfHeader::parse(bad);},"Bad PSMF magic accepted");
    badman::ProgramStream stream; const auto p=packet(0x11,0x100015F90LL);
    auto first=stream.feed(std::span(p).first(17)); require(first.empty(),"Partial PES produced a fabricated unit");
    auto second=stream.feed(std::span(p).subspan(17),true);
    require(second.size()==1&&second[0].bytes==std::vector<std::uint8_t>{0x11}&&second[0].pts==0x100015F90LL,
        "PES fragmentation or full33bit timestamp was lost");
    stream.reset(); auto audio=packet(1);audio[17]=0xBD;
    rejects([&]{stream.feed(audio,true);},"Unimplemented audio PES silently accepted");
    stream.reset(); auto wrong_prefix=packet(1);wrong_prefix[23]=0x11;
    rejects([&]{stream.feed(wrong_prefix,true);},"PES PTS accepted a DTS prefix despite PTS-only flags");
    stream.reset(); rejects([&]{stream.feed(std::span(p).first(20),true);},"Truncated declared stream accepted");
    Runtime runtime;badman::Mpeg unavailable;
    rejects([&]{unavailable.invoke(0x682A619B,runtime,runtime.cpu());},"Missing real decoder backend returned Init success");
    require(!unavailable.initialized(),"Failed Init changed library readiness");
}
void ring_callbacks_decode_and_wrap() {
    Fixture test;
    auto callback=test.call(0xB240A59E,{ring,2,3});
    require(callback&&callback->packets==2&&callback->data==data&&callback->function==0x08808000&&
        callback->argument==0x1234&&callback->gp==0x0891F420,"Real callback request ABI was lost");
    test.runtime.memory().copy_in(data,packet(0x11));test.runtime.memory().copy_in(data+2048,packet(0x12,93003));
    auto step=test.mpeg.complete_put(test.runtime.memory(),ring,2);require(!step.callback&&step.result==2,"Callback result not committed");
    test.decode(0x11);test.decode(0x12);
    test.call(0xB5F6DC87,{ring});require(test.result()==2,"Still-live packet was released too early or consumed packet retained");
    callback=test.call(0xB240A59E,{ring,2,2});require(callback&&callback->packets==1&&callback->data==data+4096,"First wrap segment wrong");
    test.runtime.memory().copy_in(callback->data,packet(0x13,96006));step=test.mpeg.complete_put(test.runtime.memory(),ring,1);
    require(step.callback&&step.callback->packets==1&&step.callback->data==data,"Callback wrap continuation wrong");
    test.runtime.memory().copy_in(data,packet(0x14,99009));step=test.mpeg.complete_put(test.runtime.memory(),ring,1);
    require(!step.callback&&step.result==2,"Wrapped submission count wrong");
    test.decode(0x13);test.decode(0x14);test.call(0xB5F6DC87,{ring});require(test.result()==3,"End-of-stream packets not released");
    test.call(0x740FCCD1,{pointer,512,destination_pointer,output+12});require(test.runtime.memory().load32(output+12)==0,"Drain fabricated a frame");
    test.call(0xCEB870B1,{pointer,1});test.call(0x591A4AA2,{pointer,test.runtime.memory().load32(output+4)});
    test.call(0x606A4649,{pointer});test.call(0x13407F13,{ring});test.call(0x874624D6);
    require(!test.mpeg.initialized()&&test.mpeg.context_count()==0,"MPEG cleanup retained a live decoder context");
}
void callback_failure_and_output_validation() {
    Fixture test; auto callback=test.call(0xB240A59E,{ring,2,3});
    const auto before=test.runtime.memory().load32(ring+12);
    const auto failed=test.mpeg.complete_put(test.runtime.memory(),ring,-17);
    require(!failed.callback&&failed.result==-17&&test.runtime.memory().load32(ring+12)==before,"Failed callback mutated packet occupancy");
    callback=test.call(0xB240A59E,{ring,1,3}); test.runtime.memory().copy_in(data,packet(0x11));
    test.mpeg.complete_put(test.runtime.memory(),ring,1);
    test.call(0xFE246728,{pointer,test.runtime.memory().load32(output+4),au,output+8});
    test.runtime.memory().store32(destination_pointer,0x09FFFFF0);
    rejects([&]{test.call(0x0E3C2E9D,{pointer,au,512,destination_pointer,output+12});},"Invalid decoded frame destination accepted");
    test.runtime.memory().store32(destination_pointer,destination);
    test.call(0x0E3C2E9D,{pointer,au,512,destination_pointer,output+12});
    require(test.result()==0&&test.runtime.memory().load32(output+12)==1,"Output validation consumed a valid undecoded AU");
}
void signed_memory_size_rejection_preserves_ownership() {
    Fixture test;
    const auto original=test.runtime.memory().load32(ring+40);
    test.call(0x37295ED8,{ring,3,data,0xFFFFFFFF,0x08808000,0x1234});
    require(test.result()==0x80610022&&test.runtime.memory().load32(ring+40)==original,
        "Negative ring memory size was accepted or changed existing ownership");
    test.call(0xD8C5F121,{pointer,workspace,0xFFFFFFFF,ring,512,0,0});
    require(test.result()==0x80610022&&test.mpeg.context_count()==1,
        "Negative MPEG context size was accepted or changed existing contexts");
}
void missing_pts_preserve_hardware_sentinels_and_explicit_anchors() {
    Fixture test(4,true); const auto callback=test.call(0xB240A59E,{ring,4,4});
    for(unsigned i=0;i<4;++i)test.runtime.memory().copy_in(data+i*2048,packet(i&1?0xEE:0x11,90000+i*3003));
    test.mpeg.complete_put(test.runtime.memory(),ring,4);
    for(unsigned i=0;i<4;++i){
        test.call(0xFE246728,{pointer,test.runtime.memory().load32(output+4),au,output+8});
        require(test.result()==0&&test.runtime.memory().load32(au)==(i&1?UINT32_MAX:0)&&
                test.runtime.memory().load32(au+4)==(i&1?UINT32_MAX:90000+i*3003),
            "Actual/missing AVC timestamp was replaced by an invented container clock");
        test.call(0x0E3C2E9D,{pointer,au,512,destination_pointer,output+12});
    }
    Fixture partial;partial.call(0xB240A59E,{ring,1,3});partial.runtime.memory().copy_in(data,packet(0xEE));
    partial.mpeg.complete_put(partial.runtime.memory(),ring,1);
    partial.call(0xFE246728,{pointer,partial.runtime.memory().load32(output+4),au,output+8});
    require(partial.result()==0&&partial.runtime.memory().load32(au)==UINT32_MAX&&
            partial.runtime.memory().load32(au+4)==UINT32_MAX,"Partial stream did not retain a real missing timestamp");
}
constexpr std::uint32_t audio_au=0x08806400,audio_es=0x08807000,audio_pcm=0x08809000;
std::uint32_t enable_audio(Fixture &test) {
    auto metadata=psmf();metadata[0x81]=2;metadata[0x92]=0xBD;metadata[0xA0]=2;metadata[0xA1]=2;
    test.runtime.memory().copy_in(header,metadata);test.call(0x21FF80E4,{pointer,header,output});
    test.call(0x42560F23,{pointer,1,0});const auto stream=test.result();
    require(stream==workspace+0x1700,"ATRAC stream handle disagrees with measured distinct offset");
    test.call(0xF8DCB679,{pointer,output+16,output+20});
    require(test.result()==0&&test.runtime.memory().load32(output+16)==0x840&&
        test.runtime.memory().load32(output+20)==0x2000,"ATRAC ES/PCM sizes differ from hardware query");
    test.runtime.memory().copy_in(audio_es,std::vector<std::uint8_t>(0x840,0xA5));
    test.call(0x167AFD9E,{pointer,audio_es,audio_au});
    require(test.result()==0&&test.runtime.memory().load32(audio_au)==0&&test.runtime.memory().load32(audio_au+4)==0&&
        test.runtime.memory().load32(audio_au+16)==audio_es,"ATRAC AU was not initialized with real ES pointer/zero timestamps");
    return stream;
}
void audio_streaming_reassembly_dual_consumers_and_real_output_contract() {
    Fixture test(4);const auto stream=enable_audio(test);
    const auto one=audio_frame(0x11),two=audio_frame(0x22),three=audio_frame(0x33);
    auto first=one;first.insert(first.end(),two.begin(),two.begin()+8);
    std::vector<std::uint8_t> second(two.begin()+8,two.end());second.insert(second.end(),three.begin(),three.end());
    test.call(0xB240A59E,{ring,2,4});
    test.runtime.memory().copy_in(data,mixed_packet(0x11,first));
    test.runtime.memory().copy_in(data+2048,mixed_packet(0x12,second,8,93428));
    test.mpeg.complete_put(test.runtime.memory(),ring,2);
    test.decode(0x11);test.decode(0x12);
    test.call(0xB5F6DC87,{ring});require(test.result()==2,"Video advanced/reused packets still held by audio consumer");
    const auto get=[&]{test.call(0xE1CE83A7,{pointer,stream,audio_au,output+24});require(test.result()==0,"Assembled ATRAC AU unavailable");};
    get();require(test.runtime.memory().load32(audio_au+4)==85069&&test.runtime.memory().load32(audio_au+8)==UINT32_MAX&&
        test.runtime.memory().load32(audio_au+20)==8&&test.runtime.memory().load32(audio_es)==0x11111111&&
        test.runtime.memory().load32(audio_es+8)==0xA5A5A5A5,"ATRAC AU payload/PTS/DTS/ES guard wrong");
    test.runtime.memory().store32(audio_pcm-4,0xA5A5A5A5);test.runtime.memory().store32(audio_pcm+0x2000,0xA5A5A5A5);
    rejects([&]{test.call(0x800C44DF,{pointer,audio_au,0x09FFFFF0,1});},"Invalid PCM destination consumed an AU");
    test.call(0x800C44DF,{pointer,audio_au,audio_pcm,1});
    require(test.result()==0&&test.runtime.memory().load16(audio_pcm)==0x11&&
        test.runtime.memory().load16(audio_pcm+8190)==0x1010&&test.runtime.memory().load32(audio_pcm-4)==0xA5A5A5A5&&
        test.runtime.memory().load32(audio_pcm+8192)==0xA5A5A5A5&&test.runtime.memory().load32(audio_au+20)==0,
        "MPEG ATRAC output was not all2048 stereo samples or damaged a guard");
    get();require(test.runtime.memory().load32(audio_au)==UINT32_MAX&&test.runtime.memory().load32(audio_au+4)==UINT32_MAX,
        "Audio frame without a PES anchor got an invented timestamp");
    test.call(0x800C44DF,{pointer,audio_au,audio_pcm,0});
    require(test.runtime.memory().load16(audio_pcm)==0x122,"ATRAC decoder history was reset between ordinary frames");
    get();require(test.runtime.memory().load32(audio_au+4)==93428,"PES anchor did not apply at its first new frame offset");
    test.call(0x800C44DF,{pointer,audio_au,audio_pcm,0});
    require(test.runtime.memory().load16(audio_pcm)==0x233,"Cross-PES ATRAC frame assembly changed codec payload/history");
    std::array<std::uint8_t,24> before{};test.runtime.memory().copy_out(audio_au,before);
    test.runtime.memory().store32(output+24,0xABCDEF01);test.call(0xE1CE83A7,{pointer,stream,audio_au,output+24});
    std::array<std::uint8_t,24> after{};test.runtime.memory().copy_out(audio_au,after);
    require(test.result()==0x80618001&&before==after&&test.runtime.memory().load32(output+24)==0xABCDEF01,
        "Audio need-data mutated AU/attribute outputs");
    test.call(0xB5F6DC87,{ring});require(test.result()==3,"Both-consumed stream packet was retained");
    test.call(0x4571CC64,{pointer});
    test.call(0xB240A59E,{ring,2,3});
    test.runtime.memory().copy_in(data+4096,mixed_packet(0x13,audio_frame(0x44),0,97608));
    test.runtime.memory().copy_in(data+6144,mixed_packet(0x14,audio_frame(0x55),0,101787));
    test.mpeg.complete_put(test.runtime.memory(),ring,2);test.decode(0x13);test.decode(0x14);
    get();test.call(0x800C44DF,{pointer,audio_au,audio_pcm,0});
    require(test.runtime.memory().load16(audio_pcm)==0x344,"AVC-only flush reset independent ATRAC decoder history");
    get();test.call(0x800C44DF,{pointer,audio_au,audio_pcm,0});
    test.call(0xB5F6DC87,{ring});require(test.result()==4,"Final interleaved padding/ring was not released by both consumers");
    test.call(0x800C44DF,{pointer,audio_au,audio_pcm,0});require(test.result()==0x807F00FD,"Missing ATRAC AU decode fabricated success");
    test.call(0x707B7629,{pointer});test.call(0xB240A59E,{ring,1,4});test.runtime.memory().copy_in(data,mixed_packet(0x15,one));
    test.mpeg.complete_put(test.runtime.memory(),ring,1);get();test.call(0x800C44DF,{pointer,audio_au,audio_pcm,0});
    require(test.runtime.memory().load16(audio_pcm)==0x11,"Flush did not reset actual audio codec history");
}
void audio_readiness_header_and_frame_rejections() {
    Fixture unavailable(3,false,false);
    rejects([&]{unavailable.call(0x42560F23,{pointer,1,0});},"Unavailable ATRAC service registered a stream");
    Fixture test;
    rejects([&]{test.mpeg.set_audio_decoder_factory({});},"Active audio factory replacement was accepted");
    // Hardware basic.expected registers ATRAC even for its video-only sample.
    // That unused handle must not pin every shared ring packet indefinitely.
    test.call(0x42560F23,{pointer,1,0});const auto absent_stream=test.result();
    test.call(0x167AFD9E,{pointer,audio_es,audio_au});
    test.call(0xB240A59E,{ring,2,3});test.runtime.memory().copy_in(data,packet(0x11));
    test.runtime.memory().copy_in(data+2048,packet(0x12));test.mpeg.complete_put(test.runtime.memory(),ring,2);
    test.call(0xE1CE83A7,{pointer,absent_stream,audio_au,0});require(test.result()==0x80618001,"Video-only sample fabricated audio");
    test.decode(0x11);test.decode(0x12);test.call(0xB5F6DC87,{ring});
    require(test.result()==2,"Unused registered ATRAC stream pinned video-only ring data");
    for(unsigned bad: {0u,1u,2u}) {
        auto metadata=psmf();metadata[0x81]=2;metadata[0x92]=0xBD;metadata[0xA0]=2;metadata[0xA1]=2;
        if(!bad)metadata[0x92]=0xC0;else if(bad==1)metadata[0xA0]=1;else metadata[0xA1]=1;
        rejects([&]{badman::PsmfHeader::parse(metadata);},"Unsupported audio stream configuration accepted");
    }
    for(unsigned bad: {0u,1u}) {
        Fixture fixture;enable_audio(fixture);auto coded=audio_frame(0x11);if(!bad)coded[2]=0x20;
        fixture.call(0xB240A59E,{ring,1,3});fixture.runtime.memory().copy_in(data,mixed_packet(0x11,coded,bad?1u:0u));
        rejects([&]{fixture.mpeg.complete_put(fixture.runtime.memory(),ring,1);},"Invalid audio frame/config/anchor accepted");
    }
}
}
int main() {
    try { parsing_and_rejection();ring_callbacks_decode_and_wrap();callback_failure_and_output_validation();
        signed_memory_size_rejection_preserves_ownership();missing_pts_preserve_hardware_sentinels_and_explicit_anchors();
        audio_streaming_reassembly_dual_consumers_and_real_output_contract();audio_readiness_header_and_frame_rejections();
        std::cout<<"MPEG packet, callback, frame ownership and lifecycle tests passed\n";return 0;
    } catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}

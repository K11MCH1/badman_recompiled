#include "badman_mpeg.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <iostream>
#include <iterator>

namespace {
int service_probe(const std::vector<std::uint8_t> &bytes,const badman::PsmfHeader &header,
                  const char *directory,const char *path) {
    psprecomp::Runtime runtime;
    badman::Mpeg mpeg([&]{return badman::make_ffmpeg_video_decoder(directory);},
                    [&]{return badman::make_ffmpeg_mpeg_audio_decoder(directory);});
    constexpr std::uint32_t ring=0x08804000,hdr=0x08805000,handle=0x08806000,work=0x08810000;
    constexpr std::uint32_t data=0x08900000,au=0x08807000,out=0x08807100,target=0x08C00000;
    constexpr std::uint32_t audio_au=0x08807040,audio_es=0x08808000,audio_pcm=0x08809000;
    const auto call=[&](std::uint32_t nid,std::initializer_list<std::uint32_t> args={}) {
        auto &cpu=runtime.cpu();for(unsigned i=4;i<12;++i)cpu.gpr[i]=0;
        unsigned i=4;for(auto value:args)cpu.gpr[i++]=value;cpu.gpr[28]=0x0891F420;
        return mpeg.invoke(nid,runtime,cpu);
    };
    runtime.memory().copy_in(hdr,std::span(bytes).first(2048));
    call(0x682A619B);call(0x37295ED8,{ring,960,data,960*0x868,0x08808000,0x1234});
    call(0xD8C5F121,{handle,work,0x10000,ring,512,0,0});call(0x21FF80E4,{handle,hdr,out});
    call(0x42560F23,{handle,0,0});const auto stream=runtime.cpu().gpr[2];
    call(0xA780CF7E,{handle});call(0x167AFD9E,{handle,1,au});
    std::uint32_t audio_stream=0;
    if(header.streams==2){
        call(0x42560F23,{handle,1,0});audio_stream=runtime.cpu().gpr[2];
        call(0xF8DCB679,{handle,out+16,out+20});
        if(runtime.cpu().gpr[2]||runtime.memory().load32(out+16)!=0x840||runtime.memory().load32(out+20)!=0x2000)
            throw psprecomp::Error("MPEG audio ES/PCM size query failed");
        call(0x167AFD9E,{handle,audio_es,audio_au});
    }
    std::ofstream output(path,std::ios::binary);if(!output)throw psprecomp::Error("Cannot create service probe artifact");
    std::ofstream pcm;
    if(audio_stream){pcm.open(std::string(path)+".pcm",std::ios::binary);if(!pcm)throw psprecomp::Error("Cannot create private PCM probe artifact");}
    std::ofstream timing(std::string(path)+".timing.csv");timing<<"type,index,pts,dts\n";
    runtime.memory().store32(out+4,target);std::uint32_t frames=0,units=0,audio_units=0,missing_pts=0,audio_missing=0;
    std::uint64_t first_pts=0,last_pts=0,last_audio_pts=0;
    std::uint32_t submitted=0,puts=0,wraps=0;
    const auto frame=[&]{
        if(!runtime.memory().load32(out+8))return;
        std::vector<std::uint8_t> row(header.width*4);
        for(std::uint32_t y=0;y<header.height;++y){runtime.memory().copy_out(target+y*512*4,row);
            output.write(reinterpret_cast<const char *>(row.data()),row.size());}
        ++frames;
    };
    for(unsigned iteration=0;;++iteration){
        if(iteration>=200000)throw psprecomp::Error("Streaming service probe did not finish within its progress bound");
        bool progress=false;
        call(0xB5F6DC87,{ring});
        const auto available=runtime.cpu().gpr[2],remaining=header.stream_size/2048-submitted;
        if(available&&remaining){
            auto callback=call(0xB240A59E,{ring,std::min({available,remaining,24u}),available});++puts;
            if(!callback)throw psprecomp::Error("Streaming ring submission omitted its file callback");
            do{
                const auto count=std::min(callback->packets,header.stream_size/2048-submitted);
                runtime.memory().copy_in(callback->data,std::span(bytes).subspan(header.stream_offset+std::size_t(submitted)*2048,std::size_t(count)*2048));
                const auto committed=mpeg.complete_put(runtime.memory(),ring,count);submitted+=count;
                callback=committed.callback;if(callback)++wraps;
            }while(callback);
            progress=true;
        }
        if(audio_stream){
            call(0xE1CE83A7,{handle,audio_stream,audio_au,out+24});
            if(runtime.cpu().gpr[2]!=0x80618001){
                if(runtime.cpu().gpr[2])throw psprecomp::Error("Service probe could not obtain an ATRAC access unit");
                const auto pts=(std::uint64_t(runtime.memory().load32(audio_au))<<32)|runtime.memory().load32(audio_au+4);
                if(pts==UINT64_MAX)++audio_missing;
                else{if(last_audio_pts&&pts<=last_audio_pts)throw psprecomp::Error("Nonmonotonic explicit audio timestamp");last_audio_pts=pts;}
                timing<<"audio,"<<audio_units<<','<<pts<<",18446744073709551615\n";
                call(0x800C44DF,{handle,audio_au,audio_pcm,audio_units?0u:1u});
                if(runtime.cpu().gpr[2])throw psprecomp::Error("Service probe ATRAC decode failed");
                std::array<std::uint8_t,0x2000> decoded{};runtime.memory().copy_out(audio_pcm,decoded);
                pcm.write(reinterpret_cast<const char *>(decoded.data()),decoded.size());++audio_units;progress=true;
            }
        }
        call(0xFE246728,{handle,stream,au,out+12});
        if(runtime.cpu().gpr[2]!=0x80618001){
            if(runtime.cpu().gpr[2])throw psprecomp::Error("Service probe could not obtain an AVC access unit");
            const auto pts=(std::uint64_t(runtime.memory().load32(au))<<32)|runtime.memory().load32(au+4);
            const auto dts=(std::uint64_t(runtime.memory().load32(au+8))<<32)|runtime.memory().load32(au+12);
            if(pts==UINT64_MAX)++missing_pts;
            else{if(last_pts&&pts<=last_pts)throw psprecomp::Error("Nonmonotonic explicit video timestamp");if(!last_pts)first_pts=pts;last_pts=pts;}
            timing<<"video,"<<units<<','<<pts<<','<<dts<<'\n';++units;
            call(0x0E3C2E9D,{handle,au,512,out+4,out+8});frame();progress=true;
        }
        if(!progress){
            if(submitted!=header.stream_size/2048)throw psprecomp::Error("Both consumers stalled a nonempty MPEG source");
            break;
        }
    }
    for(;;){call(0x740FCCD1,{handle,512,out+4,out+8});if(!runtime.memory().load32(out+8))break;frame();}
    call(0xB5F6DC87,{ring});if(runtime.cpu().gpr[2]!=960)throw psprecomp::Error("Consumed MPEG stream retained ringbuffer packets");
    call(0xCEB870B1,{handle,1});call(0x591A4AA2,{handle,stream});
    if(audio_stream)call(0x591A4AA2,{handle,audio_stream});call(0x606A4649,{handle});
    call(0x13407F13,{ring});call(0x874624D6);output.close();if(!output||!frames)throw psprecomp::Error("Incomplete service video artifact");
    timing.close();if(!timing)throw psprecomp::Error("Cannot write actual AU timing artifact");
    if(audio_stream){pcm.close();if(!pcm||!audio_units)throw psprecomp::Error("Incomplete real PCM artifact");}
    std::cout<<"Real MPEG service: AUs="<<units<<" frames="<<frames<<" first_PTS="<<first_pts
        <<" last_PTS="<<last_pts<<" missing_PTS="<<missing_pts<<" audio_AUs="<<audio_units<<" audio_missing_PTS="<<audio_missing
        <<" packets="<<submitted<<" puts="<<puts<<" wraps="<<wraps<<" ring_free=960\n";return 0;
}
}

int main(int argc,char **argv) {
    try {
        if(argc!=4&&argc!=5)throw psprecomp::Error("Usage: BadmanMpegProbe <video-only PSMF> <codec DLL directory> <raw RGBA output> [--service]");
        if(std::filesystem::file_size(argv[1])>64u*1024u*1024u+2048u)
            throw psprecomp::Error("PSMF probe input exceeds its bounded size");
        std::error_code identity_error;
        if(std::filesystem::absolute(argv[1]).lexically_normal()==std::filesystem::absolute(argv[3]).lexically_normal()||
            std::filesystem::equivalent(argv[1],argv[3],identity_error))
            throw psprecomp::Error("Preserving input: probe output must be a different file");
        std::ifstream input(argv[1],std::ios::binary);
        if(!input)throw psprecomp::Error("Cannot open local PSMF input");
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(input)),{});
        const auto header=badman::PsmfHeader::parse(bytes);
        if(header.stream_offset+header.stream_size!=bytes.size())throw psprecomp::Error("Probe requires exact bounded PSMF extent");
        if(argc==5){if(std::string(argv[4])!="--service")throw psprecomp::Error("Unknown probe option");return service_probe(bytes,header,argv[2],argv[3]);}
        if(header.streams!=1)throw psprecomp::Error("Use --service to verify interleaved video/audio PSMF");
        badman::ProgramStream stream;auto payloads=stream.feed(std::span(bytes).subspan(header.stream_offset),true);
        auto decoder=badman::make_ffmpeg_video_decoder(argv[2]);std::vector<badman::EncodedVideo> units;
        for(const auto &payload:payloads){auto next=decoder->parse(payload.bytes,payload.pts,payload.dts,false);
            for(auto &unit:next)units.push_back(std::move(unit));}
        auto tail=decoder->parse({},badman::video_no_timestamp,badman::video_no_timestamp,true);
        for(auto &unit:tail)units.push_back(std::move(unit));
        std::ofstream timing(std::string(argv[3])+".timing.csv");timing<<"index,pts,dts,source_end\n";
        for(std::size_t i=0;i<units.size();++i)timing<<i<<','<<units[i].pts<<','<<units[i].dts<<','<<units[i].source_end<<'\n';
        timing.close();if(!timing)throw psprecomp::Error("Cannot write video timing diagnostics");
        std::ofstream output(argv[3],std::ios::binary);if(!output)throw psprecomp::Error("Cannot create private raw video artifact");
        std::uint32_t frames=0;
        const auto missing_pts=std::count_if(units.begin(),units.end(),[](const auto &unit){return unit.pts==badman::video_no_timestamp;});
        const auto write=[&](const badman::VideoFrame &frame){
            if(frame.width!=header.width||frame.height!=header.height)throw psprecomp::Error("Decoded dimensions disagree with PSMF");
            output.write(reinterpret_cast<const char *>(frame.rgba.data()),frame.rgba.size());++frames;
        };
        for(const auto &unit:units)if(auto frame=decoder->decode(unit))write(*frame);
        while(auto frame=decoder->drain())write(*frame);
        output.close();if(!output)throw psprecomp::Error("Incomplete raw video artifact");
        if(!frames)throw psprecomp::Error("Decoder produced no video frames");
        std::cout<<"Real PSMF video: PES="<<payloads.size()<<" AUs="<<units.size()<<" frames="<<frames
            <<" width="<<header.width<<" height="<<header.height<<" missing_parser_PTS="<<missing_pts<<'\n';return 0;
    }catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}

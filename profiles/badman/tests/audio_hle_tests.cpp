#include "badman_host.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
using namespace psprecomp;
constexpr std::uint32_t root_entry=0x08804000,root_pc=0x08804004,child_entry=0x08805000,child_pc=0x08805004;
constexpr std::uint32_t name=0x08802000,source=0x08820000,core=0x08810000;
constexpr std::uint32_t reserve=0x5EC81C55,release=0x6FC46853,length=0xCB2E439E,rest=0xB011922F;
constexpr std::uint32_t output=0x136CAF51,panned=0x13F592BC;
void require(bool value,const char *message){if(!value)throw std::runtime_error(message);}
template<class F> void rejects(F action,const char *message){bool failed=false;try{action();}catch(const Error &){failed=true;}require(failed,message);}
struct Packet {std::uint32_t channel{};std::uint64_t ticket{};std::span<const std::int16_t> samples;std::vector<std::int16_t> copy;};
struct Observed {
    std::array<bool,8> open{};std::vector<Packet> packets;std::vector<badman::AudioProgress> updates;
    unsigned opens{},submits{},closes{};bool fail_open{},fail_poll{},close_live{true};
};
class Transport final:public badman::AudioTransport {
    std::shared_ptr<Observed> state_;
public:
    explicit Transport(std::shared_ptr<Observed> state):state_(std::move(state)){}
    void open(std::uint32_t channel) override {
        if(state_->fail_open)throw Error("Explicit injected audio device open failure");
        require(!state_->open.at(channel),"Audio voice opened twice");state_->open[channel]=true;++state_->opens;
    }
    void close(std::uint32_t channel) noexcept override {
        for(const auto &packet:state_->packets)if(packet.channel==channel)
            state_->close_live&=std::equal(packet.samples.begin(),packet.samples.end(),packet.copy.begin(),packet.copy.end());
        std::erase_if(state_->packets,[&](const auto &packet){return packet.channel==channel;});
        state_->open[channel]=false;++state_->closes;
    }
    void submit(std::uint32_t channel,std::uint64_t ticket,std::span<const std::int16_t> samples) override {
        require(state_->open[channel],"Submission reached closed device voice");
        state_->packets.push_back({channel,ticket,samples,{samples.begin(),samples.end()}});++state_->submits;
    }
    std::vector<badman::AudioProgress> poll() override {
        if(state_->fail_poll)throw Error("Explicit injected asynchronous audio failure");
        auto result=std::move(state_->updates);state_->updates.clear();
        for(const auto &update:result)if(update.complete)
            std::erase_if(state_->packets,[&](const auto &packet){return packet.ticket==update.ticket;});
        return result;
    }
};
struct Fixture {
    Runtime runtime;std::ostringstream trace;std::uint64_t now{1000000000};std::vector<std::uint64_t> sleeps;
    std::function<void()> on_sleep;std::shared_ptr<Observed> device=std::make_shared<Observed>();
    badman::Host host{runtime,0x089F0000,0x089E0000,0x0891F420,trace};
    static void guest_return(Runtime &,AllegrexContext &ctx){ctx.pc=badman::Host::thread_return_pc;}
    explicit Fixture(bool injected=true) {
        runtime.register_function(root_entry,guest_return,"audio_root");runtime.register_function(root_pc,guest_return,"audio_root_return");
        runtime.register_function(child_entry,guest_return,"audio_worker");runtime.register_function(child_pc,guest_return,"audio_worker_return");
        host.install({});host.set_scheduler_timing([this]{return now;},[this](std::uint64_t deadline){sleeps.push_back(deadline);now=deadline;if(on_sleep)on_sleep();});
        if(injected)host.configure_audio_transport(std::make_unique<Transport>(device));
        const std::array<std::uint8_t,6> text{'a','u','d','i','o',0};runtime.memory().copy_in(name,text);host.bootstrap(root_entry,"audio");
        fill(-2000,2000);
    }
    void fill(std::int16_t left,std::int16_t right){for(unsigned i=0;i<256;++i){runtime.memory().store16(source+i*4,std::bit_cast<std::uint16_t>(left));runtime.memory().store16(source+i*4+2,std::bit_cast<std::uint16_t>(right));}}
    std::uint32_t call(std::uint32_t nid,std::initializer_list<std::uint32_t> arguments={},const char *library="sceAudio"){
        auto &ctx=runtime.cpu();for(unsigned r=4;r<12;++r)ctx.gpr[r]=0;unsigned r=4;
        for(const auto value:arguments)ctx.gpr[r++]=value;runtime.invoke_import(library,nid,ctx);return ctx.gpr[2];
    }
    std::uint32_t child(){const auto uid=call(0x446D8DE6,{name,child_entry,40,4096,0,0},"ThreadManForUser");
        runtime.cpu().gpr[31]=root_pc;require(call(0xF475845D,{uid,0,0},"ThreadManForUser")==0,"Audio worker start failed");return uid;}
};
void cadence_buffers_and_real_wakeup(){
    Fixture f;auto &ctx=f.runtime.cpu();const auto channel=f.call(reserve,{0xffffffff,64,0}),worker=f.child();
    require(channel<8&&f.call(output,{channel,0x8000,source})==64&&runtime_thread_uid()==1&&f.sleeps.empty(),"First output did not accept real immutable PCM immediately");
    const auto previous=f.device->packets[0].ticket;f.fill(-4000,4000);ctx.gpr[16]=0xa55a1234;
    const auto sp=ctx.gpr[29],gp=ctx.gpr[28];ctx.gpr[31]=root_pc;f.call(panned,{channel,0x4000,0x8000,source});
    require(runtime_thread_uid()==static_cast<std::int32_t>(worker)&&ctx.pc==child_entry&&f.sleeps.empty(),"Blocking audio stranded a ready PSP worker");
    require(f.device->packets.size()==2&&f.device->packets[0].samples[0]==-2000&&f.device->packets[1].samples[0]==-2000&&f.device->packets[1].samples[1]==4000,
        "Queued packet/source rewrite changed earlier PCM or independent pan gains");
    f.fill(0,0);require(f.device->packets[1].samples[1]==4000,"Pending audio retained guest-memory pointer");
    require(f.call(rest,{channel})==128&&f.call(output,{channel,0x8000,0})==0x80260002&&f.call(release,{channel})==0x80260002&&
        f.call(length,{channel,128})==0x80260002,"Same-channel second waiter/reconfiguration did not return BUSY");
    f.now+=999999;f.call(0x293B45B8,{},"ThreadManForUser");require(runtime_thread_uid()==static_cast<std::int32_t>(worker),"Audio wait completed on elapsed time alone");
    f.device->updates={{previous,64,true}};++f.now;ctx.gpr[31]=child_pc;f.call(0x293B45B8,{},"ThreadManForUser");
    require(runtime_thread_uid()==1&&ctx.pc==root_pc&&ctx.gpr[2]==64&&ctx.gpr[16]==0xa55a1234&&ctx.gpr[29]==sp&&ctx.gpr[28]==gp,
        "Actual previous-ticket completion lost caller context/result");
    require(f.device->packets.size()==1&&f.call(rest,{channel})==64,"Wake consumed the newly queued packet instead of the previous one");
    require(f.call(length,{channel,128})==0&&f.device->packets[0].samples.size()==128,"Future channel length resized old PCM");
    require(f.call(release,{channel})==0&&f.device->close_live&&f.device->packets.empty(),"Release while playing did not use a real close barrier");
    require(f.call(rest,{channel})==0,"Imported RestLength on unreserved channel was not zero");
}
void null_drain_and_idle_polling(){
    Fixture f;const auto channel=f.call(reserve,{3,64,0});
    require(f.call(output,{channel,0x8000,0})==64&&f.device->submits==0&&f.sleeps.empty(),"Idle null drain queued fake silence or waited");
    require(f.call(output,{channel,0x8000,source})==64,"Drain fixture initial PCM failed");const auto ticket=f.device->packets[0].ticket;
    f.on_sleep=[&]{require(f.device->submits==1,"Null drain submitted PCM");f.device->updates={{ticket,64,true}};};
    f.runtime.cpu().gpr[31]=root_pc;require(f.call(output,{channel,0x8000,0})==64&&f.runtime.cpu().pc==root_pc&&f.sleeps.size()==1&&f.now==1001000000,
        "Idle audio waiter did not poll actual device progress at bounded deadline");
    require(f.call(rest,{channel})==0&&f.device->submits==1,"Completed null drain fabricated unplayed samples");
    f.on_sleep={};f.runtime.cpu().gpr[31]=root_pc;f.call(0xCEADEB47,{2000},"ThreadManForUser");
    require(f.sleeps.size()==2&&f.sleeps.back()==1003000000,"Audio polling continued after no pending audio waiter");
}
void validation_and_real_failures(){
    {
        Fixture absent(false);
        require(absent.call(reserve,{0xffffffff,63,0})==0x80260006&&absent.call(rest,{0})==0&&absent.call(output,{0,0x8000,source})==0x80260008,
            "Invalid/unreserved calls required a native device or fabricated channel");
    }
    Fixture f;
    require(f.call(reserve,{0,64,2})==0x80260007&&f.call(reserve,{8,64,0})==0x80260003&&f.device->opens==0,"Invalid reserve opened transport");
    f.device->fail_open=true;rejects([&]{f.call(reserve,{0,64,0});},"Device open failure became audio success");
    f.device->fail_open=false;require(f.call(reserve,{0,64,0})==0,"Failed open left channel reserved");
    require(f.call(panned,{0,0xffffffff,0,source})==0x8026000B&&f.device->submits==0,"Invalid panned volume queued PCM");
    rejects([&]{f.call(output,{0,0x8000,0x09fffff0});},"Truncated PCM footprint accepted");
    require(f.device->submits==0,"Invalid PCM source reached device");
    f.device->fail_poll=true;rejects([&]{f.call(output,{0,0x8000,source});},"Asynchronous device failure fabricated completion");
}
void sas_metadata_and_atomic_validation(){
    Fixture f;auto sas=[&](std::uint32_t nid,std::initializer_list<std::uint32_t> args){return f.call(nid,args,"sceSasCore");};
    auto &memory=f.runtime.memory();for(unsigned i=0;i<3616;++i)memory.store8(core+i,0xa5);
    require(sas(0x42778A9F,{core+1,256,32,0,44100})==0x80420005&&sas(0x42778A9F,{core,63,32,0,44100})==0x80420001&&
        sas(0x42778A9F,{core,256,33,0,44100})==0x80420002&&sas(0x42778A9F,{core,256,32,2,44100})==0x80420003&&
        sas(0x42778A9F,{core,256,32,0,48000})==0x80420004&&!f.host.sas_configuration(core),"Invalid SAS init created configuration");
    require(sas(0x42778A9F,{core,256,32,0,44100})==0,"Valid SAS state initialization failed");
    const auto sp=f.runtime.cpu().gpr[29];
    require(sas(0xAD84D37F,{core,7,0})==0&&sas(0x440CA7D8,{core,7,0xfffff000,0x1000,123,0xffffff85})==0&&
        sas(0xCBCD4F79,{core,7,0x800f,0x9fe0})==0,"Measured SAS zero-pitch/signed-volume/packedADSR rejected");
    const auto snapshot=*f.host.sas_configuration(core|0x40000000u);const auto &voice=snapshot.voices[7];
    require(snapshot.grain==256&&snapshot.max_voices==32&&snapshot.sample_rate==44100&&voice.pitch==0&&
        voice.volumes==std::array<std::int32_t,4>{-4096,4096,123,-123}&&voice.envelope1==0x800f&&voice.envelope2==0x9fe0&&f.runtime.cpu().gpr[29]==sp,
        "SAS state/fifth-sixth register arguments or alias identity were lost");
    require(sas(0x440CA7D8,{core,7,1,2,3,4097})==0x80420018&&sas(0xAD84D37F,{core,7,0x4001})==0x80420012&&
        sas(0xCBCD4F79,{core,7,0,0x2000})==0x80420013&&sas(0xAD84D37F,{core,32,1})==0x80420010,"SAS parameter errors accepted");
    const auto after=*f.host.sas_configuration(core);
    require(after.voices[7].volumes==voice.volumes&&after.voices[7].pitch==voice.pitch&&after.voices[7].envelope1==voice.envelope1,
        "Rejected SAS setter partially changed retained state");
    require(sas(0x42778A9F,{core,512,16,1,44100})==0&&f.host.sas_configuration(core)->grain==512,"Repeated SAS Init failed to reinitialize");
    for(unsigned i=0;i<3616;++i)require(memory.load8(core+i)==0xa5,"Configuration-only SAS API wrote invented private firmware layout");
    require(f.device->opens==0&&f.device->submits==0,"SAS metadata called a fake/real mixer or opened playback");
}
}
int main(){try{cadence_buffers_and_real_wakeup();null_drain_and_idle_polling();validation_and_real_failures();sas_metadata_and_atomic_validation();
    std::cout<<"PCM HLE/SAS metadata tests passed; explicitly injected transport, no native playback\n";return 0;
}catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}}

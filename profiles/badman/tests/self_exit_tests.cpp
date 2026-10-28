#include "badman_host.hpp"
#include "psprecomp/common.hpp"
#include <array>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
using namespace psprecomp;
constexpr std::uint32_t entry=0x08813404u,stub=0x088E9AF0u,continuation=0x0881340Cu;
constexpr const char *reason="Badman main module requested self-unload with status 1";
unsigned continuation_calls{},callback_calls{};
void require(bool value,const char *message){if(!value)throw std::runtime_error(message);}
void parameters(AllegrexContext &ctx) {
    ctx.pc=stub;ctx.gpr[31]=continuation;ctx.gpr[2]=0xC001C0DEu;
    ctx.gpr[4]=1;ctx.gpr[5]=ctx.gpr[6]=ctx.gpr[7]=ctx.gpr[8]=0;
}
std::vector<std::uint8_t> snapshot(const GuestMemory &memory,std::uint32_t base,std::size_t size) {
    std::vector<std::uint8_t> bytes(size);memory.copy_out(base,bytes);return bytes;
}
void translated_import(Runtime &runtime,AllegrexContext &ctx) {
    const auto token=capture_runtime_execution_context();const auto pc=ctx.pc,ra=ctx.gpr[31];
    runtime.invoke_import("ModuleMgrForUser",0x8F2DF740u,ctx);
    if(!runtime.stopped()&&runtime_execution_context_matches(token)&&ctx.pc==pc)ctx.pc=ra;
}
void translated_wrapper(Runtime &runtime,AllegrexContext &ctx) {
    parameters(ctx);
    if(runtime.invoke_chained_call(ctx)&&ctx.pc==continuation)++continuation_calls;
}
void unexpected_continuation(Runtime &runtime,AllegrexContext &) {
    ++continuation_calls;runtime.stop("Unexpected continuation");
}
void nonreturning_main_exit() {
    Runtime runtime;std::ostringstream trace;badman::Host host(runtime,0x089F0000,0x089E0000,0x0891F420,trace);
    host.install({});host.bootstrap(entry,"self-exit-fixture");
    runtime.register_function(entry,translated_wrapper,"recomp_unit_self_exit_wrapper");
    runtime.register_function(stub,translated_import,"ModuleMgrForUser::0x8F2DF740");
    runtime.register_function(continuation,unexpected_continuation,"recomp_unit_unexpected_continuation");
    runtime.memory().store32(0x08010000,0x12345678);runtime.memory().store32(0x04001000,0x76543210);
    const auto ram=snapshot(runtime.memory(),0x08000000,32u*1024u*1024u),vram=snapshot(runtime.memory(),0x04000000,2u*1024u*1024u);
    const auto identity=capture_runtime_execution_context();runtime.run(entry,8);
    require(runtime.stopped()&&runtime.stop_reason()==reason,"Main self-unload did not request actual process stop");
    require(continuation_calls==0&&runtime.cpu().pc==stub&&runtime.cpu().gpr[2]==0xC001C0DEu,"Self-unload returned into retired guest code or invented v0 success");
    require(snapshot(runtime.memory(),0x08000000,ram.size())==ram&&snapshot(runtime.memory(),0x04000000,vram.size())==vram,"Self-unload mutated guest RAM/VRAM");
    require(runtime_execution_context_matches(identity),"Self-unload fabricated a scheduling transition");
    require(trace.str().find("[module-self-exit]")!=std::string::npos,"Actual self-exit was not traceable");
    runtime.invoke_import("ModuleMgrForUser",0x8F2DF740u,runtime.cpu());
    require(runtime.stop_reason()==reason&&runtime.cpu().gpr[2]==0xC001C0DEu,"Stopped-runtime duplicate rewrote completion");
}
void invalid_contracts() {
    Runtime runtime;std::ostringstream trace;badman::Host host(runtime,0x089F0000,0x089E0000,0x0891F420,trace);
    host.install({});host.bootstrap(entry,"invalid-contracts");auto &ctx=runtime.cpu();
    const auto ram=snapshot(runtime.memory(),0x08000000,32u*1024u*1024u);
    for(unsigned argument=4;argument<=8;++argument) {
        parameters(ctx);ctx.gpr[argument]=argument==4?0u:0x09F00000u;
        runtime.invoke_import("ModuleMgrForUser",0x8F2DF740u,ctx);
        require(!runtime.stopped()&&ctx.gpr[2]==0x800200D2u,"Unobserved stop arguments falsely succeeded");
    }
    for(const auto pc:{0x08C00100u,0x088E9AF4u,0u}) {
        parameters(ctx);ctx.pc=pc;runtime.invoke_import("ModuleMgrForUser",0x8F2DF740u,ctx);
        require(!runtime.stopped()&&ctx.gpr[2]==0x80020136u,"Foreign imported caller unloaded main");
    }
    for(const auto ra:{0x08C00100u,0x08813410u,0u}) {
        parameters(ctx);ctx.gpr[31]=ra;runtime.invoke_import("ModuleMgrForUser",0x8F2DF740u,ctx);
        require(!runtime.stopped()&&ctx.gpr[2]==0x80020136u,"Foreign return address unloaded main");
    }
    parameters(ctx);set_runtime_thread_identity(123,"not-owned");
    runtime.invoke_import("ModuleMgrForUser",0x8F2DF740u,ctx);
    require(!runtime.stopped()&&ctx.gpr[2]==0x80020064u,"Unknown native identity unloaded main");
    set_runtime_thread_identity(1,"module_start");
    require(snapshot(runtime.memory(),0x08000000,ram.size())==ram,"Rejected self-unload changed RAM");
}
void nonmain_thread_entry_is_rejected() {
    Runtime runtime;std::ostringstream trace;badman::Host host(runtime,0x089F0000,0x089E0000,0x0891F420,trace);
    host.install({});host.bootstrap(0x08C00000,"nonmain-owner");auto &ctx=runtime.cpu();parameters(ctx);
    runtime.invoke_import("ModuleMgrForUser",0x8F2DF740u,ctx);
    require(!runtime.stopped()&&ctx.gpr[2]==0x80020136u,"A non-main module thread falsely unloaded main");
}
void no_thread_is_rejected() {
    Runtime runtime;std::ostringstream trace;badman::Host host(runtime,0x089F0000,0x089E0000,0x0891F420,trace);
    host.install({});auto &ctx=runtime.cpu();parameters(ctx);
    runtime.invoke_import("ModuleMgrForUser",0x8F2DF740u,ctx);
    require(!runtime.stopped()&&ctx.gpr[2]==0x80020064u,"Self-unload outside a thread succeeded");
}
void interrupt_callback(Runtime &runtime,AllegrexContext &ctx) {
    const auto returned=ctx.gpr[31];parameters(ctx);
    runtime.invoke_import("ModuleMgrForUser",0x8F2DF740u,ctx);
    require(!runtime.stopped()&&ctx.gpr[2]==0x80020064u,"Interrupt callback stopped the application");
    ++callback_calls;ctx.pc=returned;
}
void interrupt_rejection() {
    Runtime runtime;std::ostringstream trace;badman::Host host(runtime,0x089F0000,0x089E0000,0x0891F420,trace);
    host.install({});host.bootstrap(entry,"interrupt-fixture");auto &ctx=runtime.cpu();
    runtime.register_function(0x08810000,interrupt_callback,"test_self_exit_interrupt");
    constexpr std::uint32_t callback=0x08820100,list=0x08821000;
    for(unsigned i=0;i<4;++i)runtime.memory().store32(callback+i*4,i==2?0x08810000:0);
    runtime.memory().store32(list,0x0F000055);runtime.memory().store32(list+4,0x0C000000);
    ctx.gpr[4]=callback;runtime.invoke_import("sceGe_user",0xA4FC06A4,ctx);const auto id=ctx.gpr[2];
    ctx.gpr[4]=list;ctx.gpr[5]=0;ctx.gpr[6]=id;ctx.gpr[7]=0;
    runtime.invoke_import("sceGe_user",0xAB49E76A,ctx);
    require(callback_calls==1&&host.ge_state().completed_lists()==1&&!runtime.stopped(),"Rejected interrupt corrupted GE execution");
}
struct LifetimeDecoder final:badman::AtracDecoder {
    unsigned &live;explicit LifetimeDecoder(unsigned &n):live(n){++live;}~LifetimeDecoder()override{--live;}
    std::vector<std::int16_t> decode(const badman::AtracRiff &,std::span<const std::uint8_t>)override{return {};}
};
struct CountTransport final:badman::AudioTransport {
    unsigned &opened,&closed,&alive;CountTransport(unsigned &o,unsigned &c,unsigned &a):opened(o),closed(c),alive(a){++alive;}
    ~CountTransport()override{--alive;}
    void open(std::uint32_t)override{++opened;}void close(std::uint32_t)noexcept override{++closed;}
    void submit(std::uint32_t,std::uint64_t,std::span<const std::int16_t>)override{}
    std::vector<badman::AudioProgress>poll()override{return {};}
};
void actual_native_resource_teardown() {
    unsigned decoders=0,opened=0,closed=0,transport=0;Runtime runtime;std::ostringstream trace;
    {
        badman::Host host(runtime,0x089F0000,0x089E0000,0x0891F420,trace);host.install({});host.bootstrap(entry,"resources");
        host.set_atrac_decoder_factory([&]{return std::make_unique<LifetimeDecoder>(decoders);});
        host.configure_audio_transport(std::make_unique<CountTransport>(opened,closed,transport));auto &ctx=runtime.cpu();
        ctx.gpr[4]=0x300;runtime.invoke_import("sceUtility",0x2A2B3DE0,ctx);require(ctx.gpr[2]==0&&decoders==1,"Retained codec fixture failed");
        ctx.gpr[4]=0;ctx.gpr[5]=64;ctx.gpr[6]=0;runtime.invoke_import("sceAudio",0x5EC81C55,ctx);
        require(ctx.gpr[2]==0&&opened==1&&closed==0&&transport==1,"Actual reserved PCM fixture failed");
        parameters(ctx);runtime.invoke_import("ModuleMgrForUser",0x8F2DF740u,ctx);
        require(runtime.stop_reason()==reason&&decoders==1&&closed==0,"Self-exit prematurely invented translated resource cleanup");
    }
    require(decoders==0&&transport==0&&closed==1,"Native lifetime unwind leaked decoder or reserved audio voice");
    require(runtime_thread_uid()==-1,"Host teardown retained thread ownership");
}
}
int main() {
    try {
        nonreturning_main_exit();invalid_contracts();nonmain_thread_entry_is_rejected();no_thread_is_rejected();
        interrupt_rejection();actual_native_resource_teardown();
        std::cout<<"Badman bounded main self-exit, exact caller/arguments, nonreturn and actual native teardown tests passed\n";
        return 0;
    } catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}
}

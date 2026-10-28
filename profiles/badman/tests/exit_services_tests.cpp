#include "badman_host.hpp"
#include "psprecomp/common.hpp"
#include <array>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
using namespace psprecomp;
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void words(GuestMemory &memory, std::uint32_t address, std::initializer_list<std::uint32_t> values) {
    for (const auto value : values) { memory.store32(address, value); address += 4; }
}
void string(GuestMemory &memory, std::uint32_t address, const std::string &value) {
    for (const auto ch : value) memory.store8(address++, static_cast<std::uint8_t>(ch));
    memory.store8(address, 0);
}
template<class F> void rejected(F &&function, const char *message) {
    bool did = false;
    try { function(); } catch (const Error &) { did = true; }
    require(did, message);
}
void format_case(const std::string &format, std::initializer_list<std::uint32_t> arguments, const std::string &expected) {
    Runtime runtime; std::ostringstream trace;
    badman::Host host(runtime, 0x089F0000, 0x089E0000, 0x0891F420, trace);
    host.install({}); host.bootstrap(0x08804000, "test");
    auto &ctx=runtime.cpu(); auto &memory=runtime.memory();
    string(memory,0x08808000,format); string(memory,0x08809000,"owned-string");
    ctx.gpr[4]=0x08808000; ctx.gpr[2]=0xBAD12345; ctx.gpr[29]=0x0880A000;
    unsigned count=0;
    for (const auto value : arguments) {
        if (count<7) ctx.gpr[5+count]=value;
        else memory.store32(ctx.gpr[29]+(count-7)*4,value);
        ++count;
    }
    const auto ram=memory.bytes(),vram=memory.vram_bytes(); const auto before=ctx;
    runtime.invoke_import("SysMemUserForUser",0x13A5ABEF,ctx);
    require(!runtime.stopped(),"Printf stopped the guest");
    require(trace.str().find("[kernel-printf] "+expected)!=std::string::npos,"Printf formatted the wrong data");
    require(memory.bytes()==ram&&memory.vram_bytes()==vram,"Printf changed guest RAM/VRAM");
    require(ctx.gpr==before.gpr&&ctx.pc==before.pc,"Void printf changed registers/PC");
}
void printf_checks() {
    format_case("own:%s %% done\n",{0x08809000},"own:owned-string % done\n");
    format_case("%d|%i|%u|%x|%X|%c|%p",{0x80000000,0xFFFFFFFF,0xFFFFFFFF,0xABCDEF,0xABCDEF,'Z',0x08804000},
        "-2147483648|-1|4294967295|abcdef|ABCDEF|Z|0x08804000");
    format_case("%08x|%06d|%-8s|%5c",{0xABCD,static_cast<std::uint32_t>(-23),0x08809000,'!'},"0000abcd|-00023|owned-string|    !");
    format_case("%u/%u/%u/%u/%u/%u/%u/%u/%u",{1,2,3,4,5,6,7,8,9},"1/2/3/4/5/6/7/8/9");
    const std::array<std::string,12> invalid{"%n","%f","%llx","%1$s","%*s","%.3s","%9999999u","%","%-","%0","%+u","%#x"};
    for (const auto &format : invalid) {
        Runtime runtime; std::ostringstream trace;
        badman::Host host(runtime,0x089F0000,0x089E0000,0x0891F420,trace); host.install({});
        auto &ctx=runtime.cpu(); string(runtime.memory(),0x08808000,format);
        ctx.gpr[4]=0x08808000;ctx.gpr[5]=0xDEADBEEF;ctx.gpr[29]=0xFFFFFFFF;
        const auto ram=runtime.memory().bytes(),vram=runtime.memory().vram_bytes();
        rejected([&]{runtime.invoke_import("SysMemUserForUser",0x13A5ABEF,ctx);},"Unsupported printf conversion accepted");
        require(trace.str().find("[kernel-printf]")==std::string::npos,"Rejected printf wrote partial payload");
        require(runtime.memory().bytes()==ram&&runtime.memory().vram_bytes()==vram,"Invalid printf changed memory");
    }
    for (unsigned which=0;which<7;++which) {
        Runtime runtime;std::ostringstream trace;
        badman::Host host(runtime,0x089F0000,0x089E0000,0x0891F420,trace);host.install({});
        auto &ctx=runtime.cpu();auto &memory=runtime.memory();ctx.gpr[4]=0x08808000;ctx.gpr[5]=0x08809000;
        string(memory,ctx.gpr[4],"%s");string(memory,ctx.gpr[5],"good");
        switch(which) {
        case 0:ctx.gpr[4]=0xDEADBEEF;break;
        case 1:ctx.gpr[5]=0xDEADBEEF;break;
        case 2:for(unsigned i=0;i<1024;++i)memory.store8(ctx.gpr[4]+i,'A');break;
        case 3:for(unsigned i=0;i<1024;++i)memory.store8(ctx.gpr[5]+i,'A');break;
        case 4:string(memory,ctx.gpr[4],"%u%u%u%u%u%u%u%u");ctx.gpr[29]=0xFFFFFFFF;break;
        case 5:{std::string format;for(unsigned i=0;i<33;++i)format+="%u";string(memory,ctx.gpr[4],format);}
            ctx.gpr[29]=0x0880A000;break;
        case 6:{std::string format;for(unsigned i=0;i<5;++i)format+="%s";string(memory,ctx.gpr[4],format);
            for(unsigned i=5;i<10;++i)ctx.gpr[i]=0x08809000;string(memory,0x08809000,std::string(1000,'A'));break;}
        }
        const auto ram=memory.bytes(),vram=memory.vram_bytes();
        rejected([&]{runtime.invoke_import("SysMemUserForUser",0x13A5ABEF,ctx);},"Invalid/bounded printf did not reject");
        require(trace.str().find("[kernel-printf]")==std::string::npos,"Invalid printf produced a partial payload");
        require(memory.bytes()==ram&&memory.vram_bytes()==vram,"Invalid printf mutated guest memory");
    }
    {
        Runtime runtime; std::ostringstream trace;
        badman::Host host(runtime,0x089F0000,0x089E0000,0x0891F420,trace);host.install({});
        string(runtime.memory(),0x08808000,"owned trace failure");runtime.cpu().gpr[4]=0x08808000;
        trace.setstate(std::ios::badbit);
        rejected([&]{runtime.invoke_import("SysMemUserForUser",0x13A5ABEF,runtime.cpu());},"Failed trace output pretended to complete printf");
    }
    std::cout<<"PASS_PRINTF bounded32bit_EABI registers7_stack2 errors19 full_RAM_VRAM_unchanged\n";
}
void break_hle_binding() {
    Runtime runtime;std::ostringstream trace;
    badman::Host host(runtime,0x089F0000,0x089E0000,0x0891F420,trace);host.install({});host.bootstrap(0x08804000,"test");
    auto &ctx=runtime.cpu();constexpr auto code=0x08808000u,param=0x08809000u;
    words(runtime.memory(),code,{0x12000BAD,0x0F000055,0x0C000000});
    words(runtime.memory(),param,{0x11223344,0x55667788,0x99AABBCC,0xDDEEFF00});
    ctx.gpr[4]=code;ctx.gpr[5]=code;ctx.gpr[6]=0xFFFFFFFF;ctx.gpr[7]=0;
    runtime.invoke_import("sceGe_user",0xAB49E76A,ctx);const auto list=ctx.gpr[2];
    ctx.gpr[4]=2;ctx.gpr[5]=param;runtime.invoke_import("sceGe_user",0xB448EC0D,ctx);
    require(ctx.gpr[2]==0x80000107&&host.ge_state().cancelled_lists()==0,"HLE invalid mode mutated queue");
    const auto ram=runtime.memory().bytes(),vram=runtime.memory().vram_bytes();
    ctx.gpr[4]=1;ctx.gpr[5]=param;runtime.invoke_import("sceGe_user",0xB448EC0D,ctx);
    require(ctx.gpr[2]==0&&!runtime.stopped()&&host.ge_state().cancelled_lists()==1&&host.ge_state().completed_lists()==0,
        "HLE GeBreak failed real cancellation");
    require(runtime.memory().bytes()==ram&&runtime.memory().vram_bytes()==vram,"HLE wrote unused parameter/memory");
    ctx.gpr[4]=list;ctx.gpr[5]=1;runtime.invoke_import("sceGe_user",0x03444EB4,ctx);
    require(ctx.gpr[2]==0x80000100&&!runtime.stopped(),"HLE cancelled list did not report hardware-tested invalid ID");
    ctx.gpr[4]=1;runtime.invoke_import("sceGe_user",0xB287BD61,ctx);
    require(ctx.gpr[2]==0&&host.ge_state().executed_commands()==0,"HLE cancellation ran stalled code");
    std::cout<<"PASS_GE_BREAK_HLE real_binding invalid_mode queued_cancel parameter_untouched old_list_error\n";
}
void break_validation() {
    GuestMemory memory;badman::Ge ge(memory,[](const auto&){});
    for (const auto mode : {0xFFFFFFFFu,2u,0x80000000u})
        require(static_cast<std::uint32_t>(ge.break_queue(mode,0))==0x80000107,"Invalid GE break mode accepted");
    for (const auto param : {0xDEADBEEFu,0xFFFFFFFFu,0x7FFFFFF0u,0x80000000u})
        require(static_cast<std::uint32_t>(ge.break_queue(1,param))==0x80000023,"Invalid GE break parameter accepted");
    for (const auto param : {0u,0x08804000u,0x7FFFFFEFu})
        require(static_cast<std::uint32_t>(ge.break_queue(1,param))==0x80000020,"Empty break/unmapped unused parameter is wrong");
    constexpr std::uint32_t a=0x08804000,b=a+0x100,c=a+0x200;
    words(memory,a,{0x12000011,0x0F000001,0x0C000000});
    words(memory,b,{0x12000022,0x0F000002,0x0C000000});
    words(memory,c,{0x12000033,0x0F000003,0x0C000000});
    const auto first=ge.enqueue(a,a+4,-1),second=ge.enqueue(b,0,-1),third=ge.enqueue(c,0,-1,0,true);
    const auto before=memory.bytes(),vram=memory.vram_bytes();
    rejected([&]{ge.break_queue(0,0);},"Unsupported active mode0 claimed success");
    require(ge.command(0x12)==0x12000011&&ge.list_status(second,false)==1,"Mode0 error altered queue");
    require(static_cast<std::uint32_t>(ge.break_queue(1,0xFFFFFFFF))==0x80000023&&ge.list_status(first,false)==3,
        "Invalid break parameter cancelled work");
    require(ge.break_queue(1,0x7FFFFFEF)==0,"Mode1 failed real cancellation");
    require(ge.cancelled_lists()==3&&ge.completed_lists()==0&&ge.delivered_callbacks()==0&&ge.executed_commands()==1,
        "Cancellation invented commands/callbacks/completion");
    for(auto id:{first,second,third}){
        require(static_cast<std::uint32_t>(ge.list_status(id,false))==0x80000100,"Cancelled list did not become invalid");
        require(static_cast<std::uint32_t>(ge.list_status(id,true))==0x80000100,"Cancelled blocking query invented completion");
        rejected([&]{ge.update_stall(id,0);},"Cancelled list resumed");
    }
    require(memory.bytes()==before&&memory.vram_bytes()==vram,"Cancellation touched parameter or guest memory");
    require(ge.draw_status(true)==0&&ge.draw_status(false)==0&&ge.command(0x12)==0x12000011,"Reset did not empty queue/retain registers");
    const auto replacement=ge.enqueue(b,0,-1);
    require(ge.list_status(replacement,true)==0&&ge.completed_lists()==1&&ge.cancelled_lists()==3&&ge.command(0x12)==0x12000022,
        "Fresh queue after cancellation failed");
    std::cout<<"PASS_GE_BREAK queue3 empty_errors_K1 unused_param old_id_invalid_no_completion\n";
}
void callback_break(bool signal, bool throws_after=false) {
    GuestMemory memory;constexpr std::uint32_t code=0x08804000,replacement=code+0x100,cb=code+0x200,tail=code+0x300;
    words(memory,cb,{0x08810000,0,0x08810004,0});
    words(memory,code,signal?std::initializer_list<std::uint32_t>{0x0E010055,0x0C000000,0x12000BAD,0x0F000066,0x0C000000}:
        std::initializer_list<std::uint32_t>{0x0F000055,0x0C000000});
    words(memory,replacement,{0x12001234,0x0F000077,0x0C000000});
    words(memory,tail,{0x1200DEAD,0x0F000088,0x0C000000});
    badman::Ge *selected=nullptr;std::vector<std::uint32_t> tokens;std::int32_t fresh=-1;
    badman::Ge ge(memory,[&](const badman::GeInterrupt &event){
        tokens.push_back(event.token);
        if(event.token==0x55){
            require(selected->break_queue(1,cb)==0,"Callback break failed");
            fresh=selected->enqueue(replacement,0,0);
            if(throws_after)throw Error("Synthetic error after cancellation");
        }
    });selected=&ge;const auto callback=ge.set_callback(cb);require(callback==0,"Callback fixture expects ID0");
    const auto old=ge.enqueue(code,code,callback);const auto oldtail=ge.enqueue(tail,0,callback);
    if(throws_after)rejected([&]{ge.update_stall(old,0);},"Callback exception was swallowed");
    else ge.update_stall(old,0);
    const auto expected=signal?0x80000100u:0u;
    require(static_cast<std::uint32_t>(ge.list_status(old,false))==expected,"Old active list state is wrong after callback break");
    require(static_cast<std::uint32_t>(ge.list_status(oldtail,false))==0x80000100,"Queued tail was not cancelled");
    require(fresh>0&&ge.list_status(fresh,true)==0&&ge.draw_status(true)==0,"Outer pump popped replacement work");
    require(ge.command(0x12)==0x12001234,"Cancelled SIGNAL continuation or tail ran");
    require(tokens==std::vector<std::uint32_t>{0x55,0x77},"Cancelled old FINISH or replacement callback ordering is wrong");
    require(ge.cancelled_lists()==(signal?2:1)&&ge.completed_lists()==(signal?1:2),"Reentrant completion/cancellation counts are wrong");
    require(ge.delivered_callbacks()==(throws_after?1:2),"Thrown callback counted as delivered");
    require(ge.executed_commands()==5,"Cancelled commands executed or replacement commands were lost");
}
struct TrackedAudio final : badman::AudioTransport {
    int &opened,&closed,&destroyed;
    TrackedAudio(int &o,int &c,int &d):opened(o),closed(c),destroyed(d){}
    ~TrackedAudio(){++destroyed;}
    void open(std::uint32_t)override{++opened;}
    void close(std::uint32_t)noexcept override{++closed;}
    void submit(std::uint32_t,std::uint64_t,std::span<const std::int16_t>)override{}
    std::vector<badman::AudioProgress> poll()override{return {};}
};
bool forbidden_continuation=false,exit_callback_rejected=false;
void exit_import(Runtime &runtime,AllegrexContext &ctx){
    const auto import_pc=ctx.pc,ra=ctx.gpr[31];runtime.invoke_import("LoadExecForUser",0x05572A5F,ctx);
    if(!runtime.stopped()&&ctx.pc==import_pc)ctx.pc=ra;
}
void continuation(Runtime &,AllegrexContext &){forbidden_continuation=true;throw Error("Exit returned into guest");}
void forbidden_callback_exit(Runtime &runtime,AllegrexContext &ctx){
    try{runtime.invoke_import("LoadExecForUser",0x05572A5F,ctx);}catch(const Error &){exit_callback_rejected=true;}
    ctx.pc=ctx.gpr[31];
}
void exit_checks(){
    int opened=0,closed=0,destroyed=0;
    Runtime runtime;std::ostringstream trace;
    {
        badman::Host host(runtime,0x089F0000,0x089E0000,0x0891F420,trace);host.install({});
        rejected([&]{runtime.invoke_import("LoadExecForUser",0x05572A5F,runtime.cpu());},"Exit without running thread accepted");
        require(!runtime.stopped(),"Invalid exit stopped guest");
        host.bootstrap(0x08804000,"test");host.configure_audio_transport(std::make_unique<TrackedAudio>(opened,closed,destroyed));
        auto &ctx=runtime.cpu();ctx.gpr[4]=0;ctx.gpr[5]=64;ctx.gpr[6]=0;
        runtime.invoke_import("sceAudio",0x5EC81C55,ctx);require(ctx.gpr[2]==0&&opened==1,"Owned audio did not open");
        runtime.register_function(0x08810000,forbidden_callback_exit,"no_interrupt_exit");
        words(runtime.memory(),0x08808000,{0,0,0x08810000,0});words(runtime.memory(),0x08809000,{0x0F000055,0x0C000000});
        ctx.gpr[4]=0x08808000;runtime.invoke_import("sceGe_user",0xA4FC06A4,ctx);const auto callback=ctx.gpr[2];
        ctx.gpr[4]=0x08809000;ctx.gpr[5]=0;ctx.gpr[6]=callback;ctx.gpr[7]=0;
        runtime.invoke_import("sceGe_user",0xAB49E76A,ctx);
        require(exit_callback_rejected&&!runtime.stopped()&&host.ge_state().completed_lists()==1,"Exit was accepted inside interrupt");
        runtime.register_function(0x08804000,exit_import,"owned_exit_import");runtime.register_function(0x08805000,continuation,"must_not_return");
        ctx.gpr[31]=0x08805000;ctx.gpr[2]=0xBAD12345;const auto ram=runtime.memory().bytes(),vram=runtime.memory().vram_bytes();
        runtime.run(0x08804000,10);
        require(runtime.stopped()&&runtime.stop_reason()=="Game requested exit"&&!forbidden_continuation,"Exit did not terminate native dispatch");
        require(ctx.pc==0x08804000&&ctx.gpr[2]==0xBAD12345,"Void exit fabricated return PC/value");
        require(runtime.memory().bytes()==ram&&runtime.memory().vram_bytes()==vram,"Exit changed guest memory");
        require(closed==0&&destroyed==0,"Exit bypassed ordinary host lifetime");
    }
    require(opened==1&&closed==1&&destroyed==1,"Native unwind did not close/destroy owned transport once");
    std::cout<<"PASS_EXIT no_return thread_only actual_dispatch_stop native_RAII_audio_close\n";
}
}
int main(){
    try{
#if defined(_WIN32)
        _putenv_s("BADMAN_RENDERER","software");
#else
        setenv("BADMAN_RENDERER","software",1);
#endif
        printf_checks();break_validation();break_hle_binding();
        callback_break(true);callback_break(false);callback_break(true,true);
        std::cout<<"PASS_GE_REENTRANT signal_break_new_queue finish_break_new_queue exception_after_cancel\n";
        exit_checks();return 0;
    }catch(const std::exception &e){std::cerr<<e.what()<<'\n';return 1;}
}

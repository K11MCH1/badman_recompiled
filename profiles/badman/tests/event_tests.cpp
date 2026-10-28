#include "badman_host.hpp"
#include "psprecomp/common.hpp"
#include <array>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
using namespace psprecomp;
constexpr std::uint32_t root_entry=0x08804000,root_pc=0x08804004;
constexpr std::uint32_t child_entry=0x08805000,child_pc=0x08805004;
constexpr std::uint32_t name=0x08802000,output=0x08802100,budget=0x08802200,status=0x08802300;
constexpr std::uint32_t create=0x55C20A00,set=0x1FB15A32,clear=0x812346E4,remove=0xEF9E4C70;
constexpr std::uint32_t poll=0x30FD48F0,wait_event=0x402FCF22,refer=0xA66B0120,delay=0xCEADEB47;
void require(bool value,const char *message){if(!value)throw std::runtime_error(message);}
template<class F> void rejects(F action,const char *message){bool failed=false;try{action();}catch(const Error &){failed=true;}require(failed,message);}
void guest_return(Runtime &,AllegrexContext &ctx){ctx.pc=badman::Host::thread_return_pc;}
struct Fixture {
    Runtime runtime;std::ostringstream trace;std::uint64_t now{1000000000};std::vector<std::uint64_t> slept;
    badman::Host host{runtime,0x089F0000,0x089E0000,0x0891F420,trace};
    Fixture(){
        runtime.register_function(root_entry,guest_return,"event_root");runtime.register_function(root_pc,guest_return,"event_root_return");
        runtime.register_function(child_entry,guest_return,"event_worker");runtime.register_function(child_pc,guest_return,"event_worker_return");
        host.install({});host.set_scheduler_timing([this]{return now;},[this](std::uint64_t target){slept.push_back(target);now=target;});
        const std::array<std::uint8_t,6> text{'e','v','e','n','t',0};runtime.memory().copy_in(name,text);host.bootstrap(root_entry,"events");
    }
    std::uint32_t call(std::uint32_t nid,std::initializer_list<std::uint32_t> arguments={}){
        auto &ctx=runtime.cpu();for(unsigned r=4;r<12;++r)ctx.gpr[r]=0;
        unsigned r=4;for(const auto argument:arguments)ctx.gpr[r++]=argument;
        runtime.invoke_import("ThreadManForUser",nid,ctx);return ctx.gpr[2];
    }
    std::uint32_t event(std::uint32_t bits=0,std::uint32_t attributes=0x200){return call(create,{name,attributes,bits,0});}
    std::uint32_t child(std::uint32_t priority=40){
        const auto uid=call(0x446D8DE6,{name,child_entry,priority,4096,0,0});runtime.cpu().gpr[31]=root_pc;
        require(call(0xF475845D,{uid,0,0})==0,"Worker start failed");return uid;
    }
    std::uint32_t metadata(std::uint32_t uid,std::uint32_t offset){
        runtime.memory().store32(status,52);require(call(refer,{uid,status})==0,"Event refer failed");return runtime.memory().load32(status+offset);
    }
};
void poll_results_and_validation(){
    Fixture f;auto &memory=f.runtime.memory();const auto event=f.event(0xf0f0);
    memory.store32(output,0xdeadbeef);
    require(f.call(poll,{event,0xffff,0,output})==0x800201AF&&memory.load32(output)==0xf0f0&&!f.runtime.stopped(),
        "Unsatisfied Poll did not return EVF_COND with current output bits");
    require(f.metadata(event,44)==0xf0f0,"Unsatisfied Poll consumed bits");
    for(const auto mode:{2u,4u,0x30u,0x31u,0x40u}){
        memory.store32(output,0xdeadbeef);
        require(f.call(poll,{event,0,mode,output})==0x80020195&&memory.load32(output)==0xdeadbeef,"Invalid mode precedence/preservation failed");
    }
    require(f.call(poll,{0,0,0,output})==0x800201B1&&memory.load32(output)==0xdeadbeef,"Zero pattern did not precede UID error");
    require(f.call(poll,{0,1,0,output})==0x8002019A&&memory.load32(output)==0xdeadbeef,"Unknown UID changed output");
    require(f.call(poll,{event,0x10,0x21,output})==0&&memory.load32(output)==0xf0f0&&f.metadata(event,44)==0xf0e0,
        "Successful clear-bits Poll did not expose whole pre-clear pattern");
    require(f.call(poll,{event,0x20,0x11,0})==0&&f.metadata(event,44)==0,"Clear-all/null-output Poll failed");
    f.call(set,{event,0xabcdef});f.call(clear,{event,0xff00});require(f.metadata(event,44)==0xcd00,"Clear did not use retain mask");
    rejects([&]{f.call(poll,{event,0x100,0x11,output+1});},"Misaligned output was accepted");
    require(f.metadata(event,44)==0xcd00,"Invalid output cleared event pattern");
    require(f.call(create,{name,0x100,0,0})==0x80020191&&f.call(create,{name,0x300,0,0})==0x80020191,"Event accepted semaphore priority attributes");
    require(f.call(create,{0,0,0,0})==0x80020001,"Null event name succeeded");
}
void sequential_clear_waiters_and_delete(){
    Fixture f;auto &ctx=f.runtime.cpu();auto &memory=f.runtime.memory();
    const auto event=f.event(),first=f.child(40),second=f.child(41),signaller=f.child(42);
    memory.store32(output,0xdeadbeef);memory.store32(output+4,0xdeadbeef);memory.store32(output+8,0xdeadbeef);
    ctx.gpr[16]=0x12344321;const auto root_gp=ctx.gpr[28],root_sp=ctx.gpr[29];ctx.gpr[31]=root_pc;
    f.call(wait_event,{event,0xff,0x21,output,0});
    require(runtime_thread_uid()==static_cast<std::int32_t>(first)&&ctx.pc==child_entry&&f.slept.empty(),"Event Wait stranded a ready worker");
    ctx.gpr[31]=child_pc;f.call(wait_event,{event,0xff00,0x21,output+4,0});
    require(runtime_thread_uid()==static_cast<std::int32_t>(second),"Second Wait did not choose next ready worker");
    ctx.gpr[31]=child_pc;f.call(wait_event,{event,0xffff,0x21,output+8,0});
    require(runtime_thread_uid()==static_cast<std::int32_t>(signaller)&&f.metadata(event,48)==3,"Waiter count omitted registered blocked threads");
    ctx.gpr[31]=child_pc;f.call(set,{event,0xffff});
    require(runtime_thread_uid()==1&&ctx.pc==root_pc&&ctx.gpr[2]==0&&ctx.gpr[16]==0x12344321&&ctx.gpr[28]==root_gp&&ctx.gpr[29]==root_sp,
        "Event wake lost caller continuation/registers or completion result");
    require(memory.load32(output)==0xffff&&memory.load32(output+4)==0xff00&&memory.load32(output+8)==0,
        "Set broadcast stale pattern instead of sequential clear consumption");
    require(f.metadata(event,44)==0&&f.metadata(event,48)==1,"Set removed an unmatched waiter or retained completed waiters");
    require(f.call(remove,{event})==0&&f.call(poll,{event,1,0,0})==0x8002019A,"Deleted event remained usable");
    ctx.gpr[31]=root_pc;f.call(delay,{1000});
    require(runtime_thread_uid()==static_cast<std::int32_t>(first),"Successful waiter was not ready after root yield");
    f.call(0xAA73C935,{0});
    require(runtime_thread_uid()==static_cast<std::int32_t>(second)&&ctx.pc==child_pc&&ctx.gpr[2]==0x800201B5,
        "Deleted flag did not resume its waiter with WAIT_DELETE");
    require(memory.load32(output+8)==0,"Deletion overwrote the blocked wait's admitted output snapshot");
}
void single_wait_admission_and_remaining_timeout(){
    Fixture f;auto &ctx=f.runtime.cpu();auto &memory=f.runtime.memory();const auto event=f.event(1,0),child=f.child();
    memory.store32(budget,1000);ctx.gpr[31]=root_pc;f.call(wait_event,{event,8,0,output,budget});
    require(runtime_thread_uid()==static_cast<std::int32_t>(child)&&memory.load32(output)==1&&f.metadata(event,48)==1,"Wait admission did not publish current snapshot");
    memory.store32(output+4,0xdeadbeef);
    require(f.call(poll,{event,1,0,output+4})==0x800201B0&&memory.load32(output+4)==0xdeadbeef,"Single-wait Poll failed to reject another waiter atomically");
    require(f.call(wait_event,{event,1,0,output+4,0})==0x800201B0&&memory.load32(output+4)==0xdeadbeef,"Single-wait Wait incorrectly matched before multi-admission validation");
    f.now+=250000;ctx.gpr[31]=child_pc;f.call(set,{event,8});
    require(runtime_thread_uid()==1&&ctx.pc==root_pc&&ctx.gpr[2]==0&&memory.load32(output)==9&&memory.load32(budget)==750,
        "Timed successful waiter lost remaining microseconds or whole matching bits");
    require(f.metadata(event,48)==0,"Completed timed wait remained registered");
}
void typed_timeouts_and_zero_timeout(){
    {
        Fixture f;auto &memory=f.runtime.memory();const auto event=f.event(7);
        memory.store32(output,0xdeadbeef);memory.store32(budget,0);
        require(f.call(wait_event,{event,8,0,output,budget})==0x800201A8&&memory.load32(output)==0xdeadbeef&&f.slept.empty(),
            "Zero timeout modified output or blocked");
        memory.store32(budget,1000);f.runtime.cpu().gpr[31]=root_pc;
        f.call(wait_event,{event,8,0,output,budget});
        require(runtime_thread_uid()==1&&f.runtime.cpu().pc==root_pc&&f.runtime.cpu().gpr[2]==0x800201A8&&
            f.now==1001000000&&memory.load32(budget)==0&&memory.load32(output)==7,
            "Event timer fabricated delay success, woke early, or lost timeout outputs");
        require(f.metadata(event,48)==0&&f.metadata(event,44)==7,"Expired waiter stayed queued or consumed event bits");
    }
    {
        Fixture f;auto &ctx=f.runtime.cpu();auto &memory=f.runtime.memory();const auto event=f.event(),child=f.child();
        memory.store32(budget,1000);ctx.gpr[31]=root_pc;f.call(wait_event,{event,1,0,output,budget});
        require(runtime_thread_uid()==static_cast<std::int32_t>(child)&&f.slept.empty(),"Timed event wait slept despite ready worker");
        f.now+=1000000;ctx.gpr[31]=child_pc;f.call(set,{event,1});
        require(runtime_thread_uid()==1&&ctx.gpr[2]==0x800201A8&&memory.load32(output)==0&&memory.load32(budget)==0,
            "Late Set consumed expired waiter or replaced timeout with success");
        require(f.metadata(event,44)==1&&f.metadata(event,48)==0,"Late Set did not retain unconsumed bits");
    }
}
void masked_wait_and_invalid_buffers_preserve_state(){
    Fixture f;auto &memory=f.runtime.memory();const auto event=f.event(1);
    memory.store32(output,0xdeadbeef);f.runtime.invoke_import("Kernel_Library",0x092968F4,f.runtime.cpu());
    require(f.call(wait_event,{event,2,0,output,0})==0x800201A7&&memory.load32(output)==0xdeadbeef,"Masked Wait mutated output before CAN_NOT_WAIT");
    require(f.metadata(event,48)==0,"Rejected masked Wait enqueued a thread");f.runtime.cpu().gpr[4]=1;
    f.runtime.invoke_import("Kernel_Library",0x5F10D406,f.runtime.cpu());
    rejects([&]{f.call(wait_event,{event,1,0x10,0x09ffffff,0});},"Invalid output accepted");
    require(f.metadata(event,44)==1,"Invalid output consumed event bits");
    memory.store32(budget,20);const auto before=f.now;
    rejects([&]{f.call(wait_event,{event,2,0,output,budget+1});},"Misaligned timeout pointer accepted");
    require(memory.load32(output)==0xdeadbeef&&f.now==before&&f.metadata(event,48)==0,"Invalid timeout mutated output or registered wait");
    f.now=std::numeric_limits<std::uint64_t>::max()-100;memory.store32(budget,1);
    rejects([&]{f.call(wait_event,{event,2,0,output,budget});},"Overflowing event deadline accepted");
    require(memory.load32(output)==0xdeadbeef&&runtime_thread_uid()==1&&f.slept.empty(),"Deadline overflow partially admitted a wait");
}
}
int main(){try{
    poll_results_and_validation();sequential_clear_waiters_and_delete();single_wait_admission_and_remaining_timeout();
    typed_timeouts_and_zero_timeout();masked_wait_and_invalid_buffers_preserve_state();
    std::cout<<"Event flag tests passed\n";return 0;
}catch(const std::exception &error){std::cerr<<error.what()<<'\n';return 1;}}

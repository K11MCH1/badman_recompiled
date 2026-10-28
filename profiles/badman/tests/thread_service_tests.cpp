#include "badman_host.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
using namespace psprecomp;
constexpr std::uint32_t root_entry = 0x08804000, root_pc = 0x08804004;
constexpr std::uint32_t child_entry = 0x08805000, child_pc = 0x08805004;
constexpr std::uint32_t name = 0x08802000, output = 0x08802100;
constexpr std::uint32_t delay = 0xCEADEB47, delay_cb = 0x68DA9E36, priority = 0x71BC9871;
constexpr std::uint32_t remove_thread = 0x9FA03CD3, event_status = 0xA66B0120;
void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
template <class Function> void rejects(Function function, const char *message) {
    bool rejected = false; try { function(); } catch (const Error &) { rejected = true; }
    require(rejected, message);
}
void guest_return(Runtime &, AllegrexContext &ctx) { ctx.pc = badman::Host::thread_return_pc; }
struct Time {
    std::uint64_t now{1000000000};
    std::vector<std::uint64_t> deadlines;
    bool advance{true};
    auto clock() { return [this] { return now; }; }
    auto wait() { return [this](std::uint64_t deadline) {
        deadlines.push_back(deadline); if (advance) now = deadline;
    }; }
};
struct Fixture {
    Runtime runtime; std::ostringstream trace; Time time;
    badman::Host host{runtime, 0x089F0000, 0x089E0000, 0x0891F420, trace};
    explicit Fixture(bool bootstrap = true) {
        runtime.register_function(root_entry, guest_return, "thread_services_root");
        runtime.register_function(root_pc, guest_return, "thread_services_root_return");
        runtime.register_function(child_entry, guest_return, "thread_services_child");
        runtime.register_function(child_pc, guest_return, "thread_services_child_return");
        host.install({}); host.set_scheduler_timing(time.clock(), time.wait());
        const std::array<std::uint8_t, 5> text{'t', 'e', 's', 't', 0}; runtime.memory().copy_in(name, text);
        if (bootstrap) host.bootstrap(root_entry, "thread services");
    }
    std::uint32_t call(std::uint32_t nid, std::initializer_list<std::uint32_t> arguments = {}) {
        auto &ctx = runtime.cpu();
        for (unsigned reg = 4; reg < 10; ++reg) ctx.gpr[reg] = 0;
        unsigned reg = 4; for (const auto argument : arguments) ctx.gpr[reg++] = argument;
        runtime.invoke_import("ThreadManForUser", nid, ctx); return ctx.gpr[2];
    }
    std::uint32_t create_child(std::uint32_t child_priority = 40, std::uint32_t attributes = 0) {
        return call(0x446D8DE6, {name, child_entry, child_priority, 4096, attributes, 0});
    }
    std::uint32_t ready_child(std::uint32_t child_priority = 40) {
        const auto uid = create_child(child_priority);
        runtime.cpu().gpr[31] = root_pc; require(call(0xF475845D, {uid, 0, 0}) == 0, "Starting child failed");
        return uid;
    }
};
void delay_runs_ready_workers_and_preempts_at_deadline() {
    Fixture f; const auto child = f.ready_child(); auto &ctx = f.runtime.cpu();
    const auto root_sp = ctx.gpr[29], root_gp = ctx.gpr[28]; ctx.gpr[16] = 0x12345678; ctx.gpr[31] = root_pc;
    const auto token = capture_runtime_execution_context(); f.call(delay, {1000});
    require(runtime_thread_uid() == static_cast<std::int32_t>(child) && !runtime_execution_context_matches(token) &&
        ctx.pc == child_entry && f.time.deadlines.empty(), "Delay slept on the host instead of running a ready PSP worker");
    ctx.pc = 0x08805010; ctx.gpr[31] = child_pc; ctx.gpr[16] = 0xCAFEBABE;
    f.time.now += 999999; require(f.call(0x293B45B8) == child && runtime_thread_uid() == static_cast<std::int32_t>(child),
        "Delayed PSP thread resumed before its actual deadline");
    ++f.time.now; f.call(0x293B45B8);
    require(runtime_thread_uid() == 1 && ctx.pc == root_pc && ctx.gpr[2] == 0 && ctx.gpr[29] == root_sp &&
        ctx.gpr[28] == root_gp && ctx.gpr[16] == 0x12345678, "Due timer lost the delayed caller context or completion result");
    ctx.gpr[31] = root_pc; f.call(delay, {1000});
    require(runtime_thread_uid() == static_cast<std::int32_t>(child) && ctx.pc == child_pc && ctx.gpr[2] == child &&
        ctx.gpr[16] == 0xCAFEBABE && f.time.deadlines.empty(), "Preempted worker lost its completed import result or return PC");
}
void idle_scheduler_waits_for_earliest_timer() {
    Fixture f; const auto child = f.ready_child(); auto &ctx = f.runtime.cpu();
    ctx.gpr[31] = root_pc; f.call(delay, {10000});
    require(runtime_thread_uid() == static_cast<std::int32_t>(child), "Root delay did not schedule its worker");
    ctx.gpr[31] = child_pc; f.call(delay, {3000});
    require(f.time.deadlines == std::vector<std::uint64_t>{1003000000} && runtime_thread_uid() == static_cast<std::int32_t>(child) &&
        ctx.pc == child_pc && ctx.gpr[2] == 0, "Idle scheduler did not wait for the earliest real deadline");
    f.call(delay, {12000});
    require(f.time.deadlines.back() == 1010000000 && runtime_thread_uid() == 1 && ctx.pc == root_pc && ctx.gpr[2] == 0,
        "Earlier root timer did not resume before the later worker delay");
    f.call(delay, {20000});
    require(f.time.deadlines.back() == 1015000000 && runtime_thread_uid() == static_cast<std::int32_t>(child) &&
        ctx.pc == child_pc, "Later worker timer was prematurely consumed or lost");
}
void zero_delay_callbacks_and_clock_failures() {
    {
        Fixture f; auto &ctx = f.runtime.cpu(); ctx.gpr[31] = root_pc;
        require(f.call(delay, {0}) == 0 && ctx.pc == root_pc && f.time.deadlines.empty(), "Zero delay fabricated a host sleep");
        f.call(0xE81CAF8F, {name, child_entry, 0}); // Registered, never notified.
        ctx.gpr[31] = root_pc;
        require(f.call(delay_cb, {7}) == 0 && f.time.deadlines == std::vector<std::uint64_t>{1000007000},
            "DelayThreadCB treated mere callback registration as a pending notification");
        rejects([&] { f.host.set_scheduler_timing(f.time.clock(), f.time.wait()); }, "Active timer epoch could be replaced");
    }
    {
        Fixture f; --f.time.now;
        rejects([&] { f.call(delay, {1}); }, "Backward scheduler clock was accepted");
        require(f.time.deadlines.empty() && runtime_thread_uid() == 1, "Backward clock changed threads or waited");
    }
    {
        Fixture f; f.time.advance = false; f.runtime.cpu().gpr[31] = root_pc; f.runtime.cpu().gpr[2] = 0xA5A5A5A5;
        rejects([&] { f.call(delay, {10}); }, "Early scheduler waiter fabricated successful delay completion");
        require(f.runtime.cpu().gpr[2] == 0xA5A5A5A5, "Early waiter returned a success result");
    }
    {
        Fixture f(false);
        rejects([&] { f.host.set_scheduler_timing(f.time.clock(), {}); }, "Half-configured scheduler timing was accepted");
        f.time.now = std::numeric_limits<std::uint64_t>::max() - 100;
        f.host.set_scheduler_timing(f.time.clock(), f.time.wait()); f.host.bootstrap(root_entry, "overflow");
        rejects([&] { f.call(delay, {1}); }, "Overflowing thread delay deadline was accepted");
        require(f.time.deadlines.empty() && runtime_thread_uid() == 1, "Deadline overflow changed threads or waited");
    }
}
void priority_changes_preserve_import_flow() {
    Fixture f; const auto child = f.ready_child(); auto &ctx = f.runtime.cpu();
    for (const auto invalid : {1u, 7u, 0x78u, 0xFFFFFFFFu})
        require(f.call(priority, {0, invalid}) == 0x80020193 && runtime_thread_uid() == 1, "Illegal user priority was accepted");
    require(f.call(priority, {0x12345678, 16}) == 0x80020198, "Unknown priority target did not return UNKNOWN_THID");
    const auto dormant = f.create_child(); require(f.call(priority, {dormant, 16}) == 0x800201A2,
        "Priority change on a dormant PSP thread succeeded");
    ctx.gpr[31] = root_pc; f.call(priority, {child, 16});
    require(runtime_thread_uid() == static_cast<std::int32_t>(child) && ctx.pc == child_entry,
        "Raised ready worker priority did not preempt after the original import");
    ctx.pc = 0x08805010; ctx.gpr[31] = child_pc; ctx.gpr[16] = 0x12344321;
    f.call(priority, {0, 64});
    require(runtime_thread_uid() == 1 && ctx.pc == root_pc && ctx.gpr[2] == 0,
        "Lowering current priority did not preserve its caller result and resume the root");
    require(f.call(priority, {child, 0}) == 0 && runtime_thread_uid() == 1,
        "Priority zero did not inherit caller priority without equal-priority preemption");
    ctx.gpr[31] = root_pc; f.call(delay, {1000});
    require(runtime_thread_uid() == static_cast<std::int32_t>(child) && ctx.pc == child_pc && ctx.gpr[2] == 0 &&
        ctx.gpr[16] == 0x12344321, "Priority preemption lost the worker continuation or saved registers");
}
void same_thread_redirect_preserves_callback_entry() {
    Fixture f; const auto child = f.ready_child(); auto &ctx = f.runtime.cpu();
    ctx.gpr[31] = root_pc; f.call(delay, {1000});
    require(runtime_thread_uid() == static_cast<std::int32_t>(child), "Redirect fixture did not enter worker");
    constexpr std::uint32_t callback = 0x08807000, callback_return = 0x08000008;
    // A synthetic provider executes inside a real Host-bound import. Its only
    // instrumentation simulates another HLE service redirecting this same PSP
    // thread to a callback, while a higher-priority timer becomes due.
    f.host.set_input_provider([&] {
        ctx.pc = callback; ctx.gpr[31] = callback_return; f.time.now += 1000000;
        return badman::InputState{true, 0, 128, 128};
    });
    ctx.pc = 0x088E9B00; ctx.gpr[31] = child_pc; ctx.gpr[4] = output; ctx.gpr[5] = 1;
    f.runtime.invoke_import("sceCtrl", 0x1F803938, ctx);
    require(runtime_thread_uid() == 1 && ctx.pc == root_pc, "Due timer did not preempt after a same-thread redirect");
    ctx.gpr[31] = root_pc; f.call(delay, {1000});
    require(runtime_thread_uid() == static_cast<std::int32_t>(child) && ctx.pc == callback && ctx.gpr[31] == callback_return &&
        ctx.gpr[2] == 1, "Timer preemption skipped the redirected callback and resumed its return trampoline early");
}
void deletion_reclaims_only_dormant_threads() {
    Fixture f; auto &ctx = f.runtime.cpu();
    const auto free = f.host.free_bytes(); const auto dormant = f.create_child(40, 0x00200000);
    require(f.host.free_bytes() < free && f.call(remove_thread, {dormant}) == 0 && f.host.free_bytes() == free,
        "Deleting dormant thread did not reclaim its owned stack/TLS allocations");
    require(f.call(remove_thread, {dormant}) == 0x80020198 && f.call(remove_thread, {0}) == 0x80020197 &&
        f.call(remove_thread, {1}) == 0x800201A4, "Delete accepted an invalid or currently running PSP thread");
    const auto child = f.ready_child(); const auto allocated = f.host.free_bytes();
    require(f.call(remove_thread, {child}) == 0x800201A4 && f.host.free_bytes() == allocated,
        "Delete freed a ready worker's live stack");
    ctx.gpr[31] = root_pc; f.call(delay, {1000});
    f.call(0xAA73C935, {7}); // Ready root is delayed, so scheduler reaches its deadline.
    require(runtime_thread_uid() == 1 && f.call(remove_thread, {child}) == 0 && f.host.free_bytes() == free,
        "Exited worker could not be deleted or leaked its allocations");
}
void exited_thread_restarts_with_clean_context_and_owned_resources() {
    Fixture f; auto &ctx=f.runtime.cpu();auto &memory=f.runtime.memory();
    const auto initial_free=f.host.free_bytes();const auto child=f.create_child(16,0x4000);
    const auto allocated_free=f.host.free_bytes();
    ctx.gpr[16]=0x13572468;const auto root_sp=ctx.gpr[29];ctx.gpr[31]=root_pc;
    const std::array<std::uint8_t,5> first{1,2,3,4,5};memory.copy_in(output,first);
    require(f.call(0xF475845D,{child,static_cast<std::uint32_t>(first.size()),output})==0,"First start failed");
    const auto check=[&](const auto &arguments){
        require(runtime_thread_uid()==static_cast<std::int32_t>(child) && ctx.pc==child_entry && ctx.gpr[31]==badman::Host::thread_return_pc,
                "Restart lost UID/entry/return or retained changed priority");
        require(ctx.gpr[4]==arguments.size() && ctx.gpr[5]%16==0 && ctx.gpr[29]+64==ctx.gpr[5] && ctx.gpr[28]==0x0891F420 && ctx.gpr[26],
                "Restart lost copied arguments, aligned SP, GP or TLS");
        for(unsigned i=0;i<arguments.size();++i)require(memory.load8(ctx.gpr[5]+i)==arguments[i],"Restart argument copy was stale or overlapped destructively");
        for(unsigned reg=0;reg<32;++reg)if(reg!=4 && reg!=5 && reg!=26 && reg!=28 && reg!=29 && reg!=31)
            require(ctx.gpr[reg]==0,"Restart retained an old general register");
        require(!ctx.hi && !ctx.lo && !ctx.fcr31,"Restart retained HI/LO/FPU control");
        for(auto value:ctx.fpr)require(std::bit_cast<std::uint32_t>(value)==0,"Restart retained an old FPU register");
        for(auto value:ctx.vfpu)require(std::bit_cast<std::uint32_t>(value)==0,"Restart retained an old VFPU lane");
        for(unsigned i=0;i<ctx.vfpu_ctrl.size();++i)require(ctx.vfpu_ctrl[i]==(i<2?0xE4u:0u),"Restart retained old VFPU controls/prefixes");
        for(unsigned word=0;word<64;++word)require(memory.load32(ctx.gpr[26]+word*4)==0,"Restart retained per-invocation TLS data");
        require(f.host.free_bytes()==allocated_free,"Restart allocated/leaked stack or TLS storage");
    };
    check(first);const auto tls=ctx.gpr[26],top=ctx.gpr[5]+16;
    // Lower this invocation's priority, then let the parent wait. On restart,
    // only restoring the original16 (not40) will preempt its priority32 parent.
    ctx.gpr[31]=child_pc;f.call(priority,{0,40});require(runtime_thread_uid()==1,"Priority fixture did not resume parent");
    ctx.gpr[31]=root_pc;f.call(delay,{1000});require(runtime_thread_uid()==static_cast<std::int32_t>(child),"Parent delay did not run worker");
    const auto dirty_and_exit=[&]{
        ctx.gpr.fill(0xDEADBEEF);ctx.hi=0xFFFFFFFF;ctx.lo=0x12345678;ctx.fcr31=0x0181FFFF;
        ctx.fpr.fill(1.25f);ctx.vfpu.fill(-2.5f);ctx.vfpu_ctrl.fill(0xFFFFFFFF);ctx.pc=child_pc;
        memory.store32(tls+16,0xBAD0CAFE);
        f.call(0xAA73C935,{7});
        require(runtime_thread_uid()==1 && ctx.pc==root_pc && ctx.gpr[16]==0x13572468 && ctx.gpr[29]==root_sp,
                "Worker exit damaged its waiting parent's CPU context");
        require(f.host.free_bytes()==allocated_free,"Thread exit released owned restart resources");
    };
    dirty_and_exit();
    std::array<std::uint8_t,32> second;for(unsigned i=0;i<second.size();++i)second[i]=static_cast<std::uint8_t>(0x80+i);
    memory.copy_in(output+64,second);ctx.gpr[31]=root_pc;
    require(f.call(0xF475845D,{child,static_cast<std::uint32_t>(second.size()),output+64})==0,"Exited UID could not restart");
    check(second);require(ctx.gpr[26]==tls && ctx.gpr[5]+32==top,"Restart replaced its original TLS or stack");
    const auto previous_argument=ctx.gpr[5];
    std::array<std::uint8_t,17> third;for(unsigned i=0;i<third.size();++i)third[i]=static_cast<std::uint8_t>(0x30+i);
    memory.copy_in(previous_argument-1,third);dirty_and_exit();ctx.gpr[31]=root_pc;
    require(f.call(0xF475845D,{child,static_cast<std::uint32_t>(third.size()),previous_argument-1})==0,"Second restart failed");
    check(third);require(ctx.gpr[26]==tls && ctx.gpr[5]==previous_argument,"Repeated restart replaced reserved storage");
    dirty_and_exit();require(f.call(remove_thread,{child})==0 && f.host.free_bytes()==initial_free,"Final delete leaked restarted thread storage");
}
void start_errors_preserve_dormant_state_and_null_arguments() {
    Fixture f;auto &ctx=f.runtime.cpu();auto &memory=f.runtime.memory();const auto child=f.create_child(40);
    const auto free=f.host.free_bytes();const auto before=memory.bytes();
    require(f.call(0xF475845D,{0,0,0})==0x80020197 && f.call(0xF475845D,{0x12345678,0,0})==0x80020198 &&
            f.call(0xF475845D,{1,0,0})==0x800201A4,"Start did not distinguish NULL/unknown/current thread UID");
    for(const auto size:{0xFFFFFFFFu,0x80000000u,4096u})
        require(f.call(0xF475845D,{child,size,output})==0x800200D3,"Start accepted negative or unsafe oversized arguments");
    const auto tail=GuestMemory::kPhysicalBase+static_cast<std::uint32_t>(memory.size())-1;
    require(f.call(0xF475845D,{child,4,0xDEADBEEF})==0x800200D3 &&
            f.call(0xF475845D,{child,4,tail})==0x800200D3,"Start accepted a bad/truncated argument pointer");
    require(memory.bytes()==before && f.host.free_bytes()==free && runtime_thread_uid()==1,"Failed start wrote memory, allocated storage or scheduled a worker");
    ctx.gpr[31]=root_pc;require(f.call(0xF475845D,{child,8,0})==0,"NULL argp was not accepted as empty arguments");
    require(f.call(0xF475845D,{child,0,0})==0x800201A4,"Ready thread was started twice");
    ctx.gpr[31]=root_pc;f.call(delay,{1000});
    require(runtime_thread_uid()==static_cast<std::int32_t>(child) && ctx.gpr[4]==0 && ctx.gpr[5]==0,"NULL argp produced a nonempty argument list");
    require(f.call(0xF475845D,{child,0,0})==0x800201A4,"Running thread was restarted");
    f.call(0xAA73C935,{0});require(f.call(remove_thread,{child})==0,"Exited fixture deletion failed");
    require(f.call(0xF475845D,{child,0,0})==0x80020198,"Deleted UID was restarted");
}
void waiting_thread_start_is_refused() {
    Fixture f;const auto child=f.create_child(16);auto &ctx=f.runtime.cpu();ctx.gpr[31]=root_pc;
    f.call(0xF475845D,{child,0,0});ctx.gpr[31]=child_pc;f.call(delay,{1000});
    require(runtime_thread_uid()==1 && f.call(0xF475845D,{child,0,0})==0x800201A4 && runtime_thread_uid()==1,
            "Start unblocked or reset a genuinely waiting thread");
    f.time.now+=1000000;f.call(0x293B45B8);
    require(runtime_thread_uid()==static_cast<std::int32_t>(child) && ctx.pc==child_pc,"Rejected start destroyed the original timed continuation");
}
void event_status_layout_and_bounded_prefixes() {
    Fixture f; auto &memory = f.runtime.memory();
    const auto event = f.call(0x55C20A00, {name, 0x200, 3, 0});
    f.call(0x812346E4, {event, 1}); f.call(0x1FB15A32, {event, 8});
    for (const auto size : {0u, 1u, 2u, 4u, 8u, 44u, 48u, 52u, 56u, 0xFFFFFFFFu}) {
        std::array<std::uint8_t, 60> original; original.fill(0xA5); memory.copy_in(output, original); memory.store32(output, size);
        memory.copy_out(output, original);
        require(f.call(event_status, {event, output}) == 0, "Valid event status request failed");
        const auto copied = std::min<std::uint32_t>(size, 52);
        for (std::uint32_t i = copied; i < original.size(); ++i)
            require(memory.load8(output + i) == original[i], "Event status wrote outside the requested prefix");
        if (copied >= 4) require(memory.load32(output) == 52, "Event status did not report its canonical size");
        if (copied >= 52) require(memory.load32(output + 4) == 0x74736574 && memory.load8(output + 8) == 0 &&
            memory.load32(output + 36) == 0x200 && memory.load32(output + 40) == 3 &&
            memory.load32(output + 44) == 9 && memory.load32(output + 48) == 0,
            "Event metadata layout, initial/current bits or genuine waiter count is wrong");
    }
    std::array<std::uint8_t, 52> previous; memory.copy_out(output, previous);
    require(f.call(event_status, {0x12345678, output}) == 0x8002019A, "Unknown event status target returned success");
    std::array<std::uint8_t, 52> retained; memory.copy_out(output, retained);
    require(retained == previous, "Invalid event status UID changed output");
    rejects([&] { f.call(event_status, {event, output + 1}); }, "Unaligned event status output was accepted");
    const auto tail = GuestMemory::kPhysicalBase + static_cast<std::uint32_t>(memory.size()) - 4;
    memory.store32(tail, 52);
    rejects([&] { f.call(event_status, {event, tail}); }, "Truncated event status output was accepted");
    require(memory.load32(tail) == 52, "Rejected truncated status request partially wrote output");
    f.call(0xEF9E4C70, {event});
    require(f.call(event_status, {event, output}) == 0x8002019A, "Deleted event status returned success");
}
constexpr std::uint64_t input_period=16683333;
std::uint32_t ctrl(Fixture &f,std::uint32_t address,unsigned count=1){
    auto &ctx=f.runtime.cpu();ctx.gpr[4]=address;ctx.gpr[5]=count;
    f.runtime.invoke_import("sceCtrl",0x1F803938,ctx);return ctx.gpr[2];
}
void input_wait_yields_ready_worker_and_restores_full_continuation(){
    Fixture f;auto &ctx=f.runtime.cpu();auto &memory=f.runtime.memory();badman::InputState state{true,0x4000,7,249};
    f.host.set_input_provider([&]{return state;});require(ctrl(f,output)==1,"Initial controller sample unavailable");
    const auto child=f.ready_child();const auto sp=ctx.gpr[29],gp=ctx.gpr[28];
    ctx.gpr[16]=0x12345678;ctx.hi=0xABCD;ctx.lo=0xFEDC;ctx.fpr[3]=0x1234;ctx.vfpu[5]=0x5678;ctx.gpr[31]=root_pc;
    memory.copy_in(output,std::vector<std::uint8_t>(6*16+4,0xA5));ctrl(f,output,6);
    require(runtime_thread_uid()==static_cast<int>(child)&&ctx.pc==child_entry&&f.time.deadlines.empty()&&memory.load32(output)==0xA5A5A5A5,
        "Blocking controller read slept or wrote data instead of running a ready worker");
    f.time.now+=input_period-1;ctx.gpr[31]=child_pc;f.call(0x293B45B8);
    require(runtime_thread_uid()==static_cast<int>(child)&&memory.load32(output)==0xA5A5A5A5,"Controller waiter woke before a genuine sample");
    ++f.time.now;state.buttons=8;ctx.gpr[31]=child_pc;f.call(0x293B45B8);
    require(runtime_thread_uid()==1&&ctx.pc==root_pc&&ctx.gpr[2]==1&&ctx.gpr[29]==sp&&ctx.gpr[28]==gp&&
        ctx.gpr[16]==0x12345678&&ctx.hi==0xABCD&&ctx.lo==0xFEDC&&ctx.fpr[3]==0x1234&&ctx.vfpu[5]==0x5678,
        "Controller wait lost PSP return/context/register state");
    require(memory.load32(output)==16683&&memory.load32(output+4)==8,"Controller waiter got stale/fabricated input");
    for(unsigned i=16;i<6*16+4;++i)require(memory.load8(output+i)==0xA5,"Partial controller wake changed unused output tail");
    ctx.gpr[31]=root_pc;f.call(delay,{1000});
    require(runtime_thread_uid()==static_cast<int>(child)&&ctx.pc==child_pc&&ctx.gpr[2]==child,
        "Controller preemption lost the other PSP worker's completed HLE continuation");
}
void idle_input_wait_uses_real_sampling_deadline(){
    Fixture f;auto &ctx=f.runtime.cpu();auto &memory=f.runtime.memory();
    const auto origin=f.time.now;
    f.host.set_input_provider([&]{return badman::InputState{true,f.time.now>=origin+input_period?0x2000u:0x4000u,128,128};});
    require(ctrl(f,output)==1,"Initial sole-thread input missing");
    memory.copy_in(output,std::vector<std::uint8_t>(64*16+4,0xA5));ctx.gpr[31]=root_pc;ctrl(f,output,64);
    require(runtime_thread_uid()==1&&ctx.pc==root_pc&&ctx.gpr[2]==1&&f.time.now==origin+input_period&&
        !f.time.deadlines.empty()&&f.time.deadlines.back()==origin+input_period&&memory.load32(output+4)==0x2000,
        "Idle controller wait did not resume at one genuine sampling deadline");
    for(unsigned i=16;i<64*16+4;++i)require(memory.load8(output+i)==0xA5,"Idle read backfilled requested future samples");
}
void input_failure_masking_and_restart_preserve_waits(){
    Fixture f;auto &ctx=f.runtime.cpu();auto &memory=f.runtime.memory();unsigned calls=0;
    badman::InputState state{true,1,128,128};f.host.set_input_provider([&]{++calls;return state;});
    require(ctrl(f,output)==1,"Initial input unavailable");const auto observations=calls;
    for(auto count:{0u,65u,UINT32_MAX})require(ctrl(f,output,count)==0x80000104u,"Invalid HLE input count accepted");
    rejects([&]{ctrl(f,output+1);},"Invalid HLE controller buffer accepted");
    require(calls==observations,"Invalid controller parameters observed provider input");
    f.runtime.invoke_import("Kernel_Library",0x092968F4,ctx);
    require(ctrl(f,output)==0x800201A7u&&runtime_thread_uid()==1&&f.time.deadlines.empty(),"Interrupt-masked empty read blocked");
    ctx.gpr[4]=1;f.runtime.invoke_import("Kernel_Library",0x5F10D406,ctx);
    const auto child=f.create_child(16);ctx.gpr[31]=root_pc;f.call(0xF475845D,{child,0,0});
    require(runtime_thread_uid()==static_cast<int>(child),"Controller fixture did not start real child");
    ctx.gpr[31]=child_pc;ctrl(f,output);
    require(runtime_thread_uid()==1&&f.call(0xF475845D,{child,0,0})==0x800201A4,"Starting an input waiter reset or woke it");
    memory.store32(output,0xA5A5A5A5);state.ready=false;f.time.now+=input_period;ctx.gpr[31]=root_pc;
    rejects([&]{f.call(0x293B45B8);},"Provider loss while waiting manufactured input");
    require(runtime_thread_uid()==1&&memory.load32(output)==0xA5A5A5A5,"Provider failure changed pending input output/ownership");
    state.ready=true;state.buttons=0x200;ctx.gpr[31]=root_pc;f.call(0x293B45B8);
    require(runtime_thread_uid()==static_cast<int>(child)&&ctx.pc==child_pc&&ctx.gpr[2]==1&&memory.load32(output+4)==0x200,
        "Provider failure discarded the pending genuine controller wait");
    f.call(0xAA73C935,{0});require(runtime_thread_uid()==1,"Input worker exit did not restore root");
    memory.store32(output,0xABCDEF01);ctx.gpr[31]=root_pc;f.call(0xF475845D,{child,0,0});
    const auto second=output+64;ctx.gpr[31]=child_pc;ctrl(f,second);
    require(runtime_thread_uid()==1,"Restarted child retained a completed controller wait");
    f.time.now+=input_period;state.buttons=0x1000;ctx.gpr[31]=root_pc;f.call(0x293B45B8);
    require(runtime_thread_uid()==static_cast<int>(child)&&memory.load32(output)==0xABCDEF01&&memory.load32(second+4)==0x1000,
        "Thread restart replayed a previous controller output pointer or packet");
    f.call(0xAA73C935,{0});require(f.call(remove_thread,{child})==0,"Completed input worker retained owned resources");
}
void input_waiters_receive_distinct_fifo_observations(){
    Fixture f;auto &ctx=f.runtime.cpu();auto &memory=f.runtime.memory();badman::InputState state{true,1,128,128};
    f.host.set_input_provider([&]{return state;});require(ctrl(f,output)==1,"Initial FIFO sample missing");
    const auto second=f.ready_child(40),worker=f.ready_child(48);
    const auto second_output=output+64;memory.store32(output,0xA5A5A5A5);memory.store32(second_output,0xA5A5A5A5);
    ctx.gpr[31]=root_pc;ctrl(f,output);require(runtime_thread_uid()==static_cast<int>(second),"First blocked reader did not yield");
    ctx.gpr[31]=child_pc;ctrl(f,second_output);require(runtime_thread_uid()==static_cast<int>(worker),"Second blocked reader stranded ready worker");
    f.call(priority,{second,16});f.time.now+=input_period;state.buttons=8;ctx.gpr[31]=child_pc;f.call(0x293B45B8);
    require(runtime_thread_uid()==1&&memory.load32(output+4)==8&&memory.load32(second_output)==0xA5A5A5A5,
        "One genuine observation was duplicated or assigned past an older reader");
    ctx.gpr[31]=root_pc;f.call(delay,{100000});require(runtime_thread_uid()==static_cast<int>(worker),"FIFO test lost its ready worker");
    f.time.now+=input_period;state.buttons=0x20;ctx.gpr[31]=child_pc;f.call(0x293B45B8);
    require(runtime_thread_uid()==static_cast<int>(second)&&memory.load32(second_output+4)==0x20&&
        memory.load32(second_output)>memory.load32(output),"Second input waiter did not receive a distinct later observation");
}
void buffered_input_wakes_with_a_clock_advancing_each_query(){
    Fixture f(false);auto &ctx=f.runtime.cpu();auto &memory=f.runtime.memory();
    std::uint64_t ticks=1000000000;std::vector<std::uint64_t> waits;
    f.host.set_scheduler_timing([&]{return ++ticks;},[&](std::uint64_t deadline){waits.push_back(deadline);ticks=deadline;});
    f.host.bootstrap(root_entry,"advancing input clock");badman::InputState state{true,1,128,128};
    f.host.set_input_provider([&]{return state;});require(ctrl(f,output)==1,"Initial advancing-clock input missing");
    const auto child=f.ready_child(40);ctx.gpr[31]=root_pc;ctrl(f,output);
    require(runtime_thread_uid()==static_cast<int>(child)&&waits.empty(),"Advancing-clock reader did not yield to ready child");
    ticks+=input_period;state.buttons=0x8000;
    // A real presenter poll buffers the observation before timer wakeup. Every
    // clock call now returns a strictly later value, exposing reversed reads.
    ctx.gpr[4]=GuestMemory::kVramPhysicalBase;ctx.gpr[5]=512;ctx.gpr[6]=3;ctx.gpr[7]=0;ctx.gpr[31]=child_pc;
    f.runtime.invoke_import("sceDisplay",0x289D82FE,ctx);
    require(runtime_thread_uid()==1&&ctx.pc==root_pc&&ctx.gpr[2]==1&&memory.load32(output+4)==0x8000&&waits.empty(),
        "A buffered actual controller observation was stranded by clock query ordering");
    std::ostringstream dump;f.host.dump_scheduler(dump);
    require(dump.str().find("wait_input=")==std::string::npos,"Completed advancing-clock input left a stale typed wait");
}
}
int main() {
    try {
        delay_runs_ready_workers_and_preempts_at_deadline(); idle_scheduler_waits_for_earliest_timer();
        zero_delay_callbacks_and_clock_failures(); priority_changes_preserve_import_flow();
        same_thread_redirect_preserves_callback_entry(); deletion_reclaims_only_dormant_threads();
        exited_thread_restarts_with_clean_context_and_owned_resources();
        start_errors_preserve_dormant_state_and_null_arguments();waiting_thread_start_is_refused();
        event_status_layout_and_bounded_prefixes();
        input_wait_yields_ready_worker_and_restores_full_continuation();idle_input_wait_uses_real_sampling_deadline();
        input_failure_masking_and_restart_preserve_waits();input_waiters_receive_distinct_fifo_observations();
        buffered_input_wakes_with_a_clock_advancing_each_query();
        std::cout << "Badman scheduler deadlines, worker preemption, continuations, priorities, deletion and event-status tests passed\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}

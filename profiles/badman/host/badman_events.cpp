#include "badman_host.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <limits>

namespace badman {
namespace {
constexpr std::uint32_t unknown=0x8002019Au, illegal_mode=0x80020195u;
constexpr std::uint32_t illegal_pattern=0x800201B1u, multiple=0x800201B0u;
constexpr std::uint32_t condition=0x800201AFu, timeout=0x800201A8u;
bool matches(std::uint32_t bits,std::uint32_t wanted,std::uint32_t mode) {
    return mode&1u ? (bits&wanted)!=0 : (bits&wanted)==wanted;
}
void validate_output(const psprecomp::GuestMemory &memory,std::uint32_t address) {
    if(address&&((address&3u)||!memory.contains(address,4)))
        throw psprecomp::Error("Invalid event-flag output pointer");
}
}

std::size_t Host::count_event_waiters(std::int32_t event) const {
    return std::count_if(threads_.begin(),threads_.end(),[&](const auto &entry) {
        const auto &thread=entry.second;
        return thread.state==State::waiting&&thread.wait_event&&thread.wait_event->event==event;
    });
}
std::optional<std::uint64_t> Host::next_event_deadline() const {
    std::optional<std::uint64_t> result;
    for(const auto &[uid,thread]:threads_) {
        (void)uid;
        if(thread.state==State::waiting&&thread.wait_event&&thread.wait_event->deadline&&
           (!result||*thread.wait_event->deadline<*result))result=thread.wait_event->deadline;
    }
    return result;
}
void Host::complete_event_wait(Thread &thread,std::uint32_t result,std::uint64_t now,
                               std::optional<std::uint32_t> pattern) {
    const auto request=*thread.wait_event;
    auto &memory=runtime_.memory();
    validate_output(memory,request.output);validate_output(memory,request.timeout_output);
    if(pattern&&request.output)memory.store32(request.output,*pattern);
    if(request.timeout_output&&request.deadline) {
        const auto remaining=now<*request.deadline ? (*request.deadline-now)/1000u : 0;
        memory.store32(request.timeout_output,static_cast<std::uint32_t>(remaining));
    }
    if(const auto event=events_.find(request.event);event!=events_.end())
        std::erase(event->second.waiters,thread.uid);
    thread.wait_event.reset();thread.context.gpr[2]=result;thread.state=State::ready;
}
void Host::wake_expired_event_waits(std::uint64_t now) {
    for(auto &[uid,thread]:threads_) {
        (void)uid;
        if(thread.state==State::waiting&&thread.wait_event&&thread.wait_event->deadline&&now>=*thread.wait_event->deadline)
            complete_event_wait(thread,timeout,now);
    }
}
void Host::install_events() {
    bind("ThreadManForUser",0x55C20A00u,"sceKernelCreateEventFlag",[this](Context &ctx) {
        if(!ctx.gpr[4]){ctx.gpr[2]=0x80020001u;return;}
        const auto attributes=ctx.gpr[5];
        if(attributes&~0x2FFu){ctx.gpr[2]=0x80020191u;return;}
        // Only observed attrs0/200 are in this profile's supported contract.
        // Some low attribute bits are valid on hardware; do not mislabel those.
        if(attributes!=0&&attributes!=0x200u)throw psprecomp::Error("Unobserved event-flag attributes are not implemented");
        if(ctx.gpr[7])throw psprecomp::Error("Non-null event-flag create options are not implemented");
        Event event;event.initial=event.bits=ctx.gpr[6];event.attributes=attributes;
        for(std::uint32_t index=0;index<31;++index) {
            const auto character=runtime_.memory().load8(ctx.gpr[4]+index);
            if(!character)break;
            event.name+=static_cast<char>(character);
        }
        if(next_uid_<=0||next_uid_==std::numeric_limits<std::int32_t>::max())
            throw psprecomp::Error("Badman UID space exhausted");
        const auto uid=next_uid_;
        events_.emplace(uid,std::move(event));++next_uid_;ctx.gpr[2]=static_cast<std::uint32_t>(uid);
    });
    bind("ThreadManForUser",0x1FB15A32u,"sceKernelSetEventFlag",[this](Context &ctx) {
        const auto found=events_.find(static_cast<std::int32_t>(ctx.gpr[4]));
        if(found==events_.end()){ctx.gpr[2]=unknown;return;}
        const auto now=scheduler_now();
        // Expired waits cannot consume a subsequently issued Set. The shared
        // timer engine also calls this at cooperative scheduling boundaries.
        wake_expired_event_waits(now);
        auto &event=found->second;
        for(const auto uid:event.waiters) {
            const auto thread=threads_.find(uid);
            if(thread!=threads_.end()&&thread->second.wait_event) {
                validate_output(runtime_.memory(),thread->second.wait_event->output);
                validate_output(runtime_.memory(),thread->second.wait_event->timeout_output);
            }
        }
        event.bits|=ctx.gpr[5];
        const auto ordered=event.waiters;
        for(const auto uid:ordered) {
            const auto found_thread=threads_.find(uid);
            if(found_thread==threads_.end())continue;
            auto &thread=found_thread->second;
            if(thread.state!=State::waiting||!thread.wait_event||thread.wait_event->event!=found->first)continue;
            const auto request=*thread.wait_event;
            if(!matches(event.bits,request.wanted,request.mode))continue;
            const auto pattern=event.bits;
            if(request.mode&0x10u)event.bits=0;
            if(request.mode&0x20u)event.bits&=~request.wanted;
            complete_event_wait(thread,0,now,pattern);
        }
        ctx.gpr[2]=0;
    });
    bind("ThreadManForUser",0x812346E4u,"sceKernelClearEventFlag",[this](Context &ctx) {
        const auto found=events_.find(static_cast<std::int32_t>(ctx.gpr[4]));
        if(found==events_.end()){ctx.gpr[2]=unknown;return;}
        found->second.bits&=ctx.gpr[5];ctx.gpr[2]=0; // Retain mask, not inverted mask.
    });
    bind("ThreadManForUser",0xEF9E4C70u,"sceKernelDeleteEventFlag",[this](Context &ctx) {
        const auto found=events_.find(static_cast<std::int32_t>(ctx.gpr[4]));
        if(found==events_.end()){ctx.gpr[2]=unknown;return;}
        const auto now=scheduler_now();
        wake_expired_event_waits(now);
        const auto ordered=found->second.waiters;
        for(const auto uid:ordered) {
            auto thread=threads_.find(uid);
            if(thread!=threads_.end()&&thread->second.wait_event) {
                validate_output(runtime_.memory(),thread->second.wait_event->output);
                validate_output(runtime_.memory(),thread->second.wait_event->timeout_output);
            }
        }
        for(const auto uid:ordered) {
            auto thread=threads_.find(uid);
            if(thread!=threads_.end()&&thread->second.state==State::waiting&&thread->second.wait_event&&thread->second.wait_event->event==found->first)
                complete_event_wait(thread->second,0x800201B5u,now);
        }
        events_.erase(found);ctx.gpr[2]=0;
    });
    const auto wait=[this](Context &ctx,bool poll) {
        const auto wanted=ctx.gpr[5],mode=ctx.gpr[6];
        if((mode&~0x31u)||(mode&0x30u)==0x30u){ctx.gpr[2]=illegal_mode;return;}
        if(!wanted){ctx.gpr[2]=illegal_pattern;return;}
        const auto found=events_.find(static_cast<std::int32_t>(ctx.gpr[4]));
        if(found==events_.end()){ctx.gpr[2]=unknown;return;}
        auto &event=found->second;
        const auto now=scheduler_now();wake_expired_event_waits(now);
        if(!(event.attributes&0x200u)&&count_event_waiters(found->first)){ctx.gpr[2]=multiple;return;}
        auto &memory=runtime_.memory();
        const auto output=ctx.gpr[7],timeout_output=poll?0:ctx.gpr[8];
        validate_output(memory,output);validate_output(memory,timeout_output);
        if(!poll) {
            if(in_interrupt_context()){ctx.gpr[2]=0x80020064u;return;}
            if(!interrupts_enabled_){ctx.gpr[2]=0x800201A7u;return;}
        }
        const bool ready=matches(event.bits,wanted,mode);
        std::optional<std::uint64_t> deadline;
        if(!poll&&!ready&&timeout_output) {
            const auto duration=static_cast<std::uint64_t>(memory.load32(timeout_output))*1000u;
            if(!duration){ctx.gpr[2]=timeout;return;}
            if(now>std::numeric_limits<std::uint64_t>::max()-duration)throw psprecomp::Error("Event wait deadline overflow");
            deadline=now+duration;
        }
        if(output)memory.store32(output,event.bits);
        if(ready) {
            if(mode&0x10u)event.bits=0;
            if(mode&0x20u)event.bits&=~wanted;
            ctx.gpr[2]=0;return;
        }
        if(poll){ctx.gpr[2]=condition;return;}
        auto &thread=current();
        event.waiters.push_back(thread.uid);
        thread.wait_event=EventWait{found->first,wanted,mode,output,timeout_output,deadline};
        thread.delay_deadline.reset();thread.wait_vblank=false;
        thread.wait_thread=0;thread.wait_semaphore=0;thread.wait_semaphore_count=0;
        thread.context=ctx;thread.context.pc=ctx.gpr[31];thread.state=State::waiting;
        schedule(ctx);
    };
    bind("ThreadManForUser",0x402FCF22u,"sceKernelWaitEventFlag",[wait](Context &ctx){wait(ctx,false);});
    bind("ThreadManForUser",0x30FD48F0u,"sceKernelPollEventFlag",[wait](Context &ctx){wait(ctx,true);});
}
}

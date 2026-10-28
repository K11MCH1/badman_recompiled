#include "badman_host.hpp"
#include "badman_clock.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <limits>
#include <thread>

namespace badman {
using psprecomp::Error;
void Host::set_scheduler_timing(SchedulerClock clock, SchedulerWait wait) {
    if (!threads_.empty()) throw Error("Scheduler timing must be configured before bootstrap");
    if (static_cast<bool>(clock) != static_cast<bool>(wait)) throw Error("Scheduler clock and waiter must be configured together");
    if (!clock) {
        clock = native_clock_now;
        wait = make_native_waiter();
    }
    input_.set_timing(clock, wait);
    const auto now = clock();
    scheduler_clock_ = std::move(clock); scheduler_wait_ = std::move(wait); scheduler_last_now_ = now;
    display_origin_ns_ = now; display_tick_ = 0; display_.vblanks = 0;
}
std::uint64_t Host::scheduler_now() {
    if (!scheduler_clock_) throw Error("Scheduler timing was not initialized");
    const auto now = scheduler_clock_();
    if (now < scheduler_last_now_) throw Error("Scheduler clock moved backwards");
    scheduler_last_now_ = now; return now;
}
void Host::wake_expired_delays() {
    if (!scheduler_clock_) return;
    const auto now = scheduler_now();
    for (auto &[uid, thread] : threads_) {
        (void)uid;
        if (thread.state == State::waiting && thread.delay_deadline && now >= *thread.delay_deadline) {
            thread.delay_deadline.reset(); thread.context.gpr[2] = 0; thread.state = State::ready;
        }
    }
    wake_expired_event_waits(now);
    poll_audio_waits();
    poll_input_waits();
}
std::optional<std::uint64_t> Host::next_input_deadline() const {
    if(std::none_of(threads_.begin(),threads_.end(),[](const auto &entry){
        return entry.second.state==State::waiting&&entry.second.wait_input;
    }))return {};
    return input_.next_observation_deadline();
}
void Host::poll_input_waits() {
    for(;;){
        Thread *oldest=nullptr;
        std::int32_t oldest_uid=0;
        for(auto &[uid,thread]:threads_){
            if(thread.state==State::waiting&&thread.wait_input&&
                (!oldest||thread.wait_input->order<oldest->wait_input->order)){oldest=&thread;oldest_uid=uid;}
        }
        if(!oldest)return;
        // Sequence the deadline query before the current time. Buffered/first
        // observations additionally use an explicit0 immediate deadline, so
        // their eligibility cannot move forward as the host clock advances.
        const auto deadline=input_.next_observation_deadline();
        if(scheduler_now()<deadline)return;
        const auto waiting=*oldest->wait_input;
        const auto result=input_.try_read_positive(runtime_.memory(),waiting.address,waiting.count,
            controller_filter(oldest_uid,waiting.return_pc));
        if(!result)return;
        oldest->context.gpr[2]=static_cast<std::uint32_t>(*result);oldest->wait_input.reset();oldest->state=State::ready;
    }
}
bool Host::wait_for_next_delay(Context &ctx) {
    while (!runtime_.stopped()) {
        if (std::none_of(threads_.begin(), threads_.end(), [](const auto &entry) {
            return entry.second.state == State::waiting || entry.second.state == State::ready;
        })) return false;
        service_display_timing(ctx); wake_expired_delays();
        std::optional<std::uint64_t> deadline = next_display_deadline();
        const auto event_deadline = next_event_deadline();
        if (event_deadline && (!deadline || *event_deadline < *deadline)) deadline = event_deadline;
        const auto audio_deadline = next_audio_poll_deadline();
        if (audio_deadline && (!deadline || *audio_deadline < *deadline)) deadline = audio_deadline;
        const auto input_deadline = next_input_deadline();
        if (input_deadline && (!deadline || *input_deadline < *deadline)) deadline = input_deadline;
        for (const auto &[uid, thread] : threads_) {
            (void)uid;
            if (thread.state == State::ready) return true;
            if (thread.state == State::waiting && thread.delay_deadline && (!deadline || *thread.delay_deadline < *deadline))
                deadline = thread.delay_deadline;
        }
        if (!deadline) return false;
        const auto now = scheduler_now();
        if (now >= *deadline) continue;
        // Keep native window-close handling responsive during an otherwise idle
        // PSP. No guest worker is ready here, so this sleep cannot strand one.
        const auto slice = now + std::min<std::uint64_t>(*deadline - now, 10000000);
        scheduler_wait_(slice);
        if (scheduler_now() < slice) throw Error("Scheduler waiter returned before its deadline");
        if (window_) window_->pump();
        if (runtime_.stopped()) return false;
        input_.poll();
    }
    return false;
}
void Host::poll_thread_timers(Context &ctx, std::uint32_t continuation) {
    service_display_timing(ctx);
    wake_expired_delays();
    if (!interrupts_enabled_ || in_interrupt_context()) return;
    const auto found = threads_.find(psprecomp::runtime_thread_uid());
    if (found == threads_.end() || found->second.state != State::running) return;
    auto &thread = found->second;
    if (std::any_of(threads_.begin(), threads_.end(), [&](const auto &entry) {
        return entry.second.state == State::ready && entry.second.priority < thread.priority;
    })) {
        thread.context = ctx; thread.context.pc = continuation; thread.state = State::ready;
        schedule(ctx);
    }
}
void Host::install_thread_services() {
    if (!scheduler_clock_) set_scheduler_timing({}, {});
    const auto delay = [this](Context &ctx) {
        if (in_interrupt_context()) { ctx.gpr[2] = 0x80020064u; return; }
        if (!interrupts_enabled_) { ctx.gpr[2] = 0x800201A7u; return; }
        const auto now = scheduler_now(), duration = static_cast<std::uint64_t>(ctx.gpr[4]) * 1000;
        if (now > std::numeric_limits<std::uint64_t>::max() - duration) throw Error("Thread delay deadline overflow");
        auto &thread = current();
        thread.context = ctx; thread.context.pc = ctx.gpr[31];
        thread.wait_thread = 0; thread.wait_semaphore = 0; thread.wait_semaphore_count = 0;
        thread.wait_event.reset(); thread.wait_vblank = false;
        thread.delay_deadline = now + duration; thread.state = State::waiting;
        schedule(ctx);
    };
    bind("ThreadManForUser", 0xCEADEB47u, "sceKernelDelayThread", delay);
    bind("ThreadManForUser", 0x68DA9E36u, "sceKernelDelayThreadCB", [this, delay](Context &ctx) {
        // No notification producer currently exists. Registered callbacks alone
        // are not pending callbacks; a future notifier must populate pending.
        if (std::any_of(callbacks_.begin(), callbacks_.end(), [](const auto &entry) {
            return entry.second.owner == psprecomp::runtime_thread_uid() && entry.second.pending != 0;
        })) throw Error("DelayThreadCB pending-callback dispatch is not implemented");
        delay(ctx);
    });
    bind("ThreadManForUser", 0x71BC9871u, "sceKernelChangeThreadPriority", [this](Context &ctx) {
        const auto uid = ctx.gpr[4] ? static_cast<std::int32_t>(ctx.gpr[4]) : psprecomp::runtime_thread_uid();
        const auto found = threads_.find(uid);
        if (found == threads_.end()) { ctx.gpr[2] = 0x80020198u; return; }
        if (found->second.state == State::dormant || found->second.state == State::ended) { ctx.gpr[2] = 0x800201A2u; return; }
        const auto priority = ctx.gpr[5] ? ctx.gpr[5] : current().priority;
        if (priority < 8 || priority > 0x77) { ctx.gpr[2] = 0x80020193u; return; }
        found->second.priority = priority; ctx.gpr[2] = 0;
        // Host::bind's post-handler timer/priority check handles preemption only
        // after this import has completed in its original execution context.
    });
    bind("ThreadManForUser", 0x9FA03CD3u, "sceKernelDeleteThread", [this](Context &ctx) {
        if (!ctx.gpr[4]) { ctx.gpr[2] = 0x80020197u; return; }
        const auto found = threads_.find(static_cast<std::int32_t>(ctx.gpr[4]));
        if (found == threads_.end()) { ctx.gpr[2] = 0x80020198u; return; }
        const auto &thread = found->second;
        if (thread.state != State::dormant && thread.state != State::ended) { ctx.gpr[2] = 0x800201A4u; return; }
        if ((thread.stack && !allocations_.contains(thread.stack)) || (thread.tls && !allocations_.contains(thread.tls)))
            throw Error("Thread deletion encountered an unowned allocation");
        if ((thread.attributes & 0x00200000u) && thread.stack) runtime_.memory().zero(thread.stack, thread.stack_size);
        if (thread.stack) release(thread.stack);
        if (thread.tls) release(thread.tls);
        for (auto &[uid, waiter] : threads_) {
            (void)uid;
            if (waiter.state == State::waiting && waiter.wait_thread == thread.uid) {
                waiter.context.gpr[2] = 0x800201B5u; waiter.wait_thread = 0; waiter.state = State::ready;
            }
        }
        for (auto callback = callbacks_.begin(); callback != callbacks_.end();) {
            if (callback->second.owner == thread.uid) {
                if (exit_callback_ == callback->first) exit_callback_ = 0;
                callback = callbacks_.erase(callback);
            } else ++callback;
        }
        threads_.erase(found); ctx.gpr[2] = 0;
    });
    bind("ThreadManForUser", 0xA66B0120u, "sceKernelReferEventFlagStatus", [this](Context &ctx) {
        const auto found = events_.find(static_cast<std::int32_t>(ctx.gpr[4]));
        if (found == events_.end()) { ctx.gpr[2] = 0x8002019Au; return; }
        const auto address = ctx.gpr[5]; auto &memory = runtime_.memory();
        if ((address & 3u) || !memory.contains(address, 4)) throw Error("Invalid event-flag status output");
        const auto requested = memory.load32(address), copied = std::min<std::uint32_t>(requested, 52);
        if (!memory.contains(address, copied)) throw Error("Truncated event-flag status output");
        if (!copied) { ctx.gpr[2] = 0; return; }
        std::array<std::uint8_t, 52> packet{};
        const auto word = [&](unsigned offset, std::uint32_t value) {
            for (unsigned byte = 0; byte < 4; ++byte) packet[offset + byte] = static_cast<std::uint8_t>(value >> (byte * 8));
        };
        word(0, 52);
        const auto &event = found->second;
        std::copy_n(event.name.begin(), std::min<std::size_t>(event.name.size(), 31), packet.begin() + 4);
        word(36, event.attributes); word(40, event.initial); word(44, event.bits);
        // Count only coherent live registrations in the event scheduler.
        word(48, static_cast<std::uint32_t>(count_event_waiters(found->first)));
        memory.copy_in(address, std::span<const std::uint8_t>(packet.data(), copied)); ctx.gpr[2] = 0;
    });
}
}

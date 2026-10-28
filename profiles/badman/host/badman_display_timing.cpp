#include "badman_host.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <limits>
#include <ostream>

namespace badman {
using namespace psprecomp;
std::optional<std::uint64_t> Host::next_display_deadline() const {
    const bool needed = std::any_of(threads_.begin(), threads_.end(), [](const auto &entry) {
        return entry.second.state == State::waiting && entry.second.wait_vblank;
    }) || std::any_of(display_interrupts_.begin(), display_interrupts_.end(), [](const auto &entry) {
        return entry.enabled && entry.function;
    });
    if (!needed) return std::nullopt;
    if (display_tick_ >= (std::numeric_limits<std::uint64_t>::max() - display_origin_ns_) / display_period_ns)
        throw Error("Display deadline overflow");
    return display_origin_ns_ + (display_tick_ + 1) * display_period_ns;
}
void Host::invoke_display_interrupt(Context &interrupted, std::uint32_t index) {
    if (in_interrupt_context()) throw Error("Nested display interrupt delivery is not implemented");
    const auto handler = display_interrupts_.at(index);
    if (!handler.function || !handler.enabled) return;
    if (!runtime_.has_function(handler.function)) throw Error("Missing translated vblank handler " + hex32(handler.function));
    if (!display_callback_stack_) display_callback_stack_ = allocate(16384);
    Context callback = interrupted;
    callback.pc = handler.function; callback.gpr[4] = index; callback.gpr[5] = handler.argument;
    callback.gpr[28] = handler.gp; callback.gpr[29] = display_callback_stack_ + 16384 - 256;
    constexpr std::uint32_t returned = 0x0800000Cu;
    callback.gpr[31] = returned;
    const auto token = capture_runtime_execution_context(); const auto enabled = interrupts_enabled_;
    in_display_callback_ = true; interrupts_enabled_ = false;
    try {
        unsigned budget = 0;
        while (callback.pc != returned && !runtime_.stopped() && budget++ < 10000) {
            if (!runtime_.invoke_isolated_aot(callback.pc, callback))
                throw Error("Missing translated vblank callback PC " + hex32(callback.pc));
            if (!runtime_execution_context_matches(token)) throw Error("Vblank interrupt switched PSP thread ownership");
        }
        if (callback.pc != returned || runtime_.stopped()) throw Error("Vblank callback did not complete within its dispatch budget");
    } catch (...) { in_display_callback_ = false; interrupts_enabled_ = enabled; throw; }
    in_display_callback_ = false; interrupts_enabled_ = enabled;
    ++display_.interrupt_callbacks;
    trace_ << "[vblank-callback] index=" << index << " function=" << hex32(handler.function)
           << " argument=" << hex32(handler.argument) << " vcount=" << display_.vblanks << " owner=" << runtime_thread_uid() << '\n';
}
void Host::service_display_timing(Context &ctx) {
    if (!scheduler_clock_ || in_interrupt_context()) return;
    const auto now = scheduler_now();
    if (now < display_origin_ns_) throw Error("Display clock moved backwards");
    const auto tick = (now - display_origin_ns_) / display_period_ns;
    if (tick > display_tick_) {
        // The physical counter advances through missed edges, while the IRQ
        // line latches a single pending notification until it can be serviced.
        display_tick_ = tick; display_.vblanks = static_cast<std::uint32_t>(tick);
        const bool submitted = framebuffer_pending_;
        if (framebuffer_pending_) { display_.current = pending_framebuffer_; framebuffer_pending_ = false; }
        for (auto &[uid, thread] : threads_) {
            (void)uid;
            if (thread.state == State::waiting && thread.wait_vblank) {
                thread.wait_vblank = false; thread.context.gpr[2] = 0; thread.state = State::ready;
            }
        }
        for (auto &handler : display_interrupts_) if (handler.enabled && handler.function) handler.pending = true;
        latch_lateness_ns_ = now - (display_origin_ns_ + tick * display_period_ns); // How late this edge is serviced.
        present_display(submitted);
    }
    if (!interrupts_enabled_ || runtime_.stopped()) return;
    if (std::none_of(display_interrupts_.begin(), display_interrupts_.end(), [](const auto &handler) {
        return handler.pending && handler.enabled && handler.function;
    })) return;
    const auto previous_uid = runtime_thread_uid();
    const std::string previous_name = runtime_thread_name(); // The getter's buffer changes on identity switches.
    const auto interrupted = threads_.find(previous_uid);
    const bool borrow = interrupted == threads_.end() || interrupted->second.state == State::ended || interrupted->second.state == State::dormant;
    Context idle_context;
    if (borrow) {
        // The final runnable worker may have exited while other PSP tasks wait
        // for this IRQ. Seed its isolated stack from a deterministic live task;
        // never resurrect the exited worker or mark the borrowed task running.
        Thread *owner = nullptr;
        for (auto &[uid, thread] : threads_) {
            (void)uid;
            if ((thread.state == State::waiting || thread.state == State::ready) &&
                (!owner || thread.priority < owner->priority)) owner = &thread;
        }
        if (!owner) return; // All-ended still terminates; no fictitious idle task.
        idle_context = owner->context;
        set_runtime_thread_identity(owner->uid, owner->name);
    }
    try {
        for (std::uint32_t index = 0; index < display_interrupts_.size(); ++index) {
            auto &handler = display_interrupts_[index];
            if (!handler.pending || !handler.enabled || !handler.function) continue;
            handler.pending = false;
            try { invoke_display_interrupt(borrow ? idle_context : ctx, index); }
            catch (...) { if (handler.function) handler.pending = true; throw; }
        }
    } catch (...) {
        if (borrow) set_runtime_thread_identity(previous_uid, previous_name);
        throw;
    }
    // Keep genuine Signal/SetEventFlag wakeups to the borrowed task's saved
    // context. Only restore identity, never its state/context or generation.
    if (borrow) set_runtime_thread_identity(previous_uid, previous_name);
}
void Host::install_display_timing() {
    const auto valid = [](Context &ctx) {
        if (ctx.gpr[4] >= 67 || ctx.gpr[5] >= 16) { ctx.gpr[2] = 0x80020065u; return false; }
        if (ctx.gpr[4] != 30) throw Error("Only vblank subinterrupt registration is implemented");
        return true;
    };
    bind("InterruptManager", 0xCA04A2B9u, "sceKernelRegisterSubIntrHandler", [this, valid](Context &ctx) {
        if (!valid(ctx)) return;
        auto &handler = display_interrupts_[ctx.gpr[5]];
        if (handler.function) { ctx.gpr[2] = 0x80020067u; return; }
        if ((ctx.gpr[6] & 3u) || (ctx.gpr[6] && !runtime_.memory().contains(ctx.gpr[6], 4)))
            throw Error("Invalid vblank subinterrupt handler address");
        handler.function = ctx.gpr[6]; handler.argument = ctx.gpr[7]; handler.gp = ctx.gpr[28]; handler.pending = false;
        ctx.gpr[2] = 0;
    });
    bind("InterruptManager", 0xFB8E22ECu, "sceKernelEnableSubIntr", [this, valid](Context &ctx) {
        if (!valid(ctx)) return;
        display_interrupts_[ctx.gpr[5]].enabled = true; ctx.gpr[2] = 0;
    });
    bind("InterruptManager", 0xD61E6961u, "sceKernelReleaseSubIntrHandler", [this, valid](Context &ctx) {
        if (!valid(ctx)) return;
        auto &handler = display_interrupts_[ctx.gpr[5]];
        if (!handler.function) { ctx.gpr[2] = 0x80020068u; return; }
        handler = {}; ctx.gpr[2] = 0;
    });
}
}

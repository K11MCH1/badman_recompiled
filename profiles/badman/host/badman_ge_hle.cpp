#include "badman_host.hpp"
#include "psprecomp/common.hpp"
#include <ostream>

namespace badman {
using namespace psprecomp;
void Host::invoke_ge_callback(const GeInterrupt &interrupt) {
    if (in_interrupt_context()) throw Error("Nested interrupt delivery is not implemented");
    if (ge_callback_stack_ == 0) ge_callback_stack_ = allocate(16384);
    Context callback = runtime_.cpu();
    callback.pc = interrupt.function;
    callback.gpr[4] = interrupt.token;
    callback.gpr[5] = interrupt.argument;
    callback.gpr[6] = interrupt.next_pc;
    callback.gpr[7] = 0;
    callback.gpr[28] = gp_;
    callback.gpr[29] = ge_callback_stack_ + 16384 - 256;
    constexpr std::uint32_t returned = 0x08000004u;
    callback.gpr[31] = returned;
    trace_ << "[ge-callback] " << (interrupt.signal ? "signal" : "finish")
           << " function=" << hex32(interrupt.function) << " token=" << interrupt.token
           << " argument=" << hex32(interrupt.argument) << " next=" << hex32(interrupt.next_pc) << '\n';
    const auto token = capture_runtime_execution_context();
    const auto enabled = interrupts_enabled_;
    in_ge_callback_ = true; interrupts_enabled_ = false;
    try {
        std::uint32_t budget = 0;
        while (callback.pc != returned && !runtime_.stopped() && budget++ < 10000) {
            if (!runtime_.invoke_isolated_aot(callback.pc, callback))
                throw Error("Missing translated GE callback PC " + hex32(callback.pc));
            if (!runtime_execution_context_matches(token))
                throw Error("GE interrupt unexpectedly switched PSP thread ownership");
        }
        if (callback.pc != returned) throw Error("GE callback did not return within its dispatch budget");
    } catch (...) { in_ge_callback_ = false; interrupts_enabled_ = enabled; throw; }
    in_ge_callback_ = false; interrupts_enabled_ = enabled;
}
void Host::install_ge() {
    bind("sceGe_user", 0xB448EC0Du, "sceGeBreak", [this](Context &ctx) {
        ctx.gpr[2] = static_cast<std::uint32_t>(ge_.break_queue(ctx.gpr[4], ctx.gpr[5]));
    });
    bind("sceGe_user", 0xA4FC06A4u, "sceGeSetCallback", [this](Context &ctx) {
        ctx.gpr[2] = static_cast<std::uint32_t>(ge_.set_callback(ctx.gpr[4]));
    });
    bind("sceGe_user", 0x05DB22CEu, "sceGeUnsetCallback", [this](Context &ctx) {
        ge_.unset_callback(static_cast<std::int32_t>(ctx.gpr[4])); ctx.gpr[2] = 0;
    });
    const auto enqueue = [this](Context &ctx, bool head) {
        trace_ << "[ge-list] enqueue address=" << hex32(ctx.gpr[4]) << " stall=" << hex32(ctx.gpr[5])
               << " callback=" << static_cast<std::int32_t>(ctx.gpr[6]) << " options=" << hex32(ctx.gpr[7]) << '\n';
        ctx.gpr[2] = static_cast<std::uint32_t>(ge_.enqueue(ctx.gpr[4], ctx.gpr[5],
            static_cast<std::int32_t>(ctx.gpr[6]), ctx.gpr[7], head));
    };
    bind("sceGe_user", 0xAB49E76Au, "sceGeListEnQueue", [enqueue](Context &ctx) { enqueue(ctx, false); });
    bind("sceGe_user", 0x1C0D95A6u, "sceGeListEnQueueHead", [enqueue](Context &ctx) { enqueue(ctx, true); });
    bind("sceGe_user", 0xE0D68148u, "sceGeListUpdateStallAddr", [this](Context &ctx) {
        ge_.update_stall(static_cast<std::int32_t>(ctx.gpr[4]), ctx.gpr[5]); ctx.gpr[2] = 0;
    });
    bind("sceGe_user", 0x03444EB4u, "sceGeListSync", [this](Context &ctx) {
        if (ctx.gpr[5] > 1) throw Error("Invalid GE list sync mode");
        ctx.gpr[2] = static_cast<std::uint32_t>(ge_.list_status(static_cast<std::int32_t>(ctx.gpr[4]), ctx.gpr[5] == 0));
    });
    bind("sceGe_user", 0xB287BD61u, "sceGeDrawSync", [this](Context &ctx) {
        if (ctx.gpr[4] > 1) throw Error("Invalid GE draw sync mode");
        ctx.gpr[2] = static_cast<std::uint32_t>(ge_.draw_status(ctx.gpr[4] == 0));
    });
}
}

#include "badman_host.hpp"
#include "psprecomp/common.hpp"
#include <array>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
using namespace psprecomp;
constexpr std::uint64_t period = 16683333;
constexpr std::uint32_t root = 0x08804000, root_pc = 0x08804004;
constexpr std::uint32_t child = 0x08805000, child_pc = 0x08805004, handler = 0x08807000;
constexpr std::uint32_t name = 0x08802000, argument = 0x08802100;
constexpr std::uint32_t reg_irq = 0xCA04A2B9, enable_irq = 0xFB8E22EC, release_irq = 0xD61E6961;
void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
template <class Function> void rejects(Function function, const char *message) {
    bool rejected = false; try { function(); } catch (const Error &) { rejected = true; }
    require(rejected, message);
}
struct Observation { unsigned calls{}; std::int32_t owner{}; std::uint32_t gp{}, interrupted_sp{}, sema{}, event{};
    bool block{}, loop{}, throw_on_irq{}; } observation;
void return_thread(Runtime &, AllegrexContext &ctx) { ctx.pc = badman::Host::thread_return_pc; }
void interrupt(Runtime &runtime, AllegrexContext &ctx) {
    ++observation.calls;
    require(runtime_thread_uid() == observation.owner && ctx.gpr[4] == 0 && ctx.gpr[5] == argument &&
        ctx.gpr[28] == observation.gp && ctx.gpr[29] != observation.interrupted_sp && ctx.gpr[31] == 0x0800000C,
        "Vblank interrupt lost owner identity, ABI arguments, GP, isolated stack or return sentinel");
    if (observation.throw_on_irq) throw Error("Synthetic borrowed interrupt failure");
    runtime.memory().store32(argument, runtime.memory().load32(argument) + 1);
    ctx.gpr[16] = 0xDEADBEEF;
    if (observation.block) {
        ctx.gpr[4] = 1; runtime.invoke_import("ThreadManForUser", 0xCEADEB47, ctx);
        require(ctx.gpr[2] == 0x80020064 && runtime_thread_uid() == observation.owner,
            "Interrupt handler was permitted to block or changed PSP owner");
    }
    if (observation.sema) {
        ctx.gpr[4] = observation.sema; ctx.gpr[5] = 1;
        runtime.invoke_import("ThreadManForUser", 0x3F53E640, ctx);
        require(ctx.gpr[2] == 0 && runtime_thread_uid() == observation.owner,
            "Interrupt signal preempted before the handler returned");
    }
    if (observation.event) {
        ctx.gpr[4] = observation.event; ctx.gpr[5] = 1;
        runtime.invoke_import("ThreadManForUser", 0x1FB15A32, ctx);
        require(ctx.gpr[2] == 0 && runtime_thread_uid() == observation.owner,
            "Interrupt event signal switched away from its borrowed live identity");
    }
    ctx.pc = observation.loop ? handler : ctx.gpr[31];
}
struct Fixture {
    Runtime runtime; std::ostringstream trace;
    std::uint64_t now{1000000000}; std::vector<std::uint64_t> deadlines;
    badman::Host host{runtime, 0x089F0000, 0x089E0000, 0x0891F420, trace};
    Fixture() {
        observation = {};
        runtime.register_function(root, return_thread, "display_test_root");
        runtime.register_function(root_pc, return_thread, "display_test_root_return");
        runtime.register_function(child, return_thread, "display_test_child");
        runtime.register_function(child_pc, return_thread, "display_test_child_return");
        runtime.register_function(handler, interrupt, "display_test_interrupt");
        host.install({});
        host.set_scheduler_timing([&] { return now; }, [&](std::uint64_t deadline) {
            require(deadlines.size() < 100, "Scheduler starved a live waiter across 100 display deadlines");
            deadlines.push_back(deadline); now = deadline;
        });
        host.bootstrap(root, "display timing");
        const std::array<std::uint8_t, 5> text{'t', 'e', 's', 't', 0}; runtime.memory().copy_in(name, text);
        observation.owner = 1; observation.gp = runtime.cpu().gpr[28]; observation.interrupted_sp = runtime.cpu().gpr[29];
    }
    std::uint32_t call(const char *library, std::uint32_t nid, std::initializer_list<std::uint32_t> arguments = {}) {
        auto &ctx = runtime.cpu(); for (unsigned reg = 4; reg < 10; ++reg) ctx.gpr[reg] = 0;
        unsigned reg = 4; for (auto value : arguments) ctx.gpr[reg++] = value;
        runtime.invoke_import(library, nid, ctx); return ctx.gpr[2];
    }
    std::uint32_t thread(std::uint32_t nid, std::initializer_list<std::uint32_t> args = {}) { return call("ThreadManForUser", nid, args); }
    std::uint32_t irq(std::uint32_t nid, std::initializer_list<std::uint32_t> args = {}) { return call("InterruptManager", nid, args); }
    void install_irq() { require(irq(reg_irq, {30, 0, handler, argument}) == 0 && irq(enable_irq, {30, 0}) == 0, "Installing IRQ failed"); }
    std::uint32_t ready_child(std::uint32_t priority = 40) {
        const auto uid = thread(0x446D8DE6, {name, child, priority, 4096, 0, 0}); runtime.cpu().gpr[31] = root_pc;
        thread(0xF475845D, {uid, 0, 0}); return uid;
    }
};
void multi_thread_wait_and_frame_latch() {
    Fixture f; auto &ctx = f.runtime.cpu();
    f.call("sceDisplay", 0x289D82FE, {0x04000000, 512, 3, 0});
    f.call("sceDisplay", 0x289D82FE, {0x04088000, 512, 3, 1});
    const auto worker = f.ready_child(); ctx.gpr[31] = root_pc;
    f.call("sceDisplay", 0x984C27E7);
    require(runtime_thread_uid() == static_cast<std::int32_t>(worker) && f.deadlines.empty() &&
        f.host.display_state().current.address == 0x04000000, "Vblank wait slept despite ready worker or latched the framebuffer early");
    ctx.gpr[31] = child_pc; f.call("sceDisplay", 0x984C27E7);
    require(runtime_thread_uid() == 1 && ctx.pc == root_pc && ctx.gpr[2] == 0 && f.now == 1000000000 + period &&
        f.host.display_state().vblanks == 1 && f.host.display_state().current.address == 0x04088000,
        "Shared vblank edge did not wake waiters and latch the pending framebuffer");
    ctx.gpr[31] = root_pc; f.thread(0xCEADEB47, {1000});
    require(runtime_thread_uid() == static_cast<std::int32_t>(worker) && ctx.pc == child_pc && ctx.gpr[2] == 0,
        "Second vblank waiter lost its successful continuation");
}
void periodic_delivery_and_masked_coalescing() {
    Fixture f; auto &ctx = f.runtime.cpu(); f.install_irq(); observation.block = true;
    ctx.gpr[31] = root_pc; ctx.gpr[16] = 0x12345678;
    const auto sp = ctx.gpr[29], gp = ctx.gpr[28];
    require(f.thread(0xCEADEB47, {50000}) == 0 && observation.calls == 2 && f.host.display_state().interrupt_callbacks == 2 &&
        f.runtime.memory().load32(argument) == 2 && ctx.pc == root_pc && ctx.gpr[16] == 0x12345678 && ctx.gpr[29] == sp && ctx.gpr[28] == gp,
        "Periodic translated IRQ delivery or interrupted context preservation failed");
    f.thread(0xCEADEB47, {80}); require(observation.calls == 3, "Earliest display deadline was missed during a short remaining delay");
    f.call("Kernel_Library", 0x092968F4); f.now += 3 * period; f.thread(0x293B45B8);
    require(observation.calls == 3, "Masked interrupt was delivered before interrupts resumed");
    f.call("Kernel_Library", 0x5F10D406, {1});
    require(observation.calls == 4 && f.host.display_state().interrupt_callbacks == 4,
        "Masked physical edges did not coalesce into one pending IRQ on resume");
    require(f.irq(release_irq, {30, 0}) == 0, "Release of registered handler failed");
    f.thread(0xCEADEB47, {20000}); require(observation.calls == 4, "Released handler continued executing");
}
void interrupt_wakes_worker_after_return() {
    Fixture f; auto &ctx = f.runtime.cpu();
    const auto sema = f.thread(0xD6DA4BA1, {name, 0, 0, 1, 0}); const auto worker = f.ready_child(16);
    ctx.gpr[31] = child_pc; f.thread(0x4E3A1105, {sema, 1, 0});
    require(runtime_thread_uid() == 1, "Semaphore fixture did not block worker");
    f.install_irq(); observation.sema = sema; ctx.gpr[31] = root_pc; ctx.gpr[16] = 0x12345678;
    f.now += period; f.thread(0x293B45B8);
    require(observation.calls == 1 && f.host.display_state().interrupt_callbacks == 1 &&
        runtime_thread_uid() == static_cast<std::int32_t>(worker) && ctx.pc == child_pc && ctx.gpr[2] == 0,
        "IRQ did not finish before scheduling its released higher-priority worker");
    f.thread(0xAA73C935, {0});
    require(runtime_thread_uid() == 1 && ctx.pc == root_pc && ctx.gpr[2] == 1 && ctx.gpr[16] == 0x12345678,
        "Interrupt-triggered preemption lost the interrupted import's result or continuation");
}
void registration_failures_and_incomplete_callbacks() {
    Fixture f;
    require(f.irq(reg_irq, {70, 0, handler, argument}) == 0x80020065 &&
        f.irq(reg_irq, {30, 16, handler, argument}) == 0x80020065 &&
        f.irq(release_irq, {30, 0}) == 0x80020068, "Invalid IRQ index/empty release errors were incorrect");
    require(f.irq(reg_irq, {30, 0, 0, argument}) == 0 && f.irq(reg_irq, {30, 0, 0, argument}) == 0,
        "Observed null-handler registration behavior failed");
    require(f.irq(enable_irq, {30, 0}) == 0 && f.irq(reg_irq, {30, 0, handler, argument}) == 0 &&
        f.irq(reg_irq, {30, 0, handler, argument}) == 0x80020067, "Enable-before-register or duplicate registration failed");
    f.now += period; f.thread(0x293B45B8); require(observation.calls == 1, "Previously enabled handler did not execute after registration");
    f.irq(release_irq, {30, 0}); f.irq(reg_irq, {30, 0, handler, argument}); f.now += period; f.thread(0x293B45B8);
    require(observation.calls == 1, "Release did not disable the slot before re-registration");
    f.irq(release_irq, {30, 0});
    f.irq(reg_irq, {30, 0, 0x08807700, argument}); f.irq(enable_irq, {30, 0}); f.now += period;
    rejects([&] { f.thread(0x293B45B8); }, "Missing translated interrupt body returned fake callback success");
    require(f.host.display_state().interrupt_callbacks == 1, "Failed callback was counted as completed");
    f.irq(release_irq, {30, 0}); f.irq(reg_irq, {30, 0, handler, argument}); f.irq(enable_irq, {30, 0});
    observation.loop = true; f.now += period;
    rejects([&] { f.thread(0x293B45B8); }, "Nonreturning interrupt ignored its dispatch budget");
    require(f.host.display_state().interrupt_callbacks == 1, "Nonreturning callback was counted as completed");
    f.irq(release_irq, {30, 0}); observation.loop = false; f.runtime.cpu().gpr[31] = root_pc;
    require(f.thread(0xCEADEB47, {1}) == 0, "Callback failure left interrupt context or interrupt masking stuck");
}
void enabled_irq_does_not_keep_ended_threads_alive() {
    Fixture f; f.install_irq(); f.now += period;
    f.runtime.run(root, 8);
    require(f.runtime.stop_reason() == "All Badman threads exited" && f.deadlines.empty(),
        "An enabled IRQ kept an ended PSP process in an infinite scheduler idle loop");
}
void last_ready_worker_exits_before_irq_wakes_waiter(bool event_wait) {
    Fixture f; auto &ctx = f.runtime.cpu();
    const auto object = event_wait ? f.thread(0x55C20A00, {name, 0x200, 0, 0}) : f.thread(0xD6DA4BA1, {name, 0, 0, 1, 0});
    const auto worker = f.ready_child(); f.install_irq();
    if (event_wait) observation.event = object; else observation.sema = object;
    ctx.gpr[31] = root_pc; ctx.gpr[16] = 0x12345678;
    const auto root_sp = ctx.gpr[29], root_gp = ctx.gpr[28], root_tls = ctx.gpr[26];
    if (event_wait) f.thread(0x402FCF22, {object, 1, 0, argument + 16, 0});
    else f.thread(0x4E3A1105, {object, 1, 0});
    require(runtime_thread_uid() == static_cast<std::int32_t>(worker), "Root did not block before worker exit");
    const auto worker_token = capture_runtime_execution_context();
    f.thread(0xAA73C935, {7});
    require(observation.calls == 1 && f.host.display_state().interrupt_callbacks == 1 &&
        f.now == 1000000000 + period && runtime_thread_uid() == 1 && !runtime_execution_context_matches(worker_token),
        "Last worker exit suppressed the IRQ required to wake a live blocked task");
    require(ctx.pc == root_pc && ctx.gpr[31] == root_pc && ctx.gpr[2] == 0 && ctx.gpr[16] == 0x12345678 &&
        ctx.gpr[29] == root_sp && ctx.gpr[28] == root_gp && ctx.gpr[26] == root_tls,
        "Borrowed idle context erased a genuine waiter wakeup or its saved call/TLS state");
    if (event_wait) require(f.runtime.memory().load32(argument + 16) == 1, "Idle IRQ did not publish matched event bits");
    require(f.thread(0x9FA03CD3, {worker}) == 0, "Exited worker was resurrected instead of remaining deletable");
    require(f.thread(0x71BC9871, {worker, 40}) == 0x80020198, "Deleted exited worker remained a live scheduling target");
    f.runtime.run(root_pc, 8);
    require(f.runtime.stop_reason() == "All Badman threads exited", "IRQ kept a fully exited process alive after idle wakeup");
}
void borrowed_waiter_does_not_resume_without_its_own_signal() {
    Fixture f; auto &ctx = f.runtime.cpu();
    const auto root_sema = f.thread(0xD6DA4BA1, {name, 0, 0, 1, 0});
    const auto other_sema = f.thread(0xD6DA4BA1, {name, 0, 0, 1, 0});
    const auto other = f.ready_child(40); f.install_irq(); observation.sema = other_sema;
    ctx.gpr[31] = root_pc; ctx.gpr[16] = 0xCAFEBABE; f.thread(0x4E3A1105, {root_sema, 1, 0});
    const auto last = f.thread(0x446D8DE6, {name, child, 60, 4096, 0, 0});
    ctx.gpr[31] = child_pc; f.thread(0xF475845D, {last, 0, 0});
    ctx.gpr[31] = child_pc; f.thread(0x4E3A1105, {other_sema, 1, 0});
    require(runtime_thread_uid() == static_cast<std::int32_t>(last), "Last worker fixture did not run");
    f.thread(0xAA73C935, {0});
    require(runtime_thread_uid() == static_cast<std::int32_t>(other) && ctx.pc == child_pc && ctx.gpr[2] == 0,
        "Borrowed root waiter resumed without receiving its own semaphore permit");
    // The ISR used root's identity, but only the other waiter became ready.
    // A real signal is still required before root's saved continuation resumes.
    ctx.gpr[31] = child_pc; f.thread(0x3F53E640, {root_sema, 1});
    require(runtime_thread_uid() == 1 && ctx.pc == root_pc && ctx.gpr[2] == 0 && ctx.gpr[16] == 0xCAFEBABE,
        "Borrowed root context was mutated instead of remaining blocked until its real signal");
}
void borrowed_interrupt_failure_restores_previous_identity() {
    Fixture f; auto &ctx = f.runtime.cpu();
    const auto sema = f.thread(0xD6DA4BA1, {name, 0, 0, 1, 0}); const auto worker = f.ready_child(); f.install_irq();
    observation.throw_on_irq = true; ctx.gpr[31] = root_pc; f.thread(0x4E3A1105, {sema, 1, 0});
    require(runtime_thread_uid() == static_cast<std::int32_t>(worker), "Failure fixture did not run its worker");
    const std::string previous_name = runtime_thread_name(); const auto token = capture_runtime_execution_context();
    rejects([&] { f.thread(0xAA73C935, {0}); }, "Borrowed callback failure was hidden as a successful waiter wakeup");
    require(runtime_thread_uid() == static_cast<std::int32_t>(worker) && previous_name == runtime_thread_name() &&
        !runtime_execution_context_matches(token) && f.host.display_state().interrupt_callbacks == 0,
        "Failed borrowed interrupt lost previous identity/name, rolled back generation or fabricated completion");
    f.irq(release_irq, {30, 0});
    require(f.call("Kernel_Library", 0x092968F4) == 1, "Borrowed interrupt failure left interrupt masking stuck");
}
}
int main() {
    try {
        multi_thread_wait_and_frame_latch(); periodic_delivery_and_masked_coalescing();
        interrupt_wakes_worker_after_return(); registration_failures_and_incomplete_callbacks();
        enabled_irq_does_not_keep_ended_threads_alive();
        last_ready_worker_exits_before_irq_wakes_waiter(false); last_ready_worker_exits_before_irq_wakes_waiter(true);
        borrowed_waiter_does_not_resume_without_its_own_signal(); borrowed_interrupt_failure_restores_previous_identity();
        std::cout << "Badman vblank deadlines, multi-thread waits, framebuffer latch, IRQ ABI and completion tests passed\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}

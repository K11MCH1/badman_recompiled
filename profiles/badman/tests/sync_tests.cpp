#include "badman_host.hpp"
#include "psprecomp/common.hpp"
#include <array>
#include <initializer_list>
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
using namespace psprecomp;
constexpr std::uint32_t create = 0xD6DA4BA1, destroy = 0x28B6489C, signal = 0x3F53E640;
constexpr std::uint32_t poll = 0x58B1F937, wait = 0x4E3A1105;
constexpr std::uint32_t unknown = 0x80020199, empty = 0x800201AD, overflow = 0x800201AE;
constexpr std::uint32_t illegal = 0x800201BD, deleted = 0x800201B5;
constexpr std::uint32_t root_entry = 0x08804000, root_continuation = 0x08804004;
constexpr std::uint32_t child_a_entry = 0x08805000, child_a_continuation = 0x08805004;
constexpr std::uint32_t child_b_entry = 0x08806000, child_b_continuation = 0x08806004;
constexpr std::uint32_t name = 0x08802000;
void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
template <class Function> void rejects(Function function, const char *message) {
    bool rejected = false; try { function(); } catch (const Error &) { rejected = true; }
    require(rejected, message);
}
void root_stop(Runtime &runtime, AllegrexContext &) { runtime.stop("root continuation reached"); }
void child_return(Runtime &, AllegrexContext &ctx) { ctx.pc = badman::Host::thread_return_pc; }
struct Fixture {
    Runtime runtime;
    std::ostringstream trace;
    badman::Host host{runtime, 0x089F0000, 0x089E0000, 0x0891F420, trace};
    Fixture() {
        runtime.register_function(root_entry, root_stop, "sync_test_root");
        runtime.register_function(root_continuation, root_stop, "sync_test_root_continuation");
        runtime.register_function(child_a_entry, child_return, "sync_test_child_a");
        runtime.register_function(child_a_continuation, child_return, "sync_test_child_a_continuation");
        runtime.register_function(child_b_entry, child_return, "sync_test_child_b");
        runtime.register_function(child_b_continuation, child_return, "sync_test_child_b_continuation");
        host.install({}); host.bootstrap(root_entry, "sync");
        const std::array<std::uint8_t, 5> bytes{'s', 'y', 'n', 'c', 0}; runtime.memory().copy_in(name, bytes);
    }
    std::uint32_t call(std::uint32_t nid, std::initializer_list<std::uint32_t> arguments = {}) {
        auto &ctx = runtime.cpu();
        for (unsigned reg = 4; reg < 10; ++reg) ctx.gpr[reg] = 0;
        unsigned reg = 4; for (auto value : arguments) ctx.gpr[reg++] = value;
        runtime.invoke_import("ThreadManForUser", nid, ctx); return ctx.gpr[2];
    }
    std::uint32_t semaphore(std::uint32_t initial = 0, std::uint32_t maximum = 1, std::uint32_t attributes = 0) {
        const auto uid = call(create, {name, attributes, initial, maximum, 0});
        require(uid > 0 && uid < 0x80000000u, "Semaphore creation did not return an owned UID"); return uid;
    }
    std::uint32_t start_child(std::uint32_t entry, std::uint32_t priority) {
        runtime.cpu().gpr[31] = root_continuation;
        const auto uid = call(0x446D8DE6, {name, entry, priority, 4096, 0, 0});
        require(uid > 0 && uid < 0x80000000u, "Child creation failed");
        call(0xF475845D, {uid, 0, 0});
        require(runtime_thread_uid() == static_cast<std::int32_t>(uid), "Higher-priority child did not start"); return uid;
    }
    void block(std::uint32_t uid, std::uint32_t semaphore, std::uint32_t count, std::uint32_t continuation) {
        require(runtime_thread_uid() == static_cast<std::int32_t>(uid), "Wait ran in the wrong PSP thread");
        auto &ctx = runtime.cpu(); ctx.gpr[31] = continuation;
        ctx.gpr[29] = 0x09F00000 + uid * 0x100; ctx.gpr[28] = 0x08900000 + uid * 16;
        ctx.gpr[16] = 0xCA000000 + uid; ctx.hi = 0x12340000 + uid; ctx.lo = 0x56780000 + uid;
        const auto token = capture_runtime_execution_context(); call(wait, {semaphore, count, 0});
        require(runtime_thread_uid() == 1 && !runtime_execution_context_matches(token) && !runtime.stopped(),
            "Unsatisfied wait did not cooperatively schedule the ready root thread");
    }
    void resumed(std::uint32_t uid, std::uint32_t continuation, std::uint32_t result = 0) {
        const auto &ctx = runtime.cpu();
        require(runtime_thread_uid() == static_cast<std::int32_t>(uid) && ctx.pc == continuation && ctx.gpr[31] == continuation,
            "Semaphore wake lost the thread identity or caller continuation");
        require(ctx.gpr[2] == result && ctx.gpr[29] == 0x09F00000 + uid * 0x100 &&
            ctx.gpr[28] == 0x08900000 + uid * 16 && ctx.gpr[16] == 0xCA000000 + uid &&
            ctx.hi == 0x12340000 + uid && ctx.lo == 0x56780000 + uid,
            "Semaphore wake lost return status, SP, GP or saved register state");
    }
    void exit_child() { call(0xAA73C935, {0}); }
};
void lifecycle_counts_and_errors() {
    Fixture f;
    require(f.call(create, {0, 0, 1, 1}) == 0x80020001, "Null semaphore name was accepted");
    require(f.call(create, {name, 0x200, 0, 1}) == 0x80020191, "Illegal semaphore attributes were accepted");
    const auto uid = f.semaphore(1, 1, 0x1FF);
    require(f.call(signal, {uid, 1}) == overflow && f.call(poll, {uid, 1}) == 0 && f.call(poll, {uid, 1}) == empty,
        "Overflow mutated a semaphore or poll did not consume the real count");
    require(f.call(poll, {uid, 0}) == empty, "Empty zero-count poll did not follow observed error precedence");
    require(f.call(signal, {uid, 1}) == 0 && f.call(poll, {uid, 0}) == illegal &&
        f.call(poll, {uid, 0xFFFFFFFF}) == illegal && f.call(poll, {uid, 2}) == illegal,
        "Signaled semaphore accepted an illegal acquisition count");
    require(f.call(wait, {uid, 0, 0}) == illegal && f.call(wait, {uid, 2, 0}) == illegal &&
        f.call(wait, {uid, 0xFFFFFFFF, 0}) == illegal && f.call(wait, {uid, 1, 0}) == 0,
        "Immediate wait misvalidated counts or consumed resources after failure");
    require(f.call(signal, {uid, 0}) == 0 && f.call(signal, {uid, 0xFFFFFFFF}) == 0 &&
        f.call(signal, {uid, 2}) == 0 && f.call(poll, {uid, 1}) == 0,
        "Observed signed/zero signal behavior was rejected or calculated incorrectly");
    require(f.call(destroy, {uid}) == 0, "Deleting an existing semaphore failed");
    for (auto invalid_uid : {0u, uid, 0x12345678u}) for (auto nid : {destroy, signal, poll, wait})
        require(f.call(nid, {invalid_uid, 1, 0}) == unknown, "Invalid/deleted semaphore operation returned the wrong UID error");
    const auto above_max = f.semaphore(3, 2);
    require(f.call(poll, {above_max, 2}) == 0 && f.call(poll, {above_max, 1}) == 0,
        "Hardware-accepted initial count above maximum was not retained");
    const auto negative = f.semaphore(0xFFFFFFFD, 3);
    require(f.call(signal, {negative, 1}) == 0 && f.call(poll, {negative, 1}) == empty &&
        f.call(signal, {negative, 3}) == 0 && f.call(poll, {negative, 1}) == 0,
        "Hardware-accepted negative initial count was not retained");
    const auto negative_max = f.semaphore(0, 0xFFFFFFFF);
    require(f.call(wait, {negative_max, 1}) == illegal, "Negative maximum did not reject an invalid positive wait count");
}
void ordering_and_context(std::uint32_t attributes, bool equal_priority = false) {
    Fixture f; const auto sema = f.semaphore(0, 1, attributes);
    const auto a = f.start_child(child_a_entry, 16); f.block(a, sema, 1, child_a_continuation);
    const auto b = f.start_child(child_b_entry, equal_priority ? 16 : 8); f.block(b, sema, 1, child_b_continuation);
    const auto first = (attributes & 0x100) && !equal_priority ? b : a;
    const auto first_pc = first == a ? child_a_continuation : child_b_continuation;
    const auto second = first == a ? b : a;
    const auto second_pc = second == a ? child_a_continuation : child_b_continuation;
    f.runtime.cpu().gpr[31] = root_continuation;
    const auto token = capture_runtime_execution_context(); f.call(signal, {sema, 1});
    require(!runtime_execution_context_matches(token), "Semaphore preemption did not invalidate the old execution context");
    f.resumed(first, first_pc); f.exit_child();
    require(runtime_thread_uid() == 1 && f.runtime.cpu().pc == root_continuation && f.runtime.cpu().gpr[2] == 0,
        "Signaler return status or continuation was overwritten by preemption");
    f.runtime.cpu().gpr[31] = root_continuation; f.call(signal, {sema, 1}); f.resumed(second, second_pc);
    // Execute the registered native continuation and the existing thread-return
    // trampoline, proving normal dispatch resumes the saved guest call flow.
    f.runtime.run(f.runtime.cpu().pc, 8);
    require(f.runtime.stop_reason() == "root continuation reached" && runtime_thread_uid() == 1,
        "Woken thread did not return through ordinary runtime dispatch");
    require(f.call(poll, {sema, 1}) == empty, "Granted semaphore permits were not consumed exactly once");
}
void head_of_line_and_atomic_overflow() {
    Fixture f; const auto sema = f.semaphore(0, 2);
    const auto a = f.start_child(child_a_entry, 16); f.block(a, sema, 2, child_a_continuation);
    const auto b = f.start_child(child_b_entry, 8); f.block(b, sema, 1, child_b_continuation);
    require(f.call(signal, {sema, 1}) == 0 && runtime_thread_uid() == 1,
        "FIFO signal skipped an unsatisfied earlier waiter");
    require(f.call(poll, {sema, 1}) == empty, "Poll bypassed queued semaphore waiters");
    f.runtime.cpu().gpr[31] = root_continuation; f.call(signal, {sema, 1}); f.resumed(a, child_a_continuation); f.exit_child();
    f.runtime.cpu().gpr[31] = root_continuation; f.call(signal, {sema, 1}); f.resumed(b, child_b_continuation); f.exit_child();
    const auto binary = f.semaphore(0, 1);
    const auto waiter = f.start_child(child_a_entry, 16); f.block(waiter, binary, 1, child_a_continuation);
    require(f.call(signal, {binary, 3}) == overflow && runtime_thread_uid() == 1,
        "Overflowing signal partially released a waiting thread");
    f.runtime.cpu().gpr[31] = root_continuation; f.call(signal, {binary, 2}); f.resumed(waiter, child_a_continuation); f.exit_child();
    require(f.call(poll, {binary, 1}) == 0 && f.call(poll, {binary, 1}) == empty,
        "Waiter consumption was not accounted for before maximum-count validation");
}
void deletion_and_deferred_preemption() {
    Fixture f; const auto sema = f.semaphore(0, 1);
    const auto a = f.start_child(child_a_entry, 16); f.block(a, sema, 1, child_a_continuation);
    const auto b = f.start_child(child_b_entry, 8); f.block(b, sema, 1, child_b_continuation);
    f.runtime.cpu().gpr[31] = root_continuation; f.call(destroy, {sema}); f.resumed(b, child_b_continuation, deleted);
    f.exit_child(); f.resumed(a, child_a_continuation, deleted); f.exit_child();
    require(runtime_thread_uid() == 1 && f.call(poll, {sema, 1}) == unknown,
        "Deleted semaphore remained valid after releasing its waiters");
    const auto deferred = f.semaphore(0, 1);
    const auto child = f.start_child(child_a_entry, 16); f.block(child, deferred, 1, child_a_continuation);
    auto &ctx = f.runtime.cpu(); f.runtime.invoke_import("Kernel_Library", 0x092968F4, ctx);
    require(ctx.gpr[2] == 1, "Interrupt suspension setup failed");
    require(f.call(signal, {deferred, 1}) == 0 && runtime_thread_uid() == 1,
        "Signal switched threads while interrupts were masked");
    ctx.gpr[4] = 1; ctx.gpr[31] = root_continuation;
    f.runtime.invoke_import("Kernel_Library", 0x5F10D406, ctx); f.resumed(child, child_a_continuation);
}
void unsupported_waits_and_real_deadlock() {
    {
        Fixture f; const auto sema = f.semaphore(1, 1); const auto timeout = name + 16;
        f.runtime.memory().store32(timeout, 0x12345678);
        rejects([&] { f.call(wait, {sema, 1, timeout}); }, "Unsupported timed wait returned fake success");
        require(f.runtime.memory().load32(timeout) == 0x12345678 && f.call(poll, {sema, 1}) == 0,
            "Unsupported timed wait changed its timeout output or semaphore count");
        require(f.call(signal, {sema, 1}) == 0 && f.call(0x6D212BAC, {sema, 1, 0}) == 0 &&
                f.call(poll, {sema, 1}) == 0x800201AD,
            "Callback-aware wait without pending callbacks failed to consume a real permit");
        auto &ctx = f.runtime.cpu(); f.runtime.invoke_import("Kernel_Library", 0x092968F4, ctx);
        require(f.call(wait, {sema, 1, 0}) == 0x800201A7 && runtime_thread_uid() == 1,
            "Wait blocked with interrupts disabled");
    }
    {
        Fixture f; const auto sema = f.semaphore(0, 1);
        f.runtime.cpu().gpr[31] = root_continuation; f.runtime.cpu().gpr[2] = 0xA5A5A5A5;
        f.call(wait, {sema, 1, 0});
        require(f.runtime.stopped() && f.runtime.stop_reason() == "Badman scheduler has no runnable thread" &&
            f.runtime.cpu().gpr[2] == 0xA5A5A5A5,
            "Unsatisfied wait with no runnable signaler fabricated successful completion");
    }
}
}
int main() {
    try {
        lifecycle_counts_and_errors(); ordering_and_context(0); ordering_and_context(0x100);
        ordering_and_context(0x100, true); head_of_line_and_atomic_overflow();
        deletion_and_deferred_preemption(); unsupported_waits_and_real_deadlock();
        std::cout << "Badman semaphore lifecycle, counts, FIFO/priority waits, context, deletion and failure tests passed\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}

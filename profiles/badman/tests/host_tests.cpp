#include "badman_host.hpp"
#include "psprecomp/common.hpp"
#include <iostream>
#include <sstream>
#include <stdexcept>

namespace {
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
void root_return(psprecomp::Runtime &, psprecomp::AllegrexContext &ctx) { ctx.pc = badman::Host::thread_return_pc; }
void child_return(psprecomp::Runtime &runtime, psprecomp::AllegrexContext &ctx) {
    require(ctx.gpr[4] == 4, "Thread argument length lost");
    require(runtime.memory().load32(ctx.gpr[5]) == 0x00636261u, "Thread arguments were not copied onto its stack");
    ctx.gpr[2] = 0;
    ctx.pc = badman::Host::thread_return_pc;
}
}
void run_case(std::uint32_t child_priority, bool masked = false) {
    psprecomp::Runtime runtime;
    std::ostringstream trace;
    badman::Host host(runtime, 0x089F0000u, 0x089E0000u, 0x0891F420u, trace);
    const auto before = host.free_bytes();
    const auto first = host.allocate(257), second = host.allocate(128), high = host.allocate(2048, true);
    require(second == first + 512, "Allocator alignment or low ordering failed");
    require(high > second && high % 256 == 0, "High allocation failed");
    host.release(second); host.release(first); host.release(high);
    require(host.free_bytes() == before && host.allocate(before) == first, "Free blocks did not coalesce");
    host.release(first);
    const auto prefix = host.allocate(256);
    const auto aligned = host.allocate(512, false, 4096);
    require(aligned % 4096 == 0 && aligned > prefix, "Aligned allocation consumed a misaligned block");
    host.release(prefix); host.release(aligned);
    require(host.free_bytes() == before, "Alignment padding was lost during release");
    bool invalid_release = false;
    try { host.release(first); } catch (const psprecomp::Error &) { invalid_release = true; }
    require(invalid_release, "Double release was accepted");
    host.install({{"UnimplementedLibrary", 1, 0x08801000u}});
    auto &power_context = runtime.cpu();
    power_context.gpr[4] = 0x300;
    runtime.invoke_import("sceUtility", 0x2A2B3DE0u, power_context);
    require(power_context.gpr[2] == 0x8002013Cu && !runtime.stopped(),
            "Unavailable AV module pretended to load or suppressed guest error handling");
    for (const auto mode : {0u, 1u, 6u}) {
        power_context.gpr[4] = mode;
        runtime.invoke_import("sceSuspendForUser", 0x090CCB3Fu, power_context);
        require(power_context.gpr[2] == 0, "Valid power activity update failed");
    }
    require(host.power_tick_counts() == std::array<std::uint64_t, 2>{2, 2},
            "Suspend/display power tick selection is incorrect");
    power_context.gpr[4] = 2;
    bool invalid_tick = false;
    try { runtime.invoke_import("sceSuspendForUser", 0x090CCB3Fu, power_context); }
    catch (const psprecomp::Error &) { invalid_tick = true; }
    require(invalid_tick && host.power_tick_counts() == std::array<std::uint64_t, 2>{2, 2},
            "Invalid power tick changed idle activity");
    power_context.gpr[4] = 10000;
    runtime.invoke_import("sceCtrl", 0x6A2774F3u, power_context);
    require(power_context.gpr[2] == 0, "Controller cycle did not return its previous setting");
    power_context.gpr[4] = 1;
    runtime.invoke_import("sceCtrl", 0x6A2774F3u, power_context);
    require(power_context.gpr[2] == 0x800001FEu, "Invalid controller cycle was accepted");
    power_context.gpr[4] = 0;
    runtime.invoke_import("sceCtrl", 0x6A2774F3u, power_context);
    require(power_context.gpr[2] == 10000, "Invalid controller cycle changed its stored setting");
    power_context.gpr[4] = 1;
    runtime.invoke_import("sceCtrl", 0x1F4011E6u, power_context);
    require(power_context.gpr[2] == 0, "Controller mode did not return its previous setting");
    power_context.gpr[4] = 0;
    runtime.invoke_import("sceCtrl", 0x1F4011E6u, power_context);
    require(power_context.gpr[2] == 1, "Controller analog setting was not retained");
    power_context.gpr[4] = 0x08801000u; power_context.gpr[5] = 0x08801004u;
    runtime.invoke_import("sceImpose", 0x24FD7BCFu, power_context);
    require(power_context.gpr[2] == 0 && runtime.memory().load32(0x08801000u) == 1 &&
            runtime.memory().load32(0x08801004u) == 1, "US language/button defaults were not returned");
    runtime.memory().store32(0x08801000u, 0x12345678u); power_context.gpr[5] = 0;
    runtime.invoke_import("sceImpose", 0x24FD7BCFu, power_context);
    require(power_context.gpr[2] == 0x80000103u && runtime.memory().load32(0x08801000u) == 0x12345678u,
            "Invalid language-mode output caused a partial write");
    power_context.gpr[4] = 1; power_context.gpr[5] = 0x08801000u; power_context.gpr[6] = 16;
    runtime.invoke_import("sceUtility", 0x34B78343u, power_context);
    require(power_context.gpr[2] == 0 && runtime.memory().load8(0x08801000u) == 'P' &&
            runtime.memory().load8(0x08801006u) == 0, "Profile nickname was not copied with a terminator");
    runtime.memory().store32(0x08801000u, 0x12345678u); power_context.gpr[6] = 6;
    runtime.invoke_import("sceUtility", 0x34B78343u, power_context);
    require(power_context.gpr[2] == 0x80110103u && runtime.memory().load32(0x08801000u) == 0x12345678u,
            "Undersized nickname output was partially overwritten");
    runtime.register_function(0x08804000u, root_return, "test_root");
    runtime.register_function(0x08805000u, child_return, "test_child");
    host.bootstrap(0x08804000u, "arg");
    auto &ctx = runtime.cpu();
    if (masked) {
        runtime.invoke_import("Kernel_Library", 0x092968F4u, ctx);
        require(ctx.gpr[2] == 1, "First interrupt suspension lost the enabled state");
        runtime.invoke_import("Kernel_Library", 0x092968F4u, ctx);
        require(ctx.gpr[2] == 0, "Nested interrupt suspension returned an enabled state");
        ctx.gpr[4] = 0;
        runtime.invoke_import("Kernel_Library", 0x5F10D406u, ctx);
    }
    const auto name = host.allocate(256);
    runtime.memory().store32(name, 0x00636261u);
    ctx.gpr[4] = name; ctx.gpr[5] = 2; ctx.gpr[6] = 0;
    ctx.gpr[7] = 17; ctx.gpr[8] = 3; ctx.gpr[9] = 0;
    runtime.invoke_import("ThreadManForUser", 0xC07BB470u, ctx);
    const auto pool_uid = ctx.gpr[2];
    std::uint32_t pool_base = 0;
    for (std::uint32_t i = 0; i < 3; ++i) {
        ctx.gpr[4] = pool_uid; ctx.gpr[5] = name + 8;
        runtime.invoke_import("ThreadManForUser", 0x623AE665u, ctx);
        const auto address = runtime.memory().load32(name + 8);
        if (i == 0) pool_base = address;
        require(address == pool_base + i * 24, "Fixed-pool blocks overlap or have incorrect alignment");
    }
    bool exhausted = false;
    try { runtime.invoke_import("ThreadManForUser", 0x623AE665u, ctx); }
    catch (const psprecomp::Error &) { exhausted = true; }
    require(exhausted, "Fixed pool returned a block twice");
    ctx.gpr[4] = pool_uid;
    runtime.invoke_import("ThreadManForUser", 0xED1410E0u, ctx);
    const auto workspace = host.allocate(32);
    ctx.gpr[4] = workspace; ctx.gpr[5] = name; ctx.gpr[6] = 0x200;
    ctx.gpr[7] = 0; ctx.gpr[8] = 0;
    runtime.invoke_import("ThreadManForUser", 0x19CFF145u, ctx);
    ctx.gpr[4] = workspace; ctx.gpr[5] = 2; ctx.gpr[6] = 0;
    runtime.invoke_import("Kernel_Library", 0xBEA46419u, ctx);
    require(runtime.memory().load32(workspace) == 2 && runtime.memory().load32(workspace + 4) == 1,
            "Lightweight-mutex workspace does not reflect its owner/count");
    ctx.gpr[5] = 1;
    runtime.invoke_import("Kernel_Library", 0x15B6446Bu, ctx);
    require(runtime.memory().load32(workspace) == 1, "Partial recursive unlock released the mutex");
    runtime.invoke_import("Kernel_Library", 0x15B6446Bu, ctx);
    require(runtime.memory().load32(workspace) == 0, "Final mutex unlock did not release ownership");
    runtime.invoke_import("ThreadManForUser", 0x60107536u, ctx);
    host.release(workspace);
    ctx.gpr[4] = name; ctx.gpr[5] = 0x200; ctx.gpr[6] = 3; ctx.gpr[7] = 0;
    runtime.invoke_import("ThreadManForUser", 0x55C20A00u, ctx);
    const auto event_uid = ctx.gpr[2];
    const auto immediate_event = masked ? 0x30FD48F0u : 0x402FCF22u;
    ctx.gpr[4] = event_uid; ctx.gpr[5] = 1; ctx.gpr[6] = 0x20; ctx.gpr[7] = name + 8;
    runtime.invoke_import("ThreadManForUser", immediate_event, ctx);
    require(runtime.memory().load32(name + 8) == 3, "Event wait lost the pattern before clearing");
    ctx.gpr[5] = 2; ctx.gpr[6] = 0;
    runtime.invoke_import("ThreadManForUser", 0x30FD48F0u, ctx);
    require(runtime.memory().load32(name + 8) == 2, "Event WAITCLEAR cleared unrelated bits");
    ctx.gpr[5] = 0;
    runtime.invoke_import("ThreadManForUser", 0x812346E4u, ctx);
    ctx.gpr[5] = 4;
    runtime.invoke_import("ThreadManForUser", 0x1FB15A32u, ctx);
    ctx.gpr[5] = 6; ctx.gpr[6] = 0x11;
    runtime.invoke_import("ThreadManForUser", immediate_event, ctx);
    require(runtime.memory().load32(name + 8) == 4, "Event OR matching or retain-mask clearing failed");
    runtime.invoke_import("ThreadManForUser", 0xEF9E4C70u, ctx);
    ctx.gpr[4] = 0; ctx.gpr[5] = 1; ctx.gpr[6] = 1; ctx.gpr[7] = name + 8;
    runtime.memory().store32(name + 8, 0x12345678u);
    runtime.invoke_import("ThreadManForUser", 0x30FD48F0u, ctx);
    require(ctx.gpr[2] == 0x8002019Au && runtime.memory().load32(name + 8) == 0x12345678u,
            "Unknown event flag UID did not return an error with preserved output");
    for (const auto uid : {0u, event_uid}) {
        ctx.gpr[4] = uid;
        for (const auto nid : {0x1FB15A32u, 0x812346E4u, 0xEF9E4C70u, 0x402FCF22u, 0x30FD48F0u}) {
            runtime.invoke_import("ThreadManForUser", nid, ctx);
            require(ctx.gpr[2] == 0x8002019Au && runtime.memory().load32(name + 8) == 0x12345678u,
                    "Invalid/deleted event operation failed to preserve outputs and report UNKNOWN_EVFID");
        }
    }
    ctx.gpr[4] = name; ctx.gpr[5] = 0x08805000u; ctx.gpr[6] = child_priority; ctx.gpr[7] = 4096;
    runtime.invoke_import("ThreadManForUser", 0x446D8DE6u, ctx);
    const auto child_uid = ctx.gpr[2];
    ctx.gpr[4] = child_uid; ctx.gpr[5] = 4; ctx.gpr[6] = name;
    const auto token = psprecomp::capture_runtime_execution_context();
    runtime.invoke_import("ThreadManForUser", 0xF475845Du, ctx);
    require(psprecomp::runtime_execution_context_matches(token) == (masked || child_priority >= 32),
            "Preemption did not update the execution-context token");
    if (masked) {
        ctx.gpr[4] = 1;
        runtime.invoke_import("Kernel_Library", 0x5F10D406u, ctx);
        require(!psprecomp::runtime_execution_context_matches(token),
                "Restoring interrupts failed to schedule the higher-priority thread");
    }
    runtime.run(child_priority < 32 ? ctx.pc : 0x08804000u, 20);
    require(runtime.stop_reason() == "All Badman threads exited", "Ready child did not run after module return");
    require(host.started_threads() == 1, "Thread start count incorrect");
    runtime.invoke_import("UnimplementedLibrary", 1, ctx);
    require(runtime.stop_reason().find("Unimplemented Badman import") == 0,
            "Unimplemented import was silently accepted");
}
int main() {
    try {
        run_case(32);
        run_case(16);
        run_case(16, true);
        std::cout << "Badman allocator and thread-bootstrap tests passed\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}

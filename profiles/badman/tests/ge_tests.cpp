#include "badman_host.hpp"
#include "psprecomp/common.hpp"
#include <array>
#include <bit>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace {
using namespace psprecomp;
void require(bool condition, const char *message) { if (!condition) throw std::runtime_error(message); }
void write(GuestMemory &memory, std::uint32_t address, std::initializer_list<std::uint32_t> words) {
    for (auto word : words) { memory.store32(address, word); address += 4; }
}
void state_and_interrupt_ordering() {
    GuestMemory memory;
    std::vector<badman::GeInterrupt> events;
    std::vector<std::uint32_t> observed;
    badman::Ge *selected = nullptr;
    badman::Ge ge(memory, [&](const auto &event) { events.push_back(event); observed.push_back(selected->command(0x12)); });
    selected = &ge;
    constexpr std::uint32_t code = 0x08804000, data = 0x08805000;
    write(memory, data, {0x08810000, 0x1234, 0x08811000, 0x5678});
    const auto callback = ge.set_callback(data);
    write(memory, data, {0, 0, 0, 0}); // Registration must copy a transient guest stack structure.
    write(memory, code, {0x3A000000, 0x3B3F8000, 0x3B400000, 0x0E010123, 0x0C000000,
                         0x1200ABCD, 0x0F000045, 0x0C000000});
    const auto list = ge.enqueue(code, 0, callback);
    require(ge.list_status(list, true) == 0 && ge.draw_status(true) == 0, "Finished list did not synchronize");
    require(events.size() == 2 && events[0].signal && !events[1].signal, "SIGNAL/FINISH callback ordering is wrong");
    require(events[0].token == 0x123 && events[0].argument == 0x1234 && events[0].next_pc == code + 20,
            "Signal token/common/continuation ABI is wrong");
    require(events[1].token == 0x45 && events[1].argument == 0x5678 && events[1].next_pc == code + 32,
            "Finish token/common/continuation ABI is wrong");
    require(observed[0] == 0 && observed[1] == 0x1200ABCD, "Suspend signal callback ran after later GE state changes");
    require(ge.matrix_word(1, 0) == 0x3F800000 && ge.matrix_word(1, 1) == 0x40000000,
            "Matrix DATA streaming overwrote earlier elements");
}
void stalled_queue_ordering() {
    GuestMemory memory;
    std::vector<std::uint32_t> finished;
    badman::Ge ge(memory, [&](const auto &event) { finished.push_back(event.token); });
    constexpr std::uint32_t a = 0x08804000, b = a + 0x100, c = a + 0x200, data = a + 0x300;
    write(memory, data, {0, 0, 0x08810000, 0});
    const auto callback = ge.set_callback(data);
    write(memory, a, {0x12000001, 0x0F00000A, 0x0C000000});
    write(memory, b, {0x12000002, 0x0F00000B, 0x0C000000});
    write(memory, c, {0x12000003, 0x0F00000C, 0x0C000000});
    const auto first = ge.enqueue(a, a + 4, callback);
    const auto tail = ge.enqueue(b, 0, callback);
    const auto head = ge.enqueue(c, 0, callback, 0, true);
    require(ge.list_status(first, false) == 3 && ge.list_status(tail, false) == 1 &&
            ge.list_status(head, false) == 1 && ge.command(0x12) == 0x12000001,
            "A later queued list bypassed the active list's stall");
    bool blocked = false;
    try { ge.draw_status(true); } catch (const Error &) { blocked = true; }
    require(blocked && finished.empty(), "A blocking sync invented completion for a stalled list");
    ge.update_stall(first, 0);
    require(finished == std::vector<std::uint32_t>({10, 12, 11}), "Queue-head insertion broke GE submission order");
    require(ge.completed_lists() == 3 && ge.draw_status(true) == 0, "Stall release failed to drain the ordered queue");
}
void call_and_invalid_draw() {
    GuestMemory memory;
    badman::Ge ge(memory, [](const auto &) {});
    constexpr std::uint32_t code = 0x08804000;
    write(memory, code, {0x10080000, 0x13000001, 0x0A804080, 0x08804100});
    write(memory, 0x08804180, {0x13000002, 0x10090000, 0x0B000000});
    write(memory, 0x09804200, {0x0F000000, 0x0C000000});
    require(ge.list_status(ge.enqueue(code, 0, -1), true) == 0, "CALL/RET failed to restore OFFSET or retain BASE");
    write(memory, code, {0x04030003, 0x0F000000, 0x0C000000});
    bool rejected = false;
    try { ge.enqueue(code, 0, -1); } catch (const Error &) { rejected = true; }
    require(rejected && ge.completed_lists() == 1, "Unsupported geometry was silently treated as a completed draw");
}
void hardware_call_depth_and_options() {
    GuestMemory memory;
    badman::Ge ge(memory, [](const auto &) {});
    constexpr std::uint32_t code = 0x08804000, options = 0x08805000;
    write(memory, options, {16, 0, 1, 0x08806000});
    write(memory, code, {0x10080000, 0x0A804100, 0x0F000000, 0x0C000000});
    write(memory, code + 0x100, {0x12000001, 0x0A804200, 0x12000011, 0x0B000000});
    write(memory, code + 0x200, {0x15000002, 0x0A804300, 0x15000022, 0x0B000000});
    write(memory, code + 0x300, {0x16000033, 0x0B000000});
    require(ge.list_status(ge.enqueue(code, 0, -1, options), true) == 0, "Two hardware CALL levels failed");
    require(ge.command(0x12) == 0x12000011 && ge.command(0x15) == 0x15000022 && ge.command(0x16) == 0,
            "A third raw CALL executed instead of being skipped");
    // numStacks governs firmware SIGNAL calls, not the two raw CALL slots.
    write(memory, options, {9, 0, 0, 0});
    bool malformed = false;
    try { ge.enqueue(code, 0, -1, options); } catch (const Error &) { malformed = true; }
    require(malformed && ge.completed_lists() == 1, "Partial GE options were silently accepted");
}
void address_mask_and_sync_signal() {
    GuestMemory memory;
    std::vector<badman::GeInterrupt> callbacks;
    badman::Ge ge(memory, [&](const auto &event) { callbacks.push_back(event); });
    constexpr std::uint32_t code = 0x08804000, data = 0x08805000;
    write(memory, data, {0, 0, 0x08810000, 0});
    const auto callback = ge.set_callback(data);
    write(memory, code, {0x10180000, 0x08804100});
    write(memory, code + 0x100, {0x0E080000, 0x0C000000, 0x0F000011, 0x0C000000,
                               0x1200ABCD, 0x0F000022, 0x0C000000});
    const auto list = ge.enqueue(code, code + 0x108, callback);
    require(ge.list_status(list, false) == 3, "BASE used bits beyond the GE address's high nibble");
    ge.update_stall(list, 0);
    require(ge.command(0x12) == 0x1200ABCD && callbacks.size() == 1 && callbacks[0].token == 0x22,
            "SIGNAL SYNC failed to ignore exactly the next FINISH/END across a stall");
}
void callback_failure_and_pump_budget() {
    GuestMemory memory;
    constexpr std::uint32_t code = 0x08804000, data = 0x08805000;
    write(memory, data, {0, 0, 0x08810000, 0});
    write(memory, code, {0x0F000055, 0x0C000000});
    badman::Ge failing(memory, [](const auto &) { throw Error("Synthetic callback failure"); });
    const auto callback = failing.set_callback(data);
    bool failed = false;
    try { failing.enqueue(code, 0, callback); } catch (const Error &) { failed = true; }
    require(failed && failing.completed_lists() == 0 && failing.delivered_callbacks() == 0,
            "A failed callback was counted as successful list/callback completion");

    std::uint32_t requests = 0;
    badman::Ge *selected = nullptr;
    std::int32_t repeating_callback = -1;
    badman::Ge bounded(memory, [&](const auto &) {
        if (++requests > 20) throw Error("Pump failed to honor its configured budget");
        selected->enqueue(code, 0, repeating_callback);
    }, 16);
    selected = &bounded;
    repeating_callback = bounded.set_callback(data);
    bool exhausted = false;
    try { bounded.enqueue(code, 0, repeating_callback); } catch (const Error &) { exhausted = true; }
    require(exhausted && bounded.executed_commands() == 16 && requests == 8,
            "A callback-generated queue bypassed the pump-wide command budget");
}
std::uint32_t callback_count = 0;
void translated_callback(Runtime &runtime, AllegrexContext &context) {
    require(context.gpr[4] == 0x55 && context.gpr[6] == 0x08821008, "Host callback ABI was not preserved");
    runtime.memory().store32(context.gpr[5], context.gpr[4]);
    context.gpr[16] = 0xBAD;
    context.pc = context.gpr[31];
    ++callback_count;
}
void callback_context_isolation() {
    Runtime runtime;
    std::ostringstream trace;
    badman::Host host(runtime, 0x089F0000, 0x089E0000, 0x0891F420, trace);
    host.install({}); host.bootstrap(0x08804000, "test");
    runtime.register_function(0x08810000, translated_callback, "test_translated_callback");
    auto &ctx = runtime.cpu();
    write(runtime.memory(), 0x08820100, {0, 0, 0x08810000, 0x08820000});
    write(runtime.memory(), 0x08821000, {0x0F000055, 0x0C000000});
    ctx.gpr[4] = 0x08820100;
    runtime.invoke_import("sceGe_user", 0xA4FC06A4, ctx);
    const auto callback = ctx.gpr[2];
    ctx.gpr[4] = 0x08821000; ctx.gpr[5] = 0; ctx.gpr[6] = callback; ctx.gpr[7] = 0;
    ctx.gpr[16] = 0x12345678;
    const auto saved = ctx;
    runtime.invoke_import("sceGe_user", 0xAB49E76A, ctx);
    require(callback_count == 1 && runtime.memory().load32(0x08820000) == 0x55,
            "Registered callback did not execute guest code");
    for (std::size_t i = 0; i < ctx.gpr.size(); ++i)
        if (i != 2) require(ctx.gpr[i] == saved.gpr[i], "Callback corrupted the interrupted guest context");
    require(ctx.pc == saved.pc && runtime_thread_uid() == 1, "Callback changed caller PC/thread ownership");
}
void display_buffer_latching() {
    Runtime runtime;
    std::ostringstream trace;
    badman::Host host(runtime, 0x089F0000, 0x089E0000, 0x0891F420, trace);
    host.install({}); host.bootstrap(0x08804000, "test");
    auto &ctx = runtime.cpu();
    ctx.gpr[4] = 0; ctx.gpr[5] = 480; ctx.gpr[6] = 272;
    runtime.invoke_import("sceDisplay", 0x0E20F177, ctx);
    ctx.gpr[4] = 0x04088000; ctx.gpr[5] = 512; ctx.gpr[6] = 3; ctx.gpr[7] = 1;
    runtime.invoke_import("sceDisplay", 0x289D82FE, ctx);
    require(host.display_state().current.address == 0, "Next-vblank framebuffer latched immediately");
    runtime.invoke_import("sceDisplay", 0x984C27E7, ctx);
    require(host.display_state().current.address == 0x04088000 && host.display_state().vblanks > 0,
            "Vblank failed to latch the pending framebuffer");
    ctx.gpr[4] = 0x04088001; ctx.gpr[5] = 512; ctx.gpr[6] = 3; ctx.gpr[7] = 0;
    bool invalid = false;
    try { runtime.invoke_import("sceDisplay", 0x289D82FE, ctx); } catch (const Error &) { invalid = true; }
    require(invalid && host.display_state().current.address == 0x04088000, "Invalid framebuffer changed display state");
    runtime.invoke_import("sceDisplay", 0xDBA6C4C4, ctx);
    require(ctx.fpr[0] > 59 && ctx.fpr[0] < 61, "LCD frequency was not returned in the float ABI register");
}
void streamed_matrices_reach_raster() {
    GuestMemory memory;
    badman::Ge ge(memory, [](const auto &) {});
    constexpr std::uint32_t code = 0x08804000, vertices = 0x08806000;
    constexpr std::uint32_t pixels = 0x04020000, texture = 0x08030000;
    std::vector<std::uint32_t> words;
    const auto command = [&](unsigned id, std::uint32_t value) { words.push_back((id << 24) | (value & 0xFFFFFF)); };
    const auto floating = [&](unsigned id, float value) { command(id, std::bit_cast<std::uint32_t>(value) >> 8); };
    // A streamed world translation must move actual raster output. A test of
    // matrix_word alone cannot detect omitted GE -> Raster matrix forwarding.
    for (unsigned number : {0x3A, 0x3C, 0x3E}) {
        const unsigned rows = number == 0x3E ? 4 : 3;
        command(number, 0);
        for (unsigned column = 0; column < 4; ++column)
            for (unsigned row = 0; row < rows; ++row)
                floating(number + 1, column == row || (number == 0x3A && column == 3 && row == 0) ? 1.0f : 0.0f);
    }
    for (const unsigned id : {0x42, 0x43, 0x45, 0x46, 0x48, 0x49}) floating(id, 1.0f);
    command(0x1C, 1); command(0x1E, 1); command(0x55, 0xFFFFFF); command(0x58, 255);
    command(0x16, 7 | (7 << 10)); command(0xD5, 7 | (7 << 10)); command(0xD7, 65535);
    command(0x9C, 0x020000); command(0x9D, 64); command(0xD2, 3);
    command(0xA0, 0x030000); command(0xA8, 0x080002); command(0xB8, 0x0101);
    command(0xC3, 3); command(0xC7, 0x0101); command(0xC9, 0x0100);
    command(0x10, 0x080000); command(0x01, vertices); command(0x12, 0x1E3);
    command(0x04, 0x040004); command(0x0F, 0); command(0x0C, 0);
    for (std::size_t i = 0; i < words.size(); ++i) memory.store32(code + static_cast<std::uint32_t>(i * 4), words[i]);
    const std::array<std::array<float, 8>, 4> corners{{
        {0, 0, 0, 0, 1, -1, 1, 0}, {0, 1, 0, 0, 1, -1, -1, 0},
        {1, 0, 0, 0, 1, 1, 1, 0}, {1, 1, 0, 0, 1, 1, -1, 0}}};
    for (unsigned i = 0; i < 4; ++i) {
        memory.store32(texture + i * 4, 0xFF0000FF);
        for (unsigned j = 0; j < 8; ++j) memory.store32(vertices + i * 32 + j * 4, std::bit_cast<std::uint32_t>(corners[i][j]));
    }
    require(ge.list_status(ge.enqueue(code, 0, -1), true) == 0 && ge.color_pixels() == 4,
            "Streamed transformed strip failed to finish with four pixels");
    require(memory.load32(pixels) == 0 && (memory.load32(pixels + 4) & 0xFFFFFF) == 255 &&
            (memory.load32(pixels + 8) & 0xFFFFFF) == 255 && memory.load32(pixels + 12) == 0,
            "World translation did not move GE raster output one pixel right");
    require(ge.vertex_address() == vertices + 128, "Transformed strip did not advance the GE vertex cursor");
}
}
int main() {
    try {
        state_and_interrupt_ordering(); stalled_queue_ordering(); call_and_invalid_draw();
        hardware_call_depth_and_options(); address_mask_and_sync_signal(); callback_failure_and_pump_budget();
        callback_context_isolation(); display_buffer_latching(); streamed_matrices_reach_raster();
        std::cout << "Badman GE state, queue, stall, flow and callback tests passed\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}

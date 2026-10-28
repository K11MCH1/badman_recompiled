#include "badman_input.hpp"
#include "psprecomp/common.hpp"
#include <array>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <vector>

namespace {
using namespace psprecomp;
constexpr std::uint32_t output = 0x08804000;
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
template <class Function> void rejects(Function function, const char *message) {
    bool rejected = false;
    try { function(); } catch (const Error &) { rejected = true; }
    require(rejected, message);
}
std::array<std::uint8_t, 16> packet(const GuestMemory &memory, std::uint32_t address = output) {
    std::array<std::uint8_t, 16> result{}; memory.copy_out(address, result); return result;
}
struct Time {
    std::uint64_t now{1000000000};
    std::vector<std::uint64_t> deadlines;
    bool advance{true};
    std::function<void()> waiting;
    auto clock() { return [this] { return now; }; }
    auto wait() { return [this](std::uint64_t deadline) {
        deadlines.push_back(deadline); if (advance) now = deadline; if (waiting) waiting();
    }; }
};
void keyboard_buttons_and_focus() {
    badman::Keyboard keyboard;
    require(!keyboard.state(false).ready && keyboard.state(false).buttons == 0, "Disconnected keyboard produced an active provider");
    keyboard.set_key(badman::Key::cross, true);
    require(keyboard.state(true).buttons == 0, "Unfocused keyboard accepted a key");
    keyboard.set_focused(true);
    const std::array<badman::Key, 12> keys{badman::Key::select, badman::Key::start,
        badman::Key::up, badman::Key::right, badman::Key::down, badman::Key::left,
        badman::Key::left_trigger, badman::Key::right_trigger, badman::Key::triangle,
        badman::Key::circle, badman::Key::cross, badman::Key::square};
    const std::array<std::uint32_t, 12> masks{1, 8, 16, 32, 64, 128, 256, 512, 4096, 8192, 16384, 32768};
    for (std::size_t i = 0; i < keys.size(); ++i) {
        keyboard.set_key(keys[i], true);
        require((keyboard.state(true).buttons & masks[i]) != 0, "Keyboard button has the wrong PSP mask");
    }
    require(keyboard.state(true).ready && keyboard.state(true).buttons == 0xF3F9 &&
        keyboard.state(true).analog_x == 128 && keyboard.state(true).analog_y == 128, "Keyboard combined state or centered analog is wrong");
    keyboard.set_key(badman::Key::cross, true); keyboard.set_key(badman::Key::cross, false);
    require(keyboard.state(true).buttons == 0xB3F9, "Key repeat/release changed unrelated buttons");
    keyboard.set_focused(false);
    require(keyboard.state(true).ready && keyboard.state(true).buttons == 0, "Focus loss disconnected a live provider or left a held button stuck");
    keyboard.set_key(badman::Key::start, true); keyboard.set_focused(true);
    require(keyboard.state(true).ready && keyboard.state(true).buttons == 0, "Regaining focus invented held keys");
}
void exact_packet_modes_and_timestamps() {
    GuestMemory memory; Time time;
    badman::InputState state{true, 0x4018, 7, 249};
    badman::Input input([&] { return state; }, time.clock(), time.wait());
    memory.zero(output - 4, 24); memory.store32(output - 4, 0xA5A5A5A5); memory.store32(output + 16, 0xB6B6B6B6);
    time.now += 42000;
    require(input.read_positive(memory, output, 1, false) == 1, "Read did not return one real sample");
    const std::array<std::uint8_t, 16> expected{42, 0, 0, 0, 0x18, 0x40, 0, 0, 128, 128, 0, 0, 0, 0, 0, 0};
    require(packet(memory) == expected, "SceCtrlData layout, digital axes, timestamp, or reserved bytes are wrong");
    require(memory.load32(output - 4) == 0xA5A5A5A5 && memory.load32(output + 16) == 0xB6B6B6B6,
        "Controller packet crossed its 16-byte output range");
    require(input.set_sampling_mode(1) == 0 && input.set_sampling_mode(1) == 1, "Sampling mode did not return its old value");
    time.now += 16683333;
    input.read_positive(memory, output, 1, false);
    require(memory.load8(output + 8) == 7 && memory.load8(output + 9) == 249 && memory.load32(output) == 16725,
        "Analog provider values or actual sample timestamp were lost");
    require(input.set_sampling_mode(0) == 1, "Digital sampling transition did not return analog mode");
    rejects([&] { input.set_sampling_mode(2); }, "Unknown PSP input mode was accepted");
    require(input.sampling_mode() == 0, "Invalid mode changed controller state");
    require(input.set_sampling_cycle(5555) == 0 && input.set_sampling_cycle(20000) == 5555 &&
        input.set_sampling_cycle(0) == 20000, "Valid sampling cycles did not preserve old settings");
    for (const auto cycle : {1u, 5554u, 20001u, std::numeric_limits<std::uint32_t>::max()})
        rejects([&] { input.set_sampling_cycle(cycle); }, "Invalid PSP sampling cycle was accepted");
    require(input.sampling_cycle() == 0, "Invalid cycle changed the last valid setting");
}
void fresh_samples_pace_without_real_sleep() {
    GuestMemory memory; Time time;
    badman::InputState state{true, 0x4000, 128, 128};
    badman::Input input([&] { return state; }, time.clock(), time.wait());
    input.read_positive(memory, output, 1, false); const auto first = packet(memory);
    state.buttons = 0;
    rejects([&] { input.read_positive(memory, output, 1, false); }, "Repeated read blocked despite missing sole-thread permission");
    require(time.deadlines.empty() && packet(memory) == first, "Rejected blocking read waited or changed output");
    time.waiting = [&] { state.buttons = 0x8; };
    input.read_positive(memory, output, 1, true);
    require(time.deadlines == std::vector<std::uint64_t>{1016683333} && memory.load32(output) == 16683 &&
        memory.load32(output + 4) == 8, "Vblank-paced sample used stale buttons or the wrong deadline");
    input.set_sampling_cycle(5555);
    input.read_positive(memory, output, 1, true);
    require(time.deadlines.back() == 1022238333 && memory.load32(output) == 22238,
        "Configured microsecond sampling cycle did not pace the next sample");
    time.now += 5555000; time.waiting = {}; state.buttons = 0x100;
    input.read_positive(memory, output, 1, false);
    require(time.deadlines.size() == 2 && memory.load32(output + 4) == 0x100, "Available sample incorrectly required blocking permission");
}
void failures_preserve_output_and_consumption() {
    GuestMemory memory; Time time;
    memory.zero(output, 16); memory.store32(output, 0xDEADBEEF); const auto original = packet(memory);
    std::uint32_t calls = 0; badman::InputState state{false, 0, 128, 128};
    badman::Input input([&] { ++calls; return state; }, time.clock(), time.wait());
    for (auto count : {0u, 65u, std::numeric_limits<std::uint32_t>::max()})
        require(static_cast<std::uint32_t>(input.read_positive(memory, output, count, true)) == 0x80000104u,
            "Invalid sample count did not return the PSP invalid-size error");
    rejects([&] { input.read_positive(memory, output + 1, 1, true); }, "Unaligned controller output was accepted");
    rejects([&] { input.read_positive(memory, GuestMemory::kPhysicalBase + memory.size() - 8, 1, true); },
        "Truncated controller output was accepted");
    require(calls == 0 && packet(memory) == original, "Invalid buffer/count called the provider or changed output");
    rejects([&] { input.read_positive(memory, output, 1, true); }, "Inactive provider produced pretend controller data");
    require(time.deadlines.empty() && packet(memory) == original, "Inactive provider waited or changed output");
    state.ready = true; input.read_positive(memory, output, 1, false); const auto previous = packet(memory);
    time.waiting = [&] { state.ready = false; };
    rejects([&] { input.read_positive(memory, output, 1, true); }, "Provider loss during wait produced a successful packet");
    require(packet(memory) == previous, "Provider loss partially overwrote controller output");
    state.ready = true; state.buttons = 0x200; time.waiting = {};
    input.read_positive(memory, output, 1, false);
    require(memory.load32(output + 4) == 0x200, "Failed wait consumed a fresh sample");
    input.set_provider({}); const auto retained = packet(memory);
    rejects([&] { input.read_positive(memory, output, 1, true); }, "Missing provider produced a successful packet");
    require(packet(memory) == retained, "Missing provider overwrote output");
}
void partial_batches_and_collected_history() {
    GuestMemory memory; Time time;
    badman::InputState state{true, 0x4000, 7, 249};
    unsigned observations = 0;
    badman::Input input([&] { ++observations; return state; }, time.clock(), time.wait());
    for (std::uint32_t offset = 0; offset < 6 * 16 + 4; offset += 4) memory.store32(output + offset, 0xA5A5A5A5);
    require(input.read_positive(memory, output, 6, false) == 1 && observations == 1,
        "A six-record request manufactured multiple samples from one observation");
    for (std::uint32_t offset = 16; offset < 6 * 16 + 4; offset += 4)
        require(memory.load32(output + offset) == 0xA5A5A5A5, "Partial batch changed an unused output record or trailing guard");
    require(input.available_samples() == 0, "Partial batch did not consume its real observation");
    state.buttons = 8; time.waiting = [&] { state.buttons = 0x200; };
    require(input.read_positive(memory, output, 64, true) == 1 && time.deadlines.size() == 1 &&
        memory.load32(output) == 16683 && memory.load32(output + 4) == 0x200,
        "Empty batch did not wait for one actual new observation");
    require(input.available_samples() == 0, "Waiting batch left fabricated unread records");
    time.waiting = {};
    for (std::uint32_t i = 0; i < 6; ++i) {
        time.now += 16683333; state.buttons = 1u << i; state.analog_x = static_cast<std::uint8_t>(20 + i);
        if (i == 1) input.set_sampling_mode(1);
        require(input.poll(), "Elapsed sampling interval did not collect live input");
        require(!input.poll(), "Two polls in the same interval collected duplicate input");
    }
    require(input.available_samples() == 6, "Polling did not preserve six distinct collected records");
    require(input.read_positive(memory, output, 6, false) == 6, "Available batch incorrectly required blocking permission");
    for (std::uint32_t i = 0; i < 6; ++i) {
        const auto address = output + i * 16;
        require(memory.load32(address) == static_cast<std::uint32_t>((static_cast<std::uint64_t>(i + 2) * 16683333) / 1000) &&
            memory.load32(address + 4) == (1u << i), "Buffered history lost chronological timestamps or button changes");
        require(memory.load8(address + 8) == (i ? 20 + i : 128) && memory.load8(address + 9) == (i ? 249 : 128),
            "Changing sampling mode retroactively changed an already-collected record");
        for (unsigned byte = 10; byte < 16; ++byte)
            require(memory.load8(address + byte) == 0, "Reserved controller data bytes were not zero");
    }
    require(input.available_samples() == 0 && observations == 9, "Read resampled an already available batch");
}
void newest_snapshot_capacity_and_no_backfill() {
    GuestMemory memory; Time time;
    badman::InputState state{true, 0, 128, 128};
    badman::Input input([&] { return state; }, time.clock(), time.wait());
    for (std::uint32_t i = 0; i < 70; ++i) {
        state.buttons = i; require(input.poll(), "Live history sample was not collected");
        time.now += 16683333;
    }
    require(input.available_samples() == 64, "Controller history grew beyond its 64-record capacity");
    // Avoid collecting a 71st sample during read so the capacity boundary is exact.
    time.now -= 16683333;
    require(input.read_positive(memory, output, 64, false) == 64, "Full controller buffer returned the wrong count");
    for (std::uint32_t i = 0; i < 64; ++i)
        require(memory.load32(output + i * 16 + 4) == i + 6, "Full controller history retained an overwritten old record");
    for (std::uint32_t i = 0; i < 5; ++i) {
        time.now += 16683333; state.buttons = 100 + i; require(input.poll(), "New snapshot sample was not collected");
    }
    require(input.read_positive(memory, output, 2, false) == 2 && memory.load32(output + 4) == 103 &&
        memory.load32(output + 20) == 104 && memory.load32(output) < memory.load32(output + 16),
        "Small read did not expose the newest records in chronological order");
    require(input.available_samples() == 0, "Small read left older input to replay after newer data");
    rejects([&] { input.read_positive(memory, output, 1, false); }, "Consumed snapshot replayed older observations");
    time.now += 1000000000; state.buttons = 0x4000;
    require(input.poll() && input.available_samples() == 1 && !input.poll(),
        "Missed sampling intervals were backfilled with copied current input");
    state.ready = false;
    require(input.read_positive(memory, output, 6, false) == 1 && memory.load32(output + 4) == 0x4000,
        "Temporary provider loss discarded an already-collected genuine record");
    input.set_provider({});
    require(!input.poll() && input.available_samples() == 0, "Missing provider poll was not harmless");
    rejects([&] { input.read_positive(memory, output, 6, true); }, "Missing provider created input after polling");
}
void whole_batch_validation_and_provider_replacement() {
    GuestMemory memory; Time time;
    unsigned observations = 0; badman::InputState state{true, 1, 128, 128};
    badman::Input input([&] { ++observations; return state; }, time.clock(), time.wait());
    require(input.poll(), "Initial live sample was not collected");
    const auto original = packet(memory);
    rejects([&] { input.read_positive(memory, GuestMemory::kPhysicalBase + memory.size() - 16, 6, true); },
        "Only the returned prefix of an invalid requested buffer was validated");
    require(observations == 1 && input.available_samples() == 1 && packet(memory) == original,
        "Invalid batch pointer observed input, consumed history, or changed output");
    time.now += 16683333; state.buttons = 8; require(input.poll(), "Second live sample was not collected");
    input.set_provider([&] { ++observations; return badman::InputState{true, 0x4000, 128, 128}; });
    require(input.available_samples() == 0 && input.read_positive(memory, output, 6, false) == 1 &&
        memory.load32(output + 4) == 0x4000 && memory.load32(output) == 16683,
        "Provider replacement replayed the old provider or reset the timestamp epoch");
    input.set_provider({});
    require(input.available_samples() == 0 && !input.poll(), "Removing a provider retained unread data");
}
void clock_failures_and_timestamp_wrap() {
    GuestMemory memory; Time time; badman::InputState state{true, 0x4000, 128, 128};
    badman::Input input([&] { return state; }, time.clock(), time.wait());
    input.read_positive(memory, output, 1, false); const auto previous = packet(memory);
    --time.now;
    rejects([&] { input.read_positive(memory, output, 1, true); }, "Backward controller clock was accepted");
    require(packet(memory) == previous, "Backward clock changed controller output");
    ++time.now; time.advance = false;
    rejects([&] { input.read_positive(memory, output, 1, true); }, "An early waiter produced a fresh sample");
    require(packet(memory) == previous, "Early waiter changed controller output");
    time.advance = true; input.read_positive(memory, output, 1, true);
    require(time.deadlines.size() == 2, "Failed wait incorrectly consumed sampling state");
    Time wrapped; wrapped.now = 0;
    badman::Input long_running([&] { return state; }, wrapped.clock(), wrapped.wait());
    wrapped.now = (static_cast<std::uint64_t>(std::numeric_limits<std::uint32_t>::max()) + 42) * 1000;
    long_running.read_positive(memory, output, 1, false);
    require(memory.load32(output) == 41, "PSP 32-bit microsecond timestamp did not wrap naturally");
    rejects([&] { badman::Input inconsistent({}, wrapped.clock(), {}); }, "Half-configured input timing was accepted");
}
void scheduler_try_read_does_not_sleep_or_fabricate() {
    GuestMemory memory;Time time;badman::InputState state{true,0x4000,128,128};
    badman::Input input;input.set_timing(time.clock(),time.wait());input.set_provider([&]{return state;});
    require(input.next_observation_deadline()<=time.now,"First genuine sample was not immediately eligible");
    require(input.try_read_positive(memory,output,1)==1,"Try-read did not collect its first genuine sample");
    const auto first=packet(memory);const auto deadline=input.next_observation_deadline();
    require(deadline==1016683333&&!input.try_read_positive(memory,output,6)&&time.deadlines.empty()&&packet(memory)==first,
        "Empty try-read slept, fabricated records, changed output or consumed a pending observation");
    time.now=deadline-1;state.buttons=8;
    require(!input.try_read_positive(memory,output,64)&&packet(memory)==first,"Try-read completed before its sampling deadline");
    ++time.now;require(input.try_read_positive(memory,output,64)==1&&memory.load32(output+4)==8&&memory.load32(output)==16683,
        "Due try-read did not use one actual current provider observation");
    rejects([&]{input.set_timing(time.clock(),time.wait());},"Timing origin changed after real observations");
}
void buffered_observation_remains_eligible_at_an_earlier_clock_observation(){
    std::uint64_t now=1000000000;
    badman::Input input([]{return badman::InputState{true,0x8000,128,128};},
        [&]{return ++now;},[&](std::uint64_t deadline){now=deadline;});
    require(input.poll(),"Real provider observation was not buffered");
    const auto already_ready_at=++now;
    const auto deadline=input.next_observation_deadline();
    require(deadline<=already_ready_at,
        "A buffered actual observation became ineligible at a clock time when it was already available");
}
}
int main() {
    try {
        keyboard_buttons_and_focus(); exact_packet_modes_and_timestamps(); fresh_samples_pace_without_real_sleep();
        failures_preserve_output_and_consumption(); clock_failures_and_timestamp_wrap();
        partial_batches_and_collected_history(); newest_snapshot_capacity_and_no_backfill();
        whole_batch_validation_and_provider_replacement();
        scheduler_try_read_does_not_sleep_or_fabricate();
        buffered_observation_remains_eligible_at_an_earlier_clock_observation();
        std::cout << "Badman keyboard focus, controller ABI, bounded batches, sampling, pacing and failure tests passed\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}


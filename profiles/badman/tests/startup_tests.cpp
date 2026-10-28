#include "badman_startup.hpp"
#include "psprecomp/runtime.hpp"
#include <array>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {
using badman::StartupPolicy;
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
constexpr std::uint32_t path_address = 0x08900000u;
struct Fixture {
    psprecomp::Runtime runtime;
    psprecomp::GuestMemory &memory = runtime.memory();
    psprecomp::AllegrexContext ctx{};
    Fixture() { write_path(StartupPolicy::logo_path); }
    void write_path(const std::string &text) {
        for (std::uint32_t i = 0; i <= text.size(); ++i)
            memory.store8(path_address + i, i < text.size() ? static_cast<std::uint8_t>(text[i]) : 0);
    }
    void logo_entry() {
        ctx = {}; ctx.pc = StartupPolicy::logo_player; ctx.gpr[31] = StartupPolicy::logo_call_return; ctx.gpr[4] = path_address;
    }
};
std::array<std::uint8_t, 16> packet(std::uint32_t buttons = 0) {
    std::array<std::uint8_t, 16> result{};
    for (unsigned byte = 0; byte < 4; ++byte) result[4 + byte] = static_cast<std::uint8_t>(buttons >> (byte * 8u));
    return result;
}
std::uint32_t buttons(std::span<const std::uint8_t> data, std::size_t index = 0) {
    std::uint32_t value{};
    for (unsigned byte = 0; byte < 4; ++byte) value |= static_cast<std::uint32_t>(data[index * 16 + 4 + byte]) << (byte * 8u);
    return value;
}

void disabled_policy_changes_nothing() {
    Fixture f; StartupPolicy policy; f.logo_entry();
    require(!policy.active(), "Default startup policy is active");
    policy.observe(f.ctx, 1, f.memory, false);
    auto data = packet();
    f.memory.store32(StartupPolicy::prompt_state, 1);
    policy.filter_controller(1, StartupPolicy::logo_read_return, data, f.memory);
    policy.filter_controller(1, StartupPolicy::prompt_read_return, data, f.memory);
    require(f.ctx.gpr[5] == 0 && buttons(data) == 0, "Disabled policy changed arguments or input");
}

void logo_skip_uses_exact_call_and_alternates_presses() {
    Fixture f; StartupPolicy policy({true, false});
    // Wrong caller, existing mask, live media or another movie: untouched.
    f.logo_entry(); f.ctx.gpr[31] = 0x08800000u; policy.observe(f.ctx, 2, f.memory, false);
    require(f.ctx.gpr[5] == 0, "Another caller was armed");
    f.logo_entry(); f.ctx.gpr[5] = 0x2000u; policy.observe(f.ctx, 2, f.memory, false);
    require(f.ctx.gpr[5] == 0x2000u, "An existing skip mask was changed");
    f.logo_entry(); policy.observe(f.ctx, 2, f.memory, true);
    require(f.ctx.gpr[5] == 0, "Armed while movie resources were live");
    f.write_path("disc0:/PSP_GAME/USRDIR/data_usa/movie/other.pmf");
    f.logo_entry(); policy.observe(f.ctx, 2, f.memory, false);
    require(f.ctx.gpr[5] == 0, "Another movie was armed");
    f.write_path(StartupPolicy::logo_path);
    f.logo_entry(); policy.observe(f.ctx, 2, f.memory, false);
    require(f.ctx.gpr[5] == StartupPolicy::cross && policy.counters().logo_skips_armed == 1, "Logo call was not armed");
    auto other = packet(); policy.filter_controller(3, StartupPolicy::logo_read_return, other, f.memory);
    require(buttons(other) == 0, "Another thread's read was pressed");
    auto menu = packet(); policy.filter_controller(2, StartupPolicy::prompt_read_return, menu, f.memory);
    require(buttons(menu) == 0, "A menu read was pressed while the logo was armed");
    unsigned pressed = 0;
    for (unsigned read = 0; read < 40; ++read) {
        auto data = packet(0x0010u); policy.filter_controller(2, StartupPolicy::logo_read_return, data, f.memory);
        const bool expected = read % 2 == 0 && pressed < StartupPolicy::max_presses;
        require((buttons(data) & 0x0010u) && ((buttons(data) & StartupPolicy::cross) != 0) == expected,
            "Logo press pattern was wrong or real input was lost");
        if (expected) ++pressed;
    }
    require(policy.counters().logo_presses == StartupPolicy::max_presses, "Logo presses were not bounded");
    f.ctx = {}; f.ctx.pc = StartupPolicy::logo_call_return; policy.observe(f.ctx, 3, f.memory, false);
    require(policy.counters().logo_returns == 0, "Another thread ended the logo policy");
    policy.observe(f.ctx, 2, f.memory, false);
    require(policy.counters().logo_returns == 1, "Logo return was not observed");
    auto after = packet(); policy.filter_controller(2, StartupPolicy::logo_read_return, after, f.memory);
    require(buttons(after) == 0, "Input was pressed after the logo returned");
}

void load_prompt_answer_follows_profile_and_state() {
    for (const bool records : {false, true}) {
        Fixture f; StartupPolicy policy({false, true}); policy.set_profile_has_records(records);
        const auto expected = records ? StartupPolicy::cross : StartupPolicy::circle;
        std::array<std::uint8_t, 32> data{};
        f.memory.store32(StartupPolicy::prompt_state, 0);
        policy.filter_controller(1, StartupPolicy::prompt_read_return, data, f.memory);
        require(buttons(data) == 0, "Prompt answered before it was waiting");
        f.memory.store32(StartupPolicy::prompt_state, 1);
        data = {}; policy.filter_controller(1, 0x08800000u, data, f.memory);
        require(buttons(data) == 0, "Another controller read answered the prompt");
        data = {}; policy.filter_controller(1, StartupPolicy::prompt_read_return, data, f.memory);
        require(buttons(data, 0) == expected && buttons(data, 1) == expected, "Prompt was not answered in every returned sample");
        data = {}; policy.filter_controller(1, StartupPolicy::prompt_read_return, data, f.memory);
        require(buttons(data) == 0, "No release was observed between prompt presses");
        f.memory.store32(StartupPolicy::prompt_state, 2);
        data = {}; policy.filter_controller(1, StartupPolicy::prompt_read_return, data, f.memory);
        f.memory.store32(StartupPolicy::prompt_state, 1);
        for (unsigned read = 0; read < 4; ++read) {
            data = {}; policy.filter_controller(1, StartupPolicy::prompt_read_return, data, f.memory);
            require(buttons(data) == 0, "Prompt policy pressed again after the game advanced");
        }
        require(policy.counters().prompt_presses == 1 && policy.counters().prompt_button == expected, "Prompt counters wrong");
    }
    Fixture f; StartupPolicy policy({false, true}); f.memory.store32(StartupPolicy::prompt_state, 1);
    for (unsigned read = 0; read < 40; ++read) {
        auto data = packet(); policy.filter_controller(1, StartupPolicy::prompt_read_return, data, f.memory);
    }
    require(policy.counters().prompt_presses == StartupPolicy::max_presses, "Unanswered prompt presses were not bounded");
}
}

int main() {
    try {
        disabled_policy_changes_nothing();
        logo_skip_uses_exact_call_and_alternates_presses();
        load_prompt_answer_follows_profile_and_state();
        std::cout << "Startup policy logo-skip and load-prompt tests passed\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}

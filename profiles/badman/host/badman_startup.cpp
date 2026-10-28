#include "badman_startup.hpp"
#include <string>

namespace badman {
namespace {
bool guest_string_equals(const psprecomp::GuestMemory &memory, std::uint32_t address, const char *expected) {
    for (std::uint32_t offset = 0;; ++offset) {
        const auto at = static_cast<std::uint64_t>(address) + offset;
        if (at > 0xFFFFFFFFu || !memory.contains(static_cast<std::uint32_t>(at), 1)) return false;
        const auto value = memory.load8(static_cast<std::uint32_t>(at));
        if (value != static_cast<std::uint8_t>(expected[offset])) return false;
        if (value == 0) return true;
    }
}
// Press on alternate reads so the game always observes a release edge
// between presses. Buttons are the little-endian word at SceCtrlData+4.
void press(std::span<std::uint8_t> packets, std::uint32_t button) {
    for (std::size_t offset = 0; offset + 16 <= packets.size(); offset += 16)
        for (unsigned byte = 0; byte < 4; ++byte)
            packets[offset + 4 + byte] |= static_cast<std::uint8_t>(button >> (byte * 8u));
}
}

void StartupPolicy::observe(psprecomp::AllegrexContext &ctx, std::int32_t uid, const psprecomp::GuestMemory &memory,
                            bool live_media) {
    if (!options_.skip_logo) return;
    if (logo_armed_) {
        if (uid == logo_uid_ && ctx.pc == logo_call_return) {
            logo_armed_ = false; ++counters_.logo_returns;
            if (live_media) ++counters_.logo_returns_with_live_media;
        }
        return;
    }
    // Exact original call: return address, unskippable mask 0, a2 0 and the
    // logo movie path. Any other caller or argument is left untouched.
    if (ctx.pc != logo_player || ctx.gpr[31] != logo_call_return || ctx.gpr[5] != 0 || ctx.gpr[6] != 0 ||
        uid <= 0 || live_media || !guest_string_equals(memory, ctx.gpr[4], logo_path))
        return;
    ctx.gpr[5] = cross;
    logo_armed_ = true; logo_uid_ = uid; logo_reads_ = 0; ++counters_.logo_skips_armed;
}

void StartupPolicy::filter_controller(std::int32_t uid, std::uint32_t return_pc, std::span<std::uint8_t> packets,
                                      const psprecomp::GuestMemory &memory) {
    if (packets.size() < 16) return;
    if (logo_armed_ && uid == logo_uid_ && return_pc == logo_read_return && packets.size() == 16) {
        if (counters_.logo_presses < max_presses && (logo_reads_++ % 2u) == 0) {
            press(packets, cross); ++counters_.logo_presses;
        }
        return;
    }
    if (!options_.answer_load_prompt || prompt_done_ || return_pc != prompt_read_return) return;
    if (!memory.contains(prompt_state, 4)) return;
    if (memory.load32(prompt_state) != 1) {
        // Once answered, any state change ends the policy for this process.
        if (counters_.prompt_presses) prompt_done_ = true;
        return;
    }
    if (counters_.prompt_presses >= max_presses) { prompt_done_ = true; return; }
    if ((prompt_reads_++ % 2u) != 0) return;
    const auto button = profile_has_records_ ? cross : circle;
    press(packets, button); ++counters_.prompt_presses; counters_.prompt_button = button;
}
}

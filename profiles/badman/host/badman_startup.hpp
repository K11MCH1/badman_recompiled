#pragma once
#include "psprecomp/allegrex_context.hpp"
#include "psprecomp/guest_memory.hpp"
#include <cstdint>
#include <span>

namespace badman {
// Explicit player options for the ULUS10456 boot sequence. Both act only
// through the game's own code: an argument its logo player already accepts
// and genuine controller packets the game reads. No guest state is written
// and no game result is invented; the game runs its normal cancel/load paths.
class StartupPolicy {
public:
    struct Options {
        bool skip_logo{};          // Launcher "Skip intro and logos".
        bool answer_load_prompt{}; // Automatic profile: answer the boot Memory Stick prompt.
    };
    struct Counters {
        unsigned logo_skips_armed{}, logo_presses{}, logo_returns{}, logo_returns_with_live_media{};
        unsigned prompt_presses{};
        std::uint32_t prompt_button{};
    };
    // 088A2248 calls the logo player with a1 = skip-button mask 0 (unskippable).
    static constexpr std::uint32_t logo_player=0x088C7ACCu, logo_call_return=0x088A2250u;
    static constexpr std::uint32_t logo_read_return=0x088C855Cu; // ReadBufferPositive(buffer, 1) in the player.
    // FUN_0882F480 aggregates ReadBufferPositive(buffer, 6) for menus; the boot
    // prompt FUN_088E5AA4 waits for Cross/Circle while its state word is 1.
    static constexpr std::uint32_t prompt_read_return=0x0882F51Cu, prompt_state=0x089C89FCu;
    static constexpr std::uint32_t cross=0x4000u, circle=0x2000u;
    static constexpr const char *logo_path="disc0:/PSP_GAME/USRDIR/data_usa/movie/logo.pmf";
    static constexpr unsigned max_presses=8;

    StartupPolicy() = default;
    explicit StartupPolicy(Options options) : options_(options) {}
    bool active() const { return options_.skip_logo || options_.answer_load_prompt; }
    const Options &options() const { return options_; }
    // Cross loads an existing native profile; Circle skips loading when there
    // is none. Damaged records still choose Cross so the real error is shown.
    void set_profile_has_records(bool value) { profile_has_records_ = value; }
    // Called at guest function entries and continuations, before guest code runs.
    void observe(psprecomp::AllegrexContext &ctx, std::int32_t uid, const psprecomp::GuestMemory &memory,
                 bool live_media);
    // Called with genuine packets before they are copied to the guest buffer.
    void filter_controller(std::int32_t uid, std::uint32_t return_pc, std::span<std::uint8_t> packets,
                           const psprecomp::GuestMemory &memory);
    const Counters &counters() const { return counters_; }
private:
    Options options_;
    Counters counters_;
    bool profile_has_records_{}, logo_armed_{}, prompt_done_{};
    std::int32_t logo_uid_{};
    unsigned logo_reads_{}, prompt_reads_{};
};
}

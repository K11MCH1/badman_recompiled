#pragma once
#include "badman_scene.hpp"
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <vector>

namespace badman {
// What the GPU presenters (Direct3D 11 on Windows, OpenGL ES on Android) share:
// a recorded scene as triangles in native pixels, the check of a native-size
// re-render against the exact frame, and the pacing of in-between frames.

struct SceneGpuVertex { float x, y, u, v; std::uint32_t color; }; // Color: PSP ABGR, read as RGBA bytes.
struct SceneBatch {
    std::uint32_t first{}, count{};
    std::int32_t texture{-1};            // Index into Scene::textures.
    std::uint32_t flags{}, blend_key{};  // Flags: 1 textured, 2 bilinear, 4 texture alpha.
    std::array<float, 4> scissor{};      // Native pixels; a wider view reaches past 0 and 480.
    std::uint32_t blend_factor{};
    bool operator==(const SceneBatch &other) const {
        return texture == other.texture && flags == other.flags && blend_key == other.blend_key &&
               scissor == other.scissor && blend_factor == other.blend_factor;
    }
};
// Blend key: bit 0 blend on, bits 1..11 the GE blend mode (source factor in
// bits 0..3, destination in 4..7, equation in 8..10), bits 12..15 the RGBA
// write mask (1 red, 2 green, 4 blue, 8 alpha).
inline bool scene_blend_on(std::uint32_t key) { return (key & 1u) != 0; }
inline std::uint32_t scene_blend_mode(std::uint32_t key) { return (key >> 1) & 0x7FFu; }
inline std::uint32_t scene_write_mask(std::uint32_t key) { return (key >> 12) & 15u; }

// Builds the scene's triangles at interpolation position t (1 = this frame),
// merging neighbouring draws with the same state into batches. smooth samples
// every texture bilinearly (the player's filter; never for the exact check).
// In a wide view, placement moves 2D draws and widens full-width scissors.
// Returns false when a draw uses state the presenters cannot reproduce.
bool build_scene_geometry(std::vector<SceneGpuVertex> &vertices, std::vector<SceneBatch> &batches, const Scene &scene,
                          const SceneMotion *moving, float t, bool smooth, const WideView *view = nullptr,
                          const std::vector<ScenePlacement> *placement = nullptr);

// Pixels of a 480x272 re-render (RGBA bytes, rows pitch bytes apart, top row
// first unless bottom_up) that differ from the exact AARRGGBB frame by more
// than 40 in a color channel. Dither and rounding stay below that.
std::uint64_t scene_mismatch(const std::uint8_t *rgba, std::size_t pitch, bool bottom_up, const std::uint32_t *exact);
// A scene is used only if at most 0.5% of the pixels differ.
constexpr std::uint64_t scene_mismatch_limit = 480u * 272u / 200u;

// Timeline of matching scenes for in-between frames. Each scene carries the
// steady time of the vblank that latched it; drawing follows a display time
// one frame (plus observed lateness) behind the refresh, so every refresh
// lands at an exact fraction between two frames' true times. The display
// clock never jumps: after a late frame (a hold) it resumes where it stopped
// and catches up gently. Without in-between frames only the newest scene is kept.
class ScenePacer {
public:
    using Clock = std::chrono::steady_clock;
    struct Entry {
        std::shared_ptr<const Scene> scene;
        SceneMotion motion;   // From the entry before; invalid after a cut.
        Clock::time_point time;
    };
    struct Pick {
        const Entry *entry{};  // Null: show the exact frame.
        const Entry *before{}; // The entry interpolated from, when t is between frames.
        float t{1};
        bool in_between{};
    };
    explicit ScenePacer(bool interpolate) : interpolate_(interpolate) {}
    // A newly displayed frame, with its scene only if that re-rendered to the exact frame.
    void accept(std::shared_ptr<const Scene> scene, Clock::time_point now);
    // What to draw at a display refresh; refresh is the time right after the
    // previous present returned, taken before any per-scene work.
    Pick pick(Clock::time_point refresh);
    bool scene_mode() const noexcept { return scene_mode_; }
    // True while in-between frames are due: draw on every display refresh.
    bool animating() const noexcept { return animating_; }
    double interval() const noexcept { return interval_; }
    double lateness() const noexcept { return lateness_; }
    std::uint64_t holds() const noexcept { return holds_; }
    std::uint64_t paired() const noexcept { return paired_; }
    std::uint64_t moving() const noexcept { return moving_; }
private:
    bool interpolate_{};
    std::deque<Entry> timeline_;
    bool scene_mode_{}, animating_{};
    Clock::time_point display_clock_{}, last_refresh_{};
    bool clock_running_{}, clock_held_{};
    double interval_{1.0 / 30.0}, lateness_{0.004};
    std::uint64_t holds_{}, paired_{}, moving_{};
};
}

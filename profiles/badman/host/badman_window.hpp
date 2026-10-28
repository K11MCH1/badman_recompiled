#pragma once
#include "psprecomp/guest_memory.hpp"
#include "badman_input.hpp"
#include "badman_osk.hpp"
#include "badman_scene.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

namespace badman {
// Count accepted guest scanout submissions, not repeated host repaints.
class FrameRateCounter {
public:
    void update(std::uint64_t now_nanoseconds, bool submitted);
    double rate() const noexcept { return rate_; }
private:
    std::optional<std::uint64_t> start_;
    std::uint64_t frames_{};
    double rate_{};
};
// Offscreen conversion shared with synthetic tests. Output words are AARRGGBB
// (BGRA bytes on the little-endian Windows DIB), in visible top-to-bottom order.
std::vector<std::uint32_t> decode_framebuffer_bgra(const psprecomp::GuestMemory &memory,
    std::uint32_t address, std::uint32_t stride, std::uint32_t format,
    std::uint32_t width, std::uint32_t height);
// The same in two steps: the validated raw VRAM bytes of a framebuffer (live
// guest memory; copy before the guest continues), then their conversion.
std::span<const std::uint8_t> framebuffer_bytes(const psprecomp::GuestMemory &memory,
    std::uint32_t address, std::uint32_t stride, std::uint32_t format,
    std::uint32_t width, std::uint32_t height);
std::vector<std::uint32_t> decode_framebuffer(std::span<const std::uint8_t> bytes, std::uint32_t stride,
    std::uint32_t format, std::uint32_t width, std::uint32_t height);

// Single-thread Win32 presenter. It only displays the supplied guest VRAM;
// command execution, display latching and frame timing remain with the host.
class Window {
public:
    // Hidden creation supports native-control tests without desktop activation.
    Window(std::string_view title, std::function<void()> on_close, bool visible = true);
    ~Window();
    Window(const Window &) = delete;
    Window &operator=(const Window &) = delete;
    void pump();
    InputState input_state();
    void set_utility_message(std::string_view text);
    void set_osk_dialog(const std::optional<Osk::Dialog> &dialog);
    std::optional<Osk::Response> take_osk_response();
    // scene, when given, holds the draws behind a newly submitted frame; the
    // GPU presenter can then show in-between frames at the display's refresh.
    void present(const psprecomp::GuestMemory &memory, std::uint32_t address,
        std::uint32_t stride, std::uint32_t format, std::uint32_t width, std::uint32_t height,
        bool submitted = false, std::shared_ptr<const Scene> scene = nullptr);
    // True when the presenter will use recorded scenes (interpolation or the
    // wider view enabled).
    bool wants_scenes() const noexcept;
    // Native pixels past each screen edge that scenes should also record for
    // the wider view; 0 when it is off.
    double scene_margin() const noexcept;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

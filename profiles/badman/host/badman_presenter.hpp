#pragma once
#include "badman_scene.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace badman {
// Windows GPU scanout for the game window. A dedicated thread owns a
// Direct3D 11 flip-model swap chain on a child window that covers the game
// window's client area. The game thread only hands over finished frames;
// scaling, filtering, the FPS label and vsynced presentation at the display's
// own refresh rate run on that thread instead of the game's.
//
// With interpolation on, each frame may come with the Scene that drew it. A
// scene that re-renders to the exact frame at native size is then drawn at
// the window's resolution, and between two such frames the presenter draws
// in-between positions on every display refresh. The game still runs its 30
// frames per second; this adds one game frame of display latency. Frames
// without a matching scene (movies, menus drawn by the CPU) are shown exactly.
//
// With a wide aspect, dungeon scenes whose level extent is known are drawn
// wider than 480 pixels, up to that shape: the camera sprites the GE culled
// past the screen edges fill the sides, the view stays inside the level, and
// the 2D layer keeps to the screen edges (see wide_view and wide_placement).
// Everything else keeps the PSP's shape with bars.
class Presenter {
public:
    enum class Scaling { integer, fit, stretch };
    struct Statistics {
        std::uint64_t submitted{}, presented{};
        std::uint64_t scenes{}, matching_scenes{}, interpolated{}; // Rendered in-between frames.
    };
    // parent is the game window's HWND, created on the calling thread. Throws
    // psprecomp::Error when no hardware Direct3D 11 swap chain is available;
    // the caller then keeps its GDI composition.
    Presenter(void *parent, Scaling scaling, bool smooth, bool interpolate = false, double wide_aspect = 0);
    ~Presenter();
    Presenter(const Presenter &) = delete;
    Presenter &operator=(const Presenter &) = delete;
    // Copies one frame of AARRGGBB words, top row first. label is drawn over
    // the picture's top-left corner; empty draws none. scene, if any, is the
    // recording of the draws behind this frame.
    void submit(const std::uint32_t *pixels, std::uint32_t width, std::uint32_t height, std::wstring_view label,
                std::shared_ptr<const Scene> scene = nullptr);
    // Replaces only the label; the picture and scene stay as they are.
    void set_label(std::wstring_view label);
    // Call from the parent's thread after its client area changed.
    void resize();
    // Hidden while the parent paints native dialogs with GDI.
    void set_visible(bool visible);
    bool visible() const noexcept;
    bool interpolating() const noexcept;
    // Widest native picture width of the wider view (0 when off). Stretch
    // scaling turns it off.
    double wide_width() const noexcept;
    // True once the presentation thread stopped on an error (for example a
    // removed device). The caller must fall back to GDI; error() says why.
    bool failed() const noexcept;
    std::string error() const;
    std::string description() const;
    Statistics statistics() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

#pragma once
#include "badman_scene.hpp"
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

struct ANativeWindow;

namespace badman {
// OpenGL ES 3 scanout for the Android app's SurfaceView, the counterpart of
// the Windows Direct3D 11 presenter. A dedicated thread owns the EGL context
// and the surface; the game thread only hands over finished frames.
//
// It shows the exact frame. With in-between frames on, each frame may come
// with the Scene that drew it: a scene that re-renders to the exact frame at
// native size is drawn at the surface's resolution, and between two such
// frames the presenter draws in-between positions on every display refresh
// (up to the panel's rate, 144 Hz on the test phone), paced by ScenePacer.
// The game keeps its own 30 frames per second; this adds about one game
// frame of display latency. Frames without a matching scene (movies, CPU-drawn
// screens) are shown exactly.
class AndroidPresenter {
public:
    struct Statistics {
        std::uint64_t submitted{}, presented{};
        std::uint64_t scenes{}, matching_scenes{}, interpolated{};
    };
    AndroidPresenter(bool interpolate, bool smooth);
    ~AndroidPresenter();
    AndroidPresenter(const AndroidPresenter &) = delete;
    AndroidPresenter &operator=(const AndroidPresenter &) = delete;
    // Takes over the caller's surface reference (nullptr detaches). Returns
    // once the presentation thread no longer uses the previous surface, as
    // Android requires before surfaceDestroyed returns.
    void set_surface(ANativeWindow *surface);
    void set_smooth(bool smooth);
    // One raw guest frame (converted on the presentation thread) and the scene behind it.
    void submit(std::vector<std::uint8_t> raw, std::uint32_t stride, std::uint32_t format,
                std::uint32_t width, std::uint32_t height, std::shared_ptr<const Scene> scene);
    bool interpolating() const noexcept;
    // True once the thread stopped on an error; error() says why.
    bool failed() const noexcept;
    std::string error() const;
    Statistics statistics() const;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}

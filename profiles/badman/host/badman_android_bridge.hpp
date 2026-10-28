#pragma once
#include "badman_input.hpp"
#include "badman_osk.hpp"
#include "badman_scene.hpp"
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <vector>

struct ANativeWindow;

namespace badman {
class AndroidPresenter;
// State shared by the Android app's UI thread (through JNI) and the game
// thread's Window. The app owns the surface. Frames are handed to a scanout
// thread, so scaling and waiting for a free surface buffer never cost the
// game thread its frame time. With in-between frames on (BADMAN_INTERPOLATION=1)
// the OpenGL ES presenter (AndroidPresenter) shows them instead; the CPU
// scanout remains the fallback if it fails.
class AndroidBridge {
public:
    static AndroidBridge &instance();
    // The app process hosts a game surface; without it the Window refuses to open.
    void attach();
    bool attached() const;

    // UI thread side.
    void set_surface(ANativeWindow *surface, int width, int height); // nullptr detaches
    void set_paused(bool paused);
    void set_input(std::uint32_t buttons, std::uint8_t analog_x, std::uint8_t analog_y);
    void set_sharp(bool sharp);
    void request_close();
    std::string utility_message() const;
    std::optional<Osk::Dialog> osk_dialog() const;
    void respond_osk(Osk::Response response);

    // Game thread side.
    // Blocks while the app is in the background; true once a close was requested.
    bool wait_while_paused();
    InputState input() const;
    void set_utility_message(std::string text);
    void set_osk_dialog(const std::optional<Osk::Dialog> &dialog);
    std::optional<Osk::Response> take_osk_response();
    // Queues one raw guest framebuffer (copied) for conversion and scanout;
    // an unchanged frame without a scene is ignored. scene: the draws behind a new frame.
    void post(std::span<const std::uint8_t> bytes, std::uint32_t stride, std::uint32_t format,
              std::uint32_t width, std::uint32_t height, std::shared_ptr<const Scene> scene = nullptr);
    // A game frame was submitted: tells Android's performance hint session how
    // long the game thread worked for it (Android 13+), so the system can give
    // that thread enough CPU speed for a steady 30 frames per second.
    void frame_finished();
    // True when the presenter draws in-between frames from recorded scenes.
    bool wants_scenes() const;

private:
    AndroidBridge() = default;
    bool presenting() const;
    void scanout();
    void draw(const std::vector<std::uint32_t> &pixels, std::uint32_t width, std::uint32_t height);
    struct Hint;
    Hint *hint_{}; // Game thread only.

    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool attached_{}, paused_{}, close_{};
    std::uint32_t buttons_{};
    std::uint8_t analog_x_{128}, analog_y_{128};
    std::string message_;
    std::optional<Osk::Dialog> osk_;
    std::optional<Osk::Response> osk_response_;

    // Latest frame, handed from the game thread to the scanout thread.
    std::mutex frame_mutex_;
    std::condition_variable frame_ready_;
    std::vector<std::uint8_t> latest_;
    std::uint32_t latest_stride_{}, latest_format_{}, latest_width_{}, latest_height_{};
    bool fresh_{}, repaint_{};
    std::thread scanout_;
    // Created once in attach(); stays for the life of the process.
    std::unique_ptr<AndroidPresenter> presenter_;
    bool fallback_reported_{};

    // The surface has its own lock, held only by the scanout thread while drawing.
    std::mutex surface_mutex_;
    ANativeWindow *surface_{};
    bool sharp_{true};
    int surface_width_{}, surface_height_{};
    int buffer_width_{}, buffer_height_{};
};
}

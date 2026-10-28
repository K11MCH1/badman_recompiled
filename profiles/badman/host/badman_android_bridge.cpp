#include "badman_android_bridge.hpp"
#include "badman_android_presenter.hpp"
#include "badman_window.hpp"
#include "badman_workers.hpp"
#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <android/native_window.h>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string_view>
#include <ctime>
#include <dlfcn.h>
#include <unistd.h>

namespace badman {
AndroidBridge &AndroidBridge::instance() {
    // Never destroyed: the scanout thread may still run while the process exits.
    static auto *bridge = new AndroidBridge;
    return *bridge;
}
void AndroidBridge::attach() {
    std::lock_guard lock(mutex_);
    if (attached_) return;
    attached_ = true;
    const auto text = [](const char *name) { const auto value = std::getenv(name); return std::string_view(value ? value : ""); };
    // In-between frames need the OpenGL ES presenter; otherwise the CPU scanout shows frames.
    if (text("BADMAN_INTERPOLATION") == "1" && text("BADMAN_ANDROID_PRESENTER") != "cpu") {
        std::lock_guard surface(surface_mutex_);
        presenter_ = std::make_unique<AndroidPresenter>(true, !sharp_);
    }
    scanout_ = std::thread([this] { scanout(); });
    scanout_.detach();
}
bool AndroidBridge::attached() const { std::lock_guard lock(mutex_); return attached_; }
bool AndroidBridge::presenting() const { return presenter_ && !presenter_->failed(); }
bool AndroidBridge::wants_scenes() const { return presenter_ && presenter_->interpolating(); }

void AndroidBridge::set_surface(ANativeWindow *surface, int width, int height) {
    // The presenter takes its own reference; this one stays for the CPU fallback.
    if (presenter_) {
        if (surface) ANativeWindow_acquire(surface);
        presenter_->set_surface(surface);
    }
    std::lock_guard lock(surface_mutex_);
    if (surface_) ANativeWindow_release(surface_);
    surface_ = surface; // The caller's reference from ANativeWindow_fromSurface.
    surface_width_ = width; surface_height_ = height;
    buffer_width_ = buffer_height_ = 0;
    std::lock_guard frame(frame_mutex_);
    repaint_ = true; // A new surface shows the latest frame even while the game is paused.
    frame_ready_.notify_one();
}
void AndroidBridge::set_paused(bool paused) {
    { std::lock_guard lock(mutex_); paused_ = paused; }
    changed_.notify_all();
}
void AndroidBridge::set_input(std::uint32_t buttons, std::uint8_t analog_x, std::uint8_t analog_y) {
    std::lock_guard lock(mutex_);
    buttons_ = buttons; analog_x_ = analog_x; analog_y_ = analog_y;
}
void AndroidBridge::set_sharp(bool sharp) {
    if (presenter_) presenter_->set_smooth(!sharp);
    std::lock_guard lock(surface_mutex_);
    sharp_ = sharp; buffer_width_ = buffer_height_ = 0;
    std::lock_guard frame(frame_mutex_);
    repaint_ = true;
    frame_ready_.notify_one();
}
void AndroidBridge::request_close() {
    { std::lock_guard lock(mutex_); close_ = true; }
    changed_.notify_all();
}
std::string AndroidBridge::utility_message() const { std::lock_guard lock(mutex_); return message_; }
std::optional<Osk::Dialog> AndroidBridge::osk_dialog() const { std::lock_guard lock(mutex_); return osk_; }
void AndroidBridge::respond_osk(Osk::Response response) {
    std::lock_guard lock(mutex_);
    if (osk_ && osk_->generation == response.generation) osk_response_ = std::move(response);
}

bool AndroidBridge::wait_while_paused() {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return !paused_ || close_; });
    return close_;
}
InputState AndroidBridge::input() const {
    std::lock_guard lock(mutex_);
    // Nothing reaches the game while the app is in the background.
    if (paused_) return {true, 0, 128, 128};
    return {true, buttons_, analog_x_, analog_y_};
}
void AndroidBridge::set_utility_message(std::string text) { std::lock_guard lock(mutex_); message_ = std::move(text); }
void AndroidBridge::set_osk_dialog(const std::optional<Osk::Dialog> &dialog) {
    std::lock_guard lock(mutex_);
    if (!dialog || !osk_ || osk_->generation != dialog->generation) osk_response_.reset();
    osk_ = dialog;
}
std::optional<Osk::Response> AndroidBridge::take_osk_response() {
    std::lock_guard lock(mutex_);
    auto response = std::move(osk_response_);
    osk_response_.reset();
    return response;
}

void AndroidBridge::post(std::span<const std::uint8_t> bytes, std::uint32_t stride, std::uint32_t format,
                         std::uint32_t width, std::uint32_t height, std::shared_ptr<const Scene> scene) {
    // Development: BADMAN_PRESENT_EVERY_VBLANK redraws unchanged frames too
    // (about 60 presents a second), to study how the phone governs that rate.
    static const bool every_vblank = std::getenv("BADMAN_PRESENT_EVERY_VBLANK") != nullptr;
    std::lock_guard lock(frame_mutex_);
    // Every frame with a scene counts for the presenter's timeline.
    if (!every_vblank && !scene && stride == latest_stride_ && format == latest_format_ && width == latest_width_ &&
        height == latest_height_ && std::equal(bytes.begin(), bytes.end(), latest_.begin(), latest_.end())) return;
    latest_.assign(bytes.begin(), bytes.end());
    latest_stride_ = stride; latest_format_ = format; latest_width_ = width; latest_height_ = height;
    if (presenting()) {
        // The presenter converts the raw frame on its own thread, as the scanout thread would.
        presenter_->submit(latest_, stride, format, width, height, std::move(scene));
        return;
    }
    if (presenter_ && !fallback_reported_) {
        fallback_reported_ = true;
        std::cout << presenter_->error() << "; using CPU scanout" << std::endl;
        std::lock_guard surface(surface_mutex_);
        buffer_width_ = buffer_height_ = 0;
    }
    fresh_ = true;
    frame_ready_.notify_one();
}

// Android's performance hint API (API 33+), loaded at run time because the app
// also supports Android 10-12, where frames simply run without the hint.
struct AndroidBridge::Hint {
    using GetManager = void *(*)();
    using CreateSession = void *(*)(void *, const std::int32_t *, std::size_t, std::int64_t);
    using Report = int (*)(void *, std::int64_t);
    void *session{};
    Report report{};
    std::chrono::steady_clock::time_point last{};
    Hint() {
        auto *library = dlopen("libandroid.so", RTLD_NOW | RTLD_LOCAL);
        if (!library) return;
        const auto manager = reinterpret_cast<GetManager>(dlsym(library, "APerformanceHint_getManager"));
        const auto create = reinterpret_cast<CreateSession>(dlsym(library, "APerformanceHint_createSession"));
        report = reinterpret_cast<Report>(dlsym(library, "APerformanceHint_reportActualWorkDuration"));
        if (!manager || !create || !report || !manager()) return;
        // The game thread and the raster helpers it waits for at each flush.
        std::vector<std::int32_t> threads{static_cast<std::int32_t>(gettid())};
        for (const auto id : WorkerPool::thread_ids()) threads.push_back(id);
        session = create(manager(), threads.data(), threads.size(), 33333333); // The game's own 30 frames per second.
        last = std::chrono::steady_clock::now();
    }
};
void AndroidBridge::frame_finished() {
    // Development: BADMAN_PERFORMANCE_HINT=0 leaves the clocks to the system.
    static const bool disabled = [] {
        const auto value = std::getenv("BADMAN_PERFORMANCE_HINT");
        return value && std::string_view(value) == "0";
    }();
    if (disabled) return;
    if (!hint_) hint_ = new Hint; // Lives as long as the game thread's process.
    if (!hint_->session) return;
    // Wall time between game frames. The game thread's own CPU time would
    // leave out its waits on the raster helpers and invite lower clocks; a
    // steady 30 frames per second meets the target and any late frame is over it.
    const auto now = std::chrono::steady_clock::now();
    const auto interval = std::chrono::duration_cast<std::chrono::nanoseconds>(now - hint_->last).count();
    hint_->last = now;
    if (interval > 0) hint_->report(hint_->session, std::min<std::int64_t>(interval, 200000000));
}
void AndroidBridge::scanout() {
    std::vector<std::uint8_t> raw;
    for (;;) {
        std::uint32_t stride{}, format{}, width{}, height{};
        {
            std::unique_lock lock(frame_mutex_);
            frame_ready_.wait(lock, [this] { return fresh_ || repaint_; });
            fresh_ = repaint_ = false;
            if (latest_.empty()) continue;
            raw = latest_; stride = latest_stride_; format = latest_format_; width = latest_width_; height = latest_height_;
        }
        draw(decode_framebuffer(raw, stride, format, width, height), width, height);
    }
}
void AndroidBridge::draw(const std::vector<std::uint32_t> &pixels, std::uint32_t width, std::uint32_t height) {
    std::lock_guard lock(surface_mutex_);
    if (!surface_ || presenting()) return;
    // Sharp scaling: repeat each pixel by the smallest whole factor that covers
    // the surface, so the compositor's own filtered scale is a slight shrink.
    unsigned factor = 1;
    if (sharp_ && surface_width_ > 0 && surface_height_ > 0) {
        const auto fit = std::min(static_cast<double>(surface_width_) / width, static_cast<double>(surface_height_) / height);
        factor = std::clamp(static_cast<unsigned>(std::ceil(fit - 0.01)), 1u, 6u);
    }
    const auto w = static_cast<int>(width * factor), h = static_cast<int>(height * factor);
    if (w != buffer_width_ || h != buffer_height_) {
        if (ANativeWindow_setBuffersGeometry(surface_, w, h, WINDOW_FORMAT_RGBX_8888) != 0) return;
        buffer_width_ = w; buffer_height_ = h;
    }
    ANativeWindow_Buffer buffer{};
    if (ANativeWindow_lock(surface_, &buffer, nullptr) != 0) return;
    if (buffer.width == w && buffer.height == h && buffer.stride >= w) {
        auto *out = static_cast<std::uint32_t *>(buffer.bits);
        for (std::uint32_t y = 0; y < height; ++y) {
            auto *row = out + static_cast<std::size_t>(y * factor) * buffer.stride;
            const auto *in = pixels.data() + static_cast<std::size_t>(y) * width;
            for (std::uint32_t x = 0; x < width; ++x) {
                // AARRGGBB words to RGBX bytes (R first in memory).
                const auto p = in[x];
                const auto rgbx = 0xFF000000u | ((p & 0xFFu) << 16) | (p & 0xFF00u) | ((p >> 16) & 0xFFu);
                std::fill_n(row + x * factor, factor, rgbx);
            }
            for (unsigned repeat = 1; repeat < factor; ++repeat)
                std::memcpy(row + static_cast<std::size_t>(repeat) * buffer.stride, row, static_cast<std::size_t>(w) * 4);
        }
    }
    ANativeWindow_unlockAndPost(surface_);
}
}

#include "badman_host.hpp"
#include "psprecomp/common.hpp"
#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <thread>

namespace badman {
using namespace psprecomp;
void Host::enable_window() {
    if (!window_) window_ = std::make_unique<Window>("Badman Recompiled",
        [this] { window_closed_ = true; if (!runtime_.stopped()) runtime_.stop("User closed game window"); });
    set_input_provider([this] { return window_->input_state(); });
    ge_.set_scene_recording(window_->wants_scenes(), window_->scene_margin());
    refresh_savedata_dialog();
}
void Host::wait_for_window_close() {
    if (!window_) return;
    while (!window_closed_) {
        window_->pump();
        if (window_closed_) break;
        const auto &frame = display_.current;
        if (frame.address) window_->present(runtime_.memory(), frame.address, frame.stride,
            frame.format, display_.width, display_.height);
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}
void Host::present_display(bool submitted) {
    if (window_) window_->pump();
    if (runtime_.stopped()) return;
    input_.poll();
    refresh_savedata_dialog();
    if (frame_observer_) frame_observer_(runtime_.memory(), display_);
    if (!window_) return;
    const auto &frame = display_.current;
    // Only a newly latched frame carries the draws recorded for its buffer,
    // stamped with the steady time of the vblank that latched it (now, less
    // how late this service ran), so the presenter can pace evenly.
    std::shared_ptr<const Scene> scene;
    if (submitted && frame.address) {
        auto latched = std::chrono::steady_clock::now();
        if (latch_lateness_ns_ < 100000000ull) latched -= std::chrono::nanoseconds(latch_lateness_ns_);
        auto taken = ge_.take_scene(frame.address, std::chrono::duration_cast<std::chrono::nanoseconds>(latched.time_since_epoch()).count());
        if (taken && window_->scene_margin() > 0) note_level(*taken);
        scene = std::move(taken);
    }
    if (!runtime_.stopped() && frame.address)
        window_->present(runtime_.memory(), frame.address, frame.stride, frame.format, display_.width, display_.height,
            submitted, std::move(scene));
}
// The dungeon camera (guest function 0x0882E3A4, camera at 0x089B5318: +0
// left edge in world units, +12 zoom) moves in 18-unit tile steps and keeps
// its left edge within [-18, W*18 - 459], W being the stage width the game
// reads through 0x08834AF8 and 0x08836158: the stage object at 0x089BD048,
// width at +4, height at +8. The level, its one-tile border included, is
// world x [-18, W*18 + 18]. The extent is used only while the camera agrees
// with the scene's own view (within the one step the game may already have
// taken for the next frame) and the scene is drawn from 18-unit tiles.
void Host::note_level(Scene &scene) const {
    const auto &memory = runtime_.memory();
    constexpr std::uint32_t camera = 0x089B5318u, stage = 0x089BD048u;
    if (!memory.contains(camera, 16) || !memory.contains(stage, 12)) return;
    const auto width = memory.load32(stage + 4), height = memory.load32(stage + 8);
    const auto left = std::bit_cast<float>(memory.load32(camera)), zoom = std::bit_cast<float>(memory.load32(camera + 12));
    if (width < 8 || width > 1024 || height < 8 || height > 1024 || zoom != 1.0f || !std::isfinite(left)) return;
    const double level_right = width * 18.0 + 18;
    if (left < -18 - 18 || left + 480 > level_right + 18) return;
    const auto agrees = std::any_of(scene.transforms.begin(), scene.transforms.end(), [&](const SceneTransform &transform) {
        return std::abs(transform.view[0][3] + left) <= 18.5;
    });
    std::size_t tiles = 0;
    for (const auto &draw : scene.draws) {
        if (draw.through || draw.transform < 0 || draw.kind != SceneDraw::Kind::sprite || draw.count < 2) continue;
        const auto &a = scene.vertices[draw.first].world, &b = scene.vertices[draw.first + 1].world;
        if (std::abs(std::abs(b[0] - a[0]) - 18) < 0.01 && std::abs(std::abs(b[1] - a[1]) - 18) < 0.01) ++tiles;
    }
    if (!agrees || tiles < 100) return;
    scene.level = {-18.0, level_right};
    scene.level_known = true;
}
void Host::install_display() {
    bind("sceDisplay", 0x0E20F177u, "sceDisplaySetMode", [this](Context &ctx) {
        if (ctx.gpr[4] != 0 || ctx.gpr[5] != 480 || ctx.gpr[6] != 272)
            throw Error("Only the PSP 480x272 display mode is implemented");
        display_.width = ctx.gpr[5]; display_.height = ctx.gpr[6]; ctx.gpr[2] = 0;
    });
    bind("sceDisplay", 0x289D82FEu, "sceDisplaySetFrameBuf", [this](Context &ctx) {
        Framebuffer frame{ctx.gpr[4], ctx.gpr[5], ctx.gpr[6]};
        if (frame.format > 3 || ctx.gpr[7] > 1) throw Error("Invalid PSP display format/sync mode");
        if (frame.address != 0) {
            const auto bytes = static_cast<std::uint64_t>(frame.stride) * display_.height * (frame.format == 3 ? 4 : 2);
            if ((frame.address & 15) || frame.stride < display_.width || frame.stride > 2048 ||
                bytes > runtime_.memory().size() || !runtime_.memory().contains(frame.address, static_cast<std::size_t>(bytes)))
                throw Error("Framebuffer is unaligned or outside mapped PSP memory");
        }
        if (ctx.gpr[7] == 0) { display_.current = frame; framebuffer_pending_ = false; }
        else { pending_framebuffer_ = frame; framebuffer_pending_ = true; }
        if (ctx.gpr[7] == 0) { latch_lateness_ns_ = 0; present_display(true); }
        ctx.gpr[2] = 0;
    });
    bind("sceDisplay", 0x984C27E7u, "sceDisplayWaitVblankStart", [this](Context &ctx) {
        if (in_interrupt_context()) { ctx.gpr[2] = 0x80020064u; return; }
        if (!interrupts_enabled_) { ctx.gpr[2] = 0x800201A7u; return; }
        service_display_timing(ctx); // A new wait targets the following edge.
        auto &thread = current();
        thread.context = ctx; thread.context.pc = ctx.gpr[31]; thread.state = State::waiting;
        thread.wait_thread = 0; thread.wait_semaphore = 0; thread.wait_semaphore_count = 0;
        thread.delay_deadline.reset(); thread.wait_event.reset(); thread.wait_vblank = true;
        schedule(ctx);
    });
    bind("sceDisplay", 0x9C6EAAD7u, "sceDisplayGetVcount", [this](Context &ctx) {
        service_display_timing(ctx); ctx.gpr[2] = display_.vblanks;
        if (window_) window_->pump();
    });
    bind("sceDisplay", 0xDBA6C4C4u, "sceDisplayGetFramePerSec", [](Context &ctx) {
        ctx.fpr[0] = 59.94006f;
    });
    install_display_timing();
}
}

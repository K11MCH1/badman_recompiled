#include "badman_scene_render.hpp"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <utility>

namespace badman {
bool build_scene_geometry(std::vector<SceneGpuVertex> &vertices, std::vector<SceneBatch> &batches, const Scene &scene,
                          const SceneMotion *moving, float t, bool smooth, const WideView *view,
                          const std::vector<ScenePlacement> *placement) {
    vertices.clear(); batches.clear();
    const ScenePlacement unplaced;
    const ScenePlacement *place = &unplaced;
    const auto position = [&](std::uint32_t index) -> std::array<float, 2> {
        const auto &vertex = scene.vertices[index];
        std::array<float, 2> at{vertex.x, vertex.y};
        if (moving) {
            const auto &start = moving->previous[index];
            at = {start[0] + (vertex.x - start[0]) * t, start[1] + (vertex.y - start[1]) * t};
        }
        return {static_cast<float>(at[0] * place->scale + place->shift), at[1]};
    };
    const auto add_batch = [&](SceneBatch batch, std::size_t first) {
        batch.first = static_cast<std::uint32_t>(first);
        batch.count = static_cast<std::uint32_t>(vertices.size() - first);
        if (!batch.count) return;
        if (!batches.empty() && batches.back() == batch && batches.back().first + batches.back().count == batch.first)
            batches.back().count += batch.count;
        else batches.push_back(batch);
    };
    for (std::size_t index = 0; index < scene.draws.size(); ++index) {
        const auto &draw = scene.draws[index];
        if (placement) place = &(*placement)[index];
        if (place->skip) continue;
        SceneBatch batch;
        const auto top = static_cast<float>(draw.scissor[1]), bottom = static_cast<float>(draw.scissor[3]);
        batch.scissor = {static_cast<float>(draw.scissor[0] * place->scale + place->shift), top,
                         static_cast<float>(draw.scissor[2] * place->scale + place->shift), bottom};
        if (view && place->widen_scissor)
            batch.scissor = {static_cast<float>(view->left), top, static_cast<float>(view->left + view->width), bottom};
        const auto first = vertices.size();
        if (draw.kind == SceneDraw::Kind::clear) {
            const auto keep = draw.clear_preserve;
            std::uint32_t mask = ((keep & 0x00FFFFFFu) == 0 ? 7u : 0u) | ((keep >> 24) == 0 ? 8u : 0u);
            if ((keep & 0x00FFFFFFu) != 0 && (keep & 0x00FFFFFFu) != 0x00FFFFFFu) return false; // Partial channel masks.
            batch.blend_key = mask << 12;
            const auto a = position(draw.first), b = position(draw.first + 1);
            const auto c = draw.clear_color;
            for (const auto &[x, y] : {std::array<float, 2>{a[0], a[1]}, {b[0], a[1]}, {a[0], b[1]}, {b[0], a[1]}, {b[0], b[1]}, {a[0], b[1]}})
                vertices.push_back({x, y, 0, 0, c});
            add_batch(batch, first);
            continue;
        }
        const auto mode = draw.blend_mode & 0x7FFu;
        if (draw.blend) {
            const auto a = mode & 15u, b = (mode >> 4) & 15u, equation = (mode >> 8) & 7u;
            if (equation > 4 || (a >= 6 && a <= 9) || (b >= 6 && b <= 9)) return false;
            const bool fixed_a = a >= 10, fixed_b = b >= 10;
            if (fixed_a && fixed_b && (draw.fixed_a & 0xFFFFFFu) != (draw.fixed_b & 0xFFFFFFu)) return false;
            batch.blend_factor = fixed_a ? draw.fixed_a : draw.fixed_b;
        }
        batch.blend_key = (draw.blend ? 1u | (mode << 1) : 0u) | (7u << 12); // The alpha byte is stencil: never written.
        if (draw.textured) {
            if (draw.texture < 0 || draw.texture >= static_cast<std::int32_t>(scene.textures.size())) return false;
            batch.texture = draw.texture;
            const bool linear = draw.linear || smooth;
            batch.flags = 1u | (linear ? 2u : 0u) | (draw.texture_alpha ? 4u : 0u);
        }
        if (draw.kind == SceneDraw::Kind::sprite) {
            for (std::uint32_t k = 0; k + 1 < draw.count; k += 2) {
                const auto &va = scene.vertices[draw.first + k], &vb = scene.vertices[draw.first + k + 1];
                const auto a = position(draw.first + k), b = position(draw.first + k + 1);
                const bool rotated = (va.x < vb.x) != (va.y < vb.y);
                const auto corner = [&](bool right, bool bottom) {
                    // Sprite UVs vary along x and y separately; a rotated sprite swaps the axes.
                    const bool along_u = rotated ? bottom : right, along_v = rotated ? right : bottom;
                    return SceneGpuVertex{right ? b[0] : a[0], bottom ? b[1] : a[1], along_u ? vb.u : va.u, along_v ? vb.v : va.v, vb.color};
                };
                for (const auto &[right, bottom] : {std::pair{false, false}, {true, false}, {false, true}, {true, false}, {true, true}, {false, true}})
                    vertices.push_back(corner(right, bottom));
            }
        } else {
            for (std::uint32_t k = 0; k < draw.count; ++k) {
                const auto &vertex = scene.vertices[draw.first + k];
                const auto at = position(draw.first + k);
                vertices.push_back({at[0], at[1], vertex.u, vertex.v, vertex.color});
            }
        }
        add_batch(batch, first);
    }
    return true;
}

std::uint64_t scene_mismatch(const std::uint8_t *rgba, std::size_t pitch, bool bottom_up, const std::uint32_t *exact) {
    std::uint64_t different = 0;
    for (std::uint32_t y = 0; y < 272; ++y) {
        const auto *row = rgba + (bottom_up ? 271 - y : y) * pitch;
        for (std::uint32_t x = 0; x < 480; ++x) {
            const auto *rendered = row + x * 4u;
            const auto wanted = exact[y * 480u + x]; // AARRGGBB
            const int dr = int(rendered[0]) - int((wanted >> 16) & 255u);
            const int dg = int(rendered[1]) - int((wanted >> 8) & 255u);
            const int db = int(rendered[2]) - int(wanted & 255u);
            if (std::max({std::abs(dr), std::abs(dg), std::abs(db)}) > 40) ++different;
        }
    }
    return different;
}

void ScenePacer::accept(std::shared_ptr<const Scene> scene, Clock::time_point now) {
    if (!scene) { timeline_.clear(); scene_mode_ = false; animating_ = false; clock_running_ = false; return; }
    Entry entry{scene, {}, scene->time_ns ? Clock::time_point(std::chrono::nanoseconds(scene->time_ns)) : now};
    entry.time = std::min(entry.time, now);
    // Lateness: how long after its vblank a frame reaches the presenter.
    lateness_ = std::clamp(std::max(lateness_ * 0.98, std::chrono::duration<double>(now - entry.time).count()), 0.001, 0.03);
    if (!interpolate_) timeline_.clear(); // Without in-between frames, each scene is shown as it comes.
    if (!timeline_.empty()) {
        const auto gap = std::chrono::duration<double>(entry.time - timeline_.back().time).count();
        if (gap > 0.0005 && gap < 0.2) {
            const auto &before = timeline_.back();
            entry.motion = scene_motion(*before.scene, *scene, before.motion.valid ? &before.motion : nullptr);
            interval_ = std::clamp(interval_ * 0.8 + gap * 0.2, 1.0 / 60.0, 1.0 / 10.0);
        }
        paired_ += entry.motion.paired_draws; moving_ += entry.motion.moving_draws;
    }
    if (!entry.motion.valid) { timeline_.clear(); clock_running_ = false; } // A cut: start a new run of frames.
    timeline_.push_back(std::move(entry));
    while (timeline_.size() > 4) timeline_.pop_front();
    scene_mode_ = true; animating_ = interpolate_;
}

ScenePacer::Pick ScenePacer::pick(Clock::time_point refresh) {
    Pick result;
    if (!scene_mode_ || timeline_.empty()) return result;
    if (!interpolate_) { result.entry = &timeline_.back(); return result; }
    using seconds = std::chrono::duration<double>;
    const auto target = refresh - std::chrono::duration_cast<Clock::duration>(seconds(interval_ + lateness_ + 0.006));
    if (!clock_running_) { display_clock_ = target; clock_running_ = true; }
    else {
        const double step = clock_held_ ? 0.0 : seconds(refresh - last_refresh_).count();
        const double lag = seconds(target - display_clock_).count();
        if (std::abs(lag) > 0.15) display_clock_ = target; // After a long stall, rejoin the schedule.
        else display_clock_ += std::chrono::duration_cast<Clock::duration>(seconds(step * std::clamp(1.0 + lag * 2.0, 0.75, 1.25)));
    }
    clock_held_ = false; last_refresh_ = refresh;
    const auto display = display_clock_;
    std::size_t next = 0;
    while (next < timeline_.size() && timeline_[next].time <= display) ++next;
    if (next == timeline_.size()) {
        if (animating_) ++holds_;
        result.entry = &timeline_.back(); animating_ = false;
        display_clock_ = timeline_.back().time; clock_held_ = true; // Wait here for the late frame.
    } else if (next == 0 || !timeline_[next].motion.valid) {
        result.entry = &timeline_[next == 0 ? 0 : next - 1]; animating_ = true;
    } else {
        result.entry = &timeline_[next]; result.before = &timeline_[next - 1];
        const auto span = seconds(timeline_[next].time - timeline_[next - 1].time).count();
        result.t = static_cast<float>(std::clamp(seconds(display - timeline_[next - 1].time).count() / span, 0.0, 1.0));
        result.in_between = result.t < 1;
        animating_ = true;
    }
    return result;
}
}

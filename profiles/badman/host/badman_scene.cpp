#include "badman_scene.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <unordered_map>

namespace badman {
namespace {
std::uint64_t mix(std::uint64_t hash, std::uint64_t value) {
    hash ^= value + 0x9E3779B97F4A7C15ull + (hash << 6) + (hash >> 2);
    return hash * 0xFF51AFD7ED558CCDull;
}
// The point that identifies where a draw is: world space for transformed
// draws (static tiles keep it while the camera moves), screen space otherwise.
std::array<double, 3> anchor(const Scene &scene, const SceneDraw &draw) {
    const auto &vertex = scene.vertices[draw.first];
    if (draw.through || draw.transform < 0) return {vertex.x, vertex.y, 0};
    return vertex.world;
}
std::uint64_t twin_key(const Scene &scene, const SceneDraw &draw) {
    const auto point = anchor(scene, draw);
    auto key = draw.material;
    for (const auto value : point) key = mix(key, static_cast<std::uint64_t>(std::llround(value * 16.0)));
    return key;
}
double distance(const std::array<double, 3> &a, const std::array<double, 3> &b) {
    return std::hypot(a[0] - b[0], a[1] - b[1], a[2] - b[2]);
}
}

std::array<double, 2> project(const SceneTransform &transform, const std::array<double, 3> &world) {
    std::array<double, 4> view{};
    for (unsigned row = 0; row < 4; ++row)
        view[row] = transform.view[row][0] * world[0] + transform.view[row][1] * world[1] +
                    transform.view[row][2] * world[2] + transform.view[row][3];
    std::array<double, 4> clip{};
    for (unsigned row = 0; row < 4; ++row)
        for (unsigned k = 0; k < 4; ++k) clip[row] += transform.projection[row][k] * view[k];
    const auto w = clip[3] != 0 ? clip[3] : 1.0;
    return {clip[0] / w * transform.scale[0] + transform.center[0] - transform.offset[0],
            clip[1] / w * transform.scale[1] + transform.center[1] - transform.offset[1]};
}

SceneMotion scene_motion(const Scene &previous, const Scene &current, const SceneMotion *earlier,
                         double object_limit, double camera_limit) {
    SceneMotion motion;
    motion.previous.resize(current.vertices.size());
    motion.velocity.resize(current.draws.size());
    const bool predicted = earlier && earlier->velocity.size() == previous.draws.size();
    const auto speed = [&](std::size_t i) -> std::array<double, 3> { return predicted ? earlier->velocity[i] : std::array<double, 3>{}; };
    const auto moving = [&](std::size_t i) { const auto v = speed(i); return v[0] != 0 || v[1] != 0 || v[2] != 0; };
    for (std::size_t i = 0; i < current.vertices.size(); ++i)
        motion.previous[i] = {current.vertices[i].x, current.vertices[i].y};
    std::vector<std::int32_t> pair(current.draws.size(), -1);
    std::vector<bool> used(previous.draws.size());
    const auto pairable = [](const SceneDraw &draw) { return draw.kind != SceneDraw::Kind::clear && draw.count; };
    // Unchanged twins: same material at the same anchor.
    std::unordered_multimap<std::uint64_t, std::int32_t> twins;
    for (std::size_t i = 0; i < previous.draws.size(); ++i)
        if (pairable(previous.draws[i]) && !moving(i)) twins.emplace(twin_key(previous, previous.draws[i]), static_cast<std::int32_t>(i));
    for (std::size_t i = 0; i < current.draws.size(); ++i) {
        if (!pairable(current.draws[i])) continue;
        const auto [first, last] = twins.equal_range(twin_key(current, current.draws[i]));
        for (auto it = first; it != last; ++it)
            if (!used[it->second] && previous.draws[it->second].count == current.draws[i].count) {
                pair[i] = it->second; used[it->second] = true; twins.erase(it); break;
            }
    }
    // Moving draws: every candidate of the same key within the limit, closest
    // pairs first across the whole frame. Pairing in draw order instead let a
    // creature take a nearby look-alike's previous place (a sliding ghost).
    const auto pair_closest = [&](std::uint64_t SceneDraw::*key, double limit) {
        std::unordered_map<std::uint64_t, std::vector<std::int32_t>> remaining;
        for (std::size_t i = 0; i < previous.draws.size(); ++i)
            if (!used[i] && pairable(previous.draws[i])) remaining[previous.draws[i].*key].push_back(static_cast<std::int32_t>(i));
        struct Candidate { double distance; std::int32_t current, previous; };
        std::vector<Candidate> candidates;
        for (std::size_t i = 0; i < current.draws.size(); ++i) {
            if (pair[i] >= 0 || !pairable(current.draws[i])) continue;
            const auto found = remaining.find(current.draws[i].*key);
            if (found == remaining.end()) continue;
            const auto here = anchor(current, current.draws[i]);
            for (const auto candidate : found->second) {
                if (previous.draws[candidate].count != current.draws[i].count) continue;
                const auto then = anchor(previous, previous.draws[candidate]);
                // Rank by distance from where the candidate's own velocity puts it now.
                const auto v = speed(static_cast<std::size_t>(candidate));
                const std::array<double, 3> expected{then[0] + v[0], then[1] + v[1], then[2] + v[2]};
                const auto miss = distance(here, expected), travel = distance(here, then);
                // A fast mover (falling, thrown) may travel up to twice the limit
                // when it lands near where its velocity said it would; the
                // tolerance grows with speed to allow for acceleration.
                const auto tolerance = std::max(2.0, 0.3 * std::hypot(v[0], v[1], v[2]));
                if (travel > limit && !(moving(static_cast<std::size_t>(candidate)) && miss <= tolerance && travel <= 2 * limit)) continue;
                candidates.push_back({miss, static_cast<std::int32_t>(i), candidate});
            }
        }
        std::sort(candidates.begin(), candidates.end(), [](const Candidate &a, const Candidate &b) {
            return a.distance != b.distance ? a.distance < b.distance :
                   a.current != b.current ? a.current < b.current : a.previous < b.previous;
        });
        for (const auto &candidate : candidates) {
            if (pair[candidate.current] >= 0 || used[candidate.previous]) continue;
            pair[candidate.current] = candidate.previous; used[candidate.previous] = true; ++motion.moving_draws;
        }
    };
    pair_closest(&SceneDraw::material, object_limit);
    // An animation frame of another size: the same creature, if still close.
    pair_closest(&SceneDraw::family, object_limit / 3);
    // A part left alone moves with the moving part drawn right next to it.
    std::vector<std::array<double, 3>> carried(current.draws.size());
    std::vector<bool> carries(current.draws.size());
    for (std::size_t i = 0; i < current.draws.size(); ++i) {
        if (pair[i] >= 0 || !pairable(current.draws[i])) continue;
        const auto here = anchor(current, current.draws[i]);
        double best = object_limit * 2 / 3;
        for (const auto offset : {-1, 1, -2, 2}) {
            const auto k = static_cast<std::int64_t>(i) + offset;
            if (k < 0 || k >= static_cast<std::int64_t>(current.draws.size()) || pair[k] < 0) continue;
            const auto &neighbour = current.draws[k];
            if (neighbour.through != current.draws[i].through || neighbour.transform != current.draws[i].transform) continue;
            const auto now = anchor(current, neighbour), then = anchor(previous, previous.draws[pair[k]]);
            const std::array<double, 3> moved{now[0] - then[0], now[1] - then[1], now[2] - then[2]};
            const auto d = distance(here, now);
            if ((moved[0] || moved[1] || moved[2]) && d <= best) { best = d; carried[i] = moved; carries[i] = true; }
        }
    }
    for (std::size_t i = 0; i < current.draws.size(); ++i) {
        if (pair[i] >= 0) {
            const auto now = anchor(current, current.draws[i]), then = anchor(previous, previous.draws[pair[i]]);
            motion.velocity[i] = {now[0] - then[0], now[1] - then[1], now[2] - then[2]};
        } else if (carries[i]) motion.velocity[i] = carried[i];
    }
    // Camera: each current transform follows the previous transform of its paired draws.
    std::vector<std::int32_t> camera(current.transforms.size(), -1);
    for (std::size_t i = 0; i < current.draws.size(); ++i) {
        const auto &draw = current.draws[i];
        if (pair[i] < 0 || draw.through || draw.transform < 0) continue;
        const auto before = previous.draws[pair[i]].transform;
        if (camera[draw.transform] < 0 && before >= 0) camera[draw.transform] = before;
    }
    for (std::size_t t = 0; t < current.transforms.size(); ++t) {
        if (camera[t] < 0) continue;
        // A camera jump larger than the limit is a cut, not motion.
        const std::array<double, 3> origin{0, 0, 0};
        const auto now = project(current.transforms[t], origin), before = project(previous.transforms[camera[t]], origin);
        if (std::hypot(now[0] - before[0], now[1] - before[1]) > camera_limit) return motion;
    }
    for (std::size_t i = 0; i < current.draws.size(); ++i) {
        const auto &draw = current.draws[i];
        if (!pairable(draw)) continue;
        if (pair[i] >= 0) ++motion.paired_draws;
        const SceneDraw *before = pair[i] >= 0 ? &previous.draws[pair[i]] : nullptr;
        std::vector<std::array<float, 2>> candidate(draw.count);
        bool plausible = true;
        for (std::uint32_t k = 0; k < draw.count; ++k) {
            const auto &vertex = current.vertices[draw.first + k];
            std::array<double, 2> start{vertex.x, vertex.y};
            if (draw.through || draw.transform < 0) {
                if (before) start = {previous.vertices[before->first + k].x, previous.vertices[before->first + k].y};
                else if (carries[i]) start = {vertex.x - carried[i][0], vertex.y - carried[i][1]};
            } else {
                const auto &now_transform = current.transforms[draw.transform];
                const auto before_index = before && before->transform >= 0 ? before->transform : camera[draw.transform];
                const auto &before_transform = before_index >= 0 ? previous.transforms[before_index] : now_transform;
                auto world_before = before ? previous.vertices[before->first + k].world : vertex.world;
                if (!before && carries[i])
                    for (unsigned axis = 0; axis < 3; ++axis) world_before[axis] -= carried[i][axis];
                const auto now = project(now_transform, vertex.world), then = project(before_transform, world_before);
                // Keep the exact current position; move only by the modelled difference.
                start = {vertex.x + (then[0] - now[0]), vertex.y + (then[1] - now[1])};
            }
            if (std::hypot(start[0] - vertex.x, start[1] - vertex.y) > object_limit + camera_limit) plausible = false;
            candidate[k] = {static_cast<float>(start[0]), static_cast<float>(start[1])};
        }
        if (!plausible) continue; // A teleport or a wrong pairing: show the draw where it is now.
        std::copy(candidate.begin(), candidate.end(), motion.previous.begin() + draw.first);
    }
    motion.valid = true;
    return motion;
}

std::optional<WideView> wide_view(const Scene &scene, double width) {
    if (!scene.level_known || width <= 480) return std::nullopt;
    // The camera is the transform behind most camera sprites; menus and
    // loading screens have few or none and keep the PSP's shape.
    std::vector<std::size_t> uses(scene.transforms.size());
    std::size_t camera_draws = 0;
    for (const auto &draw : scene.draws)
        if (!draw.through && !draw.beyond && draw.transform >= 0 && draw.kind == SceneDraw::Kind::sprite) { ++uses[draw.transform]; ++camera_draws; }
    if (camera_draws < 64) return std::nullopt;
    const auto &camera = scene.transforms[std::max_element(uses.begin(), uses.end()) - uses.begin()];
    const auto left_edge = project(camera, {scene.level[0], 0, 0})[0], right_edge = project(camera, {scene.level[1], 0, 0})[0];
    if (!(right_edge > left_edge)) return std::nullopt;
    WideView view;
    view.width = width;
    // The game keeps its own view inside the level, so the clamped view
    // always contains it.
    if (right_edge - left_edge <= width) view.left = (left_edge + right_edge - width) / 2;
    else view.left = std::clamp(240 - width / 2, left_edge, right_edge - width);
    return view;
}

// Badman's dungeon darkens the PSP screen's edges with a vignette: one
// 240x136 picture drawn four times, mirrored, as blended camera sprites placed
// to cover the screen's four quarters wherever the camera is. In a wider view
// those are no longer the screen's edges, so the vignette is left out.
static bool vignette_quarter(const Scene &scene, const SceneDraw &draw) {
    if (draw.kind != SceneDraw::Kind::sprite || draw.through || draw.count != 2 || !draw.textured || !draw.blend) return false;
    const auto &a = scene.vertices[draw.first], &b = scene.vertices[draw.first + 1];
    const auto x0 = std::min(a.x, b.x), x1 = std::max(a.x, b.x), y0 = std::min(a.y, b.y), y1 = std::max(a.y, b.y);
    return std::abs(std::abs(b.u - a.u) - 240) < 0.5 && std::abs(std::abs(b.v - a.v) - 136) < 0.5 &&
           x1 - x0 > 232 && x1 - x0 < 252 && y1 - y0 > 128 && y1 - y0 < 144 &&
           (x0 <= 2 || x1 >= 478) && (y0 <= 2 || y1 >= 270);
}

std::vector<ScenePlacement> wide_placement(const Scene &scene, const WideView &view) {
    std::vector<ScenePlacement> placement(scene.draws.size());
    struct Box { double x0{}, y0{}, x1{}, y1{}; };
    std::vector<Box> boxes(scene.draws.size());
    std::vector<std::size_t> flat, group(scene.draws.size());
    for (std::size_t i = 0; i < scene.draws.size(); ++i) {
        const auto &draw = scene.draws[i];
        placement[i].widen_scissor = draw.scissor[0] == 0 && draw.scissor[2] >= 480;
        if (vignette_quarter(scene, draw)) { placement[i].skip = true; continue; }
        if (!draw.through || !draw.count) continue;
        Box box{std::numeric_limits<double>::max(), std::numeric_limits<double>::max(),
                std::numeric_limits<double>::lowest(), std::numeric_limits<double>::lowest()};
        for (std::uint32_t k = 0; k < draw.count; ++k) {
            const auto &vertex = scene.vertices[draw.first + k];
            box = {std::min<double>(box.x0, vertex.x), std::min<double>(box.y0, vertex.y),
                   std::max<double>(box.x1, vertex.x), std::max<double>(box.y1, vertex.y)};
        }
        // Clears (the game clears in full-height strips) and full-width 2D
        // layers such as fades span the view; they never join a group.
        if (draw.kind == SceneDraw::Kind::clear || (box.x0 <= 0 && box.x1 >= 480)) {
            placement[i].scale = view.width / 480; placement[i].shift = view.left; continue;
        }
        boxes[i] = box; group[i] = i; flat.push_back(i);
    }
    // Touching 2D draws (a panel, its frame and its text) move together.
    const auto root = [&](std::size_t i) { while (group[i] != i) i = group[i] = group[group[i]]; return i; };
    for (std::size_t a = 0; a < flat.size(); ++a)
        for (std::size_t b = a + 1; b < flat.size(); ++b) {
            const auto &p = boxes[flat[a]], &q = boxes[flat[b]];
            if (p.x0 <= q.x1 + 1 && q.x0 <= p.x1 + 1 && p.y0 <= q.y1 + 1 && q.y0 <= p.y1 + 1) group[root(flat[a])] = root(flat[b]);
        }
    std::unordered_map<std::size_t, Box> extent;
    for (const auto i : flat) {
        const auto [found, inserted] = extent.try_emplace(root(i), boxes[i]);
        auto &box = found->second;
        box = {std::min(box.x0, boxes[i].x0), std::min(box.y0, boxes[i].y0), std::max(box.x1, boxes[i].x1), std::max(box.y1, boxes[i].y1)};
    }
    for (const auto i : flat) {
        const auto &box = extent[root(i)];
        const auto center = (box.x0 + box.x1) / 2;
        placement[i].shift = center < 160 ? view.left : center > 320 ? view.left + view.width - 480 : view.left + (view.width - 480) / 2;
    }
    return placement;
}
}

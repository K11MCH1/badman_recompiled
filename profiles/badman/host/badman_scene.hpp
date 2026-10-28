#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

namespace badman {
// A host-owned copy of the GE draws that produced one displayed frame. The
// presentation thread re-renders it at the window's resolution and between
// two frames' positions; nothing here refers to guest memory. Guest timing,
// callbacks and the exact framebuffer are unaffected by recording it.
struct SceneTexture {
    std::uint64_t key{};                 // Content hash of texels, palette and layout.
    std::uint32_t width{}, height{};
    std::vector<std::uint32_t> texels;   // AARRGGBB, top row first.
};
struct SceneVertex {
    float x{}, y{};                      // Exact native screen position in pixels.
    float u{}, v{};                      // Texel units.
    std::uint32_t color{};               // PSP ABGR vertex color.
    std::array<double, 3> world{};       // Transformed draws: world-space position.
};
// View, projection and viewport of transformed draws. World positions map to
// native screen pixels through these, as the GE does.
struct SceneTransform {
    std::array<std::array<double, 4>, 4> view{}, projection{};
    std::array<double, 2> scale{}, center{}, offset{};
    bool operator==(const SceneTransform &) const = default;
};
struct SceneDraw {
    enum class Kind : std::uint8_t { clear, sprite, triangles };
    Kind kind{};
    bool through{};                      // Screen-space 2D geometry, not moved by the camera.
    bool textured{}, texture_alpha{}, linear{}, dither{};
    bool blend{};
    std::uint32_t blend_mode{}, fixed_a{}, fixed_b{};
    std::int32_t texture{-1};            // Index into Scene::textures.
    std::int32_t transform{-1};          // Index into Scene::transforms.
    std::array<std::uint16_t, 4> scissor{}; // x0, y0, x1, y1 (exclusive), native pixels.
    std::uint32_t first{}, count{};      // Vertices: two corners per sprite, three per triangle.
    std::uint32_t clear_color{}, clear_preserve{};
    std::uint64_t material{};            // Texture/kind/size identity used to pair draws between frames.
    std::uint64_t family{};              // The same without the size: one creature across animation frames.
    // Culled by the GE just past the native screen's left or right edge. Kept
    // only for a wider view; it never reached guest memory.
    bool beyond{};
};
struct Scene {
    std::vector<SceneDraw> draws;
    std::vector<SceneVertex> vertices;
    std::vector<SceneTransform> transforms;
    std::vector<std::shared_ptr<const SceneTexture>> textures;
    std::uint32_t target{};              // Framebuffer the draws were made into.
    std::int64_t time_ns{};              // steady_clock time of the vblank that latched this frame.
    bool complete{true};                 // False when a draw could not be recorded.
    // World X extent of the level, border included, when the game's own stage
    // data was read with this frame. A wider view stays inside it.
    std::array<double, 2> level{};
    bool level_known{};
};
// Where each vertex of `current` was one frame earlier, in native screen
// pixels. Draws are paired with `previous` by material (kind, texture size
// and format, blend mode, size, vertex count): unchanged twins first (static
// tiles and HUD), then all remaining same-material pairs within
// `object_limit` pixels, closest first across the whole frame. Transformed
// draws also follow the camera (view) change. Unpaired draws keep their
// world position and only follow the camera. Draws still unpaired then get
// a second chance by family (ignoring size, within a third of the limit), and
// a part still alone takes the motion of the moving part drawn next to it.
struct SceneMotion {
    std::vector<std::array<float, 2>> previous; // One per current vertex.
    // Per current draw: how far its anchor moved since `previous` (zero when it
    // had no partner). The next frame predicts each draw's position from it.
    std::vector<std::array<double, 3>> velocity;
    bool valid{};                               // False after a cut: show `current` as is.
    std::uint32_t paired_draws{}, moving_draws{};
};
// `earlier`, when given, is the motion that led to `previous`: moving draws
// are then matched to where their own velocity predicts them, which tells
// touching look-alikes apart.
SceneMotion scene_motion(const Scene &previous, const Scene &current, const SceneMotion *earlier = nullptr,
                         double object_limit = 24.0, double camera_limit = 96.0);
// Native screen position of a world point under one transform.
std::array<double, 2> project(const SceneTransform &transform, const std::array<double, 3> &world);

// A view wider than the PSP's 480 pixels: native x in [left, left + width).
struct WideView {
    double left{}, width{480};
};
// The view up to `width` native pixels wide that this scene fills with real
// picture: a scene drawn through the camera, with the level extent known.
// It is centered on the game's own view, then kept inside the level so it
// never shows past the border. Nothing (keep 480) for other scenes.
std::optional<WideView> wide_view(const Scene &scene, double width);
// Where each draw goes in a wide view: x' = x * scale + shift. Camera draws
// stay put. 2D (through) draws move as groups of touching draws, keeping
// their distance to the nearest screen edge (the middle third stays
// centered); clears and full-width 2D draws such as fades span the view.
// The dungeon's vignette, which darkens the PSP screen's own edges, is
// skipped. widen_scissor: the draw's scissor spans the native width, so it
// spans the view.
struct ScenePlacement {
    double scale{1}, shift{};
    bool widen_scissor{}, skip{};
};
std::vector<ScenePlacement> wide_placement(const Scene &scene, const WideView &view);
}

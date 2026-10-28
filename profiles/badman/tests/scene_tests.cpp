#include "badman_scene.hpp"
#include <cmath>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
using namespace badman;
void require(bool value, const char *message) { if (!value) throw std::runtime_error(message); }
bool near(float a, double b) { return std::abs(a - b) < 1e-3; }

// An orthographic camera: world x/y map to screen x/y minus the camera position.
SceneTransform camera(double x, double y) {
    SceneTransform transform;
    for (unsigned i = 0; i < 4; ++i) { transform.view[i][i] = 1; transform.projection[i][i] = 1; }
    transform.view[0][3] = -x; transform.view[1][3] = -y;
    transform.scale = {1, 1};
    return transform;
}
// One transformed 18x18 tile per draw, all of the same material, plus an optional 2D sprite.
Scene tiles(double camera_x, int first_column, int columns) {
    Scene scene;
    scene.transforms.push_back(camera(camera_x, 0));
    for (int column = first_column; column < first_column + columns; ++column) {
        SceneDraw draw; draw.kind = SceneDraw::Kind::sprite; draw.transform = 0; draw.material = 7;
        draw.first = static_cast<std::uint32_t>(scene.vertices.size()); draw.count = 2;
        for (const double dx : {0.0, 18.0}) {
            SceneVertex vertex; vertex.world = {column * 18.0 + dx, dx, 0};
            const auto screen = project(scene.transforms[0], vertex.world);
            vertex.x = static_cast<float>(screen[0]); vertex.y = static_cast<float>(screen[1]);
            scene.vertices.push_back(vertex);
        }
        scene.draws.push_back(draw);
    }
    return scene;
}
void add_hud_sprite(Scene &scene, float x, std::uint64_t material, std::uint64_t family = 0) {
    SceneDraw draw; draw.kind = SceneDraw::Kind::sprite; draw.through = true; draw.material = material;
    draw.family = family ? family : material;
    draw.first = static_cast<std::uint32_t>(scene.vertices.size()); draw.count = 2;
    SceneVertex a, b; a.x = x; a.y = 10; b.x = x + 8; b.y = 18;
    scene.vertices.push_back(a); scene.vertices.push_back(b);
    scene.draws.push_back(draw);
}

void static_scene_does_not_move() {
    const auto previous = tiles(0, 0, 10), current = tiles(0, 0, 10);
    const auto motion = scene_motion(previous, current);
    require(motion.valid && motion.paired_draws == 10 && motion.moving_draws == 0, "Unchanged tiles were not paired as twins");
    for (std::size_t i = 0; i < current.vertices.size(); ++i)
        require(near(motion.previous[i][0], current.vertices[i].x), "A static tile gained motion");
}
void one_tile_scroll_follows_the_camera() {
    // The camera moves exactly one tile: every tile has a same-material
    // neighbour where its twin was. Pairing must use the twin, so all tiles
    // move by the camera delta and none slides by a tile (the old black band).
    const auto previous = tiles(0, 0, 12), current = tiles(18, 1, 12);
    const auto motion = scene_motion(previous, current);
    require(motion.valid && motion.moving_draws == 0, "A scrolled tile was paired with its neighbour");
    for (std::size_t i = 0; i < current.vertices.size(); ++i)
        require(near(motion.previous[i][0], current.vertices[i].x + 18.0), "A tile did not follow the camera");
}
void new_tiles_follow_the_camera() {
    // The column entering the view has no twin; it must still move with the camera.
    const auto previous = tiles(0, 0, 10), current = tiles(18, 1, 10);
    const auto motion = scene_motion(previous, current);
    const auto &entering = current.draws.back();
    require(near(motion.previous[entering.first][0], current.vertices[entering.first].x + 18.0), "An entering tile ignored the camera");
}
void hud_sprite_moves_between_its_positions() {
    auto previous = tiles(0, 0, 4), current = tiles(0, 0, 4);
    add_hud_sprite(previous, 100, 42); add_hud_sprite(current, 110, 42);
    const auto motion = scene_motion(previous, current);
    const auto &sprite = current.draws.back();
    require(motion.moving_draws == 1, "The moved 2D sprite was not paired");
    require(near(motion.previous[sprite.first][0], 100) && near(motion.previous[sprite.first + 1][0], 108),
            "The 2D sprite does not start from its previous position");
}
void look_alikes_keep_their_own_identity() {
    // Two identical creatures at 100 and 106. One moves 3 px to 103 and is
    // drawn first; the other moves 0.5 px to 106.5. Pairing in draw order gave
    // the first one the look-alike at 106 (equally near), leaving the second
    // to slide 6.5 px from 100: a ghost. Closest pairs first keeps both.
    auto previous = tiles(0, 0, 2), current = tiles(0, 0, 2);
    add_hud_sprite(previous, 100, 42); add_hud_sprite(previous, 106, 42);
    add_hud_sprite(current, 103, 42); add_hud_sprite(current, 106.5f, 42);
    const auto motion = scene_motion(previous, current);
    const auto &first = current.draws[current.draws.size() - 2], &second = current.draws.back();
    require(near(motion.previous[first.first][0], 100) && near(motion.previous[second.first][0], 106),
            "A creature was paired with a look-alike instead of itself");
}
void resized_animation_frame_stays_the_same_creature() {
    // An attack frame of another size changes the material but not the family.
    auto previous = tiles(0, 0, 2), current = tiles(0, 0, 2);
    add_hud_sprite(previous, 100, 42, 900); add_hud_sprite(current, 102, 43, 900);
    const auto motion = scene_motion(previous, current);
    require(near(motion.previous[current.draws.back().first][0], 100), "A resized animation frame lost its creature");
}
void lone_part_moves_with_its_creature() {
    // A body (paired, moving 4 px) and a part drawn right after it that has
    // no partner this frame: the part takes the body's motion.
    auto previous = tiles(0, 0, 2), current = tiles(0, 0, 2);
    add_hud_sprite(previous, 200, 50);
    add_hud_sprite(current, 204, 50); add_hud_sprite(current, 210, 77);
    const auto motion = scene_motion(previous, current);
    require(near(motion.previous[current.draws.back().first][0], 206), "A lone part did not move with its creature");
}
void touching_look_alikes_follow_their_velocity() {
    // Two identical creatures 1.5 px apart, both crawling right at 1 px per
    // frame. Without history the nearest candidate can be the neighbour; with
    // the earlier frame's velocity each one predicts its own new place.
    auto first = tiles(0, 0, 1), second = tiles(0, 0, 1), third = tiles(0, 0, 1);
    add_hud_sprite(first, 100, 42); add_hud_sprite(first, 101.5f, 42);
    add_hud_sprite(second, 101, 42); add_hud_sprite(second, 102.5f, 42);
    add_hud_sprite(third, 102, 42); add_hud_sprite(third, 103.5f, 42);
    const auto earlier = scene_motion(first, second);
    // In the third frame the creatures are drawn in the other order.
    std::swap(third.draws[third.draws.size() - 1], third.draws[third.draws.size() - 2]);
    const auto motion = scene_motion(second, third, &earlier);
    const auto &a = third.draws[third.draws.size() - 2], &b = third.draws.back();
    require(near(motion.previous[a.first][0], 102.5) && near(motion.previous[b.first][0], 101),
            "Touching look-alikes swapped identities");
}
void steady_fast_mover_keeps_moving() {
    // A falling creature speeds up 10, 15, 20, 25, 30 px per frame. The last
    // two steps pass the normal limit but land near the predicted place, so
    // they still interpolate. A 30 px jump with no history does not.
    const float path[]{100, 110, 125, 145, 170, 200};
    Scene before = tiles(0, 0, 1);
    add_hud_sprite(before, path[0], 42);
    SceneMotion motion;
    for (std::size_t step = 1; step < std::size(path); ++step) {
        Scene now = tiles(0, 0, 1);
        add_hud_sprite(now, path[step], 42);
        motion = scene_motion(before, now, step > 1 ? &motion : nullptr);
        require(near(motion.previous[now.draws.back().first][0], path[step - 1]), "A falling creature lost its interpolation");
        before = std::move(now);
    }
    Scene start = tiles(0, 0, 1), jump = tiles(0, 0, 1);
    add_hud_sprite(start, 100, 42); add_hud_sprite(jump, 130, 42);
    const auto fresh = scene_motion(start, jump);
    require(near(fresh.previous[jump.draws.back().first][0], 130), "A 30 px jump with no history was interpolated");
}
void far_jump_is_not_motion() {
    auto previous = tiles(0, 0, 4), current = tiles(0, 0, 4);
    add_hud_sprite(previous, 10, 42); add_hud_sprite(current, 300, 42);
    const auto motion = scene_motion(previous, current);
    const auto &sprite = current.draws.back();
    require(near(motion.previous[sprite.first][0], 300), "A teleported sprite was interpolated across the screen");
}
void camera_cut_disables_interpolation() {
    const auto previous = tiles(0, 0, 10), current = tiles(400, 0, 10);
    const auto motion = scene_motion(previous, current);
    require(!motion.valid, "A camera cut was treated as motion");
}

// A dungeon row: border column -1, then columns 0..69 (world x -18 to 1260).
Scene dungeon(double camera_x) {
    auto scene = tiles(camera_x, -1, 71);
    scene.level = {-18, 1260}; scene.level_known = true;
    return scene;
}
std::size_t add_2d(Scene &scene, SceneDraw::Kind kind, float x0, float y0, float x1, float y1, std::int32_t texture = -1,
                   std::uint32_t color = 0xFFFFFFFFu) {
    SceneDraw draw; draw.kind = kind; draw.through = true; draw.texture = texture;
    draw.textured = draw.blend = draw.texture_alpha = texture >= 0;
    draw.scissor = {0, 0, 480, 272};
    draw.first = static_cast<std::uint32_t>(scene.vertices.size()); draw.count = 2;
    SceneVertex a, b; a.x = x0; a.y = y0; b.x = x1; b.y = y1; b.color = color;
    scene.vertices.push_back(a); scene.vertices.push_back(b);
    scene.draws.push_back(draw);
    return scene.draws.size() - 1;
}
void wide_view_is_centered_inside_the_level() {
    // Mid-level the wider view is centered on the game's own view.
    auto view = wide_view(dungeon(400), 640);
    require(view && near(static_cast<float>(view->left), -80) && near(static_cast<float>(view->width), 640), "The wide view was not centered");
    // At the left clamp (camera at -18) the view starts at the border column.
    view = wide_view(dungeon(-18), 640);
    require(view && near(static_cast<float>(view->left), 0), "The wide view showed past the level's left border");
    // At the right clamp it ends at the level's right edge.
    view = wide_view(dungeon(1260 - 480), 640);
    require(view && near(static_cast<float>(view->left + view->width), 480), "The wide view showed past the level's right border");
}
void wide_view_needs_a_known_level() {
    auto unknown = dungeon(400); unknown.level_known = false;
    require(!wide_view(unknown, 640), "A scene without a level extent was widened");
    require(!wide_view(dungeon(400), 480), "A 480-wide view was treated as wider");
    auto menu = tiles(0, 0, 10); menu.level = {-18, 1260}; menu.level_known = true;
    require(!wide_view(menu, 640), "A scene with few camera draws was widened");
}
void wide_placement_keeps_the_hud_at_the_edges() {
    auto scene = dungeon(400);
    auto texture = std::make_shared<SceneTexture>(); texture->width = texture->height = 8;
    scene.textures.push_back(texture);
    std::vector<std::size_t> strips;
    for (int k = 0; k < 15; ++k) strips.push_back(add_2d(scene, SceneDraw::Kind::clear, k * 32.0f, 0, k * 32.0f + 32, 272));
    const auto left_panel = add_2d(scene, SceneDraw::Kind::sprite, 4, 220, 64, 268);
    const auto left_text = add_2d(scene, SceneDraw::Kind::sprite, 60, 230, 70, 240); // Touches the panel.
    const auto right_panel = add_2d(scene, SceneDraw::Kind::sprite, 305, 220, 470, 268);
    const auto dialogue = add_2d(scene, SceneDraw::Kind::sprite, 150, 100, 330, 140);
    const auto fade = add_2d(scene, SceneDraw::Kind::sprite, 0, 0, 480, 272, -1, 0x80000000u);
    const auto shade = add_2d(scene, SceneDraw::Kind::sprite, 0, 0, 480, 272, 0, 0xAA000000u);
    // The vignette: four mirrored 240x136 camera sprites over the screen's quarters.
    std::vector<std::size_t> quarters;
    for (const auto [x0, y0, x1, y1] : {std::array<float, 4>{-2, 0, 246, 136}, {486, 0, 246, 136}, {-2, 272, 246, 136}, {486, 272, 246, 136}}) {
        SceneDraw draw; draw.kind = SceneDraw::Kind::sprite; draw.transform = 0; draw.texture = 0;
        draw.textured = draw.blend = true; draw.scissor = {0, 0, 480, 272};
        draw.first = static_cast<std::uint32_t>(scene.vertices.size()); draw.count = 2;
        SceneVertex a, b; a.x = x0; a.y = y0; b.x = x1; b.y = y1; b.u = 240; b.v = 136;
        scene.vertices.push_back(a); scene.vertices.push_back(b);
        scene.draws.push_back(draw); quarters.push_back(scene.draws.size() - 1);
    }
    const WideView view{-80, 640};
    const auto placement = wide_placement(scene, view);
    const auto at = [&](std::size_t draw, double x) { return static_cast<float>(x * placement[draw].scale + placement[draw].shift); };
    for (const auto strip : strips) require(!placement[strip].skip && placement[strip].widen_scissor, "A clear strip was not kept full height");
    require(near(at(strips.front(), 0), -80) && near(at(strips.back(), 480), 560), "The clear strips did not span the wide view");
    require(near(at(left_panel, 4), -76) && near(at(left_text, 60), -20), "The left HUD group did not keep to the left edge");
    require(near(at(right_panel, 470), 550), "The right HUD did not keep to the right edge");
    require(near(at(dialogue, 150), 150), "A centered panel moved off center");
    require(near(at(fade, 0), -80) && near(at(fade, 480), 560), "A fade did not span the wide view");
    require(!placement[fade].skip && !placement[shade].skip && near(at(shade, 480), 560), "A full-screen shade did not span the wide view");
    for (const auto quarter : quarters) require(placement[quarter].skip, "The vignette was drawn in the wide view");
    for (std::size_t i = 0; i < 71; ++i)
        require(placement[i].scale == 1 && placement[i].shift == 0 && !placement[i].skip, "A camera draw moved");
}
}

int main() {
    try {
        static_scene_does_not_move(); one_tile_scroll_follows_the_camera(); new_tiles_follow_the_camera();
        hud_sprite_moves_between_its_positions(); look_alikes_keep_their_own_identity();
        resized_animation_frame_stays_the_same_creature(); lone_part_moves_with_its_creature();
        touching_look_alikes_follow_their_velocity(); steady_fast_mover_keeps_moving(); far_jump_is_not_motion();
        camera_cut_disables_interpolation();
        wide_view_is_centered_inside_the_level(); wide_view_needs_a_known_level(); wide_placement_keeps_the_hud_at_the_edges();
        std::cout << "Badman scene pairing, motion and wide view tests passed\n";
        return 0;
    } catch (const std::exception &error) { std::cerr << error.what() << '\n'; return 1; }
}

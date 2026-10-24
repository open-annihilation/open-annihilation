// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "check.hpp"
#include "fixtures.hpp"

#include "oa/ui/hud/order_overlays.hpp"

#include <cstdint>
#include <string>
#include <vector>

using namespace oa;
using namespace oa::ui::hud;

namespace {

struct Line {
    int32_t x0{}, y0{}, x1{}, y1{};
    uint8_t color{};
};

struct Sprite {
    OverlaySprite kind{};
    uint8_t index{};
    uint16_t frame{};
    int32_t x{}, y{};
};

struct Recorder {
    std::vector<Line> lines;
    std::vector<std::string> labels;
    std::vector<Sprite> sprites;
    int32_t ground = 0;
    bool visible = true;
    OrderOverlay* primary = nullptr;
    OrderOverlay* secondary = nullptr;
    std::vector<const Unit*> visited;

    OverlaySink sink() {
        OverlaySink s{};
        s.user = this;
        s.line = [](void* u, int32_t x0, int32_t y0, int32_t x1, int32_t y1, uint8_t c) {
            static_cast<Recorder*>(u)->lines.push_back({x0, y0, x1, y1, c});
        };
        s.label = [](void* u, const char* text, int32_t, int32_t) {
            static_cast<Recorder*>(u)->labels.emplace_back(text);
        };
        s.sprite =
            [](void* u, OverlaySprite kind, uint8_t index, uint16_t frame, int32_t x, int32_t y) {
                static_cast<Recorder*>(u)->sprites.push_back({kind, index, frame, x, y});
            };
        s.ground_height = [](void* u, const FixedVec3&) {
            return static_cast<Recorder*>(u)->ground;
        };
        s.can_see = [](void* u, const Player*, const Unit&) {
            return static_cast<Recorder*>(u)->visible;
        };
        s.orders = [](void* u, const Unit& unit, bool secondary) -> OrderOverlay* {
            auto* self = static_cast<Recorder*>(u);
            self->visited.push_back(&unit);
            return secondary ? self->secondary : self->primary;
        };
        return s;
    }

    void clear() {
        lines.clear();
        labels.clear();
        sprites.clear();
        visited.clear();
    }
};

constexpr int32_t fx(int32_t v) {
    return v << 16;
}

struct Scene {
    hud_test::TestWorld match;
    Recorder recorder;
    MissionOverlay missions[256]{};
    SpriteSequence indicators[4]{{0, 0}, {4, 2}, {4, 2}, {1, 1}};

    Scene() {
        for (uint8_t i = 0; i < 16; ++i)
            match.game().ui_colors[i] = static_cast<uint8_t>(0xa0 + i);
        auto& site = match.world->unit_defs[2];
        site.bounds_min_x = fx(-16);
        site.bounds_min_y = 0;
        site.bounds_min_z = fx(-16);
        site.bounds_max_x = fx(16);
        site.bounds_max_z = fx(16);
    }

    OverlayContext context() {
        return {match.world, {nullptr, 0, 0}, recorder.sink(), missions, {3, 1}, indicators, false};
    }
};

void test_world_projection() {
    Scene scene;
    auto context = scene.context();
    context.view.project = [](void*, const FixedVec3& p) {
        return ScreenPoint{
            (static_cast<int32_t>(p.x) + static_cast<int32_t>(p.z)) / 65536,
            (static_cast<int32_t>(p.z) - static_cast<int32_t>(p.x) - static_cast<int32_t>(p.y)) /
                65536
        };
    };
    const auto projected = overlay_project(context.view, {fx(10), fx(3), fx(20)});
    CHECK(projected.x == 30 && projected.y == 7);
    OrderOverlay order{};
    order.parameter = 2;
    order.position = {fx(100), 0, fx(100)};
    FixedVec3 point{};
    draw_build_site_overlay(context, order, point);
    CHECK(scene.recorder.lines.size() == 4);
    CHECK(scene.recorder.lines[0].x0 == 168 && scene.recorder.lines[0].y0 == 0);
    CHECK(scene.recorder.lines[0].x1 == 200 && scene.recorder.lines[0].y1 == -32);
    BuildSiteBounds bounds{};
    build_site_overlay_bounds(
        context.view, scene.match.world->unit_defs[2], order.position, bounds
    );
    CHECK(static_cast<int32_t>(bounds.left) == 168 && static_cast<int32_t>(bounds.top) == -32);
    CHECK(static_cast<int32_t>(bounds.right) == 232 && static_cast<int32_t>(bounds.bottom) == 32);
    CHECK(point.x == order.position.x && point.z == order.position.z);
}

void test_build_site() {
    Scene scene;
    auto& builder = scene.match.spawn(1, 1);
    OrderOverlay order{};
    order.unit = &builder;
    order.parameter = 2;
    order.position = {fx(100), 0, fx(100)};
    FixedVec3 point{1, 2, 3};
    const auto context = scene.context();
    draw_build_site_overlay(context, order, point);
    BuildSiteBounds bounds{};
    build_site_overlay_bounds(
        context.view, scene.match.world->unit_defs[2], order.position, bounds
    );
    CHECK(bounds.left == 212 && bounds.right == 244 && bounds.top == 116 && bounds.bottom == 148);
    CHECK(scene.recorder.lines.size() == 8);
    const auto& outer = scene.recorder.lines[0];
    // Footprint 84..116 in map pixels, shifted right 128 and down 32.
    CHECK(outer.x0 == 211 && outer.y0 == 115 && outer.x1 == 211 && outer.y1 == 149);
    CHECK(outer.color == 0xa1);
    CHECK(scene.recorder.lines[4].x0 == 212 && scene.recorder.lines[4].color == 0xa9);
    CHECK(point.x == fx(100) && point.z == fx(100));

    // Ten ticks later the box has closed in to the far side.
    scene.recorder.clear();
    scene.match.game().tick = 25;
    order.issue_tick = 15;
    builder.flags |= OA_UNIT_FLAG_SELECTED;
    draw_build_site_overlay(context, order, point);
    CHECK(scene.recorder.lines[4].x0 == 244 && scene.recorder.lines[4].color == 0xaa);
    CHECK(scene.recorder.lines[0].color == 0xa3);

    // No build type: nothing drawn, point untouched.
    scene.recorder.clear();
    order.parameter = 0;
    point = {7, 7, 7};
    draw_build_site_overlay(context, order, point);
    CHECK(scene.recorder.lines.empty() && point.x == 7);
}

void test_range_circle() {
    Scene scene;
    const auto context = scene.context();
    // 16 pixels: trunc(16 * 2pi / 8) = 12 segments, drawn 0..12.
    draw_range_circle(context, {fx(200), fx(10), fx(200)}, 16, 5, "sight", 1);
    CHECK(scene.recorder.lines.size() == 13);
    CHECK(scene.recorder.labels.size() == 1 && scene.recorder.labels[0] == "sight");
    // Angle 0 sits straight down the screen (+z) from the centre, lifted by
    // half the centre height since the ground is lower.
    const auto& first = scene.recorder.lines[0];
    CHECK(first.x0 == 328 && first.y0 == 216 + 32 - 5);
    // Ground above the centre lifts the points.
    scene.recorder.clear();
    scene.recorder.ground = 40;
    draw_range_circle(context, {fx(200), fx(10), fx(200)}, 16, 5, nullptr, 0);
    CHECK(scene.recorder.lines[0].y0 == 216 + 32 - 20);
    // Radius 1 has no segments.
    scene.recorder.clear();
    draw_range_circle(context, {0, 0, 0}, 1, 5, nullptr, 0);
    CHECK(scene.recorder.lines.empty());
}

void test_path_and_target() {
    Scene scene;
    scene.missions[7] = {kOverlayPath, 1};
    auto& unit = scene.match.spawn(1, 1);
    unit.position = {0, 0, 0};
    OrderOverlay order{};
    order.mission = 7;
    order.unit = &unit;
    order.position = {fx(100), 0, 0};
    scene.recorder.primary = &order;
    const auto context = scene.context();
    draw_unit_order_overlays(context, unit, kOverlayAll, true);
    // Pips at 0, 48 and 96 pixels, then the indicator.
    int pips = 0;
    for (const auto& sprite : scene.recorder.sprites)
        if (sprite.kind == OverlaySprite::path_pip)
            ++pips;
    CHECK(pips == 3);
    CHECK(scene.recorder.sprites[0].kind == OverlaySprite::indicator);
    CHECK(scene.recorder.sprites[0].x == 228 && scene.recorder.sprites[0].y == 32);
    // 48/100 of the way truncates to 47 pixels.
    CHECK(scene.recorder.sprites[2].x == 128 + 47);

    // Without animation only the indicator is drawn.
    scene.recorder.clear();
    draw_unit_order_overlays(context, unit, kOverlayAll, false);
    CHECK(scene.recorder.sprites.size() == 1);

    // A target unit's position is latched; out of sight the latch is used.
    auto& target = scene.match.spawn(2, 1);
    target.position = {fx(300), fx(4), fx(50)};
    order.target = &target;
    FixedVec3 point{};
    draw_order_target(context, order, point);
    CHECK((order.flags & kOrderTargetSeen) != 0 && order.seen_x == 300 && order.seen_z == 50);
    CHECK(point.x == fx(300));
    target.position = {fx(400), fx(4), fx(60)};
    scene.recorder.visible = false;
    draw_order_target(context, order, point);
    CHECK(point.x == fx(300) && point.z == fx(50) && point.y == fx(4));
}

void test_ranges_and_ring() {
    Scene scene;
    auto& def = scene.match.world->unit_defs[1];
    def.sight_distance = 100;
    def.radar_distance = 200;
    def.size_x = fx(20);
    auto& unit = scene.match.spawn(1, 1);
    unit.weapons[0].flags = OA_UNIT_WEAPON_ENABLED;
    unit.weapons[0].def = oa_ref_from_index(3);
    scene.match.game().weapon_defs[3].range = 300;
    OrderOverlay order{};
    order.unit = &unit;
    auto context = scene.context();
    // In play: no cloak, not a kamikaze: nothing.
    draw_unit_ranges(context, order);
    CHECK(scene.recorder.labels.empty() && scene.recorder.lines.empty());
    // Debug display labels every range.
    context.show_ranges = true;
    draw_unit_ranges(context, order);
    CHECK(scene.recorder.labels.size() == 3);
    CHECK(scene.recorder.labels[0] == "sight" && scene.recorder.labels[1] == "radar");
    CHECK(scene.recorder.labels[2] == "weapon1 range");

    scene.recorder.clear();
    order.target = &unit;
    FixedVec3 point{};
    draw_target_ring(context, order, point);
    CHECK(scene.recorder.lines.size() == 16);
    CHECK(scene.recorder.lines[0].x0 == 128 + 20);
}

// ui.interface-fixes range-ring-3 and radius-0.
void test_interface_fix_circles() {
    Scene scene;
    auto& unit = scene.match.spawn(1, 1);
    unit.weapons[2].def = oa_ref_from_index(3);
    scene.match.game().weapon_defs[3].range = 300;
    OrderOverlay order{};
    order.unit = &unit;
    auto context = scene.context();
    context.show_ranges = true;
    // 3.1c: the third weapon's circle follows the first weapon's enable bit.
    unit.weapons[2].flags = OA_UNIT_WEAPON_ENABLED;
    draw_unit_ranges(context, order);
    CHECK(scene.recorder.labels.empty());
    unit.weapons[0].flags = OA_UNIT_WEAPON_ENABLED;
    draw_unit_ranges(context, order);
    CHECK(scene.recorder.labels.size() == 1 && scene.recorder.labels[0] == "weapon3 range");
    // The fix: its own bit.
    context.third_ring_own_weapon = true;
    unit.weapons[2].flags = 0;
    scene.recorder.clear();
    draw_unit_ranges(context, order);
    CHECK(scene.recorder.labels.empty());
    unit.weapons[0].flags = 0;
    unit.weapons[2].flags = OA_UNIT_WEAPON_ENABLED;
    draw_unit_ranges(context, order);
    CHECK(scene.recorder.labels.size() == 1 && scene.recorder.labels[0] == "weapon3 range");

    // A one-pixel circle has no segments: 3.1c draws nothing, the fix its label.
    scene.recorder.clear();
    draw_range_circle(context, {0, 0, 0}, 1, 5, "tiny", 0);
    CHECK(scene.recorder.lines.empty() && scene.recorder.labels.empty());
    context.segmentless_circle_label = true;
    draw_range_circle(context, {0, 0, 0}, 1, 5, "tiny", 0);
    CHECK(scene.recorder.lines.empty() && scene.recorder.labels.size() == 1);
    // Radius 0 stays empty either way.
    scene.recorder.clear();
    draw_range_circle(context, {0, 0, 0}, 0, 5, "none", 0);
    CHECK(scene.recorder.lines.empty() && scene.recorder.labels.empty());
}

void test_selection_and_stockpile() {
    Scene scene;
    scene.missions[1] = {kOverlayBuildSite | kOverlayRanges, 0};
    auto& match = scene.match;
    match.add_player(0, OA_PLAYER_STATUS_LOCAL);
    match.give_range(0, 1, 4);
    auto& selected = match.spawn(1, 1);
    selected.flags |= OA_UNIT_FLAG_SELECTED;
    match.spawn(2, 1);
    auto& dying = match.spawn(3, 1);
    dying.flags |= OA_UNIT_FLAG_DEATH_PENDING;
    OrderOverlay order{};
    order.mission = 1;
    scene.recorder.primary = &order;
    const auto context = scene.context();
    draw_selection_overlays(context);
    // Only the selected unit, with no builder in view.
    CHECK(scene.recorder.visited.size() == 1 && scene.recorder.visited[0] == &selected);

    // With a builder under the cursor every live unit shows its build sites.
    scene.recorder.clear();
    match.world->unit_defs[1].build_ids = 1;
    match.game().cursor_unit_id = 2;
    draw_selection_overlays(context);
    CHECK(scene.recorder.visited.size() == 2);

    // Stockpile progress from the first stockpile order.
    OrderOverlay idle{};
    OrderOverlay build{};
    build.state = kOrderStockpileBuild;
    build.parameter = 0;
    build.progress = 45;
    idle.next = &build;
    scene.recorder.secondary = &idle;
    selected.weapons[0].def = oa_ref_from_index(5);
    match.game().weapon_defs[5].reload_time = 90;
    CHECK(stockpile_percent(context, selected) == 50);
    scene.recorder.secondary = nullptr;
    CHECK(stockpile_percent(context, selected) == 0);
}

// The overlay kinds and target sprite of mission kinds numbered in the order
// of their names.
void test_mission_table() {
    constexpr uint8_t mobile_build = 25;
    constexpr uint8_t move_ground = 26;
    constexpr uint8_t patrol = 29;
    constexpr uint8_t standby = 41;
    constexpr uint8_t attack_chase = 6;
    constexpr uint8_t help_build = 23;
    CHECK(
        kMissionOverlays[mobile_build].mask == (kOverlayBuildSite | kOverlayPath | kOverlayRanges)
    );
    CHECK(kMissionOverlays[mobile_build].indicator == 0);
    CHECK(kMissionOverlays[move_ground].mask == (kOverlayPath | kOverlayRanges));
    CHECK(kMissionOverlays[move_ground].indicator == 14);
    CHECK(kMissionOverlays[patrol].mask == 0x12 && kMissionOverlays[patrol].indicator == 7);
    CHECK(
        kMissionOverlays[standby].mask == kOverlayRanges &&
        kMissionOverlays[standby].indicator == 15
    );
    CHECK(
        kMissionOverlays[attack_chase].mask == kOverlayTarget &&
        kMissionOverlays[attack_chase].indicator == 1
    );
    CHECK(kMissionOverlays[help_build].mask == (kOverlayTarget | kOverlayRanges));
    CHECK(kMissionOverlays[67].mask == 0 && kMissionOverlays[67].indicator == 19);
    CHECK(kMissionOverlays[68].mask == 0 && kMissionOverlays[68].indicator == 0);
}

} // namespace

int main() {
    test_world_projection();
    test_mission_table();
    test_build_site();
    test_range_circle();
    test_path_and_target();
    test_ranges_and_ring();
    test_interface_fix_circles();
    test_selection_and_stockpile();
    return 0;
}

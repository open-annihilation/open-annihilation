// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// ui.camera-sharing's minimap rectangles: which slots draw one, where, how
// large and in which colour; and which reported cameras share a position
// with this machine.
#include "oa/ui/hud/shared_views.hpp"

#include "oa/present/colour_vision/colour_vision.hpp"

#include "check.hpp"
#include "fixtures.hpp"

using namespace oa;
using namespace oa::ui::hud;

namespace {

void rectangles() {
    hud_test::TestWorld w;
    w.add_player(0, 1);
    w.add_player(2, 2);
    w.add_player(4, 2);
    w.world->player_info[2].color = 3;
    SharedPlayerViews shared{};
    // A 2048-pixel square map shown 120 pixels wide at (4, 140); the view
    // is 30 by 22 pixels there.
    const MinimapFrame frame{4, 140, 120, 120, 2048, 2048, 30, 22};
    std::array<CameraRectangle, OA_PLAYER_COUNT> out{};
    CHECK(camera_rectangles(*w.world, shared, false, frame, out) == 0);
    shared.shares_position[2] = true;
    // Shared, but no camera reported yet.
    CHECK(camera_rectangles(*w.world, shared, false, frame, out) == 0);
    shared.camera_x[2] = 1024;
    shared.camera_y[2] = 512;
    CHECK(camera_rectangles(*w.world, shared, false, frame, out) == 1);
    CHECK(out[0].slot == 2 && out[0].left == 4 + 60 - 15 && out[0].top == 140 + 30 - 11);
    CHECK(out[0].right == out[0].left + 30 && out[0].bottom == out[0].top + 22);
    CHECK(out[0].color == kPlayerDotColors[3]);
    // A watcher sees every reported camera, but slot 0 never.
    shared.camera_x[4] = 0;
    shared.camera_y[4] = 0;
    shared.camera_x[0] = 0;
    shared.camera_y[0] = 0;
    CHECK(camera_rectangles(*w.world, shared, true, frame, out) == 2);
    CHECK(out[1].slot == 4 && out[1].left == 4 - 15);
    // A slot without a name draws none.
    w.player(4).name[0] = '\0';
    CHECK(camera_rectangles(*w.world, shared, true, frame, out) == 1);
    CHECK(player_dot_color(*w.world, 12) == kPlayerDotColors[0]);
}

// A reported camera is placed for every slot; only a slot that allies the
// local player shares its position, and turning sharing off forgets it.
void reported_cameras() {
    hud_test::TestWorld w;
    w.add_player(0, 1);
    w.add_player(1, 2);
    w.add_player(2, 2);
    w.game().local_player_index = 0;
    // Slot 1 allies the local player; slot 2 does not, though the local
    // player allies it.
    w.player(1).alliance[0] = 1;
    w.player(0).alliance[2] = 1;
    std::array<ReportedCamera, OA_PLAYER_COUNT> reported{};
    reported[0] = {true, 10, 20};
    reported[1] = {true, 1024, 512};
    reported[2] = {true, 300, 400};
    SharedPlayerViews shared{};
    take_reported_cameras(*w.world, reported, shared);
    CHECK(shared.shares_position[1] && shared.camera_x[1] == 1024 && shared.camera_y[1] == 512);
    CHECK(!shared.shares_position[2] && shared.camera_x[2] == 300 && shared.camera_y[2] == 400);
    CHECK(!shared.shares_position[0]);
    CHECK(!shared.shares_position[3] && shared.camera_x[3] == kUnknownCamera);
    // The ally's rectangle shows for a player; a watcher sees the other one too.
    const MinimapFrame frame{0, 0, 128, 128, 2048, 2048, 32, 24};
    std::array<CameraRectangle, OA_PLAYER_COUNT> out{};
    CHECK(camera_rectangles(*w.world, shared, false, frame, out) == 1 && out[0].slot == 1);
    CHECK(camera_rectangles(*w.world, shared, true, frame, out) == 2 && out[1].slot == 2);
    // The ally turns sharing off.
    reported[1].shared = false;
    take_reported_cameras(*w.world, reported, shared);
    CHECK(!shared.shares_position[1] && shared.camera_x[1] == kUnknownCamera);
    CHECK(camera_rectangles(*w.world, shared, false, frame, out) == 0);
}

// Colour 0 is blue's dot and colour 1 is red's, with no mode and in achromatopsia.
void colour_numbers_pick_the_dot() {
    namespace vision = oa::present::colour_vision;

    struct Restore {
        ~Restore() { vision::set_active(vision::Mode::off, false); }
    } restore;

    vision::set_active(vision::Mode::off, false);
    CHECK(colour_dot_index(0) == kPlayerDotColors[0]);
    CHECK(colour_dot_index(1) == kPlayerDotColors[1]);
    vision::set_active(vision::Mode::achromatopsia, true);
    CHECK(colour_dot_index(0) == vision::stock_anchor_index(vision::Mode::achromatopsia, 0));
    CHECK(colour_dot_index(1) == vision::stock_anchor_index(vision::Mode::achromatopsia, 1));
    (void)restore;
}

} // namespace

int main() {
    rectangles();
    reported_cameras();
    colour_numbers_pick_the_dot();
    std::puts("ui-hud-shared-views-test: ok");
    return 0;
}

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The touch controls' model: the device class, the layouts at 1180x820,
// 852x393 and 956x440, the latches, the radial, the hit tests and the
// left-handed mirror; with a gamepad, the Steam Deck's 1280x800 screen at
// each Control size, the slim pad HUD, the FORCE chip, badges that never
// move a control, the build and group rings, the pad's hints, badges and
// help lines, the texts the two-meaning labels are looked up by, and the
// banner's titles in the language shown.
#include "oa/test/check.hpp"
#include "oa/ui/touch_hud.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace hud = oa::ui::touch_hud;
namespace layout = oa::ui::display_layout;
using hud::Control;
using hud::Rect;

/// Returns whether two rectangles are the same.
bool same(const Rect& a, const Rect& b) {
    return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
}

/// Returns whether a rectangle holds no pixel.
bool empty(const Rect& rect) {
    return rect.width <= 0 || rect.height <= 0;
}

/// Returns whether two rectangles share a pixel.
bool overlap(const Rect& a, const Rect& b) {
    return !empty(a) && !empty(b) && a.x < b.x + b.width && b.x < a.x + a.width &&
           a.y < b.y + b.height && b.y < a.y + a.height;
}

/// Returns whether one rectangle lies wholly inside another.
bool inside(const Rect& inner, const Rect& outer) {
    return inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.width <= outer.x + outer.width &&
           inner.y + inner.height <= outer.y + outer.height;
}

/// Returns a rectangle with every value multiplied.
Rect times(const Rect& rect, int factor) {
    return {rect.x * factor, rect.y * factor, rect.width * factor, rect.height * factor};
}

/// Returns the tablet viewport the tests pin: 1180x820 points at 1 pixel a point.
hud::Viewport tablet_viewport() {
    hud::Viewport viewport{};
    viewport.width = 1180;
    viewport.height = 820;
    viewport.px_per_point = 1.0f;
    viewport.safe = {0, 0, 0, 20};
    viewport.device = hud::DeviceClass::tablet;
    viewport.chrome = layout::make_match_layout(1180, 820);
    return viewport;
}

/// Returns a phone viewport of a size in pixels with insets in pixels.
hud::Viewport phone_viewport(int width, int height, hud::Insets safe, float px_per_point = 1.0f) {
    hud::Viewport viewport{};
    viewport.width = width;
    viewport.height = height;
    viewport.px_per_point = px_per_point;
    viewport.safe = safe;
    viewport.device = hud::DeviceClass::phone;
    viewport.chrome = layout::make_phone_layout(width, height, px_per_point, safe);
    return viewport;
}

/// Returns the phone viewport the tests pin: 852x393 with a 59 pt notch side and home bar.
hud::Viewport phone_852() {
    return phone_viewport(852, 393, {59, 0, 59, 21});
}

/// Returns a state on the match screen.
hud::HudState match_state() {
    hud::HudState state{};
    state.in_match = true;
    return state;
}

/// Returns the safe rectangle of a viewport.
Rect safe_rect(const hud::Viewport& viewport) {
    return {
        viewport.safe.left,
        viewport.safe.top,
        viewport.width - viewport.safe.left - viewport.safe.right,
        viewport.height - viewport.safe.top - viewport.safe.bottom
    };
}

/// Returns the rectangle of a control, or an empty one when it is not laid out.
Rect find(const hud::Frame& frame, Control control, int index = -1) {
    for (uint8_t slot = 0; slot < frame.control_count; ++slot) {
        const auto& laid = frame.controls[slot];
        if (laid.control == control && (index < 0 || laid.index == index))
            return laid.rect;
    }
    return {};
}

/// Returns how many controls of a kind are laid out.
int count_of(const hud::Frame& frame, Control control) {
    int total = 0;
    for (uint8_t slot = 0; slot < frame.control_count; ++slot) {
        if (frame.controls[slot].control == control)
            ++total;
    }
    return total;
}

/// Returns whether a control is a ring's wedge, whose rectangle may share corners with its
/// neighbours'.
bool wedge(Control control) {
    return control == Control::radial_item || control == Control::build_wedge ||
           control == Control::group_wedge;
}

/// Returns whether no two controls overlap (wedges of one ring may share corners).
bool no_overlaps(const hud::Frame& frame) {
    for (uint8_t a = 0; a < frame.control_count; ++a) {
        for (uint8_t b = a + 1; b < frame.control_count; ++b) {
            const auto& first = frame.controls[a];
            const auto& second = frame.controls[b];
            if (wedge(first.control) && first.control == second.control)
                continue;
            if (overlap(first.rect, second.rect)) {
                std::fprintf(
                    stderr,
                    "overlap: control %d.%d and %d.%d\n",
                    static_cast<int>(first.control),
                    first.index,
                    static_cast<int>(second.control),
                    second.index
                );
                return false;
            }
        }
    }
    return true;
}

/// Returns whether every control lies inside the safe area.
bool all_inside_safe(const hud::Frame& frame, const hud::Viewport& viewport) {
    const Rect safe = safe_rect(viewport);
    for (uint8_t slot = 0; slot < frame.control_count; ++slot) {
        if (!inside(frame.controls[slot].rect, safe))
            return false;
    }
    return true;
}

/// Returns whether a rectangle overlaps no control of a frame.
bool overlaps_no_control(const hud::Frame& frame, const Rect& rect) {
    for (uint8_t slot = 0; slot < frame.control_count; ++slot) {
        if (overlap(frame.controls[slot].rect, rect))
            return false;
    }
    return true;
}

/// Returns whether two frames are the same, control by control and region by region.
bool same_frame(const hud::Frame& a, const hud::Frame& b) {
    if (a.control_count != b.control_count || a.drawer_cell_count != b.drawer_cell_count ||
        a.more_cell_count != b.more_cell_count)
        return false;
    for (uint8_t slot = 0; slot < a.control_count; ++slot) {
        if (a.controls[slot].control != b.controls[slot].control ||
            a.controls[slot].index != b.controls[slot].index ||
            !same(a.controls[slot].rect, b.controls[slot].rect))
            return false;
    }
    for (std::size_t cell = 0; cell < hud::max_drawer_cells; ++cell) {
        if (!same(a.drawer_cells[cell], b.drawer_cells[cell]))
            return false;
    }
    for (std::size_t cell = 0; cell < hud::max_more_cells; ++cell) {
        if (!same(a.more_cells[cell], b.more_cells[cell]))
            return false;
    }
    return same(a.minimap, b.minimap) && same(a.resources, b.resources) &&
           same(a.status, b.status) && same(a.banner, b.banner) && same(a.tip, b.tip) &&
           same(a.sheet, b.sheet) && same(a.drawer_grid, b.drawer_grid) &&
           same(a.more_grid, b.more_grid) && same(a.placement_bar, b.placement_bar) &&
           same(a.panel_sheet, b.panel_sheet) && same(a.clear, b.clear);
}

/// Checks the device class from the window's size in points, in both orientations.
void device_class_follows_the_short_side() {
    OA_CHECK(hud::classify_device(852, 393) == hud::DeviceClass::phone);
    OA_CHECK(hud::classify_device(393, 852) == hud::DeviceClass::phone);
    OA_CHECK(hud::classify_device(956, 440) == hud::DeviceClass::phone);
    OA_CHECK(hud::classify_device(640, 480) == hud::DeviceClass::tablet);
    OA_CHECK(hud::classify_device(1180, 820) == hud::DeviceClass::tablet);
    OA_CHECK(hud::classify_device(1194, 834) == hud::DeviceClass::tablet);
    OA_CHECK(hud::classify_device(744, 1133) == hud::DeviceClass::tablet);
    OA_CHECK(hud::classify_device(1133, 744) == hud::DeviceClass::tablet);
    OA_CHECK(hud::classify_device(900, 459) == hud::DeviceClass::phone);
    OA_CHECK(hud::classify_device(900, 460) == hud::DeviceClass::tablet);
}

/// Checks which latch gives Shift to each class of action, and each tap's class.
void each_class_has_its_latch() {
    OA_CHECK(hud::latch_for(hud::ActionClass::selection) == hud::Latch::add);
    OA_CHECK(hud::latch_for(hud::ActionClass::order) == hud::Latch::queue);
    OA_CHECK(hud::latch_for(hud::ActionClass::build_button) == hud::Latch::times_five);
    OA_CHECK(hud::action_class(hud::TapAction::select) == hud::ActionClass::selection);
    for (const auto action :
         {hud::TapAction::move,
          hud::TapAction::attack,
          hud::TapAction::guard,
          hud::TapAction::patrol,
          hud::TapAction::repair,
          hud::TapAction::assist,
          hud::TapAction::reclaim,
          hud::TapAction::capture,
          hud::TapAction::load,
          hud::TapAction::unload,
          hud::TapAction::blast,
          hud::TapAction::build,
          hud::TapAction::place})
        OA_CHECK(hud::action_class(action) == hud::ActionClass::order);
}

/// Checks the latches: tap toggles, hold holds, use scopes and the two modes.
void latches_tap_to_latch_and_hold_for_one() {
    using hud::Latch;
    {
        hud::Latches latches;
        OA_CHECK(!latches.active(Latch::queue));
        latches.press(Latch::queue, 1000);
        OA_CHECK(latches.active(Latch::queue) && latches.held(Latch::queue));
        OA_CHECK(!latches.latched(Latch::queue));
        latches.release(Latch::queue, 1100, 350);
        OA_CHECK(latches.latched(Latch::queue) && latches.active(Latch::queue));
        OA_CHECK(!latches.held(Latch::queue));
        latches.press(Latch::queue, 2000);
        latches.release(Latch::queue, 2100, 350);
        OA_CHECK(!latches.latched(Latch::queue) && !latches.active(Latch::queue));
    }
    {
        // A press held past the hold delay only holds.
        hud::Latches latches;
        latches.press(Latch::add, 0);
        OA_CHECK(latches.active(Latch::add));
        latches.release(Latch::add, 500, 350);
        OA_CHECK(!latches.active(Latch::add) && !latches.latched(Latch::add));
    }
    {
        // A short press an action used only holds.
        hud::Latches latches;
        latches.press(Latch::queue, 0);
        latches.used(hud::ActionClass::order, hud::LatchMode::stay_on);
        OA_CHECK(latches.active(Latch::queue));
        latches.release(Latch::queue, 100, 350);
        OA_CHECK(!latches.active(Latch::queue));
    }
    {
        // One action: the latch turns off after its use; a held press stays active until lift.
        hud::Latches latches;
        latches.press(Latch::queue, 0);
        latches.release(Latch::queue, 50, 350);
        OA_CHECK(latches.latched(Latch::queue));
        latches.used(hud::ActionClass::order, hud::LatchMode::one_action);
        OA_CHECK(!latches.active(Latch::queue));
        latches.press(Latch::queue, 1000);
        latches.used(hud::ActionClass::order, hud::LatchMode::one_action);
        OA_CHECK(latches.active(Latch::queue));
        latches.release(Latch::queue, 1600, 350);
        OA_CHECK(!latches.active(Latch::queue));
    }
    {
        // Stay on: the latch survives its uses.
        hud::Latches latches;
        latches.press(Latch::add, 0);
        latches.release(Latch::add, 50, 350);
        latches.used(hud::ActionClass::selection, hud::LatchMode::stay_on);
        latches.used(hud::ActionClass::selection, hud::LatchMode::stay_on);
        OA_CHECK(latches.latched(Latch::add));
    }
    {
        // Scoping: a selection never touches QUEUE, an order never touches ADD.
        hud::Latches latches;
        for (const auto latch : {Latch::queue, Latch::add, Latch::times_five}) {
            latches.press(latch, 0);
            latches.release(latch, 10, 350);
        }
        latches.used(hud::ActionClass::selection, hud::LatchMode::one_action);
        OA_CHECK(latches.latched(Latch::queue) && !latches.latched(Latch::add));
        OA_CHECK(latches.latched(Latch::times_five));
        latches.press(Latch::add, 100);
        latches.release(Latch::add, 110, 350);
        latches.used(hud::ActionClass::order, hud::LatchMode::one_action);
        OA_CHECK(!latches.latched(Latch::queue) && latches.latched(Latch::add));
        latches.used(hud::ActionClass::build_button, hud::LatchMode::one_action);
        OA_CHECK(!latches.latched(Latch::times_five) && latches.latched(Latch::add));
    }
    {
        // Cancel ends a hold without toggling; clear ends everything.
        hud::Latches latches;
        latches.press(Latch::queue, 0);
        latches.cancel(Latch::queue);
        OA_CHECK(!latches.active(Latch::queue));
        latches.release(Latch::queue, 50, 350);
        OA_CHECK(!latches.latched(Latch::queue));
        latches.press(Latch::add, 0);
        latches.release(Latch::add, 10, 350);
        latches.press(Latch::queue, 20);
        latches.clear();
        OA_CHECK(!latches.active(Latch::add) && !latches.active(Latch::queue));
    }
}

/// Checks the order names the engine takes, the drawn labels and the status hint.
void orders_have_their_panel_names() {
    const std::vector<std::string> names{
        "MOVE",
        "ATTACK",
        "PATROL",
        "DEFEND",
        "STOP",
        "BLAST",
        "RECLAIM",
        "REPAIR",
        "CAPTURE",
        "LOAD",
        "UNLOAD",
    };
    for (std::size_t order = 0; order < hud::order_count; ++order) {
        OA_CHECK(hud::order_name(static_cast<hud::Order>(order)) == names[order]);
        OA_CHECK(!hud::order_label(static_cast<hud::Order>(order)).empty());
    }
    OA_CHECK(hud::order_label(hud::Order::guard) == "GUARD");
    OA_CHECK(hud::order_label(hud::Order::blast) == "D-GUN");
    const std::function<std::string(std::string_view)> english = [](std::string_view text) {
        return std::string(text);
    };
    OA_CHECK(
        hud::status_hint(hud::TapAction::move, hud::TapAction::attack, english) ==
        "TAP: MOVE · ENEMY: ATTACK"
    );
    OA_CHECK(
        hud::status_hint(hud::TapAction::select, hud::TapAction::none, english) == "TAP: SELECT"
    );
    OA_CHECK(hud::status_hint(hud::TapAction::none, hud::TapAction::none, english).empty());
}

/// Checks the tablet's pinned rectangles at 1180x820 beside the 3.1c HUD.
void tablet_layout_is_pinned() {
    const hud::Viewport viewport = tablet_viewport();
    OA_CHECK(viewport.chrome.left == 219 && viewport.chrome.top == 55);
    OA_CHECK(viewport.chrome.hud_width == 1093 && viewport.chrome.bottom_bar_y() == 765);
    hud::HudState state = match_state();
    state.group_counts[1] = 5;
    const hud::Frame frame = hud::lay_out(viewport, state);
    OA_CHECK(same(find(frame, Control::queue), {231, 579, 76, 62}));
    OA_CHECK(same(find(frame, Control::add), {231, 647, 76, 52}));
    OA_CHECK(same(find(frame, Control::clear), {231, 705, 76, 52}));
    OA_CHECK(same(find(frame, Control::group_chip, 1), {319, 705, 70, 52}));
    OA_CHECK(same(find(frame, Control::group_store), {395, 705, 70, 52}));
    OA_CHECK(same(find(frame, Control::select_menu), {471, 705, 100, 52}));
    OA_CHECK(same(find(frame, Control::pause), {1112, 63, 60, 52}));
    OA_CHECK(same(find(frame, Control::speed), {1112, 121, 60, 52}));
    OA_CHECK(same(find(frame, Control::centre), {1112, 179, 60, 52}));
    OA_CHECK(same(find(frame, Control::info), {1112, 353, 60, 52}));
    OA_CHECK(same(find(frame, Control::menu), {1097, 0, 75, 55}));
    OA_CHECK(same(frame.clear, {219, 55, 885, 516}));
    OA_CHECK(count_of(frame, Control::times_five) == 0);
    OA_CHECK(count_of(frame, Control::chat) == 0);
    OA_CHECK(count_of(frame, Control::group_chip) == 1);
    OA_CHECK(empty(frame.status) && empty(frame.minimap) && empty(frame.resources));
    OA_CHECK(empty(frame.sheet) && empty(frame.banner) && empty(frame.tip));
    OA_CHECK(no_overlaps(frame));
    OA_CHECK(all_inside_safe(frame, viewport));
    // Nothing on the 3.1c side column, top bar or bottom bar.
    const auto& chrome = viewport.chrome;
    const Rect side{0, 0, chrome.left, viewport.height};
    const Rect top_bar{0, 0, chrome.hud_width, chrome.top};
    const Rect bottom_bar{0, chrome.bottom_bar_y(), viewport.width, chrome.bottom};
    OA_CHECK(overlaps_no_control(frame, side));
    OA_CHECK(overlaps_no_control(frame, top_bar));
    OA_CHECK(overlaps_no_control(frame, bottom_bar));
    // No stored group: STORE takes the first chip's place.
    const hud::Frame bare = hud::lay_out(viewport, match_state());
    OA_CHECK(same(find(bare, Control::group_store), {319, 705, 70, 52}));
    OA_CHECK(same(find(bare, Control::select_menu), {395, 705, 100, 52}));
}

/// Checks the tablet's optional controls: x5 with a build page, CHAT in a shared game, no
/// SPEED for a watcher, and every stored group's chip fitting the bar.
void tablet_shows_what_the_state_asks() {
    const hud::Viewport viewport = tablet_viewport();
    hud::HudState state = match_state();
    state.build_page_loaded = true;
    hud::Frame frame = hud::lay_out(viewport, state);
    OA_CHECK(same(find(frame, Control::times_five), {231, 529, 76, 44}));
    OA_CHECK(same(frame.clear, {219, 55, 885, 466}));
    OA_CHECK(no_overlaps(frame));

    state = match_state();
    state.shared_game = true;
    frame = hud::lay_out(viewport, state);
    OA_CHECK(same(find(frame, Control::chat), {1112, 179, 60, 52}));
    OA_CHECK(same(find(frame, Control::info), {1112, 411, 60, 52}));
    OA_CHECK(no_overlaps(frame));

    state.watching = true;
    frame = hud::lay_out(viewport, state);
    OA_CHECK(count_of(frame, Control::speed) == 0);
    OA_CHECK(same(find(frame, Control::chat), {1112, 121, 60, 52}));

    state = match_state();
    for (std::size_t group = 1; group <= 9; ++group)
        state.group_counts[group] = static_cast<uint16_t>(group);
    frame = hud::lay_out(viewport, state);
    OA_CHECK(count_of(frame, Control::group_chip) == 9);
    OA_CHECK(no_overlaps(frame));
    OA_CHECK(all_inside_safe(frame, viewport));
    const Rect select = find(frame, Control::select_menu);
    OA_CHECK(select.x + select.width <= find(frame, Control::pause).x - 8);

    // Off the match screen nothing is laid out.
    const hud::Frame off = hud::lay_out(viewport, hud::HudState{});
    OA_CHECK(off.control_count == 0 && empty(off.clear));
}

/// Checks the tablet's clear area against the battlefield and the controls.
void tablet_clear_area_avoids_the_controls() {
    const hud::Viewport viewport = tablet_viewport();
    for (const bool build_page : {false, true}) {
        hud::HudState state = match_state();
        state.build_page_loaded = build_page;
        state.group_counts[2] = 3;
        const hud::Frame frame = hud::lay_out(viewport, state);
        const auto& chrome = viewport.chrome;
        const Rect field{
            chrome.left,
            chrome.top,
            viewport.width - chrome.left,
            chrome.bottom_bar_y() - chrome.top
        };
        OA_CHECK(inside(frame.clear, field));
        OA_CHECK(frame.clear.x + frame.clear.width <= find(frame, Control::pause).x - 8);
        const Rect top = find(frame, build_page ? Control::times_five : Control::queue);
        OA_CHECK(frame.clear.y + frame.clear.height <= top.y - 8);
        OA_CHECK(frame.clear.y + frame.clear.height <= find(frame, Control::group_store).y - 8);
        OA_CHECK(overlaps_no_control(frame, frame.clear));
    }
    // A banner pushes the clear area under it.
    hud::HudState state = match_state();
    state.banner.shown = true;
    const hud::Frame frame = hud::lay_out(viewport, state);
    OA_CHECK(same(frame.banner, {489, 63, 420, 36}));
    OA_CHECK(same(find(frame, Control::banner_cancel), {865, 63, 44, 36}));
    OA_CHECK(frame.clear.y == 107);
    OA_CHECK(overlaps_no_control(frame, frame.clear));
}

/// Checks the tablet's placement bar, sheets and their outside.
void tablet_placement_and_sheets() {
    const hud::Viewport viewport = tablet_viewport();
    hud::HudState state = match_state();
    state.placement.active = true;
    hud::Frame frame = hud::lay_out(viewport, state);
    OA_CHECK(same(find(frame, Control::place_cancel), {629, 709, 140, 48}));
    OA_CHECK(same(frame.placement_bar, {629, 709, 140, 48}));
    OA_CHECK(no_overlaps(frame));
    // Stored groups in its way move it.
    for (std::size_t group = 1; group <= 4; ++group)
        state.group_counts[group] = 1;
    frame = hud::lay_out(viewport, state);
    OA_CHECK(no_overlaps(frame));
    OA_CHECK(count_of(frame, Control::place_cancel) == 1);
    OA_CHECK(overlaps_no_control(frame, frame.clear));

    state = match_state();
    state.sheet = hud::Sheet::select_menu;
    frame = hud::lay_out(viewport, state);
    OA_CHECK(same(frame.sheet, {395, 397, 340, 300}));
    OA_CHECK(count_of(frame, Control::menu_item) == static_cast<int>(hud::select_item_count));
    OA_CHECK(same(find(frame, Control::menu_item, 0), {403, 405, 160, 44}));
    OA_CHECK(same(find(frame, Control::menu_item, 6), {567, 405, 160, 44}));
    OA_CHECK(no_overlaps(frame));
    for (uint8_t slot = 0; slot < frame.control_count; ++slot) {
        if (frame.controls[slot].control == Control::menu_item)
            OA_CHECK(inside(frame.controls[slot].rect, frame.sheet));
    }

    state.sheet = hud::Sheet::speed;
    frame = hud::lay_out(viewport, state);
    OA_CHECK(same(frame.sheet, {928, 93, 176, 108}));
    OA_CHECK(count_of(frame, Control::menu_item) == 2);
    OA_CHECK(no_overlaps(frame));
}

/// Checks the phone's pinned rectangles at 852x393.
void phone_layout_is_pinned() {
    const hud::Viewport viewport = phone_852();
    hud::HudState state = match_state();
    state.rail_count = hud::rail_capacity(viewport);
    state.group_counts[1] = 4;
    const hud::Frame frame = hud::lay_out(viewport, state);
    OA_CHECK(same(frame.minimap, {67, 8, 104, 104}));
    OA_CHECK(same(find(frame, Control::build_drawer), {67, 120, 104, 44}));
    OA_CHECK(same(find(frame, Control::queue), {67, 172, 50, 50}));
    OA_CHECK(same(find(frame, Control::add), {121, 172, 50, 50}));
    OA_CHECK(same(find(frame, Control::clear), {67, 226, 50, 50}));
    OA_CHECK(same(find(frame, Control::select_menu), {121, 226, 50, 50}));
    OA_CHECK(same(find(frame, Control::zoom_out), {67, 284, 50, 44}));
    OA_CHECK(same(find(frame, Control::zoom_in), {121, 284, 50, 44}));
    OA_CHECK(same(find(frame, Control::pause), {693, 8, 44, 44}));
    OA_CHECK(same(find(frame, Control::menu), {741, 8, 44, 44}));
    OA_CHECK(same(find(frame, Control::order_slot, 0), {721, 60, 64, 44}));
    OA_CHECK(same(find(frame, Control::order_slot, 4), {721, 252, 64, 44}));
    OA_CHECK(same(find(frame, Control::more, 5), {721, 300, 64, 44}));
    OA_CHECK(same(frame.resources, {183, 8, 498, 28}));
    OA_CHECK(same(frame.status, {222, 40, 420, 28}));
    OA_CHECK(same(find(frame, Control::group_chip, 1), {183, 320, 52, 44}));
    OA_CHECK(same(find(frame, Control::group_store), {239, 320, 52, 44}));
    OA_CHECK(same(frame.clear, {183, 76, 526, 236}));
    OA_CHECK(same(frame.panel_sheet, {67, 8, 718, 356}));
    OA_CHECK(no_overlaps(frame));
    OA_CHECK(all_inside_safe(frame, viewport));
    const Rect safe = safe_rect(viewport);
    for (const Rect& region : {frame.minimap, frame.resources, frame.status, frame.clear})
        OA_CHECK(inside(region, safe));
    // The strip runs between the column and PAUSE.
    OA_CHECK(frame.resources.x == 171 + 12);
    OA_CHECK(frame.resources.x + frame.resources.width == 693 - 12);
    // The panel sheet is centred in the safe area.
    OA_CHECK(
        frame.panel_sheet.x - safe.x ==
        safe.x + safe.width - (frame.panel_sheet.x + frame.panel_sheet.width)
    );
    OA_CHECK(
        frame.panel_sheet.y - safe.y ==
        safe.y + safe.height - (frame.panel_sheet.y + frame.panel_sheet.height)
    );
}

/// Checks the rail's capacity on two phones and a tablet, and that lay_out places what the
/// state lists, never more than fits.
void phone_rail_fits_the_height() {
    const hud::Viewport small = phone_852();
    const hud::Viewport large = phone_viewport(956, 440, {62, 0, 62, 21});
    OA_CHECK(hud::rail_capacity(small) == 6);
    OA_CHECK(hud::rail_capacity(large) == 7);
    OA_CHECK(hud::rail_capacity(tablet_viewport()) == 0);
    hud::HudState state = match_state();
    state.rail_count = 4;
    hud::Frame frame = hud::lay_out(small, state);
    OA_CHECK(count_of(frame, Control::order_slot) == 3 && count_of(frame, Control::more) == 1);
    OA_CHECK(same(find(frame, Control::more, 3), {721, 204, 64, 44}));
    state.rail_count = 6;
    frame = hud::lay_out(small, state);
    OA_CHECK(count_of(frame, Control::order_slot) + count_of(frame, Control::more) == 6);
    state.rail_count = 7;
    frame = hud::lay_out(small, state);
    OA_CHECK(count_of(frame, Control::order_slot) + count_of(frame, Control::more) == 6);
    frame = hud::lay_out(large, state);
    OA_CHECK(count_of(frame, Control::order_slot) + count_of(frame, Control::more) == 7);
    OA_CHECK(same(find(frame, Control::more, 6), {822, 348, 64, 44}));
    OA_CHECK(same(frame.minimap, {70, 8, 104, 104}));
    OA_CHECK(same(frame.clear, {186, 76, 624, 283}));
    OA_CHECK(no_overlaps(frame));
    OA_CHECK(all_inside_safe(frame, large));
}

/// Checks the phone's clear area against the column, the rail, the pill and the chips.
void phone_clear_area_avoids_the_controls() {
    const hud::Viewport viewport = phone_852();
    hud::HudState state = match_state();
    state.rail_count = 6;
    state.group_counts[3] = 2;
    hud::Frame frame = hud::lay_out(viewport, state);
    OA_CHECK(frame.clear.x >= find(frame, Control::add).x + 50 + 12);
    OA_CHECK(frame.clear.x + frame.clear.width <= find(frame, Control::order_slot, 0).x - 12);
    OA_CHECK(frame.clear.y >= frame.status.y + frame.status.height + 8);
    OA_CHECK(frame.clear.y + frame.clear.height <= find(frame, Control::group_store).y - 8);
    OA_CHECK(overlaps_no_control(frame, frame.clear));
    // The banner replaces the pill and pushes the clear area under it.
    state.banner.shown = true;
    frame = hud::lay_out(viewport, state);
    OA_CHECK(empty(frame.status));
    OA_CHECK(same(frame.banner, {222, 40, 420, 36}));
    OA_CHECK(frame.clear.y == 84);
    OA_CHECK(overlaps_no_control(frame, frame.clear));
    // Placement: the bar at the bottom centre, above the chips when they are in its way.
    state = match_state();
    state.placement.active = true;
    frame = hud::lay_out(viewport, state);
    OA_CHECK(same(find(frame, Control::place_cancel), {376, 316, 140, 48}));
    OA_CHECK(same(frame.placement_bar, {376, 316, 140, 48}));
    OA_CHECK(no_overlaps(frame));
    OA_CHECK(overlaps_no_control(frame, frame.clear));
    for (std::size_t group = 1; group <= 3; ++group)
        state.group_counts[group] = 1;
    frame = hud::lay_out(viewport, state);
    OA_CHECK(no_overlaps(frame));
    OA_CHECK(count_of(frame, Control::place_cancel) == 1);
    OA_CHECK(overlaps_no_control(frame, frame.clear));
    OA_CHECK(all_inside_safe(frame, viewport));
}

/// Checks the phone's drawer: its cells, header and how it moves the strip.
void phone_drawer_lays_out_its_cells() {
    const hud::Viewport viewport = phone_852();
    hud::HudState state = match_state();
    state.rail_count = 6;
    state.sheet = hud::Sheet::drawer;
    state.drawer_cells_wanted = 7;
    state.drawer_pages = 2;
    const hud::Frame frame = hud::lay_out(viewport, state);
    OA_CHECK(same(frame.sheet, {59, 0, 276, 372}));
    OA_CHECK(frame.drawer_cell_count == 7);
    OA_CHECK(same(frame.drawer_cells[0], {67, 100, 84, 84}));
    OA_CHECK(same(frame.drawer_cells[1], {155, 100, 84, 84}));
    OA_CHECK(same(frame.drawer_cells[2], {243, 100, 84, 84}));
    OA_CHECK(same(frame.drawer_cells[3], {67, 188, 84, 84}));
    OA_CHECK(same(frame.drawer_cells[6], {67, 276, 84, 84}));
    for (uint8_t cell = 0; cell < frame.drawer_cell_count; ++cell) {
        OA_CHECK(inside(frame.drawer_cells[cell], frame.drawer_grid));
        OA_CHECK(inside(frame.drawer_cells[cell], frame.sheet));
    }
    OA_CHECK(same(find(frame, Control::drawer_close), {67, 8, 44, 44}));
    OA_CHECK(same(find(frame, Control::drawer_next), {283, 8, 44, 44}));
    OA_CHECK(same(find(frame, Control::drawer_prev), {235, 8, 44, 44}));
    OA_CHECK(same(find(frame, Control::drawer_build_tab), {67, 56, 84, 36}));
    OA_CHECK(same(find(frame, Control::drawer_orders_tab), {155, 56, 84, 36}));
    OA_CHECK(same(find(frame, Control::times_five), {243, 56, 44, 36}));
    // The column is under the drawer; the strip and pill move right of it.
    OA_CHECK(empty(frame.minimap));
    OA_CHECK(count_of(frame, Control::queue) == 0);
    OA_CHECK(same(frame.resources, {347, 8, 334, 28}));
    OA_CHECK(!overlap(frame.status, frame.sheet));
    OA_CHECK(no_overlaps(frame));
    OA_CHECK(all_inside_safe(frame, viewport));
    // Open sheets do not shrink the clear area.
    OA_CHECK(same(frame.clear, {183, 76, 526, 236}));
    // One page: no PREV or NEXT.
    state.drawer_pages = 1;
    OA_CHECK(count_of(hud::lay_out(viewport, state), Control::drawer_next) == 0);
    // More cells than fit are left out.
    state.drawer_cells_wanted = 12;
    OA_CHECK(hud::lay_out(viewport, state).drawer_cell_count == 9);
}

/// Checks the phone's MORE sheet: toggles two across, buttons three across, then the last row.
void phone_more_sheet_lays_out_its_cells() {
    const hud::Viewport viewport = phone_852();
    hud::HudState state = match_state();
    state.rail_count = 6;
    state.sheet = hud::Sheet::more;
    state.more_toggle_count = 3;
    state.more_button_count = 4;
    const hud::Frame frame = hud::lay_out(viewport, state);
    OA_CHECK(same(frame.sheet, {293, 108, 420, 256}));
    OA_CHECK(frame.more_cell_count == 7);
    OA_CHECK(same(frame.more_cells[0], {301, 116, 200, 40}));
    OA_CHECK(same(frame.more_cells[1], {505, 116, 200, 40}));
    OA_CHECK(same(frame.more_cells[2], {301, 160, 200, 40}));
    OA_CHECK(same(frame.more_cells[3], {301, 208, 132, 44}));
    OA_CHECK(same(frame.more_cells[4], {437, 208, 132, 44}));
    OA_CHECK(same(frame.more_cells[5], {573, 208, 132, 44}));
    OA_CHECK(same(frame.more_cells[6], {301, 256, 132, 44}));
    const Rect info = find(frame, Control::more_item, static_cast<int>(hud::MoreItem::info));
    const Rect destruct =
        find(frame, Control::more_item, static_cast<int>(hud::MoreItem::self_destruct));
    OA_CHECK(same(info, {301, 308, 132, 48}));
    OA_CHECK(same(destruct, {437, 308, 268, 48}));
    for (uint8_t cell = 0; cell < frame.more_cell_count; ++cell) {
        OA_CHECK(inside(frame.more_cells[cell], frame.more_grid));
        OA_CHECK(inside(frame.more_cells[cell], frame.sheet));
        OA_CHECK(frame.more_cells[cell].y + frame.more_cells[cell].height <= info.y);
    }
    // MORE stays reachable beside the sheet, to close it again.
    OA_CHECK(count_of(frame, Control::more) == 1);
    OA_CHECK(!overlap(frame.sheet, find(frame, Control::more)));
    OA_CHECK(no_overlaps(frame));
    OA_CHECK(all_inside_safe(frame, viewport));
}

/// Checks the phone's menus: SELECT ▾ beside the column, MENU's sheet left of the rail.
void phone_menus_open_beside_their_buttons() {
    const hud::Viewport viewport = phone_852();
    hud::HudState state = match_state();
    state.rail_count = 6;
    state.sheet = hud::Sheet::select_menu;
    hud::Frame frame = hud::lay_out(viewport, state);
    OA_CHECK(count_of(frame, Control::menu_item) == static_cast<int>(hud::select_item_count));
    OA_CHECK(frame.sheet.x == 171 + 8);
    OA_CHECK(inside(frame.sheet, safe_rect(viewport)));
    OA_CHECK(frame.sheet.y >= 68 + 8);
    OA_CHECK(no_overlaps(frame));
    state.sheet = hud::Sheet::phone_menu;
    frame = hud::lay_out(viewport, state);
    OA_CHECK(same(frame.sheet, {537, 60, 176, 252}));
    OA_CHECK(count_of(frame, Control::menu_item) == 5);
    OA_CHECK(count_of(frame, Control::order_slot) == 5);
    OA_CHECK(no_overlaps(frame));
}

/// The phone MENU at Large and Larger stays in the safe area, in two columns
/// when one does not fit. CHAT is one of the items.
void phone_menu_fits_when_controls_are_large() {
    const hud::Viewport bases[] = {
        phone_viewport(852, 393, {59, 0, 59, 21}),
        phone_viewport(667, 375, {0, 0, 0, 0}),
    };
    const float scales[] = {1.25f, 1.5f};
    for (const hud::Viewport& base : bases) {
        for (const float scale : scales) {
            const hud::Viewport viewport =
                phone_viewport(base.width, base.height, base.safe, scale);
            hud::HudState state = match_state();
            state.rail_count = 6;
            state.sheet = hud::Sheet::phone_menu;
            const hud::Frame frame = hud::lay_out(viewport, state);
            OA_CHECK(count_of(frame, Control::menu_item) == 5);
            OA_CHECK(all_inside_safe(frame, viewport));
            OA_CHECK(no_overlaps(frame));
            const Rect chat =
                find(frame, Control::menu_item, static_cast<int>(hud::PhoneMenuItem::chat));
            OA_CHECK(chat.width > 0);
            OA_CHECK(inside(chat, safe_rect(viewport)));
            const Rect photo =
                find(frame, Control::menu_item, static_cast<int>(hud::PhoneMenuItem::photo_mode));
            OA_CHECK(photo.width > 0);
            OA_CHECK(inside(photo, safe_rect(viewport)));
            // The sheet stays to the right of the minimap. A tap on an item's
            // left end chooses that item, including where the minimap used to cover it.
            OA_CHECK(!overlap(frame.sheet, frame.minimap));
            for (int item = 0; item < 5; ++item) {
                const Rect row = find(frame, Control::menu_item, item);
                const auto chosen = hud::hit(frame, {row.x + 1, row.y + row.height / 2}, 0);
                OA_CHECK(
                    chosen.has_value() && chosen->control == Control::menu_item &&
                    chosen->index == static_cast<uint8_t>(item)
                );
            }
            // One column is 176 points wide. Two columns are wider than that.
            const int one_column = static_cast<int>(std::lround(176.0f * scale));
            OA_CHECK(frame.sheet.width > one_column);
        }
    }
}

/// Returns a phone state that shows most of what a frame can hold.
std::vector<hud::HudState> phone_states() {
    std::vector<hud::HudState> states;
    hud::HudState state = match_state();
    state.rail_count = 6;
    state.group_counts[1] = 3;
    state.group_counts[4] = 7;
    states.push_back(state);
    state.banner.shown = true;
    state.placement.active = true;
    state.tip.until_ms = 5000;
    state.tip.anchor = {121, 251};
    states.push_back(state);
    hud::HudState drawer = states[0];
    drawer.sheet = hud::Sheet::drawer;
    drawer.drawer_cells_wanted = 7;
    drawer.drawer_pages = 3;
    states.push_back(drawer);
    hud::HudState more = states[0];
    more.sheet = hud::Sheet::more;
    more.more_toggle_count = 3;
    more.more_button_count = 4;
    states.push_back(more);
    hud::HudState select = states[0];
    select.sheet = hud::Sheet::select_menu;
    states.push_back(select);
    hud::HudState menu = states[0];
    menu.sheet = hud::Sheet::phone_menu;
    states.push_back(menu);
    return states;
}

/// Checks that three pixels a point triple every rectangle of the phone's frame.
void phone_scales_with_pixels_per_point() {
    const hud::Viewport points = phone_852();
    const hud::Viewport pixels =
        phone_viewport(852 * 3, 393 * 3, {59 * 3, 0, 59 * 3, 21 * 3}, 3.0f);
    OA_CHECK(hud::rail_capacity(pixels) == 6);
    for (const hud::HudState& state : phone_states()) {
        hud::HudState scaled = state;
        scaled.tip.anchor = {state.tip.anchor.x * 3, state.tip.anchor.y * 3};
        const hud::Frame small = hud::lay_out(points, state);
        const hud::Frame large = hud::lay_out(pixels, scaled);
        hud::Frame tripled = small;
        for (uint8_t slot = 0; slot < tripled.control_count; ++slot)
            tripled.controls[slot].rect = times(small.controls[slot].rect, 3);
        for (std::size_t cell = 0; cell < hud::max_drawer_cells; ++cell)
            tripled.drawer_cells[cell] = times(small.drawer_cells[cell], 3);
        for (std::size_t cell = 0; cell < hud::max_more_cells; ++cell)
            tripled.more_cells[cell] = times(small.more_cells[cell], 3);
        for (Rect* region :
             {&tripled.minimap,
              &tripled.resources,
              &tripled.status,
              &tripled.banner,
              &tripled.tip,
              &tripled.sheet,
              &tripled.drawer_grid,
              &tripled.more_grid,
              &tripled.placement_bar,
              &tripled.panel_sheet,
              &tripled.clear})
            *region = times(*region, 3);
        OA_CHECK(same_frame(tripled, large));
        OA_CHECK(no_overlaps(large));
        OA_CHECK(all_inside_safe(large, pixels));
    }
}

/// Checks that the pressed, lit, progress and text fields never move a rectangle.
void looks_never_move_rectangles() {
    for (const hud::Viewport& viewport : {tablet_viewport(), phone_852()}) {
        for (hud::HudState state : phone_states()) {
            if (viewport.device == hud::DeviceClass::tablet &&
                (state.sheet == hud::Sheet::drawer || state.sheet == hud::Sheet::more))
                state.sheet = hud::Sheet::speed;
            hud::HudState other = state;
            other.revision = 99;
            other.paused = true;
            other.menu_open = true;
            other.following = true;
            other.chat_open = true;
            other.has_selection = true;
            other.builder_selected = true;
            other.latch_mode = hud::LatchMode::one_action;
            other.latches.press(hud::Latch::queue, 0);
            other.latches.release(hud::Latch::queue, 10, 350);
            other.latches.press(hud::Latch::add, 20);
            other.pressed[0] = {Control::queue, 0};
            other.pressed[1] = {Control::order_slot, 2};
            other.drawer_tab = hud::DrawerTab::orders;
            other.drawer_page = 2;
            other.drawer_title = "Kbot Lab";
            other.selected_group = 4;
            for (auto& slot : other.rail) {
                slot.order = hud::Order::patrol;
                slot.available = true;
                slot.lit = true;
                slot.more = true;
            }
            other.banner.order = hud::Order::patrol;
            other.banner.hint = "TAP POINTS · QUEUE KEEPS ADDING";
            other.placement.legal = true;
            other.placement.anchor = {300, 200};
            other.placement.name = "Solar Collector";
            other.selection_text = "Commander + 4 Tanks";
            other.tap_action = hud::TapAction::move;
            other.enemy_action = hud::TapAction::attack;
            other.tip.text = "QUEUE";
            other.self_destruct_progress = 0.5f;
            OA_CHECK(same_frame(hud::lay_out(viewport, state), hud::lay_out(viewport, other)));
        }
    }
}

/// Checks the left-handed layouts: the phone's column and minimap on the right and the rail
/// on the left; the tablet's controls mirrored about the battlefield, its 3.1c panel kept.
void left_handed_mirrors_the_controls() {
    const hud::Viewport right = phone_852();
    hud::Viewport left = right;
    left.left_handed = true;
    hud::HudState state = match_state();
    state.rail_count = 6;
    state.group_counts[2] = 1;
    const hud::Frame normal = hud::lay_out(right, state);
    const hud::Frame mirror = hud::lay_out(left, state);
    OA_CHECK(same(mirror.minimap, {681, 8, 104, 104}));
    OA_CHECK(same(mirror.minimap, hud::mirrored(normal.minimap, right)));
    OA_CHECK(same(find(mirror, Control::order_slot, 0), {67, 60, 64, 44}));
    OA_CHECK(
        same(find(mirror, Control::queue), hud::mirrored(find(normal, Control::queue), right))
    );
    OA_CHECK(same(find(mirror, Control::menu), {67, 8, 44, 44}));
    OA_CHECK(same(mirror.clear, hud::mirrored(normal.clear, right)));
    OA_CHECK(same(mirror.resources, hud::mirrored(normal.resources, right)));
    OA_CHECK(no_overlaps(mirror));
    OA_CHECK(all_inside_safe(mirror, left));
    OA_CHECK(overlaps_no_control(mirror, mirror.clear));
    const Rect sample{10, 20, 30, 40};
    OA_CHECK(same(hud::mirrored(hud::mirrored(sample, right), right), sample));
    OA_CHECK(same(hud::mirrored(sample, right), {812, 20, 30, 40}));

    // The drawer opens from the right edge, its insides in reading order.
    state.sheet = hud::Sheet::drawer;
    state.drawer_cells_wanted = 4;
    const hud::Frame drawer = hud::lay_out(left, state);
    OA_CHECK(same(drawer.sheet, {517, 0, 276, 372}));
    OA_CHECK(same(drawer.drawer_cells[0], {525, 100, 84, 84}));
    OA_CHECK(same(drawer.drawer_cells[1], {613, 100, 84, 84}));
    OA_CHECK(same(find(drawer, Control::drawer_close), {525, 8, 44, 44}));
    OA_CHECK(no_overlaps(drawer));
    OA_CHECK(all_inside_safe(drawer, left));

    const hud::Viewport tablet = tablet_viewport();
    hud::Viewport tablet_left = tablet;
    tablet_left.left_handed = true;
    const hud::Frame tablet_mirror = hud::lay_out(tablet_left, match_state());
    OA_CHECK(same(find(tablet_mirror, Control::queue), {1092, 579, 76, 62}));
    OA_CHECK(same(find(tablet_mirror, Control::pause), {227, 63, 60, 52}));
    OA_CHECK(same(find(tablet_mirror, Control::menu), {1097, 0, 75, 55}));
    OA_CHECK(same(tablet_mirror.clear, {295, 55, 885, 516}));
    OA_CHECK(overlaps_no_control(tablet_mirror, {0, 0, tablet.chrome.left, tablet.height}));
    OA_CHECK(no_overlaps(tablet_mirror));
    OA_CHECK(all_inside_safe(tablet_mirror, tablet_left));
    OA_CHECK(overlaps_no_control(tablet_mirror, tablet_mirror.clear));
}

/// Checks hit tests: exact, nearest within the radius, nothing beyond, and sheet_outside.
void hits_find_the_nearest_control() {
    const hud::Viewport viewport = phone_852();
    hud::HudState state = match_state();
    state.rail_count = 6;
    hud::Frame frame = hud::lay_out(viewport, state);
    auto found = hud::hit(frame, {92, 197}, 22);
    OA_CHECK(found.has_value() && found->control == Control::queue);
    found = hud::hit(frame, {200, 300}, 22);
    OA_CHECK(found.has_value() && found->control == Control::group_store);
    // 15 px right of ADD's right edge, in the open battlefield.
    found = hud::hit(frame, {171 + 15, 197}, 22);
    OA_CHECK(found.has_value() && found->control == Control::add);
    OA_CHECK(!hud::hit(frame, {171 + 30, 197}, 22).has_value());
    OA_CHECK(!hud::hit(frame, {400, 200}, 22).has_value());
    found = hud::hit(frame, {700, 82}, 22);
    OA_CHECK(found.has_value() && found->control == Control::order_slot && found->index == 0);
    // A sheet open: a point far from it closes it; near an item finds the item.
    state.sheet = hud::Sheet::phone_menu;
    frame = hud::lay_out(viewport, state);
    found = hud::hit(frame, {400, 200}, 22);
    OA_CHECK(found.has_value() && found->control == Control::sheet_outside);
    found = hud::hit(frame, {545 + 80, 68 + 22}, 22);
    OA_CHECK(found.has_value() && found->control == Control::menu_item && found->index == 0);
    found = hud::hit(frame, {537 + 2, 60 + 2}, 0);
    OA_CHECK(!found.has_value());
    // A radial open: outside its ring closes it.
    state.sheet = hud::Sheet::none;
    hud::RadialAvailability availability{};
    state.radial = hud::make_radial({400, 200}, availability, viewport);
    frame = hud::lay_out(viewport, state);
    found = hud::hit(frame, {800, 380}, 22);
    OA_CHECK(found.has_value() && found->control == Control::sheet_outside);
    const auto& radial = *state.radial;
    found = hud::hit(frame, radial.centre, 22);
    OA_CHECK(found.has_value() && found->control == Control::radial_hub);
    for (const auto& wedge : radial.wedges) {
        found = hud::hit(frame, wedge.label, 22);
        OA_CHECK(found.has_value() && found->control == Control::radial_item);
        OA_CHECK(found.has_value() && found->index == static_cast<uint8_t>(wedge.item));
    }
    OA_CHECK(no_overlaps(frame));
}

/// Checks nearest_rect and covers.
void nearest_rect_and_covers() {
    const std::vector<Rect> rects{{0, 0, 10, 10}, {40, 0, 10, 10}, {0, 0, 0, 0}, {20, 30, 5, 5}};
    OA_CHECK(hud::nearest_rect(rects.data(), rects.size(), {5, 5}, 22) == 0);
    OA_CHECK(hud::nearest_rect(rects.data(), rects.size(), {30, 5}, 22) == 1);
    OA_CHECK(hud::nearest_rect(rects.data(), rects.size(), {22, 32}, 0) == 3);
    OA_CHECK(hud::nearest_rect(rects.data(), rects.size(), {100, 100}, 22) == -1);
    OA_CHECK(hud::nearest_rect(rects.data(), rects.size(), {15, 5}, 4) == -1);
    OA_CHECK(hud::nearest_rect(rects.data(), rects.size(), {15, 5}, 6) == 0);
    OA_CHECK(hud::nearest_rect(nullptr, 0, {0, 0}, 22) == -1);

    const hud::Viewport viewport = phone_852();
    hud::HudState state = match_state();
    state.rail_count = 6;
    const hud::Frame frame = hud::lay_out(viewport, state);
    OA_CHECK(hud::covers(frame, {92, 197}));
    OA_CHECK(hud::covers(frame, {300, 50}));
    OA_CHECK(hud::covers(frame, {100, 50}));
    OA_CHECK(!hud::covers(frame, {400, 200}));
    OA_CHECK(!hud::covers(frame, {20, 200}));
}

/// Checks the radial: fixed slots, greyed wedges kept, the corner clamp, hits and the hub.
void radial_keeps_its_slots() {
    const hud::Viewport viewport = phone_852();
    hud::RadialAvailability none{};
    hud::RadialAvailability all{};
    all.orders.fill(true);
    all.info = true;
    all.type = true;
    all.blast_slot_shows_blast = true;
    all.default_item = hud::RadialItem::attack;
    const hud::Radial greyed = hud::make_radial({400, 200}, none, viewport);
    const hud::Radial open = hud::make_radial({400, 200}, all, viewport);
    OA_CHECK(open.inner_radius == 44 && open.outer_radius == 128);
    for (std::size_t slot = 0; slot < hud::radial_slot_count; ++slot) {
        OA_CHECK(greyed.wedges[slot].label.x == open.wedges[slot].label.x);
        OA_CHECK(greyed.wedges[slot].label.y == open.wedges[slot].label.y);
        OA_CHECK(!greyed.wedges[slot].available);
        OA_CHECK(open.wedges[slot].available);
        OA_CHECK(open.wedges[slot].hit.width >= 44 && open.wedges[slot].hit.height >= 44);
    }
    // Clockwise from the top: move above, the shared slot right, info below, reclaim... left.
    OA_CHECK(open.wedges[0].item == hud::RadialItem::move);
    OA_CHECK(open.wedges[0].label.x == open.centre.x && open.wedges[0].label.y < open.centre.y);
    OA_CHECK(open.wedges[3].label.y == open.centre.y && open.wedges[3].label.x > open.centre.x);
    OA_CHECK(open.wedges[6].item == hud::RadialItem::info);
    OA_CHECK(open.wedges[6].label.x == open.centre.x && open.wedges[6].label.y > open.centre.y);
    OA_CHECK(open.wedges[9].item == hud::RadialItem::repair);
    OA_CHECK(open.wedges[9].label.y == open.centre.y && open.wedges[9].label.x < open.centre.x);
    const std::vector<hud::RadialItem> order{
        hud::RadialItem::move,
        hud::RadialItem::patrol,
        hud::RadialItem::attack,
        hud::RadialItem::blast,
        hud::RadialItem::capture,
        hud::RadialItem::stop,
        hud::RadialItem::info,
        hud::RadialItem::type,
        hud::RadialItem::reclaim,
        hud::RadialItem::repair,
        hud::RadialItem::unload,
        hud::RadialItem::guard,
    };
    for (std::size_t slot = 0; slot < hud::radial_slot_count; ++slot)
        OA_CHECK(open.wedges[slot].item == order[slot]);
    // The D-gun and Load share slot 3.
    OA_CHECK(greyed.wedges[3].item == hud::RadialItem::load);
    OA_CHECK(open.wedges[2].default_item && !open.wedges[0].default_item);
    // Hits: each label gives its item; the hub; the ring's edge; nothing far off.
    for (const auto& wedge : open.wedges) {
        const auto item = hud::radial_hit(open, wedge.label, viewport);
        OA_CHECK(item.has_value() && *item == wedge.item);
        OA_CHECK(!hud::radial_hub_hit(open, wedge.label));
    }
    OA_CHECK(hud::radial_hub_hit(open, open.centre));
    OA_CHECK(!hud::radial_hit(open, open.centre, viewport).has_value());
    const hud::Point beyond{open.centre.x, open.centre.y - 128 - 15};
    const auto edge = hud::radial_hit(open, beyond, viewport);
    OA_CHECK(edge.has_value() && *edge == hud::RadialItem::move);
    OA_CHECK(
        !hud::radial_hit(open, {open.centre.x, open.centre.y - 128 - 30}, viewport).has_value()
    );
    // A corner anchor moves the ring inside the safe area and keeps the anchor.
    const Rect safe = safe_rect(viewport);
    for (const hud::Point corner : {hud::Point{0, 0}, hud::Point{851, 392}, hud::Point{60, 380}}) {
        const hud::Radial clamped = hud::make_radial(corner, all, viewport);
        OA_CHECK(clamped.anchor.x == corner.x && clamped.anchor.y == corner.y);
        const Rect ring{
            clamped.centre.x - clamped.outer_radius,
            clamped.centre.y - clamped.outer_radius,
            2 * clamped.outer_radius,
            2 * clamped.outer_radius
        };
        OA_CHECK(inside(ring, safe));
        for (const auto& wedge : clamped.wedges)
            OA_CHECK(inside(wedge.hit, safe));
    }
    // The radial's orders.
    OA_CHECK(hud::radial_order(hud::RadialItem::blast) == hud::Order::blast);
    OA_CHECK(hud::radial_order(hud::RadialItem::load) == hud::Order::load);
    OA_CHECK(hud::radial_order(hud::RadialItem::guard) == hud::Order::guard);
    OA_CHECK(hud::radial_order(hud::RadialItem::repair) == hud::Order::repair);
    OA_CHECK(!hud::radial_order(hud::RadialItem::info).has_value());
    OA_CHECK(!hud::radial_order(hud::RadialItem::type).has_value());
    // On a tablet the ring stays on the battlefield.
    const hud::Viewport tablet = tablet_viewport();
    const hud::Radial tablet_radial = hud::make_radial({10, 10}, all, tablet);
    OA_CHECK(tablet_radial.centre.x - tablet_radial.outer_radius >= tablet.chrome.left);
    OA_CHECK(tablet_radial.centre.y - tablet_radial.outer_radius >= tablet.chrome.top);
}

/// Returns a tablet viewport of a size in pixels with insets in pixels.
hud::Viewport
tablet_viewport_of(int width, int height, hud::Insets safe, float px_per_point = 1.0f) {
    hud::Viewport viewport{};
    viewport.width = width;
    viewport.height = height;
    viewport.px_per_point = px_per_point;
    viewport.safe = safe;
    viewport.device = hud::DeviceClass::tablet;
    viewport.chrome = layout::make_match_layout(
        width, height, layout::kMaxChromeScale * static_cast<double>(px_per_point)
    );
    return viewport;
}

/// Checks many window sizes, both hands and every sheet: no overlaps, everything inside the
/// safe area, the clear area free of controls and inside the safe area.
void every_window_keeps_the_rules() {
    std::vector<hud::Viewport> viewports{
        tablet_viewport(),
        tablet_viewport_of(820, 1180, {0, 24, 0, 20}),
        tablet_viewport_of(1133, 744, {0, 0, 0, 20}),
        tablet_viewport_of(744, 1133, {0, 24, 0, 20}),
        tablet_viewport_of(1366, 1024, {0, 0, 0, 20}),
        tablet_viewport_of(2732, 2048, {0, 0, 0, 40}, 2.0f),
        tablet_viewport_of(640, 480, {0, 0, 0, 0}),
        tablet_viewport_of(1920, 1080, {0, 0, 0, 0}),
        phone_852(),
        phone_viewport(956, 440, {62, 0, 62, 21}),
        phone_viewport(667, 375, {0, 0, 0, 0}),
        phone_viewport(844, 390, {47, 0, 47, 21}),
        phone_viewport(2556, 1179, {177, 0, 177, 63}, 3.0f),
    };
    std::vector<hud::HudState> states = phone_states();
    hud::HudState busy = match_state();
    busy.build_page_loaded = true;
    busy.shared_game = true;
    for (std::size_t group = 1; group <= 9; ++group)
        busy.group_counts[group] = 2;
    busy.banner.shown = true;
    busy.placement.active = true;
    states.push_back(busy);
    hud::HudState speed = match_state();
    speed.sheet = hud::Sheet::speed;
    states.push_back(speed);
    hud::RadialAvailability availability{};
    availability.orders.fill(true);
    for (hud::Viewport viewport : viewports) {
        for (const bool left_handed : {false, true}) {
            viewport.left_handed = left_handed;
            for (hud::HudState state : states) {
                state.rail_count = hud::rail_capacity(viewport);
                const hud::Frame frame = hud::lay_out(viewport, state);
                OA_CHECK(no_overlaps(frame));
                OA_CHECK(all_inside_safe(frame, viewport));
                OA_CHECK(inside(frame.clear, safe_rect(viewport)));
                if (state.sheet == hud::Sheet::none)
                    OA_CHECK(overlaps_no_control(frame, frame.clear));
                OA_CHECK(count_of(frame, Control::pause) == 1);
                OA_CHECK(
                    count_of(frame, Control::menu) ==
                    (empty(frame.sheet) ? 1 : count_of(frame, Control::menu))
                );
                // The radial opened anywhere stays inside the safe area.
                for (const hud::Point anchor :
                     {hud::Point{0, 0},
                      hud::Point{viewport.width / 2, viewport.height / 2},
                      hud::Point{viewport.width - 1, viewport.height - 1}}) {
                    hud::HudState with_radial = state;
                    with_radial.sheet = hud::Sheet::none;
                    with_radial.radial = hud::make_radial(anchor, availability, viewport);
                    const hud::Frame radial_frame = hud::lay_out(viewport, with_radial);
                    OA_CHECK(no_overlaps(radial_frame));
                    OA_CHECK(all_inside_safe(radial_frame, viewport));
                    OA_CHECK(count_of(radial_frame, Control::radial_item) == 12);
                    OA_CHECK(count_of(radial_frame, Control::radial_hub) == 1);
                }
            }
        }
    }
}

/// Checks the help lines of the 3.1c order panel's gadgets: every order button, the
/// standing-order toggles and the page buttons have one, matched by the word the name holds
/// whatever the side prefix and case; a build button and the header have none.
void order_panel_gadgets_have_help() {
    for (std::size_t value = 0; value < hud::order_count; ++value) {
        const auto order = static_cast<hud::Order>(value);
        const std::string arm = "ARM" + std::string(hud::order_name(order));
        const auto help = hud::gadget_help(arm);
        OA_CHECK(!help.empty());
        // The help names the order as the controls label it (GUARD for DEFEND, D-GUN for BLAST).
        OA_CHECK(help.substr(0, hud::order_label(order).size()) == hud::order_label(order));
    }
    OA_CHECK(hud::gadget_help("CORMOVE") == hud::gadget_help("ARMMOVE"));
    OA_CHECK(hud::gadget_help("armmove") == hud::gadget_help("ARMMOVE"));
    // A word that holds another is told apart.
    OA_CHECK(hud::gadget_help("ARMMOVEORD").substr(0, 11) == "Move orders");
    OA_CHECK(hud::gadget_help("ARMFIREORD").substr(0, 11) == "Fire orders");
    OA_CHECK(hud::gadget_help("ARMUNLOAD").substr(0, 6) == "UNLOAD");
    OA_CHECK(hud::gadget_help("ARMLOAD").substr(0, 4) == "LOAD");
    for (const std::string_view name :
         {"ARMONOFF", "ARMCLOAK", "ARMORDERS", "ARMBUILD", "ARMPREV", "ARMNEXT"})
        OA_CHECK(!hud::gadget_help(name).empty());
    OA_CHECK(hud::gadget_help("ARMPW").empty());
    OA_CHECK(hud::gadget_help("HEADER").empty());
    OA_CHECK(hud::gadget_help("").empty());
}

/// Checks every control's help line, the labels and every menu item's label.
void every_control_has_its_texts() {
    const hud::HudState state = match_state();
    for (int value = static_cast<int>(Control::queue);
         value <= static_cast<int>(Control::group_wedge);
         ++value) {
        const auto control = static_cast<Control>(value);
        OA_CHECK(!hud::control_help(control, 0).empty());
        OA_CHECK(!hud::control_help(control, 1).empty());
    }
    for (uint8_t item = 0; item < 13; ++item) {
        OA_CHECK(!hud::control_help(Control::radial_item, item).empty());
        OA_CHECK(!hud::control_label(Control::radial_item, item, state).empty());
    }
    OA_CHECK(
        hud::control_help(Control::queue, 0) ==
        "QUEUE: Shift for orders, building and placement. Tap to latch, hold for one."
    );
    OA_CHECK(hud::control_label(Control::queue, 0, state) == "QUEUE");
    OA_CHECK(hud::control_label_lookup(Control::queue, 0, state) == "QUEUE");
    // The rail's NEXT centres the next unit, as SELECT ▾'s NEXT UNIT does, and the drawer's
    // NEXT shows the next build page: each is looked up by what it means.
    OA_CHECK(hud::control_label(Control::next_unit, 0, state) == "NEXT");
    OA_CHECK(hud::control_label_lookup(Control::next_unit, 0, state) == "NEXT UNIT");
    OA_CHECK(
        hud::control_label_lookup(Control::next_unit, 0, state) ==
        hud::menu_item_label(
            hud::Sheet::select_menu, static_cast<uint8_t>(hud::SelectItem::next_unit)
        )
    );
    OA_CHECK(hud::control_label(Control::drawer_next, 0, state) == "NEXT");
    OA_CHECK(hud::control_label_lookup(Control::drawer_next, 0, state) == "NEXT PAGE");
    OA_CHECK(hud::control_label(Control::add, 0, state) == "ADD");
    OA_CHECK(hud::control_label(Control::times_five, 0, state) == "x5");
    OA_CHECK(hud::control_label(Control::clear, 0, state) == "CLEAR");
    OA_CHECK(hud::control_label(Control::select_menu, 0, state) == "SELECT");
    OA_CHECK(hud::control_label(Control::group_chip, 3, state) == "3");
    OA_CHECK(hud::control_label(Control::build_drawer, 0, state) == "ORDERS");
    hud::HudState builder = state;
    builder.builder_selected = true;
    builder.rail[1].order = hud::Order::blast;
    builder.rail[2].more = true;
    OA_CHECK(hud::control_label(Control::build_drawer, 0, builder) == "BUILD");
    OA_CHECK(hud::control_label(Control::order_slot, 1, builder) == "D-GUN");
    OA_CHECK(hud::control_label(Control::order_slot, 2, builder) == "MORE");
    OA_CHECK(
        hud::control_label(Control::more_item, static_cast<uint8_t>(hud::MoreItem::info), state) ==
        "INFO"
    );
    OA_CHECK(!hud::control_label(
                  Control::more_item, static_cast<uint8_t>(hud::MoreItem::self_destruct), state
    )
                  .empty());
    OA_CHECK(hud::control_label(Control::none, 0, state).empty());
    for (uint8_t item = 0; item < hud::select_item_count; ++item)
        OA_CHECK(!hud::menu_item_label(hud::Sheet::select_menu, item).empty());
    OA_CHECK(hud::menu_item_label(hud::Sheet::select_menu, hud::select_item_count).empty());
    for (uint8_t item = 0; item < 2; ++item)
        OA_CHECK(!hud::menu_item_label(hud::Sheet::speed, item).empty());
    OA_CHECK(hud::menu_item_label(hud::Sheet::speed, 2).empty());
    for (uint8_t item = 0; item < 5; ++item)
        OA_CHECK(!hud::menu_item_label(hud::Sheet::phone_menu, item).empty());
    OA_CHECK(hud::menu_item_label(hud::Sheet::phone_menu, 5).empty());
    OA_CHECK(hud::menu_item_label(hud::Sheet::phone_menu, 4) == "PHOTO MODE");
    OA_CHECK(hud::menu_item_label(hud::Sheet::more, 0).empty());
    OA_CHECK(hud::menu_item_label(hud::Sheet::drawer, 0).empty());
    OA_CHECK(hud::menu_item_label(hud::Sheet::select_menu, 0) == "ALL");
    OA_CHECK(hud::menu_item_label(hud::Sheet::phone_menu, 0) == "GAME MENU");
    hud::HudState menu = state;
    menu.sheet = hud::Sheet::speed;
    OA_CHECK(hud::control_label(Control::menu_item, 1, menu) == "FASTER");
}

namespace pad = oa::ui::pad_controls;

/// Returns the Steam Deck's screen at a Control size, as the runtime lays it out: 1280x800
/// window points of one canvas pixel each, the 3.1c chrome for that canvas, the touch layer's
/// points scaled, and the class from the window's points over the scale.
hud::Viewport deck_viewport(float scale) {
    hud::Viewport viewport{};
    viewport.width = 1280;
    viewport.height = 800;
    viewport.px_per_point = scale;
    viewport.device =
        hud::classify_device(static_cast<int>(1280.0f / scale), static_cast<int>(800.0f / scale));
    viewport.chrome = layout::make_match_layout(1280, 800);
    return viewport;
}

/// Returns a tablet viewport's battlefield inside its safe area, above the 3.1c bottom bar.
Rect battlefield_of(const hud::Viewport& viewport) {
    const Rect safe = safe_rect(viewport);
    const int left = std::max(viewport.chrome.left, safe.x);
    const int top = std::max(viewport.chrome.top, safe.y);
    const int bottom = std::min(viewport.chrome.bottom_bar_y(), safe.y + safe.height);
    return {left, top, safe.x + safe.width - left, bottom - top};
}

/// Returns the map of a Steam Deck with Steam Input off: trackpads and grips, right-handed.
pad::MapContext deck_map() {
    pad::MapContext map{};
    map.scheme = pad::Scheme::trackpads;
    map.trackpads = true;
    return map;
}

/// Returns the map of a pad with no grips and no trackpads: the fallback, sticks scheme.
pad::MapContext fallback_map() {
    pad::MapContext map{};
    map.scheme = pad::Scheme::sticks;
    map.fallback = true;
    return map;
}

/// Returns a rectangle's centre.
hud::Point middle_of(const Rect& rect) {
    return {rect.x + rect.width / 2, rect.y + rect.height / 2};
}

/// Returns a builder's build ring content: six build buttons, NEXT at E and PREV at W, a
/// factory's count on one and one greyed.
hud::BuildRingContent builder_ring_content() {
    hud::BuildRingContent content{};
    for (std::size_t slot = 0; slot < hud::build_ring_slot_count; ++slot) {
        content.kinds[slot] = hud::BuildWedgeKind::build;
        content.gadgets[slot] = static_cast<int16_t>(10 + slot);
        content.available[slot] = true;
    }
    content.kinds[hud::build_ring_next_slot] = hud::BuildWedgeKind::next;
    content.kinds[hud::build_ring_prev_slot] = hud::BuildWedgeKind::prev;
    content.queued[1] = 5;
    content.available[3] = false;
    return content;
}

/// Checks the Steam Deck's 1280x800 screen at Control size Standard, Large and Larger: always
/// the tablet layout (at Larger the window is 853x533 points), every control on the screen and
/// inside the safe area, none overlapping, whatever the controls show.
void deck_screen_keeps_the_tablet_layout() {
    OA_CHECK(hud::classify_device(853, 533) == hud::DeviceClass::tablet);
    const Rect screen{0, 0, 1280, 800};
    hud::RadialAvailability availability{};
    availability.orders.fill(true);
    for (const float scale : {1.0f, 1.25f, 1.5f}) {
        const hud::Viewport viewport = deck_viewport(scale);
        OA_CHECK(viewport.device == hud::DeviceClass::tablet);
        hud::HudState busy = match_state();
        busy.build_page_loaded = true;
        busy.shared_game = true;
        busy.pad.force_shown = true;
        for (std::size_t group = 1; group <= 9; ++group)
            busy.group_counts[group] = 3;
        std::vector<hud::HudState> states{match_state(), busy};
        hud::HudState marked = busy;
        marked.banner.shown = true;
        marked.placement.active = true;
        states.push_back(marked);
        for (const hud::Sheet sheet :
             {hud::Sheet::select_menu, hud::Sheet::speed, hud::Sheet::phone_menu}) {
            hud::HudState open = busy;
            open.sheet = sheet;
            states.push_back(open);
        }
        hud::HudState rings = busy;
        rings.radial = hud::make_radial({640, 400}, availability, viewport);
        states.push_back(rings);
        rings.radial.reset();
        rings.build_ring = hud::make_build_ring({640, 400}, builder_ring_content(), viewport);
        states.push_back(rings);
        rings.build_ring.reset();
        rings.pad.group_ring = hud::make_group_ring(viewport);
        states.push_back(rings);
        hud::HudState slim = busy;
        slim.pad.hud = true;
        states.push_back(slim);
        slim.pad.group_ring = hud::make_group_ring(viewport);
        slim.sheet = hud::Sheet::select_menu;
        states.push_back(slim);
        for (const hud::HudState& state : states) {
            const hud::Frame frame = hud::lay_out(viewport, state);
            OA_CHECK(no_overlaps(frame));
            OA_CHECK(all_inside_safe(frame, viewport));
            for (uint8_t slot = 0; slot < frame.control_count; ++slot)
                OA_CHECK(inside(frame.controls[slot].rect, screen));
            OA_CHECK(inside(frame.clear, screen));
        }
        // The touch layer's controls grow with the scale; the 3.1c chrome does not.
        const hud::Frame plain = hud::lay_out(viewport, match_state());
        OA_CHECK(find(plain, Control::queue).width == static_cast<int>(std::lround(76 * scale)));
        OA_CHECK(find(plain, Control::queue).x == viewport.chrome.left + std::lround(12 * scale));
        OA_CHECK(viewport.chrome.left == deck_viewport(1.0f).chrome.left);
        OA_CHECK(count_of(plain, Control::pause) == 1 && count_of(plain, Control::menu) == 1);
    }
}

/// Checks the slim pad HUD: the status pill at the battlefield's top, QUEUE, ADD and FORCE
/// under it, the stored groups' chips near its bottom, all inside the battlefield, nothing of
/// the touch layout; x5 takes QUEUE's label over a build button without moving anything.
void pad_hud_lays_out_the_slim_frame() {
    const hud::Viewport viewport = deck_viewport(1.5f);
    const Rect field = battlefield_of(viewport);
    hud::HudState state = match_state();
    state.pad.hud = true;
    state.pad.force_shown = true;
    state.group_counts[1] = 4;
    state.group_counts[2] = 12;
    state.group_counts[5] = 1;
    const hud::Frame frame = hud::lay_out(viewport, state);
    // The pill: 420x28 pt, 8 pt under the battlefield's top, centred on it.
    const Rect pill = frame.status;
    OA_CHECK(pill.width == 630 && pill.height == 42);
    OA_CHECK(pill.y == field.y + 12);
    OA_CHECK(std::abs(middle_of(pill).x - middle_of(field).x) <= 2);
    OA_CHECK(inside(pill, field));
    // QUEUE, ADD and FORCE: 88x30 pt, 4 pt under the pill, 6 pt apart, centred.
    const Rect queue = find(frame, Control::queue);
    const Rect add = find(frame, Control::add);
    const Rect force = find(frame, Control::force);
    OA_CHECK(queue.width == 132 && queue.height == 45);
    OA_CHECK(queue.y == pill.y + pill.height + 6);
    OA_CHECK(add.y == queue.y && force.y == queue.y);
    OA_CHECK(add.x == queue.x + 141 && force.x == add.x + 141);
    OA_CHECK(std::abs((queue.x + force.x + force.width) / 2 - middle_of(field).x) <= 2);
    // The group chips: 52x40 pt from 12 pt inside the battlefield's left, on its bottom line.
    OA_CHECK(count_of(frame, Control::group_chip) == 3);
    const Rect first = find(frame, Control::group_chip, 1);
    OA_CHECK(same(first, {field.x + 18, field.y + field.height - 12 - 60, 78, 60}));
    OA_CHECK(find(frame, Control::group_chip, 2).x == first.x + 84);
    OA_CHECK(find(frame, Control::group_chip, 5).x == first.x + 168);
    // Nothing of the touch layout, everything on the battlefield.
    for (const Control control :
         {Control::clear,
          Control::select_menu,
          Control::group_store,
          Control::times_five,
          Control::pause,
          Control::speed,
          Control::centre,
          Control::follow,
          Control::next_unit,
          Control::info,
          Control::menu})
        OA_CHECK(count_of(frame, control) == 0);
    OA_CHECK(frame.control_count == 6);
    for (uint8_t slot = 0; slot < frame.control_count; ++slot)
        OA_CHECK(inside(frame.controls[slot].rect, field));
    OA_CHECK(no_overlaps(frame));
    OA_CHECK(all_inside_safe(frame, viewport));
    // The overlays' area: under the chips, above the group chips.
    OA_CHECK(inside(frame.clear, field));
    OA_CHECK(overlaps_no_control(frame, frame.clear));
    OA_CHECK(frame.clear.y == queue.y + queue.height + 12);
    OA_CHECK(frame.clear.y + frame.clear.height == first.y - 12);
    OA_CHECK(!hud::covers(frame, middle_of(frame.clear)));
    OA_CHECK(hud::covers(frame, middle_of(pill)));

    // Without grips there is no FORCE; QUEUE and ADD stay centred.
    state.pad.force_shown = false;
    const hud::Frame two = hud::lay_out(viewport, state);
    OA_CHECK(count_of(two, Control::force) == 0);
    const Rect two_queue = find(two, Control::queue);
    const Rect two_add = find(two, Control::add);
    OA_CHECK(std::abs((two_queue.x + two_add.x + two_add.width) / 2 - middle_of(field).x) <= 2);
    // Over a build button QUEUE reads x5, in place.
    OA_CHECK(hud::control_label(Control::queue, 0, state) == "QUEUE");
    state.pad.over_build_button = true;
    OA_CHECK(hud::control_label(Control::queue, 0, state) == "x5");
    OA_CHECK(same_frame(hud::lay_out(viewport, state), two));
    OA_CHECK(hud::control_label(Control::force, 0, state) == "FORCE");
    // With touch controls the tablet's QUEUE keeps its label: x5 has a control of its own.
    hud::HudState touch = state;
    touch.pad.hud = false;
    OA_CHECK(hud::control_label(Control::queue, 0, touch) == "QUEUE");

    // SELECT ▾, opened by the D-pad, centred on the battlefield under the chips.
    state.sheet = hud::Sheet::select_menu;
    const hud::Frame menu = hud::lay_out(viewport, state);
    OA_CHECK(count_of(menu, Control::menu_item) == static_cast<int>(hud::select_item_count));
    OA_CHECK(inside(menu.sheet, field));
    OA_CHECK(menu.sheet.y > two_queue.y + two_queue.height);
    OA_CHECK(std::abs(middle_of(menu.sheet).x - middle_of(field).x) <= 2);
    OA_CHECK(no_overlaps(menu));
    OA_CHECK(all_inside_safe(menu, viewport));

    // Left-handed, the group chips start from the battlefield's right; the pill stays centred.
    hud::Viewport left = viewport;
    left.left_handed = true;
    state.sheet = hud::Sheet::none;
    const hud::Frame mirrored = hud::lay_out(left, state);
    const Rect last = find(mirrored, Control::group_chip, 1);
    OA_CHECK(last.x + last.width == field.x + field.width - 18);
    OA_CHECK(std::abs(middle_of(mirrored.status).x - middle_of(two.status).x) <= 2);
    OA_CHECK(mirrored.status.y == two.status.y && mirrored.status.width == two.status.width);
    OA_CHECK(no_overlaps(mirrored));
    OA_CHECK(all_inside_safe(mirrored, left));

    // Every window keeps the rules with the pad HUD, in both hands.
    for (hud::Viewport other :
         {tablet_viewport(),
          tablet_viewport_of(640, 480, {0, 0, 0, 0}),
          tablet_viewport_of(820, 1180, {0, 24, 0, 20}),
          tablet_viewport_of(2732, 2048, {0, 0, 0, 40}, 2.0f),
          phone_852(),
          deck_viewport(1.0f)}) {
        for (const bool left_handed : {false, true}) {
            other.left_handed = left_handed;
            hud::HudState slim = match_state();
            slim.pad.hud = true;
            slim.pad.force_shown = true;
            for (std::size_t group = 1; group <= 9; ++group)
                slim.group_counts[group] = 7;
            for (const hud::Sheet sheet : {hud::Sheet::none, hud::Sheet::select_menu}) {
                slim.sheet = sheet;
                slim.pad.group_ring.reset();
                const hud::Frame laid = hud::lay_out(other, slim);
                OA_CHECK(no_overlaps(laid));
                OA_CHECK(all_inside_safe(laid, other));
                OA_CHECK(inside(laid.clear, safe_rect(other)));
                OA_CHECK(count_of(laid, Control::queue) == 1);
                if (sheet == hud::Sheet::none)
                    OA_CHECK(overlaps_no_control(laid, laid.clear));
                // The group ring, while the groups layer is held (never with SELECT ▾ open),
                // covers nothing of the HUD.
                slim.pad.group_ring = hud::make_group_ring(other);
                const hud::Frame ringed = hud::lay_out(other, slim);
                OA_CHECK(no_overlaps(ringed));
                OA_CHECK(all_inside_safe(ringed, other));
                OA_CHECK(count_of(ringed, Control::group_wedge) == 9);
                if (sheet == hud::Sheet::none)
                    OA_CHECK(ringed.control_count == laid.control_count + 9);
            }
        }
    }
}

/// Checks that FORCE joins the tablet's thumb column above QUEUE (and above x5) while the pad
/// has grips, the rest of the column staying where it was.
void tablet_force_joins_the_thumb_column() {
    const hud::Viewport viewport = tablet_viewport();
    hud::HudState state = match_state();
    const hud::Frame without = hud::lay_out(viewport, state);
    OA_CHECK(count_of(without, Control::force) == 0);
    state.pad.force_shown = true;
    const hud::Frame with = hud::lay_out(viewport, state);
    OA_CHECK(same(find(with, Control::force), {231, 529, 76, 44}));
    for (const Control control :
         {Control::queue, Control::add, Control::clear, Control::select_menu, Control::pause})
        OA_CHECK(same(find(with, control), find(without, control)));
    OA_CHECK(same(with.clear, {219, 55, 885, 466}));
    OA_CHECK(no_overlaps(with));
    OA_CHECK(overlaps_no_control(with, with.clear));
    state.build_page_loaded = true;
    const hud::Frame page = hud::lay_out(viewport, state);
    OA_CHECK(same(find(page, Control::times_five), {231, 529, 76, 44}));
    OA_CHECK(same(find(page, Control::force), {231, 479, 76, 44}));
    OA_CHECK(no_overlaps(page));
    // A finger holds FORCE: its help says so.
    OA_CHECK(!hud::control_help(Control::force, 0).empty());
    const auto found = hud::hit(with, middle_of(find(with, Control::force)), 22);
    OA_CHECK(found.has_value() && found->control == Control::force);
    // The phone has no room in its column for FORCE.
    hud::HudState phone = match_state();
    phone.pad.force_shown = true;
    OA_CHECK(count_of(hud::lay_out(phone_852(), phone), Control::force) == 0);
}

/// Checks that the pad's looks (badges, glyphs, maps, lit chips, aims, hint lines, hold
/// progress, the SELECT ▾ focus, a finger on FORCE) never move a control: frames are equal with
/// them on and off.
void pad_looks_never_move_controls() {
    for (const hud::Viewport& viewport : {tablet_viewport(), phone_852(), deck_viewport(1.5f)}) {
        for (const bool slim : {false, true}) {
            hud::HudState state = match_state();
            state.pad.hud = slim;
            state.pad.force_shown = true;
            state.build_page_loaded = true;
            state.group_counts[3] = 2;
            state.group_counts[6] = 9;
            hud::HudState other = state;
            other.pad.badges = true;
            other.pad.glyphs = pad::GlyphStyle::playstation;
            other.pad.map.left_handed = true;
            other.pad.map.fallback = true;
            other.pad.map.scheme = pad::Scheme::sticks;
            other.pad.force_active = true;
            other.pad.groups_layer = true;
            other.pad.over_build_button = true;
            other.pad.radial_aim = uint8_t{3};
            other.pad.build_aim = uint8_t{2};
            other.pad.aim_dot = {400, 300};
            other.pad.ring_by_pad = true;
            other.pad.hold_progress = 0.5f;
            other.pad.hold_point = {20, 30};
            other.sheet_focus = 4;
            other.force_touch = true;
            other.armed_or_placing = true;
            other.right_click_interface = true;
            OA_CHECK(same_frame(hud::lay_out(viewport, state), hud::lay_out(viewport, other)));
        }
    }
}

/// Checks the build ring: eight wedges clockwise from the top with NEXT at E and PREV at W,
/// 44 and 128 pt radii, 56 pt pictures, the hit test, the safe area and the scale.
void build_ring_lays_out_its_wedges() {
    const hud::Viewport viewport = tablet_viewport();
    const hud::BuildRingContent content = builder_ring_content();
    const hud::BuildRing ring = hud::make_build_ring({700, 400}, content, viewport);
    OA_CHECK(ring.anchor.x == 700 && ring.anchor.y == 400);
    OA_CHECK(ring.centre.x == 700 && ring.centre.y == 400);
    OA_CHECK(ring.inner_radius == 44 && ring.outer_radius == 128);
    OA_CHECK(!ring.standing_orders);
    for (std::size_t slot = 0; slot < hud::build_ring_slot_count; ++slot) {
        const hud::BuildWedge& wedge = ring.wedges[slot];
        OA_CHECK(wedge.kind == content.kinds[slot]);
        OA_CHECK(wedge.gadget == content.gadgets[slot]);
        OA_CHECK(wedge.queued == content.queued[slot]);
        OA_CHECK(wedge.available == content.available[slot]);
        OA_CHECK(wedge.picture.width == 56 && wedge.picture.height == 56);
        OA_CHECK(same(wedge.hit, wedge.picture));
        const auto slot_hit = hud::build_ring_hit(ring, middle_of(wedge.picture), viewport);
        OA_CHECK(slot_hit.has_value() && *slot_hit == slot);
    }
    // Clockwise from the top: N above, NEXT at E, S below, PREV at W.
    const auto at = [&](std::size_t slot) { return middle_of(ring.wedges[slot].picture); };
    OA_CHECK(at(0).x == 700 && at(0).y == 400 - 86);
    OA_CHECK(at(hud::build_ring_next_slot).x == 700 + 86 && at(hud::build_ring_next_slot).y == 400);
    OA_CHECK(ring.wedges[hud::build_ring_next_slot].kind == hud::BuildWedgeKind::next);
    OA_CHECK(at(4).x == 700 && at(4).y == 400 + 86);
    OA_CHECK(at(hud::build_ring_prev_slot).x == 700 - 86 && at(hud::build_ring_prev_slot).y == 400);
    OA_CHECK(ring.wedges[hud::build_ring_prev_slot].kind == hud::BuildWedgeKind::prev);
    OA_CHECK(at(1).x > 700 && at(1).y < 400 && at(7).x < 700 && at(7).y < 400);
    // The hub and far off give nothing; just past the ring's edge still picks.
    OA_CHECK(!hud::build_ring_hit(ring, ring.centre, viewport).has_value());
    const auto edge = hud::build_ring_hit(ring, {700, 400 - 128 - 15}, viewport);
    OA_CHECK(edge.has_value() && *edge == 0);
    OA_CHECK(!hud::build_ring_hit(ring, {700, 400 - 128 - 30}, viewport).has_value());
    // An empty wedge gives nothing.
    hud::BuildRingContent gap = content;
    gap.kinds[7] = hud::BuildWedgeKind::empty;
    const hud::BuildRing gapped = hud::make_build_ring({700, 400}, gap, viewport);
    OA_CHECK(!gapped.wedges[7].available);
    OA_CHECK(!hud::build_ring_hit(gapped, middle_of(gapped.wedges[7].picture), viewport));
    // A corner anchor keeps the ring inside the battlefield and the anchor where it opened.
    const Rect field = battlefield_of(viewport);
    for (const hud::Point corner :
         {hud::Point{0, 0}, hud::Point{1179, 819}, hud::Point{230, 760}}) {
        const hud::BuildRing clamped = hud::make_build_ring(corner, content, viewport);
        OA_CHECK(clamped.anchor.x == corner.x && clamped.anchor.y == corner.y);
        const Rect box{
            clamped.centre.x - clamped.outer_radius,
            clamped.centre.y - clamped.outer_radius,
            2 * clamped.outer_radius,
            2 * clamped.outer_radius
        };
        OA_CHECK(inside(box, field));
        for (const auto& wedge : clamped.wedges)
            OA_CHECK(inside(wedge.hit, field));
    }
    // Twice the pixels a point, twice the sizes.
    const hud::Viewport dense = tablet_viewport_of(2360, 1640, {0, 0, 0, 40}, 2.0f);
    const hud::BuildRing large = hud::make_build_ring({1400, 800}, content, dense);
    OA_CHECK(large.inner_radius == 88 && large.outer_radius == 256);
    OA_CHECK(large.wedges[0].picture.width == 112);
    OA_CHECK(middle_of(large.wedges[0].picture).y == large.centre.y - 172);
    // The standing-orders ring keeps its kinds and says what it is.
    hud::BuildRingContent standing{};
    standing.standing_orders = true;
    standing.kinds[0] = hud::BuildWedgeKind::fire_orders;
    standing.kinds[1] = hud::BuildWedgeKind::move_orders;
    standing.kinds[3] = hud::BuildWedgeKind::on_off;
    standing.kinds[4] = hud::BuildWedgeKind::self_destruct;
    standing.kinds[5] = hud::BuildWedgeKind::cloak;
    standing.kinds[7] = hud::BuildWedgeKind::info;
    standing.available.fill(true);
    standing.gadgets.fill(-1);
    const hud::BuildRing orders = hud::make_build_ring({700, 400}, standing, viewport);
    OA_CHECK(orders.standing_orders);
    OA_CHECK(orders.wedges[4].kind == hud::BuildWedgeKind::self_destruct);
    OA_CHECK(!orders.wedges[2].available);
    // In the frame: a control for each wedge that holds something, over the controls under it.
    hud::HudState state = match_state();
    state.build_ring = ring;
    const hud::Frame frame = hud::lay_out(viewport, state);
    OA_CHECK(count_of(frame, Control::build_wedge) == 8);
    const auto found = hud::hit(frame, at(5), 22);
    OA_CHECK(found.has_value() && found->control == Control::build_wedge && found->index == 5);
    OA_CHECK(no_overlaps(frame));
    OA_CHECK(hud::control_label(Control::build_wedge, hud::build_ring_next_slot, state) == "NEXT");
    OA_CHECK(hud::control_label(Control::build_wedge, hud::build_ring_prev_slot, state) == "PREV");
    OA_CHECK(
        hud::control_label_lookup(Control::build_wedge, hud::build_ring_next_slot, state) ==
        "NEXT PAGE"
    );
    OA_CHECK(
        hud::control_label_lookup(Control::build_wedge, hud::build_ring_prev_slot, state) == "PREV"
    );
    OA_CHECK(hud::control_label(Control::build_wedge, 0, state).empty());
    state.build_ring = orders;
    OA_CHECK(count_of(hud::lay_out(viewport, state), Control::build_wedge) == 6);
    OA_CHECK(hud::control_label(Control::build_wedge, 7, state) == "INFO");
    state.build_ring = gapped;
    OA_CHECK(count_of(hud::lay_out(viewport, state), Control::build_wedge) == 7);
}

/// Checks the group ring: at the battlefield's lower left, clear of the touch controls and the
/// pad HUD's chips, nine wedges with group 1 at the top, mirrored for the left hand.
void group_ring_sits_at_the_lower_left() {
    const hud::Viewport viewport = deck_viewport(1.5f);
    const Rect field = battlefield_of(viewport);
    const hud::GroupRing ring = hud::make_group_ring(viewport);
    OA_CHECK(ring.inner_radius == 42 && ring.outer_radius == 126);
    OA_CHECK(!ring.aim.has_value());
    const Rect box{
        ring.centre.x - ring.outer_radius,
        ring.centre.y - ring.outer_radius,
        2 * ring.outer_radius,
        2 * ring.outer_radius
    };
    OA_CHECK(inside(box, field));
    OA_CHECK(ring.centre.x < middle_of(field).x && ring.centre.y > middle_of(field).y);
    // Clear of the pad HUD's group chips: none is left out under it.
    hud::HudState slim = match_state();
    slim.pad.hud = true;
    slim.pad.force_shown = true;
    for (std::size_t group = 1; group <= 9; ++group)
        slim.group_counts[group] = 2;
    const int chips = count_of(hud::lay_out(viewport, slim), Control::group_chip);
    slim.pad.group_ring = ring;
    const hud::Frame frame = hud::lay_out(viewport, slim);
    OA_CHECK(count_of(frame, Control::group_wedge) == 9);
    OA_CHECK(count_of(frame, Control::group_chip) == chips);
    OA_CHECK(no_overlaps(frame));
    const Rect one = find(frame, Control::group_wedge, 1);
    OA_CHECK(middle_of(one).x == ring.centre.x && middle_of(one).y < ring.centre.y);
    const auto found = hud::hit(frame, middle_of(one), 22);
    OA_CHECK(found.has_value() && found->control == Control::group_wedge && found->index == 1);
    OA_CHECK(hud::control_label(Control::group_wedge, 7, slim) == "7");
    // Clear of the touch layout's thumb column and group bar.
    hud::HudState touch = slim;
    touch.pad.hud = false;
    touch.build_page_loaded = true;
    touch.pad.group_ring.reset();
    const hud::Frame plain = hud::lay_out(viewport, touch);
    touch.pad.group_ring = ring;
    const hud::Frame ringed = hud::lay_out(viewport, touch);
    OA_CHECK(ringed.control_count == plain.control_count + 9);
    OA_CHECK(no_overlaps(ringed));
    // Left-handed: the lower right, mirrored about the battlefield.
    hud::Viewport left = viewport;
    left.left_handed = true;
    const hud::GroupRing mirrored = hud::make_group_ring(left);
    OA_CHECK(mirrored.centre.y == ring.centre.y);
    OA_CHECK(
        std::abs((ring.centre.x - field.x) - (field.x + field.width - mirrored.centre.x)) <= 1
    );
    // A phone keeps it beside its left column, inside the safe area.
    const hud::Viewport phone = phone_852();
    const hud::GroupRing small = hud::make_group_ring(phone);
    const Rect phone_box{
        small.centre.x - small.outer_radius,
        small.centre.y - small.outer_radius,
        2 * small.outer_radius,
        2 * small.outer_radius
    };
    OA_CHECK(inside(phone_box, safe_rect(phone)));
}

/// Checks the pad's status line and ring hints: what R2 and L2 do in both interface types, in
/// each glyph set, mirrored for the left hand, and the rings' release, A and B.
void pad_hints_name_the_buttons() {
    using hud::TapAction;
    const pad::MapContext map = deck_map();
    const auto words = [](const hud::PadHint& hint,
                          pad::GlyphStyle style = pad::GlyphStyle::steam_deck) {
        return hud::hint_words(hint, style);
    };
    const hud::PadHint armed =
        hud::pad_status_hint(TapAction::move, TapAction::attack, true, false, map);
    OA_CHECK(words(armed) == "R2 MOVE · ENEMY: ATTACK · L2 CANCEL");
    OA_CHECK(armed.count == 5);
    OA_CHECK(armed.parts[0].chord.has_value() && armed.parts[0].text == "MOVE");
    OA_CHECK(armed.parts[0].chord && armed.parts[0].chord->button == pad::PadButton::r2);
    OA_CHECK(!armed.parts[1].chord.has_value() && armed.parts[1].text == "·");
    OA_CHECK(!armed.parts[2].chord.has_value() && armed.parts[2].text == "ENEMY: ATTACK");
    OA_CHECK(armed.parts[4].chord && armed.parts[4].chord->button == pad::PadButton::l2);
    OA_CHECK(armed.parts[4].text == "CANCEL");
    OA_CHECK(
        words(hud::pad_status_hint(TapAction::move, TapAction::attack, false, false, map)) ==
        "R2 MOVE · ENEMY: ATTACK · L2 CLEAR"
    );
    OA_CHECK(
        words(hud::pad_status_hint(TapAction::attack, TapAction::attack, true, false, map)) ==
        "R2 ATTACK · L2 CANCEL"
    );
    OA_CHECK(
        words(hud::pad_status_hint(TapAction::build, TapAction::none, true, false, map)) ==
        "R2 BUILD · L2 CANCEL"
    );
    // The right-click interface: R2 selects, L2 gives the order.
    OA_CHECK(
        words(hud::pad_status_hint(TapAction::move, TapAction::attack, false, true, map)) ==
        "R2 SELECT · L2 MOVE · ENEMY: ATTACK"
    );
    OA_CHECK(
        words(hud::pad_status_hint(TapAction::move, TapAction::none, false, true, map)) ==
        "R2 SELECT · L2 MOVE"
    );
    OA_CHECK(
        words(hud::pad_status_hint(TapAction::select, TapAction::none, false, true, map)) ==
        "R2 SELECT"
    );
    OA_CHECK(
        words(hud::pad_status_hint(TapAction::patrol, TapAction::patrol, true, true, map)) ==
        "R2 PATROL · L2 CANCEL"
    );
    // Each glyph set names the same places.
    OA_CHECK(words(armed, pad::GlyphStyle::xbox) == "RT MOVE · ENEMY: ATTACK · LT CANCEL");
    OA_CHECK(words(armed, pad::GlyphStyle::nintendo) == "ZR MOVE · ENEMY: ATTACK · ZL CANCEL");
    OA_CHECK(words(armed, pad::GlyphStyle::playstation) == "R2 MOVE · ENEMY: ATTACK · L2 CANCEL");
    // Left-handed, the triggers trade places.
    pad::MapContext left = map;
    left.left_handed = true;
    OA_CHECK(
        words(hud::pad_status_hint(TapAction::move, TapAction::attack, true, false, left)) ==
        "L2 MOVE · ENEMY: ATTACK · R2 CANCEL"
    );
    // The rings.
    OA_CHECK(words(hud::ring_hint(false, map)) == "RELEASE R1 GIVE · A ARM · B CLOSE");
    OA_CHECK(words(hud::ring_hint(true, map)) == "RELEASE L1 GIVE · A ARM · B CLOSE");
    OA_CHECK(words(hud::ring_hint(false, left)) == "RELEASE L1 GIVE · A ARM · B CLOSE");
    // The fallback holds a bumper for a ring; the hint names the bumper alone.
    OA_CHECK(
        words(hud::ring_hint(false, fallback_map()), pad::GlyphStyle::xbox) ==
        "RELEASE RB GIVE · A ARM · B CLOSE"
    );
    const hud::PadHint ring = hud::ring_hint(false, fallback_map());
    for (std::size_t part = 0; part < ring.count; ++part) {
        const auto& chord = ring.parts[part].chord;
        OA_CHECK(!chord || (!chord->tap && !chord->hold));
        // ARM, which arms the aimed wedge, is looked up by what it means; the other words by
        // themselves.
        OA_CHECK(ring.parts[part].lookup == (ring.parts[part].text == "ARM" ? "ARM ORDER" : ""));
    }
    // The enemy's piece is looked up whole, its action filled in; the others by themselves.
    const hud::PadHint status =
        hud::pad_status_hint(TapAction::move, TapAction::attack, true, false, map);
    for (std::size_t part = 0; part < status.count; ++part) {
        const bool enemy = status.parts[part].text == "ENEMY: ATTACK";
        OA_CHECK(status.parts[part].lookup == (enemy ? "ENEMY: {action}" : ""));
        OA_CHECK(status.parts[part].action == (enemy ? "ATTACK" : ""));
    }
}

/// Checks that a label whose word stands for two things is drawn from the text that says
/// which, and in English where the language has no translation of that text.
void labels_with_two_meanings_are_told_apart() {
    using Words = std::vector<std::pair<std::string_view, std::string_view>>;
    const auto translator = [](Words words) {
        return [words](std::string_view text) {
            for (const auto& [english, shown] : words)
                if (english == text)
                    return std::string(shown);
            return std::string(text);
        };
    };
    hud::HudState state = match_state();
    state.build_ring = hud::make_build_ring({700, 400}, builder_ring_content(), tablet_viewport());
    const auto shown = [&](Control control, uint8_t index, const auto& translate) {
        return hud::shown_label(
            hud::control_label(control, index, state),
            hud::control_label_lookup(control, index, state),
            translate
        );
    };
    const auto arm_shown = [&](const auto& translate) {
        const hud::PadHint ring = hud::ring_hint(false, deck_map());
        for (std::size_t part = 0; part < ring.count; ++part)
            if (ring.parts[part].text == "ARM")
                return hud::shown_label(ring.parts[part].text, ring.parts[part].lookup, translate);
        return std::string{};
    };
    const uint8_t next_wedge = static_cast<uint8_t>(hud::build_ring_next_slot);
    // A pack that tells the meanings apart.
    const std::function<std::string(std::string_view)> told_apart = translator(
        {{"NEXT UNIT", "下一单位"},
         {"NEXT PAGE", "下一页"},
         {"NEXT", "下一页"},
         {"ARM", "CLAN"},
         {"ARM ORDER", "启用"},
         {"QUEUE", "排队"}}
    );
    OA_CHECK(shown(Control::next_unit, 0, told_apart) == "下一单位");
    OA_CHECK(shown(Control::build_wedge, next_wedge, told_apart) == "下一页");
    OA_CHECK(shown(Control::drawer_next, 0, told_apart) == "下一页");
    OA_CHECK(shown(Control::queue, 0, told_apart) == "排队");
    OA_CHECK(arm_shown(told_apart) == "启用");
    // A table that holds the bare words alone, as the game's own does (ARM is the side's name):
    // the words show in English.
    const std::function<std::string(std::string_view)> bare_words =
        translator({{"NEXT", "下一页"}, {"ARM", "CLAN"}});
    OA_CHECK(shown(Control::next_unit, 0, bare_words) == "NEXT");
    OA_CHECK(shown(Control::build_wedge, next_wedge, bare_words) == "NEXT");
    OA_CHECK(arm_shown(bare_words) == "ARM");
    // No translation at all: English.
    const std::function<std::string(std::string_view)> english = translator({});
    OA_CHECK(shown(Control::next_unit, 0, english) == "NEXT");
    OA_CHECK(shown(Control::drawer_next, 0, english) == "NEXT");
    OA_CHECK(shown(Control::queue, 0, english) == "QUEUE");
    OA_CHECK(arm_shown(english) == "ARM");
}

/// A language's translations: each English text with the words it shows.
using Words = std::vector<std::pair<std::string_view, std::string_view>>;

/// Returns a translation that knows the texts given and leaves every other one unchanged.
///
/// @param words the texts it translates
/// @return the translation
std::function<std::string(std::string_view)> translator(Words words) {
    return [words = std::move(words)](std::string_view text) {
        for (const auto& [english, shown] : words)
            if (english == text)
                return std::string(shown);
        return std::string(text);
    };
}

/// Checks that the status hint and the pad's enemy piece are translated piece by piece, the
/// action's word filled into its phrase's translation, and in English where the language has
/// no translation of the phrase.
void tap_hints_are_translated_whole() {
    using hud::TapAction;
    const auto enemy_shown = [](const hud::PadHint& hint, const auto& translate) {
        for (std::size_t part = 0; part < hint.count; ++part)
            if (!hint.parts[part].action.empty())
                return hud::shown_label(
                    hint.parts[part].text,
                    hint.parts[part].lookup,
                    translate,
                    hint.parts[part].action
                );
        return std::string{};
    };
    const hud::PadHint pad_hint =
        hud::pad_status_hint(TapAction::move, TapAction::attack, false, false, deck_map());
    // A pack that translates the phrases and the words.
    const auto chinese = translator(
        {{"TAP: {action}", "点按：{action}"},
         {"ENEMY: {action}", "敌方：{action}"},
         {"MOVE", "移动"},
         {"ATTACK", "攻击"}}
    );
    OA_CHECK(
        hud::status_hint(TapAction::move, TapAction::attack, chinese) == "点按：移动 · 敌方：攻击"
    );
    OA_CHECK(enemy_shown(pad_hint, chinese) == "敌方：攻击");
    // A table with the words alone: the phrases show in English, never around a translated word.
    const auto words_alone = translator({{"MOVE", "BEWEGEN"}, {"ATTACK", "ANGRIFF"}});
    OA_CHECK(
        hud::status_hint(TapAction::move, TapAction::attack, words_alone) ==
        "TAP: MOVE · ENEMY: ATTACK"
    );
    OA_CHECK(enemy_shown(pad_hint, words_alone) == "ENEMY: ATTACK");
    // The word filled in is never read as a field.
    const auto braces = translator({{"TAP: {action}", "{action}!"}, {"MOVE", "{action}"}});
    OA_CHECK(hud::status_hint(TapAction::move, TapAction::none, braces) == "{action}!");
    // While placing, the line is translated as one text.
    const auto placing = translator(
        {{"DRAG TO MOVE · DOUBLE-TAP OR HOLD TO PLACE", "拖动以移动 · 双击或长按以放置"}}
    );
    OA_CHECK(
        hud::status_hint(TapAction::place, TapAction::attack, placing) ==
        "拖动以移动 · 双击或长按以放置"
    );
}

/// Checks the banner's titles: the building being placed named as the language shown names it,
/// in its phrase's translation or the phrase in English, and the armed order's word in its
/// phrase's translation, or the whole title in English where the language has no translation of
/// the phrase.
void banner_titles_are_translated_piece_by_piece() {
    hud::HudState placing;
    placing.banner.shown = true;
    placing.placement.active = true;
    hud::HudState armed;
    armed.banner.shown = true;
    armed.banner.order = hud::Order::patrol;
    // English, as the titles always read.
    const auto english = translator({});
    placing.placement.name = "Solar Collector";
    OA_CHECK(hud::banner_title(placing, english) == "Place Solar Collector");
    OA_CHECK(hud::banner_title(armed, english) == "PATROL armed");
    armed.banner.order = hud::Order::blast;
    OA_CHECK(hud::banner_title(armed, english) == "D-GUN armed");
    // A pack that translates the phrases and the words; the name comes in the language shown.
    const auto chinese = translator(
        {{"Place {name}", "放置{name}"},
         {"{order} armed", "{order}已启用"},
         {"PATROL", "巡逻"},
         {"D-GUN", "D枪"}}
    );
    placing.placement.name = "太阳能采集器";
    OA_CHECK(hud::banner_title(placing, chinese) == "放置太阳能采集器");
    OA_CHECK(hud::banner_title(armed, chinese) == "D枪已启用");
    armed.banner.order = hud::Order::patrol;
    OA_CHECK(hud::banner_title(armed, chinese) == "巡逻已启用");
    // A table with the words alone: the phrases show in English, the order's word with its
    // phrase, and the name as the game data gives it in the language.
    const auto german = translator({{"PATROL", "PATROUILLE"}, {"Kbot Lab", "Kbot-Labor"}});
    placing.placement.name = "Kbot-Labor";
    OA_CHECK(hud::banner_title(placing, german) == "Place Kbot-Labor");
    OA_CHECK(hud::banner_title(armed, german) == "PATROL armed");
    // The name is never translated again, and the words filled in are never read as a field.
    placing.placement.name = "Kbot Lab";
    OA_CHECK(hud::banner_title(placing, german) == "Place Kbot Lab");
    const auto braces = translator(
        {{"Place {name}", "{name}!"}, {"{order} armed", "{order}?"}, {"PATROL", "{order}"}}
    );
    placing.placement.name = "{name}";
    OA_CHECK(hud::banner_title(placing, braces) == "{name}!");
    OA_CHECK(hud::banner_title(armed, braces) == "{order}?");
    // A banner that does not show has no title; nor has one with neither a building nor an
    // order.
    placing.banner.shown = false;
    OA_CHECK(hud::banner_title(placing, chinese).empty());
    armed.banner.order.reset();
    OA_CHECK(hud::banner_title(armed, chinese).empty());
}

/// Checks the badges the touch controls show once a pad was used, through each map, and the
/// help lines and button names that go with them.
void badges_name_the_pad_buttons() {
    using pad::PadButton;
    const pad::MapContext map = deck_map();
    const auto badge = [](Control control, const pad::MapContext& context, uint8_t index = 0) {
        return hud::control_badge(control, index, context);
    };
    const auto is = [&](Control control,
                        const pad::MapContext& context,
                        PadButton button,
                        PadButton held = PadButton::none,
                        uint8_t index = 0) {
        const auto chord = badge(control, context, index);
        if (!chord || chord->button != button || chord->held != held) {
            std::fprintf(
                stderr,
                "badge of control %d.%d: %s\n",
                static_cast<int>(control),
                index,
                chord ? hud::chord_words(*chord, pad::GlyphStyle::steam_deck).c_str() : "none"
            );
            return false;
        }
        return true;
    };
    OA_CHECK(is(Control::queue, map, PadButton::r4));
    OA_CHECK(is(Control::add, map, PadButton::l4));
    OA_CHECK(is(Control::clear, map, PadButton::b));
    OA_CHECK(is(Control::select_menu, map, PadButton::dpad_left));
    OA_CHECK(is(Control::pause, map, PadButton::x, PadButton::view));
    OA_CHECK(is(Control::chat, map, PadButton::a, PadButton::view));
    OA_CHECK(is(Control::centre, map, PadButton::l3));
    OA_CHECK(is(Control::follow, map, PadButton::r3));
    OA_CHECK(is(Control::next_unit, map, PadButton::dpad_right));
    OA_CHECK(is(Control::info, map, PadButton::view));
    OA_CHECK(is(Control::force, map, PadButton::r5));
    OA_CHECK(is(Control::group_chip, map, PadButton::dpad_up, PadButton::l5, 1));
    OA_CHECK(is(Control::group_chip, map, PadButton::y, PadButton::l5, 5));
    OA_CHECK(is(Control::group_chip, map, PadButton::x, PadButton::l5, 8));
    OA_CHECK(!badge(Control::group_chip, map, 9).has_value());
    for (const Control control :
         {Control::menu,
          Control::speed,
          Control::group_store,
          Control::times_five,
          Control::order_slot,
          Control::radial_hub,
          Control::none})
        OA_CHECK(!badge(control, map).has_value());
    // Left-handed, the grips, bumpers and stick clicks trade sides; B stays.
    pad::MapContext left = map;
    left.left_handed = true;
    OA_CHECK(is(Control::queue, left, PadButton::l4));
    OA_CHECK(is(Control::add, left, PadButton::r4));
    OA_CHECK(is(Control::force, left, PadButton::l5));
    OA_CHECK(is(Control::centre, left, PadButton::r3));
    OA_CHECK(is(Control::clear, left, PadButton::b));
    // With no grips QUEUE and ADD are taps of the bumpers, and there is no FORCE.
    const pad::MapContext fallback = fallback_map();
    const auto queue = badge(Control::queue, fallback);
    OA_CHECK(queue && queue->button == PadButton::r1 && queue->tap);
    const auto add = badge(Control::add, fallback);
    OA_CHECK(add && add->button == PadButton::l1 && add->tap);
    OA_CHECK(!badge(Control::force, fallback).has_value());
    // Help lines name the pad input too.
    const std::string queue_help =
        hud::control_help_with_pad(Control::queue, 0, map, pad::GlyphStyle::steam_deck);
    OA_CHECK(queue_help.find(std::string(hud::control_help(Control::queue, 0))) == 0);
    OA_CHECK(queue_help.ends_with(" On the pad: hold or tap R4."));
    OA_CHECK(
        hud::control_help_with_pad(Control::queue, 0, fallback, pad::GlyphStyle::xbox)
            .ends_with(" On the pad: tap RB.")
    );
    OA_CHECK(
        hud::control_help_with_pad(Control::pause, 0, map, pad::GlyphStyle::steam_deck)
            .ends_with(" On the pad: View + X.")
    );
    OA_CHECK(
        hud::control_help_with_pad(Control::force, 0, map, pad::GlyphStyle::steam_deck)
            .ends_with(" On the pad: hold R5.")
    );
    OA_CHECK(
        hud::control_help_with_pad(Control::group_chip, 2, map, pad::GlyphStyle::playstation)
            .ends_with(" On the pad: L5 + D-pad right.")
    );
    OA_CHECK(
        hud::control_help_with_pad(Control::menu, 0, map, pad::GlyphStyle::steam_deck) ==
        hud::control_help(Control::menu, 0)
    );
    // Button names by glyph set, the face buttons by place.
    OA_CHECK(hud::button_name(PadButton::a, pad::GlyphStyle::steam_deck) == "A");
    OA_CHECK(hud::button_name(PadButton::a, pad::GlyphStyle::playstation) == "Cross");
    OA_CHECK(hud::button_name(PadButton::a, pad::GlyphStyle::nintendo) == "B");
    OA_CHECK(hud::button_name(PadButton::b, pad::GlyphStyle::nintendo) == "A");
    OA_CHECK(hud::button_name(PadButton::y, pad::GlyphStyle::playstation) == "Triangle");
    OA_CHECK(hud::button_name(PadButton::l1, pad::GlyphStyle::xbox) == "LB");
    OA_CHECK(hud::button_name(PadButton::r2, pad::GlyphStyle::nintendo) == "ZR");
    OA_CHECK(hud::button_name(PadButton::view, pad::GlyphStyle::nintendo) == "Minus");
    OA_CHECK(hud::button_name(PadButton::menu, pad::GlyphStyle::playstation) == "Options");
    OA_CHECK(hud::button_name(PadButton::dpad_left, pad::GlyphStyle::xbox) == "D-pad left");
    OA_CHECK(hud::button_name(PadButton::r4, pad::GlyphStyle::steam_deck) == "R4");
    OA_CHECK(hud::button_name(PadButton::r4, pad::GlyphStyle::xbox) == "P1");
    OA_CHECK(hud::button_name(PadButton::none, pad::GlyphStyle::steam_deck).empty());
    for (std::size_t value = 1; value < pad::pad_button_count; ++value)
        for (const auto style :
             {pad::GlyphStyle::steam_deck,
              pad::GlyphStyle::xbox,
              pad::GlyphStyle::playstation,
              pad::GlyphStyle::nintendo})
            OA_CHECK(!hud::button_name(static_cast<PadButton>(value), style).empty());
    pad::Chord chord{};
    chord.held = PadButton::view;
    chord.button = PadButton::x;
    OA_CHECK(hud::chord_words(chord, pad::GlyphStyle::steam_deck) == "View + X");
    OA_CHECK(hud::chord_words(chord, pad::GlyphStyle::playstation) == "Create + Square");
    chord = {};
    chord.button = PadButton::r1;
    chord.tap = true;
    OA_CHECK(hud::chord_words(chord, pad::GlyphStyle::steam_deck) == "tap R1");
    chord.tap = false;
    chord.hold = true;
    OA_CHECK(hud::chord_words(chord, pad::GlyphStyle::xbox) == "hold RB");
}
} // namespace

int main() {
    device_class_follows_the_short_side();
    each_class_has_its_latch();
    latches_tap_to_latch_and_hold_for_one();
    orders_have_their_panel_names();
    tablet_layout_is_pinned();
    tablet_shows_what_the_state_asks();
    tablet_clear_area_avoids_the_controls();
    tablet_placement_and_sheets();
    phone_layout_is_pinned();
    phone_rail_fits_the_height();
    phone_clear_area_avoids_the_controls();
    phone_drawer_lays_out_its_cells();
    phone_more_sheet_lays_out_its_cells();
    phone_menus_open_beside_their_buttons();
    phone_menu_fits_when_controls_are_large();
    phone_scales_with_pixels_per_point();
    looks_never_move_rectangles();
    left_handed_mirrors_the_controls();
    hits_find_the_nearest_control();
    nearest_rect_and_covers();
    radial_keeps_its_slots();
    every_window_keeps_the_rules();
    every_control_has_its_texts();
    order_panel_gadgets_have_help();
    deck_screen_keeps_the_tablet_layout();
    pad_hud_lays_out_the_slim_frame();
    tablet_force_joins_the_thumb_column();
    pad_looks_never_move_controls();
    build_ring_lays_out_its_wedges();
    group_ring_sits_at_the_lower_left();
    pad_hints_name_the_buttons();
    labels_with_two_meanings_are_told_apart();
    tap_hints_are_translated_whole();
    banner_titles_are_translated_piece_by_piece();
    badges_name_the_pad_buttons();
    return oa::test::check_exit_status();
}

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The kit's pointer, finger reach, keys, two-dimensional focus, wheel,
// control names, the parts automation lists, and text editing.
//
// The wheel numbers were computed with the settings dialog's dialog_wheel at
// 5f7fdbd8, the start of this branch, by a throwaway program that was not
// committed. Step 24, limit 80. The notice's five fingers are what
// notice_finger_down lands on for its two buttons at height 150.

#include "oa/test/check.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/looks.hpp"

#include <cstddef>
#include <initializer_list>
#include <limits>
#include <span>
#include <stdint.h>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace kit = oa::ui::kit;

/// A control at a rectangle, enabled and focusable.
///
/// @param id the control's number
/// @param rect where it lies
/// @return the control
kit::Control placed(kit::ControlId id, kit::Rect rect) {
    kit::Control control;
    control.id = id;
    control.rect = rect;
    return control;
}

/// Tells whether a finger landed on a control at a point.
///
/// @param reached where the finger landed
/// @param id the control
/// @param x the point's column
/// @param y the point's row
/// @return true when the control and both coordinates match
bool landed(kit::Reached reached, kit::ControlId id, int32_t x, int32_t y) {
    return reached.control == id && reached.at.x == x && reached.at.y == y;
}

/// The nearer of two buttons, and a disabled button closer than either.
void a_finger_takes_the_nearer_of_two_buttons() {
    kit::DisplayList list;
    list.controls.push_back(placed(1, {0, 0, 20, 20}));
    list.controls.push_back(placed(2, {100, 0, 20, 20}));
    kit::Control disabled = placed(3, {24, 0, 10, 20});
    disabled.enabled = false;
    list.controls.push_back(disabled);

    // Six points from the first button's last column, seventy-five from the second.
    OA_CHECK(landed(kit::reach(list, {25, 10}, 22), 1, 19, 10));
    OA_CHECK(landed(kit::reach(list, {90, 10}, 22), 2, 100, 10));
    // The disabled button's nearest point is closer than either, and is not taken.
    OA_CHECK(landed(kit::reach(list, {30, 10}, 22), 1, 19, 10));
}

/// A disabled control the finger is on blocks the press by the blocking rule: no control,
/// and nothing within reach is searched. By the rule that passes, the press goes on to the
/// nearest enabled control, and off every control both rules search alike.
void a_disabled_control_can_block_a_finger() {
    kit::DisplayList list;
    kit::Control disabled = placed(1, {0, 0, 40, 20});
    disabled.enabled = false;
    list.controls.push_back(disabled);
    list.controls.push_back(placed(2, {0, 24, 40, 20}));
    kit::Control hidden = placed(3, {0, 50, 40, 20});
    hidden.clip = {0, 50, 40, 5};
    hidden.enabled = false;
    list.controls.push_back(hidden);

    // On the disabled control: fourteen rows above the enabled one.
    OA_CHECK(landed(kit::reach(list, {10, 10}, 22, kit::DisabledControl::passes), 2, 10, 24));
    OA_CHECK(landed(
        kit::reach(list, {10, 10}, 22, kit::DisabledControl::blocks), kit::no_control, 10, 10
    ));
    // On the enabled control, on nothing, and on the part of a disabled one its clip hides.
    OA_CHECK(landed(kit::reach(list, {10, 30}, 22, kit::DisabledControl::blocks), 2, 10, 30));
    OA_CHECK(landed(kit::reach(list, {60, 10}, 30, kit::DisabledControl::blocks), 2, 39, 24));
    OA_CHECK(landed(kit::reach(list, {60, 10}, 30, kit::DisabledControl::passes), 2, 39, 24));
    OA_CHECK(landed(kit::reach(list, {10, 60}, 22, kit::DisabledControl::blocks), 2, 10, 43));
    // On the part of the clipped one a press reaches.
    OA_CHECK(landed(
        kit::reach(list, {10, 52}, 22, kit::DisabledControl::blocks), kit::no_control, 10, 52
    ));
}

/// Equal distances take the first control. A reach one short of the distance takes none.
void equal_distances_take_the_first_button() {
    // A ends at column 20, B starts at 40. The finger at column 30 is ten from each.
    kit::DisplayList list;
    list.controls.push_back(placed(1, {0, 0, 21, 10}));
    list.controls.push_back(placed(2, {40, 0, 21, 10}));
    OA_CHECK(landed(kit::reach(list, {30, 5}, 10), 1, 20, 5));
    OA_CHECK(landed(kit::reach(list, {30, 5}, 9), kit::no_control, 30, 5));

    // The last pixel is ten down from (9, 19) and eleven down from (9, 20).
    kit::DisplayList one;
    one.controls.push_back(placed(1, {0, 0, 10, 10}));
    OA_CHECK(landed(kit::reach(one, {9, 19}, 10), 1, 9, 9));
    OA_CHECK(landed(kit::reach(one, {9, 19}, 9), kit::no_control, 9, 19));
    OA_CHECK(landed(kit::reach(one, {9, 20}, 10), kit::no_control, 9, 20));
    OA_CHECK(landed(kit::reach(one, {9, 20}, 11), 1, 9, 9));
}

/// A finger beyond every control, and a reach of nothing, stays where it is.
void a_finger_out_of_reach_takes_nothing() {
    kit::DisplayList list;
    list.controls.push_back(placed(1, {0, 0, 10, 10}));
    OA_CHECK(landed(kit::reach(list, {100, 100}, 22), kit::no_control, 100, 100));
    OA_CHECK(landed(kit::reach(list, {12, 5}, 0), kit::no_control, 12, 5));
    OA_CHECK(landed(kit::reach(list, {12, 5}, -4), kit::no_control, 12, 5));
    OA_CHECK(landed(kit::reach({}, {3, 4}, 22), kit::no_control, 3, 4));
}

/// A candidate whose nearest point lies on another control is passed over.
void a_covered_point_is_passed_over() {
    // The outer control is listed first, so a point it covers is its point.
    kit::DisplayList list;
    list.controls.push_back(placed(1, {0, 0, 80, 40}));
    list.controls.push_back(placed(2, {10, 10, 20, 20}));
    // The inner control's nearest point is (10, 29), which the outer covers.
    // The press lands on the outer's nearest point, (5, 39).
    OA_CHECK(kit::hit(list, {10, 29}) == 1);
    OA_CHECK(landed(kit::reach(list, {5, 50}, 30), 1, 5, 39));
}

/// A reach of a fraction of a point counts the fraction: the corner of the button is 11.40
/// from the finger, within 11.5 and not within 11.25.
void a_reach_may_be_a_fraction() {
    kit::DisplayList list;
    list.controls.push_back(placed(1, {0, 0, 10, 10}));
    OA_CHECK(landed(kit::reach(list, {12, 20}, 11.5F), 1, 9, 9));
    OA_CHECK(landed(kit::reach(list, {12, 20}, 11.25F), kit::no_control, 12, 20));
    OA_CHECK(landed(kit::reach(list, {12, 20}, 11), kit::no_control, 12, 20));
    OA_CHECK(landed(kit::reach(list, {12, 20}, 12), 1, 9, 9));
    OA_CHECK(landed(kit::reach(list, {5, 5}, 11.5F), 1, 5, 5));
    OA_CHECK(landed(kit::reach(list, {12, 5}, 0.0F), kit::no_control, 12, 5));
    OA_CHECK(landed(kit::reach(list, {12, 5}, -2.5F), kit::no_control, 12, 5));
    // Equal distances still take the first control, and a disabled one is passed over.
    kit::DisplayList two;
    two.controls.push_back(placed(1, {0, 0, 21, 10}));
    two.controls.push_back(placed(2, {40, 0, 21, 10}));
    OA_CHECK(landed(kit::reach(two, {30, 5}, 10.5F), 1, 20, 5));
    two.controls.front().enabled = false;
    OA_CHECK(landed(kit::reach(two, {30, 5}, 10.5F), 2, 40, 5));
}

/// The parts that draw the focused control show the focus once a key has shown it, the
/// parts of the held control show the press, and a part with no control shows neither.
void the_states_show_the_focus_and_the_press() {
    kit::DisplayList list;
    for (const kit::ControlId control : {1, 2, kit::no_control, 1}) {
        kit::Item part{};
        part.control = control;
        part.state.focused = true;
        part.state.pressed = true;
        list.items.push_back(part);
    }
    kit::Interaction interaction{};
    interaction.focused = 1;
    interaction.focus_shown = true;
    interaction.pressed = 2;
    kit::mark_states(list, interaction);
    OA_CHECK(list.items[0].state.focused && !list.items[0].state.pressed);
    OA_CHECK(!list.items[1].state.focused && list.items[1].state.pressed);
    OA_CHECK(!list.items[2].state.focused && !list.items[2].state.pressed);
    OA_CHECK(list.items[3].state.focused && !list.items[3].state.pressed);
    interaction.focus_shown = false;
    interaction.pressed = kit::no_control;
    kit::mark_states(list, interaction);
    for (const kit::Item& part : list.items)
        OA_CHECK(!part.state.focused && !part.state.pressed);
}

/// A finger already on a control keeps its own point.
void a_finger_on_a_control_lands_unshifted() {
    kit::DisplayList list;
    list.controls.push_back(placed(1, {0, 0, 40, 20}));
    list.controls.push_back(placed(2, {0, 40, 40, 20}));
    OA_CHECK(landed(kit::reach(list, {10, 10}, 22), 1, 10, 10));
    OA_CHECK(landed(kit::reach(list, {0, 0}, 22), 1, 0, 0));
    OA_CHECK(landed(kit::reach(list, {39, 19}, 22), 1, 39, 19));
}

/// A clip hides the part of a control a press cannot reach.
void a_clip_hides_the_part_a_press_cannot_reach() {
    kit::DisplayList list;
    kit::Control control = placed(1, {0, 0, 40, 40});
    control.clip = {0, 0, 40, 15};
    list.controls.push_back(control);
    OA_CHECK(landed(kit::reach(list, {10, 5}, 20), 1, 10, 5));
    OA_CHECK(landed(kit::reach(list, {10, 30}, 20), 1, 10, 14));
    OA_CHECK(landed(kit::reach(list, {10, 30}, 10), kit::no_control, 10, 30));
}

/// OK and the open button of a notice 150 points tall, reach 22.
///
/// OK is (336, 124, 52, 17) and the open button is (235, 124, 96, 17), which
/// is where notice_finger_down places them at that height. OK is tried first.
void the_notice_buttons_take_the_notices_finger() {
    kit::DisplayList notice;
    notice.controls.push_back(placed(0, {336, 124, 52, 17}));
    notice.controls.push_back(placed(1, {235, 124, 96, 17}));
    constexpr int32_t reach = 22;

    // On OK.
    OA_CHECK(landed(kit::reach(notice, {362, 132}, reach), 0, 362, 132));
    // Five under OK's last row. The open button is 1049 points squared away.
    OA_CHECK(landed(kit::reach(notice, {362, 145}, reach), 0, 362, 140));
    // Three from each button. The first, OK, wins.
    OA_CHECK(landed(kit::reach(notice, {333, 132}, reach), 0, 336, 132));
    // Nowhere near either button.
    OA_CHECK(landed(kit::reach(notice, {5, 5}, reach), kit::no_control, 5, 5));
    // Fifteen from the open button's left edge.
    OA_CHECK(landed(kit::reach(notice, {220, 132}, reach), 1, 235, 132));
}

/// Hover, a press, a release on the control and a release off it.
void the_pointer_hovers_presses_and_releases() {
    kit::DisplayList list;
    list.controls.push_back(placed(1, {0, 0, 40, 20}));
    list.controls.push_back(placed(2, {0, 40, 40, 20}));
    kit::Interaction interaction;

    const kit::PointerOutcome entered = kit::pointer_move(interaction, list, {10, 10});
    OA_CHECK(entered.result == kit::PointerResult::redraw);
    OA_CHECK(entered.control == 1);
    OA_CHECK(interaction.hovered == 1);
    const kit::PointerOutcome stayed = kit::pointer_move(interaction, list, {12, 8});
    OA_CHECK(stayed.result == kit::PointerResult::none);
    OA_CHECK(interaction.hovered == 1);
    OA_CHECK(kit::pointer_move(interaction, list, {10, 90}).result == kit::PointerResult::redraw);
    OA_CHECK(interaction.hovered == kit::no_control);

    const kit::PointerOutcome down = kit::pointer_down(interaction, list, {10, 10});
    OA_CHECK(down.result == kit::PointerResult::redraw);
    OA_CHECK(down.control == 1 && down.at.x == 10 && down.at.y == 10);
    OA_CHECK(interaction.pressed == 1);
    OA_CHECK(!interaction.focus_shown);
    OA_CHECK(interaction.focused == kit::no_control);
    const kit::PointerOutcome up = kit::pointer_up(interaction, list, {12, 12});
    OA_CHECK(up.result == kit::PointerResult::activated);
    OA_CHECK(up.control == 1 && up.at.x == 12 && up.at.y == 12);
    OA_CHECK(interaction.pressed == kit::no_control);

    OA_CHECK(kit::pointer_down(interaction, list, {10, 10}).result == kit::PointerResult::redraw);
    const kit::PointerOutcome off = kit::pointer_up(interaction, list, {10, 50});
    OA_CHECK(off.result == kit::PointerResult::redraw);
    OA_CHECK(off.control == 2);
    OA_CHECK(interaction.pressed == kit::no_control);
    OA_CHECK(interaction.focused == kit::no_control);

    OA_CHECK(kit::pointer_down(interaction, list, {10, 90}).result == kit::PointerResult::none);
    OA_CHECK(interaction.pressed == kit::no_control);
    OA_CHECK(kit::pointer_up(interaction, list, {10, 90}).result == kit::PointerResult::none);
}

/// A press moves the focus only after a key has shown it, and only onto a control that takes it.
void the_focus_follows_a_press_only_after_a_key() {
    kit::DisplayList list;
    list.controls.push_back(placed(1, {0, 0, 40, 20}));
    list.controls.push_back(placed(2, {0, 40, 40, 20}));
    kit::Control outside = placed(3, {100, 0, 20, 20});
    list.controls.push_back(outside);
    kit::Control plain = placed(4, {100, 40, 20, 20});
    plain.focusable = false;
    list.controls.push_back(plain);
    list.tab_order = {1, 2, 4};

    kit::Interaction interaction;
    OA_CHECK(kit::pointer_down(interaction, list, {10, 50}).control == 2);
    OA_CHECK(interaction.focused == kit::no_control);
    interaction.pressed = kit::no_control;

    const kit::KeyOutcome shown = kit::key(interaction, list, kit::Key::tab);
    OA_CHECK(shown.result == kit::KeyResult::redraw);
    OA_CHECK(interaction.focus_shown && interaction.focused == 1);

    OA_CHECK(kit::pointer_down(interaction, list, {10, 50}).result == kit::PointerResult::redraw);
    OA_CHECK(interaction.focused == 2);
    OA_CHECK(interaction.focus_shown);

    OA_CHECK(kit::pointer_down(interaction, list, {110, 10}).control == 3);
    OA_CHECK(interaction.focused == 2);
    OA_CHECK(kit::pointer_down(interaction, list, {110, 50}).control == 4);
    OA_CHECK(interaction.focused == 2);
}

/// A finger's shift is kept for the move and the release, then cleared.
void a_fingers_shift_stays_through_the_move_and_release() {
    kit::DisplayList list;
    list.controls.push_back(placed(1, {0, 0, 40, 20}));
    list.controls.push_back(placed(2, {0, 40, 40, 20}));

    kit::Interaction interaction;
    // Ten above the second button, which is as far as the reach allows.
    const kit::PointerOutcome down = kit::finger_down(interaction, list, {10, 30}, 10);
    OA_CHECK(down.result == kit::PointerResult::redraw);
    OA_CHECK(down.control == 2 && down.at.x == 10 && down.at.y == 40);
    OA_CHECK(interaction.pressed == 2);
    OA_CHECK(interaction.finger_shift.x == 0 && interaction.finger_shift.y == 10);

    const kit::PointerOutcome moved = kit::pointer_move(interaction, list, {12, 30});
    OA_CHECK(moved.result == kit::PointerResult::none);
    OA_CHECK(moved.at.x == 12 && moved.at.y == 40);
    OA_CHECK(interaction.finger_shift.y == 10);
    OA_CHECK(interaction.pressed == 2);

    const kit::PointerOutcome away = kit::pointer_move(interaction, list, {12, 20});
    OA_CHECK(away.result == kit::PointerResult::redraw);
    OA_CHECK(away.at.x == 12 && away.at.y == 30);
    OA_CHECK(interaction.hovered == kit::no_control);
    OA_CHECK(interaction.finger_shift.y == 10);

    const kit::PointerOutcome released = kit::pointer_up(interaction, list, {16, 30});
    OA_CHECK(released.result == kit::PointerResult::activated);
    OA_CHECK(released.control == 2 && released.at.x == 16 && released.at.y == 40);
    OA_CHECK(interaction.pressed == kit::no_control);
    OA_CHECK(interaction.finger_shift.x == 0 && interaction.finger_shift.y == 0);

    OA_CHECK(kit::finger_down(interaction, list, {10, 30}, 10).control == 2);
    const kit::PointerOutcome missed = kit::pointer_up(interaction, list, {16, 20});
    OA_CHECK(missed.result == kit::PointerResult::redraw);
    OA_CHECK(missed.at.x == 16 && missed.at.y == 30);
    OA_CHECK(interaction.finger_shift.y == 0);

    // On the button, the finger is not moved.
    const kit::PointerOutcome on = kit::finger_down(interaction, list, {10, 50}, 10);
    OA_CHECK(on.control == 2 && on.at.x == 10 && on.at.y == 50);
    OA_CHECK(interaction.finger_shift.x == 0 && interaction.finger_shift.y == 0);

    // A pointer press drops a shift a finger left behind.
    interaction.finger_shift = {4, 4};
    OA_CHECK(kit::pointer_down(interaction, list, {10, 10}).control == 1);
    OA_CHECK(interaction.finger_shift.x == 0 && interaction.finger_shift.y == 0);
}

/// Tab wraps both ways, and from a control that is not in the order.
void tab_wraps_both_ways_and_from_no_focus() {
    kit::DisplayList list;
    list.controls.push_back(placed(1, {0, 0, 10, 10}));
    list.controls.push_back(placed(2, {0, 20, 10, 10}));
    list.controls.push_back(placed(3, {0, 40, 10, 10}));
    list.tab_order = {1, 2, 3};

    OA_CHECK(kit::next_in_tab_order(list, 1, true) == 2);
    OA_CHECK(kit::next_in_tab_order(list, 2, true) == 3);
    OA_CHECK(kit::next_in_tab_order(list, 3, true) == 1);
    OA_CHECK(kit::next_in_tab_order(list, 1, false) == 3);
    OA_CHECK(kit::next_in_tab_order(list, 3, false) == 2);
    OA_CHECK(kit::next_in_tab_order(list, kit::no_control, true) == 1);
    OA_CHECK(kit::next_in_tab_order(list, kit::no_control, false) == 3);
    OA_CHECK(kit::next_in_tab_order(list, 9, true) == 1);
    OA_CHECK(kit::next_in_tab_order(list, 9, false) == 3);

    kit::DisplayList empty;
    OA_CHECK(kit::next_in_tab_order(empty, kit::no_control, true) == kit::no_control);
    kit::Interaction interaction;
    OA_CHECK(kit::key(interaction, empty, kit::Key::tab).result == kit::KeyResult::none);
}

/// Up and Down step one row of a column. Left and Right find nothing, and a
/// disabled row is skipped.
void the_arrows_step_a_column_one_row_at_a_time() {
    kit::DisplayList column;
    for (int32_t index = 0; index < 6; ++index) {
        column.controls.push_back(placed(index + 1, {0, index * 24, 80, 20}));
        column.tab_order.push_back(index + 1);
    }
    OA_CHECK(kit::focus_toward(column, 1, kit::Direction::down) == 2);
    OA_CHECK(kit::focus_toward(column, 5, kit::Direction::down) == 6);
    OA_CHECK(kit::focus_toward(column, 6, kit::Direction::up) == 5);
    OA_CHECK(kit::focus_toward(column, 2, kit::Direction::up) == 1);
    OA_CHECK(kit::focus_toward(column, 1, kit::Direction::up) == kit::no_control);
    OA_CHECK(kit::focus_toward(column, 6, kit::Direction::down) == kit::no_control);
    OA_CHECK(kit::focus_toward(column, 3, kit::Direction::left) == kit::no_control);
    OA_CHECK(kit::focus_toward(column, 3, kit::Direction::right) == kit::no_control);

    column.controls[2].enabled = false;
    OA_CHECK(kit::focus_toward(column, 2, kit::Direction::down) == 4);
    OA_CHECK(kit::focus_toward(column, 4, kit::Direction::up) == 2);

    // Closer than the next row, and not in the order, so the focus does not stop on it.
    column.controls.push_back(placed(7, {0, 20, 80, 4}));
    OA_CHECK(kit::focus_toward(column, 1, kit::Direction::down) == 2);
}

/// Up and Down follow the rectangles, not the declared order.
void up_and_down_do_not_follow_tab_order() {
    kit::DisplayList column;
    column.controls.push_back(placed(1, {0, 0, 80, 20}));
    column.controls.push_back(placed(2, {0, 24, 80, 20}));
    column.controls.push_back(placed(3, {0, 48, 80, 20}));
    // Top, bottom, middle. Down from the top would reach the bottom if it followed this.
    column.tab_order = {1, 3, 2};
    OA_CHECK(kit::focus_toward(column, 1, kit::Direction::down) == 2);
    OA_CHECK(kit::focus_toward(column, 3, kit::Direction::up) == 2);
    OA_CHECK(kit::next_in_tab_order(column, 1, true) == 3);
}

/// Right from a row reaches the details' first control at that height, and Left returns.
void right_from_a_row_reaches_the_details_beside_it() {
    kit::DisplayList list;
    for (int32_t index = 0; index < 3; ++index) {
        const int32_t y = index * 28;
        list.controls.push_back(placed(index + 1, {0, y, 90, 22}));
        list.controls.push_back(placed(index + 4, {110, y, 70, 22}));
        list.tab_order.push_back(index + 1);
    }
    // A further control at the middle row's height, after the one Right should take.
    list.controls.push_back(placed(7, {190, 28, 40, 22}));
    list.tab_order.push_back(4);
    list.tab_order.push_back(5);
    list.tab_order.push_back(7);
    list.tab_order.push_back(6);

    OA_CHECK(kit::focus_toward(list, 1, kit::Direction::right) == 4);
    OA_CHECK(kit::focus_toward(list, 2, kit::Direction::right) == 5);
    OA_CHECK(kit::focus_toward(list, 3, kit::Direction::right) == 6);
    OA_CHECK(kit::focus_toward(list, 5, kit::Direction::left) == 2);
    OA_CHECK(kit::focus_toward(list, 4, kit::Direction::left) == 1);
    OA_CHECK(kit::focus_toward(list, 6, kit::Direction::left) == 3);
}

/// Each arrow from the centre of a grid reaches the neighbour in that direction.
void a_grid_moves_to_the_neighbour_in_line() {
    kit::DisplayList grid;
    for (int32_t row = 0; row < 3; ++row) {
        for (int32_t column = 0; column < 3; ++column) {
            const kit::ControlId id = row * 3 + column + 1;
            grid.controls.push_back(placed(id, {column * 36, row * 36, 20, 20}));
            grid.tab_order.push_back(id);
        }
    }
    OA_CHECK(kit::focus_toward(grid, 5, kit::Direction::right) == 6);
    OA_CHECK(kit::focus_toward(grid, 5, kit::Direction::left) == 4);
    OA_CHECK(kit::focus_toward(grid, 5, kit::Direction::up) == 2);
    OA_CHECK(kit::focus_toward(grid, 5, kit::Direction::down) == 8);
    OA_CHECK(kit::focus_toward(grid, 1, kit::Direction::up) == kit::no_control);
    OA_CHECK(kit::focus_toward(grid, 1, kit::Direction::left) == kit::no_control);
    OA_CHECK(kit::focus_toward(grid, 1, kit::Direction::right) == 2);
    OA_CHECK(kit::focus_toward(grid, 1, kit::Direction::down) == 4);
    OA_CHECK(kit::focus_toward(grid, 9, kit::Direction::down) == kit::no_control);
    OA_CHECK(kit::focus_toward(grid, 9, kit::Direction::right) == kit::no_control);
}

/// A control in line beats a nearer one off to the side.
void the_beam_beats_a_nearer_diagonal() {
    // The diagonal scores 2 + 2 * 6. The control in line is 80 below. Down takes the line.
    kit::DisplayList list;
    list.controls.push_back(placed(1, {0, 0, 40, 10}));
    list.controls.push_back(placed(2, {46, 12, 8, 8}));
    list.controls.push_back(placed(3, {0, 90, 40, 10}));
    list.tab_order = {1, 2, 3};
    OA_CHECK(kit::focus_toward(list, 1, kit::Direction::down) == 3);
    OA_CHECK(kit::focus_toward(list, 1, kit::Direction::right) == 2);
}

/// Down from the last row reaches the footer button under it, not the one beside that button.
void down_from_the_last_row_reaches_the_footer_under_it() {
    kit::DisplayList list;
    list.controls.push_back(placed(1, {10, 0, 40, 18}));
    list.controls.push_back(placed(2, {10, 20, 40, 18}));
    list.controls.push_back(placed(3, {10, 40, 40, 18}));
    list.controls.push_back(placed(4, {10, 70, 40, 18}));
    list.controls.push_back(placed(5, {80, 70, 40, 18}));
    list.tab_order = {1, 2, 3, 4, 5};
    OA_CHECK(kit::focus_toward(list, 3, kit::Direction::down) == 4);
    OA_CHECK(kit::focus_toward(list, 4, kit::Direction::down) == kit::no_control);
    OA_CHECK(kit::focus_toward(list, 4, kit::Direction::right) == 5);
}

/// A scroll area of ten rows, four of them in view, and a footer under the view.
void a_scroll_area_is_searched_before_what_is_outside_it() {
    kit::DisplayList list;
    const kit::Rect view{0, 0, 100, 80};
    for (int32_t index = 0; index < 10; ++index) {
        kit::Control row = placed(index + 1, {0, index * 20, 80, 20});
        row.clip = view;
        row.group = 1;
        list.controls.push_back(row);
        list.tab_order.push_back(row.id);
    }
    kit::Control footer = placed(11, {0, 220, 80, 20});
    footer.group = -1;
    list.controls.push_back(footer);
    list.tab_order.push_back(11);

    // The fifth row is hidden, and it is still the next row. The footer is not.
    OA_CHECK(kit::focus_toward(list, 4, kit::Direction::down) == 5);
    OA_CHECK(kit::hit(list, {10, 80}) == kit::no_control);
    // Nothing further in the area, so Down leaves it for the footer.
    OA_CHECK(kit::focus_toward(list, 10, kit::Direction::down) == 11);
    // The hidden last row is nearer the footer than the last row in view. Up stays in view.
    OA_CHECK(kit::focus_toward(list, 11, kit::Direction::up) == 4);
    OA_CHECK(kit::focus_toward(list, 11, kit::Direction::down) == kit::no_control);
    OA_CHECK(kit::focus_toward(list, 1, kit::Direction::up) == kit::no_control);
}

/// A scroll area of ten rows, four of them in view and scrolled down three rows, a button
/// above the view and one under it. Judged where they lie, the arrows stay in view. Judged
/// with the rows laid out whole, Up from the button under the view reaches the last row and
/// Down from the one above reaches the first, both out of view; the rows step one by one and
/// leave the area for the buttons at its ends.
void the_rows_laid_out_whole_reach_past_the_view() {
    kit::DisplayList list;
    list.controls.push_back(placed(21, {0, 0, 80, 20}));
    list.tab_order.push_back(21);
    kit::ScrollArea area{};
    area.view = {0, 30, 100, 80};
    area.offset = 60;
    area.limit = 10 * 20 - 80;
    for (int32_t index = 0; index < 10; ++index) {
        kit::Control row = placed(index + 1, {0, 30 + index * 20 - area.offset, 80, 20});
        row.clip = area.view;
        row.group = 0;
        list.controls.push_back(row);
        list.tab_order.push_back(row.id);
    }
    list.controls.push_back(placed(22, {0, 120, 80, 20}));
    list.tab_order.push_back(22);
    const std::span<const kit::ScrollArea> areas(&area, 1);

    // Where they lie, rows 4 to 7 show.
    OA_CHECK(kit::focus_toward(list, 22, kit::Direction::up) == 7);
    OA_CHECK(kit::focus_toward(list, 21, kit::Direction::down) == 4);
    // Laid out whole.
    OA_CHECK(kit::focus_toward(list, 22, kit::Direction::up, areas) == 10);
    OA_CHECK(kit::focus_toward(list, 21, kit::Direction::down, areas) == 1);
    OA_CHECK(kit::focus_toward(list, 5, kit::Direction::down, areas) == 6);
    OA_CHECK(kit::focus_toward(list, 6, kit::Direction::up, areas) == 5);
    OA_CHECK(kit::focus_toward(list, 10, kit::Direction::down, areas) == 22);
    OA_CHECK(kit::focus_toward(list, 1, kit::Direction::up, areas) == 21);
    OA_CHECK(kit::focus_toward(list, 21, kit::Direction::up, areas) == kit::no_control);
    OA_CHECK(kit::focus_toward(list, 22, kit::Direction::down, areas) == kit::no_control);
}

/// Controls in no scroll area are not an area of their own: from a list of
/// entries at the left, Right reaches the row at the entry's height, not a
/// footer button; Up from the footer reaches the row above it, not an entry
/// off to the side; Down from the last entry reaches the footer under it.
void controls_in_no_area_search_every_control_at_once() {
    kit::DisplayList list;
    const kit::Rect view{100, 0, 100, 72};
    for (int32_t index = 0; index < 3; ++index) {
        kit::Control entry = placed(index + 1, {0, index * 24, 80, 20});
        list.controls.push_back(entry);
        list.tab_order.push_back(entry.id);
    }
    for (int32_t index = 0; index < 4; ++index) {
        kit::Control row = placed(index + 11, {150, index * 24 + 4, 50, 16});
        row.clip = view;
        row.group = 1;
        list.controls.push_back(row);
        list.tab_order.push_back(row.id);
    }
    list.controls.push_back(placed(21, {0, 90, 80, 16}));
    list.controls.push_back(placed(22, {150, 90, 50, 16}));
    list.tab_order.push_back(21);
    list.tab_order.push_back(22);

    OA_CHECK(kit::focus_toward(list, 2, kit::Direction::right) == 12);
    OA_CHECK(kit::focus_toward(list, 3, kit::Direction::right) == 13);
    // The fourth row lies under the view: the footer goes up to the third.
    OA_CHECK(kit::focus_toward(list, 22, kit::Direction::up) == 13);
    OA_CHECK(kit::focus_toward(list, 21, kit::Direction::up) == 3);
    OA_CHECK(kit::focus_toward(list, 3, kit::Direction::down) == 21);
    OA_CHECK(kit::focus_toward(list, 21, kit::Direction::right) == 22);
    // A row still keeps to its own area first.
    OA_CHECK(kit::focus_toward(list, 13, kit::Direction::down) == 14);
    OA_CHECK(kit::focus_toward(list, 12, kit::Direction::left) == 2);
}

/// Right reaches a button inside a row, and a control that only overlaps the row is not beyond it.
void right_reaches_the_button_inside_a_row() {
    kit::DisplayList overlapping;
    overlapping.controls.push_back(placed(1, {0, 0, 200, 24}));
    overlapping.controls.push_back(placed(2, {190, 4, 30, 16}));
    overlapping.tab_order = {1, 2};
    OA_CHECK(kit::focus_toward(overlapping, 1, kit::Direction::right) == kit::no_control);

    kit::DisplayList row;
    row.controls.push_back(placed(1, {0, 0, 200, 24}));
    row.controls.push_back(placed(2, {150, 4, 40, 16}));
    row.tab_order = {1, 2};
    OA_CHECK(kit::focus_toward(row, 1, kit::Direction::right) == 2);
    OA_CHECK(kit::focus_toward(row, 2, kit::Direction::left) == 1);
}

/// Two controls in the same place: the earlier in the declared order wins,
/// whichever was listed first.
void an_earlier_place_in_tab_order_wins_a_tie() {
    kit::DisplayList list;
    list.controls.push_back(placed(1, {0, 0, 40, 20}));
    list.controls.push_back(placed(3, {0, 30, 40, 20}));
    list.controls.push_back(placed(2, {0, 30, 40, 20}));
    list.tab_order = {1, 2, 3};
    OA_CHECK(kit::focus_toward(list, 1, kit::Direction::down) == 2);
    list.tab_order = {1, 3, 2};
    OA_CHECK(kit::focus_toward(list, 1, kit::Direction::down) == 3);
}

/// With no focus, each key shows an end or does what it does with nothing focused.
void keys_with_no_focus_show_an_end_of_the_order() {
    kit::DisplayList list;
    list.controls.push_back(placed(1, {0, 0, 30, 16}));
    kit::Control slider = placed(2, {0, 20, 30, 16});
    slider.kind = kit::ControlKind::slider;
    slider.steps = true;
    list.controls.push_back(slider);
    list.controls.push_back(placed(3, {50, 20, 30, 16}));
    kit::Control link = placed(4, {0, 40, 30, 16});
    link.kind = kit::ControlKind::link;
    list.controls.push_back(link);
    kit::Control item = placed(5, {0, 60, 30, 16});
    item.kind = kit::ControlKind::list_item;
    list.controls.push_back(item);
    list.tab_order = {1, 2, 3, 4, 5};

    const auto shown = [&](kit::Key key, kit::ControlId id) {
        kit::Interaction interaction;
        const kit::KeyOutcome outcome = kit::key(interaction, list, key);
        OA_CHECK(outcome.result == kit::KeyResult::redraw);
        OA_CHECK(outcome.control == id);
        OA_CHECK(outcome.key == key);
        OA_CHECK(interaction.focus_shown && interaction.focused == id);
    };
    shown(kit::Key::up, 5);
    shown(kit::Key::back_tab, 5);
    shown(kit::Key::tab, 1);
    shown(kit::Key::down, 1);
    shown(kit::Key::left, 1);
    shown(kit::Key::right, 1);
    shown(kit::Key::space, 1);

    kit::Interaction interaction;
    const kit::KeyOutcome accepted = kit::key(interaction, list, kit::Key::enter);
    OA_CHECK(accepted.result == kit::KeyResult::accept);
    OA_CHECK(accepted.control == kit::no_control);
    OA_CHECK(!interaction.focus_shown);

    OA_CHECK(kit::key(interaction, list, kit::Key::escape).result == kit::KeyResult::cancel);
    const kit::KeyOutcome scrolled = kit::key(interaction, list, kit::Key::page_down);
    OA_CHECK(scrolled.result == kit::KeyResult::scroll);
    OA_CHECK(scrolled.control == kit::no_control);
    OA_CHECK(kit::key(interaction, list, kit::Key::page_up).result == kit::KeyResult::scroll);
    OA_CHECK(kit::key(interaction, list, kit::Key::home).result == kit::KeyResult::scroll);
    OA_CHECK(kit::key(interaction, list, kit::Key::end).result == kit::KeyResult::scroll);
    OA_CHECK(kit::key(interaction, list, kit::Key::yes).result == kit::KeyResult::none);
    OA_CHECK(kit::key(interaction, list, kit::Key::no).result == kit::KeyResult::none);
    OA_CHECK(kit::key(interaction, list, kit::Key::info).result == kit::KeyResult::none);
    OA_CHECK(!interaction.focus_shown);

    // The first control steps, and Left still only shows it.
    list.tab_order = {2, 1, 3, 4, 5};
    kit::Interaction stepping;
    const kit::KeyOutcome left = kit::key(stepping, list, kit::Key::left);
    OA_CHECK(left.result == kit::KeyResult::redraw);
    OA_CHECK(left.control == 2);
    OA_CHECK(stepping.focused == 2);
    const kit::KeyOutcome space = kit::key(stepping, list, kit::Key::space);
    // Focus is already shown, so Space now goes to the control.
    stepping.focus_shown = false;
    stepping.focused = kit::no_control;
    const kit::KeyOutcome fresh = kit::key(stepping, list, kit::Key::space);
    OA_CHECK(fresh.result == kit::KeyResult::redraw);
    OA_CHECK(fresh.control == 2);
    OA_CHECK(space.result == kit::KeyResult::to_control);
}

/// Left and Right on a control that steps go to it, and the focus stays even
/// with a neighbour there.
void keys_on_a_stepping_control_stay_with_it() {
    kit::DisplayList list;
    list.controls.push_back(placed(1, {0, 0, 30, 16}));
    kit::Control slider = placed(2, {0, 20, 30, 16});
    slider.kind = kit::ControlKind::slider;
    slider.steps = true;
    list.controls.push_back(slider);
    list.controls.push_back(placed(3, {50, 20, 30, 16}));
    kit::Control link = placed(4, {0, 40, 30, 16});
    link.kind = kit::ControlKind::link;
    list.controls.push_back(link);
    kit::Control item = placed(5, {0, 60, 30, 16});
    item.kind = kit::ControlKind::list_item;
    list.controls.push_back(item);
    list.tab_order = {1, 2, 3, 4, 5};

    kit::Interaction interaction;
    interaction.focus_shown = true;
    interaction.focused = 2;
    const kit::KeyOutcome right = kit::key(interaction, list, kit::Key::right);
    OA_CHECK(right.result == kit::KeyResult::to_control);
    OA_CHECK(right.control == 2 && right.key == kit::Key::right);
    OA_CHECK(interaction.focused == 2);
    const kit::KeyOutcome left = kit::key(interaction, list, kit::Key::left);
    OA_CHECK(left.result == kit::KeyResult::to_control);
    OA_CHECK(interaction.focused == 2);

    const kit::KeyOutcome enter = kit::key(interaction, list, kit::Key::enter);
    OA_CHECK(enter.result == kit::KeyResult::accept);
    OA_CHECK(enter.control == kit::no_control);
    OA_CHECK(interaction.focused == 2);
    OA_CHECK(!interaction.focus_shown);
    interaction.focus_shown = true;

    const kit::KeyOutcome space = kit::key(interaction, list, kit::Key::space);
    OA_CHECK(space.result == kit::KeyResult::to_control);
    OA_CHECK(interaction.focused == 2);

    // Down is the link below, not the next control in the order, which sits to the right.
    const kit::KeyOutcome down = kit::key(interaction, list, kit::Key::down);
    OA_CHECK(down.result == kit::KeyResult::redraw);
    OA_CHECK(down.control == 4);
    OA_CHECK(interaction.focused == 4);
    const kit::KeyOutcome up = kit::key(interaction, list, kit::Key::up);
    OA_CHECK(up.result == kit::KeyResult::redraw);
    OA_CHECK(interaction.focused == 2);

    const kit::KeyOutcome tab = kit::key(interaction, list, kit::Key::tab);
    OA_CHECK(tab.result == kit::KeyResult::redraw);
    OA_CHECK(interaction.focused == 3);
    const kit::KeyOutcome back = kit::key(interaction, list, kit::Key::back_tab);
    OA_CHECK(back.result == kit::KeyResult::redraw);
    OA_CHECK(interaction.focused == 2);

    OA_CHECK(kit::key(interaction, list, kit::Key::escape).result == kit::KeyResult::cancel);
    OA_CHECK(interaction.focused == 2);
    const kit::KeyOutcome page = kit::key(interaction, list, kit::Key::page_up);
    OA_CHECK(page.result == kit::KeyResult::scroll && page.control == 2);
    OA_CHECK(kit::key(interaction, list, kit::Key::home).result == kit::KeyResult::scroll);
    OA_CHECK(kit::key(interaction, list, kit::Key::end).result == kit::KeyResult::scroll);
    OA_CHECK(kit::key(interaction, list, kit::Key::page_down).result == kit::KeyResult::scroll);
    OA_CHECK(kit::key(interaction, list, kit::Key::yes).result == kit::KeyResult::none);
    OA_CHECK(kit::key(interaction, list, kit::Key::no).result == kit::KeyResult::none);
    OA_CHECK(kit::key(interaction, list, kit::Key::info).result == kit::KeyResult::none);
    OA_CHECK(interaction.focused == 2);
}

/// Enter goes to a button, a link and a list row, and elsewhere the screen accepts.
void keys_on_a_button_a_link_and_a_list_row() {
    kit::DisplayList list;
    list.controls.push_back(placed(1, {0, 0, 30, 16}));
    list.controls.push_back(placed(3, {50, 20, 30, 16}));
    kit::Control link = placed(4, {0, 40, 30, 16});
    link.kind = kit::ControlKind::link;
    list.controls.push_back(link);
    kit::Control item = placed(5, {0, 60, 30, 16});
    item.kind = kit::ControlKind::list_item;
    list.controls.push_back(item);
    kit::Control several = placed(6, {0, 80, 30, 16});
    several.kind = kit::ControlKind::buttons;
    list.controls.push_back(several);
    list.tab_order = {1, 3, 4, 5, 6};

    kit::Interaction interaction;
    interaction.focus_shown = true;
    interaction.focused = 1;
    const kit::KeyOutcome entered = kit::key(interaction, list, kit::Key::enter);
    OA_CHECK(entered.result == kit::KeyResult::to_control);
    OA_CHECK(entered.control == 1);
    OA_CHECK(!interaction.focus_shown && interaction.focused == 1);
    interaction.focus_shown = true;
    const kit::KeyOutcome right = kit::key(interaction, list, kit::Key::right);
    OA_CHECK(right.result == kit::KeyResult::redraw);
    OA_CHECK(right.control == 3);
    OA_CHECK(interaction.focused == 3);
    const kit::KeyOutcome stayed = kit::key(interaction, list, kit::Key::right);
    OA_CHECK(stayed.result == kit::KeyResult::none);
    OA_CHECK(interaction.focused == 3);

    interaction.focused = 4;
    interaction.focus_shown = true;
    OA_CHECK(kit::key(interaction, list, kit::Key::enter).result == kit::KeyResult::to_control);
    OA_CHECK(!interaction.focus_shown);
    interaction.focused = 5;
    interaction.focus_shown = true;
    const kit::KeyOutcome row = kit::key(interaction, list, kit::Key::enter);
    OA_CHECK(row.result == kit::KeyResult::to_control && row.control == 5);
    OA_CHECK(!interaction.focus_shown);
    interaction.focused = 6;
    interaction.focus_shown = true;
    const kit::KeyOutcome group = kit::key(interaction, list, kit::Key::enter);
    OA_CHECK(group.result == kit::KeyResult::accept);
    OA_CHECK(interaction.focused == 6);
    OA_CHECK(!interaction.focus_shown);
    interaction.focus_shown = true;

    interaction.focused = 5;
    const kit::KeyOutcome wrapped = kit::key(interaction, list, kit::Key::tab);
    OA_CHECK(wrapped.result == kit::KeyResult::redraw);
    OA_CHECK(interaction.focused == 6);
    OA_CHECK(kit::key(interaction, list, kit::Key::tab).control == 1);
    interaction.focused = 1;
    OA_CHECK(kit::key(interaction, list, kit::Key::back_tab).control == 6);

    const kit::KeyOutcome space = kit::key(interaction, list, kit::Key::space);
    OA_CHECK(space.result == kit::KeyResult::to_control && space.control == 6);
}

/// The wheel's fractions, at the middle, the top and the end.
void the_wheel_keeps_the_dialogs_fractions() {
    constexpr int32_t step = 24;
    constexpr int32_t limit = 80;
    kit::WheelCarry carry;

    OA_CHECK(kit::wheel_offset(carry, 1.0F, step, 40, limit) == 16);
    OA_CHECK(carry.rows == 0.0F);
    OA_CHECK(kit::wheel_offset(carry, -1.0F, step, 40, limit) == 64);
    OA_CHECK(carry.rows == 0.0F);

    int32_t offset = kit::wheel_offset(carry, 0.4F, step, 40, limit);
    OA_CHECK(offset == 31);
    OA_CHECK(carry.rows == -0x1.33334p-1F);
    offset = kit::wheel_offset(carry, 0.4F, step, offset, limit);
    OA_CHECK(offset == 21);
    OA_CHECK(carry.rows == -0x1.999ap-3F);
    offset = kit::wheel_offset(carry, 0.4F, step, offset, limit);
    OA_CHECK(offset == 12);
    OA_CHECK(carry.rows == -0x1.9999cp-1F);

    OA_CHECK(kit::wheel_offset(carry, 50.0F, step, 40, limit) == 0);
    OA_CHECK(carry.rows == 0.0F);

    OA_CHECK(kit::wheel_offset(carry, 1.0F, step, 0, limit) == 0);
    OA_CHECK(carry.rows == 0.0F);
    OA_CHECK(kit::wheel_offset(carry, -1.0F, step, 0, limit) == 24);
    OA_CHECK(carry.rows == 0.0F);
    OA_CHECK(kit::wheel_offset(carry, 0.4F, step, 0, limit) == 0);
    OA_CHECK(carry.rows == 0.0F);
    OA_CHECK(kit::wheel_offset(carry, 0.4F, step, 0, limit) == 0);
    OA_CHECK(kit::wheel_offset(carry, 0.4F, step, 0, limit) == 0);
    OA_CHECK(carry.rows == 0.0F);
    OA_CHECK(kit::wheel_offset(carry, 50.0F, step, 0, limit) == 0);
    OA_CHECK(carry.rows == 0.0F);

    OA_CHECK(kit::wheel_offset(carry, 1.0F, step, limit, limit) == 56);
    OA_CHECK(carry.rows == 0.0F);
    OA_CHECK(kit::wheel_offset(carry, -1.0F, step, limit, limit) == limit);
    OA_CHECK(carry.rows == 0.0F);
    offset = kit::wheel_offset(carry, 0.4F, step, limit, limit);
    OA_CHECK(offset == 71);
    OA_CHECK(carry.rows == -0x1.33334p-1F);
    offset = kit::wheel_offset(carry, 0.4F, step, offset, limit);
    OA_CHECK(offset == 61);
    OA_CHECK(carry.rows == -0x1.999ap-3F);
    offset = kit::wheel_offset(carry, 0.4F, step, offset, limit);
    OA_CHECK(offset == 52);
    OA_CHECK(carry.rows == -0x1.9999cp-1F);
    OA_CHECK(kit::wheel_offset(carry, -0.4F, step, limit, limit) == limit);
    OA_CHECK(carry.rows == 0.0F);
    OA_CHECK(kit::wheel_offset(carry, 50.0F, step, limit, limit) == 0);
    OA_CHECK(carry.rows == 0.0F);
    OA_CHECK(kit::wheel_offset(carry, -50.0F, step, limit, limit) == limit);
    OA_CHECK(carry.rows == 0.0F);

    carry.rows = 0.25F;
    const float quiet = std::numeric_limits<float>::quiet_NaN();
    OA_CHECK(kit::wheel_offset(carry, quiet, step, 40, limit) == 40);
    OA_CHECK(carry.rows == 0.25F);
    const float endless = std::numeric_limits<float>::infinity();
    OA_CHECK(kit::wheel_offset(carry, endless, step, 40, limit) == 40);
    OA_CHECK(carry.rows == 0.25F);

    carry.rows = 0.5F;
    OA_CHECK(kit::wheel_offset(carry, 1.0F, step, 5, 0) == 5);
    OA_CHECK(carry.rows == 0.0F);
}

/// Names are present, at most 100 bytes, words joined by dots, and unique.
void a_name_must_be_present_well_formed_and_unique() {
    const auto lone = [](kit::ControlId id, std::string name) {
        kit::DisplayList list;
        kit::Control control = placed(id, {0, 0, 10, 10});
        control.name = std::move(name);
        list.controls.push_back(std::move(control));
        return kit::name_problem(list);
    };
    OA_CHECK(lone(4, "") == "Control 4 has no name.");
    OA_CHECK(
        lone(7, std::string(101, 'a')) ==
        "The name of control 7 is 101 bytes, and a name is at most 100."
    );
    OA_CHECK(
        lone(1, "has space") ==
        "The name \"has space\" is not words of a-z, 0-9 and hyphens joined by dots."
    );
    OA_CHECK(
        lone(1, "Settings") ==
        "The name \"Settings\" is not words of a-z, 0-9 and hyphens joined by dots."
    );
    OA_CHECK(
        lone(1, ".settings") ==
        "The name \".settings\" is not words of a-z, 0-9 and hyphens joined by dots."
    );
    OA_CHECK(
        lone(1, "settings.") ==
        "The name \"settings.\" is not words of a-z, 0-9 and hyphens joined by dots."
    );

    kit::DisplayList doubled;
    kit::Control first = placed(1, {0, 0, 10, 10});
    first.name = "settings.ok";
    kit::Control second = placed(2, {0, 20, 10, 10});
    second.name = "settings.ok";
    doubled.controls.push_back(std::move(first));
    doubled.controls.push_back(std::move(second));
    OA_CHECK(kit::name_problem(doubled) == "The name \"settings.ok\" is used more than once.");

    kit::DisplayList good;
    const std::string full(100, 'a');
    for (const char* name : {"settings.vertical-sync", "library.row.3", "map.card-2"}) {
        kit::Control control =
            placed(static_cast<kit::ControlId>(good.controls.size() + 1), {0, 0, 8, 8});
        control.name = name;
        good.controls.push_back(std::move(control));
    }
    kit::Control hundred = placed(4, {0, 0, 8, 8});
    hundred.name = full;
    good.controls.push_back(std::move(hundred));
    OA_CHECK(kit::name_problem(good).empty());
    OA_CHECK(lone(1, "a-b.c-d").empty());
}

/// Automation lists every named control, and a name finds the first control that has it.
void automation_reads_the_named_controls() {
    kit::DisplayList list;
    kit::Control ok = placed(1, {1, 2, 30, 16});
    ok.name = "screen.ok";
    ok.kind = kit::ControlKind::button;
    ok.checked = true;
    ok.text = "OK";
    list.controls.push_back(ok);

    kit::Control unnamed = placed(2, {0, 0, 10, 10});
    list.controls.push_back(unnamed);

    kit::Control row = placed(3, {0, 100, 40, 20});
    row.name = "screen.row";
    row.kind = kit::ControlKind::list_item;
    row.clip = {0, 0, 40, 10};
    row.text = "A row";
    list.controls.push_back(row);

    kit::Control slider = placed(4, {0, 40, 80, 16});
    slider.name = "screen.level";
    slider.kind = kit::ControlKind::slider;
    slider.enabled = false;
    slider.text = "40";
    list.controls.push_back(slider);

    kit::Control again = placed(5, {40, 2, 30, 16});
    again.name = "screen.ok";
    list.controls.push_back(again);

    kit::Interaction interaction;
    interaction.focus_shown = true;
    interaction.focused = 1;
    const std::vector<kit::AutomationEntry> entries = kit::automation_entries(list, interaction);
    OA_CHECK(entries.size() == 4u);
    OA_CHECK(entries[0].name == "screen.ok");
    OA_CHECK(entries[0].kind == kit::ControlKind::button);
    OA_CHECK(entries[0].rect.x == 1 && entries[0].rect.y == 2);
    OA_CHECK(entries[0].rect.width == 30 && entries[0].rect.height == 16);
    OA_CHECK(entries[0].shown && entries[0].enabled && entries[0].focused && entries[0].checked);
    OA_CHECK(entries[0].text == "OK");
    OA_CHECK(entries[1].name == "screen.row");
    OA_CHECK(!entries[1].shown && !entries[1].focused);
    OA_CHECK(entries[1].text == "A row");
    OA_CHECK(entries[2].name == "screen.level");
    OA_CHECK(entries[2].shown && !entries[2].enabled && !entries[2].focused);
    OA_CHECK(entries[3].name == "screen.ok" && !entries[3].focused);

    interaction.focus_shown = false;
    const std::vector<kit::AutomationEntry> plain = kit::automation_entries(list, interaction);
    // The tree names the focused control from the moment it is focused,
    // whether or not the ring is drawn.
    OA_CHECK(plain[0].focused);
    OA_CHECK(!plain[1].focused);

    OA_CHECK(kit::control_named(list, "screen.ok") == 1);
    OA_CHECK(kit::control_named(list, "screen.row") == 3);
    OA_CHECK(kit::control_named(list, "screen.level") == 4);
    OA_CHECK(kit::control_named(list, "missing") == kit::no_control);
}

/// Returns the entry of a name.
///
/// @param entries the entries
/// @param name the name
/// @return the entry; null when none has it
const kit::AutomationEntry*
entry_named(const std::vector<kit::AutomationEntry>& entries, std::string_view name) {
    for (const kit::AutomationEntry& entry : entries)
        if (entry.name == name)
            return &entry;
    return nullptr;
}

/// Tells whether a rectangle is one given.
///
/// @param rect the rectangle
/// @param x its left column
/// @param y its top row
/// @param width its width
/// @param height its height
/// @return true when every edge matches
bool rect_is(const kit::Rect& rect, int32_t x, int32_t y, int32_t width, int32_t height) {
    return rect.x == x && rect.y == y && rect.width == width && rect.height == height;
}

/// A switch's two halves split its rectangle, the On half from its middle
/// column, as a press there turns it on.
void a_switch_lists_its_two_halves() {
    kit::DisplayList list;
    kit::add_switch(list, {100, 20, 53, 16}, {true, false, false, "OFF", "ON"}, 4, "screen.sound");
    const auto entries = kit::automation_parts(list, {});
    OA_CHECK(entries.size() == 3u);
    if (entries.size() != 3u)
        return;
    OA_CHECK(entries[0].name == "screen.sound" && !entries[0].part);
    OA_CHECK(entries[0].kind == kit::ControlKind::toggle && entries[0].focusable);
    OA_CHECK(entries[1].name == "screen.sound.off" && entries[1].part);
    OA_CHECK(entries[2].name == "screen.sound.on" && entries[2].part);
    OA_CHECK(rect_is(entries[1].rect, 100, 20, 26, 16));
    OA_CHECK(rect_is(entries[2].rect, 126, 20, 27, 16));
    OA_CHECK(!entries[1].checked && entries[2].checked);
    OA_CHECK(entries[1].text == "OFF" && entries[2].text == "ON");
    OA_CHECK(entries[1].kind == kit::ControlKind::toggle && !entries[1].focusable);
    OA_CHECK(entries[1].enabled && entries[1].shown && !entries[1].focused);
    OA_CHECK(entries[0].role == kit::AccessRole::switch_control && entries[0].pressed);
    OA_CHECK(entries[1].role == kit::AccessRole::check_box && !entries[1].chosen);
    OA_CHECK(entries[2].role == kit::AccessRole::check_box && entries[2].chosen);
    // Each half's centre lies on its own side of the middle column.
    for (std::size_t half = 1; half < 3; ++half) {
        const kit::Rect& rect = entries[half].rect;
        const int32_t centre = rect.x + rect.width / 2;
        OA_CHECK((centre >= 100 + 53 / 2) == (half == 2));
    }
    // automation_entries lists the switch alone.
    OA_CHECK(kit::automation_entries(list, {}).size() == 1u);
}

/// A strip's levels lie where level_at finds them, and only those offered
/// take a press.
void a_strip_lists_its_levels_where_level_at_finds_them() {
    kit::DisplayList list;
    kit::LevelsLook look;
    look.captions = {"Off", "25%", "50%"};
    look.level_width = 34;
    look.chosen = 1;
    look.offered = 2;
    const kit::Rect strip{40, 10, 3 * 34 + 2, 16};
    kit::add_levels(list, strip, look, 7, "screen.edge");
    list.controls.back().parts = {"off", "25"};
    const auto entries = kit::automation_parts(list, {});
    OA_CHECK(entries.size() == 4u);
    if (entries.size() != 4u)
        return;
    OA_CHECK(entries[1].name == "screen.edge.off");
    OA_CHECK(entries[2].name == "screen.edge.25");
    // A level the control gives no word is named by its place from 1.
    OA_CHECK(entries[3].name == "screen.edge.3");
    OA_CHECK(entries[0].role == kit::AccessRole::radio_group);
    OA_CHECK(entries[0].set_size == 3 && entries[0].place == 2);
    OA_CHECK(entries[0].value_text == "25%");
    int32_t next = strip.x;
    for (std::size_t level = 0; level < 3; ++level) {
        const kit::AutomationEntry& entry = entries[level + 1];
        OA_CHECK(entry.part && entry.kind == kit::ControlKind::levels);
        // The levels share the strip, left to right, with no gap.
        OA_CHECK(entry.rect.x == next && entry.rect.y == strip.y);
        OA_CHECK(entry.rect.height == strip.height);
        next = entry.rect.x + entry.rect.width;
        for (int32_t column = entry.rect.x; column < next; ++column)
            OA_CHECK(kit::level_at(strip, 3, 34, column) == level);
        OA_CHECK(kit::level_at(strip, 3, 34, entry.rect.x + entry.rect.width / 2) == level);
        OA_CHECK(entry.checked == (level == 1));
        OA_CHECK(entry.role == kit::AccessRole::radio);
        OA_CHECK(entry.chosen == (level == 1));
        OA_CHECK(entry.place == static_cast<int32_t>(level) + 1);
        OA_CHECK(entry.set_size == 3);
        OA_CHECK(entry.text == look.captions[level]);
        OA_CHECK(entry.enabled == (level < 2));
    }
    OA_CHECK(next == strip.x + strip.width);
}

/// A part list names each control within the kit's name rule, and a disabled
/// one stays listed.
void a_part_list_names_its_controls_and_keeps_a_disabled_one() {
    kit::DisplayList list;
    kit::Item live{};
    live.role = kit::Role::button;
    live.control = 2;
    live.rect = {0, 0, 40, 16};
    live.lines = {"Saved games"};
    list.items.push_back(live);
    kit::Item off{};
    off.role = kit::Role::button;
    off.control = 3;
    off.rect = {0, 20, 40, 16};
    off.text = "Missing";
    off.state.disabled = true;
    list.items.push_back(off);
    kit::Item again = live;
    again.control = 4;
    again.rect = {0, 40, 40, 16};
    list.items.push_back(again);
    kit::Item marks{};
    marks.role = kit::Role::button;
    marks.control = 5;
    marks.rect = {0, 60, 40, 16};
    marks.text = "...";
    list.items.push_back(marks);
    kit::list_part_controls(list);
    OA_CHECK(list.controls.size() == 4);
    const kit::Control* saved = nullptr;
    const kit::Control* saved_again = nullptr;
    const kit::Control* bare = nullptr;
    const kit::Control* missing = nullptr;
    for (const kit::Control& control : list.controls) {
        if (control.name == "saved-games")
            saved = &control;
        else if (control.name == "saved-games-2")
            saved_again = &control;
        else if (control.id == 5)
            bare = &control;
        else if (control.id == 3)
            missing = &control;
    }
    OA_CHECK(saved != nullptr && saved->label == "Saved games" && saved->text == "Saved games");
    OA_CHECK(saved_again != nullptr && saved_again->label == "Saved games");
    OA_CHECK(bare != nullptr && bare->name == "part-5" && bare->label == "...");
    OA_CHECK(missing != nullptr && missing->name == "missing" && missing->label == "Missing");
    OA_CHECK(missing != nullptr && !missing->enabled);
    OA_CHECK(kit::name_problem(list).empty());
    OA_CHECK((list.tab_order == std::vector<kit::ControlId>{2, 4, 5}));
}

/// An area that takes no press and no focus, and has words, is a label.
void a_disabled_area_with_words_is_a_label() {
    kit::DisplayList list;
    kit::Control area = placed(4, {10, 10, 200, 16});
    area.name = "library.active.base";
    area.kind = kit::ControlKind::area;
    area.enabled = false;
    area.focusable = false;
    area.text = "Total Annihilation";
    list.controls.push_back(area);
    const auto entries = kit::automation_entries(list, {});
    OA_CHECK(entries.size() == 1u);
    if (entries.size() != 1u)
        return;
    OA_CHECK(entries[0].role == kit::AccessRole::label);
    OA_CHECK(entries[0].label == "Total Annihilation");
}

/// An open drop-down's items lie where its menu shows them, and a control
/// the screen made of an item is that item, listed once.
void an_open_drop_down_lists_its_items_at_choice_item() {
    kit::DisplayList list;
    const kit::Rect field{60, 40, 120, 16};
    kit::add_choice(list, field, {"Whole map", false, true}, 2, "screen.zoom");
    list.controls.back().parts = {"automatic", "whole-map", "1-32", "1-16"};
    const kit::Rect menu = kit::choice_menu(field, 4);
    kit::Item drawn;
    drawn.role = kit::Role::choice_menu;
    drawn.rect = menu;
    kit::ChoiceMenuLook shown;
    shown.shown = {"Whole map", "1/32", "1/16"};
    shown.first = 1;
    shown.total = 4;
    shown.chosen = 1;
    drawn.look = shown;
    list.items.push_back(drawn);
    // The screen's own control for the item it shows second, tried first.
    kit::Control item = placed(90, kit::choice_item(menu, 1));
    item.name = "screen.zoom.1-32";
    item.kind = kit::ControlKind::list_item;
    list.controls.insert(list.controls.begin(), item);

    const auto entries = kit::automation_parts(list, {});
    OA_CHECK(entries.size() == 4u);
    if (entries.size() != 4u)
        return;
    OA_CHECK(entries[0].name == "screen.zoom" && !entries[0].part);
    OA_CHECK(rect_is(entries[0].rect, field.x, field.y, field.width, field.height));
    const std::vector<std::string> names{
        "screen.zoom.whole-map", "screen.zoom.1-32", "screen.zoom.1-16"
    };
    for (int32_t place = 0; place < 3; ++place) {
        const kit::AutomationEntry& entry = entries[static_cast<std::size_t>(place) + 1];
        const kit::Rect at = kit::choice_item(menu, place);
        OA_CHECK(entry.name == names[static_cast<std::size_t>(place)]);
        OA_CHECK(entry.part && entry.kind == kit::ControlKind::choice);
        OA_CHECK(rect_is(entry.rect, at.x, at.y, at.width, at.height));
        OA_CHECK(entry.shown && entry.enabled);
        OA_CHECK(entry.checked == (place == 0));
        OA_CHECK(entry.text == shown.shown[static_cast<std::size_t>(place)]);
    }
    // Closed, it has no parts, and the screen's control is listed as itself.
    kit::DisplayList closed;
    kit::add_choice(closed, field, {"Whole map", false, false}, 2, "screen.zoom");
    closed.controls.back().parts = {"automatic", "whole-map"};
    OA_CHECK(kit::automation_parts(closed, {}).size() == 1u);
}

/// A menu with a divider names the items it shows by their ids, at the
/// rectangles it draws them in, and names the divider not at all.
void a_menu_with_a_divider_lists_the_other_items() {
    kit::DisplayList list;
    const kit::Rect field{60, 40, 160, 16};
    kit::add_choice(list, field, {"English", false, true}, 2, "settings.language");
    list.controls.back().parts = {"system", "en", "7", "zh-hans"};
    const kit::Rect menu = kit::choice_menu(field, 4);
    kit::Item drawn;
    drawn.role = kit::Role::choice_menu;
    drawn.rect = menu;
    kit::ChoiceMenuLook shown;
    shown.shown = {"English", "AVAILABLE", "Simplified"};
    shown.first = 1;
    shown.total = 4;
    shown.items = {
        {.id = "system"},
        {.id = "en"},
        {.divider = true},
        {.aside = "4.0 MB", .id = "zh-hans", .choosable = false},
    };
    drawn.look = shown;
    list.items.push_back(std::move(drawn));

    const auto entries = kit::automation_parts(list, {});
    OA_CHECK(entries.size() == 3u);
    if (entries.size() != 3u)
        return;
    OA_CHECK(entries[0].name == "settings.language" && !entries[0].part);
    OA_CHECK(entries[1].name == "settings.language.en");
    OA_CHECK(entries[2].name == "settings.language.zh-hans");
    const kit::Rect english = kit::choice_item(menu, 0);
    const kit::Rect simplified = kit::choice_item(menu, 2);
    OA_CHECK(rect_is(entries[1].rect, english.x, english.y, english.width, english.height));
    OA_CHECK(
        rect_is(entries[2].rect, simplified.x, simplified.y, simplified.width, simplified.height)
    );
    OA_CHECK(entries[1].enabled && entries[1].text == "English");
    OA_CHECK(!entries[2].enabled && entries[2].text == "Simplified");
    for (const kit::AutomationEntry& entry : entries)
        OA_CHECK(entry.text != "AVAILABLE" && entry.name != "settings.language.7");
}

/// A row of buttons lists each button it draws; a control's rectangle is
/// the part of it a press reaches.
void buttons_list_each_button_and_a_clip_narrows_a_control() {
    kit::DisplayList list;
    for (int32_t at = 0; at < 3; ++at) {
        kit::Item button;
        button.role = kit::Role::button;
        button.rect = {200 + at * 60, 10, 56, 16};
        button.control = 5;
        button.text = at == 0 ? "SAVES" : at == 1 ? "SCREENSHOTS" : "MODS";
        list.items.push_back(button);
    }
    kit::Control row = placed(5, {0, 10, 400, 16});
    row.clip = {200, 0, 170, 300};
    row.name = "screen.folders";
    row.kind = kit::ControlKind::buttons;
    row.parts = {"saves", "screenshots", "mods"};
    list.controls.push_back(row);
    kit::Control below = placed(6, {0, 400, 40, 16});
    below.clip = {0, 0, 400, 300};
    below.name = "screen.below";
    list.controls.push_back(below);

    kit::Interaction interaction;
    interaction.focus_shown = true;
    interaction.focused = 5;
    const auto entries = kit::automation_parts(list, interaction);
    OA_CHECK(entries.size() == 5u);
    if (entries.size() != 5u)
        return;
    OA_CHECK(entries[0].name == "screen.folders" && entries[0].focused);
    OA_CHECK(rect_is(entries[0].rect, 200, 10, 170, 16));
    OA_CHECK(entries[1].name == "screen.folders.saves" && entries[1].text == "SAVES");
    OA_CHECK(entries[2].name == "screen.folders.screenshots" && !entries[2].focused);
    OA_CHECK(entries[3].name == "screen.folders.mods");
    OA_CHECK(rect_is(entries[1].rect, 200, 10, 56, 16));
    // The last button lies partly past the clip: it is listed where a press reaches it.
    OA_CHECK(rect_is(entries[3].rect, 320, 10, 50, 16));
    OA_CHECK(entries[1].part && entries[1].kind == kit::ControlKind::buttons);
    // Scrolled out of its view, a control is listed where it lies, not shown.
    OA_CHECK(entries[4].name == "screen.below" && !entries[4].shown);
    OA_CHECK(rect_is(entries[4].rect, 0, 400, 40, 16));
    OA_CHECK(entry_named(entries, "screen.missing") == nullptr);
}

/// The keys keep the settings dialog's values up to No, and the editing
/// keys and the information key follow it.
void the_keys_keep_their_values() {
    OA_CHECK(static_cast<int>(kit::Key::enter) == 0);
    OA_CHECK(static_cast<int>(kit::Key::escape) == 1);
    OA_CHECK(static_cast<int>(kit::Key::up) == 2);
    OA_CHECK(static_cast<int>(kit::Key::down) == 3);
    OA_CHECK(static_cast<int>(kit::Key::left) == 4);
    OA_CHECK(static_cast<int>(kit::Key::right) == 5);
    OA_CHECK(static_cast<int>(kit::Key::space) == 6);
    OA_CHECK(static_cast<int>(kit::Key::tab) == 7);
    OA_CHECK(static_cast<int>(kit::Key::back_tab) == 8);
    OA_CHECK(static_cast<int>(kit::Key::page_up) == 9);
    OA_CHECK(static_cast<int>(kit::Key::page_down) == 10);
    OA_CHECK(static_cast<int>(kit::Key::home) == 11);
    OA_CHECK(static_cast<int>(kit::Key::end) == 12);
    OA_CHECK(static_cast<int>(kit::Key::yes) == 13);
    OA_CHECK(static_cast<int>(kit::Key::no) == 14);
    OA_CHECK(static_cast<int>(kit::Key::backspace) == 15);
    OA_CHECK(static_cast<int>(kit::Key::delete_forward) == 16);
    OA_CHECK(static_cast<int>(kit::Key::info) == 17);
}

/// Tells whether a field holds a text with its caret at a byte.
///
/// @param field the field
/// @param text the text it should hold
/// @param caret where its caret should stand, in bytes
/// @return true when both match
bool holds(const kit::TextField& field, const std::string& text, std::size_t caret) {
    return field.text == text && field.caret == caret;
}

/// Typed text goes in at the caret, which moves past it.
void typing_inserts_at_the_caret() {
    kit::TextField field;
    OA_CHECK(kit::insert_text(field, "ridge"));
    OA_CHECK(holds(field, "ridge", 5));
    field.caret = 0;
    OA_CHECK(kit::insert_text(field, "the "));
    OA_CHECK(holds(field, "the ridge", 4));
    field.caret = field.text.size();
    // e acute, two bytes; a Chinese character, three; a face, four.
    OA_CHECK(kit::insert_text(field, "\xc3\xa9"));
    OA_CHECK(kit::insert_text(field, "\xe4\xb8\xad"));
    OA_CHECK(kit::insert_text(field, "\xf0\x9f\x98\x80"));
    OA_CHECK(holds(field, "the ridge\xc3\xa9\xe4\xb8\xad\xf0\x9f\x98\x80", 18));
    // Nothing to insert changes nothing.
    OA_CHECK(!kit::insert_text(field, ""));
    OA_CHECK(holds(field, "the ridge\xc3\xa9\xe4\xb8\xad\xf0\x9f\x98\x80", 18));
}

/// A field refuses ill-formed UTF-8 and control characters whole.
void typing_refuses_what_is_not_text() {
    const std::string refused[] = {
        std::string("a\x01", 2), // a C0 control character
        "a\nb",                  // a new line
        "\t",                    // a tab
        "\x7f",                  // DEL
        std::string(1, '\0'),    // NUL
        "\xc2\x85",              // U+0085, a C1 control character
        "\xc2\x9f",              // U+009F, the last of them
        "\xff",                  // a byte that starts nothing
        "\x80",                  // a continuation byte alone
        "ab\xe4\xb8",            // a cut sequence
        "\xc0\xaf",              // an overlong slash
        "\xe0\x80\xaf",          // another
        "\xed\xa0\x80",          // a surrogate
        "\xf4\x90\x80\x80",      // past U+10FFFF
    };
    for (const std::string& text : refused) {
        kit::TextField field{"ridge", 2};
        OA_CHECK(!kit::insert_text(field, text));
        OA_CHECK(holds(field, "ridge", 2));
    }
    // U+00A0, the first character after the C1 controls, is taken.
    kit::TextField field{"ab", 1};
    OA_CHECK(kit::insert_text(field, "\xc2\xa0"));
    OA_CHECK(holds(
        field,
        "a\xc2\xa0"
        "b",
        3
    ));
}

/// A caret inside a character, or past the text, moves back to a boundary first.
void a_stray_caret_settles_on_a_boundary() {
    kit::TextField inside{"\xe4\xb8\xad", 2};
    OA_CHECK(kit::insert_text(inside, "a"));
    OA_CHECK(holds(inside, "a\xe4\xb8\xad", 1));

    kit::TextField past{"ab", 9};
    OA_CHECK(kit::insert_text(past, "c"));
    OA_CHECK(holds(past, "abc", 3));

    kit::TextField moved{"x\xe4\xb8\xad", 3};
    OA_CHECK(kit::edit_text(moved, kit::Key::space));
    OA_CHECK(holds(moved, "x\xe4\xb8\xad", 1));
    kit::TextField deleted{"x\xe4\xb8\xady", 2};
    OA_CHECK(kit::edit_text(deleted, kit::Key::delete_forward));
    OA_CHECK(holds(deleted, "xy", 1));
}

/// The editing keys take whole characters and move the caret by them.
void editing_takes_whole_characters() {
    // a, a Chinese character (3 bytes), a face (4 bytes), b.
    const std::string text = "a\xe4\xb8\xad\xf0\x9f\x98\x80"
                             "b";
    kit::TextField field{text, 0};
    OA_CHECK(kit::edit_text(field, kit::Key::right));
    OA_CHECK(field.caret == 1);
    OA_CHECK(kit::edit_text(field, kit::Key::right));
    OA_CHECK(field.caret == 4);
    OA_CHECK(kit::edit_text(field, kit::Key::right));
    OA_CHECK(field.caret == 8);
    OA_CHECK(kit::edit_text(field, kit::Key::right));
    OA_CHECK(field.caret == 9);
    OA_CHECK(!kit::edit_text(field, kit::Key::right));
    OA_CHECK(field.caret == 9);
    OA_CHECK(kit::edit_text(field, kit::Key::left));
    OA_CHECK(field.caret == 8);
    OA_CHECK(kit::edit_text(field, kit::Key::left));
    OA_CHECK(field.caret == 4);
    OA_CHECK(kit::edit_text(field, kit::Key::home));
    OA_CHECK(field.caret == 0);
    OA_CHECK(!kit::edit_text(field, kit::Key::left));
    OA_CHECK(!kit::edit_text(field, kit::Key::home));
    OA_CHECK(kit::edit_text(field, kit::Key::end));
    OA_CHECK(field.caret == 9);
    OA_CHECK(!kit::edit_text(field, kit::Key::end));

    OA_CHECK(!kit::edit_text(field, kit::Key::delete_forward));
    OA_CHECK(kit::edit_text(field, kit::Key::left));
    OA_CHECK(kit::edit_text(field, kit::Key::backspace));
    OA_CHECK(holds(
        field,
        "a\xe4\xb8\xad"
        "b",
        4
    ));
    OA_CHECK(kit::edit_text(field, kit::Key::backspace));
    OA_CHECK(holds(field, "ab", 1));
    OA_CHECK(kit::edit_text(field, kit::Key::home));
    OA_CHECK(!kit::edit_text(field, kit::Key::backspace));
    OA_CHECK(kit::edit_text(field, kit::Key::delete_forward));
    OA_CHECK(holds(field, "b", 0));
    OA_CHECK(kit::edit_text(field, kit::Key::delete_forward));
    OA_CHECK(holds(field, "", 0));
    OA_CHECK(!kit::edit_text(field, kit::Key::delete_forward));
    OA_CHECK(!kit::edit_text(field, kit::Key::backspace));

    // Keys that do not edit leave the field as it is.
    kit::TextField other{"ab", 1};
    for (const kit::Key pressed :
         {kit::Key::up,
          kit::Key::down,
          kit::Key::enter,
          kit::Key::escape,
          kit::Key::tab,
          kit::Key::page_up,
          kit::Key::yes,
          kit::Key::no,
          kit::Key::info}) {
        OA_CHECK(!kit::edit_text(other, pressed));
        OA_CHECK(holds(other, "ab", 1));
    }
}

/// The editing keys, Home and End go to a focused text field; Enter goes to
/// a focused tab.
void a_text_field_and_a_tab_take_their_keys() {
    kit::DisplayList list;
    kit::Control field = placed(1, {0, 0, 100, 16});
    field.kind = kit::ControlKind::text_field;
    field.steps = true;
    list.controls.push_back(field);
    kit::Control button = placed(2, {0, 20, 40, 16});
    button.kind = kit::ControlKind::button;
    list.controls.push_back(button);
    kit::Control tab = placed(3, {0, 40, 40, 18});
    tab.kind = kit::ControlKind::tab;
    list.controls.push_back(tab);
    list.tab_order = {1, 2, 3};

    kit::Interaction interaction;
    interaction.focus_shown = true;
    interaction.focused = 1;
    for (const kit::Key pressed :
         {kit::Key::left,
          kit::Key::right,
          kit::Key::home,
          kit::Key::end,
          kit::Key::backspace,
          kit::Key::delete_forward,
          kit::Key::space}) {
        const kit::KeyOutcome outcome = kit::key(interaction, list, pressed);
        OA_CHECK(outcome.result == kit::KeyResult::to_control);
        OA_CHECK(outcome.control == 1 && outcome.key == pressed);
        OA_CHECK(interaction.focused == 1);
    }
    // Enter accepts, and the field keeps its ring and its editing keys.
    OA_CHECK(kit::key(interaction, list, kit::Key::enter).result == kit::KeyResult::accept);
    OA_CHECK(interaction.focus_shown && interaction.focused == 1);
    const kit::KeyOutcome erased = kit::key(interaction, list, kit::Key::backspace);
    OA_CHECK(erased.result == kit::KeyResult::to_control && erased.control == 1);
    OA_CHECK(kit::key(interaction, list, kit::Key::page_down).result == kit::KeyResult::scroll);
    const kit::KeyOutcome down = kit::key(interaction, list, kit::Key::down);
    OA_CHECK(down.result == kit::KeyResult::redraw && down.control == 2);

    // On a button, Home and End scroll and the editing keys do nothing.
    OA_CHECK(kit::key(interaction, list, kit::Key::home).result == kit::KeyResult::scroll);
    OA_CHECK(kit::key(interaction, list, kit::Key::end).result == kit::KeyResult::scroll);
    const kit::KeyOutcome back = kit::key(interaction, list, kit::Key::backspace);
    OA_CHECK(back.result == kit::KeyResult::none && back.control == kit::no_control);
    OA_CHECK(kit::key(interaction, list, kit::Key::delete_forward).result == kit::KeyResult::none);
    OA_CHECK(interaction.focused == 2);

    // A tab takes Enter and Space; Left and Right move the focus instead.
    interaction.focused = 3;
    interaction.focus_shown = true;
    const kit::KeyOutcome entered = kit::key(interaction, list, kit::Key::enter);
    OA_CHECK(entered.result == kit::KeyResult::to_control && entered.control == 3);
    OA_CHECK(!interaction.focus_shown);
    interaction.focus_shown = true;
    OA_CHECK(kit::key(interaction, list, kit::Key::space).result == kit::KeyResult::to_control);
    OA_CHECK(kit::key(interaction, list, kit::Key::right).result == kit::KeyResult::none);

    // With no focus shown, the editing keys do nothing and show nothing.
    kit::Interaction fresh;
    OA_CHECK(kit::key(fresh, list, kit::Key::backspace).result == kit::KeyResult::none);
    OA_CHECK(!fresh.focus_shown);
}

/// A pointer move keeps a focused text field's ring. On every other kind of
/// control it hides the ring and leaves the focus, so the next key still
/// acts on that control.
void a_pointer_move_keeps_a_text_fields_ring() {
    kit::DisplayList list;
    kit::Control field = placed(1, {0, 0, 80, 16});
    field.kind = kit::ControlKind::text_field;
    list.controls.push_back(field);
    kit::Control button = placed(2, {0, 24, 40, 16});
    button.kind = kit::ControlKind::button;
    list.controls.push_back(button);
    list.tab_order = {1, 2};
    list.items.push_back(kit::Item{});
    list.items.back().control = 1;

    kit::Interaction on_field;
    on_field.focused = 1;
    on_field.focus_shown = true;
    const kit::PointerOutcome over = kit::pointer_move(on_field, list, {10, 8});
    OA_CHECK(over.result == kit::PointerResult::redraw && over.control == 1);
    OA_CHECK(on_field.focused == 1 && on_field.focus_shown);
    const kit::PointerOutcome away = kit::pointer_move(on_field, list, {10, 30});
    OA_CHECK(away.result == kit::PointerResult::redraw && away.control == 2);
    OA_CHECK(on_field.hovered == 2 && on_field.focused == 1 && on_field.focus_shown);
    kit::mark_states(list, on_field);
    OA_CHECK(list.items[0].state.focused);

    kit::Interaction on_button;
    on_button.focused = 2;
    on_button.focus_shown = true;
    std::ignore = kit::pointer_move(on_button, list, {10, 8});
    OA_CHECK(on_button.focused == 2 && on_button.focus_shown && on_button.pointer_hid_ring);
    OA_CHECK(!kit::focus_ring_visible(on_button));
}

/// A screen opens on its first control with the ring hidden, and each key does as key says.
void the_open_focus_follows_the_key_table() {
    kit::DisplayList list;
    list.controls.push_back(placed(1, {0, 0, 30, 16}));
    kit::Control slider = placed(2, {0, 20, 30, 16});
    slider.kind = kit::ControlKind::slider;
    slider.steps = true;
    list.controls.push_back(slider);
    list.controls.push_back(placed(3, {50, 20, 30, 16}));
    kit::Control link = placed(4, {0, 40, 30, 16});
    link.kind = kit::ControlKind::link;
    list.controls.push_back(link);
    kit::Control item = placed(5, {0, 60, 30, 16});
    item.kind = kit::ControlKind::list_item;
    list.controls.push_back(item);
    list.tab_order = {1, 2, 3, 4, 5};

    kit::Interaction opened;
    kit::focus_opened(opened, list, false);
    OA_CHECK(opened.focused == 1 && !opened.focus_shown);
    kit::focus_opened(opened, list, true);
    OA_CHECK(opened.focused == 1 && !opened.focus_shown);
    kit::Interaction asked;
    kit::focus_opened(asked, list, true);
    OA_CHECK(asked.focused == 1 && asked.focus_shown);
    kit::Interaction empty;
    kit::focus_opened(empty, kit::DisplayList{}, false);
    OA_CHECK(empty.focused == kit::no_control && !empty.focus_shown);

    const auto hidden = [&](kit::Key key) {
        kit::Interaction interaction;
        interaction.focused = 1;
        return kit::key(interaction, list, key);
    };
    const kit::KeyOutcome tab = hidden(kit::Key::tab);
    OA_CHECK(tab.result == kit::KeyResult::redraw && tab.control == 2);
    kit::Interaction after_tab;
    after_tab.focused = 1;
    OA_CHECK(kit::key(after_tab, list, kit::Key::tab).result == kit::KeyResult::redraw);
    OA_CHECK(after_tab.focused == 2 && after_tab.focus_shown);
    const kit::KeyOutcome back = hidden(kit::Key::back_tab);
    OA_CHECK(back.result == kit::KeyResult::redraw && back.control == 5);
    const kit::KeyOutcome up = hidden(kit::Key::up);
    OA_CHECK(up.result == kit::KeyResult::redraw && up.control == 5);
    for (const kit::Key key : {kit::Key::down, kit::Key::left, kit::Key::right, kit::Key::space}) {
        const kit::KeyOutcome shown = hidden(key);
        OA_CHECK(shown.result == kit::KeyResult::redraw && shown.control == 1);
    }
    kit::Interaction resting;
    resting.focused = 1;
    const kit::KeyOutcome accepted = kit::key(resting, list, kit::Key::enter);
    OA_CHECK(accepted.result == kit::KeyResult::accept && accepted.control == kit::no_control);
    OA_CHECK(resting.focused == 1 && !resting.focus_shown);
    OA_CHECK(kit::key(resting, list, kit::Key::escape).result == kit::KeyResult::cancel);
    OA_CHECK(kit::key(resting, list, kit::Key::page_down).result == kit::KeyResult::scroll);
    OA_CHECK(kit::key(resting, list, kit::Key::page_up).result == kit::KeyResult::scroll);
    OA_CHECK(kit::key(resting, list, kit::Key::home).result == kit::KeyResult::scroll);
    OA_CHECK(kit::key(resting, list, kit::Key::end).result == kit::KeyResult::scroll);
    OA_CHECK(kit::key(resting, list, kit::Key::yes).result == kit::KeyResult::none);
    OA_CHECK(kit::key(resting, list, kit::Key::no).result == kit::KeyResult::none);
    OA_CHECK(kit::key(resting, list, kit::Key::info).result == kit::KeyResult::none);
    OA_CHECK(kit::key(resting, list, kit::Key::backspace).result == kit::KeyResult::none);
    OA_CHECK(!resting.focus_shown && resting.focused == 1);

    // One control: Tab shows the ring on it.
    kit::DisplayList only;
    only.controls.push_back(placed(1, {0, 0, 30, 16}));
    only.tab_order = {1};
    kit::Interaction single;
    single.focused = 1;
    const kit::KeyOutcome only_tab = kit::key(single, only, kit::Key::tab);
    OA_CHECK(only_tab.result == kit::KeyResult::redraw && only_tab.control == 1);
    OA_CHECK(single.focus_shown);

    // The ring is showing. Enter presses a button and hides the ring.
    kit::Interaction shown;
    shown.focused = 1;
    shown.focus_shown = true;
    const kit::KeyOutcome pressed = kit::key(shown, list, kit::Key::enter);
    OA_CHECK(pressed.result == kit::KeyResult::to_control && pressed.control == 1);
    OA_CHECK(shown.focused == 1 && !shown.focus_shown);
    shown.focus_shown = true;
    const kit::KeyOutcome spaced = kit::key(shown, list, kit::Key::space);
    OA_CHECK(spaced.result == kit::KeyResult::to_control && spaced.control == 1);
    OA_CHECK(shown.focus_shown);
    OA_CHECK(kit::key(shown, list, kit::Key::tab).control == 2);
    OA_CHECK(shown.focus_shown && shown.focused == 2);
    OA_CHECK(kit::key(shown, list, kit::Key::back_tab).control == 1);
    // Down from the button hits the slider directly under it.
    OA_CHECK(kit::key(shown, list, kit::Key::down).control == 2);
    OA_CHECK(shown.focus_shown);
    shown.focused = 2;
    const kit::KeyOutcome stepped = kit::key(shown, list, kit::Key::right);
    OA_CHECK(stepped.result == kit::KeyResult::to_control && stepped.control == 2);
    OA_CHECK(shown.focus_shown);
    const kit::KeyOutcome slider_enter = kit::key(shown, list, kit::Key::enter);
    OA_CHECK(slider_enter.result == kit::KeyResult::accept);
    OA_CHECK(shown.focused == 2 && !shown.focus_shown);
    shown.focus_shown = true;
    OA_CHECK(kit::key(shown, list, kit::Key::escape).result == kit::KeyResult::cancel);
    OA_CHECK(shown.focus_shown && shown.focused == 2);
    OA_CHECK(kit::key(shown, list, kit::Key::page_up).result == kit::KeyResult::scroll);
    OA_CHECK(kit::key(shown, list, kit::Key::left).result == kit::KeyResult::to_control);

    // A pointer move hides the ring and leaves the focused control. The keys
    // still act on it: the hidden-ring table is only for an opening focus.
    kit::Interaction moving;
    moving.focused = 1;
    moving.focus_shown = true;
    const kit::PointerOutcome moved = kit::pointer_move(moving, list, {10, 8});
    OA_CHECK(moved.result == kit::PointerResult::redraw && moved.control == 1);
    OA_CHECK(moving.hovered == 1 && moving.focused == 1 && moving.focus_shown);
    OA_CHECK(moving.pointer_hid_ring && !kit::focus_ring_visible(moving));
    list.controls[0].name = "screen.first";
    const std::vector<kit::AutomationEntry> entries = kit::automation_entries(list, moving);
    OA_CHECK(!entries.empty() && entries[0].focused);

    kit::DisplayList marked;
    marked.items.push_back(kit::Item{});
    marked.items.back().control = 1;
    kit::mark_states(marked, moving);
    OA_CHECK(!marked.items[0].state.focused);
}

/// A click focuses a control. A pointer move hides the ring, and Return,
/// Down and Space still act on that control. The ring shows again on the
/// key. An opening focus is left for the hidden-ring table.
void a_click_then_a_move_keeps_the_keys() {
    kit::DisplayList list;
    list.controls.push_back(placed(1, {0, 0, 40, 16}));
    list.controls.push_back(placed(2, {0, 24, 40, 16}));
    list.controls.push_back(placed(3, {0, 48, 40, 16}));
    list.tab_order = {1, 2, 3};
    list.items.push_back(kit::Item{});
    list.items.back().control = 2;

    // A key has shown the ring, so a press moves the focus, as a click does
    // once the ring shows.
    kit::Interaction interaction;
    interaction.focus_shown = true;
    interaction.focused = 1;
    OA_CHECK(kit::pointer_down(interaction, list, {10, 30}).control == 2);
    const kit::PointerOutcome released = kit::pointer_up(interaction, list, {10, 30});
    OA_CHECK(released.result == kit::PointerResult::activated && released.control == 2);
    OA_CHECK(interaction.focused == 2 && interaction.focus_shown && !interaction.pointer_hid_ring);

    std::ignore = kit::pointer_move(interaction, list, {10, 4});
    OA_CHECK(interaction.focused == 2 && interaction.focus_shown && interaction.pointer_hid_ring);
    OA_CHECK(!kit::focus_ring_visible(interaction));
    kit::mark_states(list, interaction);
    OA_CHECK(!list.items[0].state.focused);

    const kit::KeyOutcome entered = kit::key(interaction, list, kit::Key::enter);
    OA_CHECK(entered.result == kit::KeyResult::to_control && entered.control == 2);
    OA_CHECK(interaction.focused == 2 && !interaction.focus_shown);

    interaction.focus_shown = true;
    interaction.focused = 2;
    interaction.pointer_hid_ring = false;
    std::ignore = kit::pointer_move(interaction, list, {10, 4});
    const kit::KeyOutcome down = kit::key(interaction, list, kit::Key::down);
    OA_CHECK(down.result == kit::KeyResult::redraw && down.control == 3);
    OA_CHECK(interaction.focused == 3 && interaction.focus_shown && !interaction.pointer_hid_ring);

    interaction.focused = 2;
    interaction.focus_shown = true;
    interaction.pointer_hid_ring = false;
    std::ignore = kit::pointer_move(interaction, list, {10, 4});
    const kit::KeyOutcome spaced = kit::key(interaction, list, kit::Key::space);
    OA_CHECK(spaced.result == kit::KeyResult::to_control && spaced.control == 2);
    OA_CHECK(interaction.focused == 2 && interaction.focus_shown && !interaction.pointer_hid_ring);

    // The opening focus has not been moved. A pointer move does not hide a
    // ring that is not drawn, and Down still shows the first control.
    kit::Interaction opening;
    opening.focused = 1;
    std::ignore = kit::pointer_move(opening, list, {10, 8});
    OA_CHECK(!opening.focus_shown && !opening.pointer_hid_ring && opening.focused == 1);
    const kit::KeyOutcome first = kit::key(opening, list, kit::Key::down);
    OA_CHECK(first.result == kit::KeyResult::redraw && first.control == 1);
    OA_CHECK(opening.focus_shown && !opening.pointer_hid_ring);
}

} // namespace

int main() {
    a_finger_takes_the_nearer_of_two_buttons();
    equal_distances_take_the_first_button();
    a_disabled_control_can_block_a_finger();
    a_finger_out_of_reach_takes_nothing();
    a_covered_point_is_passed_over();
    a_finger_on_a_control_lands_unshifted();
    a_reach_may_be_a_fraction();
    a_clip_hides_the_part_a_press_cannot_reach();
    the_notice_buttons_take_the_notices_finger();
    the_pointer_hovers_presses_and_releases();
    the_focus_follows_a_press_only_after_a_key();
    a_fingers_shift_stays_through_the_move_and_release();
    tab_wraps_both_ways_and_from_no_focus();
    the_arrows_step_a_column_one_row_at_a_time();
    up_and_down_do_not_follow_tab_order();
    right_from_a_row_reaches_the_details_beside_it();
    a_grid_moves_to_the_neighbour_in_line();
    the_beam_beats_a_nearer_diagonal();
    down_from_the_last_row_reaches_the_footer_under_it();
    a_scroll_area_is_searched_before_what_is_outside_it();
    the_rows_laid_out_whole_reach_past_the_view();
    controls_in_no_area_search_every_control_at_once();
    right_reaches_the_button_inside_a_row();
    an_earlier_place_in_tab_order_wins_a_tie();
    keys_with_no_focus_show_an_end_of_the_order();
    the_open_focus_follows_the_key_table();
    a_click_then_a_move_keeps_the_keys();
    keys_on_a_stepping_control_stay_with_it();
    keys_on_a_button_a_link_and_a_list_row();
    the_wheel_keeps_the_dialogs_fractions();
    a_name_must_be_present_well_formed_and_unique();
    automation_reads_the_named_controls();
    a_switch_lists_its_two_halves();
    a_strip_lists_its_levels_where_level_at_finds_them();
    a_part_list_names_its_controls_and_keeps_a_disabled_one();
    a_disabled_area_with_words_is_a_label();
    an_open_drop_down_lists_its_items_at_choice_item();
    a_menu_with_a_divider_lists_the_other_items();
    buttons_list_each_button_and_a_clip_narrows_a_control();
    the_keys_keep_their_values();
    typing_inserts_at_the_caret();
    typing_refuses_what_is_not_text();
    a_stray_caret_settles_on_a_boundary();
    editing_takes_whole_characters();
    a_text_field_and_a_tab_take_their_keys();
    a_pointer_move_keeps_a_text_fields_ring();
    the_states_show_the_focus_and_the_press();
    return oa::test::check_exit_status();
}

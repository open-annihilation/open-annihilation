// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Pointer and key input on the photo panel.

#include "oa/ui/photo_panel/input.hpp"

#include "oa/ui/kit/components.hpp"

#include <algorithm>
#include <cstddef>
#include <span>
#include <variant>

namespace oa::ui::photo_panel {
namespace {

/// A point on the panel, the chips, the alerts, the key line, the hint or the toast.
bool on_chrome(const Layout& placed, kit::Point at) {
    const kit::Rect* bands[] = {
        &placed.panel, &placed.chips, &placed.alerts, &placed.keyline, &placed.hint, &placed.toast
    };
    for (const kit::Rect* band : bands) {
        if (kit::contains(*band, at))
            return true;
    }
    for (const kit::Rect& chip : placed.chip_box) {
        if (kit::contains(chip, at))
            return true;
    }
    return false;
}

/// The command a control asks for. Sliders carry their stop in value.
Press command_for(kit::ControlId id, const Model& model, int32_t value) {
    Press press;
    press.value = value;
    switch (id) {
    case kHead:
        press.command = Command::leave;
        break;
    case kCamera:
        press.command = Command::toggle_section;
        press.value = kSectionCamera;
        break;
    case kZoom:
        press.command = Command::set_zoom;
        break;
    case kTurn:
        press.command = Command::set_turn;
        break;
    case kTilt:
        press.command = Command::set_tilt;
        break;
    case kClassic:
        press.command = Command::classic_view;
        break;
    case kFollow:
        press.command = Command::follow_unit;
        break;
    case kTime:
        press.command = Command::toggle_section;
        press.value = kSectionTime;
        break;
    case kRun:
        // 0 runs, 1 slows, 2 freezes. Slow is refused while the match is shared.
        if (value <= 0) {
            press.command = Command::run;
            press.value = 0;
        } else if (value == 1) {
            if (!model.slow_lock.empty())
                return {};
            press.command = Command::slow;
            press.value = model.speed_stop + 1;
        } else {
            press.command = Command::freeze;
            press.value = 0;
        }
        break;
    case kSlow:
        press.command = Command::slow;
        press.value = model.speed_stop + 1;
        break;
    case kFreeze:
        press.command = Command::freeze;
        break;
    case kSpeed:
        press.command = Command::set_speed;
        break;
    case kStep:
        press.command = Command::step;
        break;
    case kBetween:
        press.command = Command::between;
        break;
    case kLook:
        press.command = Command::toggle_section;
        press.value = kSectionLook;
        break;
    case kSelection:
        press.command = Command::toggle_selection;
        break;
    case kHealth:
        press.command = Command::toggle_health;
        break;
    case kGuides:
        press.command = Command::cycle_guides;
        break;
    case kPreset:
        press.command = Command::set_preset;
        press.value = std::clamp(value, int32_t{0}, int32_t{5});
        break;
    case kVignette:
        press.command = Command::set_vignette;
        break;
    case kRoundness:
        press.command = Command::set_roundness;
        break;
    case kGrain:
        press.command = Command::set_grain;
        break;
    case kGrainSize:
        press.command = Command::set_grain_size;
        break;
    case kFrame:
        press.command = Command::cycle_frame;
        break;
    case kLookRate:
        break;
    case kCapture:
        press.command = Command::toggle_section;
        press.value = kSectionCapture;
        break;
    case kCopySwitch:
        press.command = Command::toggle_copy;
        break;
    case kNames:
        press.command = Command::toggle_names;
        break;
    case kTake:
        press.command = Command::take_photo;
        break;
    case kCopy:
        press.command = Command::copy_photo;
        break;
    case kHint:
        press.command = Command::show_panel;
        break;
    case kSize: {
        const int32_t index = std::clamp(value, int32_t{0}, int32_t{3});
        if (!model.size_locks[static_cast<std::size_t>(index)].empty())
            return {};
        press.command = Command::set_size;
        press.value = model.size_factors[static_cast<std::size_t>(index)];
        break;
    }
    case kSmooth: {
        const int32_t index = std::clamp(value, int32_t{0}, int32_t{2});
        if (!model.smooth_locks[static_cast<std::size_t>(index)].empty())
            return {};
        press.command = Command::set_smooth;
        press.value = model.smooth_factors[static_cast<std::size_t>(index)];
        break;
    }
    case kClock:
    case kFile:
        break;
    default:
        break;
    }
    return press;
}

/// The lock that keeps a control from changing the match. Empty when it moves.
const std::string& lock_of(const Model& model, kit::ControlId id) {
    static const std::string none;
    switch (id) {
    case kZoom:
        return model.zoom_lock;
    case kTurn:
        return model.turn_lock;
    case kTilt:
        return model.tilt_lock;
    case kSlow:
        return model.slow_lock;
    case kSpeed:
        return model.speed_lock;
    case kStep:
        return model.step_lock;
    case kBetween:
        return model.between_lock;
    default:
        return none;
    }
}

/// A slider's stop from a point along its track: the nearest stop, so the
/// last is reached before the track's last point.
int32_t stop_at(const kit::Control& control, int32_t stops, kit::Point at) {
    const int32_t last = std::max(stops - 1, int32_t{0});
    const int32_t span = std::max(control.rect.width - 1, int32_t{1});
    const int32_t along = at.x - control.rect.x;
    if (last <= 0)
        return 0;
    return std::clamp((along * last + span / 2) / span, int32_t{0}, last);
}

/// How many stops a slider has.
int32_t stops_of(const Model& model, kit::ControlId id) {
    switch (id) {
    case kZoom:
        return model.zoom_stops;
    case kTurn:
        return model.turn_stops;
    case kTilt:
        return model.tilt_stops;
    case kSpeed:
        return model.speed_stops;
    case kBetween:
        return model.between_stops;
    case kVignette:
        return model.vignette_stops;
    case kRoundness:
        return model.roundness_stops;
    case kGrain:
        return model.grain_stops;
    case kGrainSize:
        return model.grain_size_stops;
    default:
        return 2;
    }
}

/// The stop a slider is on.
int32_t stop_of(const Model& model, kit::ControlId id) {
    switch (id) {
    case kZoom:
        return model.zoom_stop;
    case kTurn:
        return model.turn_stop;
    case kTilt:
        return model.tilt_stop;
    case kSpeed:
        return model.speed_stop;
    case kBetween:
        return model.between_stop;
    case kVignette:
        return model.vignette_stop;
    case kRoundness:
        return model.roundness_stop;
    case kGrain:
        return model.grain_stop;
    case kGrainSize:
        return model.grain_size_stop;
    default:
        return 0;
    }
}

/// A press of a control, dropped when the control is locked.
Press press_of(const Model& model, kit::ControlId id, int32_t value) {
    if (!lock_of(model, id).empty())
        return {};
    return command_for(id, model, value);
}

/// How many levels a strip holds. The time strip and Smooth edges have
/// three, Size four and a look six.
int32_t level_count(const kit::Control& control) {
    const std::size_t count = control.parts.empty() ? std::size_t{3} : control.parts.size();
    return static_cast<int32_t>(count);
}

/// The level a strip is on: the look's preset, the photo's size, its smooth
/// edges, or Run / Slow / Freeze.
int32_t chosen_level(const Model& model, kit::ControlId id, int32_t count) {
    const int32_t last = std::max(count - 1, int32_t{0});
    if (id == kPreset)
        return std::clamp(model.preset, int32_t{0}, last);
    if (id == kSize)
        return std::clamp(model.size_index, int32_t{0}, last);
    if (id == kSmooth)
        return std::clamp(model.smooth_index, int32_t{0}, last);
    return std::clamp(model.time_mode, int32_t{0}, last);
}

/// True when that stop of Size or Smooth edges is locked. Time mode's Slow
/// lock is handled with the time strip, not here.
bool level_locked(const Model& model, kit::ControlId id, int32_t index) {
    if (id == kSize)
        return index >= 0 && index < 4 &&
               !model.size_locks[static_cast<std::size_t>(index)].empty();
    if (id == kSmooth)
        return index >= 0 && index < 3 &&
               !model.smooth_locks[static_cast<std::size_t>(index)].empty();
    return false;
}

/// The level under a point. A wrapped strip uses its rows. One row uses the
/// width its levels are drawn at, not always a third.
int32_t level_of(const kit::DisplayList& list, const kit::Control& control, kit::Point at) {
    const int32_t count = std::max(level_count(control), int32_t{1});
    const kit::LevelsLook* levels = nullptr;
    for (const kit::Item& item : list.items) {
        if (item.control != control.id || item.role != kit::Role::levels)
            continue;
        levels = std::get_if<kit::LevelsLook>(&item.look);
        break;
    }
    if (levels != nullptr && levels->columns > 0 &&
        static_cast<std::size_t>(levels->columns) < static_cast<std::size_t>(count)) {
        const int32_t columns = levels->columns;
        const int32_t rows = (count + columns - 1) / columns;
        const int32_t gap = std::max(levels->row_gap, int32_t{0});
        const int32_t span = control.rect.height - gap * (rows - 1);
        const int32_t band_h = rows > 0 ? span / std::max(rows, int32_t{1}) : control.rect.height;
        const int32_t stride = std::max(band_h + gap, int32_t{1});
        int32_t band = (at.y - control.rect.y) / stride;
        if (band < 0)
            band = 0;
        if (band >= rows)
            band = rows - 1;
        const int32_t chip_w = std::max(levels->level_width, int32_t{1});
        int32_t place = (at.x - control.rect.x - 1) / chip_w;
        if (place < 0)
            place = 0;
        if (place >= columns)
            place = columns - 1;
        int32_t index = band * columns + place;
        if (index >= count)
            index = count - 1;
        if (index < 0)
            index = 0;
        return index;
    }
    const int32_t level_w = levels != nullptr && levels->level_width > 0
                                ? levels->level_width
                                : std::max((control.rect.width - 2) / count, int32_t{1});
    return static_cast<int32_t>(
        kit::level_at(control.rect, static_cast<std::size_t>(count), level_w, at.x)
    );
}

/// A press or a leave hides the photo notice. Moving the focus does not.
void note_panel_press(Answer& answer) {
    if (answer.press.command != Command::none || answer.leave)
        answer.dismiss_toast = true;
}

/// Tells whether a key moves the focus.
bool arrow_key(kit::Key key) noexcept {
    return key == kit::Key::up || key == kit::Key::down || key == kit::Key::left ||
           key == kit::Key::right;
}

/// The way an arrow key moves the focus.
kit::Direction direction_of(kit::Key key) noexcept {
    if (key == kit::Key::down)
        return kit::Direction::down;
    if (key == kit::Key::left)
        return kit::Direction::left;
    if (key == kit::Key::right)
        return kit::Direction::right;
    return kit::Direction::up;
}

} // namespace

Answer pointer_event(
    kit::Interaction& interaction,
    const Model& model,
    const Layout& placed,
    oa::app::ScreenInputKind kind,
    kit::Point at,
    int32_t finger_reach
) {
    Answer answer;
    const bool chrome = on_chrome(placed, at) || kit::hit(placed.list, at) != kit::no_control;
    if (kind == oa::app::ScreenInputKind::pointer_move) {
        const kit::PointerOutcome moved = kit::pointer_move(interaction, placed.list, at);
        answer.consumed = chrome;
        answer.redraw = moved.result != kit::PointerResult::none;
        return answer;
    }
    if (kind == oa::app::ScreenInputKind::pointer_down) {
        if (finger_reach > 0) {
            const kit::PointerOutcome held =
                kit::finger_down(interaction, placed.list, at, finger_reach);
            if (!chrome && interaction.pressed == kit::no_control)
                return answer;
            answer.consumed = true;
            answer.redraw = held.result != kit::PointerResult::none;
            return answer;
        }
        if (!chrome)
            return answer;
        const kit::PointerOutcome held = kit::pointer_down(interaction, placed.list, at);
        answer.consumed = true;
        answer.redraw = held.result != kit::PointerResult::none;
        return answer;
    }
    if (kind == oa::app::ScreenInputKind::pointer_up) {
        if (interaction.pressed == kit::no_control && !chrome)
            return answer;
        const kit::PointerOutcome released = kit::pointer_up(interaction, placed.list, at);
        answer.consumed = true;
        answer.redraw = released.result != kit::PointerResult::none;
        // A release on the panel, the toast or the other chrome hides the notice.
        answer.dismiss_toast = true;
        if (released.result != kit::PointerResult::activated)
            return answer;
        const kit::Control* control = kit::control_of(placed.list, released.control);
        if (control == nullptr)
            return answer;
        int32_t value = 0;
        if (control->kind == kit::ControlKind::slider)
            value = stop_at(*control, stops_of(model, control->id), released.at);
        else if (control->kind == kit::ControlKind::levels)
            value = level_of(placed.list, *control, released.at);
        answer.press = press_of(model, control->id, value);
        if (answer.press.command == Command::leave)
            answer.leave = true;
        return answer;
    }
    return answer;
}

Answer
key_event(kit::Interaction& interaction, const Model& model, const Layout& placed, kit::Key key) {
    Answer answer;
    // A pointer move hides the ring and leaves the focus where it was. Until
    // Tab or Shift+Tab shows the ring again, Return, Space and the arrows
    // are photo mode's, as they are before the first Tab.
    if (!kit::focus_ring_visible(interaction)) {
        if (key != kit::Key::tab && key != kit::Key::back_tab) {
            if (key == kit::Key::escape) {
                answer.consumed = true;
                answer.leave = true;
                answer.press.command = Command::leave;
                note_panel_press(answer);
            }
            return answer;
        }
        const kit::KeyOutcome moved = kit::key(interaction, placed.list, key);
        answer.consumed = true;
        answer.redraw = moved.result != kit::KeyResult::none;
        return answer;
    }

    // The touch layout's rows are one scroll area. Arrows judge them laid out
    // whole, so a row outside the view is still a stop. Left and right on a
    // control that steps stay with the kit, which changes the step.
    if (placed.rows.view.height > 0 && arrow_key(key)) {
        const kit::Control* focused = kit::control_of(placed.list, interaction.focused);
        const bool along_steps = focused != nullptr && focused->steps &&
                                 (key == kit::Key::left || key == kit::Key::right);
        if (!along_steps) {
            const kit::ControlId next = kit::focus_toward(
                placed.list,
                interaction.focused,
                direction_of(key),
                std::span<const kit::ScrollArea>(&placed.rows, 1)
            );
            answer.consumed = true;
            if (next == kit::no_control)
                return answer;
            interaction.focused = next;
            interaction.focus_shown = true;
            interaction.pointer_hid_ring = false;
            answer.redraw = true;
            return answer;
        }
    }

    const kit::ControlId before = interaction.focused;
    // Page Up and Page Down move a focused slider a tenth of its range, and
    // Home and End take it to its ends, as every slider does from the
    // keyboard. Elsewhere they stay photo mode's.
    if (key == kit::Key::page_up || key == kit::Key::page_down || key == kit::Key::home ||
        key == kit::Key::end) {
        const kit::Control* focused = kit::control_of(placed.list, before);
        if (focused != nullptr && focused->kind == kit::ControlKind::slider) {
            const int32_t last = std::max(stops_of(model, focused->id) - 1, int32_t{0});
            const int32_t big = std::max(last / 10, int32_t{1});
            const int32_t now = stop_of(model, focused->id);
            int32_t next = now;
            if (key == kit::Key::page_up)
                next = now + big;
            else if (key == kit::Key::page_down)
                next = now - big;
            else if (key == kit::Key::home)
                next = 0;
            else
                next = last;
            next = std::clamp(next, int32_t{0}, last);
            answer.consumed = true;
            answer.redraw = true;
            if (next != now) {
                answer.press = press_of(model, focused->id, next);
                note_panel_press(answer);
            }
            return answer;
        }
    }
    const kit::KeyOutcome moved = kit::key(interaction, placed.list, key);
    if (moved.result == kit::KeyResult::scroll)
        return answer;
    if (moved.result == kit::KeyResult::cancel) {
        answer.consumed = true;
        answer.leave = true;
        answer.press.command = Command::leave;
        note_panel_press(answer);
        return answer;
    }
    if (moved.result == kit::KeyResult::accept) {
        const kit::Control* focused = kit::control_of(placed.list, before);
        answer.consumed = true;
        answer.redraw = true;
        if (focused != nullptr && focused->kind == kit::ControlKind::slider)
            return answer;
        if (focused != nullptr && focused->kind == kit::ControlKind::levels) {
            const int32_t count = std::max(level_count(*focused), int32_t{1});
            answer.press = command_for(focused->id, model, chosen_level(model, focused->id, count));
            answer.dismiss_toast = true;
            return answer;
        }
        answer.press.command = Command::take_photo;
        note_panel_press(answer);
        return answer;
    }
    if (moved.result == kit::KeyResult::redraw) {
        answer.consumed = true;
        answer.redraw = true;
        return answer;
    }
    if (moved.result == kit::KeyResult::none) {
        const bool stayed = key == kit::Key::up || key == kit::Key::down || key == kit::Key::left ||
                            key == kit::Key::right || key == kit::Key::space ||
                            key == kit::Key::enter;
        answer.consumed = stayed;
        return answer;
    }
    if (moved.result != kit::KeyResult::to_control)
        return answer;

    answer.consumed = true;
    answer.redraw = true;
    const kit::Control* control = kit::control_of(placed.list, moved.control);
    if (control == nullptr)
        return answer;
    if (control->kind == kit::ControlKind::slider) {
        if (key == kit::Key::left || key == kit::Key::right) {
            const int32_t last = std::max(stops_of(model, control->id) - 1, int32_t{0});
            const int32_t step = key == kit::Key::right ? 1 : -1;
            const int32_t next = std::clamp(stop_of(model, control->id) + step, int32_t{0}, last);
            answer.press = press_of(model, control->id, next);
            note_panel_press(answer);
        }
        return answer;
    }
    if (control->kind == kit::ControlKind::levels) {
        const int32_t count = std::max(level_count(*control), int32_t{1});
        const int32_t current = chosen_level(model, control->id, count);
        if (key == kit::Key::left || key == kit::Key::right) {
            const int32_t step = key == kit::Key::right ? 1 : -1;
            int32_t next = std::clamp(current + step, int32_t{0}, count - 1);
            // Slow is refused in a shared match. A look's strip has no such gap.
            if (control->id == kRun && next == 1 && !model.slow_lock.empty())
                next = step > 0 ? 2 : 0;
            // A locked size or smooth step is passed over. Past the last one
            // offered nothing changes.
            while (next >= 0 && next < count && level_locked(model, control->id, next))
                next += step;
            // Repeating the lit level would toggle Freeze. Only a new level is sent.
            if (next >= 0 && next < count && next != current)
                answer.press = command_for(control->id, model, next);
            answer.dismiss_toast = true;
        } else if (key == kit::Key::space || key == kit::Key::enter) {
            answer.press = command_for(control->id, model, current);
            answer.dismiss_toast = true;
        }
        if (answer.press.command == Command::leave)
            answer.leave = true;
        return answer;
    }
    if (key == kit::Key::left || key == kit::Key::right) {
        if (control->kind == kit::ControlKind::toggle)
            answer.press = press_of(model, control->id, 0);
        note_panel_press(answer);
        return answer;
    }
    if (key == kit::Key::space || key == kit::Key::enter) {
        answer.press = press_of(model, control->id, 0);
        // A locked control still counts as a press of the panel.
        answer.dismiss_toast = true;
    }
    if (answer.press.command == Command::leave)
        answer.leave = true;
    note_panel_press(answer);
    return answer;
}

} // namespace oa::ui::photo_panel

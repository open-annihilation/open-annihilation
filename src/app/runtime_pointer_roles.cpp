// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Which button holds each pointer role for the bindings in use. The match asks
// here instead of naming the left, right or middle button. The left-click
// scheme's defaults stand in until a resolution has copied the chords, and
// whenever a role kept no chord of its own.
#include "oa/app/runtime.hpp"

#include "device_state.hpp"

#include "oa/sim/gameplay_input/order_cursor.hpp"
#include "oa/ui/key_bindings/chord.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <string_view>

namespace oa::app {

namespace keys = oa::ui::key_bindings;
namespace input = oa::sim::gameplay_input;

namespace {

/// The roles the match asks, in the order `PointerRoleTable` stores them.
/// The count matches `Runtime::kPointerRoleCount`; `rebuild_pointer_roles` asserts that.
constexpr std::size_t kRoleNameCount = 14;
constexpr std::array<std::string_view, kRoleNameCount> kRoleNames{
    "pointer.select",
    "pointer.command",
    "pointer.select-type",
    "pointer.pan",
    "pointer.mouse-look",
    "pointer.minimap-jump",
    "pointer.minimap-scroll",
    "build.add",
    "build.remove",
    "pointer.camera-orbit",
    "pointer.camera-pan",
    "camera.zoom-in",
    "camera.zoom-out",
    "build.move-queued-site",
};

constexpr uint8_t kButtonLeft = static_cast<uint8_t>(SDL_BUTTON_LEFT);
constexpr uint8_t kButtonMiddle = static_cast<uint8_t>(SDL_BUTTON_MIDDLE);
constexpr uint8_t kButtonRight = static_cast<uint8_t>(SDL_BUTTON_RIGHT);

/// The slot of a role, or -1 when the match does not ask it.
///
/// @param action the action id
/// @return the slot
int role_slot(std::string_view action) {
    for (std::size_t index = 0; index < kRoleNames.size(); ++index) {
        if (kRoleNames[index] == action)
            return static_cast<int>(index);
    }
    return -1;
}

/// One default chord: a button or a wheel, and the modifiers it needs.
struct DefaultChord {
    uint8_t modifiers{};
    uint8_t button{};
    int8_t wheel{};
    uint8_t gesture{};
};

/// The scheme's default chords for a slot. The left-click scheme uses an
/// action's left-click default where it has one.
///
/// @param slot the role
/// @param left_click the left-click interface
/// @param[out] out the chords
/// @param[out] count how many were written
void default_chords(int slot, bool left_click, std::array<DefaultChord, 4>& out, uint8_t& count) {
    count = 0;
    const auto add = [&](uint8_t modifiers, uint8_t button, int8_t wheel, uint8_t gesture) {
        if (count >= out.size())
            return;
        out[count] = DefaultChord{modifiers, button, wheel, gesture};
        ++count;
    };
    const uint8_t press = static_cast<uint8_t>(keys::Gesture::press);
    const uint8_t doubled = static_cast<uint8_t>(keys::Gesture::double_press);
    const uint8_t alt = keys::alt;
    const uint8_t shift = keys::shift;
    const uint8_t ctrl = keys::ctrl;
    switch (slot) {
    case 0: // pointer.select
        add(0, kButtonLeft, 0, press);
        break;
    case 1: // pointer.command
        add(0, left_click ? kButtonLeft : kButtonRight, 0, press);
        break;
    case 2: // pointer.select-type
        add(0, kButtonLeft, 0, doubled);
        break;
    case 3: // pointer.pan
        add(0, kButtonMiddle, 0, press);
        break;
    case 4: // pointer.mouse-look
        add(ctrl, kButtonRight, 0, press);
        break;
    case 5: // pointer.minimap-jump
        add(0, left_click ? kButtonLeft : kButtonRight, 0, press);
        break;
    case 6: // pointer.minimap-scroll
        add(0, left_click ? kButtonRight : kButtonLeft, 0, press);
        break;
    case 7: // build.add
        add(0, kButtonLeft, 0, press);
        break;
    case 8: // build.remove
        add(0, kButtonRight, 0, press);
        break;
    case 9: // pointer.camera-orbit
        add(alt, kButtonLeft, 0, press);
        add(alt, kButtonMiddle, 0, press);
        break;
    case 10: // pointer.camera-pan
        add(static_cast<uint8_t>(alt | shift), kButtonLeft, 0, press);
        break;
    case 11: // camera.zoom-in
        add(0, 0, 1, press);
        break;
    case 12: // camera.zoom-out
        add(0, 0, -1, press);
        break;
    case 13: // build.move-queued-site
        add(shift, kButtonLeft, 0, press);
        break;
    default:
        break;
    }
}

/// The modifier bits a chord requires, with Primary read as this platform's key.
///
/// @param chord_modifiers the chord's bits
/// @return the bits that must be held
uint8_t required_modifier_bits(uint8_t chord_modifiers) {
    uint8_t need = chord_modifiers;
    if ((need & keys::primary) == 0)
        return need;
    need = static_cast<uint8_t>(need & static_cast<uint8_t>(~keys::primary));
#ifdef __APPLE__
    need = static_cast<uint8_t>(need | keys::meta);
#else
    need = static_cast<uint8_t>(need | keys::ctrl);
#endif
    return need;
}

/// Whether every modifier a chord names is held. Extras are allowed.
///
/// @param chord_modifiers the chord's bits
/// @param held the bits held now
/// @return true when the chord's modifiers are among those held
bool modifiers_match(uint8_t chord_modifiers, uint8_t held) {
    const uint8_t need = required_modifier_bits(chord_modifiers);
    return (need & static_cast<uint8_t>(~held)) == 0;
}

/// How many modifiers a chord names. A higher count is the more specific chord.
///
/// @param modifiers the chord's bits
/// @return the count
int modifier_rank(uint8_t modifiers) {
    int rank = 0;
    for (const uint8_t bit :
         {uint8_t{keys::primary},
          uint8_t{keys::ctrl},
          uint8_t{keys::alt},
          uint8_t{keys::shift},
          uint8_t{keys::meta}}) {
        if ((modifiers & bit) != 0)
            ++rank;
    }
    return rank;
}

} // namespace

void Runtime::rebuild_pointer_roles(const keys::Resolved& resolved) {
    static_assert(kPointerRoleCount == kRoleNameCount);
    pointer_roles_ = {};
    for (const keys::Bound& bound : resolved.actions) {
        const int slot = role_slot(bound.id);
        if (slot < 0)
            continue;
        const auto slot_index = static_cast<std::size_t>(slot);
        uint8_t count = 0;
        for (const keys::BoundChord& item : bound.chords) {
            if (count >= kPointerRoleChordLimit)
                break;
            const keys::Chord& chord = item.chord;
            if (chord.kind != keys::TriggerKind::mouse && chord.kind != keys::TriggerKind::wheel)
                continue;
            PointerRoleChord& stored = pointer_roles_.chords[slot_index][count];
            stored.modifiers = chord.modifiers;
            stored.button = chord.button;
            stored.wheel = chord.wheel;
            stored.kind = static_cast<uint8_t>(chord.kind);
            stored.gesture = static_cast<uint8_t>(chord.gesture);
            ++count;
        }
        pointer_roles_.counts[slot_index] = count;
    }
    pointer_roles_.ready = true;
}

bool Runtime::pointer_interface_is_left_click() const {
    const int32_t face =
        match_ != nullptr ? match_->state().game.interface_type : preferences_.interface_type;
    return face == input::interface_left_click;
}

void Runtime::copy_role_chords(
    std::string_view action,
    std::array<PointerRoleChord, kPointerRoleChordLimit>& out,
    uint8_t& count
) const {
    count = 0;
    const int slot = role_slot(action);
    if (slot < 0)
        return;
    const auto slot_index = static_cast<std::size_t>(slot);
    if (pointer_roles_.ready && pointer_roles_.counts[slot_index] > 0) {
        const uint8_t stored = pointer_roles_.counts[slot_index];
        for (uint8_t index = 0; index < stored; ++index) {
            out[count] = pointer_roles_.chords[slot_index][index];
            ++count;
        }
        return;
    }
    std::array<DefaultChord, 4> defaults{};
    uint8_t default_count = 0;
    default_chords(slot, pointer_interface_is_left_click(), defaults, default_count);
    for (uint8_t index = 0; index < default_count; ++index) {
        const DefaultChord& chord = defaults[index];
        PointerRoleChord stored{};
        stored.modifiers = chord.modifiers;
        stored.button = chord.button;
        stored.wheel = chord.wheel;
        stored.kind = chord.button != 0 ? static_cast<uint8_t>(keys::TriggerKind::mouse)
                                        : static_cast<uint8_t>(keys::TriggerKind::wheel);
        stored.gesture = chord.gesture;
        out[count] = stored;
        ++count;
    }
}

uint8_t Runtime::pointer_role_button(std::string_view action) const {
    std::array<PointerRoleChord, kPointerRoleChordLimit> chords{};
    uint8_t count = 0;
    copy_role_chords(action, chords, count);
    uint8_t doubled = 0;
    const uint8_t mouse = static_cast<uint8_t>(keys::TriggerKind::mouse);
    const uint8_t double_press = static_cast<uint8_t>(keys::Gesture::double_press);
    for (uint8_t index = 0; index < count; ++index) {
        const PointerRoleChord& chord = chords[index];
        if (chord.kind != mouse || chord.button == 0)
            continue;
        if (chord.gesture != double_press)
            return chord.button;
        doubled = chord.button;
    }
    return doubled;
}

uint8_t Runtime::match_select_button() const {
    const uint8_t select = pointer_role_button("pointer.select");
    return select != 0 ? select : kButtonLeft;
}

uint8_t Runtime::match_command_button() const {
    const uint8_t select = match_select_button();
    const uint8_t command = pointer_role_button("pointer.command");
    if (command != 0 && command != select)
        return command;
    // The left-click scheme gives select and command the same button. The
    // other button is the one mouse look holds, which is the press the
    // right-hand path takes today.
    const uint8_t look = pointer_role_button("pointer.mouse-look");
    if (look != 0 && look != select)
        return look;
    return select == kButtonLeft ? kButtonRight : kButtonLeft;
}

uint8_t Runtime::pointer_radar_scroll_button() const {
    // The right-click interface starts the scroll on the select press. The
    // left-click interface starts it on the command press.
    return pointer_interface_is_left_click() ? match_command_button() : match_select_button();
}

float Runtime::pointer_zoom_amount(float turn) const {
    if (!std::isfinite(turn) || turn == 0.F)
        return turn;
    std::array<PointerRoleChord, kPointerRoleChordLimit> in_chords{};
    std::array<PointerRoleChord, kPointerRoleChordLimit> out_chords{};
    uint8_t in_count = 0;
    uint8_t out_count = 0;
    copy_role_chords("camera.zoom-in", in_chords, in_count);
    copy_role_chords("camera.zoom-out", out_chords, out_count);
    const uint8_t wheel = static_cast<uint8_t>(keys::TriggerKind::wheel);
    int8_t in_sign = 1;
    int8_t out_sign = -1;
    for (uint8_t index = 0; index < in_count; ++index) {
        if (in_chords[index].kind == wheel && in_chords[index].wheel != 0) {
            in_sign = in_chords[index].wheel > 0 ? int8_t{1} : int8_t{-1};
            break;
        }
    }
    for (uint8_t index = 0; index < out_count; ++index) {
        if (out_chords[index].kind == wheel && out_chords[index].wheel != 0) {
            out_sign = out_chords[index].wheel > 0 ? int8_t{1} : int8_t{-1};
            break;
        }
    }
    const int8_t turn_sign = turn > 0.F ? int8_t{1} : int8_t{-1};
    const float magnitude = std::fabs(turn);
    if (turn_sign == in_sign && turn_sign != out_sign)
        return magnitude;
    if (turn_sign == out_sign && turn_sign != in_sign)
        return -magnitude;
    return turn;
}

Runtime::PointerCameraDrag Runtime::pointer_camera_drag(uint8_t button, uint8_t modifiers) {
    const uint8_t mouse = static_cast<uint8_t>(keys::TriggerKind::mouse);
    const uint8_t press = static_cast<uint8_t>(keys::Gesture::press);
    int best_rank = -1;
    PointerCameraDrag best = PointerCameraDrag::none;
    const auto consider = [&](std::string_view action, PointerCameraDrag kind) {
        std::array<PointerRoleChord, kPointerRoleChordLimit> chords{};
        uint8_t count = 0;
        copy_role_chords(action, chords, count);
        for (uint8_t index = 0; index < count; ++index) {
            const PointerRoleChord& chord = chords[index];
            if (chord.kind != mouse || chord.gesture != press || chord.button != button)
                continue;
            if (!modifiers_match(chord.modifiers, modifiers))
                continue;
            const int rank = modifier_rank(chord.modifiers);
            if (rank > best_rank) {
                best_rank = rank;
                best = kind;
            }
        }
    };
    consider("pointer.camera-orbit", PointerCameraDrag::orbit);
    consider("pointer.camera-pan", PointerCameraDrag::pan);
    if (best != PointerCameraDrag::none)
        return best;
    // A plain pan is the pan role's button. It pans the rotated view only
    // while Middle-click pans is on; the flat view's middle pan is separate.
    if (!live_renderer_active() || !engine_settings().middle_click_pan)
        return PointerCameraDrag::none;
    std::array<PointerRoleChord, kPointerRoleChordLimit> pan_chords{};
    uint8_t pan_count = 0;
    copy_role_chords("pointer.pan", pan_chords, pan_count);
    for (uint8_t index = 0; index < pan_count; ++index) {
        const PointerRoleChord& chord = pan_chords[index];
        if (chord.kind != mouse || chord.gesture != press || chord.button != button)
            continue;
        if (modifiers_match(chord.modifiers, modifiers))
            return PointerCameraDrag::pan;
    }
    return PointerCameraDrag::none;
}

uint8_t Runtime::pointer_modifier_bits() const {
    uint8_t bits = 0;
    const auto value = static_cast<uint32_t>(device_state::modifiers_held());
    if ((value & SDL_KMOD_CTRL) != 0)
        bits = static_cast<uint8_t>(bits | keys::ctrl);
    if ((value & SDL_KMOD_SHIFT) != 0 || shift_held_by_check_)
        bits = static_cast<uint8_t>(bits | keys::shift);
    if ((value & SDL_KMOD_ALT) != 0)
        bits = static_cast<uint8_t>(bits | keys::alt);
    if ((value & SDL_KMOD_GUI) != 0)
        bits = static_cast<uint8_t>(bits | keys::meta);
    const auto key_down = [](SDL_Scancode code) { return device_state::key_held(code); };
    if (key_down(SDL_SCANCODE_LSHIFT) || key_down(SDL_SCANCODE_RSHIFT) || shift_held_by_check_)
        bits = static_cast<uint8_t>(bits | keys::shift);
    if (key_down(SDL_SCANCODE_LCTRL) || key_down(SDL_SCANCODE_RCTRL))
        bits = static_cast<uint8_t>(bits | keys::ctrl);
    if (key_down(SDL_SCANCODE_LALT) || key_down(SDL_SCANCODE_RALT))
        bits = static_cast<uint8_t>(bits | keys::alt);
    if (key_down(SDL_SCANCODE_LGUI) || key_down(SDL_SCANCODE_RGUI))
        bits = static_cast<uint8_t>(bits | keys::meta);
    return bits;
}

bool Runtime::pointer_role_modifiers_held(std::string_view action, uint8_t modifiers) const {
    std::array<PointerRoleChord, kPointerRoleChordLimit> chords{};
    uint8_t count = 0;
    copy_role_chords(action, chords, count);
    const uint8_t mouse = static_cast<uint8_t>(keys::TriggerKind::mouse);
    for (uint8_t index = 0; index < count; ++index) {
        const PointerRoleChord& chord = chords[index];
        if (chord.kind == mouse && modifiers_match(chord.modifiers, modifiers))
            return true;
    }
    return false;
}

} // namespace oa::app

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The numpad pointer and the chords it sets aside while it is on. The time
// is the caller's.

#include "oa/ui/key_bindings/helpers.hpp"

#include "oa/data/input_actions/actions.hpp"
#include "oa/ui/key_bindings/key_codes.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::key_bindings {
namespace {

namespace actions = oa::data::input_actions;

/// The distance a linear ramp covers between two ages, in seconds.
///
/// Speed is `pointer_keys_start_speed` at age 0 and reaches
/// `pointer_keys_top_speed` at `pointer_keys_ramp_ms`, then stays there.
///
/// @param age0 the earlier age, in seconds
/// @param age1 the later age, in seconds
/// @return points of the 640×480 game
float ramp_distance(float age0, float age1) {
    if (age1 <= age0)
        return 0.F;
    const float ramp = static_cast<float>(pointer_keys_ramp_ms) / 1000.F;
    const float start = static_cast<float>(pointer_keys_start_speed);
    const float top = static_cast<float>(pointer_keys_top_speed);
    const float span = top - start;
    const auto rising = [&](float from, float to) {
        return start * (to - from) + (span / (2.F * ramp)) * (to * to - from * from);
    };
    if (ramp <= 0.F)
        return top * (age1 - age0);
    if (age1 <= ramp)
        return rising(age0, age1);
    if (age0 >= ramp)
        return top * (age1 - age0);
    return rising(age0, ramp) + top * (age1 - ramp);
}

/// The position a named key or a place names, when it has one.
///
/// @param chord the chord
/// @return the position, or nothing for a letter, a button or a wheel
std::optional<uint16_t> chord_scancode(const Chord& chord) {
    if (chord.kind == TriggerKind::place)
        return chord.scancode;
    if (chord.kind == TriggerKind::named && chord.named < named_keys().size())
        return named_keys()[chord.named].scancode;
    return std::nullopt;
}

/// Whether one chord is a key a pointer-key default takes.
///
/// A named key and the place of that same key are one key: Faster's
/// NumpadAdd is the hold's @NumpadAdd. Ctrl or Alt on either chord keeps
/// them apart, so a group chord stays.
///
/// @param bound the chord in use
/// @param taken the pointer-key default
/// @return true when the player would call them the same key
bool takes_default(const Chord& bound, const Chord& taken) {
    if (bound.modifiers != taken.modifiers || bound.gesture != taken.gesture)
        return false;
    if (same_key(bound, taken, nullptr))
        return true;
    const std::optional<uint16_t> bound_code = chord_scancode(bound);
    const std::optional<uint16_t> taken_code = chord_scancode(taken);
    return bound_code.has_value() && taken_code.has_value() && *bound_code == *taken_code;
}

/// The bit of the pointer-keys condition, or 0 when the table has none.
///
/// @return the bit
uint32_t pointer_keys_bit() {
    const auto names = actions::conditions();
    for (std::size_t index = 0; index < names.size() && index < 32; ++index) {
        if (names[index] == "pointer-keys")
            return uint32_t{1} << static_cast<uint32_t>(index);
    }
    return 0;
}

} // namespace

void PointerKeys::set_double_press_ms(uint32_t double_press_ms) {
    double_press_ms_ = double_press_ms;
}

PointerKeys::Direction* PointerKeys::find_direction(std::string_view id) {
    for (Direction& direction : directions_) {
        if (direction.id == id)
            return &direction;
    }
    return nullptr;
}

void PointerKeys::press_button(Button& button, uint8_t which, uint8_t clicks) {
    if (button.holders++ > 0)
        return;
    edges_.push_back(Edge{true, which, clicks});
}

void PointerKeys::release_button(Button& button, uint8_t which) {
    if (button.holders <= 0)
        return;
    if (--button.holders > 0)
        return;
    edges_.push_back(Edge{false, which, 0});
}

void PointerKeys::release_sent(Button& button, uint8_t which) {
    std::vector<Edge> kept;
    kept.reserve(edges_.size());
    for (const Edge& edge : edges_) {
        if (edge.button != which)
            kept.push_back(edge);
    }
    edges_ = std::move(kept);
    button.holders = 0;
    button.toggled = false;
    button.have_last = false;
    if (!button.sent)
        return;
    edges_.push_back(Edge{false, which, 0});
}

void PointerKeys::press(std::string_view id, uint64_t now_ms) {
    if (Direction* direction = find_direction(id); direction != nullptr) {
        if (direction->held)
            return;
        direction->held = true;
        direction->pressed_ms = now_ms;
        return;
    }
    if (id == "pointer.hold-left") {
        if (left_.toggled) {
            left_.toggled = false;
            release_button(left_, pointer_keys_left);
            return;
        }
        left_.toggled = true;
        press_button(left_, pointer_keys_left, 1);
        return;
    }
    if (id == "pointer.let-go") {
        release_sent(left_, pointer_keys_left);
        release_sent(right_, pointer_keys_right);
        return;
    }
    Button* button = nullptr;
    uint8_t which = 0;
    if (id == "pointer.click-left") {
        button = &left_;
        which = pointer_keys_left;
    } else if (id == "pointer.click-right") {
        button = &right_;
        which = pointer_keys_right;
    }
    if (button == nullptr)
        return;
    uint8_t clicks = 1;
    if (button->have_last && double_press_ms_ > 0 && now_ms >= button->last_ms &&
        now_ms - button->last_ms <= double_press_ms_)
        clicks = 2;
    button->last_ms = now_ms;
    button->have_last = true;
    press_button(*button, which, clicks);
}

void PointerKeys::release(std::string_view id, uint64_t now_ms) {
    (void)now_ms;
    if (Direction* direction = find_direction(id); direction != nullptr) {
        direction->held = false;
        return;
    }
    // Hold left stays down until the key is pressed again, or Let go.
    if (id == "pointer.hold-left" || id == "pointer.let-go")
        return;
    if (id == "pointer.click-left")
        release_button(left_, pointer_keys_left);
    else if (id == "pointer.click-right")
        release_button(right_, pointer_keys_right);
}

PointerStep PointerKeys::step(uint64_t now_ms) {
    PointerStep out;
    const bool had_step = have_step_;
    const uint64_t previous_ms = last_step_ms_;
    have_step_ = true;
    if (!had_step || now_ms >= previous_ms)
        last_step_ms_ = now_ms;

    uint64_t earliest = now_ms;
    bool any = false;
    float sum_x = 0.F;
    float sum_y = 0.F;
    for (const Direction& direction : directions_) {
        if (!direction.held)
            continue;
        any = true;
        if (direction.pressed_ms < earliest)
            earliest = direction.pressed_ms;
        sum_x += direction.x;
        sum_y += direction.y;
    }
    const float length = std::hypot(sum_x, sum_y);
    if (any && length > 0.F) {
        // The first step covers the whole hold. Later steps start where the
        // previous step ended, and not before this hold began.
        uint64_t began = earliest;
        if (had_step && previous_ms > earliest)
            began = previous_ms;
        if (now_ms > began) {
            const float age0 = static_cast<float>(began - earliest) / 1000.F;
            const float age1 = static_cast<float>(now_ms - earliest) / 1000.F;
            const float distance = ramp_distance(age0, age1);
            out.dx = distance * (sum_x / length);
            out.dy = distance * (sum_y / length);
        }
    }

    std::optional<Edge> down;
    std::optional<Edge> up;
    std::vector<Edge> kept;
    kept.reserve(edges_.size());
    for (const Edge& edge : edges_) {
        if (edge.down && !down.has_value())
            down = edge;
        else if (!edge.down && !up.has_value())
            up = edge;
        else
            kept.push_back(edge);
    }
    edges_ = std::move(kept);
    if (down) {
        out.down = down->button;
        out.clicks = down->clicks;
        if (down->button == pointer_keys_left)
            left_.sent = true;
        else if (down->button == pointer_keys_right)
            right_.sent = true;
    }
    if (up) {
        out.up = up->button;
        if (up->button == pointer_keys_left)
            left_.sent = false;
        else if (up->button == pointer_keys_right)
            right_.sent = false;
    }
    return out;
}

void PointerKeys::clear() {
    for (Direction& direction : directions_)
        direction.held = false;
    release_sent(left_, pointer_keys_left);
    release_sent(right_, pointer_keys_right);
}

Resolved pointer_keys_bindings(const Resolved& resolved, bool on) {
    if (!on)
        return resolved;
    const uint32_t bit = pointer_keys_bit();
    std::vector<Chord> taken;
    for (const actions::Action& action : actions::actions()) {
        if (bit == 0 || (action.when & bit) == 0)
            continue;
        for (const std::string_view text : action.defaults) {
            ChordError error;
            if (const std::optional<Chord> chord = parse_chord(text, error))
                taken.push_back(*chord);
        }
    }
    Resolved out = resolved;
    for (Bound& bound : out.actions) {
        if (bit != 0 && (bound.when & bit) != 0)
            continue;
        std::vector<BoundChord> kept;
        kept.reserve(bound.chords.size());
        for (const BoundChord& binding : bound.chords) {
            if (binding.step == Step::own) {
                kept.push_back(binding);
                continue;
            }
            bool aside = false;
            for (const Chord& chord : taken) {
                if (takes_default(binding.chord, chord))
                    aside = true;
            }
            if (!aside)
                kept.push_back(binding);
        }
        bound.chords = std::move(kept);
    }
    return out;
}

} // namespace oa::ui::key_bindings

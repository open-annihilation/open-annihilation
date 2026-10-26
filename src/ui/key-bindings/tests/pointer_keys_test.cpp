// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Pointer keys on a synthetic clock, and the chords set aside while they are on.

#include "oa/test/check.hpp"
#include "oa/ui/key_bindings/helpers.hpp"
#include "oa/ui/key_bindings/layout.hpp"

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace keys = oa::ui::key_bindings;

using keys::Bound;
using keys::Chord;
using keys::PointerKeys;
using keys::PointerStep;
using keys::Resolved;

/// A chord the test expects to accept.
///
/// @param text the chord text
/// @return the chord
Chord must_parse(std::string_view text) {
    keys::ChordError error;
    const std::optional<Chord> chord = keys::parse_chord(text, error);
    OA_CHECK(chord.has_value());
    if (!chord)
        return {};
    return *chord;
}

/// One layout line.
///
/// @param action the action id
/// @param texts the chord texts
/// @return the binding
keys::LayoutBinding binding(std::string action, std::initializer_list<std::string_view> texts) {
    keys::LayoutBinding out;
    out.action = std::move(action);
    out.line = 1;
    out.column = 1;
    for (const std::string_view text : texts)
        out.chords.push_back(must_parse(text));
    return out;
}

/// The OA default on the US map.
///
/// @param map the keyboard, which the input keeps a pointer to
/// @return the input
keys::ResolveInput fresh(const keys::KeyboardMap& map) {
    keys::ResolveInput input;
    input.base = keys::Base::oa_default;
    input.keyboard = &map;
    return input;
}

/// Finds an action.
///
/// @param resolved the bindings
/// @param id the action id
/// @return the action, or null
const Bound* find_bound(const Resolved& resolved, std::string_view id) {
    for (const Bound& bound : resolved.actions) {
        if (bound.id == id)
            return &bound;
    }
    return nullptr;
}

/// Reports whether the chords are these texts, in order.
///
/// @param bound the action, or null
/// @param texts the texts
/// @return true when they match
bool chords_are(const Bound* bound, std::initializer_list<std::string_view> texts) {
    if (bound == nullptr || bound->chords.size() != texts.size())
        return false;
    std::size_t index = 0;
    for (const std::string_view text : texts) {
        if (keys::chord_text(bound->chords[index].chord) != text)
            return false;
        ++index;
    }
    return true;
}

/// Whether two distances are as close as the ramp's arithmetic allows.
///
/// @param got the distance
/// @param want the distance the ramp gives
/// @return true within a thousandth of a point
bool near(float got, float want) {
    return std::fabs(got - want) < 0.001F;
}

/// Speed at the start, halfway up the ramp, and at the top.
void speed_follows_the_ramp() {
    PointerKeys keys;
    keys.press("pointer.move-right", 0);
    const PointerStep same = keys.step(0);
    OA_CHECK(same.dx == 0.F && same.dy == 0.F);

    const PointerStep opening = keys.step(1);
    OA_CHECK(near(opening.dx, 0.06021F));
    OA_CHECK(opening.dy == 0.F);

    PointerKeys halfway;
    halfway.press("pointer.move-up", 0);
    OA_CHECK(halfway.step(0).dy == 0.F);
    const PointerStep at_half = halfway.step(500);
    OA_CHECK(near(at_half.dy, -82.5F));
    OA_CHECK(at_half.dx == 0.F);
    const PointerStep coast = halfway.step(501);
    OA_CHECK(near(coast.dy, -0.27021F));

    PointerKeys topped;
    topped.press("pointer.move-down", 0);
    (void)topped.step(1500);
    const PointerStep top = topped.step(1501);
    OA_CHECK(near(top.dy, 0.480F));
    OA_CHECK(top.dx == 0.F);

    PointerKeys full;
    full.press("pointer.move-left", 0);
    const PointerStep second = full.step(1000);
    OA_CHECK(near(second.dx, -270.F));
    OA_CHECK(second.dy == 0.F);
}

/// Two held directions move on the diagonal at the cardinal speed.
void two_keys_make_a_diagonal() {
    PointerKeys keys;
    keys.press("pointer.move-up", 0);
    keys.press("pointer.move-right", 0);
    const PointerStep step = keys.step(1000);
    const float cardinal = 270.F;
    const float axis = cardinal / std::sqrt(2.F);
    OA_CHECK(near(step.dx, axis));
    OA_CHECK(near(step.dy, -axis));
    OA_CHECK(near(std::hypot(step.dx, step.dy), cardinal));
}

/// A second click inside the double-press time is a double click.
void click_and_double_click() {
    PointerKeys inside;
    inside.set_double_press_ms(500);
    inside.press("pointer.click-left", 0);
    const PointerStep first = inside.step(0);
    OA_CHECK(first.down == keys::pointer_keys_left && first.clicks == 1);
    inside.release("pointer.click-left", 10);
    const PointerStep released = inside.step(10);
    OA_CHECK(released.up == keys::pointer_keys_left);
    OA_CHECK(!released.down.has_value());

    inside.press("pointer.click-left", 400);
    const PointerStep doubled = inside.step(400);
    OA_CHECK(doubled.down == keys::pointer_keys_left && doubled.clicks == 2);
    inside.release("pointer.click-left", 420);
    (void)inside.step(420);

    inside.press("pointer.click-left", 1000);
    const PointerStep later = inside.step(1000);
    OA_CHECK(later.down == keys::pointer_keys_left && later.clicks == 1);

    PointerKeys outside;
    outside.set_double_press_ms(500);
    outside.press("pointer.click-right", 0);
    (void)outside.step(0);
    outside.release("pointer.click-right", 10);
    (void)outside.step(10);
    outside.press("pointer.click-right", 600);
    const PointerStep single = outside.step(600);
    OA_CHECK(single.down == keys::pointer_keys_right && single.clicks == 1);

    PointerKeys never;
    never.set_double_press_ms(0);
    never.press("pointer.click-left", 0);
    (void)never.step(0);
    never.release("pointer.click-left", 10);
    (void)never.step(10);
    never.press("pointer.click-left", 100);
    const PointerStep plain = never.step(100);
    OA_CHECK(plain.clicks == 1);
}

/// Hold left stays down when the key comes up, and Let go releases it.
void hold_and_let_go() {
    PointerKeys keys;
    keys.press("pointer.hold-left", 0);
    const PointerStep down = keys.step(0);
    OA_CHECK(down.down == keys::pointer_keys_left);
    OA_CHECK(!down.up.has_value());
    keys.release("pointer.hold-left", 50);
    const PointerStep still = keys.step(50);
    OA_CHECK(!still.down.has_value());
    OA_CHECK(!still.up.has_value());
    keys.press("pointer.let-go", 80);
    const PointerStep up = keys.step(80);
    OA_CHECK(up.up == keys::pointer_keys_left);
    OA_CHECK(!up.down.has_value());
}

/// clear releases a button the caller was sent, and drops a direction.
void clear_releases_a_held_button() {
    PointerKeys keys;
    keys.press("pointer.hold-left", 0);
    OA_CHECK(keys.step(0).down == keys::pointer_keys_left);
    keys.press("pointer.move-right", 0);
    keys.clear();
    const PointerStep up = keys.step(1000);
    OA_CHECK(up.up == keys::pointer_keys_left);
    OA_CHECK(up.dx == 0.F && up.dy == 0.F);
    const PointerStep rest = keys.step(1500);
    OA_CHECK(!rest.up.has_value());
    OA_CHECK(rest.dx == 0.F && rest.dy == 0.F);
}

/// While pointer keys are on, Faster loses NumpadAdd and keeps `=`.
void set_aside_while_on_keeps_equals_and_own_changes() {
    const keys::KeyboardMap map = keys::KeyboardMap::us();
    keys::ResolveInput input = fresh(map);
    const Resolved resolved = keys::resolve(input);
    const Resolved off = keys::pointer_keys_bindings(resolved, false);
    OA_CHECK(chords_are(find_bound(off, "game.speed-up"), {"@Equal", "NumpadAdd"}));
    OA_CHECK(chords_are(find_bound(off, "build.page-5"), {"@Digit5", "Numpad5"}));

    const Resolved on = keys::pointer_keys_bindings(resolved, true);
    OA_CHECK(chords_are(find_bound(on, "game.speed-up"), {"@Equal"}));
    OA_CHECK(chords_are(find_bound(on, "pointer.hold-left"), {"@NumpadAdd"}));
    OA_CHECK(chords_are(find_bound(on, "build.page-5"), {"@Digit5"}));
    OA_CHECK(chords_are(find_bound(on, "group.assign-1"), {"Ctrl+@Digit1", "Ctrl+Numpad1"}));

    const std::vector<keys::Clash> clashes = keys::find_clashes(on, input);
    for (const keys::Clash& clash : clashes) {
        const bool speed = clash.a == "game.speed-up" || clash.b == "game.speed-up";
        const bool hold = clash.a == "pointer.hold-left" || clash.b == "pointer.hold-left";
        OA_CHECK(!(speed && hold));
    }

    keys::Layout player;
    player.bindings.push_back(binding("game.speed-up", {"NumpadAdd"}));
    keys::ResolveInput own = fresh(map);
    own.own = &player;
    const Resolved kept = keys::pointer_keys_bindings(keys::resolve(own), true);
    OA_CHECK(chords_are(find_bound(kept, "game.speed-up"), {"NumpadAdd"}));
}

} // namespace

int main() {
    speed_follows_the_ramp();
    two_keys_make_a_diagonal();
    click_and_double_click();
    hold_and_let_go();
    clear_releases_a_held_button();
    set_aside_while_on_keeps_equals_and_own_changes();
    return oa::test::check_exit_status();
}

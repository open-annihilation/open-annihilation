// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Slow keys, bounce keys, the double-press and long-press times, the player's
// own repeat and the drag threshold's tick limit, on synthetic timelines.
#include "oa/test/check.hpp"
#include "oa/ui/key_bindings/dispatcher.hpp"
#include "oa/ui/key_bindings/key_codes.hpp"

#include "oa/data/input_actions/actions.hpp"

#include <cstdint>
#include <cstdio>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace keys = oa::ui::key_bindings;
namespace actions = oa::data::input_actions;

using keys::ActionEvent;
using keys::Contexts;
using keys::Dispatcher;
using keys::Fed;
using keys::InputEvent;
using keys::InputKind;
using keys::KeyboardMap;
using keys::LegacyGate;
using keys::Phase;
using keys::Platform;
using keys::Resolved;
using keys::Timings;

/// The bit of a context, or 0 when the table has no such name.
///
/// @param name the context id
/// @return the bit
uint32_t context_bit(std::string_view name) {
    const std::span<const actions::Context> table = actions::contexts();
    for (std::size_t index = 0; index < table.size() && index < 32; ++index) {
        if (table[index].name == name)
            return uint32_t{1} << static_cast<uint32_t>(index);
    }
    std::fprintf(stderr, "missing context %.*s\n", static_cast<int>(name.size()), name.data());
    return 0;
}

/// The position of a UI Events code. 0 when the table has none.
///
/// @param code the code
/// @return the position number
uint16_t place_of(std::string_view code) {
    const std::optional<uint16_t> found = keys::scancode_of(code);
    if (!found)
        std::fprintf(stderr, "missing place %.*s\n", static_cast<int>(code.size()), code.data());
    OA_CHECK(found.has_value());
    return found.value_or(0);
}

/// A named key, or null when the table has none.
///
/// @param name the chord's word
/// @return the key; the table outlives the test
const keys::NamedKeySpec* named_key(std::string_view name) {
    for (const keys::NamedKeySpec& key : keys::named_keys()) {
        if (key.name == name)
            return &key;
    }
    std::fprintf(stderr, "missing named key %.*s\n", static_cast<int>(name.size()), name.data());
    return nullptr;
}

/// A key event for a letter.
///
/// @param scancode the letter's position
/// @param code the key code events carry
/// @param down the key went down
/// @param time_ms when it happened
/// @return the event
InputEvent letter_event(uint16_t scancode, uint32_t code, bool down, uint64_t time_ms) {
    InputEvent event;
    event.kind = InputKind::key;
    event.down = down;
    event.scancode = scancode;
    event.key_code = code;
    event.time_ms = time_ms;
    return event;
}

/// A key event for a named key.
///
/// @param name the chord's word
/// @param down the key went down
/// @param time_ms when it happened
/// @return the event
InputEvent named_event(std::string_view name, bool down, uint64_t time_ms) {
    InputEvent event;
    const keys::NamedKeySpec* key = named_key(name);
    OA_CHECK(key != nullptr);
    if (key == nullptr)
        return event;
    event.kind = InputKind::key;
    event.down = down;
    event.scancode = key->scancode;
    event.key_code = key->key_code;
    event.time_ms = time_ms;
    return event;
}

/// Prints the action events a case produced.
///
/// @param fed the result
void show(const Fed& fed) {
    for (const ActionEvent& item : fed.events) {
        std::fprintf(
            stderr,
            "  %.*s phase %u\n",
            static_cast<int>(item.action.size()),
            item.action.data(),
            static_cast<unsigned>(item.phase)
        );
    }
}

/// How many events have that action in that phase.
///
/// @param fed the result
/// @param action the action id
/// @param phase the phase
/// @return the count
int count_phase(const Fed& fed, std::string_view action, Phase phase) {
    int count = 0;
    for (const ActionEvent& item : fed.events)
        if (item.action == action && item.phase == phase)
            ++count;
    return count;
}

/// Whether the result has that action in that phase.
///
/// @param fed the result
/// @param action the action id
/// @param phase the phase
/// @return true when one event says so
bool has_phase(const Fed& fed, std::string_view action, Phase phase) {
    return count_phase(fed, action, phase) > 0;
}

/// oa-default on the US map.
///
/// @param map the US map
/// @return the bindings
Resolved prepared(const KeyboardMap& map) {
    keys::ResolveInput input;
    input.keyboard = &map;
    input.platform.primary_is_meta = true;
    return keys::resolve(input);
}

/// A dispatcher holding those bindings, in play.
///
/// @param resolved the bindings
/// @param map the US map
/// @return the dispatcher
Dispatcher open_dispatcher(const Resolved& resolved, const KeyboardMap& map) {
    Dispatcher dispatcher;
    dispatcher.set_bindings(resolved);
    dispatcher.set_keyboard(map);
    Platform where;
    where.primary_is_meta = true;
    dispatcher.set_platform(where);
    Contexts contexts;
    contexts.active = context_bit("play") | context_bit("match");
    dispatcher.set_contexts(contexts);
    return dispatcher;
}

/// Sets text entry for the next event.
///
/// @param dispatcher the dispatcher
/// @param text_entry whether text is being typed
void set_text_entry(Dispatcher& dispatcher, bool text_entry) {
    Contexts contexts;
    contexts.active = context_bit("play") | context_bit("match") | context_bit("chat");
    contexts.text_entry = text_entry;
    dispatcher.set_contexts(contexts);
}

/// A chosen layout whose one action has the chords named.
///
/// @param map the US map
/// @param action the action id
/// @param chords the chord texts
/// @return the dispatcher
Dispatcher
with_chords(const KeyboardMap& map, std::string_view action, std::vector<std::string_view> chords) {
    keys::Layout layout;
    layout.id = "timing";
    keys::LayoutBinding row;
    row.action = std::string{action};
    for (const std::string_view text : chords) {
        keys::ChordError error;
        const std::optional<keys::Chord> chord = keys::parse_chord(text, error);
        OA_CHECK(chord.has_value());
        if (chord)
            row.chords.push_back(*chord);
    }
    layout.bindings.push_back(std::move(row));
    keys::ResolveInput input;
    input.keyboard = &map;
    input.chosen = &layout;
    Dispatcher dispatcher;
    dispatcher.set_keyboard(map);
    dispatcher.set_bindings(keys::resolve(input));
    Contexts contexts;
    contexts.active = context_bit("play") | context_bit("match");
    dispatcher.set_contexts(contexts);
    return dispatcher;
}

/// Slow keys of 300 ms: a 100 ms tap fires nothing, and a hold fires once at 300 ms.
void slow_keys(const Resolved& resolved, const KeyboardMap& map) {
    const uint16_t key_a = place_of("KeyA");
    const uint16_t key_s = place_of("KeyS");
    Timings timings;
    timings.slow_keys_ms = 300;

    Dispatcher tap = open_dispatcher(resolved, map);
    tap.set_timings(timings);
    const Fed held_down = tap.feed(letter_event(key_a, 'a', true, 0));
    OA_CHECK(held_down.legacy == LegacyGate::hold);
    OA_CHECK(held_down.events.empty());
    const Fed early_up = tap.feed(letter_event(key_a, 'a', false, 100));
    OA_CHECK(early_up.legacy == LegacyGate::drop);
    const Fed after_tap = tap.advance(400);
    if (!after_tap.events.empty() || !after_tap.admitted.empty()) {
        std::fprintf(stderr, "a 100 ms tap fires nothing\n");
        show(after_tap);
    }
    OA_CHECK(after_tap.events.empty());
    OA_CHECK(after_tap.admitted.empty());

    Dispatcher hold = open_dispatcher(resolved, map);
    hold.set_timings(timings);
    OA_CHECK(hold.feed(letter_event(key_a, 'a', true, 0)).legacy == LegacyGate::hold);
    const Fed before = hold.advance(299);
    OA_CHECK(before.events.empty());
    OA_CHECK(before.admitted.empty());
    const Fed due = hold.advance(300);
    const bool once =
        due.admitted.size() == 1 && count_phase(due, "order.attack", Phase::pressed) == 1;
    if (!once) {
        std::fprintf(stderr, "a 400 ms hold fires once, at 300 ms\n");
        show(due);
    }
    OA_CHECK(once);
    const Fed again = hold.advance(400);
    OA_CHECK(again.events.empty());
    OA_CHECK(again.admitted.empty());
    const Fed released = hold.feed(letter_event(key_a, 'a', false, 400));
    OA_CHECK(released.legacy == LegacyGate::deliver);
    OA_CHECK(has_phase(released, "order.attack", Phase::released));

    // Another key is held to the same rule, not dropped because A was.
    Dispatcher other = open_dispatcher(resolved, map);
    other.set_timings(timings);
    OA_CHECK(other.feed(letter_event(key_s, 's', true, 0)).legacy == LegacyGate::hold);
    const Fed other_due = other.advance(300);
    OA_CHECK(count_phase(other_due, "order.stop", Phase::pressed) == 1);
}

/// Bounce keys of 300 ms drop a second press 200 ms after a release and keep one 400 ms after.
void bounce_keys(const Resolved& resolved, const KeyboardMap& map) {
    const uint16_t key_a = place_of("KeyA");
    const uint16_t key_s = place_of("KeyS");
    Timings timings;
    timings.bounce_keys_ms = 300;
    Dispatcher keys_in = open_dispatcher(resolved, map);
    keys_in.set_timings(timings);

    OA_CHECK(keys_in.feed(letter_event(key_a, 'a', true, 0)).legacy == LegacyGate::deliver);
    OA_CHECK(keys_in.feed(letter_event(key_a, 'a', false, 1000)).legacy == LegacyGate::deliver);
    const Fed soon = keys_in.feed(letter_event(key_a, 'a', true, 1200));
    OA_CHECK(soon.legacy == LegacyGate::drop);
    OA_CHECK(soon.events.empty());
    const Fed soon_up = keys_in.feed(letter_event(key_a, 'a', false, 1250));
    OA_CHECK(soon_up.legacy == LegacyGate::drop);
    OA_CHECK(soon_up.events.empty());

    // The dropped release does not move the window: 400 ms after the accepted
    // release is kept, though it is only 150 ms after the dropped one.
    const Fed later = keys_in.feed(letter_event(key_a, 'a', true, 1400));
    OA_CHECK(later.legacy == LegacyGate::deliver);
    OA_CHECK(count_phase(later, "order.attack", Phase::pressed) == 1);
    OA_CHECK(keys_in.feed(letter_event(key_a, 'a', false, 1500)).legacy == LegacyGate::deliver);

    // Another key inside the window is kept.
    const Fed other = keys_in.feed(letter_event(key_s, 's', true, 1600));
    OA_CHECK(other.legacy == LegacyGate::deliver);
    OA_CHECK(has_phase(other, "order.stop", Phase::pressed));
}

/// Two presses 550 ms apart match `:double` at 600 ms; two 650 ms apart do not.
void double_press(const KeyboardMap& map) {
    Dispatcher attack = with_chords(map, "order.attack", {"A", "A:double"});
    Timings timings;
    timings.double_press_ms = 600;
    attack.set_timings(timings);
    const uint16_t key_a = place_of("KeyA");
    OA_CHECK(
        has_phase(attack.feed(letter_event(key_a, 'a', true, 0)), "order.attack", Phase::pressed)
    );
    (void)attack.feed(letter_event(key_a, 'a', false, 10));
    const Fed close = attack.feed(letter_event(key_a, 'a', true, 550));
    OA_CHECK(has_phase(close, "order.attack", Phase::double_press));
    (void)attack.feed(letter_event(key_a, 'a', false, 560));

    const Fed apart = attack.feed(letter_event(key_a, 'a', true, 1200));
    OA_CHECK(has_phase(apart, "order.attack", Phase::pressed));
    OA_CHECK(!has_phase(apart, "order.attack", Phase::double_press));
}

/// A `:hold` chord at 800 ms fires at 800, not at 700.
void long_press(const KeyboardMap& map) {
    Dispatcher strip = with_chords(map, "game.status-strip", {"Space:hold"});
    Timings timings;
    timings.long_press_ms = 800;
    strip.set_timings(timings);
    const Fed down = strip.feed(named_event("Space", true, 0));
    OA_CHECK(down.legacy == LegacyGate::deliver);
    OA_CHECK(!has_phase(down, "game.status-strip", Phase::held));
    OA_CHECK(strip.advance(700).events.empty());
    const Fed due = strip.advance(800);
    OA_CHECK(count_phase(due, "game.status-strip", Phase::held) == 1);
    OA_CHECK(strip.advance(900).events.empty());
}

/// Repeat delay 200 and rate 10: nine repeats in 1,000 ms, and a press action never repeats.
void repeat_rate(const Resolved& resolved, const KeyboardMap& map) {
    Timings timings;
    timings.repeat_delay_ms = 200;
    timings.repeat_rate = 10;
    Dispatcher pan = open_dispatcher(resolved, map);
    pan.set_timings(timings);
    OA_CHECK(pan.feed(named_event("Right", true, 0)).legacy == LegacyGate::deliver);
    InputEvent repeated = named_event("Right", true, 50);
    repeated.repeat = true;
    const Fed ignored = pan.feed(repeated);
    OA_CHECK(ignored.legacy == LegacyGate::deliver);
    OA_CHECK(!has_phase(ignored, "camera.pan-right", Phase::repeated));
    const Fed burst = pan.advance(1000);
    const int repeats = count_phase(burst, "camera.pan-right", Phase::repeated);
    if (repeats != 9) {
        std::fprintf(stderr, "a repeat held 1000 ms fires %d, not 9\n", repeats);
        show(burst);
    }
    OA_CHECK(repeats == 9);

    Dispatcher attack = open_dispatcher(resolved, map);
    attack.set_timings(timings);
    (void)attack.feed(letter_event(place_of("KeyA"), 'a', true, 0));
    InputEvent attack_repeat = letter_event(place_of("KeyA"), 'a', true, 50);
    attack_repeat.repeat = true;
    OA_CHECK(!has_phase(attack.feed(attack_repeat), "order.attack", Phase::repeated));
    const Fed attack_burst = attack.advance(1000);
    OA_CHECK(count_phase(attack_burst, "order.attack", Phase::repeated) == 0);

    // With neither delay nor rate set, the platform's repeats pass through.
    Dispatcher system = open_dispatcher(resolved, map);
    (void)system.feed(named_event("Right", true, 0));
    InputEvent system_repeat = named_event("Right", true, 400);
    system_repeat.repeat = true;
    OA_CHECK(has_phase(system.feed(system_repeat), "camera.pan-right", Phase::repeated));
}

/// A mouse button and the wheel are delivered at once while slow and bounce keys wait.
void pointer_not_held(const Resolved& resolved, const KeyboardMap& map) {
    Timings timings;
    timings.slow_keys_ms = 300;
    timings.bounce_keys_ms = 300;
    Dispatcher pointer = open_dispatcher(resolved, map);
    pointer.set_timings(timings);

    InputEvent button;
    button.kind = InputKind::button;
    button.down = true;
    button.button = 1;
    button.time_ms = 0;
    const Fed click = pointer.feed(button);
    OA_CHECK(click.legacy == LegacyGate::deliver);
    OA_CHECK(pointer.advance(1000).events.empty());

    InputEvent wheel;
    wheel.kind = InputKind::wheel;
    wheel.down = true;
    wheel.wheel_y = 1;
    wheel.time_ms = 0;
    const Fed turned = pointer.feed(wheel);
    OA_CHECK(turned.legacy == LegacyGate::deliver);
    OA_CHECK(pointer.advance(1000).events.empty());
}

/// Typed text is delivered at once, and does not fire later.
void text_not_held(const Resolved& resolved, const KeyboardMap& map) {
    Dispatcher typing = open_dispatcher(resolved, map);
    Timings timings;
    timings.slow_keys_ms = 300;
    typing.set_timings(timings);
    set_text_entry(typing, true);
    const Fed typed = typing.feed(letter_event(place_of("KeyA"), 'a', true, 0));
    OA_CHECK(typed.legacy == LegacyGate::deliver);
    OA_CHECK(!has_phase(typed, "order.attack", Phase::pressed));
    const Fed later = typing.advance(400);
    OA_CHECK(!has_phase(later, "order.attack", Phase::pressed));
    OA_CHECK(later.admitted.empty());
}

/// A new binding list forgets a key that was held back.
void bindings_forget_a_wait(const Resolved& resolved, const KeyboardMap& map) {
    Dispatcher attack = open_dispatcher(resolved, map);
    Timings timings;
    timings.slow_keys_ms = 300;
    attack.set_timings(timings);
    OA_CHECK(attack.feed(letter_event(place_of("KeyA"), 'a', true, 0)).legacy == LegacyGate::hold);
    attack.set_bindings(resolved);
    attack.set_timings(timings);
    const Fed fed = attack.advance(1000);
    OA_CHECK(fed.events.empty());
    OA_CHECK(fed.admitted.empty());
}

/// `clamped` holds every limit, and zero stays zero where it means the system's.
void limits() {
    Timings high;
    high.slow_keys_ms = 5000;
    high.bounce_keys_ms = 5000;
    high.double_press_ms = 5000;
    high.long_press_ms = 5000;
    high.repeat_delay_ms = 5000;
    high.repeat_rate = 100;
    const Timings held = keys::clamped(high);
    OA_CHECK(held.slow_keys_ms == keys::slow_keys_most);
    OA_CHECK(held.bounce_keys_ms == keys::bounce_keys_most);
    OA_CHECK(held.double_press_ms == keys::double_press_most);
    OA_CHECK(held.long_press_ms == keys::long_press_most);
    OA_CHECK(held.repeat_delay_ms == keys::repeat_delay_most);
    OA_CHECK(held.repeat_rate == keys::repeat_rate_most);

    Timings low;
    low.double_press_ms = 1;
    low.long_press_ms = 0;
    low.repeat_delay_ms = 1;
    low.repeat_rate = 1;
    const Timings raised = keys::clamped(low);
    OA_CHECK(raised.slow_keys_ms == 0);
    OA_CHECK(raised.bounce_keys_ms == 0);
    OA_CHECK(raised.double_press_ms == keys::double_press_least);
    OA_CHECK(raised.long_press_ms == keys::long_press_least);
    OA_CHECK(raised.repeat_delay_ms == keys::repeat_delay_least);
    OA_CHECK(raised.repeat_rate == keys::repeat_rate_least);

    Timings system;
    const Timings kept = keys::clamped(system);
    OA_CHECK(kept.double_press_ms == 0);
    OA_CHECK(kept.repeat_delay_ms == 0);
    OA_CHECK(kept.repeat_rate == 0);
    OA_CHECK(kept.long_press_ms == 500);

    OA_CHECK(keys::drag_click_ticks(32) == 0x19);
    OA_CHECK(keys::drag_click_ticks(96) == 75);
    const Fed fresh;
    OA_CHECK(fresh.legacy == LegacyGate::deliver);
}

} // namespace

int main() {
    const KeyboardMap map = KeyboardMap::us();
    OA_CHECK(context_bit("play") != 0);
    const Resolved resolved = prepared(map);
    slow_keys(resolved, map);
    bounce_keys(resolved, map);
    double_press(map);
    long_press(map);
    repeat_rate(resolved, map);
    pointer_not_held(resolved, map);
    text_not_held(resolved, map);
    bindings_forget_a_wait(resolved, map);
    limits();
    return 0;
}

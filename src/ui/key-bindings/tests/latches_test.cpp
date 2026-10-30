// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Hold latches and sticky modifiers on synthetic timelines. With neither
// helper set, the dispatcher is not asked to change.

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
#include <utility>
#include <vector>

namespace {

namespace keys = oa::ui::key_bindings;
namespace actions = oa::data::input_actions;

using keys::Contexts;
using keys::Dispatcher;
using keys::Fed;
using keys::InputEvent;
using keys::InputKind;
using keys::KeyboardMap;
using keys::Phase;
using keys::Platform;
using keys::Resolved;

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

/// The position of a UI Events code.
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
/// @return the key
const keys::NamedKeySpec* named_key(std::string_view name) {
    for (const keys::NamedKeySpec& key : keys::named_keys()) {
        if (key.name == name)
            return &key;
    }
    std::fprintf(stderr, "missing named key %.*s\n", static_cast<int>(name.size()), name.data());
    return nullptr;
}

/// A key event for a letter or a modifier position.
///
/// @param scancode the position
/// @param code the key code
/// @param down the key went down
/// @param modifiers the physical modifiers
/// @param time_ms when it happened
/// @return the event
InputEvent
key_event(uint16_t scancode, uint32_t code, bool down, uint8_t modifiers, uint64_t time_ms) {
    InputEvent event;
    event.kind = InputKind::key;
    event.down = down;
    event.scancode = scancode;
    event.key_code = code;
    event.modifiers = modifiers;
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

/// A mouse button.
///
/// @param down the button went down
/// @param time_ms when it happened
/// @return the event
InputEvent click_event(bool down, uint64_t time_ms) {
    InputEvent event;
    event.kind = InputKind::button;
    event.down = down;
    event.button = 1;
    event.time_ms = time_ms;
    return event;
}

/// Whether the result has that action in that phase.
///
/// @param fed the result
/// @param action the action id
/// @param phase the phase
/// @return true when one event says so
bool has_phase(const Fed& fed, std::string_view action, Phase phase) {
    for (const keys::ActionEvent& item : fed.events) {
        if (item.action == action && item.phase == phase)
            return true;
    }
    return false;
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

/// A dispatcher in play and match.
///
/// @param resolved the bindings
/// @param map the US map
/// @return the dispatcher
Dispatcher open_dispatcher(const Resolved& resolved, const KeyboardMap& map) {
    Dispatcher dispatcher;
    dispatcher.set_bindings(Resolved{resolved});
    dispatcher.set_keyboard(map);
    Platform where;
    where.primary_is_meta = true;
    dispatcher.set_platform(where);
    Contexts contexts;
    contexts.active = context_bit("play") | context_bit("match");
    dispatcher.set_contexts(contexts);
    return dispatcher;
}

/// A tap latches the status strip and a second tap lets it go.
void tap_latches_and_second_tap_lets_go(const Resolved& resolved, const KeyboardMap& map) {
    Dispatcher dispatcher = open_dispatcher(resolved, map);
    dispatcher.set_toggle({"game.status-strip"});
    const Fed down = dispatcher.feed(named_event("Space", true, 1000));
    const Fed up = dispatcher.feed(named_event("Space", false, 1100));
    OA_CHECK(has_phase(down, "game.status-strip", Phase::pressed));
    OA_CHECK(has_phase(up, "game.status-strip", Phase::released));
    OA_CHECK(dispatcher.latches().latched("game.status-strip"));
    OA_CHECK(dispatcher.latches().shown().size() == 1);
    (void)dispatcher.feed(named_event("Space", true, 2000));
    (void)dispatcher.feed(named_event("Space", false, 2100));
    OA_CHECK(!dispatcher.latches().latched("game.status-strip"));
    OA_CHECK(dispatcher.latches().shown().empty());
}

/// A hold past the long-press time is a plain hold and leaves the latch off.
void hold_past_long_press_stays_off(const Resolved& resolved, const KeyboardMap& map) {
    Dispatcher dispatcher = open_dispatcher(resolved, map);
    dispatcher.set_toggle({"game.status-strip"});
    const uint32_t long_press = dispatcher.timings().long_press_ms;
    const Fed down = dispatcher.feed(named_event("Space", true, 1000));
    OA_CHECK(dispatcher.latches().held("game.status-strip"));
    OA_CHECK(!dispatcher.latches().latched("game.status-strip"));
    const Fed up = dispatcher.feed(named_event("Space", false, 1000 + long_press));
    OA_CHECK(has_phase(down, "game.status-strip", Phase::pressed));
    OA_CHECK(has_phase(up, "game.status-strip", Phase::released));
    OA_CHECK(!dispatcher.latches().latched("game.status-strip"));
    OA_CHECK(!dispatcher.latches().active("game.status-strip"));
}

/// A hold used by a click while it is held does not latch.
void hold_used_by_click_does_not_latch(const Resolved& resolved, const KeyboardMap& map) {
    Dispatcher dispatcher = open_dispatcher(resolved, map);
    dispatcher.set_toggle({"game.status-strip"});
    (void)dispatcher.feed(named_event("Space", true, 1000));
    (void)dispatcher.feed(click_event(true, 1100));
    (void)dispatcher.feed(click_event(false, 1110));
    (void)dispatcher.feed(named_event("Space", false, 1200));
    OA_CHECK(!dispatcher.latches().latched("game.status-strip"));
    OA_CHECK(!dispatcher.latches().held("game.status-strip"));
}

/// An action that is not in the toggle list never latches.
void action_not_in_toggle_never_latches(const Resolved& resolved, const KeyboardMap& map) {
    Dispatcher dispatcher = open_dispatcher(resolved, map);
    dispatcher.set_toggle({"order.queue-modifier"});
    const Fed down = dispatcher.feed(named_event("Space", true, 1000));
    const Fed up = dispatcher.feed(named_event("Space", false, 1100));
    OA_CHECK(has_phase(down, "game.status-strip", Phase::pressed));
    OA_CHECK(has_phase(up, "game.status-strip", Phase::released));
    OA_CHECK(!dispatcher.latches().latched("game.status-strip"));
    OA_CHECK(!dispatcher.latches().active("game.status-strip"));
}

/// A non-latchable action named in the toggle list never latches.
void non_latchable_never_latches(const Resolved& resolved, const KeyboardMap& map) {
    Dispatcher dispatcher = open_dispatcher(resolved, map);
    dispatcher.set_toggle({"select.all", "order.attack"});
    const uint16_t key_a = place_of("KeyA");
    const Fed down = dispatcher.feed(key_event(key_a, static_cast<uint32_t>('a'), true, 0, 1000));
    (void)dispatcher.feed(key_event(key_a, static_cast<uint32_t>('a'), false, 0, 1100));
    OA_CHECK(has_phase(down, "order.attack", Phase::pressed));
    OA_CHECK(!dispatcher.latches().latched("select.all"));
    OA_CHECK(!dispatcher.latches().latched("order.attack"));
    OA_CHECK(dispatcher.latches().shown().empty());
}

/// A tap arms a modifier, the next chord carries it, and the armed one lets go.
void sticky_tap_arms_next_chord(const Resolved& resolved, const KeyboardMap& map) {
    Dispatcher dispatcher = open_dispatcher(resolved, map);
    dispatcher.set_sticky_modifiers(true);
    const uint16_t control = place_of("ControlLeft");
    const uint16_t key_a = place_of("KeyA");
    (void)dispatcher.feed(key_event(control, 0, true, 0, 1000));
    const Fed up = dispatcher.feed(key_event(control, 0, false, 0, 1100));
    OA_CHECK(!has_phase(up, "order.force-modifier", Phase::active));
    OA_CHECK(dispatcher.sticky().armed() == keys::ctrl);
    OA_CHECK(dispatcher.sticky().locked() == 0);
    OA_CHECK(dispatcher.sticky().mods() == keys::ctrl);
    const Fed letter = dispatcher.feed(key_event(key_a, static_cast<uint32_t>('a'), true, 0, 1200));
    OA_CHECK(has_phase(letter, "select.all", Phase::pressed));
    OA_CHECK(dispatcher.sticky().mods() == 0);
    OA_CHECK(dispatcher.sticky().shown().empty());
}

/// Two taps within the double-press time lock until a third tap.
void sticky_double_tap_locks(const Resolved& resolved, const KeyboardMap& map) {
    Dispatcher dispatcher = open_dispatcher(resolved, map);
    dispatcher.set_sticky_modifiers(true);
    const uint32_t window = dispatcher.timings().double_press_ms;
    const uint16_t control = place_of("ControlLeft");
    const uint16_t key_a = place_of("KeyA");
    (void)dispatcher.feed(key_event(control, 0, true, 0, 1000));
    (void)dispatcher.feed(key_event(control, 0, false, 0, 1100));
    (void)dispatcher.feed(key_event(control, 0, true, 0, 1100 + window / 2));
    (void)dispatcher.feed(key_event(control, 0, false, 0, 1100 + window / 2 + 40));
    OA_CHECK(dispatcher.sticky().locked() == keys::ctrl);
    OA_CHECK(dispatcher.sticky().armed() == 0);
    OA_CHECK(dispatcher.sticky().shown().size() == 1);
    OA_CHECK(dispatcher.sticky().shown().front() == keys::ctrl);
    const Fed first = dispatcher.feed(key_event(key_a, static_cast<uint32_t>('a'), true, 0, 3000));
    (void)dispatcher.feed(key_event(key_a, static_cast<uint32_t>('a'), false, 0, 3010));
    OA_CHECK(has_phase(first, "select.all", Phase::pressed));
    OA_CHECK(dispatcher.sticky().locked() == keys::ctrl);
    const Fed second = dispatcher.feed(key_event(key_a, static_cast<uint32_t>('a'), true, 0, 4000));
    OA_CHECK(has_phase(second, "select.all", Phase::pressed));
    OA_CHECK(dispatcher.sticky().locked() == keys::ctrl);
    (void)dispatcher.feed(key_event(control, 0, true, 0, 5000));
    (void)dispatcher.feed(key_event(control, 0, false, 0, 5100));
    OA_CHECK(dispatcher.sticky().mods() == 0);
}

/// A second tap outside the double-press time lets an armed modifier go.
void sticky_tap_outside_window_lets_go(const Resolved& resolved, const KeyboardMap& map) {
    Dispatcher dispatcher = open_dispatcher(resolved, map);
    dispatcher.set_sticky_modifiers(true);
    const uint32_t window = dispatcher.timings().double_press_ms;
    const uint16_t alt = place_of("AltLeft");
    (void)dispatcher.feed(key_event(alt, 0, true, 0, 1000));
    (void)dispatcher.feed(key_event(alt, 0, false, 0, 1100));
    OA_CHECK(dispatcher.sticky().armed() == keys::alt);
    (void)dispatcher.feed(key_event(alt, 0, true, 0, 1100 + window + 50));
    (void)dispatcher.feed(key_event(alt, 0, false, 0, 1100 + window + 80));
    OA_CHECK(dispatcher.sticky().mods() == 0);
}

/// A modifier with another key between the press and the release is plain.
void sticky_key_between_is_plain(const Resolved& resolved, const KeyboardMap& map) {
    Dispatcher dispatcher = open_dispatcher(resolved, map);
    dispatcher.set_sticky_modifiers(true);
    const uint16_t shift = place_of("ShiftLeft");
    const uint16_t key_a = place_of("KeyA");
    (void)dispatcher.feed(key_event(shift, 0, true, 0, 1000));
    OA_CHECK(dispatcher.sticky().mods() == 0);
    const Fed letter = dispatcher.feed(key_event(key_a, static_cast<uint32_t>('a'), true, 0, 1100));
    OA_CHECK(has_phase(letter, "order.attack", Phase::pressed));
    OA_CHECK(has_phase(letter, "select.add-modifier", Phase::active));
    (void)dispatcher.feed(key_event(key_a, static_cast<uint32_t>('a'), false, 0, 1200));
    const Fed up = dispatcher.feed(key_event(shift, 0, false, 0, 1300));
    OA_CHECK(has_phase(up, "select.add-modifier", Phase::inactive));
    OA_CHECK(dispatcher.sticky().mods() == 0);
}

/// A modifier held while a button goes down is a plain modifier.
void sticky_click_is_plain(const Resolved& resolved, const KeyboardMap& map) {
    Dispatcher dispatcher = open_dispatcher(resolved, map);
    dispatcher.set_sticky_modifiers(true);
    const uint16_t shift = place_of("ShiftLeft");
    (void)dispatcher.feed(key_event(shift, 0, true, 0, 1000));
    const Fed click = dispatcher.feed(click_event(true, 1200));
    OA_CHECK(has_phase(click, "select.add-modifier", Phase::active));
    OA_CHECK(has_phase(click, "order.queue-modifier", Phase::active));
    (void)dispatcher.feed(click_event(false, 1210));
    (void)dispatcher.feed(key_event(shift, 0, false, 0, 1400));
    OA_CHECK(dispatcher.sticky().mods() == 0);
    OA_CHECK(dispatcher.sticky().shown().empty());
}

/// release_all lets go of latches and sticky modifiers.
void release_all_empties_both(const Resolved& resolved, const KeyboardMap& map) {
    Dispatcher dispatcher = open_dispatcher(resolved, map);
    dispatcher.set_toggle({"game.status-strip", "order.queue-modifier"});
    dispatcher.set_sticky_modifiers(true);
    (void)dispatcher.feed(named_event("Space", true, 1000));
    (void)dispatcher.feed(named_event("Space", false, 1100));
    const uint16_t control = place_of("ControlLeft");
    (void)dispatcher.feed(key_event(control, 0, true, 0, 2000));
    (void)dispatcher.feed(key_event(control, 0, false, 0, 2100));
    OA_CHECK(dispatcher.latches().latched("game.status-strip"));
    OA_CHECK(dispatcher.sticky().armed() == keys::ctrl);
    const Fed dropped = dispatcher.release_all(3000);
    OA_CHECK(dispatcher.latches().shown().empty());
    OA_CHECK(!dispatcher.latches().active("game.status-strip"));
    OA_CHECK(dispatcher.sticky().mods() == 0);
    OA_CHECK(
        has_phase(dropped, "order.queue-modifier", Phase::inactive) ||
        !dispatcher.latches().latched("order.queue-modifier")
    );
    OA_CHECK(dispatcher.sticky().mods() == 0);
}

/// shown() keeps the order the latches were turned on.
void shown_keeps_turn_on_order(const Resolved& resolved, const KeyboardMap& map) {
    Dispatcher dispatcher = open_dispatcher(resolved, map);
    dispatcher.set_toggle({"game.status-strip", "order.queue-modifier"});
    const uint16_t shift = place_of("ShiftLeft");
    (void)dispatcher.feed(named_event("Space", true, 1000));
    (void)dispatcher.feed(named_event("Space", false, 1100));
    (void)dispatcher.feed(key_event(shift, 0, true, 0, 2000));
    (void)dispatcher.feed(key_event(shift, 0, false, 0, 2100));
    const std::vector<std::string>& first = dispatcher.latches().shown();
    OA_CHECK(first.size() == 2);
    OA_CHECK(first[0] == "game.status-strip");
    OA_CHECK(first[1] == "order.queue-modifier");
    OA_CHECK(dispatcher.role_active("order.queue-modifier", 0, {}));
    (void)dispatcher.feed(named_event("Space", true, 3000));
    (void)dispatcher.feed(named_event("Space", false, 3100));
    const std::vector<std::string>& middle = dispatcher.latches().shown();
    OA_CHECK(middle.size() == 1);
    OA_CHECK(middle[0] == "order.queue-modifier");
    (void)dispatcher.feed(named_event("Space", true, 4000));
    (void)dispatcher.feed(named_event("Space", false, 4100));
    const std::vector<std::string>& again = dispatcher.latches().shown();
    OA_CHECK(again.size() == 2);
    OA_CHECK(again[0] == "order.queue-modifier");
    OA_CHECK(again[1] == "game.status-strip");
}

/// A short Shift tap latches the queue role and a second tap lets it go.
void queue_modifier_tap_latches(const Resolved& resolved, const KeyboardMap& map) {
    Dispatcher dispatcher = open_dispatcher(resolved, map);
    dispatcher.set_toggle({"order.queue-modifier"});
    const uint16_t shift = place_of("ShiftLeft");
    const Fed down = dispatcher.feed(key_event(shift, 0, true, 0, 1000));
    OA_CHECK(has_phase(down, "order.queue-modifier", Phase::active));
    const Fed up = dispatcher.feed(key_event(shift, 0, false, 0, 1100));
    OA_CHECK(!has_phase(up, "order.queue-modifier", Phase::inactive));
    OA_CHECK(dispatcher.latches().latched("order.queue-modifier"));
    OA_CHECK(dispatcher.role_active("order.queue-modifier", 0, {}));
    (void)dispatcher.feed(key_event(shift, 0, true, 0, 2000));
    (void)dispatcher.feed(key_event(shift, 0, false, 0, 2100));
    OA_CHECK(!dispatcher.latches().latched("order.queue-modifier"));
    OA_CHECK(!dispatcher.role_active("order.queue-modifier", 0, {}));
}

} // namespace

int main() {
    const KeyboardMap map = KeyboardMap::us();
    const Resolved resolved = prepared(map);
    tap_latches_and_second_tap_lets_go(resolved, map);
    hold_past_long_press_stays_off(resolved, map);
    hold_used_by_click_does_not_latch(resolved, map);
    action_not_in_toggle_never_latches(resolved, map);
    non_latchable_never_latches(resolved, map);
    sticky_tap_arms_next_chord(resolved, map);
    sticky_double_tap_locks(resolved, map);
    sticky_tap_outside_window_lets_go(resolved, map);
    sticky_key_between_is_plain(resolved, map);
    sticky_click_is_plain(resolved, map);
    release_all_empties_both(resolved, map);
    shown_keeps_turn_on_order(resolved, map);
    queue_modifier_tap_latches(resolved, map);
    return 0;
}

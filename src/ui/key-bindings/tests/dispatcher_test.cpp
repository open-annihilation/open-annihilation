// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The dispatcher on oa-default: one binding for each event, the gestures,
// and a modifier role read from the keys held now rather than from the
// events the dispatcher has seen.
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

using keys::ActionEvent;
using keys::Contexts;
using keys::Dispatcher;
using keys::Fed;
using keys::HeldKeys;
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

/// The index of a context.
///
/// @param name the context id
/// @return the index, or 0 when the table has no such name
uint8_t context_index(std::string_view name) {
    const std::span<const actions::Context> table = actions::contexts();
    for (std::size_t index = 0; index < table.size() && index < 32; ++index) {
        if (table[index].name == name)
            return static_cast<uint8_t>(index);
    }
    return 0;
}

/// The bit of a condition, or 0 when the table has no such name.
///
/// @param name the condition id
/// @return the bit
uint32_t condition_bit(std::string_view name) {
    const std::span<const std::string_view> table = actions::conditions();
    for (std::size_t index = 0; index < table.size() && index < 32; ++index) {
        if (table[index] == name)
            return uint32_t{1} << static_cast<uint32_t>(index);
    }
    std::fprintf(stderr, "missing condition %.*s\n", static_cast<int>(name.size()), name.data());
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
/// @param modifiers the physical modifiers
/// @param time_ms when it happened
/// @return the event
InputEvent
letter_event(uint16_t scancode, uint32_t code, bool down, uint8_t modifiers, uint64_t time_ms) {
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
/// @param modifiers the physical modifiers
/// @param time_ms when it happened
/// @return the event
InputEvent named_event(std::string_view name, bool down, uint8_t modifiers, uint64_t time_ms) {
    InputEvent event;
    const keys::NamedKeySpec* key = named_key(name);
    OA_CHECK(key != nullptr);
    if (key == nullptr)
        return event;
    event.kind = InputKind::key;
    event.down = down;
    event.scancode = key->scancode;
    event.key_code = key->key_code;
    event.modifiers = modifiers;
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
            "  %.*s phase %u context %u\n",
            static_cast<int>(item.action.size()),
            item.action.data(),
            static_cast<unsigned>(item.phase),
            item.context
        );
    }
}

/// Whether the result has that action in that phase.
///
/// @param fed the result
/// @param action the action id
/// @param phase the phase
/// @return true when one event says so
bool has_phase(const Fed& fed, std::string_view action, Phase phase) {
    for (const ActionEvent& item : fed.events) {
        if (item.action == action && item.phase == phase)
            return true;
    }
    return false;
}

/// Requires exactly one event, of that action and phase.
///
/// @param label the case
/// @param fed the result
/// @param action the action id
/// @param phase the phase
void expect_only(const char* label, const Fed& fed, std::string_view action, Phase phase) {
    const bool ok = fed.events.size() == 1 && has_phase(fed, action, phase);
    if (!ok) {
        std::fprintf(stderr, "%s\n", label);
        show(fed);
    }
    OA_CHECK(ok);
}

/// oa-default with the megamap and build-rotation hacks on, on the US map.
///
/// Primary is the command key, so Cmd+/ is an exact `Primary+/`.
///
/// @param map the US map
/// @return the bindings
Resolved prepared(const KeyboardMap& map) {
    keys::ResolveInput input;
    input.keyboard = &map;
    input.platform.primary_is_meta = true;
    input.hacks_on = {"ui.megamap", "units.build-rotation"};
    return keys::resolve(input);
}

/// A dispatcher holding those bindings.
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
    return dispatcher;
}

/// Sets the contexts the next event is judged in.
///
/// @param dispatcher the dispatcher
/// @param active the open context bits
/// @param conditions the conditions that hold
/// @param text_entry whether text is being typed
void use_contexts(
    Dispatcher& dispatcher, uint32_t active, uint32_t conditions, bool text_entry = false
) {
    Contexts contexts;
    contexts.active = active;
    contexts.conditions = conditions;
    contexts.text_entry = text_entry;
    dispatcher.set_contexts(contexts);
}

/// A in play gives pressed order.attack, and so do Shift+A and Alt+A.
/// Ctrl+M gives nothing.
void letters(const Resolved& resolved, const KeyboardMap& map) {
    const uint16_t key_a = place_of("KeyA");
    const uint16_t key_m = place_of("KeyM");
    const uint32_t play = context_bit("play");
    const uint32_t match = context_bit("match");
    const uint32_t developer = condition_bit("developer");
    const uint32_t debug_keys = condition_bit("debug-keys");

    Dispatcher attack = open_dispatcher(resolved, map);
    use_contexts(attack, play, 0);
    expect_only(
        "A in play gives pressed order.attack",
        attack.feed(letter_event(key_a, static_cast<uint32_t>('a'), true, 0, 1)),
        "order.attack",
        Phase::pressed
    );

    Dispatcher shifted = open_dispatcher(resolved, map);
    use_contexts(shifted, play, 0);
    expect_only(
        "Shift+A gives pressed order.attack",
        shifted.feed(letter_event(key_a, static_cast<uint32_t>('a'), true, keys::shift, 1)),
        "order.attack",
        Phase::pressed
    );

    Dispatcher alt = open_dispatcher(resolved, map);
    use_contexts(alt, play, 0);
    expect_only(
        "Alt+A gives pressed order.attack",
        alt.feed(letter_event(key_a, static_cast<uint32_t>('a'), true, keys::alt, 1)),
        "order.attack",
        Phase::pressed
    );

    // Both M bindings would take a plain M here. Extra Ctrl matches neither.
    Dispatcher blocked = open_dispatcher(resolved, map);
    use_contexts(blocked, play | match, developer | debug_keys);
    const Fed none =
        blocked.feed(letter_event(key_m, static_cast<uint32_t>('m'), true, keys::ctrl, 1));
    if (!none.events.empty() || none.bound) {
        std::fprintf(stderr, "Ctrl+M gives nothing\n");
        show(none);
    }
    OA_CHECK(none.events.empty());
    OA_CHECK(!none.bound);
}

/// Shift+Tab gives select.previous-primary. Tab with the megamap on gives
/// megamap.toggle, and with wheel zoom on select.next-primary.
void tabs(const Resolved& resolved, const KeyboardMap& map) {
    const uint32_t play = context_bit("play");
    const uint32_t match = context_bit("match");
    const uint32_t megamap = context_bit("megamap");
    const uint32_t single = condition_bit("single-player");
    const uint32_t selection = condition_bit("selection");
    const uint32_t wheel_zoom = condition_bit("wheel-zoom");
    const uint32_t no_wheel = condition_bit("no-wheel-zoom");

    Dispatcher previous = open_dispatcher(resolved, map);
    use_contexts(previous, play | match, single | selection);
    expect_only(
        "Shift+Tab gives select.previous-primary",
        previous.feed(named_event("Tab", true, keys::shift, 1)),
        "select.previous-primary",
        Phase::pressed
    );

    Dispatcher toggle = open_dispatcher(resolved, map);
    use_contexts(toggle, match | megamap, no_wheel);
    expect_only(
        "Tab with ui.megamap on and no-wheel-zoom gives megamap.toggle",
        toggle.feed(named_event("Tab", true, 0, 1)),
        "megamap.toggle",
        Phase::pressed
    );

    // Both are eligible, and the megamap context is not the one open. The
    // hack's precedence keeps megamap.toggle, reported in match.
    Dispatcher precedence = open_dispatcher(resolved, map);
    use_contexts(precedence, play | match, single | selection | no_wheel);
    const Fed kept = precedence.feed(named_event("Tab", true, 0, 1));
    expect_only("megamap.toggle wins by precedence", kept, "megamap.toggle", Phase::pressed);
    OA_CHECK(!kept.events.empty());
    OA_CHECK(kept.events.front().context == context_index("match"));

    Dispatcher next = open_dispatcher(resolved, map);
    use_contexts(next, play | match, single | selection | wheel_zoom);
    expect_only(
        "wheel zoom on (single player, a selection) gives select.next-primary",
        next.feed(named_event("Tab", true, 0, 1)),
        "select.next-primary",
        Phase::pressed
    );
}

/// Enter in chat gives chat.send, and in play chat.open.
void enter_key(const Resolved& resolved, const KeyboardMap& map) {
    const uint32_t play = context_bit("play");
    const uint32_t match = context_bit("match");
    const uint32_t chat = context_bit("chat");

    Dispatcher sending = open_dispatcher(resolved, map);
    use_contexts(sending, chat | play | match, 0);
    expect_only(
        "Enter in chat gives chat.send",
        sending.feed(named_event("Enter", true, 0, 1)),
        "chat.send",
        Phase::pressed
    );

    Dispatcher opening = open_dispatcher(resolved, map);
    use_contexts(opening, play | match, 0);
    expect_only(
        "Enter in play gives chat.open",
        opening.feed(named_event("Enter", true, 0, 1)),
        "chat.open",
        Phase::pressed
    );
}

/// = gives game.speed-up, and with the debug keys debug.refill.
///
/// The registry names both developer and debug-keys for debug.refill. The
/// console's '=' hotkey checks debug-keys alone; this case sets both, which
/// is what the binding requires.
void equals_key(const Resolved& resolved, const KeyboardMap& map) {
    const uint16_t equal = place_of("Equal");
    const uint32_t match = context_bit("match");
    const uint32_t developer = condition_bit("developer");
    const uint32_t debug_keys = condition_bit("debug-keys");
    const InputEvent down = letter_event(equal, static_cast<uint32_t>('='), true, 0, 1);

    Dispatcher faster = open_dispatcher(resolved, map);
    use_contexts(faster, match, 0);
    expect_only("= gives game.speed-up", faster.feed(down), "game.speed-up", Phase::pressed);

    Dispatcher refill = open_dispatcher(resolved, map);
    use_contexts(refill, match, developer | debug_keys);
    expect_only(
        "with debug-keys = gives debug.refill", refill.feed(down), "debug.refill", Phase::pressed
    );
}

/// With text_entry, A gives nothing and Escape gives chat.cancel.
void typing(const Resolved& resolved, const KeyboardMap& map) {
    const uint32_t play = context_bit("play");
    const uint32_t match = context_bit("match");
    Dispatcher typing_keys = open_dispatcher(resolved, map);
    use_contexts(typing_keys, play | match, 0, true);

    const Fed ignored =
        typing_keys.feed(letter_event(place_of("KeyA"), static_cast<uint32_t>('a'), true, 0, 1));
    if (!ignored.events.empty() || ignored.bound) {
        std::fprintf(stderr, "with text_entry, A gives nothing\n");
        show(ignored);
    }
    OA_CHECK(ignored.events.empty());
    OA_CHECK(!ignored.bound);

    expect_only(
        "with text_entry, Escape gives chat.cancel",
        typing_keys.feed(named_event("Escape", true, 0, 2)),
        "chat.cancel",
        Phase::pressed
    );
}

/// Cmd+/ on macOS with placing active reaches ui.keys-overlay in global.
void command_slash(const Resolved& resolved, const KeyboardMap& map) {
    Dispatcher overlay = open_dispatcher(resolved, map);
    use_contexts(overlay, context_bit("placing") | context_bit("global"), 0);
    InputEvent down;
    down.kind = InputKind::key;
    down.down = true;
    down.scancode = place_of("Slash");
    down.key_code = static_cast<uint32_t>('/');
    down.modifiers = keys::meta;
    down.time_ms = 1;
    const Fed fed = overlay.feed(down);
    if (fed.events.size() != 1 || fed.events.front().action != "ui.keys-overlay" ||
        fed.events.front().context != context_index("global")) {
        std::fprintf(
            stderr, "Cmd+/ on macOS with placing active reaches ui.keys-overlay in global\n"
        );
        show(fed);
    }
    OA_CHECK(fed.events.size() == 1);
    OA_CHECK(fed.events.front().action == "ui.keys-overlay");
    OA_CHECK(fed.events.front().phase == Phase::pressed);
    OA_CHECK(fed.events.front().context == context_index("global"));
}

/// A held Up repeats camera.pan-up and a held A does not repeat.
void repeats(const Resolved& resolved, const KeyboardMap& map) {
    const uint32_t play = context_bit("play");
    const uint32_t match = context_bit("match");

    Dispatcher camera = open_dispatcher(resolved, map);
    use_contexts(camera, match, 0);
    InputEvent down = named_event("Up", true, 0, 1);
    expect_only(
        "Up goes down as camera.pan-up", camera.feed(down), "camera.pan-up", Phase::pressed
    );
    down.repeat = true;
    down.time_ms = 2;
    expect_only(
        "a held Up repeats camera.pan-up", camera.feed(down), "camera.pan-up", Phase::repeated
    );

    Dispatcher attack = open_dispatcher(resolved, map);
    use_contexts(attack, play, 0);
    InputEvent letter = letter_event(place_of("KeyA"), static_cast<uint32_t>('a'), true, 0, 1);
    expect_only("A goes down", attack.feed(letter), "order.attack", Phase::pressed);
    letter.repeat = true;
    letter.time_ms = 2;
    const Fed again = attack.feed(letter);
    if (has_phase(again, "order.attack", Phase::repeated) || !again.events.empty()) {
        std::fprintf(stderr, "a held A does not repeat\n");
        show(again);
    }
    OA_CHECK(again.events.empty());

    // The first event can already be the repeat. It is one step of the
    // repeat action, and a press action still produces nothing.
    Dispatcher speed = open_dispatcher(resolved, map);
    use_contexts(speed, match, 0);
    InputEvent equals;
    equals.kind = InputKind::key;
    equals.down = true;
    equals.repeat = true;
    equals.scancode = keys::scancode_of("Equal").value_or(0);
    equals.time_ms = 1;
    expect_only(
        "a repeated Equals with no earlier down is game.speed-up",
        speed.feed(equals),
        "game.speed-up",
        Phase::repeated
    );
    Dispatcher plain = open_dispatcher(resolved, map);
    use_contexts(plain, play, 0);
    InputEvent attack_repeat =
        letter_event(place_of("KeyA"), static_cast<uint32_t>('a'), true, 0, 1);
    attack_repeat.repeat = true;
    const Fed lone = plain.feed(attack_repeat);
    if (!lone.events.empty()) {
        std::fprintf(stderr, "a repeated A with no earlier down does nothing\n");
        show(lone);
    }
    OA_CHECK(lone.events.empty());
}

/// Space down, Shift down, Space up gives pressed then released for
/// game.status-strip. The plain chord is not a `:hold`, so no held fires.
void status_strip(const Resolved& resolved, const KeyboardMap& map) {
    Dispatcher strip = open_dispatcher(resolved, map);
    use_contexts(strip, context_bit("play") | context_bit("match"), 0);
    const Fed down = strip.feed(named_event("Space", true, 0, 1));
    InputEvent shift = letter_event(place_of("ShiftLeft"), 0, true, 0, 2);
    const Fed shifted = strip.feed(shift);
    const Fed up = strip.feed(named_event("Space", false, keys::shift, 3));

    std::vector<Phase> phases;
    const auto take = [&phases](const Fed& fed) {
        for (const ActionEvent& item : fed.events) {
            if (item.action != "game.status-strip")
                continue;
            phases.push_back(item.phase);
            OA_CHECK(item.phase != Phase::held);
        }
    };
    take(down);
    take(shifted);
    take(up);
    const bool ok =
        phases.size() == 2 && phases[0] == Phase::pressed && phases[1] == Phase::released;
    if (!ok) {
        std::fprintf(
            stderr,
            "Space down, Shift down, Space up gives pressed then released for game.status-strip\n"
        );
        show(down);
        show(shifted);
        show(up);
    }
    OA_CHECK(ok);
    OA_CHECK(has_phase(shifted, "order.queue-modifier", Phase::active));
}

/// The left button with clicks == 2 on the battlefield gives pointer.select-type.
void double_click(const Resolved& resolved, const KeyboardMap& map) {
    Dispatcher pointer = open_dispatcher(resolved, map);
    use_contexts(pointer, context_bit("play"), 0);
    InputEvent down;
    down.kind = InputKind::button;
    down.down = true;
    down.button = 1;
    down.clicks = 2;
    down.time_ms = 1;
    expect_only(
        "the left button with clicks == 2 gives pointer.select-type",
        pointer.feed(down),
        "pointer.select-type",
        Phase::double_press
    );
}

/// A `:hold` chord fires once after 500 ms of advance, not before.
void hold_chord(const KeyboardMap& map) {
    keys::Layout layout;
    layout.id = "hold";
    keys::LayoutBinding row;
    row.action = "game.status-strip";
    keys::ChordError error;
    const std::optional<keys::Chord> chord = keys::parse_chord("Space:hold", error);
    OA_CHECK(chord.has_value());
    if (!chord)
        return;
    row.chords.push_back(*chord);
    layout.bindings.push_back(std::move(row));

    keys::ResolveInput input;
    input.keyboard = &map;
    input.chosen = &layout;
    Dispatcher dispatcher;
    dispatcher.set_keyboard(map);
    dispatcher.set_bindings(keys::resolve(input));
    use_contexts(dispatcher, context_bit("match"), 0);

    const Fed down = dispatcher.feed(named_event("Space", true, 0, 1000));
    OA_CHECK(down.bound);
    OA_CHECK(!has_phase(down, "game.status-strip", Phase::held));
    const Fed early = dispatcher.advance(1499);
    OA_CHECK(early.events.empty());
    expect_only(
        "a :hold chord fires once after 500 ms of advance",
        dispatcher.advance(1500),
        "game.status-strip",
        Phase::held
    );
    const Fed later = dispatcher.advance(2000);
    if (!later.events.empty()) {
        std::fprintf(stderr, "a :hold chord does not fire twice\n");
        show(later);
    }
    OA_CHECK(later.events.empty());
}

/// A key-up never seen down gives nothing.
void unseen_up(const Resolved& resolved, const KeyboardMap& map) {
    Dispatcher dispatcher = open_dispatcher(resolved, map);
    use_contexts(dispatcher, context_bit("play"), 0);
    const Fed fed =
        dispatcher.feed(letter_event(place_of("KeyA"), static_cast<uint32_t>('a'), false, 0, 1));
    if (!fed.events.empty() || fed.bound) {
        std::fprintf(stderr, "a key-up never seen down gives nothing\n");
        show(fed);
    }
    OA_CHECK(fed.events.empty());
    OA_CHECK(!fed.bound);
}

/// Many keys and a button go down and come up, and nothing stays remembered.
///
/// A key that has come up is kept until its double press can no longer happen.
/// Once that window has passed, the dispatcher remembers no key and no button.
void many_then_nothing(const Resolved& resolved, const KeyboardMap& map) {
    Dispatcher dispatcher = open_dispatcher(resolved, map);
    use_contexts(dispatcher, context_bit("play") | context_bit("match"), 0);

    InputEvent button;
    button.kind = InputKind::button;
    button.down = true;
    button.button = 1;
    button.time_ms = 1000;
    (void)dispatcher.feed(button);
    OA_CHECK(dispatcher.remembered() == 1);
    button.down = false;
    button.time_ms = 1100;
    (void)dispatcher.feed(button);
    OA_CHECK(dispatcher.remembered() == 0);

    constexpr uint16_t first = 1;
    constexpr uint16_t last = 40;
    for (uint16_t scancode = first; scancode <= last; ++scancode) {
        (void)dispatcher.feed(letter_event(scancode, 0, true, 0, 1000));
        (void)dispatcher.feed(letter_event(scancode, 0, false, 0, 1100));
    }
    OA_CHECK(dispatcher.remembered() == static_cast<std::size_t>(last - first + 1));
    (void)dispatcher.advance(1000 + dispatcher.timings().double_press_ms + 1);
    OA_CHECK(dispatcher.remembered() == 0);
}

/// A new binding list drops keys that are still down, and makes no release.
void re_resolve_drops_held_keys(const Resolved& resolved, const KeyboardMap& map) {
    Dispatcher dispatcher = open_dispatcher(resolved, map);
    use_contexts(dispatcher, context_bit("play") | context_bit("match"), 0);
    const uint16_t key_a = place_of("KeyA");
    const Fed down =
        dispatcher.feed(letter_event(key_a, static_cast<uint32_t>('a'), true, 0, 1000));
    OA_CHECK(has_phase(down, "order.attack", Phase::pressed));
    OA_CHECK(dispatcher.remembered() == 1);
    dispatcher.set_bindings(Resolved{resolved});
    OA_CHECK(dispatcher.remembered() == 0);
    const Fed up = dispatcher.feed(letter_event(key_a, static_cast<uint32_t>('a'), false, 0, 1100));
    if (!up.events.empty() || up.bound) {
        std::fprintf(stderr, "re-resolving drops a held key without a release\n");
        show(up);
    }
    OA_CHECK(up.events.empty());
    OA_CHECK(!up.bound);
    OA_CHECK(dispatcher.advance(10000).events.empty());
    OA_CHECK(dispatcher.remembered() == 0);
}

/// release_all after Space and Shift went down gives released game.status-strip
/// and inactive order.queue-modifier.
void release(const Resolved& resolved, const KeyboardMap& map) {
    Dispatcher dispatcher = open_dispatcher(resolved, map);
    use_contexts(dispatcher, context_bit("play") | context_bit("match"), 0);
    (void)dispatcher.feed(named_event("Space", true, 0, 1));
    (void)dispatcher.feed(letter_event(place_of("ShiftLeft"), 0, true, 0, 2));
    const Fed fed = dispatcher.release_all(3);
    const bool ok = has_phase(fed, "game.status-strip", Phase::released) &&
                    has_phase(fed, "order.queue-modifier", Phase::inactive);
    if (!ok) {
        std::fprintf(
            stderr,
            "release_all after Space and Shift went down gives released game.status-strip and "
            "inactive order.queue-modifier\n"
        );
        show(fed);
    }
    OA_CHECK(ok);
}

/// What one role test reports as held.
struct ReportedKeys {
    bool shift{};          ///< ShiftLeft is held
    uint16_t shift_left{}; ///< ShiftLeft's position
};

/// Reports ShiftLeft when the test says it is held.
///
/// @param context a `ReportedKeys`
/// @param scancode the position
/// @return true while that test holds ShiftLeft
bool shift_down(const void* context, uint16_t scancode) {
    const auto* reported = static_cast<const ReportedKeys*>(context);
    return reported != nullptr && reported->shift && scancode == reported->shift_left;
}

/// role_active for order.queue-modifier follows Shift, from the modifiers or
/// from the keys the caller reports, not from a Shift event the dispatcher saw.
void queue_role(const Resolved& resolved, const KeyboardMap& map) {
    Dispatcher dispatcher = open_dispatcher(resolved, map);
    ReportedKeys reported;
    reported.shift_left = place_of("ShiftLeft");
    HeldKeys keys;
    keys.context = &reported;
    keys.down = shift_down;

    OA_CHECK(dispatcher.role_active("order.queue-modifier", keys::shift, keys));
    reported.shift = false;
    OA_CHECK(!dispatcher.role_active("order.queue-modifier", 0, keys));
    reported.shift = true;
    OA_CHECK(dispatcher.role_active("order.queue-modifier", 0, keys));

    reported.shift = false;
    use_contexts(dispatcher, context_bit("play"), 0);
    (void)dispatcher.feed(letter_event(reported.shift_left, 0, true, 0, 1));
    OA_CHECK(!dispatcher.role_active("order.queue-modifier", 0, keys));
}

} // namespace

int main() {
    const KeyboardMap map = KeyboardMap::us();
    OA_CHECK(context_bit("play") != 0);
    OA_CHECK(context_bit("global") != 0);
    OA_CHECK(condition_bit("debug-keys") != 0);
    const Resolved resolved = prepared(map);
    letters(resolved, map);
    tabs(resolved, map);
    enter_key(resolved, map);
    equals_key(resolved, map);
    typing(resolved, map);
    command_slash(resolved, map);
    repeats(resolved, map);
    status_strip(resolved, map);
    double_click(resolved, map);
    hold_chord(map);
    unseen_up(resolved, map);
    many_then_nothing(resolved, map);
    re_resolve_drops_held_keys(resolved, map);
    release(resolved, map);
    queue_role(resolved, map);
    return oa::test::check_exit_status();
}

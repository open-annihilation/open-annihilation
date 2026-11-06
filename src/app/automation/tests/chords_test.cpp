// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A chord and an action id as key events. The action uses the registry's
// own default, not a player's layout. A mouse binding is refused. The five
// chords the automation client's action request uses stand in only when the
// registry gives no chord.
#include "oa/app/automation/chords.hpp"
#include "oa/ui/key_bindings/key_codes.hpp"

#include "oa/test/check.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace automation = oa::app::automation;
namespace keys = oa::ui::key_bindings;

/// The scancode a key's place has.
///
/// @param place the place, such as KeyA
/// @return the scancode; a missing place fails the check and returns 0
uint32_t place(const char* code) {
    const std::optional<uint16_t> found = keys::scancode_of(code);
    OA_CHECK(found.has_value());
    return found.value_or(0);
}

/// The events of a chord. A failure fails the check.
///
/// @param chord the chord
/// @param phase how it is pressed
/// @return the events; empty when the chord cannot be pressed
std::vector<automation::InputEvent>
pressed(const char* chord, automation::KeyPhase phase = automation::KeyPhase::tap) {
    std::string why;
    const std::optional<std::vector<automation::InputEvent>> events =
        automation::chord_events(chord, phase, 1, why);
    OA_CHECK(events.has_value());
    if (!events)
        return {};
    return *events;
}

/// A tap of A is the A key down, the letter, and the key up.
void check_letter() {
    OA_CHECK(place("KeyA") == 4);
    OA_CHECK(place("ControlLeft") == 224);
    const std::vector<automation::InputEvent> events = pressed("A");
    OA_CHECK(events.size() == 3);
    if (events.size() == 3) {
        OA_CHECK(events[0].kind == automation::InputKind::key_down);
        OA_CHECK(events[0].scancode == place("KeyA"));
        OA_CHECK(events[1].kind == automation::InputKind::text);
        OA_CHECK(events[1].text == "a");
        OA_CHECK(events[2].kind == automation::InputKind::key_up);
        OA_CHECK(events[2].scancode == place("KeyA"));
    }
    const std::vector<automation::InputEvent> held = pressed("A", automation::KeyPhase::press);
    OA_CHECK(held.size() == 1);
    if (held.size() == 1) {
        OA_CHECK(held[0].kind == automation::InputKind::key_down);
        OA_CHECK(held[0].scancode == place("KeyA"));
    }
}

/// Ctrl+A presses Control, then A, and types nothing.
void check_modified() {
    const std::vector<automation::InputEvent> events = pressed("Ctrl+A");
    OA_CHECK(events.size() == 4);
    if (events.size() == 4) {
        OA_CHECK(events[0].kind == automation::InputKind::key_down);
        OA_CHECK(events[0].scancode == place("ControlLeft"));
        OA_CHECK(events[1].kind == automation::InputKind::key_down);
        OA_CHECK(events[1].scancode == place("KeyA"));
        OA_CHECK(events[2].kind == automation::InputKind::key_up);
        OA_CHECK(events[2].scancode == place("KeyA"));
        OA_CHECK(events[3].kind == automation::InputKind::key_up);
        OA_CHECK(events[3].scancode == place("ControlLeft"));
    }
}

/// A chord that cannot be pressed says why.
void check_refused() {
    std::string why = "untouched";
    OA_CHECK(!automation::chord_events("", automation::KeyPhase::tap, 1, why));
    OA_CHECK(why == "chord is missing");
    why = "untouched";
    OA_CHECK(!automation::chord_events("A", automation::KeyPhase::tap, 0, why));
    OA_CHECK(why == "repeat is 1 to 30");
    why = "untouched";
    OA_CHECK(!automation::chord_events("MouseLeft", automation::KeyPhase::tap, 1, why));
    OA_CHECK(why == "the binding is not a key");
}

/// An action id uses the registry's default key. A mouse binding is refused
/// and does not fall through. An unknown id has no chord.
void check_actions() {
    std::string why;
    std::string used;
    const std::optional<std::vector<automation::InputEvent>> attack =
        automation::action_events("order.attack", automation::KeyPhase::tap, 1, used, why);
    OA_CHECK(attack.has_value());
    OA_CHECK(used == "A");
    if (attack && !attack->empty())
        OA_CHECK(attack->front().scancode == place("KeyA"));

    used.clear();
    why.clear();
    const std::optional<std::vector<automation::InputEvent>> all =
        automation::action_events("select.all", automation::KeyPhase::tap, 1, used, why);
    OA_CHECK(all.has_value());
    OA_CHECK(used == "Ctrl+A");
    if (all && all->size() >= 2) {
        OA_CHECK((*all)[0].scancode == place("ControlLeft"));
        OA_CHECK((*all)[1].scancode == place("KeyA"));
    }

    used = "kept";
    why.clear();
    OA_CHECK(!automation::action_events("pointer.select", automation::KeyPhase::tap, 1, used, why));
    OA_CHECK(why == "the binding is not a key");
    OA_CHECK(used == "kept");

    why.clear();
    OA_CHECK(!automation::action_events("not.an.action", automation::KeyPhase::tap, 1, used, why));
    OA_CHECK(why == "chord is missing");
}

/// tap, press and release, and a word that is none of them.
void check_phase() {
    OA_CHECK(automation::key_phase("tap") == automation::KeyPhase::tap);
    OA_CHECK(automation::key_phase("press") == automation::KeyPhase::press);
    OA_CHECK(automation::key_phase("release") == automation::KeyPhase::release);
    OA_CHECK(!automation::key_phase("hold"));
}

} // namespace

int main() {
    check_letter();
    check_modified();
    check_refused();
    check_actions();
    check_phase();
    return oa::test::check_exit_status();
}

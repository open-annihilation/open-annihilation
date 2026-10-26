// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Slow keys, bounce keys and the decision to ignore the platform's repeats.
// The dispatcher fires what this admits. Nothing here reads a clock.
#include "timing.hpp"

namespace oa::ui::key_bindings {
namespace {

/// The clock of one key position. A position past the map shares the unused slot.
///
/// @param clocks the clocks
/// @param scancode the position
/// @return that key's clock
KeyClock& clock_for(KeyClocks& clocks, uint16_t scancode) noexcept {
    const uint16_t index = scancode < scancode_limit ? scancode : uint16_t{0};
    return clocks.keys[index];
}

} // namespace

void clear_clocks(KeyClocks& clocks) noexcept {
    clocks = KeyClocks{};
}

bool custom_repeat(const Timings& timings) noexcept {
    return timings.repeat_delay_ms != 0 && timings.repeat_rate != 0;
}

uint32_t repeat_interval_ms(const Timings& timings) noexcept {
    if (timings.repeat_rate == 0)
        return 0;
    return 1000U / timings.repeat_rate;
}

KeyFate
consider_key(KeyClocks& clocks, const Timings& timings, bool text_entry, const InputEvent& event) {
    if (event.kind != InputKind::key || text_entry)
        return KeyFate::through;
    KeyClock& clock = clock_for(clocks, event.scancode);
    if (event.repeat) {
        if (clock.waiting || clock.swallow_up)
            return KeyFate::drop;
        if (custom_repeat(timings))
            return KeyFate::ignore_repeat;
        return KeyFate::through;
    }
    if (!event.down) {
        if (clock.waiting) {
            clock.waiting = false;
            return KeyFate::drop;
        }
        if (clock.swallow_up) {
            clock.swallow_up = false;
            return KeyFate::drop;
        }
        if (clock.down_accepted) {
            clock.have_up = true;
            clock.last_up_ms = event.time_ms;
            clock.down_accepted = false;
        }
        clock.repeat_on = false;
        return KeyFate::through;
    }
    // A second down while a dropped press's release is still due is dropped
    // with it. The release that does arrive must not move the window.
    if (clock.swallow_up)
        return KeyFate::drop;
    if (timings.bounce_keys_ms != 0 && clock.have_up && event.time_ms >= clock.last_up_ms &&
        event.time_ms - clock.last_up_ms <= timings.bounce_keys_ms) {
        clock.swallow_up = true;
        clock.waiting = false;
        clock.repeat_on = false;
        return KeyFate::drop;
    }
    if (timings.slow_keys_ms != 0) {
        clock.waiting = true;
        clock.held = event;
        clock.admit_ms = event.time_ms + timings.slow_keys_ms;
        clock.repeat_on = false;
        return KeyFate::hold;
    }
    return KeyFate::through;
}

void note_accepted_down(KeyClocks& clocks, const Timings& timings, const InputEvent& event) {
    if (event.kind != InputKind::key || !event.down || event.repeat)
        return;
    KeyClock& clock = clock_for(clocks, event.scancode);
    clock.down_accepted = true;
    clock.waiting = false;
    if (!custom_repeat(timings)) {
        clock.repeat_on = false;
        return;
    }
    clock.repeat_on = true;
    clock.next_repeat_ms = event.time_ms + timings.repeat_delay_ms;
}

std::vector<InputEvent> take_admissions(KeyClocks& clocks, uint64_t now_ms) {
    std::vector<InputEvent> admitted;
    for (KeyClock& clock : clocks.keys) {
        if (!clock.waiting || now_ms < clock.admit_ms)
            continue;
        clock.waiting = false;
        InputEvent event = clock.held;
        event.down = true;
        event.repeat = false;
        event.time_ms = clock.admit_ms;
        admitted.push_back(event);
    }
    return admitted;
}

} // namespace oa::ui::key_bindings

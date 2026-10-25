// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Per-key clocks for slow keys, bounce keys and the player's own repeat.
// The dispatcher owns the clocks. This header is private to the library:
// two translation units share it, and it reads no clock of its own.
#pragma once

#include "oa/ui/key_bindings/dispatcher.hpp"

#include <array>
#include <cstdint>
#include <vector>

namespace oa::ui::key_bindings {

/// What `feed` does with one key before the bindings see it.
enum class KeyFate : uint8_t {
    through,       ///< the bindings see the key, and the app delivers it
    hold,          ///< the key is held back until it has been down long enough
    drop,          ///< the key and its matching release are ignored
    ignore_repeat, ///< a platform repeat is not a `repeated` event; the app still delivers it
};

/// One key's slow-key wait, bounce window and repeat schedule.
struct KeyClock {
    bool waiting{};            ///< a down is held back
    InputEvent held{};         ///< the down that is held back
    uint64_t admit_ms{};       ///< when that down counts, in milliseconds
    bool have_up{};            ///< an accepted release has been seen
    uint64_t last_up_ms{};     ///< when the last accepted release happened
    bool swallow_up{};         ///< the next release belongs to a press that was dropped
    bool down_accepted{};      ///< the current down was accepted, so its release starts a window
    bool repeat_on{};          ///< the player's own repeat is running for this key
    uint64_t next_repeat_ms{}; ///< when the next repeat is due
};

/// A clock for every key position. Position 0 is the unused slot, as the
/// dispatcher's own slots use it.
struct KeyClocks {
    std::array<KeyClock, scancode_limit> keys{};
};

/// Forgets every clock. A held-back key does not fire afterwards.
///
/// @param clocks the clocks
void clear_clocks(KeyClocks& clocks) noexcept;

/// Decides whether a key reaches the bindings.
///
/// Slow keys and bounce keys apply only to keys, and not while text is being
/// typed. A release before the slow-key wait drops the down and the release.
/// A down within the bounce window of the last accepted release is dropped,
/// and that down's release does not move the window.
///
/// @param clocks the clocks
/// @param timings the timings in use
/// @param text_entry true while a field has the keyboard
/// @param event the key
/// @return what `feed` does with it
[[nodiscard]] KeyFate
consider_key(KeyClocks& clocks, const Timings& timings, bool text_entry, const InputEvent& event);

/// Remembers an accepted key down, and arms the player's repeat when both a
/// delay and a rate are set.
///
/// @param clocks the clocks
/// @param timings the timings in use
/// @param event the down that the bindings accepted
void note_accepted_down(KeyClocks& clocks, const Timings& timings, const InputEvent& event);

/// Takes every held-back down whose time has come, each once.
///
/// The event's time is the time it was due, not the time `advance` ran.
///
/// @param clocks the clocks
/// @param now_ms the time, in milliseconds
/// @return the downs, in position order
[[nodiscard]] std::vector<InputEvent> take_admissions(KeyClocks& clocks, uint64_t now_ms);

/// Reports whether the player's own repeat is in use.
///
/// Both the delay and the rate have to be set. Either one at zero leaves the
/// platform's repeats to pass through.
///
/// @param timings the timings in use
/// @return true when both are set
[[nodiscard]] bool custom_repeat(const Timings& timings) noexcept;

/// How many milliseconds pass between the player's repeats.
///
/// @param timings the timings in use; `repeat_rate` is not zero
/// @return `1000 / repeat_rate`, or zero when the rate is zero
[[nodiscard]] uint32_t repeat_interval_ms(const Timings& timings) noexcept;

} // namespace oa::ui::key_bindings

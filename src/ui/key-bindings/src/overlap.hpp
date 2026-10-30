// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The clash rule, shared by resolution and by the clash list. Not a public
// header: the public functions are in layout.hpp.
#pragma once

#include "oa/ui/key_bindings/layout.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::key_bindings {

/// How two chords on two actions relate. A shadow is not a clash.
enum class Overlap : uint8_t {
    none,
    clash,
    taken_by_hack,
    while_overlay,
    while_conditions,
};

/// One side of a comparison. The id view must outlive the call.
struct Side {
    std::string_view id{};
    uint32_t contexts{};
    uint32_t when{};
    int precedence{};
    oa::data::input_actions::Kind kind{oa::data::input_actions::Kind::press};
    oa::data::input_actions::Target target{oa::data::input_actions::Target::none};
};

/// How a comparison reads the modifiers of two chords.
enum class ChordReading : uint8_t {
    /// Primary, Ctrl and Meta are three modifiers, as a layout writes them.
    /// Resolution reads chords this way, so the chords a key press finds stay
    /// the ones the layouts wrote.
    as_written,
    /// Primary is the key it means on the input's platform, as a key press
    /// reads it. Every clash check reads chords this way.
    as_pressed,
};

/// The relation of two chords, and which side wins when one shadows the other.
struct Relation {
    Overlap overlap{Overlap::none};
    bool first_wins{}; ///< true when the first side is the one that takes the key
};

/// How far above a base action this action sits while the named hacks are on.
///
/// A pure hack action is above an action a hack changes, which is above a
/// base action. The order of `hacks_on` does not matter.
///
/// @param action the registry action
/// @param hacks_on the hacks that are on
/// @return 2 for a pure hack action, 1 when a hack changes it, otherwise 0
[[nodiscard]] int action_precedence(
    const oa::data::input_actions::Action& action, const std::vector<std::string>& hacks_on
);

/// Compares two bound chords under the clash rule.
///
/// @param first one action
/// @param first_chord its chord
/// @param second the other action
/// @param second_chord its chord
/// @param input the scheme, the keyboard and the platform
/// @param reading whether Primary is compared as written or as pressed
/// @return the relation
[[nodiscard]] Relation relate(
    const Side& first,
    const Chord& first_chord,
    const Side& second,
    const Chord& second_chord,
    const ResolveInput& input,
    ChordReading reading
);

} // namespace oa::ui::key_bindings

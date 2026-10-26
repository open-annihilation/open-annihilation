// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Which bindings share a layer, and which of those are clashes. A hack's
// precedence, an overlay and a stricter set of conditions are shadows.

#include "oa/ui/key_bindings/layout.hpp"

#include "overlap.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::key_bindings {
namespace {

namespace actions = oa::data::input_actions;

using actions::Action;
using actions::Context;
using actions::Kind;
using actions::Target;

/// Folds an ASCII letter to lower case. Any other character is unchanged.
///
/// @param character the character
/// @return the folded character
char32_t fold_ascii(char32_t character) {
    if (character >= U'A' && character <= U'Z')
        return static_cast<char32_t>(character - U'A' + U'a');
    return character;
}

/// Reports whether one context meets another.
///
/// @param first one context
/// @param second the other context
/// @return true when the two share a layer
bool pair_meets(const Context& first, const Context& second) {
    if (first.name == "global" || second.name == "global")
        return true;
    if (first.overlay || second.overlay)
        return first.overlay && second.overlay && first.name == second.name;
    if (first.name == second.name)
        return true;
    const auto match_layer = [](const Context& context) {
        return context.name == "match" || context.name == "play" || context.name == "watch";
    };
    if (!match_layer(first) || !match_layer(second))
        return false;
    const bool apart = (first.name == "play" && second.name == "watch") ||
                       (first.name == "watch" && second.name == "play");
    return !apart;
}

/// Reports whether an overlay sits over a base context.
///
/// @param overlay the overlay context
/// @param base the other context
/// @param table the context table, so a parent index can be named
/// @return true when the overlay shadows that base context
bool overlay_covers(const Context& overlay, const Context& base, std::span<const Context> table) {
    if (!overlay.overlay || base.overlay)
        return false;
    if (base.name == "global" || base.name == "menus" || base.name == "oa-screens")
        return false;
    if (overlay.parent >= table.size())
        return false;
    const Context& parent = table[overlay.parent];
    if (parent.name == "match")
        return base.name == "match" || base.name == "play" || base.name == "watch";
    if (parent.name == "play")
        return base.name == "play" || base.name == "match";
    if (parent.name == "watch")
        return base.name == "watch" || base.name == "match";
    return base.name == parent.name;
}

/// Reports whether the first side's overlay sits over the second side.
///
/// @param over the side that may be the overlay
/// @param under the side that may be underneath
/// @return true when an overlay context of the first covers a base context of the second
bool overlay_over(const Side& over, const Side& under) {
    const std::span<const Context> table = actions::contexts();
    const uint32_t count = static_cast<uint32_t>(table.size());
    for (uint32_t index = 0; index < count; ++index) {
        if ((over.contexts & (uint32_t{1} << index)) == 0)
            continue;
        if (!table[index].overlay)
            continue;
        for (uint32_t under_index = 0; under_index < count; ++under_index) {
            if ((under.contexts & (uint32_t{1} << under_index)) == 0)
                continue;
            if (overlay_covers(table[index], table[under_index], table))
                return true;
        }
    }
    return false;
}

/// Reports whether the first mask includes every bit of the second and more.
///
/// @param winner the mask that may be stricter
/// @param other the other mask
/// @return true when the winner's conditions include the other's and more
bool stricter(uint32_t winner, uint32_t other) {
    return (winner & other) == other && winner != other;
}

/// The file stem of `gui.<file>.<gadget>`, or empty when the id is not one. A
/// gadget without a name still belongs to its file.
///
/// @param id an action id
/// @return the stem between the two dots
std::string_view gui_file_stem(std::string_view id) {
    constexpr std::string_view prefix = "gui.";
    if (!id.starts_with(prefix))
        return {};
    const std::string_view rest = id.substr(prefix.size());
    const std::size_t dot = rest.find('.');
    if (dot == std::string_view::npos || dot == 0)
        return {};
    return rest.substr(0, dot);
}

/// The bit of a named context, or 0 when the table has no such name.
///
/// @param name the context id
/// @return the bit
uint32_t named_context(std::string_view name) {
    const std::span<const Context> table = actions::contexts();
    for (std::size_t index = 0; index < table.size(); ++index) {
        if (table[index].name == name)
            return uint32_t{1} << static_cast<uint32_t>(index);
    }
    return 0;
}

/// True when two `gui.` actions belong to files that are never on screen together.
///
/// Two buttons of one file share a menu, so they can clash. Two files do not,
/// except the pair a match keeps up at once: the side's order panel and the
/// build menu it is showing. The order file is the play context without the
/// panel bit. The menu on screen has the panel bit set. Two menus, or two
/// order files, are apart.
///
/// @param first one action
/// @param second the other action
/// @return true when a shared key is not a clash
bool gui_files_apart(const Side& first, const Side& second) {
    const std::string_view first_stem = gui_file_stem(first.id);
    const std::string_view second_stem = gui_file_stem(second.id);
    if (first_stem.empty() || second_stem.empty() || first_stem == second_stem)
        return false;
    const uint32_t play = named_context("play");
    const uint32_t panel = named_context("panel");
    const auto order_file = [&](const Side& side) {
        return (side.contexts & play) != 0 && (side.contexts & panel) == 0;
    };
    const auto shown_menu = [&](const Side& side) { return (side.contexts & panel) != 0; };
    const bool together =
        (order_file(first) && shown_menu(second)) || (order_file(second) && shown_menu(first));
    return !together;
}

/// A source that is part of the game even with every hack off.
///
/// @param source one source name
/// @return true for 3.1c, OA or data
bool permanent_source(std::string_view source) {
    return source == "3.1c" || source == "OA" || source == "data";
}

/// Reports whether a hack id is in the list. The list's order is ignored.
///
/// @param hacks_on the hacks that are on
/// @param hack the id
/// @return true when the list holds it
bool hack_listed(const std::vector<std::string>& hacks_on, std::string_view hack) {
    for (const std::string& listed : hacks_on) {
        if (listed == hack)
            return true;
    }
    return false;
}

/// The left button, the one the left-click scheme shares.
///
/// @param chord the chord
/// @return true when it is the left mouse button
bool left_button(const Chord& chord) {
    return chord.kind == TriggerKind::mouse && chord.button == 1;
}

/// Select and command, the pair the pointer scheme decides between.
///
/// @param first one action id
/// @param second the other action id
/// @return true for that pair, in either order
bool select_and_command(std::string_view first, std::string_view second) {
    const bool select_first = first == "pointer.select" && second == "pointer.command";
    const bool command_first = first == "pointer.command" && second == "pointer.select";
    return select_first || command_first;
}

/// Player view and lock view share one double-click the panel decides between.
///
/// Which one runs depends on whether that row is already the view. That is
/// the same shape as select and command: one gesture, the screen decides,
/// and neither chord is removed.
///
/// @param first one action id
/// @param second the other action id
/// @return true for that pair, in either order
bool resource_row_pair(std::string_view first, std::string_view second) {
    const bool view_first = first == "watch.player-view" && second == "watch.lock-view";
    const bool lock_first = first == "watch.lock-view" && second == "watch.player-view";
    return view_first || lock_first;
}

/// The kind and target of a resolved action. A made action is a press with no target.
///
/// @param bound the action
/// @return the side, apart from precedence
Side side_of(const Bound& bound, int precedence) {
    Side side;
    side.id = bound.id;
    side.contexts = bound.contexts;
    side.when = bound.when;
    side.precedence = precedence;
    side.kind = bound.entry != nullptr ? bound.entry->kind : Kind::press;
    side.target = bound.entry != nullptr ? bound.entry->target : Target::none;
    return side;
}

/// Precedence of a resolved action, from its registry row and the hacks.
///
/// @param bound the action
/// @param hacks_on the hacks that are on
/// @return the precedence
int precedence_of(const Bound& bound, const std::vector<std::string>& hacks_on) {
    if (bound.entry == nullptr)
        return 0;
    return action_precedence(*bound.entry, hacks_on);
}

} // namespace

bool layers_meet(uint32_t contexts_a, uint32_t contexts_b) {
    if (contexts_a == 0 || contexts_b == 0)
        return false;
    const std::span<const Context> table = actions::contexts();
    const uint32_t count = static_cast<uint32_t>(table.size());
    for (uint32_t first = 0; first < count; ++first) {
        if ((contexts_a & (uint32_t{1} << first)) == 0)
            continue;
        for (uint32_t second = 0; second < count; ++second) {
            if ((contexts_b & (uint32_t{1} << second)) == 0)
                continue;
            if (pair_meets(table[first], table[second]))
                return true;
        }
    }
    return false;
}

bool conditions_meet(uint32_t when_a, uint32_t when_b) {
    return !actions::exclusive(when_a, when_b);
}

bool same_key(const Chord& first, const Chord& second, const KeyboardMap* keyboard) {
    if (first.modifiers != second.modifiers || first.gesture != second.gesture)
        return false;
    if (first == second)
        return true;
    if (keyboard == nullptr)
        return false;
    const Chord* letter = nullptr;
    const Chord* place = nullptr;
    if (first.kind == TriggerKind::character && second.kind == TriggerKind::place) {
        letter = &first;
        place = &second;
    } else if (second.kind == TriggerKind::character && first.kind == TriggerKind::place) {
        letter = &second;
        place = &first;
    } else {
        return false;
    }
    const char32_t printed = keyboard->printed(place->scancode);
    if (printed == 0)
        return false;
    return fold_ascii(printed) == fold_ascii(letter->character);
}

int action_precedence(const Action& action, const std::vector<std::string>& hacks_on) {
    bool permanent = false;
    bool from_hack = false;
    for (const std::string_view source : action.source) {
        if (permanent_source(source))
            permanent = true;
        else if (hack_listed(hacks_on, source))
            from_hack = true;
    }
    if (!permanent && from_hack)
        return 2;
    for (const actions::HackChange& change : action.hacks) {
        if (hack_listed(hacks_on, change.hack))
            return 1;
    }
    return 0;
}

Relation relate(
    const Side& first,
    const Chord& first_chord,
    const Side& second,
    const Chord& second_chord,
    const ResolveInput& input
) {
    Relation relation;
    if (!same_key(first_chord, second_chord, input.keyboard))
        return relation;
    if (first.target != second.target)
        return relation;
    if (first.kind == Kind::modifier && second.kind == Kind::modifier)
        return relation;
    if (input.scheme == PointerScheme::left_click && select_and_command(first.id, second.id) &&
        left_button(first_chord)) {
        return relation;
    }
    if (resource_row_pair(first.id, second.id))
        return relation;
    if (!conditions_meet(first.when, second.when))
        return relation;
    if (layers_meet(first.contexts, second.contexts)) {
        if (first.precedence != second.precedence) {
            relation.overlap = Overlap::taken_by_hack;
            relation.first_wins = first.precedence > second.precedence;
            return relation;
        }
        if (stricter(first.when, second.when)) {
            relation.overlap = Overlap::while_conditions;
            relation.first_wins = true;
            return relation;
        }
        if (stricter(second.when, first.when)) {
            relation.overlap = Overlap::while_conditions;
            relation.first_wins = false;
            return relation;
        }
        // Two menus are not live together, so their quick keys are not a clash.
        // One file's two buttons still are, and so are the order panel and the
        // build menu a match is showing.
        if (gui_files_apart(first, second))
            return relation;
        relation.overlap = Overlap::clash;
        return relation;
    }
    if (overlay_over(first, second)) {
        relation.overlap = Overlap::while_overlay;
        relation.first_wins = true;
        return relation;
    }
    if (overlay_over(second, first)) {
        relation.overlap = Overlap::while_overlay;
        relation.first_wins = false;
        return relation;
    }
    return relation;
}

namespace {

/// Turns a relation into the shadow the editor lists, or nothing for a clash.
///
/// @param relation the relation
/// @param first the first action id
/// @param first_chord the first chord
/// @param second the second action id
/// @param second_chord the second chord
/// @param[out] listed the shadow, when the relation is one
/// @return true when the relation is a shadow
bool as_shadow(
    const Relation& relation,
    std::string_view first,
    const Chord& first_chord,
    std::string_view second,
    const Chord& second_chord,
    Shadow& listed
) {
    if (relation.overlap != Overlap::taken_by_hack && relation.overlap != Overlap::while_overlay &&
        relation.overlap != Overlap::while_conditions) {
        return false;
    }
    const bool first_wins = relation.first_wins;
    listed.winner = std::string{first_wins ? first : second};
    listed.shadowed = std::string{first_wins ? second : first};
    listed.chord = first_wins ? first_chord : second_chord;
    if (relation.overlap == Overlap::taken_by_hack)
        listed.kind = ShadowKind::taken_by_hack;
    else if (relation.overlap == Overlap::while_overlay)
        listed.kind = ShadowKind::while_overlay;
    else
        listed.kind = ShadowKind::while_conditions;
    return true;
}

} // namespace

std::vector<Clash> find_clashes(const Resolved& resolved, const ResolveInput& input) {
    std::vector<Clash> clashes;
    const std::vector<Bound>& actions = resolved.actions;
    for (std::size_t first = 0; first < actions.size(); ++first) {
        const Bound& lower = actions[first];
        const Side first_side = side_of(lower, precedence_of(lower, input.hacks_on));
        for (std::size_t second = first + 1; second < actions.size(); ++second) {
            const Bound& upper = actions[second];
            const Side second_side = side_of(upper, precedence_of(upper, input.hacks_on));
            for (const BoundChord& first_chord : lower.chords) {
                for (const BoundChord& second_chord : upper.chords) {
                    const Relation relation = relate(
                        first_side, first_chord.chord, second_side, second_chord.chord, input
                    );
                    if (relation.overlap != Overlap::clash)
                        continue;
                    Clash clash;
                    clash.a = lower.id;
                    clash.b = upper.id;
                    clash.chord = first_chord.chord;
                    clashes.push_back(std::move(clash));
                }
            }
        }
    }
    return clashes;
}

std::vector<Clash> clashes_with(
    const Resolved& resolved, const ResolveInput& input, std::string_view action, const Chord& chord
) {
    std::vector<Clash> clashes;
    const Bound* self = nullptr;
    for (const Bound& bound : resolved.actions) {
        if (bound.id == action) {
            self = &bound;
            break;
        }
    }
    Side mine;
    if (self != nullptr) {
        mine = side_of(*self, precedence_of(*self, input.hacks_on));
    } else if (const Action* entry = actions::find(action)) {
        mine.id = entry->id;
        mine.contexts = entry->context;
        mine.when = entry->when;
        mine.precedence = action_precedence(*entry, input.hacks_on);
        mine.kind = entry->kind;
        mine.target = entry->target;
    } else {
        return clashes;
    }
    for (const Bound& bound : resolved.actions) {
        if (bound.id == mine.id)
            continue;
        const Side other = side_of(bound, precedence_of(bound, input.hacks_on));
        for (const BoundChord& held : bound.chords) {
            const Relation relation = relate(mine, chord, other, held.chord, input);
            if (relation.overlap != Overlap::clash)
                continue;
            Clash clash;
            clash.a = std::string{action};
            clash.b = bound.id;
            clash.chord = chord;
            clashes.push_back(std::move(clash));
        }
    }
    return clashes;
}

std::vector<Shadow> shadows(const Resolved& resolved, const ResolveInput& input) {
    std::vector<Shadow> listed;
    const std::vector<Bound>& actions = resolved.actions;
    for (std::size_t first = 0; first < actions.size(); ++first) {
        const Bound& lower = actions[first];
        const Side first_side = side_of(lower, precedence_of(lower, input.hacks_on));
        for (std::size_t second = first + 1; second < actions.size(); ++second) {
            const Bound& upper = actions[second];
            const Side second_side = side_of(upper, precedence_of(upper, input.hacks_on));
            for (const BoundChord& first_chord : lower.chords) {
                for (const BoundChord& second_chord : upper.chords) {
                    const Relation relation = relate(
                        first_side, first_chord.chord, second_side, second_chord.chord, input
                    );
                    Shadow shadow;
                    if (!as_shadow(
                            relation,
                            lower.id,
                            first_chord.chord,
                            upper.id,
                            second_chord.chord,
                            shadow
                        )) {
                        continue;
                    }
                    listed.push_back(std::move(shadow));
                }
            }
        }
    }
    return listed;
}

} // namespace oa::ui::key_bindings

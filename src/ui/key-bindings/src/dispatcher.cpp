// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One binding per event. Overlays are searched from the top of the stack,
// then play or watch together with match, then menus, then Open
// Annihilation's screens, then global. An exact match beats a match that
// has extra Shift, Alt or Meta, including one in a context further up the
// stack. A letter and the place that prints it are the same rank, so a
// hack's precedence and then the narrower conditions decide between them.

#include "oa/ui/key_bindings/dispatcher.hpp"

#include "oa/data/input_actions/actions.hpp"
#include "oa/ui/key_bindings/key_codes.hpp"
#include "timing.hpp"

#include <array>
#include <cstdio>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::key_bindings {
namespace {

namespace actions = oa::data::input_actions;

/// The context table's indexes and bits, read once.
struct Names {
    uint8_t global{};
    uint8_t oa_screens{};
    uint8_t menus{};
    uint8_t match{};
    uint8_t play{};
    uint8_t watch{};
    uint8_t chat{};
    uint32_t global_bit{};
    uint32_t oa_screens_bit{};
    uint32_t menus_bit{};
    uint32_t match_bit{};
    uint32_t play_bit{};
    uint32_t watch_bit{};
    uint32_t chat_bit{};
    uint32_t match_layer{};
};

/// One modifier key's position and the bit it holds.
struct ModifierKey {
    uint16_t scancode{};
    uint8_t bit{};
};

/// One step of the context stack.
struct Group {
    uint8_t context{};
    uint32_t mask{};
    bool text{};
    bool match_pool{};
};

/// A chord that matches the event in the group that took it.
struct Candidate {
    std::size_t action_index{};
    std::size_t chord_index{};
    std::string_view id{};
    Chord chord{};
    int score{};
    int precedence{};
    uint32_t when{};
    actions::Target target{};
    uint8_t context{};
};

/// Which of two candidates wins. A tie is left to registry order.
enum class Which : uint8_t { left, right, tie };

/// An action a down started, released when that key or button goes up.
struct Started {
    std::size_t action_index{};
    std::size_t chord_index{};
    uint8_t context{};
};

/// A `:hold` chord armed on the down and fired once from `advance`.
struct HoldArm {
    std::size_t action_index{};
    std::size_t chord_index{};
    uint8_t context{};
    uint64_t due_ms{};
    bool fired{};
};

/// One key or button the dispatcher has seen go down.
struct Slot {
    bool down{};
    bool saw_up{};
    bool had_binding{};
    uint64_t down_ms{};
    uint8_t down_modifiers{};
    std::vector<Started> started{};
    std::vector<HoldArm> holds{};
    std::vector<std::string> latch_ids{}; ///< latchable actions this down pressed
};

/// A modifier key sticky modifiers are watching, and the latches it pressed.
struct ModifierFinger {
    bool down{};                          ///< the key went down and has not come up
    bool plain{};                         ///< another key or button went down while it was held
    std::vector<std::string> latch_ids{}; ///< latchable modifier roles this key pressed
};

/// A modifier role that is active.
struct LiveRole {
    std::size_t action_index{};
    std::size_t chord_index{};
    uint8_t context{};
};

/// Reads the context indexes from the registry.
///
/// @return the indexes and bits
Names make_names() {
    Names names;
    const std::span<const actions::Context> table = actions::contexts();
    for (std::size_t index = 0; index < table.size() && index < 32; ++index) {
        const auto bit = static_cast<uint32_t>(1u << index);
        const auto at = static_cast<uint8_t>(index);
        const std::string_view name = table[index].name;
        if (name == "global") {
            names.global = at;
            names.global_bit = bit;
        } else if (name == "oa-screens") {
            names.oa_screens = at;
            names.oa_screens_bit = bit;
        } else if (name == "menus") {
            names.menus = at;
            names.menus_bit = bit;
        } else if (name == "match") {
            names.match = at;
            names.match_bit = bit;
        } else if (name == "play") {
            names.play = at;
            names.play_bit = bit;
        } else if (name == "watch") {
            names.watch = at;
            names.watch_bit = bit;
        } else if (name == "chat") {
            names.chat = at;
            names.chat_bit = bit;
        }
    }
    names.match_layer = names.match_bit | names.play_bit | names.watch_bit;
    return names;
}

/// Returns the context indexes.
///
/// @return the indexes
const Names& context_names() {
    static const Names names = make_names();
    return names;
}

/// The modifier keys, looked up once from the position table.
///
/// @return each modifier position and its bit
const std::vector<ModifierKey>& modifier_keys() {
    static const std::vector<ModifierKey> keys = [] {
        std::vector<ModifierKey> out;
        const auto add = [&out](std::string_view code, uint8_t bit) {
            const std::optional<uint16_t> scancode = scancode_of(code);
            if (scancode)
                out.push_back(ModifierKey{*scancode, bit});
        };
        add("ShiftLeft", shift);
        add("ShiftRight", shift);
        add("ControlLeft", ctrl);
        add("ControlRight", ctrl);
        add("AltLeft", alt);
        add("AltRight", alt);
        add("MetaLeft", meta);
        add("MetaRight", meta);
        return out;
    }();
    return keys;
}

/// The modifier bit a key's own position holds.
///
/// @param scancode the position number
/// @return the bit, or 0 when the key is not a modifier
uint8_t bit_for_scancode(uint16_t scancode) {
    for (const ModifierKey& key : modifier_keys()) {
        if (key.scancode == scancode)
            return key.bit;
    }
    return 0;
}

/// How far above a base action this binding sits.
///
/// A pure hack action is above an action a hack changes, which is above a
/// base action. `Bound::hack` is set only when the hack is on.
///
/// @param bound the binding
/// @return 2, 1 or 0
int precedence_of(const Bound& bound) {
    if (bound.entry == nullptr || !bound.hack)
        return 0;
    for (const std::string_view source : bound.entry->source) {
        if (source == "3.1c" || source == "OA" || source == "data")
            return 1;
    }
    return 2;
}

/// The extra rank a place or NumpadEnter has over a character.
///
/// @param chord the chord
/// @return 1 when the chord matches by position, otherwise 0
int position_bonus(const Chord& chord) {
    const bool by_position = chord.kind == TriggerKind::place ||
                             (chord.kind == TriggerKind::named &&
                              chord.named == static_cast<uint8_t>(NamedKey::numpad_enter));
    return by_position ? 1 : 0;
}

/// Whether the first condition mask includes the second and adds something.
///
/// @param winner the mask that may be narrower
/// @param other the other mask
/// @return true when `winner` includes `other` and is not equal to it
bool narrower(uint32_t winner, uint32_t other) {
    return (winner & other) == other && winner != other;
}

/// Whether every condition the binding names holds.
///
/// @param bound the binding
/// @param contexts the conditions that hold
/// @return true when none of the binding's conditions are missing
bool conditions_hold(const Bound& bound, const Contexts& contexts) {
    return (bound.when & ~contexts.conditions) == 0;
}

/// Whether the binding is a modifier role.
///
/// @param bound the binding
/// @return true for a modifier role
bool modifier_role(const Bound& bound) {
    return bound.entry != nullptr && bound.entry->kind == actions::Kind::modifier;
}

/// Whether the binding is searched while text is typed.
///
/// @param bound the binding
/// @param names the chat bit
/// @return true for a fixed chord or an action in `chat`
bool text_binding(const Bound& bound, const Names& names) {
    const bool fixed = bound.entry != nullptr && bound.entry->fixed;
    return fixed || (bound.contexts & names.chat_bit) != 0;
}

/// The stack, from the top down, for the contexts that are open.
///
/// @param contexts the open contexts
/// @param names the context bits
/// @return the groups to search
std::vector<Group> groups_for(const Contexts& contexts, const Names& names) {
    std::vector<Group> groups;
    if (contexts.text_entry) {
        Group group;
        group.context = names.chat;
        group.mask = names.chat_bit;
        group.text = true;
        groups.push_back(group);
        return groups;
    }
    const std::span<const actions::Context> table = actions::contexts();
    for (std::size_t index = 0; index < table.size() && index < 32; ++index) {
        if (!table[index].overlay)
            continue;
        const auto bit = static_cast<uint32_t>(1u << index);
        if ((contexts.active & bit) == 0)
            continue;
        Group group;
        group.context = static_cast<uint8_t>(index);
        group.mask = bit;
        groups.push_back(group);
    }
    const uint32_t pool = names.match_layer & contexts.active;
    if (pool != 0) {
        Group group;
        group.context = names.match;
        group.mask = pool;
        group.match_pool = true;
        groups.push_back(group);
    }
    if ((contexts.active & names.menus_bit) != 0) {
        Group group;
        group.context = names.menus;
        group.mask = names.menus_bit;
        groups.push_back(group);
    }
    if ((contexts.active & names.oa_screens_bit) != 0) {
        Group group;
        group.context = names.oa_screens;
        group.mask = names.oa_screens_bit;
        groups.push_back(group);
    }
    if ((contexts.active & names.global_bit) != 0) {
        Group group;
        group.context = names.global;
        group.mask = names.global_bit;
        groups.push_back(group);
    }
    return groups;
}

/// Whether the binding is in the group.
///
/// @param bound the binding
/// @param group the group
/// @param names the chat bit, for text entry
/// @return true when the group searches this binding
bool in_group(const Bound& bound, const Group& group, const Names& names) {
    if (group.text)
        return text_binding(bound, names);
    return (bound.contexts & group.mask) != 0;
}

/// The context index recorded on an event the group took.
///
/// @param bound the binding
/// @param group the group that took it
/// @param names the play, watch and match indexes
/// @return the context index
uint8_t context_in_group(const Bound& bound, const Group& group, const Names& names) {
    if (group.text) {
        if ((bound.contexts & names.chat_bit) != 0)
            return names.chat;
        for (uint8_t bit_index = 0; bit_index < 32; ++bit_index) {
            if ((bound.contexts & (static_cast<uint32_t>(1u) << bit_index)) != 0)
                return bit_index;
        }
        return names.chat;
    }
    if (!group.match_pool)
        return group.context;
    if ((bound.contexts & names.play_bit) != 0 && (group.mask & names.play_bit) != 0)
        return names.play;
    if ((bound.contexts & names.watch_bit) != 0 && (group.mask & names.watch_bit) != 0)
        return names.watch;
    return names.match;
}

/// Whether the screen, not the registry order, keeps both actions.
///
/// @param first one action id
/// @param second the other action id
/// @return true for select and command, or the resource row's view and lock
bool screen_keeps_both(std::string_view first, std::string_view second) {
    const bool select_command = (first == "pointer.select" && second == "pointer.command") ||
                                (first == "pointer.command" && second == "pointer.select");
    const bool resource_row = (first == "watch.player-view" && second == "watch.lock-view") ||
                              (first == "watch.lock-view" && second == "watch.player-view");
    return select_command || resource_row;
}

/// Appends one action event.
///
/// @param fed the result
/// @param bound the binding
/// @param phase what happened
/// @param chord the chord
/// @param context the context index
void push_event(Fed& fed, const Bound& bound, Phase phase, const Chord& chord, uint8_t context) {
    ActionEvent out;
    out.action = bound.id;
    out.phase = phase;
    out.chord = chord;
    out.context = context;
    fed.events.push_back(out);
}

/// How many notches of this wheel chord the event carries.
///
/// @param chord the wheel chord
/// @param event the wheel event
/// @return the notch count, at least one and at most 16
int notch_count(const Chord& chord, const InputEvent& event) {
    const bool vertical = chord.wheel == 1 || chord.wheel == -1;
    int magnitude = vertical ? static_cast<int>(event.wheel_y) : static_cast<int>(event.wheel_x);
    if (magnitude < 0)
        magnitude = -magnitude;
    if (magnitude == 0)
        magnitude = 1;
    if (magnitude > 16)
        magnitude = 16;
    return magnitude;
}

/// Whether a plain key's position is held.
///
/// @param chord the chord
/// @param keys_held the keys the app reports
/// @param keyboard the map, for a character
/// @return true while that key is held
bool trigger_held(const Chord& chord, const HeldKeys& keys_held, const KeyboardMap& keyboard) {
    if (keys_held.down == nullptr)
        return false;
    if (chord.kind == TriggerKind::place)
        return keys_held.down(keys_held.context, chord.scancode);
    if (chord.kind == TriggerKind::named) {
        const std::span<const NamedKeySpec> keys = named_keys();
        if (chord.named >= keys.size())
            return false;
        return keys_held.down(keys_held.context, keys[chord.named].scancode);
    }
    if (chord.kind != TriggerKind::character)
        return false;
    char32_t expected = chord.character;
    if (expected >= U'A' && expected <= U'Z')
        expected = static_cast<char32_t>(expected - U'A' + U'a');
    for (uint16_t scancode = 1; scancode < scancode_limit; ++scancode) {
        if (keyboard.event_key_code(scancode) != static_cast<uint32_t>(expected))
            continue;
        if (keys_held.down(keys_held.context, scancode))
            return true;
    }
    return false;
}

/// Whether a modifier-alone chord's keys are held, ignoring `modifiers_now`.
///
/// @param chord the modifier-alone chord
/// @param keys_held the keys the app reports
/// @param where which key Primary means
/// @return true when each modifier the chord names is held
bool modifier_keys_down(const Chord& chord, const HeldKeys& keys_held, Platform where) {
    if (keys_held.down == nullptr || chord.kind != TriggerKind::modifier_alone)
        return false;
    const auto down = [&keys_held](std::string_view code) {
        const std::optional<uint16_t> scancode = scancode_of(code);
        return scancode.has_value() && keys_held.down(keys_held.context, *scancode);
    };
    const bool want_ctrl = (chord.modifiers & ctrl) != 0 ||
                           ((chord.modifiers & primary) != 0 && !where.primary_is_meta);
    const bool want_meta = (chord.modifiers & meta) != 0 ||
                           ((chord.modifiers & primary) != 0 && where.primary_is_meta);
    const bool want_alt = (chord.modifiers & alt) != 0;
    const bool want_shift = (chord.modifiers & shift) != 0;
    if (want_ctrl && !down("ControlLeft") && !down("ControlRight"))
        return false;
    if (want_meta && !down("MetaLeft") && !down("MetaRight"))
        return false;
    if (want_alt && !down("AltLeft") && !down("AltRight"))
        return false;
    if (want_shift && !down("ShiftLeft") && !down("ShiftRight"))
        return false;
    return want_ctrl || want_meta || want_alt || want_shift;
}

} // namespace

/// The dispatcher's memory.
struct DispatcherState {
    Resolved resolved{};
    KeyboardMap keyboard{};
    Platform platform{};
    Timings timings{};
    Contexts contexts{};
    KeyClocks clocks{};
    std::array<Slot, scancode_limit> keys{};
    std::array<Slot, 9> buttons{};
    std::vector<LiveRole> live{};
    std::vector<std::pair<std::string, std::string>> logged{};
    Latches latches{};                       ///< hold latches for the toggle list
    StickyModifiers sticky{};                ///< armed and locked modifiers
    bool sticky_on{};                        ///< sticky modifiers are in use
    std::vector<std::string> toggle{};       ///< latchable actions that latch
    std::array<ModifierFinger, 8> fingers{}; ///< one per modifier bit
    uint8_t tracked_modifiers{};             ///< modifier bits whose keys are down
};

namespace {

/// Logs one registry-order tie, once for the pair.
///
/// @param state the log
/// @param winner the action registry order kept
/// @param other the action it tied with
void log_tie(DispatcherState& state, std::string_view winner, std::string_view other) {
    if (winner == other)
        return;
    std::string first{winner};
    std::string second{other};
    if (second < first)
        std::swap(first, second);
    for (const std::pair<std::string, std::string>& row : state.logged) {
        if (row.first == first && row.second == second)
            return;
    }
    state.logged.emplace_back(first, second);
    const std::string kept{winner};
    const std::string alongside{other};
    std::fprintf(
        stderr,
        "key bindings: %s and %s tie; the earlier action is kept\n",
        kept.c_str(),
        alongside.c_str()
    );
}

/// Which candidate wins on rank, hack precedence and conditions.
///
/// A letter and the place that prints it drop the position bonus, so they
/// tie on rank. Registry order is not applied here.
///
/// @param left one candidate
/// @param right the other
/// @param keyboard the map `same_key` reads
/// @return which one wins, or a tie
Which prefer(const Candidate& left, const Candidate& right, const KeyboardMap& keyboard) {
    int left_score = left.score;
    int right_score = right.score;
    if (same_key(left.chord, right.chord, &keyboard)) {
        left_score -= position_bonus(left.chord);
        right_score -= position_bonus(right.chord);
    }
    if (left_score != right_score)
        return left_score > right_score ? Which::left : Which::right;
    if (left.precedence != right.precedence)
        return left.precedence > right.precedence ? Which::left : Which::right;
    const bool left_narrower = narrower(left.when, right.when);
    const bool right_narrower = narrower(right.when, left.when);
    if (left_narrower != right_narrower)
        return left_narrower ? Which::left : Which::right;
    return Which::tie;
}

/// The chords of one gesture that match in one group.
///
/// @param state the bindings and the keyboard
/// @param event the event
/// @param group the group
/// @param gesture the gesture
/// @param exact_only true to keep exact matches only
/// @param[out] found the matches
void collect(
    const DispatcherState& state,
    const InputEvent& event,
    const Group& group,
    Gesture gesture,
    bool exact_only,
    std::vector<Candidate>& found
) {
    const Names& names = context_names();
    for (std::size_t action_index = 0; action_index < state.resolved.actions.size();
         ++action_index) {
        const Bound& bound = state.resolved.actions[action_index];
        if (modifier_role(bound) || !conditions_hold(bound, state.contexts))
            continue;
        if (!in_group(bound, group, names))
            continue;
        for (std::size_t chord_index = 0; chord_index < bound.chords.size(); ++chord_index) {
            const Chord& chord = bound.chords[chord_index].chord;
            if (chord.gesture != gesture)
                continue;
            const Match quality = match(chord, event, state.keyboard, state.platform);
            if (quality == Match::none)
                continue;
            if (exact_only && quality != Match::exact)
                continue;
            Candidate item;
            item.action_index = action_index;
            item.chord_index = chord_index;
            item.id = bound.id;
            item.chord = chord;
            item.score = rank(chord);
            item.precedence = precedence_of(bound);
            item.when = bound.when;
            item.target = bound.entry != nullptr ? bound.entry->target : actions::Target::none;
            item.context = context_in_group(bound, group, names);
            found.push_back(item);
        }
    }
}

/// One winner for each pointer target. The same target keeps one binding.
///
/// @param state the tie log
/// @param found the matches in the winning group
/// @return the winners
std::vector<Candidate> pick(DispatcherState& state, const std::vector<Candidate>& found) {
    std::vector<Candidate> kept;
    for (const Candidate& item : found) {
        std::size_t at = kept.size();
        for (std::size_t index = 0; index < kept.size(); ++index) {
            if (kept[index].target == item.target) {
                at = index;
                break;
            }
        }
        if (at == kept.size()) {
            kept.push_back(item);
            continue;
        }
        const Which which = prefer(item, kept[at], state.keyboard);
        if (which == Which::left) {
            kept[at] = item;
            continue;
        }
        if (which != Which::tie || screen_keeps_both(item.id, kept[at].id))
            continue;
        if (item.action_index == kept[at].action_index) {
            if (item.chord_index < kept[at].chord_index)
                kept[at] = item;
            continue;
        }
        log_tie(
            state,
            item.action_index < kept[at].action_index ? item.id : kept[at].id,
            item.action_index < kept[at].action_index ? kept[at].id : item.id
        );
        if (item.action_index < kept[at].action_index)
            kept[at] = item;
    }
    std::vector<Candidate> both = kept;
    for (const Candidate& item : found) {
        bool already = false;
        for (const Candidate& winner : kept) {
            if (winner.action_index == item.action_index && winner.chord_index == item.chord_index)
                already = true;
        }
        if (already)
            continue;
        for (const Candidate& winner : kept) {
            if (winner.target != item.target || !screen_keeps_both(winner.id, item.id))
                continue;
            if (prefer(item, winner, state.keyboard) == Which::tie)
                both.push_back(item);
        }
    }
    return both;
}

/// The winning bindings for one gesture, or none.
///
/// Exact matches are searched first, from the top of the stack. When no
/// context has one, the first context with any match is used.
///
/// @param state the bindings
/// @param event the event
/// @param gesture the gesture
/// @return the winners
std::vector<Candidate> choose(DispatcherState& state, const InputEvent& event, Gesture gesture) {
    const std::vector<Group> groups = groups_for(state.contexts, context_names());
    for (const bool exact_only : {true, false}) {
        for (const Group& group : groups) {
            std::vector<Candidate> found;
            collect(state, event, group, gesture, exact_only, found);
            if (!found.empty())
                return pick(state, found);
        }
    }
    return {};
}

/// The slot for a key or a button.
///
/// @param state the slots
/// @param event the event
/// @return the slot; a position past the map shares the unused slot
Slot& slot_for(DispatcherState& state, const InputEvent& event) {
    if (event.kind == InputKind::button) {
        const uint8_t button = event.button <= 8 ? event.button : 0;
        return state.buttons[button];
    }
    const uint16_t scancode = event.scancode < scancode_limit ? event.scancode : 0;
    return state.keys[scancode];
}

/// Emits `released` for the actions a slot started, and forgets the down.
///
/// The down's time and modifiers stay, so a second down can be a double press.
///
/// @param state the bindings
/// @param slot the slot
/// @param fed the result
void release_started(DispatcherState& state, Slot& slot, Fed& fed) {
    for (const Started& started : slot.started) {
        if (started.action_index >= state.resolved.actions.size())
            continue;
        const Bound& bound = state.resolved.actions[started.action_index];
        if (started.chord_index >= bound.chords.size())
            continue;
        push_event(
            fed, bound, Phase::released, bound.chords[started.chord_index].chord, started.context
        );
    }
    slot.started.clear();
    slot.holds.clear();
    slot.down = false;
    slot.had_binding = false;
    slot.saw_up = true;
}

/// Remembers an action the down started.
///
/// @param slot the slot
/// @param item the winner
void remember(Slot& slot, const Candidate& item) {
    for (const Started& started : slot.started) {
        if (started.action_index == item.action_index)
            return;
    }
    Started started;
    started.action_index = item.action_index;
    started.chord_index = item.chord_index;
    started.context = item.context;
    slot.started.push_back(started);
}

/// Whether this key down is the second of a double press.
///
/// @param slot the previous down
/// @param event this down
/// @param window how far apart the downs may be
/// @return true when the same modifiers went down again inside the window
bool second_press(const Slot& slot, const InputEvent& event, uint32_t window) {
    if (!slot.saw_up || event.modifiers != slot.down_modifiers)
        return false;
    if (event.time_ms < slot.down_ms)
        return false;
    return event.time_ms - slot.down_ms <= window;
}

/// The index of one modifier bit, or -1 when the value is not a single bit.
///
/// @param bit a modifier bit
/// @return 0 to 7, or -1
int modifier_index(uint8_t bit) {
    if (bit == 0 || (bit & static_cast<uint8_t>(bit - 1)) != 0)
        return -1;
    int index = 0;
    uint8_t rest = bit;
    while ((rest & 1u) == 0) {
        rest = static_cast<uint8_t>(rest >> 1);
        ++index;
    }
    return index < 8 ? index : -1;
}

/// Whether the toggle list names an action.
///
/// @param state the list
/// @param id the action id
/// @return true when the id is listed
bool toggle_lists(const DispatcherState& state, std::string_view id) {
    for (const std::string& name : state.toggle) {
        if (name == id)
            return true;
    }
    return false;
}

/// Whether the binding may latch.
///
/// @param bound the binding
/// @return true when the registry marks it latchable
bool latchable_action(const Bound& bound) {
    return bound.entry != nullptr && bound.entry->latchable;
}

/// Adds modifier roles whose latch is on and that the visible modifiers missed.
///
/// @param state the latches
/// @param groups the context stack
/// @param[in,out] want the roles that should be active
void append_latched_roles(
    const DispatcherState& state, const std::vector<Group>& groups, std::vector<LiveRole>& want
) {
    if (state.toggle.empty())
        return;
    for (std::size_t action_index = 0; action_index < state.resolved.actions.size();
         ++action_index) {
        const Bound& bound = state.resolved.actions[action_index];
        if (!toggle_lists(state, bound.id) || !latchable_action(bound) || !modifier_role(bound))
            continue;
        if (!state.latches.active(bound.id) || !conditions_hold(bound, state.contexts))
            continue;
        if ((bound.contexts & state.contexts.active) == 0)
            continue;
        bool already = false;
        for (const LiveRole& role : want) {
            if (role.action_index == action_index)
                already = true;
        }
        if (already)
            continue;
        for (std::size_t chord_index = 0; chord_index < bound.chords.size(); ++chord_index) {
            if (bound.chords[chord_index].chord.kind != TriggerKind::modifier_alone)
                continue;
            LiveRole role;
            role.action_index = action_index;
            role.chord_index = chord_index;
            for (const Group& group : groups) {
                if (!in_group(bound, group, context_names()))
                    continue;
                role.context = context_in_group(bound, group, context_names());
                break;
            }
            want.push_back(role);
            break;
        }
    }
}

/// Emits `active` and `inactive` so the live roles match `visible_mods`.
///
/// @param state the roles
/// @param visible_mods the modifiers that count as held
/// @param fed the result
void sync_modifier_roles(DispatcherState& state, uint8_t visible_mods, Fed& fed) {
    Contexts stack = state.contexts;
    stack.text_entry = false;
    const std::vector<Group> groups = groups_for(stack, context_names());
    std::vector<LiveRole> want;
    for (std::size_t action_index = 0; action_index < state.resolved.actions.size();
         ++action_index) {
        const Bound& bound = state.resolved.actions[action_index];
        if (!modifier_role(bound) || !conditions_hold(bound, state.contexts))
            continue;
        if ((bound.contexts & state.contexts.active) == 0)
            continue;
        for (std::size_t chord_index = 0; chord_index < bound.chords.size(); ++chord_index) {
            const Chord& chord = bound.chords[chord_index].chord;
            if (chord.kind != TriggerKind::modifier_alone ||
                !held(chord, visible_mods, state.platform))
                continue;
            LiveRole role;
            role.action_index = action_index;
            role.chord_index = chord_index;
            for (const Group& group : groups) {
                if (!in_group(bound, group, context_names()))
                    continue;
                role.context = context_in_group(bound, group, context_names());
                break;
            }
            want.push_back(role);
            break;
        }
    }
    append_latched_roles(state, groups, want);

    for (const LiveRole& role : state.live) {
        bool still = false;
        for (const LiveRole& next : want) {
            if (next.action_index == role.action_index)
                still = true;
        }
        if (still || role.action_index >= state.resolved.actions.size())
            continue;
        const Bound& bound = state.resolved.actions[role.action_index];
        if (role.chord_index >= bound.chords.size())
            continue;
        push_event(fed, bound, Phase::inactive, bound.chords[role.chord_index].chord, role.context);
    }
    for (const LiveRole& role : want) {
        bool already = false;
        for (const LiveRole& previous : state.live) {
            if (previous.action_index == role.action_index)
                already = true;
        }
        if (already || role.action_index >= state.resolved.actions.size())
            continue;
        const Bound& bound = state.resolved.actions[role.action_index];
        if (role.chord_index >= bound.chords.size())
            continue;
        push_event(fed, bound, Phase::active, bound.chords[role.chord_index].chord, role.context);
    }
    state.live = std::move(want);
}

/// Emits `active` and `inactive` as a modifier key goes down or up.
///
/// A key that is not itself a modifier does not change a role: a check's
/// `mod` field is read by `role_active`, not by this. With no toggle list and
/// sticky modifiers off, this is the only path that touches a modifier role.
///
/// @param state the roles
/// @param event the key event
/// @param fed the result
void apply_modifiers(DispatcherState& state, const InputEvent& event, Fed& fed) {
    if (event.kind != InputKind::key || event.repeat || bit_for_scancode(event.scancode) == 0)
        return;
    const uint8_t bit = bit_for_scancode(event.scancode);
    uint8_t visible_mods = event.modifiers;
    if (event.down)
        visible_mods = static_cast<uint8_t>(visible_mods | bit);
    else
        visible_mods = static_cast<uint8_t>(visible_mods & static_cast<uint8_t>(~bit));
    sync_modifier_roles(state, visible_mods, fed);
}

/// Emits the winners of one gesture.
///
/// @param state the bindings
/// @param winners the winners
/// @param phase the phase
/// @param event the event, for a wheel's notch count
/// @param fed the result
void emit_winners(
    const DispatcherState& state,
    const std::vector<Candidate>& winners,
    Phase phase,
    const InputEvent& event,
    Fed& fed
) {
    for (const Candidate& item : winners) {
        if (item.action_index >= state.resolved.actions.size())
            continue;
        const Bound& bound = state.resolved.actions[item.action_index];
        int count = 1;
        if (event.kind == InputKind::wheel)
            count = notch_count(item.chord, event);
        for (int notch = 0; notch < count; ++notch)
            push_event(fed, bound, phase, item.chord, item.context);
    }
}

/// Ends the latches a press started.
///
/// @param state the latches
/// @param ids the action ids
/// @param now_ms the time
void release_latch_ids(DispatcherState& state, std::vector<std::string>& ids, uint64_t now_ms) {
    for (const std::string& id : ids)
        state.latches.release(id, now_ms, state.timings.long_press_ms);
    ids.clear();
}

/// Presses the modifier roles in the toggle list that this key alone holds.
///
/// @param state the latches
/// @param bit the modifier bit of the key
/// @param now_ms the time
/// @param[out] recorded the ids this key pressed
void press_modifier_latches(
    DispatcherState& state, uint8_t bit, uint64_t now_ms, std::vector<std::string>& recorded
) {
    if (bit == 0 || state.toggle.empty())
        return;
    for (const std::string& id : state.toggle) {
        const Bound* bound = nullptr;
        for (const Bound& item : state.resolved.actions) {
            if (item.id == id) {
                bound = &item;
                break;
            }
        }
        if (bound == nullptr || !latchable_action(*bound) || !modifier_role(*bound))
            continue;
        if (!conditions_hold(*bound, state.contexts) ||
            (bound->contexts & state.contexts.active) == 0)
            continue;
        if (state.latches.held(id))
            continue;
        bool mine = false;
        for (const BoundChord& item : bound->chords) {
            if (item.chord.kind == TriggerKind::modifier_alone &&
                held(item.chord, bit, state.platform)) {
                mine = true;
                break;
            }
        }
        if (!mine)
            continue;
        state.latches.press(id, now_ms);
        recorded.push_back(id);
    }
}

/// Modifier bits that are down and not yet a plain hold.
///
/// @param state the fingers
/// @return the bits
uint8_t hidden_sticky_bits(const DispatcherState& state) {
    uint8_t hidden = 0;
    if (!state.sticky_on)
        return 0;
    for (const uint8_t bit : {uint8_t{ctrl}, uint8_t{alt}, uint8_t{shift}, uint8_t{meta}}) {
        const int index = modifier_index(bit);
        if (index < 0)
            continue;
        const ModifierFinger& finger = state.fingers[static_cast<std::size_t>(index)];
        if (finger.down && !finger.plain)
            hidden = static_cast<uint8_t>(hidden | bit);
    }
    return hidden;
}

/// Marks every modifier key down, except one bit, as a plain hold.
///
/// @param state the fingers
/// @param except_bit the bit to leave, or 0 for every bit
void mark_fingers_plain(DispatcherState& state, uint8_t except_bit) {
    for (const uint8_t bit : {uint8_t{ctrl}, uint8_t{alt}, uint8_t{shift}, uint8_t{meta}}) {
        if (bit == except_bit)
            continue;
        const int index = modifier_index(bit);
        if (index < 0)
            continue;
        ModifierFinger& finger = state.fingers[static_cast<std::size_t>(index)];
        if (finger.down)
            finger.plain = true;
    }
}

/// Marks every held latch as used, so letting go does not toggle it.
///
/// @param state the latches
void used_held_latches(DispatcherState& state) {
    for (const std::string& id : state.toggle) {
        if (state.latches.held(id))
            state.latches.used(id);
    }
}

/// Presses latchable holds the down started, other than modifier roles.
///
/// @param state the latches
/// @param slot the key or button
/// @param winners the press winners
/// @param holds the hold arms
/// @param now_ms the time
void remember_key_latches(
    DispatcherState& state,
    Slot& slot,
    const std::vector<Candidate>& winners,
    const std::vector<Candidate>& holds,
    uint64_t now_ms
) {
    slot.latch_ids.clear();
    const auto consider = [&](std::size_t action_index) {
        if (action_index >= state.resolved.actions.size())
            return;
        const Bound& bound = state.resolved.actions[action_index];
        if (!toggle_lists(state, bound.id) || !latchable_action(bound) || modifier_role(bound))
            return;
        if (state.latches.held(bound.id))
            return;
        for (const std::string& already : slot.latch_ids) {
            if (already == bound.id)
                return;
        }
        state.latches.press(bound.id, now_ms);
        slot.latch_ids.push_back(bound.id);
    };
    for (const Candidate& item : winners)
        consider(item.action_index);
    for (const Candidate& item : holds)
        consider(item.action_index);
}

/// Whether a toggle list or sticky modifiers change what an accepted event does.
///
/// With both off, `feed` does not call the helper path, so the events match
/// the path that has no latches.
///
/// @param state the dispatcher
/// @return true when a latch or a sticky modifier can apply
bool helpers_engaged(const DispatcherState& state) {
    return state.sticky_on || !state.toggle.empty();
}

/// Notes a latch or a sticky modifier for an event the bindings are about to accept.
///
/// A key held back or dropped never reaches this. A modifier key that goes
/// down and up with nothing else between is a tap. Another key or button
/// makes it a plain modifier. `seen` carries the sticky bits for `choose`.
/// The event itself keeps the physical modifiers, and so does the slot.
///
/// @param state the dispatcher
/// @param event the physical event
/// @param[out] seen the event `choose` matches
/// @param fed the result
void note_accepted_helpers(
    DispatcherState& state, const InputEvent& event, InputEvent& seen, Fed& fed
) {
    const uint8_t sticky_before = state.sticky.mods();
    const bool modifier_key =
        event.kind == InputKind::key && !event.repeat && bit_for_scancode(event.scancode) != 0;
    const uint8_t bit = modifier_key ? bit_for_scancode(event.scancode) : uint8_t{0};
    const bool other_down = !event.repeat && state.sticky_on && event.down && !modifier_key &&
                            (event.kind == InputKind::key || event.kind == InputKind::button);

    if (!event.repeat && state.sticky_on && event.down && modifier_key)
        mark_fingers_plain(state, bit);
    if (other_down)
        mark_fingers_plain(state, 0);
    if (!event.repeat && event.kind == InputKind::button && event.down)
        used_held_latches(state);

    if (other_down) {
        const uint8_t physical = static_cast<uint8_t>(event.modifiers | state.tracked_modifiers);
        const uint8_t visible =
            static_cast<uint8_t>(physical & static_cast<uint8_t>(~hidden_sticky_bits(state)));
        sync_modifier_roles(state, visible, fed);
    }

    if (modifier_key) {
        const int index = modifier_index(bit);
        uint8_t physical = event.modifiers;
        if (event.down)
            physical = static_cast<uint8_t>(physical | bit);
        else
            physical = static_cast<uint8_t>(physical & static_cast<uint8_t>(~bit));
        bool tapped = false;
        if (index >= 0) {
            ModifierFinger& finger = state.fingers[static_cast<std::size_t>(index)];
            if (event.down) {
                if (state.sticky_on) {
                    finger.down = true;
                    finger.plain = false;
                    state.tracked_modifiers = static_cast<uint8_t>(state.tracked_modifiers | bit);
                }
                finger.latch_ids.clear();
                press_modifier_latches(state, bit, event.time_ms, finger.latch_ids);
            } else {
                tapped = state.sticky_on && finger.down && !finger.plain;
                release_latch_ids(state, finger.latch_ids, event.time_ms);
                finger = ModifierFinger{};
                state.tracked_modifiers =
                    static_cast<uint8_t>(state.tracked_modifiers & static_cast<uint8_t>(~bit));
            }
        }
        uint8_t visible = physical;
        if (state.sticky_on) {
            visible = static_cast<uint8_t>(
                static_cast<uint8_t>(physical | state.tracked_modifiers) &
                static_cast<uint8_t>(~hidden_sticky_bits(state))
            );
        }
        sync_modifier_roles(state, visible, fed);
        if (tapped)
            state.sticky.tap(bit, event.time_ms, state.timings.double_press_ms);
    }

    seen = event;
    if (!state.sticky_on)
        return;
    const uint8_t sticky_bits = modifier_key ? sticky_before : state.sticky.mods();
    uint8_t plain_bits = 0;
    for (const uint8_t modifier_bit :
         {uint8_t{ctrl}, uint8_t{alt}, uint8_t{shift}, uint8_t{meta}}) {
        const int index = modifier_index(modifier_bit);
        if (index < 0)
            continue;
        const ModifierFinger& finger = state.fingers[static_cast<std::size_t>(index)];
        if (finger.down && finger.plain)
            plain_bits = static_cast<uint8_t>(plain_bits | modifier_bit);
    }
    seen.modifiers = static_cast<uint8_t>(seen.modifiers | sticky_bits | plain_bits);
}

/// Turns one key or button the bindings are allowed to see into action events.
///
/// `event` is the physical key. `seen` is that key with sticky modifiers added,
/// and it is what `choose` matches. A repeat, including one that is the key's
/// first event, does not press or release a latch and does not consume a
/// sticky modifier.
///
/// @param state the bindings
/// @param event the physical key or button
/// @param seen the event `choose` matches
/// @param fed the result
void accept_control(
    DispatcherState& state, const InputEvent& event, const InputEvent& seen, Fed& fed
) {
    Slot& slot = slot_for(state, event);
    const bool modifier_key =
        event.kind == InputKind::key && !event.repeat && bit_for_scancode(event.scancode) != 0;
    if (event.repeat) {
        // A repeat can be the first event for the key: a check holds it down
        // by sending the repeat itself. Resolve that the way a press would,
        // and report one repeat of each repeat action. A press action still
        // waits for a down that was not a repeat. Neither arm latches.
        if (!slot.down) {
            const std::vector<Candidate> winners = choose(state, seen, Gesture::press);
            for (const Candidate& item : winners) {
                if (item.action_index >= state.resolved.actions.size())
                    continue;
                const Bound& bound = state.resolved.actions[item.action_index];
                const actions::Kind kind =
                    bound.entry != nullptr ? bound.entry->kind : actions::Kind::press;
                if (kind != actions::Kind::repeat)
                    continue;
                push_event(fed, bound, Phase::repeated, item.chord, item.context);
                remember(slot, item);
            }
            if (!slot.started.empty()) {
                slot.down = true;
                slot.saw_up = false;
                slot.down_modifiers = event.modifiers;
                slot.down_ms = event.time_ms;
                slot.had_binding = true;
                slot.latch_ids.clear();
            }
            fed.bound = !fed.events.empty();
            return;
        }
        for (const Started& started : slot.started) {
            if (started.action_index >= state.resolved.actions.size())
                continue;
            const Bound& bound = state.resolved.actions[started.action_index];
            const actions::Kind kind =
                bound.entry != nullptr ? bound.entry->kind : actions::Kind::press;
            if (kind != actions::Kind::repeat || started.chord_index >= bound.chords.size())
                continue;
            push_event(
                fed,
                bound,
                Phase::repeated,
                bound.chords[started.chord_index].chord,
                started.context
            );
        }
        fed.bound = slot.down && slot.had_binding;
        return;
    }

    if (!event.down) {
        if (!slot.down) {
            fed.bound = !fed.events.empty();
            return;
        }
        release_latch_ids(state, slot.latch_ids, event.time_ms);
        release_started(state, slot, fed);
        const std::vector<Candidate> winners = choose(state, seen, Gesture::release);
        emit_winners(state, winners, Phase::pressed, event, fed);
        if (state.sticky_on && !modifier_key && !winners.empty())
            state.sticky.consume();
        fed.bound = !fed.events.empty();
        return;
    }

    // A second down is a double press only after the key has come up. Two downs
    // with no up between them are a missed release, not a double press. The
    // stored modifiers stay the physical ones, so a sticky bit `choose` saw
    // does not become the next double-press's modifiers.
    const bool as_double =
        event.kind == InputKind::button
            ? event.clicks == 2
            : !slot.down && second_press(slot, event, state.timings.double_press_ms);
    if (slot.down) {
        release_latch_ids(state, slot.latch_ids, event.time_ms);
        release_started(state, slot, fed);
    }
    slot.down = true;
    slot.saw_up = false;
    slot.down_modifiers = event.modifiers;
    slot.down_ms = event.time_ms;
    slot.started.clear();
    slot.holds.clear();
    slot.latch_ids.clear();

    std::vector<Candidate> winners;
    if (as_double)
        winners = choose(state, seen, Gesture::double_press);
    if (winners.empty())
        winners = choose(state, seen, Gesture::press);
    const std::vector<Candidate> holds = choose(state, seen, Gesture::hold);
    for (const Candidate& item : holds) {
        HoldArm arm;
        arm.action_index = item.action_index;
        arm.chord_index = item.chord_index;
        arm.context = item.context;
        arm.due_ms = event.time_ms + state.timings.long_press_ms;
        slot.holds.push_back(arm);
    }
    // A double that found no `:double` binding falls through to the plain chord.
    const Phase down_phase =
        !winners.empty() && winners.front().chord.gesture == Gesture::double_press
            ? Phase::double_press
            : Phase::pressed;
    emit_winners(state, winners, down_phase, event, fed);
    for (const Candidate& item : winners)
        remember(slot, item);
    if (!modifier_key)
        remember_key_latches(state, slot, winners, holds, event.time_ms);
    slot.had_binding = !winners.empty() || !slot.holds.empty() || !fed.events.empty();
    if (state.sticky_on && !modifier_key && (!winners.empty() || !slot.holds.empty()))
        state.sticky.consume();
    fed.bound = slot.had_binding;
}

/// Emits every `repeated` event the player's own rate has due, for repeat actions only.
///
/// @param state the bindings and the clocks
/// @param fed the result
/// @param now_ms the time, in milliseconds
void emit_custom_repeats(DispatcherState& state, Fed& fed, uint64_t now_ms) {
    if (!custom_repeat(state.timings))
        return;
    const uint32_t interval = repeat_interval_ms(state.timings);
    if (interval == 0)
        return;
    for (uint16_t index = 0; index < scancode_limit; ++index) {
        KeyClock& clock = state.clocks.keys[index];
        Slot& slot = state.keys[index];
        if (!clock.repeat_on || !slot.down)
            continue;
        int guard = 0;
        while (clock.next_repeat_ms <= now_ms && guard < 64) {
            for (const Started& started : slot.started) {
                if (started.action_index >= state.resolved.actions.size())
                    continue;
                const Bound& bound = state.resolved.actions[started.action_index];
                const actions::Kind kind =
                    bound.entry != nullptr ? bound.entry->kind : actions::Kind::press;
                if (kind != actions::Kind::repeat || started.chord_index >= bound.chords.size())
                    continue;
                push_event(
                    fed,
                    bound,
                    Phase::repeated,
                    bound.chords[started.chord_index].chord,
                    started.context
                );
            }
            clock.next_repeat_ms += interval;
            ++guard;
        }
    }
}

} // namespace

Dispatcher::Dispatcher() : state_(std::make_unique<DispatcherState>()) {
}

Dispatcher::~Dispatcher() = default;

Dispatcher::Dispatcher(Dispatcher&&) noexcept = default;

Dispatcher& Dispatcher::operator=(Dispatcher&&) noexcept = default;

void Dispatcher::set_bindings(const Resolved& bindings) {
    state_->resolved = bindings;
    state_->keys = {};
    state_->buttons = {};
    state_->live.clear();
    state_->logged.clear();
    state_->latches.clear();
    state_->sticky.clear();
    state_->fingers = {};
    state_->tracked_modifiers = 0;
    clear_clocks(state_->clocks);
}

void Dispatcher::retarget(const Resolved& bindings) {
    state_->resolved = bindings;
}

void Dispatcher::set_keyboard(const KeyboardMap& keyboard) {
    state_->keyboard = keyboard;
}

void Dispatcher::set_platform(Platform platform) {
    state_->platform = platform;
}

void Dispatcher::set_timings(Timings timings) {
    state_->timings = clamped(timings);
}

void Dispatcher::set_contexts(Contexts contexts) {
    state_->contexts = contexts;
}

Fed Dispatcher::feed(const InputEvent& event) {
    Fed fed;
    if (event.kind == InputKind::pad)
        return fed;
    // Slow keys and bounce keys decide first. A key they hold back or drop
    // does not latch and does not arm a sticky modifier.
    const KeyFate fate =
        event.kind == InputKind::key
            ? consider_key(state_->clocks, state_->timings, state_->contexts.text_entry, event)
            : KeyFate::through;
    if (fate == KeyFate::hold) {
        fed.legacy = LegacyGate::hold;
        return fed;
    }
    if (fate == KeyFate::drop) {
        fed.legacy = LegacyGate::drop;
        return fed;
    }
    InputEvent seen = event;
    if (helpers_engaged(*state_))
        note_accepted_helpers(*state_, event, seen, fed);
    else if (event.kind == InputKind::key)
        apply_modifiers(*state_, event, fed);
    if (event.kind == InputKind::wheel) {
        const std::vector<Candidate> winners = choose(*state_, seen, Gesture::press);
        emit_winners(*state_, winners, Phase::pressed, event, fed);
        if (state_->sticky_on && !winners.empty())
            state_->sticky.consume();
        fed.bound = !fed.events.empty();
        return fed;
    }
    if (event.kind != InputKind::key && event.kind != InputKind::button)
        return fed;
    if (fate == KeyFate::ignore_repeat) {
        const Slot& slot = slot_for(*state_, event);
        fed.bound = slot.down && slot.had_binding;
        return fed;
    }
    accept_control(*state_, event, seen, fed);
    if (!state_->contexts.text_entry)
        note_accepted_down(state_->clocks, state_->timings, event);
    return fed;
}

Fed Dispatcher::advance(uint64_t now_ms) {
    Fed fed;
    const std::vector<InputEvent> due = take_admissions(state_->clocks, now_ms);
    for (const InputEvent& event : due) {
        InputEvent seen = event;
        if (helpers_engaged(*state_))
            note_accepted_helpers(*state_, event, seen, fed);
        else
            apply_modifiers(*state_, event, fed);
        accept_control(*state_, event, seen, fed);
        note_accepted_down(state_->clocks, state_->timings, event);
        fed.admitted.push_back(event);
    }
    const auto fire = [this, &fed, now_ms](Slot& slot) {
        if (!slot.down)
            return;
        for (HoldArm& arm : slot.holds) {
            if (arm.fired || now_ms < arm.due_ms)
                continue;
            if (arm.action_index >= state_->resolved.actions.size())
                continue;
            const Bound& bound = state_->resolved.actions[arm.action_index];
            if (arm.chord_index >= bound.chords.size())
                continue;
            arm.fired = true;
            push_event(fed, bound, Phase::held, bound.chords[arm.chord_index].chord, arm.context);
            remember(
                slot,
                Candidate{
                    arm.action_index,
                    arm.chord_index,
                    bound.id,
                    bound.chords[arm.chord_index].chord,
                    0,
                    0,
                    0,
                    actions::Target::none,
                    arm.context
                }
            );
        }
    };
    for (Slot& slot : state_->keys)
        fire(slot);
    for (Slot& slot : state_->buttons)
        fire(slot);
    emit_custom_repeats(*state_, fed, now_ms);
    return fed;
}

Fed Dispatcher::release_all(uint64_t now_ms) {
    (void)now_ms;
    clear_clocks(state_->clocks);
    Fed fed;
    const auto drop = [this, &fed](Slot& slot) {
        if (slot.down)
            release_started(*state_, slot, fed);
        slot = Slot{};
    };
    for (Slot& slot : state_->keys)
        drop(slot);
    for (Slot& slot : state_->buttons)
        drop(slot);
    for (const LiveRole& role : state_->live) {
        if (role.action_index >= state_->resolved.actions.size())
            continue;
        const Bound& bound = state_->resolved.actions[role.action_index];
        if (role.chord_index >= bound.chords.size())
            continue;
        push_event(fed, bound, Phase::inactive, bound.chords[role.chord_index].chord, role.context);
    }
    state_->live.clear();
    state_->latches.clear();
    state_->sticky.clear();
    state_->fingers = {};
    state_->tracked_modifiers = 0;
    return fed;
}

bool Dispatcher::role_active(
    std::string_view action, uint8_t modifiers_now, const HeldKeys& keys_held
) const {
    const Bound* found = nullptr;
    for (const Bound& bound : state_->resolved.actions) {
        if (bound.id == action) {
            found = &bound;
            break;
        }
    }
    if (found == nullptr)
        return false;
    if (state_->latches.active(action))
        return true;
    for (const BoundChord& item : found->chords) {
        const Chord& chord = item.chord;
        if (chord.kind == TriggerKind::modifier_alone) {
            if (held(chord, modifiers_now, state_->platform) ||
                modifier_keys_down(chord, keys_held, state_->platform))
                return true;
            continue;
        }
        if (!trigger_held(chord, keys_held, state_->keyboard))
            continue;
        InputEvent probe;
        probe.kind = InputKind::key;
        probe.down = true;
        probe.modifiers = modifiers_now;
        probe.scancode = chord.scancode;
        if (chord.kind == TriggerKind::character) {
            char32_t expected = chord.character;
            if (expected >= U'A' && expected <= U'Z')
                expected = static_cast<char32_t>(expected - U'A' + U'a');
            probe.key_code = static_cast<uint32_t>(expected);
        } else if (chord.kind == TriggerKind::named) {
            const std::span<const NamedKeySpec> keys = named_keys();
            if (chord.named < keys.size()) {
                probe.key_code = keys[chord.named].key_code;
                probe.scancode = keys[chord.named].scancode;
            }
        }
        if (match(chord, probe, state_->keyboard, state_->platform) != Match::none)
            return true;
    }
    return false;
}

void Dispatcher::set_toggle(std::vector<std::string> ids) {
    state_->toggle = std::move(ids);
    state_->latches.clear();
}

void Dispatcher::set_sticky_modifiers(bool on) {
    if (state_->sticky_on == on)
        return;
    state_->sticky_on = on;
    if (!on) {
        state_->sticky.clear();
        state_->fingers = {};
        state_->tracked_modifiers = 0;
    }
}

const Latches& Dispatcher::latches() const {
    return state_->latches;
}

const StickyModifiers& Dispatcher::sticky() const {
    return state_->sticky;
}

Timings Dispatcher::timings() const {
    return state_->timings;
}

void Dispatcher::consume_sticky() {
    state_->sticky.consume();
}

bool Dispatcher::chord_key_held(std::string_view action, const HeldKeys& keys_held) const {
    const Bound* found = nullptr;
    for (const Bound& bound : state_->resolved.actions) {
        if (bound.id == action) {
            found = &bound;
            break;
        }
    }
    if (found == nullptr)
        return false;
    for (const BoundChord& item : found->chords) {
        if (item.chord.kind == TriggerKind::modifier_alone)
            continue;
        if (trigger_held(item.chord, keys_held, state_->keyboard))
            return true;
    }
    return false;
}

} // namespace oa::ui::key_bindings

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Resolution: oa-default clashes with nothing, a layout and a mod and a
// player's own changes combine in order, and the pairs that share a key
// without clashing stay listed as shadows.
#include "oa/test/check.hpp"
#include "oa/ui/key_bindings/dispatcher.hpp"
#include "oa/ui/key_bindings/layout.hpp"

#include "oa/data/input_actions/actions.hpp"

#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace keys = oa::ui::key_bindings;
namespace acts = oa::data::input_actions;

using keys::Bound;
using keys::Chord;
using keys::Clash;
using keys::Lost;
using keys::LostReason;
using keys::Resolved;
using keys::Shadow;
using keys::ShadowKind;

/// Parses a chord the test expects to accept.
///
/// @param text the chord text
/// @return the chord, or a default when it was refused
Chord must_parse(std::string_view text) {
    keys::ChordError error;
    const std::optional<Chord> chord = keys::parse_chord(text, error);
    if (!chord) {
        std::fprintf(
            stderr, "parse failed: \"%.*s\"\n", static_cast<int>(text.size()), text.data()
        );
    }
    OA_CHECK(chord.has_value());
    if (!chord)
        return {};
    return *chord;
}

/// One layout line.
///
/// @param action the action id
/// @param texts the chord texts
/// @param line the line in the file
/// @param column the column in the file
/// @return the binding
keys::LayoutBinding binding(
    std::string action,
    std::initializer_list<std::string_view> texts,
    uint32_t line,
    uint32_t column
) {
    keys::LayoutBinding out;
    out.action = std::move(action);
    out.line = line;
    out.column = column;
    for (const std::string_view text : texts)
        out.chords.push_back(must_parse(text));
    return out;
}

/// A resolution from the OA default on the US map.
///
/// @param map the keyboard, which the input keeps a pointer to
/// @return the input
keys::ResolveInput fresh(const keys::KeyboardMap& map) {
    keys::ResolveInput input;
    input.base = keys::Base::oa_default;
    input.keyboard = &map;
    return input;
}

/// Finds an action in a result.
///
/// @param resolved the result
/// @param id the action id
/// @return the action, or null
const Bound* find_bound(const Resolved& resolved, std::string_view id) {
    for (const Bound& bound : resolved.actions) {
        if (bound.id == id)
            return &bound;
    }
    return nullptr;
}

/// Reports whether the action's chords are exactly these texts, in order.
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

/// Reports whether a lost chord has this reason and winner.
///
/// @param resolved the result
/// @param action the action that lost
/// @param reason why
/// @param winner the action that kept it
/// @return true when such a loss is listed
bool lost_to(
    const Resolved& resolved, std::string_view action, LostReason reason, std::string_view winner
) {
    for (const Lost& lost : resolved.lost) {
        if (lost.action == action && lost.reason == reason && lost.winner == winner)
            return true;
    }
    return false;
}

/// Reports whether the two actions clash.
///
/// @param clashes the list
/// @param first one id
/// @param second the other id
/// @return true when the pair is listed, in either order
bool clash_of(const std::vector<Clash>& clashes, std::string_view first, std::string_view second) {
    for (const Clash& clash : clashes) {
        const bool forward = clash.a == first && clash.b == second;
        const bool reverse = clash.a == second && clash.b == first;
        if (forward || reverse)
            return true;
    }
    return false;
}

/// Finds a shadow between two actions.
///
/// @param listed the shadows
/// @param winner the action that takes the key
/// @param shadowed the action that waits
/// @return the shadow, or null
const Shadow*
shadow_of(const std::vector<Shadow>& listed, std::string_view winner, std::string_view shadowed) {
    for (const Shadow& shadow : listed) {
        if (shadow.winner == winner && shadow.shadowed == shadowed)
            return &shadow;
    }
    return nullptr;
}

/// Prints the first clashes so a failure names the pair.
///
/// @param input the resolution that produced them
/// @param clashes the clashes
void print_clashes(const keys::ResolveInput& input, const std::vector<Clash>& clashes) {
    std::fprintf(
        stderr,
        "clashes %zu, scheme %u swap %d hacks",
        clashes.size(),
        static_cast<unsigned>(input.scheme),
        input.number_keys_select_groups ? 1 : 0
    );
    for (const std::string& hack : input.hacks_on)
        std::fprintf(stderr, " %s", hack.c_str());
    std::fprintf(stderr, "\n");
    const std::size_t shown = clashes.size() < 12 ? clashes.size() : 12;
    for (std::size_t index = 0; index < shown; ++index) {
        const std::string text = keys::chord_text(clashes[index].chord);
        std::fprintf(
            stderr,
            "  %s / %s %s\n",
            clashes[index].a.c_str(),
            clashes[index].b.c_str(),
            text.c_str()
        );
    }
}

/// Fails when the resolution clashes.
///
/// @param input the resolution
void expect_no_clash(const keys::ResolveInput& input) {
    const Resolved resolved = keys::resolve(input);
    const std::vector<Clash> clashes = keys::find_clashes(resolved, input);
    if (!clashes.empty())
        print_clashes(input, clashes);
    OA_CHECK(clashes.empty());
}

/// The bit of a context.
///
/// @param name the context id
/// @return the bit, or 0
uint32_t context_bit(std::string_view name) {
    const std::span<const acts::Context> table = acts::contexts();
    for (std::size_t index = 0; index < table.size(); ++index) {
        if (table[index].name == name)
            return uint32_t{1} << static_cast<uint32_t>(index);
    }
    return 0;
}

/// The bit of a condition.
///
/// @param name the condition id
/// @return the bit, or 0
uint32_t condition_bit(std::string_view name) {
    const std::span<const std::string_view> table = acts::conditions();
    for (std::size_t index = 0; index < table.size(); ++index) {
        if (table[index] == name)
            return uint32_t{1} << static_cast<uint32_t>(index);
    }
    return 0;
}

/// Reports whether the action's source names the hack.
///
/// @param action the registry row, or null
/// @param hack the hack id
/// @return true when the source lists it
bool source_has(const acts::Action* action, std::string_view hack) {
    if (action == nullptr)
        return false;
    for (const std::string_view source : action->source) {
        if (source == hack)
            return true;
    }
    return false;
}

/// Layers, conditions and same-key, the pieces the clash rule is made of.
void layers_conditions_and_same_key() {
    const uint32_t play = context_bit("play");
    const uint32_t watch = context_bit("watch");
    const uint32_t match = context_bit("match");
    const uint32_t chat = context_bit("chat");
    const uint32_t global = context_bit("global");
    const uint32_t menus = context_bit("menus");
    OA_CHECK(play != 0);
    OA_CHECK(watch != 0);
    OA_CHECK(!keys::layers_meet(play, watch));
    OA_CHECK(keys::layers_meet(play, match));
    OA_CHECK(keys::layers_meet(watch, match));
    OA_CHECK(!keys::layers_meet(chat, play));
    OA_CHECK(keys::layers_meet(chat, chat));
    OA_CHECK(keys::layers_meet(global, play));
    OA_CHECK(keys::layers_meet(global, chat));
    OA_CHECK(!keys::layers_meet(menus, play));
    OA_CHECK(!keys::layers_meet(0, play));

    const uint32_t single = condition_bit("single-player");
    const uint32_t multi = condition_bit("multiplayer");
    OA_CHECK(single != 0);
    OA_CHECK(multi != 0);
    OA_CHECK(!keys::conditions_meet(single, multi));
    OA_CHECK(keys::conditions_meet(0, single));
    OA_CHECK(keys::conditions_meet(single, single));

    const keys::KeyboardMap us = keys::KeyboardMap::us();
    const Chord letter = must_parse("Ctrl+B");
    const Chord place = must_parse("Ctrl+@KeyB");
    OA_CHECK(keys::same_key(letter, place, &us));
    OA_CHECK(!keys::same_key(letter, place, nullptr));
    OA_CHECK(keys::same_key(must_parse("="), must_parse("@Equal"), &us));
    OA_CHECK(!keys::same_key(must_parse("B"), must_parse("@KeyB"), nullptr));
    OA_CHECK(!keys::same_key(letter, must_parse("B"), &us));
}

/// oa-default clashes with nothing, for every hack combination the registry names.
void oa_default_has_no_clash() {
    const keys::KeyboardMap us = keys::KeyboardMap::us();
    const keys::PointerScheme schemes[] = {
        keys::PointerScheme::right_click, keys::PointerScheme::left_click
    };
    const bool swaps[] = {false, true};

    keys::ResolveInput input = fresh(us);
    for (const keys::PointerScheme scheme : schemes) {
        input.scheme = scheme;
        for (const bool swap : swaps) {
            input.number_keys_select_groups = swap;
            input.hacks_on.clear();
            expect_no_clash(input);
        }
    }

    for (const std::string_view id : acts::hack_ids()) {
        input.hacks_on = {std::string{id}};
        for (const keys::PointerScheme scheme : schemes) {
            input.scheme = scheme;
            for (const bool swap : swaps) {
                input.number_keys_select_groups = swap;
                expect_no_clash(input);
            }
        }
    }

    std::vector<std::string> all;
    for (const std::string_view id : acts::hack_ids())
        all.emplace_back(id);
    input.hacks_on = all;
    for (const keys::PointerScheme scheme : schemes) {
        input.scheme = scheme;
        for (const bool swap : swaps) {
            input.number_keys_select_groups = swap;
            expect_no_clash(input);
        }
    }
}

/// With no base, only a fixed action has a chord.
void base_none_binds_only_fixed() {
    const keys::KeyboardMap us = keys::KeyboardMap::us();
    keys::ResolveInput input = fresh(us);
    input.base = keys::Base::none;
    const Resolved resolved = keys::resolve(input);
    const Bound* cancel = find_bound(resolved, "chat.cancel");
    OA_CHECK(cancel != nullptr);
    OA_CHECK(chords_are(cancel, {"Escape"}));
    const Bound* attack = find_bound(resolved, "order.attack");
    OA_CHECK(attack != nullptr && attack->chords.empty());
    const Bound* settings = find_bound(resolved, "ui.settings");
    OA_CHECK(settings != nullptr && settings->chords.empty());
    for (const Bound& bound : resolved.actions) {
        if (bound.chords.empty())
            continue;
        OA_CHECK(bound.entry != nullptr && bound.entry->fixed);
    }
}

/// A chosen layout replaces, keeps what it omits, drops a repeated chord and keeps an unknown name.
void chosen_layout() {
    const keys::KeyboardMap us = keys::KeyboardMap::us();
    keys::Layout layout;
    layout.id = "ridge";
    layout.bindings.push_back(binding("order.attack", {"F"}, 3, 1));
    layout.bindings.push_back(binding("select.all", {"F"}, 8, 5));
    layout.bindings.push_back(binding("order.fight", {"Q"}, 9, 2));
    keys::ResolveInput input = fresh(us);
    input.chosen = &layout;
    const Resolved resolved = keys::resolve(input);

    OA_CHECK(chords_are(find_bound(resolved, "order.attack"), {"F"}));
    OA_CHECK(chords_are(find_bound(resolved, "order.reclaim"), {"E"}));
    OA_CHECK(chords_are(find_bound(resolved, "select.all"), {}));
    OA_CHECK(lost_to(resolved, "select.all", LostReason::earlier_in_file, "order.attack"));
    bool line_kept = false;
    for (const Lost& lost : resolved.lost) {
        if (lost.action == "select.all" && lost.reason == LostReason::earlier_in_file &&
            lost.line == 8 && lost.column == 5) {
            line_kept = true;
        }
    }
    OA_CHECK(line_kept);
    OA_CHECK(resolved.unknown.size() == 1);
    OA_CHECK(resolved.unknown[0].action == "order.fight");
    OA_CHECK(resolved.unknown[0].chords.size() == 1);
    OA_CHECK(keys::chord_text(resolved.unknown[0].chords[0]) == "Q");
    OA_CHECK(find_bound(resolved, "order.fight") == nullptr);
}

/// A gadget word moves the registry action, unless the layout names it. A gui key that clashes is moved.
void mod_letters() {
    const keys::KeyboardMap us = keys::KeyboardMap::us();
    keys::ModData mod;
    keys::GuiKey attack;
    attack.action = "gui.ridge.attack";
    attack.gadget = "ARMATTACK";
    attack.letter = U'T';
    mod.keys.push_back(attack);

    keys::ResolveInput input = fresh(us);
    input.mod = &mod;
    const Resolved moved = keys::resolve(input);
    OA_CHECK(chords_are(find_bound(moved, "order.attack"), {"T"}));

    keys::Layout layout;
    layout.bindings.push_back(binding("order.attack", {"F"}, 1, 1));
    input.chosen = &layout;
    const Resolved named = keys::resolve(input);
    OA_CHECK(chords_are(find_bound(named, "order.attack"), {"F"}));

    keys::ModData build;
    keys::GuiKey tab;
    tab.action = "gui.ridge.build";
    tab.gadget = "CORBUILD";
    tab.letter = U'B';
    build.keys.push_back(tab);
    keys::ResolveInput build_input = fresh(us);
    build_input.mod = &build;
    const Resolved built = keys::resolve(build_input);
    const Bound* gui = find_bound(built, "gui.ridge.build");
    OA_CHECK(gui != nullptr);
    OA_CHECK(gui != nullptr && gui->chords.empty());
    OA_CHECK(chords_are(find_bound(built, "panel.build-tab"), {"B"}));
    OA_CHECK(lost_to(built, "gui.ridge.build", LostReason::moved_by_layout, "panel.build-tab"));
}

/// Own changes take a chord from the action that had it.
void own_changes_take_the_chord() {
    const keys::KeyboardMap us = keys::KeyboardMap::us();
    keys::Layout player;
    player.bindings.push_back(binding("select.all", {"Ctrl+B"}, 4, 2));
    keys::ResolveInput input = fresh(us);
    input.own = &player;
    const Resolved resolved = keys::resolve(input);
    OA_CHECK(chords_are(find_bound(resolved, "select.all"), {"Ctrl+B"}));
    OA_CHECK(chords_are(find_bound(resolved, "select.builders"), {}));
    OA_CHECK(lost_to(resolved, "select.builders", LostReason::later_step, "select.all"));
}

/// A hack that replaces an action takes the chord the player gave the replaced id.
void replaces_follows_the_player() {
    const keys::KeyboardMap us = keys::KeyboardMap::us();
    keys::Layout player;
    player.bindings.push_back(binding("select.builders", {"Ctrl+Q"}, 2, 1));

    keys::ResolveInput on = fresh(us);
    on.own = &player;
    on.hacks_on = {"ui.selection-shortcuts"};
    const Resolved replaced = keys::resolve(on);
    OA_CHECK(find_bound(replaced, "select.builders") == nullptr);
    OA_CHECK(chords_are(find_bound(replaced, "select.idle-builders"), {"Ctrl+Q"}));

    keys::ResolveInput off = fresh(us);
    off.own = &player;
    const Resolved plain = keys::resolve(off);
    OA_CHECK(chords_are(find_bound(plain, "select.builders"), {"Ctrl+Q"}));
    OA_CHECK(find_bound(plain, "select.idle-builders") == nullptr);
}

/// A hack's default replaces the base, and a hack can add a chord.
void hack_defaults() {
    const keys::KeyboardMap us = keys::KeyboardMap::us();
    keys::ResolveInput remaps = fresh(us);
    remaps.hacks_on = {"console.key-remaps"};
    const Resolved remapped = keys::resolve(remaps);
    OA_CHECK(chords_are(find_bound(remapped, "game.repeat-command"), {"Insert"}));

    keys::ResolveInput modes = fresh(us);
    modes.hacks_on = {"ui.display-modes"};
    const Resolved displayed = keys::resolve(modes);
    OA_CHECK(chords_are(find_bound(displayed, "game.screenshot"), {"Ctrl+F9", "PrintScreen"}));
}

/// The left-click scheme and the number-key swap.
void scheme_and_swap() {
    const keys::KeyboardMap us = keys::KeyboardMap::us();
    keys::ResolveInput right = fresh(us);
    right.scheme = keys::PointerScheme::right_click;
    const Resolved right_resolved = keys::resolve(right);
    OA_CHECK(chords_are(find_bound(right_resolved, "pointer.minimap-jump"), {"MouseRight"}));

    keys::ResolveInput left = fresh(us);
    left.scheme = keys::PointerScheme::left_click;
    const Resolved left_resolved = keys::resolve(left);
    OA_CHECK(chords_are(find_bound(left_resolved, "pointer.minimap-jump"), {"MouseLeft"}));
    OA_CHECK(chords_are(find_bound(left_resolved, "pointer.select"), {"MouseLeft"}));
    OA_CHECK(chords_are(find_bound(left_resolved, "pointer.command"), {"MouseLeft"}));
    const std::vector<Clash> left_clashes = keys::find_clashes(left_resolved, left);
    OA_CHECK(!clash_of(left_clashes, "pointer.select", "pointer.command"));

    keys::ResolveInput swapped = fresh(us);
    swapped.number_keys_select_groups = true;
    const Resolved after = keys::resolve(swapped);
    const Bound* select = find_bound(after, "group.select-3");
    const Bound* page = find_bound(after, "build.page-3");
    OA_CHECK(chords_are(select, {"@Digit3", "Numpad3"}));
    OA_CHECK(
        chords_are(page, {"Alt+@Digit3", "Alt+Numpad3", "Alt+Shift+@Digit3", "Alt+Shift+Numpad3"})
    );
    OA_CHECK(select != nullptr && select->chords[0].step == keys::Step::base);
    OA_CHECK(page != nullptr && page->chords[0].step == keys::Step::base);

    keys::Layout layout;
    layout.bindings.push_back(binding("group.select-3", {"F5"}, 1, 1));
    const Chord before = layout.bindings[0].chords[0];
    swapped.chosen = &layout;
    const Resolved written = keys::resolve(swapped);
    OA_CHECK(layout.bindings[0].chords[0] == before);
    OA_CHECK(chords_are(find_bound(written, "build.page-3"), {"F5"}));
    OA_CHECK(chords_are(find_bound(written, "group.select-3"), {"@Digit3", "Numpad3"}));
}

/// The action one key down produces, or empty when it produces none.
///
/// @param resolved the bindings
/// @param map the keyboard
/// @param scancode the key's position
/// @param key_code the key code the event carries
/// @param modifiers the physical modifiers
/// @return the action id
std::string played(
    const Resolved& resolved,
    const keys::KeyboardMap& map,
    uint16_t scancode,
    uint32_t key_code,
    uint8_t modifiers
) {
    keys::Dispatcher dispatcher;
    dispatcher.set_bindings(keys::Resolved{resolved});
    dispatcher.set_keyboard(map);
    keys::Contexts contexts;
    const std::span<const acts::Context> table = acts::contexts();
    for (std::size_t index = 0; index < table.size() && index < 32; ++index) {
        if (table[index].name == "global" || table[index].name == "match" ||
            table[index].name == "play") {
            contexts.active |= uint32_t{1} << static_cast<uint32_t>(index);
        }
    }
    dispatcher.set_contexts(contexts);
    keys::InputEvent event;
    event.kind = keys::InputKind::key;
    event.down = true;
    event.scancode = scancode;
    event.key_code = key_code;
    event.modifiers = modifiers;
    const keys::Fed fed = dispatcher.feed(event);
    if (fed.events.size() != 1)
        return {};
    return std::string{fed.events[0].action};
}

/// With the swap on, Alt+Shift+1 is a page and Shift+1 adds the group.
///
/// oa-default clashes neither way: oa_default_has_no_clash covers that. The
/// keypad follows the digit row. The damage-bar chords on those digits yield.
void number_swap_moves_add_and_pages() {
    const keys::KeyboardMap us = keys::KeyboardMap::us();
    keys::ResolveInput swapped = fresh(us);
    swapped.number_keys_select_groups = true;
    const Resolved after = keys::resolve(swapped);
    OA_CHECK(chords_are(find_bound(after, "group.add-1"), {"Shift+@Digit1", "Shift+Numpad1"}));
    OA_CHECK(chords_are(
        find_bound(after, "build.page-1"),
        {"Alt+@Digit1", "Alt+Numpad1", "Alt+Shift+@Digit1", "Alt+Shift+Numpad1"}
    ));
    OA_CHECK(chords_are(find_bound(after, "group.select-1"), {"@Digit1", "Numpad1"}));
    OA_CHECK(lost_to(after, "game.health-bars-mode-1", LostReason::number_swap, "group.add-1"));
    OA_CHECK(lost_to(after, "game.health-bars-mode-2", LostReason::number_swap, "group.add-3"));
    OA_CHECK(lost_to(after, "game.health-bars-mode-3", LostReason::number_swap, "group.add-8"));
    OA_CHECK(chords_are(find_bound(after, "game.health-bars-mode-1"), {}));
    OA_CHECK(chords_are(find_bound(after, "game.health-bars-mode-4"), {"Shift+@Backquote"}));

    keys::ResolveInput plain = fresh(us);
    const Resolved before = keys::resolve(plain);
    OA_CHECK(
        chords_are(find_bound(before, "group.add-1"), {"Alt+Shift+@Digit1", "Alt+Shift+Numpad1"})
    );
    OA_CHECK(chords_are(find_bound(before, "build.page-1"), {"@Digit1", "Numpad1"}));
    OA_CHECK(chords_are(find_bound(before, "game.health-bars-mode-1"), {"Shift+@Digit1"}));

    const std::optional<uint16_t> digit = keys::scancode_of("Digit1");
    OA_CHECK(digit.has_value());
    const keys::NamedKeySpec* pad = nullptr;
    for (const keys::NamedKeySpec& key : keys::named_keys()) {
        if (key.name == "Numpad1")
            pad = &key;
    }
    OA_CHECK(pad != nullptr);
    const uint8_t alt_shift = static_cast<uint8_t>(keys::alt | keys::shift);
    const uint8_t shift_only = static_cast<uint8_t>(keys::shift);
    const uint16_t digit_place = digit.value_or(0);
    const uint16_t pad_place = pad != nullptr ? pad->scancode : 0;
    const uint32_t pad_code = pad != nullptr ? pad->key_code : 0;
    OA_CHECK(played(after, us, digit_place, 0, alt_shift) == "build.page-1");
    OA_CHECK(played(after, us, digit_place, 0, shift_only) == "group.add-1");
    OA_CHECK(played(after, us, pad_place, pad_code, alt_shift) == "build.page-1");
    OA_CHECK(played(after, us, pad_place, pad_code, shift_only) == "group.add-1");
}

/// Escape stays on chat.cancel. Nothing else is held back.
void fixed_escape() {
    const keys::KeyboardMap us = keys::KeyboardMap::us();
    keys::Layout layout;
    layout.bindings.push_back(binding("chat.cancel", {"F9"}, 1, 1));
    layout.bindings.push_back(binding("chat.send", {"Escape"}, 2, 4));
    keys::ResolveInput input = fresh(us);
    input.chosen = &layout;
    const Resolved resolved = keys::resolve(input);
    const Bound* cancel = find_bound(resolved, "chat.cancel");
    OA_CHECK(chords_are(cancel, {"Escape", "F9"}));
    OA_CHECK(cancel != nullptr && cancel->chords[0].step == keys::Step::base);
    OA_CHECK(cancel != nullptr && cancel->chords[1].step == keys::Step::layout);
    OA_CHECK(chords_are(find_bound(resolved, "chat.send"), {}));
    OA_CHECK(lost_to(resolved, "chat.send", LostReason::fixed, "chat.cancel"));

    keys::Layout settings_layout;
    settings_layout.bindings.push_back(binding("ui.settings", {"F9"}, 1, 1));
    keys::ResolveInput settings_input = fresh(us);
    settings_input.chosen = &settings_layout;
    const Resolved settings = keys::resolve(settings_input);
    OA_CHECK(chords_are(find_bound(settings, "ui.settings"), {"F9"}));
    OA_CHECK(chords_are(find_bound(settings, "chat.cancel"), {"Escape"}));

    keys::Layout back;
    back.bindings.push_back(binding("game.back", {"@KeyJ"}, 1, 1));
    back.bindings.push_back(binding("order.stop", {"Escape"}, 2, 1));
    keys::ResolveInput back_input = fresh(us);
    back_input.chosen = &back;
    const Resolved backed = keys::resolve(back_input);
    OA_CHECK(chords_are(find_bound(backed, "game.back"), {"@KeyJ"}));
    OA_CHECK(chords_are(find_bound(backed, "order.stop"), {"Escape"}));
    OA_CHECK(chords_are(find_bound(backed, "chat.cancel"), {"Escape"}));
    const std::vector<Clash> clashes = keys::find_clashes(backed, back_input);
    OA_CHECK(!clash_of(clashes, "order.stop", "chat.cancel"));
}

/// Pairs that share a key and are not clashes, including a category letter the registry already uses.
void shared_without_a_clash() {
    const keys::KeyboardMap us = keys::KeyboardMap::us();
    keys::ResolveInput input = fresh(us);
    const Resolved resolved = keys::resolve(input);
    const std::vector<Clash> clashes = keys::find_clashes(resolved, input);
    OA_CHECK(!clash_of(clashes, "order.queue-modifier", "select.add-modifier"));
    OA_CHECK(!clash_of(clashes, "order.queue-modifier", "build.queue-modifier"));
    OA_CHECK(!clash_of(clashes, "select.add-modifier", "build.queue-modifier"));
    OA_CHECK(!clash_of(clashes, "debug.refill", "game.speed-up"));
    OA_CHECK(!clash_of(clashes, "chat.send", "chat.open"));
    OA_CHECK(!clash_of(clashes, "select.next-primary", "game.team-menu"));
    OA_CHECK(!clash_of(clashes, "pointer.select", "build.add"));

    const std::vector<Shadow> listed = keys::shadows(resolved, input);
    const Shadow* refill = shadow_of(listed, "debug.refill", "game.speed-up");
    OA_CHECK(refill != nullptr);
    if (refill != nullptr)
        OA_CHECK(refill->kind == ShadowKind::while_conditions);
    const Shadow* chat = shadow_of(listed, "chat.send", "chat.open");
    OA_CHECK(chat != nullptr);
    if (chat != nullptr)
        OA_CHECK(chat->kind == ShadowKind::while_overlay);

    keys::ResolveInput mega = fresh(us);
    mega.hacks_on = {"ui.megamap"};
    const Resolved megamap = keys::resolve(mega);
    const std::vector<Clash> mega_clashes = keys::find_clashes(megamap, mega);
    OA_CHECK(!clash_of(mega_clashes, "megamap.toggle", "select.next-primary"));
    OA_CHECK(!clash_of(mega_clashes, "camera.zoom-in", "megamap.close-wheel"));
    OA_CHECK(chords_are(find_bound(megamap, "megamap.toggle"), {"Tab"}));
    OA_CHECK(chords_are(find_bound(megamap, "select.next-primary"), {"Tab"}));
    const std::vector<Shadow> mega_shadows = keys::shadows(megamap, mega);
    const Shadow* taken = shadow_of(mega_shadows, "megamap.toggle", "select.next-primary");
    OA_CHECK(taken != nullptr);
    if (taken != nullptr) {
        OA_CHECK(taken->kind == ShadowKind::taken_by_hack);
        const Bound* toggle = find_bound(megamap, taken->winner);
        OA_CHECK(toggle != nullptr && source_has(toggle->entry, "ui.megamap"));
    }

    keys::ModData mod;
    mod.categories = "VE";
    keys::ResolveInput letters = fresh(us);
    letters.mod = &mod;
    const Resolved categorised = keys::resolve(letters);
    OA_CHECK(find_bound(categorised, "select.category-v") == nullptr);
    OA_CHECK(chords_are(find_bound(categorised, "select.category-e"), {"Ctrl+E"}));
}

/// A play binding clashes with match and with global, and a letter clashes with its place.
void where_layers_meet() {
    const keys::KeyboardMap us = keys::KeyboardMap::us();
    keys::Layout home;
    home.bindings.push_back(binding("order.move", {"Home"}, 1, 1));
    keys::ResolveInput home_input = fresh(us);
    home_input.chosen = &home;
    const Resolved home_resolved = keys::resolve(home_input);
    const std::vector<Clash> home_clashes = keys::find_clashes(home_resolved, home_input);
    OA_CHECK(clash_of(home_clashes, "order.move", "camera.centre"));
    const Chord home_chord = must_parse("Home");
    const std::vector<Clash> editor =
        keys::clashes_with(home_resolved, home_input, "order.patrol", home_chord);
    OA_CHECK(clash_of(editor, "order.patrol", "camera.centre"));

    keys::Layout full;
    full.bindings.push_back(binding("order.move", {"Alt+Enter"}, 1, 1));
    keys::ResolveInput full_input = fresh(us);
    full_input.chosen = &full;
    const Resolved full_resolved = keys::resolve(full_input);
    OA_CHECK(
        clash_of(keys::find_clashes(full_resolved, full_input), "order.move", "ui.full-screen")
    );

    keys::Layout place;
    place.bindings.push_back(binding("select.all", {"Ctrl+@KeyB"}, 1, 1));
    keys::ResolveInput place_input = fresh(us);
    place_input.chosen = &place;
    const Resolved place_resolved = keys::resolve(place_input);
    OA_CHECK(
        clash_of(keys::find_clashes(place_resolved, place_input), "select.all", "select.builders")
    );
    place_input.keyboard = nullptr;
    OA_CHECK(
        !clash_of(keys::find_clashes(place_resolved, place_input), "select.all", "select.builders")
    );

    keys::ResolveInput plain = fresh(us);
    const Resolved plain_resolved = keys::resolve(plain);
    OA_CHECK(!clash_of(keys::find_clashes(plain_resolved, plain), "pointer.select", "build.add"));
    OA_CHECK(chords_are(find_bound(plain_resolved, "pointer.select"), {"MouseLeft"}));
    OA_CHECK(chords_are(find_bound(plain_resolved, "build.add"), {"MouseLeft"}));
}

/// The same hacks in two orders resolve to the same chords.
void hacks_on_order_does_not_matter() {
    const keys::KeyboardMap us = keys::KeyboardMap::us();
    std::vector<std::string> forward;
    for (const std::string_view id : acts::hack_ids())
        forward.emplace_back(id);
    std::vector<std::string> reverse = forward;
    for (std::size_t index = 0; index < reverse.size() / 2; ++index)
        std::swap(reverse[index], reverse[reverse.size() - 1 - index]);

    keys::ResolveInput first = fresh(us);
    first.hacks_on = forward;
    first.scheme = keys::PointerScheme::left_click;
    first.number_keys_select_groups = true;
    keys::ResolveInput second = first;
    second.hacks_on = std::move(reverse);
    const Resolved a = keys::resolve(first);
    const Resolved b = keys::resolve(second);
    OA_CHECK(a.actions.size() == b.actions.size());
    const std::size_t count =
        a.actions.size() < b.actions.size() ? a.actions.size() : b.actions.size();
    for (std::size_t index = 0; index < count; ++index) {
        OA_CHECK(a.actions[index].id == b.actions[index].id);
        OA_CHECK(a.actions[index].when == b.actions[index].when);
        OA_CHECK(a.actions[index].hack == b.actions[index].hack);
        OA_CHECK(a.actions[index].chords.size() == b.actions[index].chords.size());
        const std::size_t chords = a.actions[index].chords.size() < b.actions[index].chords.size()
                                       ? a.actions[index].chords.size()
                                       : b.actions[index].chords.size();
        for (std::size_t chord_index = 0; chord_index < chords; ++chord_index) {
            OA_CHECK(
                a.actions[index].chords[chord_index].chord ==
                b.actions[index].chords[chord_index].chord
            );
            OA_CHECK(
                a.actions[index].chords[chord_index].step ==
                b.actions[index].chords[chord_index].step
            );
        }
    }
}

/// A fifth chord is too many. The four before it stay.
void fifth_chord_is_too_many() {
    const keys::KeyboardMap us = keys::KeyboardMap::us();
    keys::Layout layout;
    layout.bindings.push_back(binding("order.move", {"F5", "F6", "F7", "F8", "F9"}, 1, 1));
    keys::ResolveInput input = fresh(us);
    input.chosen = &layout;
    const Resolved resolved = keys::resolve(input);
    OA_CHECK(chords_are(find_bound(resolved, "order.move"), {"F5", "F6", "F7", "F8"}));
    bool dropped = false;
    for (const Lost& lost : resolved.lost) {
        if (lost.action == "order.move" && lost.reason == LostReason::too_many &&
            keys::chord_text(lost.chord) == "F9") {
            dropped = true;
        }
    }
    OA_CHECK(dropped);
}

/// Both resource-row double-clicks stay bound. Precedence does not delete a chord.
void resource_rows_keep_both_chords() {
    const keys::KeyboardMap us = keys::KeyboardMap::us();
    keys::ResolveInput input = fresh(us);
    input.hacks_on = {"ui.resource-panel"};
    const Resolved resolved = keys::resolve(input);
    OA_CHECK(chords_are(find_bound(resolved, "watch.player-view"), {"MouseLeft:double"}));
    OA_CHECK(chords_are(find_bound(resolved, "watch.lock-view"), {"MouseLeft:double"}));
    OA_CHECK(
        !clash_of(keys::find_clashes(resolved, input), "watch.player-view", "watch.lock-view")
    );
}

} // namespace

int main() {
    layers_conditions_and_same_key();
    oa_default_has_no_clash();
    base_none_binds_only_fixed();
    chosen_layout();
    mod_letters();
    own_changes_take_the_chord();
    replaces_follows_the_player();
    hack_defaults();
    scheme_and_swap();
    number_swap_moves_add_and_pages();
    fixed_escape();
    shared_without_a_clash();
    where_layers_meet();
    hacks_on_order_does_not_matter();
    fifth_chord_is_too_many();
    resource_rows_keep_both_chords();
    return oa::test::check_exit_status();
}

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The mouse picture and the table of roles. The scheme passed in decides
// what a click does. The layout's bindings are not written.

#include "oa/ui/key_editor/model.hpp"

#include "oa/data/input_actions/actions.hpp"
#include "oa/ui/key_bindings/chord.hpp"
#include "oa/ui/key_bindings/layout.hpp"

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::key_editor {
namespace {

namespace keys = oa::ui::key_bindings;
namespace acts = oa::data::input_actions;

/// One area's tint and letter. A tint is set only when the letter is too.
struct AreaStyle {
    std::string_view id{};
    kit::KeyTint tint{kit::KeyTint::none};
    std::string_view letter{};
};

constexpr AreaStyle kStyles[] = {
    {"orders", kit::KeyTint::orders, words::letter_orders},
    {"mouse", kit::KeyTint::pointer, words::letter_pointer},
    {"selection", kit::KeyTint::selection, words::letter_selection},
    {"groups", kit::KeyTint::groups, words::letter_groups},
    {"camera", kit::KeyTint::camera, words::letter_camera},
    {"building", kit::KeyTint::building, words::letter_building},
    {"game", kit::KeyTint::game, words::letter_game},
    {"chat", kit::KeyTint::chat, words::letter_chat},
    {"window", kit::KeyTint::window, words::letter_window},
    {"developer", kit::KeyTint::developer, words::letter_developer},
    {"megamap", kit::KeyTint::hacks, words::letter_hacks},
    {"whiteboard", kit::KeyTint::hacks, words::letter_hacks},
};

/// The playing context's bit.
constexpr uint32_t kPlay = 1U << 4;

/// Which picture button a chord lands on. The order is MouseButton's.
enum class Slot : uint8_t {
    left,
    right,
    middle,
    wheel_up,
    wheel_down,
    back,
    forward,
    button6,
    button7,
    button8,
    none,
};

/// One place in the table, in the order the table lists them.
struct PlaceWant {
    std::string_view id{};
    std::string_view word{};
    acts::Target target{};
};

/// The action's short name, already looked up.
///
/// @param bound the action
/// @param hooks the lookup
/// @return the name
std::string action_name(const keys::Bound& bound, const TextHooks& hooks) {
    if (bound.entry != nullptr && !bound.entry->words.name.empty())
        return show(hooks, bound.entry->words.name);
    return bound.id;
}

/// How well a context suits a button's face.
///
/// @param contexts the action's context bits
/// @return 2 in play, 1 when it meets play, otherwise 0
int face_rank(uint32_t contexts) noexcept {
    if ((contexts & kPlay) != 0)
        return 2;
    if (keys::layers_meet(contexts, kPlay))
        return 1;
    return 0;
}

/// The picture button a mouse or wheel chord uses.
///
/// @param chord the chord
/// @return the button, or none
Slot slot_of(const keys::Chord& chord) noexcept {
    if (chord.kind == keys::TriggerKind::mouse) {
        switch (chord.button) {
        case 1:
            return Slot::left;
        case 2:
            return Slot::right;
        case 3:
            return Slot::middle;
        case 4:
            return Slot::back;
        case 5:
            return Slot::forward;
        case 6:
            return Slot::button6;
        case 7:
            return Slot::button7;
        case 8:
            return Slot::button8;
        default:
            return Slot::none;
        }
    }
    if (chord.kind == keys::TriggerKind::wheel) {
        if (chord.wheel == 1)
            return Slot::wheel_up;
        if (chord.wheel == -1)
            return Slot::wheel_down;
    }
    return Slot::none;
}

/// The word of a picture button.
///
/// @param slot the button
/// @return the word
std::string_view button_word(Slot slot) noexcept {
    switch (slot) {
    case Slot::left:
        return words::button_left;
    case Slot::right:
        return words::button_right;
    case Slot::middle:
        return words::button_middle;
    case Slot::wheel_up:
    case Slot::wheel_down:
        return words::button_wheel;
    case Slot::back:
        return words::card_back;
    case Slot::forward:
        return words::button_forward;
    case Slot::button6:
        return words::button_6;
    case Slot::button7:
        return words::button_7;
    case Slot::button8:
        return words::button_8;
    case Slot::none:
        return {};
    }
    return {};
}

/// Puts an area's tint and letter on a button.
///
/// @param[in,out] cap the button
/// @param area the area id
/// @param hooks the lookup
void apply_style(kit::KeycapLook& cap, std::string_view area, const TextHooks& hooks) {
    for (const AreaStyle& style : kStyles) {
        if (style.id != area || style.letter.empty() || style.tint == kit::KeyTint::none)
            continue;
        const std::string letter = show(hooks, style.letter);
        if (letter.empty())
            return;
        cap.tint = style.tint;
        cap.letter = letter;
        return;
    }
}

/// Copies a resolution input and selects the scheme the picture is showing.
///
/// @param input the editor's resolution input
/// @param scheme the scheme
/// @return the input with that scheme
keys::ResolveInput with_scheme(keys::ResolveInput input, keys::PointerScheme scheme) {
    input.scheme = scheme;
    return input;
}

/// One role drawn on a button.
struct Role {
    std::string name{};
    std::string id{};
    std::string area{};
    keys::Step step{keys::Step::base};
    int rank{0};
    int order{1000};         ///< the catalogue order; later when the action is made
    bool battlefield{false}; ///< the role is read on the battlefield
};

/// True when `role` should be the name on the button instead of `best`.
///
/// A higher face rank wins. A battlefield role then beats one that only
/// meets play, and an earlier catalogue order beats a later one.
///
/// @param role the candidate
/// @param best the name so far
/// @return true when role replaces best
bool better_role(const Role& role, const Role& best) noexcept {
    if (role.rank != best.rank)
        return role.rank > best.rank;
    if (role.battlefield != best.battlefield)
        return role.battlefield;
    if (role.order != best.order)
        return role.order < best.order;
    return false;
}

} // namespace

kit::MouseLook mouse_look(const Editor& editor, keys::PointerScheme scheme) {
    const keys::ResolveInput input = with_scheme(editor.resolve_input(), scheme);
    const keys::Resolved bound = keys::resolve(input);
    const std::vector<keys::Clash> clashes = keys::find_clashes(bound, input);
    const TextHooks& hooks = editor.text();

    std::array<std::vector<Role>, 10> roles;
    for (const keys::Bound& action : bound.actions) {
        const int rank = face_rank(action.contexts);
        if (rank == 0)
            continue;
        for (const keys::BoundChord& chord : action.chords) {
            const Slot slot = slot_of(chord.chord);
            if (slot == Slot::none)
                continue;
            Role role;
            role.name = action_name(action, hooks);
            role.id = action.id;
            if (action.entry != nullptr)
                role.area = std::string(action.entry->area);
            role.step = chord.step;
            role.rank = rank;
            if (action.entry != nullptr) {
                role.order = static_cast<int>(action.entry->order);
                role.battlefield = action.entry->target == acts::Target::battlefield;
            }
            roles[static_cast<std::size_t>(slot)].push_back(std::move(role));
        }
    }

    kit::MouseLook look;
    for (std::size_t index = 0; index < roles.size(); ++index) {
        const std::vector<Role>& group = roles[index];
        kit::KeycapLook& cap = look.buttons[index];
        if (group.empty()) {
            cap.label = show(hooks, words::free_role);
            continue;
        }
        const Role* best = &group.front();
        for (const Role& role : group) {
            if (better_role(role, *best))
                best = &role;
        }
        cap.label = best->name;
        std::string joined = best->name;
        for (const Role& role : group) {
            if (&role == best || role.name.empty() || role.name == best->name)
                continue;
            joined += " · ";
            joined += role.name;
        }
        if (joined != best->name)
            cap.sub = std::move(joined);
        apply_style(cap, best->area, hooks);
        const Slot slot = static_cast<Slot>(index);
        bool clash = false;
        for (const keys::Clash& one : clashes) {
            if (slot_of(one.chord) == slot)
                clash = true;
        }
        if (clash)
            cap.mark = kit::KeyMark::clash;
        else if (best->step == keys::Step::own)
            cap.mark = kit::KeyMark::changed;
    }
    return look;
}

std::vector<RoleLine> mouse_table(const Editor& editor, keys::PointerScheme scheme) {
    const keys::ResolveInput input = with_scheme(editor.resolve_input(), scheme);
    const keys::Resolved bound = keys::resolve(input);
    const TextHooks& hooks = editor.text();
    const PlaceWant places[] = {
        {"playing", words::card_while_playing, acts::Target::battlefield},
        {"minimap", words::place_minimap, acts::Target::minimap},
        {"build-button", words::place_build, acts::Target::build_button},
        {"whiteboard", words::place_whiteboard, acts::Target::marker},
    };
    std::vector<RoleLine> rows;
    for (const PlaceWant& place : places) {
        const std::size_t before = rows.size();
        for (const keys::Bound& action : bound.actions) {
            if (action.entry == nullptr || action.entry->target != place.target)
                continue;
            if (action.entry->kind != acts::Kind::pointer &&
                action.entry->kind != acts::Kind::wheel)
                continue;
            for (const keys::BoundChord& chord : action.chords) {
                const Slot slot = slot_of(chord.chord);
                if (slot == Slot::none)
                    continue;
                RoleLine line;
                line.place = show(hooks, place.word);
                line.place_id = std::string(place.id);
                line.button = show(hooks, button_word(slot));
                line.role = action_name(action, hooks);
                line.action = action.id;
                line.chord = chord.chord;
                line.has_chord = true;
                if (chord.step == keys::Step::own)
                    line.mark = kit::KeyMark::changed;
                rows.push_back(std::move(line));
            }
        }
        if (rows.size() == before) {
            RoleLine empty;
            empty.place = show(hooks, place.word);
            empty.place_id = std::string(place.id);
            empty.role = show(hooks, words::free_role);
            empty.free = true;
            rows.push_back(std::move(empty));
        }
    }
    return rows;
}

} // namespace oa::ui::key_editor

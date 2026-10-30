// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The keyboard picture and a key's card. The kit draws the picture. This
// file only decides what each key says, in the player's letters.

#include "oa/ui/key_editor/model.hpp"

#include "oa/data/input_actions/actions.hpp"
#include "oa/ui/key_bindings/chord.hpp"
#include "oa/ui/key_bindings/key_codes.hpp"
#include "oa/ui/key_bindings/layout.hpp"
#include "oa/ui/kit/keys.hpp"

#include <memory>
#include <optional>
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

/// The areas a key can wear. Watching and Menus have no tinted letter.
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
    {"mod.menus", kit::KeyTint::mod, words::letter_mod},
    {"mod.categories", kit::KeyTint::mod, words::letter_mod},
};

/// The playing context's bit.
constexpr uint32_t kPlay = 1U << 4;
/// The watching context's bit.
constexpr uint32_t kWatch = 1U << 5;
/// The placing context's bit.
constexpr uint32_t kPlacing = 1U << 11;
/// The context that meets every other one.
constexpr uint32_t kEverywhere = 1U << 0;

/// Folds one ASCII letter. Any other character is kept.
///
/// @param character the character
/// @return the folded character
char32_t fold_char(char32_t character) noexcept {
    if (character >= U'A' && character <= U'Z')
        return character - U'A' + U'a';
    return character;
}

/// Writes one character as UTF-8.
///
/// @param character the character
/// @return the bytes
std::string utf8_of(char32_t character) {
    std::string text;
    if (character < 0x80) {
        text.push_back(static_cast<char>(character));
    } else if (character < 0x800) {
        text.push_back(static_cast<char>(0xC0 | (character >> 6)));
        text.push_back(static_cast<char>(0x80 | (character & 0x3F)));
    } else if (character < 0x10000) {
        text.push_back(static_cast<char>(0xE0 | (character >> 12)));
        text.push_back(static_cast<char>(0x80 | ((character >> 6) & 0x3F)));
        text.push_back(static_cast<char>(0x80 | (character & 0x3F)));
    } else {
        text.push_back(static_cast<char>(0xF0 | (character >> 18)));
        text.push_back(static_cast<char>(0x80 | ((character >> 12) & 0x3F)));
        text.push_back(static_cast<char>(0x80 | ((character >> 6) & 0x3F)));
        text.push_back(static_cast<char>(0x80 | (character & 0x3F)));
    }
    return text;
}

/// Primary folded into Ctrl or Meta, so a layer can match either spelling.
///
/// @param bits the chord's modifier bits
/// @param platform which key Primary means
/// @return the bits with Primary rewritten
uint8_t folded_modifiers(uint8_t bits, keys::Platform platform) noexcept {
    if ((bits & keys::primary) == 0)
        return bits;
    auto folded = static_cast<uint8_t>(bits & static_cast<uint8_t>(~keys::primary));
    if (platform.primary_is_meta)
        folded = static_cast<uint8_t>(folded | keys::meta);
    else
        folded = static_cast<uint8_t>(folded | keys::ctrl);
    return folded;
}

/// True when a chord's trigger is this key, whatever its modifiers.
///
/// A letter matches the key that prints it, or whose events carry it.
///
/// @param chord the chord
/// @param code the W3C code
/// @param map the player's keyboard
/// @return true when the chord is that key
bool trigger_matches(
    const keys::Chord& chord, std::string_view code, const keys::KeyboardMap& map
) {
    const std::optional<uint16_t> scan = keys::scancode_of(code);
    if (!scan)
        return false;
    if (chord.kind == keys::TriggerKind::place)
        return chord.scancode == *scan;
    if (chord.kind == keys::TriggerKind::named) {
        if (chord.scancode != 0 && chord.scancode == *scan)
            return true;
        for (const keys::NamedKeySpec& spec : keys::named_keys()) {
            if (spec.scancode == *scan &&
                (chord.named == static_cast<uint8_t>(spec.id) || chord.scancode == spec.scancode))
                return true;
        }
        return false;
    }
    if (chord.kind != keys::TriggerKind::character)
        return false;
    const char32_t want = fold_char(chord.character);
    const char32_t printed = map.printed(*scan);
    if (printed != 0 && fold_char(printed) == want)
        return true;
    const uint32_t event = map.event_key_code(*scan);
    if (event != 0 && event <= 0x10FFFF && fold_char(static_cast<char32_t>(event)) == want)
        return true;
    return false;
}

/// How well a context suits a key's face. Play itself wins over a context
/// that only meets play. Anything else is not a face.
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

/// The word a named key shows, or empty.
///
/// @param code the W3C code
/// @param labels the named keys' words
/// @return the word
std::string named_word(std::string_view code, const Labels& labels) {
    for (const NamedLabel& label : labels.keys) {
        if (label.code == code)
            return label.word;
    }
    return {};
}

/// What the key prints, in capitals, or empty when it does not print a mark.
///
/// A space is left to the named word. A control character is not a label.
///
/// @param code the W3C code
/// @param map the player's keyboard
/// @return the label
std::string printed_label(std::string_view code, const keys::KeyboardMap& map) {
    const std::optional<uint16_t> scan = keys::scancode_of(code);
    if (!scan)
        return {};
    char32_t printed = map.printed(*scan);
    if (printed < 0x21 || printed == 0x7F)
        return {};
    if (printed >= U'a' && printed <= U'z')
        printed = printed - U'a' + U'A';
    return utf8_of(printed);
}

/// The name a Mac's modifier key shows: ⌘ Cmd, ⌥ Option, ⌃ Ctrl and ⇧ Shift,
/// or the name alone where the platform shows no symbols. Off a Mac, and for
/// any other key, it is empty, and the geometry's own label shows.
///
/// @param code the W3C code
/// @param platform which key Primary means, and how the modifiers are named
/// @return the name, or empty
std::string mac_modifier_label(std::string_view code, keys::Platform platform) {
    if (!platform.primary_is_meta)
        return {};
    if (code == "ShiftLeft" || code == "ShiftRight")
        return keys::modifier_name(keys::shift, platform);
    if (code == "ControlLeft" || code == "ControlRight")
        return keys::modifier_name(keys::ctrl, platform);
    if (code == "AltLeft" || code == "AltRight")
        return keys::modifier_name(keys::alt, platform);
    if (code == "MetaLeft" || code == "MetaRight")
        return keys::modifier_name(keys::meta, platform);
    return {};
}

/// True when this key is the modifier the layer is holding.
///
/// @param code the W3C code
/// @param layer the layer's modifier bit, or 0
/// @return true for that modifier's keys
bool modifier_held(std::string_view code, uint8_t layer) noexcept {
    if (layer == keys::shift)
        return code == "ShiftLeft" || code == "ShiftRight";
    if (layer == keys::ctrl)
        return code == "ControlLeft" || code == "ControlRight";
    if (layer == keys::alt)
        return code == "AltLeft" || code == "AltRight";
    if (layer == keys::meta)
        return code == "MetaLeft" || code == "MetaRight";
    return false;
}

/// The action's short name, already looked up.
///
/// A menu button with no registry name uses the name the list shows, which
/// is the text the button draws. The id stays in the line's id.
///
/// @param editor the list's names
/// @param bound the action
/// @param hooks the lookup
/// @return the name
std::string action_name(const Editor& editor, const keys::Bound& bound, const TextHooks& hooks) {
    if (bound.entry != nullptr && !bound.entry->words.name.empty())
        return show(hooks, bound.entry->words.name);
    const std::string named = editor.details(bound.id).name;
    if (!named.empty())
        return named;
    return bound.id;
}

/// The digits after a prefix, or empty when the rest is not a number.
///
/// `build.page-next` is not page "next": only a tail of digits counts.
///
/// @param id the action id
/// @param prefix the part before the number
/// @return the digits
std::string number_tail(std::string_view id, std::string_view prefix) {
    if (!id.starts_with(prefix))
        return {};
    const std::string_view tail = id.substr(prefix.size());
    if (tail.empty())
        return {};
    for (const char digit : tail) {
        if (digit < '0' || digit > '9')
            return {};
    }
    return std::string(tail);
}

/// The short caption a key shows. The card keeps the catalogue's full name.
///
/// A numbered family shares one word. A category is "Cat." and its letter.
/// Anything else keeps the catalogue name, which is already short for
/// Attack, Settings and the other one-word actions.
///
/// @param editor the list's names
/// @param bound the action
/// @param hooks the lookup
/// @return the caption
std::string key_caption(const Editor& editor, const keys::Bound& bound, const TextHooks& hooks) {
    const std::string_view id = bound.id;

    struct Numbered {
        std::string_view prefix;
        std::string_view word;
    };

    static constexpr Numbered numbered[] = {
        {"group.assign-", words::cap_store},
        {"group.select-", words::cap_group},
        {"group.add-", "Add"},
        {"camera.bookmark-set-", words::cap_set},
        {"camera.bookmark-go-", "View"},
        {"build.page-", words::cap_page},
    };
    for (const Numbered& row : numbered) {
        const std::string tail = number_tail(id, row.prefix);
        if (tail.empty())
            continue;
        std::string text = show(hooks, row.word);
        text += ' ';
        text += tail;
        return text;
    }

    struct Exact {
        std::string_view action;
        std::string_view word;
    };

    static constexpr Exact exact[] = {
        {"select.all", "All"},
        {"select.builders", words::cap_sel_bld},
        {"select.factories", words::cap_sel_fac},
        {"select.aircraft", "Air"},
        {"select.commander", words::cap_cmdr},
        {"select.same-type", words::cap_same},
        {"select.armed-on-screen", words::cap_armed},
        {"select.idle-builders", words::cap_idle_bld},
        {"select.idle-factories", words::cap_idle_fac},
        {"select.box-combat", words::cap_box_armed},
        {"select.box-factories", words::cap_box_fact},
        {"select.box-builders", words::cap_box_bld},
        {"select.next-primary", "Next"},
        {"order.self-destruct", words::cap_destruct},
        {"order.on-off", words::cap_on_off},
        {"order.fire-state", words::cap_fire},
        {"order.move-state", words::cap_moves},
        {"order.force-modifier", words::cap_force},
        {"panel.build-tab", words::cap_build},
        {"panel.orders-tab", "Orders"},
        {"game.screenshot", words::cap_shot},
        {"game.resource-panel", words::cap_resources},
        {"game.kill-board", words::cap_kills},
        {"game.chat-macro", words::cap_macro},
        {"game.clear-messages", words::cap_clear},
        {"game.health-bars", words::cap_bars},
        {"game.speed-up", words::cap_faster},
        {"game.speed-down", words::cap_slower},
        {"game.pin-unit", words::cap_pin},
        {"game.status-strip", words::cap_status},
        {"whiteboard.jump", words::cap_marker},
        {"build.page-previous", words::cap_prev_pg},
        {"build.page-next", words::cap_next_pg},
        {"camera.follow-previous", words::cap_follow_back},
        {"build.spacing-up", words::cap_space_up},
        {"build.spacing-down", words::cap_space_down},
        {"ui.keys-overlay", "Keys"},
    };
    for (const Exact& row : exact) {
        if (row.action == id)
            return show(hooks, row.word);
    }
    constexpr std::string_view category = "select.category-";
    if (id.starts_with(category) && id.size() == category.size() + 1) {
        const char raw = id.back();
        const char letter = raw >= 'a' && raw <= 'z' ? static_cast<char>(raw - 'a' + 'A') : raw;
        std::string text = show(hooks, words::cap_cat);
        text += ' ';
        text.push_back(letter);
        return text;
    }
    return action_name(editor, bound, hooks);
}

/// Puts an area's tint and letter on a key. A tint without a letter is not set.
///
/// @param[in,out] cap the key
/// @param area the area id
/// @param hooks the lookup
void apply_style(kit::KeycapLook& cap, std::string_view area, const TextHooks& hooks) {
    for (const AreaStyle& style : kStyles) {
        if (style.id != area || style.letter.empty())
            continue;
        const std::string letter = show(hooks, style.letter);
        if (letter.empty() || style.tint == kit::KeyTint::none)
            return;
        cap.tint = style.tint;
        cap.letter = letter;
        return;
    }
}

/// Where a card line is read.
///
/// @param contexts the action's context bits
/// @param hooks the lookup
/// @return the phrase
std::string card_context(uint32_t contexts, const TextHooks& hooks) {
    if ((contexts & kPlay) != 0)
        return show(hooks, words::card_while_playing);
    if ((contexts & kWatch) != 0)
        return show(hooks, words::card_while_watching);
    if ((contexts & kPlacing) != 0)
        return show(hooks, words::card_placing);
    if ((contexts & kEverywhere) != 0)
        return show(hooks, words::context_everywhere);
    std::string line;
    const std::size_t count = acts::contexts().size();
    for (std::size_t index = 0; index < count; ++index) {
        if ((contexts & (1U << index)) == 0)
            continue;
        std::string_view word;
        switch (index) {
        case 1:
            word = words::context_oa;
            break;
        case 2:
            word = words::context_menus;
            break;
        case 3:
            word = words::context_match;
            break;
        case 6:
            word = words::context_chat;
            break;
        case 7:
            word = words::context_panels;
            break;
        case 8:
            word = words::context_unit_info;
            break;
        case 9:
            word = words::context_megamap;
            break;
        case 10:
            word = words::context_whiteboard;
            break;
        case 12:
            word = words::context_box;
            break;
        default:
            break;
        }
        if (word.empty())
            continue;
        if (!line.empty())
            line += ", ";
        line += show(hooks, word);
    }
    return line;
}

/// A source id as a short phrase. `ui.selection-shortcuts` is "Selection shortcuts".
///
/// @param source the source id
/// @return the phrase
std::string pretty_source(std::string_view source) {
    if (source.starts_with("ui."))
        source.remove_prefix(3);
    std::string text(source);
    for (char& character : text) {
        if (character == '-' || character == '.' || character == '_')
            character = ' ';
    }
    if (!text.empty() && text.front() >= 'a' && text.front() <= 'z')
        text.front() = static_cast<char>(text.front() - 'a' + 'A');
    return text;
}

/// The note under one chord: a gesture, a hack standing in, and the player's change.
///
/// @param bound the action
/// @param chord the chord
/// @param hooks the lookup
/// @return the note; empty when the chord is an ordinary press
std::string
chord_note(const keys::Bound& bound, const keys::BoundChord& chord, const TextHooks& hooks) {
    std::string note;
    if (chord.chord.gesture == keys::Gesture::double_press)
        note = show(hooks, words::a_double_press);
    else if (chord.chord.gesture == keys::Gesture::hold)
        note = show(hooks, words::a_hold);
    else if (chord.chord.gesture == keys::Gesture::release)
        note = show(hooks, words::a_release);
    if (bound.entry != nullptr && !bound.entry->replaces.empty() && !bound.entry->source.empty()) {
        const acts::Action* replaced = acts::find(bound.entry->replaces);
        const std::string instead = replaced != nullptr ? show(hooks, replaced->words.name)
                                                        : std::string(bound.entry->replaces);
        const std::string standing = pretty_source(bound.entry->source.front()) + ", " +
                                     std::string(show(hooks, words::in_place_of)) + " " + instead;
        if (!note.empty())
            note += " · ";
        note += standing;
    }
    if (chord.step == keys::Step::own) {
        if (!note.empty())
            note += " ";
        note += show(hooks, words::your_change);
    }
    return note;
}

/// True when a clash is this key in this layer.
///
/// @param clashes the clashes
/// @param code the W3C code
/// @param layer the layer's modifier bit, or 0
/// @param map the player's keyboard
/// @param platform which key Primary means
/// @return true when one is
bool clashing_key(
    const std::vector<keys::Clash>& clashes,
    std::string_view code,
    uint8_t layer,
    const keys::KeyboardMap& map,
    keys::Platform platform
) {
    for (const keys::Clash& clash : clashes) {
        if (folded_modifiers(clash.chord.modifiers, platform) != layer)
            continue;
        if (trigger_matches(clash.chord, code, map))
            return true;
    }
    return false;
}

/// The best face on one key: a plain press of exactly the layer.
struct Face {
    std::size_t action{0}; ///< the action's place in the resolved list
    std::size_t chord{0};  ///< the chord's place on that action
    int rank{0};           ///< face_rank
};

/// The plain press that names the key, if one is read while playing.
///
/// @param bound the resolved actions
/// @param code the W3C code
/// @param layer the layer
/// @param map the player's keyboard
/// @param platform which key Primary means
/// @return the face, or nothing
std::optional<Face> best_face(
    const keys::Resolved& bound,
    std::string_view code,
    uint8_t layer,
    const keys::KeyboardMap& map,
    keys::Platform platform
) {
    std::optional<Face> best;
    for (std::size_t action_at = 0; action_at < bound.actions.size(); ++action_at) {
        const keys::Bound& action = bound.actions[action_at];
        const int rank = face_rank(action.contexts);
        if (rank == 0)
            continue;
        for (std::size_t chord_at = 0; chord_at < action.chords.size(); ++chord_at) {
            const keys::BoundChord& chord = action.chords[chord_at];
            if (chord.chord.gesture != keys::Gesture::press)
                continue;
            if (folded_modifiers(chord.chord.modifiers, platform) != layer)
                continue;
            if (!trigger_matches(chord.chord, code, map))
                continue;
            if (best && rank <= best->rank)
                continue;
            Face face;
            face.action = action_at;
            face.chord = chord_at;
            face.rank = rank;
            best = face;
        }
    }
    return best;
}

} // namespace

kit::KeyboardLook
keyboard_look(const Editor& editor, const KeyboardViewInputs& inputs, const Labels& labels) {
    const std::shared_ptr<const Editor::InUse> kept = editor.in_use();
    const keys::Resolved& bound = kept->resolved;
    const std::vector<keys::Clash>& clashes = kept->clashes;
    const keys::KeyboardMap& map = editor.keyboard();
    const keys::Platform platform = editor.platform();
    const TextHooks& hooks = editor.text();

    kit::KeyboardLook look;
    look.shape = inputs.shape;
    for (const kit::KeyPlace& place : kit::keyboard_places(inputs.shape)) {
        kit::KeyboardKey key;
        key.code = std::string(place.code);
        std::string word = named_word(place.code, labels);
        if (word.empty())
            word = mac_modifier_label(place.code, platform);
        key.cap.label = word.empty() ? printed_label(place.code, map) : word;
        key.cap.held = modifier_held(place.code, inputs.layer);
        const std::optional<Face> face = best_face(bound, place.code, inputs.layer, map, platform);
        if (face) {
            const keys::Bound& action = bound.actions[face->action];
            const keys::BoundChord& chord = action.chords[face->chord];
            key.cap.sub = key_caption(editor, action, hooks);
            if (action.entry != nullptr)
                apply_style(key.cap, action.entry->area, hooks);
            if (clashing_key(clashes, place.code, inputs.layer, map, platform))
                key.cap.mark = kit::KeyMark::clash;
            else if (chord.step == keys::Step::own)
                key.cap.mark = kit::KeyMark::changed;
        }
        const bool labelled = !key.cap.label.empty();
        const bool named = !key.cap.sub.empty();
        if (!labelled && !named && !key.cap.held && key.cap.mark == kit::KeyMark::none)
            continue;
        look.keys.push_back(std::move(key));
    }
    return look;
}

std::vector<CardLine> key_card(const Editor& editor, std::string_view code, uint8_t layer) {
    const std::shared_ptr<const Editor::InUse> kept = editor.in_use();
    const keys::Resolved& bound = kept->resolved;
    const keys::KeyboardMap& map = editor.keyboard();
    const keys::Platform platform = editor.platform();
    const TextHooks& hooks = editor.text();
    std::vector<CardLine> lines;
    for (const keys::Bound& action : bound.actions) {
        if (action.id == "chat.cancel" || action.id == "ui.screen-cancel")
            continue;
        for (const keys::BoundChord& chord : action.chords) {
            if (folded_modifiers(chord.chord.modifiers, platform) != layer)
                continue;
            if (!trigger_matches(chord.chord, code, map))
                continue;
            CardLine line;
            line.context = card_context(action.contexts, hooks);
            line.action = action_name(editor, action, hooks);
            line.note = chord_note(action, chord, hooks);
            line.id = action.id;
            lines.push_back(std::move(line));
        }
    }
    if (code == "Escape" && layer == 0) {
        CardLine chat;
        chat.context = show(hooks, words::card_chat_line);
        chat.action = show(hooks, words::card_close_line);
        chat.note = show(hooks, words::card_always);
        chat.id = "chat.cancel";
        chat.fixed = true;
        lines.push_back(std::move(chat));
        CardLine screens;
        screens.context = show(hooks, words::card_oa_screens);
        screens.action = show(hooks, words::card_back);
        screens.note = show(hooks, words::card_always);
        screens.id = "ui.screen-cancel";
        screens.fixed = true;
        lines.push_back(std::move(screens));
    }
    return lines;
}

std::vector<std::string> keys_of(const Editor& editor, std::string_view action) {
    const std::shared_ptr<const Editor::InUse> kept = editor.in_use();
    const keys::Resolved& bound = kept->resolved;
    const keys::Bound* found = nullptr;
    for (const keys::Bound& row : bound.actions) {
        if (row.id == action)
            found = &row;
    }
    std::vector<std::string> codes;
    if (found == nullptr)
        return codes;
    const keys::KeyboardMap& map = editor.keyboard();
    for (const keys::KeyPosition& position : keys::key_positions()) {
        bool hit = false;
        for (const keys::BoundChord& chord : found->chords) {
            if (trigger_matches(chord.chord, position.code, map))
                hit = true;
        }
        if (!hit)
            continue;
        bool listed = false;
        for (const std::string& code : codes) {
            if (code == position.code)
                listed = true;
        }
        if (!listed)
            codes.emplace_back(position.code);
    }
    return codes;
}

Labels key_labels(const TextHooks& hooks) {
    Labels labels;
    const auto add = [&](const char* code, std::string_view word) {
        NamedLabel label;
        label.code = code;
        label.word = show(hooks, word);
        labels.keys.push_back(std::move(label));
    };
    add("Space", words::key_space);
    add("Enter", words::key_enter);
    add("NumpadEnter", words::key_enter);
    add("Tab", words::key_tab);
    return labels;
}

} // namespace oa::ui::key_editor

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/key_editor/model.hpp"

#include "oa/data/input_actions/actions.hpp"
#include "oa/ui/key_bindings/chord.hpp"
#include "oa/ui/key_bindings/layout_file.hpp"
#include "oa/ui/key_bindings/tags.hpp"

#include <algorithm>
#include <optional>
#include <string>
#include <utility>

namespace oa::ui::key_editor {
namespace {

namespace keys = oa::ui::key_bindings;
namespace acts = oa::data::input_actions;

/// One area the nav can show, in list order.
struct AreaRow {
    std::string_view id{};
    std::string_view title{};
    kit::KeyTint tint{kit::KeyTint::none};
    std::string_view letter{};
    bool main{false};
};

/// The areas, in the order the nav lists them. U is Menus.
constexpr AreaRow kAreas[] = {
    {"orders", words::area_orders, kit::KeyTint::orders, words::letter_orders, true},
    {"mouse", words::area_pointer, kit::KeyTint::pointer, words::letter_pointer, true},
    {"selection", words::area_selection, kit::KeyTint::selection, words::letter_selection, true},
    {"groups", words::area_groups, kit::KeyTint::groups, words::letter_groups, true},
    {"camera", words::area_camera, kit::KeyTint::camera, words::letter_camera, true},
    {"building", words::area_building, kit::KeyTint::building, words::letter_building, true},
    {"game", words::area_game, kit::KeyTint::game, words::letter_game, true},
    {"chat", words::area_chat, kit::KeyTint::chat, words::letter_chat, true},
    {"watching", words::area_watching, kit::KeyTint::none, {}, true},
    {"window", words::area_window, kit::KeyTint::window, words::letter_window, true},
    {"menus", words::area_menus, kit::KeyTint::none, words::letter_menus, true},
    {"developer", words::area_developer, kit::KeyTint::developer, words::letter_developer, false},
    {"megamap", words::area_megamap, kit::KeyTint::hacks, words::letter_hacks, false},
    {"whiteboard", words::area_whiteboard, kit::KeyTint::hacks, words::letter_hacks, false},
};

/// The commands most players change first, in the order Common commands
/// lists them: the orders, then selecting, the camera, the game and chat.
constexpr std::string_view kCommon[] = {
    "order.attack",     "order.move",       "order.stop",       "order.patrol",
    "order.guard",      "order.repair",     "order.reclaim",    "order.capture",
    "order.dgun",       "order.cloak",      "order.load",       "order.unload",
    "order.on-off",     "order.fire-state", "order.move-state", "order.self-destruct",
    "select.all",       "select.builders",  "select.same-type", "select.idle-builders",
    "select.commander", "camera.follow",    "game.pause",       "chat.open",
};

/// True when Common commands lists the action.
///
/// @param id the action id
/// @return true for one of kCommon
bool common_command(std::string_view id) {
    for (const std::string_view common : kCommon) {
        if (common == id)
            return true;
    }
    return false;
}

/// One action as the list and the details read it.
struct Listed {
    std::string id{};
    std::string name{};
    std::string description{};
    std::string aside{};
    std::string category{};
    std::string category_title{};
    kit::KeyMark mark{kit::KeyMark::none};
    std::vector<keys::Chord> chords{};
    std::vector<keys::Step> steps{};
    bool read_only{false};
    bool changed{false};
    bool latchable{false};
    bool confirmable{false};
    bool latch_on{false};
    bool confirm_on{false};
    const acts::Action* entry = nullptr;
    std::string gadget{};
    std::string base{};
    std::string where{};
    std::string kind{};
    std::string works_with{};
    kit::KeyTint tint{kit::KeyTint::none};
    std::string letter{};
};

/// Folds ASCII letters to lower case. Any other byte is kept.
///
/// @param text the text
/// @return the folded text
std::string folded(std::string_view text) {
    std::string lower(text);
    for (char& byte : lower) {
        if (byte >= 'A' && byte <= 'Z')
            byte = static_cast<char>(byte - 'A' + 'a');
    }
    return lower;
}

/// Joins pieces with a separator, skipping empty ones.
///
/// @param parts the pieces
/// @param separator the separator
/// @return the line
std::string joined(const std::vector<std::string>& parts, std::string_view separator) {
    std::string line;
    for (const std::string& part : parts) {
        if (part.empty())
            continue;
        if (!line.empty())
            line += separator;
        line += part;
    }
    return line;
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

/// The area row of an id, or null.
///
/// @param id the area id
/// @return the row
const AreaRow* area_named(std::string_view id) {
    for (const AreaRow& area : kAreas) {
        if (area.id == id)
            return &area;
    }
    return nullptr;
}

/// True when the area is one of the nav's main areas.
///
/// @param id the area id
/// @return true for those areas
bool main_area(std::string_view id) {
    const AreaRow* area = area_named(id);
    return area != nullptr && area->main;
}

/// The English word of one context bit, or empty.
///
/// @param index the bit's index
/// @return the word
std::string_view context_word(std::size_t index) {
    switch (index) {
    case 0:
        return words::context_everywhere;
    case 1:
        return words::context_oa;
    case 2:
        return words::context_menus;
    case 3:
        return words::context_match;
    case 4:
        return words::context_playing;
    case 5:
        return words::context_watching;
    case 6:
        return words::context_chat;
    case 7:
        return words::context_panels;
    case 8:
        return words::context_unit_info;
    case 9:
        return words::context_megamap;
    case 10:
        return words::context_whiteboard;
    case 11:
        return words::context_placing;
    case 12:
        return words::context_box;
    default:
        return {};
    }
}

/// The English word of one condition bit, or empty.
///
/// @param index the bit's index
/// @return the word
std::string_view condition_word(std::size_t index) {
    switch (index) {
    case 0:
        return words::when_single;
    case 1:
        return words::when_multi;
    case 2:
        return words::when_seat;
    case 3:
        return words::when_selection;
    case 4:
        return words::when_developer;
    case 5:
        return words::when_debug;
    case 6:
        return words::when_wheel;
    case 7:
        return words::when_no_wheel;
    case 8:
        return words::when_pointer;
    default:
        return {};
    }
}

/// How an action fires, looked up.
///
/// @param hooks the lookup
/// @param kind how it fires
/// @return the word
std::string kind_text(const TextHooks& hooks, acts::Kind kind) {
    switch (kind) {
    case acts::Kind::repeat:
        return show(hooks, words::kind_repeat);
    case acts::Kind::hold:
        return show(hooks, words::kind_hold);
    case acts::Kind::modifier:
        return show(hooks, words::kind_modifier);
    case acts::Kind::pointer:
        return show(hooks, words::kind_pointer);
    case acts::Kind::wheel:
        return show(hooks, words::kind_wheel);
    case acts::Kind::press:
        break;
    }
    return show(hooks, words::kind_press);
}

/// The kind of a bound action, from its row or its family.
///
/// @param bound the action
/// @return the kind
acts::Kind kind_of(const keys::Bound& bound) {
    if (bound.entry != nullptr)
        return bound.entry->kind;
    for (const acts::Family& family : acts::families()) {
        const std::string dotted = std::string(family.id) + ".";
        const std::string dashed = std::string(family.id) + "-";
        if (bound.id.starts_with(dotted) || bound.id.starts_with(dashed))
            return family.kind;
    }
    return acts::Kind::press;
}

/// The select.tag family, or null.
///
/// @return the family
const acts::Family* tag_family() {
    for (const acts::Family& family : acts::families()) {
        if (family.id == "select.tag")
            return &family;
    }
    return nullptr;
}

/// The word a `select.tag-` id shows. A gathered word keeps the data's
/// spelling. A tag the loaded units lack shows only the id's suffix.
///
/// @param inputs the inputs
/// @param id the action id
/// @return the word, or empty when the id is not a tag
std::string tag_display_word(const EditorInputs& inputs, std::string_view id) {
    constexpr std::string_view prefix = "select.tag-";
    if (!id.starts_with(prefix) || id.size() <= prefix.size())
        return {};
    for (const std::string& word : inputs.mod.tags) {
        if (keys::tag_action_id(word) == id)
            return word;
    }
    return std::string(id.substr(prefix.size()));
}

/// Fills a family template. The template is looked up; the word is not.
///
/// @param hooks the lookup
/// @param pattern the English template
/// @param word the data's spelling, or an unknown id's suffix
/// @return the sentence
std::string tag_sentence(const TextHooks& hooks, std::string_view pattern, std::string_view word) {
    return fill(show(hooks, pattern), {{"word", word}});
}

/// A source token as the details say it.
///
/// @param hooks the lookup
/// @param source the registry's token
/// @return the words
std::string source_text(const TextHooks& hooks, std::string_view source) {
    if (source == "3.1c")
        return show(hooks, words::source_31c);
    if (source == "OA")
        return show(hooks, words::source_oa);
    if (source == "data")
        return show(hooks, words::source_data);
    return std::string(source);
}

/// True when a gadget names an order the registry already has.
///
/// @param gadget the gadget
/// @return true when some action names it
bool order_panel(std::string_view gadget) {
    if (gadget.empty())
        return false;
    for (const acts::Action& action : acts::actions()) {
        if (action.gadget == gadget)
            return true;
    }
    return false;
}

/// The mod key of a made action, or null.
///
/// @param mod the mod data
/// @param id the action id
/// @return the key
const keys::GuiKey* gui_key(const keys::ModData& mod, std::string_view id) {
    for (const keys::GuiKey& key : mod.keys) {
        if (key.action == id)
            return &key;
    }
    return nullptr;
}

/// Parses a chord text. An empty chord when it does not parse.
///
/// @param text the text
/// @return the chord
keys::Chord parsed_chord(std::string_view text) {
    keys::ChordError error;
    const std::optional<keys::Chord> chord = keys::parse_chord(text, error);
    if (chord)
        return *chord;
    return {};
}

/// Escape, the one fixed chord.
///
/// @return the chord
keys::Chord escape_chord() {
    return parsed_chord("Escape");
}

/// The chords of a binding as one comparable line.
///
/// @param binding the binding
/// @return the line
std::string binding_line(const keys::LayoutBinding& binding) {
    std::string line = binding.action;
    for (const keys::Chord& chord : binding.chords) {
        line.push_back('\n');
        line += keys::chord_text(chord);
    }
    return line;
}

/// How two helper lists differ. An unset list and a set list differ.
///
/// @param left one list
/// @param right the other
/// @return how many ids differ, or 1 when only the set-ness differs
std::size_t list_diff(
    const std::optional<std::vector<std::string>>& left,
    const std::optional<std::vector<std::string>>& right
) {
    if (!left && !right)
        return 0;
    if (!left || !right)
        return 1;
    std::size_t count = 0;
    for (const std::string& id : *left) {
        if (std::find(right->begin(), right->end(), id) == right->end())
            ++count;
    }
    for (const std::string& id : *right) {
        if (std::find(left->begin(), left->end(), id) == left->end())
            ++count;
    }
    return count;
}

/// A slug a layout id can use.
///
/// @param name the name the player typed
/// @return the slug; "layout" when the name has no letters or digits
std::string slug_of(std::string_view name) {
    std::string id;
    bool dash = false;
    for (const unsigned char byte : name) {
        unsigned char lower = byte;
        if (lower >= 'A' && lower <= 'Z')
            lower = static_cast<unsigned char>(lower - 'A' + 'a');
        const bool word = (lower >= 'a' && lower <= 'z') || (lower >= '0' && lower <= '9');
        if (word) {
            id.push_back(static_cast<char>(lower));
            dash = false;
        } else if (!id.empty() && !dash) {
            id.push_back('-');
            dash = true;
        }
    }
    while (!id.empty() && id.back() == '-')
        id.pop_back();
    if (id.empty())
        id = "layout";
    return id;
}

/// True when an id is in a list.
///
/// @param ids the list
/// @param id the id
/// @return true when it is there
bool listed_id(const std::vector<std::string>& ids, std::string_view id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

/// Drops an id from a list.
///
/// @param ids the list
/// @param id the id
/// @return true when it was there
bool erase_id(std::vector<std::string>& ids, std::string_view id) {
    const auto found = std::find(ids.begin(), ids.end(), id);
    if (found == ids.end())
        return false;
    ids.erase(found);
    return true;
}

/// The category id of one bound action. Empty when the editor does not list it.
///
/// @param bound the action
/// @param inputs the inputs
/// @return the category id
std::string category_id(const keys::Bound& bound, const EditorInputs& inputs) {
    const std::string& id = bound.id;
    if (id.starts_with("ui.screen-"))
        return "oa-screens";
    if (id.starts_with("select.category-") || id.starts_with("select.tag-"))
        return "mod.categories";
    if (id.starts_with("gui."))
        return "mod.menus";
    const std::string area =
        bound.entry != nullptr ? std::string(bound.entry->area) : std::string{};
    if (area == "developer")
        return inputs.developer_mode ? "developer" : std::string{};
    if (area == "megamap")
        return listed_id(inputs.hacks_on, "ui.megamap") ? "ui.megamap" : std::string{};
    if (area == "whiteboard")
        return listed_id(inputs.hacks_on, "ui.whiteboard") ? "ui.whiteboard" : std::string{};
    if (bound.entry != nullptr) {
        for (const std::string_view source : bound.entry->source) {
            if (listed_id(inputs.hacks_on, source) && !main_area(area))
                return std::string(source);
        }
    }
    if (main_area(area))
        return area;
    return {};
}

/// Where an action works.
///
/// @param hooks the lookup
/// @param bits the context bits
/// @param held true when the action is held
/// @param area_title the area's title, already looked up
/// @return the line
std::string
where_text(const TextHooks& hooks, uint32_t bits, bool held, const std::string& area_title) {
    std::string context;
    if ((bits & (1U << 4)) != 0)
        context = show(hooks, words::while_playing);
    else if ((bits & (1U << 5)) != 0)
        context = show(hooks, words::while_watching);
    else {
        std::vector<std::string> titles;
        const std::size_t count = acts::contexts().size();
        for (std::size_t index = 0; index < count; ++index) {
            if ((bits & (1U << index)) == 0)
                continue;
            const std::string_view word = context_word(index);
            if (!word.empty())
                titles.push_back(show(hooks, word));
        }
        context = joined(titles, ", ");
    }
    const std::string playing = show(hooks, words::while_playing);
    const std::string watching = show(hooks, words::while_watching);
    if (held && (context == playing || context == watching))
        return area_title + " · " + show(hooks, words::aside_held) + " " + context;
    if (context.empty())
        return area_title;
    if (area_title.empty())
        return context;
    return area_title + " · " + context;
}

/// The conditions an action names.
///
/// @param hooks the lookup
/// @param bits the condition bits
/// @return the line; empty when none are set
std::string works_text(const TextHooks& hooks, uint32_t bits) {
    std::vector<std::string> titles;
    const std::size_t count = acts::conditions().size();
    for (std::size_t index = 0; index < count; ++index) {
        if ((bits & (1U << index)) == 0)
            continue;
        const std::string_view word = condition_word(index);
        if (!word.empty())
            titles.push_back(show(hooks, word));
    }
    return joined(titles, ", ");
}

/// The base chords as the details show them.
///
/// @param hooks the lookup
/// @param keyboard the keyboard
/// @param platform the platform
/// @param texts the chord texts
/// @param source where they come from
/// @param gadget an order-panel gadget, or empty
/// @return the line
std::string base_text(
    const TextHooks& hooks,
    const keys::KeyboardMap& keyboard,
    keys::Platform platform,
    const std::vector<std::string>& texts,
    const std::string& source,
    std::string_view gadget
) {
    std::vector<std::string> shown;
    for (const std::string& text : texts) {
        keys::ChordError error;
        const std::optional<keys::Chord> chord = keys::parse_chord(text, error);
        if (chord)
            shown.push_back(keys::display_text(*chord, keyboard, platform));
        else
            shown.push_back(text);
    }
    const std::string chords = joined(shown, ", ");
    if (!gadget.empty())
        return fill(show(hooks, words::from_order_panel), {{"chords", chords}, {"gadget", gadget}});
    if (chords.empty())
        return source;
    return fill(show(hooks, words::from_source), {{"chords", chords}, {"source", source}});
}

/// What one chord is.
///
/// @param hooks the lookup
/// @param chord the chord
/// @param keyboard the keyboard
/// @param platform the platform
/// @param step the step that bound it
/// @return the sentence
std::string explain_chord(
    const TextHooks& hooks,
    const keys::Chord& chord,
    const keys::KeyboardMap& keyboard,
    keys::Platform platform,
    keys::Step step
) {
    std::string text;
    if (chord.kind == keys::TriggerKind::character) {
        text = fill(show(hooks, words::letter_key), {{"key", utf8_of(chord.character)}});
    } else if (chord.kind == keys::TriggerKind::place) {
        text = fill(
            show(hooks, words::place_key), {{"key", keys::display_text(chord, keyboard, platform)}}
        );
    } else if (chord.kind == keys::TriggerKind::mouse) {
        text = fill(show(hooks, words::mouse_button), {{"button", std::to_string(chord.button)}});
    } else {
        text = fill(
            show(hooks, words::named_key), {{"key", keys::display_text(chord, keyboard, platform)}}
        );
    }
    if (step == keys::Step::own) {
        text.push_back(' ');
        text += show(hooks, words::your_change);
    }
    return text;
}

/// True when the draft names an action.
///
/// @param own the draft
/// @param id the action id
/// @return true when a binding names it
bool draft_names(const keys::Layout& own, std::string_view id) {
    for (const keys::LayoutBinding& binding : own.bindings) {
        if (binding.action == id)
            return true;
    }
    return false;
}

/// True when a pair names an action.
///
/// @param clashes the pairs
/// @param id the action id
/// @return true when it is in a pair
bool in_clash(const std::vector<keys::Clash>& clashes, std::string_view id) {
    for (const keys::Clash& clash : clashes) {
        if (clash.a == id || clash.b == id)
            return true;
    }
    return false;
}

/// The mark a row shows. A clash wins, then no chord, then a draft, then fixed.
///
/// @param clashing the action is in a clash
/// @param empty the action has no chord
/// @param changed the draft names it
/// @param fixed the action's chord is fixed
/// @return the mark
kit::KeyMark row_mark(bool clashing, bool empty, bool changed, bool fixed) {
    if (clashing)
        return kit::KeyMark::clash;
    if (empty)
        return kit::KeyMark::unbound;
    if (changed)
        return kit::KeyMark::changed;
    if (fixed)
        return kit::KeyMark::fixed;
    return kit::KeyMark::none;
}

/// True when Latch is on for an action.
///
/// @param helpers the helpers in use
/// @param id the action id
/// @return true when the list names it
bool latch_is_on(const oa::formats::oakeys::Helpers& helpers, std::string_view id) {
    return helpers.toggle && listed_id(*helpers.toggle, id);
}

/// True when Hold to confirm is on for an action. Unset means every confirmable action.
///
/// @param helpers the helpers in use
/// @param id the action id
/// @return true when it requires a hold
bool confirm_is_on(const oa::formats::oakeys::Helpers& helpers, std::string_view id) {
    if (!helpers.hold_to_confirm)
        return true;
    return listed_id(*helpers.hold_to_confirm, id);
}

/// The words of one category id.
///
/// @param hooks the lookup
/// @param id the category id
/// @param inputs the inputs, for a hack's id and the mod's name
/// @param title the title
/// @param tint the tint
/// @param letter the letter
void category_look(
    const TextHooks& hooks,
    std::string_view id,
    const EditorInputs& inputs,
    std::string& title,
    kit::KeyTint& tint,
    std::string& letter
) {
    if (id == "all") {
        title = show(hooks, words::all_actions);
        tint = kit::KeyTint::none;
        letter.clear();
        return;
    }
    if (id == "common") {
        title = show(hooks, words::common_commands);
        tint = kit::KeyTint::none;
        letter.clear();
        return;
    }
    if (id == "oa-screens") {
        title = show(hooks, words::oa_screens);
        tint = kit::KeyTint::none;
        letter.clear();
        return;
    }
    if (id == "unknown") {
        title = show(hooks, words::not_in_version);
        tint = kit::KeyTint::none;
        letter.clear();
        return;
    }
    if (id == "mod.categories") {
        title = show(hooks, words::tags);
        tint = kit::KeyTint::mod;
        letter = show(hooks, words::letter_mod);
        return;
    }
    if (id == "mod.menus") {
        title = show(hooks, words::menus);
        tint = kit::KeyTint::mod;
        letter = show(hooks, words::letter_mod);
        return;
    }
    if (id == "developer") {
        title = show(hooks, words::developer);
        tint = kit::KeyTint::developer;
        letter = show(hooks, words::letter_developer);
        return;
    }
    if (id == "ui.megamap" || id == "ui.whiteboard") {
        const AreaRow* area = area_named(id == "ui.megamap" ? "megamap" : "whiteboard");
        if (area != nullptr) {
            title = show(hooks, area->title);
            tint = area->tint;
            letter = area->letter.empty() ? std::string{} : show(hooks, area->letter);
            return;
        }
    }
    if (const AreaRow* area = area_named(id)) {
        title = show(hooks, area->title);
        tint = area->tint;
        letter = area->letter.empty() ? std::string{} : show(hooks, area->letter);
        return;
    }
    if (listed_id(inputs.hacks_on, id)) {
        title = std::string(id);
        tint = kit::KeyTint::hacks;
        letter = show(hooks, words::letter_hacks);
        return;
    }
    title = std::string(id);
    tint = kit::KeyTint::none;
    letter.clear();
    static_cast<void>(inputs.mod_name);
}

/// Builds one listed action from a bound row.
///
/// @param bound the action
/// @param inputs the inputs
/// @param own the draft
/// @param effective the helpers in use
/// @param clashes the clashes
/// @param keyboard the keyboard
/// @return the row; category empty when it is not listed
Listed list_bound(
    const keys::Bound& bound,
    const EditorInputs& inputs,
    const keys::Layout& own,
    const oa::formats::oakeys::Helpers& effective,
    const std::vector<keys::Clash>& clashes,
    const keys::KeyboardMap& keyboard
) {
    Listed listed;
    listed.id = bound.id;
    listed.category = category_id(bound, inputs);
    if (listed.category.empty())
        return listed;
    const TextHooks& hooks = inputs.text;
    category_look(
        hooks, listed.category, inputs, listed.category_title, listed.tint, listed.letter
    );
    listed.entry = bound.entry;
    const keys::GuiKey* made = gui_key(inputs.mod, bound.id);
    if (bound.entry != nullptr) {
        listed.name = show(hooks, bound.entry->words.name);
        listed.description = show(hooks, bound.entry->words.description);
        listed.latchable = bound.entry->latchable;
        listed.confirmable = bound.entry->confirmable;
        listed.gadget = std::string(bound.entry->gadget);
    } else if (made != nullptr) {
        listed.name = made->gadget.empty() ? bound.id : made->gadget;
        listed.description = made->help;
        listed.gadget = made->gadget;
    } else if (
        bound.id.starts_with("select.category-") &&
        bound.id.size() > std::string("select.category-").size()
    ) {
        listed.name = utf8_of(static_cast<char32_t>(bound.id.back() - 'a' + 'A'));
    } else if (bound.id.starts_with("select.tag-")) {
        const std::string word = tag_display_word(inputs, bound.id);
        const acts::Family* family = tag_family();
        if (family != nullptr && !word.empty()) {
            listed.name = tag_sentence(hooks, family->words.name, word);
            listed.description = tag_sentence(hooks, family->words.description, word);
        } else {
            listed.name = bound.id;
        }
    } else {
        listed.name = bound.id;
    }
    listed.changed = draft_names(own, bound.id);
    listed.latch_on = listed.latchable && latch_is_on(effective, bound.id);
    listed.confirm_on = listed.confirmable && confirm_is_on(effective, bound.id);
    const acts::Kind kind = kind_of(bound);
    listed.kind = kind_text(hooks, kind);
    for (const keys::BoundChord& held : bound.chords) {
        listed.chords.push_back(held.chord);
        listed.steps.push_back(held.step);
    }
    listed.mark = row_mark(
        in_clash(clashes, bound.id),
        listed.chords.empty(),
        listed.changed,
        bound.entry != nullptr && bound.entry->fixed
    );
    std::vector<std::string> aside;
    if (kind == acts::Kind::hold)
        aside.push_back(show(hooks, words::aside_held));
    if (listed.latch_on)
        aside.push_back(show(hooks, words::aside_latches));
    if (listed.confirm_on)
        aside.push_back(show(hooks, words::aside_confirm));
    for (const std::string& word : inputs.mod.tags) {
        const std::string action = keys::tag_action_id(word);
        if (action.empty() || action.starts_with("select.tag-"))
            continue;
        const bool held_here = action == bound.id;
        const bool replaced = bound.entry != nullptr && bound.entry->replaces == action;
        if (held_here || replaced)
            aside.push_back(word);
    }
    listed.aside = joined(aside, " · ");
    const bool held = kind == acts::Kind::hold;
    listed.where = where_text(hooks, bound.contexts, held, listed.category_title);
    listed.works_with = works_text(hooks, bound.when);
    std::vector<std::string> defaults;
    std::string source;
    std::string panel;
    if (bound.entry != nullptr) {
        for (const std::string_view text : bound.entry->defaults)
            defaults.emplace_back(text);
        std::vector<std::string> sources;
        for (const std::string_view token : bound.entry->source)
            sources.push_back(source_text(hooks, token));
        source = joined(sources, ", ");
    } else if (bound.id.starts_with("select.category-")) {
        defaults.push_back(std::string("Ctrl+") + listed.name);
        source = source_text(hooks, "3.1c");
    } else if (bound.id.starts_with("select.tag-")) {
        source = source_text(hooks, "data");
    } else if (made != nullptr) {
        source = source_text(hooks, "data");
        if (order_panel(made->gadget))
            panel = made->gadget;
    }
    listed.base = base_text(hooks, keyboard, inputs.platform, defaults, source, panel);
    return listed;
}

/// Builds one read-only row from a name this version does not know.
///
/// @param binding the binding
/// @param inputs the inputs
/// @param own the draft
/// @param clashes the clashes
/// @return the row
Listed list_unknown(
    const keys::LayoutBinding& binding,
    const EditorInputs& inputs,
    const keys::Layout& own,
    const std::vector<keys::Clash>& clashes
) {
    Listed listed;
    listed.id = binding.action;
    listed.read_only = true;
    listed.changed = draft_names(own, binding.action);
    constexpr std::string_view prefix = "select.tag-";
    const bool missing_tag =
        binding.action.starts_with(prefix) && binding.action.size() > prefix.size();
    listed.category = missing_tag ? "mod.categories" : "unknown";
    category_look(
        inputs.text, listed.category, inputs, listed.category_title, listed.tint, listed.letter
    );
    if (missing_tag) {
        const std::string word(binding.action.substr(prefix.size()));
        const acts::Family* family = tag_family();
        if (family != nullptr) {
            listed.name = tag_sentence(inputs.text, family->words.name, word);
            listed.description = tag_sentence(inputs.text, family->words.description, word);
        } else {
            listed.name = binding.action;
        }
        listed.aside = show(inputs.text, words::not_in_loaded_units);
    } else {
        listed.name = binding.action;
    }
    for (const keys::Chord& chord : binding.chords) {
        listed.chords.push_back(chord);
        listed.steps.push_back(keys::Step::own);
    }
    listed.mark =
        row_mark(in_clash(clashes, binding.action), listed.chords.empty(), listed.changed, false);
    listed.where = listed.category_title;
    return listed;
}

/// Chords the draft took, so the other action lost them at resolution.
///
/// Only a later step and an earlier line in the same file are a take. A fixed
/// key, a fifth chord and the number-key swap are not: those are not a pair
/// the card gives back.
///
/// @param resolved the bindings in use, and the chords that lost
/// @param own the draft
/// @return one entry per chord the draft took
std::vector<DraftTake> takes_of(const keys::Resolved& resolved, const keys::Layout& own) {
    std::vector<DraftTake> takes;
    for (const keys::Lost& lost : resolved.lost) {
        if (lost.reason != keys::LostReason::later_step &&
            lost.reason != keys::LostReason::earlier_in_file)
            continue;
        if (lost.winner.empty() || lost.action.empty())
            continue;
        if (!draft_names(own, lost.winner) && !draft_names(own, lost.action))
            continue;
        DraftTake take;
        take.winner = lost.winner;
        take.loser = lost.action;
        take.chord = lost.chord;
        takes.push_back(std::move(take));
    }
    return takes;
}

/// Every row the editor lists, in category order.
///
/// @param inputs the inputs
/// @param own the draft
/// @param has_chosen a layout other than OA default is in use
/// @param chosen that layout
/// @param helpers the draft's helpers
/// @param number_keys SwitchAlt
/// @return the rows
std::vector<Listed> build_list(
    const EditorInputs& inputs,
    const keys::Layout& own,
    bool has_chosen,
    const LayoutSummary& chosen,
    const oa::formats::oakeys::Helpers& helpers,
    bool number_keys
) {
    keys::ResolveInput input;
    input.base = inputs.base;
    input.chosen = has_chosen ? &chosen.layout : nullptr;
    input.mod = &inputs.mod;
    input.own = &own;
    input.hacks_on = inputs.hacks_on;
    input.scheme = inputs.scheme;
    input.number_keys_select_groups = number_keys;
    input.platform = inputs.platform;
    input.keyboard = &inputs.keyboard;
    const keys::Resolved resolved = keys::resolve(input);
    std::vector<keys::Clash> clashes = keys::find_clashes(resolved, input);
    // A chord the draft took is still listed, on both actions, even though
    // resolution already dropped the loser's copy.
    for (const DraftTake& take : takes_of(resolved, own)) {
        keys::Clash mark;
        mark.a = take.winner;
        mark.b = take.loser;
        mark.chord = take.chord;
        clashes.push_back(std::move(mark));
    }
    oa::formats::oakeys::Helpers effective =
        has_chosen ? chosen.helpers : oa::formats::oakeys::Helpers{};
    if (helpers.toggle)
        effective.toggle = helpers.toggle;
    if (helpers.hold_to_confirm)
        effective.hold_to_confirm = helpers.hold_to_confirm;

    std::vector<Listed> rows;
    for (const keys::Bound& bound : resolved.actions) {
        Listed listed = list_bound(bound, inputs, own, effective, clashes, inputs.keyboard);
        if (!listed.category.empty())
            rows.push_back(std::move(listed));
    }
    for (const keys::LayoutBinding& binding : resolved.unknown)
        rows.push_back(list_unknown(binding, inputs, own, clashes));
    return rows;
}

/// The words a search matches, folded.
///
/// @param listed the row
/// @return the haystack
std::string haystack_of(const Listed& listed) {
    return folded(
        listed.name + " " + listed.description + " " + listed.aside + " " + listed.id + " " +
        listed.gadget + " " + listed.category_title
    );
}

/// True when every word of the query occurs in the haystack.
///
/// @param listed the row
/// @param query the search
/// @return true when it matches
bool matches(const Listed& listed, std::string_view query) {
    if (query.empty())
        return true;
    const std::string haystack = haystack_of(listed);
    std::string word;
    const std::string folded_query = folded(query);
    const auto take = [&]() { return word.empty() || haystack.find(word) != std::string::npos; };
    for (const char byte : folded_query) {
        if (byte == ' ') {
            if (!take())
                return false;
            word.clear();
        } else {
            word.push_back(byte);
        }
    }
    return take();
}

/// True when a filter keeps a row.
///
/// @param listed the row
/// @param filter the filter
/// @return true when it stays
bool kept(const Listed& listed, Filter filter) {
    switch (filter) {
    case Filter::changed:
        return listed.changed;
    case Filter::clashes:
        return listed.mark == kit::KeyMark::clash;
    case Filter::unbound:
        return listed.chords.empty();
    case Filter::latchable:
        return listed.latchable;
    case Filter::all:
        break;
    }
    return true;
}

/// Copies a listed row into the screen's row.
///
/// @param listed the row
/// @return the row
Row row_of(const Listed& listed) {
    Row row;
    row.id = listed.id;
    row.name = listed.name;
    row.aside = listed.aside;
    row.where = listed.where;
    row.mark = listed.mark;
    row.chords = listed.chords;
    row.read_only = listed.read_only;
    row.category = listed.category;
    return row;
}

/// Finds a listed row.
///
/// @param rows the rows
/// @param id the action id
/// @return the row, or null
const Listed* find_listed(const std::vector<Listed>& rows, std::string_view id) {
    for (const Listed& listed : rows) {
        if (listed.id == id)
            return &listed;
    }
    return nullptr;
}

/// True when an id is an action this version does not know.
///
/// @param rows the rows
/// @param id the action id
/// @return true when the row is read only
bool unknown_row(const std::vector<Listed>& rows, std::string_view id) {
    const Listed* listed = find_listed(rows, id);
    return listed != nullptr && listed->read_only;
}

/// Replaces or appends one draft binding.
///
/// @param own the draft
/// @param id the action id
/// @param chords the chords that remain
void put_binding(keys::Layout& own, std::string id, std::vector<keys::Chord> chords) {
    for (keys::LayoutBinding& binding : own.bindings) {
        if (binding.action == id) {
            binding.chords = std::move(chords);
            return;
        }
    }
    keys::LayoutBinding binding;
    binding.action = std::move(id);
    binding.chords = std::move(chords);
    own.bindings.push_back(std::move(binding));
}

/// Drops one draft binding.
///
/// @param own the draft
/// @param id the action id
/// @return true when it was there
bool erase_binding(keys::Layout& own, std::string_view id) {
    for (auto binding = own.bindings.begin(); binding != own.bindings.end(); ++binding) {
        if (binding->action == id) {
            own.bindings.erase(binding);
            return true;
        }
    }
    return false;
}

/// The own manifest's other keys.
///
/// @param layout_id the layout the changes sit on
/// @param helpers the draft's helpers
/// @return the manifest, with no bindings yet
oa::formats::oakeys::Manifest
own_rest(std::string_view layout_id, const oa::formats::oakeys::Helpers& helpers) {
    oa::formats::oakeys::Manifest manifest;
    manifest.format = oa::formats::oakeys::format_version;
    manifest.id = std::string("own-") + std::string(layout_id);
    manifest.name = "Your changes";
    manifest.version.major = 1;
    manifest.version.minor = 0;
    manifest.version.parts = 2;
    manifest.author.name = "You";
    manifest.tags = {"own"};
    manifest.base = oa::formats::oakeys::Base::oa_default;
    manifest.helpers = helpers;
    return manifest;
}

/// An installed layout as a manifest.
///
/// @param summary the layout
/// @return the manifest
oa::formats::oakeys::Manifest manifest_of(const LayoutSummary& summary) {
    oa::formats::oakeys::Manifest rest;
    rest.format = oa::formats::oakeys::format_version;
    rest.id = summary.id;
    rest.name = summary.name;
    rest.version.major = 1;
    rest.version.minor = 0;
    rest.version.parts = 2;
    rest.author.name = summary.author.empty() ? "You" : summary.author;
    rest.tags = {"own"};
    rest.base = oa::formats::oakeys::Base::oa_default;
    rest.helpers = summary.helpers;
    keys::Layout layout = summary.layout;
    layout.id = summary.id;
    layout.base = keys::Base::oa_default;
    return keys::manifest_from_layout(layout, rest);
}

/// How two drafts' bindings differ.
///
/// @param left one draft
/// @param right the other
/// @return how many bindings differ
std::size_t binding_diff(const keys::Layout& left, const keys::Layout& right) {
    std::size_t count = 0;
    for (const keys::LayoutBinding& binding : left.bindings) {
        bool same = false;
        for (const keys::LayoutBinding& other : right.bindings) {
            if (binding_line(binding) == binding_line(other))
                same = true;
        }
        if (!same)
            ++count;
    }
    for (const keys::LayoutBinding& binding : right.bindings) {
        bool found = false;
        for (const keys::LayoutBinding& other : left.bindings) {
            if (other.action == binding.action)
                found = true;
        }
        if (!found)
            ++count;
    }
    return count;
}

} // namespace

namespace keys = oa::ui::key_bindings;
namespace acts = oa::data::input_actions;

Editor::Editor(EditorInputs inputs) : inputs_(std::move(inputs)) {
    own_ = inputs_.own;
    helpers_ = inputs_.helpers;
    number_keys_ = inputs_.number_keys_select_groups;
    has_chosen_ = inputs_.has_chosen;
    chosen_ = inputs_.chosen;
    installed_ = inputs_.installed;
    opened_.layout_id = layout_id();
    opened_.number_keys = number_keys_;
    opened_.scheme = inputs_.scheme;
    opened_.own = own_;
    opened_.helpers = helpers_;
    for (const LayoutSummary& layout : installed_)
        opened_.names.push_back(layout.id + "\t" + layout.name);
}

keys::ResolveInput Editor::resolve_input() const {
    keys::ResolveInput input;
    input.base = inputs_.base;
    input.chosen = has_chosen_ ? &chosen_.layout : nullptr;
    input.mod = &inputs_.mod;
    input.own = &own_;
    input.hacks_on = inputs_.hacks_on;
    input.scheme = inputs_.scheme;
    input.number_keys_select_groups = number_keys_;
    input.platform = inputs_.platform;
    input.keyboard = &inputs_.keyboard;
    return input;
}

keys::Resolved Editor::resolved() const {
    const keys::ResolveInput input = resolve_input();
    return keys::resolve(input);
}

oa::formats::oakeys::Helpers Editor::effective_helpers() const {
    oa::formats::oakeys::Helpers helpers =
        has_chosen_ ? chosen_.helpers : oa::formats::oakeys::Helpers{};
    if (helpers_.toggle)
        helpers.toggle = helpers_.toggle;
    if (helpers_.hold_to_confirm)
        helpers.hold_to_confirm = helpers_.hold_to_confirm;
    if (helpers_.sticky_modifiers)
        helpers.sticky_modifiers = helpers_.sticky_modifiers;
    if (helpers_.pointer_keys)
        helpers.pointer_keys = helpers_.pointer_keys;
    if (helpers_.radial_orders)
        helpers.radial_orders = helpers_.radial_orders;
    if (helpers_.edge_scroll)
        helpers.edge_scroll = helpers_.edge_scroll;
    return helpers;
}

namespace {

/// The rows the editor lists from its draft.
///
/// @param inputs the inputs
/// @param own the draft
/// @param has_chosen a layout other than OA default is in use
/// @param chosen that layout
/// @param helpers the draft's helpers
/// @param number_keys SwitchAlt
/// @return the rows
std::vector<Listed> listed_of(
    const EditorInputs& inputs,
    const keys::Layout& own,
    bool has_chosen,
    const LayoutSummary& chosen,
    const oa::formats::oakeys::Helpers& helpers,
    bool number_keys
) {
    return build_list(inputs, own, has_chosen, chosen, helpers, number_keys);
}

/// Keeps the bindings this version does not know, and clears the helpers.
///
/// @param rows the rows as they are listed
/// @param own the draft
/// @param helpers the draft's helpers
void keep_unknowns(
    const std::vector<Listed>& rows, keys::Layout& own, oa::formats::oakeys::Helpers& helpers
) {
    std::vector<keys::LayoutBinding> kept;
    for (const keys::LayoutBinding& binding : own.bindings) {
        if (unknown_row(rows, binding.action))
            kept.push_back(binding);
    }
    own.bindings = std::move(kept);
    helpers = {};
}

/// True when a chord is Escape's fixed chord on a fixed action.
///
/// @param listed the row
/// @param chord the chord
/// @param keyboard the keyboard
/// @return true when it cannot be removed
bool fixed_escape(
    const Listed& listed, const keys::Chord& chord, const keys::KeyboardMap& keyboard
) {
    return listed.entry != nullptr && listed.entry->fixed &&
           keys::same_key(chord, escape_chord(), &keyboard);
}

/// A slug that is not already a layout id.
///
/// @param name the name the player typed
/// @param installed the layouts already installed
/// @return the slug
std::string unique_slug(std::string_view name, const std::vector<LayoutSummary>& installed) {
    const std::string base = slug_of(name);
    auto taken = [&](const std::string& id) {
        if (id == "oa-default")
            return true;
        for (const LayoutSummary& layout : installed) {
            if (layout.id == id)
                return true;
        }
        return false;
    };
    if (!taken(base))
        return base;
    for (int suffix = 2; suffix < 10000; ++suffix) {
        const std::string id = base + "-" + std::to_string(suffix);
        if (!taken(id))
            return id;
    }
    return base + "-x";
}

/// The id at the start of an "id\\tname" line.
///
/// @param line the line
/// @return the id
std::string line_id(const std::string& line) {
    const std::size_t tab = line.find('\t');
    if (tab == std::string::npos)
        return line;
    return line.substr(0, tab);
}

/// The name after the tab of an "id\\tname" line.
///
/// @param line the line
/// @return the name
std::string line_name(const std::string& line) {
    const std::size_t tab = line.find('\t');
    if (tab == std::string::npos)
        return {};
    return line.substr(tab + 1);
}

/// Folds one ASCII letter. Any other byte is kept.
///
/// @param byte the byte
/// @return the folded byte
unsigned char folded_byte(unsigned char byte) {
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<unsigned char>(byte - 'A' + 'a');
    return byte;
}

/// True when the left word sorts first, ASCII case ignored.
///
/// @param left one Category word
/// @param right the other
/// @return true when the registry would list `left` first
bool word_before(std::string_view left, std::string_view right) {
    const std::size_t count = left.size() < right.size() ? left.size() : right.size();
    for (std::size_t index = 0; index < count; ++index) {
        const unsigned char a = folded_byte(static_cast<unsigned char>(left[index]));
        const unsigned char b = folded_byte(static_cast<unsigned char>(right[index]));
        if (a != b)
            return a < b;
    }
    return left.size() < right.size();
}

/// The listed action a tag word shows, or the action that replaces it.
///
/// @param rows the listed actions
/// @param action the action the word maps to
/// @return the row, or null
const Listed* action_row(const std::vector<Listed>& rows, std::string_view action) {
    if (const Listed* direct = find_listed(rows, action))
        return direct;
    for (const Listed& row : rows) {
        if (row.entry != nullptr && row.entry->replaces == action)
            return &row;
    }
    return nullptr;
}

/// The Tags group: one row per gathered word, then the group's other rows.
///
/// A word another action holds lists that action. All still lists it once,
/// in the action's own category. A tag the loaded units lack stays here.
///
/// @param listed the listed actions
/// @param inputs the inputs
/// @param filter the filter
/// @param query the search
/// @return the rows
std::vector<Row> tag_group_rows(
    const std::vector<Listed>& listed,
    const EditorInputs& inputs,
    Filter filter,
    std::string_view query
) {
    std::vector<std::size_t> order(inputs.mod.tags.size());
    for (std::size_t index = 0; index < order.size(); ++index)
        order[index] = index;
    std::stable_sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        return word_before(inputs.mod.tags[left], inputs.mod.tags[right]);
    });

    std::vector<std::string> shown;
    std::vector<Row> out;
    auto take = [&](const Listed& row) {
        if (listed_id(shown, row.id))
            return;
        shown.push_back(row.id);
        if (!kept(row, filter) || !matches(row, query))
            return;
        Row copy = row_of(row);
        copy.category = "mod.categories";
        out.push_back(std::move(copy));
    };
    for (const std::size_t index : order) {
        const std::string action = keys::tag_action_id(inputs.mod.tags[index]);
        if (action.empty())
            continue;
        const Listed* row = action_row(listed, action);
        if (row != nullptr)
            take(*row);
    }
    for (const Listed& row : listed) {
        if (row.category == "mod.categories")
            take(row);
    }
    return out;
}

} // namespace

std::vector<CategoryEntry> Editor::categories() const {
    const std::vector<Listed> listed =
        listed_of(inputs_, own_, has_chosen_, chosen_, helpers_, number_keys_);
    auto count_of = [&](std::string_view id) {
        std::size_t count = 0;
        for (const Listed& row : listed) {
            if (row.category == id)
                ++count;
        }
        return count;
    };
    std::vector<CategoryEntry> out;
    auto add = [&](std::string id, std::size_t count, bool read_only) {
        CategoryEntry entry;
        entry.id = std::move(id);
        entry.count = count;
        entry.read_only = read_only;
        category_look(inputs_.text, entry.id, inputs_, entry.title, entry.tint, entry.letter);
        out.push_back(std::move(entry));
    };
    std::size_t common = 0;
    for (const Listed& row : listed) {
        if (common_command(row.id))
            ++common;
    }
    add("common", common, false);
    for (const AreaRow& area : kAreas) {
        if (area.main)
            add(std::string(area.id), count_of(area.id), false);
    }
    std::vector<std::string> hacks;
    for (const std::string_view hack : acts::hack_ids()) {
        if (listed_id(inputs_.hacks_on, hack))
            hacks.emplace_back(hack);
    }
    for (const std::string& hack : inputs_.hacks_on) {
        if (!listed_id(hacks, hack))
            hacks.push_back(hack);
    }
    if (!hacks.empty()) {
        CategoryEntry heading;
        heading.id = "heading.hacks";
        heading.title = show(inputs_.text, words::hacks_on);
        heading.read_only = true;
        out.push_back(std::move(heading));
        for (const std::string& hack : hacks)
            add(hack, count_of(hack), false);
    }
    const std::size_t tag_count = tag_group_rows(listed, inputs_, Filter::all, "").size();
    const bool mod_rows = tag_count > 0 || count_of("mod.menus") > 0;
    const bool mod_data =
        !inputs_.mod.categories.empty() || !inputs_.mod.keys.empty() || !inputs_.mod.tags.empty();
    if (!inputs_.mod_name.empty() || mod_rows || mod_data) {
        CategoryEntry heading;
        heading.id = "heading.mod";
        heading.title =
            inputs_.mod_name.empty() ? show(inputs_.text, words::tags) : inputs_.mod_name;
        heading.read_only = true;
        out.push_back(std::move(heading));
    }
    if (tag_count > 0 || !inputs_.mod.categories.empty() || !inputs_.mod.tags.empty())
        add("mod.categories", tag_count, false);
    if (count_of("mod.menus") > 0 || !inputs_.mod.keys.empty())
        add("mod.menus", count_of("mod.menus"), false);
    if (inputs_.developer_mode)
        add("developer", count_of("developer"), false);
    add("oa-screens", count_of("oa-screens"), false);
    if (count_of("unknown") > 0)
        add("unknown", count_of("unknown"), true);
    add("all", listed.size(), false);
    return out;
}

std::vector<Row>
Editor::rows(std::string_view category, Filter filter, std::string_view query) const {
    if (category.starts_with("heading."))
        return {};
    const std::vector<Listed> listed =
        listed_of(inputs_, own_, has_chosen_, chosen_, helpers_, number_keys_);
    if (category == "mod.categories")
        return tag_group_rows(listed, inputs_, filter, query);
    if (category == "common") {
        // The common commands in their own order. A search reaches every
        // action: the common commands that match come first, then the rest
        // in the nav's order.
        std::vector<Row> out;
        for (const std::string_view id : kCommon) {
            const Listed* row = find_listed(listed, id);
            if (row == nullptr || !kept(*row, filter) || !matches(*row, query))
                continue;
            Row copy = row_of(*row);
            copy.area = row->category_title;
            out.push_back(std::move(copy));
        }
        if (query.empty())
            return out;
        for (Row& row : rows("all", filter, query)) {
            if (common_command(row.id))
                continue;
            if (const Listed* one = find_listed(listed, row.id))
                row.area = one->category_title;
            out.push_back(std::move(row));
        }
        return out;
    }
    const bool every = category.empty() || category == "all";
    std::vector<Row> out;
    for (const Listed& row : listed) {
        if (!every && row.category != category)
            continue;
        if (!kept(row, filter) || !matches(row, query))
            continue;
        out.push_back(row_of(row));
    }
    if (!every)
        return out;
    const std::vector<CategoryEntry> order = categories();
    auto index_of = [&](const std::string& id) {
        std::size_t index = 0;
        for (const CategoryEntry& entry : order) {
            if (entry.id.starts_with("heading."))
                continue;
            if (entry.id == id)
                return index;
            ++index;
        }
        return order.size();
    };
    std::stable_sort(out.begin(), out.end(), [&](const Row& left, const Row& right) {
        return index_of(left.category) < index_of(right.category);
    });
    return out;
}

Details Editor::details(std::string_view id) const {
    Details shown;
    const std::vector<Listed> listed =
        listed_of(inputs_, own_, has_chosen_, chosen_, helpers_, number_keys_);
    const Listed* row = find_listed(listed, id);
    // A hack's actions stay out of the list until the hack is on. An import
    // can still have written their chords, and those chords are what SAVE writes.
    if (row == nullptr) {
        for (const keys::LayoutBinding& binding : own_.bindings) {
            if (binding.action != id)
                continue;
            shown.id = std::string(id);
            if (const acts::Action* action = acts::find(id))
                shown.name = std::string(action->words.name);
            for (const keys::Chord& chord : binding.chords) {
                ChordDetail detail;
                detail.chord = chord;
                detail.step = keys::Step::own;
                shown.chords.push_back(std::move(detail));
            }
            return shown;
        }
        return shown;
    }
    shown.id = row->id;
    shown.name = row->name;
    shown.where = row->where;
    shown.kind = row->kind;
    shown.base = row->base;
    shown.description = row->description;
    shown.works_with = row->works_with;
    shown.latch_offered = row->latchable;
    shown.latch_on = row->latch_on;
    shown.confirm_offered = row->confirmable;
    shown.confirm_on = row->confirm_on;
    shown.confirm_setting_off = row->confirmable && !inputs_.hold_to_confirm_setting;
    shown.reset = fill(show(inputs_.text, words::reset_to), {{"layout", layout_name()}});
    shown.read_only = row->read_only;
    shown.tint = row->tint;
    shown.letter = row->letter;
    shown.technical = row->id;
    if (!row->gadget.empty() && row->gadget != row->name) {
        shown.technical += " · ";
        shown.technical += row->gadget;
    }
    shown.chords.reserve(row->chords.size());
    for (std::size_t index = 0; index < row->chords.size(); ++index) {
        ChordDetail chord;
        chord.chord = row->chords[index];
        chord.step = index < row->steps.size() ? row->steps[index] : keys::Step::base;
        chord.fixed = fixed_escape(*row, chord.chord, inputs_.keyboard);
        chord.removable = !row->read_only && !chord.fixed;
        chord.explanation = explain_chord(
            inputs_.text, chord.chord, inputs_.keyboard, inputs_.platform, chord.step
        );
        shown.chords.push_back(std::move(chord));
    }
    return shown;
}

FilterCounts Editor::counts() const {
    FilterCounts counts;
    const std::vector<Listed> listed =
        listed_of(inputs_, own_, has_chosen_, chosen_, helpers_, number_keys_);
    for (const Listed& row : listed) {
        ++counts.all;
        if (row.changed)
            ++counts.changed;
        if (row.mark == kit::KeyMark::clash)
            ++counts.clashes;
        if (row.chords.empty())
            ++counts.unbound;
        if (row.latchable)
            ++counts.latchable;
    }
    return counts;
}

bool Editor::remove_chord(std::string_view id, std::size_t index) {
    const EditStep step(*this);
    const std::vector<Listed> listed =
        listed_of(inputs_, own_, has_chosen_, chosen_, helpers_, number_keys_);
    const Listed* row = find_listed(listed, id);
    if (row == nullptr || row->read_only || index >= row->chords.size())
        return false;
    if (fixed_escape(*row, row->chords[index], inputs_.keyboard))
        return false;
    std::vector<keys::Chord> remain;
    for (std::size_t chord = 0; chord < row->chords.size(); ++chord) {
        if (chord != index)
            remain.push_back(row->chords[chord]);
    }
    put_binding(own_, std::string(id), std::move(remain));
    return true;
}

bool Editor::reset_action(std::string_view id) {
    const EditStep step(*this);
    const std::vector<Listed> listed =
        listed_of(inputs_, own_, has_chosen_, chosen_, helpers_, number_keys_);
    const Listed* row = find_listed(listed, id);
    if (row == nullptr || row->read_only)
        return false;
    bool changed = erase_binding(own_, id);
    if (helpers_.toggle && erase_id(*helpers_.toggle, id))
        changed = true;
    if (helpers_.hold_to_confirm && erase_id(*helpers_.hold_to_confirm, id))
        changed = true;
    return changed;
}

void Editor::restore_defaults() {
    const EditStep step(*this);
    const std::vector<Listed> listed =
        listed_of(inputs_, own_, has_chosen_, chosen_, helpers_, number_keys_);
    keep_unknowns(listed, own_, helpers_);
    number_keys_ = inputs_.number_keys_select_groups;
    held_clashes_.clear();
    imported_from_.reset();
}

bool Editor::set_latch(std::string_view id, bool on) {
    const EditStep step(*this);
    const std::vector<Listed> listed =
        listed_of(inputs_, own_, has_chosen_, chosen_, helpers_, number_keys_);
    const Listed* row = find_listed(listed, id);
    if (row == nullptr || row->read_only || !row->latchable)
        return false;
    if (latch_is_on(effective_helpers(), id) == on)
        return false;
    std::vector<std::string> list;
    if (helpers_.toggle)
        list = *helpers_.toggle;
    else if (has_chosen_ && chosen_.helpers.toggle)
        list = *chosen_.helpers.toggle;
    if (on) {
        if (!listed_id(list, id))
            list.emplace_back(id);
    } else {
        erase_id(list, id);
    }
    helpers_.toggle = std::move(list);
    return true;
}

bool Editor::set_confirm(std::string_view id, bool on) {
    const EditStep step(*this);
    const std::vector<Listed> listed =
        listed_of(inputs_, own_, has_chosen_, chosen_, helpers_, number_keys_);
    const Listed* row = find_listed(listed, id);
    if (row == nullptr || row->read_only || !row->confirmable)
        return false;
    if (confirm_is_on(effective_helpers(), id) == on)
        return false;
    std::vector<std::string> list;
    const oa::formats::oakeys::Helpers effective = effective_helpers();
    if (!effective.hold_to_confirm) {
        for (const acts::Action& action : acts::actions()) {
            if (action.confirmable && action.id != id)
                list.emplace_back(action.id);
        }
        helpers_.hold_to_confirm = std::move(list);
        return true;
    }
    list = *effective.hold_to_confirm;
    if (on) {
        if (!listed_id(list, id))
            list.emplace_back(id);
    } else {
        erase_id(list, id);
    }
    helpers_.hold_to_confirm = std::move(list);
    return true;
}

void Editor::set_number_keys(bool on) {
    const EditStep step(*this);
    number_keys_ = on;
}

bool Editor::choose_layout(std::string_view id, KeepChanges keep) {
    const EditStep step(*this);
    std::optional<LayoutSummary> next;
    if (id != "oa-default") {
        for (const LayoutSummary& layout : installed_) {
            if (layout.id == id) {
                next = layout;
                break;
            }
        }
        if (!next)
            return false;
    }
    if (layout_id() == id && keep == KeepChanges::keep)
        return false;
    if (id == "oa-default") {
        has_chosen_ = false;
        chosen_ = {};
    } else {
        has_chosen_ = true;
        chosen_ = *next;
    }
    if (keep == KeepChanges::aside) {
        const std::vector<Listed> listed =
            listed_of(inputs_, own_, has_chosen_, chosen_, helpers_, number_keys_);
        keep_unknowns(listed, own_, helpers_);
        held_clashes_.clear();
        imported_from_.reset();
    }
    return true;
}

bool Editor::duplicate(std::string_view name) {
    const EditStep step(*this);
    if (name.empty())
        return false;
    const std::vector<Listed> before =
        listed_of(inputs_, own_, has_chosen_, chosen_, helpers_, number_keys_);
    std::vector<keys::LayoutBinding> unknowns;
    for (const keys::LayoutBinding& binding : own_.bindings) {
        if (unknown_row(before, binding.action))
            unknowns.push_back(binding);
    }
    LayoutSummary made;
    made.id = unique_slug(name, installed_);
    made.name = std::string(name);
    made.author = "You";
    made.version = "1.0";
    made.layout.id = made.id;
    made.layout.base = keys::Base::oa_default;
    if (has_chosen_)
        made.layout.bindings = chosen_.layout.bindings;
    for (const keys::LayoutBinding& binding : own_.bindings) {
        bool replaced = false;
        for (keys::LayoutBinding& existing : made.layout.bindings) {
            if (existing.action == binding.action) {
                existing = binding;
                replaced = true;
                break;
            }
        }
        if (!replaced)
            made.layout.bindings.push_back(binding);
    }
    made.helpers = effective_helpers();
    installed_.push_back(made);
    has_chosen_ = true;
    chosen_ = std::move(made);
    own_.bindings = std::move(unknowns);
    helpers_ = {};
    held_clashes_.clear();
    imported_from_.reset();
    if (!listed_id(written_, chosen_.id))
        written_.push_back(chosen_.id);
    return true;
}

bool Editor::rename(std::string_view name) {
    const EditStep step(*this);
    if (!has_chosen_ || name.empty() || chosen_.name == name)
        return false;
    chosen_.name = std::string(name);
    bool found = false;
    for (LayoutSummary& layout : installed_) {
        if (layout.id == chosen_.id) {
            layout.name = chosen_.name;
            found = true;
            break;
        }
    }
    if (!found)
        installed_.push_back(chosen_);
    if (!listed_id(written_, chosen_.id))
        written_.push_back(chosen_.id);
    return true;
}

std::size_t Editor::changes() const {
    std::size_t count = own_.bindings.size();
    if (helpers_.toggle)
        count += helpers_.toggle->empty() ? 1 : helpers_.toggle->size();
    if (helpers_.hold_to_confirm)
        count += 1;
    return count;
}

std::size_t Editor::unsaved() const {
    std::size_t count = 0;
    if (layout_id() != opened_.layout_id)
        count += 1;
    if (number_keys_ != opened_.number_keys)
        count += 1;
    if (inputs_.scheme != opened_.scheme)
        count += 1;
    count += binding_diff(own_, opened_.own);
    count += list_diff(helpers_.toggle, opened_.helpers.toggle);
    count += list_diff(helpers_.hold_to_confirm, opened_.helpers.hold_to_confirm);
    std::vector<std::string> now;
    for (const LayoutSummary& layout : installed_)
        now.push_back(layout.id + "\t" + layout.name);
    for (const std::string& line : now) {
        bool same = false;
        for (const std::string& opened : opened_.names) {
            if (line_id(opened) == line_id(line) && line_name(opened) == line_name(line))
                same = true;
        }
        if (!same)
            ++count;
    }
    for (const std::string& opened : opened_.names) {
        bool present = false;
        for (const std::string& line : now) {
            if (line_id(line) == line_id(opened))
                present = true;
        }
        if (!present)
            ++count;
    }
    return count;
}

std::size_t Editor::clashes() const {
    const keys::ResolveInput input = resolve_input();
    const keys::Resolved bound = keys::resolve(input);
    return keys::find_clashes(bound, input).size();
}

std::size_t Editor::draft_clashes() const {
    const keys::ResolveInput input = resolve_input();
    const keys::Resolved bound = keys::resolve(input);
    std::size_t count = 0;
    for (const keys::Clash& clash : keys::find_clashes(bound, input)) {
        if (draft_names(own_, clash.a) || draft_names(own_, clash.b))
            ++count;
    }
    count += held_clashes_.size();
    return count;
}

bool Editor::can_save() const {
    return draft_clashes() == 0;
}

SaveResult Editor::result() const {
    SaveResult saved;
    keys::Layout layout = own_;
    layout.id = std::string("own-") + layout_id();
    layout.base = keys::Base::oa_default;
    oa::formats::oakeys::Manifest rest = own_rest(layout_id(), helpers_);
    if (imported_from_ && !imported_from_->tool.empty() && !imported_from_->file.empty() &&
        !imported_from_->date.empty())
        rest.imported_from = imported_from_;
    saved.manifest = keys::manifest_from_layout(layout, rest);
    saved.number_keys = number_keys_;
    saved.layout_id = layout_id();
    for (const std::string& id : written_) {
        for (const LayoutSummary& summary : installed_) {
            if (summary.id == id) {
                saved.layouts.push_back(manifest_of(summary));
                break;
            }
        }
    }
    return saved;
}

bool Editor::number_keys() const noexcept {
    return number_keys_;
}

std::string Editor::layout_id() const {
    if (has_chosen_ && !chosen_.id.empty())
        return chosen_.id;
    return "oa-default";
}

std::string Editor::layout_name() const {
    if (has_chosen_ && !chosen_.name.empty())
        return chosen_.name;
    return show(inputs_.text, words::oa_default);
}

std::string Editor::layout_author() const {
    if (has_chosen_ && !chosen_.author.empty())
        return chosen_.author;
    return show(inputs_.text, words::source_oa);
}

std::string Editor::layout_version() const {
    if (has_chosen_ && !chosen_.version.empty())
        return chosen_.version;
    return "1.0";
}

const std::vector<LayoutSummary>& Editor::installed() const noexcept {
    return installed_;
}

const TextHooks& Editor::text() const noexcept {
    return inputs_.text;
}

const oa::ui::key_bindings::KeyboardMap& Editor::keyboard() const noexcept {
    return inputs_.keyboard;
}

oa::ui::key_bindings::Platform Editor::platform() const noexcept {
    return inputs_.platform;
}

keys::PointerScheme Editor::scheme() const noexcept {
    return inputs_.scheme;
}

void Editor::set_scheme(keys::PointerScheme scheme) {
    const EditStep step(*this);
    inputs_.scheme = scheme;
}

const std::string& Editor::mod_name() const noexcept {
    return inputs_.mod_name;
}

bool Editor::set_chords(std::string_view id, std::vector<keys::Chord> chords) {
    const EditStep step(*this);
    constexpr std::size_t chord_limit = 4;
    if (chords.size() > chord_limit)
        return false;
    const std::vector<Listed> listed =
        listed_of(inputs_, own_, has_chosen_, chosen_, helpers_, number_keys_);
    const Listed* row = find_listed(listed, id);
    // The same hack-hidden actions: the binding is kept for when the hack is on.
    if (row == nullptr) {
        if (acts::find(id) == nullptr)
            return false;
        put_binding(own_, std::string(id), std::move(chords));
        return true;
    }
    if (row->read_only)
        return false;
    for (const keys::Chord& had : row->chords) {
        if (!fixed_escape(*row, had, inputs_.keyboard))
            continue;
        bool kept = false;
        for (const keys::Chord& next : chords) {
            if (keys::same_key(next, had, &inputs_.keyboard))
                kept = true;
        }
        if (!kept)
            return false;
    }
    put_binding(own_, std::string(id), std::move(chords));
    return true;
}

bool Editor::clear_chords(std::string_view id) {
    const EditStep step(*this);
    const std::vector<Listed> listed =
        listed_of(inputs_, own_, has_chosen_, chosen_, helpers_, number_keys_);
    const Listed* row = find_listed(listed, id);
    if (row == nullptr || row->read_only)
        return false;
    return erase_binding(own_, id);
}

std::vector<keys::Clash> Editor::clashes_for(std::string_view id, const keys::Chord& chord) const {
    keys::Chord asked = chord;
    if (asked.kind == keys::TriggerKind::character)
        asked.scancode = 0;
    const keys::ResolveInput input = resolve_input();
    const keys::Resolved bound = keys::resolve(input);
    std::vector<keys::Clash> found = keys::clashes_with(bound, input, id, asked);
    const std::optional<keys::Chord> paired = paired_chord(asked, inputs_.platform);
    if (paired) {
        const std::vector<keys::Clash> more = keys::clashes_with(bound, input, id, *paired);
        for (const keys::Clash& extra : more) {
            bool seen = false;
            for (const keys::Clash& have : found) {
                if (have.b == extra.b)
                    seen = true;
            }
            if (!seen)
                found.push_back(extra);
        }
    }
    auto contexts_of = [&](std::string_view action) -> uint32_t {
        for (const keys::Bound& row : bound.actions) {
            if (row.id == action)
                return row.contexts;
        }
        if (const acts::Action* entry = acts::find(action))
            return entry->context;
        return 0;
    };
    const uint32_t mine = contexts_of(id);
    auto named = [&](std::string_view other) {
        for (const keys::Clash& have : found) {
            if (have.b == other)
                return true;
        }
        return false;
    };
    for (const DraftTake& take : takes_of(bound, own_)) {
        if (!chords_match(take.chord, asked, inputs_.keyboard, inputs_.platform))
            continue;
        auto add_other = [&](const std::string& other) {
            if (other.empty() || other == id || named(other))
                return;
            if (!keys::layers_meet(mine, contexts_of(other)))
                return;
            keys::Clash extra;
            extra.a = std::string(id);
            extra.b = other;
            extra.chord = asked;
            found.push_back(std::move(extra));
        };
        add_other(take.winner);
        add_other(take.loser);
    }
    return found;
}

std::vector<DraftTake> Editor::draft_takes() const {
    return takes_of(resolved(), own_);
}

std::optional<keys::Clash> Editor::draft_clash(std::string_view id) const {
    for (const HeldClash& held : held_clashes_) {
        if (held.pasted != id && held.holder != id)
            continue;
        keys::Clash oriented;
        oriented.chord = held.chord;
        oriented.a = std::string(id);
        oriented.b = held.pasted == id ? held.holder : held.pasted;
        return oriented;
    }
    const keys::ResolveInput input = resolve_input();
    const keys::Resolved bound = keys::resolve(input);
    for (const keys::Clash& clash : keys::find_clashes(bound, input)) {
        if (!draft_names(own_, clash.a) && !draft_names(own_, clash.b))
            continue;
        if (clash.a != id && clash.b != id)
            continue;
        keys::Clash oriented = clash;
        if (oriented.a != id)
            std::swap(oriented.a, oriented.b);
        return oriented;
    }
    for (const DraftTake& take : takes_of(bound, own_)) {
        if (take.winner != id && take.loser != id)
            continue;
        keys::Clash oriented;
        oriented.chord = take.chord;
        if (take.winner == id) {
            oriented.a = take.winner;
            oriented.b = take.loser;
        } else {
            oriented.a = take.loser;
            oriented.b = take.winner;
        }
        return oriented;
    }
    return std::nullopt;
}

void Editor::set_share_name(std::string name) {
    share_name_ = std::move(name);
}

const std::string& Editor::share_name() const noexcept {
    return share_name_;
}

bool Editor::settle_paste_clash(const keys::Clash& clash, Settle which) {
    const EditStep step(*this);
    for (std::size_t index = 0; index < held_clashes_.size(); ++index) {
        const HeldClash& held = held_clashes_[index];
        if (!keys::same_key(held.chord, clash.chord, &inputs_.keyboard))
            continue;
        const bool here_is_pasted = clash.a == held.pasted;
        const bool here_is_holder = clash.a == held.holder;
        if (!here_is_pasted && !here_is_holder)
            continue;
        const std::string& other = here_is_pasted ? held.holder : held.pasted;
        if (!clash.b.empty() && clash.b != other)
            continue;
        const bool keep_pasted = (here_is_pasted && which == Settle::keep_here) ||
                                 (here_is_holder && which == Settle::keep_there);
        const HeldClash taken = held;
        held_clashes_.erase(held_clashes_.begin() + static_cast<std::ptrdiff_t>(index));
        if (!keep_pasted)
            return true;
        const Details holder = details(taken.holder);
        std::vector<keys::Chord> remain;
        remain.reserve(holder.chords.size());
        for (const ChordDetail& one : holder.chords) {
            if (!keys::same_key(one.chord, taken.chord, &inputs_.keyboard))
                remain.push_back(one.chord);
        }
        if (!holder.id.empty() && !holder.read_only)
            set_chords(taken.holder, std::move(remain));
        const Details pasted = details(taken.pasted);
        if (pasted.id.empty() || pasted.read_only)
            return true;
        std::vector<keys::Chord> next;
        next.reserve(pasted.chords.size() + 1);
        bool already = false;
        for (const ChordDetail& one : pasted.chords) {
            next.push_back(one.chord);
            if (keys::same_key(one.chord, taken.chord, &inputs_.keyboard))
                already = true;
        }
        if (!already && next.size() < 4)
            next.push_back(taken.chord);
        set_chords(taken.pasted, std::move(next));
        return true;
    }
    return false;
}

EditStep::EditStep(Editor& editor) : editor_(editor) {
    if (editor_.step_depth_ == 0)
        editor_.undo_.push_back(editor_.draft_now());
    ++editor_.step_depth_;
}

EditStep::~EditStep() {
    --editor_.step_depth_;
    if (editor_.step_depth_ != 0 || editor_.undo_.empty())
        return;
    if (Editor::draft_print(editor_.undo_.back()) == Editor::draft_print(editor_.draft_now())) {
        editor_.undo_.pop_back();
        return;
    }
    if (editor_.undo_.size() > undo_limit)
        editor_.undo_.erase(editor_.undo_.begin());
}

Editor::Draft Editor::draft_now() const {
    Draft draft;
    draft.own = own_;
    draft.helpers = helpers_;
    draft.number_keys = number_keys_;
    draft.has_chosen = has_chosen_;
    draft.chosen = chosen_;
    draft.installed = installed_;
    draft.written = written_;
    draft.held_clashes = held_clashes_;
    draft.imported_from = imported_from_;
    draft.scheme = inputs_.scheme;
    return draft;
}

std::string Editor::draft_print(const Draft& draft) {
    std::string print;
    for (const keys::LayoutBinding& binding : draft.own.bindings) {
        print += binding_line(binding);
        print += '\n';
    }
    const auto list = [&](const std::optional<std::vector<std::string>>& ids) {
        if (!ids) {
            print += "-\n";
            return;
        }
        for (const std::string& id : *ids) {
            print += id;
            print += ',';
        }
        print += '\n';
    };
    list(draft.helpers.toggle);
    list(draft.helpers.hold_to_confirm);
    print += draft.number_keys ? "n1" : "n0";
    print += draft.has_chosen ? draft.chosen.id + "\t" + draft.chosen.name : std::string("-");
    print += '\n';
    for (const LayoutSummary& layout : draft.installed)
        print += layout.id + "\t" + layout.name + "\n";
    for (const std::string& id : draft.written)
        print += id + ",";
    print += '\n';
    for (const HeldClash& held : draft.held_clashes)
        print += held.pasted + "\t" + held.holder + "\n";
    print += draft.imported_from ? "i1" : "i0";
    print += draft.scheme == keys::PointerScheme::left_click ? "l" : "r";
    return print;
}

bool Editor::undo() {
    if (undo_.empty() || step_depth_ != 0)
        return false;
    Draft draft = std::move(undo_.back());
    undo_.pop_back();
    own_ = std::move(draft.own);
    helpers_ = std::move(draft.helpers);
    number_keys_ = draft.number_keys;
    has_chosen_ = draft.has_chosen;
    chosen_ = std::move(draft.chosen);
    installed_ = std::move(draft.installed);
    written_ = std::move(draft.written);
    held_clashes_ = std::move(draft.held_clashes);
    imported_from_ = std::move(draft.imported_from);
    inputs_.scheme = draft.scheme;
    return true;
}

bool Editor::can_undo() const noexcept {
    return !undo_.empty();
}

} // namespace oa::ui::key_editor

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The keys overlay's groups, its panel beside the side column, and the
// blended ground it draws on.
#include "oa/ui/key_editor/overlay.hpp"

#include "oa/data/input_actions/actions.hpp"
#include "oa/ui/key_bindings/chord.hpp"
#include "oa/ui/key_bindings/key_codes.hpp"
#include "oa/ui/kit/chrome.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/keys.hpp"
#include "oa/ui/kit/theme.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::key_editor {
namespace {

namespace keys = oa::ui::key_bindings;
namespace acts = oa::data::input_actions;
namespace kit = oa::ui::kit;

/// One category, in the order the overlay lists them. "This mod" is not here:
/// it is inserted immediately before Game.
struct AreaSpec {
    std::string_view id{};
    std::string_view title{};
    kit::KeyTint tint{kit::KeyTint::none};
    std::string_view letter{};
};

constexpr AreaSpec kAreas[] = {
    {"orders", words::area_orders, kit::KeyTint::orders, words::letter_orders},
    {"mouse", words::area_pointer, kit::KeyTint::pointer, words::letter_pointer},
    {"selection", words::area_selection, kit::KeyTint::selection, words::letter_selection},
    {"groups", words::area_groups, kit::KeyTint::groups, words::letter_groups},
    {"camera", words::area_camera, kit::KeyTint::camera, words::letter_camera},
    {"building", words::area_building, kit::KeyTint::building, words::letter_building},
    {"game", words::area_game, kit::KeyTint::game, words::letter_game},
    {"chat", words::area_chat, kit::KeyTint::chat, words::letter_chat},
    {"watching", words::area_watching, kit::KeyTint::none, {}},
    {"window", words::area_window, kit::KeyTint::window, words::letter_window},
    {"menus", words::area_menus, kit::KeyTint::none, words::letter_menus},
    {"developer", words::area_developer, kit::KeyTint::developer, words::letter_developer},
    {"megamap", words::area_megamap, kit::KeyTint::hacks, words::letter_hacks},
    {"whiteboard", words::area_whiteboard, kit::KeyTint::hacks, words::letter_hacks},
};

/// The id used for the hack group, which is not a registry area.
constexpr std::string_view kModArea = "mod";

/// One action that can be listed, before a family is collapsed.
struct Row {
    std::string name{};
    std::string chord{};
    std::string aside{};
    std::string stem{};
    int number{0};
    int key_index{-1};
    uint8_t modifiers{};
    keys::TriggerKind trigger{keys::TriggerKind::character};
    keys::Gesture gesture{keys::Gesture::press};
    uint16_t order{};
    std::string area{};
};

/// The bit of a context named in the registry.
///
/// @param name the context id
/// @return the bit, or 0 when the registry has no such context
uint32_t context_bit(std::string_view name) {
    const auto table = acts::contexts();
    for (std::size_t index = 0; index < table.size(); ++index) {
        if (table[index].name == name && index < 32)
            return uint32_t{1} << static_cast<uint32_t>(index);
    }
    return 0;
}

/// Reports whether a chord is a key rather than a mouse, a wheel or a pad.
///
/// @param kind the trigger
/// @return true for a character, a place, a named key or a modifier alone
bool key_trigger(keys::TriggerKind kind) {
    return kind == keys::TriggerKind::character || kind == keys::TriggerKind::place ||
           kind == keys::TriggerKind::named || kind == keys::TriggerKind::modifier_alone;
}

/// The first chord of an action that is a key.
///
/// @param bound the action
/// @return that chord, or null when every chord is a mouse, a wheel or a pad
const keys::Chord* first_key_chord(const keys::Bound& bound) {
    for (const keys::BoundChord& chord : bound.chords) {
        if (key_trigger(chord.chord.kind))
            return &chord.chord;
    }
    return nullptr;
}

/// Reports whether every source of an action is a hack id.
///
/// @param entry the registry row
/// @return true when the action exists only because a hack is on
bool all_hack_sources(const acts::Action& entry) {
    if (entry.source.empty())
        return false;
    const auto hacks = acts::hack_ids();
    for (const std::string_view source : entry.source) {
        if (std::find(hacks.begin(), hacks.end(), source) == hacks.end())
            return false;
    }
    return true;
}

/// Splits a trailing "-N" off an action id.
///
/// @param id the action id
/// @param[out] stem the id without "-N"
/// @param[out] number N, greater than 0
/// @return true when the id ends in a family number
bool family_of(std::string_view id, std::string& stem, int& number) {
    const std::size_t dash = id.rfind('-');
    if (dash == std::string_view::npos || dash == 0 || dash + 1 >= id.size())
        return false;
    int value = 0;
    for (std::size_t index = dash + 1; index < id.size(); ++index) {
        const char digit = id[index];
        if (digit < '0' || digit > '9')
            return false;
        value = value * 10 + (digit - '0');
    }
    if (value <= 0)
        return false;
    stem.assign(id.substr(0, dash));
    number = value;
    return true;
}

/// The place a digit key has in a run. Digit1 is 0 and Digit0 follows Digit9.
///
/// @param code a UI Events code
/// @return the place, or -1 when the code is not a digit
int digit_place(std::string_view code) {
    if (code.size() == 6 && code.substr(0, 5) == "Digit") {
        const char digit = code[5];
        if (digit >= '1' && digit <= '9')
            return digit - '1';
        if (digit == '0')
            return 9;
    }
    return -1;
}

/// Where a chord's key sits in a numbered run.
///
/// Digit keys, F keys and numpad keys each form their own run. Any other key
/// has no place, so it never continues a run.
///
/// @param chord the chord
/// @return the place, or -1
int key_place(const keys::Chord& chord) {
    if (chord.kind == keys::TriggerKind::place)
        return digit_place(keys::code_of(chord.scancode));
    if (chord.kind == keys::TriggerKind::named) {
        const int named = static_cast<int>(chord.named);
        const int f1 = static_cast<int>(keys::NamedKey::f1);
        const int f24 = static_cast<int>(keys::NamedKey::f24);
        if (named >= f1 && named <= f24)
            return named - f1;
        const int pad0 = static_cast<int>(keys::NamedKey::numpad_0);
        const int pad9 = static_cast<int>(keys::NamedKey::numpad_9);
        if (named >= pad0 && named <= pad9)
            return named - pad0;
        return -1;
    }
    if (chord.kind == keys::TriggerKind::character) {
        if (chord.character >= U'1' && chord.character <= U'9')
            return static_cast<int>(chord.character - U'1');
        if (chord.character == U'0')
            return 9;
        if (chord.character >= U'A' && chord.character <= U'Z')
            return static_cast<int>(chord.character);
    }
    return -1;
}

/// Drops a trailing " N" from a family action's name.
///
/// @param name the looked-up name
/// @param number the family's number
/// @return the name without that number
std::string family_name(std::string name, int number) {
    const std::string tail = " " + std::to_string(number);
    if (name.size() >= tail.size() &&
        name.compare(name.size() - tail.size(), tail.size(), tail) == 0)
        name.resize(name.size() - tail.size());
    return name;
}

/// The text of a collapsed run: the shared modifier prefix once, then the two keys.
///
/// @param hooks the lookup
/// @param first_text the first chord
/// @param last_text the last chord
/// @return the range, such as Ctrl+1…9 or F5…F8
std::string
range_text(const TextHooks& hooks, std::string_view first_text, std::string_view last_text) {
    std::string_view last_key = last_text;
    const std::size_t plus = first_text.rfind('+');
    if (plus != std::string_view::npos && last_text.size() > plus &&
        first_text.substr(0, plus + 1) == last_text.substr(0, plus + 1))
        last_key = last_text.substr(plus + 1);
    return fill(show(hooks, words::chord_range), {{"first", first_text}, {"last", last_key}});
}

/// Reports whether the next row continues the run the previous row is in.
///
/// @param previous the row already in the run
/// @param next the following filtered row
/// @return true when the family number and the key both advance by one
bool continues_run(const Row& previous, const Row& next) {
    if (previous.stem.empty() || previous.stem != next.stem)
        return false;
    if (next.number != previous.number + 1)
        return false;
    if (previous.key_index < 0 || next.key_index != previous.key_index + 1)
        return false;
    return previous.modifiers == next.modifiers && previous.trigger == next.trigger &&
           previous.gesture == next.gesture;
}

/// The aside a run keeps: the one every member shares, or empty.
///
/// @param rows the run
/// @param first the first index
/// @param last one past the last index
/// @return the shared aside
std::string shared_aside(const std::vector<Row>& rows, std::size_t first, std::size_t last) {
    if (first >= last)
        return {};
    const std::string& aside = rows[first].aside;
    for (std::size_t index = first + 1; index < last; ++index) {
        if (rows[index].aside != aside)
            return {};
    }
    return aside;
}

/// Appends one text item.
///
/// @param[in,out] list the display list
/// @param rect where the text sits
/// @param text the words
/// @param font which font
/// @param ink the colour
/// @param clip the scroll viewport; empty means none
void add_text(
    kit::DisplayList& list,
    const kit::Rect& rect,
    std::string text,
    kit::FontRole font,
    kit::Colour ink,
    const kit::Rect& clip
) {
    kit::Item item;
    item.role = kit::Role::text;
    item.rect = rect;
    item.clip = clip;
    item.text = std::move(text);
    item.font = font;
    item.colour = ink;
    item.align = kit::Align::left;
    list.items.push_back(std::move(item));
}

/// Primary's name on this platform, from the keycaps a chord shows.
///
/// @param inputs the keyboard and the platform
/// @return Ctrl, or Cmd where Primary is the command key
std::string primary_name(const OverlayInputs& inputs) {
    keys::Chord sample;
    sample.modifiers = keys::primary;
    sample.kind = keys::TriggerKind::character;
    sample.character = U'A';
    const std::string text = keys::display_text(sample, inputs.keyboard, inputs.platform);
    const std::size_t plus = text.find('+');
    if (plus == std::string::npos)
        return text;
    return text.substr(0, plus);
}

/// How wide the panel wants to be, before the battlefield clamps it.
///
/// @param size_class the match dialog's class
/// @return 240, 420 or 520 points
int32_t preferred_width(kit::SizeClass size_class) {
    if (size_class == kit::SizeClass::large)
        return 520;
    if (size_class == kit::SizeClass::regular)
        return 420;
    return 240;
}

/// One group's block, measured before it is placed.
struct Block {
    OverlayGroup group{};
    int32_t height{};
};

/// Measures one group. The height is the letter, one row per line and a gap.
///
/// @param group the group
/// @return the block
Block measure_group(const OverlayGroup& group) {
    Block block;
    block.group = group;
    const int32_t line = kit::compact_metrics.regular_line;
    block.height = kit::keycap_height + 4;
    block.height += static_cast<int32_t>(group.lines.size()) * (line + 2);
    block.height += 8;
    return block;
}

/// Where a two-column list splits, and how tall the content is.
struct ColumnSplit {
    std::size_t split{}; ///< the first group of the right column
    int32_t height{};    ///< the taller column, in points
};

/// Splits groups across one or two columns.
///
/// @param blocks the measured groups
/// @param two true when the list has two columns
/// @return the split and the content's height
ColumnSplit split_columns(const std::vector<Block>& blocks, bool two) {
    int32_t total = 0;
    for (const Block& block : blocks)
        total += block.height;
    if (!two || blocks.size() < 2)
        return {blocks.size(), total};
    int32_t running = 0;
    const int32_t half = (total + 1) / 2;
    std::size_t split = blocks.size();
    for (std::size_t index = 0; index < blocks.size(); ++index) {
        if (index > 0 && running >= half) {
            split = index;
            break;
        }
        running += blocks[index].height;
    }
    if (split == 0)
        split = 1;
    if (split >= blocks.size())
        split = blocks.size() - 1;
    int32_t left_height = 0;
    int32_t right_height = 0;
    for (std::size_t index = 0; index < split; ++index)
        left_height += blocks[index].height;
    for (std::size_t index = split; index < blocks.size(); ++index)
        right_height += blocks[index].height;
    return {split, std::max(left_height, right_height)};
}

/// Places the groups into one or two columns.
///
/// @param[in,out] list the display list
/// @param fonts the fonts the text is cut to
/// @param blocks the measured groups
/// @param origin the list's top left
/// @param width the list's width
/// @param split the first group of the right column
/// @param scroll how far the list is scrolled
/// @param viewport the part that stays visible
void place_groups(
    kit::DisplayList& list,
    const kit::Fonts& fonts,
    const std::vector<Block>& blocks,
    kit::Point origin,
    int32_t width,
    std::size_t split,
    int32_t scroll,
    const kit::Rect& viewport
) {
    const int32_t gap = 8;
    const bool two = split < blocks.size();
    const int32_t column_width = two ? std::max((width - gap) / 2, int32_t{0}) : width;
    const auto place_column = [&](std::size_t begin, std::size_t end, int32_t column_x) {
        int32_t y = origin.y;
        for (std::size_t index = begin; index < end; ++index) {
            const OverlayGroup& group = blocks[index].group;
            const int32_t shown_y = y - scroll;
            kit::KeycapLook cap;
            cap.label = group.letter;
            cap.tint = group.tint;
            const int32_t badge = kit::keycap_height;
            kit::add_keycap(list, {column_x, shown_y, badge, badge}, cap, kit::no_control, {});
            if (!list.items.empty())
                list.items.back().clip = viewport;
            const int32_t title_x = column_x + badge + 4;
            const int32_t title_w = std::max(column_width - badge - 4, int32_t{0});
            kit::Item heading;
            heading.role = kit::Role::heading;
            heading.rect = {title_x, shown_y, title_w, badge};
            heading.clip = viewport;
            heading.look = kit::HeadingLook{
                kit::cut_to_width(fonts, kit::FontRole::small, group.title, title_w)
            };
            list.items.push_back(std::move(heading));
            int32_t line_y = shown_y + badge + 4;
            const int32_t line_h = kit::compact_metrics.regular_line;
            for (const OverlayLine& line : group.lines) {
                const int32_t chord_w =
                    std::min(std::max(column_width / 2, int32_t{0}), int32_t{120});
                const int32_t name_w = std::max(column_width - chord_w - 4, int32_t{0});
                std::string shown = line.name;
                if (!line.aside.empty()) {
                    shown.push_back(' ');
                    shown += line.aside;
                }
                add_text(
                    list,
                    {column_x, line_y, name_w, line_h},
                    kit::cut_to_width(fonts, kit::FontRole::small, shown, name_w),
                    kit::FontRole::small,
                    kit::colour::text,
                    viewport
                );
                add_text(
                    list,
                    {column_x + name_w, line_y, chord_w, line_h},
                    kit::cut_to_width(fonts, kit::FontRole::small, line.chord, chord_w),
                    kit::FontRole::small,
                    kit::colour::accent,
                    viewport
                );
                line_y += line_h + 2;
            }
            y += blocks[index].height;
        }
    };
    place_column(0, split, origin.x);
    if (two)
        place_column(split, blocks.size(), origin.x + column_width + gap);
}

} // namespace

std::string overlay_title(const OverlayInputs& inputs) {
    const uint32_t placing = context_bit("placing");
    const uint32_t watch = context_bit("watch");
    const uint32_t play = context_bit("play");
    if ((inputs.contexts.active & placing) != 0)
        return show(inputs.text, words::keys_while_placing);
    if ((inputs.contexts.active & watch) != 0 && (inputs.contexts.active & play) == 0)
        return show(inputs.text, words::keys_while_watching);
    return show(inputs.text, words::keys_while_playing);
}

std::vector<OverlayGroup> overlay_groups(const OverlayInputs& inputs) {
    std::vector<Row> rows;
    rows.reserve(inputs.resolved.actions.size());
    for (const keys::Bound& bound : inputs.resolved.actions) {
        if (bound.entry == nullptr)
            continue;
        if (bound.entry->kind == acts::Kind::pointer || bound.entry->kind == acts::Kind::wheel)
            continue;
        if ((bound.contexts & inputs.contexts.active) == 0)
            continue;
        if ((bound.when & inputs.contexts.conditions) != bound.when)
            continue;
        if (!keys::conditions_meet(bound.when, inputs.contexts.conditions))
            continue;
        const keys::Chord* chord = first_key_chord(bound);
        if (chord == nullptr)
            continue;
        Row row;
        row.name = show(inputs.text, bound.entry->words.name);
        row.chord = keys::display_text(*chord, inputs.keyboard, inputs.platform);
        if (bound.entry->kind == acts::Kind::hold || bound.entry->kind == acts::Kind::modifier)
            row.aside = show(inputs.text, words::aside_held);
        if (!family_of(bound.id, row.stem, row.number)) {
            row.stem.clear();
            row.number = 0;
        }
        row.key_index = key_place(*chord);
        row.modifiers = chord->modifiers;
        row.trigger = chord->kind;
        row.gesture = chord->gesture;
        row.order = bound.entry->order;
        row.area =
            all_hack_sources(*bound.entry) ? std::string(kModArea) : std::string(bound.entry->area);
        rows.push_back(std::move(row));
    }

    std::vector<OverlayGroup> groups;
    const auto emit_area = [&](std::string_view area,
                               kit::KeyTint tint,
                               std::string_view letter,
                               std::string_view title) {
        std::vector<Row> area_rows;
        for (const Row& row : rows) {
            if (row.area == area)
                area_rows.push_back(row);
        }
        std::stable_sort(area_rows.begin(), area_rows.end(), [](const Row& a, const Row& b) {
            if (a.order != b.order)
                return a.order < b.order;
            return a.name < b.name;
        });
        if (area_rows.empty())
            return;
        OverlayGroup group;
        group.tint = tint;
        group.letter = show(inputs.text, letter);
        group.title = show(inputs.text, title);
        for (std::size_t index = 0; index < area_rows.size();) {
            std::size_t end = index + 1;
            while (end < area_rows.size() && continues_run(area_rows[end - 1], area_rows[end]))
                ++end;
            OverlayLine line;
            if (end - index >= 2) {
                line.name = family_name(area_rows[index].name, area_rows[index].number);
                line.chord =
                    range_text(inputs.text, area_rows[index].chord, area_rows[end - 1].chord);
                line.aside = shared_aside(area_rows, index, end);
                group.lines.push_back(std::move(line));
                index = end;
            } else {
                line.name = std::move(area_rows[index].name);
                line.chord = std::move(area_rows[index].chord);
                line.aside = std::move(area_rows[index].aside);
                group.lines.push_back(std::move(line));
                ++index;
            }
        }
        groups.push_back(std::move(group));
    };

    for (const AreaSpec& area : kAreas) {
        if (area.id == "game") {
            emit_area(kModArea, kit::KeyTint::mod, words::letter_mod, words::this_mod);
        }
        emit_area(area.id, area.tint, area.letter, area.title);
    }
    return groups;
}

OverlayLayout overlay_layout(
    const OverlayInputs& inputs,
    const std::vector<OverlayGroup>& groups,
    const OverlayRoom& room,
    const kit::Fonts& fonts,
    int32_t scroll
) {
    const kit::Metrics& metrics = kit::metrics_of(room.size_class);
    const int32_t pad = metrics.padding;
    const int32_t field_right = room.battlefield.x + room.battlefield.width;
    const int32_t field_bottom = room.battlefield.y + room.battlefield.height;
    int32_t panel_x = std::max(room.battlefield.x, room.column_right);
    int32_t panel_y = room.battlefield.y + pad;
    int32_t room_w = field_right - panel_x - pad;
    if (room_w < 0)
        room_w = 0;
    int32_t panel_w = std::min(preferred_width(room.size_class), room_w);
    const bool two = room.size_class != kit::SizeClass::compact && panel_w >= 360;

    const std::string title = overlay_title(inputs);
    const std::string layout_name =
        inputs.layout_name.empty() ? show(inputs.text, words::oa_default) : inputs.layout_name;
    const int32_t changes = std::max(inputs.changes, int32_t{0});
    const std::string aside = fill(
        show(inputs.text, words::overlay_changes),
        {{"name", layout_name}, {"n", std::to_string(changes)}}
    );
    std::string on_text;
    if (!inputs.on.empty()) {
        on_text = show(inputs.text, words::on_line);
        for (const std::string& chip : inputs.on) {
            on_text.push_back(' ');
            on_text += chip;
        }
    }
    const std::string footer =
        fill(show(inputs.text, words::overlay_footer), {{"modifier", primary_name(inputs)}});
    const std::string edit = show(inputs.text, words::edit_keys);

    const int32_t line_h = metrics.regular_line;
    const int32_t small_h = metrics.small_line;
    int32_t header_h = line_h + 4 + small_h + 4 + metrics.button_height + 4;
    if (!on_text.empty())
        header_h += small_h + 4;
    const int32_t footer_h = small_h * 2;

    const int32_t inner = std::max(panel_w - 2 * pad, int32_t{0});
    std::vector<Block> blocks;
    blocks.reserve(groups.size());
    for (const OverlayGroup& group : groups)
        blocks.push_back(measure_group(group));
    const ColumnSplit columns = split_columns(blocks, two);
    const int32_t content_h = columns.height;

    const int32_t natural = pad + header_h + content_h + footer_h + pad;
    int32_t max_h = field_bottom - panel_y - pad;
    if (max_h < 0)
        max_h = 0;
    int32_t panel_h = std::min(natural, max_h);
    const int32_t least = pad + header_h + footer_h + pad;
    if (panel_h < least)
        panel_h = std::min(least, std::max(max_h, least));
    if (panel_y + panel_h > field_bottom)
        panel_h = std::max(field_bottom - panel_y, int32_t{0});
    if (panel_x + panel_w > field_right)
        panel_w = std::max(field_right - panel_x, int32_t{0});

    int32_t viewport_h = panel_h - pad - header_h - footer_h - pad;
    if (viewport_h < 0)
        viewport_h = 0;
    const int32_t scroll_max = std::max(content_h - viewport_h, int32_t{0});
    const int32_t clamped = std::clamp(scroll, int32_t{0}, scroll_max);

    OverlayLayout laid;
    laid.panel = {panel_x, panel_y, panel_w, panel_h};
    laid.scroll = clamped;
    laid.scroll_max = scroll_max;

    const int32_t close_side = metrics.button_height;
    const kit::Rect close_rect{
        std::max(panel_w - pad - close_side, int32_t{0}), pad, close_side, close_side
    };
    kit::ButtonLook close_look;
    close_look.style = kit::ButtonStyle::quiet;
    close_look.mark = kit::ButtonMark::close;
    kit::add_button(
        laid.list, close_rect, close_look, id_overlay_close, std::string(name_keys_overlay_close)
    );

    const int32_t title_w = std::max(close_rect.x - pad - 4, int32_t{0});
    add_text(
        laid.list,
        {pad, pad, title_w, line_h},
        kit::cut_to_width(fonts, kit::FontRole::regular, title, title_w),
        kit::FontRole::regular,
        kit::colour::text,
        {}
    );
    add_text(
        laid.list,
        {pad, pad + line_h + 4, inner, small_h},
        kit::cut_to_width(fonts, kit::FontRole::small, aside, inner),
        kit::FontRole::small,
        kit::colour::quiet,
        {}
    );
    const int32_t button_y = pad + line_h + 4 + small_h + 4;
    int32_t button_w = kit::text_width(fonts, kit::FontRole::small, edit) + 16;
    button_w = std::min(std::max(button_w, int32_t{80}), inner);
    kit::ButtonLook edit_look;
    edit_look.caption = edit;
    edit_look.style = kit::ButtonStyle::plain;
    kit::add_button(
        laid.list,
        {pad, button_y, button_w, metrics.button_height},
        edit_look,
        id_overlay_edit,
        std::string(name_keys_overlay_edit)
    );

    int32_t list_top = button_y + metrics.button_height + 4;
    if (!on_text.empty()) {
        add_text(
            laid.list,
            {pad, list_top, inner, small_h},
            kit::cut_to_width(fonts, kit::FontRole::small, on_text, inner),
            kit::FontRole::small,
            kit::colour::hint,
            {}
        );
        list_top += small_h + 4;
    }
    const kit::Rect viewport{pad, list_top, inner, viewport_h};
    const int32_t bar_w = scroll_max > 0 ? 8 : 0;
    const int32_t list_w = std::max(inner - bar_w, int32_t{0});
    place_groups(
        laid.list, fonts, blocks, {pad, list_top}, list_w, columns.split, clamped, viewport
    );

    if (scroll_max > 0 && viewport_h > 0) {
        const int32_t content_span = scroll_max + viewport_h;
        int32_t thumb_h = viewport_h;
        if (content_span > 0)
            thumb_h = std::max(viewport_h * viewport_h / content_span, int32_t{12});
        if (thumb_h > viewport_h)
            thumb_h = viewport_h;
        const int32_t travel = std::max(viewport_h - thumb_h, int32_t{0});
        int32_t thumb_y = list_top;
        if (scroll_max > 0)
            thumb_y += clamped * travel / scroll_max;
        kit::Item bar;
        bar.role = kit::Role::scroll_bar;
        bar.rect = {pad + list_w, list_top, bar_w, viewport_h};
        kit::ScrollBarLook look;
        look.thumb = {bar.rect.x, thumb_y, bar_w, thumb_h};
        bar.look = look;
        laid.list.items.push_back(std::move(bar));
        kit::Control bar_control;
        bar_control.id = id_overlay_bar;
        bar_control.rect = bar.rect;
        bar_control.name = "keys-overlay.bar";
        bar_control.kind = kit::ControlKind::scroll_bar;
        bar_control.focusable = false;
        laid.list.controls.push_back(std::move(bar_control));
    }

    add_text(
        laid.list,
        {pad, panel_h - pad - footer_h, inner, footer_h},
        kit::cut_to_width(fonts, kit::FontRole::small, footer, inner),
        kit::FontRole::small,
        kit::colour::quiet,
        {}
    );

    std::string listed = title;
    listed.push_back('\n');
    listed += aside;
    listed.push_back('\n');
    if (!on_text.empty()) {
        listed += on_text;
        listed.push_back('\n');
    }
    for (const OverlayGroup& group : groups) {
        listed += group.title;
        listed.push_back('\n');
        for (const OverlayLine& line : group.lines) {
            listed += line.name;
            listed.push_back(' ');
            listed += line.chord;
            if (!line.aside.empty()) {
                listed.push_back(' ');
                listed += line.aside;
            }
            listed.push_back('\n');
        }
    }
    listed += footer;
    kit::Control list_control;
    list_control.id = id_overlay_list;
    list_control.rect = viewport;
    list_control.name = std::string(name_keys_overlay_list);
    list_control.kind = kit::ControlKind::list;
    list_control.focusable = false;
    list_control.text = std::move(listed);
    laid.list.controls.push_back(std::move(list_control));
    return laid;
}

void draw_overlay(const kit::Canvas& canvas, const kit::DisplayList& list, kit::Point size) {
    const kit::Colour face = kit::hud_colour::panel;
    const uint32_t opacity = (static_cast<uint32_t>(face.a) * 256u + 127u) / 255u;
    kit::blend_rect(canvas, {0, 0, size.x, size.y}, face, opacity);
    kit::outline_rect(canvas, {0, 0, size.x, size.y}, kit::colour::rule);
    kit::paint(canvas, list);
}

} // namespace oa::ui::key_editor

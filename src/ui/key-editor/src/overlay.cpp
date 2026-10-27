// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The keys overlay's groups, its panel beside the side column, and the
// translucent ground it draws on.
#include "oa/ui/key_editor/overlay.hpp"

#include "oa/data/input_actions/actions.hpp"
#include "oa/ui/key_bindings/chord.hpp"
#include "oa/ui/key_bindings/key_codes.hpp"
#include "oa/ui/kit/chrome.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/controls.hpp"
#include "oa/ui/kit/keys.hpp"
#include "oa/ui/kit/text.hpp"
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

/// One registry area, in the order the overlay lists them. "This mod" is
/// not one: it goes just before Game.
struct AreaSpec {
    std::string_view id{};    ///< the registry's area
    std::string_view title{}; ///< its title, in English
    kit::KeyTint tint{kit::KeyTint::none};
    std::string_view letter{}; ///< its letter, in English; empty for none
};

/// The areas, in the key editor's order.
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
};

/// The group that holds the actions a hack adds. It is no registry area.
constexpr std::string_view kModArea = "mod";

/// Room between a group's heading and its first line, and between lines'
/// keycaps, in points.
constexpr int32_t kLineGap = 4;

/// One action that can be listed, before a family is collapsed.
struct Row {
    std::string name{};
    std::string chord{};
    std::vector<std::string> keys{};
    std::string gesture{};
    std::string aside{};
    std::string stem{};  ///< the family's id without its number; empty for none
    int32_t number{};    ///< the family's number, from 1
    int32_t place{-1};   ///< where the key sits in a run; -1 for no run
    uint8_t modifiers{}; ///< the chord's modifiers
    keys::TriggerKind trigger{keys::TriggerKind::character};
    keys::Gesture gesture_kind{keys::Gesture::press};
    uint16_t order{};
    std::string area{};
};

/// Returns the bit of a context the registry names.
///
/// @param name the context's id
/// @return the bit, or 0 when the registry has no such context
uint32_t context_bit(std::string_view name) {
    const auto table = acts::contexts();
    for (std::size_t index = 0; index < table.size() && index < 32; ++index) {
        if (table[index].name == name)
            return uint32_t{1} << static_cast<uint32_t>(index);
    }
    return 0;
}

/// Reports whether a chord is a key rather than a mouse button, a wheel or a pad.
///
/// @param kind the trigger
/// @return true for a character, a place, a named key or a modifier alone
bool key_trigger(keys::TriggerKind kind) {
    return kind == keys::TriggerKind::character || kind == keys::TriggerKind::place ||
           kind == keys::TriggerKind::named || kind == keys::TriggerKind::modifier_alone;
}

/// Returns an action's first chord that is a key.
///
/// @param bound the action
/// @return that chord, or null when every chord is a mouse button, a wheel or a pad
const keys::Chord* first_key_chord(const keys::Bound& bound) {
    for (const keys::BoundChord& chord : bound.chords) {
        if (key_trigger(chord.chord.kind))
            return &chord.chord;
    }
    return nullptr;
}

/// Reports whether every source of an action is a hack.
///
/// @param entry the registry's row
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
/// @param[out] number N, 1 or more
/// @return true when the id ends in a family's number
bool family_of(std::string_view id, std::string& stem, int32_t& number) {
    const std::size_t dash = id.rfind('-');
    if (dash == std::string_view::npos || dash == 0 || dash + 1 >= id.size() ||
        id.size() - dash > 4)
        return false;
    int32_t value = 0;
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

/// Returns where a digit key sits in a run: Digit1 is 0, and Digit0 follows Digit9.
///
/// @param code a key's code, as the W3C names it
/// @return the place, or -1 when the code is not a digit
int32_t digit_place(std::string_view code) {
    if (code.size() == 6 && code.substr(0, 5) == "Digit") {
        const char digit = code[5];
        if (digit >= '1' && digit <= '9')
            return digit - '1';
        if (digit == '0')
            return 9;
    }
    return -1;
}

/// Returns where a chord's key sits in a numbered run.
///
/// The digits, the F keys and the keypad's digits each make a run of their
/// own, and the letters one in alphabetical order. Any other key has no
/// place, so it never continues a run.
///
/// @param chord the chord
/// @return the place, or -1
int32_t key_place(const keys::Chord& chord) {
    if (chord.kind == keys::TriggerKind::place)
        return digit_place(keys::code_of(chord.scancode));
    if (chord.kind == keys::TriggerKind::named) {
        const int32_t named = static_cast<int32_t>(chord.named);
        const int32_t f1 = static_cast<int32_t>(keys::NamedKey::f1);
        const int32_t f24 = static_cast<int32_t>(keys::NamedKey::f24);
        if (named >= f1 && named <= f24)
            return 100 + named - f1;
        const int32_t pad0 = static_cast<int32_t>(keys::NamedKey::numpad_0);
        const int32_t pad9 = static_cast<int32_t>(keys::NamedKey::numpad_9);
        if (named >= pad0 && named <= pad9)
            return 200 + named - pad0;
        return -1;
    }
    if (chord.kind == keys::TriggerKind::character) {
        if (chord.character >= U'1' && chord.character <= U'9')
            return static_cast<int32_t>(chord.character - U'1');
        if (chord.character == U'0')
            return 9;
        if (chord.character >= U'A' && chord.character <= U'Z')
            return 300 + static_cast<int32_t>(chord.character - U'A');
    }
    return -1;
}

/// Drops a trailing " N" from a family member's name.
///
/// @param name the looked-up name
/// @param number the member's number
/// @return the name without it
std::string family_name(std::string name, int32_t number) {
    const std::string tail = " " + std::to_string(number);
    if (name.size() > tail.size() &&
        name.compare(name.size() - tail.size(), tail.size(), tail) == 0)
        name.resize(name.size() - tail.size());
    return name;
}

/// Reports whether a row continues the run the row before it is in.
///
/// @param previous the run's last row
/// @param next the row after it
/// @return true when the family's number and the key both step by one, and
///     the modifiers, the trigger and the gesture are the same
bool continues_run(const Row& previous, const Row& next) {
    if (previous.stem.empty() || previous.stem != next.stem)
        return false;
    if (next.number != previous.number + 1)
        return false;
    if (previous.place < 0 || next.place != previous.place + 1)
        return false;
    return previous.modifiers == next.modifiers && previous.trigger == next.trigger &&
           previous.gesture_kind == next.gesture_kind && previous.keys.size() == next.keys.size();
}

/// Returns the aside every row of a run shares, or nothing.
///
/// @param rows the rows
/// @param first the run's first row
/// @param last one past its last row
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

/// Collapses a run of rows into one line: the family's name, the first
/// chord's keycaps with the last one holding both ends, and the chord's text
/// with the shared modifiers once.
///
/// @param hooks the lookup
/// @param rows the area's rows
/// @param first the run's first row
/// @param last one past its last row
/// @return the line
OverlayLine range_line(
    const TextHooks& hooks, const std::vector<Row>& rows, std::size_t first, std::size_t last
) {
    const Row& head = rows[first];
    const Row& tail = rows[last - 1];
    const std::string pattern = show(hooks, words::chord_range);
    OverlayLine line;
    line.name = family_name(head.name, head.number);
    line.keys = head.keys;
    line.gesture = head.gesture;
    line.aside = shared_aside(rows, first, last);
    const std::string first_key = head.keys.empty() ? std::string{} : head.keys.back();
    const std::string last_key = tail.keys.empty() ? std::string{} : tail.keys.back();
    if (!line.keys.empty())
        line.keys.back() = fill(pattern, {{"first", first_key}, {"last", last_key}});
    // The chord's text: the first chord, then the last key after the range mark.
    std::string first_text = head.chord;
    std::string gesture_tail;
    if (!head.gesture.empty() && first_text.size() > head.gesture.size() + 1 &&
        first_text.ends_with(" " + head.gesture)) {
        gesture_tail = first_text.substr(first_text.size() - head.gesture.size() - 1);
        first_text.resize(first_text.size() - head.gesture.size() - 1);
    }
    line.chord = fill(pattern, {{"first", first_text}, {"last", last_key}}) + gesture_tail;
    return line;
}

/// Returns Primary's name on this platform, as a chord's keycaps show it.
///
/// @param inputs the keyboard and the platform
/// @return Ctrl, or Cmd where Primary is the command key
std::string primary_name(const OverlayInputs& inputs) {
    keys::Chord sample;
    sample.modifiers = keys::primary;
    sample.kind = keys::TriggerKind::character;
    sample.character = U'A';
    const std::vector<keys::KeyCap> caps = keys::keycaps(sample, inputs.keyboard, inputs.platform);
    return caps.empty() ? std::string{} : caps.front().text;
}

/// Returns the panel's width at a class, before the battlefield narrows it.
///
/// @param size_class the class
/// @return 240, 420 or 520 points
int32_t preferred_width(kit::SizeClass size_class) {
    if (size_class == kit::SizeClass::large)
        return 520;
    if (size_class == kit::SizeClass::regular)
        return 420;
    return 240;
}

/// The heights the groups are laid out with, at one class.
struct Rhythm {
    int32_t heading{}; ///< a group's heading: its letter keycap
    int32_t line{};    ///< one line: a keycap and the room round it
    int32_t gap{};     ///< between two groups
};

/// Returns the groups' rhythm at a class.
///
/// @param metrics the class's metrics
/// @return the heights
Rhythm rhythm_of(const kit::Metrics& metrics) {
    Rhythm rhythm;
    rhythm.heading = kit::keycap_height + kLineGap;
    rhythm.line = std::max(metrics.regular_line, kit::keycap_height) + kLineGap;
    rhythm.gap = metrics.row_gap;
    return rhythm;
}

/// Returns how tall one group is.
///
/// @param group the group
/// @param rhythm the class's heights
/// @return its height, the gap under it included
int32_t group_height(const OverlayGroup& group, const Rhythm& rhythm) {
    return rhythm.heading + static_cast<int32_t>(group.lines.size()) * rhythm.line + rhythm.gap;
}

/// Where a two-column list splits, and how tall it is.
struct ColumnSplit {
    std::size_t split{}; ///< the first group of the right column; the count for one column
    int32_t height{};    ///< the taller column, in points
};

/// Splits the groups into one column or two of about the same height,
/// keeping their order.
///
/// @param heights each group's height
/// @param two the list has two columns
/// @return the split and the list's height
ColumnSplit split_columns(const std::vector<int32_t>& heights, bool two) {
    int32_t total = 0;
    for (const int32_t height : heights)
        total += height;
    if (!two || heights.size() < 2)
        return {heights.size(), total};
    ColumnSplit best{heights.size(), total};
    int32_t left = 0;
    for (std::size_t split = 1; split < heights.size(); ++split) {
        left += heights[split - 1];
        const int32_t taller = std::max(left, total - left);
        if (taller < best.height)
            best = {split, taller};
    }
    return best;
}

/// Appends a text item.
///
/// @param[in,out] list the display list
/// @param rect where the text sits
/// @param text the words
/// @param font which font
/// @param ink the colour
/// @param clip what the text may touch; empty for the rectangle alone
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

/// Returns the part of a rectangle inside a clip.
///
/// @param rect the rectangle
/// @param clip the clip
/// @return the shared part; empty when they do not meet
kit::Rect clipped_to(const kit::Rect& rect, const kit::Rect& clip) {
    const int32_t left = std::max(rect.x, clip.x);
    const int32_t top = std::max(rect.y, clip.y);
    const int32_t right = std::min(rect.x + rect.width, clip.x + clip.width);
    const int32_t bottom = std::min(rect.y + rect.height, clip.y + clip.height);
    if (right <= left || bottom <= top)
        return {};
    return {left, top, right - left, bottom - top};
}

/// Appends one line of a group: its name, its aside and its chord chip at
/// the right. A part the viewport does not meet is left out, and a part it
/// meets in part is clipped to it.
///
/// @param[in,out] list the display list
/// @param fonts the fonts
/// @param line the line
/// @param rect the line's place
/// @param viewport the part of the list that shows
void add_line(
    kit::DisplayList& list,
    const kit::Fonts& fonts,
    const OverlayLine& line,
    const kit::Rect& rect,
    const kit::Rect& viewport
) {
    if (clipped_to(rect, viewport).width <= 0)
        return;
    kit::ChordChipLook chip;
    chip.keys = line.keys;
    chip.gesture = line.gesture;
    if (chip.keys.empty())
        chip.none_text = line.chord;
    const int32_t chip_room = std::max(rect.width * 3 / 5, int32_t{0});
    const int32_t chip_w = std::min(kit::chord_chip_width(fonts, chip), chip_room);
    const int32_t chip_top = rect.y + (rect.height - kit::keycap_height) / 2;
    const kit::Rect chip_rect{rect.x + rect.width - chip_w, chip_top, chip_w, kit::keycap_height};
    const int32_t name_w = std::max(chip_rect.x - rect.x - kLineGap, int32_t{0});
    const std::string name = kit::cut_to_width(fonts, kit::FontRole::regular, line.name, name_w);
    add_text(
        list,
        {rect.x, rect.y, name_w, rect.height},
        name,
        kit::FontRole::regular,
        kit::colour::text,
        clipped_to({rect.x, rect.y, name_w, rect.height}, viewport)
    );
    if (!line.aside.empty() && name == line.name) {
        const int32_t used = kit::text_width(fonts, kit::FontRole::regular, name) + kLineGap;
        const int32_t aside_w = kit::text_width(fonts, kit::FontRole::small, line.aside);
        if (used + aside_w <= name_w) {
            const kit::Rect aside_rect{rect.x + used, rect.y, aside_w, rect.height};
            add_text(
                list,
                aside_rect,
                line.aside,
                kit::FontRole::small,
                kit::colour::text_tertiary,
                clipped_to(aside_rect, viewport)
            );
        }
    }
    if (chip_w > 0) {
        kit::add_chord_chip(list, chip_rect, chip, kit::no_control, {});
        list.items.back().clip = clipped_to(chip_rect, viewport);
        if (list.items.back().clip.width <= 0)
            list.items.pop_back();
    }
}

/// Appends one column of groups.
///
/// @param[in,out] list the display list
/// @param fonts the fonts
/// @param groups the groups
/// @param begin the column's first group
/// @param end one past its last group
/// @param origin the column's top left, before the scroll
/// @param width the column's width
/// @param rhythm the class's heights
/// @param viewport the part of the list that shows
void add_column(
    kit::DisplayList& list,
    const kit::Fonts& fonts,
    const std::vector<OverlayGroup>& groups,
    std::size_t begin,
    std::size_t end,
    kit::Point origin,
    int32_t width,
    const Rhythm& rhythm,
    const kit::Rect& viewport
) {
    int32_t y = origin.y;
    for (std::size_t index = begin; index < end; ++index) {
        const OverlayGroup& group = groups[index];
        const kit::Rect heading_rect{origin.x, y, width, kit::keycap_height};
        if (clipped_to(heading_rect, viewport).width > 0) {
            int32_t title_x = origin.x;
            if (!group.letter.empty()) {
                kit::KeycapLook cap;
                cap.label = group.letter;
                cap.tint = group.tint;
                const int32_t cap_w = std::min(kit::keycap_width(fonts, cap), width);
                const kit::Rect cap_rect{origin.x, y, cap_w, kit::keycap_height};
                kit::add_keycap(list, cap_rect, cap, kit::no_control, {});
                list.items.back().clip = clipped_to(cap_rect, viewport);
                title_x += cap_w + kLineGap;
            }
            const int32_t title_w = std::max(origin.x + width - title_x, int32_t{0});
            kit::Item heading;
            heading.role = kit::Role::heading;
            heading.rect = {title_x, y, title_w, kit::keycap_height};
            heading.clip = clipped_to(heading.rect, viewport);
            heading.look = kit::HeadingLook{
                kit::cut_to_width(fonts, kit::FontRole::small, group.title, title_w)
            };
            if (heading.clip.width > 0)
                list.items.push_back(std::move(heading));
        }
        int32_t line_y = y + rhythm.heading;
        for (const OverlayLine& line : group.lines) {
            add_line(list, fonts, line, {origin.x, line_y, width, rhythm.line}, viewport);
            line_y += rhythm.line;
        }
        y += group_height(group, rhythm);
    }
}

} // namespace

std::string overlay_title(const OverlayInputs& inputs) {
    const uint32_t active = inputs.contexts.active;
    if ((active & context_bit("placing")) != 0)
        return show(inputs.text, words::keys_while_placing);
    if ((active & context_bit("watch")) != 0 && (active & context_bit("play")) == 0)
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
        // The dispatcher's own test: a context of the action is open and
        // every condition it names holds.
        if ((bound.contexts & inputs.contexts.active) == 0)
            continue;
        if ((bound.when & ~inputs.contexts.conditions) != 0)
            continue;
        const keys::Chord* chord = first_key_chord(bound);
        if (chord == nullptr)
            continue;
        Row row;
        row.name = show(inputs.text, bound.entry->words.name);
        row.chord = keys::display_text(*chord, inputs.keyboard, inputs.platform);
        const std::vector<keys::KeyCap> caps =
            keys::keycaps(*chord, inputs.keyboard, inputs.platform);
        const bool gestured = chord->gesture != keys::Gesture::press;
        for (std::size_t index = 0; index < caps.size(); ++index) {
            if (gestured && index + 1 == caps.size())
                row.gesture = caps[index].text;
            else
                row.keys.push_back(caps[index].text);
        }
        if (bound.entry->kind == acts::Kind::hold || bound.entry->kind == acts::Kind::modifier)
            row.aside = show(inputs.text, words::aside_held);
        if (!family_of(bound.id, row.stem, row.number)) {
            row.stem.clear();
            row.number = 0;
        }
        row.place = key_place(*chord);
        row.modifiers = chord->modifiers;
        row.trigger = chord->kind;
        row.gesture_kind = chord->gesture;
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
        if (area_rows.empty())
            return;
        // A hack's actions come from several areas: keep each area's order
        // by the registry's, which numbers an area's rows from 1.
        std::stable_sort(area_rows.begin(), area_rows.end(), [](const Row& a, const Row& b) {
            return a.order < b.order;
        });
        OverlayGroup group;
        group.tint = tint;
        group.letter = letter.empty() ? std::string{} : show(inputs.text, letter);
        group.title = show(inputs.text, title);
        for (std::size_t index = 0; index < area_rows.size();) {
            std::size_t end = index + 1;
            while (end < area_rows.size() && continues_run(area_rows[end - 1], area_rows[end]))
                ++end;
            if (end - index >= 2) {
                group.lines.push_back(range_line(inputs.text, area_rows, index, end));
            } else {
                OverlayLine line;
                line.name = std::move(area_rows[index].name);
                line.chord = std::move(area_rows[index].chord);
                line.keys = std::move(area_rows[index].keys);
                line.gesture = std::move(area_rows[index].gesture);
                line.aside = std::move(area_rows[index].aside);
                group.lines.push_back(std::move(line));
            }
            index = end;
        }
        groups.push_back(std::move(group));
    };

    for (const AreaSpec& area : kAreas) {
        if (area.id == "game")
            emit_area(kModArea, kit::KeyTint::hacks, words::letter_hacks, words::this_mod);
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
    const Rhythm rhythm = rhythm_of(metrics);
    const int32_t pad = metrics.padding;
    const int32_t margin = metrics.space * 2;
    const int32_t field_right = room.battlefield.x + room.battlefield.width;
    const int32_t field_bottom = room.battlefield.y + room.battlefield.height;

    // The panel: right of the column, below the battlefield's top.
    const int32_t panel_x = std::max(room.battlefield.x, room.column_right) + margin;
    const int32_t panel_y = room.battlefield.y + margin;
    const int32_t room_w = std::max(field_right - margin - panel_x, int32_t{0});
    const int32_t room_h = std::max(field_bottom - margin - panel_y, int32_t{0});
    const int32_t panel_w = std::min(preferred_width(room.size_class), room_w);
    const int32_t inner = std::max(panel_w - 2 * pad, int32_t{0});

    // The words.
    const std::string title = overlay_title(inputs);
    const std::string layout_name =
        inputs.layout_name.empty() ? show(inputs.text, words::oa_default) : inputs.layout_name;
    // One change reads "1 change", as the editor's own line does.
    const std::string aside =
        inputs.changes == 1
            ? fill(show(inputs.text, words::compact_context_one), {{"layout", layout_name}})
            : fill(
                  show(inputs.text, words::overlay_changes),
                  {{"name", layout_name},
                   {"n", std::to_string(std::max(inputs.changes, int32_t{0}))}}
              );
    const std::string on_label = show(inputs.text, words::on_line);
    const std::string footer =
        fill(show(inputs.text, words::overlay_footer), {{"modifier", primary_name(inputs)}});
    const std::string edit = show(inputs.text, words::edit_keys);

    // The header: the title with EDIT KEYS… and the close mark at its
    // right, the layout's line under it, and EDIT KEYS… on a row of its own
    // when the title leaves it no room.
    const int32_t button_h = metrics.button_height;
    const int32_t close_side = button_h;
    kit::ButtonLook edit_look;
    edit_look.caption = edit;
    edit_look.style = kit::ButtonStyle::quiet;
    const int32_t edit_w = std::min(
        kit::text_width(fonts, kit::FontRole::small, edit) + 2 * metrics.chip_padding + kLineGap,
        inner
    );
    const int32_t title_h = std::max(metrics.regular_line, button_h);
    const int32_t title_natural = kit::text_width(fonts, kit::FontRole::regular, title);
    const bool edit_beside = title_natural + kLineGap + edit_w + kLineGap + close_side <= inner;
    const int32_t close_x = pad + std::max(inner - close_side, int32_t{0});
    const int32_t title_room = std::max(
        (edit_beside ? close_x - edit_w - kLineGap : close_x) - pad - kLineGap, int32_t{0}
    );
    int32_t cursor = pad;
    const kit::Rect title_rect{pad, cursor, title_room, title_h};
    const kit::Rect close_rect{close_x, cursor, close_side, close_side};
    const kit::Rect edit_beside_rect{close_x - kLineGap - edit_w, cursor, edit_w, button_h};
    cursor += title_h + metrics.space;
    const kit::Rect aside_rect{pad, cursor, inner, metrics.small_line};
    cursor += metrics.small_line + metrics.space;
    kit::Rect edit_rect = edit_beside_rect;
    if (!edit_beside) {
        edit_rect = {pad, cursor, edit_w, button_h};
        cursor += button_h + metrics.space;
    }
    // The On line: the latches and sticky modifiers that are on, as chips.
    const int32_t chip_h = metrics.chip_height;
    const int32_t on_h = std::max(metrics.small_line, chip_h);
    const int32_t on_top = cursor;
    if (!inputs.on.empty())
        cursor += on_h + metrics.space;
    const int32_t list_top = cursor;

    // The footer, wrapped into at most two lines.
    const kit::Measure measure_small = [&fonts](std::string_view text) {
        return kit::text_width(fonts, kit::FontRole::small, text);
    };
    kit::WrapRules footer_rules;
    footer_rules.most_lines = 2;
    footer_rules.shorten_last = [&fonts, inner](std::string_view rest) {
        return kit::cut_to_width(fonts, kit::FontRole::small, rest, inner);
    };
    const std::vector<std::string> footer_lines =
        kit::wrap(footer, std::max(inner, int32_t{1}), measure_small, footer_rules);
    const int32_t footer_h = static_cast<int32_t>(footer_lines.size()) * metrics.small_line;

    // The groups, in one column at Compact and two at the larger classes
    // when each column keeps room for a name and a chord.
    std::vector<int32_t> heights;
    heights.reserve(groups.size());
    for (const OverlayGroup& group : groups)
        heights.push_back(group_height(group, rhythm));
    const int32_t column_gap = metrics.section_gap;
    const bool two = room.size_class != kit::SizeClass::compact && inner >= 2 * 160 + column_gap;
    const ColumnSplit columns = split_columns(heights, two);
    const int32_t content_h = std::max(columns.height - rhythm.gap, int32_t{0});

    const int32_t fixed_h = list_top + metrics.space + footer_h + pad;
    const int32_t natural_h = fixed_h + content_h;
    const int32_t panel_h = std::min(natural_h, room_h);
    const int32_t viewport_h = std::max(panel_h - fixed_h, int32_t{0});
    const int32_t scroll_max = std::max(content_h - viewport_h, int32_t{0});
    const int32_t clamped = std::clamp(scroll, int32_t{0}, scroll_max);

    OverlayLayout laid;
    laid.panel = {panel_x, panel_y, panel_w, panel_h};
    laid.radius = metrics.card_radius;
    laid.scroll = clamped;
    laid.scroll_max = scroll_max;
    kit::DisplayList& list = laid.list;

    // The list first, so a press on a button over it reaches the button.
    const int32_t bar_w = scroll_max > 0 ? metrics.scroll_thumb_width : 0;
    const kit::Rect viewport{pad, list_top, inner, viewport_h};
    const int32_t list_w = std::max(inner - (bar_w > 0 ? bar_w + kLineGap : 0), int32_t{0});
    const int32_t column_w = two ? std::max((list_w - column_gap) / 2, int32_t{0}) : list_w;
    add_column(
        list, fonts, groups, 0, columns.split, {pad, list_top - clamped}, column_w, rhythm, viewport
    );
    if (columns.split < groups.size())
        add_column(
            list,
            fonts,
            groups,
            columns.split,
            groups.size(),
            {pad + column_w + column_gap, list_top - clamped},
            column_w,
            rhythm,
            viewport
        );
    if (bar_w > 0 && viewport_h > 0) {
        const int32_t span = scroll_max + viewport_h;
        const int32_t thumb_h = std::min(
            std::max(viewport_h * viewport_h / span, metrics.least_thumb_height), viewport_h
        );
        const int32_t travel = std::max(viewport_h - thumb_h, int32_t{0});
        kit::Item bar;
        bar.role = kit::Role::scroll_bar;
        bar.rect = {pad + inner - bar_w, list_top, bar_w, viewport_h};
        kit::ScrollBarLook look;
        look.thumb = {bar.rect.x, list_top + clamped * travel / scroll_max, bar_w, thumb_h};
        bar.look = look;
        kit::Control bar_control;
        bar_control.id = id_overlay_bar;
        bar_control.rect = bar.rect;
        bar_control.name = std::string(name_keys_overlay_bar);
        bar_control.kind = kit::ControlKind::scroll_bar;
        bar_control.focusable = false;
        list.items.push_back(std::move(bar));
        list.controls.push_back(std::move(bar_control));
    }

    // The header.
    add_text(
        list,
        title_rect,
        kit::cut_to_width(fonts, kit::FontRole::regular, title, title_rect.width),
        kit::FontRole::regular,
        kit::colour::text,
        {}
    );
    kit::ButtonLook close_look;
    close_look.style = kit::ButtonStyle::quiet;
    close_look.mark = kit::ButtonMark::close;
    kit::add_button(
        list, close_rect, close_look, id_overlay_close, std::string(name_keys_overlay_close)
    );
    kit::add_button(
        list, edit_rect, edit_look, id_overlay_edit, std::string(name_keys_overlay_edit)
    );
    add_text(
        list,
        aside_rect,
        kit::cut_to_width(fonts, kit::FontRole::small, aside, inner),
        kit::FontRole::small,
        kit::colour::text_secondary,
        {}
    );
    std::string on_text;
    if (!inputs.on.empty()) {
        const int32_t label_w = kit::text_width(fonts, kit::FontRole::small, on_label);
        add_text(
            list,
            {pad, on_top, std::min(label_w, inner), on_h},
            on_label,
            kit::FontRole::small,
            kit::colour::text_tertiary,
            {}
        );
        on_text = on_label;
        int32_t chip_x = pad + label_w + metrics.chip_gap;
        const int32_t chip_y = on_top + (on_h - chip_h) / 2;
        for (const OverlayChip& on : inputs.on) {
            kit::ChipLook chip;
            chip.text = on.text;
            chip.state = on.filled ? kit::ChipState::playing : kit::ChipState::installed;
            const int32_t chip_w = kit::chip_width(fonts, chip);
            on_text.push_back(' ');
            on_text += on.text;
            if (chip_x + chip_w > pad + inner)
                continue;
            kit::add_chip(list, {chip_x, chip_y, chip_w, chip_h}, chip, kit::no_control, {});
            chip_x += chip_w + metrics.chip_gap;
        }
    }
    int32_t footer_y = panel_h - pad - footer_h;
    for (const std::string& footer_line : footer_lines) {
        add_text(
            list,
            {pad, footer_y, inner, metrics.small_line},
            footer_line,
            kit::FontRole::small,
            kit::colour::text_tertiary,
            {}
        );
        footer_y += metrics.small_line;
    }

    // The list's control: every word the panel shows, one line each, so
    // automation reads what a player reads.
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
            if (!line.aside.empty()) {
                listed.push_back(' ');
                listed += line.aside;
            }
            listed.push_back(' ');
            listed += line.chord;
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
    // Tried after the buttons and the bar, so a press on them reaches them.
    list.controls.push_back(std::move(list_control));
    list.tab_order = {id_overlay_edit, id_overlay_close};
    return laid;
}

void draw_overlay(const kit::Canvas& canvas, const OverlayLayout& laid) {
    const kit::Rect whole{0, 0, laid.panel.width, laid.panel.height};
    if (whole.width <= 0 || whole.height <= 0)
        return;
    const kit::Colour face = kit::hud_colour::panel;
    const uint32_t opacity = (static_cast<uint32_t>(face.a) * 256u + 127u) / 255u;
    kit::blend_corner_rect(canvas, whole, laid.radius, face, opacity);
    kit::outline_corner_rect(canvas, whole, laid.radius, kit::colour::strong_edge);
    kit::paint(canvas, laid.list);
}

} // namespace oa::ui::key_editor

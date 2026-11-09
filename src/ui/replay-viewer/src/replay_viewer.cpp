// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The playback bar's layout, hit testing and drawing.

#include "oa/ui/replay_viewer/interface_words.hpp"
#include "oa/ui/replay_viewer/replay_viewer.hpp"

#include "oa/data/languages/interface_text.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/kit/theme.hpp"

#include <algorithm>
#include <cmath>
#include <string>
#include <utility>

namespace oa::ui::replay_viewer {
namespace {

namespace kit = oa::ui::kit;

/// Looks a word up and copies it, so the catalogue may move afterwards.
///
/// @param english the English word
/// @return the text to show
std::string shown(std::string_view english) {
    return std::string(oa::data::languages::interface_text(english));
}

/// Fills the places of a pattern that has already been looked up.
///
/// @param pattern the looked-up pattern
/// @param places each place's name, without braces, and the text that replaces it
/// @return the filled text
std::string fill(
    std::string_view pattern,
    std::initializer_list<std::pair<std::string_view, std::string_view>> places
) {
    std::string out;
    out.reserve(pattern.size());
    for (std::size_t at = 0; at < pattern.size();) {
        if (pattern[at] != '{') {
            out.push_back(pattern[at]);
            ++at;
            continue;
        }
        const std::size_t end = pattern.find('}', at + 1);
        if (end == std::string_view::npos) {
            out.push_back(pattern[at]);
            ++at;
            continue;
        }
        const std::string_view name = pattern.substr(at + 1, end - at - 1);
        bool found = false;
        for (const auto& place : places) {
            if (place.first != name)
                continue;
            out.append(place.second);
            found = true;
            break;
        }
        if (!found)
            out.append(pattern.substr(at, end - at + 1));
        at = end + 1;
    }
    return out;
}

/// Tells whether a rectangle holds any point.
///
/// @param rect the rectangle
/// @return true when it has a width and a height
bool live(const kit::Rect& rect) noexcept {
    return rect.width > 0 && rect.height > 0;
}

/// The column of a tick along a track. A tick past the length sits at the end.
///
/// @param track the track
/// @param tick the game tick
/// @param length the recording's length
/// @return the column
int32_t column_of(const kit::Rect& track, uint32_t tick, uint32_t length) noexcept {
    if (track.width <= 1 || length == 0)
        return track.x;
    const uint32_t clamped = std::min(tick, length);
    const auto span = static_cast<uint64_t>(track.width - 1);
    const auto at = static_cast<uint64_t>(clamped) * span / length;
    return track.x + static_cast<int32_t>(at);
}

/// Adds one control.
///
/// @param[in,out] list the display list
/// @param id the control's number
/// @param rect where it sits
/// @param name its automation name, after "replay-viewer."
/// @param kind what it is
/// @param text its caption
/// @param checked whether it is on
/// @param enabled a press reaches it
void add_control(
    kit::DisplayList& list,
    Control id,
    const kit::Rect& rect,
    std::string_view name,
    kit::ControlKind kind,
    std::string text,
    bool checked,
    bool enabled = true
) {
    if (!live(rect))
        return;
    kit::Control control;
    control.id = static_cast<kit::ControlId>(id);
    control.rect = rect;
    control.name = std::string("replay-viewer.") + std::string(name);
    control.kind = kind;
    control.label = text;
    control.text = std::move(text);
    control.checked = checked;
    control.enabled = enabled;
    control.focusable =
        id != Control::timeline && id != Control::keys_card && id != Control::end_card;
    const bool focusable = control.focusable;
    const kit::ControlId control_id = control.id;
    list.controls.push_back(std::move(control));
    if (focusable)
        list.tab_order.push_back(control_id);
}

/// Draws a diamond of stacked rows, for a commander's death.
///
/// @param canvas where it is drawn
/// @param box the diamond's bounds
/// @param ink its colour
void fill_diamond(const kit::Canvas& canvas, const kit::Rect& box, kit::Colour ink) {
    if (box.width < 2 || box.height < 2)
        return;
    const int32_t mid_row = box.height / 2;
    const int32_t mid_column = box.width / 2;
    for (int32_t row = 0; row < box.height; ++row) {
        const int32_t from_mid = row < mid_row ? mid_row - row : row - mid_row;
        const int32_t half = mid_column - (mid_column * from_mid) / std::max(mid_row, int32_t{1});
        if (half <= 0)
            continue;
        kit::fill_rect(canvas, {box.x + mid_column - half, box.y + row, half * 2, 1}, ink);
    }
}

/// Draws a small flag, for the end of the recording.
///
/// @param canvas where it is drawn
/// @param at the flag's pole, at the track
/// @param ink its colour
void fill_flag(const kit::Canvas& canvas, kit::Point at, kit::Colour ink) {
    kit::fill_rect(canvas, {at.x, at.y - 12, 1, 12}, ink);
    kit::fill_rect(canvas, {at.x + 1, at.y - 12, 7, 5}, ink);
}

/// A mark drawn on a button, in place of a word.
enum class Glyph : uint8_t {
    back,    ///< back ten seconds
    play,    ///< play
    pause,   ///< pause
    forward, ///< forward ten seconds
    chat,    ///< recorded chat
    keys,    ///< the shortcut card
    leave,   ///< the in-game menu
};

/// Fills a triangle one row at a time. The point is the right or the left end.
///
/// @param canvas where it is drawn
/// @param left the triangle's left column
/// @param top its top row
/// @param width its width
/// @param height its height
/// @param point_right the point is on the right
/// @param ink its colour
void fill_triangle(
    const kit::Canvas& canvas,
    int32_t left,
    int32_t top,
    int32_t width,
    int32_t height,
    bool point_right,
    kit::Colour ink
) {
    if (width <= 0 || height <= 0)
        return;
    const int32_t mid = std::max(height / 2, int32_t{1});
    for (int32_t row = 0; row < height; ++row) {
        const int32_t from_mid = row < mid ? mid - row : row - mid;
        const int32_t span = std::max(width - (width * from_mid) / mid, int32_t{1});
        const int32_t column = point_right ? left : left + width - span;
        kit::fill_rect(canvas, {column, top + row, span, 1}, ink);
    }
}

/// Draws a turning arrow: a ring with a gap, and an arrow head in the gap.
/// Forward is the mirror of back. Neither is a bar with a triangle.
///
/// @param canvas where it is drawn
/// @param left the mark's left column
/// @param top its top row
/// @param forward the arrow turns the other way
/// @param ink its colour
void fill_turning_arrow(
    const kit::Canvas& canvas, int32_t left, int32_t top, bool forward, kit::Colour ink
) {
    constexpr int32_t kBox = 12;
    for (int32_t row = 0; row < kBox; ++row) {
        for (int32_t column = 0; column < kBox; ++column) {
            const int32_t x = forward ? (kBox - 1 - column) : column;
            const int32_t dx = x - 5;
            const int32_t dy = row - 6;
            const int32_t dist2 = dx * dx + dy * dy;
            const bool ring = dist2 >= 10 && dist2 <= 25;
            const bool gap = x <= 3 && row <= 4;
            if (ring && !gap)
                kit::fill_rect(canvas, {left + column, top + row, 1, 1}, ink);
        }
    }
    const int32_t tip = forward ? left + 8 : left + 0;
    fill_triangle(canvas, tip, top, 4, 5, !forward, ink);
}

/// Draws one transport or tool mark, centred, and clear of the button's edge.
///
/// @param canvas where it is drawn
/// @param rect the button
/// @param glyph which mark
/// @param ink its colour
void draw_glyph(const kit::Canvas& canvas, const kit::Rect& rect, Glyph glyph, kit::Colour ink) {
    constexpr int32_t kGlyph = 12;
    const int32_t left = rect.x + (rect.width - kGlyph) / 2;
    const int32_t top = rect.y + (rect.height - kGlyph) / 2;
    switch (glyph) {
    case Glyph::back:
        fill_turning_arrow(canvas, left, top, false, ink);
        break;
    case Glyph::play:
        fill_triangle(canvas, left + 1, top, kGlyph - 2, kGlyph, true, ink);
        break;
    case Glyph::pause:
        kit::fill_rect(canvas, {left + 2, top + 1, 3, kGlyph - 2}, ink);
        kit::fill_rect(canvas, {left + 7, top + 1, 3, kGlyph - 2}, ink);
        break;
    case Glyph::forward:
        fill_turning_arrow(canvas, left, top, true, ink);
        break;
    case Glyph::chat:
        kit::fill_rect(canvas, {left + 1, top + 1, 10, 7}, ink);
        kit::fill_rect(canvas, {left + 3, top + 8, 3, 2}, ink);
        kit::fill_rect(canvas, {left + 2, top + 10, 2, 1}, ink);
        break;
    case Glyph::keys:
        for (int32_t row = 0; row < 2; ++row) {
            for (int32_t column = 0; column < 2; ++column)
                kit::fill_rect(canvas, {left + 1 + column * 6, top + 1 + row * 6, 4, 4}, ink);
        }
        break;
    case Glyph::leave:
        kit::fill_rect(canvas, {left + 1, top + 1, 7, 1}, ink);
        kit::fill_rect(canvas, {left + 1, top + 1, 1, kGlyph - 2}, ink);
        kit::fill_rect(canvas, {left + 7, top + 1, 1, kGlyph - 2}, ink);
        kit::fill_rect(canvas, {left + 1, top + kGlyph - 2, 7, 1}, ink);
        kit::fill_rect(canvas, {left + 8, top + 5, 3, 2}, ink);
        break;
    }
}

/// Draws a button's face: the whole rectangle, with a small corner radius.
///
/// @param canvas where it is drawn
/// @param rect the button
/// @param lit the button is on
/// @param enabled a press reaches it
void draw_face(const kit::Canvas& canvas, const kit::Rect& rect, bool lit, bool enabled) {
    kit::Colour fill = lit ? kit::hud_colour::lit : kit::hud_colour::panel;
    // Opaque, so the whole face is the fill even where the bar already painted.
    fill.a = 255;
    if (!enabled)
        fill.a = static_cast<uint8_t>(255U * 2U / 5U);
    kit::fill_corner_rect(canvas, rect, 4, fill);
    kit::outline_corner_rect(canvas, rect, 4, kit::hud_colour::edge);
}

/// Draws one button and its words.
///
/// @param canvas where it is drawn
/// @param rect the button
/// @param text its caption, already looked up
/// @param lit the button is on
/// @param enabled a press reaches it
void draw_button(
    const kit::Canvas& canvas,
    const kit::Rect& rect,
    std::string_view text,
    bool lit,
    bool enabled = true
) {
    const kit::Colour ink = !enabled ? kit::hud_colour::quiet
                            : lit    ? kit::hud_colour::ink
                                     : kit::hud_colour::label;
    draw_face(canvas, rect, lit, enabled);
    kit::box_text(canvas, kit::FontRole::small, text, rect, kit::Align::centre, ink);
}

/// Draws one button and its mark.
///
/// @param canvas where it is drawn
/// @param rect the button
/// @param glyph which mark
/// @param lit the button is on
void draw_icon_button(const kit::Canvas& canvas, const kit::Rect& rect, Glyph glyph, bool lit) {
    const kit::Colour ink = lit ? kit::hud_colour::ink : kit::hud_colour::label;
    draw_face(canvas, rect, lit, true);
    draw_glyph(canvas, rect, glyph, ink);
}

/// The English pattern a marker's hover uses.
///
/// @param marker the marker
/// @return the pattern
std::string_view marker_pattern(const Marker& marker) noexcept {
    switch (marker.kind) {
    case MarkerKind::commander_death:
        return words::commander_destroyed;
    case MarkerKind::dropped:
        return words::dropped;
    case MarkerKind::left:
        return words::left;
    case MarkerKind::defeated:
        return words::defeated;
    case MarkerKind::alliance:
        return marker.allied ? words::allied : words::broke;
    case MarkerKind::chat:
        return words::chat_line;
    case MarkerKind::pause:
        return words::paused;
    case MarkerKind::end:
        return words::end_marker;
    }
    return words::end_marker;
}

/// Draws the thin track: watched, ready, the rest, pauses and the heat.
///
/// @param canvas where it is drawn
/// @param layout the layout
/// @param model what the bar shows
void draw_track(const kit::Canvas& canvas, const Layout& layout, const Model& model) {
    const kit::Colour rest{0x14, 0x16, 0x12, 255};
    const kit::Colour ready_ink{182, 224, 90, 210};
    const kit::Colour watched = kit::hud_colour::lit;
    kit::fill_corner_rect(canvas, layout.track, 2, rest);
    const auto span_of = [&](uint32_t begin, uint32_t end) {
        const int32_t left = column_of(layout.track, begin, model.length);
        const int32_t right = column_of(layout.track, end, model.length);
        return kit::Rect{
            left, layout.track.y, std::max(right - left, int32_t{1}), layout.track.height
        };
    };
    for (const ReadySpan& span : model.ready) {
        if (span.end < span.begin)
            continue;
        kit::fill_rect(canvas, span_of(span.begin, span.end), ready_ink);
    }
    if (model.length > 0 && model.tick > 0)
        kit::fill_rect(canvas, span_of(0, model.tick), watched);
    for (const Marker& marker : model.markers) {
        if (marker.kind != MarkerKind::pause)
            continue;
        const uint32_t end = std::max(marker.end_tick, marker.tick);
        const kit::Rect gap = span_of(marker.tick, end);
        for (int32_t column = gap.x; column < gap.x + gap.width; column += 3)
            kit::fill_rect(canvas, {column, gap.y, 1, gap.height}, rest);
    }
    if (!model.heat.empty() && live(layout.heat)) {
        uint32_t peak = 1;
        for (const uint32_t sample : model.heat)
            peak = std::max(peak, sample);
        const int32_t bucket =
            std::max(layout.heat.width / static_cast<int32_t>(model.heat.size()), int32_t{1});
        for (std::size_t index = 0; index < model.heat.size(); ++index) {
            const uint32_t sample = model.heat[index];
            const uint8_t hot = static_cast<uint8_t>(40 + (sample * 180) / peak);
            const kit::Colour ink{hot, static_cast<uint8_t>(48 + hot / 5), 32, 230};
            const int32_t left = layout.heat.x + static_cast<int32_t>(index) * layout.heat.width /
                                                     static_cast<int32_t>(model.heat.size());
            kit::fill_rect(canvas, {left, layout.heat.y, bucket, layout.heat.height}, ink);
        }
    }
}

/// Draws the marks over the track.
///
/// @param canvas where it is drawn
/// @param layout the layout
/// @param model what the bar shows
void draw_markers(const kit::Canvas& canvas, const Layout& layout, const Model& model) {
    for (const Marker& marker : model.markers) {
        const int32_t column = column_of(layout.track, marker.tick, model.length);
        const int32_t mid = layout.track.y + layout.track.height / 2;
        switch (marker.kind) {
        case MarkerKind::commander_death:
            fill_diamond(canvas, {column - 5, mid - 5, 10, 10}, marker.colour);
            break;
        case MarkerKind::dropped:
        case MarkerKind::left:
        case MarkerKind::defeated:
            kit::fill_rect(canvas, {column - 3, mid - 3, 6, 6}, marker.colour);
            break;
        case MarkerKind::alliance:
            kit::fill_rect(
                canvas, {column - 1, layout.track.y - 3, 2, layout.track.height + 6}, marker.colour
            );
            break;
        case MarkerKind::chat:
            kit::fill_rect(
                canvas, {column, layout.track.y, 2, layout.track.height}, kit::hud_colour::quiet
            );
            break;
        case MarkerKind::pause:
            break;
        case MarkerKind::end:
            fill_flag(canvas, {column, layout.track.y}, kit::hud_colour::label);
            break;
        }
    }
    kit::fill_round_rect(canvas, layout.knob, layout.knob.height / 2, kit::hud_colour::label);
    kit::outline_round_rect(canvas, layout.knob, layout.knob.height / 2, kit::hud_colour::ink);
}

/// Draws a card's panel.
///
/// @param canvas where it is drawn
/// @param rect the card
void draw_card(const kit::Canvas& canvas, const kit::Rect& rect) {
    kit::fill_corner_rect(canvas, rect, 6, card_fill);
    kit::outline_corner_rect(canvas, rect, 6, kit::hud_colour::edge);
}

/// Draws the speed menu.
///
/// @param canvas where it is drawn
/// @param layout the layout
/// @param model what the bar shows
void draw_speed_menu(const kit::Canvas& canvas, const Layout& layout, const Model& model) {
    if (!live(layout.speed_menu))
        return;
    draw_card(canvas, layout.speed_menu);
    for (std::size_t index = 0; index < layout.speed_items.size() && index < speed_count; ++index) {
        const bool chosen = model.asked_milli == speed_milli[index];
        draw_button(
            canvas, layout.speed_items[index], shown(speed_word(speed_milli[index])), chosen, true
        );
    }
}

/// The shortcut card's actions, used when the app has not filled the rows.
constexpr std::string_view kKeyActions[] = {
    "Play or pause",
    "Back 10 seconds",
    "Forward 10 seconds",
    "Back a minute",
    "Forward a minute",
    "Previous marker",
    "Next marker",
    "Go to the start",
    "Go to the end",
    "Pin the bar",
    "Shortcut card",
};

/// Draws the shortcut card. Each row's keys are the ones the app resolved.
///
/// @param canvas where it is drawn
/// @param layout the layout
/// @param model what the bar shows
void draw_keys(const kit::Canvas& canvas, const Layout& layout, const Model& model) {
    if (!live(layout.keys_card))
        return;
    draw_card(canvas, layout.keys_card);
    const std::size_t count =
        model.key_chords.empty() ? std::size(kKeyActions) : model.key_chords.size();
    const int32_t row_h =
        std::max((layout.keys_card.height - 16) / static_cast<int32_t>(count), int32_t{12});
    for (std::size_t index = 0; index < count; ++index) {
        const int32_t top = layout.keys_card.y + 8 + static_cast<int32_t>(index) * row_h;
        const kit::Rect name{layout.keys_card.x + 10, top, layout.keys_card.width / 2, row_h};
        const kit::Rect chord{
            name.x + name.width, top, layout.keys_card.width - name.width - 20, row_h
        };
        const std::string action = index < model.key_chords.size()
                                       ? model.key_chords[index].action
                                       : std::string(kKeyActions[index]);
        const std::string keys =
            index < model.key_chords.size() ? model.key_chords[index].chord : std::string{};
        kit::box_text(
            canvas,
            kit::FontRole::small,
            shown(action),
            name,
            kit::Align::left,
            kit::hud_colour::label
        );
        kit::box_text(
            canvas, kit::FontRole::small, keys, chord, kit::Align::right, kit::hud_colour::quiet
        );
    }
}

/// Draws the time under the pointer, and a marker's line when the point names one.
///
/// @param canvas where it is drawn
/// @param layout the layout
/// @param model what the bar shows
void draw_hover(const kit::Canvas& canvas, const Layout& layout, const Model& model) {
    if (!model.pointer_over || !layout.shown)
        return;
    const kit::Point at{model.pointer_x, model.pointer_y};
    std::string line;
    const int found = marker_index(layout, at);
    if (found >= 0 && static_cast<std::size_t>(found) < model.markers.size())
        line = marker_caption(model.markers[static_cast<std::size_t>(found)]);
    else if (model.catch_up_percent < 0 && kit::contains(layout.timeline, at))
        line = time_text(tick_at(layout.timeline, at.x, model.length), model.length);
    if (line.empty())
        return;
    const int32_t tip_width = std::min(std::max(layout.bar.width / 3, int32_t{120}), int32_t{360});
    int32_t left = at.x - tip_width / 2;
    if (left < layout.bar.x)
        left = layout.bar.x;
    if (left + tip_width > layout.bar.x + layout.bar.width)
        left = std::max(layout.bar.x + layout.bar.width - tip_width, layout.bar.x);
    const kit::Rect tip{left, std::max(at.y - 22, int32_t{0}), tip_width, 18};
    kit::fill_corner_rect(canvas, tip, 3, card_fill);
    kit::box_text(
        canvas, kit::FontRole::small, line, tip, kit::Align::centre, kit::hud_colour::label
    );
}

/// Draws the end card.
///
/// @param canvas where it is drawn
/// @param layout the layout
/// @param model what the bar shows
void draw_end(const kit::Canvas& canvas, const Layout& layout, const Model& model) {
    if (!live(layout.end))
        return;
    draw_card(canvas, layout.end);
    std::string title = shown(words::undecided);
    if (!model.winner.empty())
        title = fill(shown(words::winner), {{"name", model.winner}});
    else if (model.no_result)
        title = shown(words::no_result);
    kit::box_text(
        canvas,
        kit::FontRole::regular,
        title,
        {layout.end.x + 16, layout.end.y + 12, layout.end.width - 32, 24},
        kit::Align::left,
        kit::hud_colour::label
    );
    kit::box_text(
        canvas,
        kit::FontRole::small,
        clock_text(model.length),
        {layout.end.x + 16, layout.end.y + 36, layout.end.width - 32, 16},
        kit::Align::left,
        kit::hud_colour::quiet
    );
    int32_t row = layout.end.y + 58;
    const int32_t row_h = 16;
    kit::box_text(
        canvas,
        kit::FontRole::small,
        shown(words::kills),
        {layout.end.x + layout.end.width - 120, row, 50, row_h},
        kit::Align::right,
        kit::hud_colour::quiet
    );
    kit::box_text(
        canvas,
        kit::FontRole::small,
        shown(words::losses),
        {layout.end.x + layout.end.width - 64, row, 48, row_h},
        kit::Align::right,
        kit::hud_colour::quiet
    );
    row += row_h;
    int32_t notes = 0;
    if (model.damaged)
        notes += 18;
    if (!model.can_export)
        notes += 18;
    for (const PlayerRow& player : model.players) {
        if (player.watcher)
            continue;
        if (row + row_h > layout.watch_again.y - 8 - notes)
            break;
        kit::fill_rect(canvas, {layout.end.x + 16, row + 4, 8, 8}, player.colour);
        kit::box_text(
            canvas,
            kit::FontRole::small,
            player.name,
            {layout.end.x + 30, row, layout.end.width - 160, row_h},
            kit::Align::left,
            kit::hud_colour::label
        );
        kit::box_text(
            canvas,
            kit::FontRole::small,
            std::to_string(player.kills),
            {layout.end.x + layout.end.width - 120, row, 50, row_h},
            kit::Align::right,
            kit::hud_colour::label
        );
        kit::box_text(
            canvas,
            kit::FontRole::small,
            std::to_string(player.losses),
            {layout.end.x + layout.end.width - 64, row, 48, row_h},
            kit::Align::right,
            kit::hud_colour::label
        );
        row += row_h;
    }
    int32_t note_y = layout.watch_again.y;
    if (!model.can_export) {
        note_y -= 18;
        kit::box_text(
            canvas,
            kit::FontRole::small,
            shown(words::export_outside),
            {layout.end.x + 16, note_y, layout.end.width - 32, 16},
            kit::Align::left,
            kit::hud_colour::quiet
        );
    }
    if (model.damaged) {
        note_y -= 18;
        kit::box_text(
            canvas,
            kit::FontRole::small,
            damaged_caption(model.damage_tick),
            {layout.end.x + 16, note_y, layout.end.width - 32, 16},
            kit::Align::left,
            kit::hud_colour::danger_label
        );
    }
    draw_button(canvas, layout.watch_again, shown(words::watch_again), false);
    draw_button(canvas, layout.library, shown(words::back_to_library), false);
    draw_button(canvas, layout.export_button, shown("Export…"), false, model.can_export);
}

constexpr int32_t kListMargin = 8;
constexpr int32_t kListHeader = 22;
constexpr int32_t kListRow = 36;
constexpr int32_t kChip = 16;
constexpr int32_t kChipGap = 4;

/// Places the player list at the top right.
///
/// @param[in,out] layout the layout
/// @param model what the bar shows
/// @param width the window's width
/// @param compact the window is under 700 points tall
/// @param left_limit the list's left edge, when the end card needs the space
///     before it; 0 leaves the list at its own width
void place_watch(
    Layout& layout, const Model& model, int32_t width, bool compact, int32_t left_limit = 0
) {
    layout.list_folded = model.list_folded;
    layout.watch_indices.clear();
    layout.player_rows.clear();
    layout.player_chips.clear();
    if (model.watch.empty())
        return;
    const int32_t count = static_cast<int32_t>(model.watch.size());
    // The 640×480 battlefield is 512 points wide. A 240-point list covers
    // nearly half of it, so that width takes a narrower list. Beside an end
    // card the list keeps at least that narrower width, or it is not shown.
    int32_t list_w = width <= 512 ? 168 : (compact ? 240 : 280);
    int32_t x = std::max(width - kListMargin - list_w, kListMargin);
    if (left_limit > x) {
        x = left_limit;
        list_w = width - kListMargin - x;
    }
    if (list_w < 168) {
        layout.players = {};
        return;
    }
    if (model.list_folded) {
        const int32_t stride = kChip + kChipGap;
        const int32_t per = std::max((list_w - 16) / stride, int32_t{1});
        const int32_t chip_rows = (count + per - 1) / per;
        const int32_t list_h = kListHeader + chip_rows * stride + 8;
        layout.players = {x, kListMargin, list_w, list_h};
        layout.list_scroll = 0;
        for (int32_t index = 0; index < count; ++index) {
            const int32_t column = index % per;
            const int32_t chip_row = index / per;
            layout.player_chips.push_back(
                {layout.players.x + 8 + column * stride,
                 layout.players.y + kListHeader + chip_row * stride,
                 kChip,
                 kChip}
            );
            layout.watch_indices.push_back(index);
        }
        return;
    }
    const int32_t visible = std::min(count, watch_visible_rows);
    const int32_t max_scroll = std::max(count - visible, int32_t{0});
    int32_t scroll = model.list_scroll;
    if (scroll < 0)
        scroll = 0;
    if (scroll > max_scroll)
        scroll = max_scroll;
    layout.list_scroll = scroll;
    const int32_t list_h = kListHeader + visible * kListRow + 8;
    layout.players = {x, kListMargin, list_w, list_h};
    for (int32_t row = 0; row < visible; ++row) {
        layout.player_rows.push_back(
            {layout.players.x + 4,
             layout.players.y + kListHeader + row * kListRow,
             layout.players.width - 12,
             kListRow - 2}
        );
        layout.watch_indices.push_back(scroll + row);
    }
}

/// Places one menu above a button.
///
/// @param[out] menu the menu's rectangle
/// @param[out] items one rectangle per row
/// @param button the button it opens from
/// @param rows how many rows
/// @param width the menu's width
void place_menu(
    kit::Rect& menu,
    std::vector<kit::Rect>& items,
    const kit::Rect& button,
    int32_t rows,
    int32_t width,
    int32_t window_width
) {
    if (rows <= 0)
        return;
    constexpr int32_t kItem = 22;
    const int32_t menu_h = kItem * rows + 4;
    int32_t menu_y = button.y - menu_h - 4;
    if (menu_y < 4)
        menu_y = 4;
    int32_t menu_x = button.x + button.width - width;
    if (menu_x < 4)
        menu_x = 4;
    if (menu_x + width > window_width - 4)
        menu_x = std::max(window_width - 4 - width, int32_t{4});
    menu = {menu_x, menu_y, width, menu_h};
    items.reserve(static_cast<std::size_t>(rows));
    for (int32_t index = 0; index < rows; ++index) {
        items.push_back(
            {menu.x + 4,
             menu.y + 2 + index * kItem,
             std::max(menu.width - 8, int32_t{1}),
             kItem - 2}
        );
    }
}

/// Draws the mark that opens a menu, at the button's right.
///
/// @param canvas where it is drawn
/// @param rect the button
/// @param ink its colour
void draw_menu_mark(const kit::Canvas& canvas, const kit::Rect& rect, kit::Colour ink) {
    const int32_t cx = rect.x + rect.width - 8;
    const int32_t cy = rect.y + rect.height / 2;
    kit::fill_rect(canvas, {cx - 3, cy - 1, 7, 1}, ink);
    kit::fill_rect(canvas, {cx - 2, cy, 5, 1}, ink);
    kit::fill_rect(canvas, {cx - 1, cy + 1, 3, 1}, ink);
    kit::fill_rect(canvas, {cx, cy + 2, 1, 1}, ink);
}

/// Draws a button whose caption leaves room for a menu mark.
///
/// @param canvas where it is drawn
/// @param rect the button
/// @param text its caption
/// @param lit the button is on
void draw_menu_button(
    const kit::Canvas& canvas, const kit::Rect& rect, std::string_view text, bool lit
) {
    const kit::Colour ink = lit ? kit::hud_colour::ink : kit::hud_colour::label;
    draw_face(canvas, rect, lit, true);
    const int32_t room = std::max(rect.width - menu_mark_points, int32_t{1});
    const kit::Rect words{rect.x, rect.y, room, rect.height};
    // The control's name keeps the full caption. What is drawn is shortened
    // when the slot cannot hold it.
    const kit::Measure measure = [&canvas](std::string_view line) {
        return kit::measure_text(canvas, kit::FontRole::small, line);
    };
    const std::string fitted = kit::fit(text, room, measure);
    kit::box_text(canvas, kit::FontRole::small, fitted, words, kit::Align::centre, ink);
    draw_menu_mark(canvas, rect, ink);
}

/// Draws a colour chip, or a ring for every player.
///
/// @param canvas where it is drawn
/// @param box the chip
/// @param ink its colour
/// @param ring a ring, for every player
void draw_swatch(const kit::Canvas& canvas, const kit::Rect& box, kit::Colour ink, bool ring) {
    if (ring) {
        kit::outline_corner_rect(canvas, box, box.width / 2, ink);
        return;
    }
    kit::fill_corner_rect(canvas, box, 2, ink);
}

/// Draws the mark on the row whose view is on show.
///
/// @param canvas where it is drawn
/// @param x the mark's left
/// @param y the mark's top
void draw_eye(const kit::Canvas& canvas, int32_t x, int32_t y) {
    const kit::Colour sclera = kit::hud_colour::label;
    const kit::Colour iris = kit::hud_colour::ink;
    const kit::Colour pupil = kit::hud_colour::panel;
    // An almond, about 12 by 6, that fits the row's right edge.
    kit::fill_rect(canvas, {x + 3, y, 6, 1}, sclera);
    kit::fill_rect(canvas, {x + 1, y + 1, 10, 1}, sclera);
    kit::fill_rect(canvas, {x, y + 2, 12, 2}, sclera);
    kit::fill_rect(canvas, {x + 1, y + 4, 10, 1}, sclera);
    kit::fill_rect(canvas, {x + 3, y + 5, 6, 1}, sclera);
    kit::fill_rect(canvas, {x + 4, y + 2, 4, 2}, iris);
    kit::fill_rect(canvas, {x + 5, y + 2, 2, 2}, pupil);
}

/// Draws the player list.
///
/// @param canvas where it is drawn
/// @param layout the layout
/// @param model what the bar shows
void draw_players(const kit::Canvas& canvas, const Layout& layout, const Model& model) {
    if (!live(layout.players) || model.watch.empty())
        return;
    kit::fill_corner_rect(canvas, layout.players, 4, card_fill);
    kit::outline_corner_rect(canvas, layout.players, 4, kit::hud_colour::edge);
    kit::box_text(
        canvas,
        kit::FontRole::small,
        shown(words::watching),
        {layout.players.x + 8, layout.players.y + 2, layout.players.width - 16, kListHeader - 2},
        kit::Align::left,
        kit::hud_colour::quiet
    );
    if (layout.list_folded) {
        for (std::size_t index = 0; index < layout.player_chips.size(); ++index) {
            if (index >= layout.watch_indices.size())
                break;
            const int row_index = layout.watch_indices[index];
            if (row_index < 0 || static_cast<std::size_t>(row_index) >= model.watch.size())
                continue;
            const WatchRow& row = model.watch[static_cast<std::size_t>(row_index)];
            const kit::Rect& chip = layout.player_chips[index];
            draw_swatch(canvas, chip, row.colour, row.slot < 0);
            if (row.eye)
                kit::outline_corner_rect(canvas, chip, 2, kit::hud_colour::ink);
        }
        return;
    }
    for (std::size_t index = 0; index < layout.player_rows.size(); ++index) {
        if (index >= layout.watch_indices.size())
            break;
        const int row_index = layout.watch_indices[index];
        if (row_index < 0 || static_cast<std::size_t>(row_index) >= model.watch.size())
            continue;
        const WatchRow& row = model.watch[static_cast<std::size_t>(row_index)];
        const kit::Rect& box = layout.player_rows[index];
        if (row.eye)
            kit::outline_corner_rect(canvas, box, 3, kit::hud_colour::ink);
        const kit::Rect swatch{box.x + 4, box.y + 4, 12, 12};
        draw_swatch(canvas, swatch, row.colour, row.slot < 0);
        const int32_t text_x = swatch.x + swatch.width + 6;
        const int32_t text_w = std::max(box.width - (text_x - box.x) - 16, int32_t{1});
        kit::box_text(
            canvas,
            kit::FontRole::small,
            row.name,
            {text_x, box.y, text_w, 16},
            kit::Align::left,
            kit::hud_colour::label
        );
        if (row.eye)
            draw_eye(canvas, box.x + box.width - 14, box.y + 4);
        const std::string detail = watch_detail(row);
        if (!detail.empty()) {
            const int32_t detail_w = watch_detail_room(box);
            const kit::Measure measure = [&canvas](std::string_view line) {
                return kit::measure_text(canvas, kit::FontRole::small, line);
            };
            kit::box_text(
                canvas,
                kit::FontRole::small,
                kit::fit(detail, detail_w, measure),
                {text_x, box.y + 16, detail_w, 16},
                kit::Align::left,
                kit::hud_colour::quiet
            );
        }
    }
    const int32_t count = static_cast<int32_t>(model.watch.size());
    if (count <= watch_visible_rows)
        return;
    const int32_t track_h = std::max(layout.players.height - kListHeader - 10, int32_t{1});
    const int32_t thumb = std::max(track_h * watch_visible_rows / count, int32_t{8});
    const int32_t span = std::max(count - watch_visible_rows, int32_t{1});
    const int32_t travel = std::max(track_h - thumb, int32_t{0});
    const int32_t thumb_y = layout.players.y + kListHeader + travel * layout.list_scroll / span;
    kit::fill_rect(
        canvas,
        {layout.players.x + layout.players.width - 6, layout.players.y + kListHeader, 2, track_h},
        kit::hud_colour::quiet
    );
    kit::fill_rect(
        canvas,
        {layout.players.x + layout.players.width - 6, thumb_y, 2, thumb},
        kit::hud_colour::label
    );
}

/// Draws the view menu or the camera menu.
///
/// @param canvas where it is drawn
/// @param menu the menu
/// @param items its rows
/// @param captions one caption per row
void draw_choice_menu(
    const kit::Canvas& canvas,
    const kit::Rect& menu,
    const std::vector<kit::Rect>& items,
    const std::vector<std::string>& captions,
    int selected
) {
    if (!live(menu))
        return;
    kit::fill_corner_rect(canvas, menu, 4, card_fill);
    kit::outline_corner_rect(canvas, menu, 4, kit::hud_colour::edge);
    for (std::size_t index = 0; index < items.size() && index < captions.size(); ++index) {
        const bool current = static_cast<int>(index) == selected;
        if (current)
            kit::fill_rect(
                canvas,
                {items[index].x, items[index].y + items[index].height / 2 - 1, 6, 3},
                kit::hud_colour::lit
            );
        const kit::Rect words = current
                                    ? kit::Rect{
                                          items[index].x + 10,
                                          items[index].y,
                                          std::max(items[index].width - 10, int32_t{1}),
                                          items[index].height}
                                    : items[index];
        kit::box_text(
            canvas,
            kit::FontRole::small,
            captions[index],
            words,
            kit::Align::left,
            current ? kit::hud_colour::lit : kit::hud_colour::label
        );
    }
}

} // namespace

void BarVisibility::advance(
    uint32_t dt_ms, int32_t pointer_y, int32_t window_height, int32_t bar_top
) {
    if (pinned_) {
        shown_ = true;
        idle_ms_ = 0;
        return;
    }
    const int32_t band = bar_top - edge_band_points;
    const bool over = pointer_y >= band && pointer_y >= 0 && pointer_y < window_height;
    if (over) {
        shown_ = true;
        idle_ms_ = 0;
        return;
    }
    if (!shown_)
        return;
    const uint64_t waited = static_cast<uint64_t>(idle_ms_) + dt_ms;
    idle_ms_ = waited > hide_after_ms ? hide_after_ms : static_cast<uint32_t>(waited);
    if (idle_ms_ >= hide_after_ms)
        shown_ = false;
}

void BarVisibility::show() {
    shown_ = true;
    idle_ms_ = 0;
}

void BarVisibility::toggle_pin() {
    pinned_ = !pinned_;
    if (pinned_) {
        shown_ = true;
        idle_ms_ = 0;
    }
}

std::string clock_text(uint32_t tick) {
    const uint32_t seconds_total = tick / ticks_per_second;
    const uint32_t minutes = seconds_total / 60;
    const uint32_t seconds = seconds_total % 60;
    std::string text = std::to_string(minutes);
    text.push_back(':');
    if (seconds < 10)
        text.push_back('0');
    text += std::to_string(seconds);
    return text;
}

std::string time_text(uint32_t tick, uint32_t length) {
    return clock_text(tick) + " / " + clock_text(length);
}

std::string speed_word(uint32_t milli) {
    if (milli == 500)
        return std::string(words::half_speed);
    if (milli < 2000) {
        const uint32_t tenths = (milli + 50) / 100;
        const uint32_t whole = tenths / 10;
        const uint32_t fraction = tenths % 10;
        if (fraction == 0)
            return std::to_string(whole) + "×";
        return std::to_string(whole) + "." + std::to_string(fraction) + "×";
    }
    const uint32_t whole = (milli + 500) / 1000;
    return std::to_string(whole) + "×";
}

std::string
speed_caption(uint32_t asked_milli, uint32_t reached_milli, bool reached_lower, bool skipping) {
    if (skipping)
        return shown("Skip");
    const std::string asked = shown(speed_word(asked_milli));
    const std::string reached = shown(speed_word(reached_milli));
    if (!reached_lower || asked == reached)
        return asked;
    return fill(shown(words::speed_behind), {{"asked", asked}, {"reached", reached}});
}

std::string fitted_speed_caption(const Model& model, SpeedForm form) {
    // The catch-up's own line says it is catching up, so a button without
    // room for the full caption keeps the asked speed during a catch-up.
    if (form == SpeedForm::full)
        return speed_caption(
            model.asked_milli, model.reached_milli, model.reached_lower, model.skipping
        );
    const std::string asked = shown(speed_word(model.asked_milli));
    const std::string reached = shown(speed_word(model.reached_milli));
    if (form == SpeedForm::asked || model.skipping || !model.reached_lower || asked == reached)
        return asked;
    return asked + " · " + reached;
}

int player_colour(int live, int remembered, int recorded, int number) noexcept {
    if (live >= 0)
        return live;
    if (remembered >= 0)
        return remembered;
    if (recorded >= 0)
        return recorded;
    if (number < 0)
        return -1;
    const int wrapped = number % 10;
    return wrapped < 0 ? wrapped + 10 : wrapped;
}

/// A chat line without the `<name>` the recording puts in front of it.
///
/// The hover already names the player, so the prefix would say the name twice.
///
/// @param marker the chat marker
/// @return the line a player typed
std::string chat_body(const Marker& marker) {
    const std::string_view line = marker.line;
    if (!marker.name.empty()) {
        const std::string wrapped = "<" + marker.name + "> ";
        if (line.starts_with(wrapped))
            return std::string(line.substr(wrapped.size()));
    }
    if (line.starts_with('<')) {
        const std::size_t end = line.find("> ");
        if (end != std::string_view::npos)
            return std::string(line.substr(end + 2));
    }
    return marker.line;
}

std::string marker_caption(const Marker& marker) {
    const std::string time = clock_text(marker.tick);
    const std::string line = marker.kind == MarkerKind::chat ? chat_body(marker) : marker.line;
    return fill(
        shown(marker_pattern(marker)), {{"time", time}, {"name", marker.name}, {"line", line}}
    );
}

/// The empty-timeline line while markers are still being found.
///
/// @param percent how far the pass has come
/// @return the line
std::string finding_caption(int32_t percent) {
    const int32_t clamped = std::clamp(percent, int32_t{0}, int32_t{99});
    return fill(shown(words::finding), {{"percent", std::to_string(clamped)}});
}

std::string catching_up_caption(int32_t percent) {
    const int32_t clamped = std::clamp(percent, int32_t{0}, int32_t{100});
    return fill(shown(words::catching_up), {{"percent", std::to_string(clamped)}});
}

std::string damaged_caption(uint32_t tick) {
    return fill(shown(words::damaged), {{"time", clock_text(tick)}});
}

Layout lay_out(int32_t width, int32_t height, const Model& model, bool shown) {
    Layout layout;
    layout.shown = shown;
    layout.export_enabled = model.can_export;
    const bool compact = height < 700;
    const int32_t icon = compact ? 26 : 30;
    const int32_t play_w = compact ? 30 : 36;
    const int32_t play_h = compact ? 28 : 34;
    int32_t view_w = compact ? 96 : 128;
    int32_t camera_w = compact ? 64 : 80;
    const int32_t pad = 8;
    const int32_t gap = 4;
    const int32_t top_pad = 6;
    const int32_t mid_gap = 4;
    const int32_t bottom_pad = 6;
    const int32_t timeline_h = compact ? 22 : 26;
    const int32_t control_h = std::max(play_h, icon);
    const int32_t bar_h = top_pad + timeline_h + mid_gap + control_h + bottom_pad;
    int32_t time_w = compact ? 108 : 132;
    int32_t speed_w = compact ? 72 : 96;
    // What the text needs, measured by the app, with a little room inside the
    // button. Unmeasured widths are 0 and leave the defaults alone.
    const TextWidths& need = model.widths;
    const auto padded = [](int32_t text, int32_t room) { return text > 0 ? text + room : 0; };
    const int32_t time_need = padded(need.time, 4);
    const int32_t skip_need = padded(need.skip, 2);
    const int32_t full_need = std::max(padded(need.speed_full, 2), skip_need);
    const int32_t brief_need = padded(need.speed_brief, 2);
    const int32_t asked_need = padded(need.speed_asked, 2);
    const int32_t view_need = need.view > 0 ? need.view + 4 : 0;
    const int32_t camera_need = need.camera > 0 ? need.camera + 4 : 0;
    // The view and camera stay at their asked width until the time has kept
    // its text and the speed button has taken its form. Growing them first
    // pushes Keys and Leave off a 640-wide window. The time grows to its
    // text before anything else gives way.
    if (time_need > time_w)
        time_w = time_need;
    if (full_need > speed_w)
        speed_w = full_need;
    const int32_t safe_width = std::max(width, int32_t{1});
    const int32_t safe_height = std::max(height, int32_t{1});
    // Five icons, play, the view slot and the camera slot, and the gaps between
    // them. Time and speed give up width before those slots do: the time down
    // to what its text needs, then the speed button to its full caption, its
    // brief caption and the asked speed alone. Only past all of those do both
    // shrink to their least.
    const int32_t fixed = pad * 2 + icon * 5 + play_w + view_w + camera_w + gap * 9;
    int32_t spare = safe_width - fixed - time_w - speed_w;
    const auto give = [&spare](int32_t& from, int32_t floor) {
        if (spare >= 0)
            return;
        const int32_t given = std::min(-spare, std::max(from - floor, int32_t{0}));
        from -= given;
        spare += given;
    };
    give(time_w, std::max(int32_t{48}, time_need));
    give(speed_w, std::max(int32_t{36}, full_need));
    if (spare < 0 && full_need > 0) {
        layout.speed_form = SpeedForm::brief;
        give(speed_w, std::max(int32_t{36}, brief_need));
    }
    if (spare < 0 && brief_need > 0) {
        layout.speed_form = SpeedForm::asked;
        give(speed_w, std::max(int32_t{36}, asked_need));
    }
    give(time_w, std::max(int32_t{48}, time_need));
    give(speed_w, 36);
    // The view and the camera never shrink below the slot they were given.
    // They grow only into width the row still has, and never past the caption.
    int32_t room = std::max(spare, int32_t{0});
    if (view_need > view_w && room > 0) {
        const int32_t taken = std::min(room, view_need - view_w);
        view_w += taken;
        room -= taken;
    }
    if (camera_need > camera_w && room > 0) {
        const int32_t taken = std::min(room, camera_need - camera_w);
        camera_w += taken;
        room -= taken;
    }
    const int32_t grow = room;
    layout.bar = {0, safe_height - bar_h, safe_width, bar_h};
    const int32_t timeline_y = layout.bar.y + top_pad;
    layout.timeline = {pad, timeline_y, std::max(safe_width - pad * 2, int32_t{1}), timeline_h};
    const int32_t track_h = 6;
    const int32_t heat_h = 4;
    layout.track = {layout.timeline.x, timeline_y + 2, layout.timeline.width, track_h};
    layout.heat = {layout.timeline.x, layout.track.y + track_h + 2, layout.timeline.width, heat_h};
    const int32_t row_y = timeline_y + timeline_h + mid_gap;
    const int32_t icon_y = row_y + (control_h - icon) / 2;
    const int32_t play_y = row_y + (control_h - play_h) / 2;
    int32_t x = pad;
    layout.back = {x, icon_y, icon, icon};
    x += icon + gap;
    layout.play = {x, play_y, play_w, play_h};
    x += play_w + gap;
    layout.forward = {x, icon_y, icon, icon};
    x += icon + gap;
    layout.time = {x, row_y, time_w, control_h};
    x += time_w + gap;
    layout.speed = {x, row_y, speed_w, control_h};
    x += speed_w + gap + grow;
    layout.view = {x, row_y, view_w, control_h};
    x += view_w + gap;
    layout.camera = {x, row_y, camera_w, control_h};
    x += camera_w + gap;
    layout.chat = {x, icon_y, icon, icon};
    x += icon + gap;
    layout.keys = {x, icon_y, icon, icon};
    x += icon + gap;
    layout.leave = {x, icon_y, icon, icon};
    const int32_t button = control_h;
    const int32_t knob_w = 12;
    const int32_t knob_h = track_h + 4;
    const int32_t knob_x = column_of(layout.track, model.tick, model.length) - knob_w / 2;
    layout.knob = {
        std::clamp(
            knob_x,
            layout.track.x,
            std::max(layout.track.x + layout.track.width - knob_w, layout.track.x)
        ),
        layout.track.y - 2,
        knob_w,
        knob_h,
    };
    layout.marker_hits.reserve(model.markers.size());
    for (const Marker& marker : model.markers) {
        const int32_t column = column_of(layout.track, marker.tick, model.length);
        const int32_t hit_w = marker_reach_points * 2;
        layout.marker_hits.push_back(
            {column - marker_reach_points, layout.track.y - 4, hit_w, layout.track.height + 8}
        );
    }
    if (shown && model.speed_open) {
        const int32_t item_h = 26;
        const int32_t menu_h = item_h * static_cast<int32_t>(speed_count);
        const int32_t menu_w = std::max(layout.speed.width, int32_t{88});
        int32_t menu_y = layout.speed.y - menu_h - 4;
        if (menu_y < 4)
            menu_y = 4;
        layout.speed_menu = {layout.speed.x + layout.speed.width - menu_w, menu_y, menu_w, menu_h};
        layout.speed_items.reserve(speed_count);
        for (std::size_t index = 0; index < speed_count; ++index) {
            layout.speed_items.push_back(
                {layout.speed_menu.x + 4,
                 layout.speed_menu.y + 2 + static_cast<int32_t>(index) * item_h,
                 layout.speed_menu.width - 8,
                 item_h - 2}
            );
        }
    }
    if (shown && model.keys_open) {
        const int32_t card_w = std::min(int32_t{360}, std::max(safe_width - 16, int32_t{120}));
        const int32_t card_h = std::min(int32_t{240}, std::max(layout.bar.y - 16, int32_t{80}));
        int32_t card_x = layout.keys.x + layout.keys.width - card_w;
        if (card_x < 8)
            card_x = 8;
        if (card_x + card_w > safe_width - 8)
            card_x = std::max(safe_width - 8 - card_w, int32_t{8});
        layout.keys_card = {
            card_x, std::max(layout.bar.y - card_h - 8, int32_t{8}), card_w, card_h
        };
    }
    if (model.end_card) {
        constexpr int32_t kMargin = 8;
        const int32_t card_w = std::min(int32_t{520}, std::max(safe_width - 32, int32_t{200}));
        const int32_t card_room = std::max(layout.bar.y - kMargin * 2, int32_t{80});
        int32_t card_h = std::min(int32_t{340}, card_room);
        int32_t card_y = layout.bar.y - card_h - kMargin;
        if (card_y < kMargin) {
            card_y = kMargin;
            card_h = std::max(layout.bar.y - kMargin - card_y, int32_t{80});
        }
        layout.end = {(safe_width - card_w) / 2, card_y, card_w, card_h};
        const int32_t button_w = std::max((card_w - 16 * 2 - 8 * 2) / 3, int32_t{40});
        const int32_t button_y = layout.end.y + layout.end.height - 16 - button;
        layout.watch_again = {layout.end.x + 16, button_y, button_w, button};
        layout.library = {layout.watch_again.x + button_w + 8, button_y, button_w, button};
        layout.export_button = {layout.library.x + button_w + 8, button_y, button_w, button};
    }
    place_watch(layout, model, safe_width, compact);
    // The card keeps the width its captions need. On a 512-point battlefield
    // a list that would run under it, chips included, is hidden. On a wider
    // battlefield the card slides left of the list, and the list takes the
    // room that remains, so a departed row stays on show.
    const auto card_hits_list = [&] {
        return live(layout.end) && live(layout.players) &&
               layout.end.x < layout.players.x + layout.players.width &&
               layout.end.x + layout.end.width > layout.players.x &&
               layout.end.y < layout.players.y + layout.players.height &&
               layout.players.y < layout.end.y + layout.end.height;
    };
    if (card_hits_list()) {
        if (safe_width <= 512) {
            layout.players = {};
            layout.player_rows.clear();
            layout.player_chips.clear();
            layout.watch_indices.clear();
        } else {
            int32_t card_x = layout.players.x - kListMargin - layout.end.width;
            if (card_x < kListMargin)
                card_x = kListMargin;
            const int32_t moved = card_x - layout.end.x;
            layout.end.x += moved;
            layout.watch_again.x += moved;
            layout.library.x += moved;
            layout.export_button.x += moved;
            const int32_t left_limit = layout.end.x + layout.end.width + kListMargin;
            if (layout.players.x < left_limit)
                place_watch(layout, model, safe_width, compact, left_limit);
        }
    }
    if (shown && model.view_open) {
        place_menu(
            layout.view_menu,
            layout.view_items,
            layout.view,
            static_cast<int32_t>(model.watch.size()),
            std::max(layout.view.width, int32_t{160}),
            safe_width
        );
    }
    if (shown && model.camera_open) {
        layout.camera_modes.push_back(CameraMode::free);
        if (model.offer_follow_camera)
            layout.camera_modes.push_back(CameraMode::follow_theirs);
        place_menu(
            layout.camera_menu,
            layout.camera_items,
            layout.camera,
            static_cast<int32_t>(layout.camera_modes.size()),
            std::max(layout.camera.width, int32_t{168}),
            safe_width
        );
    }
    return layout;
}

Control hit_control(const Layout& layout, kit::Point at) {
    const auto in = [&](Control id, const kit::Rect& rect) {
        return live(rect) && kit::contains(rect, at) ? id : Control::none;
    };
    // The speed menu and the shortcut card are drawn over the end card, so
    // they take a press first.
    for (std::size_t index = 0; index < layout.speed_items.size() && index < speed_count; ++index) {
        if (kit::contains(layout.speed_items[index], at))
            return static_cast<Control>(
                static_cast<int32_t>(Control::speed_500) + static_cast<int32_t>(index)
            );
    }
    if (const Control hit = in(Control::keys_card, layout.keys_card); hit != Control::none)
        return hit;
    if (const Control hit = in(Control::watch_again, layout.watch_again); hit != Control::none)
        return hit;
    if (const Control hit = in(Control::library, layout.library); hit != Control::none)
        return hit;
    if (live(layout.export_button) && kit::contains(layout.export_button, at))
        return layout.export_enabled ? Control::export_replay : Control::none;
    if (const Control hit = in(Control::end_card, layout.end); hit != Control::none)
        return hit;
    if (!layout.shown)
        return Control::none;
    if (const Control hit = in(Control::back, layout.back); hit != Control::none)
        return hit;
    if (const Control hit = in(Control::play, layout.play); hit != Control::none)
        return hit;
    if (const Control hit = in(Control::forward, layout.forward); hit != Control::none)
        return hit;
    if (const Control hit = in(Control::timeline, layout.timeline); hit != Control::none)
        return hit;
    if (const Control hit = in(Control::speed, layout.speed); hit != Control::none)
        return hit;
    if (const Control hit = in(Control::view, layout.view); hit != Control::none)
        return hit;
    if (const Control hit = in(Control::camera, layout.camera); hit != Control::none)
        return hit;
    if (const Control hit = in(Control::chat, layout.chat); hit != Control::none)
        return hit;
    if (const Control hit = in(Control::keys, layout.keys); hit != Control::none)
        return hit;
    if (const Control hit = in(Control::leave, layout.leave); hit != Control::none)
        return hit;
    return Control::none;
}

int marker_index(const Layout& layout, kit::Point at) {
    int found = -1;
    int64_t best = 0;
    for (std::size_t index = 0; index < layout.marker_hits.size(); ++index) {
        const kit::Rect& rect = layout.marker_hits[index];
        if (!kit::contains(rect, at))
            continue;
        const int64_t dx = static_cast<int64_t>(at.x) - (rect.x + rect.width / 2);
        const int64_t dy = static_cast<int64_t>(at.y) - (rect.y + rect.height / 2);
        const int64_t distance = dx * dx + dy * dy;
        if (found < 0 || distance < best) {
            found = static_cast<int>(index);
            best = distance;
        }
    }
    return found;
}

uint32_t tick_at(const kit::Rect& timeline, int32_t x, uint32_t length) {
    if (timeline.width <= 1 || length == 0)
        return 0;
    int32_t local = x - timeline.x;
    if (local < 0)
        local = 0;
    if (local >= timeline.width)
        local = timeline.width - 1;
    return static_cast<uint32_t>(
        (static_cast<uint64_t>(local) * length) / static_cast<uint64_t>(timeline.width - 1)
    );
}

uint32_t milli_of(Control control) noexcept {
    const auto index = static_cast<int32_t>(control) - static_cast<int32_t>(Control::speed_500);
    if (index < 0 || static_cast<std::size_t>(index) >= speed_count)
        return 0;
    return speed_milli[index];
}

void fill_controls(const Layout& layout, const Model& model, kit::DisplayList& list) {
    list.controls.clear();
    list.tab_order.clear();
    list.items.clear();
    if (model.end_card) {
        add_control(
            list,
            Control::watch_again,
            layout.watch_again,
            "watch-again",
            kit::ControlKind::button,
            shown(words::watch_again),
            false
        );
        add_control(
            list,
            Control::library,
            layout.library,
            "library",
            kit::ControlKind::button,
            shown(words::back_to_library),
            false
        );
        add_control(
            list,
            Control::export_replay,
            layout.export_button,
            "export",
            kit::ControlKind::button,
            shown("Export…"),
            false,
            layout.export_enabled
        );
        add_control(
            list, Control::end_card, layout.end, "end-card", kit::ControlKind::area, {}, false
        );
    }
    if (layout.shown && model.speed_open) {
        const std::string_view names[] = {
            "speed-500", "speed-1000", "speed-2000", "speed-4000", "speed-8000", "speed-16000"
        };
        for (std::size_t index = 0; index < layout.speed_items.size() && index < speed_count;
             ++index) {
            add_control(
                list,
                static_cast<Control>(
                    static_cast<int32_t>(Control::speed_500) + static_cast<int32_t>(index)
                ),
                layout.speed_items[index],
                names[index],
                kit::ControlKind::button,
                speed_word(speed_milli[index]),
                model.asked_milli == speed_milli[index]
            );
        }
    }
    if (layout.shown && model.keys_open) {
        std::string chords;
        for (const KeyChord& row : model.key_chords) {
            if (row.chord.empty())
                continue;
            if (!chords.empty())
                chords.push_back('\n');
            chords += row.chord;
        }
        add_control(
            list,
            Control::keys_card,
            layout.keys_card,
            "keys-card",
            kit::ControlKind::area,
            std::move(chords),
            false
        );
    }
    for (std::size_t index = 0; index < layout.watch_indices.size(); ++index) {
        const int row_index = layout.watch_indices[index];
        if (row_index < 0 || static_cast<std::size_t>(row_index) >= model.watch.size())
            continue;
        const WatchRow& row = model.watch[static_cast<std::size_t>(row_index)];
        const bool chip = layout.list_folded;
        const kit::Rect& rect = chip ? layout.player_chips[index] : layout.player_rows[index];
        const std::string name = (chip ? std::string("watch-chip.") : std::string("watch-row.")) +
                                 std::to_string(row_index);
        std::string caption = row.name;
        if (!row.departed.empty()) {
            caption.push_back(' ');
            caption += row.departed;
        }
        add_control(list, Control::view, rect, name, kit::ControlKind::button, caption, row.eye);
    }
    if (layout.shown && model.view_open) {
        for (std::size_t index = 0; index < layout.view_items.size() && index < model.watch.size();
             ++index) {
            add_control(
                list,
                Control::view,
                layout.view_items[index],
                "view-item." + std::to_string(index),
                kit::ControlKind::button,
                model.watch[index].name,
                static_cast<int>(index) == model.viewed
            );
        }
    }
    if (layout.shown && model.camera_open) {
        for (std::size_t index = 0;
             index < layout.camera_items.size() && index < layout.camera_modes.size();
             ++index) {
            add_control(
                list,
                Control::camera,
                layout.camera_items[index],
                "camera-item." + std::to_string(index),
                kit::ControlKind::button,
                camera_button_caption(layout.camera_modes[index]),
                layout.camera_modes[index] == model.camera
            );
        }
    }
    if (!layout.shown)
        return;
    add_control(
        list,
        Control::back,
        layout.back,
        "back",
        kit::ControlKind::button,
        shown("Back 10 seconds"),
        false
    );
    add_control(
        list,
        Control::play,
        layout.play,
        "play",
        kit::ControlKind::button,
        model.paused ? shown("Play") : shown("Pause"),
        !model.paused
    );
    add_control(
        list,
        Control::forward,
        layout.forward,
        "forward",
        kit::ControlKind::button,
        shown("Forward 10 seconds"),
        false
    );
    add_control(
        list,
        Control::timeline,
        layout.timeline,
        "timeline",
        kit::ControlKind::slider,
        time_text(model.tick, model.length),
        false
    );
    add_control(
        list,
        Control::speed,
        layout.speed,
        "speed",
        kit::ControlKind::button,
        speed_caption(model.asked_milli, model.reached_milli, model.reached_lower, model.skipping),
        model.speed_open
    );
    add_control(
        list,
        Control::view,
        layout.view,
        "view",
        kit::ControlKind::button,
        view_button_caption(model),
        model.view_open
    );
    add_control(
        list,
        Control::camera,
        layout.camera,
        "camera",
        kit::ControlKind::button,
        camera_button_caption(model.camera),
        model.camera_open
    );
    add_control(
        list,
        Control::chat,
        layout.chat,
        "chat",
        kit::ControlKind::toggle,
        shown(words::chat),
        model.chat_shown
    );
    add_control(
        list,
        Control::keys,
        layout.keys,
        "keys",
        kit::ControlKind::button,
        shown(words::keys),
        model.keys_open
    );
    add_control(
        list,
        Control::leave,
        layout.leave,
        "leave",
        kit::ControlKind::button,
        shown(words::leave),
        false
    );
}

void draw(const kit::Canvas& canvas, const Layout& layout, const Model& model) {
    if (layout.shown) {
        kit::fill_rect(canvas, layout.bar, kit::hud_colour::panel);
        kit::fill_rect(
            canvas, {layout.bar.x, layout.bar.y, layout.bar.width, 1}, kit::hud_colour::edge
        );
        draw_icon_button(canvas, layout.back, Glyph::back, false);
        draw_icon_button(
            canvas, layout.play, model.paused ? Glyph::play : Glyph::pause, !model.paused
        );
        draw_icon_button(canvas, layout.forward, Glyph::forward, false);
        kit::box_text(
            canvas,
            kit::FontRole::small,
            time_text(model.tick, model.length),
            layout.time,
            kit::Align::centre,
            kit::hud_colour::label
        );
        draw_track(canvas, layout, model);
        draw_markers(canvas, layout, model);
        draw_button(
            canvas, layout.speed, fitted_speed_caption(model, layout.speed_form), model.speed_open
        );
        draw_menu_button(canvas, layout.view, view_button_caption(model), model.view_open);
        draw_menu_button(
            canvas, layout.camera, camera_button_caption(model.camera), model.camera_open
        );
        draw_icon_button(canvas, layout.chat, Glyph::chat, model.chat_shown);
        draw_icon_button(canvas, layout.keys, Glyph::keys, model.keys_open);
        draw_icon_button(canvas, layout.leave, Glyph::leave, false);
        if (model.finding) {
            const std::string line = finding_caption(model.finding_percent);
            const kit::Rect tip{
                layout.timeline.x,
                layout.timeline.y,
                std::min(layout.timeline.width, int32_t{360}),
                layout.timeline.height
            };
            kit::fill_corner_rect(canvas, tip, 3, card_fill);
            kit::box_text(
                canvas, kit::FontRole::small, line, tip, kit::Align::centre, kit::hud_colour::label
            );
        }
        if (model.catch_up_percent >= 0) {
            const std::string line = catching_up_caption(model.catch_up_percent);
            const kit::Rect tip{
                layout.timeline.x,
                std::max(layout.timeline.y - 20, int32_t{0}),
                std::min(layout.timeline.width, int32_t{220}),
                18
            };
            kit::fill_corner_rect(canvas, tip, 3, card_fill);
            kit::box_text(
                canvas, kit::FontRole::small, line, tip, kit::Align::centre, kit::hud_colour::label
            );
        }
    }
    // The end card sits where the speed menu and the shortcut card open, so
    // those, and the hover, are drawn over it.
    draw_end(canvas, layout, model);
    draw_players(canvas, layout, model);
    if (layout.shown) {
        draw_hover(canvas, layout, model);
        draw_speed_menu(canvas, layout, model);
        draw_keys(canvas, layout, model);
        if (model.view_open) {
            std::vector<std::string> captions;
            captions.reserve(model.watch.size());
            for (const WatchRow& row : model.watch)
                captions.push_back(row.name);
            draw_choice_menu(canvas, layout.view_menu, layout.view_items, captions, model.viewed);
        }
        if (model.camera_open) {
            std::vector<std::string> captions;
            captions.reserve(layout.camera_modes.size());
            int camera_selected = -1;
            for (std::size_t index = 0; index < layout.camera_modes.size(); ++index) {
                captions.push_back(camera_button_caption(layout.camera_modes[index]));
                if (layout.camera_modes[index] == model.camera)
                    camera_selected = static_cast<int>(index);
            }
            draw_choice_menu(
                canvas, layout.camera_menu, layout.camera_items, captions, camera_selected
            );
        }
    }
}

namespace {

/// A figure of zero or more, with one decimal.
///
/// @param value the figure
/// @return such as "10.0"
std::string one_decimal(float value) {
    const double clamped = value > 0.f ? static_cast<double>(value) : 0.0;
    const long tenths = std::lround(clamped * 10.0);
    return std::to_string(tenths / 10) + "." + std::to_string(tenths % 10);
}

/// A whole figure of zero or more.
///
/// @param value the figure
/// @return such as "100"
std::string whole_number(float value) {
    const double clamped = value > 0.f ? static_cast<double>(value) : 0.0;
    return std::to_string(std::lround(clamped));
}

/// The first rectangle that holds a point.
///
/// @param rects the rectangles
/// @param at the point
/// @return the rectangle's place, or -1
int index_in(const std::vector<kit::Rect>& rects, kit::Point at) {
    for (std::size_t index = 0; index < rects.size(); ++index)
        if (kit::contains(rects[index], at))
            return static_cast<int>(index);
    return -1;
}

} // namespace

std::string view_button_caption(const Model& model) {
    if (model.viewed >= 0 && static_cast<std::size_t>(model.viewed) < model.watch.size())
        return model.watch[static_cast<std::size_t>(model.viewed)].name;
    return shown(words::all_players);
}

std::string camera_button_caption(CameraMode mode) {
    switch (mode) {
    case CameraMode::follow_theirs:
        return shown(words::follow_their_camera);
    case CameraMode::follow_unit:
        return shown(words::follow_a_unit);
    case CameraMode::free:
        break;
    }
    return shown(words::free_camera);
}

std::string income_caption(float metal, float energy) {
    const std::string metal_text = one_decimal(metal);
    const std::string energy_text = whole_number(energy);
    return fill(shown(words::income_rate), {{"metal", metal_text}, {"energy", energy_text}});
}

std::string team_caption(int team) {
    const std::string number = std::to_string(team);
    return fill(shown(words::team_label), {{"team", number}});
}

std::string departed_caption(bool defeated, std::string_view time) {
    return fill(shown(defeated ? words::defeated_at : words::left_at), {{"time", time}});
}

int watch_slot(std::span<const int> numbers, int number, int slot_count) noexcept {
    if (number < 0 || slot_count <= 0)
        return -1;
    for (int place = 0; place < static_cast<int>(numbers.size()); ++place) {
        if (numbers[static_cast<std::size_t>(place)] != number)
            continue;
        if (place < slot_count)
            return place;
    }
    if (number < slot_count)
        return number;
    if (number > 0 && number - 1 < slot_count)
        return number - 1;
    return -1;
}

int32_t watch_detail_room(const kit::Rect& row) noexcept {
    // The swatch sits 4 points in and is 12 wide, then 6 points of gap. The
    // name keeps 16 for the eye; the detail uses that room as well.
    const int32_t inset = 4 + 12 + 6;
    return std::max(row.width - inset, int32_t{1});
}

std::string watch_detail(const WatchRow& row) {
    // Every player has no side, no count and no departure. A seat that has
    // gone still says how it went. The departure leads, so the end of a
    // narrow row can be cut without losing "Left 1:20".
    if (row.slot < 0 && row.side.empty() && row.units == 0 && row.departed.empty() && !row.income)
        return {};
    std::string line;
    const auto append = [&line](const std::string& part) {
        if (part.empty())
            return;
        if (!line.empty())
            line += " · ";
        line += part;
    };
    append(row.departed);
    append(row.side);
    if (row.team >= 0)
        append(team_caption(row.team));
    const std::string_view count_word = row.units == 1 ? words::unit_count : words::units_count;
    append(fill(shown(count_word), {{"units", std::to_string(row.units)}}));
    if (row.income)
        append(income_caption(row.metal, row.energy));
    return line;
}

int watch_index(const Layout& layout, kit::Point at) {
    const int visual = index_in(layout.list_folded ? layout.player_chips : layout.player_rows, at);
    if (visual < 0 || static_cast<std::size_t>(visual) >= layout.watch_indices.size())
        return -1;
    return layout.watch_indices[static_cast<std::size_t>(visual)];
}

int view_menu_index(const Layout& layout, kit::Point at) {
    return index_in(layout.view_items, at);
}

int camera_menu_index(const Layout& layout, kit::Point at) {
    return index_in(layout.camera_items, at);
}

bool over_watch_chrome(const Layout& layout, kit::Point at) {
    return kit::contains(layout.players, at) || kit::contains(layout.view_menu, at) ||
           kit::contains(layout.camera_menu, at);
}

} // namespace oa::ui::replay_viewer

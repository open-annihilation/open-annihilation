// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Where the photo panel, its chips, its key line and its toast lie.

#include "oa/ui/photo_panel/layout.hpp"

#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/kit/theme.hpp"
#include "oa/ui/photo_panel/crop.hpp"
#include "oa/ui/photo_panel/words.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::photo_panel {
namespace {

/// Points kept clear of the window's edge.
constexpr int32_t kMargin = 8;
/// The top band the chips and the alerts stay inside.
constexpr int32_t kChipBand = 72;
/// The bottom band the key line stays inside.
constexpr int32_t kBottomBand = 44;
/// The toast's place, above the key line and left of the panel.
constexpr int32_t kToastH = 72;
/// Points inside the panel's edge.
constexpr int32_t kPad = 6;
/// The zoom track stays at least this wide. A lock's reason takes its own
/// line, so it is not part of the measure.
constexpr int32_t kMinTrack = 64;
/// The most rows the size row's reason wraps to. A longer reason ends in
/// an ellipsis on the last of them.
constexpr std::size_t kSizeReasonRows = 4;

/// A locked control's value, with its reason where it has one.
///
/// @param value the value
/// @param reason the reason; empty when the control moves
/// @return the text automation reads
std::string with_reason(const std::string& value, const std::string& reason) {
    if (reason.empty())
        return value;
    if (value.empty())
        return reason;
    return value + "  " + reason;
}

/// A small-font line's width, or `stand_in` when the fonts measure nothing.
///
/// @param fonts the fonts
/// @param text the line
/// @param stand_in the width used when measuring gives nothing
/// @return the width, in points
int32_t line_width(const kit::Fonts& fonts, std::string_view text, int32_t stand_in) {
    const int32_t measured = kit::text_width(fonts, kit::FontRole::small, text);
    if (measured > 0)
        return measured;
    return stand_in;
}

/// A line shortened so it ends inside `width`. A line that fits is unchanged.
///
/// @param fonts the fonts
/// @param text the line
/// @param width the room, in points
/// @return the line as it is drawn
std::string fit_line(const kit::Fonts& fonts, std::string_view text, int32_t width) {
    if (text.empty() || width <= 0)
        return std::string(text);
    const int32_t full = kit::text_width(fonts, kit::FontRole::small, text);
    if (full <= 0 || full <= width)
        return std::string(text);
    return kit::fit(text, width, [&](std::string_view part) {
        return kit::text_width(fonts, kit::FontRole::small, part);
    });
}

/// The label and the track. The value is measured on its own, never with a
/// lock's reason, so the track keeps its width.
struct SliderParts {
    int32_t label_w{0};
    int32_t track_w{0};
};

/// Splits a slider's row. The label is measured against the row, and the
/// track stays at least kMinTrack while the row allows it.
///
/// @param fonts the fonts the label and the value are measured in
/// @param column_width the row's width
/// @param label the caption
/// @param shown the value, without a lock's reason
/// @param reserve values the slider can show; the widest sets the value's room
/// @return the label's width and the track's width
SliderParts slider_parts(
    const kit::Fonts& fonts,
    int32_t column_width,
    std::string_view label,
    std::string_view shown,
    const std::vector<std::string>& reserve
) {
    int32_t label_w = line_width(fonts, label, 48) + 8;
    int32_t value_w = line_width(fonts, shown, 36);
    for (const std::string& wide : reserve)
        value_w = std::max(value_w, line_width(fonts, wide, 36));
    value_w += 8;
    if (label_w < 36)
        label_w = 36;
    if (value_w < 28)
        value_w = 28;
    int32_t track_w = column_width - label_w - value_w;
    if (column_width >= kMinTrack && track_w < kMinTrack) {
        int32_t need = kMinTrack - track_w;
        const int32_t from_value = std::min(need, std::max(value_w - 16, int32_t{0}));
        value_w -= from_value;
        need -= from_value;
        const int32_t from_label = std::min(need, std::max(label_w - 24, int32_t{0}));
        label_w -= from_label;
        track_w = column_width - label_w - value_w;
    }
    if (track_w < 0) {
        label_w = std::max(label_w + track_w, int32_t{0});
        value_w = std::max(column_width - label_w, int32_t{0});
        track_w = std::max(column_width - label_w - value_w, int32_t{0});
    }
    return {label_w, track_w};
}

/// Sets a control's caption and value, and keeps it in the Tab order.
///
/// @param[in,out] list the display list
/// @param id the control
/// @param label the caption
/// @param text the value
void stamp(kit::DisplayList& list, kit::ControlId id, std::string label, std::string text) {
    for (kit::Control& control : list.controls) {
        if (control.id != id)
            continue;
        control.label = std::move(label);
        control.text = std::move(text);
        control.enabled = true;
        control.focusable = true;
        break;
    }
    list.tab_order.push_back(id);
}

/// A button's look.
///
/// @param model the panel
/// @param id the control
/// @param caption the words
/// @param accent true for the lime primary
/// @param enabled false draws the locked face
/// @return the look
kit::ButtonLook
button_look(const Model& model, kit::ControlId id, std::string caption, bool accent, bool enabled) {
    kit::ButtonLook look;
    look.caption = std::move(caption);
    look.style = accent ? kit::ButtonStyle::accent : kit::ButtonStyle::plain;
    look.bright = accent;
    look.enabled = enabled;
    look.hovered = model.hot == id;
    look.font = kit::FontRole::small;
    return look;
}

/// How many reason lines the open sections add under their controls.
///
/// @param model the panel
/// @param size_reason_rows the rows the size row's reason wraps to
/// @return the extra rows
int reason_rows(const Model& model, int size_reason_rows) {
    int extra = 0;
    if (model.camera_open) {
        if (!model.zoom_lock.empty() || !model.zoom_note.empty())
            ++extra;
        if (!model.turn_lock.empty())
            ++extra;
        if (!model.tilt_lock.empty())
            ++extra;
    }
    if (model.time_open) {
        if (!model.slow_lock.empty())
            ++extra;
        if (!model.speed_lock.empty())
            ++extra;
        if (!model.step_lock.empty())
            ++extra;
        if (!model.between_lock.empty())
            ++extra;
    }
    if (model.capture_open && !model.size_reason.empty())
        extra += std::max(size_reason_rows, 1);
    return extra;
}

/// How many preset names fit on one row with every name whole.
///
/// Six, then three, then two, then one. A name the font cannot measure is
/// counted as eight points a byte, so an accented word still asks for room.
///
/// @param fonts the fonts the names are measured in
/// @param labels the six names
/// @param row_width the row, in points
/// @return 6, 3, 2 or 1
int32_t preset_columns(
    const kit::Fonts& fonts, const std::array<std::string, 6>& labels, int32_t row_width
) {
    int32_t widest = 1;
    for (const std::string& label : labels) {
        int32_t measured = kit::text_width(fonts, kit::FontRole::small, label);
        if (measured <= 0)
            measured = static_cast<int32_t>(label.size()) * 8;
        if (measured > widest)
            widest = measured;
    }
    const int32_t need = widest + 4;
    const int32_t inner = std::max(row_width - 2, int32_t{1});
    const int32_t choices[] = {6, 3, 2, 1};
    for (const int32_t columns : choices) {
        if (inner / std::max(columns, int32_t{1}) >= need)
            return columns;
    }
    return 1;
}

/// How many rows the preset names take.
///
/// @param fonts the fonts the names are measured in
/// @param labels the six names
/// @param row_width the row, in points
/// @return the rows, at least 1
int preset_band_rows(
    const kit::Fonts& fonts, const std::array<std::string, 6>& labels, int32_t row_width
) {
    const int32_t columns = std::max(preset_columns(fonts, labels, row_width), int32_t{1});
    return (6 + columns - 1) / columns;
}

/// The most columns, of six, three, two and one, at or under `columns`
/// that leave each name at least `least` wide, so a finger can choose one.
///
/// @param columns the columns the names' own widths allow
/// @param row_width the row, in points
/// @param least the narrowest a name may be, in points
/// @return 6, 3, 2 or 1
int32_t finger_columns(int32_t columns, int32_t row_width, int32_t least) {
    const int32_t inner = std::max(row_width - 2, int32_t{1});
    const int32_t choices[] = {6, 3, 2, 1};
    for (const int32_t choice : choices) {
        if (choice <= columns && inner / choice >= least)
            return choice;
    }
    return 1;
}

/// The most columns, of every stop and of half of them, that leave each stop
/// of a strip at least `least` wide, so a finger can choose one.
///
/// @param stops the strip's stops
/// @param row_width the row, in points
/// @param least the narrowest a stop may be, in points
/// @return the stops, half of them rounded up, or 1
int32_t stop_columns(int32_t stops, int32_t row_width, int32_t least) {
    const int32_t inner = std::max(row_width - 2, int32_t{1});
    const int32_t choices[] = {stops, (stops + 1) / 2};
    for (const int32_t choice : choices) {
        if (choice > 0 && inner / choice >= least)
            return choice;
    }
    return 1;
}

/// How many rows the open panel lays out.
///
/// @param model the panel
/// @param preset_rows how many rows the preset names take
/// @param size_reason_rows the rows the size row's reason wraps to
/// @param depth_note_rows the rows the depth page's notes and locks wrap to
/// @return the rows
int row_count(const Model& model, int preset_rows, int size_reason_rows, int depth_note_rows) {
    int rows = 1;
    rows += 1;
    if (model.camera_open)
        rows += 5;
    rows += 1;
    if (model.time_open)
        rows += 5;
    rows += 1;
    if (model.look_open) {
        // The depth page replaces the grade: five sliders, the rows its
        // notes and locks wrap to, and the two switches and Guides. In the
        // turned view the haze lock's rows become the depth-of-field slider,
        // so the page grows no taller there.
        if (model.depth_page)
            rows += 5 + depth_note_rows + 3;
        else
            rows += 8 + std::max(preset_rows, 1);
        if (!model.preview_note.empty())
            ++rows;
    }
    rows += 1;
    if (model.capture_open)
        rows += 7;
    rows += reason_rows(model, size_reason_rows);
    return rows;
}

/// One column of rows inside the panel.
struct Column {
    kit::DisplayList* list{};
    const Model* model{};
    const kit::Fonts* fonts{};
    int32_t x{0};
    int32_t width{0};
    int32_t y{0};
    int32_t row_h{16};
    int32_t gap{2};

    /// The next row's rectangle.
    ///
    /// @return the row
    kit::Rect take() {
        const kit::Rect row{x, y, width, row_h};
        y += row_h + gap;
        return row;
    }
};

/// A line of text, clipped to its box. It is not a control.
void add_line(kit::DisplayList& list, kit::Rect rect, std::string text) {
    if (rect.width <= 0 || rect.height <= 0 || text.empty())
        return;
    kit::Item item;
    item.role = kit::Role::text;
    item.rect = rect;
    item.clip = rect;
    item.font = kit::FontRole::small;
    item.align = kit::Align::left;
    item.colour = kit::colour::text;
    item.text = std::move(text);
    list.items.push_back(std::move(item));
}

/// Words the reader reads and Tab does not stop on.
void add_quiet(
    kit::DisplayList& list,
    kit::Rect rect,
    kit::ControlId id,
    const char* name,
    std::string label,
    std::string text,
    uint8_t live
) {
    kit::Control control;
    control.id = id;
    control.rect = rect;
    control.enabled = false;
    control.focusable = false;
    control.name = name;
    control.kind = kit::ControlKind::area;
    control.label = std::move(label);
    control.text = std::move(text);
    control.live = live;
    list.controls.push_back(std::move(control));
}

/// The reason under a control: a short line when the sentence is wider than
/// the row, and the whole sentence in the control's text.
void add_reason(Column& column, kit::ControlId id, const char* name, const std::string& reason) {
    if (reason.empty())
        return;
    const kit::Rect row = column.take();
    add_line(*column.list, row, fit_line(*column.fonts, reason, row.width));
    add_quiet(*column.list, row, id, name, {}, reason, 0);
}

/// A reason broken into the rows it is drawn on, each no wider than `width`.
/// At most kSizeReasonRows; a longer reason ends in an ellipsis.
///
/// @param fonts the fonts
/// @param reason the sentence
/// @param width the room, in points
/// @return the rows; none for an empty reason
std::vector<std::string>
wrapped_reason(const kit::Fonts& fonts, std::string_view reason, int32_t width) {
    if (reason.empty())
        return {};
    if (width <= 0)
        return {std::string(reason)};
    kit::WrapRules rules;
    rules.most_lines = kSizeReasonRows;
    rules.shorten_last = [&fonts, width](std::string_view rest) {
        return fit_line(fonts, rest, width);
    };
    std::vector<std::string> rows = kit::wrap(
        reason,
        width,
        [&fonts](std::string_view part) {
            return kit::text_width(fonts, kit::FontRole::small, part);
        },
        rules
    );
    if (rows.empty())
        rows.emplace_back(reason);
    return rows;
}

/// Appends a reason over as many rows as it was wrapped to. A screen reader
/// reads it whole, once.
void add_wrapped_reason(
    Column& column,
    kit::ControlId id,
    const char* name,
    const std::string& reason,
    const std::vector<std::string>& rows
) {
    if (reason.empty() || rows.empty())
        return;
    kit::Rect whole{};
    bool first = true;
    for (const std::string& text : rows) {
        const kit::Rect row = column.take();
        add_line(*column.list, row, fit_line(*column.fonts, text, row.width));
        if (first)
            whole = row;
        else
            whole.height = row.y + row.height - whole.y;
        first = false;
    }
    add_quiet(*column.list, whole, id, name, {}, reason, 0);
}

/// The real automation name, after add_button's stand-in.
void rename(kit::DisplayList& list, kit::ControlId id, std::string name) {
    for (kit::Control& control : list.controls) {
        if (control.id == id)
            control.name = std::move(name);
    }
}

/// Appends a button row. A lock's reason is the next row, not the caption.
void add_button_row(
    Column& column,
    kit::ControlId id,
    const std::string& caption,
    bool accent,
    const std::string& lock,
    kit::ControlId reason_id,
    const char* reason_name
) {
    const kit::Rect row = column.take();
    const bool enabled = lock.empty();
    const int32_t room = std::max(row.width - 10, int32_t{0});
    const std::string fitted = fit_line(*column.fonts, caption, room);
    kit::add_button(
        *column.list,
        row,
        button_look(*column.model, id, fitted, accent, enabled),
        id,
        "photo." + std::to_string(id)
    );
    stamp(*column.list, id, caption, with_reason(caption, lock));
    if (!lock.empty())
        add_reason(column, reason_id, reason_name, lock);
}

/// Appends a slider. The drawn value is the short one. The reason, when
/// there is one, is the next row and is also the control's text.
void add_slider_row(
    Column& column,
    kit::ControlId id,
    const std::string& label,
    int32_t stop,
    int32_t stops,
    const std::string& value,
    const std::string& lock,
    const std::string& note,
    const char* name,
    kit::ControlId reason_id,
    const char* reason_name,
    const std::vector<std::string>& reserve = {},
    bool wrap_reason = false
) {
    const kit::Rect row = column.take();
    const SliderParts parts = slider_parts(*column.fonts, row.width, label, value, reserve);
    const kit::Rect track{row.x + parts.label_w, row.y, parts.track_w, row.height};
    kit::SliderLook look;
    look.stop = stop;
    look.stops = std::max(stops, int32_t{2});
    look.locked = !lock.empty();
    look.hovered = column.model->hot == id;
    kit::add_slider(*column.list, track, look, id, name);
    const std::string reason = !lock.empty() ? lock : note;
    stamp(*column.list, id, label, with_reason(value, reason));
    // A finger's row is taller than the knob. The knob and its track sit at
    // the row's middle, level with the label and the value, and the whole
    // row still takes the finger.
    const int32_t knob_h = kit::compact_metrics.knob_height;
    const int32_t lower =
        column.model->touch ? std::max((row.height - knob_h) / 2, int32_t{0}) : int32_t{0};
    for (kit::Item& item : column.list->items) {
        if (item.control != id || item.role != kit::Role::slider)
            continue;
        item.text = value;
        item.rect.y += lower;
        item.rect.height -= lower;
    }
    const int32_t label_box = std::max(parts.label_w - 4, int32_t{0});
    add_line(
        *column.list,
        {row.x, row.y, label_box, row.height},
        fit_line(*column.fonts, label, label_box)
    );
    const int32_t value_x = track.x + track.width + 4;
    const int32_t value_w = std::max(row.x + row.width - value_x, int32_t{0});
    add_line(
        *column.list, {value_x, row.y, value_w, row.height}, fit_line(*column.fonts, value, value_w)
    );
    if (reason.empty())
        return;
    if (wrap_reason)
        add_wrapped_reason(
            column,
            reason_id,
            reason_name,
            reason,
            wrapped_reason(*column.fonts, reason, column.width)
        );
    else
        add_reason(column, reason_id, reason_name, reason);
}

/// Appends a switch, wide enough for its On and Off words, leaving the label
/// the rest of the row.
void add_switch_row(
    Column& column, kit::ControlId id, const std::string& label, bool on, const char* name
) {
    const kit::Rect row = column.take();
    const int32_t off_w = line_width(*column.fonts, column.model->off_word, 24);
    const int32_t on_w = line_width(*column.fonts, column.model->on_word, 24);
    const int32_t caption = std::max(off_w, on_w);
    int32_t switch_w = std::max(int32_t{88}, caption * 2 + 12);
    const int32_t label_w = line_width(*column.fonts, label, 48);
    constexpr int32_t kGap = 8;
    if (label_w + kGap + switch_w > row.width) {
        const int32_t room = row.width - label_w - kGap;
        if (room >= caption * 2 + 8)
            switch_w = room;
        else {
            const int32_t floor = std::max(row.width / 3, int32_t{1});
            switch_w = std::max(int32_t{48}, std::min(switch_w, floor));
        }
    }
    if (switch_w > row.width)
        switch_w = row.width;
    const int32_t label_box = std::max(row.width - switch_w - kGap, int32_t{0});
    const kit::Rect track{row.x + row.width - switch_w, row.y, switch_w, row.height};
    const int32_t half = std::max(switch_w / 2 - 4, int32_t{1});
    kit::SwitchLook look;
    look.on = on;
    look.hovered = column.model->hot == id;
    look.off_caption = fit_line(*column.fonts, column.model->off_word, half);
    look.on_caption = fit_line(*column.fonts, column.model->on_word, half);
    kit::add_switch(*column.list, track, look, id, name);
    stamp(*column.list, id, label, on ? column.model->on_word : column.model->off_word);
    add_line(
        *column.list,
        {row.x, row.y, label_box, row.height},
        fit_line(*column.fonts, label, label_box)
    );
}

/// Clock and File: words in the section, not a button that does nothing.
void add_readout(
    Column& column,
    kit::ControlId id,
    const std::string& label,
    const std::string& value,
    const char* name
) {
    const kit::Rect row = column.take();
    const std::string shown = value.empty() ? label : label + "  " + value;
    add_line(*column.list, row, fit_line(*column.fonts, shown, row.width));
    add_quiet(*column.list, row, id, name, label, value, 0);
}

/// The label, the stops and the value on one levels row. An empty value
/// takes no column, so Smooth edges does not reserve one.
struct LevelsParts {
    int32_t label_w{0};
    int32_t track_w{0};
    int32_t value_w{0};
};

/// Splits a levels row. The track stays at least kMinTrack while the row
/// allows it. The value is measured on its own.
///
/// @param fonts the fonts
/// @param column_width the row's width
/// @param label the caption
/// @param value the pixels, or empty
/// @return the three widths
LevelsParts levels_parts(
    const kit::Fonts& fonts, int32_t column_width, std::string_view label, std::string_view value
) {
    const bool has_value = !value.empty();
    int32_t label_w = line_width(fonts, label, 48) + 8;
    int32_t value_w = 0;
    if (has_value)
        value_w = line_width(fonts, value, 36) + 8;
    if (label_w < 36)
        label_w = 36;
    if (has_value && value_w < 28)
        value_w = 28;
    int32_t track_w = column_width - label_w - value_w;
    if (column_width >= kMinTrack && track_w < kMinTrack) {
        int32_t need = kMinTrack - track_w;
        const int32_t value_floor = has_value ? 16 : 0;
        const int32_t from_value = std::min(need, std::max(value_w - value_floor, int32_t{0}));
        value_w -= from_value;
        need -= from_value;
        const int32_t from_label = std::min(need, std::max(label_w - 24, int32_t{0}));
        label_w -= from_label;
        track_w = column_width - label_w - value_w;
    }
    if (track_w < 0) {
        label_w = std::max(label_w + track_w, int32_t{0});
        value_w = has_value ? std::max(column_width - label_w, int32_t{0}) : 0;
        track_w = std::max(column_width - label_w - value_w, int32_t{0});
    }
    return {label_w, track_w, value_w};
}

/// A strip of choices. The label and, for Size, the chosen picture's pixels
/// are drawn on the row. The control is the stops alone. Stops from the
/// first locked one on are faded.
///
/// On a touch layout each stop is a finger's target, as wide as a row is
/// tall. The label and the pixels then take a row of their own, and the
/// stops the whole row under them, wrapped onto two rows when they would be
/// narrower than that.
void add_levels_row(
    Column& column,
    kit::ControlId id,
    const char* name,
    const std::string& label,
    const std::string& value,
    const std::string* captions,
    const std::string* locks,
    const char* const* parts,
    int count,
    int32_t chosen
) {
    const int stops = std::max(count, 1);
    const bool touch = column.model->touch;
    kit::Rect row{};
    LevelsParts split{};
    kit::Rect track{};
    int32_t columns = stops;
    if (touch) {
        row = column.take();
        const std::string shown = value.empty() ? label : label + "  " + value;
        add_line(*column.list, row, fit_line(*column.fonts, shown, row.width));
        const kit::Rect first = column.take();
        columns = stop_columns(stops, first.width, column.row_h);
        const int32_t rows = (stops + columns - 1) / columns;
        for (int32_t extra = 1; extra < rows; ++extra)
            column.take();
        const int32_t band_bottom = column.y - column.gap;
        track = {first.x, first.y, first.width, std::max(band_bottom - first.y, first.height)};
    } else {
        row = column.take();
        split = levels_parts(*column.fonts, row.width, label, value);
        track = {row.x + split.label_w, row.y, std::max(split.track_w, int32_t{0}), row.height};
    }
    const bool wrapped = columns < stops;
    const int32_t level_w =
        wrapped ? std::max((track.width - 2) / columns, int32_t{1})
                : std::max((track.width - static_cast<int32_t>(stops - 1)) / stops, int32_t{1});
    const int32_t index = std::clamp(chosen, int32_t{0}, static_cast<int32_t>(stops - 1));
    std::size_t offered = static_cast<std::size_t>(stops);
    for (int stop = 0; stop < stops; ++stop) {
        if (locks[stop].empty())
            continue;
        offered = static_cast<std::size_t>(stop);
        break;
    }
    kit::LevelsLook levels;
    levels.level_width = level_w;
    levels.chosen = static_cast<std::size_t>(index);
    levels.offered = offered;
    levels.hovered = column.model->hot == id;
    if (wrapped) {
        levels.columns = columns;
        levels.row_gap = column.gap;
    }
    const int32_t caption_room = std::max(level_w - 4, int32_t{1});
    for (int stop = 0; stop < stops; ++stop)
        levels.captions.push_back(fit_line(*column.fonts, captions[stop], caption_room));
    kit::add_levels(*column.list, track, levels, id, name);
    stamp(*column.list, id, label, with_reason(captions[index], locks[index]));
    for (kit::Control& control : column.list->controls) {
        if (control.id != id)
            continue;
        control.parts.assign(parts, parts + stops);
    }
    if (touch)
        return;
    const int32_t label_box = std::max(split.label_w - 4, int32_t{0});
    add_line(
        *column.list,
        {row.x, row.y, label_box, row.height},
        fit_line(*column.fonts, label, label_box)
    );
    if (!value.empty()) {
        const int32_t value_x = track.x + track.width + 4;
        const int32_t value_w = std::max(row.x + row.width - value_x, int32_t{0});
        add_line(
            *column.list,
            {value_x, row.y, value_w, row.height},
            fit_line(*column.fonts, value, value_w)
        );
    }
}

/// The toast's width. It grows with the window and stops short of the panel.
/// The width depends only on the window, so covers_chrome and the layout agree.
int32_t toast_width(int32_t window_w, int32_t panel_x) noexcept {
    int32_t want = 304;
    if (window_w >= 1600)
        want = 520;
    else if (window_w >= 1100)
        want = 440;
    const int32_t room = panel_x - kMargin * 2;
    if (want > room)
        want = std::max(room, int32_t{0});
    return want;
}

/// The notice a screen reader says when a photo has just been saved. It is
/// not in the Tab order. A26 reads the polite live region.
void add_saved(Layout& placed, const Model& model) {
    if (!model.toast || placed.toast.width <= 0 || placed.toast.height <= 0)
        return;
    std::string text = model.toast_name;
    if (!model.toast_note.empty()) {
        if (!text.empty())
            text += "  ";
        text += model.toast_note;
    }
    add_quiet(placed.list, placed.toast, kSaved, "photo.saved", {}, std::move(text), 1);
}

/// Look's header, with Depth sharing the row so the page costs no extra row.
void add_look_header(Column& column) {
    const Model& model = *column.model;
    const kit::Rect row = column.take();
    constexpr int32_t kGap = 4;
    int32_t depth_w = line_width(*column.fonts, model.depth_label, 48) + 16;
    // A finger's target is at least as wide as the row is tall.
    if (model.touch)
        depth_w = std::max(depth_w, row.height);
    const int32_t most = std::max(row.width / 2, int32_t{1});
    if (depth_w > most)
        depth_w = most;
    if (depth_w > row.width - kGap)
        depth_w = std::max(row.width - kGap, int32_t{0});
    const int32_t look_w = std::max(row.width - kGap - depth_w, int32_t{0});
    const kit::Rect look_rect{row.x, row.y, look_w, row.height};
    const kit::Rect depth_rect{
        row.x + look_w + kGap, row.y, std::max(row.width - look_w - kGap, int32_t{0}), row.height
    };
    const std::string value = model.look_open ? model.open_word : model.closed_word;
    const std::string shown = value.empty() ? model.look_title : model.look_title + "  " + value;
    const int32_t room = std::max(look_rect.width - 10, int32_t{0});
    kit::add_button(
        *column.list,
        look_rect,
        button_look(model, kLook, fit_line(*column.fonts, shown, room), false, true),
        kLook,
        "photo.section"
    );
    stamp(*column.list, kLook, model.look_title, value);
    for (kit::Control& control : column.list->controls) {
        if (control.id == kLook)
            control.expanded = model.look_open;
    }
    rename(*column.list, kLook, "photo.section.look");
    const int32_t depth_room = std::max(depth_rect.width - 10, int32_t{0});
    kit::add_button(
        *column.list,
        depth_rect,
        button_look(
            model,
            kDepth,
            fit_line(*column.fonts, model.depth_label, depth_room),
            model.depth_page,
            true
        ),
        kDepth,
        "photo.depth"
    );
    stamp(
        *column.list, kDepth, model.depth_label, model.depth_page ? model.on_word : model.off_word
    );
}

/// The tilt-shift and the haze, in place of the grade.
void add_depth_body(Column& column) {
    const Model& model = *column.model;
    add_slider_row(
        column,
        kBandCentre,
        model.band_centre_label,
        model.band_centre_stop,
        model.band_centre_stops,
        model.band_centre_value,
        {},
        {},
        "photo.band-centre",
        kTiltNote,
        nullptr,
        model.band_centre_reserve
    );
    add_slider_row(
        column,
        kBandWidth,
        model.band_width_label,
        model.band_width_stop,
        model.band_width_stops,
        model.band_width_value,
        {},
        {},
        "photo.band-width",
        kTiltNote,
        nullptr,
        model.band_width_reserve
    );
    add_slider_row(
        column,
        kBlur,
        model.blur_label,
        model.blur_stop,
        model.blur_stops,
        model.blur_value,
        {},
        {},
        "photo.blur",
        kTiltNote,
        nullptr,
        model.blur_reserve
    );
    add_wrapped_reason(
        column,
        kTiltNote,
        "photo.tilt-shift.note",
        model.tilt_note,
        wrapped_reason(*column.fonts, model.tilt_note, column.width)
    );
    add_slider_row(
        column,
        kHaze,
        model.haze_label,
        model.haze_stop,
        model.haze_stops,
        model.haze_value,
        model.haze_lock,
        {},
        "photo.haze",
        kHazeNote,
        "photo.haze.reason",
        model.haze_reserve,
        true
    );
    if (model.haze_lock.empty() && !model.haze_note.empty())
        add_wrapped_reason(
            column,
            kHazeNote,
            "photo.haze.note",
            model.haze_note,
            wrapped_reason(*column.fonts, model.haze_note, column.width)
        );
    add_slider_row(
        column,
        kDepthOfField,
        model.depth_of_field_label,
        model.depth_of_field_stop,
        model.depth_of_field_stops,
        model.depth_of_field_value,
        model.depth_of_field_lock,
        {},
        "photo.depth-of-field",
        kDepthNote,
        "photo.depth-of-field.reason",
        model.depth_of_field_reserve,
        true
    );
}

/// A section header, or the head's leave button when it does not fold.
void add_header(
    Column& column, kit::ControlId id, const std::string& title, bool open, bool folds
) {
    const kit::Rect row = column.take();
    const Model& model = *column.model;
    const std::string value = open ? model.open_word : model.closed_word;
    const std::string shown = !folds || value.empty() ? title : title + "  " + value;
    const int32_t room = std::max(row.width - 10, int32_t{0});
    kit::add_button(
        *column.list,
        row,
        button_look(model, id, fit_line(*column.fonts, shown, room), false, true),
        id,
        "photo.section"
    );
    stamp(*column.list, id, folds ? title : model.head, folds ? value : model.head);
    if (!folds)
        return;
    for (kit::Control& control : column.list->controls) {
        if (control.id == id)
            control.expanded = open;
    }
}

/// The sections, in order. Leave and Take photo are left out when the touch
/// layout pins them itself.
///
/// @param[in,out] column the rows
/// @param with_leave true when Leave is the first row
/// @param with_take true when Take photo sits in the Photo section
void fill_sections(Column& column, bool with_leave, bool with_take) {
    const Model& model = *column.model;
    const kit::Fonts& fonts = *column.fonts;
    if (with_leave) {
        add_header(column, kHead, model.head, true, false);
        rename(*column.list, kHead, "photo.leave");
    }

    add_header(column, kCamera, model.camera_title, model.camera_open, true);
    rename(*column.list, kCamera, "photo.section.camera");
    if (model.camera_open) {
        add_slider_row(
            column,
            kZoom,
            model.zoom_label,
            model.zoom_stop,
            model.zoom_stops,
            model.zoom_value,
            model.zoom_lock,
            model.zoom_note,
            "photo.zoom",
            kZoomNote,
            "photo.zoom.note",
            model.zoom_reserve
        );
        add_slider_row(
            column,
            kTurn,
            model.turn_label,
            model.turn_stop,
            model.turn_stops,
            model.turn_value,
            model.turn_lock,
            {},
            "photo.turn",
            kTurnReason,
            "photo.turn.reason"
        );
        add_slider_row(
            column,
            kTilt,
            model.tilt_label,
            model.tilt_stop,
            model.tilt_stops,
            model.tilt_value,
            model.tilt_lock,
            {},
            "photo.tilt",
            kTiltReason,
            "photo.tilt.reason"
        );
        add_button_row(column, kClassic, model.classic_label, false, {}, kHint, nullptr);
        rename(*column.list, kClassic, "photo.classic");
        add_button_row(column, kFollow, model.follow_label, false, {}, kHint, nullptr);
        rename(*column.list, kFollow, "photo.follow");
    }

    add_header(column, kTime, model.time_title, model.time_open, true);
    rename(*column.list, kTime, "photo.section.time");
    if (model.time_open) {
        const kit::Rect modes = column.take();
        const int32_t level_w = std::max((modes.width - 2) / 3, int32_t{1});
        const int32_t chosen = std::clamp(model.time_mode, int32_t{0}, int32_t{2});
        const std::string captions[] = {model.run_label, model.slow_label, model.freeze_caption};
        kit::LevelsLook levels;
        levels.level_width = level_w;
        levels.chosen = static_cast<std::size_t>(chosen);
        levels.offered = 3;
        levels.hovered = model.hot == kRun;
        for (const std::string& caption : captions)
            levels.captions.push_back(fit_line(fonts, caption, std::max(level_w - 4, int32_t{1})));
        kit::add_levels(*column.list, modes, levels, kRun, "photo.time-mode");
        stamp(*column.list, kRun, captions[chosen], with_reason(captions[chosen], model.slow_lock));
        for (kit::Control& control : column.list->controls) {
            if (control.id != kRun)
                continue;
            control.parts = {"run", "slow", "freeze"};
        }
        if (!model.slow_lock.empty())
            add_reason(column, kTimeReason, "photo.time.reason", model.slow_lock);
        add_slider_row(
            column,
            kSpeed,
            model.speed_label,
            model.speed_stop,
            model.speed_stops,
            model.speed_value,
            model.speed_lock,
            {},
            "photo.speed",
            kSpeedReason,
            "photo.speed.reason"
        );
        add_button_row(
            column,
            kStep,
            model.step_label,
            false,
            model.step_lock,
            kStepReason,
            "photo.step.reason"
        );
        rename(*column.list, kStep, "photo.step");
        add_slider_row(
            column,
            kBetween,
            model.between_label,
            model.between_stop,
            model.between_stops,
            model.between_value,
            model.between_lock,
            {},
            "photo.between",
            kBetweenReason,
            "photo.between.reason"
        );
        add_readout(column, kClock, model.clock_label, model.clock, "photo.clock");
    }

    add_look_header(column);
    if (model.look_open && model.depth_page)
        add_depth_body(column);
    if (model.look_open && !model.depth_page) {
        constexpr int kPresetCount = 6;
        int32_t chip_columns =
            std::max(preset_columns(fonts, model.preset_labels, column.width), int32_t{1});
        // On a touch layout each name is a finger's target, as wide as a row
        // is tall, so the names wrap sooner.
        if (model.touch)
            chip_columns = finger_columns(chip_columns, column.width, column.row_h);
        const int32_t chip_rows = (kPresetCount + chip_columns - 1) / chip_columns;
        const kit::Rect first_band = column.take();
        for (int32_t extra = 1; extra < chip_rows; ++extra)
            column.take();
        const int32_t band_bottom = column.y - column.gap;
        const kit::Rect presets{
            first_band.x,
            first_band.y,
            first_band.width,
            std::max(band_bottom - first_band.y, first_band.height),
        };
        const int32_t level_w = std::max((presets.width - 2) / chip_columns, int32_t{1});
        const int32_t chosen = std::clamp(model.preset, int32_t{0}, int32_t{kPresetCount - 1});
        kit::LevelsLook levels;
        levels.level_width = level_w;
        levels.chosen = static_cast<std::size_t>(chosen);
        levels.offered = static_cast<std::size_t>(kPresetCount);
        levels.hovered = model.hot == kPreset;
        levels.columns = chip_columns;
        if (chip_rows > 1)
            levels.row_gap = column.gap;
        for (const std::string& caption : model.preset_labels)
            levels.captions.push_back(caption);
        kit::add_levels(*column.list, presets, levels, kPreset, "photo.look");
        stamp(
            *column.list,
            kPreset,
            model.preset_labels[static_cast<std::size_t>(chosen)],
            model.preset_labels[static_cast<std::size_t>(chosen)]
        );
        for (kit::Control& control : column.list->controls) {
            if (control.id != kPreset)
                continue;
            control.parts = {"none", "1997", "dusk", "arctic", "noir", "bleach"};
        }
        add_slider_row(
            column,
            kVignette,
            model.vignette_label,
            model.vignette_stop,
            model.vignette_stops,
            model.vignette_value,
            {},
            {},
            "photo.vignette",
            kLookRate,
            nullptr,
            model.vignette_reserve
        );
        add_slider_row(
            column,
            kRoundness,
            model.roundness_label,
            model.roundness_stop,
            model.roundness_stops,
            model.roundness_value,
            {},
            {},
            "photo.roundness",
            kLookRate,
            nullptr,
            model.roundness_reserve
        );
        add_slider_row(
            column,
            kGrain,
            model.grain_label,
            model.grain_stop,
            model.grain_stops,
            model.grain_value,
            {},
            {},
            "photo.grain",
            kLookRate,
            nullptr,
            model.grain_reserve
        );
        add_slider_row(
            column,
            kGrainSize,
            model.grain_size_label,
            model.grain_size_stop,
            model.grain_size_stops,
            model.grain_size_value,
            {},
            {},
            "photo.grain-size",
            kLookRate,
            nullptr,
            model.grain_size_reserve
        );
        const std::string frame_caption = model.frame_value.empty()
                                              ? model.frame_label
                                              : model.frame_label + "  " + model.frame_value;
        add_button_row(column, kFrame, frame_caption, false, {}, kHint, nullptr);
        rename(*column.list, kFrame, "photo.frame");
        for (kit::Control& control : column.list->controls) {
            if (control.id != kFrame)
                continue;
            control.label = model.frame_label;
            control.text = model.frame_value;
        }
    }
    if (model.look_open) {
        if (!model.preview_note.empty())
            add_reason(column, kLookRate, "photo.look.rate", model.preview_note);
        add_switch_row(
            column, kSelection, model.selection_label, model.selection, "photo.selection"
        );
        add_switch_row(column, kHealth, model.health_label, model.health, "photo.health");
        const std::string guides_caption = model.guide_value.empty()
                                               ? model.guides_label
                                               : model.guides_label + "  " + model.guide_value;
        add_button_row(column, kGuides, guides_caption, false, {}, kHint, nullptr);
        rename(*column.list, kGuides, "photo.guides");
        for (kit::Control& control : column.list->controls) {
            if (control.id != kGuides)
                continue;
            control.label = model.guides_label;
            control.text = model.guide_value;
        }
    }

    add_header(column, kCapture, model.capture_title, model.capture_open, true);
    rename(*column.list, kCapture, "photo.section.capture");
    if (model.capture_open) {
        std::string size_shown[4];
        const char* size_fallback[] = {"Screen", "2×", "3×", "4×"};
        for (int stop = 0; stop < 4; ++stop)
            size_shown[stop] = model.size_captions[static_cast<std::size_t>(stop)].empty()
                                   ? size_fallback[stop]
                                   : model.size_captions[static_cast<std::size_t>(stop)];
        const char* size_parts[] = {"screen", "2", "3", "4"};
        add_levels_row(
            column,
            kSize,
            "photo.size",
            model.size_label,
            model.size_value,
            size_shown,
            model.size_locks.data(),
            size_parts,
            4,
            model.size_index
        );
        std::string smooth_shown[3];
        const char* smooth_fallback[] = {"Off", "2×", "4×"};
        for (int stop = 0; stop < 3; ++stop)
            smooth_shown[stop] = model.smooth_captions[static_cast<std::size_t>(stop)].empty()
                                     ? smooth_fallback[stop]
                                     : model.smooth_captions[static_cast<std::size_t>(stop)];
        const char* smooth_parts[] = {"off", "2", "4"};
        add_levels_row(
            column,
            kSmooth,
            "photo.smooth",
            model.smooth_label,
            {},
            smooth_shown,
            model.smooth_locks.data(),
            smooth_parts,
            3,
            model.smooth_index
        );
        add_wrapped_reason(
            column,
            kSizeReason,
            "photo.size.reason",
            model.size_reason,
            wrapped_reason(fonts, model.size_reason, column.width)
        );
        add_switch_row(
            column, kCopySwitch, model.copy_switch_label, model.copy_on, "photo.copy-to-clipboard"
        );
        add_switch_row(column, kNames, model.names_label, model.names_on, "photo.player-names");
        add_readout(column, kFile, model.file_label, model.file_name, "photo.file");
        if (with_take) {
            const kit::Rect take = column.take();
            kit::add_button(
                *column.list,
                take,
                button_look(
                    model,
                    kTake,
                    fit_line(fonts, model.take_label, std::max(take.width - 10, 0)),
                    true,
                    true
                ),
                kTake,
                "photo.take"
            );
            stamp(*column.list, kTake, model.take_label, model.take_label);
        }
        add_button_row(column, kCopy, model.copy_label, false, {}, kHint, nullptr);
        rename(*column.list, kCopy, "photo.copy");
    }
}

/// Moves a rectangle down. A clip that is empty stays empty.
kit::Rect shift_down(kit::Rect rect, int32_t down) noexcept {
    if (rect.width > 0 && rect.height > 0)
        rect.y += down;
    return rect;
}

/// Places every body row in the view.
///
/// A row the view cuts stays in the list, clipped to the view, so a finger
/// and the focus can still reach the part that shows. An empty view places
/// nothing: a clip of no area would draw the row with no clip at all.
void place_body(
    kit::DisplayList& into, const kit::DisplayList& body, int32_t scroll, const kit::Rect& view
) {
    if (view.width <= 0 || view.height <= 0)
        return;
    const int32_t down = view.y - scroll;
    for (const kit::Control& control : body.controls) {
        kit::Control copy = control;
        copy.rect = shift_down(copy.rect, down);
        copy.clip = view;
        copy.group = 0;
        into.controls.push_back(std::move(copy));
    }
    into.tab_order.insert(into.tab_order.end(), body.tab_order.begin(), body.tab_order.end());
    for (const kit::Item& item : body.items) {
        kit::Item copy = item;
        copy.rect = shift_down(copy.rect, down);
        copy.clip = view;
        into.items.push_back(std::move(copy));
    }
}

/// The touch layout. Leave and Take photo stay put. One section is open, and
/// the rows between them scroll. `placed` already holds the chips and the key line.
Layout layout_touch(Layout placed, const Model& model, const kit::Fonts& fonts) {
    const int32_t window_w = std::max(model.width, int32_t{1});
    const int32_t window_h = std::max(model.height, int32_t{1});
    const int32_t panel_w = panel_width(model.size_class, window_w);
    const int32_t panel_x = std::max(window_w - kMargin - panel_w, int32_t{0});
    const int32_t row = std::max(model.touch_row, int32_t{44});
    const int32_t gap = std::max(int32_t{2}, row / 11);
    const bool keyline = !model.key_line.empty();
    int32_t bottom_limit = keyline ? window_h - kBottomBand - 4 : window_h - kMargin;
    if (bottom_limit > window_h)
        bottom_limit = window_h;
    int32_t panel_y = kChipBand;
    if (bottom_limit - panel_y < row * 3 + 8)
        panel_y = kMargin;
    if (panel_y >= window_h)
        panel_y = 0;
    int32_t panel_h = std::max(bottom_limit - panel_y, int32_t{0});
    if (panel_y + panel_h > window_h)
        panel_h = std::max(window_h - panel_y, int32_t{0});
    placed.panel = {panel_x, panel_y, panel_w, panel_h};
    // A caption bar raises the key line. Where the panel then reaches it, the
    // line ends before the panel, as on the keyboard layout.
    if (placed.keyline.height > 0 && placed.panel.y + placed.panel.height > placed.keyline.y &&
        panel_x > placed.keyline.x)
        placed.keyline.width = std::max(panel_x - kMargin - placed.keyline.x, int32_t{0});

    const int32_t inner_x = panel_x + kPad;
    const int32_t inner_w = std::max(panel_w - kPad * 2, int32_t{0});
    const int32_t content_top = panel_y + kPad;
    const int32_t content_bottom = panel_y + panel_h - kPad;

    Column pinned;
    pinned.list = &placed.list;
    pinned.model = &model;
    pinned.fonts = &fonts;
    pinned.x = inner_x;
    pinned.width = inner_w;
    pinned.y = content_top;
    pinned.row_h = row;
    pinned.gap = 0;
    add_header(pinned, kHead, model.head, true, false);
    rename(placed.list, kHead, "photo.leave");

    const int32_t view_top = content_top + row + gap;
    int32_t take_y = content_bottom - row;
    if (take_y < view_top)
        take_y = view_top;
    if (take_y + row > panel_y + panel_h)
        take_y = std::max(panel_y + panel_h - row, content_top);
    const kit::Rect take_row{inner_x, take_y, inner_w, row};
    const int32_t view_bottom = take_y - gap;
    const int32_t view_h = std::max(view_bottom - view_top, int32_t{0});

    Model folded = model;
    bool kept = false;
    const auto keep = [&](bool& open) {
        if (open && !kept) {
            kept = true;
            return;
        }
        open = false;
    };
    keep(folded.camera_open);
    keep(folded.time_open);
    keep(folded.look_open);
    keep(folded.capture_open);

    kit::DisplayList body;
    Column column;
    column.list = &body;
    column.model = &folded;
    column.fonts = &fonts;
    column.x = inner_x;
    column.width = inner_w;
    column.y = 0;
    column.row_h = row;
    column.gap = gap;
    fill_sections(column, false, false);
    const int32_t content_h = column.y > gap ? column.y - gap : 0;
    const int32_t max_scroll = std::max(content_h - view_h, int32_t{0});
    int32_t scroll = std::clamp(model.scroll, int32_t{0}, max_scroll);
    kit::ScrollArea rows;
    rows.view = {inner_x, view_top, inner_w, view_h};
    rows.content_height = content_h;
    rows.limit = max_scroll;
    rows.page_step = view_h;
    rows.offset = scroll;
    if (model.reveal != kit::no_control && view_h > 0) {
        const kit::Control* found = nullptr;
        const kit::Control* tail = nullptr;
        for (const kit::Control& control : body.controls) {
            if (control.rect.width <= 0 || control.rect.height <= 0)
                continue;
            tail = &control;
            if (control.id == model.reveal)
                found = &control;
        }
        // The row's top, before scrolling, is its content top plus the view's.
        if (found != nullptr)
            scroll =
                rows.offset_showing(found->rect.y + view_top, found->rect.height, found == tail);
        scroll = std::clamp(scroll, int32_t{0}, max_scroll);
        rows.offset = scroll;
    }
    placed.scroll = scroll;
    placed.rows = rows;
    place_body(placed.list, body, scroll, rows.view);

    kit::add_button(
        placed.list,
        take_row,
        button_look(
            model,
            kTake,
            fit_line(fonts, model.take_label, std::max(take_row.width - 10, 0)),
            true,
            true
        ),
        kTake,
        "photo.take"
    );
    stamp(placed.list, kTake, model.take_label, model.take_label);
    add_saved(placed, model);
    return placed;
}

} // namespace

namespace {

/// Writes `times` wherever the source uses a multiplication sign.
std::string with_times(std::string_view text, std::string_view times) {
    std::string out(text);
    const std::string sign = "\u00d7";
    if (times == sign)
        return out;
    for (std::size_t at = out.find(sign); at != std::string::npos; at = out.find(sign, at))
        out.replace(at, sign.size(), times);
    return out;
}

/// Joins names with commas, and `and_word` before the last.
std::string join_and(const std::vector<std::string>& names, std::string_view and_word) {
    if (names.empty())
        return {};
    if (names.size() == 1)
        return names[0];
    std::string out = names[0];
    for (std::size_t index = 1; index + 1 < names.size(); ++index) {
        out += ", ";
        out += names[index];
    }
    out += ' ';
    out += and_word;
    out += ' ';
    out += names.back();
    return out;
}

} // namespace

ChoiceCopy choice_copy(
    const std::array<ChoiceStop, 4>& sizes,
    const std::array<ChoiceStop, 3>& smooth,
    int size_index,
    int,
    const ChoiceWords& words
) {
    const int at_size = std::clamp(size_index, 0, 3);
    const std::string sign = words.times.empty() ? std::string("x") : std::string(words.times);
    const std::string size_names[4] = {
        with_times(words.screen, sign),
        with_times(words.two, sign),
        with_times(words.three, sign),
        with_times(words.four, sign),
    };
    const std::string smooth_names[3] = {
        with_times(words.off, sign),
        with_times(words.two, sign),
        with_times(words.four, sign),
    };
    ChoiceCopy copy;
    copy.size_label = std::string(words.size);
    copy.smooth_label = std::string(words.smooth);
    for (int index = 0; index < 4; ++index)
        copy.size_captions[static_cast<std::size_t>(index)] = size_names[index];
    for (int index = 0; index < 3; ++index)
        copy.smooth_captions[static_cast<std::size_t>(index)] = smooth_names[index];
    const ChoiceStop& chosen = sizes[static_cast<std::size_t>(at_size)];
    copy.size_value = size_names[at_size] + " " + std::to_string(chosen.width) + sign +
                      std::to_string(chosen.height);

    struct Named {
        bool smooth{false};
        std::string caption{};
    };

    std::vector<std::string> reasons;
    std::vector<std::vector<Named>> groups;
    int locked = 0;
    const auto add = [&](bool smooth_stop, const std::string& caption, const std::string& lock) {
        if (lock.empty())
            return;
        ++locked;
        for (std::size_t index = 0; index < reasons.size(); ++index) {
            if (reasons[index] == lock) {
                groups[index].push_back(Named{smooth_stop, caption});
                return;
            }
        }
        reasons.push_back(lock);
        groups.push_back({Named{smooth_stop, caption}});
    };
    for (int index = 0; index < 4; ++index)
        add(false, size_names[index], sizes[static_cast<std::size_t>(index)].lock);
    for (int index = 0; index < 3; ++index)
        add(true, smooth_names[index], smooth[static_cast<std::size_t>(index)].lock);
    if (locked == 7 && reasons.size() == 1) {
        copy.reason = reasons[0];
        return copy;
    }
    const std::string smooth_label = copy.smooth_label;
    const std::string_view and_word =
        words.and_word.empty() ? std::string_view("and") : words.and_word;
    for (std::size_t group = 0; group < reasons.size(); ++group) {
        bool any_size = false;
        bool any_smooth = false;
        for (const Named& named : groups[group]) {
            any_size = any_size || !named.smooth;
            any_smooth = any_smooth || named.smooth;
        }
        std::vector<std::string> names;
        if (!any_size && any_smooth) {
            for (const Named& named : groups[group])
                names.push_back(named.caption);
            const std::string head = smooth_label + " " + join_and(names, and_word);
            if (!copy.reason.empty())
                copy.reason += ". ";
            copy.reason += head + ": " + reasons[group];
            continue;
        }
        for (const Named& named : groups[group]) {
            if (named.smooth)
                names.push_back(smooth_label + " " + named.caption);
            else
                names.push_back(named.caption);
        }
        if (!copy.reason.empty())
            copy.reason += ". ";
        copy.reason += join_and(names, and_word) + ": " + reasons[group];
    }
    return copy;
}

/// The same step photo.zoom-in takes (app::kZoomWheelFactor). The panel does
/// not include the app header, so the factor is repeated here.
constexpr float kZoomStep = 1.15F;

/// Tells whether two positive zooms are the same stop.
///
/// @param left one zoom
/// @param right the other
/// @return true when they differ by less than a thousandth
bool same_stop(float left, float right) noexcept {
    const float scale = std::max(std::fabs(left), std::fabs(right));
    return std::fabs(left - right) <= scale * 0.001F;
}

std::vector<float> zoom_stops(float least, float most) {
    if (!(least > 0.F) || !(most > 0.F))
        return {1.F};
    if (least > most)
        std::swap(least, most);
    std::vector<float> raw;
    raw.push_back(least);
    raw.push_back(most);
    if (least <= 1.F && most >= 1.F)
        raw.push_back(1.F);
    for (float zoom = 1.F; zoom * kZoomStep <= most && zoom < most; zoom *= kZoomStep) {
        const float next = zoom * kZoomStep;
        if (next >= least && next <= most)
            raw.push_back(next);
    }
    for (float zoom = 1.F; zoom / kZoomStep >= least && zoom > least; zoom /= kZoomStep) {
        const float next = zoom / kZoomStep;
        if (next >= least && next <= most)
            raw.push_back(next);
    }
    std::sort(raw.begin(), raw.end());
    std::vector<float> stops;
    stops.reserve(raw.size());
    for (const float stop : raw) {
        if (!stops.empty() && same_stop(stops.back(), stop)) {
            if (stop == 1.F)
                stops.back() = 1.F;
            continue;
        }
        stops.push_back(stop);
    }
    if (!stops.empty() && same_stop(stops.front(), least))
        stops.front() = least;
    if (!stops.empty() && same_stop(stops.back(), most))
        stops.back() = most;
    if (least <= 1.F && most >= 1.F) {
        for (float& stop : stops) {
            if (same_stop(stop, 1.F))
                stop = 1.F;
        }
    }
    return stops;
}

int32_t zoom_stop_nearest(const std::vector<float>& stops, float zoom) noexcept {
    if (stops.empty() || !(zoom > 0.F))
        return 0;
    int32_t best = 0;
    float best_distance = 0.F;
    bool any = false;
    const float log_zoom = std::log(zoom);
    for (int32_t index = 0; index < static_cast<int32_t>(stops.size()); ++index) {
        const float stop = stops[static_cast<std::size_t>(index)];
        if (!(stop > 0.F))
            continue;
        const float distance = std::fabs(std::log(stop) - log_zoom);
        if (!any || distance < best_distance) {
            best_distance = distance;
            best = index;
            any = true;
        }
    }
    return best;
}

std::string_view zoom_scale_key(float zoom) noexcept {
    if (zoom < 0.25F)
        return words::whole_map;
    if (zoom < 1.F)
        return words::far;
    if (zoom < 2.5F)
        return words::medium;
    if (zoom < 6.F)
        return words::near;
    return words::closest;
}

int32_t panel_width(kit::SizeClass size_class, int32_t window_width) noexcept {
    int32_t want = 248;
    if (size_class == kit::SizeClass::regular)
        want = 300;
    else if (size_class == kit::SizeClass::large)
        want = 320;
    const int32_t room = window_width - kMargin * 2;
    if (want > room)
        want = room > 0 ? room : window_width;
    if (want < 0)
        want = 0;
    return want;
}

/// The toast's rectangle. It starts at the margin, so its right edge is past
/// a band measured from the window's left by the toast's width alone.
///
/// @param window_w the window's width
/// @param window_h the window's height
/// @return the rectangle a toast is drawn in
kit::Rect toast_place(int32_t window_w, int32_t window_h) noexcept {
    const int32_t panel = panel_width(kit::size_class_of(window_w, window_h), window_w);
    const int32_t panel_x = std::max(window_w - kMargin - panel, int32_t{0});
    kit::Rect place;
    place.x = kMargin;
    place.y = std::max(window_h - kBottomBand - kToastH, int32_t{0});
    place.width = toast_width(window_w, panel_x);
    place.height = kToastH - 4;
    return place;
}

namespace {

/// The points from the window's bottom up to a caption bar the key line
/// keeps above, or 0 when the key line stays in the bottom band.
///
/// The bar lies at the bottom of the photo: of the window, or of the crop a
/// guide keeps. A crop that leaves the key line's band free under it keeps
/// the key line there, out of the photo.
///
/// @param model the panel: its caption clearance and its guide
/// @param window_w the window's width
/// @param window_h the window's height
/// @return the points the key line keeps above, or 0
int32_t keyline_clear(const Model& model, int32_t window_w, int32_t window_h) {
    if (model.caption_clear <= 0)
        return 0;
    const kit::Rect crop = crop_bounds(window_w, window_h, model.guide);
    const int32_t under = crop.height > 0 ? window_h - (crop.y + crop.height) : 0;
    if (under >= kBottomBand - 6)
        return 0;
    return model.caption_clear;
}

} // namespace

bool covers_chrome(int x, int y, int width, int height) noexcept {
    if (width <= 0 || height <= 0 || x < 0 || y < 0 || x >= width || y >= height)
        return false;
    if (y < kChipBand)
        return true;
    if (y >= height - kBottomBand)
        return true;
    const int32_t span = panel_width(kit::size_class_of(width, height), width) + kMargin;
    if (x >= width - span)
        return true;
    const kit::Rect toast = toast_place(width, height);
    if (toast.width > 0 && toast.height > 0 && x >= toast.x && x < toast.x + toast.width &&
        y >= toast.y && y < toast.y + toast.height)
        return true;
    return false;
}

Layout layout_of(const Model& model, const kit::Fonts& fonts) {
    Layout placed;
    const int32_t window_w = std::max(model.width, int32_t{1});
    const int32_t window_h = std::max(model.height, int32_t{1});
    const int32_t panel_w = panel_width(model.size_class, window_w);
    const int32_t panel_x = std::max(window_w - kMargin - panel_w, int32_t{0});

    const int32_t key_h = std::min(kBottomBand - 10, window_h);
    int32_t key_y = std::max(window_h - kBottomBand + 6, int32_t{0});
    // A caption bar owns the bottom of the photo. The key line stays under a
    // crop that leaves it room, and otherwise sits above the bar, whether the
    // panel is open or hidden, so the bar can be read.
    const int32_t caption_clear = keyline_clear(model, window_w, window_h);
    if (caption_clear > 0)
        key_y = std::max(window_h - caption_clear - key_h, int32_t{0});
    placed.keyline = {
        kMargin,
        key_y,
        std::max(window_w - kMargin * 2, int32_t{0}),
        key_h,
    };
    // An empty touch key line is not a band: the panel may use the bottom.
    if (model.touch && model.key_line.empty())
        placed.keyline = {};

    int32_t chip_x = kMargin;
    constexpr int32_t chip_h = 22;
    int32_t chip_top = kMargin;
    int32_t chip_bottom = chip_top;
    const int32_t chip_limit = std::max(panel_x - kMargin, chip_x + 32);
    int chip_index = 0;
    for (const std::string& chip : model.chips) {
        if (chip.empty())
            continue;
        int32_t chip_w = kit::text_width(fonts, kit::FontRole::small, chip) + 16;
        chip_w = std::clamp(chip_w, int32_t{36}, int32_t{160});
        if (chip_x + chip_w > chip_limit)
            break;
        if (chip_index < static_cast<int>(placed.chip_box.size()))
            placed.chip_box[static_cast<std::size_t>(chip_index)] = {
                chip_x, chip_top, chip_w, chip_h
            };
        ++chip_index;
        chip_x += chip_w + 4;
        chip_bottom = chip_top + chip_h;
    }
    if (chip_bottom > chip_top)
        placed.chips = {kMargin, chip_top, chip_x - kMargin - 4, chip_h};
    if (!model.alerts.empty()) {
        placed.alerts = {
            kMargin,
            chip_bottom > chip_top ? chip_bottom + 4 : chip_top,
            std::max(chip_limit - kMargin, int32_t{0}),
            20,
        };
    }

    if (model.toast) {
        placed.toast = toast_place(window_w, window_h);
        // With a caption bar the toast sits above both the key line and the
        // bar, as far above them as it sits above the key line without one.
        if (model.caption_clear > 0) {
            // A touch layout with no key line has no band: the bar alone counts.
            const int32_t key_top = placed.keyline.height > 0 ? placed.keyline.y : window_h;
            const int32_t above = std::min(key_top, window_h - model.caption_clear);
            placed.toast.y = std::max(above - kToastH - 6, int32_t{0});
        }
    }

    if (!model.shown) {
        int32_t hint_w = line_width(fonts, model.hint_label, 140) + 16;
        if (hint_w > panel_w)
            hint_w = panel_w;
        if (hint_w < 1)
            hint_w = panel_w;
        int32_t hint_h = 28;
        if (model.touch)
            hint_h = std::max(model.touch_row, int32_t{44});
        if (hint_h > window_h - kMargin)
            hint_h = std::max(window_h - kMargin, int32_t{0});
        placed.hint = {panel_x + panel_w - hint_w, kMargin, hint_w, hint_h};
        kit::add_button(
            placed.list,
            placed.hint,
            button_look(
                model,
                kHint,
                fit_line(fonts, model.hint_label, std::max(hint_w - 10, int32_t{0})),
                false,
                true
            ),
            kHint,
            "photo.hint"
        );
        stamp(placed.list, kHint, model.hint_label, model.hint_label);
        add_saved(placed, model);
        return placed;
    }

    if (model.touch)
        return layout_touch(std::move(placed), model, fonts);

    int32_t panel_y = kMargin;
    const int32_t kept_bottom = std::max(kBottomBand, caption_clear);
    int32_t panel_h = window_h - kept_bottom - panel_y - 4;
    if (panel_h < 32)
        panel_h = std::max(window_h - panel_y, int32_t{0});

    const int32_t column_w = std::max(panel_w - kPad * 2, int32_t{0});
    const int preset_rows =
        model.look_open ? preset_band_rows(fonts, model.preset_labels, column_w) : 1;
    const int size_reason_rows =
        model.capture_open
            ? static_cast<int>(wrapped_reason(fonts, model.size_reason, column_w).size())
            : 0;
    int depth_note_rows = 0;
    if (model.look_open && model.depth_page) {
        const auto lines = [&](const std::string& text) {
            if (text.empty())
                return 0;
            return static_cast<int>(wrapped_reason(fonts, text, column_w).size());
        };
        depth_note_rows = lines(model.tilt_note);
        if (model.haze_lock.empty())
            depth_note_rows += lines(model.haze_note);
        else
            depth_note_rows += lines(model.haze_lock);
        depth_note_rows += lines(model.depth_of_field_lock);
    }
    const int rows = row_count(model, preset_rows, size_reason_rows, depth_note_rows);
    const auto rows_need = [&](int32_t height, int32_t between) {
        const int32_t gaps = rows > 1 ? (rows - 1) * between : 0;
        return rows * height + gaps;
    };
    const auto row_room = [](int32_t height) { return std::max(height - kPad * 2, int32_t{0}); };
    // The look's rows do not fit above the key line at 640x480. The panel
    // then grows into that band, and a row may be 10 points. A caption bar
    // keeps the bottom of the picture, so the panel stops above it while its
    // rows still fit there; otherwise it grows over the bar's right end.
    if (rows > 0 && rows_need(12, 1) > row_room(panel_h)) {
        int32_t floor_y = window_h - kMargin;
        if (caption_clear > 0 && rows_need(10, 1) <= row_room(window_h - caption_clear - panel_y))
            floor_y = window_h - caption_clear;
        const int32_t grown = std::max(floor_y - panel_y, int32_t{0});
        if (grown > panel_h)
            panel_h = grown;
    }
    placed.panel = {panel_x, panel_y, panel_w, panel_h};
    if (placed.panel.y + placed.panel.height > placed.keyline.y && panel_x > placed.keyline.x) {
        const int32_t end = panel_x - kMargin;
        placed.keyline.width = std::max(end - placed.keyline.x, int32_t{0});
    }

    const int32_t top = panel_y + kPad;
    const int32_t bottom = panel_y + panel_h - kPad;
    const int32_t room = std::max(bottom - top, int32_t{0});
    int32_t row_h = 16;
    int32_t gap = 2;
    const auto fits = [&](int32_t height, int32_t between) {
        return rows_need(height, between) <= room;
    };
    if (fits(24, 3)) {
        row_h = 24;
        gap = 3;
    } else if (fits(18, 2)) {
        row_h = 18;
        gap = 2;
    } else if (fits(16, 1)) {
        row_h = 16;
        gap = 1;
    } else if (rows > 0) {
        gap = 1;
        const int32_t gaps = rows > 1 ? rows - 1 : 0;
        row_h = (room - gaps) / rows;
        // With every section open at 640x480, a shared match's reasons and
        // the photo's size rows can leave less than 10 points a row. The gap
        // between rows then closes before a row would fall out of the panel.
        if (row_h < 10) {
            gap = 0;
            row_h = std::max(room / rows, int32_t{10});
        }
    }

    Column column;
    column.list = &placed.list;
    column.model = &model;
    column.fonts = &fonts;
    column.x = panel_x + kPad;
    column.width = std::max(panel_w - kPad * 2, int32_t{0});
    column.y = top;
    column.row_h = row_h;
    column.gap = gap;

    fill_sections(column, true, true);

    add_saved(placed, model);
    return placed;
}

} // namespace oa::ui::photo_panel

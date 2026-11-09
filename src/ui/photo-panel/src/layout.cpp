// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Where the photo panel, its chips, its key line and its toast lie.

#include "oa/ui/photo_panel/layout.hpp"

#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/looks.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/kit/theme.hpp"
#include "oa/ui/photo_panel/words.hpp"

#include <algorithm>
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
/// @return the extra rows
int reason_rows(const Model& model) {
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
    return extra;
}

/// How many rows the open panel lays out.
///
/// @param model the panel
/// @return the rows
int row_count(const Model& model) {
    int rows = 1;
    rows += 1;
    if (model.camera_open)
        rows += 5;
    rows += 1;
    if (model.time_open)
        rows += 5;
    rows += 1;
    if (model.look_open)
        rows += 3;
    rows += 1;
    if (model.capture_open)
        rows += 6;
    rows += reason_rows(model);
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
    const std::vector<std::string>& reserve = {}
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
    for (kit::Item& item : column.list->items) {
        if (item.control == id && item.role == kit::Role::slider)
            item.text = value;
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
    if (!reason.empty())
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

/// Clock, Size and File: words in the section, not a button that does nothing.
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

} // namespace

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

    placed.keyline = {
        kMargin,
        std::max(window_h - kBottomBand + 6, int32_t{0}),
        std::max(window_w - kMargin * 2, int32_t{0}),
        std::min(kBottomBand - 10, window_h),
    };

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

    if (model.toast)
        placed.toast = toast_place(window_w, window_h);

    if (!model.shown) {
        int32_t hint_w = line_width(fonts, model.hint_label, 140) + 16;
        if (hint_w > panel_w)
            hint_w = panel_w;
        if (hint_w < 1)
            hint_w = panel_w;
        placed.hint = {panel_x + panel_w - hint_w, kMargin, hint_w, 28};
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

    int32_t panel_y = kMargin;
    int32_t panel_h = window_h - kBottomBand - panel_y - 4;
    if (panel_h < 32)
        panel_h = std::max(window_h - panel_y, int32_t{0});
    placed.panel = {panel_x, panel_y, panel_w, panel_h};

    const int rows = row_count(model);
    const int32_t top = panel_y + kPad;
    const int32_t bottom = panel_y + panel_h - kPad;
    const int32_t room = std::max(bottom - top, int32_t{0});
    int32_t row_h = 16;
    int32_t gap = 2;
    const auto fits = [&](int32_t height, int32_t between) {
        const int32_t gaps = rows > 1 ? (rows - 1) * between : 0;
        return rows * height + gaps <= room;
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
        if (row_h < 12)
            row_h = 12;
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

    const auto header = [&](kit::ControlId id, const std::string& title, bool open, bool folds) {
        const kit::Rect row = column.take();
        const std::string value = open ? model.open_word : model.closed_word;
        const std::string shown = !folds || value.empty() ? title : title + "  " + value;
        const int32_t room = std::max(row.width - 10, int32_t{0});
        kit::add_button(
            placed.list,
            row,
            button_look(model, id, fit_line(fonts, shown, room), false, true),
            id,
            "photo.section"
        );
        stamp(placed.list, id, folds ? title : model.head, folds ? value : model.head);
        if (!folds)
            return;
        for (kit::Control& control : placed.list.controls) {
            if (control.id == id)
                control.expanded = open;
        }
    };
    const auto named = [&](kit::ControlId id, const char* name) { rename(placed.list, id, name); };

    header(kHead, model.head, true, false);
    named(kHead, "photo.leave");

    header(kCamera, model.camera_title, model.camera_open, true);
    named(kCamera, "photo.section.camera");
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
        named(kClassic, "photo.classic");
        add_button_row(column, kFollow, model.follow_label, false, {}, kHint, nullptr);
        named(kFollow, "photo.follow");
    }

    header(kTime, model.time_title, model.time_open, true);
    named(kTime, "photo.section.time");
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
        kit::add_levels(placed.list, modes, levels, kRun, "photo.time-mode");
        stamp(placed.list, kRun, captions[chosen], with_reason(captions[chosen], model.slow_lock));
        for (kit::Control& control : placed.list.controls) {
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
        named(kStep, "photo.step");
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

    header(kLook, model.look_title, model.look_open, true);
    named(kLook, "photo.section.look");
    if (model.look_open) {
        add_switch_row(
            column, kSelection, model.selection_label, model.selection, "photo.selection"
        );
        add_switch_row(column, kHealth, model.health_label, model.health, "photo.health");
        const std::string guides_caption = model.guide_value.empty()
                                               ? model.guides_label
                                               : model.guides_label + "  " + model.guide_value;
        add_button_row(column, kGuides, guides_caption, false, {}, kHint, nullptr);
        named(kGuides, "photo.guides");
        for (kit::Control& control : placed.list.controls) {
            if (control.id != kGuides)
                continue;
            control.label = model.guides_label;
            control.text = model.guide_value;
        }
    }

    header(kCapture, model.capture_title, model.capture_open, true);
    named(kCapture, "photo.section.capture");
    if (model.capture_open) {
        add_readout(column, kSize, model.size_label, model.size_value, "photo.size");
        add_switch_row(
            column, kCopySwitch, model.copy_switch_label, model.copy_on, "photo.copy-to-clipboard"
        );
        add_switch_row(column, kNames, model.names_label, model.names_on, "photo.player-names");
        add_readout(column, kFile, model.file_label, model.file_name, "photo.file");
        const kit::Rect take = column.take();
        kit::add_button(
            placed.list,
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
        stamp(placed.list, kTake, model.take_label, model.take_label);
        add_button_row(column, kCopy, model.copy_label, false, {}, kHint, nullptr);
        named(kCopy, "photo.copy");
    }

    add_saved(placed, model);
    return placed;
}

} // namespace oa::ui::photo_panel

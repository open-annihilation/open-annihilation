// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "frame_stats_panel.hpp"

#include "oa/ui/hud/health_bar.hpp"

#include <algorithm>
#include <cstddef>

namespace oa::app::frame_stats_panel {

using frame_pacing::FrameStatsRowKind;
using frame_pacing::FrameStatsText;
using frame_pacing::kFrameStatsValueColumns;
using frame_pacing::TimeSeverity;

namespace {

/// Returns a text's width, 0 for an empty one.
///
/// @param measure the widths of the table's texts
/// @param view the text
/// @return source pixels
int text_width(const TextWidthHooks& measure, std::string_view view) {
    if (view.empty() || measure.width == nullptr)
        return 0;
    return std::max(0, measure.width(measure.context, view));
}

/// Returns a table text's width, 0 for an empty one.
///
/// @param measure the widths of the table's texts
/// @param text the text
/// @return source pixels
int text_width(const TextWidthHooks& measure, const FrameStatsText& text) {
    return text_width(measure, text.view());
}

/// Returns how many bytes of a text's start fit in a width without ending
/// inside a character (frame_pacing::whole_characters).
///
/// @param measure the widths of the table's texts
/// @param text the text
/// @param room source pixels the start may take
/// @return bytes of the longest start that fits; 0 when none does
std::size_t fitting_bytes(const TextWidthHooks& measure, std::string_view text, int room) {
    std::size_t fits = 0;
    for (std::size_t end = 1; end <= text.size(); ++end) {
        const auto start = frame_pacing::whole_characters(text, end);
        if (start.size() != end)
            continue;
        if (text_width(measure, start) > room)
            break;
        fits = end;
    }
    return fits;
}

} // namespace

uint8_t severity_slot(TimeSeverity severity, uint8_t ungraded) noexcept {
    switch (severity) {
    case TimeSeverity::within_frame:
        return kWithinFrameSlot;
    case TimeSeverity::within_tick:
        return static_cast<uint8_t>(oa::ui::hud::kHealthMidColor);
    case TimeSeverity::over_tick:
        return static_cast<uint8_t>(oa::ui::hud::kHealthLowColor);
    case TimeSeverity::none:
        break;
    }
    return ungraded;
}

PanelLayout lay_out_panel(
    const frame_pacing::FrameStatsTable& table, const TextWidthHooks& measure, int row_height
) noexcept {
    PanelLayout layout{};
    layout.row_height = row_height;
    const std::size_t rows = std::min(table.row_count, table.rows.size());
    // The columns' widths, then where they end with the labels at 0. The
    // title runs across the columns and sets none of them; the rows that
    // run on set nothing, as their texts are fitted where they are drawn.
    int labels = 0;
    int title = 0;
    std::array<int, kFrameStatsValueColumns> widths{};
    for (std::size_t row = 0; row < rows; ++row) {
        const auto& cells = table.rows[row];
        if (cells.kind == FrameStatsRowKind::title) {
            title = std::max(title, text_width(measure, cells.label));
            continue;
        }
        if (frame_pacing::runs_on(cells.kind))
            continue;
        labels = std::max(labels, text_width(measure, cells.label));
        for (std::size_t column = 0; column < kFrameStatsValueColumns; ++column)
            widths[column] = std::max(widths[column], text_width(measure, cells.values[column]));
    }
    std::array<int, kFrameStatsValueColumns> ends{};
    int end = labels;
    for (std::size_t column = 0; column < kFrameStatsValueColumns; ++column) {
        end += kColumnGap + widths[column];
        ends[column] = end;
    }
    // Each row's note follows the last value it shows; the widest row sets
    // the table's width.
    std::array<int, frame_pacing::kFrameStatsRowsMost> notes{};
    int table_width = title;
    for (std::size_t row = 0; row < rows; ++row) {
        const auto& cells = table.rows[row];
        if (cells.kind == FrameStatsRowKind::title || frame_pacing::runs_on(cells.kind))
            continue;
        int last = labels;
        for (std::size_t column = 0; column < kFrameStatsValueColumns; ++column)
            if (!cells.values[column].view().empty())
                last = ends[column];
        notes[row] = last + kColumnGap;
        const int note = text_width(measure, cells.note);
        table_width = std::max(table_width, note != 0 ? notes[row] + note : last);
    }
    const int graph_width = static_cast<int>(frame_pacing::kFrameGraphColumns) * kBarWidth;
    const int framed_graph = graph_width + 2 * kGraphEdge;
    const int content = std::max(table_width, framed_graph);
    const int inner = kBevel + kPadding;
    // A graph wider than the table moves the values and notes right by the
    // difference, keeping the labels at the left.
    const int shift = content - table_width;
    layout.label_x = inner;
    layout.rule_width = content;
    for (std::size_t column = 0; column < kFrameStatsValueColumns; ++column)
        layout.value_right[column] = inner + ends[column] + shift;
    // The rows, the ones under the column names below the rule and its gap.
    layout.rule_y = -1;
    int y = inner;
    for (std::size_t row = 0; row < rows; ++row) {
        layout.row_y[row] = y;
        layout.note_x[row] =
            frame_pacing::runs_on(table.rows[row].kind) ? inner : inner + notes[row] + shift;
        y += row_height;
        if (table.rows[row].kind == FrameStatsRowKind::heading && layout.rule_y < 0) {
            layout.rule_y = y;
            y += 1 + kRuleGap;
        }
    }
    layout.graph.x = inner + content - framed_graph + kGraphEdge;
    layout.graph.y = y + kGraphGap + kGraphEdge;
    layout.graph.width = graph_width;
    layout.graph.height = kGraphHeight;
    layout.width = 2 * inner + content;
    layout.height = layout.graph.y + kGraphHeight + kGraphEdge + inner;
    return layout;
}

RunOnFit fit_run_on_row(
    const frame_pacing::FrameStatsRow& row, const TextWidthHooks& measure, const PanelLayout& layout
) noexcept {
    const int right = layout.width - kBevel - kPadding;
    const auto label = row.label.view();
    const auto note = row.note.view();
    RunOnFit fit{};
    fit.label_bytes = fitting_bytes(measure, label, right - layout.label_x);
    fit.note_x =
        layout.label_x + text_width(measure, label.substr(0, fit.label_bytes)) + kColumnGap;
    if (fit.label_bytes == label.size())
        fit.note_bytes = fitting_bytes(measure, note, right - fit.note_x);
    return fit;
}

oa::ui::display_layout::Rect
battlefield_quarter(const oa::ui::display_layout::MatchLayout& match) noexcept {
    const int half_width = match.battlefield_width() / 2;
    const int half_height = match.battlefield_height() / 2;
    return {
        match.battlefield_x() + half_width,
        match.battlefield_y() + half_height,
        match.battlefield_width() - half_width,
        match.battlefield_height() - half_height
    };
}

PanelPlace place_panel(
    const PanelLayout& layout, const oa::ui::display_layout::MatchLayout& match, int text_scale
) noexcept {
    const auto quarter = battlefield_quarter(match);
    int scale = std::max(text_scale, 1);
    while (scale > 1 && ((layout.width + kInset) * scale > quarter.width ||
                         (layout.height + kInset) * scale > quarter.height))
        --scale;
    PanelPlace place{};
    place.scale = scale;
    place.panel.width = layout.width * scale;
    place.panel.height = layout.height * scale;
    place.panel.x =
        match.battlefield_x() + match.battlefield_width() - kInset * scale - place.panel.width;
    place.panel.y = match.bottom_bar_y() - kInset * scale - place.panel.height;
    return place;
}

int bar_height(uint64_t frame_ns) noexcept {
    const uint64_t capped = std::min(frame_ns, kGraphTopNs);
    const uint64_t rounded = (capped * kGraphHeight + kGraphTopNs / 2) / kGraphTopNs;
    return static_cast<int>(std::max<uint64_t>(rounded, 1));
}

int line_rise(uint64_t time_ns) noexcept {
    return bar_height(time_ns) + 1;
}

} // namespace oa::app::frame_stats_panel

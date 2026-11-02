// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The "+stats" overlay's panel laid out over texts of known widths: the
// title, the column names over a rule, labels at the left, values
// right-aligned in columns as wide as their widest text, each note after
// its row's last value, the columns moved right together when the graph is
// the wider, the graph under the table, and the same layout whatever the
// figures, and a row taller with the turned view's graphics device; its
// place and scale on windows of several sizes; the height of each frame's
// bar and of the graph's lines; and each grade's colour.
#include "frame_stats_panel.hpp"

#include "oa/app/frame_pacing.hpp"
#include "oa/ui/display_layout.hpp"
#include "oa/ui/hud/health_bar.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <string_view>
#include <utility>

namespace {

int failures = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);     \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

namespace frame_pacing = oa::app::frame_pacing;
namespace panel = oa::app::frame_stats_panel;
using frame_pacing::kNanosecondsPerMillisecond;

/// Source pixels from one row to the next in these tests.
constexpr int kRowHeight = 13;
/// Source pixels across the graph and its edge: one bar for each column of
/// the history.
constexpr int kFramedGraph =
    static_cast<int>(frame_pacing::kFrameGraphColumns) * panel::kBarWidth + 2 * panel::kGraphEdge;
/// Source pixels between the panel's outside and its contents.
constexpr int kInner = panel::kBevel + panel::kPadding;

/// Measures every character as a given number of pixels wide.
///
/// @param context the pixels a character, an int
/// @param text the text
/// @return the text's width
int fixed_width(void* context, std::string_view text) {
    return static_cast<int>(text.size()) * *static_cast<const int*>(context);
}

/// The first character console_width has a width for: the space.
constexpr unsigned char kFirstPrintable = ' ';
/// The widths, in pixels, of the match label font's glyphs
/// (fonts/CONSOLE.FNT) for the printable ASCII characters, from the space
/// to the tilde: digits are six pixels wide, and letters from four (i, l)
/// to eight (A, T, V to Y, m, v, w, y).
constexpr std::array<uint8_t, 95> kConsoleWidths{
    7, 2, 5, 7, 6, 8, 8, 2, 5, 5, 6, 6, 3, 6, 3, 8, 6, 6, 6, 6, 6, 6, 6, 6, 6, 6, 3, 3, 6, 8, 6, 6,
    8, 8, 6, 7, 6, 6, 6, 7, 6, 6, 5, 6, 6, 7, 6, 7, 6, 8, 7, 6, 8, 6, 8, 8, 8, 8, 7, 4, 8, 4, 8, 8,
    3, 7, 6, 6, 6, 6, 7, 6, 6, 4, 5, 6, 4, 8, 6, 6, 6, 6, 6, 5, 6, 6, 8, 8, 7, 8, 6, 5, 2, 5, 8
};
/// The widest of the font's glyphs, which console_width gives every byte
/// outside printable ASCII.
constexpr int kWidestGlyph = 8;

/// Measures a text in the widths of the match label font's glyphs
/// (kConsoleWidths), every other byte as kWidestGlyph.
///
/// @param text the text
/// @return the text's width
int console_width(void*, std::string_view text) {
    int width = 0;
    for (const char glyph : text) {
        // A byte below the space wraps past the table's end.
        const std::size_t code =
            static_cast<std::size_t>(static_cast<unsigned char>(glyph)) - kFirstPrintable;
        width += code < kConsoleWidths.size() ? kConsoleWidths[code] : kWidestGlyph;
    }
    return width;
}

/// Writes a text into one of a table's texts.
///
/// @param[out] out the table's text
/// @param text the text, shorter than the text's bytes
void put(frame_pacing::FrameStatsText& out, std::string_view text) {
    out.text = {};
    for (std::size_t at = 0; at < text.size(); ++at)
        out.text[at] = text[at];
}

/// Adds a row to a table.
///
/// @param[in,out] table the table
/// @param kind what the row shows
/// @param label the row's label
/// @param least its first value column; empty for none
/// @param mean its second
/// @param most its third
/// @param note its note; empty for none
void add_row(
    frame_pacing::FrameStatsTable& table,
    frame_pacing::FrameStatsRowKind kind,
    std::string_view label,
    std::string_view least,
    std::string_view mean,
    std::string_view most,
    std::string_view note
) {
    auto& row = table.rows[table.row_count++];
    row.kind = kind;
    put(row.label, label);
    put(row.values[0], least);
    put(row.values[1], mean);
    put(row.values[2], most);
    put(row.note, note);
}

// Six pixels a character: the graph is wider than the table, so the values
// and notes move right until the widest row ends at the padding; the title
// runs across the columns, and the rows under the column names sit below
// the rule.
void test_columns_under_a_wider_graph() {
    frame_pacing::FrameStatsTable table{};
    add_row(table, frame_pacing::FrameStatsRowKind::title, "Frame stats (ms)", "", "", "", "");
    add_row(table, frame_pacing::FrameStatsRowKind::heading, "", "min", "avg", "max", "");
    add_row(
        table,
        frame_pacing::FrameStatsRowKind::measure,
        "present",
        "10.00",
        "8.33",
        "123.45",
        "30/s"
    );
    add_row(table, frame_pacing::FrameStatsRowKind::rate, "FPS", "", "120", "", "limit 120");
    add_row(table, frame_pacing::FrameStatsRowKind::count, "units", "", "", "", "40 drawn");
    int pixels = 6;
    const panel::TextWidthHooks measure{&pixels, fixed_width};
    const auto layout = panel::lay_out_panel(table, measure, kRowHeight);
    // Labels 42 wide ("present"; the title sets no column); columns 30, 24
    // and 36 wide; the widest row ends with the 24 of "30/s", and the graph
    // is wider, so everything right of the labels moves right by the
    // difference.
    constexpr int kGap = panel::kColumnGap;
    constexpr std::array<int, 3> kEnds{42 + kGap + 30, 42 + 2 * kGap + 54, 42 + 3 * kGap + 90};
    constexpr int kWidest = kEnds[2] + kGap + 24;
    constexpr int kShift = kFramedGraph - kWidest;
    static_assert(kShift > 0);
    CHECK(layout.label_x == kInner);
    for (std::size_t column = 0; column < kEnds.size(); ++column)
        CHECK(layout.value_right[column] == kInner + kEnds[column] + kShift);
    // Notes after the last value each row shows, or after the labels.
    CHECK(layout.note_x[2] == layout.value_right[2] + kGap);
    CHECK(layout.note_x[3] == layout.value_right[1] + kGap);
    CHECK(layout.note_x[4] == kInner + 42 + kGap + kShift);
    // The widest row ends at the padding.
    CHECK(layout.note_x[2] + 4 * pixels == layout.width - kInner);
    CHECK(layout.width == 2 * kInner + kFramedGraph);
    CHECK(layout.rule_width == kFramedGraph);
    // The values in a column end together, the widest starting a gap after
    // the column before.
    CHECK(layout.value_right[1] - 4 * pixels - panel::kColumnGap >= layout.value_right[0]);
    CHECK(layout.value_right[2] - 6 * pixels - panel::kColumnGap == layout.value_right[1]);
    // The title and the column names a row apart; the rule a row under the
    // names, and the next row the rule and its gap below that.
    CHECK(layout.row_height == kRowHeight);
    CHECK(layout.row_y[0] == kInner);
    CHECK(layout.row_y[1] == kInner + kRowHeight);
    CHECK(layout.rule_y == kInner + 2 * kRowHeight);
    CHECK(layout.row_y[2] == layout.rule_y + 1 + panel::kRuleGap);
    CHECK(layout.row_y[3] == layout.row_y[2] + kRowHeight);
    CHECK(layout.row_y[4] == layout.row_y[3] + kRowHeight);
    // The graph fills the width under the rows.
    CHECK(layout.graph.x == kInner + panel::kGraphEdge);
    CHECK(layout.graph.y == layout.row_y[4] + kRowHeight + panel::kGraphGap + panel::kGraphEdge);
    CHECK(layout.graph.width == static_cast<int>(frame_pacing::kFrameGraphColumns));
    CHECK(layout.graph.height == panel::kGraphHeight);
    CHECK(layout.height == layout.graph.y + panel::kGraphHeight + panel::kGraphEdge + kInner);
}

// Twenty pixels a character: the table is wider than the graph, which ends
// at the right padding; nothing moves. A table with no column names has no
// rule.
void test_graph_under_a_wider_table() {
    frame_pacing::FrameStatsTable table{};
    add_row(table, frame_pacing::FrameStatsRowKind::measure, "frame", "8.00", "8.50", "9.00", "");
    add_row(
        table, frame_pacing::FrameStatsRowKind::measure, "tick", "2.00", "3.00", "4.00", "30/s"
    );
    int pixels = 20;
    const panel::TextWidthHooks measure{&pixels, fixed_width};
    const auto layout = panel::lay_out_panel(table, measure, kRowHeight);
    const int labels = 5 * pixels;
    const int column = 4 * pixels;
    CHECK(layout.value_right[0] == kInner + labels + panel::kColumnGap + column);
    CHECK(layout.value_right[2] == kInner + labels + 3 * (panel::kColumnGap + column));
    const int table_width = labels + 3 * (panel::kColumnGap + column) + panel::kColumnGap + column;
    CHECK(table_width > kFramedGraph);
    CHECK(layout.width == 2 * kInner + table_width);
    CHECK(layout.graph.x + layout.graph.width + panel::kGraphEdge == layout.width - kInner);
    CHECK(layout.rule_y < 0);
    CHECK(layout.row_y[1] == kInner + kRowHeight);
}

// The renderer row sets no width: the panel is as wide as without it,
// however long its texts. Its label runs on from the labels' left edge and
// its note a gap after the label, each cut where it would pass the panel's
// padding, and the note left out when the label is cut.
void test_renderer_row_sets_no_width() {
    frame_pacing::FrameStatsTable table{};
    add_row(table, frame_pacing::FrameStatsRowKind::measure, "frame", "8.00", "8.50", "9.00", "");
    frame_pacing::FrameStatsTable alone = table;
    add_row(
        table, frame_pacing::FrameStatsRowKind::renderer, "standard: metal", "", "", "", "Apple M2"
    );
    constexpr int kGap = panel::kColumnGap;
    int pixels = 6;
    const panel::TextWidthHooks measure{&pixels, fixed_width};
    // At six pixels a character the graph sets the width, and the whole row
    // fits: the label 90 wide, and the note 48 after a gap.
    auto layout = panel::lay_out_panel(table, measure, kRowHeight);
    auto without = panel::lay_out_panel(alone, measure, kRowHeight);
    CHECK(layout.width == 2 * kInner + kFramedGraph);
    CHECK(layout.width == without.width);
    CHECK(layout.value_right == without.value_right);
    CHECK(layout.note_x[1] == layout.label_x);
    auto fit = panel::fit_run_on_row(table.rows[1], measure, layout);
    CHECK(fit.label_bytes == 15);
    CHECK(fit.note_x == kInner + 90 + kGap);
    CHECK(fit.note_bytes == 8);
    // At twenty the measure row sets the width, 100 + 3 * (8 + 80) = 364:
    // the label, 300, fits, and two characters of the note in the 56 left.
    pixels = 20;
    layout = panel::lay_out_panel(table, measure, kRowHeight);
    without = panel::lay_out_panel(alone, measure, kRowHeight);
    constexpr int kTable = 5 * 20 + 3 * (kGap + 4 * 20);
    static_assert(kTable > kFramedGraph);
    CHECK(layout.width == 2 * kInner + kTable);
    CHECK(layout.width == without.width);
    fit = panel::fit_run_on_row(table.rows[1], measure, layout);
    CHECK(fit.label_bytes == 15);
    CHECK(fit.note_x == kInner + 300 + kGap);
    CHECK(fit.note_bytes == 2);
    // A label wider than the panel: 18 characters fit in 364, and no note.
    put(table.rows[1].label, "standard: a-render-driv");
    fit = panel::fit_run_on_row(table.rows[1], measure, layout);
    CHECK(fit.label_bytes == 18);
    CHECK(fit.note_x == kInner + 18 * 20 + kGap);
    CHECK(fit.note_bytes == 0);
}

// In the match label font, whose capitals are up to two pixels wider than
// its digits, the renderer row never passes the panel's padding, and the
// panel laid out from the widest table is as wide as without that row: a
// short adapter's name shows whole; a long one, or one of capitals, is cut
// where one more character would pass the padding; one of two-byte
// characters is cut between them.
void test_renderer_row_fits_the_panel() {
    const panel::TextWidthHooks measure{nullptr, console_width};
    const auto widest = frame_pacing::frame_stats_widest_table();
    const auto layout = panel::lay_out_panel(widest, measure, kRowHeight);
    // Without the renderer and display rows, the last two.
    auto without = widest;
    without.row_count -= 2;
    CHECK(panel::lay_out_panel(without, measure, kRowHeight).width == layout.width);
    const int right = layout.width - kInner;
    // Eleven e-acutes, each two bytes.
    constexpr std::string_view kTwoByteName =
        "\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9\xc3\xa9";

    struct Names {
        std::string_view driver;  ///< the render driver
        std::string_view adapter; ///< the adapter's name
        bool cut;                 ///< the panel cuts the adapter's name
    };

    const std::array kNames{
        Names{"metal", "Apple M2", false},
        Names{"direct3d11", "NVIDIA GeForce RTX 3060 Laptop GPU", true},
        Names{"direct3d12", "AMD Radeon(TM) Graphics", true},
        Names{"opengles2", "WWWWWWWWWWWWWWWWWWWWWWWWWWWW", true},
        Names{"vulkan", kTwoByteName, true},
    };
    for (const auto& names : kNames) {
        frame_pacing::FrameStatsRenderer renderer{};
        renderer.tier = "standard";
        renderer.driver = names.driver;
        renderer.adapter = names.adapter;
        const auto table = frame_pacing::frame_stats_table({}, {}, renderer);
        const auto& row = table.rows[table.row_count - 2];
        CHECK(row.kind == frame_pacing::FrameStatsRowKind::renderer);
        const auto fit = panel::fit_run_on_row(row, measure, layout);
        const auto label = row.label.view();
        const auto note = row.note.view();
        CHECK(fit.label_bytes == label.size());
        const int label_end = layout.label_x + console_width(nullptr, label);
        CHECK(label_end <= right);
        CHECK(fit.note_x == label_end + panel::kColumnGap);
        const auto shown = note.substr(0, fit.note_bytes);
        CHECK(fit.note_x + console_width(nullptr, shown) <= right);
        CHECK((fit.note_bytes < note.size()) == names.cut);
        if (fit.note_bytes < note.size()) {
            CHECK(frame_pacing::whole_characters(note, fit.note_bytes).size() == fit.note_bytes);
            std::size_t next = fit.note_bytes + 1;
            while (frame_pacing::whole_characters(note, next).size() != next)
                ++next;
            CHECK(fit.note_x + console_width(nullptr, note.substr(0, next)) > right);
        }
    }
}

// The turned view's graphics device's row sets no width either: laid out
// with it, the panel is as wide as without it and a row taller. In the
// match label font, the name of a laptop's discrete card, cut to one of the
// table's texts, shows whole after "3D" and the device's driver, under the
// window's renderer, whose adapter on the processor's graphics is cut where
// one more character would pass the padding, as before.
void test_turned_device_row_fits_the_panel() {
    const panel::TextWidthHooks measure{nullptr, console_width};
    const auto without =
        panel::lay_out_panel(frame_pacing::frame_stats_widest_table(), measure, kRowHeight);
    const auto layout =
        panel::lay_out_panel(frame_pacing::frame_stats_widest_table(true), measure, kRowHeight);
    CHECK(layout.width == without.width);
    CHECK(layout.value_right == without.value_right);
    CHECK(layout.height == without.height + kRowHeight);
    const int right = layout.width - kInner;
    for (const std::string_view driver : {"vulkan", "metal", "direct3d12"}) {
        frame_pacing::FrameStatsRenderer renderer{};
        renderer.tier = "full";
        renderer.anti_aliasing = "4x4";
        renderer.driver = "direct3d11";
        renderer.adapter = "Intel(R) UHD Graphics";
        renderer.turned_device = true;
        renderer.turned_driver = driver;
        renderer.turned_adapter = "NVIDIA GeForce RTX 4070 Laptop GPU";
        const auto table = frame_pacing::frame_stats_table({}, {}, renderer);
        CHECK(table.row_count == frame_pacing::kFrameStatsRowsMost);
        const std::size_t device_row = table.row_count - 2;
        const auto& row = table.rows[device_row];
        CHECK(row.kind == frame_pacing::FrameStatsRowKind::turned_device);
        CHECK(layout.note_x[device_row] == layout.label_x);
        const auto fit = panel::fit_run_on_row(row, measure, layout);
        CHECK(fit.label_bytes == row.label.view().size());
        CHECK(row.note.view() == "NVIDIA GeForce RTX 4070");
        CHECK(fit.note_bytes == row.note.view().size());
        CHECK(fit.note_x + console_width(nullptr, row.note.view()) <= right);
        const auto& window_row = table.rows[device_row - 1];
        CHECK(window_row.kind == frame_pacing::FrameStatsRowKind::renderer);
        const auto window_fit = panel::fit_run_on_row(window_row, measure, layout);
        CHECK(window_fit.label_bytes == window_row.label.view().size());
        CHECK(window_row.note.view().substr(0, window_fit.note_bytes) == "Intel(R) UHD Graphic");
    }
}

// In the match label font, the display row of a window or of full screen
// at the sizes of today's displays shows whole, and the longest it can
// read never passes the panel's padding: its note is cut, between whole
// characters.
void test_display_row_fits_the_panel() {
    const panel::TextWidthHooks measure{nullptr, console_width};
    const auto layout =
        panel::lay_out_panel(frame_pacing::frame_stats_widest_table(), measure, kRowHeight);
    const int right = layout.width - kInner;

    struct Display {
        frame_pacing::FrameStatsScreen screen; ///< how the window shows the screen
        int32_t frame;                         ///< the frame's width; its height is 9 / 16 of it
        int32_t mode;                          ///< the mode's width, as frame
        float rate;                            ///< the mode's refresh rate
        float scale;                           ///< the display's scale
        bool cut;                              ///< the panel cuts the note
    };

    const std::array kDisplays{
        Display{frame_pacing::FrameStatsScreen::window, 1280, 1920, 60.0F, 1.0F, false},
        Display{frame_pacing::FrameStatsScreen::full_screen, 1280, 3840, 144.0F, 1.5F, false},
        Display{frame_pacing::FrameStatsScreen::exclusive, 2560, 2560, 240.0F, 1.25F, false},
        Display{frame_pacing::FrameStatsScreen::full_screen, 16384, 16384, 240.0F, 1.75F, true},
    };
    for (const auto& shown : kDisplays) {
        frame_pacing::FrameStatsDisplay display{};
        display.screen = shown.screen;
        display.frame_width = shown.frame;
        display.frame_height = shown.frame * 9 / 16;
        display.mode_width = shown.mode;
        display.mode_height = shown.mode * 9 / 16;
        display.refresh_rate = shown.rate;
        display.display_scale = shown.scale;
        const auto table = frame_pacing::frame_stats_table({}, {}, {}, display);
        const auto& row = table.rows[table.row_count - 1];
        CHECK(row.kind == frame_pacing::FrameStatsRowKind::display);
        const auto fit = panel::fit_run_on_row(row, measure, layout);
        const auto label = row.label.view();
        const auto note = row.note.view();
        CHECK(fit.label_bytes == label.size());
        const int label_end = layout.label_x + console_width(nullptr, label);
        CHECK(label_end <= right);
        CHECK(fit.note_x + console_width(nullptr, note.substr(0, fit.note_bytes)) <= right);
        CHECK((fit.note_bytes < note.size()) == shown.cut);
    }
}

// With no measure every text is 0 wide: the columns are a gap apart and
// the graph sets the width.
void test_without_a_measure() {
    frame_pacing::FrameStatsTable table{};
    add_row(table, frame_pacing::FrameStatsRowKind::measure, "frame", "8.00", "8.50", "9.00", "");
    const auto layout = panel::lay_out_panel(table, {}, kRowHeight);
    CHECK(layout.width == 2 * kInner + kFramedGraph);
    CHECK(layout.value_right[1] - layout.value_right[0] == panel::kColumnGap);
    CHECK(layout.value_right[2] - layout.value_right[1] == panel::kColumnGap);
    CHECK(layout.value_right[2] == layout.width - kInner);
}

// Laid out from the widest table, the panel is the same whatever the
// figures: idle or busy, with no units or with thousands, before the
// first second or after a hitch. In the match label font's widths and 13
// pixels a row, it fits the battlefield's bottom right quarter of a
// 640x480 screen (256 by 208 pixels) with its inset.
void test_layout_stays_in_place() {
    const panel::TextWidthHooks measure{nullptr, console_width};
    const auto widest = frame_pacing::frame_stats_widest_table();
    // Without a graphics device of the turned view, a row fewer than most.
    CHECK(widest.row_count == frame_pacing::kFrameStatsRowsMost - 1);
    const auto layout = panel::lay_out_panel(widest, measure, kRowHeight);
    // Every text the table shows in play fits the room the widest kept.
    frame_pacing::FrameStatsWindow window{};
    uint64_t now = frame_pacing::kNanosecondsPerSecond;
    (void)frame_pacing::roll_frame_stats(window, now);
    for (uint32_t frame = 0; frame < 120; ++frame) {
        const uint64_t frame_ns = (frame % 30 == 0 ? 140 : 8) * kNanosecondsPerMillisecond;
        now += frame_ns;
        for (std::size_t measure_index = 0; measure_index < frame_pacing::kFrameMeasureCount;
             ++measure_index)
            frame_pacing::note_frame_measure(
                window, static_cast<frame_pacing::FrameMeasure>(measure_index), frame_ns
            );
    }
    (void)frame_pacing::roll_frame_stats(window, now);
    for (const frame_pacing::FrameStatsNotes notes : {
             frame_pacing::FrameStatsNotes{120, 120, 0, 0},
             frame_pacing::FrameStatsNotes{120, 30, 25, 0},
             frame_pacing::FrameStatsNotes{1000, 1000, 1200, 1200},
             frame_pacing::FrameStatsNotes{0, 0, 9999, 9999},
         }) {
        frame_pacing::FrameStatsRenderer renderer{};
        renderer.tier = "standard";
        renderer.driver = "direct3d12";
        renderer.adapter = "NVIDIA GeForce RTX 4090";
        const auto table = frame_pacing::frame_stats_table(window, notes, renderer);
        CHECK(table.row_count == widest.row_count);
        for (std::size_t row = 0; row < table.row_count; ++row) {
            CHECK(table.rows[row].kind == widest.rows[row].kind);
            // The renderer and display rows keep no room: they are cut where
            // they are drawn (test_renderer_row_fits_the_panel).
            if (frame_pacing::runs_on(table.rows[row].kind))
                continue;
            for (std::size_t column = 0; column < table.rows[row].values.size(); ++column)
                CHECK(
                    table.rows[row].values[column].view().size() <=
                    widest.rows[row].values[column].view().size()
                );
            CHECK(table.rows[row].note.view().size() <= widest.rows[row].note.view().size());
        }
    }
    constexpr int kQuarterWidth = 256;
    constexpr int kQuarterHeight = 208;
    CHECK(layout.width + panel::kInset <= kQuarterWidth);
    CHECK(layout.height + panel::kInset <= kQuarterHeight);
    // With the turned view's device's row the panel is a row taller: it
    // reaches that row above the quarter of a 640x480 battlefield, where it
    // keeps its corner at scale 1, and fits the quarter from 800x600 up.
    const auto turned =
        panel::lay_out_panel(frame_pacing::frame_stats_widest_table(true), measure, kRowHeight);
    CHECK(turned.height == layout.height + kRowHeight);
    CHECK(turned.height + panel::kInset > kQuarterHeight);
    namespace display_layout = oa::ui::display_layout;
    const auto small = display_layout::make_match_layout(640, 480);
    const auto placed = panel::place_panel(turned, small, 1);
    CHECK(placed.scale == 1);
    CHECK(placed.panel.x + placed.panel.width + panel::kInset == 640);
    CHECK(placed.panel.y + placed.panel.height + panel::kInset == small.bottom_bar_y());
    CHECK(placed.panel.y >= small.battlefield_y());
    for (const auto& [width, height] :
         {std::pair{800, 600}, std::pair{1024, 768}, std::pair{1280, 960}, std::pair{1920, 1080}}) {
        const auto match = display_layout::make_match_layout(width, height);
        const auto place = panel::place_panel(turned, match, 2);
        const auto quarter = panel::battlefield_quarter(match);
        CHECK(place.panel.x >= quarter.x && place.panel.y >= quarter.y);
    }
}

// The panel sits kInset in from the battlefield's bottom right corner, at
// the HUD's text scale where it fits the quarter, and smaller where not.
void test_panel_place() {
    namespace display_layout = oa::ui::display_layout;
    const panel::TextWidthHooks measure{nullptr, console_width};
    const auto layout =
        panel::lay_out_panel(frame_pacing::frame_stats_widest_table(), measure, kRowHeight);
    const auto inside = [](const display_layout::Rect& box, const display_layout::Rect& area) {
        return box.x >= area.x && box.y >= area.y && box.x + box.width <= area.x + area.width &&
               box.y + box.height <= area.y + area.height;
    };
    // 640x480: the battlefield from (128, 32) to (640, 448); its quarter
    // from (384, 240).
    auto match = display_layout::make_match_layout(640, 480);
    const auto quarter = panel::battlefield_quarter(match);
    CHECK(quarter.x == 384 && quarter.y == 240 && quarter.width == 256 && quarter.height == 208);
    auto place = panel::place_panel(layout, match, 1);
    CHECK(place.scale == 1);
    CHECK(place.panel.x + place.panel.width + panel::kInset == 640);
    CHECK(place.panel.y + place.panel.height + panel::kInset == match.bottom_bar_y());
    CHECK(inside(place.panel, quarter));
    // 1024x768 rounds the HUD's scale of 1.6 up to 2, at which the panel
    // would cover more than the quarter: it stays at 1.
    match = display_layout::make_match_layout(1024, 768);
    place = panel::place_panel(layout, match, 2);
    CHECK(place.scale == 1);
    CHECK(inside(place.panel, panel::battlefield_quarter(match)));
    // 1280x960 and 1920x1080 hold it at 2.
    for (const auto& [width, height] : {std::pair{1280, 960}, std::pair{1920, 1080}}) {
        match = display_layout::make_match_layout(width, height);
        place = panel::place_panel(layout, match, 2);
        CHECK(place.scale == 2);
        CHECK(place.panel.width == 2 * layout.width);
        CHECK(place.panel.x + place.panel.width + 2 * panel::kInset == width);
        CHECK(inside(place.panel, panel::battlefield_quarter(match)));
    }
    // A text scale below 1 draws at 1.
    CHECK(panel::place_panel(layout, display_layout::make_match_layout(640, 480), 0).scale == 1);
}

void test_bar_heights() {
    // Three pixels for every four milliseconds, rounded to the nearest; at
    // least one pixel, at most the graph's height.
    CHECK(panel::bar_height(0) == 1);
    CHECK(panel::bar_height(kNanosecondsPerMillisecond) == 1);
    CHECK(panel::bar_height(8'333'333) == 6);
    CHECK(panel::bar_height(8'500'000) == 6);
    CHECK(panel::bar_height(frame_pacing::kTickBudgetNs) == 25);
    CHECK(panel::bar_height(panel::kGraphTopNs) == panel::kGraphHeight);
    CHECK(panel::bar_height(frame_pacing::kNanosecondsPerSecond) == panel::kGraphHeight);
    CHECK(panel::bar_height(std::numeric_limits<uint64_t>::max()) == panel::kGraphHeight);
    // A line sits on the row above a bar of its time, so a bar on time
    // stays under the line at the frame's allowance and a later one rises
    // over it; the tick's line sits inside the graph.
    const uint64_t allowance =
        frame_pacing::frame_allowance_ns(120, frame_pacing::FrameWait::precise);
    CHECK(panel::line_rise(allowance) == panel::bar_height(allowance) + 1);
    CHECK(panel::bar_height(8'333'333) < panel::line_rise(allowance));
    CHECK(panel::bar_height(allowance) < panel::line_rise(allowance));
    CHECK(panel::bar_height(11'000'000) >= panel::line_rise(allowance));
    CHECK(panel::line_rise(frame_pacing::kTickBudgetNs) == 26);
    CHECK(panel::line_rise(frame_pacing::kTickBudgetNs) <= panel::kGraphHeight);
}

// Each grade's colour: a mid green within the frame's allowance, the
// health bar's yellow within a tick and its red over a tick.
void test_severity_slots() {
    constexpr uint8_t kUngraded = 23;
    CHECK(
        panel::severity_slot(frame_pacing::TimeSeverity::within_frame, kUngraded) ==
        panel::kWithinFrameSlot
    );
    CHECK(
        panel::severity_slot(frame_pacing::TimeSeverity::within_tick, kUngraded) ==
        oa::ui::hud::kHealthMidColor
    );
    CHECK(
        panel::severity_slot(frame_pacing::TimeSeverity::over_tick, kUngraded) ==
        oa::ui::hud::kHealthLowColor
    );
    CHECK(panel::severity_slot(frame_pacing::TimeSeverity::none, kUngraded) == kUngraded);
    CHECK(panel::kWithinFrameSlot != oa::ui::hud::kHealthHighColor);
}

} // namespace

int main() {
    test_columns_under_a_wider_graph();
    test_graph_under_a_wider_table();
    test_without_a_measure();
    test_renderer_row_sets_no_width();
    test_renderer_row_fits_the_panel();
    test_turned_device_row_fits_the_panel();
    test_display_row_fits_the_panel();
    test_layout_stays_in_place();
    test_panel_place();
    test_bar_heights();
    test_severity_slots();
    if (failures != 0) {
        std::fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}

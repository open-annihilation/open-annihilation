// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The application loop's frames apart from the simulation's ticks: each
// frame's time from the pacer, the rate the loop keeps and its wait, the
// fraction of a tick each frame shows, the resource readout's eases for the
// frame's time, and the "+stats" overlay of frame, tick, draw and present
// times and the latest frames' graph, with its console check.
#include "oa/app/runtime.hpp"
#include "device_state.hpp"
#include "frame_stats_panel.hpp"
#include "full_presentation.hpp"
#include "graphics_report.hpp"
#include "match_clock.hpp"
#include "pad_state.hpp"
#include "oa/app/frame_pacing.hpp"
#include "oa/app/match_console.hpp"
#include "oa/ui/console/console.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/gadget_render.hpp"
#include "oa/ui/hud/health_bar.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>

namespace oa::app {

namespace console = oa::ui::console;
namespace panel = frame_stats_panel;
using frame_pacing::begin_paced_frame;
using frame_pacing::capped_frame_rate;
using frame_pacing::end_paced_frame;
using frame_pacing::FrameActivity;
using frame_pacing::FrameMeasure;
using frame_pacing::FrameStatsNotes;
using frame_pacing::FrameStatsRowKind;
using frame_pacing::FrameStatsTable;
using frame_pacing::FrameTicks;
using frame_pacing::FrameWait;
using frame_pacing::kInputActivityNs;
using frame_pacing::kNanosecondsPerMillisecond;
using frame_pacing::next_presentation_alpha;
using frame_pacing::note_frame_measure;
using frame_pacing::paced_frame_rate;
using frame_pacing::roll_frame_stats;
using frame_pacing::TimeSeverity;
using frame_pacing::vsync_frame_cap;

namespace {

/// GUI palette slot of a raised gadget's light top and left edges, the
/// panel's and the graph's.
constexpr auto kLightEdgeSlot = static_cast<uint8_t>(oa::ui::gadget_render::color_slot::light_edge);
/// GUI palette slot of a raised gadget's dark bottom and right edges
/// (black): the panel's outline, and what its and the graph's fills darken
/// toward.
constexpr auto kDarkEdgeSlot = static_cast<uint8_t>(oa::ui::gadget_render::color_slot::dark_edge);
/// GUI palette slot of the table's labels and the units count: a light gray.
constexpr uint8_t kLabelSlot = 20;
/// GUI palette slot of the column names, the notes and a missing measure: a
/// mid gray.
constexpr uint8_t kNoteSlot = 23;
/// GUI palette slot of the rule under the column names: a dark gray.
constexpr uint8_t kRuleSlot = 27;
/// GUI palette slot of the graph's lines at a tick and at the frame's
/// allowance: a neutral gray, apart from every grade's colour.
constexpr uint8_t kGraphLineSlot = 22;
/// How much of the battlefield's light the panel's fill takes away, and how
/// much more the graph's, in 256ths.
constexpr uint32_t kPanelOpacity = panel::kPanelOpacity;
constexpr uint32_t kGraphOpacity = panel::kGraphOpacity;
/// How strongly the graph's line at a tick shows over the graph, in 256ths.
constexpr uint32_t kTickLineOpacity = 176;
/// How strongly the dots of its line at the frame's allowance show, in
/// 256ths: fainter than the tick's.
constexpr uint32_t kAllowanceLineOpacity = 144;
/// How strongly the cell behind a time over a tick shows its red, in 256ths.
constexpr uint32_t kOverTickCellOpacity = 96;
/// Rows a match label font's line keeps under its glyphs
/// (oa::formats::fnt::line_height).
constexpr int kLineGapRows = 2;
// The graph shows a frame a column at the loop's default rate.
static_assert(frame_pacing::kFrameGraphColumnsPerSecond == kDefaultMaxFramesPerSecond);
/// Times a second of frame time the resource readout eases toward the
/// stores: once a frame at the loop's default rate, as it eased once a draw.
constexpr uint64_t kReadoutEasesPerSecond = kDefaultMaxFramesPerSecond;
/// The most eases one frame takes; a longer gap since the last starts afresh
/// with one.
constexpr uint64_t kMostReadoutEases = 8;

/// Tells whether an event comes from the player's keyboard, pointer, touch or controller.
///
/// @param event the event
/// @return true for input
bool input_event(const SDL_Event& event) {
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
    case SDL_EVENT_TEXT_INPUT:
    case SDL_EVENT_MOUSE_MOTION:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_FINGER_DOWN:
    case SDL_EVENT_FINGER_UP:
    case SDL_EVENT_FINGER_MOTION:
    case SDL_EVENT_FINGER_CANCELED:
#if SDL_VERSION_ATLEAST(3, 4, 0)
    case SDL_EVENT_PINCH_BEGIN:
    case SDL_EVENT_PINCH_UPDATE:
    case SDL_EVENT_PINCH_END:
#endif
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
        return true;
    // A gyro's readings come while the pad lies still: no input.
    case SDL_EVENT_GAMEPAD_SENSOR_UPDATE:
        return false;
    default:
        return false;
    }
}

/// The font the panel's texts are measured in.
struct LabelFont {
    const oa::formats::fnt::Font* font{}; ///< the match label font
};

/// Measures a text in the match label font, for the panel's layout.
///
/// @param context the LabelFont
/// @param text the text
/// @return source pixels
int label_width(void* context, std::string_view text) {
    return static_cast<int>(
        oa::formats::fnt::measure_text(*static_cast<const LabelFont*>(context)->font, text)
    );
}

} // namespace

float Runtime::presentation_alpha() const noexcept {
    return presentation_alpha_;
}

void Runtime::set_presentation_alpha(float alpha) noexcept {
    presentation_alpha_ = std::clamp(alpha, 0.0F, 1.0F);
}

uint64_t Runtime::frame_clock_ns() const {
    return frame_run_clock_ns_ ? *frame_run_clock_ns_ : frame_pacing::steady_now_ns();
}

void Runtime::begin_loop_frame() {
    const uint64_t now = frame_pacing::steady_now_ns();
    // The frame that ends now, and the samples of the one that starts, are
    // graded against the rate and the wait chosen as the last frame ended.
    frame_stats_.allowance_ns =
        frame_pacing::frame_allowance_ns(frame_stats_notes().paced_frames_per_second, frame_wait_);
    frame_time_ns_ = begin_paced_frame(frame_pacer_, frame_clock_ns());
    loop_frame_time_ = true;
    // The statistics are drawn from the shown second whenever they are
    // drawn, so whether this frame ended a second does not matter here.
    std::ignore = roll_frame_stats(frame_stats_, now);
    // A gap of a second or more (the loop waited for the window's focus) is
    // no frame's time.
    if (previous_loop_frame_start_ns_ != 0 && now > previous_loop_frame_start_ns_ &&
        now - previous_loop_frame_start_ns_ < frame_pacing::kNanosecondsPerSecond)
        note_frame_measure(frame_stats_, FrameMeasure::frame, now - previous_loop_frame_start_ns_);
    previous_loop_frame_start_ns_ = now;
    loop_frame_start_ns_ = now;
}

void Runtime::take_frame_time() {
    if (loop_frame_time_)
        loop_frame_time_ = false;
    else
        frame_time_ns_ = frame_clock_ns();
    count_readout_eases();
}

void Runtime::count_readout_eases() {
    readout_eases_.reset();
    // A check's fixed clock eases once a draw, as every draw did.
    if (options_.fixed_clock && !frame_run_clock_ns_)
        return;
    const uint64_t period = frame_pacing::kNanosecondsPerSecond / kReadoutEasesPerSecond;
    if (!readout_clock_ns_ || frame_time_ns_ < *readout_clock_ns_ ||
        frame_time_ns_ - *readout_clock_ns_ > kMostReadoutEases * period) {
        readout_clock_ns_ = frame_time_ns_;
        readout_eases_ = 1;
        return;
    }
    const uint64_t due = (frame_time_ns_ - *readout_clock_ns_) / period;
    *readout_clock_ns_ += due * period;
    readout_eases_ = static_cast<uint32_t>(due);
}

uint32_t Runtime::take_readout_eases() {
    const uint32_t eases = readout_eases_.value_or(1);
    readout_eases_.reset();
    return eases;
}

uint32_t Runtime::frame_clock_milliseconds() const {
    if (options_.fixed_clock && !frame_run_clock_ns_)
        return clock_milliseconds();
    // The low 32 bits of the milliseconds, as clock_milliseconds() keeps them.
    return static_cast<uint32_t>(frame_time_ns_ / kNanosecondsPerMillisecond);
}

void Runtime::step_match_frame() {
    bool stepped = match_clock_steps();
    const uint32_t ticks_before = match_timing_.tick;
    const uint32_t now_ms = frame_clock_milliseconds();
    // On the loop's clock, a clock set past the frame's time during the
    // frame (a load, a screenshot or a film frame sets it to the moment it
    // ends) steps from there on the next frame, so the time it left out
    // stays out; a reading that has turned over to 0 steps, as in 3.1c. A
    // check's fixed clock steps as it always has.
    const bool behind = clock_reading_behind(
        oa::base::game_loop::scaled_clock(now_ms, match_clock_scale()), match_timing_.previous_clock
    );
    const bool fixed = options_.fixed_clock && !frame_run_clock_ns_;
    if (stepped && !fixed && behind)
        stepped = false;
    if (stepped)
        advance_match_clock(now_ms);
    present_frame_between_ticks(stepped, ticks_before);
    place_tracking_camera();
}

void Runtime::present_frame_between_ticks(bool stepped, uint32_t ticks_before) {
    FrameTicks ticks{};
    ticks.owed = stepped ? match_timing_.pending_steps : 0;
    ticks.ran = match_timing_.tick - ticks_before;
    // A check's fixed clock draws whole ticks, as before frames were drawn
    // between them; a --frame-rate run's clock does not.
    bool whole = !stepped || !match_ || (options_.fixed_clock && !frame_run_clock_ns_);
    if (match_) {
        const oa::Game& game = match_->state().game;
        // A film frame is saved as drawn, and shows the whole tick.
        const bool film_frame = game.capture_enabled > 0 && game.capture_rate > 0 &&
                                game.next_capture_tick <= game.tick;
        whole = whole || (game.sim_run_flags & console::kSimRunPaused) != 0 || film_frame;
    }
    ticks.whole = whole;
    // Frames one a clock unit: the loop paced at the tick rate, or a
    // --frame-rate run at it.
    const uint32_t frames_per_second =
        frame_run_clock_ns_ ? frame_run_frames_per_second_ : frame_pacer_.frames_per_second;
    ticks.unit_frames = frames_per_second == frame_pacing::kTicksPerSecond;
    presentation_alpha_ =
        next_presentation_alpha(tick_presentation_, match_timing_, ticks, frame_time_ns_);
}

void Runtime::pace_next_frame(bool& running) {
    const uint64_t now = frame_pacing::steady_now_ns();
    if (now > loop_frame_start_ns_)
        note_frame_measure(frame_stats_, FrameMeasure::work, now - loop_frame_start_ns_);
    FrameActivity activity{};
    activity.match_advancing =
        match_clock_steps() && (match_->state().game.sim_run_flags & console::kSimRunPaused) == 0;
    activity.camera_moving = camera_moved_;
    // A finger resting on the screen, or a pad's button, stick or trackpad
    // held, is a held button: hold timers, ghost drags and auto-scroll need
    // the full rate.
    activity.input_recent = (last_input_ns_ != 0 && now - last_input_ns_ < kInputActivityNs) ||
                            device_state::buttons_held() != 0 || touch_finger_count() != 0 ||
                            PadAccess::input_held(*this);
    activity.unattended = options_.unattended;
    paced_frames_per_second_ = paced_frame_rate(options_.max_frames_per_second, activity);
    // While the renderer waits for the display, the loop keeps just below
    // the display's rate, so that each present finds the display ready.
    if (vertical_sync_in_effect_)
        paced_frames_per_second_ =
            capped_frame_rate(paced_frames_per_second_, vsync_frame_cap(display_refresh_rate()));
    const bool idle = !activity.match_advancing && !activity.camera_moving &&
                      !activity.input_recent && !activity.unattended;
    frame_wait_ = idle && sdl_.window != nullptr ? FrameWait::idle : FrameWait::precise;
    const uint64_t wait = end_paced_frame(frame_pacer_, frame_clock_ns(), paced_frames_per_second_);
    if (wait == 0)
        return;
    if (frame_wait_ == FrameWait::idle) {
        // An idle wait ends early when an event comes, so the pointer and
        // the keys answer at once; the frame that follows starts a new run.
        SDL_Event event{};
        const auto timeout_ms = static_cast<int32_t>(
            (wait + kNanosecondsPerMillisecond - 1) / kNanosecondsPerMillisecond
        );
        if (SDL_WaitEventTimeout(&event, timeout_ms))
            dispatch_event(event, running);
        return;
    }
    // A frame at the full rate waits to the nanosecond, sleeping first and
    // spinning the last moment, so that frames stay evenly spaced on every
    // platform's timer; an idle wait above sleeps only.
    SDL_DelayPrecise(wait);
}

void Runtime::note_input_activity(const SDL_Event& event) {
    if (input_event(event))
        last_input_ns_ = frame_pacing::steady_now_ns();
}

void Runtime::show_frame_stats(bool shown) {
    frame_stats_shown_ = shown;
}

frame_pacing::FrameStatsRenderer Runtime::frame_stats_renderer() const {
    frame_pacing::FrameStatsRenderer renderer{};
    renderer.tier = full_presentation()          ? full_tier_name
                    : accelerated_presentation() ? basic_tier_name
                                                 : standard_tier_name;
    // The full tier's anti-aliasing, where the world target draws it: the
    // samples a pixel, across by down.
    constexpr std::string_view twice = "2x2";
    constexpr std::string_view four_times = "4x4";
    constexpr std::string_view eight_times = "8x8";
    constexpr std::string_view sixteen_times = "16x16";
    const uint32_t supersample = full_supersample();
    renderer.anti_aliasing = supersample == 2    ? twice
                             : supersample == 4  ? four_times
                             : supersample == 8  ? eight_times
                             : supersample == 16 ? sixteen_times
                                                 : "";
    renderer.driver = renderer_driver_;
    renderer.adapter = renderer_adapter_;
    // The full tier's pages holding as much as they may, which leaves out of
    // a frame what it needs past them.
    if (full_presentation() && full_->page_memory_full)
        renderer.limit = "sprite memory full";
    return renderer;
}

frame_pacing::FrameStatsDisplay Runtime::frame_stats_display() const {
    frame_pacing::FrameStatsDisplay display{};
    display.frame_width = match_layout_.width;
    display.frame_height = match_layout_.height;
    if (sdl_.window == nullptr)
        return display;
    const SDL_WindowFlags flags = SDL_GetWindowFlags(sdl_.window);
    display.screen = (flags & SDL_WINDOW_FULLSCREEN) == 0 ? frame_pacing::FrameStatsScreen::window
                     : SDL_GetWindowFullscreenMode(sdl_.window) != nullptr
                         ? frame_pacing::FrameStatsScreen::exclusive
                         : frame_pacing::FrameStatsScreen::full_screen;
    if (const SDL_DisplayMode* mode =
            SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(sdl_.window));
        mode != nullptr) {
        display.mode_width = mode->w;
        display.mode_height = mode->h;
        display.refresh_rate = mode->refresh_rate;
    }
    display.display_scale = SDL_GetWindowDisplayScale(sdl_.window);
    return display;
}

FrameStatsNotes Runtime::frame_stats_notes() const {
    FrameStatsNotes notes{};
    notes.max_frames_per_second = options_.max_frames_per_second;
    // Before the loop has paced a frame (a check's frames), the full rate.
    notes.paced_frames_per_second =
        frame_pacer_.started ? paced_frames_per_second_ : options_.max_frames_per_second;
    notes.units_drawn = frame_draws_.units_drawn;
    notes.units_between_ticks = frame_draws_.units_between_ticks;
    return notes;
}

void Runtime::draw_frame_stats() {
    frame_stats_place_.reset();
    if (!frame_stats_shown_ || !match_)
        return;
    // The panel is laid out for the game's font.
    const PanelText panel(*this);
    const oa::formats::fnt::Font* font = match_label_font();
    if (font == nullptr)
        return;
    ensure_ui_colors();
    const FrameStatsTable table = frame_pacing::frame_stats_table(
        frame_stats_, frame_stats_notes(), frame_stats_renderer(), frame_stats_display()
    );
    LabelFont label_font{font};
    panel::TextWidthHooks measure{};
    measure.context = &label_font;
    measure.width = label_width;
    const auto layout = panel::lay_out_panel(
        frame_pacing::frame_stats_widest_table(),
        measure,
        static_cast<int>(oa::formats::fnt::line_height(*font))
    );
    const auto placed = panel::place_panel(layout, match_layout_, hud_text_scale());
    const int scale = placed.scale;
    const int left = placed.panel.x;
    const int top = placed.panel.y;
    FrameStatsPlace place{};
    place.panel = placed.panel;
    place.graph = {
        left + layout.graph.x * scale,
        top + layout.graph.y * scale,
        layout.graph.width * scale,
        layout.graph.height * scale
    };
    place.scale = scale;
    for (std::size_t column = 0; column < place.value_right.size(); ++column)
        place.value_right[column] = left + layout.value_right[column] * scale;
    for (std::size_t row = 0; row < place.row_top.size(); ++row)
        place.row_top[row] = top + layout.row_y[row] * scale;
    frame_stats_place_ = place;

    auto& target = paint_target();
    const auto at = canvas_paint(left, top);
    // A rectangle in canvas pixels from the panel's corner. In the Full
    // tier the card draws a blend over the battlefield as its quad, since
    // the panel is painted on the overlay canvas.
    const auto fill = [&](int x, int y, int w, int h, uint8_t slot, uint32_t opacity) {
        if (opacity < renderer::blend_opaque &&
            paint_world_blend(at.x + x, at.y + y, w, h, ui_color_rgb(slot), opacity))
            return;
        renderer::blend_rect(target, at.x + x, at.y + y, w, h, ui_color_rgb(slot), opacity);
    };
    // A run of an edge, in source pixels from the panel's corner.
    const auto edge_run = [&](int x, int y, int w, int h, uint8_t slot) {
        fill(x * scale, y * scale, w * scale, h * scale, slot, renderer::blend_opaque);
    };
    // The panel: the battlefield darkened under it, a black outline, and
    // inside it the raised edge of a 3.1c gadget, light at the top and left
    // and dark at the bottom and right, the two meeting on the diagonal at
    // the corners.
    static_assert(panel::kBevel == 2);
    const int panel_width = layout.width;
    const int panel_height = layout.height;
    fill(0, 0, placed.panel.width, placed.panel.height, kDarkEdgeSlot, kPanelOpacity);
    edge_run(0, 0, panel_width, 1, kDarkEdgeSlot);
    edge_run(0, 1, 1, panel_height - 1, kDarkEdgeSlot);
    edge_run(panel_width - 1, 1, 1, panel_height - 1, kDarkEdgeSlot);
    edge_run(1, panel_height - 1, panel_width - 2, 1, kDarkEdgeSlot);
    edge_run(1, 1, panel_width - 2, 1, kLightEdgeSlot);
    edge_run(1, 2, 1, panel_height - 3, kLightEdgeSlot);
    edge_run(panel_width - 2, 2, 1, panel_height - 3, kDarkEdgeSlot);
    edge_run(2, panel_height - 2, panel_width - 4, 1, kDarkEdgeSlot);

    // The graph: a darker well in a 3.1c gadget's one-pixel sunken edge,
    // dark at the top and left and light at the bottom and right; behind
    // the bars, a line at a tick and a fainter dotted one at the frame's
    // allowance; and a bar for each column of the latest frames, the newest
    // at the right, capped in white where it stops at the graph's top.
    static_assert(panel::kGraphEdge == 1);
    const int frame_left = layout.graph.x - 1;
    const int frame_top = layout.graph.y - 1;
    const int frame_right = layout.graph.x + layout.graph.width;
    const int frame_bottom = layout.graph.y + layout.graph.height;
    fill(
        frame_left * scale,
        frame_top * scale,
        (frame_right - frame_left + 1) * scale,
        (frame_bottom - frame_top + 1) * scale,
        kDarkEdgeSlot,
        kGraphOpacity
    );
    edge_run(frame_left, frame_top, frame_right - frame_left + 1, 1, kDarkEdgeSlot);
    edge_run(frame_left, frame_top, 1, frame_bottom - frame_top + 1, kDarkEdgeSlot);
    edge_run(frame_right, frame_top + 1, 1, frame_bottom - frame_top, kLightEdgeSlot);
    edge_run(frame_left + 1, frame_bottom, frame_right - frame_left, 1, kLightEdgeSlot);
    const int graph_x = layout.graph.x * scale;
    const int graph_width = layout.graph.width * scale;
    const int floor_y = (layout.graph.y + layout.graph.height) * scale;
    const auto line_y = [&](uint64_t ns) { return floor_y - panel::line_rise(ns) * scale; };
    fill(
        graph_x,
        line_y(frame_pacing::kTickBudgetNs),
        graph_width,
        scale,
        kGraphLineSlot,
        kTickLineOpacity
    );
    const uint64_t allowance = frame_stats_.allowance_ns;
    if (allowance < frame_pacing::kTickBudgetNs)
        for (int x = 0; x < graph_width; x += panel::kDotPitch * scale)
            fill(
                graph_x + x, line_y(allowance), scale, scale, kGraphLineSlot, kAllowanceLineOpacity
            );
    const int bar = panel::kBarWidth * scale;
    const std::size_t columns = std::min(
        frame_pacing::kFrameGraphColumns,
        static_cast<std::size_t>(layout.graph.width / panel::kBarWidth)
    );
    for (std::size_t age = 0; age < columns; ++age) {
        const auto column = frame_pacing::frame_history_column(frame_stats_.history, age);
        if (column.severity == TimeSeverity::none)
            continue;
        const int rise = panel::bar_height(column.frame_ns) * scale;
        const int x = graph_x + graph_width - static_cast<int>(age + 1) * bar;
        fill(
            x,
            floor_y - rise,
            bar,
            rise,
            panel::severity_slot(column.severity, kNoteSlot),
            renderer::blend_opaque
        );
        if (column.frame_ns >= panel::kGraphTopNs)
            fill(
                x,
                floor_y - rise,
                bar,
                scale,
                static_cast<uint8_t>(kUiColorText),
                renderer::blend_opaque
            );
    }

    // The table: the title in the text's white; the column names in a mid
    // gray over a rule; labels in a light gray; values right-aligned in
    // their columns, each time in its grade's colour and one over a tick on
    // a red cell as well; notes after them.
    const auto text = [&](int x, int y, std::string_view view, uint8_t slot) {
        if (!view.empty())
            paint_text(*font, at.x + x, at.y + y, view, ui_color_rgb(slot), scale);
    };
    const int glyph_rows = layout.row_height - kLineGapRows;
    for (std::size_t index = 0; index < table.row_count; ++index) {
        const auto& row = table.rows[index];
        const int row_y = layout.row_y[index];
        if (row.kind == FrameStatsRowKind::title) {
            text(
                layout.label_x * scale,
                row_y * scale,
                row.label.view(),
                static_cast<uint8_t>(kUiColorText)
            );
            continue;
        }
        if (frame_pacing::runs_on(row.kind)) {
            // The renderer's names and the display's, cut where they would
            // pass the panel.
            const auto fit = panel::fit_run_on_row(row, measure, layout);
            text(
                layout.label_x * scale,
                row_y * scale,
                row.label.view().substr(0, fit.label_bytes),
                kLabelSlot
            );
            text(
                fit.note_x * scale,
                row_y * scale,
                row.note.view().substr(0, fit.note_bytes),
                kNoteSlot
            );
            continue;
        }
        const bool heading = row.kind == FrameStatsRowKind::heading;
        if (heading && layout.rule_y >= 0)
            edge_run(layout.label_x, layout.rule_y, layout.rule_width, 1, kRuleSlot);
        text(layout.label_x * scale, row_y * scale, row.label.view(), kLabelSlot);
        const uint8_t ungraded = row.kind == FrameStatsRowKind::count ? kLabelSlot : kNoteSlot;
        for (std::size_t column = 0; column < row.values.size(); ++column) {
            const auto& value = row.values[column];
            const auto view = value.view();
            if (view.empty())
                continue;
            const int value_left = layout.value_right[column] - label_width(&label_font, view);
            if (value.severity == TimeSeverity::over_tick)
                fill(
                    (value_left - panel::kCellMargin) * scale,
                    row_y * scale,
                    (layout.value_right[column] - value_left + panel::kCellMargin) * scale,
                    glyph_rows * scale,
                    panel::severity_slot(value.severity, kNoteSlot),
                    kOverTickCellOpacity
                );
            text(
                value_left * scale,
                row_y * scale,
                view,
                heading ? kNoteSlot : panel::severity_slot(value.severity, ungraded)
            );
        }
        text(layout.note_x[index] * scale, row_y * scale, row.note.view(), kNoteSlot);
    }
}

void Runtime::check_console_stats(const std::function<void(const char*)>& enter_line) {
    oa::Game& game = match_->state().game;
    // The echoed lines stay out of the compared frames.
    const auto capture = [&] {
        oa::sim::messages::clear_messages(game);
        render_match_surface();
        return surface_;
    };
    const auto pixel = [](const renderer::Surface& frame, int x, int y) {
        const auto at =
            (static_cast<std::size_t>(y) * frame.width + static_cast<std::size_t>(x)) * 3U;
        return std::array<uint8_t, 3>{frame.rgb[at], frame.rgb[at + 1], frame.rgb[at + 2]};
    };
    const auto changed = [](const renderer::Surface& a, const renderer::Surface& b) {
        std::size_t count = 0;
        for (std::size_t at = 0; at + 2 < a.rgb.size() && at + 2 < b.rgb.size(); at += 3)
            count += a.rgb[at] != b.rgb[at] || a.rgb[at + 1] != b.rgb[at + 1] ||
                             a.rgb[at + 2] != b.rgb[at + 2]
                         ? 1
                         : 0;
        return count;
    };
    // A colour blended over a pixel, as the drawing blends it.
    const auto blend_over =
        [](std::array<uint8_t, 3> under, std::array<uint8_t, 3> color, uint32_t opacity) {
            renderer::Surface one{1, 1, {}};
            one.rgb.assign(under.begin(), under.end());
            renderer::blend_rect(one, 0, 0, 1, 1, color, opacity);
            return std::array<uint8_t, 3>{one.rgb[0], one.rgb[1], one.rgb[2]};
        };
    show_frame_stats(false);
    const auto before = capture();
    // An option: it runs without the passphrase and echoes to this machine
    // alone.
    const auto flags = console::console_flags(game);
    console::set_console_flags(
        game, static_cast<uint16_t>(flags & ~console::console_flag::developer)
    );
    enter_line("+stats");
    console::set_console_flags(game, flags);
    if (!frame_stats_shown_)
        throw std::runtime_error("console check: +stats did not show the frame statistics");
    const uint32_t ran =
        console::console_execute(match_console(), "Stats 1", console::command_class::option);
    if ((ran & console::command_class::option) == 0 ||
        (ran & console::command_class::private_echo) == 0 || !frame_stats_shown_)
        throw std::runtime_error("console check: Stats is not an option echoed to this machine");

    // At every window size, the panel and its inset fit the battlefield's
    // bottom right quarter, at the HUD's text scale where they can: 1 at
    // 640x480, 2 at 1280x960 and above.
    const oa::formats::fnt::Font* font = match_label_font();
    if (font == nullptr)
        throw std::runtime_error("console check: +stats has no font to lay its panel out in");
    LabelFont label_font{font};
    const panel::TextWidthHooks measure{&label_font, label_width};
    const auto layout = panel::lay_out_panel(
        frame_pacing::frame_stats_widest_table(),
        measure,
        static_cast<int>(oa::formats::fnt::line_height(*font))
    );

    struct WindowScale {
        int width{};       ///< the window's width, pixels
        int height{};      ///< its height, pixels
        int panel_scale{}; ///< the panel's scale there; 0 for any that fits
    };

    constexpr std::array kWindows{
        WindowScale{640, 480, 1},
        WindowScale{800, 600, 0},
        WindowScale{960, 720, 0},
        WindowScale{1024, 768, 0},
        WindowScale{1280, 720, 0},
        WindowScale{1280, 960, 2},
        WindowScale{1280, 1024, 2},
        WindowScale{1600, 900, 0},
        WindowScale{1920, 1080, 2},
        WindowScale{2560, 1440, 2},
        WindowScale{3840, 2160, 2},
    };
    const auto inside = [](const oa::ui::display_layout::Rect& box,
                           const oa::ui::display_layout::Rect& area) {
        return box.x >= area.x && box.y >= area.y && box.x + box.width <= area.x + area.width &&
               box.y + box.height <= area.y + area.height;
    };
    for (const auto& window : kWindows) {
        const auto match = oa::ui::display_layout::make_match_layout(window.width, window.height);
        // The HUD's text scale there, as hud_text_scale() reads it.
        const int text_scale = std::max(1, static_cast<int>(std::lround(match.scale)));
        const auto placed = panel::place_panel(layout, match, text_scale);
        if (!inside(placed.panel, panel::battlefield_quarter(match)) || placed.scale < 1 ||
            placed.scale > text_scale ||
            (window.panel_scale != 0 && placed.scale != window.panel_scale))
            throw std::runtime_error(
                "console check: +stats at " + std::to_string(window.width) + "x" +
                std::to_string(window.height) + " placed its panel at scale " +
                std::to_string(placed.scale) + " outside the battlefield's bottom right quarter"
            );
    }

    // The panel sits kInset in from the battlefield's bottom right corner,
    // inside the battlefield's bottom right quarter, and nothing outside the
    // panel changes.
    const auto quarter = panel::battlefield_quarter(match_layout_);
    const auto light = ui_color_rgb(kLightEdgeSlot);
    const auto dark = ui_color_rgb(kDarkEdgeSlot);
    const auto check_panel = [&](const renderer::Surface& shown, const char* state) {
        const std::string what = std::string("console check: +stats ") + state;
        if (!frame_stats_place_)
            throw std::runtime_error(what + " noted no place for its panel");
        const auto& place = *frame_stats_place_;
        const auto& box = place.panel;
        if (!inside(box, quarter) ||
            box.x + box.width + panel::kInset * place.scale !=
                match_layout_.battlefield_x() + match_layout_.battlefield_width() ||
            box.y + box.height + panel::kInset * place.scale != match_layout_.bottom_bar_y())
            throw std::runtime_error(
                what + " placed its panel other than at its inset in the bottom right quarter"
            );
        std::size_t outside = 0;
        for (std::size_t at = 0; at + 2 < shown.rgb.size() && at + 2 < before.rgb.size(); at += 3) {
            const auto index = at / 3;
            const auto x = static_cast<int>(index % shown.width);
            const auto y = static_cast<int>(index / shown.width);
            const bool differs = shown.rgb[at] != before.rgb[at] ||
                                 shown.rgb[at + 1] != before.rgb[at + 1] ||
                                 shown.rgb[at + 2] != before.rgb[at + 2];
            if (differs && !inside({x, y, 1, 1}, box))
                ++outside;
        }
        const auto drawn = changed(before, shown);
        if (drawn < kTextMinPixels || outside != 0)
            throw std::runtime_error(
                what + " changed " + std::to_string(drawn) + " pixels, " + std::to_string(outside) +
                " outside its panel"
            );
        // A black outline; inside it a raised edge, light at the top and
        // left, dark at the bottom and right; inside that, the battlefield
        // darkened by the panel's fill.
        const int middle_x = box.x + box.width / 2;
        const int middle_y = box.y + box.height / 2;
        const int ring = place.scale;
        if (pixel(shown, middle_x, box.y) != dark || pixel(shown, box.x, middle_y) != dark ||
            pixel(shown, middle_x, box.y + ring) != light ||
            pixel(shown, box.x + ring, middle_y) != light ||
            pixel(shown, middle_x, box.y + box.height - 1 - ring) != dark ||
            pixel(shown, box.x + box.width - 1 - ring, middle_y) != dark)
            throw std::runtime_error(what + " drew its panel without its outline and raised edge");
        const int padding_x = box.x + panel::kBevel * place.scale;
        if (pixel(shown, padding_x, middle_y) !=
            blend_over(pixel(before, padding_x, middle_y), dark, kPanelOpacity))
            throw std::runtime_error(what + " did not darken the battlefield under its panel");
        // The graph: a sunken edge, light at the bottom, under the table.
        const auto& graph = place.graph;
        if (!inside(graph, box) ||
            pixel(shown, graph.x + graph.width / 2, graph.y + graph.height) != light ||
            pixel(shown, graph.x - 1, graph.y + graph.height / 2) != dark)
            throw std::runtime_error(what + " drew no graph in its panel");
        return place;
    };
    const auto first_place = check_panel(capture(), "shown");

    // Two seconds of frames, most on time, some over the frame's allowance,
    // some over a tick and some past the graph's top, ending with one just
    // over a tick: the graph shows each column's longest frame at its
    // height and in its grade's colour, the newest at the right, over its
    // lines, and the table shows times of each grade, right-aligned.
    const auto kept = frame_stats_;
    const uint32_t rate = frame_stats_notes().paced_frames_per_second;
    const uint64_t allowance = frame_pacing::frame_allowance_ns(rate, FrameWait::precise);
    const uint64_t on_time = frame_pacing::frame_budget_ns(rate);
    const uint64_t late = (allowance + frame_pacing::kTickBudgetNs) / 2;
    const uint64_t slow = 2 * frame_pacing::kTickBudgetNs;
    const uint64_t last = frame_pacing::kTickBudgetNs + 3 * kNanosecondsPerMillisecond;
    constexpr int kLateEvery = 10;
    constexpr int kSlowEvery = 30;
    constexpr uint64_t kSeconds = 2;
    frame_stats_ = {};
    frame_stats_.allowance_ns = allowance;
    uint64_t now = 0;
    // The synthetic seconds are read once they are all rolled.
    std::ignore = roll_frame_stats(frame_stats_, now);
    for (int frame = 0; now < kSeconds * frame_pacing::kNanosecondsPerSecond; ++frame) {
        const uint64_t frame_ns = frame % kSlowEvery == kSlowEvery - 1   ? slow
                                  : frame % kLateEvery == kLateEvery - 1 ? late
                                                                         : on_time;
        now += frame_ns;
        note_frame_measure(frame_stats_, FrameMeasure::frame, frame_ns);
        note_frame_measure(frame_stats_, FrameMeasure::work, frame_ns);
        std::ignore = roll_frame_stats(frame_stats_, now);
    }
    note_frame_measure(frame_stats_, FrameMeasure::frame, last);
    const auto synthetic = frame_stats_;
    const auto stressed = capture();
    frame_stats_ = kept;
    const auto place = check_panel(stressed, "with late frames");
    // The panel and its columns stay where they were, whatever the figures.
    const auto same_box = [](const oa::ui::display_layout::Rect& a,
                             const oa::ui::display_layout::Rect& b) {
        return a.x == b.x && a.y == b.y && a.width == b.width && a.height == b.height;
    };
    if (!same_box(place.panel, first_place.panel) || !same_box(place.graph, first_place.graph) ||
        place.value_right != first_place.value_right || place.row_top != first_place.row_top)
        throw std::runtime_error("console check: +stats moved its panel or its columns");
    const auto& graph = place.graph;
    const int scale = place.scale;
    const int floor_y = graph.y + graph.height;
    const int tick_y = floor_y - panel::line_rise(frame_pacing::kTickBudgetNs) * scale;
    const int dot_y = floor_y - panel::line_rise(allowance) * scale;
    std::size_t under_tick_line = frame_pacing::kFrameGraphColumns;
    for (std::size_t age = 0; age < frame_pacing::kFrameGraphColumns; ++age) {
        const auto column = frame_pacing::frame_history_column(synthetic.history, age);
        const int x = graph.x + graph.width - static_cast<int>(age + 1) * scale;
        const int rise = panel::bar_height(column.frame_ns) * scale;
        const auto color = ui_color_rgb(panel::severity_slot(column.severity, kNoteSlot));
        const bool capped = column.frame_ns >= panel::kGraphTopNs;
        const auto top_color = capped ? ui_color_rgb(static_cast<uint8_t>(kUiColorText)) : color;
        // A bar as high as a line's row covers it.
        const auto covers = [&](int line_y) {
            return line_y < floor_y - rise + (capped ? scale : 0) ||
                   pixel(stressed, x, line_y) == color;
        };
        if (column.severity == TimeSeverity::none || pixel(stressed, x, floor_y - 1) != color ||
            pixel(stressed, x, floor_y - rise) != top_color ||
            (!capped && pixel(stressed, x, floor_y - rise - 1) == color) || !covers(tick_y) ||
            !covers(dot_y))
            throw std::runtime_error(
                "console check: +stats drew the graph's column " + std::to_string(age) +
                " from the right at the wrong height or in the wrong colour"
            );
        if (column.frame_ns < on_time + kNanosecondsPerMillisecond)
            under_tick_line = age;
    }
    if (frame_pacing::frame_history_column(synthetic.history, 0).severity !=
        TimeSeverity::over_tick)
        throw std::runtime_error("console check: +stats did not graph the newest frame last");
    // The lines, behind the bars, over an on-time column: the tick's gray,
    // and at the frame's allowance a dot or a gap in its fainter gray.
    if (under_tick_line == frame_pacing::kFrameGraphColumns)
        throw std::runtime_error("console check: +stats graphed no frame on time");
    const int line_x = graph.x + graph.width - static_cast<int>(under_tick_line + 1) * scale;
    const auto well = [&](int x, int y) {
        return blend_over(
            blend_over(pixel(before, x, y), dark, kPanelOpacity), dark, kGraphOpacity
        );
    };
    const auto line_color = ui_color_rgb(kGraphLineSlot);
    if (pixel(stressed, line_x, tick_y) !=
        blend_over(well(line_x, tick_y), line_color, kTickLineOpacity))
        throw std::runtime_error("console check: +stats drew no line at a tick behind its bars");
    if (allowance < frame_pacing::kTickBudgetNs) {
        const bool dot = (line_x - graph.x) / scale % panel::kDotPitch == 0;
        const auto expected =
            dot ? blend_over(well(line_x, dot_y), line_color, kAllowanceLineOpacity)
                : well(line_x, dot_y);
        if (pixel(stressed, line_x, dot_y) != expected)
            throw std::runtime_error(
                "console check: +stats drew no dotted line at the frame's allowance"
            );
    }
    // Each grade's colour in the table, and the frame row's times
    // right-aligned in their columns, the longest, over a tick, on a red
    // cell.
    oa::ui::display_layout::Rect table_area = place.panel;
    table_area.height = graph.y - place.panel.y;
    const auto counted = [&](const oa::ui::display_layout::Rect& area, uint8_t slot) {
        const auto color = ui_color_rgb(slot);
        std::size_t count = 0;
        for (int y = area.y; y < area.y + area.height; ++y)
            for (int x = area.x; x < area.x + area.width; ++x)
                count += pixel(stressed, x, y) == color ? 1 : 0;
        return count;
    };
    constexpr std::array kGrades{
        TimeSeverity::within_frame, TimeSeverity::within_tick, TimeSeverity::over_tick
    };
    for (const auto grade : kGrades) {
        // An allowance of a tick or more (no limit, or an idle rate) grades
        // no time within a tick.
        if (frame_pacing::time_severity(late, allowance) != TimeSeverity::within_tick &&
            grade == TimeSeverity::within_tick)
            continue;
        const auto slot = panel::severity_slot(grade, kNoteSlot);
        if (counted(graph, slot) == 0 || counted(table_area, slot) == 0)
            throw std::runtime_error(
                "console check: +stats did not show a time of each grade in its colour"
            );
    }
    const auto table = frame_pacing::frame_stats_table(
        synthetic, frame_stats_notes(), frame_stats_renderer(), frame_stats_display()
    );
    // The last row names how the screen is shown and the frame the match is
    // drawn at: here a window, or none in a headless run.
    const auto& display_row = table.rows[table.row_count - 1];
    const std::string shown_screen = sdl_.window == nullptr
                                         ? std::string("no window")
                                         : "window " + std::to_string(match_layout_.width) + "x" +
                                               std::to_string(match_layout_.height);
    const bool windowed =
        sdl_.window == nullptr || (SDL_GetWindowFlags(sdl_.window) & SDL_WINDOW_FULLSCREEN) == 0;
    if (display_row.kind != FrameStatsRowKind::display ||
        (windowed && display_row.label.view() != shown_screen))
        throw std::runtime_error(
            "console check: +stats named the screen as \"" + std::string(display_row.label.view()) +
            "\", not \"" + shown_screen + "\""
        );
    const auto frame_row = static_cast<std::size_t>(
        std::find_if(
            table.rows.begin(),
            table.rows.end(),
            [](const auto& row) { return row.kind == FrameStatsRowKind::measure; }
        ) -
        table.rows.begin()
    );
    const int glyph_rows =
        (static_cast<int>(oa::formats::fnt::line_height(*font)) - kLineGapRows) * scale;
    const int row_top = place.row_top[frame_row];
    for (std::size_t column = 0; column < frame_pacing::kFrameStatsValueColumns; ++column) {
        const auto& value = table.rows[frame_row].values[column];
        const int right = place.value_right[column];
        const oa::ui::display_layout::Rect end_of_value{
            right - 2 * scale, row_top, 2 * scale, glyph_rows
        };
        const oa::ui::display_layout::Rect after_value{
            right, row_top, panel::kColumnGap * scale, glyph_rows
        };
        const auto slot = panel::severity_slot(value.severity, kNoteSlot);
        if (value.view().empty() || counted(end_of_value, slot) == 0 ||
            counted(after_value, slot) != 0)
            throw std::runtime_error(
                "console check: +stats did not end the frame row's times at their columns' right "
                "edge"
            );
    }
    const auto& longest = table.rows[frame_row].values[frame_pacing::kFrameStatsMostColumn];
    const int cell_x = place.value_right[frame_pacing::kFrameStatsMostColumn] - 1;
    const int cell_y = row_top + glyph_rows - 1;
    if (longest.severity != TimeSeverity::over_tick ||
        pixel(stressed, cell_x, cell_y) !=
            blend_over(
                blend_over(pixel(before, cell_x, cell_y), dark, kPanelOpacity),
                ui_color_rgb(panel::severity_slot(TimeSeverity::over_tick, kNoteSlot)),
                kOverTickCellOpacity
            ))
        throw std::runtime_error(
            "console check: +stats drew no red cell behind a time over a tick"
        );

    enter_line("+stats 0");
    if (frame_stats_shown_ || changed(before, capture()) != 0 || frame_stats_place_)
        throw std::runtime_error("console check: +stats 0 left the frame statistics drawn");
    enter_line("+stats");
    if (!frame_stats_shown_)
        throw std::runtime_error("console check: +stats did not show the statistics again");
    enter_line("+stats");
    if (frame_stats_shown_ || changed(before, capture()) != 0)
        throw std::runtime_error("console check: +stats typed again left the statistics drawn");
}

} // namespace oa::app

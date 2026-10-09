// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// How the application loop paces its frames apart from the simulation's 30
// ticks a second: the frame pacer and the rate it keeps, the fraction of a
// tick each frame shows (the presentation fraction the match drawing reads),
// the camera's scroll for a frame's real time, and the frame statistics the
// "+stats" overlay shows: each measure's spread over a second, the latest
// two seconds of frames, and how each time compares with the frame's
// allowance and a tick. Nothing here reads or writes the match: the loop
// hands it times and counts, and it hands back waits, fractions and the
// overlay's table.
#pragma once

#include "oa/base/game_loop.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace oa::app::frame_pacing {

/// Nanoseconds in a second.
inline constexpr uint64_t kNanosecondsPerSecond = 1'000'000'000;
/// Nanoseconds in a millisecond.
inline constexpr uint64_t kNanosecondsPerMillisecond = 1'000'000;
/// Frames a second while nothing on the screen moves on its own: a menu, a
/// paused match or a match under its menu, with no input for a while. It is
/// the rate the frontend's own clock steps at, so its animations keep their
/// pace.
inline constexpr uint32_t kIdleFramesPerSecond = 30;
/// How long after the last input event the loop keeps its full rate, in
/// nanoseconds, so that the pointer and what follows it stay smooth.
inline constexpr uint64_t kInputActivityNs = 500 * kNanosecondsPerMillisecond;
/// Ticks a second of the simulation at normal speed: the units of the match
/// clock a real second holds, one tick each.
inline constexpr uint32_t kTicksPerSecond = 30;

/// The schedule of the loop's frames: a run of frames due one period apart
/// from a start, which begins again whenever a frame starts early or ends
/// late, or the rate changes. At the tick rate (kTicksPerSecond) every frame
/// is due at the middle of a match clock unit instead, as the first of a run
/// of its own (end_paced_frame).
struct FramePacer {
    uint64_t run_start_ns{};      ///< when the run of evenly spaced frames began
    uint64_t frames_in_run{};     ///< frames of the run started since run_start_ns
    uint64_t frame_start_ns{};    ///< the time the frame under way stands for
    uint32_t frames_per_second{}; ///< the rate the run keeps; 0 before the first frame
    bool started{};               ///< a frame has started
};

/// Notes that a frame starts and returns the time the frame stands for.
///
/// A frame that starts within a period after it is due, because the wait
/// overslept a little, keeps the run and stands for its due time, so the
/// frames stand for evenly spaced times and the oversleep does not add up
/// from frame to frame. The first frame, one that starts before it is due
/// (an input event woke the loop early) and one that starts a period or more
/// late begin a new run from now and stand for now.
///
/// @param[in,out] pacer the loop's schedule
/// @param now_ns the frame's start, nanoseconds on a steady clock
/// @return the frame's time, nanoseconds on the same clock: its due time,
///     or now_ns
[[nodiscard]] uint64_t begin_paced_frame(FramePacer& pacer, uint64_t now_ns) noexcept;

/// Reads the steady clock the loop paces its frames on and measures them
/// by.
///
/// @return nanoseconds since the clock's epoch
[[nodiscard]] uint64_t steady_now_ns() noexcept;

/// Schedules the next frame and returns how long to wait for it.
///
/// Frames are due 1 / frames_per_second apart, counted from the start of the
/// run, so the spacing does not drift. A frame that ends before the next is
/// due waits for it, whatever the frame took. One that ends late starts the
/// next at once and begins a new run from it: a late frame is never followed
/// by frames closer together to catch up. A change of rate begins a new run
/// from the start of the frame that ends.
///
/// At the tick rate (kTicksPerSecond) the next frame is due at the middle of
/// the match clock unit after the one the ending frame's time lies in
/// (next_clock_unit_middle), so frames on time stand a clock unit apart, each
/// at the middle of its unit, and each steps the match clock by exactly one
/// unit, one tick at normal speed, wherever the clock's whole milliseconds
/// turn its units over. Frames spaced evenly at that rate from any start
/// instead would, from some starts, step it by none and then two.
///
/// @param[in,out] pacer the loop's schedule
/// @param now_ns the frame's end, nanoseconds on the same clock
/// @param frames_per_second the rate to keep; 0 waits for nothing
/// @return nanoseconds to wait before the next frame starts
[[nodiscard]] uint64_t
end_paced_frame(FramePacer& pacer, uint64_t now_ns, uint32_t frames_per_second) noexcept;

/// Returns when the frame numbered `frame` of a run is due.
///
/// @param run_start_ns when the run began, nanoseconds
/// @param frame frames since the run began
/// @param frames_per_second the run's rate, above 0
/// @return the frame's due time, nanoseconds; frame / frames_per_second
///     seconds after the start, rounded down to a nanosecond
[[nodiscard]] uint64_t
paced_frame_due(uint64_t run_start_ns, uint64_t frame, uint32_t frames_per_second) noexcept;

/// Returns the middle of the match clock unit after the one a time lies in.
///
/// The match clock reads the time's whole milliseconds, wrapped to 32 bits,
/// as scaled_clock reads them at kTicksPerSecond units a second (the product
/// wrapping at 32 bits). The result lies half a unit into the next unit, at
/// least 16 ms from either end of it as the clock reads whole milliseconds,
/// so the clock reads exactly one unit more there than at the time, and the
/// clock's fraction of that unit (next_presentation_alpha) is a half. A unit
/// the clock's wrap-around cuts short is not allowed for.
///
/// @param time_ns the time, nanoseconds on the clock whose milliseconds the
///     match clock reads
/// @return the time of the next unit's middle, nanoseconds on the same
///     clock; from about half a unit to a unit and a half after time_ns
[[nodiscard]] uint64_t next_clock_unit_middle(uint64_t time_ns) noexcept;

/// What moved, or may move, on the screen in the frame just drawn, which
/// decides the rate the loop keeps.
struct FrameActivity {
    bool match_advancing{}; ///< the running match's clock steps and is not paused
    bool camera_moving{};   ///< the camera scrolled or the zoom eased this frame
    bool input_recent{};    ///< an input event came within kInputActivityNs, or a button is held
    bool unattended{};      ///< a scripted run nobody watches (Options::unattended)
};

/// Chooses the frames a second the loop keeps.
///
/// The full rate while anything moves or may: a stepping match (a
/// multiplayer match waiting on another machine among them), a moving
/// camera, recent input, or a scripted run, which keeps one rate
/// throughout. Otherwise kIdleFramesPerSecond, or the full rate when that is
/// lower, so that an idle or paused game, a multiplayer one included, uses
/// little of the processor; the loop still runs the extension's pump each
/// frame.
///
/// @param max_frames_per_second the full rate (Options::max_frames_per_second);
///     0 for no limit
/// @param activity what moved in the frame just drawn
/// @return frames a second to pace the next frame at; 0 for no limit
[[nodiscard]] uint32_t
paced_frame_rate(uint32_t max_frames_per_second, const FrameActivity& activity) noexcept;

/// The highest cap vsync_frame_cap gives, in frames a second: the highest
/// rate the loop keeps under any limit.
inline constexpr uint32_t kHighestVsyncCap = 1000;

/// Returns the most frames a second the loop keeps while Vertical sync is in
/// effect: the largest whole rate below the display's refresh rate, so that
/// each present finds the display ready, frames keep their even spacing and
/// none queue up. It is never under kTicksPerSecond, so each tick is still
/// drawn, nor over kHighestVsyncCap.
///
/// @param display_hz the display's refresh rate, in hertz; 0 when the
///     display reports none
/// @return frames a second: 59 at 60 Hz and at 59.94 Hz, 119 at 120 Hz, 143
///     at 144 Hz, kTicksPerSecond at 30 Hz or less; 0, for no cap, when the
///     display reports no rate
[[nodiscard]] uint32_t vsync_frame_cap(float display_hz) noexcept;

/// Returns the frames a second the loop keeps under a cap: the lower of the
/// two, where 0 stands for no limit and no cap.
///
/// @param frames_per_second the rate the loop would keep (paced_frame_rate);
///     0 for no limit
/// @param cap the cap (vsync_frame_cap); 0 for none
/// @return frames a second; 0 for no limit
[[nodiscard]] uint32_t capped_frame_rate(uint32_t frames_per_second, uint32_t cap) noexcept;

/// The presentation fraction from frame to frame: how far between the
/// previous tick's state and the current tick's the last frame was drawn,
/// and whether frames hold the current state whole until more ticks run.
struct TickPresentation {
    float alpha{1.0F}; ///< the last frame's fraction, 0 to 1
    bool hold{true};   ///< frames show the current state whole until a batch of ticks runs
};

/// What the frame's clock step did, as the presentation fraction needs it.
struct FrameTicks {
    /// The frame shows the current state whole: the match clock does not step
    /// (no match, a menu over a match played alone, the outcome), the match is
    /// paused, or the frame is one that must show the whole tick (a check's
    /// fixed clock, a film frame).
    bool whole{};
    int32_t owed{}; ///< steps the clock offered this frame (Timing::pending_steps)
    uint32_t ran{}; ///< ticks the match advanced this frame
    /// Frames are drawn one a clock unit: the loop paces them at the tick
    /// rate (kTicksPerSecond), or a run draws them at that rate.
    bool unit_frames{};
};

/// Returns the presentation fraction of the frame about to be drawn.
///
/// The fraction says how far between the state before the latest batch of
/// ticks (the previous state) and the state after it (the current state)
/// the frame shows: 0 the previous, 1 the current. It never goes past the
/// current tick, so a frame shows the match at most one batch of ticks
/// behind. A batch is the ticks one frame runs: one at normal speed, more
/// above it.
///
/// It follows the clock that runs the ticks: the clock units since the last
/// clock step (Timing::previous_clock), and the fraction of the current unit,
/// from the same milliseconds scaled_clock reads and finer within the
/// millisecond. At normal speed and above, a batch runs at each unit and
/// the fraction is the time since in units; below normal speed a tick runs
/// every few units and the fraction adds Timing::remainder, the progress
/// toward the next tick, to the units since times the speed. Between batches
/// the fraction never goes back, even when the clock's time is set back (a
/// screenshot or film frame resets it); a batch starts it again.
///
/// When frames are drawn one a clock unit (`ticks.unit_frames`), a frame
/// counts the current unit whole, as the next frame is due at the next unit:
/// at normal speed and above it shows the state its batch of ticks reached
/// whole, so that at normal speed each frame draws one tick and the next
/// frame the next, and below normal speed it shows the progress the clock
/// makes by the unit's end, so that frames still move evenly between the
/// ticks.
///
/// Frames hold the current state whole (fraction 1) while `ticks.whole`
/// says so, and from a frame that owed steps none of which ran (the match
/// waits on another machine), ran fewer ticks than it owed, or ran more
/// than a batch at its speed holds (catching up after a long frame), until
/// a batch of ticks runs normally again.
///
/// @param[in,out] presentation the fraction from frame to frame
/// @param timing the match clock after this frame's step
/// @param ticks what this frame's clock step did
/// @param now_ns the time the frame stands for (begin_paced_frame), the
///     one the match clock stepped to, nanoseconds on the clock whose
///     milliseconds the match clock reads
/// @return the frame's fraction, 0 to 1
[[nodiscard]] float next_presentation_alpha(
    TickPresentation& presentation,
    const oa::base::game_loop::Timing& timing,
    const FrameTicks& ticks,
    uint64_t now_ns
) noexcept;

/// Returns the map pixels the camera scrolls in a stretch of real time.
///
/// The scroll speed is pixels per clock unit (30 a second), as in 3.1c, so a
/// second of scrolling covers the same ground at any frame rate. The result
/// keeps its fraction, which the caller carries from frame to frame, so the
/// camera moves by a steady amount each frame instead of a whole step at
/// each clock unit. A frame of a clock unit or more scrolls at most
/// oa::ui::hud::kMaxScrollStep pixels, as a 3.1c frame does; a shorter one at
/// most its share of that, so that a speed above kMaxScrollStep a unit
/// covers the same ground a second at every rate.
///
/// @param scroll_speed Game.scroll_speed, pixels per clock unit
/// @param elapsed_ns real time since the previous frame, nanoseconds
/// @return pixels to scroll, 0 to kMaxScrollStep
[[nodiscard]] double scroll_distance(uint8_t scroll_speed, uint64_t elapsed_ns) noexcept;

/// The parts of a frame the statistics time.
enum class FrameMeasure : uint8_t {
    frame,   ///< from one frame's start to the next's
    work,    ///< a frame's events, ticks, drawing and presenting, without the wait
    tick,    ///< one simulation tick
    draw,    ///< composing the frame's pictures on the processor
    present, ///< uploading the composed pictures and presenting them
};

/// Number of FrameMeasure values.
inline constexpr std::size_t kFrameMeasureCount = 5;

/// A simulation tick at normal speed, in nanoseconds: the longest a frame
/// can take without the match clock owing more than one tick.
inline constexpr uint64_t kTickBudgetNs = kNanosecondsPerSecond / kTicksPerSecond;
/// How far past its period a frame after a precise wait may run and still
/// count as on time, in nanoseconds: about what that wait oversleeps by.
inline constexpr uint64_t kFrameBudgetSlackNs = 500'000;
/// How far past its period a frame after an idle wait may run and still
/// count as on time, in nanoseconds: the idle wait is rounded up to whole
/// milliseconds, and the event wait's timer oversleeps by up to about
/// another.
inline constexpr uint64_t kIdleWaitSlackNs = 2'000'000;

/// How the loop waited for a frame to be due.
enum class FrameWait : uint8_t {
    precise, ///< to the nanosecond, sleeping and then spinning, as at the full rate
    idle,    ///< for the next event, at most the time left rounded up to whole milliseconds
};

/// How a time compares with the frame's allowance and a simulation tick.
enum class TimeSeverity : uint8_t {
    none,         ///< nothing to grade: a heading, a count, or no time measured yet
    within_frame, ///< within the frame's allowance (frame_allowance_ns)
    within_tick,  ///< over the frame's allowance, within a simulation tick
    over_tick,    ///< over the frame's allowance and over a simulation tick
};

/// Returns the time one frame has at a frame rate.
///
/// @param frames_per_second the rate the loop keeps; 0 for no limit
/// @return 1 / frames_per_second seconds in nanoseconds, rounded down; a
///     tick (kTickBudgetNs) for no limit
[[nodiscard]] uint64_t frame_budget_ns(uint32_t frames_per_second) noexcept;

/// Returns the longest a frame may take and still count as on time.
///
/// @param frames_per_second the rate the loop keeps; 0 for no limit
/// @param wait how the loop waits for the frame to be due
/// @return frame_budget_ns, and kFrameBudgetSlackNs after a precise wait or
///     kIdleWaitSlackNs after an idle one, in nanoseconds
[[nodiscard]] uint64_t frame_allowance_ns(uint32_t frames_per_second, FrameWait wait) noexcept;

/// Grades a time against a frame's allowance and a simulation tick.
///
/// Up to the allowance, a time is within_frame; past it and up to a tick
/// (kTickBudgetNs), within_tick; longer, over_tick. An allowance of a tick
/// or more leaves no time within_tick.
///
/// @param elapsed_ns the time, nanoseconds
/// @param allowance_ns the frame's allowance (frame_allowance_ns), nanoseconds
/// @return its grade, never TimeSeverity::none
[[nodiscard]] TimeSeverity time_severity(uint64_t elapsed_ns, uint64_t allowance_ns) noexcept;

/// The spread of one measure over a second, each sample graded as it is taken.
struct MeasureSpread {
    uint64_t total_ns{};           ///< sum of the samples
    uint64_t least_ns{};           ///< the shortest sample; 0 without samples
    uint64_t most_ns{};            ///< the longest sample
    uint64_t allowance_total_ns{}; ///< sum of the frame allowances the samples were taken under
    uint32_t count{};              ///< samples taken
    TimeSeverity least_severity{}; ///< the shortest sample's grade when it was taken
    TimeSeverity most_severity{};  ///< the longest sample's grade when it was taken
};

/// Seconds of frames the frame graph shows.
inline constexpr uint32_t kFrameGraphSeconds = 2;
/// Columns a second of the frame graph: one a frame at the loop's default
/// rate (kDefaultMaxFramesPerSecond).
inline constexpr uint32_t kFrameGraphColumnsPerSecond = 120;
/// Columns of the frame graph: kFrameGraphSeconds of them a second.
inline constexpr std::size_t kFrameGraphColumns =
    std::size_t{kFrameGraphSeconds} * kFrameGraphColumnsPerSecond;
/// The frame allowance the statistics grade against until the loop sets
/// one: a precise wait at kFrameGraphColumnsPerSecond.
inline constexpr uint64_t kDefaultFrameAllowanceNs =
    kNanosecondsPerSecond / kFrameGraphColumnsPerSecond + kFrameBudgetSlackNs;

/// The latest kFrameGraphSeconds of frames, laid end to end in the order
/// they ran and cut into columns 1 / kFrameGraphColumnsPerSecond seconds
/// wide. A frame covers each column whose middle lies after its start and
/// no later than its end, or, when none does, the column its end lies in;
/// each column holds the longest frame that covers it and that frame's
/// grade.
struct FrameHistory {
    /// Each column's longest frame, nanoseconds, by column number modulo
    /// kFrameGraphColumns.
    std::array<uint64_t, kFrameGraphColumns> longest_ns{};
    /// That frame's grade; TimeSeverity::none for a column no frame covers.
    std::array<TimeSeverity, kFrameGraphColumns> severity{};
    uint64_t end_ns{}; ///< the frames' times summed: where the latest frame ends
    uint64_t
        newest_column{}; ///< the newest column a frame covers, numbered from 0 at the first frame's start
};

/// Adds a frame to the history after the latest; the columns it covers
/// past the newest so far start empty, and the oldest leave the history.
///
/// @param[in,out] history the latest frames
/// @param elapsed_ns the frame's time, nanoseconds
/// @param severity the frame's grade when it ran
void note_frame_history(FrameHistory& history, uint64_t elapsed_ns, TimeSeverity severity) noexcept;

/// One column of the frame history.
struct FrameColumn {
    uint64_t frame_ns{};     ///< the longest frame covering it, nanoseconds; 0 for none
    TimeSeverity severity{}; ///< that frame's grade; TimeSeverity::none for none
};

/// Returns one column of the frame history.
///
/// @param history the latest frames
/// @param age columns before the newest a frame covers: 0 for the newest
/// @return the column; empty for an age of kFrameGraphColumns or more, or
///     before the first frame
[[nodiscard]] FrameColumn
frame_history_column(const FrameHistory& history, std::size_t age) noexcept;

/// The frame statistics: each measure's samples over the second being
/// measured, and over the last whole second, which the overlay shows, and
/// the latest frames, which its graph shows. Every sample is graded as it
/// is noted, against the allowance of the frame it belongs to.
struct FrameStatsWindow {
    uint64_t measuring_since_ns{}; ///< start of the second being measured
    bool started{};                ///< the first second has begun
    std::array<MeasureSpread, kFrameMeasureCount> measuring{}; ///< the second under way
    std::array<MeasureSpread, kFrameMeasureCount> shown{};     ///< the last whole second
    uint64_t shown_span_ns{}; ///< how long the shown second lasted; 0 before the first
    FrameHistory history{};   ///< the latest frames (FrameMeasure::frame)
    /// The allowance (frame_allowance_ns) the samples noted from now on are
    /// graded against, nanoseconds; the loop sets it at each frame's start.
    uint64_t allowance_ns{kDefaultFrameAllowanceNs};
};

/// Adds a sample of one measure to the second being measured, graded
/// against the window's allowance; a frame's time goes into the frame
/// history too.
///
/// @param[in,out] window the statistics
/// @param measure what was timed
/// @param elapsed_ns how long it took, nanoseconds
void note_frame_measure(
    FrameStatsWindow& window, FrameMeasure measure, uint64_t elapsed_ns
) noexcept;

/// Starts the statistics' first second, or ends the second being measured
/// once a second has passed and makes it the one shown.
///
/// @param[in,out] window the statistics
/// @param now_ns the time, nanoseconds on a steady clock
/// @return true when a second ended and the shown statistics changed
bool roll_frame_stats(FrameStatsWindow& window, uint64_t now_ns) noexcept;

/// What the overlay shows besides the measures.
struct FrameStatsNotes {
    uint32_t max_frames_per_second{};   ///< the full rate (--max-fps); 0 for no limit
    uint32_t paced_frames_per_second{}; ///< the rate the loop keeps now; 0 for no limit
    uint32_t units_drawn{};             ///< unit models the last frame drew
    uint32_t units_between_ticks{};     ///< of those, drawn between two ticks
};

/// What the "+stats" table's renderer row names. The tier, its
/// anti-aliasing and the driver, joined, and the adapter are each cut to
/// one of the table's texts, never inside a character (whole_characters).
struct FrameStatsRenderer {
    std::string_view tier{}; ///< the tier frames are drawn in: "standard", "basic" or "full"
    /// The anti-aliasing the full tier draws the battlefield with, "2x" or
    /// "4x", after the tier; empty with none or in another tier.
    std::string_view anti_aliasing{};
    std::string_view driver{};  ///< SDL's name for the render driver; empty without a renderer
    std::string_view adapter{}; ///< the adapter's name; empty where none was read
    /// A limit of the machine the tier draws within, which the row's note
    /// names in place of the adapter: "sprite memory full" while the full
    /// tier leaves out of a frame what its pages may not hold; empty for none.
    std::string_view limit{};
};

/// How the game's window shows the screen, as the "+stats" table's display
/// row names it.
enum class FrameStatsScreen : uint8_t {
    none,        ///< no window
    window,      ///< a window on the desktop
    full_screen, ///< full screen on the display's desktop mode
    exclusive,   ///< full screen at a display mode of the game's own
};

/// What the "+stats" table's display row names: how the window shows the
/// screen, the size the match is drawn at, the display's mode and its scale.
struct FrameStatsDisplay {
    FrameStatsScreen screen{}; ///< a window, or full screen and how
    /// The frame the match is laid out and drawn at, in its own pixels: the
    /// window's size in a window and on the display's own mode, and the
    /// screen size chosen where the frame is scaled to the screen.
    int32_t frame_width{};
    int32_t frame_height{}; ///< the frame's height, as frame_width
    /// The display's mode now, in the window system's units; 0 when not known.
    int32_t mode_width{};
    int32_t mode_height{}; ///< the mode's height, as mode_width
    float refresh_rate{};  ///< the mode's frames a second; 0 when not known
    /// The display's pixels to a unit of the window's size, times the
    /// system's scale for text and controls (SDL_GetWindowDisplayScale):
    /// 2 on a Retina display, 1.5 at Windows' 150%; 0 when not known.
    float display_scale{};
};

/// What a frame's drawing showed of the units. The loop clears it before each
/// match frame is drawn; the unit drawing adds to it, and the frame
/// statistics and the frame log of a --frame-rate run read it.
struct FrameDrawCounts {
    uint32_t units_drawn{};         ///< unit models drawn
    uint32_t units_between_ticks{}; ///< of those, drawn between two ticks' states
    uint16_t probe_unit{};          ///< unit slot the frame log follows; 0 for none
    bool probe_drawn{};             ///< the drawing drew the probe unit this frame
    int32_t probe_x{};              ///< where it drew the probe unit: X, 16.16 map units
    int32_t probe_z{};              ///< where it drew the probe unit: Z, 16.16 map units
};

/// Bytes one text of the "+stats" table holds, its terminating zero included.
inline constexpr std::size_t kFrameStatsTextBytes = 24;
/// Value columns of the "+stats" table: a measure's least, mean and most.
inline constexpr std::size_t kFrameStatsValueColumns = 3;
/// The value column of a measure's least time.
inline constexpr std::size_t kFrameStatsLeastColumn = 0;
/// The value column of a measure's mean time, where a row of one value shows it.
inline constexpr std::size_t kFrameStatsMeanColumn = 1;
/// The value column of a measure's most time.
inline constexpr std::size_t kFrameStatsMostColumn = 2;
/// Rows of the "+stats" table: the title, the column names, the frames a
/// second, one for each measure, the units drawn, the renderer and the
/// display.
inline constexpr std::size_t kFrameStatsRowsMost = 3 + kFrameMeasureCount + 3;

/// Returns the start of a UTF-8 text that fits in a number of bytes and
/// does not end inside a character: the whole text when it fits, else the
/// text cut at the start of the character the limit falls in.
///
/// @param text the text
/// @param bytes the most bytes kept
/// @return the start of text
[[nodiscard]] std::string_view whole_characters(std::string_view text, std::size_t bytes) noexcept;

/// One text of the "+stats" table and the grade of the time it shows.
struct FrameStatsText {
    std::array<char, kFrameStatsTextBytes> text{}; ///< zero-terminated; empty for none
    TimeSeverity severity{};                       ///< TimeSeverity::none for no time

    /// Returns the text.
    ///
    /// @return the characters before the terminating zero
    [[nodiscard]] std::string_view view() const noexcept;
};

/// What a row of the "+stats" table shows.
enum class FrameStatsRowKind : uint8_t {
    title,   ///< the table's title and the unit of its times
    heading, ///< the value columns' names
    rate,    ///< the frames drawn in the last second, and the rate the loop keeps
    measure, ///< a measure's least, mean and most time, in milliseconds
    count,   ///< the units the last frame drew, and those drawn between two ticks
    /// the tier frames are drawn in and the render driver, which run on
    /// across the columns as the title does, and the adapter as the note;
    /// the panel cuts the row where it would pass its width
    renderer,
    /// how the window shows the screen and the size the match is drawn at,
    /// with the display's mode, refresh rate and scale as the note; it runs
    /// on and is cut as the renderer row is
    display,
};

/// Tells whether a row of the "+stats" table runs on across the columns
/// from the labels' left edge, cut where it would pass the panel's width,
/// and keeps no room in the columns.
///
/// @param kind what the row shows
/// @return true for the renderer and display rows
[[nodiscard]] constexpr bool runs_on(FrameStatsRowKind kind) noexcept {
    return kind == FrameStatsRowKind::renderer || kind == FrameStatsRowKind::display;
}

/// One row of the "+stats" table: a label, up to three right-aligned
/// values, and a note after the row's last value.
struct FrameStatsRow {
    FrameStatsRowKind kind{};                                     ///< what the row shows
    FrameStatsText label{};                                       ///< the left column
    std::array<FrameStatsText, kFrameStatsValueColumns> values{}; ///< empty where none shows
    FrameStatsText note{}; ///< after the last value the row shows; empty for none
};

/// The "+stats" overlay's table, which its drawing lays out and colours.
struct FrameStatsTable {
    std::array<FrameStatsRow, kFrameStatsRowsMost> rows{}; ///< top to bottom
    std::size_t row_count{};                               ///< rows in use
};

/// Describes the "+stats" overlay's table.
///
/// The title, "Frame stats (ms)"; the value columns' names, "min", "avg"
/// and "max"; the frames drawn in the last whole second as the mean
/// column's value, graded by the mean frame time against the mean of the
/// frames' allowances, and the rate the loop keeps as the note ("limit
/// 120", "idle 30" or "no limit"); the frame, work, tick, draw and present
/// rows with their least, mean and most times in milliseconds, the least
/// and the most graded as they were when they were taken and the mean
/// against the mean of its samples' allowances, the tick row noting the
/// ticks a second; the units the last frame drew as the mean column's
/// value, with how many were drawn between two ticks as the note; and the
/// renderer: the tier and the render driver as "standard: metal", or the
/// tier alone without a driver, with the adapter as the note, each cut to
/// the kFrameStatsTextBytes - 1 bytes of a text, never inside a character;
/// and the display (display_row_texts). Before the first whole second, and
/// for a measure with no sample in it, the mean column reads "--".
///
/// @param window the statistics
/// @param notes the rates and counts to show beside them
/// @param renderer the renderer to name
/// @param display the window and the display to name
/// @return the table, kFrameStatsRowsMost rows
[[nodiscard]] FrameStatsTable frame_stats_table(
    const FrameStatsWindow& window,
    const FrameStatsNotes& notes,
    const FrameStatsRenderer& renderer,
    const FrameStatsDisplay& display = {}
) noexcept;

/// Fills the "+stats" table's display row: as its label, how the window
/// shows the screen, "window", "full screen" (on the display's desktop
/// mode) or "exclusive" (at a mode of the game's own), then the frame the
/// match is drawn at, as "full screen 1280x720"; as its note, the
/// display's mode, its refresh rate rounded to whole frames a second and
/// its scale with at most two decimals and no trailing zeros, as
/// "1728x1117@120 2x". A mode, rate or scale not known is left out, and
/// without a window the label reads "no window" with no note.
///
/// @param[out] row the row; its kind is set to FrameStatsRowKind::display
/// @param display the window and the display to name
void set_display_row(FrameStatsRow& row, const FrameStatsDisplay& display) noexcept;

/// Describes the "+stats" table with the widest text each of its cells
/// shows in play, which its layout reserves room for: the rows of
/// frame_stats_table, each time "000.00", the frames a second "0000" with
/// "limit 0000", the ticks a second "000/s", and "0000" units with "0000
/// between ticks". A time of a second or more, or a count past these, runs
/// past its column. The renderer and display rows hold no text: no room is
/// kept for them, and the panel cuts their texts where they would pass its
/// width, so the panel is as wide whatever they name.
///
/// @return the table, kFrameStatsRowsMost rows
[[nodiscard]] FrameStatsTable frame_stats_widest_table() noexcept;

} // namespace oa::app::frame_pacing

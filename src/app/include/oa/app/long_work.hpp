// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The progress dialog for a moment of work: a panel, a title, one line and a
// bar or a spinner, with no buttons. It is drawn over whatever is already on
// the window, only once the work has lasted a short while, and it closes on
// its own when the work is done and the line has been up long enough to read.
#pragma once

#include <SDL3/SDL.h>

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>

namespace oa::ui::frontend_renderer {
struct Surface;
}

namespace oa::ui::kit {
struct Frame;
}

namespace oa::ui::paint {
struct Canvas;
}

namespace oa::app {

class RendererHost;

/// How long work runs before the progress dialog appears, in milliseconds.
/// Quicker work shows nothing.
inline constexpr uint32_t long_work_show_after_ms = 300;

/// How long the progress dialog stays up once it has appeared, in
/// milliseconds, even when the work finishes first.
inline constexpr uint32_t long_work_stay_ms = 800;

/// What closing the window does while the progress dialog is up.
enum class LongWorkClose : uint8_t {
    after_job, ///< the close is put back and acted on once the job has finished
    ends_game, ///< the game ends at once, after the renderer records a clean exit
};

/// How far a piece of work has come, written by the worker and read by the
/// thread that draws. `done` of -1 is a spinner. Otherwise `done` and
/// `whole` are counts the dialog scales to a full bar.
struct LongWork {
    std::atomic<int32_t> done{-1};    ///< how far it has come; -1 for a spinner
    std::atomic<int32_t> whole{1000}; ///< the whole amount; ignored while done is -1
    std::atomic<bool> stop{false};    ///< set when a close ends the game during the job

    /// Replaces the line the dialog shows. `key` is an English catalogue key.
    ///
    /// @param key the line
    void use_line(std::string_view key);

    /// The line the job last published, or `fallback` when it has published none.
    ///
    /// @param fallback the line the dialog opened with
    /// @return the English key to show
    [[nodiscard]] std::string line_key(std::string_view fallback) const;

  private:

    mutable std::mutex line_mutex_; ///< guards line_key_
    std::string line_key_;          ///< the English key the job published
};

/// What one run of the progress dialog did.
struct LongWorkResult {
    bool shown{false};        ///< the dialog was drawn
    int distinct_turns{0};    ///< spinner eighths that were drawn, when a spinner showed
    bool bar{false};          ///< a bar was drawn
    bool closed_itself{true}; ///< nothing the player did was asked to close it
    int32_t least_bar{-1};    ///< the least bar amount drawn, or -1 when no bar was
    int32_t most_bar{-1};     ///< the greatest bar amount drawn, or -1 when no bar was
};

/// Tells whether the progress dialog's work is already running on this thread's
/// behalf. A nested call runs its work inline and draws no second dialog.
///
/// @return true while run_long_work is drawing for a job
[[nodiscard]] bool long_work_running() noexcept;

/// Paints the progress dialog once off the screen, at the window's size, so
/// what its first showing keeps for the rest of the process is kept now:
/// the bundled fonts with the glyphs of its words, and the app icon.
/// Nothing is shown.
///
/// @param window the game's window
/// @param renderer its renderer
/// @return true when it was painted; false when the window has no size or
///     the bundled fonts cannot be read
bool warm_progress_dialog(SDL_Window* window, SDL_Renderer* renderer) noexcept;

/// Fills a canvas with the quiet launch colour, or with `under` stretched
/// over it when that picture has pixels.
///
/// @param canvas the canvas
/// @param under the picture behind the dialog; null, or empty, is the quiet colour
void fill_progress_behind(
    oa::ui::paint::Canvas& canvas, const oa::ui::frontend_renderer::Surface* under
);

/// Paints the progress dialog centred on a frame, over `under` or the quiet
/// colour, dimmed. A progress below 0 draws the spinner at `turn`; otherwise
/// the bar is that far along a full bar.
///
/// @param canvas the window's pixels
/// @param frame where the dialog is centred, and its size class
/// @param title the header, as shown
/// @param line the one line, as shown
/// @param progress -1 for the spinner, otherwise 0 to a full bar
/// @param turn the spinner's eighth
/// @param under the picture behind the dialog; null is the quiet colour
void paint_progress_frame(
    oa::ui::paint::Canvas& canvas,
    const oa::ui::kit::Frame& frame,
    std::string_view title,
    std::string_view line,
    int32_t progress,
    uint32_t turn,
    const oa::ui::frontend_renderer::Surface* under
);

/// Shows the progress dialog once, its spinner still, over the quiet colour,
/// and leaves it on the window until the next picture replaces it: for work
/// on the thread that draws, such as a soft restart's. The texts are shown
/// as given. With no window, nothing is drawn.
///
/// @param window the window; null draws nothing
/// @param renderer the window's renderer; null draws nothing
/// @param host told of the presented frame; null is left untold
/// @param title the header, as shown
/// @param line the one line, as shown
void show_work_frame(
    SDL_Window* window,
    SDL_Renderer* renderer,
    RendererHost* host,
    std::string_view title,
    std::string_view line
);

/// Runs `job` off the thread that draws, and shows the progress dialog while
/// it lasts longer than long_work_show_after_ms.
///
/// The dialog is the work dialog: no buttons, over `behind`, animating until
/// the job has finished and the dialog has been up for long_work_stay_ms.
/// The line and the title are English keys of the interface catalogue,
/// read before the job starts and again once it has finished, since a job
/// may change the language the catalogue shows. A text the catalogue does
/// not hold shows as it is given, so the line may instead be one already in
/// the language shown, such as a soft restart's that names the mod switched
/// (Options::restart_line). The job may replace the
/// line while it runs (LongWork::use_line); the new line is read when it
/// changes, so a job that does must leave the language alone. Input is
/// discarded, and the window's events with it, though the pointer's hold in
/// full screen follows them (drop_window_events). A close is put back for
/// after the dialog has closed, unless `close` is LongWorkClose::ends_game:
/// the game then ends at once, after the renderer records a clean exit,
/// without waiting for the job. The dialog closes on its own.
///
/// With no window, or while a dialog is already running, `job` runs on the
/// caller and nothing is drawn. The job must not call back onto this thread
/// and wait. An exception from the job is thrown again after the dialog has
/// closed.
///
/// @param window the window; null runs the job inline
/// @param renderer the window's renderer; null runs the job inline
/// @param host told of each presented frame; null is left untold
/// @param title the header's English key
/// @param line the line's English key, or the line as shown
/// @param behind paints the picture the dialog lies on; it runs on the drawing thread
/// @param job the work; it may store how far it has come
/// @param show_after_ms how long the work runs before the dialog appears;
///        long_work_show_after_ms when the caller leaves it
/// @param close what closing the window does while the job runs
/// @return what the dialog did
[[nodiscard]] LongWorkResult run_long_work(
    SDL_Window* window,
    SDL_Renderer* renderer,
    RendererHost* host,
    std::string_view title,
    std::string_view line,
    const std::function<void(oa::ui::paint::Canvas&)>& behind,
    const std::function<void(LongWork&)>& job,
    uint32_t show_after_ms = long_work_show_after_ms,
    LongWorkClose close = LongWorkClose::after_job
);

/// Requires icloud_download_line, in English and in German, French, Spanish
/// and Italian, to fit the progress dialog at the 640 by 480 size.
///
/// Throws std::runtime_error when a language has no words or the line is cut.
void require_icloud_download_lines();

} // namespace oa::app

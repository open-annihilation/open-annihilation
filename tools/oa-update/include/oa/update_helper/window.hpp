// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The small window shown while an update is put in place. It uses the same
// work dialog the game shows for a long task. A quick swap shows nothing.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace oa::update_helper {

class Log;

/// Where the window's line and bar sit, in points. Fonts are not required.
struct PlacedUpdateWindow {
    int32_t width{};         ///< the dialog's width
    int32_t height{};        ///< the dialog's height
    int32_t line_x{};        ///< the line's left edge
    int32_t line_y{};        ///< the line's top
    int32_t line_width{};    ///< the line's width
    int32_t line_height{};   ///< the line's height
    std::string line_text{}; ///< the line, as placed
    int32_t bar_width{};     ///< the bar's width; 0 when a bar is not shown
    int32_t bar_height{};    ///< the bar's height
};

/// Places the window's text without fonts.
///
/// @param title the title
/// @param line the one line
/// @param progress the bar, 0 to 1000; below 0 draws no bar
/// @return the placed window
[[nodiscard]] PlacedUpdateWindow
place_update_window(std::string_view title, std::string_view line, int32_t progress);

/// The window. It opens on the first show that asks for it, and only after
/// the usual delay unless the caller forces it. A display that cannot open
/// is logged and the update continues.
class ProgressWindow {
  public:

    /// Starts closed. Nothing is opened yet.
    ProgressWindow();

    /// Closes the window and, when this window opened the display, leaves it.
    ~ProgressWindow();

    ProgressWindow(const ProgressWindow&) = delete;
    ProgressWindow& operator=(const ProgressWindow&) = delete;

    /// Where a failure to open the display is written. May be null.
    ///
    /// @param log the log
    void set_log(Log* log) noexcept { log_ = log; }

    /// Draws the window.
    ///
    /// Ignored until the delay has passed, unless `now` is set. A failure to
    /// open the display is remembered and later calls do nothing.
    ///
    /// @param title the title
    /// @param line the one line
    /// @param progress the bar, 0 to 1000
    /// @param now true to show at once, for a failure
    void show(std::string_view title, std::string_view line, int32_t progress, bool now);

    /// Closes the window. The update is not affected.
    void close();

  private:

    struct Shown;
    Shown* shown_{};
    bool unavailable_{};
    bool display_open_{};
    Log* log_{};
    int64_t started_ms_{};
};

} // namespace oa::update_helper

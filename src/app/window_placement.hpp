// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Where Open Annihilation's windows show on the OA layer: the Window size
// choice that overrides their size class, where a window that moves shows
// (dragged by its title, snapped to the room's centre and safe edges, kept
// on screen, remembered per window and size class in the preferences), and
// where the in-match Settings shows, measured on the whole window. Nothing
// here reads the window or the runtime: the OA layer (oa_layer.cpp) passes
// what it needs and keeps the preferences.
#pragma once

#include "oa/ui/display_layout.hpp"
#include "oa/ui/kit/layout.hpp"

#include <map>
#include <optional>
#include <stdint.h>
#include <string>
#include <string_view>

namespace oa::app {

/// Where the settings dialog shows in a match, and the size class and scale
/// it is laid out and drawn at there (window_placement::match_window).
struct MatchDialogPlace {
    /// The dialog's rectangle on the match's canvas, in its pixels: the
    /// class's dialog points at the scale (kit::to_canvas), or on a phone
    /// Compact fitted to the safe area.
    oa::ui::display_layout::Rect shown{};
    /// The class the dialog is laid out at: from the points of the window's
    /// safe area at the scale, or the Window size chosen; Compact on a phone.
    oa::ui::kit::SizeClass size_class{oa::ui::kit::SizeClass::compact};
    /// The scale the dialog is drawn at, in percent: a whole step times 100,
    /// or 150. 0 on a phone, where Compact is drawn at 1× and fitted to its
    /// place.
    int32_t scale_percent{100};
};

namespace window_placement {

/// The preference that overrides the size class OA's windows are laid out
/// at: "auto", "compact", "regular" or "large". Absent, or any other text,
/// is auto: each room takes the largest class whose dialog fits it.
inline constexpr std::string_view window_size_key = "open-annihilation.ui.window-size";

/// The start of each window's remembered place in the preferences. The key
/// is this, the window's screen name, a dot and its size class's word
/// (open-annihilation.window.settings.large); the value is the window's
/// offset from the place it shows unmoved, its centre, in points, as two
/// whole numbers with a comma between ("-120,48"). Absent, the window shows
/// centred.
inline constexpr std::string_view place_key_start = "open-annihilation.window.";

/// The points within which a dragged window snaps to the room's centre and
/// to its safe edges.
inline constexpr int32_t snap_points = 12;

/// The least points of a moved window kept in the room across and down. Its
/// top, which holds the title it is dragged by, also stays in the room.
inline constexpr int32_t least_shown_points = 64;

/// The points Alt and an arrow move the focused window.
inline constexpr int32_t key_step_points = 8;

/// The points Alt, Shift and an arrow move the focused window.
inline constexpr int32_t large_key_step_points = 32;

/// How long a finger holds a window's title before it drags the window, in
/// milliseconds.
inline constexpr uint64_t finger_hold_ms = 400;

/// How far a finger may stray from where it pressed while it holds a title,
/// in points: further before the hold ends, and the press drags nothing.
inline constexpr int32_t finger_hold_points = 8;

/// The Window size choice (window_size_key).
enum class WindowSize : uint8_t {
    automatic, ///< each room takes the largest class whose dialog fits it
    compact,   ///< Compact everywhere
    regular,   ///< Regular wherever its dialog fits, else Compact
    large,     ///< Large wherever its dialog fits, else the largest that does
};

/// Returns the Window size a preference's text names.
///
/// @param text the preference's value
/// @return compact, regular or large for their words; automatic for "auto"
///     and any other text
[[nodiscard]] WindowSize window_size_of(std::string_view text) noexcept;

/// Returns the word the preference keeps a Window size as.
///
/// @param size the Window size
/// @return "auto", "compact", "regular" or "large"
[[nodiscard]] std::string_view window_size_word(WindowSize size) noexcept;

/// Returns the Window size the preferences hold (window_size_key).
///
/// @param preferences the preferences' keys and values
/// @return the size; automatic when the key is absent
[[nodiscard]] WindowSize stored_window_size(const std::map<std::string, std::string>& preferences);

/// Returns the class Auto's scale serves for a Window size
/// (kit::layer_viewport's wanted class): Large for automatic, else the
/// class chosen.
///
/// @param size the Window size
/// @return the class
[[nodiscard]] oa::ui::kit::SizeClass wanted_class(WindowSize size) noexcept;

/// Returns the class a room is laid out at under a Window size: the room's
/// own class for automatic, else the class chosen held to what fits the room
/// (kit::size_class_within).
///
/// @param size the Window size
/// @param room_class the room's own class (kit::size_class_of of its points)
/// @param width_points the room's width, in points
/// @param height_points the room's height, in points
/// @return the class
[[nodiscard]] oa::ui::kit::SizeClass class_under(
    WindowSize size, oa::ui::kit::SizeClass room_class, int32_t width_points, int32_t height_points
) noexcept;

/// Returns the word a size class is named by in a window's place key.
///
/// @param size_class the class
/// @return "compact", "regular" or "large"
[[nodiscard]] std::string_view class_word(oa::ui::kit::SizeClass size_class) noexcept;

/// Returns the preference key of a window's place at a size class.
///
/// @param window the window's screen name, such as "settings"
/// @param size_class the class the window is laid out at
/// @return place_key_start, the name, a dot and the class's word
[[nodiscard]] std::string place_key(std::string_view window, oa::ui::kit::SizeClass size_class);

/// Reads an offset as a place key's value holds it.
///
/// @param text two whole numbers with a comma between, each optionally
///     signed, with no spaces; each within a million
/// @return the offset, in points; nothing for any other text
[[nodiscard]] std::optional<oa::ui::kit::Point> offset_of(std::string_view text) noexcept;

/// Writes an offset as a place key's value holds it.
///
/// @param offset the offset, in points
/// @return "x,y"
[[nodiscard]] std::string offset_text(oa::ui::kit::Point offset);

/// Returns a window's remembered offset at a size class.
///
/// @param preferences the preferences' keys and values
/// @param window the window's screen name
/// @param size_class the class it is laid out at
/// @return the offset, in points; 0, 0 when none is remembered or its text
///     cannot be read
[[nodiscard]] oa::ui::kit::Point stored_offset(
    const std::map<std::string, std::string>& preferences,
    std::string_view window,
    oa::ui::kit::SizeClass size_class
);

/// Remembers a window's offset at a size class: 0, 0, its centre, erases
/// the key.
///
/// @param[in,out] preferences the preferences' keys and values
/// @param window the window's screen name
/// @param size_class the class it is laid out at
/// @param offset the offset, in points
/// @return true when the preferences changed
bool store_offset(
    std::map<std::string, std::string>& preferences,
    std::string_view window,
    oa::ui::kit::SizeClass size_class,
    oa::ui::kit::Point offset
);

/// Forgets every window's remembered place: every key that starts with
/// place_key_start, and no other.
///
/// @param[in,out] preferences the preferences' keys and values
/// @return true when a key was erased
bool forget_places(std::map<std::string, std::string>& preferences);

/// Tells whether a screen of the OA layer is a window that moves by its
/// title: Settings, the key editor, the Library, the map browser and the
/// mutator and brain pickers. Notices and questions, which need an answer,
/// stay centred, and so does every other screen.
///
/// @param screen_name the screen's name (LayerScreen::name)
/// @return true for a window that moves
[[nodiscard]] bool moves(std::string_view screen_name) noexcept;

/// Where a window may show: the room it keeps to, where it shows unmoved,
/// and the scale it is drawn at.
struct WindowRoom {
    oa::ui::kit::Rect room{};    ///< the room, in window pixels: the canvas less its safe insets
    oa::ui::kit::Rect centred{}; ///< where the window shows unmoved, in window pixels
    int32_t scale_percent{100};  ///< window pixels a point takes, in percent; above 0
};

/// Returns a length in points as window pixels at a scale, to the nearest
/// pixel, a half away from zero.
///
/// @param points the length, in points
/// @param scale_percent the scale, in percent; 0 or less counts as 100
/// @return the length, in window pixels
[[nodiscard]] int32_t pixels_of(int32_t points, int32_t scale_percent) noexcept;

/// Returns a length in window pixels as points at a scale, to the nearest
/// point, a half away from zero.
///
/// @param pixels the length, in window pixels
/// @param scale_percent the scale, in percent; 0 or less counts as 100
/// @return the length, in points
[[nodiscard]] int32_t points_of(int32_t pixels, int32_t scale_percent) noexcept;

/// Returns where a window shows at an offset from its centre.
///
/// 0, 0 is where it shows unmoved, exactly. Any other offset moves it by
/// the offset at the scale (pixels_of), then keeps it on screen: at least
/// least_shown_points of it across and down in the room, and its top in the
/// room, unless its unmoved place already lies past those bounds. An edge
/// that ends less than a point from a safe edge of the room lies on it.
///
/// @param room the room, the unmoved place and the scale
/// @param offset the offset, in points
/// @return the window's rectangle, in window pixels, as large as the unmoved place
[[nodiscard]] oa::ui::kit::Rect
shown_at(const WindowRoom& room, oa::ui::kit::Point offset) noexcept;

/// Returns the offset from its centre at which a window shows at a place:
/// the place less the unmoved place, in points (points_of).
///
/// @param room the room, the unmoved place and the scale
/// @param shown where the window shows, in window pixels
/// @return the offset, in points
[[nodiscard]] oa::ui::kit::Point
offset_at(const WindowRoom& room, const oa::ui::kit::Rect& shown) noexcept;

/// Snaps a window's place to the room, across and down each on their own:
/// to its unmoved column or row where it lies within snap_points of it,
/// else centred in the room where its centre lies within snap_points of the
/// room's, else to the room's near or far safe edge where that edge of the
/// window lies within snap_points of it. On the front end the unmoved place
/// is the room's centre; in a match it may stand beside the side column.
///
/// @param room the room, the unmoved place and the scale
/// @param shown where the window would show, in window pixels
/// @return where it snaps to, in window pixels
[[nodiscard]] oa::ui::kit::Rect
snapped(const WindowRoom& room, const oa::ui::kit::Rect& shown) noexcept;

/// Returns the offset a drag gives a window: where it showed when pressed,
/// moved by the pointer's travel, snapped (snapped) and kept on screen
/// (shown_at).
///
/// @param room the room, the unmoved place and the scale
/// @param start_offset the window's offset when the press landed, in points
/// @param travel how far the pointer moved since the press, in window pixels
/// @return the offset, in points
[[nodiscard]] oa::ui::kit::Point dragged_offset(
    const WindowRoom& room, oa::ui::kit::Point start_offset, oa::ui::kit::Point travel
) noexcept;

/// Returns the offset a key's move gives a window: key_step_points, or
/// large_key_step_points, in the arrow's direction from where it shows, kept
/// on screen (shown_at).
///
/// @param room the room, the unmoved place and the scale
/// @param offset the window's offset now, in points
/// @param across -1 for left, 1 for right, 0 for neither
/// @param down -1 for up, 1 for down, 0 for neither
/// @param large Shift is held: the larger step
/// @return the offset, in points
[[nodiscard]] oa::ui::kit::Point moved_by_key(
    const WindowRoom& room, oa::ui::kit::Point offset, int32_t across, int32_t down, bool large
) noexcept;

/// A press on a window's title, which moves the window while it is held.
struct TitleDrag {
    std::string window{}; ///< the window's screen name
    /// The class its place is remembered for.
    oa::ui::kit::SizeClass size_class{oa::ui::kit::SizeClass::compact};
    oa::ui::kit::Point grab{};         ///< where the press landed, in window pixels
    oa::ui::kit::Point start_offset{}; ///< the window's offset when pressed, in points
    oa::ui::kit::Point offset{};       ///< the offset it shows at now, in points
    bool finger{};                     ///< a finger pressed: it drags once it has held
    uint64_t pressed_ms{};             ///< when the press landed, in milliseconds
    bool moving{};                     ///< the pointer moves the window: a mouse, or a held finger
};

/// What a pointer's move did to a drag (drag_move).
enum class DragStep : uint8_t {
    holds,     ///< a finger still holds the title; the window has not moved
    moves,     ///< the window shows at the drag's offset
    cancelled, ///< a finger strayed before it had held: the press drags nothing
};

/// Moves a drag to the pointer: a mouse's moves the window at once; a
/// finger's only once it has held finger_hold_ms within finger_hold_points
/// of where it pressed, and one that strays further sooner cancels it.
///
/// @param[in,out] drag the drag
/// @param room the room, the unmoved place and the scale
/// @param at the pointer, in window pixels
/// @param now_ms the time now, in milliseconds, on the clock pressed_ms reads
/// @return what the move did
DragStep
drag_move(TitleDrag& drag, const WindowRoom& room, oa::ui::kit::Point at, uint64_t now_ms) noexcept;

/// Returns the safe area of a match's canvas.
///
/// @param match_layout the match's layout
/// @return the canvas less its safe-area insets, in window pixels
[[nodiscard]] oa::ui::display_layout::Rect
match_safe_area(const oa::ui::display_layout::MatchLayout& match_layout) noexcept;

/// Returns the match's canvas at the scale OA's windows take on it: the
/// Interface size's step, 150%, or for Auto the Auto scale of its height,
/// one step down where that gives the class the Window size wants
/// (kit::layer_viewport, with 150% among the steps when interface_percent is
/// kit::auto_with_fraction_percent), never above the side column's scale,
/// which HUD scaling caps: the largest whole step at or below it instead,
/// and at least 1.
///
/// @param match_layout the match's layout
/// @param interface_percent the Interface size in effect, in percent; 0 for Auto
/// @param size the Window size in effect
/// @return the canvas, its pixels, density and safe insets, at that scale
[[nodiscard]] oa::ui::kit::Viewport match_viewport(
    const oa::ui::display_layout::MatchLayout& match_layout,
    int32_t interface_percent,
    WindowSize size = WindowSize::automatic
) noexcept;

/// Returns where the settings dialog, and a window placed as it is, shows
/// in a match, and the size class and scale it is laid out and drawn at.
///
/// The scale is match_viewport's. The class comes from the points of the
/// window's whole safe area at that scale (kit::frame_of), or the Window
/// size held to what fits it, as on the front end: the dialog measures the
/// whole window, not the area beside the side column, and may cover the
/// column. Its place is the class's dialog points at the scale
/// (kit::to_canvas), centred in the safe area; where that would cover part
/// of the side column and the safe area right of the column holds it, it is
/// centred there across instead. So a 640 by 480 window is Compact at 1× at
/// (144, 78), as before; 800 by 600 Regular at 1×; 1024 by 768 Large at 1×
/// over the column; 1280 by 720, 1366 by 768 and 1280 by 800 Large at 1×;
/// 1920 by 1080 Large at 1×, or at 150% while it is offered; 2560 by 1440
/// Large at 2×.
///
/// Fitted to a phone's safe area, Compact is drawn at 1× and fitted to the
/// largest scale that shows it whole there (min(safe width / dialog_width,
/// safe height / dialog_height)), centred in it, with a scale of 0.
///
/// @param match_layout the match's layout
/// @param interface_percent the Interface size in effect, in percent: 0 for Auto
/// @param size the Window size in effect
/// @param fit the dialog fits the phone's safe area
/// @return the dialog's rectangle, class and scale
[[nodiscard]] MatchDialogPlace match_window(
    const oa::ui::display_layout::MatchLayout& match_layout,
    int32_t interface_percent,
    WindowSize size,
    bool fit
) noexcept;

} // namespace window_placement

} // namespace oa::app

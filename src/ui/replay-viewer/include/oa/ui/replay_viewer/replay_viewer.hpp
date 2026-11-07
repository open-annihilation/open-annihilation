// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The replay playback bar: where its controls sit, which one a point hits,
// the game time it writes, and when the bar hides. Nothing here reads a
// recording or a match. The app fills a Model and draws it.
#pragma once

#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/layout.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::replay_viewer {

/// Ticks of game time in one second. The bar's clock divides by this.
inline constexpr uint32_t ticks_per_second = 30;
/// How long the pointer may stay off the battlefield's lower edge before the
/// bar hides, in milliseconds.
inline constexpr uint32_t hide_after_ms = 3000;
/// How far above the bar still counts as the battlefield's lower edge, in
/// points.
inline constexpr int32_t edge_band_points = 16;
/// How far from a marker a point still names that marker, in points.
inline constexpr int32_t marker_reach_points = 8;
/// The speeds the menu offers, in thousandths of 1×.
inline constexpr uint32_t speed_milli[] = {500, 1000, 2000, 4000, 8000, 16000};
/// How many speeds the menu offers.
inline constexpr std::size_t speed_count = sizeof(speed_milli) / sizeof(speed_milli[0]);
/// The shortcut card and the end card. Opaque, so their text reads on a light battlefield.
inline constexpr kit::Colour card_fill{0x14, 0x16, 0x12, 255};

/// What a mark on the timeline is.
enum class MarkerKind : uint8_t {
    commander_death, ///< a commander's death, drawn in that player's colour
    dropped,         ///< a player whose connection dropped
    left,            ///< a player who left
    defeated,        ///< a player who was defeated
    alliance,        ///< an alliance made or broken
    chat,            ///< a recorded chat line, drawn as a small tick
    pause,           ///< a pause, drawn as a gap through to `end_tick`
    end,             ///< the end of the recording, drawn as a flag
};

/// One mark the timeline draws and a point can name.
struct Marker {
    MarkerKind kind{MarkerKind::chat};          ///< what it marks
    uint32_t tick{};                            ///< the game tick it happened on
    uint32_t end_tick{};                        ///< a pause's last tick; otherwise `tick`
    int number{-1};                             ///< the player's number; -1 for none
    bool allied{};                              ///< an alliance that was made, rather than broken
    kit::Colour colour{kit::hud_colour::quiet}; ///< the player's colour
    std::string name{};                         ///< the player's name
    std::string line{};                         ///< a chat line
};

/// One player on the end card.
struct PlayerRow {
    std::string name{};                         ///< the name the card shows
    int32_t kills{};                            ///< units this player destroyed
    int32_t losses{};                           ///< units this player lost
    kit::Colour colour{kit::hud_colour::label}; ///< the player's colour
    bool watcher{};                             ///< a player who only watched; the card skips them
};

/// One stretch of the recording a keyframe can jump into. Both ends are included.
struct ReadySpan {
    uint32_t begin{}; ///< the first tick
    uint32_t end{};   ///< the last tick
};

/// One row of the shortcut card: the action's name, and the keys it is bound to.
struct KeyChord {
    std::string action{}; ///< the action's English name, looked up whole
    std::string chord{};  ///< the keys, as the key editor names them
};

/// How much of the speed caption the button has room for.
enum class SpeedForm : uint8_t {
    full,  ///< "16× · 9× now"
    brief, ///< "16× · 9×": the reached speed without its word
    asked, ///< "16×": the asked speed alone
};

/// The widths the bar's text needs in the font on show, in points. The app
/// measures them. Zero is not measured, and the bar then keeps its own sizes.
struct TextWidths {
    int32_t time{};        ///< the widest time this recording shows, such as "45:18 / 45:18"
    int32_t speed_full{};  ///< the widest behind caption, such as "16× · 9× now"
    int32_t speed_brief{}; ///< the same without its word, such as "16× · 9×"
    int32_t speed_asked{}; ///< the widest asked speed alone, such as "16×"
    int32_t skip{};        ///< the caption a catch-up shows, such as "Skip"
};

/// What the bar shows for one frame. The app fills it. The module only reads it.
struct Model {
    uint32_t tick{};              ///< the game tick on show
    uint32_t length{};            ///< the recording's length in ticks; 0 while it is not known
    bool paused{true};            ///< the replay is paused
    bool skipping{};              ///< the replay is skipping to a tick
    int32_t catch_up_percent{-1}; ///< a seek's percent, or -1 while none is running
    uint32_t asked_milli{1000};   ///< the speed that was asked for, in thousandths
    uint32_t reached_milli{1000}; ///< the speed to write when it is behind
    bool reached_lower{};         ///< the picture is behind the asked speed
    bool chat_shown{true};        ///< recorded chat is on show
    bool keys_open{};             ///< the shortcut card is open
    bool speed_open{};            ///< the speed menu is open
    bool end_card{};              ///< the end card is on show
    std::string winner{};         ///< who won; empty is "Undecided" or "No result"
    bool no_result{};             ///< the index ended and nobody won
    bool can_export{true};        ///< Export opens; false greys the button and says why
    bool finding{};               ///< the index pass is still finding markers
    int32_t finding_percent{};    ///< how far that pass has come, 0 to 99
    TextWidths widths{};          ///< what the time and the speed captions need
    std::vector<KeyChord> key_chords{};          ///< the shortcut card's rows, in order
    bool damaged{};                              ///< the recording failed part way
    uint32_t damage_tick{};                      ///< the game tick of that failure
    std::vector<PlayerRow> players{};            ///< the end card's rows
    std::vector<Marker> markers{};               ///< the timeline's marks, in any order
    std::vector<ReadySpan> ready{};              ///< stretches a keyframe can jump into
    std::vector<uint32_t> heat{};                ///< damage in each stretch, for the heat strip
    uint32_t heat_period{ticks_per_second * 10}; ///< ticks one heat sample covers
    bool pointer_over{}; ///< the pointer is over the screen, so a hover may show
    int32_t pointer_x{}; ///< the pointer's column, in the screen's points
    int32_t pointer_y{}; ///< the pointer's row
};

/// A control a point can press. `none` is no control.
enum class Control : int32_t {
    none = -1,
    back,          ///< back ten seconds
    play,          ///< play or pause
    forward,       ///< forward ten seconds
    timeline,      ///< the timeline
    speed,         ///< the speed button
    chat,          ///< show or hide recorded chat
    keys,          ///< the shortcut card
    leave,         ///< the in-game menu
    speed_500,     ///< half speed
    speed_1000,    ///< 1×
    speed_2000,    ///< 2×
    speed_4000,    ///< 4×
    speed_8000,    ///< 8×
    speed_16000,   ///< 16×
    watch_again,   ///< start the replay again
    library,       ///< back to the Library
    export_replay, ///< the export dialog
    keys_card,     ///< the shortcut card's face; a press there is taken and does nothing
    end_card,      ///< the end card's face; a press there is taken and does nothing
};

/// Where the bar and its cards sit, in points.
struct Layout {
    kit::Rect bar{};                      ///< the bar, along the foot
    kit::Rect back{};                     ///< back ten seconds
    kit::Rect play{};                     ///< play or pause
    kit::Rect forward{};                  ///< forward ten seconds
    kit::Rect time{};                     ///< the "11:40 / 23:52" text; not a control
    kit::Rect timeline{};                 ///< the timeline, including its track and heat
    kit::Rect track{};                    ///< the thin track the colours sit on
    kit::Rect heat{};                     ///< the heat strip under the track
    kit::Rect knob{};                     ///< the playhead
    kit::Rect speed{};                    ///< the speed button
    kit::Rect view{};                     ///< empty room for the view control; not a control
    kit::Rect camera{};                   ///< empty room for the camera control; not a control
    kit::Rect chat{};                     ///< recorded chat
    kit::Rect keys{};                     ///< the shortcut card's button
    kit::Rect leave{};                    ///< the in-game menu
    kit::Rect speed_menu{};               ///< the speed menu; empty while it is closed
    std::vector<kit::Rect> speed_items{}; ///< one row per offered speed
    kit::Rect keys_card{};                ///< the shortcut card; empty while it is closed
    kit::Rect end{};                      ///< the end card; empty while it is closed
    kit::Rect watch_again{};              ///< the end card's first button
    kit::Rect library{};                  ///< the end card's second button
    kit::Rect export_button{};            ///< the end card's third button
    std::vector<kit::Rect> marker_hits{}; ///< one hit area per marker, same order
    bool shown{true};                     ///< the bar itself is on show; a card may show without it
    bool export_enabled{true};            ///< a press on Export opens the dialog
    SpeedForm speed_form{SpeedForm::full}; ///< how much of the speed caption the button holds
};

/// How long the bar has been waiting to hide, and whether it is pinned.
class BarVisibility {
  public:

    /// Moves the hide timer on.
    ///
    /// A pointer on the bar or in the band above it shows the bar and clears
    /// the wait. Off that band the wait grows, and the bar hides once it
    /// reaches hide_after_ms. A pinned bar stays shown and the wait stays clear.
    ///
    /// @param dt_ms how long this step lasted, in milliseconds
    /// @param pointer_y the pointer's row, in the same points as the window
    /// @param window_height the window's height, in points
    /// @param bar_top the bar's top row
    void advance(uint32_t dt_ms, int32_t pointer_y, int32_t window_height, int32_t bar_top);

    /// Shows the bar and clears the wait. A playback key and a tap do this.
    void show();

    /// Pins the bar, or takes the pin off. Pinning shows it.
    void toggle_pin();

    /// Tells whether the bar is pinned.
    ///
    /// @return true while it is pinned
    [[nodiscard]] bool pinned() const noexcept { return pinned_; }

    /// Tells whether the bar is on show.
    ///
    /// @return true while it is on show
    [[nodiscard]] bool shown() const noexcept { return shown_; }

  private:

    bool pinned_{};      ///< B is holding the bar on screen
    bool shown_{true};   ///< the bar is on show
    uint32_t idle_ms_{}; ///< how long the pointer has been off the lower edge
};

/// Writes a game tick as minutes and seconds. Minutes have no leading zero.
/// Seconds have two digits.
///
/// @param tick the game tick
/// @return such as "11:40" or "0:01"
[[nodiscard]] std::string clock_text(uint32_t tick);

/// Writes the played time and the length, as the bar shows them.
///
/// @param tick the game tick on show
/// @param length the recording's length in ticks
/// @return such as "11:40 / 23:52"
[[nodiscard]] std::string time_text(uint32_t tick, uint32_t length);

/// Writes one speed as the button shows it. 500 is "½×". Under 2× the tenths
/// are written, and a whole tenth drops the decimal. From 2× up it is the
/// nearest whole multiple of 1×.
///
/// @param milli the speed in thousandths
/// @return the word
[[nodiscard]] std::string speed_word(uint32_t milli);

/// Writes the speed button's caption, looked up in the language on show.
///
/// "Behind" is written only when the reached speed's word differs from the
/// asked speed's.
///
/// @param asked_milli the speed that was asked for
/// @param reached_milli the speed to write when the picture is behind
/// @param reached_lower the picture is behind the asked speed
/// @param skipping the replay is skipping
/// @return the caption
[[nodiscard]] std::string
speed_caption(uint32_t asked_milli, uint32_t reached_milli, bool reached_lower, bool skipping);

/// Writes the speed button's caption in the form its width holds. A speed
/// that is not behind reads the same in every form. Only the full form says
/// Skip during a catch-up; the shorter forms keep the asked speed, and the
/// catch-up's own line says it is catching up. The button's name for a
/// screen reader is always the full caption.
///
/// @param model what the bar shows
/// @param form how much the button has room for
/// @return the caption to draw
[[nodiscard]] std::string fitted_speed_caption(const Model& model, SpeedForm form);

/// Writes a marker's hover line, looked up in the language on show.
///
/// @param marker the marker
/// @return the line
[[nodiscard]] std::string marker_caption(const Marker& marker);

/// Writes the line shown while the index pass is still finding markers.
///
/// @param percent how far that pass has come, 0 to 99
/// @return the line
[[nodiscard]] std::string finding_caption(int32_t percent);

/// The colour to draw for one player.
///
/// A colour the match is showing wins, then one remembered from earlier in
/// the match, then one the recording names, and only then the player's number.
///
/// @param live the match's colour; negative when the match does not name it
/// @param remembered a colour seen earlier; negative when none was
/// @param recorded the recording's colour; negative when it names none
/// @param number the player's number; negative when none is known
/// @return the colour number, or -1 when nothing is known
[[nodiscard]] int player_colour(int live, int remembered, int recorded, int number) noexcept;

/// Writes the catch-up line, looked up in the language on show.
///
/// @param percent the whole percent
/// @return the line
[[nodiscard]] std::string catching_up_caption(int32_t percent);

/// Writes the damaged-recording line, looked up in the language on show.
///
/// @param tick the game tick of the first failure
/// @return the line
[[nodiscard]] std::string damaged_caption(uint32_t tick);

/// Places the bar for a window.
///
/// The timeline is its own row across the bar. Under it, from the left: back,
/// play, forward, the time, speed, an empty view slot, an empty camera slot,
/// chat, keys and leave. The view and camera slots are as wide as the view and
/// camera controls will be, and they are not controls. A hidden bar still has
/// its rectangles, so the lower edge stays put. The speed menu, the shortcut
/// card and the end card are placed only while the model asks for them. The
/// end card sits above the bar. The time keeps the width `widths.time` needs.
/// The speed button grows to the full caption's width from the spare, and
/// where the row has no room for it, it takes the brief form and then the
/// asked speed alone (`Layout::speed_form`). Neither gives up the view or
/// camera slots.
///
/// @param width the window's width, in points
/// @param height the window's height, in points
/// @param model what the bar shows
/// @param shown whether the bar itself is on show
/// @return the layout
[[nodiscard]] Layout lay_out(int32_t width, int32_t height, const Model& model, bool shown);

/// Returns the control a point presses.
///
/// A hidden bar has no bar control. A card's buttons come before the card's
/// face. The view and camera slots are never returned.
///
/// @param layout the layout
/// @param at the point, in the same points
/// @return the control, or Control::none
[[nodiscard]] Control hit_control(const Layout& layout, kit::Point at);

/// Returns the marker a point names, the nearest whose hit area holds the point.
///
/// @param layout the layout
/// @param at the point
/// @return the marker's place in the layout's hit list, or -1
[[nodiscard]] int marker_index(const Layout& layout, kit::Point at);

/// Returns the game tick under a column of the timeline.
///
/// The column is clamped to the timeline. An empty timeline or a recording
/// whose length is not known returns 0.
///
/// @param timeline the timeline's rectangle
/// @param x the column
/// @param length the recording's length in ticks
/// @return the tick
[[nodiscard]] uint32_t tick_at(const kit::Rect& timeline, int32_t x, uint32_t length);

/// The speed a speed-menu control asks for.
///
/// @param control one of the speed controls
/// @return its thousandths, or 0 when the control is not a speed
[[nodiscard]] uint32_t milli_of(Control control) noexcept;

/// Fills a display list with the controls that are on show, for automation.
///
/// A hidden bar contributes no bar control. The view and camera slots are
/// never controls. Names start with "replay-viewer.".
///
/// @param layout the layout
/// @param model what the bar shows
/// @param[out] list the display list; its controls are replaced
void fill_controls(const Layout& layout, const Model& model, kit::DisplayList& list);

/// Draws the bar, and the cards the model has open.
///
/// A hidden bar draws no bar. What is not drawn stays as the canvas found it,
/// so a screen that is not opaque leaves the battlefield showing. While
/// `pointer_over` is set, a point on a marker draws that marker's line, and a
/// point on the timeline draws the time there.
///
/// @param canvas where it is drawn
/// @param layout the layout
/// @param model what the bar shows
void draw(const kit::Canvas& canvas, const Layout& layout, const Model& model);

} // namespace oa::ui::replay_viewer

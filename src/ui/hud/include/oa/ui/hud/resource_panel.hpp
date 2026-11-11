// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The resource panel of ui.resource-panel: a floating panel with one row
// per listed player (stored metal and energy, their bars and incomes), the
// clock, wind and tidal line, and a watcher's switching to another
// player's view and camera.
#pragma once

#include "oa/ui/hud/shared_views.hpp"

#include "oa/core/world.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>

namespace oa::ui::hud {

inline constexpr int32_t kResourcePanelWidth = 210;
inline constexpr int32_t kResourcePanelRowHeight = 30;
/// Pixels of a full storage bar, which shows at most one less.
inline constexpr int32_t kResourcePanelBarWidth = 100;
/// The panel's first place: this far down, against the right edge.
inline constexpr int32_t kResourcePanelFirstY = 50;
/// Player slots the panel can list: 1 to 9. Slot 0 stands for the
/// watcher's own view and is never listed.
inline constexpr uint8_t kResourcePanelFirstSlot = 1;
inline constexpr uint8_t kResourcePanelSlotEnd = 10;
/// Columns from the row's left edge: the player's colour square, the name
/// and bars, and the incomes.
inline constexpr int32_t kResourcePanelSquareX = 0x24;
inline constexpr int32_t kResourcePanelBarX = 0x2c;
inline constexpr int32_t kResourcePanelIncomeX = 0x95;
/// Width of one character of the panel's text, by which numbers are aligned.
inline constexpr int32_t kResourcePanelCharWidth = 8;
/// Characters the stored amounts are right-aligned in.
inline constexpr int32_t kResourcePanelAmountChars = 5;
/// Palette indices of the panel.
inline constexpr uint8_t kResourcePanelNameColor = 0x51;
inline constexpr uint8_t kResourcePanelTextColor = 0xfe;
inline constexpr uint8_t kResourcePanelMetalBarColor = 0xe0;
inline constexpr uint8_t kResourcePanelEnergyBarColor = 0xd0;
inline constexpr uint8_t kResourcePanelTroughColor = 0x00;
inline constexpr uint8_t kResourcePanelViewRowColor = 0x34;
/// The locked-on row alternates between these two from one draw to the next.
inline constexpr uint8_t kResourcePanelLockColor = 0x14;
inline constexpr uint8_t kResourcePanelLockFlash = 0x20;
/// Text of the row that returns a watcher to their own view.
inline constexpr const char* kResourcePanelOwnViewRow = "My Original Camera";

/// How the panel's background is drawn (the player's choice).
enum class ResourcePanelBackground : uint8_t {
    none,  ///< no background; the bars get black edges
    text,  ///< no background
    solid, ///< a black panel
};

/// The panel's state kept between frames.
struct ResourcePanel {
    int32_t x{-1}; ///< left edge in source pixels; -1 before the first draw
    int32_t y{kResourcePanelFirstY};
    bool hidden{};    ///< put away by F4
    bool dragging{};  ///< the left button holds it
    int32_t drag_x{}; ///< pointer place the drag last moved from
    int32_t drag_y{};
    uint8_t flash{kResourcePanelLockColor}; ///< the locked-on row's colour this draw
    ResourcePanelBackground background{};
    /// The slot whose view a watcher shows, 0 for their own.
    uint8_t viewed_slot{};
    /// The slot whose camera a watcher's camera follows, 0 for none.
    uint8_t locked_slot{};
};

/// What one row shows of its player's economy.
struct ResourcePanelFigures {
    float metal{};          ///< stored metal
    float metal_storage{};  ///< metal storage
    float metal_income{};   ///< metal produced per settlement
    float energy{};         ///< stored energy
    float energy_storage{}; ///< energy storage
    float energy_income{};  ///< energy produced per settlement
};

/// The players a panel lists, in the order it lists them.
struct ResourcePanelRows {
    std::array<uint8_t, OA_PLAYER_COUNT> slots{};
    /// Each row's colour square: the palette index the player's colour is
    /// drawn in on the minimap (player_dot_color).
    std::array<uint8_t, OA_PLAYER_COUNT> colours{};
    /// Each row's economy. A row with none shows its name and colour square
    /// alone.
    std::array<std::optional<ResourcePanelFigures>, OA_PLAYER_COUNT> figures{};
    uint8_t count{};
    bool own_view_row{}; ///< a last row returns a watcher to their own view
};

/// One player a replay's panel lists, as the replay's view control shows them.
struct ResourcePanelSeat {
    uint8_t slot{};        ///< the player's slot, 0 to 9
    uint8_t colour{};      ///< palette index of the colour square
    bool economy{};        ///< the recording has given the player's stored resources
    bool income{};         ///< the recording's economy has given an income
    float metal_income{};  ///< metal produced per settlement, while `income` is set
    float energy_income{}; ///< energy produced per settlement, while `income` is set
    bool gone{};           ///< the seat has left the match
};

/// Tells whether the local player watches: a watcher of a multiplayer game.
///
/// @param world players and their setup records
/// @return true when the local player's setup marks it a watcher
[[nodiscard]] bool local_player_watches(const World& world) noexcept;

/// Lists the panel's players.
///
/// Slots 1 to 9 in order: a watcher lists every slot in use that has a
/// name, its own included; anyone else lists the slots that share their map
/// position with this machine. Each row's square is the slot's dot colour
/// (player_dot_color) and its figures the slot's own.
///
/// @param world players
/// @param watching the local player watches (local_player_watches)
/// @param shared what other machines report
/// @return the rows
[[nodiscard]] ResourcePanelRows
resource_panel_rows(const World& world, bool watching, const SharedPlayerViews& shared) noexcept;

/// Lists a replay's players: one row per seat, in the order given, at most
/// OA_PLAYER_COUNT, with no own-view row. A seat whose stored resources the
/// recording has not given, and a seat that has gone, show their name and
/// colour square alone. The stored amounts and storage are the slot's; the
/// incomes are the recording's where it has given one, else the slot's.
///
/// @param world players and their economies
/// @param seats the replay's players, in the order its view control lists them
/// @return the rows
[[nodiscard]] ResourcePanelRows
resource_panel_seat_rows(const World& world, std::span<const ResourcePanelSeat> seats) noexcept;

/// Formats a stored amount: whole below 10000, thousands to one decimal
/// with a K below 100000, whole thousands with a K from there.
///
/// @param[out] out destination
/// @param size bytes of `out`
/// @param amount the amount
void format_panel_amount(char* out, std::size_t size, float amount);

/// Returns the filled pixels of a storage bar: the stored share of the
/// capacity of 100 pixels, truncated, at most 99; none for no capacity.
///
/// @param stored the stored amount
/// @param capacity the storage capacity
/// @return the pixels, 0 to 99
[[nodiscard]] int32_t panel_bar_pixels(float stored, float capacity) noexcept;

/// Formats an income: metal to one decimal, energy whole, both with a plus.
///
/// @param[out] out destination
/// @param size bytes of `out`
/// @param income the income
/// @param metal true for metal
void format_panel_income(char* out, std::size_t size, float income, bool metal);

/// Drawing services of the panel, in source pixels.
struct ResourcePanelSink {
    void* user{};
    void (*fill)(void* user, int32_t x, int32_t y, int32_t width, int32_t height, uint8_t color){};
    void (*text)(void* user, int32_t x, int32_t y, const char* text, uint8_t color){};
};

/// Returns the panel's height for its rows.
///
/// @param rows the rows
/// @return pixels
[[nodiscard]] int32_t resource_panel_height(const ResourcePanelRows& rows) noexcept;

/// Draws the panel: for each row the highlight of the viewed or locked-on
/// player, the player's colour square, name, and, while the row has its
/// figures, the stored metal and energy, their bars and incomes; a watcher's
/// last row returns to their own view.
///
/// @param world players and their economies
/// @param[in,out] panel place and highlights; the lock colour alternates
/// @param rows the rows
/// @param sink drawing services
void draw_resource_panel(
    const World& world,
    ResourcePanel& panel,
    const ResourcePanelRows& rows,
    const ResourcePanelSink& sink
);

/// What a press on the panel lands on.
struct ResourcePanelHit {
    bool inside{};   ///< on the panel
    int8_t row{-1};  ///< the row's index in ResourcePanelRows, -1 for none
    bool own_view{}; ///< the row that returns to the watcher's own view
};

/// Finds what a pointer place lands on.
///
/// @param panel place
/// @param rows the rows
/// @param x pointer column in source pixels
/// @param y pointer row
/// @return the hit
[[nodiscard]] ResourcePanelHit resource_panel_hit(
    const ResourcePanel& panel, const ResourcePanelRows& rows, int32_t x, int32_t y
) noexcept;

/// Handles F4 with the game's stats board: with the panel out and the
/// board away F4 puts the panel away; with the panel away and the board
/// out it brings the panel back; otherwise the key goes to the board.
///
/// @param[in,out] panel the panel
/// @param board_out the kill board is pinned out
/// @param has_rows the panel lists anyone
/// @return true when the panel took the key
bool resource_panel_f4(ResourcePanel& panel, bool board_out, bool has_rows) noexcept;

/// What a watcher's double-click on a row asks for.
enum class ViewSwitch : uint8_t {
    none,          ///< nothing: not a watcher, or not a row
    view_player,   ///< show the slot's view
    lock_camera,   ///< the slot is shown already: follow its camera
    unlock_camera, ///< the slot's camera is followed already: stop
    own_view,      ///< back to the watcher's own view
};

/// Decides a watcher's double-click on the panel and records it.
///
/// @param[in,out] panel viewed and locked slots
/// @param rows the rows
/// @param hit what the double-click landed on
/// @param watching the local player watches
/// @return the switch, whose view the caller then shows
ViewSwitch resource_panel_view_switch(
    ResourcePanel& panel, const ResourcePanelRows& rows, const ResourcePanelHit& hit, bool watching
) noexcept;

/// The wind a wind generator shows on the clock line, in energy.
struct WindReadout {
    int32_t current{};
    int32_t minimum{};
    int32_t maximum{};
};

/// The energy a wind generator of the clock line makes when the unit type
/// table names none: 30, and 20 for a solar collector.
inline constexpr int32_t kDefaultWindGenerator = 30;

/// Returns the clock line's wind: each wind strength (now, the map's least
/// and most) times the generator's output over the strength divisor,
/// rounded to nearest and at most the output; all 0 without a divisor.
///
/// @param strength the strength now
/// @param minimum the map's least strength
/// @param maximum the map's most strength
/// @param divisor the strength divisor (Game.wind_strength_divisor)
/// @param generator a wind generator's output (kDefaultWindGenerator without one)
/// @return the wind
[[nodiscard]] WindReadout wind_readout(
    int32_t strength, int32_t minimum, int32_t maximum, int32_t divisor, int32_t generator
) noexcept;

/// Formats "Game Time : hh:mm:ss" for a tick count, 30 ticks a second.
void format_game_time(char* out, std::size_t size, uint32_t tick);

/// Formats "Wind : +<now> (<least>-<most>)", or for a watcher "Wind : (<least>-<most>)".
void format_wind(char* out, std::size_t size, const WindReadout& wind, bool watching);

/// Formats "Tidal : +<strength>", the strength truncated.
void format_tidal(char* out, std::size_t size, float tidal_strength);

/// Where the top bar's art repeats right of PANELTOP: the column the first
/// PANELBOT piece starts at and each piece's width, 0 for none.
struct TopBarPieces {
    int32_t left{};
    int32_t width{};
};

/// Sections of equal width a PANELBOT piece is divided into by bevels.
inline constexpr int32_t kTopBarPieceSections = 3;
/// Columns from a section's left edge to the clock line's text in it.
inline constexpr int32_t kClockLineInset = 16;
/// Rows of the wind's and the tidal strength's lines in the top bar, level
/// with its produced and consumed rates.
inline constexpr int32_t kClockLineWindY = 5;
inline constexpr int32_t kClockLineTidalY = 17;
/// Row of the game time in the top bar, midway between them.
inline constexpr int32_t kClockLineTimeY = 11;
/// Columns between the wind's and the tidal strength's figures and the game
/// time beside them, where the bar holds one section.
inline constexpr int32_t kClockLineBesideGap = 12;
/// The widest window, in pixels, whose clock line with the game time stays
/// on the battlefield, and whose chrome keeps its scale for the line; on a
/// wider one the line goes in the top bar.
inline constexpr int32_t kClockLineBattlefieldMaxWidth = 1024;

/// The clock line's place on the battlefield: three lines from the
/// battlefield's top left, two columns right of the 128-column side panel
/// and two rows under the top bar.
inline constexpr int32_t kClockLineLeft = 130;
inline constexpr int32_t kClockLineTop = 34;
/// Rows between the battlefield's three lines.
inline constexpr int32_t kClockLineStep = 10;

/// Where the clock line goes.
enum class ClockLineSpot : uint8_t {
    /// Three lines on the battlefield, at kClockLineLeft and kClockLineTop.
    battlefield,
    /// The top bar's first two sections past PANELTOP: the wind over the
    /// tidal strength in the first, the game time in the second.
    sections,
    /// The top bar past PANELTOP, where it holds one section: the wind
    /// over the tidal strength, and beside them "Game Time" over the time.
    beside,
    /// The top bar's first section past PANELTOP, while the line leaves
    /// out the game time: the wind over the tidal strength, on a window of
    /// any width.
    first_section,
};

/// Widths, in pixels, of the clock line's parts.
struct ClockLineWidths {
    int32_t label{};   ///< the longer of the wind's and the tidal strength's labels, " : " included
    int32_t figures{}; ///< the wider of the two lines' figures
    int32_t time{};    ///< the game time's whole line
    int32_t time_label{}; ///< "Game Time", without " : "
    int32_t time_value{}; ///< the time alone
};

/// Where the clock line goes and its columns there.
struct ClockLinePlace {
    ClockLineSpot spot{};
    /// Column the wind's and the tidal strength's figures start at, their
    /// labels ending there.
    int32_t figures_x{};
    /// Column the game time starts at: its whole line in the second
    /// section, or "Game Time" over the time beside the figures.
    int32_t time_x{};
};

/// Places the clock line: with the game time, on a window no wider than
/// kClockLineBattlefieldMaxWidth, on the battlefield; on a wider one, and
/// without the game time on a window of any width, in the top bar past
/// PANELTOP, whose PANELBOT pieces are each divided into
/// kTopBarPieceSections sections.
///
/// The first section holds the wind over the tidal strength: the longer
/// label starts kClockLineInset columns into it and both figures start
/// right after it, so that the labels end together. Where the second
/// section and the bar hold the game time's line kClockLineInset columns
/// into it, it goes there; otherwise "Game Time" goes over the time,
/// kClockLineBesideGap columns past the figures, while that fits in the
/// bar. A line without the game time takes the first section alone
/// (ClockLineSpot::first_section) where the bar reaches past the figures.
/// A bar with no room past PANELTOP for the line leaves it on the
/// battlefield.
///
/// @param pieces the top bar's repeated pieces
/// @param bar_end the column just past the top bar's last
/// @param widths the widths of the line's parts; the game time's are not
///        read without it
/// @param window_width the window's width in pixels
/// @param game_time whether the line shows the game time
/// @return where the line goes
[[nodiscard]] ClockLinePlace place_clock_line(
    const TopBarPieces& pieces,
    int32_t bar_end,
    const ClockLineWidths& widths,
    int32_t window_width,
    bool game_time
) noexcept;

/// Returns how far the top bar must reach for place_clock_line to put the
/// clock line at a spot: two sections or the game time beside the figures
/// on a window wider than kClockLineBattlefieldMaxWidth, the first section
/// alone on a window of any width.
///
/// @param pieces the top bar's repeated pieces
/// @param widths the widths of the line's parts
/// @param spot ClockLineSpot::sections, ClockLineSpot::beside or
///        ClockLineSpot::first_section
/// @return the least column just past the bar's last that puts the line
///         there; 0 where no bar does, as for the battlefield, for two
///         sections whose first does not hold the wind's and the tidal
///         strength's lines or whose second does not hold the game time's,
///         and without pieces
[[nodiscard]] int32_t clock_line_bar_end(
    const TopBarPieces& pieces, const ClockLineWidths& widths, ClockLineSpot spot
) noexcept;

/// The least scale the top and bottom bars are drawn at to make room for
/// the clock line's two sections: one and a half times the interface's
/// size, as on a 1280x720 window.
inline constexpr double kClockLineSectionsLeastScale = 1.5;

/// Returns the scale the interface's chrome is drawn at on a window wider
/// than kClockLineBattlefieldMaxWidth so that the top bar has room for the
/// clock line.
///
/// The chrome keeps its scale where its bar already reaches far enough for
/// two sections. Otherwise it is drawn at the largest scale at which a bar
/// running from the side column's right edge to the window's right edge
/// reaches kClockLineInset columns past the game time in the second
/// section (the window's width over the column it must reach), while that
/// is at least kClockLineSectionsLeastScale. Where it is not, the chrome
/// keeps its scale where its bar reaches far enough for "Game Time" beside
/// the figures, and is otherwise drawn at the largest scale at which the
/// bar reaches kClockLineInset columns past it. A line without the game
/// time needs only the first section: the chrome keeps its scale where the
/// bar reaches past the figures, and is otherwise drawn at the largest
/// scale at which the bar reaches kClockLineInset columns past them. A
/// window kClockLineBattlefieldMaxWidth pixels wide or narrower, and a bar
/// with no room for the line at any scale, keep the scale.
///
/// @param pieces the top bar's repeated pieces
/// @param widths the widths of the line's parts
/// @param game_time whether the line shows the game time
/// @param window_width the window's width in pixels
/// @param bar_end the column just past the top bar's last at the chrome's
///        own scale
/// @param scale the chrome's own scale: canvas pixels per source pixel
/// @return the scale, no larger than `scale`
[[nodiscard]] double clock_line_chrome_scale(
    const TopBarPieces& pieces,
    const ClockLineWidths& widths,
    bool game_time,
    int32_t window_width,
    int32_t bar_end,
    double scale
) noexcept;

/// Rows of the clock line's lines on the battlefield.
struct ClockLineRows {
    std::optional<int32_t> time{}; ///< the game time's; none without it
    int32_t wind{};
    int32_t tidal{};
};

/// Returns the rows of the clock line's lines on the battlefield, from
/// kClockLineTop and kClockLineStep apart: the game time, the wind and the
/// tidal strength; without the game time, the wind and the tidal strength
/// in the first two lines' places.
///
/// @param game_time whether the line shows the game time
/// @return the rows
[[nodiscard]] ClockLineRows clock_line_battlefield_rows(bool game_time) noexcept;

/// A clock line cut into its parts, any of them empty.
struct ClockLineParts {
    std::string_view label{};  ///< up to and including the " : " that ends it
    std::string_view amount{}; ///< a signed amount right after it, "+26"
    std::string_view rest{};   ///< what follows: the wind's range, the time
};

/// Cuts a line format_game_time, format_wind or format_tidal wrote into its
/// label, its signed amount and the rest.
///
/// @param line the line; a line without " : " is all label
/// @return the parts, views into `line`
[[nodiscard]] ClockLineParts split_clock_line(std::string_view line) noexcept;

} // namespace oa::ui::hud

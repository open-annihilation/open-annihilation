// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A mod profile's display rules (ModProfile::ui) as the records and small
// decisions of the modules that carry them out: the match's voices and
// explosions, how strongly the explosions' flashes are drawn, the music
// folder, the display modes, the build cursor's snap and the victory
// announcement. A default-constructed UiRules gives 3.1c's
// behaviour everywhere.
#pragma once

#include "oa/data/mod_profile.hpp"
#include "oa/sim/match_runtime.hpp"
#include "oa/ui/engine_settings.hpp"
#include "oa/ui/frontend_state/initialization.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app::view_rules {

/// Returns how a match's units speak and its explosions show under a
/// profile's display rules.
///
/// @param ui the profile's display rules
/// @return the match's display rules; 3.1c's for a profile without them
[[nodiscard]] sim::match_runtime::DisplayRules
match_display_rules(const data::mod_profile::UiRules& ui) noexcept;

/// Returns how strongly explosions' flashes are drawn: the lower of the
/// player's Explosion flash setting and the level a profile's display rules
/// design them at (ui.explosion-flash), so that a player can always reduce
/// the flash or turn it off whatever the mod asks, and a mod can reduce it
/// but never draw it above the player's choice.
///
/// @param ui the profile's display rules; 3.1c's full flash without the hack
/// @param player the player's Explosion flash setting
/// @return the level drawn
[[nodiscard]] oa::ui::engine_settings::ExplosionFlash explosion_flash_drawn(
    const data::mod_profile::UiRules& ui, oa::ui::engine_settings::ExplosionFlash player
) noexcept;

/// Ticks after the victory banner was last drawn before the victory
/// announcement may play again.
inline constexpr uint32_t victory_announcement_interval = 300;

/// Decides whether the victory announcement plays as the victory banner is
/// drawn at a match tick, and records that tick.
///
/// @quirk The tick is recorded at every drawing, not only when the
///        announcement plays, and starts at 0, so a banner drawn every
///        frame announces once, a match won before tick 301 is not
///        announced, and a banner drawn at an earlier tick than the last
///        (a later match) always is.
///
/// @param tick the match tick the banner is drawn at
/// @param[in,out] last_tick the tick the banner was last drawn at
/// @return true when the announcement plays
[[nodiscard]] bool victory_announcement_due(uint32_t tick, uint32_t& last_tick) noexcept;

/// Where a profile's music comes from (ui.audio music).
enum class MusicSource : uint8_t {
    disc,         ///< numbered music files as the game disc (3.1c's CD)
    numbered_mp3, ///< 1.mp3, 2.mp3 and on, from track 1, with no gap
    folder_scan,  ///< every MP3 file of the folder, in name order
};

/// Returns where a profile's music comes from.
///
/// @param ui the profile's display rules
/// @return the music source; the disc for a profile without ui.audio
[[nodiscard]] MusicSource music_source(const data::mod_profile::UiRules& ui) noexcept;

/// Returns the shortest display mode the options screen offers, in rows.
///
/// @param ui the profile's display rules
/// @return 768 under ui.display-modes min-height-768, else 3.1c's 480
[[nodiscard]] int32_t minimum_mode_height(const data::mod_profile::UiRules& ui) noexcept;

/// Returns the default and floor of the DisplaymodeWidth and
/// DisplaymodeHeight settings.
///
/// @param ui the profile's display rules
/// @return 1024 by 768, raising smaller stored values, under ui.display-modes
///     min-height-768; else 3.1c's 640 by 480
[[nodiscard]] oa::ui::frontend_state::initialization::DisplayModeSetting
display_mode_setting(const data::mod_profile::UiRules& ui) noexcept;

/// The characters a screenshot's file name has replaced by '_'.
inline constexpr std::string_view screenshot_forbidden_characters = "\\/:*?\"<>|";

/// Returns text with every character a file name may not hold replaced by '_'.
///
/// @param text the text
/// @return the text, safe in a file name
[[nodiscard]] std::string screenshot_safe_text(std::string_view text);

/// What a screenshot taken during a match is named after.
struct ScreenshotScene {
    std::string map{};                     ///< the map's name
    std::array<std::string, 10> players{}; ///< each slot's player name; empty for none
};

/// Returns the file name of a screenshot (ui.display-modes).
///
/// The date text comes first, then, during a match, the map's name, " - ",
/// the first slot's name (even an empty one) and ", " with each later
/// non-empty one, and a space; outside a match "SHOT". The index follows
/// with at least four digits, then ".pcx". The date, the map and every
/// player name are made safe (screenshot_safe_text).
///
/// @param date the date as the player's clock shows it, followed by " - "
/// @param scene the match the screenshot is taken in; null outside a match
/// @param index the screenshot's number, the first unused from 0
/// @return the file name, without a directory
[[nodiscard]] std::string
screenshot_file_name(std::string_view date, const ScreenshotScene* scene, int32_t index);

/// The most cells a click snap may move a click (ui.click-snap).
inline constexpr int32_t click_snap_radius_limit = 9;

/// Returns the radius a click snap searches with: the player's setting held
/// to 0 and to the profile's maximum (and click_snap_radius_limit).
///
/// @param setting the player's radius, in cells
/// @param maximum the profile's maximum, in cells
/// @return the radius, 0 for no snap
[[nodiscard]] int32_t click_snap_radius(int32_t setting, int32_t maximum) noexcept;

/// Scores a cell a click snap may move to: how many of the footprint's cells
/// it would put on what the snap looks for; 0 or less leaves it out.
using SnapScore = std::function<int32_t(int32_t cell_x, int32_t cell_z)>;

/// Finds the cell a click snaps to.
///
/// Every cell within `radius` of `cell` along each axis is scored; of those
/// that score above 0, the highest scores win, and of them the one whose
/// middle lies nearest the cursor. Ties keep the first, the cells taken
/// column by column from the most negative offset.
///
/// @param cell the cell under the cursor (the footprint's middle)
/// @param radius cells searched each way
/// @param cursor_x the cursor's map x, in pixels
/// @param cursor_z the cursor's map z, in pixels
/// @param score the cell's score
/// @return the cell, and its score; nothing when no cell scores
[[nodiscard]] std::optional<std::array<int32_t, 3>> snap_cell(
    std::array<int32_t, 2> cell,
    int32_t radius,
    double cursor_x,
    double cursor_z,
    const SnapScore& score
);

/// Returns the first offset of a footprint's cells from its middle cell:
/// minus half the size, truncated.
///
/// @param size the footprint's cells along the axis
/// @return the offset
[[nodiscard]] int32_t footprint_first_offset(int32_t size) noexcept;

/// Returns the last offset of a footprint's cells from its middle cell.
///
/// @param size the footprint's cells along the axis
/// @return the offset; below the first for an empty footprint
[[nodiscard]] int32_t footprint_last_offset(int32_t size) noexcept;

/// Tells whether a building's yard map holds a geothermal cell, which makes
/// a click snap move it onto the nearest site it may be built on.
///
/// @quirk The yard map is read as text of at most 64 cells, so an open cell
///        ('.', which compiles to 0) ends it: a geothermal cell after one is
///        not seen.
///
/// @param yard the compiled yard map, row by row
/// @return true when a geothermal cell is seen
[[nodiscard]] bool yard_has_geothermal_cell(std::span<const uint8_t> yard) noexcept;

/// The most extra cells the line and ring build tools leave between
/// buildings (ui.build-tools).
inline constexpr int32_t build_tool_spacing_limit = 10;
/// The most buildings a line lays past its first.
inline constexpr int32_t build_line_step_limit = 999;

/// One building a line or ring lays: the map pixel a build click would be
/// made at, as the game keeps it in 16 bits.
struct BuildSlot {
    int16_t x{};
    int16_t z{};
    /// Compares both coordinates.
    bool operator==(const BuildSlot&) const = default;
};

/// The axis a line advances whole buildings along; the other moves a cell
/// at a time.
enum class LineAxis : uint8_t {
    across = 1, ///< along x
    down = 2,   ///< along z
};

/// The buildings of a line build.
struct BuildLine {
    std::vector<BuildSlot> slots{};
    LineAxis axis{LineAxis::down};
};

/// Lays a line of buildings from a start pixel towards an end pixel
/// (ui.build-tools).
///
/// The cells between the two, each difference divided by 16 toward zero,
/// decide the line: along the axis with more of them (z on a tie) it steps
/// a footprint plus the spacing each building, as many whole steps as fit;
/// along the other it moves one cell at a time, spread over the steps as a
/// straight line spreads them.
///
/// @param start_x the start's map x, in pixels
/// @param start_z the start's map z, in pixels
/// @param end_x the end's map x, in pixels
/// @param end_z the end's map z, in pixels
/// @param footprint_x the building's footprint along x, at least 1
/// @param footprint_z the building's footprint along z, at least 1
/// @param spacing extra cells between buildings
/// @return the line; nothing when it would hold more than
///     build_line_step_limit buildings after the first
[[nodiscard]] std::optional<BuildLine> line_build_slots(
    int32_t start_x,
    int32_t start_z,
    int32_t end_x,
    int32_t end_z,
    int32_t footprint_x,
    int32_t footprint_z,
    int32_t spacing
);

/// Reorders a line of 2 by 2 buildings (dragon's teeth) to build as a
/// staggered double row: walking from the second building, where the next
/// but one, or else the next, stands in the same row across the line, the
/// two swap places in the order.
///
/// @param[in,out] line the line; only the order of its buildings changes
void optimize_dt_rows(BuildLine& line) noexcept;

/// Lays a ring of buildings around a rectangle of cells (ui.build-tools).
///
/// The top row runs left to right outside the rectangle, the right column
/// top to bottom, the bottom row right to left and the left column bottom
/// to top. Each side holds the rectangle's side over the footprint's, plus
/// one; with full rings, a side the footprint does not divide, for a
/// footprint under 3 by 3, gets one more, closing the corner.
///
/// @param x the rectangle's left edge, in map pixels
/// @param z its top edge, in map pixels
/// @param width its width, in cells
/// @param height its height, in cells
/// @param footprint_x the building's footprint along x, at least 1
/// @param footprint_z the building's footprint along z, at least 1
/// @param full_rings rings include their corner places
/// @return the buildings, in the order they are given
[[nodiscard]] std::vector<BuildSlot> ring_build_slots(
    int32_t x,
    int32_t z,
    int32_t width,
    int32_t height,
    int32_t footprint_x,
    int32_t footprint_z,
    bool full_rings
);

/// The facings a building may be placed in, in the order the rotate key
/// steps through them.
enum class BuildFacing : uint8_t {
    south = 0, ///< as 3.1c builds every building
    east = 1,
    north = 2,
    west = 3,
};

/// Tells whether a building type may be placed in a facing (ui.build-preview).
///
/// @param facings the type's facings, as match_rules::build_facing bits
/// @param facing the facing
/// @return true for south, and for a facing the bits name
[[nodiscard]] bool facing_allowed(uint8_t facings, BuildFacing facing) noexcept;

/// Returns the facing the rotate key or the wheel turns a building to: the
/// next facing the type allows, stepping south, east, north, west forward
/// or back.
///
/// @param facings the type's facings, as match_rules::build_facing bits
/// @param current the facing chosen now
/// @param direction above 0 steps forward, else back
/// @return the next allowed facing; the current one when the type allows
///     fewer than two
[[nodiscard]] BuildFacing
next_build_facing(uint8_t facings, BuildFacing current, int32_t direction) noexcept;

/// Returns the facing that turns a building toward a point: east or west
/// when the point lies farther off across than down, else south or north
/// (ui.build-preview).
///
/// @param dx whole x of the point less that of the building
/// @param dz whole z of the point less that of the building
/// @return east for dx above 0 and west otherwise when |dx| > |dz|; else
///     south for dz above 0 and north otherwise
[[nodiscard]] BuildFacing facing_toward(int32_t dx, int32_t dz) noexcept;

/// Returns the facing the build preview of a type that faces its opponent
/// (PreviewFaceOpponent) is drawn in (ui.build-preview), as the finished
/// defence would turn.
///
/// The site must lie within build distance of one of the local player's
/// selected, finished mobile units (its player's units from the first up to
/// the last, the last not counted, with a build distance). The nearest enemy
/// is then the first unit of each other player in use who is no watcher and
/// whom the local player does not ally, when it has a movement object and a
/// type; distances are squared whole world units, the first of equals
/// kept. The facing turns toward it (facing_toward).
///
/// @param world the match's world
/// @param site_x whole x of the site's centre
/// @param site_z whole z of the site's centre
/// @return the facing, or nothing without such a unit or enemy
[[nodiscard]] std::optional<BuildFacing>
opponent_facing(const oa::World& world, int32_t site_x, int32_t site_z) noexcept;

/// Returns the letter that names a facing: S, E, N or W.
///
/// @param facing the facing
/// @return the letter
[[nodiscard]] char facing_letter(BuildFacing facing) noexcept;

/// Returns the hint shown while the rotate key is not yet known: "Press
/// <key>, or <modifier>+wheel, to rotate".
///
/// @param key the rotate key's name
/// @param modifier the snap override key's name, held to turn with the wheel
/// @return the hint
[[nodiscard]] std::string rotate_hint(std::string_view key, std::string_view modifier);

/// Tells whether a build preview's piece list names a piece: the list's
/// names are separated by commas, with spaces around them, and matched
/// without case.
///
/// @param list the type's preview piece list
/// @param piece the piece's name
/// @return true when the list names it
[[nodiscard]] bool preview_lists_piece(std::string_view list, std::string_view piece) noexcept;

/// Tells whether a build preview leaves a piece out when its type names no
/// pieces to draw: a muzzle flash or a wake, a piece named flare, flash,
/// muzzle, fire, flame or wake, matched without case, which the unit's
/// script would hide (ui.build-preview).
///
/// @param piece the piece's name
/// @return true when the preview leaves it out
[[nodiscard]] bool preview_skips_piece(std::string_view piece) noexcept;

/// Ticks of the match each pulse of a build preview takes: a second at
/// the game's normal speed (ui.build-preview).
inline constexpr uint32_t build_preview_pulse_ticks = 30;
/// Ticks at the start of each pulse its scanline takes to climb the model.
inline constexpr uint32_t build_preview_scan_ticks = 15;
/// The palette entry of a build preview's scanline: bright green.
inline constexpr uint8_t build_preview_scan_colour = 0xfa;

/// How a build preview draws its building at a tick of its pulse.
struct BuildPreviewLook {
    uint8_t outline{}; ///< palette entry of the model's outline
    /// Palette entry the model is filled with; none without the fill.
    std::optional<uint8_t> fill{};
    /// The heights the scanline lights, in pixels above the model's lowest
    /// point, from scan_low through scan_high; none between climbs.
    bool scanning{};
    int32_t scan_low{};
    int32_t scan_high{};
};

/// Returns how a build preview draws its building at a tick of its pulse
/// (ui.build-preview).
///
/// The pulse starts again whenever the player turns the building and
/// repeats every build_preview_pulse_ticks. Its colours walk palette
/// 0xa0..0xaf, bright green to black, and back, once each pulse, in 32
/// steps every 30 ticks: the outline starts 24 steps into the walk, on its
/// way back from black, and the fill, with the profile's fill, half a walk
/// away from the outline. For the first build_preview_scan_ticks of each
/// pulse the scanline climbs the model from its lowest point to its
/// highest: on tick k of the pulse it lights the heights whose place up
/// the model, counted from 1 at the lowest point to 255 at the highest and
/// rounded, lies within 2 of 1 + 254 k / 14, rounded down.
///
/// @param pulse_tick ticks since the pulse started
/// @param fill the preview fills the model (ui.build-preview fill)
/// @param height the model's height, its highest point less its lowest,
///     in pixels
/// @return the look
[[nodiscard]] BuildPreviewLook
build_preview_look(uint32_t pulse_tick, bool fill, int32_t height) noexcept;

/// A rectangle in the game's 640 by 480 source pixels.
struct SourceBox {
    int32_t x{};
    int32_t y{};
    int32_t width{};
    int32_t height{};
    /// Compares every field.
    bool operator==(const SourceBox&) const = default;
};

/// The palette colour of the message log's backdrop (the accessible chat,
/// and the Game text background setting): black.
inline constexpr uint8_t chat_backdrop_color = 0;

/// Returns the backdrop the accessible chat lays under one line of the
/// message log (ui.text-rendering chat-backdrop): from 4 pixels left of
/// the log to 4 past the line's text, a logo counted as a line height
/// wide, from the row above the line to the row below it.
///
/// @param has_logo the line starts with its sender's logo
/// @param y the line's top row
/// @param text_width the text's width in pixels
/// @param line_height the line's height in pixels
/// @return the backdrop
[[nodiscard]] SourceBox
chat_backdrop_rect(bool has_logo, int32_t y, int32_t text_width, int32_t line_height) noexcept;

/// Splits the chat macro into the lines F11 sends, at carriage returns and
/// line feeds, leaving out empty lines.
///
/// @param macro the macro's text
/// @return the lines, in order
[[nodiscard]] std::vector<std::string> chat_macro_lines(std::string_view macro);

/// Returns the battle room buttons a profile adds, as the multiplayer
/// screens' lobby_button bits (ui.share-dialog-and-lobby-buttons).
///
/// @param ui the profile's display rules
/// @return the bits; 0 without the hack
[[nodiscard]] uint8_t lobby_button_bits(const data::mod_profile::UiRules& ui) noexcept;

/// Returns where a share-threshold slider's knob stands for a threshold
/// (ui.share-dialog-and-lobby-buttons): the threshold over the storage,
/// whole units, times the knob positions, in single precision, truncated.
///
/// @param threshold the player's share threshold
/// @param storage the player's storage
/// @param range the slider's knob positions
/// @return the knob position; 0 without storage
[[nodiscard]] int16_t share_threshold_knob(float threshold, float storage, int16_t range) noexcept;

/// Returns the threshold a share-threshold slider's knob stands for: the
/// knob over the last position, held to 0 to 1, times the storage in whole
/// units, in single precision, truncated.
///
/// @param knob the knob position
/// @param range the slider's knob positions
/// @param storage the player's storage
/// @return the threshold
[[nodiscard]] int32_t share_threshold_value(int16_t knob, int16_t range, float storage) noexcept;

/// What patrolling builders do under one standing move order, as the
/// options dialog offers it.
enum class PatrolOption : uint8_t {
    reclaim_only, ///< reclaim, never assist
    both,         ///< reclaim and assist
    assist_only,  ///< assist, never reclaim
};

/// What guarding builders do under one standing move order, as the options
/// dialog offers it.
enum class GuardOption : uint8_t {
    stay,    ///< keep their place
    base,    ///< as 3.1c
    scatter, ///< spread out
};

/// What the resource panel draws behind its text, as the options dialog
/// offers it.
enum class PanelBackground : uint8_t {
    none,
    text,
    solid,
};

/// The preferences section the player's display-rule settings are kept in.
inline constexpr std::string_view view_settings_section = "Eye";

/// The settings the profile's display rules let the player change, in the
/// options dialog (ui.options-dialog). They are this machine's own. The
/// three key sliders read and write the bound actions, not this record.
struct ViewSettings {
    bool rotate_key_discovered{}; ///< the rotate key's hint is no longer shown
    bool build_menu_rotation{};   ///< the build menu shows the facing chosen
    /// Patrolling builders under Hold position, Maneuver and Roam.
    std::array<PatrolOption, 3> patrol{
        PatrolOption::reclaim_only, PatrolOption::both, PatrolOption::both
    };
    /// Guarding builders under Hold position, Maneuver and Roam.
    std::array<GuardOption, 3> guard{GuardOption::base, GuardOption::base, GuardOption::base};
    int32_t mex_snap_radius{};   ///< cells; held to the profile's maximum
    int32_t wreck_snap_radius{}; ///< cells; held to the profile's maximum
    /// The chat macro F11 sends, one line each, separated by carriage returns.
    std::string chat_macro{};
    PanelBackground panel_background{PanelBackground::text};
    bool optimize_dt_rows{true}; ///< 2x2 lines lay out as a staggered double row
    bool full_rings{true};       ///< rings include their corner places
    bool chat_backdrop{};        ///< chat lines get a dark backdrop
    bool vsync{};                ///< frames wait for the display
};

/// The chat macro's text before the player changes it.
inline constexpr std::string_view default_chat_macro =
    "+setshareenergy 1000\r+setsharemetal 1000\r+shareall\r+shootall";

/// Reads a stored setting by name: a number, or text.
struct ViewSettingsStore {
    std::function<std::optional<uint32_t>(std::string_view name)> number{};
    std::function<std::optional<std::string>(std::string_view name)> text{};
};

/// Returns the settings a player has: what the store keeps, else the
/// profile's values and the dialog's defaults.
///
/// @param ui the profile's display rules, which give the snap radii, their
///     maxima and the chat backdrop
/// @param builders the profile's options for patrolling and guarding
///     builders
/// @param store the stored settings; either reader may be empty
/// @return the settings, the snap radii held to the profile's maxima
[[nodiscard]] ViewSettings read_view_settings(
    const data::mod_profile::UiRules& ui,
    const data::match_rules::OrdersConPatrolGuardOptions& builders,
    const ViewSettingsStore& store
);

/// Puts the player's options for patrolling and guarding builders into the
/// rules a match is built with: they apply to this player's own units.
///
/// @param settings the player's settings
/// @param[in,out] builders the match's options; left as they are while the
///     profile does not turn the options on
void apply_builder_options(
    const ViewSettings& settings, data::match_rules::OrdersConPatrolGuardOptions& builders
) noexcept;

/// Writes the settings, one value each.
///
/// A snap radius is written only while the profile lets the player choose
/// it, as the options dialog does (dialog_option_locks): one the profile
/// gives no room is not the player's choice, so the value stored before
/// stays, and with none the profile's default plays once it has room.
/// The stored key names are left as they are, for the one-time import.
///
/// @param settings the settings
/// @param ui the profile's display rules, which say which snap radii the
///     player may choose
/// @param number stores a number under a name
/// @param text stores text under a name
void write_view_settings(
    const ViewSettings& settings,
    const data::mod_profile::UiRules& ui,
    const std::function<void(std::string_view name, uint32_t value)>& number,
    const std::function<void(std::string_view name, std::string_view value)>& text
);

/// Returns the settings the options dialog shows (ui.options-dialog): the
/// builders' options, the snap radii with the profile's maxima, the build
/// tools' switches, the chat backdrop and the resource bar's background.
/// The three key sliders are filled from the bound actions, not from here.
///
/// @param settings the player's settings
/// @param ui the profile's display rules, which give the snap radii's maxima
/// @return the dialog's settings
[[nodiscard]] oa::ui::engine_settings::ModOptions
dialog_options(const ViewSettings& settings, const data::mod_profile::UiRules& ui) noexcept;

/// Puts what the options dialog chose into the player's settings; the
/// settings it does not show stay as they are.
///
/// @param options the dialog's settings
/// @param ui the profile's display rules, which hold the snap radii
/// @param[in,out] settings the player's settings
void apply_dialog_options(
    const oa::ui::engine_settings::ModOptions& options,
    const data::mod_profile::UiRules& ui,
    ViewSettings& settings
) noexcept;

/// Returns what the options dialog cannot change: a snap radius whose
/// maximum the profile sets to 0, or that the profile does not turn on, is
/// set by the mod.
///
/// @param ui the profile's display rules
/// @return the locks; Locks::mex_snap and wreck_snap alone are set
[[nodiscard]] oa::ui::engine_settings::Locks
dialog_option_locks(const data::mod_profile::UiRules& ui) noexcept;

/// The option-key code a key slider shows for an action's first chord.
///
/// The code is one of the dialog's option keys when that key is the chord,
/// compared the way a stored key becomes a chord. Any other chord, or none,
/// shows as Alt.
///
/// @param action_id the action
/// @param chords the action's chords, as a layout writes them; the first is shown
/// @return the slider's key code
[[nodiscard]] uint32_t
slider_value(std::string_view action_id, std::span<const std::string_view> chords);

/// The chords a key slider writes for an action.
///
/// The key is mapped the way a stored key is imported. A lone modifier the
/// action does not take as a modifier maps to nothing, and the binding stays.
///
/// @param action_id the action
/// @param code the slider's key code
/// @return the chord texts, or none when the key cannot be mapped
[[nodiscard]] std::vector<std::string> slider_chords(std::string_view action_id, uint32_t code);

/// One key the options dialog used to store, and the code stored for it.
struct ImportedKey {
    std::string_view setting{}; ///< the stored name
    uint32_t code{};            ///< the stored code, or the code used when none is stored
};

/// Reads the stored code of one key the options dialog used to keep.
///
/// The three keys are the snap override, the line key and the rotate key.
/// Absent gives Alt, X or slash. Any other action is not read.
///
/// @param action_id the action
/// @param number reads a stored number by name; empty when nothing is stored
/// @return the key, or nothing for an action that has no stored key
[[nodiscard]] std::optional<ImportedKey> imported_key(
    std::string_view action_id,
    const std::function<std::optional<uint32_t>(std::string_view name)>& number
);

/// The texts a profile's strings show in place of the engine's own. Each is
/// null where the profile leaves 3.1c's text, or without a profile: the
/// engine's own text then shows, in the player's language where the game
/// data translates it. A text that differs from 3.1c's is shown as the
/// profile writes it, whatever the language.
struct ProfileTexts {
    const char* nanolathing_status{}; ///< strings.status.nanolathing
    const char* paralyzed_status{};   ///< strings.status.paralyzed
    const char* leave_question{};     ///< strings.message.exit-confirm
    const char* kill_lead{};          ///< strings.message.kill-lead
    /// strings.message.elimination, each ending compared with 3.1c's own.
    std::array<const char*, 3> elimination_endings{};
};

/// Returns the texts a profile's strings show in place of the engine's own.
///
/// @param profile the resolved profile; null plays 3.1c's
/// @return the texts, pointing into `profile`; all null without one
[[nodiscard]] ProfileTexts profile_texts(const data::mod_profile::ModProfile* profile) noexcept;

/// Returns the name of the main-menu gadget a click on which opens the
/// credits (strings.gadget.credits).
///
/// @param profile the resolved profile; null plays 3.1c's
/// @return the profile's name, or 3.1c's "Credits" without one
[[nodiscard]] std::string_view
credits_gadget(const data::mod_profile::ModProfile* profile) noexcept;

} // namespace oa::app::view_rules

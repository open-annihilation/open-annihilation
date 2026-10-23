// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Open Annihilation settings: their values and defaults, the keys the
// preferences file keeps them under, and which of them a running game
// locks. Every default plays the game as it plays without the settings;
// the dialog that shows them is in oa/ui/engine_settings/dialog.hpp.
#pragma once

#include "oa/data/languages.hpp"
#include "oa/data/limits.hpp"
#include "oa/data/mod_profile/overrides.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/present/text_style.hpp"
#include "oa/ui/pad_controls.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::engine_settings {

/// The preferences keys of the settings. 3.1c's own SwitchAlt keeps its key
/// in the game's section, which the frontend's preferences read and write.
namespace key {
/// Path nodes a game tick, decimal (EngineSettings::path_search_nodes).
inline constexpr std::string_view path_search_nodes = "open-annihilation.path-search-nodes";
/// 1 or 0 (EngineSettings::wheel_zoom).
inline constexpr std::string_view wheel_zoom = "open-annihilation.wheel-zoom";
/// "automatic", "whole-map", or the share of normal size: "1/32", "1/16",
/// "1/8", "1/4" or "1/2" (EngineSettings::max_zoom_out).
inline constexpr std::string_view max_zoom_out = "open-annihilation.max-zoom-out";
/// Times normal size in decimal: "1", "2", "3" or "4"
/// (EngineSettings::max_zoom_in).
inline constexpr std::string_view max_zoom_in = "open-annihilation.max-zoom-in";
/// "off", or the share of the battlefield in percent: "25" or "50"
/// (EngineSettings::view_past_map_edge).
inline constexpr std::string_view view_past_map_edge = "open-annihilation.view-past-map-edge";
/// "capture", "sticky" or "free" (EngineSettings::window_edge).
inline constexpr std::string_view window_edge = "open-annihilation.window-edge";
/// How firmly Sticky holds, a decimal from "0.0" to "5.0" in tenths
/// (EngineSettings::edge_stickiness).
inline constexpr std::string_view edge_stickiness = "open-annihilation.window-edge-stickiness";
/// 1 or 0 (EngineSettings::middle_click_pan).
inline constexpr std::string_view middle_click_pan = "open-annihilation.middle-click-pan";
/// 1 or 0 (EngineSettings::escape_opens_menu).
inline constexpr std::string_view escape_opens_menu = "open-annihilation.escape-opens-menu";
/// Units per player, decimal (EngineSettings::unit_limit).
inline constexpr std::string_view unit_limit = "open-annihilation.unit-limit";
/// Frames a second, decimal (EngineSettings::max_frame_rate).
inline constexpr std::string_view max_frame_rate = "open-annihilation.max-fps";
/// The level's samples along each axis, 1 for off (EngineSettings::anti_aliasing).
inline constexpr std::string_view anti_aliasing = "open-annihilation.anti-aliasing";
/// 1 or 0 (EngineSettings::frame_stats).
inline constexpr std::string_view frame_stats = "open-annihilation.frame-stats";
/// "desktop", or the width and height in decimal joined by an "x", as
/// "800x600" (EngineSettings::screen_size).
inline constexpr std::string_view screen_size = "open-annihilation.screen-size";
/// The mod folder played from the next start, as a UTF-8 path; absent for
/// none (EngineSettings::mod_folder).
inline constexpr std::string_view mod_directory = "open-annihilation.mod-directory";
/// The folder the player picked with an earlier version's Pick Folder...,
/// as a UTF-8 path, kept only while key::mod_directory names the same
/// folder; the Library lists it while it is still a folder. Absent for
/// none (EngineSettings::picked_mod_folder).
inline constexpr std::string_view picked_mod_directory = "open-annihilation.picked-mod-directory";
/// The base game the player plays: "totala" or "totala-demo"
/// (the content choice). Absent, or any other text, the choice is totala
/// with every copied expansion of it on.
inline constexpr std::string_view base_game = "open-annihilation.base-game";
/// The expansions that are on for one base game. The base game's id completes
/// it, as "open-annihilation.expansions.totala": their ids separated by
/// commas, in the built-in order. Empty means none are on. Absent, every
/// copied expansion of that game is on.
inline constexpr std::string_view expansions_prefix = "open-annihilation.expansions.";
/// "required-expansion" when a mod was cleared because an expansion it needs
/// went off (the content choice). Absent, no such note is waiting. The
/// sentence is chosen from this token.
inline constexpr std::string_view content_note = "open-annihilation.content-note";
/// "off", "basic" or "full" (EngineSettings::hardware_acceleration). A
/// whole number reads as the On and Off switch the setting was before: 1
/// or any number above 0 is "full", 0 or below "off".
inline constexpr std::string_view hardware_acceleration = "open-annihilation.hardware-acceleration";
/// 1 or 0 (EngineSettings::vertical_sync).
inline constexpr std::string_view vertical_sync = "open-annihilation.vertical-sync";
/// 1 or 0 (EngineSettings::smooth_motion).
inline constexpr std::string_view smooth_motion = "open-annihilation.smooth-motion";
/// "sharp", "whole-steps" or "unfiltered" (EngineSettings::menu_scaling).
inline constexpr std::string_view menu_scaling = "open-annihilation.menu-scaling";
/// 1 or 0 (EngineSettings::native_density).
inline constexpr std::string_view native_density = "open-annihilation.native-density";
/// "off", "reduced" or "full" (EngineSettings::explosion_flash).
inline constexpr std::string_view explosion_flash = "open-annihilation.explosion-flash";
/// "classic" or "smooth" (EngineSettings::fog_of_war).
inline constexpr std::string_view fog_of_war = "open-annihilation.fog-of-war";
/// "rendered" or "dots" (EngineSettings::zoomed_out_units).
inline constexpr std::string_view zoomed_out_units = "open-annihilation.zoomed-out-units";
/// The share of normal size past which Zoomed out units applies: "1/2",
/// "1/3", "1/4", "1/6", "1/8", "1/12" or "1/16"
/// (EngineSettings::zoomed_out_after).
inline constexpr std::string_view zoomed_out_after = "open-annihilation.zoomed-out-after";
/// "hidden-in-play" or "always-shown" (EngineSettings::window_frame).
inline constexpr std::string_view window_frame = "open-annihilation.window-frame";
/// 1 or 0 (EngineSettings::hud_scaling).
inline constexpr std::string_view hud_scaling = "open-annihilation.hud-scaling";
/// "automatic", or the percent "100", "200", "300" or "400"
/// (EngineSettings::interface_size).
inline constexpr std::string_view interface_size = "open-annihilation.interface-size";
/// 1 or 0 (EngineSettings::modern_fonts).
inline constexpr std::string_view modern_fonts = "open-annihilation.modern-fonts";
/// 1 or 0 (EngineSettings::text_outline).
inline constexpr std::string_view text_outline = "open-annihilation.text-outline";
/// 1 or 0 (EngineSettings::text_shadow).
inline constexpr std::string_view text_shadow = "open-annihilation.text-shadow";
/// 1 or 0 (EngineSettings::text_background).
inline constexpr std::string_view text_background = "open-annihilation.text-background";
/// 1 or 0 (EngineSettings::unicode_chat).
inline constexpr std::string_view unicode_chat = "open-annihilation.unicode-chat";
/// Percent of the game fonts' sizes, decimal (EngineSettings::text_size).
inline constexpr std::string_view text_size = "open-annihilation.text-size";
/// "system" for the operating system's choice, or a language's BCP-47 tag,
/// as "de" (EngineSettings::language).
inline constexpr std::string_view language = "open-annihilation.language";
/// Tags the player answered with Use English when the game asked, at the
/// start, about a language the operating system wanted that was not
/// installed. Comma-separated. Absent, none have been answered.
inline constexpr std::string_view language_notice_dismissed =
    "open-annihilation.language-notice-dismissed";
/// 1 or 0 (EngineSettings::developer_mode).
inline constexpr std::string_view developer_mode = "open-annihilation.developer-mode";
/// "automatic", "box" or "scroll" (EngineSettings::touch_drag).
inline constexpr std::string_view touch_drag = "open-annihilation.touch-drag";
/// Milliseconds, decimal (EngineSettings::touch_hold_ms).
inline constexpr std::string_view touch_hold_delay = "open-annihilation.touch-hold-delay";
/// "stay-on" or "one-action" (EngineSettings::touch_latches).
inline constexpr std::string_view touch_latches = "open-annihilation.touch-latches";
/// 1 or 0 (EngineSettings::touch_haptics).
inline constexpr std::string_view touch_haptics = "open-annihilation.touch-haptics";
/// 1 or 0 (EngineSettings::touch_left_handed).
inline constexpr std::string_view touch_left_handed = "open-annihilation.touch-left-handed";
/// "standard", "large" or "larger" (EngineSettings::touch_control_size).
inline constexpr std::string_view touch_control_size = "open-annihilation.touch-control-size";
/// "trackpads" or "sticks" (EngineSettings::pad_scheme).
inline constexpr std::string_view pad_scheme = "open-annihilation.pad-scheme";
/// "relative" or "absolute" (EngineSettings::pad_right_trackpad).
inline constexpr std::string_view pad_right_trackpad = "open-annihilation.pad-right-trackpad";
/// Percent, decimal (EngineSettings::pad_pointer_speed).
inline constexpr std::string_view pad_pointer_speed = "open-annihilation.pad-pointer-speed";
/// "off", "low" or "high" (EngineSettings::pad_acceleration).
inline constexpr std::string_view pad_acceleration = "open-annihilation.pad-acceleration";
/// 1 or 0 (EngineSettings::pad_glide).
inline constexpr std::string_view pad_glide = "open-annihilation.pad-glide";
/// "zoom", "pointer" or "nothing" (EngineSettings::pad_right_stick).
inline constexpr std::string_view pad_right_stick = "open-annihilation.pad-right-stick";
/// 1 or 0 (EngineSettings::pad_magnetism).
inline constexpr std::string_view pad_magnetism = "open-annihilation.pad-magnetism";
/// "off", "right-pad", "right-stick" or "always" (EngineSettings::pad_gyro).
inline constexpr std::string_view pad_gyro = "open-annihilation.pad-gyro";
/// Percent, decimal (EngineSettings::pad_gyro_speed).
inline constexpr std::string_view pad_gyro_speed = "open-annihilation.pad-gyro-speed";
/// "off", "light" or "strong" (EngineSettings::pad_haptics).
inline constexpr std::string_view pad_haptics = "open-annihilation.pad-haptics";
/// "automatic", "steam-deck", "xbox", "playstation", "nintendo" or "off"
/// (EngineSettings::pad_prompts).
inline constexpr std::string_view pad_prompts = "open-annihilation.pad-prompts";
/// 1 or 0 (EngineSettings::pad_left_handed).
inline constexpr std::string_view pad_left_handed = "open-annihilation.pad-left-handed";
/// 1 or 0 (EngineSettings::game_files_backed_up).
inline constexpr std::string_view game_files_backed_up = "open-annihilation.game-files-backed-up";
/// The start of the key a profile's overrides of its standard hacks are
/// kept under, which the profile's id ends (Inputs::profile_id), as
/// "open-annihilation.hack-overrides.ta-3.1c": their text as
/// oa::data::mod_profile::overrides_text writes it
/// (EngineSettings::hack_overrides).
inline constexpr std::string_view hack_overrides = "open-annihilation.hack-overrides.";
/// "automatically", "library" or "never" (EngineSettings::content_check).
/// When Open Annihilation looks for new content and updates.
inline constexpr std::string_view content_check = "open-annihilation.content-updates";
/// "on" or "off" (EngineSettings::file_type_oamod). Absent, or any text
/// other than off, the game opens mod packages.
inline constexpr std::string_view file_type_oamod = "open-annihilation.file-types.oamod";
/// "on" or "off" (EngineSettings::file_type_oamap). Absent, or any text
/// other than off, the game opens map packs.
inline constexpr std::string_view file_type_oamap = "open-annihilation.file-types.oamap";
/// "on" or "off" (EngineSettings::file_type_oalang). Absent, or any text
/// other than off, the game opens language packs.
inline constexpr std::string_view file_type_oalang = "open-annihilation.file-types.oalang";
/// "on" or "off" (EngineSettings::file_type_oareg). Absent, or any text
/// other than off, the game opens registry files.
inline constexpr std::string_view file_type_oareg = "open-annihilation.file-types.oareg";
/// "on" or "off" (EngineSettings::file_type_oamut). Absent, or any text
/// other than off, the game opens mutator packages.
inline constexpr std::string_view file_type_oamut = "open-annihilation.file-types.oamut";
/// "on" or "off" (EngineSettings::file_type_oabrain). Absent, or any text
/// other than off, the game opens brain packages.
inline constexpr std::string_view file_type_oabrain = "open-annihilation.file-types.oabrain";
/// "on" or "off" (EngineSettings::file_type_oalaunch). Absent, or any text
/// other than off, the game opens launch files.
inline constexpr std::string_view file_type_oalaunch = "open-annihilation.file-types.oalaunch";
/// The last mutator set a skirmish setup started a game with: the canonical
/// loadout text, mutators in order with versions, package SHA-256s and
/// options. Absent, or text that cannot be read as that loadout, means no
/// set. Restore defaults does not erase it.
inline constexpr std::string_view mutators_skirmish = "open-annihilation.mutators.skirmish";
/// The last mutator set the multiplayer host's setup started a game with:
/// the canonical loadout text, mutators in order with versions, package
/// SHA-256s and options. Absent, or text that cannot be read as that
/// loadout, means no set. Restore defaults does not erase it.
inline constexpr std::string_view mutators_multiplayer = "open-annihilation.mutators.multiplayer";
/// The last mutator set a campaign setup started a game with: the canonical
/// loadout text, mutators in order with versions, package SHA-256s and
/// options. Absent, or text that cannot be read as that loadout, means no
/// set. Restore defaults does not erase it.
inline constexpr std::string_view mutators_campaign = "open-annihilation.mutators.campaign";
static_assert(base_game.find('|') == std::string_view::npos);
static_assert(base_game.find('\\') == std::string_view::npos);
static_assert(expansions_prefix.find('|') == std::string_view::npos);
static_assert(expansions_prefix.back() == '.');
static_assert(content_note.find('|') == std::string_view::npos);
static_assert(mutators_skirmish.find('|') == std::string_view::npos);
static_assert(mutators_multiplayer.find('|') == std::string_view::npos);
static_assert(mutators_campaign.find('|') == std::string_view::npos);
} // namespace key

/// Tells whether two tags are the same, ignoring ASCII case and reading '_' as '-'.
///
/// @param left a tag
/// @param right a tag
/// @return true when they match
[[nodiscard]] inline bool
language_notice_same_tag(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto fold = [](char letter) noexcept {
            if (letter == '_')
                return '-';
            if (letter >= 'A' && letter <= 'Z')
                return static_cast<char>(letter - 'A' + 'a');
            return letter;
        };
        if (fold(left[index]) != fold(right[index]))
            return false;
    }
    return true;
}

/// Drops spaces and tabs from both ends of a tag list's piece.
///
/// @param text the piece
/// @return the piece
[[nodiscard]] inline std::string_view language_notice_trim(std::string_view text) noexcept {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
        text.remove_prefix(1);
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
        text.remove_suffix(1);
    return text;
}

/// Tells whether a dismissed-language list holds a tag.
///
/// The list is comma-separated. Each piece is trimmed. The match ignores
/// the case of ASCII letters and reads '_' as '-'.
///
/// @param stored the preference's value; empty when the key is absent
/// @param tag the language's tag
/// @return true when the list holds it
[[nodiscard]] inline bool
language_notice_dismissed_has(std::string_view stored, std::string_view tag) noexcept {
    const std::string_view wanted = language_notice_trim(tag);
    if (wanted.empty())
        return false;
    std::size_t start = 0;
    while (start <= stored.size()) {
        const auto comma = stored.find(',', start);
        const auto end = comma == std::string_view::npos ? stored.size() : comma;
        if (language_notice_same_tag(
                language_notice_trim(stored.substr(start, end - start)), wanted
            ))
            return true;
        if (comma == std::string_view::npos)
            break;
        start = comma + 1;
    }
    return false;
}

/// Returns the dismissed-language list with a tag added, when it is not there.
///
/// A tag already in the list leaves the stored text unchanged. An empty tag
/// leaves it unchanged. Otherwise the tag is appended after a comma.
///
/// @param stored the preference's value; empty when the key is absent
/// @param tag the language's tag
/// @return the value to store
[[nodiscard]] inline std::string
language_notice_dismiss_tag(std::string_view stored, std::string_view tag) {
    const std::string_view wanted = language_notice_trim(tag);
    if (wanted.empty() || language_notice_dismissed_has(stored, wanted))
        return std::string(stored);
    const std::string_view kept = language_notice_trim(stored);
    if (kept.empty())
        return std::string(wanted);
    std::string next(kept);
    next += ", ";
    next.append(wanted);
    return next;
}

/// Path nodes the path search may visit in a game tick, all players
/// together, at 1x: the credit every match starts with.
inline constexpr int32_t base_path_search_nodes = 1333;
/// The most the Pathfinding cycles setting multiplies base_path_search_nodes by.
inline constexpr int32_t highest_path_search_multiplier = 8;
/// The most path nodes a game tick the setting allows: 8x.
inline constexpr int32_t highest_path_search_nodes =
    base_path_search_nodes * highest_path_search_multiplier;

/// The lowest unit limit the setting offers, in units per player.
inline constexpr uint16_t lowest_unit_limit = 50;
/// The highest unit limit the setting offers, in units per player, unless a
/// mod's limits allow more (highest_offered_unit_limit).
inline constexpr uint16_t highest_unit_limit = 1500;
/// The unit limit setting's step, in units per player.
inline constexpr uint16_t unit_limit_step = 50;
/// The unit limit without a stored value or an installation's own, in units per player.
inline constexpr uint16_t default_unit_limit = 250;
/// The lowest unit limit the preferences keep, in units per player: the
/// lowest an installation's totala.ini can set, below the setting's stops.
inline constexpr uint16_t lowest_stored_unit_limit = 20;

/// The fewest skirmish player rows the Engine Limits slider offers.
inline constexpr int32_t lowest_skirmish_players = 2;
/// The most skirmish player rows the slider offers: every stored slot but the spare.
inline constexpr int32_t highest_skirmish_players = 10;
/// Skirmish player rows when none are stored. The setup reads the same count.
inline constexpr int32_t default_skirmish_players = 10;

/// The lowest maximum frame rate the setting offers, in frames a second:
/// the simulation's ticks a second, a frame for each tick.
inline constexpr uint32_t lowest_frame_rate = 30;
/// The highest maximum frame rate the setting offers, and its default, in frames a second.
inline constexpr uint32_t highest_frame_rate = 120;
/// The maximum frame rate setting's step, in frames a second.
inline constexpr uint32_t frame_rate_step = 5;
/// The maximum frame rate a Raspberry Pi starts with, in frames a second:
/// what its graphics keep up with at the game's resolutions.
inline constexpr uint32_t raspberry_pi_frame_rate = 60;
/// The maximum frame rate a light machine starts with, in frames a second.
inline constexpr uint32_t light_machine_frame_rate = 60;

/// The smallest text size the setting offers, in percent of the game fonts'
/// sizes.
inline constexpr int32_t lowest_text_size = oa::present::lowest_text_size;
/// The largest text size the setting offers, in percent.
inline constexpr int32_t highest_text_size = oa::present::highest_text_size;
/// The text size setting's step, in percent.
inline constexpr int32_t text_size_step = 10;
/// The text size a player starts with, in percent: a fifth smaller than the
/// game fonts.
inline constexpr int32_t default_text_size = oa::present::default_text_size;

/// The size the game's window, or the screen in full screen, is set to, in
/// pixels; zero by zero is the desktop's own size, the game's default.
struct ScreenSize {
    uint16_t width{};  ///< pixels across; 0 with height 0 for the desktop's size
    uint16_t height{}; ///< pixels down

    friend bool operator==(const ScreenSize&, const ScreenSize&) = default;
};

/// The desktop's size, as ScreenSize keeps it.
inline constexpr ScreenSize desktop_screen_size{};

/// The screen sizes the setting offers where the display's own are not
/// known, in the order the dialog offers them. The game offers Desktop and
/// the display's own sizes instead (Dialog::offered_screen_sizes).
inline constexpr std::array<ScreenSize, 5> screen_sizes{{
    desktop_screen_size,
    {640, 480},
    {800, 600},
    {1024, 768},
    {1280, 1024},
}};

/// The smallest screen size the preferences keep, the game's own screen.
inline constexpr ScreenSize smallest_screen_size{640, 480};
/// The longest side of a screen size the preferences keep, in pixels.
inline constexpr uint16_t longest_screen_side = 8192;

/// The screen size a light machine starts with.
inline constexpr ScreenSize light_machine_screen_size{800, 600};
/// The screen size a light machine starts with when its desktop is smaller
/// than light_machine_screen_size.
inline constexpr ScreenSize small_desktop_screen_size{640, 480};

/// The most bytes of an installation's totala.ini the defaults read.
inline constexpr std::size_t installation_ini_limit = std::size_t{64} * 1024;

/// Enhanced anti-aliasing: how many times finer, along each axis, each
/// unit's model is drawn before it is reduced into the frame. The value is
/// that number; off draws units as without the setting.
enum class AntiAliasing : uint8_t {
    off = 1,
    x2 = 2,
    x4 = 4,
    x8 = 8,
    x16 = 16,
};

/// The levels of enhanced anti-aliasing, in the order the dialog offers them.
inline constexpr std::array<AntiAliasing, 5> anti_aliasing_levels{
    AntiAliasing::off,
    AntiAliasing::x2,
    AntiAliasing::x4,
    AntiAliasing::x8,
    AntiAliasing::x16,
};

/// A key the mod options' key settings offer: its SDL key code and name.
struct OptionKey {
    uint32_t code{};         ///< the SDL key code
    std::string_view name{}; ///< what the dialog shows
};

/// The keys the mod options' key settings offer, in the order their
/// sliders step through them: the modifier keys, Tab, letters and marks.
inline constexpr std::array<OptionKey, 16> option_keys{{
    {0x400000e2U, "Alt"},
    {0x400000e0U, "Ctrl"},
    {0x400000e1U, "Shift"},
    {'\t', "Tab"},
    {'b', "B"},
    {'c', "C"},
    {'e', "E"},
    {'f', "F"},
    {'g', "G"},
    {'q', "Q"},
    {'r', "R"},
    {'v', "V"},
    {'x', "X"},
    {'z', "Z"},
    {'/', "/"},
    {'\\', "\\"},
}};

/// The most cells the mod options' snap radii go up to.
inline constexpr int32_t most_snap_radius = 9;

/// The settings a mod's display rules let the player change (its options
/// dialog, ui.options-dialog), as the mod options' sections show them. The
/// game keeps them with the mod's own settings, not with these; a dialog
/// opened for the engine's settings leaves them as they are.
struct ModOptions {
    uint32_t snap_override_key{}; ///< SDL key code held to stop a click snapping
    uint32_t autoclick_key{};     ///< SDL key code held to lay lines and rings
    uint32_t rotate_build_key{};  ///< SDL key code that turns a building
    /// Patrolling builders under Hold position, Maneuver and Roam: 0 reclaim
    /// only, 1 both, 2 assist only.
    std::array<uint8_t, 3> patrol{};
    /// Guarding builders under Hold position, Maneuver and Roam: 0 stay, 1
    /// as the game does, 2 scatter.
    std::array<uint8_t, 3> guard{};
    int32_t mex_snap_radius{};   ///< cells, 0 to mex_snap_most
    int32_t wreck_snap_radius{}; ///< cells, 0 to wreck_snap_most
    int32_t mex_snap_most{};     ///< the mod's most; the player cannot change it
    int32_t wreck_snap_most{};   ///< the mod's most; the player cannot change it
    bool optimize_dt_rows{};     ///< 2x2 lines lay out as a staggered double row
    bool full_rings{};           ///< rings include their corner places
    bool chat_backdrop{};        ///< chat lines get a dark backdrop
    uint8_t panel_background{};  ///< 0 none, 1 text, 2 solid

    friend bool operator==(const ModOptions&, const ModOptions&) = default;
};

/// Hardware acceleration: how much of each frame the graphics card takes
/// on. The processor still draws every pixel the game decides at Off and
/// Basic.
enum class HardwareAcceleration : uint8_t {
    off,   ///< the processor draws and scales every frame, as without the setting
    basic, ///< the graphics card scales and composes the frames, where it is able to
    /// The graphics card also draws the battlefield. Where it cannot, Basic
    /// draws instead and the status says why: the game cannot save its
    /// files, there is too little memory, the card lacks a feature Full
    /// needs, or Full stopped in this run or failed before on the driver;
    /// a shared game or a replay takes Full from the next game.
    full,
};

/// The levels of hardware acceleration, in the order the dialog offers them.
inline constexpr std::array<HardwareAcceleration, 3> hardware_acceleration_levels{
    HardwareAcceleration::off,
    HardwareAcceleration::basic,
    HardwareAcceleration::full,
};

/// Menu scaling: how the menus and the other screens the game draws at
/// 640x480 are enlarged to the window.
enum class MenuScaling : uint8_t {
    /// As large as the window holds, each of the screen's pixels as wide as
    /// the next: where the graphics card has the pixel-art filter, the edge
    /// between two of them blends over at most one column of the window;
    /// elsewhere as unfiltered.
    sharp,
    /// The largest whole number of window pixels to each of the screen's
    /// pixels that the window holds, the rest of the window black; as sharp
    /// in a window smaller than 640x480.
    whole_steps,
    /// As large as the window holds, each of the screen's pixels repeated
    /// over the window's pixels it covers, as without the setting.
    unfiltered,
};

/// The ways of Menu scaling, in the order the dialog offers them.
inline constexpr std::array<MenuScaling, 3> menu_scaling_choices{
    MenuScaling::sharp,
    MenuScaling::whole_steps,
    MenuScaling::unfiltered,
};

/// Interface size: how large Open Annihilation's own screens (Settings, its
/// notices and its prompts) are drawn outside a match. A size the window
/// cannot hold the smallest layout at is drawn at the largest whole step
/// that holds it.
enum class InterfaceSize : uint8_t {
    /// The window's own size: its height over 720, the nearest step, and
    /// never below the display's density. While the modern fonts are on and
    /// opened the steps include 150% (1x below 900 lines, 150% from 900, 2x
    /// from 1260); otherwise they are whole steps (1x below 1080, 2x from
    /// 1080). The OA layer passes that to the kit as 0 or
    /// kit::auto_with_fraction_percent (interface_size_percent is 0 either way)
    automatic,
    size_100, ///< one window pixel a point
    /// One and a half window pixels a point, drawn in the modern fonts: it
    /// is offered, and kept, only while Use modern fonts for game text is On
    /// (interface_size_with_fonts), as the game's own fonts take whole steps
    size_150,
    size_200, ///< two window pixels a point
    size_300, ///< three window pixels a point
    size_400, ///< four window pixels a point
};

/// The Interface sizes, in the order the dialog offers them; it offers
/// size_150 only while it shows Use modern fonts for game text On.
inline constexpr std::array<InterfaceSize, 6> interface_size_choices{
    InterfaceSize::automatic,
    InterfaceSize::size_100,
    InterfaceSize::size_150,
    InterfaceSize::size_200,
    InterfaceSize::size_300,
    InterfaceSize::size_400,
};

/// Returns the Interface size a choice is while Use modern fonts for game
/// text is On or Off: 150% is drawn in the modern fonts, so with them Off it
/// is 100%; every other size is itself.
///
/// @param size the Interface size chosen
/// @param modern_fonts Use modern fonts for game text is On
/// @return the size
[[nodiscard]] constexpr InterfaceSize
interface_size_with_fonts(InterfaceSize size, bool modern_fonts) noexcept {
    return size == InterfaceSize::size_150 && !modern_fonts ? InterfaceSize::size_100 : size;
}

/// Returns the scale an Interface size chooses, as the OA layer takes it
/// (oa::ui::kit::layer_viewport).
///
/// @param size the Interface size
/// @return the scale in percent: 100, 150, 200, 300 or 400; 0 for Auto
[[nodiscard]] constexpr int32_t interface_size_percent(InterfaceSize size) noexcept {
    switch (size) {
    case InterfaceSize::size_100:
        return 100;
    case InterfaceSize::size_150:
        return 150;
    case InterfaceSize::size_200:
        return 200;
    case InterfaceSize::size_300:
        return 300;
    case InterfaceSize::size_400:
        return 400;
    case InterfaceSize::automatic:
        break;
    }
    return 0;
}

/// Explosion flash: how strongly the flash of each explosion lights the
/// battlefield under it.
enum class ExplosionFlash : uint8_t {
    off,     ///< no flash is drawn
    reduced, ///< at half its light: about one and a half times as bright at the centre
    full,    ///< as 3.1c draws it: about twice as bright at the centre
};

/// The levels of Explosion flash, in the order the dialog offers them.
inline constexpr std::array<ExplosionFlash, 3> explosion_flash_choices{
    ExplosionFlash::off,
    ExplosionFlash::reduced,
    ExplosionFlash::full,
};

/// Fog of War: how the edges of the fog are drawn where the graphics card
/// draws the battlefield (Hardware acceleration Full). Where the processor
/// draws it, the fog is always drawn as the game drew it. Either way it
/// changes only what the frames show, never what any player can see.
enum class FogOfWar : uint8_t {
    /// as the game drew it: black and greyed ground with hard edges that
    /// step from sight cell to sight cell, through the game's own fog tiles
    /// with their dithered fringe
    classic,
    /// soft edges that fade across each sight cell
    smooth,
};

/// The ways of Fog of War, in the order the dialog offers them.
inline constexpr std::array<FogOfWar, 2> fog_of_war_choices{
    FogOfWar::classic,
    FogOfWar::smooth,
};

/// Zoomed out units: how the battlefield's units are drawn farther out than
/// After zoom.
enum class ZoomedOutUnits : uint8_t {
    rendered, ///< as at any zoom: each unit's model, as far out as the view goes
    dots,     ///< each unit a dot of its owner's colour, framed while selected
    icons,    ///< each unit an icon of its kind; offered, not yet chosen
};

/// The ways of Zoomed out units, in the order the dialog's strip shows them.
inline constexpr std::array<ZoomedOutUnits, 3> zoomed_out_units_choices{
    ZoomedOutUnits::rendered,
    ZoomedOutUnits::dots,
    ZoomedOutUnits::icons,
};

/// The ways of Zoomed out units a player may choose, from the strip's left:
/// Icons is shown, but cannot be chosen yet.
inline constexpr std::size_t offered_zoomed_out_units = 2;

/// Window edge: whether a window keeps the pointer while a match's
/// battlefield shows, so the view edge-scrolls. Full screen holds the
/// pointer either way.
enum class WindowEdge : uint8_t {
    /// the pointer cannot leave the window while the battlefield shows and
    /// the window has the focus
    capture,
    sticky, ///< the pointer rests on the edge until a firm push lets it go
    free,   ///< the pointer leaves a window's edge, as before
};

/// The ways of Window edge, in the order the dialog offers them.
inline constexpr std::array<WindowEdge, 3> window_edge_choices{
    WindowEdge::capture,
    WindowEdge::sticky,
    WindowEdge::free,
};

/// Edge stickiness's lowest value, in tenths: 0.0, a sticky edge that never
/// holds.
inline constexpr uint32_t lowest_edge_stickiness = 0;
/// Edge stickiness's highest value, in tenths: 5.0, five times as firm as
/// 1.0.
inline constexpr uint32_t highest_edge_stickiness = 50;
/// Edge stickiness's slider steps, in tenths: 0.1.
inline constexpr uint32_t edge_stickiness_step = 1;
/// Edge stickiness's default, in tenths: 1.0.
inline constexpr uint32_t default_edge_stickiness = 10;
/// Tenths in one whole level of Edge stickiness.
inline constexpr uint32_t edge_stickiness_per_level = 10;
static_assert(
    default_edge_stickiness >= lowest_edge_stickiness &&
        default_edge_stickiness <= highest_edge_stickiness,
    "Edge stickiness's default lies on its slider"
);

/// Returns Edge stickiness as the slider and the preferences show it: one
/// decimal, as "1.0" or "2.5".
///
/// @param tenths the level in tenths
/// @return the text
[[nodiscard]] std::string edge_stickiness_text(uint32_t tenths);

/// Reads Edge stickiness from its stored text: a decimal number with at
/// most one sign, digits and a point, as "1.0", "2.5", "3" or "0.25".
///
/// @param text the stored text
/// @return the level in tenths, rounded to the nearest tenth (a half rounds
///     up) and held to lowest_edge_stickiness to highest_edge_stickiness;
///     nothing for any other text
[[nodiscard]] std::optional<uint32_t> edge_stickiness_from_text(std::string_view text) noexcept;

/// Window frame: when the game's window shows its title bar and borders.
/// Full screen has none either way.
enum class WindowFrame : uint8_t {
    /// hidden while a game is played; shown while the game menu or a panel
    /// it opens is over the game, and on every other screen
    hidden_in_play,
    always_shown, ///< shown on every screen, as the window system draws them
};

/// The ways of Window frame, in the order the dialog offers them.
inline constexpr std::array<WindowFrame, 2> window_frame_choices{
    WindowFrame::hidden_in_play,
    WindowFrame::always_shown,
};

/// After zoom: the zoom farther out than which Zoomed out units applies,
/// as a share of normal size.
enum class ZoomedOutAfter : uint8_t {
    one_half,      ///< farther out than 1/2
    one_third,     ///< than 1/3
    one_quarter,   ///< than 1/4
    one_sixth,     ///< than 1/6
    one_eighth,    ///< than 1/8
    one_twelfth,   ///< than 1/12
    one_sixteenth, ///< than 1/16
};

/// The After zoom choices, in the order the dialog offers them.
inline constexpr std::array<ZoomedOutAfter, 7> zoomed_out_afters{
    ZoomedOutAfter::one_half,
    ZoomedOutAfter::one_third,
    ZoomedOutAfter::one_quarter,
    ZoomedOutAfter::one_sixth,
    ZoomedOutAfter::one_eighth,
    ZoomedOutAfter::one_twelfth,
    ZoomedOutAfter::one_sixteenth,
};

/// Returns the zoom an After zoom choice stands for.
///
/// @param after the choice
/// @return screen pixels per map pixel: Zoomed out units applies below it
[[nodiscard]] constexpr float zoomed_out_zoom(ZoomedOutAfter after) noexcept {
    switch (after) {
    case ZoomedOutAfter::one_half:
        return 1.0F / 2.0F;
    case ZoomedOutAfter::one_third:
        return 1.0F / 3.0F;
    case ZoomedOutAfter::one_quarter:
        return 1.0F / 4.0F;
    case ZoomedOutAfter::one_sixth:
        return 1.0F / 6.0F;
    case ZoomedOutAfter::one_eighth:
        return 1.0F / 8.0F;
    case ZoomedOutAfter::one_twelfth:
        return 1.0F / 12.0F;
    case ZoomedOutAfter::one_sixteenth:
        return 1.0F / 16.0F;
    }
    return 1.0F / 6.0F;
}

/// What a one-finger drag on the battlefield does (the Touch section's One-finger drag).
enum class TouchDrag : uint8_t {
    automatic, ///< a selection box on a tablet, scrolling on a phone
    box,       ///< a selection box
    scroll,    ///< the map follows the finger
};

/// Whether QUEUE, ADD and x5 stay on after use (the Touch section's QUEUE and ADD).
enum class TouchLatches : uint8_t {
    stay_on,    ///< a latched control stays on until tapped again
    one_action, ///< a latched control turns off after the first action that uses it
};

/// The shortest hold delay of the touch controls, in milliseconds.
inline constexpr uint32_t lowest_touch_hold_ms = 250;
/// The longest hold delay of the touch controls, in milliseconds.
inline constexpr uint32_t highest_touch_hold_ms = 700;
/// The hold delay's step, in milliseconds.
inline constexpr uint32_t touch_hold_step_ms = 50;
/// The hold delay of the touch controls when the player has chosen none, in milliseconds.
inline constexpr uint32_t default_touch_hold_ms = 350;
static_assert(
    (highest_touch_hold_ms - lowest_touch_hold_ms) % touch_hold_step_ms == 0 &&
        (default_touch_hold_ms - lowest_touch_hold_ms) % touch_hold_step_ms == 0,
    "the hold delay's stops run from its lowest to its highest, its default among them"
);

/// The ways of One-finger drag, in the order the dialog offers them.
inline constexpr std::array<TouchDrag, 3> touch_drag_choices{
    TouchDrag::automatic,
    TouchDrag::box,
    TouchDrag::scroll,
};

/// The ways of QUEUE and ADD, in the order the dialog offers them.
inline constexpr std::array<TouchLatches, 2> touch_latches_choices{
    TouchLatches::stay_on,
    TouchLatches::one_action,
};

/// The touch layer's size on a dense screen (the Touch and Controller sections' Control size).
enum class ControlSize : uint8_t {
    standard, ///< the touch layer's own sizes
    large,    ///< a quarter larger
    larger,   ///< half as large again
};

/// The Control sizes, in the order the dialog offers them.
inline constexpr std::array<ControlSize, 3> control_size_choices{
    ControlSize::standard,
    ControlSize::large,
    ControlSize::larger,
};

/// Maximum zoom out: how far out the battlefield's view zooms. No choice but
/// Automatic zooms out past the point where the whole map fits the view.
enum class ZoomOutLimit : uint8_t {
    /// As far as the drawing allows: a sixth of normal size while the
    /// graphics card draws the battlefield (Full), a half otherwise.
    automatic,
    whole_map,         ///< until the whole map fits the view
    one_thirty_second, ///< to 1/32 of normal size
    one_sixteenth,     ///< to 1/16
    one_eighth,        ///< to 1/8
    one_quarter,       ///< to 1/4
    one_half,          ///< to 1/2
};

/// The Maximum zoom out choices, in the order the dialog offers them.
inline constexpr std::array<ZoomOutLimit, 7> zoom_out_limits{
    ZoomOutLimit::automatic,
    ZoomOutLimit::whole_map,
    ZoomOutLimit::one_thirty_second,
    ZoomOutLimit::one_sixteenth,
    ZoomOutLimit::one_eighth,
    ZoomOutLimit::one_quarter,
    ZoomOutLimit::one_half,
};

/// Returns the share of normal size a Maximum zoom out choice stops at.
///
/// @param limit the choice
/// @return screen pixels per map pixel: 1/32 to 1/2 for a share, 0 for
///     Whole map, which only the whole map's fit stops; nothing for
///     Automatic, which the drawing decides
[[nodiscard]] constexpr std::optional<float> zoom_out_share(ZoomOutLimit limit) noexcept {
    switch (limit) {
    case ZoomOutLimit::automatic:
        return std::nullopt;
    case ZoomOutLimit::whole_map:
        return 0.0F;
    case ZoomOutLimit::one_thirty_second:
        return 1.0F / 32.0F;
    case ZoomOutLimit::one_sixteenth:
        return 1.0F / 16.0F;
    case ZoomOutLimit::one_eighth:
        return 1.0F / 8.0F;
    case ZoomOutLimit::one_quarter:
        return 1.0F / 4.0F;
    case ZoomOutLimit::one_half:
        return 1.0F / 2.0F;
    }
    return std::nullopt;
}

/// Maximum zoom in: how far in the battlefield's view zooms, in times
/// normal size.
enum class ZoomInLimit : uint8_t {
    none = 1,        ///< never past normal size
    twice = 2,       ///< to 2x normal size
    three_times = 3, ///< to 3x
    four_times = 4,  ///< to 4x
};

/// The Maximum zoom in choices, in the order the dialog offers them.
inline constexpr std::array<ZoomInLimit, 4> zoom_in_limits{
    ZoomInLimit::none,
    ZoomInLimit::twice,
    ZoomInLimit::three_times,
    ZoomInLimit::four_times,
};

/// Returns the zoom a Maximum zoom in choice stops at.
///
/// @param limit the choice
/// @return screen pixels per map pixel: 1, 2, 3 or 4
[[nodiscard]] constexpr float closest_zoom(ZoomInLimit limit) noexcept {
    return static_cast<float>(static_cast<uint8_t>(limit));
}

/// View past the map's edge: how much of the battlefield the view may show
/// past the map's edges.
enum class ViewPastMapEdge : uint8_t {
    off,         ///< none: the view stays on the map, as 3.1c's does
    one_quarter, ///< up to a quarter of the battlefield
    one_half,    ///< up to half of it: the map's edge reaches its middle
};

/// The View past the map's edge choices, in the order the dialog offers them.
inline constexpr std::array<ViewPastMapEdge, 3> view_past_map_edge_choices{
    ViewPastMapEdge::off,
    ViewPastMapEdge::one_quarter,
    ViewPastMapEdge::one_half,
};

/// Returns the share of the battlefield a View past the map's edge choice
/// lets the view show past each of the map's edges.
///
/// @param limit the choice
/// @return 0, 0.25 or 0.5
[[nodiscard]] constexpr double past_map_edge_share(ViewPastMapEdge limit) noexcept {
    switch (limit) {
    case ViewPastMapEdge::off:
        return 0.0;
    case ViewPastMapEdge::one_quarter:
        return 0.25;
    case ViewPastMapEdge::one_half:
        return 0.5;
    }
    return 0.5;
}

/// The Control size a Steam Deck starts with: its screen is small and dense,
/// so the touch controls come close to a tablet's in size.
inline constexpr ControlSize steam_deck_control_size = ControlSize::larger;

/// Returns the points scale of a Control size.
///
/// @param size the Control size
/// @return 1, 1.25 or 1.5
[[nodiscard]] float control_size_scale(ControlSize size) noexcept;

/// Returns a hold delay as the setting keeps it: held to its range and put
/// on its nearest stop.
///
/// @param milliseconds the delay, in milliseconds
/// @return lowest_touch_hold_ms to highest_touch_hold_ms, a whole number of
///     touch_hold_step_ms above the lowest; half a step rounds up
[[nodiscard]] constexpr uint32_t snapped_touch_hold_ms(int64_t milliseconds) noexcept {
    const int64_t lowest = lowest_touch_hold_ms;
    const int64_t highest = highest_touch_hold_ms;
    const int64_t step = touch_hold_step_ms;
    const int64_t held = milliseconds < lowest    ? lowest
                         : milliseconds > highest ? highest
                                                  : milliseconds;
    return static_cast<uint32_t>(lowest + (held - lowest + step / 2) / step * step);
}

/// When Open Annihilation looks for new content and updates. It never
/// installs anything until the player says so.
enum class ContentCheck : uint8_t {
    automatically,      ///< at start and when the Library opens
    when_library_opens, ///< only when the Library opens
    never,              ///< only when the player asks
};

/// The settings, as the dialog shows them and the game puts them in effect.
struct EngineSettings {
    /// Path nodes the path search may visit in a game tick, all players
    /// together: base_path_search_nodes times the Pathfinding cycles.
    int32_t path_search_nodes{base_path_search_nodes};
    bool wheel_zoom{true}; ///< the mouse wheel zooms the battlefield
    /// How far out the battlefield's view zooms, by the wheel, a pinch, the
    /// touch controls or a controller.
    ZoomOutLimit max_zoom_out{ZoomOutLimit::automatic};
    /// How far in the battlefield's view zooms, by the same.
    ZoomInLimit max_zoom_in{ZoomInLimit::four_times};
    /// How much of the battlefield the view may show past the map's edges.
    ViewPastMapEdge view_past_map_edge{ViewPastMapEdge::one_half};
    /// A middle click pans the view towards the pointer until another press.
    bool middle_click_pan{true};
    bool escape_opens_menu{}; ///< Escape with nothing to cancel opens the game menu
    bool switch_alt{};        ///< 3.1c's SwitchAlt: a number key alone selects its group
    /// Whether a window keeps the pointer while a match's battlefield
    /// shows. Full screen holds the pointer either way.
    WindowEdge window_edge{WindowEdge::capture};
    /// How firmly Sticky holds the pointer at a window's edges, in tenths:
    /// 10 is 1.0. Kept whatever Window edge is.
    uint32_t edge_stickiness{default_edge_stickiness};
    uint16_t unit_limit{default_unit_limit};       ///< units per player, from the next game
    uint32_t max_frame_rate{highest_frame_rate};   ///< frames a second
    AntiAliasing anti_aliasing{AntiAliasing::off}; ///< enhanced anti-aliasing of units
    bool frame_stats{}; ///< the frame and tick times over the battlefield (+stats)
    TouchDrag touch_drag{TouchDrag::automatic};            ///< One-finger drag
    uint32_t touch_hold_ms{default_touch_hold_ms};         ///< Hold delay, ms
    TouchLatches touch_latches{TouchLatches::stay_on};     ///< QUEUE and ADD
    bool touch_haptics{true};                              ///< Haptics
    bool touch_left_handed{};                              ///< Left-handed layout
    ControlSize touch_control_size{ControlSize::standard}; ///< Control size
    /// Scheme (Controller).
    oa::ui::pad_controls::Scheme pad_scheme{oa::ui::pad_controls::Scheme::trackpads};
    /// Right trackpad (Controller).
    oa::ui::pad_controls::RightTrackpad pad_right_trackpad{
        oa::ui::pad_controls::RightTrackpad::relative
    };
    /// Pointer speed, percent (Controller).
    uint32_t pad_pointer_speed{oa::ui::pad_controls::default_pointer_speed};
    /// Pointer acceleration (Controller).
    oa::ui::pad_controls::Acceleration pad_acceleration{oa::ui::pad_controls::Acceleration::low};
    bool pad_glide{}; ///< Trackpad glide (Controller)
    /// Right stick (Controller).
    oa::ui::pad_controls::RightStick pad_right_stick{
        oa::ui::pad_controls::RightStick::zoom_and_pages
    };
    bool pad_magnetism{true}; ///< Magnetism (stick pointer) (Controller)
    /// Gyro pointer (Controller).
    oa::ui::pad_controls::Gyro pad_gyro{oa::ui::pad_controls::Gyro::off};
    /// Gyro speed, percent (Controller).
    uint32_t pad_gyro_speed{oa::ui::pad_controls::default_gyro_speed};
    /// Haptics (Controller).
    oa::ui::pad_controls::Haptics pad_haptics{oa::ui::pad_controls::Haptics::light};
    /// Button prompts (Controller).
    oa::ui::pad_controls::Prompts pad_prompts{oa::ui::pad_controls::Prompts::automatic};
    bool pad_left_handed{}; ///< Left-handed (Controller)
    /// The game files are kept in the device's backups (only where the platform keeps them).
    bool game_files_backed_up{};
    /// The window's size, and the screen's in full screen, from the next start.
    ScreenSize screen_size{desktop_screen_size};
    /// How much of each frame the graphics card may take on, where it is
    /// able to.
    HardwareAcceleration hardware_acceleration{HardwareAcceleration::off};
    /// Each frame waits for the display to be ready for it, so that no frame
    /// tears, and the frame rate keeps just below the display's.
    bool vertical_sync{};
    /// Units, their moving and spinning pieces, shots, debris and effects
    /// are drawn part of the way between the game's ticks, so they move
    /// smoothly at every frame rate. Off, every frame draws them where the
    /// latest tick left them, as the game drew them 30 times a second. It
    /// changes only what the frames show, never the game.
    bool smooth_motion{true};
    /// How the menus are enlarged to the window.
    MenuScaling menu_scaling{MenuScaling::sharp};
    /// The window opens at the display's own pixel density, from the next
    /// start: on macOS the Retina resolution, elsewhere the display's scale.
    /// Always on where the platform opens every window so
    /// (Inputs::native_density_windows).
    bool native_density{};
    /// How strongly explosions' flashes light the battlefield; a mod's
    /// profile may hold them lower still (ui.explosion-flash).
    ExplosionFlash explosion_flash{ExplosionFlash::full};
    /// How the fog's edges are drawn while the graphics card draws the
    /// battlefield; the processor's drawing is always Classic.
    FogOfWar fog_of_war{FogOfWar::smooth};
    /// How units are drawn farther out than zoomed_out_after: as at any
    /// zoom, or as dots. Never icons, which the dialog shows but does not
    /// offer yet.
    ZoomedOutUnits zoomed_out_units{ZoomedOutUnits::rendered};
    /// The zoom farther out than which zoomed_out_units applies.
    ZoomedOutAfter zoomed_out_after{ZoomedOutAfter::one_sixth};
    /// When the game's window shows its title bar and borders.
    WindowFrame window_frame{WindowFrame::hidden_in_play};
    /// The side column and the top and bottom bars of a game grow with the
    /// window, up to twice the original game's size; off, they keep the
    /// original game's size on every window. The touch controls' layouts
    /// keep their own sizes either way.
    bool hud_scaling{true};
    /// How large Open Annihilation's own screens are drawn outside a match:
    /// the window's Auto scale, or a step the OA layer holds to what fits
    /// the smallest layout in the window; 150% only with modern_fonts On. It
    /// takes effect when the dialog's OK is pressed, and changes no game
    /// text, HUD or menu.
    InterfaceSize interface_size{InterfaceSize::automatic};
    /// Game text is drawn in the modern fonts, which hold the letters of
    /// many languages, rather than the game's own 8-bit fonts. On by default
    /// with the player's own preferences file (default_settings).
    bool modern_fonts{};
    bool text_outline{true}; ///< modern text has a dark outline round each letter
    bool text_shadow{true};  ///< modern text casts a dark shadow
    bool text_background{};  ///< each line of modern text is drawn on a shaded box
    /// Multiplayer chat is sent and read as UTF-8, in any language, to the
    /// players' machines that read it, and as the code page with '?' for
    /// the letters it lacks to the others. Off by default; a language pack
    /// that asks for it turns it on while its language is shown. It changes
    /// only how chat lines are written, never the simulation or a saved
    /// game, so players with different settings play together.
    bool unicode_chat{};
    /// The size of game text in the modern fonts, in percent of the game
    /// fonts' sizes, lowest_text_size to highest_text_size; the game's own
    /// fonts keep their sizes.
    int32_t text_size{default_text_size};
    /// The language the game shows its text in: system_choice
    /// (oa/data/languages.hpp) for the operating system's preferred
    /// language, else a known language's tag; English, the game's own,
    /// unless default_settings says otherwise. It changes only what players
    /// read: the simulation, a saved game and what a shared game sends are
    /// the same in every language.
    std::string language{oa::data::languages::english().tag};
    /// The mod folder played from the next start, as an absolute UTF-8 path:
    /// one of Inputs::mod_folders, or a folder the player picked; empty for
    /// none, the game as 3.1c plays it.
    std::string mod_folder;
    /// The folder the player picked with an earlier version's Pick
    /// Folder..., as an absolute UTF-8 path, while it is the mod
    /// (mod_folder); the Library lists it while it is still a folder.
    /// Empty for none.
    std::string picked_mod_folder;
    /// Developer Mode: the player's overrides of the standard hacks
    /// (hack_overrides) are laid over the profile the game plays. Off, the
    /// profile plays as it ships, and the overrides are kept.
    bool developer_mode{};
    /// The player's overrides of the standard hacks of the profile the game
    /// plays, or of the plain 3.1c baseline without a mod: at most one for
    /// each hack, kept under the profile's id (Inputs::profile_id). They
    /// apply while developer_mode is on: a display (view-scope) hack's at
    /// once, a rule (sim-scope) hack's from the next match's start.
    std::vector<oa::data::mod_profile::HackOverride> hack_overrides;
    /// The mod's own options, which only a dialog opened for them changes;
    /// read_settings and write_settings leave them out.
    ModOptions mod_options{};
    /// When Open Annihilation looks for new content and updates. It takes
    /// effect when OK is pressed; Cancel and Restore defaults apply.
    ContentCheck content_check{ContentCheck::automatically};
    /// The game opens mod packages (.oamod). On unless the preference is the
    /// word off. Windows and Linux register it when OK is pressed. On macOS
    /// the switch shows whether this copy is the default opener, and OK
    /// changes that only when the switch moved. iOS takes the type from the app.
    bool file_type_oamod{true};
    bool file_type_oamap{true};   ///< the game opens map packs (.oamap)
    bool file_type_oalang{true};  ///< the game opens language packs (.oalang)
    bool file_type_oareg{true};   ///< the game opens registry files (.oareg)
    bool file_type_oamut{true};   ///< the game opens mutator packages (.oamut)
    bool file_type_oabrain{true}; ///< the game opens brain packages (.oabrain)
    /// The game opens launch files (.oalaunch), which start it in their
    /// battle room, as -c does.
    bool file_type_oalaunch{true};
    /// How many player rows the skirmish setup shows. The dialog's working
    /// copy of NumSkirmishPlayers: read_settings and write_settings leave it
    /// out, and OK stores it as that one count.
    int32_t skirmish_players{default_skirmish_players};

    friend bool operator==(const EngineSettings&, const EngineSettings&) = default;
};

/// What the defaults depend on besides the engine itself.
struct Inputs {
    /// The preferences file is the player's own: --preferences-file did not
    /// name one. With a named file every default is the game's own
    /// behaviour on every platform, and the installation is not read.
    bool players_own_profile{};
    bool macos{}; ///< the game runs on macOS
    /// The installation's totala.ini, at most installation_ini_limit bytes
    /// of it; empty when it has none.
    std::string_view installation_ini{};
    bool raspberry_pi{}; ///< the game runs on a Raspberry Pi
    /// The game runs on a light machine (oa::platform::light_machine): one
    /// processor, no SSE2 or under 512 MiB of memory.
    bool light_machine{};
    /// The desktop's size; zero by zero when it is not known.
    ScreenSize desktop{};
    /// The unit limits the game allows: the limit a player starts with, and
    /// the range a stored or installation limit is clamped to. 3.1c's by
    /// default; a mod's profile may raise them.
    oa::data::limits::UnitsPerPlayer units_per_player{};
    /// The mod folders the game folder offers, as the preferences keep a
    /// chosen one (an absolute UTF-8 path), in the order the setting offers
    /// them.
    std::span<const std::string> mod_folders{};
    /// The id of the profile the game plays, which its overrides are kept
    /// under (key::hack_overrides): the mod's, or
    /// oa::data::mod_profile::base_game_id without a mod. Empty reads no
    /// overrides.
    std::string_view profile_id{};
    /// The game runs on a Steam Deck whose screen refreshes this many times a second (60 on
    /// the LCD model, 90 on the OLED); 0 on every other machine.
    uint32_t steam_deck_panel_hz{};
    /// The platform the game is built for opens every window at the
    /// display's own pixel density, so that Native pixel density is always
    /// on.
    bool native_density_windows{};
};

/// Returns the highest unit limit the setting offers and keeps.
///
/// @param units the unit limits the game allows
/// @return highest_unit_limit, or the limits' maximum when that is higher
[[nodiscard]] constexpr uint16_t
highest_offered_unit_limit(const oa::data::limits::UnitsPerPlayer& units) noexcept {
    return units.maximum > highest_unit_limit ? units.maximum : highest_unit_limit;
}

/// Returns the settings a player has before changing any.
///
/// Escape opens the game menu by default on macOS with the player's own
/// preferences file; the unit limit is the installation's
/// (installation_unit_limit) with the player's own file, else
/// the limits' default_limit (default_unit_limit for 3.1c). On a Raspberry Pi with the player's own file the
/// maximum frame rate is raspberry_pi_frame_rate and enhanced
/// anti-aliasing is off. On a light machine with the player's own file the
/// maximum frame rate is light_machine_frame_rate, enhanced anti-aliasing is
/// off and the screen size is light_machine_screen_size, or
/// small_desktop_screen_size on a known desktop narrower or shorter than it.
/// Hardware acceleration is Full with the player's own file, on every
/// machine, and Off with a named one; whether the graphics card is used is
/// decided apart from the setting. Vertical sync is Off everywhere. Menu
/// scaling is Sharp everywhere. Mouse wheel zoom is On, Maximum zoom out
/// Automatic, Maximum zoom in 4x and View past the map's edge 50%
/// everywhere. Native pixel density is
/// Off, but On where the platform opens every window at native density.
/// Explosion flash is Full everywhere, as 3.1c draws it. Zoomed out units
/// are Rendered and After zoom 1/6 everywhere. Window frame is Hidden in
/// play, HUD scaling On and Interface size Auto everywhere. Modern
/// fonts for game text are On with the player's own file and Off with a
/// named one; their outline and shadow are On, their background Off and
/// their size default_text_size everywhere. The language is the operating
/// system's choice with the player's own file and English, the game's own
/// default, with a named one. Developer Mode is Off, with no overrides. The
/// Touch section is the same everywhere: One-finger drag Automatic, Hold
/// delay default_touch_hold_ms, QUEUE and ADD Stay on, Haptics On,
/// Left-handed layout Off and Control size Standard. On a Steam Deck
/// (Inputs::steam_deck_panel_hz above 0) with the player's own file the
/// maximum frame rate is the screen's rate, held to lowest_frame_rate to
/// highest_frame_rate and put on its nearest stop, and Control size is
/// steam_deck_control_size; nothing else changes there. The Controller
/// section is the same everywhere: Scheme Trackpads, Right trackpad
/// relative, Pointer speed default_pointer_speed, Pointer acceleration Low,
/// Trackpad glide Off, Right stick Zoom and build pages, Magnetism On, Gyro
/// pointer Off, Gyro speed default_gyro_speed, Haptics Light, Button prompts
/// Automatic and Left-handed Off (oa/ui/pad_controls.hpp).
///
/// @param inputs the platform, the preferences file and the installation
/// @return the defaults
[[nodiscard]] EngineSettings default_settings(const Inputs& inputs);

/// Reads the settings from the preferences.
///
/// A key that is absent, or whose value is not a whole decimal number,
/// gives the default; a value out of a setting's range is clamped into it.
/// The ranges: path nodes base_path_search_nodes to
/// highest_path_search_nodes; unit limit the limits' minimum
/// (lowest_stored_unit_limit for 3.1c) to highest_offered_unit_limit; frame rate lowest_frame_rate to highest_frame_rate;
/// anti-aliasing the highest level not above the stored number, off below
/// 2; a switch is on for a number above 0. A value between a setting's
/// stops is kept as stored. The screen size is "desktop" or a size as
/// "WIDTHxHEIGHT" (screen_size_from_text); any other value gives the
/// default.
/// Hardware acceleration is "off", "basic" or "full"
/// (hardware_acceleration_from_text), or a whole number as the switch it
/// was before: Full above 0, else Off; any other value gives the default.
/// The mod is the stored folder, and the picked folder the one
/// remembered_picked_folder gives; a mod folder the game folder does not
/// offer is the picked folder whatever that key holds, so that the
/// Library lists it. The Language switches
/// (modern fonts, text outline, shadow and background) and Developer Mode
/// read as every
/// switch does, and the text size as every number, lowest_text_size to
/// highest_text_size. The language is stored_language's. The overrides are
/// read from the key that Inputs::profile_id ends, as
/// oa::data::mod_profile::read_overrides reads them: none without the key,
/// from a text the profile grammar does not read as a mapping (one that
/// names a hack twice among them), or with an empty profile id. In the
/// Touch section, One-finger drag and QUEUE and ADD read the words
/// touch_drag_text and touch_latches_text write, and any other value gives
/// the default; the hold delay reads as a number, put on its nearest stop
/// within its range (snapped_touch_hold_ms); Haptics and Left-handed layout
/// read as every switch does. Menu scaling reads the words
/// menu_scaling_text writes, and any other value gives the default; Native
/// pixel density reads as every switch does, but stays on where the
/// platform opens every window at native density. Explosion flash reads
/// "off", "reduced" or "full", and any other value gives the default.
/// Control size and the
/// Controller section's choices read the words their keys name ("standard",
/// "large", "larger"; "trackpads", "sticks"; "relative", "absolute"; "off",
/// "low", "high"; "zoom", "pointer", "nothing"; "off", "right-pad",
/// "right-stick", "always"; "off", "light", "strong"; "automatic",
/// "steam-deck", "xbox", "playstation", "nintendo", "off"), and any other
/// value gives the default; Pointer speed and Gyro speed read as numbers,
/// held to their ranges and put on their nearest stops (half a step rounds
/// up); Trackpad glide, Magnetism and Left-handed read as every switch does.
/// Maximum zoom out reads "automatic", "whole-map", "1/32", "1/16", "1/8",
/// "1/4" or "1/2", Maximum zoom in "1", "2", "3" or "4", and View past the
/// map's edge "off", "25" or "50"; any other value gives the default.
/// Zoomed out units reads "rendered" or "dots", and After zoom
/// "1/2", "1/3", "1/4", "1/6", "1/8", "1/12" or "1/16"; any other value,
/// "icons" among them, gives the default. Window frame reads
/// "hidden-in-play" or "always-shown", and any other value gives the
/// default. Interface size reads the words interface_size_text writes, and
/// any other value, "0" and an empty one among them, gives Auto; "150" read
/// while Use modern fonts for game text reads Off gives 100%
/// (interface_size_with_fonts). A stored value always wins over a Steam
/// Deck's defaults.
///
/// @param values the preferences
/// @param inputs the platform, the preferences file and the installation
/// @param switch_alt 3.1c's SwitchAlt as the frontend's preferences hold it
/// @return the settings
[[nodiscard]] EngineSettings read_settings(
    const oa::platform::preferences::Values& values, const Inputs& inputs, bool switch_alt
);

/// Writes the settings a player chose into the preferences.
///
/// For each setting but switch_alt and skirmish_players (the setup stores
/// that count as NumSkirmishPlayers): after Restore defaults (`restored`), a
/// setting at its default has its key erased; otherwise a setting that
/// differs from `opened` has its key written, in decimal, a switch as 1 or
/// 0, the screen size as "desktop" or "WIDTHxHEIGHT", hardware
/// acceleration as "off", "basic" or "full", Menu scaling, One-finger drag
/// and QUEUE and ADD as their words (menu_scaling_text, touch_drag_text,
/// touch_latches_text), Explosion flash as "off", "reduced" or "full", the
/// hold delay in milliseconds, Maximum zoom out, Maximum zoom in, View past
/// the map's edge, Zoomed out units, After zoom, Window frame, Interface
/// size (interface_size_text), Control size and the Controller section's
/// choices as the words read_settings reads,
/// Pointer speed and Gyro speed in percent, the mod and the picked folder as their
/// paths, or erased
/// for none; Restore defaults leaves the picked folder as it is. The picked
/// folder's key is then erased unless the mod key names the same folder,
/// a key an earlier save left included: once another mod or No Mod is
/// chosen, the picked folder is forgotten. The overrides, when they differ
/// from `opened`, are written under the key `profile_id` ends, as
/// oa::data::mod_profile::overrides_text writes them, or that key is
/// erased when none are left; Restore defaults leaves them as they are.
/// Every other key is left as it is. switch_alt is never written here:
/// 3.1c's SwitchAlt key goes with the frontend's own preferences.
///
/// @param[in,out] values the preferences
/// @param opened the settings when the dialog opened
/// @param chosen the settings the player keeps
/// @param defaults the defaults (default_settings)
/// @param restored Restore defaults was pressed while the dialog was open
/// @param profile_id the id the overrides are kept under (Inputs::profile_id);
///     empty writes no overrides
void write_settings(
    oa::platform::preferences::Values& values,
    const EngineSettings& opened,
    const EngineSettings& chosen,
    const EngineSettings& defaults,
    bool restored,
    std::string_view profile_id = {}
);

/// Returns the folder the player picked, as the preferences remember it.
///
/// A picked folder is remembered only while it is the mod played: the
/// picked folder's key (key::picked_mod_directory) counts while the mod key
/// (key::mod_directory) holds the same path; a key naming any other folder,
/// or with no mod stored, is stale and gives none.
///
/// @param values the preferences
/// @return the picked folder's path; empty for none
[[nodiscard]] std::string remembered_picked_folder(const oa::platform::preferences::Values& values);

/// Returns the language the preferences choose: the language key's value
/// when it is oa::data::languages::system_choice or the tag of a language
/// this build draws, matched without regard to case. A language that is
/// not installed yet is kept too, when this build can draw it, so a choice
/// of one stays until the pack is there. Anything else, a file without the
/// key included, is the default. "pt" is not the tag "pt-BR".
///
/// @param values the preferences
/// @param players_own_profile the preferences file is the player's own
///     (Inputs::players_own_profile)
/// @return system_choice or a known tag, as the registry writes it; the
///     default is system_choice with the player's own file and "en" with a
///     named one
[[nodiscard]] std::string
stored_language(const oa::platform::preferences::Values& values, bool players_own_profile);

/// Returns how game text is drawn under the settings.
///
/// @param settings the settings in effect
/// @return modern fonts, their outline, shadow, background and size as the
///     settings' Language section sets them
[[nodiscard]] constexpr oa::present::TextStyle text_style(const EngineSettings& settings) noexcept {
    return {
        .modern_fonts = settings.modern_fonts,
        .outline = settings.text_outline,
        .shadow = settings.text_shadow,
        .background = settings.text_background,
        .size = settings.text_size,
    };
}

/// Returns the text the preferences keep a screen size as.
///
/// @param size the screen size
/// @return "desktop" for desktop_screen_size, else "WIDTHxHEIGHT" in decimal
[[nodiscard]] std::string screen_size_text(ScreenSize size);

/// Returns the screen size a preferences text names.
///
/// @param text the stored text
/// @return the size, when the text is "desktop" or "WIDTHxHEIGHT" as
///     screen_size_text writes it: whole decimal numbers with no sign or
///     leading zero joined by a lower-case "x", at least smallest_screen_size
///     and neither side above longest_screen_side; nothing otherwise
[[nodiscard]] std::optional<ScreenSize> screen_size_from_text(std::string_view text);

/// Returns the word the preferences and the command line keep a level of
/// hardware acceleration as.
///
/// @param level the level
/// @return "off", "basic" or "full"
[[nodiscard]] std::string_view hardware_acceleration_text(HardwareAcceleration level) noexcept;

/// Returns the level of hardware acceleration a word names.
///
/// @param text the word, in lower case as hardware_acceleration_text gives it
/// @return the level; nothing for any other text
[[nodiscard]] std::optional<HardwareAcceleration>
hardware_acceleration_from_text(std::string_view text) noexcept;

/// Returns the word the preferences keep a way of Menu scaling as.
///
/// @param scaling the way
/// @return "sharp", "whole-steps" or "unfiltered"
[[nodiscard]] std::string_view menu_scaling_text(MenuScaling scaling) noexcept;

/// Returns the way of Menu scaling a word names.
///
/// @param text the word, in lower case as menu_scaling_text gives it
/// @return the way; nothing for any other text
[[nodiscard]] std::optional<MenuScaling> menu_scaling_from_text(std::string_view text) noexcept;

/// Returns the word the preferences keep an Interface size as.
///
/// @param size the Interface size
/// @return "automatic", "100", "150", "200", "300" or "400"
[[nodiscard]] std::string_view interface_size_text(InterfaceSize size) noexcept;

/// Returns the Interface size a word names.
///
/// @param text the word, as interface_size_text gives it
/// @return the size; nothing for any other text, "0" among them
[[nodiscard]] std::optional<InterfaceSize> interface_size_from_text(std::string_view text) noexcept;

/// Returns the word the preferences keep a way of One-finger drag as.
///
/// @param drag the way
/// @return "automatic", "box" or "scroll"
[[nodiscard]] std::string_view touch_drag_text(TouchDrag drag) noexcept;

/// Returns the way of One-finger drag a word names.
///
/// @param text the word, in lower case as touch_drag_text gives it
/// @return the way; nothing for any other text
[[nodiscard]] std::optional<TouchDrag> touch_drag_from_text(std::string_view text) noexcept;

/// Returns the word the preferences keep a way of QUEUE and ADD as.
///
/// @param latches the way
/// @return "stay-on" or "one-action"
[[nodiscard]] std::string_view touch_latches_text(TouchLatches latches) noexcept;

/// Returns the way of QUEUE and ADD a word names.
///
/// @param text the word, in lower case as touch_latches_text gives it
/// @return the way; nothing for any other text
[[nodiscard]] std::optional<TouchLatches> touch_latches_from_text(std::string_view text) noexcept;

/// Returns the unit limit an installation's totala.ini sets.
///
/// Reads [Preferences] UnitLimit, the section and key matched without
/// regard to case, from the first [Preferences] section; the first
/// UnitLimit line there counts. The value is its leading whole number, 0 when it has none or is negative,
/// clamped to the game's range, 3.1c's own 20 to 500 by default. Lines may end in CR LF or LF;
/// a line starting with ';' is a comment. Only the first
/// installation_ini_limit bytes are read.
///
/// @param ini_text the file's text
/// @param units the unit limits the game allows; the value is clamped to
///     their minimum and maximum
/// @return the limit in units per player; nothing when the file sets none
[[nodiscard]] std::optional<uint16_t> installation_unit_limit(
    std::string_view ini_text, const oa::data::limits::UnitsPerPlayer& units = {}
);

/// Returns the Pathfinding cycles a path credit shows as.
///
/// @param nodes path nodes a game tick
/// @return nodes over base_path_search_nodes, to the nearest whole number,
///     from 1 to highest_path_search_multiplier
[[nodiscard]] int32_t path_search_multiplier(int32_t nodes) noexcept;

/// Returns the path credit a match plays at.
///
/// The setting's Pathfinding cycles multiply the game's own credit, which a
/// mod's limits may raise (data::limits::PathSearch::nodes).
///
/// @param settings the settings in effect
/// @param shared_or_replay the match is played with other machines or
///     replays a recording
/// @param game_nodes the game's own path nodes a tick: base_path_search_nodes
///     for 3.1c
/// @return game_nodes in a shared game or a replay, else the setting's
///     path_search_nodes scaled by game_nodes / base_path_search_nodes,
///     truncated and held to data::limits::highest_path_search_nodes
[[nodiscard]] int32_t match_path_search_nodes(
    const EngineSettings& settings,
    bool shared_or_replay,
    int32_t game_nodes = base_path_search_nodes
) noexcept;

/// Why a setting cannot be changed now.
enum class Lock : uint8_t {
    none,        ///< it can be changed
    in_game,     ///< a game is running; it can be changed outside a game
    set_by_host, ///< a shared game or a replay decides it
    /// --max-fps decides the frame rate, or --hardware-acceleration, in any
    /// of its forms, or --no-hardware-acceleration decides hardware
    /// acceleration, for this run
    command_line,
    unavailable, ///< nothing in the game could make the setting help this run
    set_by_mod,  ///< the mod allows no other value
    /// The modern fonts are off: the game's own fonts, which draw game text
    /// then, have fixed sizes.
    needs_modern_fonts,
    /// The platform opens every window at the display's own pixel density,
    /// so the setting is always on.
    always_on,
    /// The language shown needs it: its text draws in the modern fonts, or
    /// its pack asks for multiplayer chat in UTF-8. The setting shows On.
    set_by_language,
    /// Zoomed out units is Rendered: units are drawn the same at every
    /// zoom, so After zoom changes nothing.
    needs_dots,
    /// Window edge is Capture or Free: only a sticky edge has a firmness.
    needs_sticky,
    /// The welcome setup is walked through again only from the main menu.
    main_menu,
};

/// The game the dialog opens over.
struct GameState {
    bool in_game{};                      ///< a match is running
    bool shared_game{};                  ///< the match is played with other machines
    bool replay{};                       ///< the match replays a recording
    bool frame_rate_from_command_line{}; ///< --max-fps was given
    /// --hardware-acceleration, in any of its forms, or
    /// --no-hardware-acceleration was given.
    bool renderer_from_command_line{};
    /// Nothing in the game could have the graphics card scale this run's
    /// frames: the environment names a render driver, the renderer cannot,
    /// or the machine has under 2 GiB of memory.
    bool acceleration_unavailable{};
    /// The renderer cannot wait for the display, as far as the game can tell,
    /// or cannot change it without resetting its device.
    bool vertical_sync_unavailable{};
    /// 3.1c's command line names the language, which then decides it for the
    /// run.
    bool language_from_command_line{};
    /// The platform opens every window at the display's own pixel density
    /// (Inputs::native_density_windows).
    bool native_density_windows{};
    /// --native-density was given, which opened this run's window at the
    /// display's own pixel density.
    bool native_density_from_command_line{};
};

/// What the dialog cannot change, and what its header says of the game.
struct Locks {
    Lock path_search{};           ///< Pathfinding cycles
    Lock unit_limit{};            ///< Unit limit
    Lock skirmish_players{};      ///< Skirmish players
    Lock max_frame_rate{};        ///< Maximum frame rate
    bool shared_game{};           ///< the header says the shared game is still running
    Lock hardware_acceleration{}; ///< Hardware acceleration
    Lock vertical_sync{};         ///< Vertical sync
    Lock mex_snap{};              ///< the mod options' Mex snap radius
    Lock wreck_snap{};            ///< the mod options' Wreck snap radius
    /// Text size: the dialog itself locks it needs_modern_fonts while it
    /// shows Use modern fonts for game text Off.
    Lock text_size{};
    Lock language{}; ///< Language
    /// Use modern fonts for game text: the dialog itself locks it
    /// set_by_language while the language it shows draws in them.
    Lock modern_fonts{};
    /// Enable Unicode Multiplayer Chat: the dialog itself locks it
    /// set_by_language while a pack of the language it shows asks for it.
    Lock unicode_chat{};
    /// Native pixel density: locked command_line, the stored choice stays
    /// shown and takes effect from a start without the flag.
    Lock native_density{};
    /// After zoom: the dialog itself locks it needs_dots while it shows
    /// Zoomed out units Rendered.
    Lock zoomed_out_after{};
    /// Edge stickiness: the dialog itself locks it needs_sticky while it
    /// shows Window edge Capture or Free.
    Lock edge_stickiness{};
    /// Welcome setup: locked while a game is running. SHOW AGAIN is chosen
    /// from the main menu.
    Lock welcome_setup{};
};

/// Returns the locks a game state puts on the settings.
///
/// Pathfinding cycles, Unit limit and Skirmish players are locked during any
/// game, as set_by_host in a shared game or a replay; the maximum frame rate is
/// locked while --max-fps decides it. Hardware acceleration is locked
/// command_line while a flag decides it, else unavailable when nothing in
/// the game could help, and never by a game, so that it can always be set
/// to Off. Vertical sync is locked unavailable where the renderer
/// cannot wait for the display, else in_game in a shared game or a replay,
/// where its value holds until the match ends. The language is locked
/// command_line while 3.1c's command line names one, and never by a game:
/// it changes only what players read. Native pixel density is locked always_on where
/// the platform opens every window at native density, else command_line
/// while --native-density decides it, and never by a game: it takes effect
/// from the next start. No game locks Menu scaling or Interface size.
///
/// @param state the game the dialog opens over
/// @return the locks
[[nodiscard]] Locks settings_locks(const GameState& state) noexcept;

} // namespace oa::ui::engine_settings

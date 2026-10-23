// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The settings dialog's rows as one table, in Setting's order: each row's
// spec and the small functions it is bound through, the sections' rows,
// names and headings, and the geometry functions that read a row.

#include "settings_rows.hpp"

#include "stored_words.hpp"

#include "geometry.hpp"
#include "language_page.hpp"

#include "oa/data/languages.hpp"
#include "oa/platform/display_modes.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/pad_controls.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace oa::ui::engine_settings {

namespace geometry {

namespace {

namespace pad_controls = oa::ui::pad_controls;
namespace languages = oa::data::languages;

using Spec = kit::RowSpec<SettingsModel>;
using Stepper = kit::Stepper<SettingsModel>;
using Hint = std::array<std::string_view, 2>;

/// The number of settings: Setting's enumerators.
constexpr std::size_t kSettingCount = static_cast<std::size_t>(Setting::fog_of_war) + 1;

// ---------------------------------------------------------------------------
// The sections' rows.

/// Controls' rows: the zoom's limits right under the switch for the wheel,
/// how far past the map's edges the view goes under them, middle-click
/// panning, whether a window keeps the pointer, and how firmly a sticky
/// edge holds right under that.
constexpr std::array<Setting, 8> kControlsRows{
    Setting::wheel_zoom,
    Setting::max_zoom_out,
    Setting::max_zoom_in,
    Setting::view_past_map_edge,
    Setting::middle_click_pan,
    Setting::switch_alt,
    Setting::window_edge,
    Setting::window_edge_stickiness,
};
/// Engine Limits' rows: the unit limit, pathfinding, then how many player
/// rows the skirmish setup shows.
constexpr std::array<Setting, 3> kEngineLimitsRows{
    Setting::unit_limit,
    Setting::path_search,
    Setting::skirmish_players,
};
/// Graphics' rows: how the frames are drawn, Smooth motion right under
/// Vertical sync, the fog's edges under the explosions' flash, then how
/// units look zoomed out, and the zoom they look so from right under the
/// way.
constexpr std::array<Setting, 11> kGraphicsRows{
    Setting::max_frame_rate,
    Setting::anti_aliasing,
    Setting::screen_size,
    Setting::hardware_acceleration,
    Setting::vertical_sync,
    Setting::smooth_motion,
    Setting::native_density,
    Setting::explosion_flash,
    Setting::fog_of_war,
    Setting::zoomed_out_units,
    Setting::zoomed_out_after,
};
/// User Interface's rows: how the menus scale, the window's frame, the
/// HUD's scaling, the size Open Annihilation's own screens are drawn at,
/// then whether Escape opens the game menu.
constexpr std::array<Setting, 5> kUserInterfaceRows{
    Setting::menu_scaling,
    Setting::window_frame,
    Setting::hud_scaling,
    Setting::interface_size,
    Setting::escape_opens_menu,
};
/// Language's rows: the language first, and the text size right
/// under the switch it needs.
constexpr std::array<Setting, 7> kLanguageRows{
    Setting::language,
    Setting::modern_fonts,
    Setting::text_size,
    Setting::text_outline,
    Setting::text_shadow,
    Setting::text_background,
    Setting::unicode_chat,
};
/// Language's rows on the main menu's dialog, where the Library opens over
/// it (Dialog::library_link): MORE IN THE LIBRARY… right under the
/// language's row and the packs its list offers to download.
constexpr std::array<Setting, 8> kLanguageLibraryRows{
    Setting::language,
    Setting::more_in_library,
    Setting::modern_fonts,
    Setting::text_size,
    Setting::text_outline,
    Setting::text_shadow,
    Setting::text_background,
    Setting::unicode_chat,
};
/// Touch's rows: how a finger's drag and hold work, then the latches, the
/// haptics, the side the controls stand on and their size.
constexpr std::array<Setting, 6> kTouchRows{
    Setting::touch_drag,
    Setting::touch_hold_delay,
    Setting::touch_latches,
    Setting::touch_haptics,
    Setting::touch_left_handed,
    Setting::touch_control_size,
};
/// Controller's rows: the scheme, the right trackpad's pointer, the right
/// stick and the gyro, what the pad feels and shows, the mirror, then the
/// rows it shares with Touch.
constexpr std::array<Setting, 15> kControllerRows{
    Setting::pad_scheme,
    Setting::pad_right_trackpad,
    Setting::pad_pointer_speed,
    Setting::pad_acceleration,
    Setting::pad_glide,
    Setting::pad_right_stick,
    Setting::pad_magnetism,
    Setting::pad_gyro,
    Setting::pad_gyro_speed,
    Setting::pad_haptics,
    Setting::pad_prompts,
    Setting::pad_left_handed,
    Setting::touch_control_size,
    Setting::touch_hold_delay,
    Setting::touch_latches,
};
/// Controller's rows under the Steam Input notice, while the gamepad
/// reaches the game through Steam Input.
constexpr std::array<Setting, kControllerRows.size() + 1> kSteamInputControllerRows = [] {
    std::array<Setting, kControllerRows.size() + 1> rows{};
    rows[0] = Setting::pad_steam_input_notice;
    for (std::size_t index = 0; index < kControllerRows.size(); ++index)
        rows[index + 1] = kControllerRows[index];
    return rows;
}();
/// Game files' rows: what is installed with its MANAGE… button, the
/// backups switch, and where the files are.
constexpr std::array<Setting, 3> kGameFilesRows{
    Setting::game_files_summary,
    Setting::game_files_backed_up,
    Setting::game_files_location,
};
/// Developer's rows, over its list: the gallery after the statistics.
constexpr std::array<Setting, developer_row_count> kDeveloperRows{
    Setting::developer_mode,
    Setting::frame_stats,
    Setting::kit_gallery,
};
/// The mod's keys.
constexpr std::array<Setting, 3> kModKeysRows{
    Setting::snap_override_key,
    Setting::autoclick_key,
    Setting::rotate_build_key,
};
/// Patrolling builders.
constexpr std::array<Setting, 3> kModPatrolRows{
    Setting::patrol_hold,
    Setting::patrol_maneuver,
    Setting::patrol_roam,
};
/// Guarding builders.
constexpr std::array<Setting, 3> kModGuardRows{
    Setting::guard_hold,
    Setting::guard_maneuver,
    Setting::guard_roam,
};
/// The build tools and the mex snap.
constexpr std::array<Setting, 3> kModToolsRows{
    Setting::optimize_dt_rows,
    Setting::full_rings,
    Setting::mex_snap_radius,
};
/// The wreck snap, the chat and the resource bar.
constexpr std::array<Setting, 3> kModChatRows{
    Setting::wreck_snap_radius,
    Setting::chat_backdrop,
    Setting::panel_background,
};
/// Downloads builds its rows from the host's offer, not from settings.
constexpr std::array<Setting, 0> kDownloadsRows{};
/// System where the game registers file types at run time: Your files, then
/// Welcome setup, then the group's heading and one switch for each type.
constexpr std::array<Setting, 10> kSystemRuntimeRows{
    Setting::user_folder,
    Setting::welcome_setup,
    Setting::file_type_registration,
    Setting::file_type_oamod,
    Setting::file_type_oamap,
    Setting::file_type_oalang,
    Setting::file_type_oareg,
    Setting::file_type_oamut,
    Setting::file_type_oabrain,
    Setting::file_type_oalaunch,
};
/// System where the app's bundle declares the types: Your files, then
/// Welcome setup, then the text row in place of the switches.
constexpr std::array<Setting, 3> kSystemBundleRows{
    Setting::user_folder,
    Setting::welcome_setup,
    Setting::file_types_note,
};
/// System's rows for this system. A dialog may show the other layout
/// (Dialog::file_type_switches); section_settings reads that.
constexpr std::span<const Setting> kSystemRows = file_types_switchable
                                                     ? std::span<const Setting>{kSystemRuntimeRows}
                                                     : std::span<const Setting>{kSystemBundleRows};

/// What each section shows, in Page's order.
constexpr std::array<std::span<const Setting>, page_count> kPageRows{
    kControlsRows,
    kEngineLimitsRows,
    kLanguageRows,
    kGraphicsRows,
    kUserInterfaceRows,
    kTouchRows,
    kControllerRows,
    kDeveloperRows,
    kGameFilesRows,
    kDownloadsRows,
    kSystemRows,
    {},
    kModKeysRows,
    kModPatrolRows,
    kModGuardRows,
    kModToolsRows,
    kModChatRows,
    {},
};
/// Each section's name, as its list entry shows it, in Page's order.
constexpr std::array<std::string_view, page_count> kPageNames{
    "Controls",
    "Engine Limits",
    "Language & Text",
    "Graphics",
    "User Interface",
    "Touch",
    "Controller",
    "Developer",
    "Game files",
    "Downloads",
    "System",
    "Game Content",
    "Keys",
    "Patrolling",
    "Guarding",
    "Build tools",
    "Snap & chat",
    "",
};
/// Each section's heading, in capitals, in Page's order.
constexpr std::array<std::string_view, page_count> kPageHeadings{
    "CONTROLS",
    "ENGINE LIMITS",
    "LANGUAGE & TEXT",
    "GRAPHICS",
    "USER INTERFACE",
    "TOUCH",
    "CONTROLLER",
    "DEVELOPER",
    "GAME FILES",
    "DOWNLOADS",
    "SYSTEM",
    "GAME CONTENT",
    "MOD KEYS",
    "PATROLLING BUILDERS",
    "GUARDING BUILDERS",
    "BUILD TOOLS",
    "SNAP & CHAT",
    "",
};
/// Each section's automation word, in kebab case, in Page's order.
/// Engine Limits' word is engine-limits; its enumerator stays common_tweaks.
constexpr std::array<std::string_view, page_count> kPageWords{
    "controls",
    "engine-limits",
    "language",
    "graphics",
    "user-interface",
    "touch",
    "controller",
    "developer",
    "game-files",
    "downloads",
    "system",
    "game-content",
    "mod-keys",
    "mod-patrol",
    "mod-guard",
    "mod-tools",
    "mod-chat",
    "registered",
};
static_assert(
    static_cast<std::size_t>(Page::registered) + 1 == page_count,
    "every section has its rows, its name, its heading and its word"
);

// ---------------------------------------------------------------------------
// What the rows offer.

/// Hardware acceleration's captions, in hardware_acceleration_levels' order.
constexpr std::array<std::string_view, 3> kAccelerationCaptions{"Off", "Basic", "Full"};
static_assert(
    kAccelerationCaptions.size() == hardware_acceleration_levels.size(),
    "every level of hardware acceleration has its caption"
);
/// Enhanced anti-aliasing's captions, in anti_aliasing_levels' order.
constexpr std::array<std::string_view, 5> kAntiAliasingCaptions{"Off", "2x", "4x", "8x", "16x"};
static_assert(
    kAntiAliasingCaptions.size() == anti_aliasing_levels.size(),
    "every level of anti-aliasing has its caption"
);
/// Menu scaling's captions, in menu_scaling_choices' order.
constexpr std::array<std::string_view, 3> kMenuScalingCaptions{
    "Sharp", "Whole steps", "Unfiltered"
};
static_assert(
    kMenuScalingCaptions.size() == menu_scaling_choices.size(),
    "every way of Menu scaling has its caption"
);
/// Explosion flash's captions, in explosion_flash_choices' order.
constexpr std::array<std::string_view, 3> kExplosionFlashCaptions{"Off", "Reduced", "Full"};
static_assert(
    kExplosionFlashCaptions.size() == explosion_flash_choices.size(),
    "every level of Explosion flash has its caption"
);
/// View past the map's edge's captions, in view_past_map_edge_choices' order.
constexpr std::array<std::string_view, 3> kViewPastMapEdgeCaptions{"Off", "25%", "50%"};
static_assert(
    kViewPastMapEdgeCaptions.size() == view_past_map_edge_choices.size(),
    "every choice of View past the map's edge has its caption"
);
/// Fog of War's captions, in fog_of_war_choices' order.
constexpr std::array<std::string_view, 2> kFogOfWarCaptions{"Classic", "Smooth"};
static_assert(
    kFogOfWarCaptions.size() == fog_of_war_choices.size(), "every way of Fog of War has its caption"
);
/// Zoomed out units' captions, in zoomed_out_units_choices' order.
constexpr std::array<std::string_view, 3> kZoomedOutUnitsCaptions{"Rendered", "Dots", "Icons"};
static_assert(
    kZoomedOutUnitsCaptions.size() == zoomed_out_units_choices.size(),
    "every way of Zoomed out units has its caption"
);
/// Window edge's captions, in window_edge_choices' order.
constexpr std::array<std::string_view, 3> kWindowEdgeCaptions{"Capture", "Sticky", "Free"};
static_assert(
    kWindowEdgeCaptions.size() == window_edge_choices.size(),
    "every way of Window edge has its caption"
);
/// Window frame's captions, in window_frame_choices' order.
constexpr std::array<std::string_view, 2> kWindowFrameCaptions{"Hidden in play", "Always shown"};
static_assert(
    kWindowFrameCaptions.size() == window_frame_choices.size(),
    "every way of Window frame has its caption"
);
/// Interface size's choices' texts, in interface_size_choices' order.
constexpr std::array<std::string_view, 6> kInterfaceSizeCaptions{
    "Auto", "100%", "150%", "200%", "300%", "400%"
};
static_assert(
    kInterfaceSizeCaptions.size() == interface_size_choices.size(),
    "every Interface size has its caption"
);
/// One-finger drag's captions, in touch_drag_choices' order.
constexpr std::array<std::string_view, 3> kTouchDragCaptions{"Automatic", "Box", "Scroll"};
static_assert(
    kTouchDragCaptions.size() == touch_drag_choices.size(),
    "every way of One-finger drag has its caption"
);
/// QUEUE and ADD's captions, in touch_latches_choices' order.
constexpr std::array<std::string_view, 2> kTouchLatchesCaptions{"Stay on", "One action"};
static_assert(
    kTouchLatchesCaptions.size() == touch_latches_choices.size(),
    "every way of QUEUE and ADD has its caption"
);
/// Control size's captions, in control_size_choices' order.
constexpr std::array<std::string_view, 3> kControlSizeCaptions{"Standard", "Large", "Larger"};
static_assert(
    kControlSizeCaptions.size() == control_size_choices.size(), "every Control size has its caption"
);
/// The schemes, in the order Scheme's strip offers them.
constexpr std::array<pad_controls::Scheme, 2> kSchemeChoices{
    pad_controls::Scheme::trackpads, pad_controls::Scheme::sticks
};
/// Scheme's captions, in kSchemeChoices' order.
constexpr std::array<std::string_view, 2> kSchemeCaptions{"Trackpads", "Sticks"};
/// The right trackpad's ways, in the order Right trackpad's strip offers them.
constexpr std::array<pad_controls::RightTrackpad, 2> kRightTrackpadChoices{
    pad_controls::RightTrackpad::relative, pad_controls::RightTrackpad::absolute
};
/// Right trackpad's captions, in kRightTrackpadChoices' order: the pointer,
/// relative and absolute.
constexpr std::array<std::string_view, 2> kRightTrackpadCaptions{"Relative", "Absolute"};
/// The accelerations, in the order Pointer acceleration's strip offers them.
constexpr std::array<pad_controls::Acceleration, 3> kAccelerationChoices{
    pad_controls::Acceleration::off,
    pad_controls::Acceleration::low,
    pad_controls::Acceleration::high
};
/// Pointer acceleration's captions, in kAccelerationChoices' order.
constexpr std::array<std::string_view, 3> kPadAccelerationCaptions{"Off", "Low", "High"};
/// The right stick's roles, in the order Right stick's strip offers them.
constexpr std::array<pad_controls::RightStick, 3> kRightStickChoices{
    pad_controls::RightStick::zoom_and_pages,
    pad_controls::RightStick::pointer,
    pad_controls::RightStick::nothing
};
/// Right stick's captions, in kRightStickChoices' order: Zoom is zoom and
/// build pages, which its hint says in full.
constexpr std::array<std::string_view, 3> kRightStickCaptions{"Zoom", "Pointer", "Nothing"};
/// The gyro's ways, in the order Gyro pointer's drop-down offers them.
constexpr std::array<pad_controls::Gyro, 4> kGyroChoices{
    pad_controls::Gyro::off,
    pad_controls::Gyro::right_pad_touched,
    pad_controls::Gyro::right_stick_touched,
    pad_controls::Gyro::always
};
/// Gyro pointer's choices' texts, in kGyroChoices' order.
constexpr std::array<std::string_view, 4> kGyroCaptions{
    "Off", "While the right pad is touched", "While the right stick is touched", "Always"
};
/// The haptics' strengths, in the order Haptics' strip offers them.
constexpr std::array<pad_controls::Haptics, 3> kHapticsChoices{
    pad_controls::Haptics::off, pad_controls::Haptics::light, pad_controls::Haptics::strong
};
/// The Controller section's Haptics' captions, in kHapticsChoices' order.
constexpr std::array<std::string_view, 3> kPadHapticsCaptions{"Off", "Light", "Strong"};
/// The button prompts, in the order Button prompts' drop-down offers them.
constexpr std::array<pad_controls::Prompts, 6> kPromptsChoices{
    pad_controls::Prompts::automatic,
    pad_controls::Prompts::steam_deck,
    pad_controls::Prompts::xbox,
    pad_controls::Prompts::playstation,
    pad_controls::Prompts::nintendo,
    pad_controls::Prompts::off
};
/// Button prompts' choices' texts, in kPromptsChoices' order.
constexpr std::array<std::string_view, 6> kPromptsCaptions{
    "Automatic", "Steam Deck", "Xbox", "PlayStation", "Nintendo", "Off"
};
/// Maximum zoom out's choices' texts, in zoom_out_limits' order.
constexpr std::array<std::string_view, 7> kZoomOutCaptions{
    "Automatic", "Whole map", "1/32", "1/16", "1/8", "1/4", "1/2"
};
static_assert(
    kZoomOutCaptions.size() == zoom_out_limits.size(),
    "every Maximum zoom out choice has its caption"
);
/// After zoom's choices' texts, in zoomed_out_afters' order.
constexpr std::array<std::string_view, 7> kZoomedOutAfterCaptions{
    "1/2", "1/3", "1/4", "1/6", "1/8", "1/12", "1/16"
};
static_assert(
    kZoomedOutAfterCaptions.size() == zoomed_out_afters.size(),
    "every After zoom choice has its caption"
);
/// Maximum zoom in's choices' texts, in zoom_in_limits' order.
constexpr std::array<std::string_view, 4> kZoomInCaptions{"None", "2x", "3x", "4x"};
static_assert(
    kZoomInCaptions.size() == zoom_in_limits.size(), "every Maximum zoom in choice has its caption"
);
/// What patrolling builders do at each stop of their sliders.
constexpr std::array<std::string_view, 3> kPatrolCaptions{"Reclaim only", "Both", "Assist only"};
/// What guarding builders do at each stop of their sliders.
constexpr std::array<std::string_view, 3> kGuardCaptions{"Stay", "Normal", "Scatter"};
/// What the resource bar shows behind its text at each stop of its slider.
constexpr std::array<std::string_view, 3> kPanelBackgroundCaptions{"None", "Text", "Solid"};

// ---------------------------------------------------------------------------
// Steps, stops and captions.

/// Returns a choice's place among the choices a strip or a drop-down offers.
///
/// @param choices the choices, in order
/// @param chosen the choice
/// @return its index; 0 for a choice not offered
template <typename Choice, std::size_t Count>
int32_t choice_place(const std::array<Choice, Count>& choices, Choice chosen) noexcept {
    const auto found = std::find(choices.begin(), choices.end(), chosen);
    return found == choices.end() ? 0 : static_cast<int32_t>(found - choices.begin());
}

/// Returns a stop held to a slider's stops.
///
/// @param stop the stop
/// @param stops the slider's stops
/// @return the stop, 0 to stops - 1
int32_t on_stops(int32_t stop, int32_t stops) noexcept {
    return std::clamp(stop, int32_t{0}, std::max(stops - 1, int32_t{0}));
}

/// Returns a whole number of a range's steps, rounded to the nearest.
///
/// @param value the value
/// @param lowest the range's lowest value
/// @param step the step
/// @return (value - lowest) / step, rounded half up
int32_t steps_from(int64_t value, int64_t lowest, int64_t step) noexcept {
    return static_cast<int32_t>((value - lowest + step / 2) / step);
}

/// Returns the word the preferences keep a choice as, from the setting's
/// table of words (stored_words.hpp).
template <const auto& Words, class Choice>
std::string table_word(Choice choice) {
    return std::string(word_of(Words, choice));
}

/// Returns the word the preferences keep a choice as, from the setting's
/// text function.
template <auto Text, class Choice>
std::string text_word(Choice choice) {
    return std::string(Text(choice));
}

/// Returns the word the preferences keep a level of anti-aliasing as: its
/// factor, Off's 1.
std::string anti_aliasing_word(AntiAliasing level) {
    return std::to_string(static_cast<int>(level));
}

/// Returns the word of a way of Zoomed out units: the word the preferences
/// keep it as, or for Icons, which they keep none of yet, its own name.
std::string zoomed_out_units_word(ZoomedOutUnits units) {
    const std::string_view stored = word_of(zoomed_out_units_words, units);
    return std::string(stored.empty() ? std::string_view("icons") : stored);
}

/// Returns the text of a slider's stop: the value's own text at the stop it
/// is on, so that a value between two stops shows as it is, and the text of
/// the stop's value at any other.
template <
    int32_t (*Get)(const SettingsModel&),
    void (*Set)(SettingsModel&, int32_t),
    std::string (*Text)(const SettingsModel&)>
std::string stop_caption(const SettingsModel& model, int32_t stop) {
    if (stop == Get(model))
        return Text(model);
    EngineSettings moved = *model.settings;
    SettingsModel probe{&moved, model.dialog};
    Set(probe, stop);
    return Text(probe);
}

/// A slider over a number of the settings, from its lowest in equal steps.
template <
    class Number,
    Number EngineSettings::* Field,
    int64_t Lowest,
    int64_t Highest,
    int64_t Step>
struct Linear {
    /// Returns its stops.
    static int32_t count(const SettingsModel&) {
        return static_cast<int32_t>((Highest - Lowest) / Step + 1);
    }

    /// Returns the stop nearest its value.
    static int32_t get(const SettingsModel& model) {
        return on_stops(steps_from(model.settings->*Field, Lowest, Step), count(model));
    }

    /// Sets it to a stop's value.
    static void set(SettingsModel& model, int32_t stop) {
        model.settings->*Field = static_cast<Number>(Lowest + on_stops(stop, count(model)) * Step);
    }
};

/// A strip of levels over a choice of the settings, each level named by the
/// word the preferences keep its choice as. A level past those it offers is
/// shown, and choosing it changes nothing.
template <
    class Choice,
    std::size_t Count,
    const std::array<Choice, Count>& Choices,
    Choice EngineSettings::* Field,
    const std::array<std::string_view, Count>& Captions,
    std::string (*Word)(Choice),
    std::size_t Offered = Count>
struct LevelStrip {
    /// Returns its levels.
    static int32_t count(const SettingsModel&) { return static_cast<int32_t>(Count); }

    /// Returns the level it shows.
    static int32_t get(const SettingsModel& model) {
        return choice_place(Choices, model.settings->*Field);
    }

    /// Sets it to a level it offers.
    static void set(SettingsModel& model, int32_t level) {
        if (level < 0 || static_cast<std::size_t>(level) >= Offered)
            return;
        model.settings->*Field = Choices[static_cast<std::size_t>(level)];
    }

    /// Returns a level's caption, in English.
    static std::string caption(const SettingsModel&, int32_t level) {
        if (level < 0 || static_cast<std::size_t>(level) >= Count)
            return {};
        return std::string(Captions[static_cast<std::size_t>(level)]);
    }

    /// Returns the levels a player may choose, from the left.
    static int32_t offered(const SettingsModel&) { return static_cast<int32_t>(Offered); }

    /// Returns a level's id: its choice's stored word, in name form.
    static std::string id(const SettingsModel&, int32_t level) {
        if (level < 0 || static_cast<std::size_t>(level) >= Count)
            return {};
        return word_form(Word(Choices[static_cast<std::size_t>(level)]), false);
    }

    /// Returns its stepper.
    static constexpr Stepper stepper() noexcept {
        return {&count, &get, &set, &caption, Offered == Count ? nullptr : &offered, &id};
    }
};

/// A drop-down over a choice of the settings, each item named by the word
/// the preferences keep its choice as.
template <
    class Choice,
    std::size_t Count,
    const std::array<Choice, Count>& Choices,
    Choice EngineSettings::* Field,
    const std::array<std::string_view, Count>& Captions,
    std::string (*Word)(Choice)>
struct DropDown {
    /// Returns its items.
    static int32_t count(const SettingsModel&) { return static_cast<int32_t>(Count); }

    /// Returns the item it shows.
    static int32_t get(const SettingsModel& model) {
        return choice_place(Choices, model.settings->*Field);
    }

    /// Sets it to an item, held to its items.
    static void set(SettingsModel& model, int32_t item) {
        model.settings->*Field = Choices[static_cast<std::size_t>(on_stops(item, count(model)))];
    }

    /// Returns an item's text, as the dialog shows it.
    static std::string caption(const SettingsModel&, int32_t item) {
        if (item < 0 || static_cast<std::size_t>(item) >= Count)
            return {};
        return std::string(shown_text(Captions[static_cast<std::size_t>(item)]));
    }

    /// Returns an item's id: its choice's stored word, in name form.
    static std::string id(const SettingsModel&, int32_t item) {
        if (item < 0 || static_cast<std::size_t>(item) >= Count)
            return {};
        return word_form(Word(Choices[static_cast<std::size_t>(item)]), false);
    }

    /// Returns its stepper.
    static constexpr Stepper stepper() noexcept {
        return {&count, &get, &set, &caption, nullptr, &id};
    }
};

/// Returns whether a switch of the settings is On.
template <bool EngineSettings::* Flag>
bool engine_flag(const SettingsModel& model) {
    return model.settings->*Flag;
}

/// Sets a switch of the settings.
template <bool EngineSettings::* Flag>
void set_engine_flag(SettingsModel& model, bool on) {
    model.settings->*Flag = on;
}

/// Returns whether a switch of the mod options is On.
template <bool ModOptions::* Flag>
bool option_flag(const SettingsModel& model) {
    return model.settings->mod_options.*Flag;
}

/// Sets a switch of the mod options.
template <bool ModOptions::* Flag>
void set_option_flag(SettingsModel& model, bool on) {
    model.settings->mod_options.*Flag = on;
}

/// Makes the row of a switch of the settings.
template <bool EngineSettings::* Flag>
constexpr Spec engine_switch(std::string_view id, std::string_view label, Hint hint) noexcept {
    return kit::toggle(id, label, &engine_flag<Flag>, &set_engine_flag<Flag>, hint);
}

/// Makes the row of a switch of the mod options.
template <bool ModOptions::* Flag>
constexpr Spec option_switch(std::string_view id, std::string_view label, Hint hint) noexcept {
    return kit::toggle(id, label, &option_flag<Flag>, &set_option_flag<Flag>, hint);
}

/// Copies one field of the settings.
template <auto Field>
void copy_field(const SettingsModel& to, const SettingsModel& from) {
    to.settings->*Field = from.settings->*Field;
}

/// Copies one field of the mod options.
template <auto Field>
void copy_option(const SettingsModel& to, const SettingsModel& from) {
    to.settings->mod_options.*Field = from.settings->mod_options.*Field;
}

/// Returns a fixed number of hint lines.
template <std::size_t Lines>
std::size_t lines(const SettingsModel&) {
    return Lines;
}

// Pathfinding cycles: base_path_search_nodes times its stop and one.

/// Returns Pathfinding cycles' stops: one for each multiplier.
int32_t path_search_stops(const SettingsModel&) {
    return highest_path_search_multiplier;
}

/// Returns the stop of the multiplier nearest the path nodes.
int32_t path_search_stop(const SettingsModel& model) {
    return on_stops(
        path_search_multiplier(model.settings->path_search_nodes) - 1, path_search_stops(model)
    );
}

/// Sets the path nodes to a stop's multiplier of base_path_search_nodes.
void set_path_search_stop(SettingsModel& model, int32_t stop) {
    model.settings->path_search_nodes =
        base_path_search_nodes * (on_stops(stop, path_search_stops(model)) + 1);
}

/// Returns the multiplier, as "4x".
std::string path_search_text(const SettingsModel& model) {
    return std::to_string(path_search_multiplier(model.settings->path_search_nodes)) + "x";
}

// Unit limit: from lowest_unit_limit in unit_limit_step steps to the
// dialog's highest offered.

/// Returns the unit limit's stops, up to the dialog's highest offered.
int32_t unit_limit_stops(const SettingsModel& model) {
    const uint16_t highest =
        model.dialog != nullptr ? model.dialog->highest_offered_unit : highest_unit_limit;
    return (highest - lowest_unit_limit) / unit_limit_step + 1;
}

/// Returns the stop nearest the unit limit.
int32_t unit_limit_stop(const SettingsModel& model) {
    return on_stops(
        steps_from(model.settings->unit_limit, lowest_unit_limit, unit_limit_step),
        unit_limit_stops(model)
    );
}

/// Sets the unit limit to a stop's.
void set_unit_limit_stop(SettingsModel& model, int32_t stop) {
    model.settings->unit_limit = static_cast<uint16_t>(
        lowest_unit_limit + on_stops(stop, unit_limit_stops(model)) * unit_limit_step
    );
}

/// Returns the unit limit, as "250 per player".
std::string unit_limit_text(const SettingsModel& model) {
    return std::to_string(model.settings->unit_limit) + " " + std::string(shown_text("per player"));
}

/// Skirmish players: one stop for each count from the lowest to the highest.
using SkirmishPlayers = Linear<
    int32_t,
    &EngineSettings::skirmish_players,
    lowest_skirmish_players,
    highest_skirmish_players,
    1>;

/// Returns the skirmish player count as the slider shows it.
std::string skirmish_players_text(const SettingsModel& model) {
    return std::to_string(model.settings->skirmish_players);
}

// Maximum frame rate, Text size, Hold delay, Pointer speed and Gyro speed.

using FrameRate = Linear<
    uint32_t,
    &EngineSettings::max_frame_rate,
    lowest_frame_rate,
    highest_frame_rate,
    frame_rate_step>;
using TextSize = Linear<
    int32_t,
    &EngineSettings::text_size,
    lowest_text_size,
    highest_text_size,
    text_size_step>;
using HoldDelay = Linear<
    uint32_t,
    &EngineSettings::touch_hold_ms,
    lowest_touch_hold_ms,
    highest_touch_hold_ms,
    touch_hold_step_ms>;
using PointerSpeed = Linear<
    uint32_t,
    &EngineSettings::pad_pointer_speed,
    pad_controls::lowest_pointer_speed,
    pad_controls::highest_pointer_speed,
    pad_controls::pointer_speed_step>;
using GyroSpeed = Linear<
    uint32_t,
    &EngineSettings::pad_gyro_speed,
    pad_controls::lowest_gyro_speed,
    pad_controls::highest_gyro_speed,
    pad_controls::gyro_speed_step>;

/// Returns the maximum frame rate, as "120 fps".
std::string frame_rate_text(const SettingsModel& model) {
    return std::to_string(model.settings->max_frame_rate) + " " + std::string(shown_text("fps"));
}

/// Returns the text size, as "80%".
std::string text_size_text(const SettingsModel& model) {
    return std::to_string(model.settings->text_size) + "%";
}

/// Returns the hold delay, as "350 ms".
std::string hold_delay_text(const SettingsModel& model) {
    return std::to_string(model.settings->touch_hold_ms) + " " + std::string(shown_text("ms"));
}

/// Returns the pointer speed, as "100%".
std::string pointer_speed_text(const SettingsModel& model) {
    return std::to_string(model.settings->pad_pointer_speed) + "%";
}

/// Returns the gyro speed, as "100%".
std::string gyro_speed_text(const SettingsModel& model) {
    return std::to_string(model.settings->pad_gyro_speed) + "%";
}

/// Edge stickiness: one stop for each tenth from 0.0 to 5.0.
using EdgeStickiness = Linear<
    uint32_t,
    &EngineSettings::edge_stickiness,
    lowest_edge_stickiness,
    highest_edge_stickiness,
    edge_stickiness_step>;

/// Returns Edge stickiness as the slider shows it, as "1.0".
std::string edge_stickiness_slider_text(const SettingsModel& model) {
    return edge_stickiness_text(model.settings->edge_stickiness);
}

// Screen size: the dialog's offered sizes; in a window, the window's own
// size until its knob moves (Dialog::window_screen_size).

/// Returns the screen size the slider shows.
///
/// @param model the model
/// @return the window's own size while the dialog keeps it, else the setting
ScreenSize shown_screen_size(const SettingsModel& model) {
    if (model.dialog != nullptr && model.dialog->window_screen_size)
        return *model.dialog->window_screen_size;
    return model.settings->screen_size;
}

/// Returns the sizes the slider offers.
///
/// @param model the model
/// @return the dialog's offered sizes, or screen_sizes without a dialog
std::span<const ScreenSize> offered_sizes(const SettingsModel& model) {
    if (model.dialog == nullptr)
        return screen_sizes;
    return model.dialog->offered_screen_sizes;
}

/// Returns Screen size's stops: one for each offered size, at least one.
int32_t screen_size_stops(const SettingsModel& model) {
    return std::max(static_cast<int32_t>(offered_sizes(model).size()), int32_t{1});
}

/// Returns the stop of the size shown; the first for a size not offered.
int32_t screen_size_stop(const SettingsModel& model) {
    const auto sizes = offered_sizes(model);
    const auto found = std::find(sizes.begin(), sizes.end(), shown_screen_size(model));
    const int32_t stop = found == sizes.end() ? 0 : static_cast<int32_t>(found - sizes.begin());
    return on_stops(stop, screen_size_stops(model));
}

/// Sets the screen size to a stop's size.
void set_screen_size_stop(SettingsModel& model, int32_t stop) {
    const auto sizes = offered_sizes(model);
    if (!sizes.empty())
        model.settings->screen_size =
            sizes[static_cast<std::size_t>(on_stops(stop, screen_size_stops(model)))];
}

/// Returns a screen size as the slider shows it: Custom for the window's own
/// size where the display offers no such size (Dialog::custom_screen_size).
///
/// @param model the model
/// @param size the size
/// @return the text
std::string screen_size_text(const SettingsModel& model, ScreenSize size) {
    if (model.dialog != nullptr && model.dialog->custom_screen_size &&
        size == *model.dialog->custom_screen_size)
        return std::string(shown_text("Custom"));
    if (size == desktop_screen_size)
        return std::string(shown_text("Desktop"));
    return oa::platform::display_modes::listed_size_text({size.width, size.height});
}

/// Returns a stop's text: the size shown at its own stop, else the stop's size.
std::string screen_size_caption(const SettingsModel& model, int32_t stop) {
    if (stop == screen_size_stop(model))
        return screen_size_text(model, shown_screen_size(model));
    const auto sizes = offered_sizes(model);
    if (stop < 0 || static_cast<std::size_t>(stop) >= sizes.size())
        return {};
    return screen_size_text(model, sizes[static_cast<std::size_t>(stop)]);
}

// The mod's keys: option_keys, in their order.

/// Returns a key's place among option_keys.
///
/// @param code the key's SDL key code
/// @return its index; 0 for a key not offered
int32_t option_key_index(uint32_t code) noexcept {
    for (std::size_t index = 0; index < option_keys.size(); ++index)
        if (option_keys[index].code == code)
            return static_cast<int32_t>(index);
    return 0;
}

/// A slider over one of the mod's keys.
template <uint32_t ModOptions::* Key>
struct KeySlider {
    static int32_t count(const SettingsModel&) { return static_cast<int32_t>(option_keys.size()); }

    static int32_t get(const SettingsModel& model) {
        return option_key_index(model.settings->mod_options.*Key);
    }

    static void set(SettingsModel& model, int32_t stop) {
        model.settings->mod_options.*Key =
            option_keys[static_cast<std::size_t>(on_stops(stop, count(model)))].code;
    }

    static std::string text(const SettingsModel& model) {
        return std::string(option_keys[static_cast<std::size_t>(get(model))].name);
    }

    static constexpr Stepper stepper() noexcept {
        return {&count, &get, &set, &stop_caption<&get, &set, &text>, nullptr};
    }
};

/// A slider of three choices of the mod options: patrolling or guarding
/// builders under one move order, or the resource bar's background.
template <uint8_t& (*Choice)(ModOptions&), const std::array<std::string_view, 3>& Captions>
struct ThreeWay {
    static int32_t count(const SettingsModel&) { return 3; }

    static int32_t get(const SettingsModel& model) {
        return on_stops(Choice(model.settings->mod_options), count(model));
    }

    static void set(SettingsModel& model, int32_t stop) {
        Choice(model.settings->mod_options) = static_cast<uint8_t>(on_stops(stop, count(model)));
    }

    static std::string text(const SettingsModel& model) {
        const auto choice = std::min<std::size_t>(Choice(model.settings->mod_options), 2);
        return std::string(shown_text(Captions[choice]));
    }

    static void copy(const SettingsModel& to, const SettingsModel& from) {
        Choice(to.settings->mod_options) = Choice(from.settings->mod_options);
    }

    static constexpr Stepper stepper() noexcept {
        return {&count, &get, &set, &stop_caption<&get, &set, &text>, nullptr};
    }
};

/// Returns the patrolling or guarding builders' choice under one move order.
template <std::array<uint8_t, 3> ModOptions::* Orders, std::size_t Order>
uint8_t& builders(ModOptions& options) {
    return (options.*Orders)[Order];
}

/// Returns the resource bar's background.
uint8_t& panel_background(ModOptions& options) {
    return options.panel_background;
}

/// A snap radius' slider: from 0 cells to the mod's most, at least one.
template <int32_t ModOptions::* Radius, int32_t ModOptions::* Most>
struct SnapRadius {
    static int32_t count(const SettingsModel& model) {
        return std::clamp(model.settings->mod_options.*Most, int32_t{1}, most_snap_radius) + 1;
    }

    static int32_t get(const SettingsModel& model) {
        return on_stops(model.settings->mod_options.*Radius, count(model));
    }

    static void set(SettingsModel& model, int32_t stop) {
        ModOptions& options = model.settings->mod_options;
        options.*Radius =
            std::min(on_stops(stop, count(model)), std::max(options.*Most, int32_t{0}));
    }

    static std::string text(const SettingsModel& model) {
        return std::to_string(model.settings->mod_options.*Radius) + " " +
               std::string(shown_text("cells"));
    }

    static constexpr Stepper stepper() noexcept {
        return {&count, &get, &set, &stop_caption<&get, &set, &text>, nullptr};
    }
};

// Use modern fonts for game text, and the Interface sizes it offers.

/// Tells whether the dialog shows Use modern fonts for game text On: the
/// switch is On, or the language shown needs the modern fonts, which locks
/// it On (Lock::set_by_language).
///
/// @param model the model
/// @return true while it shows On
bool shows_modern_fonts(const SettingsModel& model) {
    if (model.settings->modern_fonts)
        return true;
    return model.dialog != nullptr &&
           geometry::shown_locks(*model.dialog).modern_fonts == Lock::set_by_language;
}

/// Sets Use modern fonts for game text. Turned Off, it takes 150% out of
/// Interface size, which the game's own fonts cannot draw, leaving 100%.
///
/// @param[in,out] model the model
/// @param on the switch's new state
void set_modern_fonts(SettingsModel& model, bool on) {
    EngineSettings& settings = *model.settings;
    settings.modern_fonts = on;
    settings.interface_size = interface_size_with_fonts(settings.interface_size, on);
}

/// Returns the Interface sizes the dialog offers now.
///
/// @param model the model
/// @return how many: every size while it shows the modern fonts On, all
///     but 150% while it shows them Off
int32_t interface_size_count(const SettingsModel& model) {
    const auto every = static_cast<int32_t>(interface_size_choices.size());
    return shows_modern_fonts(model) ? every : every - 1;
}

/// Returns the place among interface_size_choices of an item the dialog
/// offers.
///
/// @param model the model
/// @param item the item, held to the items offered
/// @return its place in interface_size_choices and kInterfaceSizeCaptions
std::size_t interface_size_place(const SettingsModel& model, int32_t item) {
    const auto offered = static_cast<std::size_t>(on_stops(item, interface_size_count(model)));
    const auto fractional =
        static_cast<std::size_t>(choice_place(interface_size_choices, InterfaceSize::size_150));
    // Without the modern fonts the items from 150% on are one place on.
    return shows_modern_fonts(model) || offered < fractional ? offered : offered + 1;
}

/// Returns the item the dialog shows chosen: the size as the modern fonts
/// it shows leave it (interface_size_with_fonts).
///
/// @param model the model
/// @return the item, from 0
int32_t interface_size_item(const SettingsModel& model) {
    const bool modern = shows_modern_fonts(model);
    const InterfaceSize shown = interface_size_with_fonts(model.settings->interface_size, modern);
    const int32_t place = choice_place(interface_size_choices, shown);
    const int32_t fractional = choice_place(interface_size_choices, InterfaceSize::size_150);
    return modern || place < fractional ? place : place - 1;
}

/// Sets Interface size to an item the dialog offers.
///
/// @param[in,out] model the model
/// @param item the item, held to the items offered
void set_interface_size(SettingsModel& model, int32_t item) {
    model.settings->interface_size = interface_size_choices[interface_size_place(model, item)];
}

/// Returns an offered item's text, as the dialog shows it.
///
/// @param model the model
/// @param item the item
/// @return its caption; empty for an item past the ones offered
std::string interface_size_caption(const SettingsModel& model, int32_t item) {
    if (item < 0 || item >= interface_size_count(model))
        return {};
    return std::string(shown_text(kInterfaceSizeCaptions[interface_size_place(model, item)]));
}

/// Returns an offered item's id: the word the preferences keep its size as.
///
/// @param model the model
/// @param item the item
/// @return the id; empty for an item past the ones offered
std::string interface_size_id(const SettingsModel& model, int32_t item) {
    if (item < 0 || item >= interface_size_count(model))
        return {};
    return word_form(
        interface_size_text(interface_size_choices[interface_size_place(model, item)]), false
    );
}

/// Interface size's drop-down: its items depend on the Use modern fonts for
/// game text the dialog shows.
constexpr Stepper kInterfaceSizeStepper{
    &interface_size_count,
    &interface_size_item,
    &set_interface_size,
    &interface_size_caption,
    nullptr,
    &interface_size_id,
};

// Hardware acceleration's status.

/// The texts of Hardware acceleration's status, two lines for each state;
/// an empty second line is the reach line, which says what the graphics
/// card does on this machine.
struct StatusText {
    AccelerationState state{}; ///< the state
    std::string_view first;    ///< what runs, or why not
    std::string_view second;   ///< what draws the view or what to do; empty for the reach
};

/// The second line of a state the processor draws in.
constexpr std::string_view kProcessorDraws = "The processor draws and scales the view.";
/// The first line of the Off states.
constexpr std::string_view kOff = "Off: the processor draws and scales the view.";
/// The second line of a state that setting it to Off and back, or Restore
/// defaults, may lift.
constexpr std::string_view kRetry = "Set it to Off and back, or restore defaults.";
/// The second line of a state at a start that passed over a failed driver.
constexpr std::string_view kDriverSkipped = "A failed graphics driver is skipped.";
/// The first line of a machine under 2 GiB.
constexpr std::string_view kNeedsMemory = "Not in use: it needs at least 2 GB of memory.";

/// The second line of a state in which Full was asked for and Basic is in
/// use, that setting it to Off and back, or Restore defaults, may lift.
constexpr std::string_view kRetryFull = "Set it to Off and back, or restore defaults.";
/// The first line of Full in use.
constexpr std::string_view kFullInUse = "Full in use: the graphics card draws the view.";

/// Every state's status, in AccelerationState's order.
constexpr std::array<StatusText, 25> kStatusTexts{{
    {AccelerationState::off_driver_skipped, kOff, kDriverSkipped},
    {AccelerationState::needs_memory_driver_skipped, kNeedsMemory, kDriverSkipped},
    {AccelerationState::needs_memory, kNeedsMemory, kProcessorDraws},
    {AccelerationState::off_by_setting, kOff, "Basic lets the graphics card scale it evenly."},
    {AccelerationState::off_by_command_line, kOff, "For this run only. The setting is kept."},
    {AccelerationState::environment_driver,
     "Not in use: the environment names a driver.",
     kProcessorDraws},
    {AccelerationState::too_little_memory,
     "Not in use: there is too little memory.",
     kProcessorDraws},
    {AccelerationState::waiting_for_game_end,
     "Off for this game: in a shared game, Basic",
     "takes effect from the next game."},
    {AccelerationState::engine_error, "Not in use: an error stopped it for this run.", kRetry},
    {AccelerationState::driver_failed, "Not in use: the graphics driver failed.", kRetry},
    {AccelerationState::game_stopped, "Not in use: the game stopped while using it.", kRetry},
    {AccelerationState::no_usable_card,
     "Not in use: no usable graphics card was found.",
     kProcessorDraws},
    {AccelerationState::lacks_feature,
     "Not in use: the graphics card lacks a feature.",
     kProcessorDraws},
    {AccelerationState::cannot_save,
     "Not in use: the game cannot save its files.",
     kProcessorDraws},
    {AccelerationState::next_start,
     "Takes effect from the next start.",
     "The processor draws and scales the view until then."},
    {AccelerationState::full_cannot_save, "Basic in use: the game cannot save its files.", {}},
    {AccelerationState::full_too_little_memory,
     "Basic in use: there is too little memory for Full.",
     {}},
    {AccelerationState::full_stopped, "Basic in use: Full stopped for this run.", kRetryFull},
    {AccelerationState::full_failed_before,
     "Basic in use: Full failed before on this driver.",
     kRetryFull},
    {AccelerationState::full_lacks_feature,
     "Basic in use: the card lacks a feature Full needs.",
     {}},
    {AccelerationState::full_waiting_for_game_end,
     "Basic for this game: in a shared game, Full",
     "takes effect from the next game."},
    {AccelerationState::in_use_on_another_driver,
     "Basic in use, on another driver: one failed.",
     {}},
    {AccelerationState::in_use_no_smoothing,
     "Basic in use; no smoothing when zoomed out here.",
     {}},
    {AccelerationState::full_in_use, kFullInUse, {}},
    {AccelerationState::in_use, "Basic in use.", {}},
}};

/// Tells whether kStatusTexts holds every state once, in AccelerationState's order.
///
/// @return true when each entry's state is its index
constexpr bool status_texts_in_order() noexcept {
    for (std::size_t index = 0; index < kStatusTexts.size(); ++index)
        if (static_cast<std::size_t>(kStatusTexts[index].state) != index)
            return false;
    return kStatusTexts.size() == static_cast<std::size_t>(AccelerationState::in_use) + 1;
}

static_assert(status_texts_in_order(), "every state of Hardware acceleration has its status");

/// The first line of AccelerationState::waiting_for_game_end for Full in a
/// shared game; kStatusTexts holds Basic's.
constexpr std::string_view kWaitingForFull = "Off for this game: in a shared game, Full";
/// The first line of AccelerationState::waiting_for_game_end for Basic in a replay.
constexpr std::string_view kWaitingInReplay = "Off for this game: in a replay, Basic";
/// The first line of AccelerationState::waiting_for_game_end for Full in a replay.
constexpr std::string_view kWaitingForFullInReplay = "Off for this game: in a replay, Full";
/// The first line of AccelerationState::full_waiting_for_game_end in a
/// replay; kStatusTexts holds a shared game's.
constexpr std::string_view kBasicWaitingForFullInReplay = "Basic for this game: in a replay, Full";
/// The second line of Full in use, by its anti-aliasing: none, and 2, 4, 8
/// and 16 samples across.
constexpr std::string_view kFullReach = "Smoothed at every zoom.";
constexpr std::string_view kFullReachTwice = "Smoothed at every zoom; 2x2 samples a pixel.";
constexpr std::string_view kFullReachFourfold = "Smoothed at every zoom; 4x4 samples a pixel.";
constexpr std::string_view kFullReachEightfold = "Smoothed at every zoom; 8x8 samples a pixel.";
constexpr std::string_view kFullReachSixteenfold = "Smoothed at every zoom; 16x16 samples a pixel.";
/// The anti-aliasing the lines above name.
constexpr uint8_t kSupersampleTwice = 2;
constexpr uint8_t kSupersampleFourfold = 4;
constexpr uint8_t kSupersampleEightfold = 8;
constexpr uint8_t kSupersampleSixteenfold = 16;

/// Returns the first line of AccelerationState::waiting_for_game_end: the
/// match it waits for, and the level that takes effect after it.
///
/// @param acceleration the status
/// @return the line; Basic's for a status that asks for no more
std::string_view waiting_line(const AccelerationStatus& acceleration) noexcept {
    if (acceleration.state == AccelerationState::full_waiting_for_game_end)
        return acceleration.replay ? kBasicWaitingForFullInReplay
                                   : kStatusTexts[static_cast<std::size_t>(
                                                      AccelerationState::full_waiting_for_game_end
                                                  )]
                                         .first;
    const bool full = acceleration.asked == HardwareAcceleration::full;
    if (acceleration.replay)
        return full ? kWaitingForFullInReplay : kWaitingInReplay;
    return full ? kWaitingForFull
                : kStatusTexts[static_cast<std::size_t>(AccelerationState::waiting_for_game_end)]
                      .first;
}

/// Tells whether a state is Full in use, whose second line names its
/// anti-aliasing rather than the reach.
///
/// @param state the state
/// @return true for Full in use
bool full_in_use(AccelerationState state) noexcept {
    return state == AccelerationState::full_in_use;
}

/// Returns the second line of Full in use: smoothed at every zoom, with its
/// anti-aliasing where there is any.
///
/// @param supersample the samples a pixel across; 1 for no anti-aliasing
/// @return the line
std::string_view full_line(uint8_t supersample) noexcept {
    if (supersample >= kSupersampleSixteenfold)
        return kFullReachSixteenfold;
    if (supersample >= kSupersampleEightfold)
        return kFullReachEightfold;
    if (supersample >= kSupersampleFourfold)
        return kFullReachFourfold;
    if (supersample >= kSupersampleTwice)
        return kFullReachTwice;
    return kFullReach;
}

/// Returns the reach line: what the graphics card does on this machine.
///
/// @param reach the reach
/// @return the line
std::string_view reach_line(AccelerationReach reach) noexcept {
    switch (reach) {
    case AccelerationReach::menus:
        return "It scales the menus and the interface evenly.";
    case AccelerationReach::zoomed_in:
        return "It scales the interface and zoomed-in view evenly.";
    case AccelerationReach::zoomed_out:
        return "It scales evenly and smooths the zoomed-out view.";
    case AccelerationReach::nearest_zoomed_out:
        return "It smooths the zoomed-out view.";
    case AccelerationReach::nearest_none:
        return "Here the view is drawn as when it is off.";
    }
    return {};
}

// The hints that change with the settings or the dialog. Each returns its
// line in English, as the source writes it, or, for a host's texts, as the
// host gave them.

/// The lowest level that draws units finer and needs the warning hint.
constexpr AntiAliasing kDemandingLevel = AntiAliasing::x8;

/// Returns a line of a hint of two lines.
///
/// @param lines the hint
/// @param line the line, from 0
/// @return the line; empty past the second
std::string line_of(const Hint& lines, std::size_t line) {
    return line < lines.size() ? std::string(lines[line]) : std::string();
}

/// Returns the acceleration status the dialog shows.
///
/// @param model the model
/// @return the dialog's status, or the default without a dialog
AccelerationStatus acceleration_of(const SettingsModel& model) {
    return model.dialog != nullptr ? model.dialog->acceleration : AccelerationStatus{};
}

/// Returns a line of Maximum zoom out's hint, by the choice.
std::string max_zoom_out_hint(const SettingsModel& model, std::size_t line) {
    // What the choice stops at, and how units look there.
    switch (model.settings->max_zoom_out) {
    case ZoomOutLimit::automatic:
        return line_of(
            {"As far as the view always went: 1/6 of normal size with Full hardware acceleration, "
             "else 1/2."},
            line
        );
    case ZoomOutLimit::whole_map:
        return line_of(
            {"Out until the whole map fits the view. Graphics' Zoomed out units says how units "
             "look."},
            line
        );
    case ZoomOutLimit::one_thirty_second:
        return line_of(
            {"Out to 1/32 of normal size, or the whole map. Graphics' Zoomed out units says how "
             "units look."},
            line
        );
    case ZoomOutLimit::one_sixteenth:
        return line_of(
            {"Out to 1/16 of normal size, or the whole map. Graphics' Zoomed out units says how "
             "units look."},
            line
        );
    case ZoomOutLimit::one_eighth:
        return line_of(
            {"Out to 1/8 of normal size, or the whole map. Graphics' Zoomed out units says how "
             "units look."},
            line
        );
    case ZoomOutLimit::one_quarter:
        return line_of(
            {"Out to 1/4 of normal size, or the whole map. Graphics' Zoomed out units says how "
             "units look."},
            line
        );
    case ZoomOutLimit::one_half:
        return line_of(
            {"Out to 1/2 of normal size, or the whole map. Graphics' Zoomed out units says how "
             "units look."},
            line
        );
    }
    return {};
}

/// Returns a line of Maximum zoom in's hint, by the choice.
std::string max_zoom_in_hint(const SettingsModel& model, std::size_t line) {
    // What the choice stops at, and every way of zooming it holds.
    switch (model.settings->max_zoom_in) {
    case ZoomInLimit::none:
        return line_of(
            {"Never closer than normal size. The wheel, a pinch and a controller stop there."}, line
        );
    case ZoomInLimit::twice:
        return line_of(
            {"In to 2x normal size at most. The wheel, a pinch and a controller stop there."}, line
        );
    case ZoomInLimit::three_times:
        return line_of(
            {"In to 3x normal size at most. The wheel, a pinch and a controller stop there."}, line
        );
    case ZoomInLimit::four_times:
        return line_of(
            {"In to 4x normal size at most. The wheel, a pinch and a controller stop there."}, line
        );
    }
    return {};
}

/// Returns a line of View past the map's edge's hint, by the choice.
std::string view_past_map_edge_hint(const SettingsModel& model, std::size_t line) {
    // How much of the battlefield may lie past the map, and whether a zoom
    // keeps to it.
    switch (model.settings->view_past_map_edge) {
    case ViewPastMapEdge::off:
        return line_of(
            {"The view stays on the map, as the game kept it. With more than the map in view, it "
             "is centred."},
            line
        );
    case ViewPastMapEdge::one_quarter:
        return line_of(
            {"Up to a quarter of the battlefield past the map's edges, however the view moves."},
            line
        );
    case ViewPastMapEdge::one_half:
        return line_of(
            {"Up to half the battlefield past the map's edges; a zoom keeps the ground under the "
             "pointer."},
            line
        );
    }
    return {};
}

/// Returns a line of Maximum frame rate's hint, and on a Steam Deck the rate it starts at.
std::string max_frame_rate_hint(const SettingsModel& model, std::size_t line) {
    // On a Steam Deck, a second line names the screen's rate, which the
    // setting starts at.
    const uint32_t rate = model.dialog != nullptr ? model.dialog->steam_deck_panel_hz : 0;
    if (line == 1 && rate != 0) {
        constexpr std::string_view rate_field = "{rate}";
        std::string text(shown_text(steam_deck_rate_text));
        const auto at = text.find(rate_field);
        if (at != std::string::npos)
            text.replace(at, rate_field.size(), std::to_string(rate));
        return text;
    }
    return line_of({"Lower it to save power.", {}}, line);
}

/// Returns Maximum frame rate's hint lines: two on a Steam Deck.
std::size_t max_frame_rate_lines(const SettingsModel& model) {
    // On a Steam Deck, Maximum frame rate names the screen's rate it starts at.
    return model.dialog != nullptr && model.dialog->steam_deck_panel_hz != 0 ? 2 : 1;
}

/// Returns Enhanced anti-aliasing's hint, the whole sentence for the level
/// and for what Full draws.
std::string anti_aliasing_hint(const SettingsModel& model, std::size_t line) {
    if (line != 0)
        return {};
    const EngineSettings& settings = *model.settings;
    const uint8_t drawn = acceleration_of(model).full_supersample;
    if (drawn != 0) {
        // Fewer samples than the row asks: the texture limit or the memory
        // allows no more at this window's size.
        const bool capped = drawn >= 2 && drawn < static_cast<uint8_t>(settings.anti_aliasing);
        if (drawn >= 16)
            return capped ? "In Full the card draws 16x16 samples a pixel the most this window "
                            "allows, scaled down."
                          : "In Full the card draws 16x16 samples a pixel and scales them down for "
                            "smoother edges.";
        if (drawn >= 8)
            return capped ? "In Full the card draws 8x8 samples a pixel the most this window "
                            "allows, scaled down."
                          : "In Full the card draws 8x8 samples a pixel and scales them down for "
                            "smoother edges.";
        if (drawn >= 4)
            return capped ? "In Full the card draws 4x4 samples a pixel the most this window "
                            "allows, scaled down."
                          : "In Full the card draws 4x4 samples a pixel and scales them down for "
                            "smoother edges.";
        if (drawn >= 2)
            return capped ? "In Full the card draws 2x2 samples a pixel the most this window "
                            "allows, scaled down."
                          : "In Full the card draws 2x2 samples a pixel and scales them down for "
                            "smoother edges.";
        return "In Full the graphics card draws the view 1:1; a level draws it finer for smoother "
               "edges.";
    }
    if (settings.anti_aliasing == AntiAliasing::x16)
        return "Units drawn at 16x and scaled down. Needs a fast CPU.";
    if (static_cast<uint8_t>(settings.anti_aliasing) >= static_cast<uint8_t>(kDemandingLevel))
        return "Units drawn at 8x and scaled down. Needs a fast CPU.";
    return "Units drawn at higher resolution and scaled down for smoother edges.";
}

/// Returns a line of Hardware acceleration's status.
std::string hardware_acceleration_hint(const SettingsModel& model, std::size_t line) {
    // Its hint is its status, which the host keeps up to date.
    return std::string(status_line(acceleration_of(model), line));
}

/// Returns a line of Menu scaling's hint, by the way chosen.
std::string menu_scaling_hint(const SettingsModel& model, std::size_t line) {
    // What the way chosen does to the menus.
    switch (model.settings->menu_scaling) {
    case MenuScaling::sharp:
        return line_of({"The menus fill the window, every pixel as wide as the next."}, line);
    case MenuScaling::whole_steps:
        return line_of(
            {"The largest whole-number scale that fits: smaller menus, every pixel square."}, line
        );
    case MenuScaling::unfiltered:
        return line_of(
            {"The menus fill the window, unfiltered, as the game always drew them."}, line
        );
    }
    return {};
}

/// Returns a line of Explosion flash's hint, by the level.
std::string explosion_flash_hint(const SettingsModel& model, std::size_t line) {
    // What the level chosen draws; a mod's profile may draw less.
    switch (model.settings->explosion_flash) {
    case ExplosionFlash::off:
        return line_of({"Explosions do not light up the ground, whatever a mod asks for."}, line);
    case ExplosionFlash::reduced:
        return line_of(
            {"Explosions light up the ground at half strength, or less where a mod asks."}, line
        );
    case ExplosionFlash::full:
        return line_of(
            {"Explosions light up the ground as the game drew it, or less where a mod asks."}, line
        );
    }
    return {};
}

/// Returns a line of Fog of War's hint, by the way chosen.
std::string fog_of_war_hint(const SettingsModel& model, std::size_t line) {
    // How the fog's edges look, and where Smooth draws them so: the
    // processor's drawing is always the game's own.
    switch (model.settings->fog_of_war) {
    case FogOfWar::classic:
        return line_of({"Hard edges that step from cell to cell, as the game drew the fog."}, line);
    case FogOfWar::smooth:
        return line_of({"Soft edges where the graphics card draws the battlefield (Full)."}, line);
    }
    return {};
}

/// Returns a line of Zoomed out units' hint, by the way chosen.
std::string zoomed_out_units_hint(const SettingsModel& model, std::size_t line) {
    // What the way chosen draws past After zoom, and what it costs.
    switch (model.settings->zoomed_out_units) {
    case ZoomedOutUnits::rendered:
    case ZoomedOutUnits::icons:
        return line_of(
            {"Units are drawn as models at every zoom. Far out on a large map, needs a fast CPU."},
            line
        );
    case ZoomedOutUnits::dots:
        return line_of(
            {"Past After zoom, each unit is a dot of its owner's colour, framed while selected."},
            line
        );
    }
    return {};
}

/// Returns a line of After zoom's hint, by the choice.
std::string zoomed_out_after_hint(const SettingsModel& model, std::size_t line) {
    // Where the dots begin, and what is drawn nearer.
    switch (model.settings->zoomed_out_after) {
    case ZoomedOutAfter::one_half:
        return line_of(
            {"Dots farther out than 1/2 of normal size. Closer in, units are drawn as models."},
            line
        );
    case ZoomedOutAfter::one_third:
        return line_of(
            {"Dots farther out than 1/3 of normal size. Closer in, units are drawn as models."},
            line
        );
    case ZoomedOutAfter::one_quarter:
        return line_of(
            {"Dots farther out than 1/4 of normal size. Closer in, units are drawn as models."},
            line
        );
    case ZoomedOutAfter::one_sixth:
        return line_of(
            {"Dots farther out than 1/6 of normal size. Closer in, units are drawn as models."},
            line
        );
    case ZoomedOutAfter::one_eighth:
        return line_of(
            {"Dots farther out than 1/8 of normal size. Closer in, units are drawn as models."},
            line
        );
    case ZoomedOutAfter::one_twelfth:
        return line_of(
            {"Dots farther out than 1/12 of normal size. Closer in, units are drawn as models."},
            line
        );
    case ZoomedOutAfter::one_sixteenth:
        return line_of(
            {"Dots farther out than 1/16 of normal size. Closer in, units are drawn as models."},
            line
        );
    }
    return {};
}

/// Returns a line of Window edge's hint, by the way chosen.
std::string window_edge_hint(const SettingsModel& model, std::size_t line) {
    // Whether a window keeps the pointer while a battlefield shows.
    switch (model.settings->window_edge) {
    case WindowEdge::capture:
        return line_of({"The pointer stays in the window while you play.", {}}, line);
    case WindowEdge::sticky:
        return line_of({"The pointer rests on the edge so the view scrolls.", {}}, line);
    case WindowEdge::free:
        return line_of({"The pointer leaves a window's edge, as before.", {}}, line);
    }
    return {};
}

/// Returns a line of Edge stickiness's hint: what the level does.
std::string edge_stickiness_hint(const SettingsModel& model, std::size_t line) {
    if (model.settings->edge_stickiness == lowest_edge_stickiness)
        return line_of({"At 0.0 the edge never holds.", {}}, line);
    return line_of({"How firmly Sticky holds the pointer at the edge.", {}}, line);
}

/// Returns a line of Window frame's hint, by the way chosen.
std::string window_frame_hint(const SettingsModel& model, std::size_t line) {
    // When a window shows its frame; full screen has none.
    switch (model.settings->window_frame) {
    case WindowFrame::hidden_in_play:
        return line_of(
            {"A window hides its title bar and borders while a game is played; menus show them."},
            line
        );
    case WindowFrame::always_shown:
        return line_of(
            {"A window shows its title bar and borders on every screen, a game's included."}, line
        );
    }
    return {};
}

/// Returns a line of HUD scaling's hint, by the switch.
std::string hud_scaling_hint(const SettingsModel& model, std::size_t line) {
    // The size the game's side column and bars are drawn at.
    if (model.settings->hud_scaling)
        return line_of(
            {"The side panel and bars grow with the window, up to twice the original game's size."},
            line
        );
    return line_of(
        {"The side panel and bars keep the original game's size on every window."}, line
    );
}

/// Returns a line of Smooth motion's hint, by the switch.
std::string smooth_motion_hint(const SettingsModel& model, std::size_t line) {
    // Whether frames between the game's ticks show the way from one to the next.
    if (model.settings->smooth_motion)
        return line_of(
            {"Units, shots and spinning parts move smoothly between the game's ticks."}, line
        );
    return line_of(
        {"Units and shots move 30 times a second, as the game drew them; fast spins may flicker."},
        line
    );
}

/// Returns a line of Text size's hint, by the modern fonts' switch.
std::string text_size_hint(const SettingsModel& model, std::size_t line) {
    if (line != 0)
        return {};
    if (model.settings->modern_fonts)
        return "The size of game text in the modern fonts. Larger sizes are easier to read.";
    return "The size of game text in the modern fonts. The game's own fonts have fixed sizes.";
}

/// Returns a line of One-finger drag's hint, by the way chosen.
std::string touch_drag_hint(const SettingsModel& model, std::size_t line) {
    // What a drag does now, and what still gives the other.
    switch (model.settings->touch_drag) {
    case TouchDrag::automatic:
        return line_of({"Automatic: a selection box on a tablet, scrolling on a phone."}, line);
    case TouchDrag::box:
        return line_of({"A drag draws a selection box; two fingers scroll the map."}, line);
    case TouchDrag::scroll:
        return line_of({"A drag scrolls the map; hold, then drag, for a selection box."}, line);
    }
    return {};
}

/// Returns a line of QUEUE and ADD's hint, by the way chosen.
std::string touch_latches_hint(const SettingsModel& model, std::size_t line) {
    if (model.settings->touch_latches == TouchLatches::one_action)
        return line_of(
            {"A tapped QUEUE, ADD or x5 turns off after the next order or selection."}, line
        );
    return line_of({"A tapped QUEUE, ADD or x5 stays on until it is tapped again."}, line);
}

/// Returns a line of Scheme's hint, by the scheme.
std::string pad_scheme_hint(const SettingsModel& model, std::size_t line) {
    if (model.settings->pad_scheme == pad_controls::Scheme::sticks)
        return line_of({"The right stick moves the pointer and the left stick the map."}, line);
    return line_of(
        {"The right trackpad points and the left one moves the map; a pad without trackpads plays "
         "Sticks."},
        line
    );
}

/// Returns Right trackpad's hint, by the way chosen.
std::string pad_right_trackpad_hint(const SettingsModel& model, std::size_t line) {
    if (model.settings->pad_right_trackpad == pad_controls::RightTrackpad::absolute)
        return line_of({"Each point of the pad is a point of the view.", {}}, line);
    return line_of({"The pointer moves as the thumb slides.", {}}, line);
}

/// Returns a line of Right stick's hint, by its role.
std::string pad_right_stick_hint(const SettingsModel& model, std::size_t line) {
    switch (model.settings->pad_right_stick) {
    case pad_controls::RightStick::zoom_and_pages:
        return line_of(
            {"Up and down zoom about the pointer; a flick left or right turns the build page while "
             "building."},
            line
        );
    case pad_controls::RightStick::pointer:
        return line_of({"The right stick moves the pointer, as in the Sticks scheme."}, line);
    case pad_controls::RightStick::nothing:
        return line_of({"The right stick does nothing.", {}}, line);
    }
    return {};
}

/// Returns the most characters a line of a host's text under a row holds in
/// a model's dialog: its size class's, or Compact's without a dialog.
///
/// @param model the model
/// @return the characters
std::size_t line_characters(const SettingsModel& model) noexcept {
    return model.dialog == nullptr ? hint_line_characters
                                   : sizes_of(*model.dialog).hint_line_characters;
}

/// Returns a line of Controller's Steam Input notice, broken between words.
std::string steam_input_notice_lines(const SettingsModel& model, std::size_t line) {
    // The notice, in the language shown, between words, as wide as the
    // dialog's class lets a line be.
    const auto lines =
        break_lines(shown_text(steam_input_notice_text), line_characters(model), most_notice_lines);
    return line < lines.size() ? lines[line] : std::string{};
}

/// Tells that a line is a notice: every line of the Steam Input notice is.
bool every_line_a_notice(const SettingsModel&, std::size_t) {
    return true;
}

/// Returns a line of the host's texts under Installed: the summary, then its sizes.
std::string game_files_summary_hint(const SettingsModel& model, std::size_t line) {
    // What is installed, then its size and the free space.
    if (model.dialog == nullptr)
        return {};
    if (line == 0)
        return model.dialog->game_files_summary;
    return line == 1 ? model.dialog->game_files_sizes : std::string{};
}

/// Include in device backups' hint: {device} is the device's own name.
constexpr Hint kBackupsHint{
    "After restoring this {device} from a backup, add the game files again."
};

/// Returns a line of Include in device backups' hint, naming the device.
std::string backups_hint(const SettingsModel& model, std::size_t line) {
    // The device's own name, or the neutral word, in the line shown.
    std::string text(shown_text(line_of(kBackupsHint, line)));
    constexpr std::string_view device_field = "{device}";
    const std::string device = model.dialog == nullptr || model.dialog->game_files_device.empty()
                                   ? std::string(shown_text("device"))
                                   : model.dialog->game_files_device;
    for (std::size_t at = text.find(device_field); at != std::string::npos;
         at = text.find(device_field, at + device.size()))
        text.replace(at, device_field.size(), device);
    return text;
}

/// Returns a line of where the files are, broken between words.
std::string game_files_location_hint(const SettingsModel& model, std::size_t line) {
    if (model.dialog == nullptr)
        return {};
    const auto lines = break_lines(model.dialog->game_files_location, line_characters(model), 2);
    return line < lines.size() ? lines[line] : std::string{};
}

/// Returns a line under Your files: the folder's path, then why a folder
/// could not be opened, or what the folder holds.
std::string user_folder_hint(const SettingsModel& model, std::size_t line) {
    // The player's own folder, then why a folder could not be opened, as a
    // notice.
    if (line == 0)
        return model.dialog != nullptr ? model.dialog->user_folder : std::string{};
    if (line == 1 && model.dialog != nullptr && !model.dialog->folder_notice.empty())
        return model.dialog->folder_notice;
    return line_of({std::string_view{}, user_folder_hint_text}, line);
}

/// Tells whether a line under Your files is a notice: why a folder could not be opened.
bool user_folder_notice(const SettingsModel& model, std::size_t line) {
    return line == 1 && model.dialog != nullptr && !model.dialog->folder_notice.empty();
}

// Specs that take more than their factory's arguments.

/// Gives a spec hint lines that change with the model.
constexpr Spec changing(
    Spec spec,
    std::string (*text)(const SettingsModel&, std::size_t),
    std::size_t (*count)(const SettingsModel&)
) noexcept {
    spec.hint_text = text;
    spec.hint_lines = count;
    return spec;
}

/// Gives a spec its control width: a strip's level width or a drop-down's
/// field width.
constexpr Spec wide(Spec spec, int32_t width) noexcept {
    spec.control_width = width;
    return spec;
}

/// Makes a slider's row.
constexpr Spec
slider(std::string_view id, std::string_view label, Stepper stepper, Hint hint) noexcept {
    return kit::slider(id, label, stepper, hint);
}

/// Makes a slider's row of a number in equal steps.
template <class Slider, std::string (*Text)(const SettingsModel&)>
constexpr Spec linear(std::string_view id, std::string_view label, Hint hint) noexcept {
    return kit::slider(
        id,
        label,
        Stepper{
            &Slider::count,
            &Slider::get,
            &Slider::set,
            &stop_caption<&Slider::get, &Slider::set, Text>,
            nullptr
        },
        hint
    );
}

/// The Game files summary row: what is installed, and MANAGE….
constexpr Spec game_files_summary_row() noexcept {
    Spec spec = kit::value_and_button<SettingsModel>(
        "game-files-summary", "Installed", nullptr, manage_text, "manage", manage_game_files_action
    );
    spec.control_width = manage_button_width;
    return changing(spec, &game_files_summary_hint, &lines<2>);
}

/// Returns UI kit gallery's hint as shown, broken into lines between words
/// as wide as the dialog's class lets a line be.
///
/// @param model the model
/// @return the lines; at least one
std::vector<std::string> kit_gallery_hint_lines(const SettingsModel& model) {
    std::vector<std::string> broken = break_lines(
        shown_text(kit_gallery_hint_text), line_characters(model), kit_gallery_hint_most_lines
    );
    if (broken.empty())
        broken.emplace_back();
    return broken;
}

/// Returns a line of UI kit gallery's hint.
std::string kit_gallery_hint(const SettingsModel& model, std::size_t line) {
    const std::vector<std::string> broken = kit_gallery_hint_lines(model);
    return line < broken.size() ? broken[line] : std::string{};
}

/// Returns how many lines UI kit gallery's hint takes: one at Regular and
/// Large, two at Compact.
std::size_t kit_gallery_hint_count(const SettingsModel& model) {
    return kit_gallery_hint_lines(model).size();
}

/// Welcome setup's hint, in English.
constexpr std::string_view welcome_setup_hint_text =
    "Walk through the language, online features and the OA buttons again.";
/// The most lines Welcome setup's hint takes.
constexpr std::size_t welcome_setup_hint_most_lines = 2;

/// Returns Welcome setup's hint as shown, broken into lines between words
/// as wide as the dialog's class lets a line be.
std::vector<std::string> welcome_setup_hint_lines(const SettingsModel& model) {
    std::vector<std::string> broken = break_lines(
        shown_text(welcome_setup_hint_text), line_characters(model), welcome_setup_hint_most_lines
    );
    if (broken.empty())
        broken.emplace_back();
    return broken;
}

/// Returns a line of Welcome setup's hint.
std::string welcome_setup_hint(const SettingsModel& model, std::size_t line) {
    const std::vector<std::string> broken = welcome_setup_hint_lines(model);
    return line < broken.size() ? broken[line] : std::string{};
}

/// Returns how many lines Welcome setup's hint takes: one at Regular and
/// Large, two at Compact.
std::size_t welcome_setup_hint_count(const SettingsModel& model) {
    return welcome_setup_hint_lines(model).size();
}

/// The Welcome setup row: SHOW AGAIN walks through the first-run setup again
/// at the next main menu, once OK is pressed. It keeps no setting.
constexpr Spec welcome_setup_row() noexcept {
    const Spec spec = kit::value_and_button<SettingsModel>(
        "first-run",
        "Welcome setup",
        nullptr,
        "SHOW AGAIN",
        "",
        show_welcome_action,
        {welcome_setup_hint_text, {}}
    );
    return changing(spec, &welcome_setup_hint, &welcome_setup_hint_count);
}

/// The UI kit gallery row: SHOW opens the gallery of every part of Open
/// Annihilation's own screens. It keeps no setting.
constexpr Spec kit_gallery_row() noexcept {
    const Spec spec = kit::value_and_button<SettingsModel>(
        "kit-gallery",
        "UI kit gallery",
        nullptr,
        "SHOW",
        "show",
        show_gallery_action,
        {kit_gallery_hint_text, {}}
    );
    return changing(spec, &kit_gallery_hint, &kit_gallery_hint_count);
}

/// File Type Registration's heading, one sentence the row wraps.
constexpr std::string_view file_type_heading_text =
    "Open these files with the game from your file manager.";
/// The text row's first line, on every system that shows it.
constexpr std::string_view file_types_taken_text =
    "The system takes these file types from the app.";
/// The Finder sentence. iOS has no Finder, so it shows none.
constexpr std::string_view file_types_finder_text =
    "Finder's Get Info › Open with chooses another opener.";

/// The line under File Type Registration. While macOS kept this copy as an
/// opener, that sentence replaces the usual hint.
std::vector<std::string> file_type_heading_lines(const SettingsModel& model) {
    if (model.dialog != nullptr && !model.dialog->file_type_kept_hint.empty())
        return {model.dialog->file_type_kept_hint};
    return {std::string(file_type_heading_text)};
}

/// Returns the line under File Type Registration.
std::string file_type_heading_hint(const SettingsModel& model, std::size_t line) {
    const std::vector<std::string> lines = file_type_heading_lines(model);
    return line < lines.size() ? lines[line] : std::string{};
}

/// Returns how many lines File Type Registration's hint takes before it wraps.
std::size_t file_type_heading_hint_count(const SettingsModel& model) {
    return file_type_heading_lines(model).size();
}

/// The File Type Registration heading. It keeps no setting.
constexpr Spec file_type_heading_row() noexcept {
    const Spec spec =
        kit::text<SettingsModel>("file-type-registration", "File Type Registration", nullptr);
    return changing(spec, &file_type_heading_hint, &file_type_heading_hint_count);
}

/// Returns a line of the File types text row. The system's sentence is the
/// first. On a Mac the Finder sentence follows, and the row wraps it. iOS
/// has no Finder.
std::string file_types_note_hint(const SettingsModel& model, std::size_t line) {
    if (line == 0)
        return std::string(file_types_taken_text);
    static_cast<void>(model);
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
    return {};
#else
    if (line == 1)
        return std::string(file_types_finder_text);
    return {};
#endif
}

/// Returns how many lines the File types text row takes before they wrap.
std::size_t file_types_note_hint_count(const SettingsModel&) {
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
    return 1;
#else
    return 2;
#endif
}

/// The File types text row, in place of the switches. It keeps no setting.
constexpr Spec file_types_note_row() noexcept {
    const Spec spec = kit::text<SettingsModel>("file-types", "File types", nullptr);
    return changing(spec, &file_types_note_hint, &file_types_note_hint_count);
}

/// The Your files row: the player's own folder and its three folders' buttons.
constexpr Spec user_folder_row() noexcept {
    Spec spec = kit::buttons<SettingsModel>(
        "user-folder",
        "Your files",
        {"SAVES", "SCREENSHOTS", "MODS"},
        {"saves", "screenshots", "mods"},
        {first_folder_action + static_cast<kit::ActionId>(FolderButton::saves),
         first_folder_action + static_cast<kit::ActionId>(FolderButton::screenshots),
         first_folder_action + static_cast<kit::ActionId>(FolderButton::mods)}
    );
    spec.hint_notice = &user_folder_notice;
    return changing(spec, &user_folder_hint, &lines<2>);
}

/// Controller's Steam Input notice: a text row whose lines are a notice.
constexpr Spec steam_input_notice_row() noexcept {
    Spec spec = kit::text<SettingsModel>("pad-steam-input-notice", "Steam Input", nullptr);
    spec.hint_notice = &every_line_a_notice;
    return changing(spec, &steam_input_notice_lines, &lines<most_notice_lines>);
}

/// Where the files are: a text row of the host's text.
constexpr Spec game_files_location_row() noexcept {
    return changing(
        kit::text<SettingsModel>("game-files-location", "Where the files are", nullptr),
        &game_files_location_hint,
        &lines<2>
    );
}

/// Hardware acceleration: a strip whose two hint lines are its status.
constexpr Spec hardware_acceleration_row() noexcept {
    Spec spec = kit::levels(
        "hardware-acceleration",
        "Hardware acceleration",
        LevelStrip<
            HardwareAcceleration,
            3,
            hardware_acceleration_levels,
            &EngineSettings::hardware_acceleration,
            kAccelerationCaptions,
            &text_word<&hardware_acceleration_text, HardwareAcceleration>>::stepper(),
        acceleration_level_width
    );
    spec.hint_is_status = true;
    return changing(spec, &hardware_acceleration_hint, &lines<2>);
}

// The patrol, guard and background sliders' stops.

using PatrolHold = ThreeWay<&builders<&ModOptions::patrol, 0>, kPatrolCaptions>;
using PatrolManeuver = ThreeWay<&builders<&ModOptions::patrol, 1>, kPatrolCaptions>;
using PatrolRoam = ThreeWay<&builders<&ModOptions::patrol, 2>, kPatrolCaptions>;
using GuardHold = ThreeWay<&builders<&ModOptions::guard, 0>, kGuardCaptions>;
using GuardManeuver = ThreeWay<&builders<&ModOptions::guard, 1>, kGuardCaptions>;
using GuardRoam = ThreeWay<&builders<&ModOptions::guard, 2>, kGuardCaptions>;
using PanelBackground = ThreeWay<&panel_background, kPanelBackgroundCaptions>;
using MexSnap = SnapRadius<&ModOptions::mex_snap_radius, &ModOptions::mex_snap_most>;
using WreckSnap = SnapRadius<&ModOptions::wreck_snap_radius, &ModOptions::wreck_snap_most>;

/// What a patrolling builder's row says.
constexpr Hint kPatrolHint{"What patrolling builders do. Applies from the next game."};
/// What a guarding builder's row says.
constexpr Hint kGuardHint{"What guarding builders do. Applies from the next game."};

/// Makes a patrolling or guarding builder's row. Its hint is one sentence.
constexpr Spec
builders_row(std::string_view id, std::string_view label, Stepper stepper, Hint hint) noexcept {
    Spec spec = kit::slider(id, label, stepper, hint);
    spec.hint_lines = &lines<1>;
    return spec;
}

// ---------------------------------------------------------------------------
// The table.

/// One setting's row: its spec, and what the dialog reads of it besides.
struct SettingRow {
    Setting setting{}; ///< the setting, the row's place in the table
    Spec spec;         ///< what the row is, shows and changes
    /// The field of the dialog's locks that locks the row; null for a row
    /// the dialog never locks.
    Lock Locks::* lock{};
    /// Copies the row's own fields from one model to another, exactly; null
    /// copies the row through its kind's get and set.
    void (*copy)(const SettingsModel& to, const SettingsModel& from){};
    /// The first hint line is a folder's path, shown as its tail that fits.
    bool path_hint{};
};

using S = Setting;
using E = EngineSettings;
using O = ModOptions;

// clang-format off
/// Every setting's row, in Setting's order.
constexpr std::array<SettingRow, kSettingCount> kDeclared{{
    {.setting = S::path_search,
     .spec = slider("path-search", "Pathfinding cycles",
                    {&path_search_stops, &path_search_stop, &set_path_search_stop,
                     &stop_caption<&path_search_stop, &set_path_search_stop, &path_search_text>, nullptr},
                    {"More cycles find routes faster but use more CPU.", {}}),
     .lock = &Locks::path_search,
     .copy = &copy_field<&E::path_search_nodes>},
    {.setting = S::wheel_zoom,
     .spec = engine_switch<&E::wheel_zoom>("wheel-zoom", "Mouse wheel zoom",
                                           {"Scroll to zoom the battlefield in and out.", {}})},
    {.setting = S::max_zoom_out,
     .spec = changing(wide(kit::choice("max-zoom-out", "Maximum zoom out",
                                       DropDown<ZoomOutLimit, 7, zoom_out_limits, &E::max_zoom_out, kZoomOutCaptions, &table_word<zoom_out_words, ZoomOutLimit>>::stepper()),
                           choice_width),
                      &max_zoom_out_hint, &lines<1>)},
    {.setting = S::max_zoom_in,
     .spec = changing(wide(kit::choice("max-zoom-in", "Maximum zoom in",
                                       DropDown<ZoomInLimit, 4, zoom_in_limits, &E::max_zoom_in, kZoomInCaptions, &table_word<zoom_in_words, ZoomInLimit>>::stepper()),
                           choice_width),
                      &max_zoom_in_hint, &lines<1>)},
    {.setting = S::view_past_map_edge,
     .spec = changing(kit::levels("view-past-map-edge", "View past the map's edge",
                                  LevelStrip<ViewPastMapEdge, 3, view_past_map_edge_choices, &E::view_past_map_edge, kViewPastMapEdgeCaptions, &table_word<view_past_map_edge_words, ViewPastMapEdge>>::stepper(),
                                  view_past_map_edge_level_width),
                      &view_past_map_edge_hint, &lines<1>)},
    {.setting = S::middle_click_pan,
     .spec = engine_switch<&E::middle_click_pan>(
         "middle-click-pan", "Middle-click pans",
         {"A middle click pans the view towards the pointer until you click or press a key.", {}})},
    {.setting = S::escape_opens_menu,
     .spec = engine_switch<&E::escape_opens_menu>("escape-opens-menu", "Escape opens the game menu",
                                                  {"The first press clears the selection, the second opens the menu."})},
    {.setting = S::switch_alt,
     .spec = engine_switch<&E::switch_alt>("switch-alt", "Select groups without Alt",
                                           {"A number key selects its group on its own.", {}})},
    {.setting = S::window_edge,
     .spec = changing(kit::levels("window-edge", "Window edge",
                                  LevelStrip<WindowEdge, 3, window_edge_choices, &E::window_edge, kWindowEdgeCaptions, &table_word<window_edge_words, WindowEdge>>::stepper(),
                                  window_edge_level_width),
                      &window_edge_hint, &lines<1>)},
    {.setting = S::window_edge_stickiness,
     .spec = changing(linear<EdgeStickiness, &edge_stickiness_slider_text>("window-edge-stickiness", "Edge stickiness", {}),
                      &edge_stickiness_hint, &lines<1>),
     .lock = &Locks::edge_stickiness,
     .copy = &copy_field<&E::edge_stickiness>},
    {.setting = S::unit_limit,
     .spec = slider("unit-limit", "Unit limit",
                    {&unit_limit_stops, &unit_limit_stop, &set_unit_limit_stop,
                     &stop_caption<&unit_limit_stop, &set_unit_limit_stop, &unit_limit_text>, nullptr},
                    {"Units each player can have. Applies from the next game."}),
     .lock = &Locks::unit_limit,
     .copy = &copy_field<&E::unit_limit>},
    {.setting = S::max_frame_rate,
     .spec = changing(linear<FrameRate, &frame_rate_text>("max-frame-rate", "Maximum frame rate", {}),
                      &max_frame_rate_hint, &max_frame_rate_lines),
     .lock = &Locks::max_frame_rate,
     .copy = &copy_field<&E::max_frame_rate>},
    {.setting = S::anti_aliasing,
     .spec = changing(kit::levels("anti-aliasing", "Enhanced anti-aliasing",
                                  LevelStrip<AntiAliasing, 5, anti_aliasing_levels, &E::anti_aliasing, kAntiAliasingCaptions, &anti_aliasing_word>::stepper(),
                                  level_width),
                      &anti_aliasing_hint, &lines<1>)},
    {.setting = S::screen_size,
     .spec = slider("screen-size", "Screen size",
                    {&screen_size_stops, &screen_size_stop, &set_screen_size_stop, &screen_size_caption, nullptr},
                    {"Full screen at this size, or a window of it. Applies when you press OK."}),
     .copy = &copy_field<&E::screen_size>},
    {.setting = S::developer_mode,
     .spec = engine_switch<&E::developer_mode>("developer-mode", "Enable Developer Mode",
                                               {"Your changes to the profile's hacks apply while on.", {}})},
    {.setting = S::frame_stats,
     .spec = engine_switch<&E::frame_stats>("frame-stats", "Show performance statistics",
                                            {"Frame and tick times over the battlefield.", {}})},
    {.setting = S::hardware_acceleration,
     .spec = hardware_acceleration_row(),
     .lock = &Locks::hardware_acceleration},
    {.setting = S::vertical_sync,
     .spec = engine_switch<&E::vertical_sync>("vertical-sync", "Vertical sync",
                                              {"Each frame waits for the display: no tearing.", {}}),
     .lock = &Locks::vertical_sync},
    {.setting = S::language,
     .spec = language_row(),
     .lock = &Locks::language,
     .copy = &copy_field<&E::language>},
    {.setting = S::modern_fonts,
     .spec = kit::toggle("modern-fonts", "Use modern fonts for game text", &engine_flag<&E::modern_fonts>, &set_modern_fonts,
                         {"Modern fonts for in-game text, including internationalization."}),
     .lock = &Locks::modern_fonts},
    {.setting = S::text_outline,
     .spec = engine_switch<&E::text_outline>("text-outline", "Font outline",
                                             {"A dark edge round each letter of modern text.", {}})},
    {.setting = S::text_shadow,
     .spec = engine_switch<&E::text_shadow>("text-shadow", "Font shadow",
                                            {"A dark shadow under modern text.", {}})},
    {.setting = S::text_background,
     .spec = engine_switch<&E::text_background>("text-background", "Game text background",
                                                {"A shaded box behind each line of game text.", {}})},
    {.setting = S::unicode_chat,
     .spec = engine_switch<&E::unicode_chat>("unicode-chat", "Enable Unicode Multiplayer Chat",
                                             {"Chat in any language with players who have it; others see ? for letters they lack."}),
     .lock = &Locks::unicode_chat},
    {.setting = S::text_size,
     .spec = changing(linear<TextSize, &text_size_text>("text-size", "Text size", {}),
                      &text_size_hint, &lines<1>),
     .lock = &Locks::text_size,
     .copy = &copy_field<&E::text_size>},
    {.setting = S::touch_drag,
     .spec = changing(kit::levels("touch-drag", "One-finger drag",
                                  LevelStrip<TouchDrag, 3, touch_drag_choices, &E::touch_drag, kTouchDragCaptions, &text_word<&touch_drag_text, TouchDrag>>::stepper(),
                                  touch_drag_level_width),
                      &touch_drag_hint, &lines<1>)},
    {.setting = S::touch_hold_delay,
     .spec = linear<HoldDelay, &hold_delay_text>("touch-hold-delay", "Hold delay",
                                                 {"How long a finger or button is held for a hold.", {}}),
     .copy = &copy_field<&E::touch_hold_ms>},
    {.setting = S::touch_latches,
     .spec = changing(kit::levels("touch-latches", "QUEUE and ADD",
                                  LevelStrip<TouchLatches, 2, touch_latches_choices, &E::touch_latches, kTouchLatchesCaptions, &text_word<&touch_latches_text, TouchLatches>>::stepper(),
                                  touch_latches_level_width),
                      &touch_latches_hint, &lines<1>)},
    {.setting = S::touch_haptics,
     .spec = engine_switch<&E::touch_haptics>("touch-haptics", "Haptics",
                                              {"A short vibration as a touch control acts.", {}})},
    {.setting = S::touch_left_handed,
     .spec = engine_switch<&E::touch_left_handed>("touch-left-handed", "Left-handed layout",
                                                  {"The minimap and the thumb controls on the right, the orders on the left."})},
    {.setting = S::touch_control_size,
     .spec = kit::levels("touch-control-size", "Control size",
                         LevelStrip<ControlSize, 3, control_size_choices, &E::touch_control_size, kControlSizeCaptions, &table_word<control_size_words, ControlSize>>::stepper(),
                         control_size_level_width,
                         {"The size of the touch controls; the game's own screens keep theirs."})},
    {.setting = S::pad_scheme,
     .spec = changing(kit::levels("pad-scheme", "Scheme",
                                  LevelStrip<pad_controls::Scheme, 2, kSchemeChoices, &E::pad_scheme, kSchemeCaptions, &table_word<scheme_words, pad_controls::Scheme>>::stepper(),
                                  scheme_level_width),
                      &pad_scheme_hint, &lines<1>)},
    {.setting = S::pad_right_trackpad,
     .spec = changing(kit::levels("pad-right-trackpad", "Right trackpad",
                                  LevelStrip<pad_controls::RightTrackpad, 2, kRightTrackpadChoices, &E::pad_right_trackpad, kRightTrackpadCaptions, &table_word<right_trackpad_words, pad_controls::RightTrackpad>>::stepper(),
                                  right_trackpad_level_width),
                      &pad_right_trackpad_hint, &lines<1>)},
    {.setting = S::pad_pointer_speed,
     .spec = linear<PointerSpeed, &pointer_speed_text>("pad-pointer-speed", "Pointer speed",
                                                       {"How far the pointer moves for a slide.", {}}),
     .copy = &copy_field<&E::pad_pointer_speed>},
    {.setting = S::pad_acceleration,
     .spec = kit::levels("pad-acceleration", "Pointer acceleration",
                         LevelStrip<pad_controls::Acceleration, 3, kAccelerationChoices, &E::pad_acceleration, kPadAccelerationCaptions, &table_word<acceleration_words, pad_controls::Acceleration>>::stepper(),
                         pad_acceleration_level_width,
                         {"A quick slide moves the pointer further.", {}})},
    {.setting = S::pad_glide,
     .spec = engine_switch<&E::pad_glide>("pad-glide", "Trackpad glide",
                                          {"The pointer keeps moving after a quick flick.", {}})},
    {.setting = S::pad_right_stick,
     .spec = changing(kit::levels("pad-right-stick", "Right stick",
                                  LevelStrip<pad_controls::RightStick, 3, kRightStickChoices, &E::pad_right_stick, kRightStickCaptions, &table_word<right_stick_words, pad_controls::RightStick>>::stepper(),
                                  right_stick_level_width),
                      &pad_right_stick_hint, &lines<1>)},
    {.setting = S::pad_magnetism,
     .spec = engine_switch<&E::pad_magnetism>("pad-magnetism", "Magnetism (stick pointer)",
                                              {"The stick pointer settles on a lone unit near it.", {}})},
    {.setting = S::pad_gyro,
     .spec = wide(kit::choice("pad-gyro", "Gyro pointer",
                              DropDown<pad_controls::Gyro, 4, kGyroChoices, &E::pad_gyro, kGyroCaptions, &table_word<gyro_words, pad_controls::Gyro>>::stepper(),
                              {"Turning the controller fine-tunes the pointer.", {}}),
                  wide_choice_width)},
    {.setting = S::pad_gyro_speed,
     .spec = linear<GyroSpeed, &gyro_speed_text>("pad-gyro-speed", "Gyro speed",
                                                 {"How far the pointer moves as the controller turns.", {}}),
     .copy = &copy_field<&E::pad_gyro_speed>},
    {.setting = S::pad_haptics,
     .spec = kit::levels("pad-haptics", "Haptics",
                         LevelStrip<pad_controls::Haptics, 3, kHapticsChoices, &E::pad_haptics, kPadHapticsCaptions, &table_word<haptics_words, pad_controls::Haptics>>::stepper(),
                         pad_haptics_level_width,
                         {"Small ticks and bumps felt through the controller.", {}})},
    {.setting = S::pad_prompts,
     .spec = wide(kit::choice("pad-prompts", "Button prompts",
                              DropDown<pad_controls::Prompts, 6, kPromptsChoices, &E::pad_prompts, kPromptsCaptions, &table_word<prompts_words, pad_controls::Prompts>>::stepper(),
                              {"The button pictures the controls and rings show.", {}}),
                  choice_width)},
    {.setting = S::pad_left_handed,
     .spec = engine_switch<&E::pad_left_handed>("pad-left-handed", "Left-handed",
                                                {"Mirrors the roles: the left pad points, the right pad moves the map, triggers and grips swap."})},
    {.setting = S::pad_steam_input_notice,
     .spec = steam_input_notice_row()},
    {.setting = S::snap_override_key,
     .spec = slider("snap-override-key", "Snap override key", KeySlider<&O::snap_override_key>::stepper(),
                    {"Held, a click is not snapped.", {}}),
     .copy = &copy_option<&O::snap_override_key>},
    {.setting = S::autoclick_key,
     .spec = slider("autoclick-key", "Autoclick key", KeySlider<&O::autoclick_key>::stepper(),
                    {"Held, a build click lays a line or a ring.", {}}),
     .copy = &copy_option<&O::autoclick_key>},
    {.setting = S::rotate_build_key,
     .spec = slider("rotate-build-key", "Rotate build key", KeySlider<&O::rotate_build_key>::stepper(),
                    {"Turns the building being placed.", {}}),
     .copy = &copy_option<&O::rotate_build_key>},
    {.setting = S::patrol_hold,
     .spec = builders_row("patrol-hold", "Hold position", PatrolHold::stepper(), kPatrolHint),
     .copy = &PatrolHold::copy},
    {.setting = S::patrol_maneuver,
     .spec = builders_row("patrol-maneuver", "Maneuver", PatrolManeuver::stepper(), kPatrolHint),
     .copy = &PatrolManeuver::copy},
    {.setting = S::patrol_roam,
     .spec = builders_row("patrol-roam", "Roam", PatrolRoam::stepper(), kPatrolHint),
     .copy = &PatrolRoam::copy},
    {.setting = S::guard_hold,
     .spec = builders_row("guard-hold", "Hold position", GuardHold::stepper(), kGuardHint),
     .copy = &GuardHold::copy},
    {.setting = S::guard_maneuver,
     .spec = builders_row("guard-maneuver", "Maneuver", GuardManeuver::stepper(), kGuardHint),
     .copy = &GuardManeuver::copy},
    {.setting = S::guard_roam,
     .spec = builders_row("guard-roam", "Roam", GuardRoam::stepper(), kGuardHint),
     .copy = &GuardRoam::copy},
    {.setting = S::mex_snap_radius,
     .spec = slider("mex-snap-radius", "Mex snap radius", MexSnap::stepper(),
                    {"Cells a metal extractor snaps to metal.", {}}),
     .lock = &Locks::mex_snap,
     .copy = &copy_option<&O::mex_snap_radius>},
    {.setting = S::wreck_snap_radius,
     .spec = slider("wreck-snap-radius", "Wreck snap radius", WreckSnap::stepper(),
                    {"Cells a reclaim click snaps to a wreck.", {}}),
     .lock = &Locks::wreck_snap,
     .copy = &copy_option<&O::wreck_snap_radius>},
    {.setting = S::optimize_dt_rows,
     .spec = option_switch<&O::optimize_dt_rows>("optimize-dt-rows", "Optimize DT rows",
                                                 {"Lines of 2x2 walls build as a double row.", {}})},
    {.setting = S::full_rings,
     .spec = option_switch<&O::full_rings>("full-rings", "Full rings",
                                           {"Rings include their corners.", {}})},
    {.setting = S::chat_backdrop,
     .spec = option_switch<&O::chat_backdrop>("chat-backdrop", "Accessible chat",
                                              {"A dark backdrop under each chat line.", {}})},
    {.setting = S::panel_background,
     .spec = slider("panel-background", "Resource bar background", PanelBackground::stepper(),
                    {"Behind the resource bar's text.", {}}),
     .copy = &PanelBackground::copy},
    {.setting = S::game_files_summary,
     .spec = game_files_summary_row()},
    {.setting = S::game_files_backed_up,
     .spec = changing(engine_switch<&E::game_files_backed_up>("game-files-backed-up", "Include in device backups", kBackupsHint),
                      &backups_hint, &lines<1>)},
    {.setting = S::game_files_location,
     .spec = game_files_location_row()},
    {.setting = S::user_folder,
     .spec = user_folder_row(),
     .path_hint = true},
    {.setting = S::menu_scaling,
     .spec = changing(kit::levels("menu-scaling", "Menu scaling",
                                  LevelStrip<MenuScaling, 3, menu_scaling_choices, &E::menu_scaling, kMenuScalingCaptions, &text_word<&menu_scaling_text, MenuScaling>>::stepper(),
                                  menu_scaling_level_width),
                      &menu_scaling_hint, &lines<1>)},
    {.setting = S::native_density,
     .spec = engine_switch<&E::native_density>("native-density", "Native pixel density",
                                               {"The display's own pixel density (Retina on a Mac). Applies from the next start."}),
     .lock = &Locks::native_density},
    {.setting = S::explosion_flash,
     .spec = changing(kit::levels("explosion-flash", "Explosion flash",
                                  LevelStrip<ExplosionFlash, 3, explosion_flash_choices, &E::explosion_flash, kExplosionFlashCaptions, &table_word<explosion_flash_words, ExplosionFlash>>::stepper(),
                                  explosion_flash_level_width),
                      &explosion_flash_hint, &lines<1>)},
    {.setting = S::zoomed_out_units,
     .spec = changing(kit::levels("zoomed-out-units", "Zoomed out units",
                                  LevelStrip<ZoomedOutUnits, 3, zoomed_out_units_choices, &E::zoomed_out_units, kZoomedOutUnitsCaptions, &zoomed_out_units_word, offered_zoomed_out_units>::stepper(),
                                  zoomed_out_units_level_width),
                      &zoomed_out_units_hint, &lines<1>),
     .copy = &copy_field<&E::zoomed_out_units>},
    {.setting = S::zoomed_out_after,
     .spec = changing(wide(kit::choice("zoomed-out-after", "After zoom",
                                       DropDown<ZoomedOutAfter, 7, zoomed_out_afters, &E::zoomed_out_after, kZoomedOutAfterCaptions, &table_word<zoomed_out_after_words, ZoomedOutAfter>>::stepper()),
                           choice_width),
                      &zoomed_out_after_hint, &lines<1>),
     .lock = &Locks::zoomed_out_after},
    {.setting = S::window_frame,
     .spec = changing(kit::levels("window-frame", "Window frame",
                                  LevelStrip<WindowFrame, 2, window_frame_choices, &E::window_frame, kWindowFrameCaptions, &table_word<window_frame_words, WindowFrame>>::stepper(),
                                  window_frame_level_width),
                      &window_frame_hint, &lines<1>)},
    {.setting = S::hud_scaling,
     .spec = changing(engine_switch<&E::hud_scaling>("hud-scaling", "HUD scaling", {}),
                      &hud_scaling_hint, &lines<1>)},
    {.setting = S::interface_size,
     .spec = wide(kit::choice("interface-size", "Interface size", kInterfaceSizeStepper,
                              {"How large Open Annihilation's own screens are drawn. Auto suits the window's height."}),
                  choice_width)},
    {.setting = S::more_in_library,
     .spec = more_in_library_row()},
    {.setting = S::kit_gallery,
     .spec = kit_gallery_row()},
    {.setting = S::file_type_registration,
     .spec = file_type_heading_row()},
    {.setting = S::file_type_oamod,
     .spec = engine_switch<&E::file_type_oamod>("file-type-oamod", "Mod packages (.oamod)", {})},
    {.setting = S::file_type_oamap,
     .spec = engine_switch<&E::file_type_oamap>("file-type-oamap", "Map packs (.oamap)", {})},
    {.setting = S::file_type_oalang,
     .spec = engine_switch<&E::file_type_oalang>("file-type-oalang", "Language packs (.oalang)", {})},
    {.setting = S::file_type_oareg,
     .spec = engine_switch<&E::file_type_oareg>("file-type-oareg", "Registry files (.oareg)", {})},
    {.setting = S::file_type_oamut,
     .spec = engine_switch<&E::file_type_oamut>("file-type-oamut", "Mutator packages (.oamut)", {})},
    {.setting = S::file_type_oabrain,
     .spec = engine_switch<&E::file_type_oabrain>("file-type-oabrain", "Brain packages (.oabrain)", {})},
    {.setting = S::file_type_oalaunch,
     .spec = engine_switch<&E::file_type_oalaunch>("file-type-oalaunch", "Launch files (.oalaunch)", {})},
    {.setting = S::file_types_note,
     .spec = file_types_note_row()},
    {.setting = S::skirmish_players,
     .spec = linear<SkirmishPlayers, &skirmish_players_text>(
         "skirmish-players",
         "Skirmish players",
         {"The skirmish setup shows that many player rows.", {}}
     ),
     .lock = &Locks::skirmish_players,
     .copy = &copy_field<&E::skirmish_players>},
    {.setting = S::smooth_motion,
     .spec = changing(engine_switch<&E::smooth_motion>("smooth-motion", "Smooth motion", {}),
                      &smooth_motion_hint, &lines<1>)},
    {.setting = S::welcome_setup,
     .spec = welcome_setup_row(),
     .lock = &Locks::welcome_setup},
    {.setting = S::fog_of_war,
     .spec = changing(kit::levels("fog-of-war", "Fog of War",
                                  LevelStrip<FogOfWar, 2, fog_of_war_choices, &E::fog_of_war, kFogOfWarCaptions, &table_word<fog_of_war_words, FogOfWar>>::stepper(),
                                  fog_of_war_level_width),
                      &fog_of_war_hint, &lines<1>),
     .copy = &copy_field<&E::fog_of_war>},
}};
// clang-format on

/// Every setting's name, in kebab case, in Setting's order: each its row's id.
constexpr std::array<std::string_view, kSettingCount> kSettingNames{
    "path-search",
    "wheel-zoom",
    "max-zoom-out",
    "max-zoom-in",
    "view-past-map-edge",
    "middle-click-pan",
    "escape-opens-menu",
    "switch-alt",
    "window-edge",
    "window-edge-stickiness",
    "unit-limit",
    "max-frame-rate",
    "anti-aliasing",
    "screen-size",
    "developer-mode",
    "frame-stats",
    "hardware-acceleration",
    "vertical-sync",
    "language",
    "modern-fonts",
    "text-outline",
    "text-shadow",
    "text-background",
    "unicode-chat",
    "text-size",
    "touch-drag",
    "touch-hold-delay",
    "touch-latches",
    "touch-haptics",
    "touch-left-handed",
    "touch-control-size",
    "pad-scheme",
    "pad-right-trackpad",
    "pad-pointer-speed",
    "pad-acceleration",
    "pad-glide",
    "pad-right-stick",
    "pad-magnetism",
    "pad-gyro",
    "pad-gyro-speed",
    "pad-haptics",
    "pad-prompts",
    "pad-left-handed",
    "pad-steam-input-notice",
    "snap-override-key",
    "autoclick-key",
    "rotate-build-key",
    "patrol-hold",
    "patrol-maneuver",
    "patrol-roam",
    "guard-hold",
    "guard-maneuver",
    "guard-roam",
    "mex-snap-radius",
    "wreck-snap-radius",
    "optimize-dt-rows",
    "full-rings",
    "chat-backdrop",
    "panel-background",
    "game-files-summary",
    "game-files-backed-up",
    "game-files-location",
    "user-folder",
    "menu-scaling",
    "native-density",
    "explosion-flash",
    "zoomed-out-units",
    "zoomed-out-after",
    "window-frame",
    "hud-scaling",
    "interface-size",
    "more-in-library",
    "kit-gallery",
    "file-type-registration",
    "file-type-oamod",
    "file-type-oamap",
    "file-type-oalang",
    "file-type-oareg",
    "file-type-oamut",
    "file-type-oabrain",
    "file-type-oalaunch",
    "file-types",
    "skirmish-players",
    "smooth-motion",
    "first-run",
    "fog-of-war",
};

/// Tells whether every row stands at its setting's place and is named by
/// its setting.
///
/// @return true when each row's setting is its index and its id its name
constexpr bool rows_in_order() noexcept {
    for (std::size_t index = 0; index < kDeclared.size(); ++index)
        if (static_cast<std::size_t>(kDeclared[index].setting) != index ||
            kDeclared[index].spec.id != kSettingNames[index])
            return false;
    return true;
}

static_assert(
    kDeclared.size() == kSettingCount, "the table has a row for every enumerator of Setting"
);
static_assert(rows_in_order(), "each row stands at its setting's place, its id its name");

/// Returns the lock the dialog shows on a setting's row: its field of the
/// locks the dialog shows (shown_locks), as a check's own section has it.
template <Setting Row>
std::string_view row_lock_text(const SettingsModel& model) {
    if (model.dialog == nullptr)
        return {};
    const Dialog& dialog = *model.dialog;
    return lock_text(section_lock(shown_locks(dialog), Row, dialog.section_hooks));
}

/// Gives every row's spec its lock: the lock the dialog shows on it.
template <std::size_t... Index>
constexpr std::array<SettingRow, kSettingCount>
locked_rows(std::array<SettingRow, kSettingCount> rows, std::index_sequence<Index...>) noexcept {
    ((rows[Index].spec.lock = &row_lock_text<static_cast<Setting>(Index)>), ...);
    return rows;
}

/// The table, each row's spec locked as the dialog shows it.
constexpr std::array<SettingRow, kSettingCount> kRows =
    locked_rows(kDeclared, std::make_index_sequence<kSettingCount>{});

/// Returns a setting's row.
///
/// @param setting the setting
/// @return its row; the last for a value past Setting's enumerators
const SettingRow& entry(Setting setting) noexcept {
    return kRows[std::min(static_cast<std::size_t>(setting), kRows.size() - 1)];
}

/// Returns a dialog that offers loose settings what a dialog offers its
/// rows: the unit limit slider's highest stop, the Screen size slider's
/// stops, Hardware acceleration's status and what it shows beyond each
/// setting's rows.
///
/// @param highest_offered_unit the unit limit slider's highest stop
/// @param offered_sizes the Screen size slider's stops
/// @param acceleration Hardware acceleration's status
/// @param context the Steam Input notice and a Steam Deck's rate
/// @return the dialog
Dialog offering(
    uint16_t highest_offered_unit = highest_unit_limit,
    std::span<const ScreenSize> offered_sizes = screen_sizes,
    const AccelerationStatus& acceleration = {},
    const RowContext& context = {}
) {
    Dialog dialog;
    dialog.highest_offered_unit = highest_offered_unit;
    dialog.offered_screen_sizes.assign(offered_sizes.begin(), offered_sizes.end());
    dialog.acceleration = acceleration;
    dialog.steam_input = context.steam_input;
    dialog.steam_deck_panel_hz = context.steam_deck_panel_hz;
    dialog.file_type_switches = context.file_type_switches;
    return dialog;
}

/// The settings of the widest sliders: each snap radius up to the most any
/// mod allows.
EngineSettings widest_settings() noexcept {
    EngineSettings settings{};
    settings.mod_options.mex_snap_most = most_snap_radius;
    settings.mod_options.wreck_snap_most = most_snap_radius;
    return settings;
}

/// Returns a stop or an item given as an unsigned number, held to what a
/// row's index can be.
///
/// @param index the stop or item
/// @return the index
int32_t index_of(std::size_t index) noexcept {
    return static_cast<int32_t>(std::min<std::size_t>(index, INT32_MAX));
}

/// Returns a line of a row's hint as its row gives it: in English, or, for
/// a host's texts, as the host gave them; and whether it is a notice.
///
/// @param model the row's model
/// @param setting the row's setting
/// @param line the line, from 0
/// @return the line; empty past the last
HintLine hint_of(const SettingsModel& model, Setting setting, std::size_t line) {
    const Spec& spec = entry(setting).spec;
    std::string text;
    if (spec.hint_text != nullptr)
        text = spec.hint_text(model, line);
    else if (line < spec.hint.size())
        text = std::string(spec.hint[line]);
    return {std::move(text), spec.hint_notice != nullptr && spec.hint_notice(model, line)};
}

/// Returns a slider's value as its row shows it.
///
/// @param model the row's model
/// @param setting the row's setting
/// @return the text; empty for a row that is no slider
std::string slider_text(const SettingsModel& model, Setting setting) {
    const Spec& spec = entry(setting).spec;
    if (spec.kind != kit::RowKind::slider)
        return {};
    return kit::row_caption(spec, model, kit::row_index(spec, model));
}

} // namespace

// ---------------------------------------------------------------------------
// The table's own functions.

const kit::RowSpec<SettingsModel>& row_spec(Setting setting) noexcept {
    return entry(setting).spec;
}

kit::RowKind kind_of(Setting setting) noexcept {
    return entry(setting).spec.kind;
}

std::string_view setting_name(Setting setting) noexcept {
    return kSettingNames[std::min(static_cast<std::size_t>(setting), kSettingNames.size() - 1)];
}

Lock lock_of(const Locks& locks, Setting setting) noexcept {
    const SettingRow& row = entry(setting);
    return row.lock != nullptr ? locks.*row.lock : Lock::none;
}

Lock section_lock(const Locks& locks, Setting setting, const SectionHooks* section) {
    const Lock lock = lock_of(locks, setting);
    if (section == nullptr || section->lock == nullptr)
        return lock;
    return section->lock(section->context, setting, lock);
}

bool section_hint_is_status(Setting setting, const SectionHooks* section) {
    if (section == nullptr || section->hint_is_status == nullptr)
        return hint_is_status(setting);
    return section->hint_is_status(section->context, setting);
}

bool hint_is_path(Setting setting) noexcept {
    return entry(setting).path_hint;
}

void copy_row(Setting setting, const SettingsModel& to, const SettingsModel& from) {
    const SettingRow& row = entry(setting);
    if (row.copy != nullptr) {
        row.copy(to, from);
        return;
    }
    SettingsModel target = to;
    switch (row.spec.kind) {
    case kit::RowKind::toggle:
        kit::set_row_on(row.spec, target, kit::row_on(row.spec, from));
        break;
    case kit::RowKind::choice:
    case kit::RowKind::slider:
    case kit::RowKind::levels:
        kit::set_row_index(row.spec, target, kit::row_index(row.spec, from));
        break;
    case kit::RowKind::value_and_button:
    case kit::RowKind::buttons:
    case kit::RowKind::text_field:
    case kit::RowKind::link:
    case kit::RowKind::text:
        break;
    }
}

std::string_view page_word(Page page) noexcept {
    return kPageWords[std::min(static_cast<std::size_t>(page), kPageWords.size() - 1)];
}

std::string word_form(std::string_view text, bool keep_dots) {
    std::string word;
    for (std::size_t at = 0; at < text.size();) {
        const auto byte = static_cast<unsigned char>(text[at]);
        const std::size_t bytes = std::max<std::size_t>(kit::character_bytes(text.substr(at)), 1);
        at += bytes;
        if (byte >= 'A' && byte <= 'Z')
            word += static_cast<char>(byte - 'A' + 'a');
        else if (
            (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '-' ||
            (keep_dots && byte == '.')
        )
            word += static_cast<char>(byte);
        else
            word += '-';
    }
    return word;
}

std::string
choice_word(const kit::RowSpec<SettingsModel>& spec, const SettingsModel& model, int32_t at) {
    const std::string id = word_form(kit::row_choice_id(spec, model, at), false);
    return id.empty() ? std::to_string(at + 1) : id;
}

std::string_view status_line(const AccelerationStatus& acceleration, std::size_t line) noexcept {
    const auto index = static_cast<std::size_t>(acceleration.state);
    if (index >= kStatusTexts.size())
        return {};
    const StatusText& text = kStatusTexts[index];
    if (line == 0)
        return acceleration.state == AccelerationState::waiting_for_game_end ||
                       acceleration.state == AccelerationState::full_waiting_for_game_end
                   ? waiting_line(acceleration)
                   : text.first;
    if (line == 1) {
        if (full_in_use(acceleration.state))
            return full_line(acceleration.supersample);
        return text.second.empty() ? reach_line(acceleration.reach) : text.second;
    }
    return {};
}

std::string_view page_name(Page page) noexcept {
    return kPageNames[std::min(static_cast<std::size_t>(page), kPageNames.size() - 1)];
}

std::string_view page_heading(Page page) noexcept {
    return kPageHeadings[std::min(static_cast<std::size_t>(page), kPageHeadings.size() - 1)];
}

std::span<const Setting>
section_settings(Page page, const SectionHooks* section, const RowContext& context) {
    if (section == nullptr || section->settings == nullptr) {
        if (page == Page::system)
            return context.file_type_switches ? std::span<const Setting>(kSystemRuntimeRows)
                                              : std::span<const Setting>(kSystemBundleRows);
        if (page == Page::controller && context.steam_input)
            return std::span<const Setting>(kSteamInputControllerRows);
        if (page == Page::language && context.library_link)
            return std::span<const Setting>(kLanguageLibraryRows);
        return page_settings(page);
    }
    return section->settings(section->context, page);
}

std::span<const oa::data::languages::Language* const> offered_languages() {
    static std::vector<const oa::data::languages::Language*> offered;
    static uint64_t built_at = 0;
    static bool built = false;
    const uint64_t generation = languages::registry_generation();
    if (!built || built_at != generation) {
        offered.clear();
        for (const languages::Language* language : languages::known_languages())
            if (languages::playable(*language))
                offered.push_back(language);
        built_at = generation;
        built = true;
    }
    return {offered.data(), offered.size()};
}

// ---------------------------------------------------------------------------
// The geometry functions that read a row: each reads the table.

Slider slider_of(
    Setting setting, uint16_t highest_offered_unit, std::span<const ScreenSize> offered_sizes
) noexcept {
    const auto& spec = row_spec(setting);
    if (spec.kind != kit::RowKind::slider)
        return Slider{2};
    const Dialog offers = offering(highest_offered_unit, offered_sizes);
    const EngineSettings widest = widest_settings();
    return Slider{kit::row_count(spec, reading(widest, offers))};
}

int32_t stops_of(
    const EngineSettings& settings,
    Setting setting,
    uint16_t highest_offered_unit,
    std::span<const ScreenSize> offered_sizes
) noexcept {
    const Dialog offers = offering(highest_offered_unit, offered_sizes);
    return kit::row_count(row_spec(setting), reading(settings, offers));
}

int32_t stop_of(
    const EngineSettings& settings,
    Setting setting,
    uint16_t highest_offered_unit,
    std::span<const ScreenSize> offered_sizes
) noexcept {
    const Dialog offers = offering(highest_offered_unit, offered_sizes);
    return kit::row_index(row_spec(setting), reading(settings, offers));
}

void set_stop(
    EngineSettings& settings,
    Setting setting,
    int32_t stop,
    uint16_t highest_offered_unit,
    std::span<const ScreenSize> offered_sizes
) noexcept {
    const Dialog offers = offering(highest_offered_unit, offered_sizes);
    SettingsModel model{&settings, &offers};
    kit::set_row_index(row_spec(setting), model, stop);
}

Strip strip_of(Setting setting) noexcept {
    const auto& spec = row_spec(setting);
    if (spec.kind != kit::RowKind::levels)
        return Strip{};
    const Dialog offers = offering();
    const EngineSettings settings{};
    const SettingsModel model = reading(settings, offers);
    const auto levels = static_cast<std::size_t>(kit::row_count(spec, model));
    const auto offered = static_cast<std::size_t>(kit::row_offered(spec, model));
    return Strip{levels, spec.control_width, offered == levels ? 0 : offered};
}

std::size_t strip_level(const EngineSettings& settings, Setting setting) noexcept {
    const Dialog offers = offering();
    return static_cast<std::size_t>(kit::row_index(row_spec(setting), reading(settings, offers)));
}

void set_strip_level(EngineSettings& settings, Setting setting, std::size_t level) noexcept {
    const Dialog offers = offering();
    SettingsModel model{&settings, &offers};
    kit::set_row_index(row_spec(setting), model, index_of(level));
}

std::string strip_caption(Setting setting, std::size_t level) {
    const auto& spec = row_spec(setting);
    if (spec.kind != kit::RowKind::levels)
        return {};
    const Dialog offers = offering();
    const EngineSettings settings{};
    return kit::row_caption(spec, reading(settings, offers), index_of(level));
}

std::size_t choice_count(const Dialog& dialog, Setting setting) {
    const auto& spec = row_spec(setting);
    if (spec.kind != kit::RowKind::choice)
        return 0;
    return static_cast<std::size_t>(kit::row_count(spec, reading(dialog.chosen, dialog)));
}

std::string choice_text(const Dialog& dialog, Setting setting, std::size_t index) {
    return kit::row_caption(row_spec(setting), reading(dialog.chosen, dialog), index_of(index));
}

std::size_t choice_index(const Dialog& dialog, Setting setting) {
    return static_cast<std::size_t>(
        kit::row_index(row_spec(setting), reading(dialog.chosen, dialog))
    );
}

void set_choice(Dialog& dialog, Setting setting, std::size_t index) {
    SettingsModel model{&dialog.chosen, &dialog};
    kit::set_row_index(row_spec(setting), model, index_of(index));
}

bool switch_on(const EngineSettings& settings, Setting setting) noexcept {
    const Dialog offers = offering();
    return kit::row_on(row_spec(setting), reading(settings, offers));
}

void set_switch(EngineSettings& settings, Setting setting, bool on) noexcept {
    const Dialog offers = offering();
    SettingsModel model{&settings, &offers};
    kit::set_row_on(row_spec(setting), model, on);
}

bool hint_is_status(Setting setting) noexcept {
    return row_spec(setting).hint_is_status;
}

std::string_view label_of(Setting setting) noexcept {
    return row_spec(setting).label;
}

std::string hint_line(
    Setting setting,
    const EngineSettings& settings,
    const AccelerationStatus& acceleration,
    std::size_t line
) {
    const Dialog offers = offering(highest_unit_limit, screen_sizes, acceleration);
    return hint_of(reading(settings, offers), setting, line).text;
}

std::size_t hint_line_count(Setting setting) noexcept {
    return hint_line_count(setting, RowContext{});
}

std::size_t hint_line_count(Setting setting, const RowContext& context) noexcept {
    const Dialog offers = offering(highest_unit_limit, screen_sizes, {}, context);
    const EngineSettings settings{};
    return kit::view_of(row_spec(setting), reading(settings, offers), {}).hints.size();
}

HintLine row_hint(const Dialog& dialog, Setting setting, std::size_t line) {
    return hint_of(reading(dialog.chosen, dialog), setting, line);
}

std::string value_text(Setting setting, const EngineSettings& settings) {
    const Dialog offers = offering();
    return slider_text(reading(settings, offers), setting);
}

std::string value_text(Setting setting, const Dialog& dialog) {
    return slider_text(reading(dialog.chosen, dialog), setting);
}

} // namespace geometry

std::string kept_opener_hint(const std::span<const std::string_view> dotted) {
    if (dotted.empty())
        return {};
    // "and" is its own catalogue word, so a translation can say the list.
    const std::string and_word(geometry::shown_text("and"));
    std::string types(dotted[0]);
    if (dotted.size() == 2) {
        types += " ";
        types += and_word;
        types += " ";
        types += dotted[1];
    } else if (dotted.size() > 2) {
        for (std::size_t index = 1; index < dotted.size(); ++index) {
            if (index + 1 < dotted.size())
                types += ", ";
            else {
                types += " ";
                types += and_word;
                types += " ";
            }
            types += dotted[index];
        }
    }
    // One type is named where the player looks and where they act. Several
    // are named once; the rest of the sentence says to pick one of them, so
    // the whole instruction still fits under the switches.
    constexpr std::string_view one_type =
        "macOS kept Open Annihilation as the opener of {types}. To choose another, select a "
        "{types} file in Finder, then File › Get Info › Open with › Change All.";
    constexpr std::string_view several =
        "macOS kept Open Annihilation as the opener of {types}. To choose another, select one of "
        "them in Finder, then File › Get Info › Open with › Change All.";
    return geometry::filled(dotted.size() == 1 ? one_type : several, {{"types", types}});
}

std::span<const Setting> page_settings(Page page) noexcept {
    return geometry::kPageRows[std::min(static_cast<std::size_t>(page), page_count - 1)];
}

} // namespace oa::ui::engine_settings

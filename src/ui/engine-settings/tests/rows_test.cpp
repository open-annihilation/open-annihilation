// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Every setting's row, written out as literals: its kind, its label, the
// lines its hint takes without and with Controller's Steam Input notice and
// a Steam Deck's screen rate, the stops, levels or choices it offers, a
// strip's level width or a drop-down's field width, the levels a strip lets
// a player choose, whether its hint lines are its status, its lock while a
// game is in progress, and the field of the dialog's locks that locks it.
// Every row's id and every item's or level's id, each one word, the latter
// the word the preferences keep it as. Then the dialog's display list, on
// every section of every kind of dialog, at its top and its scroll end,
// unlocked and under a game's locks: every control named once, every
// control the dialog's layout lists there, and Tab in the dialog's focus
// order.

#include "geometry.hpp"

#include "oa/test/check.hpp"
#include "oa/ui/engine_settings.hpp"
#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/kit/rows.hpp"

#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

#include <array>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <set>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace settings = oa::ui::engine_settings;
namespace geometry = oa::ui::engine_settings::geometry;
namespace kit = oa::ui::kit;

using kit::RowKind;
using settings::Lock;
using settings::Locks;
using settings::Setting;

/// What a setting's row is, as the dialog shows it.
struct Expected {
    Setting setting{};      ///< the setting
    RowKind kind{};         ///< what its control is
    std::string_view label; ///< its label, in English
    std::size_t lines{};    ///< the lines its hint takes
    /// The lines its hint takes with the Steam Input notice and a Steam
    /// Deck's screen rate.
    std::size_t deck_lines{};
    /// A slider's stops, a strip's levels or a drop-down's choices; 0 for none.
    int32_t values{};
    int32_t width{};          ///< a strip's level width or a drop-down's field width; 0 for none
    int32_t offered{};        ///< the levels of a strip a player may choose; 0 for none
    bool hint_is_status{};    ///< its hint lines are its status
    Lock in_game{};           ///< its lock while a game is in progress
    Lock Locks::* own_lock{}; ///< the field of the dialog's locks that locks it; null for none
};

// The rows, in Setting's order. A strip's offered levels and a drop-down's
// choices are those of a dialog of the engine's settings opened over the
// defaults: Language offers System default and the built-in languages.
// clang-format off
constexpr std::array<Expected, 99> kExpected{{
    {Setting::path_search, RowKind::slider, "Pathfinding cycles", 1, 1, 8, 0, 0, false, Lock::in_game, &Locks::path_search},
    {Setting::wheel_zoom, RowKind::toggle, "Mouse wheel zoom", 1, 1, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::max_zoom_out, RowKind::choice, "Maximum zoom out", 1, 1, 7, 200, 0, false, Lock::none, nullptr},
    {Setting::max_zoom_in, RowKind::choice, "Maximum zoom in", 1, 1, 4, 200, 0, false, Lock::none, nullptr},
    {Setting::view_past_map_edge, RowKind::levels, "View past the map's edge", 1, 1, 3, 34, 3, false, Lock::none, nullptr},
    {Setting::middle_click_pan, RowKind::toggle, "Middle-click pans", 1, 1, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::escape_opens_menu, RowKind::toggle, "Escape opens the game menu", 1, 1, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::switch_alt, RowKind::toggle, "Select groups without Alt", 1, 1, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::window_edge, RowKind::levels, "Window edge", 1, 1, 3, 50, 3, false, Lock::none, nullptr},
    {Setting::window_edge_stickiness, RowKind::slider, "Edge stickiness", 1, 1, 51, 0, 0, false, Lock::none, &Locks::edge_stickiness},
    {Setting::unit_limit, RowKind::slider, "Unit limit", 1, 1, 30, 0, 0, false, Lock::in_game, &Locks::unit_limit},
    {Setting::max_frame_rate, RowKind::slider, "Maximum frame rate", 1, 2, 19, 0, 0, false, Lock::none, &Locks::max_frame_rate},
    {Setting::anti_aliasing, RowKind::levels, "Enhanced anti-aliasing", 1, 1, 5, 23, 5, false, Lock::none, nullptr},
    {Setting::screen_size, RowKind::slider, "Screen size", 1, 1, 5, 0, 0, false, Lock::none, nullptr},
    {Setting::developer_mode, RowKind::toggle, "Enable Developer Mode", 1, 1, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::frame_stats, RowKind::toggle, "Show performance statistics", 1, 1, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::hardware_acceleration, RowKind::levels, "Hardware acceleration", 2, 2, 3, 34, 3, true, Lock::none, &Locks::hardware_acceleration},
    {Setting::vertical_sync, RowKind::toggle, "Vertical sync", 1, 1, 0, 0, 0, false, Lock::none, &Locks::vertical_sync},
    {Setting::language, RowKind::choice, "Language", 1, 1, 6, 200, 0, false, Lock::none, &Locks::language},
    {Setting::modern_fonts, RowKind::toggle, "Use modern fonts for game text", 1, 1, 0, 0, 0, false, Lock::none, &Locks::modern_fonts},
    {Setting::text_outline, RowKind::toggle, "Font outline", 1, 1, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::text_shadow, RowKind::toggle, "Font shadow", 1, 1, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::text_background, RowKind::toggle, "Game text background", 1, 1, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::unicode_chat, RowKind::toggle, "Enable Unicode Multiplayer Chat", 1, 1, 0, 0, 0, false, Lock::none, &Locks::unicode_chat},
    {Setting::text_size, RowKind::slider, "Text size", 1, 1, 26, 0, 0, false, Lock::none, &Locks::text_size},
    {Setting::touch_drag, RowKind::levels, "One-finger drag", 1, 1, 3, 59, 3, false, Lock::none, nullptr},
    {Setting::touch_hold_delay, RowKind::slider, "Hold delay", 1, 1, 10, 0, 0, false, Lock::none, nullptr},
    {Setting::touch_latches, RowKind::levels, "QUEUE and ADD", 1, 1, 2, 64, 2, false, Lock::none, nullptr},
    {Setting::touch_haptics, RowKind::toggle, "Haptics", 1, 1, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::touch_left_handed, RowKind::toggle, "Left-handed layout", 1, 1, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::touch_control_size, RowKind::levels, "Control size", 1, 1, 3, 53, 3, false, Lock::none, nullptr},
    {Setting::pad_scheme, RowKind::levels, "Scheme", 1, 1, 2, 60, 2, false, Lock::none, nullptr},
    {Setting::pad_right_trackpad, RowKind::levels, "Right trackpad", 1, 1, 2, 51, 2, false, Lock::none, nullptr},
    {Setting::pad_pointer_speed, RowKind::slider, "Pointer speed", 1, 1, 26, 0, 0, false, Lock::none, nullptr},
    {Setting::pad_acceleration, RowKind::levels, "Pointer acceleration", 1, 1, 3, 34, 3, false, Lock::none, nullptr},
    {Setting::pad_glide, RowKind::toggle, "Trackpad glide", 1, 1, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::pad_right_stick, RowKind::levels, "Right stick", 1, 1, 3, 46, 3, false, Lock::none, nullptr},
    {Setting::pad_magnetism, RowKind::toggle, "Magnetism (stick pointer)", 1, 1, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::pad_gyro, RowKind::choice, "Gyro pointer", 1, 1, 4, 248, 0, false, Lock::none, nullptr},
    {Setting::pad_gyro_speed, RowKind::slider, "Gyro speed", 1, 1, 36, 0, 0, false, Lock::none, nullptr},
    {Setting::pad_haptics, RowKind::levels, "Haptics", 1, 1, 3, 41, 3, false, Lock::none, nullptr},
    {Setting::pad_prompts, RowKind::choice, "Button prompts", 1, 1, 6, 200, 0, false, Lock::none, nullptr},
    {Setting::pad_left_handed, RowKind::toggle, "Left-handed", 1, 1, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::pad_steam_input_notice, RowKind::text, "Steam Input", 4, 4, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::snap_override_key, RowKind::slider, "Snap override key", 1, 1, 16, 0, 0, false, Lock::none, nullptr},
    {Setting::autoclick_key, RowKind::slider, "Autoclick key", 1, 1, 16, 0, 0, false, Lock::none, nullptr},
    {Setting::rotate_build_key, RowKind::slider, "Rotate build key", 1, 1, 16, 0, 0, false, Lock::none, nullptr},
    {Setting::patrol_hold, RowKind::slider, "Hold position", 1, 1, 3, 0, 0, false, Lock::none, nullptr},
    {Setting::patrol_maneuver, RowKind::slider, "Maneuver", 1, 1, 3, 0, 0, false, Lock::none, nullptr},
    {Setting::patrol_roam, RowKind::slider, "Roam", 1, 1, 3, 0, 0, false, Lock::none, nullptr},
    {Setting::guard_hold, RowKind::slider, "Hold position", 1, 1, 3, 0, 0, false, Lock::none, nullptr},
    {Setting::guard_maneuver, RowKind::slider, "Maneuver", 1, 1, 3, 0, 0, false, Lock::none, nullptr},
    {Setting::guard_roam, RowKind::slider, "Roam", 1, 1, 3, 0, 0, false, Lock::none, nullptr},
    {Setting::mex_snap_radius, RowKind::slider, "Mex snap radius", 1, 1, 2, 0, 0, false, Lock::none, &Locks::mex_snap},
    {Setting::wreck_snap_radius, RowKind::slider, "Wreck snap radius", 1, 1, 2, 0, 0, false, Lock::none, &Locks::wreck_snap},
    {Setting::optimize_dt_rows, RowKind::toggle, "Optimize DT rows", 1, 1, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::full_rings, RowKind::toggle, "Full rings", 1, 1, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::chat_backdrop, RowKind::toggle, "Accessible chat", 1, 1, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::panel_background, RowKind::slider, "Resource bar background", 1, 1, 3, 0, 0, false, Lock::none, nullptr},
    {Setting::game_files_summary, RowKind::value_and_button, "Installed", 2, 2, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::game_files_backed_up, RowKind::toggle, "Include in device backups", 1, 1, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::game_files_location, RowKind::text, "Where the files are", 2, 2, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::user_folder, RowKind::buttons, "Your files", 2, 2, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::menu_scaling, RowKind::levels, "Menu scaling", 1, 1, 3, 71, 3, false, Lock::none, nullptr},
    {Setting::native_density, RowKind::toggle, "Native pixel density", 1, 1, 0, 0, 0, false, Lock::none, &Locks::native_density},
    {Setting::explosion_flash, RowKind::levels, "Explosion flash", 1, 1, 3, 50, 3, false, Lock::none, nullptr},
    {Setting::zoomed_out_units, RowKind::levels, "Zoomed out units", 1, 1, 3, 56, 2, false, Lock::none, nullptr},
    {Setting::zoomed_out_after, RowKind::choice, "After zoom", 1, 1, 7, 200, 0, false, Lock::none, &Locks::zoomed_out_after},
    {Setting::window_frame, RowKind::levels, "Window frame", 1, 1, 2, 85, 2, false, Lock::none, nullptr},
    {Setting::hud_scaling, RowKind::toggle, "HUD scaling", 1, 1, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::interface_size, RowKind::choice, "Interface size", 1, 1, 5, 200, 0, false, Lock::none, nullptr},
    {Setting::more_in_library, RowKind::link, "", 1, 1, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::kit_gallery, RowKind::value_and_button, "UI kit gallery", 2, 2, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::file_type_registration, RowKind::text, "File Type Registration", 1, 1, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::file_type_oamod, RowKind::toggle, "Mod packages (.oamod)", 0, 0, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::file_type_oamap, RowKind::toggle, "Map packs (.oamap)", 0, 0, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::file_type_oalang, RowKind::toggle, "Language packs (.oalang)", 0, 0, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::file_type_oareg, RowKind::toggle, "Registry files (.oareg)", 0, 0, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::file_type_oamut, RowKind::toggle, "Mutator packages (.oamut)", 0, 0, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::file_type_oabrain, RowKind::toggle, "Brain packages (.oabrain)", 0, 0, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::file_type_oalaunch, RowKind::toggle, "Launch files (.oalaunch)", 0, 0, 0, 0, 0, false, Lock::none, nullptr},
#if defined(TARGET_OS_IPHONE) && TARGET_OS_IPHONE
    {Setting::file_types_note, RowKind::text, "File types", 1, 1, 0, 0, 0, false, Lock::none, nullptr},
#else
    {Setting::file_types_note, RowKind::text, "File types", 2, 2, 0, 0, 0, false, Lock::none, nullptr},
#endif
    {Setting::skirmish_players, RowKind::slider, "Skirmish players", 1, 1, 9, 0, 0, false, Lock::in_game, &Locks::skirmish_players},
    {Setting::smooth_motion, RowKind::toggle, "Smooth motion", 1, 1, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::weapon_lighting, RowKind::slider, "Coloured weapon lighting", 0, 0, 21, 0, 0, false, Lock::none, nullptr},
    {Setting::nanolathe_lighting, RowKind::slider, "Nanolathe glow", 0, 0, 21, 0, 0, false, Lock::none, nullptr},
    {Setting::wreck_glow, RowKind::slider, "Wreck glow", 0, 0, 21, 0, 0, false, Lock::none, nullptr},
    {Setting::heat_distortion, RowKind::slider, "Heat distortion", 0, 0, 21, 0, 0, false, Lock::none, nullptr},
    {Setting::water_ripples, RowKind::slider, "Water ripples", 0, 0, 21, 0, 0, false, Lock::none, &Locks::water_effects},
    {Setting::coastal_waves, RowKind::slider, "Coastal waves", 0, 0, 21, 0, 0, false, Lock::none, &Locks::water_effects},
    {Setting::underwater_shimmer, RowKind::slider, "Underwater shimmer", 0, 0, 21, 0, 0, false, Lock::none, &Locks::water_effects},
    {Setting::naval_reflections, RowKind::slider, "Naval reflections", 0, 0, 21, 0, 0, false, Lock::none, &Locks::water_effects},
    {Setting::allow_full_3d, RowKind::toggle, "Allow full 3D mode", 1, 1, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::unlocked_camera, RowKind::toggle, "Unlocked camera", 1, 1, 0, 0, 0, false, Lock::none, &Locks::unlocked_camera},
    {Setting::welcome_setup, RowKind::value_and_button, "Welcome setup", 2, 2, 0, 0, 0, false, Lock::main_menu, &Locks::welcome_setup},
    {Setting::fog_of_war, RowKind::levels, "Fog of War", 1, 1, 2, 56, 2, false, Lock::none, nullptr},
    {Setting::mod_limits_heading, RowKind::text, "Set by the mod", 0, 0, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::weapon_types, RowKind::text, "Weapon types", 1, 1, 0, 0, 0, false, Lock::none, nullptr},
    {Setting::sides, RowKind::text, "Sides", 1, 1, 0, 0, 0, false, Lock::none, nullptr},
}};
// clang-format on

static_assert(
    kExpected.size() == static_cast<std::size_t>(Setting::sides) + 1, "every setting has its row"
);

/// Every field of the dialog's locks that locks a row.
constexpr std::array<Lock Locks::*, 18> kLockFields{
    &Locks::path_search,
    &Locks::unit_limit,
    &Locks::skirmish_players,
    &Locks::max_frame_rate,
    &Locks::hardware_acceleration,
    &Locks::vertical_sync,
    &Locks::mex_snap,
    &Locks::wreck_snap,
    &Locks::text_size,
    &Locks::language,
    &Locks::modern_fonts,
    &Locks::unicode_chat,
    &Locks::native_density,
    &Locks::zoomed_out_after,
    &Locks::edge_stickiness,
    &Locks::unlocked_camera,
    &Locks::water_effects,
    &Locks::welcome_setup,
};

/// Returns a dialog of the engine's settings opened over the defaults.
///
/// @return the dialog
settings::Dialog engine_dialog() {
    settings::Dialog dialog;
    settings::open_dialog(dialog, {}, {}, {}, "v0.8.0", settings::Page::controls);
    return dialog;
}

/// Returns a model of a dialog's chosen settings, the dialog's only read.
///
/// @param dialog the dialog
/// @return the model
geometry::SettingsModel model_of(const settings::Dialog& dialog) {
    return geometry::reading(dialog.chosen, dialog);
}

/// Returns what a setting's row is: its row spec's kind.
///
/// @param setting the setting
/// @return its kind
RowKind row_kind(Setting setting) {
    return geometry::kind_of(setting);
}

/// Returns the lines a setting's hint takes, as its row spec gives them.
///
/// @param setting the setting
/// @param deck with the Steam Input notice and a Steam Deck's 90 Hz screen
/// @return the lines
std::size_t row_lines(Setting setting, bool deck) {
    settings::Dialog dialog = engine_dialog();
    dialog.steam_input = deck;
    dialog.steam_deck_panel_hz = deck ? 90 : 0;
    return kit::view_of(geometry::row_spec(setting), model_of(dialog), {}).hints.size();
}

/// Returns the stops, levels or choices a setting's row offers.
///
/// @param dialog a dialog opened over the defaults
/// @param setting the setting
/// @return the count; 0 for a row that offers none
int32_t row_values(const settings::Dialog& dialog, Setting setting) {
    return kit::row_count(geometry::row_spec(setting), model_of(dialog));
}

/// Returns a strip's level width or a drop-down's field width.
///
/// @param setting the setting
/// @return the width; 0 for any other row
int32_t row_width(Setting setting) {
    const auto& spec = geometry::row_spec(setting);
    if (spec.kind != RowKind::levels && spec.kind != RowKind::choice)
        return 0;
    return spec.control_width;
}

/// Returns the levels a strip lets a player choose.
///
/// @param setting the setting
/// @return the levels; 0 for any other row
int32_t row_offered(Setting setting) {
    const auto& spec = geometry::row_spec(setting);
    if (spec.kind != RowKind::levels)
        return 0;
    return kit::row_offered(spec, model_of(engine_dialog()));
}

/// Returns whether a setting's hint lines are its status.
///
/// @param setting the setting
/// @return true when they are
bool row_status(Setting setting) {
    return geometry::row_spec(setting).hint_is_status;
}

/// Returns a setting's lock among the dialog's locks: the field of the
/// locks its row reads.
///
/// @param locks the dialog's locks
/// @param setting the setting
/// @return its lock
Lock row_lock(const Locks& locks, Setting setting) {
    return geometry::lock_of(locks, setting);
}

/// Returns a setting's label.
///
/// @param setting the setting
/// @return the label, in English
std::string_view row_label(Setting setting) {
    return geometry::row_spec(setting).label;
}

void every_setting_has_its_row() {
    const settings::Dialog dialog = engine_dialog();
    const Locks game = settings::settings_locks(settings::GameState{true, false, false, false});
    for (std::size_t index = 0; index < kExpected.size(); ++index) {
        const Expected& expected = kExpected[index];
        const Setting setting = expected.setting;
        OA_CHECK(static_cast<std::size_t>(setting) == index);
        OA_CHECK(row_kind(setting) == expected.kind);
        OA_CHECK(row_label(setting) == expected.label);
        OA_CHECK(row_lines(setting, false) == expected.lines);
        OA_CHECK(row_lines(setting, true) == expected.deck_lines);
        OA_CHECK(row_values(dialog, setting) == expected.values);
        OA_CHECK(row_width(setting) == expected.width);
        OA_CHECK(row_offered(setting) == expected.offered);
        OA_CHECK(row_status(setting) == expected.hint_is_status);
        OA_CHECK(row_lock(game, setting) == expected.in_game);
    }
}

void each_row_reads_its_own_lock() {
    for (const Expected& expected : kExpected) {
        // Its own field locks it, and no other.
        for (const auto field : kLockFields) {
            Locks locks{};
            locks.*field = Lock::set_by_host;
            const Lock lock = row_lock(locks, expected.setting);
            OA_CHECK(lock == (field == expected.own_lock ? Lock::set_by_host : Lock::none));
        }
        Locks every{};
        for (const auto field : kLockFields)
            every.*field = Lock::command_line;
        OA_CHECK(
            row_lock(every, expected.setting) ==
            (expected.own_lock != nullptr ? Lock::command_line : Lock::none)
        );
    }
}

/// Returns a dialog of the engine's settings.
///
/// @param locks what cannot be changed now
/// @param page the section shown
/// @param touch Touch is listed
/// @param game_files Game files is listed
/// @param controller Controller is listed
/// @return the dialog
settings::Dialog engine_dialog_of(
    const Locks& locks, settings::Page page, bool touch, bool game_files, bool controller
) {
    settings::Dialog dialog;
    settings::open_dialog(
        dialog,
        {},
        {},
        locks,
        "v0.8.0",
        page,
        {},
        settings::highest_unit_limit,
        {},
        nullptr,
        touch,
        game_files,
        controller
    );
    dialog.user_folder = "/Users/player/Documents/Open Annihilation";
    return dialog;
}

/// Checks a dialog's display list: every control named once, by words of a
/// to z, 0 to 9 and hyphens; every control the dialog's layout lists is a
/// control of the same number in the list, the part lying in the control's
/// rectangle (a switch's halves, a strip's levels, Your files' buttons, an
/// entry's words and the scroll bar's well lie inside theirs, and every other
/// part is its whole control); and Tab stops only on the list's focusable,
/// enabled controls, each once.
///
/// @param dialog the dialog
/// @param what what the dialog shows, for a failure's message
void check_list(const settings::Dialog& dialog, const std::string& what) {
    const kit::DisplayList list = geometry::dialog_list(dialog, nullptr);
    const std::string problem = kit::name_problem(list);
    if (!problem.empty())
        std::cerr << what << ": " << problem << '\n';
    OA_CHECK(problem.empty());
    std::set<int32_t> numbers;
    for (const kit::Control& control : list.controls)
        OA_CHECK(numbers.insert(control.id).second);
    for (const settings::LayoutPart& part : settings::dialog_layout(dialog)) {
        if (part.control == settings::no_control)
            continue;
        const kit::Control* control = kit::control_of(list, part.control);
        const bool listed = control != nullptr && kit::wholly_in(part.rect, control->rect);
        if (!listed)
            std::cerr << what << ": control " << part.control << " (" << part.text
                      << ") is not in the list where the layout has it\n";
        OA_CHECK(listed);
    }
    // Tab stops only on the list's focusable, enabled controls, each once.
    std::set<int32_t> stops;
    for (const int32_t stop : list.tab_order) {
        const kit::Control* control = kit::control_of(list, stop);
        OA_CHECK(control != nullptr && control->focusable && control->enabled);
        OA_CHECK(stops.insert(stop).second);
    }
}

/// Checks a dialog's every section at its top and at its scroll end.
///
/// @param dialog the dialog, its kind and its offers set
/// @param what what the dialog is, for a failure's message
void check_every_section(settings::Dialog dialog, const std::string& what) {
    const auto pages =
        settings::dialog_pages(dialog.kind, dialog.touch, dialog.game_files, dialog.controller);
    for (const settings::Page page : pages) {
        dialog.page = page;
        const auto scroll = static_cast<std::size_t>(page);
        const std::string section = what + " section " + std::to_string(scroll);
        dialog.scroll[scroll] = 0;
        check_list(dialog, section + " at its top");
        // The section scrolls no further than its end.
        dialog.scroll[scroll] = 100000;
        check_list(dialog, section + " at its end");
        dialog.scroll[scroll] = 0;
    }
}

void every_section_names_its_controls_and_keeps_their_order() {
    const Locks game = settings::settings_locks(settings::GameState{true, false, false, false});
    for (const bool locked : {false, true}) {
        const Locks locks = locked ? game : Locks{};
        const std::string under = locked ? " under a game's locks" : "";
        for (int listed = 0; listed < 8; ++listed) {
            const bool touch = (listed & 1) != 0;
            const bool game_files = (listed & 2) != 0;
            const bool controller = (listed & 4) != 0;
            settings::Dialog dialog =
                engine_dialog_of(locks, settings::Page::controls, touch, game_files, controller);
            check_every_section(dialog, "engine " + std::to_string(listed) + under);
            // The Steam Input notice and a Steam Deck's rate add lines.
            dialog.steam_input = true;
            dialog.steam_deck_panel_hz = 90;
            check_every_section(dialog, "steam deck " + std::to_string(listed) + under);
        }
        settings::Dialog options;
        settings::open_mod_options_dialog(
            options, {}, {}, locks, "v0.8.0", settings::Page::mod_keys
        );
        check_every_section(options, "mod options" + under);
        settings::Dialog language;
        settings::open_language_text_dialog(language, {}, {}, locks, "v0.8.0");
        check_every_section(language, "language alone" + under);
    }
}

void developer_names_every_hack_and_parameter() {
    // Every area and every hack open, Developer Mode on: an area, a hack, a
    // parameter, a set's value and a list's item and length each have a name.
    settings::Dialog dialog = engine_dialog_of({}, settings::Page::developer, true, true, true);
    dialog.chosen.developer_mode = true;
    for (uint8_t& open : dialog.developer.areas_open)
        open = 1;
    for (uint8_t& open : dialog.developer.hacks_open)
        open = 1;
    check_list(dialog, "developer, every hack open");
    const kit::DisplayList list = geometry::dialog_list(dialog, nullptr);
    OA_CHECK(kit::control_named(list, "settings.hack-area.ai") != settings::no_control);
    OA_CHECK(kit::control_named(list, "settings.active-only") == settings::active_only_control);
    OA_CHECK(
        kit::control_named(list, "settings.restore-profile-values") ==
        settings::restore_profile_control
    );
    dialog.scroll[static_cast<std::size_t>(settings::Page::developer)] = 100000;
    check_list(dialog, "developer, every hack open, at its end");
}

void an_open_list_names_its_controls() {
    // An open drop-down's items are controls of their own while it is open,
    // each named by the word the preferences keep it as.
    settings::Dialog dialog = engine_dialog_of({}, settings::Page::language, true, true, true);
    static_cast<void>(settings::dialog_key(dialog, settings::DialogKey::tab));
    static_cast<void>(settings::dialog_key(dialog, settings::DialogKey::space));
    OA_CHECK(dialog.open_list == settings::first_row_control);
    check_list(dialog, "language, its list open");
    const kit::DisplayList open = geometry::dialog_list(dialog, nullptr);
    OA_CHECK(
        kit::control_named(open, "settings.language.system") == geometry::menu_item_control(0)
    );
    OA_CHECK(
        kit::hit(open, {open.controls.front().rect.x + 1, open.controls.front().rect.y + 1}) ==
        open.controls.front().id
    );
}

/// A level or a drop-down's setting and the key the preferences keep it under.
struct StoredChoice {
    Setting setting{};    ///< the setting
    std::string_view key; ///< its preferences key
};

/// Every strip's and drop-down's setting, with its key.
constexpr std::array<StoredChoice, 24> kStoredChoices{{
    {Setting::max_zoom_out, settings::key::max_zoom_out},
    {Setting::max_zoom_in, settings::key::max_zoom_in},
    {Setting::view_past_map_edge, settings::key::view_past_map_edge},
    {Setting::window_edge, settings::key::window_edge},
    {Setting::anti_aliasing, settings::key::anti_aliasing},
    {Setting::hardware_acceleration, settings::key::hardware_acceleration},
    {Setting::language, settings::key::language},
    {Setting::touch_drag, settings::key::touch_drag},
    {Setting::touch_latches, settings::key::touch_latches},
    {Setting::touch_control_size, settings::key::touch_control_size},
    {Setting::pad_scheme, settings::key::pad_scheme},
    {Setting::pad_right_trackpad, settings::key::pad_right_trackpad},
    {Setting::pad_acceleration, settings::key::pad_acceleration},
    {Setting::pad_right_stick, settings::key::pad_right_stick},
    {Setting::pad_gyro, settings::key::pad_gyro},
    {Setting::pad_haptics, settings::key::pad_haptics},
    {Setting::pad_prompts, settings::key::pad_prompts},
    {Setting::menu_scaling, settings::key::menu_scaling},
    {Setting::explosion_flash, settings::key::explosion_flash},
    {Setting::fog_of_war, settings::key::fog_of_war},
    {Setting::zoomed_out_units, settings::key::zoomed_out_units},
    {Setting::zoomed_out_after, settings::key::zoomed_out_after},
    {Setting::window_frame, settings::key::window_frame},
    {Setting::interface_size, settings::key::interface_size},
}};

void every_row_and_choice_has_an_id_of_one_word() {
    // Each section's rows are named by words, none twice in a section.
    for (std::size_t index = 0; index < settings::page_count; ++index) {
        std::set<std::string_view> seen;
        for (const Setting setting : settings::page_settings(static_cast<settings::Page>(index))) {
            const std::string_view id = geometry::setting_name(setting);
            OA_CHECK(kit::row_word(id) && id == geometry::row_spec(setting).id);
            OA_CHECK(seen.insert(id).second);
        }
    }
    // Every item of a drop-down and every level of a strip has an id, a word
    // none other of its row has: the word the preferences keep it as.
    const settings::Dialog dialog = engine_dialog();
    std::size_t rows = 0;
    for (std::size_t index = 0; index < kExpected.size(); ++index) {
        const auto setting = static_cast<Setting>(index);
        const auto& spec = geometry::row_spec(setting);
        if (spec.kind != RowKind::choice && spec.kind != RowKind::levels)
            continue;
        ++rows;
        const StoredChoice* stored = nullptr;
        for (const StoredChoice& choice : kStoredChoices)
            if (choice.setting == setting)
                stored = &choice;
        OA_CHECK(stored != nullptr);
        std::set<std::string> ids;
        const geometry::SettingsModel model = model_of(dialog);
        for (int32_t at = 0; at < kit::row_count(spec, model); ++at) {
            const std::string id = kit::row_choice_id(spec, model, at);
            OA_CHECK(kit::row_word(id) && geometry::choice_word(spec, model, at) == id);
            OA_CHECK(ids.insert(id).second);
            // What the preferences keep once the row is set there, in name form.
            settings::EngineSettings chosen = dialog.chosen;
            geometry::SettingsModel choosing{&chosen, &dialog};
            kit::set_row_index(spec, choosing, at);
            if (kit::row_index(spec, geometry::reading(chosen, dialog)) != at)
                continue; // a level not offered, which no preference keeps
            settings::EngineSettings opened = chosen;
            geometry::SettingsModel opening{&opened, &dialog};
            kit::set_row_index(spec, opening, at == 0 ? 1 : 0);
            oa::platform::preferences::Values values;
            settings::write_settings(values, opened, chosen, dialog.defaults, false);
            const auto written = values.find(std::string(stored != nullptr ? stored->key : ""));
            OA_CHECK(written != values.end());
            if (written != values.end())
                OA_CHECK(geometry::word_form(written->second, false) == id);
        }
    }
    OA_CHECK(rows == kStoredChoices.size());
    // A few, written out.
    const geometry::SettingsModel model = model_of(dialog);
    const auto ids_of = [&model](Setting setting) {
        std::vector<std::string> ids;
        const auto& spec = geometry::row_spec(setting);
        for (int32_t at = 0; at < kit::row_count(spec, model); ++at)
            ids.push_back(kit::row_choice_id(spec, model, at));
        return ids;
    };
    OA_CHECK(
        ids_of(Setting::max_zoom_out) ==
        (std::vector<std::string>{"automatic", "whole-map", "1-32", "1-16", "1-8", "1-4", "1-2"})
    );
    OA_CHECK(
        ids_of(Setting::menu_scaling) ==
        (std::vector<std::string>{"sharp", "whole-steps", "unfiltered"})
    );
    OA_CHECK(
        ids_of(Setting::anti_aliasing) == (std::vector<std::string>{"1", "2", "4", "8", "16"})
    );
    OA_CHECK(
        ids_of(Setting::zoomed_out_units) == (std::vector<std::string>{"rendered", "dots", "icons"})
    );
    OA_CHECK(ids_of(Setting::fog_of_war) == (std::vector<std::string>{"classic", "smooth"}));
    OA_CHECK(
        ids_of(Setting::interface_size) ==
        (std::vector<std::string>{"automatic", "100", "200", "300", "400"})
    );
    OA_CHECK(!ids_of(Setting::language).empty() && ids_of(Setting::language)[0] == "system");
    // A tag's capitals and a value's slash become name form.
    OA_CHECK(geometry::word_form("zh-Hans", false) == "zh-hans");
    OA_CHECK(geometry::word_form("1/32", false) == "1-32");
    OA_CHECK(geometry::word_form("ai.no-cheat", true) == "ai.no-cheat");
    OA_CHECK(geometry::word_form("ai.no-cheat", false) == "ai-no-cheat");
}

/// Returns a dialog's Tab order as its display list has it.
///
/// @param dialog the dialog
/// @return the controls Tab moves through, in order
std::vector<int32_t> tab_order(const settings::Dialog& dialog) {
    return geometry::dialog_list(dialog, nullptr).tab_order;
}

void tab_follows_the_dialogs_focus_order() {
    // Controls: its rows, the footer, then the sections' entries, Game files
    // then Downloads, Game Content and System, listed before Developer.
    const settings::Dialog controls =
        engine_dialog_of({}, settings::Page::controls, true, true, true);
    OA_CHECK(
        tab_order(controls) == (std::vector<int32_t>{
                                   16,
                                   17,
                                   18,
                                   19,
                                   20,
                                   21,
                                   22,
                                   12,
                                   13,
                                   14,
                                   0,
                                   1,
                                   2,
                                   3,
                                   4,
                                   5,
                                   6,
                                   8,
                                   9,
                                   11,
                                   10,
                                   7,
                                   settings::close_x_control
                               })
    );
    // Graphics: After zoom, locked while units are drawn whole, takes none.
    settings::Dialog graphics = controls;
    graphics.page = settings::Page::graphics;
    OA_CHECK(
        tab_order(graphics) ==
        (std::vector<int32_t>{16, 17, 18, 19, 20, 21, 22, 23,
                              24, 25, 27, 28, 29, 30, 31, 32,
                              12, 13, 14, 0,  1,  2,  3,  4,
                              5,  6,  8,  9,  11, 10, 7,  settings::close_x_control})
    );
    // Developer with its first area open: its rows, UI kit gallery's SHOW
    // last of them, its list's headers, Show Active Only and Restore profile
    // values, then the rest.
    settings::Dialog developer = controls;
    developer.page = settings::Page::developer;
    developer.chosen.developer_mode = true;
    developer.developer.areas_open[0] = 1;
    OA_CHECK(
        tab_order(developer) ==
        (std::vector<int32_t>{
            16, 17, 18, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31, 32, 33,
            34, 35, 36, 37, 38, 39, 40, 41, 42, 43, 44, 45, 46, 47, 19, 20,
            12, 13, 14, 0,  1,  2,  3,  4,  5,  6,  8,  9,  11, 10, 7,  settings::close_x_control
        })
    );
}

} // namespace

int main() {
    every_setting_has_its_row();
    each_row_reads_its_own_lock();
    every_row_and_choice_has_an_id_of_one_word();
    every_section_names_its_controls_and_keeps_their_order();
    developer_names_every_hack_and_parameter();
    an_open_list_names_its_controls();
    tab_follows_the_dialogs_focus_order();
    return oa::test::check_exit_status();
}

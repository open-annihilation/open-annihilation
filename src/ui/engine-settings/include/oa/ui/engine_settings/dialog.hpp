// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Open Annihilation settings dialog: its sections and rows, what a press,
// a drag or a key does to them, and how it and the OA button that opens it
// are drawn. The dialog is laid out in points, one point a pixel of the
// game's 640x480 picture, at its size class (Dialog::size_class: Compact,
// 0.7.3's 480 by 324, Regular or Large), and drawn without the game's art
// (oa/ui/frontend_renderer/artless.hpp) in the game's own fonts at a whole
// scale. A host places it, darkens what lies under it, turns its events into
// dialog points and puts the settings it reports in effect.
#pragma once

#include "oa/formats/fnt.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/data/languages.hpp"
#include "oa/present/game_text.hpp"
#include "oa/ui/engine_settings.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/frontend_renderer/artless.hpp"
#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/rows.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/kit/theme.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#if defined(__APPLE__)
#include <TargetConditionals.h>
#endif

namespace oa::ui::engine_settings {

/// The dialog's width at Compact, in source pixels. Each size class has its
/// own (oa::ui::kit::metrics_of).
inline constexpr int32_t dialog_width = oa::ui::kit::compact_metrics.dialog_width;
/// The dialog's height at Compact, in source pixels.
inline constexpr int32_t dialog_height = oa::ui::kit::compact_metrics.dialog_height;
/// The OA button's side on the main menu, in source pixels: one and a half
/// times the 32 it was.
inline constexpr int32_t menu_button_side = 48;
/// The OA button's side in the in-game menu's column, in source pixels.
inline constexpr int32_t ingame_button_side = 24;

/// The colour the screen under the dialog is darkened with.
inline constexpr oa::ui::frontend_renderer::Rgb backdrop_color =
    oa::ui::kit::rgb(oa::ui::kit::colour::backdrop);
/// How far the main menu is darkened under the dialog, in 256ths.
inline constexpr uint32_t menu_backdrop_opacity = oa::ui::kit::menu_backdrop_opacity;
/// How far the in-game menu's column is darkened beside the dialog, in 256ths.
inline constexpr uint32_t ingame_backdrop_opacity = oa::ui::kit::ingame_backdrop_opacity;

/// The dialog's sections, in the order its list shows them: the engine's
/// settings, then the mod options' (ui.options-dialog), which a dialog of
/// each kind lists alone.
enum class Page : uint8_t {
    controls,      ///< Controls
    common_tweaks, ///< Engine Limits: the unit limit, pathfinding and skirmish players
    language,      ///< Language & Text: the language and how game text is drawn
    graphics,      ///< Graphics
    /// User Interface: menu scaling, the window's frame, HUD scaling,
    /// Interface size, and whether Escape opens the game menu
    user_interface,
    /// Touch: how the touch controls answer a finger; listed only while the
    /// game has touch controls (Dialog::touch)
    touch,
    /// Controller: how a gamepad points, moves the map and answers; listed,
    /// after Touch, only once a gamepad has sent input in this run
    /// (Dialog::controller)
    controller,
    /// Developer, at the foot of the list under a divider: its rows over
    /// Developer Mode's list of the standard hacks
    developer,
    /// Game files: what is installed, the backups switch and where the
    /// files are; listed, between Controller and Downloads, only where the
    /// platform brings game files in and the dialog is the main menu's
    /// (Dialog::game_files)
    game_files,
    /// Downloads: when catalogues are checked, each registry's install ID
    /// and the downloaded files. On the main menu a link opens the Library
    /// on Registries. Listed after Game files and before Game Content.
    downloads,
    /// System: the player's own folder, and, where the game registers file
    /// types at run time, a switch for each. Listed after Game Content and
    /// before Developer.
    system,
    /// Game Content: each copied bundle, its size and its check, and the
    /// buttons that check again, copy again or open the games folder.
    /// Listed after Downloads and before System. Its number follows System's
    /// so System stays 10.
    game_content,
    /// Updates: this build's version and whether a release is out, and when
    /// Open Annihilation looks for one. Listed after Downloads and before
    /// Game Content. Its number follows Game Content's, so Downloads, System
    /// and Game Content stay 9, 10 and 11.
    updates,
    /// Accessibility: the input helpers, in one section. Listed after User
    /// Interface and before Touch. Its number follows Updates', so Touch,
    /// Controller, Developer, Game files, Downloads, System, Game Content
    /// and Updates stay 5, 6, 7, 8, 9, 10, 11 and 12.
    accessibility,
    mod_keys,   ///< the mod's keys
    mod_patrol, ///< what patrolling builders do
    mod_guard,  ///< what guarding builders do
    mod_tools,  ///< the build tools and the mex snap
    mod_chat,   ///< the wreck snap, the chat and the resource bar
    /// One registered page the engine's settings show, laid out from the row
    /// specs set_registered_pages gave. A mod's options never list one.
    registered,
};

/// The number of sections, of every kind of dialog, counting Downloads,
/// Updates, Accessibility, Game Content, System and the registered page.
inline constexpr std::size_t page_count = 20;
/// The most sections a dialog lists: the engine's settings' fourteen with
/// Touch, Controller and Game files, eleven without them; a mod's options have
/// five.
inline constexpr std::size_t most_listed_pages = 14;
/// Windows and Linux register the eight file types at run time, and macOS
/// chooses whether this copy is the default opener of each, so System lists
/// a switch for each. iOS takes the types from the app and has no way to
/// choose a default opener, so System shows a text row. Android does not
/// register at run time either.
#if defined(_WIN32) || (defined(__linux__) && !defined(__ANDROID__)) ||                            \
    (defined(__APPLE__) && !TARGET_OS_IPHONE)
inline constexpr bool file_types_switchable = true;
#else
inline constexpr bool file_types_switchable = false;
#endif

/// Which settings a dialog shows.
enum class DialogKind : uint8_t {
    /// the engine's settings: the sections up to Developer, Touch among them
    /// only while the game has touch controls and Controller only once a
    /// gamepad has sent input
    engine,
    mod_options, ///< a mod's options: the last five sections
    /// Language alone, as the Game files screen opens it before the game's
    /// files are installed
    language_text,
};

/// Returns the sections a kind of dialog lists, in order.
///
/// @param kind the dialog's kind
/// @param touch the game has touch controls (Dialog::touch), so that the
///     engine's settings list Touch after Accessibility and before Downloads
/// @param game_files the dialog lists Game files (Dialog::game_files),
///     after Touch and Controller and before Developer
/// @param controller a gamepad has sent input in this run
///     (Dialog::controller), so that the engine's settings list Controller
///     after Touch and before Game files and Developer
/// @return eleven sections for the engine's settings (Controls, Common
///     Tweaks, Language & Text, Graphics, User Interface, Accessibility,
///     Downloads, Updates, Game Content, System and Developer), one more
///     for each of Touch, Controller and Game files listed, fourteen with
///     all three; five for a mod's options and one for Language & Text
///     alone, whatever the others say
[[nodiscard]] std::span<const Page> dialog_pages(
    DialogKind kind, bool touch = false, bool game_files = false, bool controller = false
) noexcept;

/// Returns a section's word: its name in kebab case, the same in every
/// language, as automation names its entry (`oa.settings.nav.<word>`) and a
/// stage file may name the section ("language", "user-interface",
/// "engine-limits").
///
/// @param page the section
/// @return the word
[[nodiscard]] std::string_view page_word(Page page) noexcept;

/// The settings, as the dialog's rows show them.
enum class Setting : uint8_t {
    path_search, ///< Pathfinding cycles: a slider
    wheel_zoom,  ///< Mouse wheel zoom: a switch
    /// Maximum zoom out: a drop-down of Automatic, Whole map, 1/32, 1/16,
    /// 1/8, 1/4 and 1/2
    max_zoom_out,
    max_zoom_in, ///< Maximum zoom in: a drop-down of None, 2x, 3x and 4x
    /// View past the map's edge: a strip of Off, 25% and 50%
    view_past_map_edge,
    middle_click_pan,  ///< Middle-click pans: a switch
    escape_opens_menu, ///< Escape opens the game menu: a switch
    switch_alt,        ///< Select groups without Alt: a switch
    /// Window edge: a strip of Capture, Sticky and Free
    window_edge,
    /// Edge stickiness: a slider from 0.0 to 5.0, locked unless Window edge
    /// is Sticky
    window_edge_stickiness,
    unit_limit,     ///< Unit limit: a slider
    max_frame_rate, ///< Maximum frame rate: a slider
    anti_aliasing,  ///< Enhanced anti-aliasing: a strip of levels
    screen_size,    ///< Screen size: a slider
    developer_mode, ///< Enable Developer Mode: a switch
    frame_stats,    ///< Show performance statistics: a switch
    /// Hardware acceleration: a strip of Off, Basic and Full whose two hint
    /// lines are its status
    hardware_acceleration,
    vertical_sync, ///< Vertical sync: a switch
    /// Language: a drop-down of System default and the languages the game
    /// draws, each named in itself
    language,
    modern_fonts,    ///< Use modern fonts for game text: a switch
    text_outline,    ///< Font outline: a switch
    text_shadow,     ///< Font shadow: a switch
    text_background, ///< Game text background: a switch
    unicode_chat,    ///< Enable Unicode Multiplayer Chat: a switch
    text_size,       ///< Text size: a slider, locked while modern fonts are off
    /// One-finger drag: a strip of Automatic, Box and Scroll
    touch_drag,
    touch_hold_delay,  ///< Hold delay: a slider of milliseconds
    touch_latches,     ///< QUEUE and ADD: a strip of Stay on and One action
    touch_haptics,     ///< Haptics: a switch
    touch_left_handed, ///< Left-handed layout: a switch
    /// Control size: a strip of Standard, Large and Larger, in Touch and in
    /// Controller
    touch_control_size,
    pad_scheme, ///< Scheme: a strip of Trackpads and Sticks
    /// Right trackpad: a strip of the pointer's two ways, Relative and
    /// Absolute
    pad_right_trackpad,
    pad_pointer_speed, ///< Pointer speed: a slider of percent
    pad_acceleration,  ///< Pointer acceleration: a strip of Off, Low and High
    pad_glide,         ///< Trackpad glide: a switch
    /// Right stick: a strip of Zoom (zoom and build pages), Pointer and
    /// Nothing
    pad_right_stick,
    pad_magnetism, ///< Magnetism (stick pointer): a switch
    /// Gyro pointer: a drop-down of Off, While the right pad is touched,
    /// While the right stick is touched and Always
    pad_gyro,
    pad_gyro_speed, ///< Gyro speed: a slider of percent
    pad_haptics,    ///< Haptics: a strip of Off, Light and Strong
    /// Button prompts: a drop-down of Automatic, Steam Deck, Xbox,
    /// PlayStation, Nintendo and Off
    pad_prompts,
    pad_left_handed, ///< Left-handed: a switch
    /// Steam Input: a text row, Controller's first while the gamepad reaches
    /// the game through Steam Input (Dialog::steam_input), saying so; it
    /// changes no setting
    pad_steam_input_notice,
    snap_override_key, ///< the mod's snap override key: a slider of option_keys
    autoclick_key,     ///< the mod's autoclick key: a slider of option_keys
    rotate_build_key,  ///< the mod's rotate key: a slider of option_keys
    patrol_hold,       ///< patrolling builders under Hold position: a slider of three
    patrol_maneuver,   ///< patrolling builders under Maneuver
    patrol_roam,       ///< patrolling builders under Roam
    guard_hold,        ///< guarding builders under Hold position: a slider of three
    guard_maneuver,    ///< guarding builders under Maneuver
    guard_roam,        ///< guarding builders under Roam
    mex_snap_radius,   ///< the mex snap radius: a slider up to the mod's most
    wreck_snap_radius, ///< the wreck snap radius: a slider up to the mod's most
    optimize_dt_rows,  ///< Optimize DT rows: a switch
    full_rings,        ///< Full rings: a switch
    chat_backdrop,     ///< Accessible chat: a switch
    panel_background,  ///< the resource bar's background: a slider of three
    /// Game files: what is installed, its sizes line and a MANAGE… button
    game_files_summary,
    game_files_backed_up, ///< Include in device backups: a switch, with its hint
    game_files_location,  ///< Where the files are: a text row
    /// Your files: where the player's own folder is, and buttons that open
    /// its Saves, Screenshots and Mods folders in the system's file manager;
    /// it changes no setting
    user_folder,
    /// Menu scaling: a strip of Sharp, Whole steps and Unfiltered
    menu_scaling,
    native_density, ///< Native pixel density: a switch
    /// Explosion flash: a strip of Off, Reduced and Full
    explosion_flash,
    /// Zoomed out units: a strip of Rendered, Dots and Icons, of which
    /// Icons is shown faded and cannot be chosen yet
    zoomed_out_units,
    /// After zoom: a drop-down of 1/2, 1/3, 1/4, 1/6, 1/8, 1/12 and 1/16,
    /// locked while Zoomed out units shows Rendered
    zoomed_out_after,
    /// Window frame: a strip of Hidden in play and Always shown
    window_frame,
    hud_scaling, ///< HUD scaling: a switch
    /// Interface size: a drop-down of Auto, 100%, 200%, 300% and 400%, on
    /// User Interface
    interface_size,
    /// MORE IN THE LIBRARY…: a link under Language's row that opens the
    /// Library on Languages, listed only while Dialog::library_link is set;
    /// it changes no setting
    more_in_library,
    /// UI kit gallery: Developer's row whose SHOW opens the gallery of every
    /// part of Open Annihilation's own screens (DialogAction::show_gallery);
    /// it changes no setting
    kit_gallery,
    /// File Type Registration: the heading over the eight file-type switches,
    /// with the group's hint. It changes no setting.
    file_type_registration,
    file_type_oamod,    ///< Mod packages (.oamod): a switch
    file_type_oamap,    ///< Map packs (.oamap): a switch
    file_type_oalang,   ///< Language packs (.oalang): a switch
    file_type_oareg,    ///< Registry files (.oareg): a switch
    file_type_oamut,    ///< Mutator packages (.oamut): a switch
    file_type_oabrain,  ///< Brain packages (.oabrain): a switch
    file_type_oakeys,   ///< Key layout packages (.oakeys): a switch
    file_type_oalaunch, ///< Launch files (.oalaunch): a switch
    /// File types: the text row iOS shows in place of the switches.
    /// It changes no setting.
    file_types_note,
    /// Skirmish players: a slider of how many player rows the skirmish setup
    /// shows, the same count the setup's key codes store.
    skirmish_players,
    smooth_motion,      ///< Smooth motion: a switch
    weapon_lighting,    ///< Coloured weapon lighting: an intensity slider
    nanolathe_lighting, ///< Nanolathe glow: an intensity slider
    wreck_glow,         ///< Wreck glow: an intensity slider
    heat_distortion,    ///< Heat distortion: an intensity slider

    water_ripples,      ///< Water ripples: an intensity slider
    coastal_waves,      ///< Coastal waves: an intensity slider
    underwater_shimmer, ///< Underwater shimmer: an intensity slider
    naval_reflections,  ///< Naval reflections: an intensity slider

    allow_full_3d,   ///< Allow full 3D mode: a switch
    unlocked_camera, ///< Unrestricted camera angles: a switch
    /// Welcome setup: SHOW AGAIN walks through the language, online features
    /// and the OA buttons again. It changes no setting. During a game the
    /// button is locked, and the choice is made from the main menu.
    welcome_setup,
    /// Fog of War: a strip of Classic and Smooth
    fog_of_war,
    /// Set by the mod: the heading over the limits the mod fixes.
    /// It keeps no setting.
    mod_limits_heading,
    /// Weapon types: how many weapon types the mod allows, and in a match
    /// how many of them the match loaded. It keeps no setting.
    weapon_types,
    /// Sides: how many sides the mod allows, and in a match how many of
    /// them the match loaded. It keeps no setting.
    sides,
    /// Key layout: the layout in use, and how many of its actions the player
    /// changed, with EDIT KEYS…. It keeps no setting. The layout itself is
    /// chosen in the editor.
    key_layout,
    /// Key layout with the mod being played: Same as the game, or one
    /// installed layout. Shown only while a mod is played.
    key_layout_for_mod,
    /// Show keys on buttons: order buttons and build cells show their key.
    show_keys_on_buttons,
    /// Hold keys that latch: how many latchable actions are on, and CHOOSE…
    /// which opens the key editor on its Latchable filter. It keeps no setting.
    latching,
    sticky_modifiers, ///< Sticky modifiers: a switch
    hold_to_confirm,  ///< Hold to confirm: a switch
    long_press,       ///< Long press: a slider of milliseconds
    slow_keys,        ///< Slow keys: a slider of milliseconds, Off at 0
    bounce_keys,      ///< Bounce keys: a slider of milliseconds, Off at 0
    order_wheel,      ///< Order wheel: a strip of Off, Hold right and Hold middle
    pointer_keys,     ///< Pointer keys: a switch
    /// Double press: a slider of milliseconds. The first stop is the system's time.
    double_press,
    /// Key repeat delay: a slider of milliseconds. The first stop is the system's delay.
    repeat_delay,
    /// Key repeat rate: a slider of repeats a second. The first stop is the system's rate.
    repeat_rate,
    drag_threshold, ///< Drag threshold: a slider of pixels
    /// Edge scrolling: a switch. Off stops the view scrolling at the window's edge.
    edge_scroll,
    /// Window size: a drop-down of Auto, Compact, Regular and Large, on User
    /// Interface under Interface size
    window_size,
    /// Reset window positions: Reset asks the host to put every window the
    /// player moved back in its place (Dialog::window_reset_asked). It keeps
    /// no setting.
    reset_window_positions,
};

/// Returns the settings a section shows, top to bottom.
///
/// A section holds any number of rows: when they are taller than the space
/// under its heading, its rows scroll there. Developer's rows stay at its
/// top, over Developer Mode's list of the standard hacks, which scrolls.
///
/// @param page the section
/// @return one or more settings
[[nodiscard]] std::span<const Setting> page_settings(Page page) noexcept;

// Every control has a number: the sections' entries, then the footer's
// buttons, then the scroll bar, then the open section's rows, which have no
// upper end, so a row never takes a fixed control's number. The entries
// keep room for the most sections a dialog lists, so every other number is
// the same whether or not the dialog lists Touch, Controller or Game files.

/// No control: what Dialog::hovered, pressed and focused hold when they name none.
inline constexpr int32_t no_control = -1;
/// The header's close mark. It asks for the same close as Cancel: the
/// dialog puts back what it opened with, including a change not kept with OK.
/// An open drop-down's items take the numbers just under no_control, so the
/// mark keeps a number clear of that run and of the registered list.
inline constexpr int32_t close_x_control = -9000;
/// Change layout… on Controls' key layout card: it asks the host to open the
/// Library on Keys (DialogAction::open_library). Its number follows the close
/// mark's, clear of an open list's items and of the registered list.
inline constexpr int32_t change_layout_control = close_x_control - 1;
/// Turn every helper off, on Accessibility's practice pad.
inline constexpr int32_t helpers_off_control = close_x_control - 2;
/// The update card's second button on Updates (UpdatesOffer::actions); its
/// first is the card's row control.
inline constexpr int32_t update_second_control = close_x_control - 3;
/// The update card's third button.
inline constexpr int32_t update_third_control = close_x_control - 4;
/// The first section's entry in the list; each section's entry is its
/// place among its kind of dialog's sections with Touch and Controller
/// (page_control), whether or not the dialog lists them.
inline constexpr int32_t first_page_control = 0;
/// Restore defaults: the first number after the sections' entries.
inline constexpr int32_t restore_control =
    first_page_control + static_cast<int32_t>(most_listed_pages);
/// Cancel.
inline constexpr int32_t cancel_control = restore_control + 1;
/// OK.
inline constexpr int32_t ok_control = cancel_control + 1;
/// The open section's scroll bar, shown while its rows are taller than the
/// space they scroll in. It takes no keyboard focus.
inline constexpr int32_t scroll_bar_control = ok_control + 1;
/// The open section's first row's control; the next rows' follow it, one
/// for each row the section has.
inline constexpr int32_t first_row_control = scroll_bar_control + 1;
/// Developer's rows, over its list: Enable Developer Mode, Show
/// performance statistics, then UI kit gallery.
inline constexpr int32_t developer_row_count = 3;
/// Developer's Enable Developer Mode switch, its first row's control.
inline constexpr int32_t developer_mode_control = first_row_control;
/// Developer's Show Active Only switch, under its list.
inline constexpr int32_t active_only_control = first_row_control + developer_row_count;
/// Developer's Restore profile values button, under its list.
inline constexpr int32_t restore_profile_control = active_only_control + 1;
/// The control of the first row of Developer's list that takes input: an
/// area's or a hack's header, or a parameter's control. The next such
/// rows' follow it in the list's order, which has no upper end.
inline constexpr int32_t first_hack_list_control = restore_profile_control + 1;
static_assert(
    first_page_control + static_cast<int32_t>(most_listed_pages) <= restore_control &&
        restore_control < cancel_control && cancel_control < ok_control &&
        ok_control < scroll_bar_control && scroll_bar_control < first_row_control,
    "the sections' entries come first, then the footer's buttons, the scroll bar and the rows"
);
static_assert(
    static_cast<std::size_t>(Page::developer) < most_listed_pages &&
        static_cast<std::size_t>(Page::registered) + 1 == page_count,
    "every engine section has an entry's number, and every section a scroll"
);

/// The first registered page's entry. Each next page's counts down from it,
/// under an open list's items.
inline constexpr int32_t first_registered_nav_control = -10000;

/// Returns a registered page's entry's control.
///
/// @param index the page's place in the pages set_registered_pages gave, from 0
/// @return its control's number
[[nodiscard]] constexpr int32_t registered_nav_control(std::size_t index) noexcept {
    return first_registered_nav_control - static_cast<int32_t>(index);
}

/// Returns the control of a section's entry in the list: its place among
/// its kind of dialog's sections, each section counted whether or not it is
/// listed, so that Touch is 5, Controller 6, Developer 7, Game files 8,
/// Downloads 9, System 10, Game Content 11, Updates 12 and Accessibility 13
/// in every dialog of the engine's settings, and a dialog without Touch has
/// no control 5, one without Controller no control 6.
/// The registered page has no entry of this form; its entries are
/// registered_nav_control.
///
/// @param page the section
/// @return its control's number, or no_control for the registered page
[[nodiscard]] constexpr int32_t page_control(Page page) noexcept {
    if (page == Page::registered)
        return no_control;
    const auto index = static_cast<int32_t>(page);
    const auto first_mod = static_cast<int32_t>(Page::mod_keys);
    return first_page_control + (index >= first_mod ? index - first_mod : index);
}

/// The keys the dialog answers to: the kit's keys, which a host gives the
/// platform's keys the meanings of. dialog_key says what each does in the
/// dialog; Backspace and Delete, which edit a kit screen's text, do nothing
/// here, nor in the dialog's drop-down lists, notices and prompts.
using DialogKey = oa::ui::kit::Key;

/// The buttons of the Your files row, left to right: each opens a folder of
/// the player's own folder.
enum class FolderButton : uint8_t {
    saves,       ///< the Saves folder
    screenshots, ///< the Screenshots folder
    mods,        ///< the Mods folder
};
/// The number of buttons the Your files row has.
inline constexpr std::size_t folder_button_count = 3;

/// What Hardware acceleration's status says: whether the graphics card
/// scales the frames, and why not when it does not. The states keep the
/// order the host tests them in; the first that applies is shown. Basic or
/// Full, by the setting or a flag, asks for the graphics card; Off does not.
enum class AccelerationState : uint8_t {
    /// Off, by the setting or --no-hardware-acceleration, and a failed
    /// graphics driver was passed over at this start.
    off_driver_skipped,
    /// Basic or Full, but the machine has under 2 GiB of memory, or does
    /// not say, and a failed graphics driver was passed over at this start.
    needs_memory_driver_skipped,
    /// Not in use: the machine has under 2 GiB of memory, or does not say,
    /// whatever the setting or the flags.
    needs_memory,
    off_by_setting,      ///< Off, by the setting
    off_by_command_line, ///< Off, by --no-hardware-acceleration or --hardware-acceleration=off
    /// Basic or Full, but the environment names a render driver, or the
    /// video driver draws no window, so the processor scales the frames.
    environment_driver,
    /// Basic or Full, but dropped in this run when the machine ran short of
    /// memory.
    too_little_memory,
    /// Basic or Full, waiting for a shared game or a replay to end: in one,
    /// either takes effect from the next game (AccelerationStatus::replay
    /// says which, and AccelerationStatus::asked which level).
    waiting_for_game_end,
    engine_error,   ///< Basic or Full, but an error stopped it for this run
    driver_failed,  ///< Basic or Full, but the graphics driver failed, in this run or before
    game_stopped,   ///< Basic or Full, but the game stopped while using it before
    no_usable_card, ///< Basic or Full, but no usable graphics card was found
    lacks_feature,  ///< Basic or Full, but the graphics card lacks something it needs
    /// Basic or Full, but the game cannot save the files that guard trying it.
    cannot_save,
    next_start, ///< Basic or Full, from the next start
    /// Full, but the game cannot save the files that guard trying it: Basic
    /// is in use in its place.
    full_cannot_save,
    /// Full, but dropped to Basic in this run when the machine ran short of
    /// memory.
    full_too_little_memory,
    /// Full, but the graphics card failed while drawing the battlefield, so
    /// Basic is in use for the rest of the run.
    full_stopped,
    /// Full, but Full failed before on this graphics driver, so Basic is in
    /// use.
    full_failed_before,
    /// Full, but the graphics card lacks something Full needs: Basic is in
    /// use.
    full_lacks_feature,
    /// Full, waiting for a shared game or a replay to end: Basic is in use
    /// for this game, and Full takes effect from the next
    /// (AccelerationStatus::replay says which match).
    full_waiting_for_game_end,
    in_use_on_another_driver, ///< In use, on another graphics driver: one failed
    in_use_no_smoothing,      ///< In use, with no smoothing when zoomed out on this machine
    full_in_use,              ///< Full in use: the graphics card draws the view
    in_use,                   ///< In use
};

/// What the graphics card does on this machine while it is in use, which
/// the status's second line says.
enum class AccelerationReach : uint8_t {
    menus,      ///< it scales the menus and the interface
    zoomed_in,  ///< it scales the interface and the zoomed-in battlefield
    zoomed_out, ///< it scales everything and smooths the zoomed-out battlefield
    /// it scales nothing, and smooths the zoomed-out battlefield
    nearest_zoomed_out,
    nearest_none, ///< it scales nothing: the frames look as with it off
};

/// Hardware acceleration's status, as the host reports it. It names no
/// graphics interface and no driver.
struct AccelerationStatus {
    AccelerationState state{AccelerationState::off_by_setting}; ///< what runs, or why not
    AccelerationReach reach{AccelerationReach::menus};          ///< what it does while in use
    /// The match AccelerationState::waiting_for_game_end and
    /// full_waiting_for_game_end wait for replays a recording rather than
    /// being played with other machines.
    bool replay{};
    /// The level asked for, by the setting or a flag, which
    /// AccelerationState::waiting_for_game_end names.
    HardwareAcceleration asked{HardwareAcceleration::off};
    /// Full's anti-aliasing while it is in use: the samples a pixel across
    /// the graphics card draws the view with, 1 for none, which the second
    /// line of AccelerationState::full_in_use names.
    uint8_t supersample{1};
    /// How many times finer than the window, along each axis, the graphics
    /// card drew the battlefield in the last Full frame: the Enhanced
    /// anti-aliasing row's level, as the texture limit and the memory allow;
    /// 0 while frames are not drawn in Full. The row's hint says what its
    /// level does in Full from it.
    uint8_t full_supersample{};

    friend bool operator==(const AccelerationStatus&, const AccelerationStatus&) = default;
};

/// What an event asks of the host.
enum class DialogAction : uint8_t {
    none,   ///< nothing
    redraw, ///< only its look changed: a hover, the focus, a press, a scroll or the section
    /// Dialog::chosen changed, or Restore defaults was pressed, which asks
    /// for this even when no setting moved: put it in effect and redraw
    changed,
    accepted,  ///< OK: keep Dialog::chosen in effect, save it and close the dialog
    cancelled, ///< Cancel: put Dialog::opened back in effect and close the dialog
    /// MANAGE… was pressed: the host opens the Game files screen and keeps
    /// the dialog open
    manage_game_files,
    /// A button of Your files: open the folder Dialog::folder_to_open names
    /// in the system's file manager, making it first when it is missing; the
    /// dialog stays open. A folder that cannot be opened is told to the
    /// dialog with set_folder_notice.
    open_folder,
    /// RESET on an install ID: the host makes a new ID for
    /// Dialog::install_id_registry and the dialog stays open. Cancel does
    /// not put the old ID back.
    reset_install_id,
    /// EMPTY on Downloaded files: the host removes downloaded files nothing
    /// is using, and the dialog stays open. Cancel does not put them back.
    empty_downloads,
    /// An available language was chosen: download Dialog::download_request,
    /// keep the dialog open, and apply the language when its pack arrives
    download_language,
    /// Kept so a host's switch over every action still compiles. The Library's
    /// Registries tab asks for its own add, not this.
    add_registry,
    /// Kept so a host's switch over every action still compiles. The Library's
    /// Registries tab asks for its own mirror, not this.
    add_mirror,
    /// MORE IN THE LIBRARY… was pressed: the host opens the Library on
    /// Dialog::library_tab over the dialog, which stays open under it
    open_library,
    /// SHOW on Developer's UI kit gallery: the host opens the gallery over
    /// the dialog, which stays open under it
    show_gallery,
    /// CHECK AGAIN on Game Content: the host checks the copy again and the
    /// dialog stays open
    check_content,
    /// COPY AGAIN FROM… on Game Content: the host asks for a place and copies
    /// from it, and the dialog stays open
    copy_content_again,
    /// OPEN FOLDER on Game Content: open the Games folder in the system's
    /// file manager. The dialog stays open.
    open_content_folder,
    /// CANCEL on the bundle being copied: stop that copy. What was already
    /// in the managed folders stays. The dialog stays open.
    cancel_content_copy,
    /// CHECK NOW on Updates: look for a release now. The dialog stays open.
    /// Nothing is asked while the check is Never.
    check_updates,
    /// UPDATE… on Updates: open the release's page, or download the package
    /// where this install has a route. The dialog stays open.
    open_release,
    /// RESTART TO UPDATE on Updates: the verified package installs as the
    /// player quits, and the game asks to quit now. The dialog stays open
    /// until that quit runs.
    restart_update,
    /// RETRY on Updates: start the download of the offered package again.
    /// The dialog stays open.
    retry_update,
    /// ROLL BACK on Updates: ask, then put the kept version back. The dialog
    /// stays open until that question is answered.
    roll_back,
    /// EDIT KEYS… on Key layout, or CHOOSE… on Hold keys that latch: the host
    /// opens the key editor over the dialog, which stays open under it, in
    /// the menus and in a match. Dialog::edit_keys_filter says which list.
    edit_keys,
    /// One of the update card's buttons on Updates: the host carries out the
    /// update centre's action Dialog::update_act names. The dialog stays open.
    update_action,
};

/// Which list EDIT KEYS… or CHOOSE… opens the key editor on.
enum class KeyEditorFilter : uint8_t {
    all,       ///< every action. Key layout's EDIT KEYS… asks for this.
    latchable, ///< actions the registry marks latchable. Hold keys that latch asks for this.
};

/// One installed layout Key layout with a mod offers.
struct KeyLayoutChoice {
    std::string id{};   ///< the layout's id, a word automation names it by
    std::string name{}; ///< the name players see
    /// The layout's works-with names the mod being played.
    bool made_for{};
};

/// The text Key layout shows: the layout's name, or that name and how many
/// actions the player's own layout names. None is the name alone. One is
/// "1 change of your own". More is "{n} changes of your own". The count is
/// the editor's Changed filter: each action the own layout names, once.
///
/// @param name the layout's name, as the player should read it
/// @param changes how many actions the own layout names; negative counts as none
/// @return the text
[[nodiscard]] std::string key_layout_summary_text(std::string_view name, int32_t changes);

/// One of the Library's tabs, as the dialog asks the host to open it
/// (DialogAction::open_library). It mirrors the Library's own tabs, so that
/// the dialog needs nothing of the Library.
enum class LibraryTab : uint8_t {
    mods,       ///< Mods
    maps,       ///< Maps
    languages,  ///< Languages
    updates,    ///< Updates
    registries, ///< Registries
    keys,       ///< Keys. Appended so the earlier tabs keep their numbers.
};

/// How the OA button looks.
enum class ButtonLook : uint8_t {
    idle,    ///< at rest
    hovered, ///< the pointer is over it
    pressed, ///< a press on it is held
};

/// A section of a check's own, shown in place of a section's rows: the rows
/// it holds, and how each is locked. It lets the dialog's tests and the
/// game's own checks give a section more rows than the view under its
/// heading holds, and lock any row. A host never sets one.
struct SectionHooks {
    void* context{}; ///< passed back to each function
    /// Returns the settings a section shows, top to bottom, in place of
    /// page_settings(page), and on Developer of its list and the list's
    /// footer too; the span stays valid while the hooks are set. Null shows
    /// page_settings(page).
    std::span<const Setting> (*settings)(void* context, Page page){};
    /// Returns a setting's lock, given the one Dialog::locks puts on it;
    /// null keeps that one.
    Lock (*lock)(void* context, Setting setting, Lock lock){};
    /// Tells whether a setting's hint lines are its status: a locked row
    /// whose hint lines are its status keeps its control, faded, and fades
    /// only its label line and that control. Null leaves it as the dialog
    /// has it.
    bool (*hint_is_status)(void* context, Setting setting){};
};

/// Developer Mode's list of the standard hacks, in the Developer section:
/// how the profile the game plays resolves each, which of the list's parts
/// are open, and its filter.
struct DeveloperList {
    /// Every standard hack as the profile resolves it, without overrides,
    /// in the registry's order (oa::data::mod_profile::standard_hacks).
    std::vector<oa::data::mod_profile::HackState> profile;
    /// Which areas are open (1) or closed (0), in developer_areas' order;
    /// every one starts closed.
    std::vector<uint8_t> areas_open;
    /// Which hacks are open (1) or closed (0), in the registry's order;
    /// every one starts closed.
    std::vector<uint8_t> hacks_open;
    /// Show Active Only: the list shows only the hacks that are on, and the
    /// areas that hold one.
    bool active_only{};
};

/// One area of the standard hacks, as Developer Mode groups them.
struct HackArea {
    std::string_view name;  ///< the registry's area, such as "ui"
    std::string_view title; ///< its name as players see it, in English, such as "Interface"
    /// Its hacks' places among oa::data::mod_profile::standard_hacks,
    /// alphabetically by their titles in English.
    std::vector<std::size_t> hacks;
};

/// Returns the areas of the standard hacks. The list shows the areas, and
/// the hacks within each, alphabetically by their titles in the language
/// shown, which in English is this order.
///
/// @return each area, alphabetically by its title in English
[[nodiscard]] std::span<const HackArea> developer_areas();

struct Dialog;

/// One language pack a registry offers that is not installed. The host fills
/// it; the Language page reads it and does not ask the content service or
/// the fonts itself.
struct AvailableLanguage {
    std::string tag;          ///< the pack's tag, as the preference stores it
    std::string endonym;      ///< its name in itself
    std::string english_name; ///< its name in English, when the endonym cannot be drawn
    std::string aside;        ///< the size and coverage shown at the row's right
    /// The endonym is drawn; false shows english_name. True while the font
    /// stack is not open, which the host decides.
    bool endonym_drawable{true};
    /// A press on the row chooses it. False greys the row: downloads are off.
    bool choosable{true};

    /// Where a download of this pack stands, from the download queue.
    enum class State : uint8_t {
        offered,     ///< listed, and not queued yet: the field shows the name only
        waiting,     ///< queued, or asking, or paused
        downloading, ///< bytes are arriving, or the pack is being checked
        installing,  ///< the pack is being installed
        failed,      ///< the download failed or was refused
    };

    State state{State::offered}; ///< where the download stands
    uint8_t percent{};           ///< how much of a download has arrived, 0 to 100
    std::string failure;         ///< why it failed; empty otherwise
};

/// Returns a pack's size as the Language page shows it: under 1,000,000
/// bytes, whole kB; under 100 MB, one decimal of MB; otherwise whole MB.
///
/// @param bytes the pack's size
/// @return the size, such as "312 kB", "4.0 MB" or "188 MB"
[[nodiscard]] std::string download_size_text(uint64_t bytes);

/// Returns a package's size as the Library shows it, the figure the update
/// prompt and Settings › Updates share: binary KB below 1 MiB, MB to one
/// decimal below 100 MiB, then whole MB and GB.
///
/// @param bytes the package's size
/// @return the size, such as "23.4 MB"
[[nodiscard]] std::string update_size_text(uint64_t bytes);

/// Returns the aside of an available language.
///
/// A reviewed pack from "OA team" shows its size and the download mark.
/// A reviewed pack from anyone else shows "community", its coverage when it
/// has one, and its size. A pack that is not reviewed shows "not reviewed"
/// in place of "community", whoever published it. Downloads off shows
/// "downloads off" and nothing else.
///
/// @param reviewed a built-in registry reviewed the pack
/// @param publisher who published it
/// @param coverage the fraction of the game's text the pack has, from 0 to
///     1; none when the pack gives no coverage
/// @param bytes the pack's size
/// @param downloads_on the registry can download
/// @return the aside
[[nodiscard]] std::string language_download_aside(
    bool reviewed,
    std::string_view publisher,
    std::optional<double> coverage,
    uint64_t bytes,
    bool downloads_on
);

/// One bundle on Game Content. The host fills the words; the page shows them
/// as they are, so a product name is not looked up again.
struct GameContentRow {
    std::string id;       ///< the bundle's id, a row word such as totala
    std::string name;     ///< the name shown, in the language shown
    std::string size;     ///< the size, already as the page shows it
    std::string check;    ///< the check, already in the language shown
    bool warn{};          ///< the check names files that differ
    bool copying{};       ///< this bundle is being copied; the row offers Cancel
    bool checking{};      ///< a check is running; `check` is its progress
    int32_t bar_done{-1}; ///< the bar's filled part; -1 draws no bar
    int32_t bar_whole{1}; ///< the bar's whole
};

/// One registry's install ID, as Downloads shows it. The host fills this
/// from the registries that use an install ID, built-in first, and never
/// puts a whole ID here: id_text is the short form.
struct InstallIdRow {
    std::string registry_id;   ///< the registry's id, kebab-case
    std::string registry_name; ///< the name a player sees
    bool on{true};             ///< false only while the stored ID is off
    std::string id_text;       ///< the short form, or empty when none is stored
    bool made{};               ///< an ID is stored
};

/// What Downloads shows besides the check for content updates, which the
/// host gives with set_downloads_offer. The switches the player moves live
/// on the dialog, as its chosen settings do, and take effect at OK.
struct DownloadsOffer {
    std::vector<InstallIdRow> install_ids; ///< one row per registry that uses an install ID
    uint64_t kept_bytes{};                 ///< downloaded files kept for resuming and repairs
    bool downloads_busy{};                 ///< a download is queued, running or paused
    /// True when this system reads an https registry address as http. The
    /// check's hint then says so, once.
    bool https_over_http{};
};

/// Where a download of the offered package stands, as Updates shows it.
enum class DownloadShown : uint8_t {
    none,        ///< no download to show; the status is the check's own line
    downloading, ///< "Downloading {version} · {done} of {size}"
    paused,      ///< "Paused during the game"
    ready,       ///< "{version} is ready · {size} downloaded", RESTART TO UPDATE
    stopped,     ///< "The download stopped: {reason}", RETRY
};

/// One of the update centre's actions, as Updates' card shows it.
struct UpdateCardAction {
    int32_t act{};         ///< the host's number for it; Dialog::update_act carries it back
    std::string id{};      ///< the button's word to automation: a-z, 0-9 and hyphens
    std::string caption{}; ///< already as shown, in title case
};

/// The most buttons Updates' card shows.
inline constexpr std::size_t most_update_card_actions = 3;

/// What Updates shows besides the check, which the host fills. The check the
/// player moves lives on the dialog, as its chosen settings do, and takes
/// effect at OK. CHECK NOW, UPDATE…, RESTART TO UPDATE and RETRY act at once.
struct UpdatesOffer {
    std::string version_line; ///< "Open Annihilation {version}", already filled
    std::string status;       ///< the status line, already as the page shows it
    bool can_check{};         ///< CHECK NOW can be pressed
    bool offered{};           ///< a release is out; the button is UPDATE…
    int32_t mode{};           ///< the check as UpdateCheck, so the page can hide the button
    /// "Updated by {name}", already filled, when this copy cannot replace
    /// itself and a packager is named. Empty otherwise. The status line
    /// joins it after the check's own words.
    std::string updated_by;
    /// The background switch and the download's own status. Off leaves the
    /// two rows the page has always shown, so a picture of that page does
    /// not move.
    bool show_download{};
    DownloadShown download{DownloadShown::none}; ///< which download line the status shows
    std::string download_version;                ///< the release being fetched
    std::string download_done;                   ///< bytes so far, as the player reads a size
    std::string download_size;                   ///< the whole file, as the player reads a size
    std::string download_reason;                 ///< why it stopped; not looked up
    /// The version kept beside this one. Empty hides Previous version, so the
    /// page keeps the two rows a picture of it was drawn with.
    std::string kept_version;
    /// The update centre's state, as the card at the page's top shows it,
    /// filled by the host. An empty heading leaves the status row as it was,
    /// with its one button.
    std::string card_status;
    std::string card_installed; ///< "Installed {version}", already filled; empty for none
    std::string card_detail;    ///< the line under the heading; empty for none
    std::string card_note;      ///< guidance under the buttons; empty for none
    int32_t percent{-1};        ///< the card's bar, 0 to 100; -1 draws none
    bool paused{};              ///< the bar is drawn paused, in the lock colour
    /// The state's actions, at most most_update_card_actions, the first in
    /// the accent. Roll back… stays Previous version's.
    std::vector<UpdateCardAction> actions{};
};

namespace updates {

/// The question ROLL BACK asks. {version} is the kept one, {current} this build.
///
/// @param kept the version that would be put back
/// @param current the version that is running
/// @return the question, already in the language shown
[[nodiscard]] oa::ui::kit::Question
roll_back_question(std::string_view kept, std::string_view current);

} // namespace updates

/// Where a check for a release stands, as the host tells update_lines.
enum class UpdateStand : uint8_t {
    idle,       ///< no check has run
    checking,   ///< a check is in progress
    up_to_date, ///< the feed verified and offers nothing newer
    offered,    ///< the feed verified and offers a newer release
    failed,     ///< the check failed, or the feed was refused
    cancelled,  ///< the check was cancelled before it finished
};

/// What the host knows when it asks for Updates' lines.
struct UpdateReading {
    std::string_view version;             ///< this build's version, without a leading v
    std::string_view offer_version;       ///< the offered release's version, when one is out
    UpdateStand stand{UpdateStand::idle}; ///< where the check stands
    bool never{};                         ///< the check the dialog shows is Never
    std::optional<int64_t> checked_at;    ///< when the server answered, seconds since 1970
    int64_t now{};                        ///< the clock now, seconds since 1970
    /// True when this build may replace itself. False keeps CHECK NOW in
    /// place of UPDATE…, because the player asks for a check and does not install.
    bool replaces_itself{true};
    /// Who updates this copy when it cannot replace itself. Empty names nobody.
    std::string_view packager{};
};

/// The lines Updates shows, and which button they allow.
struct UpdateLines {
    std::string version_line; ///< the label
    std::string status;       ///< the status; a key while it is still looked up
    bool can_check{};         ///< CHECK NOW can be pressed
    bool offered{};           ///< the button is UPDATE…
    /// "Updated by {name}", already filled. Empty when this build replaces
    /// itself or names nobody.
    std::string updated_by;
};

/// Returns the lines Updates shows for one reading. The templates are looked
/// up; the version and the time are filled in and are not looked up again.
///
/// @param reading what the host knows
/// @return the lines
[[nodiscard]] UpdateLines update_lines(const UpdateReading& reading);

/// One install-ID switch, as the dialog keeps the player's choice.
struct InstallIdChoice {
    std::string registry_id; ///< the registry's id
    bool on{true};           ///< the switch
};

/// One registered settings page the engine's dialog lists after its own
/// sections, under a divider naming its owner. The rows stay the caller's.
struct RegisteredPage {
    std::string id;                           ///< the page's word in automation names
    std::string title;                        ///< the section heading, as registered
    std::string owner;                        ///< the divider's name, as registered
    const oa::ui::kit::PreferenceRow* rows{}; ///< the page's rows; the caller keeps them
    std::size_t row_count{};                  ///< how many rows
    /// Called by the host once per key OK changed, after the preferences are written.
    void (*applied)(void* state, const char* key, const char* value){};
    void* state{}; ///< what applied is called with
};

/// One key OK changed on a registered page, and who the host tells once it is saved.
struct RegisteredWrite {
    std::string key;   ///< the preference key
    std::string value; ///< the text to store; empty when the key is cleared
    void (*applied)(void* state, const char* key, const char* value){}; ///< the page's applied
    void* state{};                                                      ///< the page's state
};

/// The weapon types and sides a mod fixes, as Engine Limits shows them.
/// The host fills it after open_dialog. The default is the base game: no
/// mod, 256 weapon types and 5 sides, and nothing loaded.
struct ModLimitsShown {
    /// A mod profile is in use. Without one the rows show 256 and 5, in a
    /// match as well as at the menu.
    bool mod{};
    /// The weapon types the profile allows, or 256 for the base game.
    uint32_t weapon_types{256};
    /// The sides the profile allows, or 5 for the base game.
    uint32_t sides{5};
    /// The weapon types the playing match loaded. Empty outside a match.
    /// The rows ignore it for the base game.
    std::optional<uint32_t> loaded_weapon_types;
    /// The sides the playing match loaded. Empty outside a match. The rows
    /// ignore it for the base game.
    std::optional<uint32_t> loaded_sides;
};

/// A modifier the practice pad on Accessibility tells apart.
enum class PracticeModifier : uint8_t {
    none,  ///< not a modifier: a letter, a digit or another key
    shift, ///< Shift
    ctrl,  ///< Ctrl
    alt,   ///< Alt, Option on a Mac keyboard
};

/// One key the host hands the practice pad: a modifier, or another key and
/// the legend its keycap shows.
struct PracticeKey {
    PracticeModifier modifier{PracticeModifier::none}; ///< which modifier; none for another key
    /// What the key's cap shows, as the player reads it: "A", "Shift", "Ctrl".
    std::string label;
};

/// What the practice pad's last key did, with the helpers the dialog shows.
enum class PracticeOutcome : uint8_t {
    none,      ///< nothing pressed yet
    latched,   ///< a modifier tapped alone stays down for the next key (Sticky modifiers)
    locked,    ///< a modifier tapped twice stays down until it is tapped again
    unlocked,  ///< a locked modifier tapped again lets go
    counted,   ///< a key counted, with the modifiers held or latched
    too_short, ///< a key let go before Slow keys' time: it did not count
    bounced,   ///< a key pressed again within Bounce keys' time: it did not count
    held,      ///< Hold to confirm's chord held to its end: the order would go
    cancelled, ///< Hold to confirm's chord let go before its end: nothing happens
};

/// The bit of a modifier in PracticePad's sets of modifiers.
///
/// @param modifier the modifier
/// @return its bit; 0 for none
[[nodiscard]] constexpr uint8_t practice_bit(PracticeModifier modifier) noexcept {
    return modifier == PracticeModifier::none
               ? uint8_t{0}
               : static_cast<uint8_t>(1U << (static_cast<uint32_t>(modifier) - 1U));
}

/// Accessibility's practice pad: the keys the player tries there, as the
/// helpers the dialog shows treat them. Keys pressed here never reach the
/// game or the dialog's other controls. Each set of modifiers is
/// practice_bit's bits.
struct PracticePad {
    uint8_t held{};    ///< the modifiers down now
    uint8_t latched{}; ///< the modifiers Sticky modifiers keeps down for the next key
    uint8_t locked{};  ///< the modifiers a second tap keeps down until tapped again
    /// The modifiers pressed since the last other key, so that one let go
    /// with none pressed in between counts as a tap.
    uint8_t alone{};
    std::string key_down;       ///< the other key held now; empty for none
    uint8_t key_modifiers{};    ///< the modifiers held or latched as it went down
    uint64_t key_down_ms{};     ///< when it went down, in milliseconds
    bool key_counted{};         ///< it counted already (Slow keys' time passed)
    bool key_bounced{};         ///< it came again within Bounce keys' time and does not count
    std::string last_released;  ///< the last key that counted and was let go, for Bounce keys
    uint64_t last_release_ms{}; ///< when it was let go, in milliseconds
    PracticeOutcome outcome{PracticeOutcome::none}; ///< what the last key did
    std::string outcome_key;                        ///< the key it was about
    uint8_t outcome_modifiers{};                    ///< the modifiers with it
    uint8_t outcome_released{}; ///< the latched modifiers the key used, which then let go
    uint32_t outcome_ms{}; ///< how long it was held, or how soon it came again, in milliseconds
    /// Hold to confirm's chord, the confirm modifier and D, is held: its bar fills.
    bool holding{};
    uint64_t hold_started_ms{}; ///< when the chord's hold began, in milliseconds
    uint64_t now_ms{};          ///< the time the pad was last told, in milliseconds
};

/// One open dialog. A host reads opened, chosen, defaults, restored, page
/// and forget_renderer_failures, sets acceleration, and reads and may keep
/// developer between openings; section_hooks is set only by tests and
/// checks; the other members after them are the dialog's own.
struct Dialog {
    EngineSettings opened{};           ///< in effect as it opened; Cancel puts them back
    EngineSettings chosen{};           ///< what it shows; in effect as they change
    EngineSettings defaults{};         ///< what Restore defaults sets
    Locks locks{};                     ///< what cannot be changed now
    AccelerationStatus acceleration{}; ///< Hardware acceleration's status
    std::string version;               ///< the header's version text
    /// A note beside the version, already translated. Empty shows none.
    /// A shared game's note still takes the header instead.
    std::string header_note;
    Page page{Page::controls}; ///< the section shown
    bool restored{};           ///< Restore defaults was pressed
    /// The times the player asked, since the dialog opened, for the graphics
    /// card to be tried afresh: each press of Restore defaults, and each
    /// time Hardware acceleration passed to a higher level, from Off to
    /// Basic or Full or from Basic to Full. The count stays if the row goes
    /// back down.
    uint32_t forget_renderer_failures{};
    int32_t hovered{no_control}; ///< the control under the pointer
    int32_t pressed{no_control}; ///< the control a held press is on
    int32_t focused{no_control}; ///< the control with the keyboard focus; shown once a key moves it
    bool dragging{};             ///< the held press drags a slider's knob or the scroll bar's thumb
    /// Each section's scroll offset, in source pixels from its top; clamped
    /// to the section's limit wherever it is used. Every section starts at
    /// its top when the dialog opens.
    std::array<int32_t, page_count> scroll{};
    /// The part of a source pixel the wheel has turned and not yet
    /// scrolled; negative towards the section's top.
    float wheel_rows{};
    /// The pixel row of the scroll bar's thumb, from the thumb's top, that a
    /// held press on the bar holds.
    int32_t scroll_grab{};
    bool pointer_known{}; ///< the dialog has had a pointer event
    int32_t pointer_x{};  ///< the last pointer event's column, in source pixels
    int32_t pointer_y{};  ///< the last pointer event's row, in source pixels
    /// The columns a finger's held press was moved by to reach the control
    /// it took (dialog_finger_down); the press's moves and its release are
    /// moved as far. Zero for a press on a control and for every mouse
    /// press.
    int32_t finger_shift_x{};
    int32_t finger_shift_y{}; ///< the rows a finger's held press was moved by, as finger_shift_x
    /// A check's own section in place of the dialog's; null for the dialog's.
    const SectionHooks* section_hooks{};
    /// The unit limit slider's highest stop (highest_offered_unit_limit).
    uint16_t highest_offered_unit{highest_unit_limit};
    /// The Screen size slider's stops, in order: Desktop, then the sizes
    /// the display offers, narrower first. open_dialog sets screen_sizes;
    /// the game sets the display's own once the dialog has opened. Never
    /// empty.
    std::vector<ScreenSize> offered_screen_sizes{screen_sizes.begin(), screen_sizes.end()};
    /// The window's own size, which Screen size shows in a window until its
    /// knob moves, whatever the setting chosen: Desktop leaves a window as
    /// it is. Empty in full screen, and once the knob has moved. The game
    /// sets it with the stops, which list it.
    std::optional<ScreenSize> window_screen_size;
    /// The stop of offered_screen_sizes that stands for the window's own
    /// size where the display offers no such size, which the slider shows
    /// as "Custom"; empty when there is none. The game sets it with the
    /// stops.
    std::optional<ScreenSize> custom_screen_size;
    /// The player's own folder, as an absolute UTF-8 path, which the Your
    /// files row shows; the host sets it once the dialog has opened. Empty
    /// shows no path.
    std::string user_folder;
    /// The Your files button the keys mark, which Space presses: Saves at
    /// first.
    FolderButton folder_marked{FolderButton::saves};
    /// The Your files button under the pointer, or a held press is on;
    /// folder_button_count for none.
    std::size_t folder_hovered{folder_button_count};
    /// The folder the last DialogAction::open_folder asks for.
    FolderButton folder_to_open{FolderButton::saves};
    /// Why the last folder asked for could not be opened, which the Your
    /// files row's second hint line shows in amber; empty for none.
    std::string folder_notice;
    /// Which settings it shows.
    DialogKind kind{DialogKind::engine};
    /// The game has touch controls, so that the engine's settings list
    /// Touch (dialog_pages); a host gives it to open_dialog and keeps it
    /// with set_touch_controls.
    bool touch{};
    /// A gamepad has sent input in this run, so that the engine's settings
    /// list Controller (dialog_pages); a host gives it to open_dialog and
    /// keeps it with set_controller_section.
    bool controller{};
    /// The gamepad in use reaches the game through Steam Input, so that
    /// Controller's first row says so and how to turn it off; a host keeps
    /// it with set_controller_section.
    bool steam_input{};
    /// The game runs on a Steam Deck whose screen refreshes this many times
    /// a second (Inputs::steam_deck_panel_hz), so that Maximum frame rate's
    /// hint has a second line naming the rate it starts at; 0 elsewhere. A
    /// host sets it as the dialog opens.
    uint32_t steam_deck_panel_hz{};
    /// The Game files section is listed (dialog_pages): a host gives it to
    /// open_dialog where the platform brings game files in and the dialog is
    /// the main menu's.
    bool game_files{};
    /// System lists a switch for each file type. Off, it shows the text row
    /// instead. The default is this system's own capability
    /// (file_types_switchable). open_dialog starts from that; a test may set
    /// it afterwards to draw the other layout. OK reads the settings either
    /// way.
    bool file_type_switches{file_types_switchable};
    /// File Type Registration's hint when macOS kept this copy as the opener
    /// of a type OK asked it to hand back. Empty shows the usual hint. The
    /// host sets it for this run, and clears it when every switch matches
    /// what was asked.
    std::string file_type_kept_hint{};
    /// The Game files section's summary: "3.1c · Core Contingency · Battle Tactics · music · 1 mod".
    std::string game_files_summary{};
    /// Its sizes line: what the game files use and what is free on the device.
    std::string game_files_sizes{};
    /// Where the files are: the platform's folder_location word.
    std::string game_files_location{};
    /// The device's name in the backups switch's hint.
    std::string game_files_device{};
    /// Weapon types and sides, as the mod fixes them and as a match loaded
    /// them. The host sets it as the dialog opens. The default is the base game.
    ModLimitsShown mod_limits{};
    /// The mod being played, as players see its name. Empty while no mod is
    /// played, and Controls then hides Key layout with that mod.
    std::string played_mod;
    /// That mod's id, which the choice is saved under. Empty while no mod
    /// is played.
    std::string played_mod_id;
    /// What Key layout shows: the layout in use and how many keys the player
    /// changed. The host sets it as the dialog opens and after the editor saves.
    std::string key_layout_summary;
    /// The key layout card's name of the layout in use. Empty shows
    /// key_layout_summary as the name, and no count. The host sets it with the
    /// summary.
    std::string key_layout_name;
    /// How many actions the player's own layout over it changes, which the
    /// card's badge counts; 0 shows no badge. The host sets it with the summary.
    int32_t key_layout_changes{};
    /// The layout in use is the one chosen for the mod being played, so that
    /// the card says it applies with that mod only; false, it applies to every
    /// game. The host sets it with the summary.
    bool key_layout_mod_only{};
    /// The modifier Key layout's hint names in "{primary}+/". The host gives
    /// Cmd on macOS; elsewhere, and in the tests and their pins, it is Ctrl.
    std::string primary_modifier{"Ctrl"};
    /// Which list the next EDIT KEYS… or CHOOSE… opens the key editor on.
    /// EDIT KEYS… sets all. CHOOSE… on Hold keys that latch sets latchable.
    KeyEditorFilter edit_keys_filter{KeyEditorFilter::all};
    /// How many latchable actions the layout in use holds on. The host sets
    /// it as the dialog opens and after the editor saves. The row's total is
    /// the registry's count of latchable actions.
    uint32_t latched_count{};
    /// Installed layouts Key layout with the mod offers, after Same as the
    /// game. The row lists those made for the mod first. The host sets it as
    /// the dialog opens.
    std::vector<KeyLayoutChoice> key_layouts;
    /// The layout chosen for the mod being played: `same`, or one of
    /// key_layouts' ids. EngineSettings::key_layout_for_mod holds the same
    /// text, which the row reads and writes. The host sets both as the dialog
    /// opens.
    std::string key_layout_for_mod{"same"};
    /// Developer Mode's list of the standard hacks.
    DeveloperList developer{};
    /// The language the operating system's preferred locales choose, which
    /// the Language drop-down's System default names; null names English.
    const oa::data::languages::Language* system_language{};
    /// The tags of the languages whose packs ask for multiplayer chat in
    /// UTF-8 (unicode: true): while the language chosen is one, Enable
    /// Unicode Multiplayer Chat shows On, locked. A host sets it as the
    /// dialog opens.
    std::vector<std::string> unicode_chat_languages{};
    /// Language packs the enabled registries offer that are not installed,
    /// in the order the page lists them under AVAILABLE TO DOWNLOAD. Empty
    /// draws the page as it did with nothing to download. The host fills it
    /// as the dialog opens and as the catalogue or a download moves.
    std::vector<AvailableLanguage> available_languages{};
    /// The tag DialogAction::download_language asks the host to download.
    std::string download_request;
    /// Language lists MORE IN THE LIBRARY… under its row: a host sets it as
    /// the engine's settings open on the main menu, where the Library can
    /// open over them; never in a match, and never for Language alone.
    bool library_link{};
    /// The tab DialogAction::open_library asks the host to open the Library on.
    LibraryTab library_tab{LibraryTab::mods};
    /// The row whose drop-down list is open: its control; no_control while
    /// no list is open. An open list takes every pointer event and key.
    int32_t open_list{no_control};
    /// The open list's item the pointer or the keys mark, from 0.
    int32_t list_marked{};
    /// The open list's item a held press is on; -1 for none.
    int32_t list_pressed{-1};
    /// The open list's first item shown, while it holds more items than it
    /// shows.
    int32_t list_first{};
    /// What Downloads shows, which the host sets with set_downloads_offer
    /// after the dialog has opened. open_dialog clears it.
    DownloadsOffer downloads{};
    /// What Updates shows, which the host fills after the dialog has opened.
    /// open_dialog clears it, and sets mode from the check in effect.
    UpdatesOffer updates{};
    /// The update centre's action the card's button asked for
    /// (DialogAction::update_action), as UpdateCardAction::act; -1 for none.
    int32_t update_act{-1};
    /// A short word at the right of Updates' entry in the section list, in
    /// the accent: how far a download has come ("41%") or that a release is
    /// ready, as the update centre's pill says it. Empty shows none. The host
    /// keeps it as the update moves.
    std::string updates_badge;
    /// Each install-ID switch as the player has it, in downloads.install_ids'
    /// order. A switch takes effect at OK.
    std::vector<InstallIdChoice> install_id_chosen{};
    /// Those switches as the dialog opened, which Cancel puts back.
    std::vector<InstallIdChoice> install_id_opened{};
    /// The registry the last RESET names.
    std::string install_id_registry{};
    /// The size class it is laid out at: its size, padding, section list and
    /// content column are the class's (oa::ui::kit::metrics_of), and a larger
    /// class shows more rows, each at its Compact size. A host sets it from
    /// the room it has; the scroll offsets are held to the class's limits
    /// wherever they are used.
    oa::ui::kit::SizeClass size_class{oa::ui::kit::SizeClass::compact};
    /// Registered pages listed after the engine's sections. Empty leaves the
    /// list as the engine's own, Developer pinned at its foot.
    std::vector<RegisteredPage> registered_pages;
    /// Which registered page is open while page is Page::registered.
    std::size_t registered_page{};
    /// The preference values the registered rows show.
    oa::ui::kit::PreferenceModel registered_values{};
    /// Those values as the pages were given. Cancel puts them back, and OK
    /// writes a key whose text differs.
    oa::ui::kit::PreferenceModel registered_opened{};
    /// The section list's scroll, in source pixels, while the list is taller
    /// than its room: registered pages, or the engine's own fourteen sections
    /// at Compact. Developer stays pinned under the divider and does not scroll.
    int32_t nav_scroll{};
    /// The part of a source pixel the wheel has turned over the section list
    /// and not yet scrolled.
    float nav_wheel_rows{};
    /// SHOW AGAIN was pressed. OK forgets that the welcome setup is done, so
    /// it shows at the next main menu. Cancel leaves the preference.
    bool welcome_again{};
    /// Each bundle Game Content lists, in the table's order. The host fills
    /// it as the dialog opens and when CHECK AGAIN returns. Empty still shows
    /// the sentence and the three buttons.
    std::vector<GameContentRow> game_content;
    /// Reset on Reset window positions was pressed, which reports
    /// DialogAction::changed: the host forgets the place of every window the
    /// player moved, at once, clears this and sets window_places_reset.
    /// Cancel does not put the places back.
    bool window_reset_asked{};
    /// The host forgot every moved window's place while the dialog was open,
    /// so that the row says each window is back in its place.
    bool window_places_reset{};
    /// Accessibility's practice pad.
    PracticePad practice{};
};

/// Returns every standard hack as Developer Mode shows it: as the profile
/// resolves it, with the chosen overrides laid over it while Developer Mode
/// is on (EngineSettings::developer_mode).
///
/// @param dialog the dialog
/// @return one state for each standard hack, in the registry's order
[[nodiscard]] std::vector<oa::data::mod_profile::HackState> shown_hacks(const Dialog& dialog);

/// Counts the standard hacks that are on as Developer Mode shows them: the
/// X of Show Active Only (X/Y), whose Y is every standard hack.
///
/// @param dialog the dialog
/// @return the hacks that are on
[[nodiscard]] std::size_t active_hack_count(const Dialog& dialog);

/// The font a text of the dialog is drawn in.
using DialogFont = oa::ui::kit::FontRole;

/// One part of the dialog as it is drawn now: a text or a control, and the
/// rectangle it keeps to.
struct LayoutPart {
    oa::ui::frontend_renderer::SourceRect rect{}; ///< in source pixels from the dialog's top left
    std::string text;                             ///< the text drawn in it; empty for a control
    DialogFont font{};                            ///< the font of text
    int32_t tracking{};          ///< extra columns after each of text's glyphs but the last
    int32_t control{no_control}; ///< the control it is; no_control for a text
};

/// The fonts the dialog and the OA button draw their texts in.
using DialogFonts = oa::ui::kit::Fonts;

/// Returns the parts the dialog draws now: the header's texts, the list's
/// entries, the open section's heading, labels, hints, locks, controls and
/// values, the scroll bar while the section scrolls, and the footer's
/// buttons. No two overlap, and each lies inside the dialog's edge. Of the
/// open section's rows only the parts that lie wholly in its view are
/// listed; a part the view cuts is drawn but not listed. The player's own
/// folder's path shows as its tail that fits its place (path_tail): measured in the
/// fonts when they are given, else at estimated_character_width a
/// character.
///
/// @param dialog the dialog
/// @param fonts the fonts the dialog is drawn in; null to estimate widths
/// @return the parts
[[nodiscard]] std::vector<LayoutPart>
dialog_layout(const Dialog& dialog, const DialogFonts* fonts = nullptr);

/// Returns a UTF-8 text's width as the dialog draws it: the characters a
/// font draws at its glyphs' widths, and the others in the modern fonts.
///
/// @param fonts the dialog's fonts
/// @param font which of them
/// @param text the text, in UTF-8
/// @return the width, in source pixels
[[nodiscard]] int32_t
dialog_text_width(const DialogFonts& fonts, DialogFont font, std::string_view text);

/// Loads the dialog's fonts from the game's files: the game's button font as
/// the regular one and its label font as the small one, each readied for
/// text in one colour (oa::ui::frontend_renderer::text_font): its letters
/// keep their shading and lose the dark outline round them.
///
/// Throws std::runtime_error when a font or the game's palette is missing, or a
/// font is malformed.
///
/// @param assets the game's files
/// @return the fonts
[[nodiscard]] DialogFonts load_dialog_fonts(oa::AssetStore& assets);

/// Gives the engine's dialog the registered pages to list, and the preference
/// values their rows read. A dialog that is not the engine's settings keeps
/// none. The rows stay the caller's for as long as the dialog shows them.
///
/// @param[in,out] dialog the dialog
/// @param pages the pages, in the order the list shows them
/// @param values the stored preferences the rows read and OK writes back
void set_registered_pages(
    Dialog& dialog,
    std::span<const RegisteredPage> pages,
    const oa::platform::preferences::Values& values
);

/// Returns the registered keys whose stored text differs from the values the
/// dialog was given. A key missing from either side compares as empty.
///
/// @param dialog the dialog
/// @return one write for each changed key, in page order then row order
[[nodiscard]] std::vector<RegisteredWrite> registered_writes(const Dialog& dialog);

/// Opens the dialog over settings in effect.
///
/// @param[out] dialog the dialog; whatever it held is replaced
/// @param current the settings in effect
/// @param defaults what Restore defaults sets
/// @param locks what cannot be changed now
/// @param version the header's version text
/// @param page the section to show
/// @param acceleration Hardware acceleration's status
/// @param highest_offered_unit the unit limit slider's highest stop, in units
///     per player (highest_offered_unit_limit)
/// @param profile_hacks every standard hack as the profile the game plays
///     resolves it, in the registry's order (DeveloperList::profile); empty
///     gives every one off, as 3.1c plays it
/// @param system_language the language the operating system's preferred
///     locales choose, which the Language drop-down's System default names;
///     null names English
/// @param touch the game has touch controls, so that the dialog lists Touch
///     (Dialog::touch); without them, a dialog asked to show Touch shows its
///     first section instead
/// @param game_files the dialog lists Game files (Dialog::game_files);
///     without it, a dialog asked to show Game files shows its first section
/// @param controller a gamepad has sent input in this run, so that the
///     dialog lists Controller (Dialog::controller); without it, a dialog
///     asked to show Controller shows its first section
void open_dialog(
    Dialog& dialog,
    const EngineSettings& current,
    const EngineSettings& defaults,
    const Locks& locks,
    std::string_view version,
    Page page,
    const AccelerationStatus& acceleration = {},
    uint16_t highest_offered_unit = highest_unit_limit,
    std::span<const oa::data::mod_profile::HackState> profile_hacks = {},
    const oa::data::languages::Language* system_language = nullptr,
    bool touch = false,
    bool game_files = false,
    bool controller = false
);

/// Gives Downloads what it shows. The first call, and any call whose
/// registries are not the same ids in the same order, copies each row's
/// switch into the dialog's chosen and opened switches. A later call with
/// the same registries refreshes the IDs, the sizes and whether downloads
/// are busy, and leaves the switches as the player has them.
///
/// @param[in,out] dialog the dialog
/// @param offer what Downloads shows
void set_downloads_offer(Dialog& dialog, const DownloadsOffer& offer);

/// Opens the dialog over a mod's options (ui.options-dialog): its sections
/// list the mod options alone, and only EngineSettings::mod_options change.
///
/// @param[out] dialog the dialog; whatever it held is replaced
/// @param current the settings in effect, the mod's options among them
/// @param defaults what Restore defaults sets
/// @param locks what cannot be changed now: Locks::mex_snap and wreck_snap
/// @param version the header's version text
/// @param page the section to show; one of the mod options' or the first
void open_mod_options_dialog(
    Dialog& dialog,
    const EngineSettings& current,
    const EngineSettings& defaults,
    const Locks& locks,
    std::string_view version,
    Page page = Page::mod_keys
);

/// Opens the dialog over the Language section alone
/// (DialogKind::language_text), as the Game files screen opens it before
/// the game's files are installed: it lists that one section and keeps
/// Restore defaults, which restores only that section's settings, Cancel
/// and OK. Drawn with DialogFonts that hold no glyphs, every text is in the
/// modern fonts.
///
/// @param[out] dialog the dialog; whatever it held is replaced
/// @param current the settings in effect
/// @param defaults what Restore defaults sets
/// @param locks what cannot be changed now
/// @param version the header's version text
/// @param system_language the language the operating system's preferred
///     locales choose, which the Language drop-down's System default names;
///     null names English
void open_language_text_dialog(
    Dialog& dialog,
    const EngineSettings& current,
    const EngineSettings& defaults,
    const Locks& locks,
    std::string_view version,
    const oa::data::languages::Language* system_language = nullptr
);

/// Tells the dialog why the folder its last DialogAction::open_folder asked
/// for could not be opened, or that it was; the Your files row's second hint
/// line shows the reason in amber until another folder opens.
///
/// @param[in,out] dialog the dialog
/// @param reason why it could not be opened, in a few words; empty when it
///     opened
/// @return DialogAction::redraw when the line changed, else DialogAction::none
[[nodiscard]] DialogAction set_folder_notice(Dialog& dialog, std::string_view reason);

/// Shortens a folder's path to a tail that fits a width: the whole path when
/// it fits; else "..." and the most of its last components, each whole, that
/// fit after it, with the separator before them; else "..." and as much of
/// the end of its last component as fits. A separator at the path's end is
/// dropped first.
///
/// @param path the path, in UTF-8; '/' and '\\' separate its components
/// @param width the room, in source pixels
/// @param text_width a text's width in the font it is drawn in
/// @return the text to show, in UTF-8; "..." alone when nothing more fits
[[nodiscard]] std::string path_tail(
    std::string_view path, int32_t width, const std::function<int32_t(std::string_view)>& text_width
);

/// The width dialog_layout counts each character of the player's own
/// folder's path at without the fonts, in source pixels.
inline constexpr int32_t estimated_character_width = oa::ui::kit::estimated_character_width;

/// Gives the dialog Hardware acceleration's status as it is now; a host
/// calls it each frame while the dialog is open.
///
/// @param[in,out] dialog the dialog
/// @param acceleration the status
/// @return DialogAction::redraw when the status changed, else DialogAction::none
[[nodiscard]] DialogAction
set_acceleration_status(Dialog& dialog, const AccelerationStatus& acceleration) noexcept;

/// Tells the dialog whether the game has touch controls now; a host calls
/// it each frame while the dialog is open, so that Touch is listed from the
/// moment a finger turns the touch controls on. A dialog that stops listing
/// Touch while it shows it shows its first section, and the focus leaves
/// Touch's controls.
///
/// @param[in,out] dialog the dialog
/// @param touch the game has touch controls (Dialog::touch)
/// @return DialogAction::redraw when the list changed, else DialogAction::none
[[nodiscard]] DialogAction set_touch_controls(Dialog& dialog, bool touch) noexcept;

/// Tells the dialog whether a gamepad has sent input in this run and
/// whether it reaches the game through Steam Input; a host calls it each
/// frame while the dialog is open (as set_touch_controls), so that
/// Controller is listed from the moment a gamepad is used and its Steam
/// Input notice shows while it applies. A dialog that stops listing
/// Controller while it shows it shows its first section, and the focus
/// leaves Controller's controls; the notice coming or going while
/// Controller shows keeps the focus on the row it was on.
///
/// @param[in,out] dialog the dialog
/// @param controller a gamepad has sent input in this run (Dialog::controller)
/// @param steam_input the gamepad reaches the game through Steam Input
///     (Dialog::steam_input)
/// @return DialogAction::redraw when the list or the section changed, else
///     DialogAction::none
[[nodiscard]] DialogAction
set_controller_section(Dialog& dialog, bool controller, bool steam_input) noexcept;

/// Moves the pointer: hovers a control, or drags what a held press holds. A
/// slider's knob follows the pointer's column only; the scroll bar's thumb
/// follows its row only, wherever the pointer goes, and the open section
/// scrolls with the thumb. Over an open drop-down list it marks the item
/// under it. While a finger's press is held, the point is moved as the
/// press was (dialog_finger_down).
///
/// @param[in,out] dialog the dialog
/// @param x the pointer's column, in source pixels from the dialog's left edge
/// @param y the pointer's row, in source pixels from the dialog's top edge
/// @return what the move asks of the host
[[nodiscard]] DialogAction dialog_pointer_move(Dialog& dialog, int32_t x, int32_t y);

/// Presses the pointer's button: a press on a control holds it, and moves
/// the keyboard focus to it once a key has shown the focus. A press on a
/// slider moves its knob to the nearest stop. A press on the scroll bar
/// holds the bar and leaves the focus where it is: on the thumb it grabs
/// the thumb where it is pressed; on the well above or below the thumb, the
/// thumb's middle jumps to the pointer, the section scrolls with it and the
/// drag starts there. While a drop-down list is open, a press on one of its
/// items holds the item, and a press anywhere else, its field included,
/// closes the list and does nothing more.
///
/// @param[in,out] dialog the dialog
/// @param x the pointer's column, in source pixels from the dialog's left edge
/// @param y the pointer's row, in source pixels from the dialog's top edge
/// @return what the press asks of the host
[[nodiscard]] DialogAction dialog_pointer_down(Dialog& dialog, int32_t x, int32_t y);

/// Presses with a finger: as dialog_pointer_down, but a press with no
/// control under it takes the nearest control whose pressable part lies
/// within `reach` of it, pressed at that part's point nearest the finger;
/// while a drop-down list is open, the nearest of its items. The press's
/// moves and its release are then moved as far as the press was
/// (Dialog::finger_shift_x and finger_shift_y), so that a release where the
/// finger landed acts on the control it took. With nothing within reach it
/// does what dialog_pointer_down does.
///
/// @param[in,out] dialog the dialog
/// @param x the finger's column, in source pixels from the dialog's left edge
/// @param y the finger's row, in source pixels from the dialog's top edge
/// @param reach how far a control may lie from the finger, in source pixels:
///     the touch controls' pick distance in the dialog's own pixels
/// @return what the press asks of the host
[[nodiscard]] DialogAction dialog_finger_down(Dialog& dialog, int32_t x, int32_t y, int32_t reach);

/// Releases the pointer's button: a release over the control the press held
/// acts on it; over a drop-down's field, it opens the field's list, marking
/// the item chosen. A release over the list item the press held chooses the
/// item and closes the list. A finger's release is moved as its press was
/// (dialog_finger_down).
///
/// @param[in,out] dialog the dialog
/// @param x the pointer's column, in source pixels from the dialog's left edge
/// @param y the pointer's row, in source pixels from the dialog's top edge
/// @return what the release asks of the host
[[nodiscard]] DialogAction dialog_pointer_up(Dialog& dialog, int32_t x, int32_t y);

/// Takes a key.
///
/// Enter is OK and Escape Cancel, whatever has the focus. Tab and Shift+Tab
/// move the focus to the next and the previous control in the declared
/// order, round from one end to the other: the open section's rows that
/// take a change (on Developer, then its list's rows, Show Active Only and,
/// while Developer Mode is on, Restore profile values), the footer's
/// buttons, then the sections' entries. Up and Down move the focus to the
/// control above or below, by where the controls lie
/// (oa::ui::kit::focus_toward): the open section's rows and Developer's
/// list are each searched first from within, rows they do not show
/// included. Left and
/// Right step a focused control that takes steps: a switch, a strip, a
/// slider or a drop-down, Your files' mark, a row of Developer's list, Show
/// Active Only. From any other control they move the focus to the control
/// on that side, as Up and Down do. A row's control lies across its row
/// for the arrows. An arrow that finds no control that way leaves the focus
/// where it is. From no focus, Up shows the focus on the last control in
/// the declared order; Down, Left, Right, Tab and Space on the first.
///
/// Page Up, Page Down, Home and End scroll the open section whatever has
/// the focus, and never move or show it. A key that moves the focus onto a
/// row, or acts on a focused row, first scrolls the least that shows the
/// row whole. Space opens a focused drop-down's list, and Left and Right
/// step its choice. While a list is open the keys work it: Up and Down mark
/// the item above or below, Page Up and Page Down a list's height of items
/// away, Home and End the first and the last; Enter and Space choose the
/// marked item and close the list; Escape closes it unchanged; Tab and
/// Shift+Tab close it and move the focus. In Developer Mode's list, Space
/// opens or closes an area or a hack, and Left and Right close and open an
/// area or turn a hack off and on. Y and N do nothing.
///
/// @param[in,out] dialog the dialog
/// @param key the key's meaning
/// @return what the key asks of the host
[[nodiscard]] DialogAction dialog_key(Dialog& dialog, DialogKey key);

/// Turns the mouse wheel over the dialog: scrolls the open section 24 source
/// pixels a notch, when its rows are taller than the space they scroll in.
/// A fraction of a pixel carries over to the next turn; what is carried
/// towards an end the section has reached is dropped, and all of it when
/// another section shows. A turn outside the dialog, or while a press is
/// held, does nothing. While a drop-down list is open, a turn over it
/// scrolls a list that holds more items than it shows, an item a notch,
/// and the section stays.
///
/// @param[in,out] dialog the dialog
/// @param x the pointer's column, in source pixels from the dialog's left edge
/// @param y the pointer's row, in source pixels from the dialog's top edge
/// @param notches the wheel's turn, positive away from the player, which
///     scrolls towards the section's top
/// @return what the turn asks of the host: DialogAction::redraw when the
///     section scrolled, else DialogAction::none
[[nodiscard]] DialogAction dialog_wheel(Dialog& dialog, int32_t x, int32_t y, float notches);

/// Returns how far Page Up and Page Down scroll the open section: its view
/// at the dialog's size class, less three hint lines, so that what showed
/// at one edge still shows at the other.
///
/// @param dialog the dialog
/// @return the rows, in source pixels
[[nodiscard]] int32_t dialog_page_step(const Dialog& dialog) noexcept;

/// Lays the dialog out at a size class (Dialog::size_class) and scrolls its
/// section list the least that shows the open section's entry there.
///
/// @param[in,out] dialog the dialog
/// @param size_class the class
void set_dialog_size_class(Dialog& dialog, oa::ui::kit::SizeClass size_class);

/// Takes a key the player presses or lets go while Accessibility shows: the
/// practice pad tries it with the helpers the dialog shows (Sticky
/// modifiers, Slow keys, Bounce keys and Hold to confirm), and it reaches
/// nothing else. On any other section the pad takes no key. A host hands it
/// the modifiers and the keys that print a legend, and the keys the dialog
/// answers to (dialog_key) still go there.
///
/// @param[in,out] dialog the dialog
/// @param key the key
/// @param down true as it goes down, false as it is let go
/// @param now_ms the time, in milliseconds, from one clock for every call
/// @return redraw when the pad shows something else; none when it took no key
[[nodiscard]] DialogAction
dialog_practice_key(Dialog& dialog, const PracticeKey& key, bool down, uint64_t now_ms);

/// Moves the practice pad's time on: a key held for Slow keys' time counts,
/// and Hold to confirm's bar fills and ends. A host calls it every frame
/// while the dialog is open.
///
/// @param[in,out] dialog the dialog
/// @param now_ms the time, in milliseconds, from the clock dialog_practice_key is given
/// @return redraw while the pad's bar or outcome moved; none otherwise
[[nodiscard]] DialogAction dialog_practice_tick(Dialog& dialog, uint64_t now_ms);

/// Counts the settings the dialog shows changed from what it opened with: each
/// setting whose stored value would change, an install ID's switch, a
/// registered page's key and Skirmish players. The footer shows the count.
///
/// @param dialog the dialog
/// @return the count; 0 when nothing changed
[[nodiscard]] std::size_t changed_settings_count(const Dialog& dialog);

/// Tells whether a point lies on the dialog at Compact.
///
/// @param x the point's column, in source pixels from the dialog's left edge
/// @param y the point's row, in source pixels from the dialog's top edge
/// @return true inside its dialog_width by dialog_height
[[nodiscard]] bool dialog_contains(int32_t x, int32_t y) noexcept;

/// Returns the most the open section scrolls at the dialog's size class, or
/// on Developer its list: the rows' height less their view's. A
/// larger class shows more of the same rows, and scrolls less.
///
/// @param dialog the dialog
/// @return the greatest offset, in points; 0 when the rows fit their view
[[nodiscard]] int32_t scroll_limit(const Dialog& dialog);

/// Tells whether a point lies on the dialog at its size class.
///
/// @param dialog the dialog
/// @param x the point's column, in points from the dialog's left edge
/// @param y the point's row, in points from the dialog's top edge
/// @return true inside its class's width and height
[[nodiscard]] bool dialog_contains(const Dialog& dialog, int32_t x, int32_t y) noexcept;

/// Draws the dialog at its size class (Dialog::size_class), at the
/// placement's whole scale. Its header shows the Open Annihilation icon,
/// scaled to 20 by 20 source pixels at the surface's own resolution; without
/// the icon it shows the OA mark, green letters in a green outlined square.
///
/// @param[in,out] target the surface
/// @param placement where the dialog's top left corner lands, and its scale
/// @param dialog the dialog
/// @param fonts its fonts
/// @param icon the Open Annihilation icon; an empty picture draws the OA mark
void draw_dialog(
    oa::ui::frontend_renderer::Surface& target,
    const oa::ui::frontend_renderer::Placement& placement,
    const Dialog& dialog,
    const DialogFonts& fonts,
    const oa::ui::frontend_renderer::RgbaPicture& icon
);

/// Draws the dialog at its size class on a canvas of either form: its
/// display list (geometry::dialog_list), laid out in the canvas's game
/// fonts, painted with kit::paint. The crisp form draws what draw_dialog
/// draws; the smooth form draws the same parts at its own scale.
///
/// @param canvas where the dialog's top left corner lands, its fonts and its
///     icon; without game fonts nothing is drawn
/// @param dialog the dialog
void draw_dialog(const oa::ui::kit::Canvas& canvas, const Dialog& dialog);

namespace geometry {

/// Returns what the dialog draws, in the order it draws it, and its
/// controls, in the order a press tries them, each named for automation:
/// the window's face, the header, the section list, the open section's
/// heading and rows (the kit's rows, or Developer's rows, list and
/// footer), the footer band and buttons, the edge and an open drop-down
/// list. Tab follows the declared order: the open section's rows that take
/// a change (on Developer, then its list's rows, Show Active Only and,
/// while Developer Mode is on, Restore profile values), the footer's
/// buttons, then the sections' entries. A row's control lies across the
/// section at its control's line, so that the arrows keep to the column of
/// rows, and a press reaches the control itself where the view shows it.
/// The controls that take Left and Right as steps say so; the open
/// section's rows and Developer's list each have a scroll group, and the
/// entries and the footer none.
///
/// The controls are named for automation, which lists them after oa.
/// (docs/automation.md): settings.nav.<section>, settings.<row> and its
/// parts, settings.<row>.<item> while a drop-down is open,
/// settings.hack-area.<area>, settings.hack.<hack> and its parameters,
/// settings.active-only, settings.restore-profile-values,
/// settings.restore-defaults, settings.cancel, settings.ok and
/// settings.scroll-bar. Each word is one
/// of the code's, never a text as shown.
///
/// @param dialog the dialog
/// @param fonts the fonts it is drawn in; null measures texts at
///     estimated_character_width a character
/// @return the display list
[[nodiscard]] oa::ui::kit::DisplayList dialog_list(const Dialog& dialog, const DialogFonts* fonts);

} // namespace geometry

/// Draws the OA button: a small bevelled square showing the Open
/// Annihilation icon, scaled to the button's side less 3 source pixels all
/// round at the surface's own resolution. Under the pointer the button
/// lights and a green outline rings the icon; held, its bevel sinks and
/// the icon moves one source pixel right and down. Without the icon it
/// shows the OA mark, green letters in a green outlined square, lighter
/// under the pointer and held.
///
/// @param[in,out] target the surface
/// @param placement where the button's top left corner lands, and its scale
/// @param side the button's side, in source pixels (menu_button_side or ingame_button_side)
/// @param look how it looks
/// @param fonts the dialog's fonts
/// @param icon the Open Annihilation icon; an empty picture draws the OA mark
void draw_oa_button(
    oa::ui::frontend_renderer::Surface& target,
    const oa::ui::frontend_renderer::Placement& placement,
    int32_t side,
    ButtonLook look,
    const DialogFonts& fonts,
    const oa::ui::frontend_renderer::RgbaPicture& icon
);

/// Draws the Open Annihilation mark alone, for other screens that show it
/// as the dialog does: the icon scaled to fill a square at the surface's
/// own resolution, or without the icon the OA mark the OA button shows at
/// rest, green letters in a green outlined square 20/32 of the square's
/// side in its middle. Nothing else of the square is drawn.
///
/// @param[in,out] target the surface
/// @param placement where the square's top left corner lands, and its scale
/// @param side the square's side, in source pixels; 0 or less draws nothing
/// @param icon the Open Annihilation icon; an empty picture draws the OA mark
void draw_oa_mark(
    oa::ui::frontend_renderer::Surface& target,
    const oa::ui::frontend_renderer::Placement& placement,
    int32_t side,
    const oa::ui::frontend_renderer::RgbaPicture& icon
);

/// The sentence File Type Registration shows when macOS kept this copy as the
/// opener of each extension in `dotted` (each with its dot, in switch order).
/// One type names that type twice. Several name each type, then say to select
/// one of them. Empty when `dotted` is empty.
///
/// @param dotted the extensions that stayed, each with its dot
/// @return the sentence, in the language shown
[[nodiscard]] std::string kept_opener_hint(std::span<const std::string_view> dotted);

} // namespace oa::ui::engine_settings

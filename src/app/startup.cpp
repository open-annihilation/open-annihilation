// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Command-line parsing for oa-game.
#include "oa/app/app.hpp"
#include "oa/app/brain_match.hpp"
#include "oa/data/mutators/choice.hpp"
#include "oa/app/extension.hpp"
#include "oa/app/game_directory.hpp"
#include "net_options.hpp"
#include "oa/app/netgame/launch_file.hpp"
#include "oa/data/content_bundles/content_choice.hpp"
#include "oa/app/hook_call.hpp"
#include "oa/app/package_install.hpp"
#include "oa/app/package_install/inbox.hpp"
#include "oa/platform/display_modes.hpp"
#include "oa/platform/job_pool.hpp"
#include "oa/base/signing/ed25519.hpp"
#include "oa/formats/oamod/package_keys.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/platform/system.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace oa::app {
namespace {

/// The flag that names a level of hardware acceleration, before its value.
constexpr std::string_view kAccelerationLevelFlag = "--hardware-acceleration=";

constexpr std::size_t kMaximumRunFrames = 10'000'000;

/// The frames a --check-game-files run draws at the main menu when --frames is not given.
constexpr std::size_t kGameFilesCheckFrames = 30;

[[nodiscard]] uint32_t parse_seed(std::string_view text) {
    uint32_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size())
        throw std::runtime_error("--seed expects an integer from 0 through 4294967295");
    return value;
}

// The command line from a long option on: the arguments after it, as the
// engine's options and OptionValues read them.
struct ArgumentCursor {
    int argc{};
    char** argv{};
    int* index{};
    std::string_view name;
};

const char* next_value(void* arguments) {
    auto& cursor = *static_cast<ArgumentCursor*>(arguments);
    if (++*cursor.index >= cursor.argc || std::string_view(cursor.argv[*cursor.index]).empty())
        throw std::runtime_error(std::string(cursor.name) + " requires a value");
    return cursor.argv[*cursor.index];
}

/// Names the argument after the option without taking it (OptionValues::following).
///
/// @param arguments the ArgumentCursor
/// @return that argument, or null when the command line ends on the option
const char* following_value(void* arguments) {
    const auto& cursor = *static_cast<ArgumentCursor*>(arguments);
    const int following = *cursor.index + 1;
    if (following >= cursor.argc)
        return nullptr;
    return cursor.argv[following];
}

// The extension's text for `which`, or `fallback` when it has none.
const char* extension_text(const Extension& extension, ExtensionText which, const char* fallback) {
    const char* text = call_hook_or_raise<&Extension::text>(extension, which);
    return text != nullptr ? text : fallback;
}

// The extension's switch handler as the command line's parse calls it,
// which must not throw: each call goes through call_hook, and the first
// error a call catches is kept until the parse returns.
struct GuardedSwitches {
    const command_line::SwitchHandler* handler{}; // the extension's
    command_line::SwitchHandler guard{};          // what the parse is given
    HookError error;                              // the first error caught
};

/// Offers a switch letter to the extension's handler (SwitchHandler::take).
///
/// @param context the GuardedSwitches
/// @param letter the switch letter, in lower case
/// @param arguments what follows the letter
/// @param[out] effects switch effects of the handler
/// @return the handler's answer; 0 once a call has thrown
int take_guarded_switch(
    void* context, char letter, const command_line::SwitchArguments* arguments, uint32_t* effects
) noexcept {
    auto& switches = *static_cast<GuardedSwitches*>(context);
    if (switches.error.caught)
        return 0;
    return call_hook<&command_line::SwitchHandler::take>(
        *switches.handler, switches.error, letter, arguments, effects
    );
}

/// Resets the extension's handler (SwitchHandler::reset).
///
/// @param context the GuardedSwitches
void reset_guarded_switches(void* context) noexcept {
    auto& switches = *static_cast<GuardedSwitches*>(context);
    if (!switches.error.caught)
        call_hook<&command_line::SwitchHandler::reset>(*switches.handler, switches.error);
}

/// Returns the frames a second a --max-fps or --frame-rate value names.
///
/// Throws std::runtime_error with `expected` unless the whole text is a
/// decimal integer from `least` through kHighestFrameRate, or 0 when
/// `zero_allowed`.
///
/// @param text the option's value
/// @param least the lowest rate above 0 the option takes
/// @param zero_allowed the option takes 0 as well
/// @param expected the message that says what the option takes
/// @return the rate
[[nodiscard]] uint32_t
parse_frame_rate(std::string_view text, uint32_t least, bool zero_allowed, const char* expected) {
    uint32_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    const bool in_range =
        (value >= least && value <= kHighestFrameRate) || (zero_allowed && value == 0);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        !in_range)
        throw std::runtime_error(expected);
    return value;
}

/// The latest millisecond a --frame-rate run's clock may start at: about 31
/// years, which leaves the run's nanoseconds far inside 64 bits.
constexpr uint64_t kLatestFrameClockMs = 1'000'000'000'000;

/// Returns the millisecond a --frame-clock value names.
///
/// Throws std::runtime_error unless the whole text is a decimal integer from
/// 0 through kLatestFrameClockMs.
///
/// @param text the option's value
/// @return milliseconds into the steady clock
[[nodiscard]] uint64_t parse_frame_clock(std::string_view text) {
    uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        value > kLatestFrameClockMs)
        throw std::runtime_error("--frame-clock expects milliseconds from 0 through 1000000000000");
    return value;
}

/// Returns the byte count a --game-files-* option names.
///
/// Throws std::runtime_error naming `option` unless the whole text is a
/// decimal integer that fits 64 bits; `positive` refuses 0 too.
///
/// @param text the option's value
/// @param option the option, for the error
/// @param positive true when 0 is refused
/// @return the bytes
[[nodiscard]] uint64_t
parse_game_files_bytes(std::string_view text, std::string_view option, bool positive) {
    uint64_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        (positive && value == 0))
        throw std::runtime_error(
            std::string(option) + (positive ? " expects a whole number of bytes above 0"
                                            : " expects a whole number of bytes")
        );
    return value;
}

/// Returns the route a --game-files-route value names.
///
/// Throws std::runtime_error unless it is folder, demo, copy-yourself or manage.
///
/// @param text the option's value
/// @return the route
[[nodiscard]] GameFilesRoute parse_game_files_route(std::string_view text) {
    if (text == "folder")
        return GameFilesRoute::folder;
    if (text == "demo")
        return GameFilesRoute::demo;
    if (text == "copy-yourself")
        return GameFilesRoute::copy_yourself;
    if (text == "manage")
        return GameFilesRoute::manage;
    throw std::runtime_error("--game-files-route takes folder, demo, copy-yourself or manage");
}

/// Returns the expectation a --game-files-expect value names.
///
/// Throws std::runtime_error unless it is main-menu, stopped-kept, resumed,
/// not-a-game, short-space or next-start.
///
/// @param text the option's value
/// @return the expectation
[[nodiscard]] GameFilesExpect parse_game_files_expect(std::string_view text) {
    if (text == "main-menu")
        return GameFilesExpect::main_menu;
    if (text == "stopped-kept")
        return GameFilesExpect::stopped_kept;
    if (text == "resumed")
        return GameFilesExpect::resumed;
    if (text == "not-a-game")
        return GameFilesExpect::not_a_game;
    if (text == "short-space")
        return GameFilesExpect::short_space;
    if (text == "next-start")
        return GameFilesExpect::next_start;
    throw std::runtime_error(
        "--game-files-expect takes main-menu, stopped-kept, resumed, not-a-game, short-space or "
        "next-start"
    );
}

/// Checks the Game files screen's options: the --game-files-* companions need
/// --check-game-files, which refuses a named or chosen game folder, named
/// archives, the notice in place of the screen (--no-game-files-screen), a
/// headless run, the director's runs, a capture, a showcase, a benchmark and
/// the other checks, and runs unattended to the main menu.
/// --no-game-files-screen is the player's own switch and needs nothing.
///
/// Throws std::runtime_error naming the options that cannot be used together.
///
/// @param[in,out] options the parsed options; a --check-game-files run gets
///        unattended, and a frame limit when none was given
void check_game_files_options(Options& options) {
    if (!options.check_game_files) {
        const std::pair<bool, const char*> companions[] = {
            {options.game_files_route != GameFilesRoute::folder, "--game-files-route"},
            {options.game_files_expect != GameFilesExpect::main_menu, "--game-files-expect"},
            {!options.game_files_source.empty(), "--game-files-source"},
            {options.game_files_free_bytes.has_value(), "--game-files-free-bytes"},
            {options.game_files_copy_rate.has_value(), "--game-files-copy-rate"},
            {options.game_files_stop_after.has_value(), "--game-files-stop-after"},
        };
        for (const auto& [given, name] : companions)
            if (given)
                throw std::runtime_error(std::string(name) + " needs --check-game-files");
        return;
    }
    // The director's runs and checks are headless: they are named before
    // --headless-check, which they set.
    const std::pair<bool, const char*> refused[] = {
        {!options.game_dir.empty(), "--game-dir"},
        {options.choose_game_dir, "--choose-game-dir"},
        {!options.archives.empty(), "--archive"},
        {options.no_game_files_screen, "--no-game-files-screen"},
        {!options.generate_script.empty(), "--generate-script"},
        {!options.render_script.empty(), "--render-script"},
        {options.check_director_view, "--check-director-view"},
        {options.check_director_render, "--check-director-render"},
        {options.check_interpolation, "--check-interpolation"},
        {options.check_unit_playout, "--check-unit-playout"},
        {options.headless_check, "--headless-check"},
        {!options.capture_video.empty(), "--capture-video"},
        {options.showcase != Showcase::none, "--showcase"},
        {options.benchmark_frames.has_value(), "--benchmark"},
        {options.check_navigation, "--check-navigation"},
        {options.check_match_dialogs, "--check-match-dialogs"},
        {options.check_load_save, "--check-load-save"},
        {options.check_frontend_controls, "--check-frontend-controls"},
        {options.check_scroll_bars, "--check-scroll-bars"},
        {options.check_engine_settings, "--check-engine-settings"},
        {options.check_registered_kit, "--check-registered-kit"},
        {options.check_kit_gallery, "--check-kit-gallery"},
        {options.check_user_folder, "--check-user-folder"},
        {options.check_mod_switch, "--check-mod-switch"},
        {options.check_map_packs, "--check-map-packs"},
        {options.check_map_browser, "--check-map-browser"},
        {options.check_mutator_picker, "--check-mutator-picker"},
        {options.check_brain_picker, "--check-brain-picker"},
        {options.check_brain_seat_buttons, "--check-brain-seat-buttons"},
        {options.check_brain_campaign, "--check-brain-campaign"},
        {options.check_mutator_prompts, "--check-mutator-prompts"},
        {options.check_mutator_button, "--check-mutator-button"},
        {options.check_mutator_room, "--check-mutator-room"},
        {options.check_brain_room, "--check-brain-room"},
        {options.check_map_pack_lists, "--check-map-pack-lists"},
        {options.check_mod_map_settings, "--check-mod-map-settings"},
        {options.check_mod_warning, "--check-mod-warning"},
        {options.check_mod_install, "--check-mod-install"},
        {options.check_language_install, "--check-language-install"},
        {options.check_challenge, "--check-challenge"},
        {options.check_registries, "--check-registries"},
        {options.check_library, "--check-library"},
        {options.check_library_install, "--check-library-install"},
        {options.check_library_map_previews, "--check-library-map-previews"},
        {options.check_library_content, "--check-library-content"},
        {options.check_library_update_question, "--check-library-update-question"},
        {!options.open_files.empty(), "--open"},
        {options.check_renderer_ladder, "--check-renderer-ladder"},
        {options.check_renderer_proof, "--check-renderer-proof"},
        {options.check_live_renderer, "--check-live-renderer"},
        {options.check_briefing_narration, "--check-briefing-narration"},
        {options.check_match_layers, "--check-match-layers"},
        {options.check_render_tiers, "--check-render-tiers"},
        {options.check_match_orders, "--check-match-orders"},
        {options.check_side_orders, "--check-side-orders"},
        {options.check_skirmish_sides, "--check-skirmish-sides"},
        {options.check_mod_limits, "--check-mod-limits"},
        {options.check_factory_orders, "--check-factory-orders"},
        {options.check_unit_speech, "--check-unit-speech"},
        {options.check_mutator_layer, "--check-mutator-layer"},
        {options.check_mutator_verdicts, "--check-mutator-verdicts"},
        {options.check_mutator_dry_run, "--check-mutator-dry-run"},
        {options.check_replay_loadout, "--check-replay-loadout"},
        {options.check_side_data_per_match, "--check-side-data-per-match"},
        {options.check_gadget_art_per_match, "--check-gadget-art-per-match"},
        {options.check_download_builds, "--check-download-builds"},
        {options.check_stockpile_builds, "--check-stockpile-builds"},
        {options.check_unit_page_memory, "--check-unit-page-memory"},
        {options.check_side_column, "--check-side-column"},
        {options.check_match_bars, "--check-match-bars"},
        {!options.check_unit_pages.empty(), "--check-unit-pages"},
        {!options.check_key_table.empty(), "--check-key-table"},
        {options.check_key_latches, "--check-key-latches"},
        {options.check_hold_confirm, "--check-hold-confirm"},
        {options.check_key_timing, "--check-key-timing"},
        {options.check_pointer_keys, "--check-pointer-keys"},
        {options.check_kill_board, "--check-kill-board"},
        {options.check_paused_save, "--check-paused-save"},
        {options.check_saved_brains, "--check-saved-brains"},
        {options.check_simulation_hash, "--check-simulation-hash"},
        {options.check_zoom_memory, "--check-zoom-memory"},
        {!options.check_unit_language.empty(), "--check-unit-language"},
        {options.check_language_switch, "--check-language-switch"},
        {options.check_language_registry, "--check-language-registry"},
        {options.check_language_notice, "--check-language-notice"},
        {options.check_first_run, "--check-first-run"},
        {options.check_setup_window, "--check-setup-window"},
        {options.check_window_place, "--check-window-place"},
        {options.check_progress, "--check-progress"},
        {options.check_icloud_wait, "--check-icloud-wait"},
        {options.check_text_fit, "--check-text-fit"},
        {options.check_folder_chooser, "--check-folder-chooser"},
        {options.check_patrol_reclaim, "--check-patrol-reclaim"},
        {options.check_reclaim_cursor, "--check-reclaim-cursor"},
        {options.check_wreck_pick, "--check-wreck-pick"},
        {options.check_click_snap, "--check-click-snap"},
        {options.check_build_preview, "--check-build-preview"},
        {options.check_pointer_interfaces, "--check-pointer-interfaces"},
        {options.check_megamap_clicks, "--check-megamap-clicks"},
        {options.check_single_player_offers, "--check-single-player-offers"},
        {options.check_radar_orders, "--check-radar-orders"},
        {options.check_touch_controls, "--check-touch-controls"},
        {options.check_pad_controls, "--check-pad-controls"},
        {options.check_running_while_inactive, "--check-running-while-inactive"},
        {options.check_multiplayer_menu, "--check-multiplayer-menu"},
    };
    for (const auto& [given, name] : refused)
        if (given)
            throw std::runtime_error(std::string("--check-game-files cannot be used with ") + name);
    // The route ends at the main menu, which the run leaves after a few frames.
    if (!options.frame_limit)
        options.frame_limit = kGameFilesCheckFrames;
    options.unattended = true;
}

/// Returns the drawing threads a --draw-threads or OA_DRAW_THREADS value names.
///
/// Throws std::runtime_error naming `source` unless the whole text is a
/// decimal integer from 1 through job_pool::max_threads.
///
/// @param text the value
/// @param source the option or variable the value came from
/// @return the threads, the drawing thread included
[[nodiscard]] uint32_t parse_draw_threads(std::string_view text, std::string_view source) {
    uint32_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        value < 1 || value > oa::platform::job_pool::max_threads)
        throw std::runtime_error(
            std::string(source) + " expects threads from 1 through " +
            std::to_string(oa::platform::job_pool::max_threads)
        );
    return value;
}

/// Returns the navigation check's group a word after --check-navigation
/// names, or nothing when it names none.
///
/// @param text the word after --check-navigation
/// @return the group, or nothing
[[nodiscard]] std::optional<NavigationGroup> navigation_group_named(std::string_view text) {
    static constexpr std::array<std::pair<std::string_view, NavigationGroup>, 8> kGroups{{
        {"screens", NavigationGroup::screens},
        {"orders", NavigationGroup::orders},
        {"outcomes", NavigationGroup::outcomes},
        {"zoom", NavigationGroup::zoom},
        {"zoom-1366x768", NavigationGroup::zoom_1366x768},
        {"zoom-1920x1080", NavigationGroup::zoom_1920x1080},
        {"zoom-2560x1440", NavigationGroup::zoom_2560x1440},
        {"campaign", NavigationGroup::campaign},
    }};
    for (const auto& [name, group] : kGroups)
        if (text == name)
            return group;
    return std::nullopt;
}

/// Returns the showcase a --showcase value names.
///
/// Throws std::runtime_error naming the showcases for any other value.
///
/// @param text the option's value
/// @return the showcase
[[nodiscard]] Showcase parse_showcase(std::string_view text) {
    if (text == "arm-first-mission")
        return Showcase::arm_first_mission;
    if (text == "skirmish-battle")
        return Showcase::skirmish_battle;
    if (text == "base-construction")
        return Showcase::base_construction;
    throw std::runtime_error(
        "--showcase knows arm-first-mission, skirmish-battle and base-construction"
    );
}

/// Returns the chunks a --chunks value names: A-B, both included, or A alone.
///
/// Throws std::runtime_error for any other value.
///
/// @param text the option's value
/// @return the first and last chunk, counted from 0
[[nodiscard]] std::pair<uint32_t, uint32_t> parse_chunks(std::string_view text) {
    const auto number = [&](std::string_view part) {
        uint32_t value = 0;
        const auto result = std::from_chars(part.data(), part.data() + part.size(), value);
        if (part.empty() || result.ec != std::errc{} || result.ptr != part.data() + part.size())
            throw std::runtime_error(
                "--chunks expects A-B or A: chunk numbers from 0, the first not after the last"
            );
        return value;
    };
    const auto separator = text.find('-');
    const uint32_t first = number(text.substr(0, separator));
    const uint32_t last =
        separator == std::string_view::npos ? first : number(text.substr(separator + 1));
    if (last < first)
        throw std::runtime_error(
            "--chunks expects A-B or A: chunk numbers from 0, the first not after the last"
        );
    return {first, last};
}

/// Returns the frames a --stills value names: frame numbers from 0, in
/// increasing order, separated by commas.
///
/// Throws std::runtime_error for any other value.
///
/// @param text the option's value
/// @return the frames
[[nodiscard]] std::vector<uint64_t> parse_stills(std::string_view text) {
    std::vector<uint64_t> frames;
    std::size_t start = 0;
    while (true) {
        const auto comma = text.find(',', start);
        const auto part =
            text.substr(start, comma == std::string_view::npos ? comma : comma - start);
        uint64_t frame = 0;
        const auto result = std::from_chars(part.data(), part.data() + part.size(), frame);
        if (part.empty() || result.ec != std::errc{} || result.ptr != part.data() + part.size() ||
            (!frames.empty() && frame <= frames.back()))
            throw std::runtime_error(
                "--stills expects frame numbers from 0, in increasing order, separated by commas"
            );
        frames.push_back(frame);
        if (comma == std::string_view::npos)
            return frames;
        start = comma + 1;
    }
}

/// Takes an --effect value, NAME=PERCENT: an effect's name
/// (engine_settings::EffectSlider) and a whole percent from 0 to 200 in steps
/// of 10, as the effect's slider sets it.
///
/// Throws std::runtime_error for any other value, and for an effect named
/// twice.
///
/// @param text the option's value
/// @param[in,out] effects the percents taken so far, in effect_sliders' order
void take_effect(
    std::string_view text,
    std::array<std::optional<uint32_t>, oa::ui::engine_settings::effect_sliders.size()>& effects
) {
    namespace settings = oa::ui::engine_settings;
    constexpr uint32_t most_percent = 200;
    constexpr uint32_t percent_step = 10;
    const auto refuse = [] {
        std::string names;
        for (const auto& slider : settings::effect_sliders)
            names += (names.empty() ? "" : ", ") + std::string(slider.name);
        throw std::runtime_error(
            "--effect expects NAME=PERCENT: NAME one of " + names +
            ", PERCENT from 0 to 200 in steps of 10"
        );
    };
    const auto equals = text.find('=');
    if (equals == std::string_view::npos)
        refuse();
    const auto name = text.substr(0, equals);
    const auto digits = text.substr(equals + 1);
    uint32_t percent = 0;
    const auto result = std::from_chars(digits.data(), digits.data() + digits.size(), percent);
    if (digits.empty() || result.ec != std::errc{} || result.ptr != digits.data() + digits.size() ||
        percent > most_percent || percent % percent_step != 0)
        refuse();
    for (std::size_t index = 0; index < settings::effect_sliders.size(); ++index) {
        if (settings::effect_sliders[index].name != name)
            continue;
        if (effects[index])
            throw std::runtime_error("--effect names " + std::string(name) + " twice");
        effects[index] = percent;
        return;
    }
    refuse();
}

/// Checks the director's options against the others, and has a director
/// run headless: --generate-script and --render-script run on their own,
/// on the fixed clock and seed, so that a script's analysis and its render
/// replay the recording alike.
///
/// Throws std::runtime_error naming the options that cannot be used
/// together.
///
/// @param[in,out] options the parsed options; a director run gets
///        headless_check and skip_intro
void check_director_options(Options& options) {
    const bool generate = !options.generate_script.empty();
    const bool render = !options.render_script.empty();
    const bool effects = std::any_of(
        options.director_effects.begin(),
        options.director_effects.end(),
        [](const std::optional<uint32_t>& percent) { return percent.has_value(); }
    );
    if (!generate && !render) {
        if (!options.director_output.empty())
            throw std::runtime_error("--output needs --generate-script or --render-script");
        if (options.director_chunks)
            throw std::runtime_error("--chunks needs --render-script");
        if (!options.director_stills.empty())
            throw std::runtime_error("--stills needs --render-script");
        if (effects)
            throw std::runtime_error("--effect needs --render-script");
        return;
    }
    if (generate && render)
        throw std::runtime_error("--generate-script and --render-script cannot be used together");
    if (generate && options.director_chunks)
        throw std::runtime_error("--chunks needs --render-script");
    if (generate && !options.director_stills.empty())
        throw std::runtime_error("--stills needs --render-script");
    if (generate && effects)
        throw std::runtime_error("--effect needs --render-script");
    if (render && options.window_resolution)
        throw std::runtime_error(
            "--render-script takes the frame size from the script, not from --resolution"
        );
    // A render's frames are the processor's, or the graphics card's Full
    // tier's; Basic presents the processor's frames and draws none itself.
    if (render &&
        options.hardware_acceleration == oa::ui::engine_settings::HardwareAcceleration::basic)
        throw std::runtime_error(
            "--render-script draws its frames on the processor, or on the graphics card with "
            "--hardware-acceleration=full; Basic draws no frames of its own"
        );
    const char* run = generate ? "--generate-script" : "--render-script";
    // Options of other runs, and those that would change the match, its
    // seed or its camera.
    const std::pair<bool, const char*> refused[] = {
        {options.seed.has_value(), "--seed"},
        {!options.capture_video.empty(), "--capture-video"},
        {options.showcase != Showcase::none, "--showcase"},
        {options.benchmark_frames.has_value(), "--benchmark"},
        {options.frame_limit.has_value(), "--frames"},
        {!options.snapshot.empty(), "--snapshot"},
        {options.match_ticks.has_value(), "--match-ticks"},
        {options.frame_rate.has_value(), "--frame-rate"},
        {!options.campaign.empty() || options.campaign_mission.has_value(), "--campaign"},
        {!options.load_file.empty(), "--load"},
        {options.save_after.has_value(), "--save-after"},
        {!options.save_file.empty(), "--save-file"},
        {options.camera.has_value(), "--camera"},
        {options.match_zoom != kDefaultBattlefieldZoom, "--zoom"},
        {options.combat_units != 0, "--combat"},
        {options.busy_combat, "--busy-combat"},
        {!options.stage_file.empty(), "--stage"},
        {options.reclaim_check, "--reclaim-check"},
        {!options.weapon_fire_check.empty(), "--weapon-fire-check"},
        {options.give_orders, "--give-orders"},
        {options.check_navigation, "--check-navigation"},
        {options.check_match_dialogs, "--check-match-dialogs"},
        {options.check_load_save, "--check-load-save"},
        {options.check_frontend_controls, "--check-frontend-controls"},
        {options.check_scroll_bars, "--check-scroll-bars"},
        {options.check_briefing_narration, "--check-briefing-narration"},
        {options.check_match_layers, "--check-match-layers"},
        {options.check_render_tiers, "--check-render-tiers"},
        {options.check_match_orders, "--check-match-orders"},
        {options.check_side_orders, "--check-side-orders"},
        {options.check_skirmish_sides, "--check-skirmish-sides"},
        {options.check_mod_limits, "--check-mod-limits"},
        {options.check_factory_orders, "--check-factory-orders"},
        {options.check_unit_speech, "--check-unit-speech"},
        {options.check_mutator_layer, "--check-mutator-layer"},
        {options.check_mutator_verdicts, "--check-mutator-verdicts"},
        {options.check_mutator_dry_run, "--check-mutator-dry-run"},
        {options.check_replay_loadout, "--check-replay-loadout"},
        {options.check_side_data_per_match, "--check-side-data-per-match"},
        {options.check_gadget_art_per_match, "--check-gadget-art-per-match"},
        {options.check_download_builds, "--check-download-builds"},
        {options.check_stockpile_builds, "--check-stockpile-builds"},
        {options.check_unit_page_memory, "--check-unit-page-memory"},
        {options.check_side_column, "--check-side-column"},
        {options.check_match_bars, "--check-match-bars"},
        {!options.check_unit_pages.empty(), "--check-unit-pages"},
        {!options.check_key_table.empty(), "--check-key-table"},
        {options.check_key_latches, "--check-key-latches"},
        {options.check_hold_confirm, "--check-hold-confirm"},
        {options.check_key_timing, "--check-key-timing"},
        {options.check_pointer_keys, "--check-pointer-keys"},
        {options.check_kill_board, "--check-kill-board"},
        {options.check_paused_save, "--check-paused-save"},
        {options.check_saved_brains, "--check-saved-brains"},
        {options.check_simulation_hash, "--check-simulation-hash"},
        {options.check_zoom_memory, "--check-zoom-memory"},
        {!options.check_unit_language.empty(), "--check-unit-language"},
        {options.check_language_switch, "--check-language-switch"},
        {options.check_language_registry, "--check-language-registry"},
        {options.check_language_notice, "--check-language-notice"},
        {options.check_first_run, "--check-first-run"},
        {options.check_setup_window, "--check-setup-window"},
        {options.check_window_place, "--check-window-place"},
        {options.check_progress, "--check-progress"},
        {options.check_icloud_wait, "--check-icloud-wait"},
        {options.check_text_fit, "--check-text-fit"},
        {options.check_folder_chooser, "--check-folder-chooser"},
        {options.check_patrol_reclaim, "--check-patrol-reclaim"},
        {options.check_reclaim_cursor, "--check-reclaim-cursor"},
        {options.check_wreck_pick, "--check-wreck-pick"},
        {options.check_click_snap, "--check-click-snap"},
        {options.check_build_preview, "--check-build-preview"},
        {options.check_pointer_interfaces, "--check-pointer-interfaces"},
        {options.check_megamap_clicks, "--check-megamap-clicks"},
        {options.check_single_player_offers, "--check-single-player-offers"},
        {options.check_radar_orders, "--check-radar-orders"},
        {options.check_touch_controls, "--check-touch-controls"},
        {options.check_pad_controls, "--check-pad-controls"},
        {options.check_running_while_inactive, "--check-running-while-inactive"},
        {options.check_multiplayer_menu, "--check-multiplayer-menu"},
        {options.check_director_view, "--check-director-view"},
        {options.check_director_render, "--check-director-render"},
        {options.check_interpolation, "--check-interpolation"},
        {options.check_unit_playout, "--check-unit-playout"},
        {options.check_engine_settings, "--check-engine-settings"},
        {options.check_registered_kit, "--check-registered-kit"},
        {options.check_kit_gallery, "--check-kit-gallery"},
        {options.check_user_folder, "--check-user-folder"},
        {options.check_mod_switch, "--check-mod-switch"},
        {options.check_map_packs, "--check-map-packs"},
        {options.check_map_browser, "--check-map-browser"},
        {options.check_mutator_picker, "--check-mutator-picker"},
        {options.check_brain_picker, "--check-brain-picker"},
        {options.check_brain_seat_buttons, "--check-brain-seat-buttons"},
        {options.check_brain_campaign, "--check-brain-campaign"},
        {options.check_mutator_prompts, "--check-mutator-prompts"},
        {options.check_mutator_button, "--check-mutator-button"},
        {options.check_mutator_room, "--check-mutator-room"},
        {options.check_brain_room, "--check-brain-room"},
        {options.check_map_pack_lists, "--check-map-pack-lists"},
        {options.check_mod_map_settings, "--check-mod-map-settings"},
        {options.check_mod_warning, "--check-mod-warning"},
        {options.check_mod_install, "--check-mod-install"},
        {options.check_language_install, "--check-language-install"},
        {options.check_challenge, "--check-challenge"},
        {options.check_registries, "--check-registries"},
        {options.check_library, "--check-library"},
        {options.check_library_install, "--check-library-install"},
        {options.check_library_map_previews, "--check-library-map-previews"},
        {options.check_library_content, "--check-library-content"},
        {options.check_library_update_question, "--check-library-update-question"},
        {!options.open_files.empty(), "--open"},
        {options.check_renderer_ladder, "--check-renderer-ladder"},
        {options.check_renderer_proof, "--check-renderer-proof"},
        {options.check_live_renderer, "--check-live-renderer"},
    };
    for (const auto& [given, name] : refused)
        if (given)
            throw std::runtime_error(std::string(run) + " cannot be used with " + name);
    options.headless_check = true;
    options.skip_intro = true;
}

/// Refuses the self-checks, a --check-* option, --reclaim-check or
/// --weapon-fire-check, in a build that leaves them out (self_checks_built).
///
/// Throws std::runtime_error naming the first one the options ask for.
///
/// @param options the parsed options
void refuse_left_out_self_checks([[maybe_unused]] const Options& options) {
    if constexpr (!self_checks_built) {
        const std::pair<bool, const char*> checks[] = {
            {options.reclaim_check, "--reclaim-check"},
            {!options.weapon_fire_check.empty(), "--weapon-fire-check"},
            {options.check_navigation, "--check-navigation"},
            {options.check_match_dialogs, "--check-match-dialogs"},
            {options.check_load_save, "--check-load-save"},
            {options.check_frontend_controls, "--check-frontend-controls"},
            {options.check_scroll_bars, "--check-scroll-bars"},
            {options.check_briefing_narration, "--check-briefing-narration"},
            {options.check_match_layers, "--check-match-layers"},
            {options.check_render_tiers, "--check-render-tiers"},
            {options.check_match_orders, "--check-match-orders"},
            {options.check_side_orders, "--check-side-orders"},
            {options.check_skirmish_sides, "--check-skirmish-sides"},
            {options.check_mod_limits, "--check-mod-limits"},
            {options.check_factory_orders, "--check-factory-orders"},
            {options.check_unit_speech, "--check-unit-speech"},
            {options.check_mutator_layer, "--check-mutator-layer"},
            {options.check_mutator_verdicts, "--check-mutator-verdicts"},
            {options.check_mutator_dry_run, "--check-mutator-dry-run"},
            {options.check_replay_loadout, "--check-replay-loadout"},
            {options.check_side_data_per_match, "--check-side-data-per-match"},
            {options.check_gadget_art_per_match, "--check-gadget-art-per-match"},
            {options.check_download_builds, "--check-download-builds"},
            {options.check_stockpile_builds, "--check-stockpile-builds"},
            {options.check_unit_page_memory, "--check-unit-page-memory"},
            {options.check_side_column, "--check-side-column"},
            {options.check_match_bars, "--check-match-bars"},
            {!options.check_unit_pages.empty(), "--check-unit-pages"},
            {!options.check_key_table.empty(), "--check-key-table"},
            {options.check_key_latches, "--check-key-latches"},
            {options.check_hold_confirm, "--check-hold-confirm"},
            {options.check_key_timing, "--check-key-timing"},
            {options.check_pointer_keys, "--check-pointer-keys"},
            {options.check_kill_board, "--check-kill-board"},
            {options.check_paused_save, "--check-paused-save"},
            {options.check_saved_brains, "--check-saved-brains"},
            {options.check_simulation_hash, "--check-simulation-hash"},
            {options.check_zoom_memory, "--check-zoom-memory"},
            {!options.check_unit_language.empty(), "--check-unit-language"},
            {options.check_language_switch, "--check-language-switch"},
            {options.check_language_registry, "--check-language-registry"},
            {options.check_language_notice, "--check-language-notice"},
            {options.check_first_run, "--check-first-run"},
            {options.check_setup_window, "--check-setup-window"},
            {options.check_window_place, "--check-window-place"},
            {options.check_progress, "--check-progress"},
            {options.update_feed.has_value(), "--update-feed"},
            {!options.update_keys.empty(), "--update-key"},
            {options.check_engine_feed, "--check-engine-feed"},
            {options.check_update_offer, "--check-update-offer"},
            {options.check_update_download, "--check-update-download"},
            {options.update_install_folder.has_value(), "--update-install-folder"},
            {options.update_and_quit, "--update-and-quit"},
            {!options.update_download_case.empty(), "--update-download-case"},
            {options.check_icloud_wait, "--check-icloud-wait"},
            {options.check_text_fit, "--check-text-fit"},
            {!options.check_languages.empty(), "--check-language"},
            {options.check_folder_chooser, "--check-folder-chooser"},
            {options.check_patrol_reclaim, "--check-patrol-reclaim"},
            {options.check_reclaim_cursor, "--check-reclaim-cursor"},
            {options.check_wreck_pick, "--check-wreck-pick"},
            {options.check_click_snap, "--check-click-snap"},
            {options.check_build_preview, "--check-build-preview"},
            {options.check_pointer_interfaces, "--check-pointer-interfaces"},
            {options.check_megamap_clicks, "--check-megamap-clicks"},
            {options.check_single_player_offers, "--check-single-player-offers"},
            {options.check_radar_orders, "--check-radar-orders"},
            {options.check_touch_controls, "--check-touch-controls"},
            {options.check_pad_controls, "--check-pad-controls"},
            {options.check_running_while_inactive, "--check-running-while-inactive"},
            {options.check_game_files, "--check-game-files"},
            {!options.check_content_copy.empty(), "--check-content-copy"},
            {!options.check_installer.empty(), "--check-installer"},
            {options.check_multiplayer_menu, "--check-multiplayer-menu"},
            {options.check_director_view, "--check-director-view"},
            {options.check_director_render, "--check-director-render"},
            {options.check_interpolation, "--check-interpolation"},
            {options.check_unit_playout, "--check-unit-playout"},
            {options.check_engine_settings, "--check-engine-settings"},
            {options.check_registered_kit, "--check-registered-kit"},
            {options.check_kit_gallery, "--check-kit-gallery"},
            {options.check_user_folder, "--check-user-folder"},
            {options.check_mod_switch, "--check-mod-switch"},
            {options.check_map_packs, "--check-map-packs"},
            {options.check_map_browser, "--check-map-browser"},
            {options.check_mutator_picker, "--check-mutator-picker"},
            {options.check_brain_picker, "--check-brain-picker"},
            {options.check_brain_seat_buttons, "--check-brain-seat-buttons"},
            {options.check_brain_campaign, "--check-brain-campaign"},
            {options.check_mutator_prompts, "--check-mutator-prompts"},
            {options.check_mutator_button, "--check-mutator-button"},
            {options.check_mutator_room, "--check-mutator-room"},
            {options.check_brain_room, "--check-brain-room"},
            {options.check_map_pack_lists, "--check-map-pack-lists"},
            {options.check_mod_map_settings, "--check-mod-map-settings"},
            {options.check_mod_warning, "--check-mod-warning"},
            {options.check_mod_install, "--check-mod-install"},
            {options.check_language_install, "--check-language-install"},
            {options.check_challenge, "--check-challenge"},
            {options.check_registries, "--check-registries"},
            {options.check_library, "--check-library"},
            {options.check_library_install, "--check-library-install"},
            {options.check_library_map_previews, "--check-library-map-previews"},
            {options.check_library_content, "--check-library-content"},
            {options.check_library_update_question, "--check-library-update-question"},
            {options.check_renderer_ladder, "--check-renderer-ladder"},
            {options.check_renderer_proof, "--check-renderer-proof"},
            {options.check_live_renderer, "--check-live-renderer"},
        };
        for (const auto& [given, name] : checks)
            if (given)
                throw std::runtime_error(
                    std::string(name) +
                    " is a self-check, and this build leaves the self-checks out; Check and "
                    "Debug builds hold them"
                );
    }
}

} // namespace

[[nodiscard]] std::size_t parse_count(std::string_view text) {
    std::size_t value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || result.ec != std::errc{} || result.ptr != text.data() + text.size() ||
        value > kMaximumRunFrames)
        throw std::runtime_error("frame counts must be integers from 0 through 10,000,000");
    return value;
}

oa::ui::engine_settings::HardwareAcceleration hardware_acceleration_asked(
    const Options& options, oa::ui::engine_settings::HardwareAcceleration setting
) noexcept {
    return options.hardware_acceleration.value_or(setting);
}

namespace {

/// Reads --render-fault's value: a failure's name, optionally followed by
/// @FRAME, a presented frame from 1 through 10,000,000.
///
/// Throws std::runtime_error naming the names it takes when the name is
/// unknown or the frame is not a count from 1, and when create, which acts
/// at start-up, is given a frame.
///
/// @param text the option's value
/// @return the failure and its frame
[[nodiscard]] RenderFault parse_render_fault(std::string_view text) {
    static constexpr std::pair<std::string_view, RenderFaultPoint> points[] = {
        {"create", RenderFaultPoint::create},
        {"present", RenderFaultPoint::present},
        {"reset", RenderFaultPoint::reset},
        {"lost", RenderFaultPoint::lost},
        {"stall", RenderFaultPoint::stall},
        {"float", RenderFaultPoint::float_state},
        {"memory", RenderFaultPoint::memory},
        {"card", RenderFaultPoint::card},
        {"full-memory", RenderFaultPoint::full_memory},
        {"effects", RenderFaultPoint::effects},
    };
    const char* const usage = "--render-fault takes create, present, reset, lost, stall, float, "
                              "memory, card, full-memory or effects, optionally @FRAME";
    const auto at = text.find('@');
    const auto name = text.substr(0, at);
    const auto named = std::find_if(std::begin(points), std::end(points), [&](const auto& point) {
        return point.first == name;
    });
    if (named == std::end(points))
        throw std::runtime_error(usage);
    RenderFault fault;
    fault.point = named->second;
    if (at == std::string_view::npos)
        return fault;
    if (fault.point == RenderFaultPoint::create)
        throw std::runtime_error("--render-fault create acts at start-up and takes no frame");
    std::size_t frame = 0;
    try {
        frame = parse_count(text.substr(at + 1));
    } catch (const std::runtime_error&) {
        throw std::runtime_error(usage);
    }
    if (frame == 0)
        throw std::runtime_error(usage);
    fault.frame = static_cast<uint32_t>(frame);
    return fault;
}

} // namespace

namespace {

/// Returns a file to open as the command line names it, made absolute.
///
/// @param text the path, UTF-8
/// @return the absolute path
[[nodiscard]] fs::path opened_file_path(std::string_view text) {
    const fs::path file = path_from_utf8(text);
    std::error_code error;
    const fs::path whole = fs::absolute(file, error);
    return error ? file : whole;
}

/// Tells whether a bare argument names a file to open, by its extension, any case.
///
/// @param argument the argument
/// @return true for a .oamod, .oalang, .oamap, .oamut, .oabrain or .oareg file
[[nodiscard]] bool names_opened_file(std::string_view argument) {
    return package_install::opens_file(path_from_utf8(argument));
}

/// An option that takes no value and turns on one member of Options.
struct FlagOption {
    std::string_view name;
    bool Options::* member{};
};

/// The options that take no value and do nothing but turn on their member.
/// The options that take a value or do more are read by the functions below.
constexpr FlagOption kFlagOptions[] = {
    {"--choose-game-dir", &Options::choose_game_dir},
    {"--base-game", &Options::base_game},
    {"--no-mutators", &Options::no_mutators},
    {"--print-profile", &Options::print_profile},
    {"--accept-unimplemented-hacks", &Options::accept_unimplemented_hacks},
    {"--force-capable", &Options::force_capable},
    {"--native-density", &Options::native_density},
    {"--scroll-camera", &Options::scroll_camera},
    {"--march", &Options::march},
    {"--follow", &Options::follow},
    {"--past-outcome", &Options::campaign_past_outcome},
    {"--busy-combat", &Options::busy_combat},
    {"--give-orders", &Options::give_orders},
    {"--reclaim-check", &Options::reclaim_check},
    {"--skip-intro", &Options::skip_intro},
    {"--first-run", &Options::first_run},
    {"--headless-check", &Options::headless_check},
    {"--mute", &Options::mute},
    {"--check-match-dialogs", &Options::check_match_dialogs},
    {"--check-load-save", &Options::check_load_save},
    {"--check-frontend-controls", &Options::check_frontend_controls},
    {"--check-scroll-bars", &Options::check_scroll_bars},
    {"--check-engine-settings", &Options::check_engine_settings},
    {"--check-registered-kit", &Options::check_registered_kit},
    {"--check-kit-gallery", &Options::check_kit_gallery},
    {"--check-user-folder", &Options::check_user_folder},
    {"--check-mod-switch", &Options::check_mod_switch},
    {"--check-map-packs", &Options::check_map_packs},
    {"--check-map-browser", &Options::check_map_browser},
    {"--check-mutator-button", &Options::check_mutator_button},
    {"--check-mutator-room", &Options::check_mutator_room},
    {"--check-brain-room", &Options::check_brain_room},
    {"--check-mutator-picker", &Options::check_mutator_picker},
    {"--check-brain-picker", &Options::check_brain_picker},
    {"--check-brain-seat-buttons", &Options::check_brain_seat_buttons},
    {"--check-brain-campaign", &Options::check_brain_campaign},
    {"--check-mutator-prompts", &Options::check_mutator_prompts},
    {"--check-map-pack-lists", &Options::check_map_pack_lists},
    {"--check-mod-map-settings", &Options::check_mod_map_settings},
    {"--check-mod-warning", &Options::check_mod_warning},
    {"--check-mod-install", &Options::check_mod_install},
    {"--check-language-install", &Options::check_language_install},
    {"--check-challenge", &Options::check_challenge},
    {"--check-registries", &Options::check_registries},
    {"--check-library", &Options::check_library},
    {"--check-library-install", &Options::check_library_install},
    {"--check-library-map-previews", &Options::check_library_map_previews},
    {"--check-library-content", &Options::check_library_content},
    {"--check-library-update-question", &Options::check_library_update_question},
    {"--check-renderer-ladder", &Options::check_renderer_ladder},
    {"--check-renderer-proof", &Options::check_renderer_proof},
    {"--check-live-renderer", &Options::check_live_renderer},
    {"--experimental-3d", &Options::experimental_3d},
    {"--check-briefing-narration", &Options::check_briefing_narration},
    {"--check-match-layers", &Options::check_match_layers},
    {"--check-render-tiers", &Options::check_render_tiers},
    {"--check-match-orders", &Options::check_match_orders},
    {"--check-side-orders", &Options::check_side_orders},
    {"--check-skirmish-sides", &Options::check_skirmish_sides},
    {"--check-mod-limits", &Options::check_mod_limits},
    {"--check-factory-orders", &Options::check_factory_orders},
    {"--check-unit-speech", &Options::check_unit_speech},
    {"--check-mutator-layer", &Options::check_mutator_layer},
    {"--check-mutator-verdicts", &Options::check_mutator_verdicts},
    {"--check-mutator-dry-run", &Options::check_mutator_dry_run},
    {"--check-replay-loadout", &Options::check_replay_loadout},
    {"--check-side-data-per-match", &Options::check_side_data_per_match},
    {"--check-gadget-art-per-match", &Options::check_gadget_art_per_match},
    {"--check-download-builds", &Options::check_download_builds},
    {"--check-stockpile-builds", &Options::check_stockpile_builds},
    {"--check-unit-page-memory", &Options::check_unit_page_memory},
    {"--check-side-column", &Options::check_side_column},
    {"--check-match-bars", &Options::check_match_bars},
    {"--check-key-latches", &Options::check_key_latches},
    {"--check-hold-confirm", &Options::check_hold_confirm},
    {"--check-kill-board", &Options::check_kill_board},
    {"--check-paused-save", &Options::check_paused_save},
    {"--check-saved-brains", &Options::check_saved_brains},
    {"--check-simulation-hash", &Options::check_simulation_hash},
    {"--check-zoom-memory", &Options::check_zoom_memory},
    {"--check-language-switch", &Options::check_language_switch},
    {"--check-language-registry", &Options::check_language_registry},
    {"--check-language-notice", &Options::check_language_notice},
    {"--check-first-run", &Options::check_first_run},
    {"--check-setup-window", &Options::check_setup_window},
    {"--check-window-place", &Options::check_window_place},
    {"--check-progress", &Options::check_progress},
    {"--check-engine-feed", &Options::check_engine_feed},
    {"--check-update-offer", &Options::check_update_offer},
    {"--check-update-download", &Options::check_update_download},
    {"--update-and-quit", &Options::update_and_quit},
    {"--check-icloud-wait", &Options::check_icloud_wait},
    {"--check-text-fit", &Options::check_text_fit},
    {"--check-folder-chooser", &Options::check_folder_chooser},
    {"--check-patrol-reclaim", &Options::check_patrol_reclaim},
    {"--check-reclaim-cursor", &Options::check_reclaim_cursor},
    {"--check-wreck-pick", &Options::check_wreck_pick},
    {"--check-click-snap", &Options::check_click_snap},
    {"--check-build-preview", &Options::check_build_preview},
    {"--check-pointer-interfaces", &Options::check_pointer_interfaces},
    {"--check-key-timing", &Options::check_key_timing},
    {"--check-pointer-keys", &Options::check_pointer_keys},
    {"--check-megamap-clicks", &Options::check_megamap_clicks},
    {"--check-single-player-offers", &Options::check_single_player_offers},
    {"--check-radar-orders", &Options::check_radar_orders},
    {"--check-touch-controls", &Options::check_touch_controls},
    {"--check-pad-controls", &Options::check_pad_controls},
    {"--check-running-while-inactive", &Options::check_running_while_inactive},
    {"--touch-controls", &Options::touch_controls},
    {"--check-game-files", &Options::check_game_files},
    {"--no-game-files-screen", &Options::no_game_files_screen},
    {"--check-multiplayer-menu", &Options::check_multiplayer_menu},
    {"--check-director-view", &Options::check_director_view},
    {"--check-director-render", &Options::check_director_render},
    {"--check-interpolation", &Options::check_interpolation},
    {"--check-unit-playout", &Options::check_unit_playout},
    {"--trace-input", &Options::trace_input},
    {"--keys-reset", &Options::keys_reset},
    {"--debug-order-lines", &Options::debug_order_lines},
};

/// Turns on the member of an option kFlagOptions lists.
///
/// @param argument the option
/// @param[in,out] options the options read so far
/// @return true when kFlagOptions lists the option
[[nodiscard]] bool take_flag_option(std::string_view argument, Options& options) {
    for (const auto& [name, member] : kFlagOptions)
        if (argument == name) {
            options.*member = true;
            return true;
        }
    return false;
}

/// Returns the value after the option the cursor stands on, and moves the
/// cursor onto it.
///
/// Throws std::runtime_error naming the option when no value follows or the
/// value is empty.
///
/// @param[in,out] cursor the command line, standing on the option
/// @return the value
[[nodiscard]] std::string_view option_value(ArgumentCursor& cursor) {
    return next_value(&cursor);
}

/// Reads an option that names a folder or file the run reads or writes, or
/// a file to open: the game and mod folders, archives, the snapshot, the
/// preferences, the data, the player's and the log's folders.
///
/// @param argument the option
/// @param[in,out] cursor the command line, standing on the option; moved
///        onto its value when it takes one
/// @param[in,out] options the options read so far
/// @return true when the option is one of these
[[nodiscard]] bool
take_folder_option(std::string_view argument, ArgumentCursor& cursor, Options& options) {
    if (argument == "--game-dir") {
        options.game_dir = path_from_utf8(option_value(cursor));
        return true;
    }
    if (argument == "--archive") {
        options.archives.push_back(path_from_utf8(option_value(cursor)));
        return true;
    }
    if (argument == "--mod") {
        options.mod_file = path_from_utf8(option_value(cursor));
        return true;
    }
    if (argument == "--mod-dir") {
        options.mod_dir = path_from_utf8(option_value(cursor));
        return true;
    }
    if (argument == "--snapshot") {
        options.snapshot = path_from_utf8(option_value(cursor));
        return true;
    }
    if (argument == "--preferences-file") {
        options.preferences_file = path_from_utf8(option_value(cursor));
        return true;
    }
    if (argument == "--data-dir") {
        options.data_dir = path_from_utf8(option_value(cursor));
        return true;
    }
    if (argument == "--user-folder") {
        options.user_folder = path_from_utf8(option_value(cursor));
        return true;
    }
    if (argument == "--log-dir") {
        options.log_dir = path_from_utf8(option_value(cursor));
        return true;
    }
    if (argument == "--open" || argument == "--install-mod") {
        options.open_files.push_back(opened_file_path(option_value(cursor)));
        return true;
    }
    if (argument == "--check-content-copy") {
        options.check_content_copy = path_from_utf8(option_value(cursor));
        return true;
    }
    if (argument == "--check-installer") {
        options.check_installer = path_from_utf8(option_value(cursor));
        return true;
    }
    return false;
}

/// Reads an option that sets how long a run lasts and how it draws: its
/// frames, its frame rates, its window and the threads that draw.
///
/// @param argument the option
/// @param[in,out] cursor the command line, standing on the option; moved
///        onto its value when it takes one
/// @param[in,out] options the options read so far
/// @return true when the option is one of these
[[nodiscard]] bool
take_frame_option(std::string_view argument, ArgumentCursor& cursor, Options& options) {
    if (argument == "--frames") {
        options.frame_limit = parse_count(option_value(cursor));
        return true;
    }
    if (argument == "--benchmark") {
        options.benchmark_frames = parse_count(option_value(cursor));
        return true;
    }
    if (argument == "--max-fps") {
        options.max_frames_per_second = parse_frame_rate(
            option_value(cursor),
            kLowestMaxFramesPerSecond,
            true,
            "--max-fps expects 0 for no limit, or frames a second from 30 through 1000"
        );
        options.max_frames_per_second_given = true;
        return true;
    }
    if (argument == "--display-modes") {
        options.display_modes = std::string(option_value(cursor));
        if (!oa::platform::display_modes::report_from_text(options.display_modes))
            throw std::runtime_error(
                "--display-modes expects WIDTHxHEIGHT[@RATE][/DENSITY] modes separated "
                "by commas, or none"
            );
        return true;
    }
    if (argument == "--frame-rate") {
        options.frame_rate = parse_frame_rate(
            option_value(cursor),
            1,
            false,
            "--frame-rate expects frames a second from 1 through 1000"
        );
        return true;
    }
    if (argument == "--frame-log") {
        options.frame_log = path_from_utf8(option_value(cursor));
        return true;
    }
    if (argument == "--frame-clock") {
        options.frame_clock_ms = parse_frame_clock(option_value(cursor));
        return true;
    }
    if (argument == "--resolution") {
        const auto text = std::string(option_value(cursor));
        const auto separator = text.find('x');
        if (separator == std::string::npos)
            throw std::runtime_error("--resolution expects WIDTHxHEIGHT");
        options.match_width = static_cast<int>(parse_count(text.substr(0, separator)));
        options.match_height = static_cast<int>(parse_count(text.substr(separator + 1)));
        if (options.match_width <= 0 || options.match_height <= 0)
            throw std::runtime_error("--resolution expects a width and height above 0");
        options.window_resolution = true;
        return true;
    }
    if (argument == "--draw-threads") {
        options.draw_threads = parse_draw_threads(option_value(cursor), argument);
        return true;
    }
    return false;
}

/// Reads an option that sets up the match a run plays: its length, the
/// campaign mission, the armies, the stage, the saved games, the zoom, the
/// camera and the random seed.
///
/// @param argument the option
/// @param[in,out] cursor the command line, standing on the option; moved
///        onto its value when it takes one
/// @param[in,out] options the options read so far
/// @return true when the option is one of these
[[nodiscard]] bool
take_match_option(std::string_view argument, ArgumentCursor& cursor, Options& options) {
    if (argument == "--match-ticks") {
        options.match_ticks = parse_count(option_value(cursor));
        return true;
    }
    if (argument == "--campaign") {
        options.campaign = option_value(cursor);
        return true;
    }
    if (argument == "--mission") {
        options.campaign_mission = parse_count(option_value(cursor));
        return true;
    }
    if (argument == "--restart-at") {
        options.campaign_restart_tick = parse_count(option_value(cursor));
        return true;
    }
    if (argument == "--combat") {
        options.combat_units = parse_count(option_value(cursor));
        return true;
    }
    if (argument == "--stage") {
        options.stage_file = path_from_utf8(option_value(cursor));
        return true;
    }
    if (argument == "--weapon-fire-check") {
        options.weapon_fire_check = std::string(option_value(cursor));
        return true;
    }
    if (argument == "--save-after") {
        options.save_after = parse_count(option_value(cursor));
        return true;
    }
    if (argument == "--save-file") {
        options.save_file = path_from_utf8(option_value(cursor));
        return true;
    }
    if (argument == "--load") {
        options.load_file = path_from_utf8(option_value(cursor));
        return true;
    }
    if (argument == "--zoom") {
        const auto text = std::string(option_value(cursor));
        char* end = nullptr;
        options.match_zoom = std::strtof(text.c_str(), &end);
        if (end == text.c_str() || *end != '\0' || !(options.match_zoom > 0.0F))
            throw std::runtime_error("--zoom expects a positive number");
        return true;
    }
    if (argument == "--camera") {
        const auto text = std::string(option_value(cursor));
        const auto separator = text.find(',');
        if (separator == std::string::npos)
            throw std::runtime_error("--camera expects X,Z");
        options.camera = {
            static_cast<int>(parse_count(text.substr(0, separator))),
            static_cast<int>(parse_count(text.substr(separator + 1)))
        };
        return true;
    }
    if (argument == "--seed") {
        options.seed = parse_seed(option_value(cursor));
        return true;
    }
    if (argument == "--brain-match") {
        options.brain_match = option_value(cursor);
        return true;
    }
    if (argument == "--brain-levels") {
        options.brain_levels = option_value(cursor);
        return true;
    }
    if (argument == "--brain-map") {
        options.brain_map = option_value(cursor);
        return true;
    }
    if (argument == "--brain-shadow") {
        options.brain_shadow = option_value(cursor);
        return true;
    }
    if (argument == "--brain-shadow-sections") {
        options.brain_shadow_sections = option_value(cursor);
        return true;
    }
    if (argument == "--brain-shadow-log") {
        options.brain_shadow_log = option_value(cursor);
        return true;
    }
    if (argument == "--brain-trace") {
        options.brain_trace = option_value(cursor);
        return true;
    }
    if (argument == "--brain-record") {
        options.brain_record = option_value(cursor);
        return true;
    }
    return false;
}

/// Reads a self-check's option that takes a value, or a word after it: the
/// navigation check and its group, the failure the renderer ladder check
/// narrows to, and the pages and the language the unit checks show.
///
/// @param argument the option
/// @param[in,out] cursor the command line, standing on the option; moved
///        onto its value when it takes one
/// @param[in,out] options the options read so far
/// @return true when the option is one of these
[[nodiscard]] bool
take_check_option(std::string_view argument, ArgumentCursor& cursor, Options& options) {
    if (argument == "--check-navigation") {
        options.check_navigation = true;
        // A group's name may follow; any other word is read as before.
        if (*cursor.index + 1 < cursor.argc)
            if (const auto group = navigation_group_named(cursor.argv[*cursor.index + 1])) {
                options.navigation_group = *group;
                ++*cursor.index;
            }
        return true;
    }
    if (argument == "--render-fault") {
        options.render_fault = parse_render_fault(option_value(cursor));
        return true;
    }
    if (argument == "--check-unit-pages") {
        options.check_unit_pages = option_value(cursor);
        return true;
    }
    if (argument == "--check-key-table") {
        options.check_key_table = option_value(cursor);
        return true;
    }
    if (argument == "--check-unit-language") {
        options.check_unit_language = option_value(cursor);
        return true;
    }
    if (argument == "--check-language") {
        options.check_languages.emplace_back(option_value(cursor));
        return true;
    }
    if (argument == "--update-feed") {
        options.update_feed = std::string(option_value(cursor));
        return true;
    }
    if (argument == "--update-key") {
        const std::string_view text = option_value(cursor);
        const std::size_t split = text.find('=');
        bool accepted = split != std::string_view::npos && split != 0;
        if (accepted) {
            const std::string_view ident = text.substr(0, split);
            for (const char byte : ident) {
                const auto byte_value = static_cast<unsigned char>(byte);
                if (byte_value <= static_cast<unsigned char>(' ') || byte_value == 0x7F)
                    accepted = false;
            }
            accepted =
                accepted && oa::base::signing::parse_public_key(text.substr(split + 1)).has_value();
        }
        if (!accepted)
            throw std::runtime_error("--update-key must be ID=ed25519:KEY");
        options.update_keys.emplace_back(text);
        return true;
    }
    if (argument == "--updated-from") {
        const std::string text{option_value(cursor)};
        if (!oa::formats::oamod::parse_engine_version(text).has_value())
            throw std::runtime_error("--updated-from must be a version such as 0.8.7");
        options.updated_from = text;
        return true;
    }
    if (argument == "--update-install-folder") {
        const std::string_view text = option_value(cursor);
        if (text.empty())
            throw std::runtime_error("--update-install-folder needs a folder");
        options.update_install_folder = path_from_utf8(text);
        return true;
    }
    if (argument == "--update-download-case") {
        const std::string text{option_value(cursor)};
        if (text.empty())
            throw std::runtime_error("--update-download-case needs a name");
        options.update_download_case = text;
        return true;
    }
    return false;
}

/// Reads one of --check-game-files's companions, which set the route the
/// check takes through the Game files screen and what it expects there.
///
/// @param argument the option
/// @param[in,out] cursor the command line, standing on the option; moved
///        onto its value
/// @param[in,out] options the options read so far
/// @return true when the option is one of these
[[nodiscard]] bool
take_game_files_option(std::string_view argument, ArgumentCursor& cursor, Options& options) {
    if (argument == "--game-files-route") {
        options.game_files_route = parse_game_files_route(option_value(cursor));
        return true;
    }
    if (argument == "--game-files-expect") {
        options.game_files_expect = parse_game_files_expect(option_value(cursor));
        return true;
    }
    if (argument == "--game-files-source") {
        options.game_files_source = path_from_utf8(option_value(cursor));
        return true;
    }
    if (argument == "--game-files-free-bytes") {
        options.game_files_free_bytes =
            parse_game_files_bytes(option_value(cursor), argument, false);
        return true;
    }
    if (argument == "--game-files-copy-rate") {
        options.game_files_copy_rate = parse_game_files_bytes(option_value(cursor), argument, true);
        return true;
    }
    if (argument == "--game-files-stop-after") {
        options.game_files_stop_after =
            parse_game_files_bytes(option_value(cursor), argument, true);
        return true;
    }
    return false;
}

/// Reads an option of the director, the traces, the video capture or the
/// showcases.
///
/// @param argument the option
/// @param[in,out] cursor the command line, standing on the option; moved
///        onto its value
/// @param[in,out] options the options read so far
/// @return true when the option is one of these
[[nodiscard]] bool
take_recording_option(std::string_view argument, ArgumentCursor& cursor, Options& options) {
    if (argument == "--generate-script") {
        options.generate_script = path_from_utf8(option_value(cursor));
        return true;
    }
    if (argument == "--render-script") {
        options.render_script = path_from_utf8(option_value(cursor));
        return true;
    }
    if (argument == "--output") {
        options.director_output = path_from_utf8(option_value(cursor));
        return true;
    }
    if (argument == "--chunks") {
        options.director_chunks = parse_chunks(option_value(cursor));
        return true;
    }
    if (argument == "--stills") {
        options.director_stills = parse_stills(option_value(cursor));
        return true;
    }
    if (argument == "--effect") {
        take_effect(option_value(cursor), options.director_effects);
        return true;
    }
    if (argument == "--trace-lookups") {
        options.trace_lookups = path_from_utf8(option_value(cursor));
        return true;
    }
    if (argument == "--trace-digest") {
        options.trace_digest = path_from_utf8(option_value(cursor));
        return true;
    }
    if (argument == "--trace-units") {
        options.trace_units = path_from_utf8(option_value(cursor));
        return true;
    }
    if (argument == "--capture-video") {
        options.capture_video = path_from_utf8(option_value(cursor));
        return true;
    }
    if (argument == "--showcase") {
        options.showcase = parse_showcase(option_value(cursor));
        return true;
    }
    return false;
}

/// Reads one of the engine's options that turns on a member or takes a
/// value. The hardware acceleration flags, --help, an extension's options,
/// files to open and the game's switches are read by parse_options itself.
///
/// @param argument the option
/// @param[in,out] cursor the command line, standing on the option; moved
///        onto its value when it takes one
/// @param[in,out] options the options read so far
/// @return true when the option is one of these
[[nodiscard]] bool
take_engine_option(std::string_view argument, ArgumentCursor& cursor, Options& options) {
    if (argument == "--mutator") {
        const std::string value{option_value(cursor)};
        std::string error;
        if (!oa::data::mutators::parse_choice(value, &error))
            throw std::runtime_error("--mutator: " + error);
        options.mutators.push_back(value);
        return true;
    }
    if (argument == "--keys") {
        options.keys = std::string(option_value(cursor));
        return true;
    }
    if (argument == "--content") {
        options.content = std::string(option_value(cursor));
        return true;
    }
    return take_flag_option(argument, options) || take_folder_option(argument, cursor, options) ||
           take_frame_option(argument, cursor, options) ||
           take_match_option(argument, cursor, options) ||
           take_check_option(argument, cursor, options) ||
           take_game_files_option(argument, cursor, options) ||
           take_recording_option(argument, cursor, options);
}

} // namespace

namespace {

/// Writes `text` to the console this program was started from, when Windows
/// gave the program no console of its own and nothing is capturing the output.
void write_parent_console(std::string_view text) {
#if defined(_WIN32)
    // The console of the process that started this one. Windows names it -1.
    constexpr DWORD kParentConsole = 0xFFFFFFFFu;
    const HANDLE current = GetStdHandle(STD_OUTPUT_HANDLE);
    if (current != nullptr && current != INVALID_HANDLE_VALUE) {
        const DWORD kind = GetFileType(current);
        if (kind == FILE_TYPE_PIPE || kind == FILE_TYPE_DISK)
            return;
    }
    const HMODULE kernel = GetModuleHandleW(L"kernel32.dll");
    if (kernel == nullptr)
        return;
    using AttachConsoleFunction = BOOL(WINAPI*)(DWORD);
    // A direct cast of GetProcAddress is a cast between function types, which
    // this build rejects. Copy the pointer instead.
    const auto looked_up = GetProcAddress(kernel, "AttachConsole");
    AttachConsoleFunction attach = nullptr;
    static_assert(sizeof(attach) == sizeof(looked_up));
    std::memcpy(&attach, &looked_up, sizeof(attach));
    if (attach == nullptr || attach(kParentConsole) == FALSE)
        return;
    const HANDLE console = CreateFileW(
        L"CONOUT$",
        GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (console == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    std::ignore =
        WriteFile(console, text.data(), static_cast<DWORD>(text.size()), &written, nullptr);
    std::ignore = CloseHandle(console);
#else
    (void)text;
#endif
}

/// Finds the game folder, where a launch file named without a folder is looked for after the current folder.
///
/// It is the folder --game-dir names, or else the one the preferences
/// remember from an earlier start.
///
/// @param context the Options being parsed
/// @return the folder, or nothing when neither names one
std::optional<fs::path> launch_game_folder(const void* context) {
    const auto& options = *static_cast<const Options*>(context);
    if (!options.game_dir.empty())
        return options.game_dir;
    try {
        const auto values = oa::platform::preferences::load(
            options.preferences_file ? *options.preferences_file
                                     : oa::platform::preferences::default_file()
        );
        const auto found = values.find(std::string(kGameDirectoryPreference));
        if (found != values.end() && !found->second.empty())
            return path_from_utf8(found->second);
    } catch (const std::exception&) {
        // Unreadable preferences remember no folder.
    }
    return std::nullopt;
}

} // namespace

void write_startup_line(std::string_view text) {
    std::string bytes(text);
    if (bytes.empty() || bytes.back() != '\n')
        bytes.push_back('\n');
    // A test replaces the output buffer and reads it back. The console is
    // written only for the program's own buffer, so that capture stays one copy.
    static std::streambuf* const own_buffer = std::cout.rdbuf();
    if (std::cout.rdbuf() == own_buffer)
        write_parent_console(bytes);
    std::cout << bytes;
    std::exit(0);
}

[[nodiscard]] Options parse_options(int argc, char** argv, const Extension& extension) {
    Options result;
    // The game's switches, each argument one word: the system has already
    // split the command line, honouring its quotes.
    std::vector<const char*> switch_words;
    uint32_t extension_effects = 0;
    // The acceleration flags, wherever they stand, must name one level: the
    // first given and the first that names another are refused once the
    // line is read.
    using oa::ui::engine_settings::HardwareAcceleration;

    struct AccelerationFlagGiven {
        std::string text;
        HardwareAcceleration level{};
    };

    std::optional<AccelerationFlagGiven> acceleration_first;
    std::optional<AccelerationFlagGiven> acceleration_differing;
    const auto take_acceleration = [&](std::string_view text, HardwareAcceleration level) {
        result.hardware_acceleration = level;
        if (!acceleration_first)
            acceleration_first = AccelerationFlagGiven{std::string(text), level};
        else if (acceleration_first->level != level && !acceleration_differing)
            acceleration_differing = AccelerationFlagGiven{std::string(text), level};
    };
    for (int index = 1; index < argc; ++index) {
        const std::string_view argument(argv[index]);
        ArgumentCursor cursor{argc, argv, &index, argument};
        // The engine's options come before an extension's. Each is matched
        // by its whole name, so no two of them can take the same argument.
        if (take_engine_option(argument, cursor, result))
            continue;
        if (argument == "--hardware-acceleration") {
            take_acceleration(argument, HardwareAcceleration::full);
        } else if (argument.starts_with(kAccelerationLevelFlag)) {
            const auto level = oa::ui::engine_settings::hardware_acceleration_from_text(
                argument.substr(kAccelerationLevelFlag.size())
            );
            if (!level)
                throw std::runtime_error("--hardware-acceleration takes off, basic or full");
            take_acceleration(argument, *level);
        } else if (argument == "--no-hardware-acceleration") {
            take_acceleration(argument, HardwareAcceleration::off);
        } else if (argument == "--help" || (argument == "-h" && index + 1 == argc)) {
            // -h with nothing after it is the help flag; with a word after
            // it, it is the game's "-h NAME".
            std::ostringstream usage;
            usage << "usage: open-annihilation [--version] [--game-dir PATH | --choose-game-dir] "
                     "[--archive PATH]... "
                     "[--mod FILE] [--mod-dir PATH | --base-game] "
                     "[--mutator PATH|ID[:OPTION=VALUE,...]] [--no-mutators] "
                     "[--keys ID|PATH] [--keys-reset] "
                     "[--content GAME[:EXPANSION,...]] [--print-profile] "
                     "[--accept-unimplemented-hacks] "
                     "[--skip-intro] [--first-run] [--frames N] [--headless-check] "
                     "[--snapshot PATH.ppm] [--preferences-file PATH] [--data-dir PATH] "
                     "[--user-folder PATH] [--log-dir PATH] "
                     "[--mute] "
                     "[--check-navigation [screens|orders|outcomes|zoom|zoom-1366x768|"
                     "zoom-1920x1080|zoom-2560x1440|campaign]] "
                     "[--check-match-dialogs] [--check-match-layers] "
                     "[--check-render-tiers [--force-capable]] "
                     "[--check-match-orders] [--check-side-orders] [--check-skirmish-sides] "
                     "[--check-mod-limits] "
                     "[--check-factory-orders] "
                     "[--check-unit-speech] [--check-mutator-layer] [--check-mutator-verdicts] "
                     "[--check-replay-loadout] "
                     "[--check-side-data-per-match] "
                     "[--check-gadget-art-per-match] "
                     "[--check-download-builds] [--check-stockpile-builds] "
                     "[--check-unit-page-memory] [--check-side-column] [--check-match-bars] "
                     "[--check-unit-pages whole|scaled:TYPE,...] "
                     "[--check-kill-board] [--check-paused-save] [--check-saved-brains] "
                     "[--check-simulation-hash] "
                     "[--check-zoom-memory] "
                     "[--check-unit-language TAG] "
                     "[--check-language-switch [--check-language TAG]] "
                     "[--check-language-registry] "
                     "[--check-language-notice] [--check-first-run] [--check-setup-window] "
                     "[--check-window-place] "
                     "[--check-progress] [--update-feed URL] [--update-key ID=ed25519:KEY] "
                     "[--updated-from VERSION] [--update-install-folder DIR] [--update-and-quit] "
                     "[--check-engine-feed] [--check-update-offer] [--check-update-download] "
                     "[--update-download-case NAME] [--check-icloud-wait] "
                     "[--check-text-fit [--check-language TAG]...] "
                     "[--check-folder-chooser] "
                     "[--check-patrol-reclaim] [--check-reclaim-cursor] [--check-wreck-pick] "
                     "[--check-click-snap] "
                     "[--check-build-preview] "
                     "[--check-megamap-clicks] [--check-single-player-offers] "
                     "[--check-radar-orders] "
                     "[--check-key-latches] [--check-hold-confirm] "
                     "[--check-key-table FILE] [--check-key-timing] [--check-pointer-keys] "
                     "[--check-pointer-interfaces] [--check-pad-controls] [--check-touch-controls] "
                     "[--check-game-files] [--check-content-copy PATH] "
                     "[--check-installer FILE] "
                     "[--check-multiplayer-menu] "
                     "[--check-load-save] [--check-frontend-controls] "
                     "[--check-scroll-bars] [--check-engine-settings [--force-capable]] "
                     "[--check-registered-kit] [--check-kit-gallery] "
                     "[--check-user-folder] [--check-mod-switch] [--check-map-packs] "
                     "[--check-map-pack-lists] [--check-mod-map-settings] "
                     "[--check-map-browser] [--check-mutator-button] [--check-mutator-picker] "
                     "[--check-brain-picker] "
                     "[--check-brain-seat-buttons] "
                     "[--check-brain-campaign] "
                     "[--check-mutator-prompts] [--check-mutator-dry-run] "
                     "[--check-mutator-room] [--check-brain-room] [--check-mod-warning] "
                     "[--check-mod-install] [--check-language-install] [--check-challenge] "
                     "[--check-registries] "
                     "[--check-library] [--check-library-install] "
                     "[--check-library-map-previews] [--check-library-content] "
                     "[--check-library-update-question] "
                     "[--check-renderer-ladder [--render-fault POINT[@FRAME]]] "
                     "[--check-renderer-proof] [--check-live-renderer] "
                     "[--check-briefing-narration] [--check-director-view] "
                     "[--check-director-render] [--check-interpolation] "
                     "[--check-unit-playout] [--check-running-while-inactive] "
                     "[--trace-input] "
                  << extension_text(extension, ExtensionText::usage_checks, "")
                  << "[--debug-order-lines] "
                     "[--max-fps N] "
                     "[--hardware-acceleration[=off|basic|full] | --no-hardware-acceleration] "
                     "[--experimental-3d] [--native-density] [--display-modes MODES] "
                     "[--benchmark FRAMES] [--match-ticks N "
                     "[--frame-rate FPS [--frame-log FILE] [--scroll-camera] [--march] "
                     "[--follow] [--frame-clock MS]]] "
                     "[--campaign NAME --mission N [--past-outcome] [--restart-at TICK]] "
                     "[--resolution WxH] [--touch-controls] "
                     "[--game-files-route folder|demo|copy-yourself|manage] "
                     "[--game-files-expect main-menu|stopped-kept|resumed|not-a-game|short-space|"
                     "next-start] "
                     "[--game-files-source PATH] [--game-files-free-bytes N] "
                     "[--game-files-copy-rate BYTES] [--game-files-stop-after BYTES] "
                     "[--no-game-files-screen] "
                     "[--zoom FACTOR] [--combat UNITS [--busy-combat]] [--stage FILE] "
                     "[--reclaim-check] [--weapon-fire-check TYPE] "
                     "[--camera X,Z] "
                  << extension_text(extension, ExtensionText::usage_runs, "")
                  << "[--save-after TICK] "
                     "[--save-file PATH.sav] [--load PATH.sav] [--give-orders] [--seed N] "
                     "[--brain-match A,B] [--brain-levels L1,L2] [--brain-map NAME] "
                     "[--brain-shadow BRAIN [--brain-shadow-sections LIST] "
                     "[--brain-shadow-log DIR]] "
                     "[--brain-trace FILE] [--brain-record FILE] "
                     "[--trace-digest FILE] [--trace-units FILE] [--trace-lookups FILE] "
                     "[--draw-threads N] "
                     "[--capture-video PATH.mp4] [--showcase "
                     "arm-first-mission|skirmish-battle|base-construction] "
                     "[--open FILE]... [--install-mod FILE]... "
                     "[--generate-script RECORDING [--output PATH.oascript|PATH.oamovie] "
                     "[--resolution WxH]] "
                     "[--render-script PATH.oascript|PATH.oamovie [--output DIR] "
                     "[--chunks A-B] [--stills F,F...]] "
                     "[game switches such as "
                  << extension_text(extension, ExtensionText::usage_switches, "")
                  << "-d|-df -f -l -r TEXT -s -w] "
                     "[LANGUAGE]\n"
                     "--generate-script plans a director script from a recording an "
                     "extension of this build replays;\n"
                     "--render-script renders a director script, or a bundle of one and "
                     "its recording, to video (docs/director.md).\n"
                  << extension_text(extension, ExtensionText::usage_note, "");
            write_startup_line(usage.str());
        } else if (argument.starts_with("--")) {
            const OptionValues values{&cursor, next_value, following_value};
            uint32_t effects = 0;
            if (!call_hook_or_raise<&Extension::take_option>(
                    extension, argv[index], values, effects
                ))
                throw std::runtime_error("unknown option: " + std::string(argument));
            if ((effects & option_effect::headless_check) != 0)
                result.headless_check = true;
            if ((effects & option_effect::skip_intro) != 0)
                result.skip_intro = true;
            if ((effects & option_effect::remote_controlled) != 0)
                result.remote_controlled = true;
            extension_effects |= effects;
        } else if (argument.starts_with("-psn_")) {
            // macOS names the process it started from the Finder; nothing to read.
            continue;
        } else if (names_opened_file(argument)) {
            // One of the six file types the game installs or adds, opened with the
            // game or dropped on it.
            result.open_files.push_back(opened_file_path(argument));
        } else {
            switch_words.push_back(argv[index]);
        }
    }
    GuardedSwitches switches;
    switches.handler = call_hook_or_raise<&Extension::switch_handler>(extension);
    switches.guard = {&switches, take_guarded_switch, reset_guarded_switches};
    const auto parsed = oa::app::command_line::parse(
        switch_words, result.launch, switches.handler != nullptr ? &switches.guard : nullptr
    );
    // A handler that threw stops the start with its message, as a hook that
    // raises its error does.
    if (switches.error.caught)
        throw std::runtime_error(switches.error.message);
    switch (parsed) {
    case oa::app::command_line::Status::run:
        break;
    case oa::app::command_line::Status::register_application:
        // 3.1c registers itself with DirectPlay and ends; there is nothing
        // to register here, so the game says so and ends as well.
        write_startup_line(extension_text(
            extension,
            ExtensionText::register_switch,
            "-r registers the game for multiplayer, which this game does not need"
        ));
        break;
    case oa::app::command_line::Status::line_too_long:
        throw std::runtime_error("the game switches exceed the command-line limit");
    case oa::app::command_line::Status::unavailable_switch:
        throw std::runtime_error(
            std::string("-") + result.launch.unavailable_switch + " is not handled by this build"
        );
    }
    refuse_left_out_self_checks(result);
    if (!result.update_keys.empty() && !result.update_feed.has_value())
        throw std::runtime_error("--update-key needs --update-feed");
    if (result.update_install_folder.has_value() && !result.update_feed.has_value())
        throw std::runtime_error("--update-install-folder needs --update-feed");
    if (result.update_and_quit && !result.update_feed.has_value())
        throw std::runtime_error("--update-and-quit needs --update-feed");
    if (!result.update_download_case.empty() && !result.check_update_download)
        throw std::runtime_error("--update-download-case needs --check-update-download");
    if (!result.keys.empty() && result.keys_reset)
        throw std::runtime_error("--keys and --keys-reset cannot be used together");
    if (acceleration_differing) {
        // Named in one order whichever came first.
        const std::string& first = acceleration_first->text;
        const std::string& second = acceleration_differing->text;
        throw std::runtime_error(
            std::min(first, second) + " and " + std::max(first, second) + " cannot be used together"
        );
    }
    if (result.campaign_mission.has_value() != !result.campaign.empty())
        throw std::runtime_error("--campaign and --mission are used together");
    if (result.campaign_restart_tick && !result.campaign_mission)
        throw std::runtime_error("--restart-at needs --campaign and --mission");
    if (!result.trace_units.empty() && result.trace_digest.empty())
        throw std::runtime_error("--trace-units needs --trace-digest");
    if (result.frame_rate && (!result.match_ticks || result.campaign_mission || result.save_after ||
                              !result.load_file.empty()))
        throw std::runtime_error("--frame-rate draws a headless skirmish of --match-ticks ticks");
    if (!result.frame_rate && (!result.frame_log.empty() || result.scroll_camera || result.march ||
                               result.follow || result.frame_clock_ms))
        throw std::runtime_error(
            "--frame-log, --scroll-camera, --march, --follow and --frame-clock need --frame-rate"
        );
    {
        // A launch file named without a folder is looked for in the game
        // folder too, which only the engine's options name, while the hook
        // reads it.
        struct ForgetGameFolder {
            ~ForgetGameFolder() { find_launch_files_in({}); }
        };

        find_launch_files_in({launch_game_folder, &result});
        const ForgetGameFolder forget;
        call_hook_or_raise<&Extension::check_options>(extension);
    }
    // A launch file refused inside the hook is raised here, so main exits 2.
    raise_launch_refusal();
    adopt_launch_run(result);
    note_game_switches(result);
    if (const auto env = oa::platform::environment_value("OA_DEBUG_ORDER_LINES");
        env && !env->empty())
        result.debug_order_lines = true;
    if (const auto env = oa::platform::environment_value("OA_DRAW_THREADS");
        !result.draw_threads && env && !env->empty())
        result.draw_threads = parse_draw_threads(*env, "OA_DRAW_THREADS");
    // These checks skip the window and intro; an offscreen GPU check starts
    // only the SDL subsystems it needs.
    if (result.check_director_view || result.check_director_render || result.check_interpolation ||
        result.check_unit_playout || result.check_renderer_proof || result.check_live_renderer) {
        result.headless_check = true;
        result.skip_intro = true;
    }
    if (result.check_live_renderer)
        result.experimental_3d = true;
    check_director_options(result);
    if (result.busy_combat && result.combat_units == 0)
        throw std::runtime_error("--busy-combat needs --combat");
    if (!result.stage_file.empty() &&
        (!result.match_ticks || !result.load_file.empty() || !result.campaign.empty()))
        throw std::runtime_error("--stage stages a headless skirmish of --match-ticks ticks");
    if (!result.weapon_fire_check.empty() &&
        (!result.headless_check || !result.match_ticks || !result.load_file.empty() ||
         !result.campaign.empty()))
        throw std::runtime_error(
            "--weapon-fire-check fires in a headless skirmish of --match-ticks ticks"
        );
    if (!result.brain_levels.empty() && result.brain_match.empty())
        throw std::runtime_error("--brain-levels needs --brain-match");
    if (!result.brain_map.empty() && result.brain_match.empty())
        throw std::runtime_error("--brain-map needs --brain-match");
    if (!result.brain_shadow_sections.empty() && result.brain_shadow.empty())
        throw std::runtime_error("--brain-shadow-sections needs --brain-shadow");
    if (!result.brain_shadow_log.empty() && result.brain_shadow.empty())
        throw std::runtime_error("--brain-shadow-log needs --brain-shadow");
    if (!result.brain_trace.empty()) {
        if (!result.headless_check)
            throw std::runtime_error("--brain-trace needs --headless-check");
        if (!result.match_ticks)
            throw std::runtime_error("--brain-trace needs --match-ticks");
    }
    if (!result.brain_record.empty()) {
        if (!result.headless_check)
            throw std::runtime_error("--brain-record needs --headless-check");
        if (!result.match_ticks)
            throw std::runtime_error("--brain-record needs --match-ticks");
    }
    if (!result.brain_match.empty()) {
        if (!result.headless_check)
            throw std::runtime_error("--brain-match needs --headless-check");
        if (!result.match_ticks)
            throw std::runtime_error("--brain-match needs --match-ticks");
        if (result.brain_map.empty())
            throw std::runtime_error("--brain-match needs --brain-map");
        if (!result.stage_file.empty())
            throw std::runtime_error("--brain-match cannot be used with --stage");
        if (result.combat_units != 0)
            throw std::runtime_error("--brain-match cannot be used with --combat");
        if (!result.load_file.empty())
            throw std::runtime_error("--brain-match cannot be used with --load");
        if (result.save_after)
            throw std::runtime_error("--brain-match cannot be used with --save-after");
        if (!result.campaign.empty())
            throw std::runtime_error("--brain-match cannot be used with --campaign");
        if (result.frame_rate)
            throw std::runtime_error("--brain-match cannot be used with --frame-rate");
        if (result.benchmark_frames)
            throw std::runtime_error("--brain-match cannot be used with --benchmark");
        std::string error;
        if (!parse_brain_match(result.brain_match, result.brain_levels, error))
            throw std::runtime_error(error);
    }
    if (result.check_render_tiers && result.headless_check)
        throw std::runtime_error(
            "--check-render-tiers draws in a window and cannot be used with --headless-check"
        );
    if (result.check_progress && result.headless_check)
        throw std::runtime_error(
            "--check-progress draws in a window and cannot be used with --headless-check"
        );
    if (result.check_engine_feed && result.headless_check)
        throw std::runtime_error(
            "--check-engine-feed draws in a window and cannot be used with --headless-check"
        );
    if (result.check_update_offer && result.headless_check)
        throw std::runtime_error(
            "--check-update-offer draws in a window and cannot be used with --headless-check"
        );
    if (result.check_update_download && result.headless_check)
        throw std::runtime_error(
            "--check-update-download draws in a window and cannot be used with --headless-check"
        );
    if (result.check_icloud_wait && result.headless_check)
        throw std::runtime_error(
            "--check-icloud-wait draws in a window and cannot be used with --headless-check"
        );
    if (result.check_running_while_inactive && result.headless_check)
        throw std::runtime_error(
            "--check-running-while-inactive keeps a window and cannot be used with "
            "--headless-check"
        );
    if (result.force_capable && !result.check_render_tiers && !result.check_build_preview &&
        !result.check_engine_settings && !result.check_kill_board)
        throw std::runtime_error(
            "--force-capable is accepted only with --check-render-tiers, "
            "--check-build-preview, --check-engine-settings and --check-kill-board"
        );
    if (result.render_fault && !result.check_renderer_ladder)
        throw std::runtime_error("--render-fault needs --check-renderer-ladder");
    if (!result.check_languages.empty() && !result.check_text_fit && !result.check_language_switch)
        throw std::runtime_error(
            "--check-language needs --check-text-fit or --check-language-switch"
        );
    if (result.check_languages.size() > 1 && result.check_language_switch)
        throw std::runtime_error("--check-language-switch takes one --check-language");
    check_game_files_options(result);
    result.fixed_clock =
        result.headless_check || result.check_match_layers || result.check_render_tiers ||
        result.check_match_dialogs || result.check_load_save || result.check_frontend_controls ||
        result.check_scroll_bars || result.check_engine_settings || result.check_registered_kit ||
        result.check_kit_gallery || result.check_user_folder || result.check_mod_switch ||
        result.check_mod_warning || result.check_mod_install || result.check_language_install ||
        result.check_challenge || result.check_registries || result.check_library ||
        result.check_library_install || result.check_library_map_previews ||
        result.check_library_content || result.check_library_update_question ||
        result.check_renderer_ladder || result.check_match_orders || result.check_factory_orders ||
        result.check_unit_speech || result.check_mutator_layer || result.check_mutator_verdicts ||
        result.check_mutator_dry_run || result.check_replay_loadout ||
        result.check_side_data_per_match || result.check_gadget_art_per_match ||
        result.check_download_builds || result.check_stockpile_builds ||
        result.check_unit_page_memory || result.check_side_column || result.check_match_bars ||
        !result.check_unit_pages.empty() || !result.check_key_table.empty() ||
        result.check_key_latches || result.check_hold_confirm || result.check_kill_board ||
        !result.check_unit_language.empty() || result.check_language_switch ||
        result.check_language_registry || result.check_language_notice || result.check_first_run ||
        result.check_setup_window || result.check_window_place || result.check_progress ||
        result.check_engine_feed || result.check_update_offer || result.check_update_download ||
        result.check_text_fit || result.check_folder_chooser || result.check_patrol_reclaim ||
        result.check_reclaim_cursor || result.check_wreck_pick || result.check_build_preview ||
        result.check_pointer_interfaces || result.check_megamap_clicks ||
        result.check_single_player_offers || result.check_radar_orders ||
        result.check_touch_controls || result.check_pad_controls ||
        result.check_running_while_inactive || result.check_director_view ||
        result.check_director_render || result.check_interpolation || result.check_unit_playout ||
        result.check_paused_save || result.check_saved_brains || result.check_simulation_hash ||
        result.check_zoom_memory || result.check_map_packs || result.check_map_browser ||
        result.check_mutator_button || result.check_mutator_room || result.check_brain_room ||
        result.check_mutator_picker || result.check_brain_picker ||
        result.check_brain_seat_buttons || result.check_brain_campaign ||
        result.check_mutator_prompts || result.check_map_pack_lists ||
        result.check_mod_map_settings || result.check_side_orders || result.check_skirmish_sides ||
        result.check_mod_limits || result.check_click_snap;
    // A capture and a showcase need the application's own loop and window,
    // which checks and benchmarks do not run.
    const bool check_run = result.fixed_clock || result.check_navigation ||
                           result.check_zoom_memory || result.check_multiplayer_menu ||
                           result.check_briefing_narration || result.check_key_timing ||
                           result.check_pointer_keys || result.benchmark_frames;
#if OA_PROCESS_SPAWNING == 0
    // The capture and the director's encoder run the ffmpeg program, which a
    // build without OA_PROCESS_SPAWNING cannot start. A render with encoding
    // off (OA_DIRECTOR_ENCODER=none, director_output.hpp) starts nothing.
    if (!result.capture_video.empty())
        throw std::runtime_error(
            "--capture-video needs the ffmpeg program, and this build starts no "
            "other programs"
        );
    if (!result.render_script.empty()) {
        const auto encoder = oa::platform::environment_value("OA_DIRECTOR_ENCODER");
        if (!encoder || *encoder != "none")
            throw std::runtime_error(
                "--render-script encodes its video with the ffmpeg program, and "
                "this build starts no other programs; set "
                "OA_DIRECTOR_ENCODER=none to render without it"
            );
    }
#endif
    if (!result.capture_video.empty() && check_run)
        throw std::runtime_error(
            "--capture-video captures the game or a --showcase, not a check or benchmark"
        );
    if (result.showcase != Showcase::none && check_run)
        throw std::runtime_error("--showcase plays in a window; it is not a check or benchmark");
    // A showcase plays its mission from the same random state on every run.
    if (result.showcase != Showcase::none && !result.seed)
        result.seed = kFixedRandomSeed;
    result.unattended = result.fixed_clock || result.check_navigation ||
                        result.check_multiplayer_menu || result.check_briefing_narration ||
                        result.check_key_timing || result.check_pointer_keys ||
                        result.benchmark_frames || result.frame_limit || !result.snapshot.empty() ||
                        result.showcase != Showcase::none;
    if ((extension_effects & option_effect::unattended) != 0)
        result.unattended = true;
    if (result.check_game_files)
        result.unattended = true;
    if (!result.check_content_copy.empty())
        result.unattended = true;
    if (!result.check_installer.empty())
        result.unattended = true;
    if (!result.display_modes.empty() && !result.unattended)
        throw std::runtime_error(
            "--display-modes is accepted only with checks, snapshots, benchmarks and frame "
            "limits"
        );
    // -df starts a player's run full screen and -d, with any other suffix,
    // in a window. Without either, a player's run on Windows starts full
    // screen. Unattended runs and video captures keep a window.
    namespace display_option = oa::app::command_line::display_option;
#ifdef _WIN32
    constexpr bool platform_full_screen = true;
#else
    constexpr bool platform_full_screen = false;
#endif
    const int32_t display = result.launch.display_option;
    result.start_full_screen =
        (display == display_option::f_suffix || (display == 0 && platform_full_screen)) &&
        !result.unattended && result.capture_video.empty();
    // 3.1c never plays its intro in the window -d asks for.
    if (display == display_option::other)
        result.skip_intro = true;
    if (result.print_profile && result.mod_file.empty() && result.mod_dir.empty() &&
        result.game_dir.empty())
        throw std::runtime_error(
            "--print-profile needs --mod FILE, --mod-dir PATH or --game-dir PATH"
        );
    if (result.accept_unimplemented_hacks && result.mod_file.empty() && result.mod_dir.empty() &&
        result.game_dir.empty() && !result.print_profile)
        throw std::runtime_error(
            "--accept-unimplemented-hacks needs a mod: --mod, --mod-dir, --game-dir or "
            "--print-profile"
        );
    if (result.base_game && !result.mod_dir.empty())
        throw std::runtime_error("--base-game and --mod-dir cannot be used together");
    if (result.choose_game_dir && !result.game_dir.empty())
        throw std::runtime_error("--choose-game-dir and --game-dir cannot be used together");
    // A file is opened only once the player answers its questions, which a
    // run nobody watches never shows; --check-mod-install answers them itself.
    if (!result.open_files.empty() && result.headless_check)
        throw std::runtime_error(
            "a file to open (.oamod, .oalang, .oamap, .oamut, .oabrain or .oareg), after --open or "
            "on its "
            "own, "
            "needs the game's window, which asks before it installs"
        );
    if (!result.open_files.empty() && result.unattended && !result.check_mod_install)
        throw std::runtime_error(
            "a file to open (.oamod, .oalang, .oamap, .oamut, .oabrain or .oareg), after --open or "
            "on its "
            "own, "
            "is asked about before it installs, which headless, check, benchmark, --frames and "
            "--snapshot runs never show"
        );
    // A file opened with the game goes to the main menu at once.
    if (!result.open_files.empty())
        result.skip_intro = true;
    if (result.choose_game_dir && result.unattended)
        throw std::runtime_error(
            "--choose-game-dir opens a dialog, which headless, check, benchmark, --frames and "
            "--snapshot runs never show"
        );
    // A launch's content was checked as it was taken (adopt_launch_run).
    if (!result.content.empty()) {
        const oa::data::content_bundles::ContentChoice named =
            oa::data::content_bundles::parse_content_argument(result.content);
        if (!named.error.empty())
            throw std::runtime_error(named.error);
    }
    return result;
}

} // namespace oa::app

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Command-line parsing for oa-game.
#include "oa/app/app.hpp"
#include "oa/app/extension.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/hook_call.hpp"
#include "oa/app/mod_install.hpp"
#include "oa/platform/display_modes.hpp"
#include "oa/platform/job_pool.hpp"
#include "oa/platform/system.hpp"
#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

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

// The arguments after a long option, as OptionValues hands them out.
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
        {options.check_user_folder, "--check-user-folder"},
        {options.check_mod_switch, "--check-mod-switch"},
        {options.check_mod_warning, "--check-mod-warning"},
        {options.check_mod_install, "--check-mod-install"},
        {!options.install_mods.empty(), "--install-mod"},
        {options.check_renderer_ladder, "--check-renderer-ladder"},
        {options.check_briefing_narration, "--check-briefing-narration"},
        {options.check_match_layers, "--check-match-layers"},
        {options.check_render_tiers, "--check-render-tiers"},
        {options.check_match_orders, "--check-match-orders"},
        {options.check_factory_orders, "--check-factory-orders"},
        {options.check_unit_speech, "--check-unit-speech"},
        {options.check_download_builds, "--check-download-builds"},
        {options.check_stockpile_builds, "--check-stockpile-builds"},
        {options.check_unit_page_memory, "--check-unit-page-memory"},
        {options.check_side_column, "--check-side-column"},
        {options.check_match_bars, "--check-match-bars"},
        {!options.check_unit_pages.empty(), "--check-unit-pages"},
        {options.check_kill_board, "--check-kill-board"},
        {options.check_paused_save, "--check-paused-save"},
        {options.check_simulation_hash, "--check-simulation-hash"},
        {!options.check_unit_language.empty(), "--check-unit-language"},
        {options.check_language_switch, "--check-language-switch"},
        {options.check_patrol_reclaim, "--check-patrol-reclaim"},
        {options.check_reclaim_cursor, "--check-reclaim-cursor"},
        {options.check_build_preview, "--check-build-preview"},
        {options.check_pointer_interfaces, "--check-pointer-interfaces"},
        {options.check_megamap_clicks, "--check-megamap-clicks"},
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
    throw std::runtime_error(
        "--showcase knows two showcases: arm-first-mission and skirmish-battle"
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
    if (!generate && !render) {
        if (!options.director_output.empty())
            throw std::runtime_error("--output needs --generate-script or --render-script");
        if (options.director_chunks)
            throw std::runtime_error("--chunks needs --render-script");
        if (!options.director_stills.empty())
            throw std::runtime_error("--stills needs --render-script");
        return;
    }
    if (generate && render)
        throw std::runtime_error("--generate-script and --render-script cannot be used together");
    if (generate && options.director_chunks)
        throw std::runtime_error("--chunks needs --render-script");
    if (generate && !options.director_stills.empty())
        throw std::runtime_error("--stills needs --render-script");
    if (render && options.window_resolution)
        throw std::runtime_error(
            "--render-script takes the frame size from the script, not from --resolution"
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
        {options.check_factory_orders, "--check-factory-orders"},
        {options.check_unit_speech, "--check-unit-speech"},
        {options.check_download_builds, "--check-download-builds"},
        {options.check_stockpile_builds, "--check-stockpile-builds"},
        {options.check_unit_page_memory, "--check-unit-page-memory"},
        {options.check_side_column, "--check-side-column"},
        {options.check_match_bars, "--check-match-bars"},
        {!options.check_unit_pages.empty(), "--check-unit-pages"},
        {options.check_kill_board, "--check-kill-board"},
        {options.check_paused_save, "--check-paused-save"},
        {options.check_simulation_hash, "--check-simulation-hash"},
        {!options.check_unit_language.empty(), "--check-unit-language"},
        {options.check_language_switch, "--check-language-switch"},
        {options.check_patrol_reclaim, "--check-patrol-reclaim"},
        {options.check_reclaim_cursor, "--check-reclaim-cursor"},
        {options.check_build_preview, "--check-build-preview"},
        {options.check_pointer_interfaces, "--check-pointer-interfaces"},
        {options.check_megamap_clicks, "--check-megamap-clicks"},
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
        {options.check_user_folder, "--check-user-folder"},
        {options.check_mod_switch, "--check-mod-switch"},
        {options.check_mod_warning, "--check-mod-warning"},
        {options.check_mod_install, "--check-mod-install"},
        {!options.install_mods.empty(), "--install-mod"},
        {options.check_renderer_ladder, "--check-renderer-ladder"},
    };
    for (const auto& [given, name] : refused)
        if (given)
            throw std::runtime_error(std::string(run) + " cannot be used with " + name);
    options.headless_check = true;
    options.skip_intro = true;
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
    };
    const char* const usage = "--render-fault takes create, present, reset, lost, stall, float, "
                              "memory, card or full-memory, optionally @FRAME";
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

/// Returns a mod package's path as the command line names it, made absolute.
///
/// @param text the path, UTF-8
/// @return the absolute path
[[nodiscard]] fs::path mod_package_path(std::string_view text) {
    const fs::path file = path_from_utf8(text);
    std::error_code error;
    const fs::path whole = fs::absolute(file, error);
    return error ? file : whole;
}

/// Tells whether a bare argument names a mod package by its extension, any case.
///
/// @param argument the argument
/// @return true for a .oamod file
[[nodiscard]] bool names_mod_package(std::string_view argument) {
    return mod_install::names_mod_package(path_from_utf8(argument));
}

} // namespace

[[nodiscard]] Options parse_options(int argc, char** argv, const Extension& extension) {
    Options result;
    std::string joined_line;
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
        auto value = [&](std::string_view name) -> std::string_view {
            ArgumentCursor cursor{argc, argv, &index, name};
            return next_value(&cursor);
        };
        if (argument == "--game-dir")
            result.game_dir = path_from_utf8(value(argument));
        else if (argument == "--choose-game-dir")
            result.choose_game_dir = true;
        else if (argument == "--archive")
            result.archives.push_back(path_from_utf8(value(argument)));
        else if (argument == "--mod")
            result.mod_file = path_from_utf8(value(argument));
        else if (argument == "--mod-dir")
            result.mod_dir = path_from_utf8(value(argument));
        else if (argument == "--base-game")
            result.base_game = true;
        else if (argument == "--print-profile")
            result.print_profile = true;
        else if (argument == "--accept-unimplemented-hacks")
            result.accept_unimplemented_hacks = true;
        else if (argument == "--snapshot")
            result.snapshot = path_from_utf8(value(argument));
        else if (argument == "--preferences-file")
            result.preferences_file = path_from_utf8(value(argument));
        else if (argument == "--data-dir")
            result.data_dir = path_from_utf8(value(argument));
        else if (argument == "--user-folder")
            result.user_folder = path_from_utf8(value(argument));
        else if (argument == "--log-dir")
            result.log_dir = path_from_utf8(value(argument));
        else if (argument == "--frames")
            result.frame_limit = parse_count(value(argument));
        else if (argument == "--benchmark")
            result.benchmark_frames = parse_count(value(argument));
        else if (argument == "--match-ticks")
            result.match_ticks = parse_count(value(argument));
        else if (argument == "--max-fps") {
            result.max_frames_per_second = parse_frame_rate(
                value(argument),
                kLowestMaxFramesPerSecond,
                true,
                "--max-fps expects 0 for no limit, or frames a second from 30 through 1000"
            );
            result.max_frames_per_second_given = true;
        } else if (argument == "--hardware-acceleration") {
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
        } else if (argument == "--force-capable")
            result.force_capable = true;
        else if (argument == "--display-modes") {
            result.display_modes = std::string(value(argument));
            if (!oa::platform::display_modes::report_from_text(result.display_modes))
                throw std::runtime_error(
                    "--display-modes expects WIDTHxHEIGHT[@RATE][/DENSITY] modes separated "
                    "by commas, or none"
                );
        } else if (argument == "--native-density")
            result.native_density = true;
        else if (argument == "--frame-rate")
            result.frame_rate = parse_frame_rate(
                value(argument),
                1,
                false,
                "--frame-rate expects frames a second from 1 through 1000"
            );
        else if (argument == "--frame-log")
            result.frame_log = path_from_utf8(value(argument));
        else if (argument == "--scroll-camera")
            result.scroll_camera = true;
        else if (argument == "--march")
            result.march = true;
        else if (argument == "--follow")
            result.follow = true;
        else if (argument == "--frame-clock")
            result.frame_clock_ms = parse_frame_clock(value(argument));
        else if (argument == "--campaign")
            result.campaign = value(argument);
        else if (argument == "--mission")
            result.campaign_mission = parse_count(value(argument));
        else if (argument == "--past-outcome")
            result.campaign_past_outcome = true;
        else if (argument == "--restart-at")
            result.campaign_restart_tick = parse_count(value(argument));
        else if (argument == "--combat")
            result.combat_units = parse_count(value(argument));
        else if (argument == "--busy-combat")
            result.busy_combat = true;
        else if (argument == "--stage")
            result.stage_file = path_from_utf8(value(argument));
        else if (argument == "--save-after")
            result.save_after = parse_count(value(argument));
        else if (argument == "--save-file")
            result.save_file = path_from_utf8(value(argument));
        else if (argument == "--load")
            result.load_file = path_from_utf8(value(argument));
        else if (argument == "--give-orders")
            result.give_orders = true;
        else if (argument == "--zoom") {
            const auto text = std::string(value(argument));
            char* end = nullptr;
            result.match_zoom = std::strtof(text.c_str(), &end);
            if (end == text.c_str() || *end != '\0' || !(result.match_zoom > 0.0F))
                throw std::runtime_error("--zoom expects a positive number");
        } else if (argument == "--reclaim-check")
            result.reclaim_check = true;
        else if (argument == "--resolution") {
            const auto text = std::string(value(argument));
            const auto separator = text.find('x');
            if (separator == std::string::npos)
                throw std::runtime_error("--resolution expects WIDTHxHEIGHT");
            result.match_width = static_cast<int>(parse_count(text.substr(0, separator)));
            result.match_height = static_cast<int>(parse_count(text.substr(separator + 1)));
            if (result.match_width <= 0 || result.match_height <= 0)
                throw std::runtime_error("--resolution expects a width and height above 0");
            result.window_resolution = true;
        } else if (argument == "--camera") {
            const auto text = std::string(value(argument));
            const auto separator = text.find(',');
            if (separator == std::string::npos)
                throw std::runtime_error("--camera expects X,Z");
            result.camera = {
                static_cast<int>(parse_count(text.substr(0, separator))),
                static_cast<int>(parse_count(text.substr(separator + 1)))
            };
        } else if (argument == "--skip-intro")
            result.skip_intro = true;
        else if (argument == "--headless-check")
            result.headless_check = true;
        else if (argument == "--mute")
            result.mute = true;
        else if (argument == "--check-navigation") {
            result.check_navigation = true;
            // A group's name may follow; any other word is read as before.
            if (index + 1 < argc)
                if (const auto group = navigation_group_named(argv[index + 1])) {
                    result.navigation_group = *group;
                    ++index;
                }
        } else if (argument == "--check-match-dialogs")
            result.check_match_dialogs = true;
        else if (argument == "--check-load-save")
            result.check_load_save = true;
        else if (argument == "--check-frontend-controls")
            result.check_frontend_controls = true;
        else if (argument == "--check-scroll-bars")
            result.check_scroll_bars = true;
        else if (argument == "--check-engine-settings")
            result.check_engine_settings = true;
        else if (argument == "--check-user-folder")
            result.check_user_folder = true;
        else if (argument == "--check-mod-switch")
            result.check_mod_switch = true;
        else if (argument == "--check-mod-warning")
            result.check_mod_warning = true;
        else if (argument == "--check-mod-install")
            result.check_mod_install = true;
        else if (argument == "--install-mod")
            result.install_mods.push_back(mod_package_path(value(argument)));
        else if (argument == "--check-renderer-ladder")
            result.check_renderer_ladder = true;
        else if (argument == "--render-fault")
            result.render_fault = parse_render_fault(value(argument));
        else if (argument == "--check-briefing-narration")
            result.check_briefing_narration = true;
        else if (argument == "--check-match-layers")
            result.check_match_layers = true;
        else if (argument == "--check-render-tiers")
            result.check_render_tiers = true;
        else if (argument == "--check-match-orders")
            result.check_match_orders = true;
        else if (argument == "--check-factory-orders")
            result.check_factory_orders = true;
        else if (argument == "--check-unit-speech")
            result.check_unit_speech = true;
        else if (argument == "--check-download-builds")
            result.check_download_builds = true;
        else if (argument == "--check-stockpile-builds")
            result.check_stockpile_builds = true;
        else if (argument == "--check-unit-page-memory")
            result.check_unit_page_memory = true;
        else if (argument == "--check-side-column")
            result.check_side_column = true;
        else if (argument == "--check-match-bars")
            result.check_match_bars = true;
        else if (argument == "--check-unit-pages")
            result.check_unit_pages = value(argument);
        else if (argument == "--check-kill-board")
            result.check_kill_board = true;
        else if (argument == "--check-paused-save")
            result.check_paused_save = true;
        else if (argument == "--check-simulation-hash")
            result.check_simulation_hash = true;
        else if (argument == "--check-unit-language")
            result.check_unit_language = value(argument);
        else if (argument == "--check-language-switch")
            result.check_language_switch = true;
        else if (argument == "--check-patrol-reclaim")
            result.check_patrol_reclaim = true;
        else if (argument == "--check-reclaim-cursor")
            result.check_reclaim_cursor = true;
        else if (argument == "--check-build-preview")
            result.check_build_preview = true;
        else if (argument == "--check-pointer-interfaces")
            result.check_pointer_interfaces = true;
        else if (argument == "--check-megamap-clicks")
            result.check_megamap_clicks = true;
        else if (argument == "--check-radar-orders")
            result.check_radar_orders = true;
        else if (argument == "--check-touch-controls")
            result.check_touch_controls = true;
        else if (argument == "--check-pad-controls")
            result.check_pad_controls = true;
        else if (argument == "--check-running-while-inactive")
            result.check_running_while_inactive = true;
        else if (argument == "--touch-controls")
            result.touch_controls = true;
        else if (argument == "--check-game-files")
            result.check_game_files = true;
        else if (argument == "--no-game-files-screen")
            result.no_game_files_screen = true;
        else if (argument == "--game-files-route")
            result.game_files_route = parse_game_files_route(value(argument));
        else if (argument == "--game-files-expect")
            result.game_files_expect = parse_game_files_expect(value(argument));
        else if (argument == "--game-files-source")
            result.game_files_source = path_from_utf8(value(argument));
        else if (argument == "--game-files-free-bytes")
            result.game_files_free_bytes = parse_game_files_bytes(value(argument), argument, false);
        else if (argument == "--game-files-copy-rate")
            result.game_files_copy_rate = parse_game_files_bytes(value(argument), argument, true);
        else if (argument == "--game-files-stop-after")
            result.game_files_stop_after = parse_game_files_bytes(value(argument), argument, true);
        else if (argument == "--check-multiplayer-menu")
            result.check_multiplayer_menu = true;
        else if (argument == "--check-director-view")
            result.check_director_view = true;
        else if (argument == "--check-director-render")
            result.check_director_render = true;
        else if (argument == "--check-interpolation")
            result.check_interpolation = true;
        else if (argument == "--check-unit-playout")
            result.check_unit_playout = true;
        else if (argument == "--generate-script")
            result.generate_script = path_from_utf8(value(argument));
        else if (argument == "--render-script")
            result.render_script = path_from_utf8(value(argument));
        else if (argument == "--output")
            result.director_output = path_from_utf8(value(argument));
        else if (argument == "--chunks")
            result.director_chunks = parse_chunks(value(argument));
        else if (argument == "--stills")
            result.director_stills = parse_stills(value(argument));
        else if (argument == "--trace-input")
            result.trace_input = true;
        else if (argument == "--trace-lookups")
            result.trace_lookups = path_from_utf8(value(argument));
        else if (argument == "--debug-order-lines")
            result.debug_order_lines = true;
        else if (argument == "--trace-digest")
            result.trace_digest = path_from_utf8(value(argument));
        else if (argument == "--trace-units")
            result.trace_units = path_from_utf8(value(argument));
        else if (argument == "--seed")
            result.seed = parse_seed(value(argument));
        else if (argument == "--draw-threads")
            result.draw_threads = parse_draw_threads(value(argument), argument);
        else if (argument == "--capture-video")
            result.capture_video = path_from_utf8(value(argument));
        else if (argument == "--showcase")
            result.showcase = parse_showcase(value(argument));
        // A bare -h stays the help flag, so the game's "-h NAME" (name as
        // the next argument) must be written -hNAME here.
        else if (argument == "--help" || argument == "-h") {
            std::cout
                << "usage: open-annihilation [--game-dir PATH | --choose-game-dir] "
                   "[--archive PATH]... "
                   "[--mod FILE] [--mod-dir PATH | --base-game] [--print-profile] "
                   "[--accept-unimplemented-hacks] "
                   "[--skip-intro] [--frames N] [--headless-check] "
                   "[--snapshot PATH.ppm] [--preferences-file PATH] [--data-dir PATH] "
                   "[--user-folder PATH] [--log-dir PATH] "
                   "[--mute] "
                   "[--check-navigation [screens|orders|outcomes|zoom|zoom-1366x768|"
                   "zoom-1920x1080|zoom-2560x1440|campaign]] "
                   "[--check-match-dialogs] [--check-match-layers] "
                   "[--check-render-tiers [--force-capable]] "
                   "[--check-match-orders] [--check-factory-orders] [--check-unit-speech] "
                   "[--check-download-builds] [--check-stockpile-builds] "
                   "[--check-unit-page-memory] [--check-side-column] [--check-match-bars] "
                   "[--check-unit-pages whole|scaled:TYPE,...] "
                   "[--check-kill-board] [--check-simulation-hash] "
                   "[--check-unit-language TAG] "
                   "[--check-language-switch] "
                   "[--check-patrol-reclaim] [--check-reclaim-cursor] [--check-build-preview] "
                   "[--check-megamap-clicks] [--check-radar-orders] "
                   "[--check-pointer-interfaces] [--check-pad-controls] [--check-touch-controls] "
                   "[--check-game-files] "
                   "[--check-multiplayer-menu] "
                   "[--check-load-save] [--check-frontend-controls] "
                   "[--check-scroll-bars] [--check-engine-settings [--force-capable]] "
                   "[--check-user-folder] [--check-mod-switch] [--check-mod-warning] "
                   "[--check-mod-install] "
                   "[--check-renderer-ladder [--render-fault POINT[@FRAME]]] "
                   "[--check-briefing-narration] [--check-director-view] "
                   "[--check-director-render] [--check-interpolation] "
                   "[--check-unit-playout] [--check-running-while-inactive] "
                   "[--trace-input] "
                << extension_text(extension, ExtensionText::usage_checks, "")
                << "[--debug-order-lines] "
                   "[--max-fps N] "
                   "[--hardware-acceleration[=off|basic|full] | --no-hardware-acceleration] "
                   "[--native-density] [--display-modes MODES] "
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
                   "[--reclaim-check] "
                   "[--camera X,Z] "
                << extension_text(extension, ExtensionText::usage_runs, "")
                << "[--save-after TICK] "
                   "[--save-file PATH.sav] [--load PATH.sav] [--give-orders] [--seed N] "
                   "[--trace-digest FILE] [--trace-units FILE] [--trace-lookups FILE] "
                   "[--draw-threads N] "
                   "[--capture-video PATH.mp4] [--showcase arm-first-mission|skirmish-battle] "
                   "[--install-mod FILE.oamod]... "
                   "[--generate-script RECORDING [--output PATH.oascript|PATH.oamovie] "
                   "[--resolution WxH]] "
                   "[--render-script PATH.oascript|PATH.oamovie [--output DIR] "
                   "[--chunks A-B] [--stills F,F...]] "
                   "[game switches such as "
                << extension_text(extension, ExtensionText::usage_switches, "")
                << "-d -s] "
                   "[LANGUAGE]\n"
                   "--generate-script plans a director script from a recording an "
                   "extension of this build replays;\n"
                   "--render-script renders a director script, or a bundle of one and "
                   "its recording, to video (docs/director.md).\n"
                << extension_text(extension, ExtensionText::usage_note, "");
            std::exit(0);
        } else if (argument.starts_with("--")) {
            ArgumentCursor cursor{argc, argv, &index, argument};
            const OptionValues values{&cursor, next_value};
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
        } else if (names_mod_package(argument)) {
            // A .oamod file opened with the game, or dropped on it.
            result.install_mods.push_back(mod_package_path(argument));
        } else {
            if (!joined_line.empty())
                joined_line += ' ';
            joined_line += argument;
        }
    }
    GuardedSwitches switches;
    switches.handler = call_hook_or_raise<&Extension::switch_handler>(extension);
    switches.guard = {&switches, take_guarded_switch, reset_guarded_switches};
    const auto parsed = oa::app::command_line::parse(
        joined_line.c_str(), result.launch, switches.handler != nullptr ? &switches.guard : nullptr
    );
    // A handler that threw stops the start with its message, as a hook that
    // raises its error does.
    if (switches.error.caught)
        throw std::runtime_error(switches.error.message);
    switch (parsed) {
    case oa::app::command_line::Status::run:
        break;
    case oa::app::command_line::Status::register_application:
        throw std::runtime_error(extension_text(
            extension,
            ExtensionText::register_switch,
            "-r registers the game for multiplayer, which this game does not need"
        ));
    case oa::app::command_line::Status::line_too_long:
        throw std::runtime_error("the game switches exceed the command-line limit");
    case oa::app::command_line::Status::argument_too_long:
        throw std::runtime_error("the language argument is too long");
    case oa::app::command_line::Status::unavailable_switch:
        throw std::runtime_error(
            std::string("-") + result.launch.unavailable_switch + " is not handled by this build"
        );
    }
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
    call_hook_or_raise<&Extension::check_options>(extension);
    if (const auto env = oa::platform::environment_value("OA_DEBUG_ORDER_LINES");
        env && !env->empty())
        result.debug_order_lines = true;
    if (const auto env = oa::platform::environment_value("OA_DRAW_THREADS");
        !result.draw_threads && env && !env->empty())
        result.draw_threads = parse_draw_threads(*env, "OA_DRAW_THREADS");
    // The director view check runs headless, where SDL is never started.
    if (result.check_director_view || result.check_director_render || result.check_interpolation ||
        result.check_unit_playout) {
        result.headless_check = true;
        result.skip_intro = true;
    }
    check_director_options(result);
    if (result.busy_combat && result.combat_units == 0)
        throw std::runtime_error("--busy-combat needs --combat");
    if (!result.stage_file.empty() &&
        (!result.match_ticks || !result.load_file.empty() || !result.campaign.empty()))
        throw std::runtime_error("--stage stages a headless skirmish of --match-ticks ticks");
    if (result.check_render_tiers && result.headless_check)
        throw std::runtime_error(
            "--check-render-tiers draws in a window and cannot be used with --headless-check"
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
    check_game_files_options(result);
    result.fixed_clock =
        result.headless_check || result.check_match_layers || result.check_render_tiers ||
        result.check_match_dialogs || result.check_load_save || result.check_frontend_controls ||
        result.check_scroll_bars || result.check_engine_settings || result.check_user_folder ||
        result.check_mod_switch || result.check_mod_warning || result.check_mod_install ||
        result.check_renderer_ladder || result.check_match_orders || result.check_factory_orders ||
        result.check_unit_speech || result.check_download_builds || result.check_stockpile_builds ||
        result.check_unit_page_memory || result.check_side_column || result.check_match_bars ||
        !result.check_unit_pages.empty() || result.check_kill_board ||
        !result.check_unit_language.empty() || result.check_language_switch ||
        result.check_patrol_reclaim || result.check_reclaim_cursor || result.check_build_preview ||
        result.check_pointer_interfaces || result.check_megamap_clicks ||
        result.check_radar_orders || result.check_touch_controls || result.check_pad_controls ||
        result.check_running_while_inactive || result.check_director_view ||
        result.check_director_render || result.check_interpolation || result.check_unit_playout ||
        result.check_paused_save || result.check_simulation_hash;
    // A capture and a showcase need the application's own loop and window,
    // which checks and benchmarks do not run.
    const bool check_run = result.fixed_clock || result.check_navigation ||
                           result.check_multiplayer_menu || result.check_briefing_narration ||
                           result.benchmark_frames;
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
                        result.benchmark_frames || result.frame_limit || !result.snapshot.empty() ||
                        result.showcase != Showcase::none;
    if ((extension_effects & option_effect::unattended) != 0)
        result.unattended = true;
    if (result.check_game_files)
        result.unattended = true;
    if (!result.display_modes.empty() && !result.unattended)
        throw std::runtime_error(
            "--display-modes is accepted only with checks, snapshots, benchmarks and frame "
            "limits"
        );
#ifdef _WIN32
    // On Windows a player's run starts full screen; -d, with any suffix, keeps
    // a window, and so do unattended runs and video captures.
    result.start_full_screen =
        result.launch.display_option == 0 && !result.unattended && result.capture_video.empty();
#endif
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
    // A package is installed only once the player answers its questions,
    // which a run nobody watches never shows; --check-mod-install answers
    // them itself.
    if (!result.install_mods.empty() && result.headless_check)
        throw std::runtime_error(
            "a .oamod file to install, after --install-mod or on its own, needs the game's "
            "window, which asks before it installs"
        );
    if (!result.install_mods.empty() && result.unattended && !result.check_mod_install)
        throw std::runtime_error(
            "a .oamod file to install, after --install-mod or on its own, is asked about "
            "before it installs, which headless, check, benchmark, --frames and --snapshot "
            "runs never show"
        );
    // A package opened with the game goes to the main menu at once.
    if (!result.install_mods.empty())
        result.skip_intro = true;
    if (result.choose_game_dir && result.unattended)
        throw std::runtime_error(
            "--choose-game-dir opens a dialog, which headless, check, benchmark, --frames and "
            "--snapshot runs never show"
        );
    return result;
}

} // namespace oa::app

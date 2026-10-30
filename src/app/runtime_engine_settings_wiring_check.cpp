// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-engine-settings, the settings' part: each setting takes effect,
// in step with its console command and the command line.

#include "engine_settings_state.hpp"
#include "render_host.hpp"
#include "render_run.hpp"

#include "oa/app/acceleration_status.hpp"
#include "oa/app/runtime.hpp"
#include "oa/sim/messages.hpp"
#include "oa/ui/frontend_dialogs.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>

namespace oa::app {

namespace {

namespace settings = oa::ui::engine_settings;

/// A file the check names as the preferences file's folder, so that saving fails.
constexpr std::string_view kBlockingFileName = "engine-settings-not-a-folder";

/// The zoom the check gives the battlefield before the Mouse wheel zoom setting turns off.
constexpr float kCheckZoom = 2.0F;

/// Twice the base path credit: Pathfinding cycles at 2x.
constexpr int32_t kDoubledPathNodes = settings::base_path_search_nodes * 2;

/// A unit limit above 3.1c's own highest.
constexpr uint16_t kCheckUnitLimit = 500;

/// A maximum frame rate below the default.
constexpr uint32_t kCheckFrameRate = 60;

/// The frame rate the check gives --max-fps.
constexpr uint32_t kCommandLineFrameRate = 90;

/// Throws when a condition fails.
///
/// @param ok the condition
/// @param what what failed
void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("engine settings check: " + std::string(what));
}

/// Returns an Escape press.
///
/// @param repeat the press is a held key's repeat
/// @return the event
SDL_Event escape_press(bool repeat) {
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.key = SDLK_ESCAPE;
    event.key.scancode = SDL_SCANCODE_ESCAPE;
    event.key.down = true;
    event.key.repeat = repeat;
    return event;
}

/// Escape let go.
SDL_Event escape_release() {
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_UP;
    event.key.key = SDLK_ESCAPE;
    event.key.scancode = SDL_SCANCODE_ESCAPE;
    return event;
}

} // namespace

void Runtime::check_engine_settings_wiring() {
    auto& state = engine_settings_state();
    if (state.dialog)
        (void)take_engine_settings_action(settings::DialogAction::cancelled);
    bool running = true;
    const auto send = [&](SDL_Event event) {
        handle_sdl_event(event, running);
        require(running, "an event ended the run");
    };
    // Escape goes in as the event loop takes a key (dispatch_event), which
    // feeds the action dispatcher each down and its up, as a keyboard sends
    // them.
    const auto send_key = [&](SDL_Event event) {
        dispatch_event(event, running);
        require(running, "an event ended the run");
    };
    const auto tap_escape = [&] {
        send_key(escape_press(false));
        send_key(escape_release());
    };

    // With --preferences-file, every default is the game's own behaviour on
    // every platform, and the installation's options are not read.
    const auto defaults = settings::default_settings(EngineSettingsState::inputs(*this));
    require(!defaults.escape_opens_menu, "Escape opens the menu by default with a named file");
    require(defaults.unit_limit == settings::default_unit_limit, "the unit limit's default moved");
    require(state.installation_ini.empty(), "the installation's totala.ini was read");

    // The main menu: a held Escape's repeats do not quit.
    if (screen_ == Screen::main_menu && !frame_owned_by_package()) {
        require(
            frontend_game().max_units_setting == engine_settings().unit_limit,
            "the next game's unit limit is not the setting's"
        );
        send(escape_press(true));
    }

    if (screen_ != Screen::match || !match_)
        start_benchmark_skirmish();
    if (match_paused_)
        resume_match_pause();
    auto chosen = engine_settings();
    const auto kept = chosen;

    // The locks a game played alone puts on the settings.
    auto locks = engine_settings_locks();
    require(
        locks.path_search == settings::Lock::in_game &&
            locks.unit_limit == settings::Lock::in_game && !locks.shared_game,
        "a game played alone does not lock Pathfinding cycles and Unit limit"
    );

    // Maximum frame rate: at once, unless --max-fps holds it for the run.
    if (!options_.max_frames_per_second_given) {
        chosen.max_frame_rate = kCheckFrameRate;
        apply_engine_settings(chosen);
        require(
            options_.max_frames_per_second == kCheckFrameRate &&
                frame_stats_notes().max_frames_per_second == kCheckFrameRate,
            "the maximum frame rate did not take effect"
        );
        // The lowest, a frame a tick, reaches the loop as the others do.
        chosen.max_frame_rate = settings::lowest_frame_rate;
        apply_engine_settings(chosen);
        require(
            options_.max_frames_per_second == kLowestMaxFramesPerSecond &&
                frame_stats_notes().max_frames_per_second == kLowestMaxFramesPerSecond,
            "the lowest maximum frame rate did not take effect"
        );
        options_.max_frames_per_second_given = true;
        options_.max_frames_per_second = kCommandLineFrameRate;
        chosen.max_frame_rate = settings::highest_frame_rate;
        apply_engine_settings(chosen);
        require(
            options_.max_frames_per_second == kCommandLineFrameRate,
            "the setting changed the frame rate --max-fps gave"
        );
        require(
            engine_settings_locks().max_frame_rate == settings::Lock::command_line,
            "--max-fps does not lock the maximum frame rate"
        );
        options_.max_frames_per_second_given = false;
        apply_engine_settings(chosen);
        require(
            options_.max_frames_per_second == settings::highest_frame_rate,
            "the maximum frame rate did not go back"
        );
    }

    // Enhanced anti-aliasing sets how finely units are drawn.
    for (const auto level : settings::anti_aliasing_levels) {
        chosen.anti_aliasing = level;
        apply_engine_settings(chosen);
        require(
            oa::present::model::supersampling_factor(unit_supersampling_) ==
                static_cast<uint32_t>(level),
            "enhanced anti-aliasing did not set the units' drawing"
        );
    }
    chosen.anti_aliasing = settings::AntiAliasing::off;
    apply_engine_settings(chosen);

    // Vertical sync: the renderer waits for the display while it is On,
    // read back from SDL, and stops when it is Off. While it is in effect the
    // loop keeps below the display's rate; Off, the rate is the setting's.
    // On SDL's software renderer it is out of reach unless --force-capable
    // lifts that, and then the renderer is never asked.
    const auto renderer_waits = [this] {
        int vsync = 0;
        return SDL_GetRenderVSync(sdl_.renderer, &vsync) && vsync == 1;
    };
    require(!renderer_waits(), "the renderer waits for the display with Vertical sync Off");
    const bool vertical_sync_reachable = !acceleration_report().vertical_sync_unavailable;
    require(
        vertical_sync_reachable == options_.force_capable,
        "Vertical sync is out of reach otherwise than on the software renderer"
    );
    chosen.vertical_sync = true;
    apply_engine_settings(chosen);
    require(
        renderer_waits() == vertical_sync_reachable &&
            vertical_sync_in_effect_ == vertical_sync_reachable,
        "Vertical sync On did not reach the renderer as its lock allows"
    );
    chosen.vertical_sync = false;
    apply_engine_settings(chosen);
    require(
        !renderer_waits() && !vertical_sync_in_effect_,
        "Vertical sync Off did not stop the renderer waiting"
    );
    {
        // Out of reach, On asks the renderer nothing.
        const bool kept_force = options_.force_capable;
        options_.force_capable = false;
        chosen.vertical_sync = true;
        apply_engine_settings(chosen);
        const bool waited = renderer_waits();
        const auto software_locks = engine_settings_locks();
        chosen.vertical_sync = false;
        apply_engine_settings(chosen);
        options_.force_capable = kept_force;
        require(!waited, "Vertical sync reached SDL's software renderer");
        require(
            software_locks.vertical_sync == settings::Lock::unavailable &&
                software_locks.hardware_acceleration == settings::Lock::unavailable,
            "SDL's software renderer under the dummy video driver does not lock both rows"
        );
    }

    // Hardware acceleration: a flag at any level locks it for the run and
    // says so; the processor draws whatever it says. Without a flag it is
    // locked under 2 GiB, and on SDL's software renderer or the
    // environment's driver unless --force-capable lifts them; a renderer
    // nothing has looked at leaves it unlocked.
    {
        const auto kept_flag = options_.hardware_acceleration;
        options_.hardware_acceleration = settings::HardwareAcceleration::off;
        const auto off = engine_settings_locks();
        const auto off_report = acceleration_report();
        options_.hardware_acceleration = settings::HardwareAcceleration::full;
        const auto on = engine_settings_locks();
        options_.hardware_acceleration = settings::HardwareAcceleration::basic;
        const auto basic = engine_settings_locks();
        options_.hardware_acceleration = kept_flag;
        require(
            off.hardware_acceleration == settings::Lock::command_line &&
                on.hardware_acceleration == settings::Lock::command_line &&
                basic.hardware_acceleration == settings::Lock::command_line,
            "an acceleration flag does not lock Hardware acceleration"
        );
        // Under 2 GiB the status says the machine needs more memory first,
        // whatever the flags.
        const auto facts = acceleration_facts();
        const bool memory = enough_memory_for_acceleration(facts.physical_memory);
        require(
            off_report.status.state == (memory ? settings::AccelerationState::off_by_command_line
                                               : settings::AccelerationState::needs_memory),
            "--no-hardware-acceleration does not say it turned acceleration off, or why "
            "not under 2 GiB"
        );
        const bool ruled_out = !memory || (!facts.force_capable &&
                                           (facts.software_renderer || facts.environment_driver));
        require(
            engine_settings_locks().hardware_acceleration ==
                (options_.hardware_acceleration ? settings::Lock::command_line
                 : ruled_out                    ? settings::Lock::unavailable
                                                : settings::Lock::none),
            "Hardware acceleration's lock did not go back"
        );
    }

    // Hardware acceleration through the dialog, where the graphics card may
    // be used: here SDL's software renderer, which --force-capable lets the
    // accelerated tier use from 2 GiB. Basic draws in the accelerated tier
    // at once, and so does Full, which the game cannot draw yet, its status
    // saying Basic is in use; Off draws in the standard tier at once. A
    // failure in the run keeps the standard tier, and the row within reach,
    // until the row is set to Off and back or Restore defaults is pressed:
    // a drop after a failed call, and a start-up function test that drew
    // wrongly, which then runs again.
    if (options_.force_capable && !options_.hardware_acceleration && render_run_ &&
        render_run_->host != nullptr &&
        enough_memory_for_acceleration(acceleration_facts().physical_memory)) {
        auto& host = *render_run_->host;
        auto& inputs = host.tier_inputs();
        const auto graphics = settings::page_settings(settings::Page::graphics);
        const auto row =
            std::find(graphics.begin(), graphics.end(), settings::Setting::hardware_acceleration);
        require(row != graphics.end(), "Hardware acceleration is not in the Graphics section");
        const int32_t row_control =
            settings::first_row_control + static_cast<int32_t>(row - graphics.begin());
        // Clicks the middle of the dialog's part that shows a control, with
        // a text where it is given, and does what the click asks, the
        // section a page down from its top, where Hardware acceleration's
        // row shows whole.
        const auto click = [&](int32_t control, std::string_view text) {
            auto* dialog = engine_settings_dialog();
            require(dialog != nullptr, "the dialog is not open");
            std::ignore = settings::dialog_key(*dialog, settings::DialogKey::home);
            std::ignore = settings::dialog_key(*dialog, settings::DialogKey::page_down);
            const auto parts = settings::dialog_layout(*dialog);
            const auto part = std::find_if(parts.begin(), parts.end(), [&](const auto& shown) {
                return shown.control == control && (text.empty() || shown.text == text);
            });
            require(part != parts.end(), "a control of the dialog does not show");
            const int32_t x = part->rect.x + part->rect.width / 2;
            const int32_t y = part->rect.y + part->rect.height / 2;
            std::ignore = settings::dialog_pointer_move(*dialog, x, y);
            std::ignore = settings::dialog_pointer_down(*dialog, x, y);
            std::ignore = take_engine_settings_action(settings::dialog_pointer_up(*dialog, x, y));
        };
        // Tells whether the dialog, its status brought up to date as the
        // host does each frame, lists a text in its view.
        const auto shows_dialog_text = [&](std::string_view text) {
            auto* dialog = engine_settings_dialog();
            require(dialog != nullptr, "the dialog is not open");
            std::ignore = settings::set_acceleration_status(*dialog, acceleration_report().status);
            const auto parts = settings::dialog_layout(*dialog);
            return std::any_of(parts.begin(), parts.end(), [&](const auto& part) {
                return part.text == text;
            });
        };
        const auto off_then_on = [&] {
            click(row_control, "Off");
            require(!accelerated_presentation(), "Off did not switch the accelerated tier off");
            click(row_control, "Basic");
        };
        require(!accelerated_presentation(), "the accelerated tier drew before it was asked for");
        state.last_page = settings::Page::graphics;
        std::ignore = open_engine_settings_dialog();
        click(row_control, "Basic");
        require(
            engine_settings().hardware_acceleration == settings::HardwareAcceleration::basic &&
                accelerated_presentation() &&
                inputs.function_test == render_policy::FunctionTest::passed,
            "Hardware acceleration Basic did not draw in the accelerated tier at once"
        );
        // In use, at whatever the rung this machine starts at says of it.
        const auto in_use = [](settings::AccelerationState state) {
            return state == settings::AccelerationState::in_use ||
                   state == settings::AccelerationState::in_use_no_smoothing ||
                   state == settings::AccelerationState::in_use_on_another_driver;
        };
        require(
            in_use(acceleration_report().status.state),
            "Hardware acceleration Basic does not say it is in use"
        );
        // Full draws on the card at once, and the status says so.
        click(row_control, "Full");
        require(
            engine_settings().hardware_acceleration == settings::HardwareAcceleration::full &&
                full_presentation(),
            "Hardware acceleration Full did not draw in the full tier at once"
        );
        require(
            acceleration_report().status.state == settings::AccelerationState::full_in_use &&
                shows_dialog_text("Full in use: the graphics card draws the view."),
            "Hardware acceleration Full does not say it is in use"
        );
        click(row_control, "Off");
        require(
            !accelerated_presentation(),
            "Hardware acceleration Off did not draw in the standard tier at once"
        );
        // A drop: the standard tier, the failure said, the row within reach.
        click(row_control, "Basic");
        drop_acceleration("the engine settings check drops it");
        update_render_tier();
        const auto dropped = acceleration_report();
        require(
            !accelerated_presentation() &&
                dropped.status.state == settings::AccelerationState::driver_failed &&
                !dropped.acceleration_unavailable,
            "a drop did not keep the standard tier with the row within reach"
        );
        off_then_on();
        require(accelerated_presentation(), "Off then On did not lift a drop");
        // A function test that draws wrongly: it runs again on Off then On,
        // fails and keeps the standard tier; drawn right, it passes.
        host.faults().function_test.pattern_nearest = true;
        inputs.function_test = render_policy::FunctionTest::failed;
        off_then_on();
        require(
            !accelerated_presentation() &&
                inputs.function_test == render_policy::FunctionTest::failed,
            "a function test that drew wrongly did not keep the standard tier"
        );
        host.faults().function_test.pattern_nearest = false;
        off_then_on();
        require(
            accelerated_presentation() &&
                inputs.function_test == render_policy::FunctionTest::passed,
            "Off then On did not run the function test again"
        );
        // Restore defaults retries too, here under --hardware-acceleration=basic,
        // whose lock keeps the row: the next frame draws accelerated again.
        click(settings::cancel_control, {});
        options_.hardware_acceleration = settings::HardwareAcceleration::basic;
        update_render_tier();
        drop_acceleration("the engine settings check drops it");
        update_render_tier();
        require(!accelerated_presentation(), "a drop did not keep the standard tier");
        std::ignore = open_engine_settings_dialog();
        click(settings::restore_control, {});
        update_render_tier();
        require(accelerated_presentation(), "Restore defaults did not lift a drop");
        click(settings::cancel_control, {});
        options_.hardware_acceleration.reset();
        update_render_tier();
        require(
            engine_settings_dialog() == nullptr &&
                engine_settings().hardware_acceleration == settings::HardwareAcceleration::off &&
                !accelerated_presentation(),
            "Cancel did not put Hardware acceleration back Off"
        );
    }

    // Show performance statistics is the +stats panel.
    chosen.frame_stats = true;
    apply_engine_settings(chosen);
    require(frame_stats_shown_, "Show performance statistics did not show the panel");
    chosen.frame_stats = false;
    apply_engine_settings(chosen);
    require(!frame_stats_shown_, "Show performance statistics did not hide the panel");

    // Select groups without Alt is 3.1c's SwitchAlt, in the match and in the
    // preferences, and saved under its own key.
    const auto switch_alt_on = [&] {
        return (match_->state().game.graphics_flags & init::preference_flags::switch_alt) != 0 &&
               (preferences_.graphics_flags & init::preference_flags::switch_alt) != 0;
    };
    auto opened = engine_settings();
    chosen.switch_alt = true;
    apply_engine_settings(chosen);
    require(
        switch_alt_on() && engine_settings().switch_alt, "SwitchAlt did not follow the setting"
    );
    require(!save_engine_settings(opened, chosen, false), "the settings were not saved");
    require(saved_general_number("SwitchAlt") == 1, "SwitchAlt was not saved");
    opened = chosen;
    chosen.switch_alt = false;
    apply_engine_settings(chosen);
    require(!save_engine_settings(opened, chosen, false), "the settings were not saved");
    require(
        !switch_alt_on() && saved_general_number("SwitchAlt") == 0,
        "SwitchAlt was not cleared and saved"
    );

    // Mouse wheel zoom off: the wheel does nothing and the battlefield eases
    // back to its own scale about its centre; Cancel brings the zoom back.
    chosen.wheel_zoom = true;
    apply_engine_settings(chosen);
    match_zoom_ = kCheckZoom;
    match_zoom_target_ = kCheckZoom;
    zoom_focus_ = {};
    {
        auto& dialog = open_engine_settings_dialog();
        dialog.chosen.wheel_zoom = false;
        (void)take_engine_settings_action(settings::DialogAction::changed);
    }
    require(
        match_zoom_target_ == kDefaultBattlefieldZoom && !zoom_focus_.follows_pointer &&
            zoom_focus_.x == static_cast<float>(match_layout_.left) +
                                 static_cast<float>(match_layout_.battlefield_width()) / 2.0F &&
            zoom_focus_.y == static_cast<float>(match_layout_.top) +
                                 static_cast<float>(match_layout_.battlefield_height()) / 2.0F,
        "turning the wheel zoom off did not ease to 1x about the centre"
    );
    SDL_Event wheel{};
    wheel.type = SDL_EVENT_MOUSE_WHEEL;
    wheel.wheel.y = 1.0F;
    std::ignore = frame_to_window(
        sdl_.renderer,
        static_cast<float>(match_layout_.left + match_layout_.battlefield_width() / 2),
        static_cast<float>(match_layout_.top + match_layout_.battlefield_height() / 2),
        &wheel.wheel.mouse_x,
        &wheel.wheel.mouse_y
    );
    send(wheel);
    require(match_zoom_target_ == kDefaultBattlefieldZoom, "the wheel zoomed with the setting off");
    require(
        take_engine_settings_action(settings::DialogAction::cancelled) && !engine_settings_dialog(),
        "Cancel did not close the dialog"
    );
    require(
        engine_settings().wheel_zoom && match_zoom_target_ == kCheckZoom,
        "Cancel did not bring the wheel zoom and the zoom back"
    );
    send(wheel);
    require(match_zoom_target_ > kCheckZoom, "the wheel did not zoom with the setting on");
    chosen = engine_settings();

    // Escape opens the game menu: off, Escape only clears the selection;
    // on, the first press cancels or clears and the next opens the menu.
    uint16_t commander = 0;
    uint16_t commander_type = 0;
    for (const auto& slot : match_->world().slots)
        if (commander == 0 && slot.unit != nullptr && slot.record.type_index != 0 &&
            slot.record.owner_index == match_local_player_) {
            commander = slot.unit_index;
            commander_type = slot.record.type_index;
        }
    require(commander != 0, "no local unit to select");
    chosen.escape_opens_menu = false;
    apply_engine_settings(chosen);
    clear_local_selection();
    tap_escape();
    require(!match_paused_, "Escape opened the menu with the setting off");
    chosen.escape_opens_menu = true;
    apply_engine_settings(chosen);
    adopt_selection(commander);
    match_command_ = MatchCommand::move;
    tap_escape();
    require(
        !match_paused_ && match_command_ == MatchCommand::none && has_local_selection(),
        "Escape did not first cancel the armed order"
    );
    tap_escape();
    require(!match_paused_ && !has_local_selection(), "Escape did not clear the selection next");
    send_key(escape_press(false));
    require(ingame_menu_column_shown(), "Escape with nothing to clear did not open the menu");
    send_key(escape_press(true));
    require(match_paused_, "a held Escape's repeat closed the menu");
    send_key(escape_release());
    tap_escape();
    require(!match_paused_, "Escape did not close the menu it opened");
    open_chat_line();
    tap_escape();
    require(!chat_composing_ && !match_paused_, "Escape did not close the chat line first");
    // The unit info panel closes first, then a pending build is cancelled,
    // and both keep the selection.
    adopt_selection(commander);
    match_->state().game.cursor_unit_id = commander;
    const bool info_opened = open_unit_info() && unit_info_panel_;
    match_->state().game.cursor_unit_id = 0;
    require(info_opened, "F1 did not open the unit info panel");
    tap_escape();
    require(
        !unit_info_panel_ && has_local_selection() && !match_paused_,
        "Escape did not close the unit info panel first"
    );
    pending_build_type_ = commander_type;
    tap_escape();
    require(
        pending_build_type_ == 0 && has_local_selection() && !match_paused_,
        "Escape did not cancel the pending build before the selection"
    );
    tap_escape();
    require(!has_local_selection() && !match_paused_, "Escape did not clear the selection");
    // In a shared game the menu Escape opens leaves the game running.
    {
        const Extension kept_extension = extension_;
        extension_.state = [](void*, const Runtime&) -> uint32_t {
            return extension_state::multiplayer | extension_state::shared_match;
        };
        tap_escape();
        const bool menu_opened = ingame_menu_column_shown();
        const bool game_running = match_clock_steps();
        tap_escape();
        const bool menu_closed = !ingame_menu_column_shown();
        extension_ = kept_extension;
        require(menu_opened, "Escape with nothing to clear did not open the menu in a shared game");
        require(game_running, "the menu Escape opened stopped a shared game");
        require(menu_closed, "Escape did not close the menu in a shared game");
        if (match_paused_)
            resume_match_pause();
    }
    chosen.escape_opens_menu = false;
    apply_engine_settings(chosen);

    // A failed save says so in the message log, and the dialog closes.
    {
        const fs::path kept_path = preference_path_;
        const fs::path blocking = fs::current_path() / kBlockingFileName;
        std::ofstream(blocking) << "not a folder\n";
        preference_path_ = blocking / "preferences.conf";
        auto& game = match_->state().game;
        oa::sim::messages::clear_messages(game);
        auto& dialog = open_engine_settings_dialog();
        dialog.chosen.switch_alt = !dialog.opened.switch_alt;
        const bool closed = take_engine_settings_action(settings::DialogAction::accepted);
        preference_path_ = kept_path;
        std::error_code removed;
        fs::remove(blocking, removed);
        bool reported = false;
        for (int32_t index = 0; index < oa::sim::messages::line_capacity(game); ++index) {
            const auto* line = oa::sim::messages::message_line(game, static_cast<uint32_t>(index));
            reported = reported || (line != nullptr &&
                                    std::string_view(line->text) == "Settings were not saved.");
        }
        require(closed && !engine_settings_dialog(), "a failed save left the dialog open");
        require(reported, "a failed save was not reported in the message log");
        oa::sim::messages::clear_messages(game);
        // The kept choice is saved once the file can be written again.
        opened = engine_settings();
        chosen = opened;
        chosen.switch_alt = kept.switch_alt;
        apply_engine_settings(chosen);
        require(!save_engine_settings(opened, chosen, false), "the settings were not saved");
        require(
            saved_general_number("SwitchAlt") == (kept.switch_alt ? 1 : 0),
            "SwitchAlt was not saved back"
        );
    }

    // Pathfinding cycles and Unit limit apply from the next game: the
    // running one keeps its own.
    const int32_t running_credit = match_->path_search_jobs().tick_credit;
    chosen.path_search_nodes = kDoubledPathNodes;
    chosen.unit_limit = kCheckUnitLimit;
    apply_engine_settings(chosen);
    require(
        match_->path_search_jobs().tick_credit == running_credit,
        "Pathfinding cycles changed the running game"
    );
    require(
        frontend_game().max_units_setting == kCheckUnitLimit,
        "the unit limit did not set the next game's"
    );
    apply_skirmish_players();
    require(match_ && !altitude_sight_blocked_, "the next skirmish did not start");
    enter_match_view();
    require(
        match_->path_search_jobs().tick_credit == kDoubledPathNodes,
        "a game played alone did not take the Pathfinding cycles"
    );
    require(
        match_->state().game.max_units_setting == kCheckUnitLimit &&
            EngineSettingsState::restart_unit_limit(*this) == kCheckUnitLimit,
        "the next skirmish did not take the unit limit"
    );
    // A match that turns out to be a replay or shared plays at the base credit.
    EngineSettingsState::hold_path_credit(*this, extension_state::replay);
    require(
        match_->path_search_jobs().tick_credit == settings::base_path_search_nodes,
        "a replay did not play at the base path credit"
    );
    EngineSettingsState::hold_path_credit(*this, 0);
    require(
        match_->path_search_jobs().tick_credit == settings::base_path_search_nodes,
        "a replay left the base path credit"
    );
    EngineSettingsState::start_path_credit(*this, true);
    require(
        match_->path_search_jobs().tick_credit == settings::base_path_search_nodes,
        "a shared game did not play at the base path credit"
    );

    // Back to the settings the check found, in a skirmish at their limit.
    apply_engine_settings(kept);
    frontend_game().max_units_setting = kept.unit_limit;
    apply_skirmish_players();
    require(match_ && !altitude_sight_blocked_, "the last skirmish did not start");
    enter_match_view();
    require(
        match_->path_search_jobs().tick_credit == kept.path_search_nodes &&
            match_->state().game.max_units_setting == kept.unit_limit,
        "the skirmish did not go back to the settings"
    );
    std::cout << "engine settings check: the maximum frame rate (and --max-fps over it), "
                 "enhanced anti-aliasing, Vertical sync, the acceleration flags' lock, "
                 "Hardware acceleration and its retries, the "
                 "performance statistics, SwitchAlt, the wheel zoom with Cancel, Escape's order, "
                 "a failed save, and the path credit and unit limit of the next game take "
                 "effect\n";
}

} // namespace oa::app

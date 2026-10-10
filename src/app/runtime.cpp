// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Runtime construction, main loop and screen loading.
#include "oa/app/runtime.hpp"
#include "oa/app/view_rules.hpp"
#include "oa/data/defs/layout.hpp"
#include "map_picture_state.hpp"
#include "oa/app/asset_files.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/hook_call.hpp"
#include "oa/app/mod_install/inbox.hpp"
#include "match_clock.hpp"
#include "graphics_report.hpp"
#include "render_host.hpp"
#include "render_run.hpp"
#include "oa/base/float_precision.hpp"
#include "oa/data/defs/version.hpp"
#include "oa/platform/app_loop.hpp"
#include "oa/platform/job_pool.hpp"
#include "oa/platform/log_files.hpp"
#include "oa/platform/system.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/gui_input/gadget_panel.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::app {

void Runtime::destroy_render_run(RenderRun* run) noexcept {
    delete run;
}

namespace {

/// The longest the idle loop waits for an event while it looks for a second
/// start's mod packages, in milliseconds.
constexpr int32_t kHandoffWaitMs = 1000;

// Names a built-in screen in the headless check's output.
[[nodiscard]] std::string_view screen_label(Screen screen) {
    switch (screen) {
    case Screen::main_menu:
        return "main menu";
    case Screen::single_player:
        return "single player";
    case Screen::skirmish:
        return "skirmish";
    case Screen::map_selection:
        return "map selection";
    case Screen::loading:
        return "loading";
    case Screen::match:
        return "match";
    case Screen::options:
        return "options";
    case Screen::sound:
        return "sound";
    case Screen::visuals:
        return "visuals";
    case Screen::speeds:
        return "speeds";
    case Screen::music:
        return "music";
    case Screen::new_campaign:
        return "new campaign";
    case Screen::any_mission:
        return "any mission";
    case Screen::load_game:
        return "load game";
    case Screen::campaign_end:
        return "campaign end";
    case Screen::briefing:
        return "briefing";
    }
    return "unnamed screen";
}

/// Returns the record of a frontend panel that holds the keyboard focus.
///
/// The panel is loaded as its loader loads it, undrawn, with its records
/// where its GUI file has them: at their root's corner. With `from` -1 the
/// focus is the loader's, passed on to the next record when the panel's
/// setup hid it; otherwise it moves from `from` as focus_nearest() moves it.
///
/// @param source the panel's records as the screen holds them
/// @param from the record holding the focus, or -1 for the loader's
/// @param direction where the focus moves from `from`
/// @return the record, or -1 for none
int32_t frontend_panel_focus(
    const oa::ui::gui_layout::Layout& source,
    int32_t from = -1,
    oa::ui::gui_input::FocusDirection direction = oa::ui::gui_input::FocusDirection::next
) {
    namespace gui = oa::ui::gui_input;
    if (source.gadgets.empty())
        return -1;
    auto layout = source;
    const auto& root = layout.gadgets.front().common;
    for (std::size_t index = 1; index < layout.gadgets.size(); ++index) {
        auto& common = layout.gadgets[index].common;
        if (common.width <= 0 || common.height <= 0)
            continue;
        common.x = static_cast<int16_t>(common.x - root.x);
        common.y = static_cast<int16_t>(common.y - root.y);
    }
    gui::GadgetPanel panel;
    gui::init_gadget_panel(panel);
    auto* owner = gui::load_panel(panel, layout, root.name, gui::panel_flag::no_draw);
    if (owner == nullptr)
        return -1;
    const auto shown = [&source](int32_t index) {
        return index > 0 && static_cast<std::size_t>(index) < source.gadgets.size() &&
               source.gadgets[static_cast<std::size_t>(index)].common.active != 0;
    };
    if (from >= 0) {
        owner->focus = from;
        gui::focus_nearest(panel, direction);
    } else if (owner->focus > 0 && !shown(owner->focus)) {
        gui::focus_nearest(panel, gui::FocusDirection::next);
    }
    return owner->focus > 0 && static_cast<std::size_t>(owner->focus) < source.gadgets.size()
               ? owner->focus
               : -1;
}

} // namespace

Runtime::Runtime(
    Options options,
    oa::AssetStore& assets,
    const Extension& extension,
    SDL_Window* window,
    SDL_Renderer* renderer,
    RendererHost* renderer_host
)
    : options_(std::move(options)), assets_(assets), extension_(extension),
      unit_sound_catalog_(oa::audio::game_audio::UnitSoundCatalog::load(assets)),
      audio_player_(assets), offline_effects_(effect_boundary_, effect_boundary_) {
    // The capacities a mod's profile set, before anything reads them.
    if (options_.mod_profile)
        limits_ = options_.mod_profile->limits;
    if (extension_.frontend_game == nullptr)
        frontend_game_ = std::make_unique<oa::Game>();
    if (const uint32_t draw_threads = options_.draw_threads.value_or(
            oa::platform::job_pool::default_threads(oa::platform::processor_count())
        );
        draw_threads > 1)
        draw_pool_ = std::make_unique<oa::platform::job_pool::Pool>(draw_threads);
    if (window != nullptr && renderer != nullptr) {
        sdl_.window = window;
        sdl_.renderer = renderer;
        sdl_.borrowed = true;
        if (renderer_host != nullptr) {
            render_run_.reset(new RenderRun{renderer_host});
            take_renderer_names(
                renderer_host->facts().renderer, stats_adapter_name(renderer_host->facts())
            );
        }
    }
    start_session_display();
    choose_web_links();
    call_hook_or_raise<&Extension::startup>(extension_, *this);
    load_all_sounds();
    register_screens();
    call_hook_or_raise<&Extension::ready>(extension_, *this);
    load_preference_file();
    // The player's own folder, and the one-time move of the saved games into
    // it, before the frontend's preferences read the Image Output Directory
    // that defaults to it.
    start_user_folder();
    // Stored after the one-time legacy import, which runs only while no
    // preferences file exists. An unwritable preferences file only costs the
    // next start the dialog.
    if (!options_.remember_game_dir.empty()) {
        remember_game_directory(preference_values_, options_.remember_game_dir);
        try {
            oa::platform::preferences::save(preference_path_, preference_values_);
        } catch (const std::exception& error) {
            std::cerr << "open-annihilation: the chosen game directory is not remembered: "
                      << error.what() << '\n';
        }
    }
    load_logo_textures();
    discover_first_map();
    // The language the game shows its text in, which loads the translation
    // table and the fonts: 3.1c's command line, else the setting, else the
    // operating system's.
    start_language();
    // SIDEDATA, and the sides' interface art and fonts it names, from the
    // language's folders first.
    load_side_table();
    require_side_files();
    init::reset_player_slots(state_, player_storage_, false);
    init::load_preferences(
        state_,
        skirmish_settings_,
        preferences_,
        *this,
        view_rules::display_mode_setting(ui_rules())
    );
    // The Open Annihilation settings, after the frontend's preferences hold SwitchAlt.
    load_engine_settings();
    // The settings the profile's display rules let the player change.
    load_view_settings();
    // The GUI text loops hand characters outside the 8-bit fonts to the
    // system's fonts when the profile's text rendering asks for it.
    install_game_text_hooks();
    // Session start sets the display gamma from the saved Gamma.
    apply_saved_gamma();
    audio_player_.set_volume(wave_volume_, preferences_.fx_volume);
    state_.state = frontend::state_id::main_menu;
    state_.signal = frontend::signal_id::initialize;
    state_.pending_signal = frontend::signal_id::initialize;
    state_.video_context_flags = frontend::flags::fullscreen_mode;
    environment_.messages_enabled = 1;
    environment_.message_target.value = kMessageTargetHandle;
    // Fields the game switches set before the dispatcher first runs; the
    // extension then names the states it runs in place of the engine's.
    state_.skip_intro = options_.launch.skip_intro;
    call_hook_or_raise<&Extension::frontend_states>(extension_, frontend_states_);
    step(frontend::Step::reload_unit_overrides, state_);
    frontend::dispatch(state_, *this, frontend_states_);
}

Runtime::ExtensionRelease::~ExtensionRelease() {
    // The runtime's state past its match is gone by now, its error report
    // among it, so an error goes straight to stderr.
    call_hook_or_report<&Extension::release_runtime>(
        runtime.extension_,
        [](const char* hook, const char* message) {
            std::fprintf(stderr, "open-annihilation: extension hook %s: %s\n", hook, message);
        },
        runtime
    );
}

int Runtime::run() {
    int exit_code = 0;
    const auto extension_run = [&](RunPhase phase) {
        return call_hook_or_raise<&Extension::run_mode>(extension_, *this, phase, exit_code);
    };
    if (extension_run(RunPhase::start))
        return exit_code;
    if (options_.headless_check) {
        // The director's runs come before the extension's headless runs,
        // which never see them; an extension takes part only through
        // open_recording.
        if (!options_.generate_script.empty()) {
            const int status = run_generate_script();
            flush_preferences();
            return status;
        }
        if (!options_.render_script.empty()) {
            const int status = run_render_script();
            flush_preferences();
            return status;
        }
        if constexpr (self_checks_built) {
            if (options_.check_director_view) {
                check_director_view();
                flush_preferences();
                return 0;
            }
            if (options_.check_director_render) {
                check_director_render();
                flush_preferences();
                return 0;
            }
            if (options_.check_interpolation) {
                check_interpolation();
                flush_preferences();
                return 0;
            }
            if (options_.check_unit_playout) {
                check_unit_playout();
                flush_preferences();
                return 0;
            }
        }
        if (extension_run(RunPhase::headless_first))
            return exit_code;
        if constexpr (self_checks_built)
            if (options_.check_navigation)
                check_navigation();
        if (extension_run(RunPhase::headless))
            return exit_code;
        if (options_.save_after || !options_.load_file.empty()) {
            run_headless_saveload();
            flush_preferences();
            return 0;
        }
        if (options_.campaign_mission) {
            const auto result =
                run_headless_campaign(options_.match_ticks.value_or(kDefaultCampaignTicks));
            flush_preferences();
            return result;
        }
        if (options_.match_ticks) {
            if (options_.frame_rate)
                run_headless_frames(*options_.match_ticks, *options_.frame_rate);
            else
                run_headless_match(*options_.match_ticks);
            flush_preferences();
            return 0;
        }
        if (options_.skip_intro && !options_.snapshot.empty())
            write_ppm(options_.snapshot, surface_);
        std::cout << "native check: " << surface_.width << 'x' << surface_.height << ", "
                  << resources_.layout.gadgets.size() << " GUI gadgets, " << audio_registry_.size()
                  << " registered sounds\n";
        std::cout << "native check: " << screen_label(screen_) << " open, "
                  << ui::frontend_dialogs::dialog_count() << " dialogs over it\n";
        std::cout << "native check: the game offers " << (offers_movies() ? "" : "no ")
                  << "movies\n";
        flush_preferences();
        return 0;
    }
    initialize_sdl();
    // The app lifecycle's events are acted on as SDL queues them, until the
    // run ends however it ends.
    install_lifecycle_watch();

    struct LifecycleWatchRemoval {
        Runtime* runtime{};

        ~LifecycleWatchRemoval() { runtime->remove_lifecycle_watch(); }
    } lifecycle_watch_removal{this};

    if constexpr (self_checks_built) {
        if (options_.check_match_dialogs) {
            check_match_dialogs();
            flush_preferences();
            return 0;
        }
        if (options_.check_load_save) {
            check_load_save();
            flush_preferences();
            return 0;
        }
        if (options_.check_frontend_controls) {
            check_frontend_controls();
            flush_preferences();
            return 0;
        }
        if (options_.check_scroll_bars) {
            check_scroll_bars();
            flush_preferences();
            return 0;
        }
        if (options_.check_engine_settings) {
            check_engine_settings();
            flush_preferences();
            return 0;
        }
        if (options_.check_user_folder) {
            check_user_folder();
            flush_preferences();
            return 0;
        }
        if (options_.check_mod_switch) {
            check_mod_switch();
            flush_preferences();
            return 0;
        }
        if (options_.check_mod_warning) {
            check_mod_warning();
            flush_preferences();
            return 0;
        }
        if (options_.check_mod_install) {
            check_mod_install();
            flush_preferences();
            return 0;
        }
        if (options_.check_renderer_ladder) {
            const int status = check_renderer_ladder();
            flush_preferences();
            return status;
        }
        if (options_.check_briefing_narration) {
            check_briefing_narration();
            flush_preferences();
            return 0;
        }
        if (options_.check_match_layers) {
            check_match_layers();
            flush_preferences();
            return 0;
        }
        if (options_.check_render_tiers) {
            const int status = check_render_tiers();
            flush_preferences();
            return status;
        }
        if (options_.check_build_preview) {
            const int status = check_build_preview();
            flush_preferences();
            return status;
        }
        if (options_.check_match_orders) {
            check_match_orders();
            flush_preferences();
            return 0;
        }
        if (options_.check_factory_orders) {
            check_factory_orders();
            flush_preferences();
            return 0;
        }
        if (options_.check_unit_speech) {
            check_unit_speech();
            flush_preferences();
            return 0;
        }
        if (options_.check_download_builds) {
            check_download_builds();
            flush_preferences();
            return 0;
        }
        if (options_.check_stockpile_builds) {
            check_stockpile_builds();
            flush_preferences();
            return 0;
        }
        if (options_.check_unit_page_memory) {
            check_unit_page_memory();
            flush_preferences();
            return 0;
        }
        if (options_.check_side_column) {
            check_side_column();
            flush_preferences();
            return 0;
        }
        if (options_.check_match_bars) {
            check_match_bars();
            flush_preferences();
            return 0;
        }
        if (!options_.check_unit_pages.empty()) {
            check_unit_pages();
            flush_preferences();
            return 0;
        }
        if (!options_.check_unit_language.empty()) {
            check_unit_language();
            flush_preferences();
            return 0;
        }
        if (options_.check_language_switch) {
            check_language_switch();
            flush_preferences();
            return 0;
        }
        if (options_.check_kill_board) {
            check_kill_board();
            flush_preferences();
            return 0;
        }
        if (options_.check_paused_save) {
            check_paused_save();
            flush_preferences();
            return 0;
        }
        if (options_.check_simulation_hash) {
            check_simulation_hash();
            flush_preferences();
            return 0;
        }
        if (options_.check_patrol_reclaim) {
            check_patrol_reclaim();
            flush_preferences();
            return 0;
        }
        if (options_.check_reclaim_cursor) {
            check_reclaim_cursor();
            flush_preferences();
            return 0;
        }
        if (options_.check_pointer_interfaces) {
            check_pointer_interfaces();
            flush_preferences();
            return 0;
        }
        if (options_.check_megamap_clicks) {
            check_megamap_clicks();
            flush_preferences();
            return 0;
        }
        if (options_.check_radar_orders) {
            check_radar_orders();
            flush_preferences();
            return 0;
        }
        if (options_.check_touch_controls) {
            check_touch_controls();
            flush_preferences();
            return 0;
        }
        if (options_.check_pad_controls) {
            check_pad_controls();
            flush_preferences();
            return 0;
        }
        if (options_.check_multiplayer_menu) {
            check_multiplayer_menu();
            flush_preferences();
            return 0;
        }
    }
    if (options_.benchmark_frames) {
        run_benchmark(*options_.benchmark_frames);
        flush_preferences();
        return 0;
    }
    // The main menu checks Revision.GPF once, as it first opens.
    const oa::data::defs::Files files = asset_files(assets_);
    if (oa::data::defs::revision_gpf_mismatch(&files, nullptr))
        std::cerr << "warning: gamedata/version.tdf does not name Revision.GPF "
                  << oa::data::defs::expected_gpf_version
                  << "; the game data may not match this build\n";
    if (!call_hook_or_raise<&Extension::start_scene>(extension_, *this))
        start_menu_music();
    if (options_.showcase != Showcase::none)
        run_showcase();
    std::size_t frames = 0;
    // A showcase has played the whole run; the loop does not start.
    bool running = options_.showcase == Showcase::none;
    while (running && !exit_requested_ &&
           (!options_.frame_limit || frames < *options_.frame_limit)) {
        // The check holds the window inactive before the loop decides whether
        // to wait, and keeps it so for the whole run.
        if constexpr (self_checks_built)
            if (options_.check_running_while_inactive)
                begin_inactive_loop_check();
        park_music_while_inactive();
        // The menu's loop is silent while the application is inactive.
        audio_player_.hold_loop(!application_active_);
        SDL_Event event{};
        // A frame-limited run is scripted and must finish without focus. A
        // remote-controlled run is served from the frame hooks every frame,
        // and so is a run an extension has asked to keep running while the
        // window is inactive (keep_running_while_inactive).
        const bool live = keeps_running_inactive() || options_.remote_controlled;
        if (!options_.frame_limit && oa::platform::application_waits_for_events(
                                         application_active_, live, keep_running_while_inactive_
                                     )) {
            // While this copy takes a second start's mod packages, it looks
            // for them once a second even in the background.
            if (mod_install::handoff_folder()) {
                if (SDL_WaitEventTimeout(&event, kHandoffWaitMs))
                    dispatch_event(event, running);
                take_handed_mod_files();
            } else if (SDL_WaitEvent(&event)) {
                dispatch_event(event, running);
            }
            oa::platform::log_files::maintain();
            continue;
        }
        run_frame(running);
        ++frames;
        pace_next_frame(running);
    }
    if constexpr (self_checks_built)
        if (options_.check_running_while_inactive)
            finish_inactive_loop_check();
    if (video_capture_) {
        video_capture_->finish();
        video_capture_.reset();
    }
    call_hook_or_raise<&Extension::shutdown>(extension_, *this);
    flush_preferences();
    release_pointer(sdl_.window);
    return exit_status_;
}

void Runtime::take_video_capture(std::unique_ptr<VideoCapture> capture) {
    video_capture_ = std::move(capture);
}

void Runtime::run_frame(bool& running) {
    begin_loop_frame();
    SDL_Event event{};
    while (SDL_PollEvent(&event))
        dispatch_event(event, running);
    // Whatever showed or hid the system's pointer since the last frame, the
    // rule holds again before this frame is drawn; so does the window's
    // frame, for the screen the events left.
    apply_system_pointer(false);
    apply_window_frame();
    idle_tick();
    const auto now_ms = static_cast<uint32_t>(SDL_GetTicks());
    if (oa::platform::finished_stream_sweep_due(now_ms, last_stream_sweep_ms_)) {
        audio_player_.collect_finished();
        last_stream_sweep_ms_ = now_ms;
    }
    oa::platform::log_files::maintain();
}

void Runtime::dispatch_event(SDL_Event& event, bool& running) {
    // The inactive-loop check's wake ends the run. It is not a focus change,
    // and it must not be read as one.
    if constexpr (self_checks_built)
        if (take_inactive_loop_wake(event)) {
            exit_requested_ = true;
            return;
        }
    note_window_activation(event);
    note_input_activity(event);
    // A hardware keyboard's Cmd alternates stand for their keys while touch
    // controls are on; the app lifecycle's events, which the watch acted on,
    // reach no screen; the touch controls take fingers and the presses on
    // their controls, and the gamepads their own events.
    remap_command_key(event);
    // The keypad's Enter is Return to everything after this, as it is in
    // 3.1c: it opens and sends the chat line and answers dialogs.
    remap_keypad_enter(event);
    // Each key press, whichever screen takes it, forgets the quick key the
    // last one answered a panel with; the press records its own again.
    if (event.type == SDL_EVENT_KEY_DOWN)
        answered_key_ = 0;
    // Shift let go after a building was queued ends build mode before any
    // event that follows acts: a click just after the release is no build.
    end_build_on_shift_release();
    if (take_lifecycle_event(event))
        return;
    if (take_touch_event(event, running)) {
        apply_screen_request();
        return;
    }
    if (take_pad_event(event, running)) {
        apply_screen_request();
        return;
    }
    // The renderer's own events reach no screen.
    if (take_render_event(event))
        return;
    // While an input method composes, the keys it uses are its own.
    if (take_composition_event(event))
        return;
    // Alt+Enter switches between full screen and a window on every screen,
    // before the screen or a screen package sees the key; its repeats reach
    // no screen either, so a held Alt+Enter never opens the chat line or
    // presses a dialog's default button.
    if (take_full_screen_event(event))
        return;
    // The macOS application menu's Settings… item asks for the settings on
    // whatever screen shows.
    if (take_engine_settings_request(event)) {
        apply_screen_request();
        return;
    }
    // Keys the profile's display rules take on every screen come next.
    if (handle_view_rule_key(event)) {
        apply_screen_request();
        return;
    }
    if (!dispatch_screen_input(event))
        handle_sdl_event(event, running);
    apply_screen_request();
}

void Runtime::idle_tick() {
    // A program driving the run reads extension_clock, which advances one
    // fixed step a frame. The count is this frame, before the frame hooks,
    // so it matches the frame the endpoint answers with.
    if (options_.remote_controlled)
        ++extension_clock_frame_;
    take_frame_time();
    camera_moved_ = false;
    tick_screen_packages();
    sync_engine_settings_menu_item();
    step_music();
    present_unit_announcements();
    if (exit_requested_)
        return;
    if (screen_ == Screen::briefing && !briefing_from_pause_)
        tick_mission_briefing();
    move_match_camera();
    // The gamepads' sticks, timers and looks, then the touch controls'
    // timers, camera and layout.
    tick_pad();
    tick_touch();
    // QUEUE let go on the touch controls or the pad ends build mode as
    // Shift does.
    end_build_on_shift_release();
    // Each game frame opens a profile window, and the pump, the
    // ticks and the drawing are charged as they end.
    const bool profiled = screen_ == Screen::match && match_;
    if (profiled)
        begin_profile_window();
    call_hook_or_raise<&Extension::frame>(extension_, *this, FrameStage::pump);
    if constexpr (self_checks_built)
        if (options_.check_running_while_inactive)
            note_inactive_loop_frame();
    if (profiled)
        mark_profile(OA_PROFILE_SYNC);
    call_hook_or_raise<&Extension::frame>(extension_, *this, FrameStage::after_pump);
    // The extension may have asked to end the run (ScreenServices::quit):
    // it ends here, leaving the match first, and nothing more of the frame
    // runs.
    finish_quit_request();
    if (exit_requested_)
        return;
    // The frame's pointer pass picks the unit under the still pointer too,
    // before the ticks, so a unit that moves under it becomes the cursor unit.
    if (screen_ == Screen::match && match_ && !match_paused_ && !match_finished_)
        pick_cursor_unit(false);
    // A held scroll bar or arrow moves its knob in the frame's update.
    tick_scroll_bars();
    step_match_frame();
    if (screen_ == Screen::match && match_)
        present_match_outcome();
    // The frame is drawn between the ticks at the fraction the clock step
    // chose; whatever draws after it shows whole ticks.
    frame_draws_.units_drawn = 0;
    frame_draws_.units_between_ticks = 0;
    frame_draws_.probe_drawn = false;
    // A new renderer record is told of once the main menu shows, the saved
    // games' move once that notice is closed, where the game folder was
    // found after that, and a mod that cannot start a game once both are.
    tell_renderer_records();
    tell_saves_moved();
    tell_found_install();
    tell_incomplete_mod();
    tell_mod_installs();
    render();
    presentation_alpha_ = 1.0F;
    capture_film_frame();
    if (profiled && match_)
        mark_profile(OA_PROFILE_RENDER_STATIC);
    // The extensions see the frame as it was shown, outside the frame
    // profile's drawing bucket.
    call_hook_or_raise<&Extension::frame>(extension_, *this, FrameStage::presented);
    // The frame just drawn showed the outcome's title; the end screen follows
    // on its own.
    if (screen_ == Screen::match && match_finished_)
        finish_match_outcome();
}

void Runtime::move_match_camera() {
    if (screen_ == Screen::match && selected_tnt_)
        step_match_zoom();
    if (screen_ != Screen::match || match_paused_)
        return;
    pan_match_camera();
    if (match_)
        follow_match_camera_unit();
    if (match_tracking_) {
        if (!match_unit_present(tracked_match_unit_))
            stop_match_tracking();
        else
            center_camera_on_unit(tracked_match_unit_);
    }
}

uint32_t Runtime::clock_milliseconds() const {
    if (options_.fixed_clock)
        return match_timing_.tick * kFixedClockMsPerTick;
    return static_cast<uint32_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                     std::chrono::steady_clock::now().time_since_epoch()
    )
                                     .count());
}

bool Runtime::match_running() const {
    return match_ && screen_ == Screen::match;
}

bool Runtime::match_clock_steps() const {
    if (!match_running() || match_tick_blocked_)
        return false;
    const bool shared = (current_extension_state() & extension_state::shared_match) != 0;
    // The team menu and its panels hold no match, as 3.1c holds a game on
    // this machine alone only for its in-game menu.
    const bool menu_holds = match_paused_ && !match_finished_ && !team_panel_open();
    return match_clock_runs(shared, menu_holds, match_finished_);
}

oa::base::game_loop::Timing Runtime::saved_match_timing() const {
    auto timing = match_timing_;
    if (match_)
        timing.flags = clock_flags_with_pause(timing.flags, match_->state().game.sim_run_flags);
    return timing;
}

namespace {

/// Returns the instance generation of a unit slot of the match, which tells
/// the unit playout a unit made in a slot freed within the same tick from
/// the one before (oa::present::unit_playout::Hooks::slot_generation).
///
/// @param context the match (oa::sim::match_runtime::Match)
/// @param slot the unit slot
/// @return SlotRuntime::instance_generation; 0 for a slot outside the pool
uint32_t unit_slot_generation(void* context, uint32_t slot) noexcept {
    try {
        return static_cast<oa::sim::match_runtime::Match*>(context)
            ->runtime_state(static_cast<uint16_t>(slot))
            .instance_generation;
    } catch (const std::exception&) {
        return 0;
    }
}

/// Fills in what a unit's movement holds for the unit playout to move it on
/// by (oa::present::unit_playout::Hooks::motion): its ground movement record,
/// the route its owner shared with its mirrored navigator, and its air
/// driver's point, heading and seek goal. Reads the match only.
///
/// @param context the match (oa::sim::match_runtime::Match)
/// @param slot the unit slot
/// @param[out] motion what its movement holds; left as it is for a slot
///     without a movement record or outside the pool
void unit_motion(void* context, uint32_t slot, oa::present::unit_playout::Motion& motion) noexcept {
    try {
        auto& match = *static_cast<oa::sim::match_runtime::Match*>(context);
        const auto index = static_cast<uint16_t>(slot);
        const auto* ground = std::as_const(match).ground_runtime(index);
        if (ground == nullptr)
            return;
        motion.ground = true;
        motion.speed = ground->movement.speed;
        motion.velocity = {
            ground->movement.velocity[0], ground->movement.velocity[1], ground->movement.velocity[2]
        };
        motion.layer =
            static_cast<uint8_t>(ground->movement.flags & oa::sim::unit_movement::occupancy_mask);
        motion.blocked = (ground->movement.flags & oa::sim::unit_movement::collision_blocked) != 0;
        if (ground->mirrored_driver) {
            const auto& route = ground->mirrored_navigation;
            motion.route_count = static_cast<uint8_t>(std::clamp<int32_t>(route.count, 0, 3));
            for (std::size_t i = 0; i < motion.route.size() && i < route.points.size(); ++i)
                motion.route[i] = route.points[i];
        }
        if (const auto* driver = match.air_driver(index)) {
            motion.air = true;
            motion.air_point = driver->position;
            motion.air_velocity = driver->velocity;
            motion.air_heading = driver->heading;
            if (driver->goal != nullptr && driver->goal->kind == oa::sim::air::AirGoalKind::seek) {
                motion.seek = true;
                motion.seek_step = driver->goal->step;
            }
        }
    } catch (const std::exception&) {
        motion = {};
    }
}

} // namespace

void Runtime::advance_match_clock(uint32_t now_ms) {
    // Every tick rounds with the settings the game started with.
    oa::base::float_precision::restore_program_float_control();
    match_timing_.flags =
        clock_flags_with_pause(match_timing_.flags, match_->state().game.sim_run_flags);
    // A clock state the loop refuses runs no step this frame.
    if (const auto clock_error = oa::base::game_loop::update_timing(
            match_timing_, oa::base::game_loop::scaled_clock(now_ms, match_clock_scale())
        );
        clock_error != oa::base::game_loop::LoopError::none) {
        report_match_tick_error(oa::base::game_loop::loop_error_text(clock_error));
        return;
    }
    try {
        for (int32_t step = 0; step < match_timing_.pending_steps; ++step) {
            // Each step that runs a tick is timed for the frame statistics,
            // on the real clock; a step the extension holds runs none.
            const auto step_start = std::chrono::steady_clock::now();
            const uint32_t tick_before = match_timing_.tick;
            // A step the extension fails to run is reported as a simulation
            // error, and the frame's remaining steps are dropped.
            HookError step_error;
            const bool stepped =
                call_hook<&Extension::simulation_step>(extension_, step_error, *this);
            if (step_error.caught) {
                report_match_tick_error(step_error.message);
                break;
            }
            if (!stepped) {
                ++match_timing_.tick;
                match_->simulation().tick = match_timing_.tick;
                // A stage's lines timed for this tick run before it.
                run_due_stage_lines();
                match_->tick();
            }
            // A fault the step's tick noted, here or in the extension, ends
            // the frame's steps, as an error the tick threw did.
            if (const char* fault = match_->fault()) {
                report_match_tick_error(fault);
                match_->clear_fault();
                break;
            }
            if (match_timing_.tick != tick_before) {
                const auto tick_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                         std::chrono::steady_clock::now() - step_start
                )
                                         .count();
                frame_pacing::note_frame_measure(
                    frame_stats_, frame_pacing::FrameMeasure::tick, static_cast<uint64_t>(tick_ns)
                );
                phase_times_.simulation += tick_ns;
            }
            // The step's records are applied: the playout of the units of
            // players this machine does not simulate reads where they are.
            // It throws nothing, so it costs the match no step.
            observe_unit_playout();
        }
    } catch (const std::exception& error) {
        report_match_tick_error(error.what());
    }
    if (match_timing_.pending_steps != 0)
        oa::sim::messages::expire_oldest_message(match_->state().game);
    // The map's timed units are placed once a frame, as they come due.
    step_timed_map_units();
}

void Runtime::observe_unit_playout() noexcept {
    if (match_)
        unit_playout_.observe(match_->state(), {match_.get(), unit_slot_generation, unit_motion});
}

void Runtime::report_match_tick_error(std::string_view message) {
    status_ = "match tick " + std::to_string(match_timing_.tick) + ": " + std::string(message);
    if (message == last_tick_error_) {
        ++tick_error_repeats_;
        if ((tick_error_repeats_ & (tick_error_repeats_ - 1)) != 0)
            return;
    } else {
        last_tick_error_ = message;
        tick_error_repeats_ = 1;
    }
    const auto line =
        status_ + (tick_error_repeats_ > 1 ? " (x" + std::to_string(tick_error_repeats_) + ")"
                                           : std::string());
    std::cerr << "simulation error: " << line << '\n';
    console_post_message("Simulation error: " + line);
}

void Runtime::report_hook_error(const char* hook, const char* message) noexcept {
    try {
        std::string line = std::string(hook) + ": " + message;
        if (line == last_hook_error_) {
            ++hook_error_repeats_;
            if ((hook_error_repeats_ & (hook_error_repeats_ - 1)) != 0)
                return;
        } else {
            last_hook_error_ = line;
            hook_error_repeats_ = 1;
        }
        if (hook_error_repeats_ > 1)
            line += " (x" + std::to_string(hook_error_repeats_) + ")";
        std::cerr << "open-annihilation: extension hook " << line << '\n';
    } catch (...) {
        std::fprintf(stderr, "open-annihilation: extension hook %s: %s\n", hook, message);
    }
}

void Runtime::present_unit_announcements() {
    for (auto& event : offline_services_.pump_announcements()) {
        if (event.sound_resource)
            play_wave_file(*event.sound_resource);
        if (event.text)
            post_unit_report(event.unit_index, *event.text);
    }
}

uint32_t Runtime::match_clock_scale() const {
    // 30 clock units per real second. Game speed 10 * 0.1 rate = 30 Hz.
    // +/- changes actual_rate only; folding speed into the clock scale as well
    // made speed 20 run at 4x instead of 2x.
    return 30u;
}

uint32_t Runtime::frontend_tick() const {
    if (fake_frontend_tick_)
        return *fake_frontend_tick_;
    return oa::base::game_loop::scaled_clock(
        static_cast<uint32_t>(SDL_GetTicks()), match_clock_scale()
    );
}

bool Runtime::frame_owned_by_package() const {
    return screen_ == Screen::main_menu && state_.state == frontend::state_id::pump_only;
}

uint32_t Runtime::current_extension_state() const {
    return call_hook_or_raise<&Extension::state>(extension_, *this);
}

void Runtime::load_all_sounds() {
    const oa::data::defs::Files files = asset_files(assets_);
    const oa::data::defs::SoundCache cache{
        &audio_registry_,
        [](void* context) { static_cast<oa::audio::game_audio::Registry*>(context)->clear(); },
        [](void* context, const char* name, const char* sound) {
            static_cast<oa::audio::game_audio::Registry*>(context)->add(name, sound);
        },
    };
    oa::data::defs::sound_category_table_free(&unit_table_.sound_categories);
    oa::data::defs::load_all_sounds(&files, nullptr, cache, &unit_table_.sound_categories);
}

bool Runtime::frontend_has_keyboard() const {
    return screen_ != Screen::match && screen_ != Screen::loading && !frame_owned_by_package() &&
           !resources_.layout.gadgets.empty() && oa::ui::frontend_dialogs::dialog_count() == 0;
}

int32_t Runtime::frontend_focus() const {
    const auto& gadgets = resources_.layout.gadgets;
    if ((screen_ == Screen::new_campaign || screen_ == Screen::any_mission) &&
        !campaign_setup_focus_.empty()) {
        for (std::size_t index = 1; index < gadgets.size(); ++index)
            if (tdf_names_equal(gadgets[index].common.name, campaign_setup_focus_))
                return static_cast<int32_t>(index);
        return -1;
    }
    return frontend_focus_ > 0 && static_cast<std::size_t>(frontend_focus_) < gadgets.size()
               ? frontend_focus_
               : -1;
}

void Runtime::move_frontend_focus(oa::ui::gui_input::FocusDirection direction) {
    const auto focus = frontend_focus();
    const auto moved = frontend_panel_focus(resources_.layout, std::max(focus, 0), direction);
    frontend_focus_ = moved;
    if ((screen_ == Screen::new_campaign || screen_ == Screen::any_mission) && moved > 0)
        campaign_setup_focus_ =
            resources_.layout.gadgets[static_cast<std::size_t>(moved)].common.name;
}

void Runtime::load(Screen screen) {
    // The load and save dialogs are drawn over the screen they open from.
    if (screen == Screen::load_game && screen_ != Screen::load_game)
        capture_load_game_parent();
    // Screens over or after the match read the options the match changed.
    if (screen_ == Screen::match && match_)
        take_match_options(match_->state().game);
    if (const auto* previous = screen_find(&screens_, screen_id(screen_));
        previous != nullptr && previous->leave != nullptr) {
        auto context = screen_context();
        previous->leave(&context, previous->state);
    }
    screen_ = screen;
    widget_gaf_frames_.clear();
    widget_text_stages_.clear();
    widget_sprites_.clear();
    typed_key_hook_ = TypedKeyHook::none;
    typed_keys_.fill(0);
    const auto* desc = screen_find(&screens_, screen_id(screen));
    if (desc == nullptr)
        desc = screen_find(&screens_, screen_id(Screen::map_selection));
    // A screen without a panel of its own has no scroll bars.
    frontend_scrolls_layout_ = nullptr;
    auto context = screen_context();
    if (desc->assets.layout != nullptr) {
        const char* background = desc->background != nullptr
                                     ? desc->background(&context, desc->state)
                                     : desc->assets.background;
        // A layout named without a folder lies in the GUI directory.
        const std::string_view named_layout = desc->assets.layout;
        const auto layout = named_layout.find_first_of("/\\") == std::string_view::npos
                                ? oa::data::defs::gui_path(named_layout)
                                : std::string(named_layout);
        const renderer::ScreenAssetNames names{
            layout, "", desc->assets.palette, desc->assets.sprites, desc->assets.shared_sprites
        };
        resources_ = renderer::load_screen(assets_, names);
        // The panel's pictures in the shown language.
        caption_gaf_pictures(names.sprites, resources_.sprites);
        caption_gaf_pictures(names.shared_sprites, resources_.shared_sprites);
        // A panel's first draw binds its scroll bars: at once for a panel
        // drawn as it loads, after its setup for NEWGAME.GUI, which loads
        // undrawn and is set up first.
        if (!first_draw_after_setup(screen))
            bind_frontend_scrolls(names.layout, names.sprites);
        // The panel is up first; its setup then asks for the named background.
        // A bitmap that cannot be read throws; whether the backdrop changed
        // is not needed.
        if (background != nullptr)
            std::ignore = load_named_background(background, false, false, false);
    }
    if (desc->enter != nullptr)
        desc->enter(&context, desc->state);
    if (desc->assets.layout != nullptr && first_draw_after_setup(screen))
        bind_set_up_frontend_scrolls(desc->assets.layout, desc->assets.sprites);
    selected_ = -1;
    hovered_.reset();
    // The panel takes the focus its loader gives it, from its records as its
    // setup left them.
    frontend_focus_ = -1;
    if (desc->assets.layout != nullptr)
        frontend_focus_ = frontend_panel_focus(resources_.layout);
    apply_output_mode();
    // Each new screen has the window system hide the pointer again where
    // the game draws its own.
    apply_system_pointer(true);
    rebuild_surface();
}

void Runtime::rebuild_surface() {
    // A front-end frame painted again is a new picture for its prescale
    // target (accelerated_.screen_revision); one kept as shown is not. The
    // loading screen's frames count their own as the sink presents them.
    if (screen_ == Screen::match) {
        refresh_filtered_terrain();
        render_match_surface();
        draw_screen_packages();
        tick_and_draw_cursor();
        return;
    }
    if (screen_ == Screen::loading) {
        draw_loading_screen();
        draw_screen_packages();
        tick_and_draw_cursor();
        return;
    }
    if (frame_owned_by_package()) {
        if (surface_.width != kCanvasWidth || surface_.height != kCanvasHeight) {
            surface_.width = kCanvasWidth;
            surface_.height = kCanvasHeight;
            surface_.rgb.assign(static_cast<std::size_t>(kCanvasWidth) * kCanvasHeight * 3U, 0);
        }
        ++accelerated_.screen_revision;
        draw_screen_packages();
        tick_and_draw_cursor();
        return;
    }
    if (draw_end_screen_battlefield()) {
        ++accelerated_.screen_revision;
        draw_screen_packages();
        return;
    }
    // A panel whose setup has not yet asked for its named background keeps
    // the frame shown until it does.
    if (resources_.background.width == 0 || resources_.background.height == 0)
        return;
    ++accelerated_.screen_revision;
    std::vector<renderer::ButtonPresentation> presentation;
    presentation.reserve(resources_.layout.gadgets.size());
    const bool keyboard = frontend_has_keyboard();
    const auto focus = frontend_focus();
    for (std::size_t index = 0; index < resources_.layout.gadgets.size(); ++index) {
        auto condition = renderer::ButtonCondition::normal;
        // A button whose status is set (the chosen member of its group) shows
        // pressed like one pressed and held with the pointer still over it.
        // The pointer over a button without a press leaves it as it is.
        const auto* button =
            std::get_if<oa::ui::gui_layout::ButtonFields>(&resources_.layout.gadgets[index].fields);
        if (button != nullptr && button->grayed_out)
            condition = renderer::ButtonCondition::disabled;
        else if (
            (selected_ == static_cast<int32_t>(index) && hovered_ == index) ||
            (button != nullptr && button->status != 0)
        )
            condition = renderer::ButtonCondition::pressed;
        const auto& name = resources_.layout.gadgets[index].common.name;
        const auto frame = widget_gaf_frames_.find(name);
        const auto text_stage = widget_text_stages_.find(name);
        const auto sprite = widget_sprites_.find(name);
        presentation.push_back(
            {name,
             condition,
             frame == widget_gaf_frames_.end() ? std::nullopt
                                               : std::optional<std::size_t>(frame->second),
             text_stage == widget_text_stages_.end()
                 ? std::nullopt
                 : std::optional<std::size_t>(text_stage->second),
             sprite == widget_sprites_.end()
                 ? std::nullopt
                 : std::optional<renderer::SpriteOverride>(sprite->second)}
        );
        // The button's quick key is underlined in its caption, and the record
        // holding the keyboard focus is ringed while the screen has the
        // keyboard.
        if (button != nullptr)
            presentation.back().quick_key = static_cast<char>(button->quick_key);
        presentation.back().focused = keyboard && static_cast<int32_t>(index) == focus;
    }
    std::vector<renderer::ListPresentation> lists;
    if (screen_ == Screen::map_selection && !bound_map_names_.empty()) {
        const auto selected = static_cast<std::size_t>(std::max<int16_t>(0, modal_map_index_));
        lists.push_back({"MAPNAMES", bound_map_names_, map_first_visible(), selected});
    }
    // A list its scroll bar scrolls shows the rows the bar brings into view.
    if (const auto first = frontend_list_first("Campaign"))
        campaign_first_visible_ = *first;
    if (const auto first = frontend_list_first("Missions"))
        campaign_mission_first_visible_ = *first;
    if (screen_ == Screen::any_mission || screen_ == Screen::new_campaign) {
        if (!campaign_labels_.empty())
            lists.push_back(
                {"Campaign", campaign_labels_, campaign_first_visible_, selected_campaign_index_}
            );
        if (screen_ == Screen::any_mission && !campaign_mission_labels_.empty())
            lists.push_back(
                {"Missions",
                 campaign_mission_labels_,
                 campaign_mission_first_visible_,
                 selected_mission_index_}
            );
    }
    if (screen_ == Screen::campaign_end && !end_mission_rows_.empty()) {
        lists.push_back(
            {"Missions",
             end_mission_rows_,
             campaign_mission_first_visible_,
             selected_mission_index_}
        );
    }
    if (screen_ == Screen::load_game)
        present_load_game_panel(lists);
    renderer::render_screen_into(surface_, resources_, presentation, lists);
    if (auto* scrolls = frontend_scrolls()) {
        renderer::refresh_layout_scrolls(*scrolls, resources_.layout);
        renderer::draw_layout_scrolls(
            surface_,
            renderer::grayed_paint(resources_, frontend_gray_table_),
            frontend_scrolls_own_art_ ? &resources_.sprites : nullptr,
            resources_.shared_sprites,
            *scrolls,
            0,
            0
        );
    }
    if (screen_ == Screen::main_menu) {
        if (menu_sparks_.dest.empty())
            renderer::reset_menu_sparks(menu_sparks_, resources_.background);
        renderer::step_menu_sparks(menu_sparks_, surface_, resources_.background);
    } else {
        menu_sparks_ = {};
    }
    if (screen_ == Screen::briefing)
        draw_briefing_overlays();
    if (const auto* picture = map_picture_.get(); screen_ == Screen::map_selection &&
                                                  picture != nullptr && !picture->rgb.empty() &&
                                                  picture->width > 0 && picture->height > 0) {
        const auto* target = widget("MAPPIC");
        if (target != nullptr && picture->destination_width > 0 &&
            picture->destination_height > 0) {
            // The picture drawer clears the gadget-sized picture to index 0 before the
            // fitted map is drawn into it.
            for (int y = 0; y < target->common.height; ++y)
                for (int x = 0; x < target->common.width; ++x) {
                    const int destination_x = target->common.x + x;
                    const int destination_y = target->common.y + y;
                    if (destination_x < 0 || destination_y < 0 ||
                        destination_x >= static_cast<int>(surface_.width) ||
                        destination_y >= static_cast<int>(surface_.height))
                        continue;
                    std::copy_n(
                        picture->clear_rgb.begin(),
                        3,
                        surface_.rgb.begin() +
                            static_cast<std::ptrdiff_t>(
                                (static_cast<std::size_t>(destination_y) * surface_.width +
                                 static_cast<std::size_t>(destination_x)) *
                                3U
                            )
                    );
                }
            for (int y = 0; y < picture->destination_height; ++y) {
                const auto source_y = static_cast<std::size_t>(y) * picture->source_height /
                                      static_cast<std::size_t>(picture->destination_height);
                for (int x = 0; x < picture->destination_width; ++x) {
                    const int destination_x = target->common.x + picture->destination_x + x;
                    const int destination_y = target->common.y + picture->destination_y + y;
                    if (destination_x < 0 || destination_y < 0 ||
                        destination_x >= static_cast<int>(surface_.width) ||
                        destination_y >= static_cast<int>(surface_.height))
                        continue;
                    const auto source_x = static_cast<std::size_t>(x) * picture->source_width /
                                          static_cast<std::size_t>(picture->destination_width);
                    const auto source = (source_y * picture->width + source_x) * 3U;
                    const auto destination =
                        (static_cast<std::size_t>(destination_y) * surface_.width +
                         static_cast<std::size_t>(destination_x)) *
                        3U;
                    std::copy_n(
                        picture->rgb.begin() + static_cast<std::ptrdiff_t>(source),
                        3,
                        surface_.rgb.begin() + static_cast<std::ptrdiff_t>(destination)
                    );
                }
            }
        }
    }
    if (screen_ == Screen::map_selection && modal_parent_surface_.width == kCanvasWidth &&
        modal_parent_surface_.height == kCanvasHeight) {
        auto composed = modal_parent_surface_;
        const auto& modal_root = resources_.layout.gadgets.front().common;
        const auto modal_width = static_cast<uint32_t>(modal_root.width);
        const auto modal_height = static_cast<uint32_t>(modal_root.height);
        const int offset_x = (kCanvasWidth - static_cast<int>(modal_width)) / 2;
        const int offset_y = (kCanvasHeight - static_cast<int>(modal_height)) / 2;
        for (uint32_t y = 0; y < modal_height; ++y) {
            for (uint32_t x = 0; x < modal_width; ++x) {
                const auto source = (static_cast<std::size_t>(y) * surface_.width + x) * 3U;
                const auto destination =
                    (static_cast<std::size_t>(offset_y + static_cast<int>(y)) * composed.width +
                     static_cast<std::size_t>(offset_x + static_cast<int>(x))) *
                    3U;
                std::copy_n(
                    surface_.rgb.begin() + static_cast<std::ptrdiff_t>(source),
                    3,
                    composed.rgb.begin() + static_cast<std::ptrdiff_t>(destination)
                );
            }
        }
        surface_ = std::move(composed);
    }
    if (screen_ == Screen::campaign_end)
        draw_campaign_end_title();
    if (panel_over_screen())
        compose_panel_over_parent();
    draw_screen_packages();
    if (!end_screen_hides_cursor())
        tick_and_draw_cursor();
}

} // namespace oa::app

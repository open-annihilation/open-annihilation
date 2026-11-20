// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A second test extension, built on the recorder: its library links the
// recorder's, so the game lists it after the recorder whatever order the
// two were registered in. It fills every hook of the extension table but
// frontend_game and frontend_states, which one extension at most may fill,
// and counts each call in the recorder's counts under "follower.<hook>".
// It answers as a null hook does, except that its usage note replaces the
// engine's, so the game behaves as it does with the recorder alone.
//
// Where the combined table must call it in a set order beside the
// recorder, it checks the recorder's counts and counts
// "follower.order <hook>" when the order holds, or
// "follower.misordered <hook>" when it does not: its startup after the
// recorder's, its shutdown and release_runtime before, and its take_option,
// run_mode, return_label and open_recording each before the recorder's.
//
// It includes extension.hpp alone, never runtime.hpp: an extension that
// adds no Runtime members needs no more.
#include "recorder.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

using namespace oa::app;
using hook_recorder::calls;
using hook_recorder::record;

// The follower's usage note, which --help prints in place of the engine's.
constexpr const char* kUsageNote = "The recorder test extensions record every hook's calls.\n";
// The environment variable that has the follower fill frontend_game too.
constexpr const char* kDoubleFrontendGame = "OA_RECORDER_FOLLOWER_FRONTEND_GAME";

/// Counts one of the follower's calls.
///
/// @param hook the hook's name
/// @param detail the enumerator it was given; null for none
void count(const char* hook, const char* detail = nullptr) {
    record((std::string("follower.") + hook).c_str(), detail);
}

/// Counts whether the combined table called the follower in the order it
/// must, beside the recorder.
///
/// @param hook the hook's name
/// @param in_order whether the order held
void count_order(const char* hook, bool in_order) {
    record(in_order ? "follower.order" : "follower.misordered", hook);
}

/// Returns the name of a RunPhase.
///
/// @param phase the phase
/// @return its enumerator's name
const char* phase_name(RunPhase phase) {
    switch (phase) {
    case RunPhase::start:
        return "start";
    case RunPhase::headless_first:
        return "headless_first";
    case RunPhase::headless:
        return "headless";
    }
    return "unknown";
}

/// Returns the name of a FrameStage.
///
/// @param stage the stage
/// @return its enumerator's name
const char* stage_name(FrameStage stage) {
    switch (stage) {
    case FrameStage::pump:
        return "pump";
    case FrameStage::after_pump:
        return "after_pump";
    case FrameStage::presented:
        return "presented";
    }
    return "unknown";
}

void fill(Extension* table) {
    table->take_option = [](void*, const char*, const OptionValues&, uint32_t&) {
        count("take_option");
        // Asked first: the recorder has been asked once for each earlier option.
        count_order("take_option", calls("take_option") + 1 == calls("follower.take_option"));
        return false;
    };
    table->check_options = [](void*) { count("check_options"); };
    table->switch_handler = [](void*) -> const command_line::SwitchHandler* {
        count("switch_handler");
        return nullptr;
    };
    table->text = [](void*, ExtensionText which) -> const char* {
        count("text");
        return which == ExtensionText::usage_note ? kUsageNote : nullptr;
    };
    table->startup = [](void*, Runtime&) {
        count("startup");
        count_order("startup", calls("startup") == 1);
    };
    table->register_screens = [](void*, ScreenRegistry*) { count("register_screens"); };
    table->register_settings_pages = [](void*, SettingsRegistry*) {
        count("register_settings_pages");
    };
    table->ready = [](void*, Runtime&) { count("ready"); };
    table->frontend_entry = [](void*, FrontendEntry&) { count("frontend_entry"); };
    table->run_mode = [](void*, Runtime&, RunPhase phase, int&) {
        count("run_mode", phase_name(phase));
        count_order(
            "run_mode",
            calls("run_mode", phase_name(phase)) + 1 ==
                calls("follower.run_mode", phase_name(phase))
        );
        return false;
    };
    table->start_scene = [](void*, Runtime&) {
        count("start_scene");
        return false;
    };
    table->shutdown = [](void*, Runtime&) {
        count("shutdown");
        count_order("shutdown", calls("shutdown") == 0);
    };
    table->select_multiplayer = [](void*, Runtime&) {
        count("select_multiplayer");
        return MultiplayerSelection::unavailable;
    };
    if (const char* value = std::getenv(kDoubleFrontendGame);
        value != nullptr && std::strcmp(value, "1") == 0)
        table->frontend_game = [](void*) -> oa::Game* {
            std::abort(); // the start is refused first
        };
    table->keep_stored_password = [](void*) {
        count("keep_stored_password");
        return false;
    };
    table->check_multiplayer_menu = [](void*, Runtime&) { count("check_multiplayer_menu"); };
    table->state = [](void*, const Runtime&) -> uint32_t {
        count("state");
        return 0;
    };
    table->frame = [](void*, Runtime&, FrameStage stage) { count("frame", stage_name(stage)); };
    table->simulation_step = [](void*, Runtime&) {
        count("simulation_step");
        return false;
    };
    table->outcome_ready = [](void*, Runtime&) {
        count("outcome_ready");
        return true;
    };
    table->match_game = [](void*, oa::Game&) { count("match_game"); };
    table->match_event = [](void*, Runtime&, MatchEvent) { count("match_event"); };
    table->disconnect_text = [](void*, uint8_t) -> const char* {
        count("disconnect_text");
        return nullptr;
    };
    table->give_resources = [](void*, Runtime&, uint8_t, uint8_t, float, bool) {
        count("give_resources");
        return false;
    };
    table->message_hooks = [](void*, Runtime&, oa::sim::messages::Hooks&) {
        count("message_hooks");
    };
    table->player_gone = [](void*, Runtime&, oa::World&, const oa::Player&) {
        count("player_gone");
        return false;
    };
    table->console_host = [](void*, Runtime&, oa::ui::console::ConsoleHost&) {
        count("console_host");
    };
    table->check_console = [](void*, Runtime&, void (*)(void* user, const char* line), void*) {
        count("check_console");
    };
    table->draw_loading = [](void*, Runtime&, oa::Surface&, const oa::present::GafSprites*) {
        count("draw_loading");
    };
    table->draw_match_hud = [](void*, Runtime&) { count("draw_match_hud"); };
    table->draw_match_overlay = [](void*, Runtime&, const MatchOverlay&) {
        count("draw_match_overlay");
    };
    table->pause_changed = [](void*, Runtime&, bool) { count("pause_changed"); };
    table->load_progress = [](void*, Runtime&, const uint8_t*, size_t) { count("load_progress"); };
    table->team_panel_host = [](void*, Runtime&, oa::ui::hud::TeamPanelHost&) {
        count("team_panel_host");
    };
    table->close_requested = [](void*, Runtime&) {
        count("close_requested");
        return false;
    };
    table->return_label = [](void*) -> const char* {
        count("return_label");
        count_order("return_label", calls("return_label") + 1 == calls("follower.return_label"));
        return nullptr;
    };
    table->speed_changed = [](void*, Runtime&, uint16_t) { count("speed_changed"); };
    table->app_mode_set = [](void*, Runtime&, int32_t) { count("app_mode_set"); };
    table->open_recording =
        [](void*, Runtime&, const RecordingInput&, ReplayHooks&, RecordingInfo&) {
            count("open_recording");
            count_order(
                "open_recording", calls("open_recording") + 1 == calls("follower.open_recording")
            );
            return false;
        };
    table->release_runtime = [](void*, Runtime&) {
        count("release_runtime");
        count_order("release_runtime", calls("release_runtime") == 0);
    };
    table->replay_frame = [](void*, Runtime&, uint32_t, const ReplayFrameHost&) {
        count("replay_frame");
    };
    table->replay_hold = [](void*, Runtime&, uint32_t) { count("replay_hold"); };
    table->replay_play = [](void*, Runtime&) { count("replay_play"); };
    table->replay_pause = [](void*, Runtime&) { count("replay_pause"); };
    table->replay_set_speed = [](void*, Runtime&, int32_t) { count("replay_set_speed"); };
    table->replay_step_speed = [](void*, Runtime&, int32_t) { count("replay_step_speed"); };
    table->replay_seek = [](void*, Runtime&, uint32_t) { count("replay_seek"); };
    // A null hook fills nothing and lets the next extension answer.
    table->replay_state = [](void*, Runtime&, ReplayPlaybackView*) {
        count("replay_state");
        return false;
    };
}

} // namespace

void oa_extension_init_recorder_follower(oa::app::Extension* table) {
    fill(table);
}

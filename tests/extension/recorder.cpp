// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A test extension that fills every hook of oa-game's extension table with
// a recorder, but frontend_game and frontend_states: one extension at most
// may fill those, and network play, which the engine registers in every
// build, fills both. The game lists the recorder after network play. Each
// call is counted, under the hook's name and, for a hook that takes one,
// the enumerator it was given; every answer is the one a null hook stands
// for, so the game behaves as it does with network play alone. When
// oa-game exits, the counts go to the file --record-hooks names, one
// "<hook>[ <enumerator>] <count>" line each in name order, together with
// the follower's (follower.cpp), whose hooks start with "follower.".
//
// The recorder registers one Settings page, Recorder, whose one toggle is
// bound to recorder.toggle, from register_settings_pages; with
// --record-refused-page it gives the page an id the registry refuses, and
// the start stops.
//
// --check-multiplayer-menu runs network play's check of the multiplayer
// screens first; the recorder's check_multiplayer_menu then says that it
// ran. The recorder also adds one member to Runtime
// (recorder_runtime_members.hpp) and calls it through RecorderExtension, the
// friend, as the extension table's startup hook runs.
//
// --record-quit STATUS reaches the screen services an extension keeps: an
// overlay with nothing to draw keeps the host and services its create is
// given, and on the fifth frame the recorder stops the sounds, plays BGM on
// the alternate route and asks for a frontend pass, and on the tenth ends
// the run through quit with STATUS. Without the option none of this runs.
//
// The check host (check_host.hpp) is driven as a check an extension runs
// drives it, through its entries and runtime_options alone: after network
// play's --check-multiplayer-menu check, a round of the main menu clicks
// MULTI twice, which the recorder takes over (select_multiplayer answers
// taken during the round) so that the main menu stays up; and with
// --record-check-host the recorder takes the headless run (run_mode
// headless) for the entries that work without a window.
#include "recorder.hpp"

#include "oa/app/check_host.hpp"
#include "oa/app/runtime.hpp"
#include "oa/ui/kit/rows.hpp"
#include "oa/ui/screen_registry/kit.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <stdexcept>
#include <string>
#include <string_view>

namespace oa::app {

uint32_t Runtime::count_recorder_member_call() {
    return ++recorder_member_calls_;
}

namespace {

namespace kit = oa::ui::kit;

// The version of the extension table's contract the recorder follows, and
// the hooks the table holds after its context at that version. A change to
// the table raises OA_EXTENSION_API_VERSION (extension.hpp); both follow it.
// Version 14 adds only functions an extension calls rather than hooks, so the
// hook count is unchanged. Version 15 adds register_settings_pages, which the
// recorder records and registers its own page from. Version 16 adds
// run_mutators, a function an extension calls, so the hook count is unchanged.
// Version 17 adds hear_remote_seat_chat, also a function an extension calls,
// so the hook count is unchanged. Version 18 adds the kit's key pictures,
// not a hook, so the hook count is unchanged.
constexpr uint32_t kExtensionApiVersionRecorded = 18;
constexpr std::size_t kHookCount = 41;
static_assert(
    extension_api_version == kExtensionApiVersionRecorded,
    "the extension table's contract changed: record every hook here, "
    "then raise kExtensionApiVersionRecorded"
);
static_assert(
    sizeof(Extension) == sizeof(void*) * (1 + kHookCount),
    "the extension table's hooks changed: record every one of them here "
    "and raise OA_EXTENSION_API_VERSION"
);

// The option that names the file the counts go to.
constexpr std::string_view kRecordOption = "--record-hooks";
// The option that ends the run through ScreenServices::quit with a status.
constexpr std::string_view kQuitOption = "--record-quit";
// The option that has the recorder take the headless run for the check host.
constexpr std::string_view kCheckHostOption = "--record-check-host";
// The option that has the recorder register a Settings page the registry
// refuses, which stops the start.
constexpr std::string_view kRefusedPageOption = "--record-refused-page";
// The after_pump frames --record-quit waits before the sound and frontend
// services, and before quit.
constexpr uint64_t kServicesFrame = 5;
constexpr uint64_t kQuitFrame = 10;

struct Recorder {
    std::string path{};                       // --record-hooks FILE; empty writes nothing
    std::map<std::string, uint64_t> counts{}; // by "<hook>[ <enumerator>]"
    bool taking_multi{};                      // a check host round takes MULTI over
    bool quit{};                              // --record-quit was given
    int quit_status{};                        // its STATUS
    uint64_t after_pump_frames{};             // frames seen at FrameStage::after_pump
    void* host{};                             // ScreenContext::host, kept from the overlay
    const ScreenServices* services{};         // ScreenContext::services, kept likewise
    bool check_host{};                        // --record-check-host was given
    bool refused_page{};                      // --record-refused-page was given
};

/// Returns the recorder of this process.
///
/// @return the recorder, which lives until the process exits
Recorder& recorder() {
    static Recorder state;
    return state;
}

/// Returns the key a hook's calls are counted under.
///
/// @param hook the hook's name
/// @param detail the enumerator it was given; null for none
/// @return "<hook>[ <detail>]"
std::string record_key(const char* hook, const char* detail) {
    std::string key = hook;
    if (detail != nullptr)
        key += std::string(" ") + detail;
    return key;
}

using hook_recorder::record;

/// Writes the counts to the file --record-hooks named; nothing without one.
void write_record() {
    const auto& state = recorder();
    if (state.path.empty())
        return;
    std::FILE* file = std::fopen(state.path.c_str(), "w");
    if (file == nullptr) {
        std::fprintf(stderr, "recorder: cannot write %s\n", state.path.c_str());
        return;
    }
    for (const auto& [key, count] : state.counts)
        std::fprintf(file, "%s %llu\n", key.c_str(), static_cast<unsigned long long>(count));
    std::fclose(file);
}

/// Returns the name of an ExtensionText.
///
/// @param which the text
/// @return its enumerator's name
const char* text_name(ExtensionText which) {
    switch (which) {
    case ExtensionText::usage_checks:
        return "usage_checks";
    case ExtensionText::usage_runs:
        return "usage_runs";
    case ExtensionText::usage_switches:
        return "usage_switches";
    case ExtensionText::usage_note:
        return "usage_note";
    case ExtensionText::register_switch:
        return "register_switch";
    }
    return "unknown";
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

/// Returns the name of a MatchEvent.
///
/// @param event the event
/// @return its enumerator's name
const char* event_name(MatchEvent event) {
    switch (event) {
    case MatchEvent::finished:
        return "finished";
    case MatchEvent::torn_down:
        return "torn_down";
    case MatchEvent::left:
        return "left";
    case MatchEvent::results_reported:
        return "results_reported";
    case MatchEvent::results_released:
        return "results_released";
    case MatchEvent::watching_kept:
        return "watching_kept";
    }
    return "unknown";
}

/// Takes --record-hooks FILE, --record-quit STATUS and --record-check-host and no other
/// option (Extension::take_option).
///
/// @param context Extension::context (unused)
/// @param name the option as given
/// @param values takes the arguments after it
/// @param[out] effects option_effect bits; left 0
/// @return true for --record-hooks, --record-quit, --record-check-host and
///         --record-refused-page
bool take_option(
    void* /*context*/, const char* name, const OptionValues& values, uint32_t& /*effects*/
) {
    record("take_option");
    if (std::string_view(name) == kRecordOption) {
        recorder().path = values.next(values.arguments);
        return true;
    }
    if (std::string_view(name) == kQuitOption) {
        recorder().quit = true;
        recorder().quit_status = std::atoi(values.next(values.arguments));
        return true;
    }
    if (std::string_view(name) == kCheckHostOption) {
        recorder().check_host = true;
        return true;
    }
    if (std::string_view(name) == kRefusedPageOption) {
        recorder().refused_page = true;
        return true;
    }
    return false;
}

/// Counts the check of the options (Extension::check_options).
///
/// @param context Extension::context (unused)
void check_options(void* /*context*/) {
    record("check_options");
}

/// Refuses the reserved game switches (Extension::switch_handler).
///
/// @param context Extension::context (unused)
/// @return null
const oa::app::command_line::SwitchHandler* switch_handler(void* /*context*/) {
    record("switch_handler");
    return nullptr;
}

/// Keeps the engine's wording (Extension::text).
///
/// @param context Extension::context (unused)
/// @param which the text asked for
/// @return null
const char* text(void* /*context*/, ExtensionText which) {
    record("text", text_name(which));
    return nullptr;
}

/// Keeps the host and services an overlay's create is given.
///
/// @param ctx the overlay's context
void keep_services(ScreenContext* ctx, void* /*state*/) {
    recorder().host = ctx->host;
    recorder().services = ctx->services;
}

/// Counts the registration of the screens; with --record-quit, registers an
/// overlay that only keeps the services (Extension::register_screens).
///
/// @param context Extension::context (unused)
/// @param[in,out] registry the runtime's registry; left as it is without
///        --record-quit
void register_screens(void* /*context*/, ScreenRegistry* registry) {
    record("register_screens");
    if (!recorder().quit)
        return;
    OverlayDesc overlay{};
    overlay.name = "recorder services";
    overlay.screen = kScreenAny;
    overlay.create = keep_services;
    (void)overlay_register(registry, &overlay);
}

/// Returns the recorder's Settings page's rows: one toggle bound to
/// recorder.toggle.
///
/// @return the rows, which live until the process exits
const kit::PreferenceRow* recorder_page_rows() {
    static const kit::PreferenceRow rows[] = {
        kit::preference_toggle("toggle", "Recorder toggle", "recorder.toggle", "0"),
    };
    return rows;
}

/// Counts the registration of the Settings pages and registers the
/// recorder's own page, whose one toggle is bound to recorder.toggle; with
/// --record-refused-page, under an id the registry refuses
/// (Extension::register_settings_pages).
///
/// @param context Extension::context (unused)
/// @param[in,out] registry the runtime's Settings pages
void register_settings_pages(void* /*context*/, SettingsRegistry* registry) {
    record("register_settings_pages");
    SettingsPageDesc page{};
    page.id = recorder().refused_page ? "Recorder options" : "recorder.options";
    page.title = "Recorder";
    page.owner = "Recorder";
    page.rows = recorder_page_rows();
    page.row_count = 1;
    // A page the registry refuses stops the start once this returns.
    (void)settings_page_register(registry, page);
}

/// Uses the kept screen services on the frames --record-quit names.
void use_services() {
    auto& state = recorder();
    if (!state.quit || state.services == nullptr)
        return;
    ++state.after_pump_frames;
    if (state.after_pump_frames == kServicesFrame) {
        state.services->stop_sounds(state.host);
        state.services->play_sound_alternate(state.host, "BGM");
        state.services->run_frontend(state.host);
        std::printf("recorder: stop_sounds, play_sound_alternate and run_frontend\n");
    } else if (state.after_pump_frames == kQuitFrame) {
        std::printf("recorder: quit %d\n", state.quit_status);
        std::fflush(stdout);
        state.services->quit(state.host, nullptr, state.quit_status);
    }
}

/// Leaves the frontend's launch values to the preferences (Extension::frontend_entry).
///
/// @param context Extension::context (unused)
/// @param[out] entry the values; left null
void frontend_entry(void* /*context*/, FrontendEntry& /*entry*/) {
    record("frontend_entry");
}

/// Leaves MULTI to network play, but takes it over during a check host round
/// (Extension::select_multiplayer).
///
/// @param context Extension::context (unused)
/// @param[in,out] runtime the running app; left as it is
/// @return taken during a check host round of the main menu; unavailable otherwise
MultiplayerSelection select_multiplayer(void* /*context*/, Runtime& /*runtime*/) {
    record("select_multiplayer");
    return recorder().taking_multi ? MultiplayerSelection::taken
                                   : MultiplayerSelection::unavailable;
}

/// Leaves the preferences write to write the password (Extension::keep_stored_password).
///
/// @param context Extension::context (unused)
/// @return false
bool keep_stored_password(void* /*context*/) {
    record("keep_stored_password");
    return false;
}

/// Leaves a new match's Game block to the engine (Extension::match_game).
///
/// @param context Extension::context (unused)
/// @param[in,out] game the new match's Game block; left as it is
void match_game(void* /*context*/, oa::Game& /*game*/) {
    record("match_game");
}

/// Shows no disconnect text (Extension::disconnect_text).
///
/// @param context Extension::context (unused)
/// @param reason the local player's reject reason (unused)
/// @return null
const char* disconnect_text(void* /*context*/, uint8_t /*reason*/) {
    record("disconnect_text");
    return nullptr;
}

/// Gives no return label (Extension::return_label).
///
/// @param context Extension::context (unused)
/// @return null
const char* return_label(void* /*context*/) {
    record("return_label");
    return nullptr;
}

// The sound the main menu's big buttons play, and the file allsound.tdf
// gives it.
constexpr const char* kBigButtonSound = "BIGBUTTON";
constexpr std::string_view kBigButtonFile = "sounds/butmain1.wav";
// Frames a click leaves the menus to settle in.
constexpr int kSettleFrames = 4;
// The clock is held this far ahead of real time.
constexpr uint32_t kHeldTicks = 1000;

/// Fails a check host round unless a condition holds.
///
/// @param condition the condition
/// @param what the failure, thrown as "recorder check host: <what>"
void require(bool condition, const std::string& what) {
    if (!condition)
        throw std::runtime_error("recorder check host: " + what);
}

// A canvas point.
struct Point {
    int32_t x{};
    int32_t y{};
};

/// Returns the centre of a gadget of the layout shown, found by its name.
///
/// @param host the check host
/// @param name the gadget's name
/// @return its centre; the round fails when the layout has none
Point gadget_centre(const CheckHost& host, const char* name) {
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
    require(
        host.gadget(host.context, name, &x, &y, &width, &height),
        std::string("the layout has no ") + name
    );
    return {x + width / 2, y + height / 2};
}

/// Checks that the main menu is shown, its frame and input the frontend's own.
///
/// @param host the check host
/// @param when the step, for the failure
void require_main_menu(const CheckHost& host, const std::string& when) {
    require(
        host.screen(host.context) == screen_id(Screen::main_menu) &&
            host.frontend_state(host.context) == frontend::state_id::main_menu &&
            !host.frame_owned_by_package(host.context),
        when + ": the main menu is not shown"
    );
}

/// Checks the entries that read the game's files and sounds and write its
/// preferences.
///
/// @param host the check host
/// @param options the game's options
void check_files(const CheckHost& host, const Options& options) {
    char file[64]{};
    require(
        host.sound_resource(host.context, kBigButtonSound, file, sizeof file) &&
            std::string_view(file) == kBigButtonFile,
        std::string(kBigButtonSound) + " does not play " + std::string(kBigButtonFile)
    );
    char small[4]{};
    require(
        !host.sound_resource(host.context, kBigButtonSound, small, sizeof small),
        "a sound's file is handed back where it does not fit"
    );
    const oa::AssetStore* assets = host.assets(host.context);
    require(
        assets != nullptr && assets->file_size(file) > 0, "the store has no " + std::string(file)
    );
    host.write_preferences(host.context);
    require(
        options.preferences_file && std::filesystem::is_regular_file(*options.preferences_file),
        "the preferences were not written"
    );
}

/// Checks that the clock holds where it is held, and follows real time once
/// released.
///
/// @param host the check host
void check_clock(const CheckHost& host) {
    const uint32_t held = host.clock(host.context) + kHeldTicks;
    host.hold_clock(host.context, held);
    require(host.clock(host.context) == held, "the held clock moved");
    host.release_clock(host.context);
    require(host.clock(host.context) < held, "the released clock stays held");
}

/// Checks a composed frame of the frontend: 640x480.
///
/// @param host the check host
void check_composed_frame(const CheckHost& host) {
    host.package_pass(host.context);
    host.compose(host.context);
    const renderer::Surface* frame = host.surface(host.context);
    require(
        frame != nullptr && frame->width == static_cast<uint32_t>(kCanvasWidth) &&
            frame->height == static_cast<uint32_t>(kCanvasHeight) &&
            frame->rgb.size() == static_cast<std::size_t>(kCanvasWidth) * kCanvasHeight * 3U,
        "the composed frame is not the 640x480 canvas"
    );
}

/// Clicks a canvas point through the check host: a motion, a press and a
/// release, each with a frame after it, then the frames the menus settle in.
///
/// @param host the check host
/// @param at the point
void click(const CheckHost& host, Point at) {
    for (const uint32_t type :
         {static_cast<uint32_t>(SDL_EVENT_MOUSE_MOTION),
          static_cast<uint32_t>(SDL_EVENT_MOUSE_BUTTON_DOWN),
          static_cast<uint32_t>(SDL_EVENT_MOUSE_BUTTON_UP)}) {
        require(host.pointer(host.context, type, at.x, at.y, 1), "a click ended the run");
        host.frame(host.context);
    }
    for (int frame = 0; frame < kSettleFrames; ++frame)
        host.frame(host.context);
}

/// Drives a round of the main menu through the check host, with the window
/// up: MULTI clicked twice asks the extensions (select_multiplayer) each
/// time, and the recorder takes it over, so the main menu stays up. The
/// cursor follows the pointer, the clock holds, and the files, sounds and
/// preferences entries answer.
///
/// @param runtime the running game, on the main menu with its window up
void check_host_menu_round(Runtime& runtime) {
    const CheckHost host = check_host(runtime);
    require(host.window_id(host.context) != 0, "the window has no id");
    require(
        SDL_GetWindowFromID(host.window_id(host.context)) != nullptr,
        "SDL knows no window by the host's id"
    );
    require_main_menu(host, "the start");
    const Point multi = gadget_centre(host, "multi");
    const uint64_t asked = hook_recorder::calls("select_multiplayer");
    recorder().taking_multi = true;
    click(host, multi);
    int32_t cursor_x = 0;
    int32_t cursor_y = 0;
    host.cursor(host.context, &cursor_x, &cursor_y);
    require(
        std::abs(cursor_x - multi.x) <= 1 && std::abs(cursor_y - multi.y) <= 1,
        "the cursor is at (" + std::to_string(cursor_x) + ", " + std::to_string(cursor_y) +
            "), not on MULTI"
    );
    require(hook_recorder::calls("select_multiplayer") == asked + 1, "MULTI was not asked");
    require_main_menu(host, "MULTI");
    click(host, multi);
    recorder().taking_multi = false;
    require(
        hook_recorder::calls("select_multiplayer") == asked + 2, "a second MULTI was not asked"
    );
    require_main_menu(host, "the second MULTI");
    check_clock(host);
    check_composed_frame(host);
    check_files(host, runtime_options(runtime));
    std::printf(
        "recorder: check host, windowed: MULTI clicked twice, taken over by the recorder\n"
    );
}

/// Drives the entries that work without a window through the check host,
/// in the headless run: the main menu and its gadgets, a motion that moves
/// the cursor to the canvas point, the clock, a composed frame, the files,
/// sounds and preferences, a close request, which ends the run, and a frame,
/// which needs the window.
///
/// @param runtime the running game, headless on the main menu
void check_host_headless(Runtime& runtime) {
    const CheckHost host = check_host(runtime);
    require(host.window_id(host.context) == 0, "a headless run has a window id");
    require_main_menu(host, "the start");
    const Point multi = gadget_centre(host, "MULTI");
    require(
        host.pointer(host.context, SDL_EVENT_MOUSE_MOTION, multi.x, multi.y, 0),
        "a motion ended the run"
    );
    int32_t cursor_x = 0;
    int32_t cursor_y = 0;
    host.cursor(host.context, &cursor_x, &cursor_y);
    require(cursor_x == multi.x && cursor_y == multi.y, "the cursor did not follow the motion");
    check_clock(host);
    check_composed_frame(host);
    check_files(host, runtime_options(runtime));
    SDL_Event close{};
    close.type = SDL_EVENT_WINDOW_CLOSE_REQUESTED;
    require(!host.dispatch(host.context, &close), "a close request did not end the run");
    bool refused = false;
    try {
        host.frame(host.context);
    } catch (const std::runtime_error&) {
        refused = true;
    }
    require(refused, "a frame ran without the window");
    std::printf(
        "recorder: check host, headless: the main menu, a motion, the clock, a frame "
        "composed, the files and a close request\n"
    );
}

} // namespace

void hook_recorder::record(const char* hook, const char* detail) {
    ++recorder().counts[record_key(hook, detail)];
}

uint64_t hook_recorder::calls(const char* hook, const char* detail) {
    const auto& counts = recorder().counts;
    const auto found = counts.find(record_key(hook, detail));
    return found != counts.end() ? found->second : 0;
}

// The recorder's hooks that take the runtime.
struct RecorderExtension {
    /// Counts the start and calls the recorder's Runtime member (Extension::startup).
    ///
    /// @param context Extension::context (unused)
    /// @param[in,out] runtime the runtime being built
    static void startup(void* /*context*/, Runtime& runtime) {
        record("startup");
        const uint32_t calls = runtime.count_recorder_member_call();
        if (calls == runtime.recorder_member_calls_)
            record("runtime_member");
    }

    /// Counts the end of the start (Extension::ready).
    ///
    /// @param context Extension::context (unused)
    /// @param[in,out] runtime the runtime being built; left as it is
    static void ready(void* /*context*/, Runtime& /*runtime*/) { record("ready"); }

    /// Runs nothing of its own, but with --record-check-host takes the headless
    /// run for the check host's entries that work without a window (Extension::run_mode).
    ///
    /// @param context Extension::context (unused)
    /// @param[in,out] runtime the running app; driven through the check host
    ///        when the recorder takes the run
    /// @param phase the point of Runtime::run
    /// @param[out] exit_code oa-game's exit status; 0 when the recorder takes the run
    /// @return true when the recorder took the run
    static bool run_mode(void* /*context*/, Runtime& runtime, RunPhase phase, int& exit_code) {
        record("run_mode", phase_name(phase));
        if (!recorder().check_host || phase != RunPhase::headless)
            return false;
        check_host_headless(runtime);
        exit_code = 0;
        return true;
    }

    /// Leaves the first scene to the menu music (Extension::start_scene).
    ///
    /// @param context Extension::context (unused)
    /// @param[in,out] runtime the running app; left as it is
    /// @return false
    static bool start_scene(void* /*context*/, Runtime& /*runtime*/) {
        record("start_scene");
        return false;
    }

    /// Counts the end of the main loop (Extension::shutdown).
    ///
    /// @param context Extension::context (unused)
    /// @param[in,out] runtime the running app; left as it is
    static void shutdown(void* /*context*/, Runtime& /*runtime*/) { record("shutdown"); }

    /// Counts a runtime being destroyed (Extension::release_runtime).
    ///
    /// @param context Extension::context (unused)
    /// @param runtime the runtime being destroyed; left as it is
    static void release_runtime(void* /*context*/, Runtime& /*runtime*/) {
        record("release_runtime");
    }

    /// Says that --check-multiplayer-menu ran, after network play's check of
    /// the multiplayer screens, then runs a round of the main menu through the
    /// check host, whose clicks on MULTI reach select_multiplayer
    /// (Extension::check_multiplayer_menu).
    ///
    /// @param context Extension::context (unused)
    /// @param[in,out] runtime the running app, whose main menu the check drives
    static void check_multiplayer_menu(void* /*context*/, Runtime& runtime) {
        record("check_multiplayer_menu");
        std::printf("recorder: --check-multiplayer-menu\n");
        check_host_menu_round(runtime);
    }

    /// Reports no session of its own (Extension::state).
    ///
    /// @param context Extension::context (unused)
    /// @param runtime the running app (unused)
    /// @return 0
    static uint32_t state(void* /*context*/, const Runtime& /*runtime*/) {
        record("state");
        return 0;
    }

    /// Counts a frame stage (Extension::frame).
    ///
    /// @param context Extension::context (unused)
    /// @param[in,out] runtime the running app; left as it is
    /// @param stage the point of the frame
    static void frame(void* /*context*/, Runtime& /*runtime*/, FrameStage stage) {
        record("frame", stage_name(stage));
        if (stage == FrameStage::after_pump)
            use_services();
    }

    /// Leaves the simulation step to the engine (Extension::simulation_step).
    ///
    /// @param context Extension::context (unused)
    /// @param[in,out] runtime the running app; left as it is
    /// @return false
    static bool simulation_step(void* /*context*/, Runtime& /*runtime*/) {
        record("simulation_step");
        return false;
    }

    /// Lets the finished match move on (Extension::outcome_ready).
    ///
    /// @param context Extension::context (unused)
    /// @param[in,out] runtime the running app; left as it is
    /// @return true
    static bool outcome_ready(void* /*context*/, Runtime& /*runtime*/) {
        record("outcome_ready");
        return true;
    }

    /// Counts a match event (Extension::match_event).
    ///
    /// @param context Extension::context (unused)
    /// @param[in,out] runtime the running app; left as it is
    /// @param event what happened
    static void match_event(void* /*context*/, Runtime& /*runtime*/, MatchEvent event) {
        record("match_event", event_name(event));
    }

    /// Leaves a console gift to the engine (Extension::give_resources).
    ///
    /// @param context Extension::context (unused)
    /// @param[in,out] runtime the running app; left as it is
    /// @param from the giving player's index (unused)
    /// @param to the receiving player's index (unused)
    /// @param amount the amount given (unused)
    /// @param metal true for metal, false for energy (unused)
    /// @return false
    static bool give_resources(
        void* /*context*/,
        Runtime& /*runtime*/,
        uint8_t /*from*/,
        uint8_t /*to*/,
        float /*amount*/,
        bool /*metal*/
    ) {
        record("give_resources");
        return false;
    }

    /// Leaves the message log's hooks to the engine (Extension::message_hooks).
    ///
    /// @param context Extension::context (unused)
    /// @param[in,out] runtime the running app; left as it is
    /// @param[in,out] hooks the hooks being built; left as the engine filled them
    static void message_hooks(
        void* /*context*/, Runtime& /*runtime*/, oa::sim::messages::Hooks& /*hooks*/
    ) {
        record("message_hooks");
    }

    /// Leaves the announcement to the engine (Extension::player_gone).
    ///
    /// @param context Extension::context (unused)
    /// @param[in,out] runtime the running app; left as it is
    /// @param[in,out] world the match world; left as it is
    /// @param player the player who lost its last unit (unused)
    /// @return false
    static bool player_gone(
        void* /*context*/, Runtime& /*runtime*/, oa::World& /*world*/, const oa::Player& /*player*/
    ) {
        record("player_gone");
        return false;
    }

    /// Leaves the console's host to the engine (Extension::console_host).
    ///
    /// @param context Extension::context (unused)
    /// @param[in,out] runtime the running app; left as it is
    /// @param[in,out] host the console's host; left as the engine filled it
    static void console_host(
        void* /*context*/, Runtime& /*runtime*/, oa::ui::console::ConsoleHost& /*host*/
    ) {
        record("console_host");
    }

    /// Types no console line of its own (Extension::check_console).
    ///
    /// @param context Extension::context (unused)
    /// @param[in,out] runtime the running app; left as it is
    /// @param enter_line types one console line (unused)
    /// @param user the first argument of enter_line (unused)
    static void check_console(
        void* /*context*/,
        Runtime& /*runtime*/,
        void (* /*enter_line*/)(void* user, const char* line),
        void* /*user*/
    ) {
        record("check_console");
    }

    /// Draws nothing over the loading screen (Extension::draw_loading).
    ///
    /// @param context Extension::context (unused)
    /// @param[in,out] runtime the running app; left as it is
    /// @param[in,out] target the locked display surface; left as it is
    /// @param font the GUI font, or null (unused)
    static void draw_loading(
        void* /*context*/,
        Runtime& /*runtime*/,
        oa::Surface& /*target*/,
        const oa::present::GafSprites* /*font*/
    ) {
        record("draw_loading");
    }

    /// Draws nothing in the match HUD (Extension::draw_match_hud).
    ///
    /// @param context Extension::context (unused)
    /// @param[in,out] runtime the running app; left as it is
    static void draw_match_hud(void* /*context*/, Runtime& /*runtime*/) {
        record("draw_match_hud");
    }

    /// Draws nothing over the battlefield (Extension::draw_match_overlay).
    ///
    /// @param context Extension::context (unused)
    /// @param[in,out] runtime the running app; left as it is
    /// @param overlay the battlefield and its painter (unused)
    static void draw_match_overlay(
        void* /*context*/, Runtime& /*runtime*/, const MatchOverlay& /*overlay*/
    ) {
        record("draw_match_overlay");
    }

    /// Keeps the pause on this machine (Extension::pause_changed).
    ///
    /// @param context Extension::context (unused)
    /// @param[in,out] runtime the running app; left as it is
    /// @param paused the pause bit after the flip
    static void pause_changed(void* /*context*/, Runtime& /*runtime*/, bool paused) {
        record("pause_changed", paused ? "on" : "off");
    }

    /// Counts a change of the loading screen's rows (Extension::load_progress).
    ///
    /// @param context Extension::context (unused)
    /// @param[in,out] runtime the running app; left as it is
    /// @param rows the loading screen's rows (unused)
    /// @param row_count how many rows `rows` holds (unused)
    static void load_progress(
        void* /*context*/, Runtime& /*runtime*/, const uint8_t* /*rows*/, size_t /*row_count*/
    ) {
        record("load_progress");
    }

    /// Leaves the team panels' host empty (Extension::team_panel_host).
    ///
    /// @param context Extension::context (unused)
    /// @param[in,out] runtime the running app; left as it is
    /// @param[in,out] host the panels' host; left with every entry null
    static void team_panel_host(
        void* /*context*/, Runtime& /*runtime*/, oa::ui::hud::TeamPanelHost& /*host*/
    ) {
        record("team_panel_host");
    }

    /// Leaves a close request to the engine (Extension::close_requested).
    ///
    /// @param context Extension::context (unused)
    /// @param[in,out] runtime the running app; left as it is
    /// @return false
    static bool close_requested(void* /*context*/, Runtime& /*runtime*/) {
        record("close_requested");
        return false;
    }

    /// Keeps a speed the local player set on this machine (Extension::speed_changed).
    ///
    /// @param context Extension::context (unused)
    /// @param[in,out] runtime the running app; left as it is
    /// @param speed the match's game speed now (unused)
    static void speed_changed(void* /*context*/, Runtime& /*runtime*/, uint16_t /*speed*/) {
        record("speed_changed");
    }

    /// Counts an application mode the frontend set (Extension::app_mode_set).
    ///
    /// @param context Extension::context (unused)
    /// @param[in,out] runtime the running app; left as it is
    /// @param mode the mode set (unused)
    static void app_mode_set(void* /*context*/, Runtime& /*runtime*/, int32_t /*mode*/) {
        record("app_mode_set");
    }

    /// Leaves a recording to the other extensions (Extension::open_recording).
    ///
    /// @param context Extension::context (unused)
    /// @param[in,out] runtime the running app; left as it is
    /// @param input the recording (unused)
    /// @param[out] replay the replay; left null
    /// @param[out] info what the recording holds; left zero
    /// @return false
    static bool open_recording(
        void* /*context*/,
        Runtime& /*runtime*/,
        const RecordingInput& /*input*/,
        ReplayHooks& /*replay*/,
        RecordingInfo& /*info*/
    ) {
        record("open_recording");
        return false;
    }
};

} // namespace oa::app

void oa_extension_init_recorder(oa::app::Extension* table) {
    using namespace oa::app;
    table->context = &recorder();
    table->take_option = take_option;
    table->check_options = check_options;
    table->switch_handler = switch_handler;
    table->text = text;
    table->startup = RecorderExtension::startup;
    table->register_screens = register_screens;
    table->register_settings_pages = register_settings_pages;
    table->ready = RecorderExtension::ready;
    table->frontend_entry = frontend_entry;
    table->run_mode = RecorderExtension::run_mode;
    table->start_scene = RecorderExtension::start_scene;
    table->shutdown = RecorderExtension::shutdown;
    table->select_multiplayer = select_multiplayer;
    table->keep_stored_password = keep_stored_password;
    table->check_multiplayer_menu = RecorderExtension::check_multiplayer_menu;
    table->state = RecorderExtension::state;
    table->frame = RecorderExtension::frame;
    table->simulation_step = RecorderExtension::simulation_step;
    table->outcome_ready = RecorderExtension::outcome_ready;
    table->match_game = match_game;
    table->match_event = RecorderExtension::match_event;
    table->disconnect_text = disconnect_text;
    table->give_resources = RecorderExtension::give_resources;
    table->message_hooks = RecorderExtension::message_hooks;
    table->player_gone = RecorderExtension::player_gone;
    table->console_host = RecorderExtension::console_host;
    table->check_console = RecorderExtension::check_console;
    table->draw_loading = RecorderExtension::draw_loading;
    table->draw_match_hud = RecorderExtension::draw_match_hud;
    table->draw_match_overlay = RecorderExtension::draw_match_overlay;
    table->pause_changed = RecorderExtension::pause_changed;
    table->load_progress = RecorderExtension::load_progress;
    table->team_panel_host = RecorderExtension::team_panel_host;
    table->close_requested = RecorderExtension::close_requested;
    table->return_label = return_label;
    table->speed_changed = RecorderExtension::speed_changed;
    table->app_mode_set = RecorderExtension::app_mode_set;
    table->open_recording = RecorderExtension::open_recording;
    table->release_runtime = RecorderExtension::release_runtime;
    // A hook left unset here would fall back to the engine's behaviour
    // unrecorded: stop before anything runs. Network play's own two are
    // left unset.
    uintptr_t words[1 + kHookCount]{};
    std::memcpy(words, table, sizeof words);
    const std::array<std::size_t, 2> network_play_only{
        offsetof(Extension, frontend_states) / sizeof(void*),
        offsetof(Extension, frontend_game) / sizeof(void*)
    };
    for (std::size_t hook = 1; hook <= kHookCount; ++hook) {
        if (hook == network_play_only[0] || hook == network_play_only[1])
            continue;
        if (words[hook] == 0) {
            std::fprintf(stderr, "recorder: hook %zu of the extension table is not set\n", hook);
            std::exit(1);
        }
    }
    std::atexit(write_record);
}

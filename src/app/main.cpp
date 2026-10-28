// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// oa-game entry point: display setup, the Game files screen where the game
// folder is missing and the platform brings game files in, intro playback
// and runtime launch.
#include "oa/app/runtime.hpp"
#include "folder_chooser_screen.hpp"
#include "game_files_check.hpp"
#include "game_files_screen.hpp"
#include "mod_install_watch.hpp"
#include "pad_state.hpp"
#include "render_host.hpp"
#if OA_SELF_CHECKS
#include "renderer_proof.hpp"
#endif
#include "screen_size.hpp"
#include "oa/app/extension_list.hpp"
#include "oa/app/full_screen.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/game_files_hooks.hpp"
#include "oa/app/content_copy.hpp"
#include "oa/app/long_work.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/app/setup_before_game.hpp"
#include "oa/app/disk_image_source.hpp"
#include "oa/app/game_files_import.hpp"
#include "oa/app/input_hints.hpp"
#include "oa/app/package_install.hpp"
#include "oa/app/package_install/handoff.hpp"
#include "oa/app/package_install/inbox.hpp"
#include "oa/app/mod_profile_loader.hpp"
#include "oa/app/mod_summary.hpp"
#include "oa/app/netgame/launch_file.hpp"
#include "map_packs.hpp"
#include "net_options.hpp"
#include "oa/base/text.hpp"
#include "oa/data/campaign/campaign_assets.hpp"
#include "oa/data/campaign/map_catalog.hpp"
#include "oa/data/content_bundles.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/brain_library/library.hpp"
#include "oa/data/mutator_library/library.hpp"
#include "oa/data/mutators/choice.hpp"
#include "oa/data/mutators/pipeline.hpp"
#include "oa/formats/json.hpp"
#include "oa/formats/ota.hpp"
#include "oa/app/platform_hooks.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/app/video_capture.hpp"
#include "oa/app/window_icon.hpp"
#include "oa/app/window_surface.hpp"
#include "oa/base/float_precision.hpp"
#include "oa/base/threads.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/platform/allocator.hpp"
#include "oa/platform/dataless.hpp"
#include "oa/platform/machine.hpp"
#include "oa/platform/memory_status.hpp"
#include "oa/platform/file_types.hpp"
#include "oa/platform/log_files.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/platform/system.hpp"
#include "oa/ui/engine_settings.hpp"
#include "oa/ui/frontend_renderer/game_text.hpp"
#include "oa/ui/gadget_render.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <cstring>
#include <iostream>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <thread>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#include <SDL3/SDL_main.h>

#include "oa_engine_build.hpp"
#include "oa/app/version_line.hpp"

#ifndef OA_ENGINE_VERSION
#error                                                                                             \
    "OA_ENGINE_VERSION names the engine's version, which the native-density record is written under"
#endif
#ifndef OA_NATIVE_DENSITY_WINDOWS
#error "OA_NATIVE_DENSITY_WINDOWS (0 or 1) says whether the window opens at native density"
#endif
#ifndef OA_TOUCH_FIRST
#error "OA_TOUCH_FIRST (0 or 1) says whether the touch controls are on from the start"
#endif

// The status stream's entry points (automation/status.hpp, which says what
// each does).
namespace oa::app::automation {
bool status_stdout_wanted() noexcept;
bool status_following() noexcept;
void capture_status_stdout();
void begin_status_lines(std::string_view version);
void finish_status_lines(int played);
void finish_refused_status_lines(int code);
int status_exit_code(int played) noexcept;
void write_started_output(std::string_view line);
} // namespace oa::app::automation

namespace oa::app {
namespace {

// The platform the game is built for opens its windows at the display's own
// pixel density, where it would otherwise scale a lower-density window
// softly (the OA_NATIVE_DENSITY_WINDOWS build option; off on the desktop).
constexpr bool kNativeDensityWindows = OA_NATIVE_DENSITY_WINDOWS != 0;
// The touch controls are on from the start (the OA_TOUCH_FIRST build option;
// off on the desktop, where the first finger or --touch-controls switches
// them on and the runtime sets their input hints then).
constexpr bool kTouchFirst = OA_TOUCH_FIRST != 0;

// Process exit status after an out-of-memory report.
constexpr int kOutOfMemoryExitStatus = 3;
// Process exit status of a launch the game cannot use.
constexpr int kLaunchRefusedStatus = 2;

// The folder ErrorLog.txt goes in, found once at start-up so that the
// out-of-memory report does not have to look for it.
std::string error_log_folder;

/// Reports running out of memory and ends the process.
///
/// Appends 3.1c's out-of-memory line to ErrorLog.txt (error_log_folder),
/// then tells the player what happened: the heap's state, where the log is,
/// and that closing other programs may help. The handler cannot know the
/// size that was asked for. The box is built on the stack. The process then
/// exits at once with status 3 (kOutOfMemoryExitStatus). A second entry,
/// from the report itself running out of memory, exits without reporting again.
[[noreturn]] void handle_out_of_memory() {
    static std::atomic_flag entered;
    if (entered.test_and_set(std::memory_order_relaxed))
        std::_Exit(kOutOfMemoryExitStatus);
    // The process ends whatever happens: a log that cannot be written still
    // leaves the box, and a box that cannot be shown the log.
    std::ignore = oa::platform::append_error_log(
        error_log_folder.c_str(), oa::platform::out_of_memory_message
    );
    const oa::platform::HeapStats heap = oa::platform::heap_stats();
    char box[768];
    const bool known_folder = !error_log_folder.empty();
    std::snprintf(
        box,
        sizeof box,
        "Open Annihilation ran out of memory while allocating memory for the game.\n"
        "The heap holds %" PRIu64 " live blocks, %" PRIu64 " live bytes, and peaked at %" PRIu64
        " bytes.\n"
        "ErrorLog.txt is %s%s.\n"
        "Closing other programs may help.\n",
        heap.live_blocks,
        heap.live_bytes,
        heap.peak_bytes,
        known_folder ? "in " : "",
        known_folder ? error_log_folder.c_str() : "beside the program"
    );
    std::ignore = oa::platform::append_error_log(error_log_folder.c_str(), box);
    // The box needs the pointer, which full screen keeps on the window.
    release_pointer(SDL_GetGrabbedWindow());
    std::ignore = SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Open Annihilation", box, nullptr);
    std::_Exit(kOutOfMemoryExitStatus);
}

/// Logs that the floating-point settings had changed and have been put back.
///
/// The settings decide how the simulation's arithmetic rounds, so the game
/// puts back any that changed while it ran (a graphics driver may change
/// them); this reports the first such change of the run.
///
/// @param found the settings found
/// @param saved the settings the game started with, now set again
void report_float_control_change(
    void*,
    const oa::base::float_precision::FloatControl& found,
    const oa::base::float_precision::FloatControl& saved
) {
    std::fprintf(
        stderr,
        "open-annihilation: the floating-point settings had changed and have been put back "
        "(found %" PRIx32 " %" PRIx32 " %" PRIx64 ", started with %" PRIx32 " %" PRIx32 " %" PRIx64
        ")\n",
        found.older_unit,
        found.vector_unit,
        found.arm_unit,
        saved.older_unit,
        saved.vector_unit,
        saved.arm_unit
    );
}

/// Gives the window the game's icon (window_icon.hpp).
///
/// A failure to decode the icon is reported on stderr, and a video driver
/// that has no window icons, such as the dummy one, refuses it silently;
/// either way the game starts without it.
///
/// @param window the game's window
void set_window_icon(SDL_Window* window) {
    WindowIcon icon;
    std::string error;
    if (!decode_window_icon(window_icon_png(), icon, error)) {
        std::cerr << "open-annihilation: window icon: " << error << '\n';
        return;
    }
    SDL_Surface* surface = SDL_CreateSurfaceFrom(
        static_cast<int>(icon.width),
        static_cast<int>(icon.height),
        SDL_PIXELFORMAT_RGBA32,
        icon.pixels.data(),
        static_cast<int>(icon.width * window_icon_pixel_bytes)
    );
    if (surface == nullptr) {
        std::cerr << "open-annihilation: window icon: " << SDL_GetError() << '\n';
        return;
    }
    std::ignore = SDL_SetWindowIcon(window, surface);
    SDL_DestroySurface(surface);
}

/// Refuses every render driver but SDL's software renderer, for
/// --render-fault create.
///
/// @param driver SDL's name for the render driver
/// @return true for every driver but software
bool refuse_all_but_software(void*, std::string_view driver) {
    return driver != oa::platform::render_probe::software_renderer;
}

/// Returns what --check-renderer-ladder forces of the renderer from the
/// start: with --render-fault create, every render driver but SDL's
/// software renderer refuses; otherwise nothing.
///
/// @param options the parsed command line
/// @return the faults; empty in a player's run
RenderFaultHooks start_faults(const Options& options) {
    RenderFaultHooks faults;
    if (options.check_renderer_ladder && options.render_fault &&
        options.render_fault->point == RenderFaultPoint::create)
        faults.refuse_driver = refuse_all_but_software;
    return faults;
}

/// Returns where the start keeps the renderer records: beside the player's
/// own preferences file, or in memory for the run with a named
/// --preferences-file, or where the player's folder cannot be found.
///
/// @param options the parsed command line
/// @return the place
RecordsPlace records_place(const Options& options) {
    RecordsPlace place;
    place.engine_build = OA_ENGINE_BUILD;
    if (options.preferences_file)
        return place;
    try {
        place.folder = preference_file(std::nullopt).parent_path();
    } catch (const std::exception& error) {
        std::cerr << "open-annihilation: the renderer records are kept in memory for this run: "
                  << error.what() << '\n';
    }
    return place;
}

namespace {

/// The quiet frame drawn as soon as the renderer exists, until the setup or
/// the menu takes the window.
struct LaunchFrame {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    bool shown = false;
    bool released = false;
};

LaunchFrame& launch_frame() {
    static LaunchFrame frame;
    return frame;
}

/// Clears the window to the quiet launch colour. The window's first frame is
/// recorded when the present succeeds. A long moment after this shows the
/// progress dialog instead of a bar on this frame.
void draw_launch_frame() {
    LaunchFrame& shown = launch_frame();
    if (shown.renderer == nullptr || shown.released)
        return;
    if (!SDL_SetRenderDrawColor(shown.renderer, 0x1b, 0x1e, 0x19, 255) ||
        !SDL_RenderClear(shown.renderer))
        return;
    if (!SDL_RenderPresent(shown.renderer))
        return;
    if (!shown.shown) {
        shown.shown = true;
        oa::platform::log_files::note_timing("window first frame");
    }
}

} // namespace

struct HostDisplay {
    SDL_Window* window = nullptr;
    // The window's renderer, made by walking SDL's render drivers, and what
    // the probe found of it.
    RendererHost renderer_host{};
    bool active = false;
    /// start_video has run: SDL's video and sound are up, with or without
    /// the window.
    bool video_started = false;
    /// initialize has run: the window opens once, early for the Game files
    /// screen or where the game opens it.
    bool initialized = false;
    // The mode Alt+Enter last asked for while the intro movies play.
    FullScreenSwitch full_screen{};

    /// Starts SDL's video and sound without opening the window.
    ///
    /// Sets the hints SDL reads as its video starts, takes the game's sound
    /// for a capture first, starts the gamepads and the watch on files the
    /// system opens the game with. A launch file the system opens the game
    /// with arrives once the video has started, before any window opens. A
    /// build whose touch controls are on from the start sets their input
    /// hints before SDL starts (set_input_hints).
    ///
    /// Throws std::runtime_error when SDL's video fails. It runs at most once
    /// (video_started).
    ///
    /// @param options the parsed command line
    void start_video(const Options& options) {
        if (video_started)
            return;
        video_started = true;
        if (!SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1"))
            throw std::runtime_error("SDL mouse focus click-through hint was rejected");
        // Closing the window reaches the game as a close request, which a
        // running match answers with its surrender confirmation, rather than
        // as a quit SDL adds on its own.
        if (!SDL_SetHint(SDL_HINT_QUIT_ON_LAST_WINDOW_CLOSE, "0"))
            throw std::runtime_error("SDL last-window quit hint was rejected");
#ifdef SDL_PLATFORM_MACOS
        // In full screen the menu bar and the Dock stay hidden at the
        // screen's edges, whether Alt+Enter or the title bar's full-screen
        // button switched the window; an environment variable of the hint's
        // name keeps them reachable instead.
        std::ignore = SDL_SetHint(SDL_HINT_VIDEO_MAC_FULLSCREEN_MENU_VISIBILITY, "0");
#endif
        // A capture takes the game's sound for itself before SDL starts it.
        if (!options.capture_video.empty())
            prepare_capture_audio(options.capture_video);
        // Touch controls on from the start read fingers and the pen with
        // their own hints from the first event; a desktop build keeps SDL's.
        if constexpr (kTouchFirst)
            set_input_hints();
        if (!SDL_Init(SDL_INIT_VIDEO))
            throw std::runtime_error(std::string("SDL_Init: ") + SDL_GetError());
        // Sound starts on its own: a system with no sound driver SDL can use,
        // such as a Windows 95 that lacks the sound library SDL loads, plays
        // silently rather than not at all.
        if (!SDL_InitSubSystem(SDL_INIT_AUDIO))
            std::cerr << "warning: sound is not available: " << SDL_GetError() << '\n';
        active = true;
        // Gamepads reach the folder chooser and the Game files screen too.
        start_gamepad_subsystem();
        // A .oamod file opened in the game, now or later, reaches the inbox
        // whatever polls SDL's events at that moment.
        watch_opened_files();
    }

    /// Starts SDL's video and sound (start_video) when they are not up yet,
    /// and opens the window, at the size
    /// --resolution gives when it is given, else at the Screen size setting's
    /// (start_settings, starting_screen_size), else at the default, held to
    /// the desktop in Steam's Game Mode (default_window_size), and logs the
    /// size it opened at (report_window_size); at the display's own pixel
    /// density only where decide_window_density allows it, with the
    /// renderer records read first (records_place,
    /// RendererHost::open_records), and its renderer
    /// (RendererHost::create), which it describes and logs with the tier its
    /// first frame is drawn in, from the flags and the Hardware acceleration
    /// setting read before the window opens (RendererHost::decide_start_tier).
    /// A window of a set screen size takes the display's mode of that size
    /// in full screen where full screen switches modes and the display has
    /// one, and is otherwise drawn at the size and scaled to the screen
    /// there (take_screen_size, FullScreenSwitch::screen_width); a set size
    /// the display does not offer opens as Desktop for this run
    /// (shown_screen_size). A place remembered from the last run
    /// (open-annihilation.window-place) opens directly: full screen on that
    /// display, or the window at its size and place, fitted onto the main
    /// display when the old one is gone. A first start is unchanged. -d and
    /// --resolution win for this run and are not remembered. The
    /// platform's window_ready hook, when there is one, is told once the
    /// window and its renderer are made.
    ///
    /// Throws std::runtime_error when SDL, the window or the renderer fails.
    /// It runs at most once (initialized).
    ///
    /// @param options the parsed command line
    void initialize(const Options& options) {
        const uint32_t probe_began = oa::platform::tick_ms();
        initialized = true;
        start_video(options);
        // The settings the window and its renderer start with, read before
        // either exists.
        const auto desktop = desktop_size(options);
        const auto start = start_settings(options, desktop);
        // A stored size the display does not offer shows the desktop's
        // for this run.
        const auto screen = shown_screen_size(options, starting_screen_size(options, start));
        const bool sized = screen != oa::ui::engine_settings::desktop_screen_size;
        // With no size named, Steam's Game Mode holds the window to the
        // desktop, the most of it gamescope's pointer reaches.
        const bool steam_game_mode = oa::platform::running_in_steam_game_mode();
        const auto fallback = default_window_size(desktop, steam_game_mode);
        // The renderer records, read before the window opens so that their
        // native-density key reaches the window's density, and kept for the
        // walk of the render drivers.
        const RecordsPlace place = records_place(options);
        renderer_host.open_records(place);
        // The window's pixel density is fixed once it opens: the display's
        // own only where the rule allows it (decide_window_density).
        DensityRequest density;
        density.flag = options.hardware_acceleration;
        density.platform_native = options.native_density_windows;
        density.asked = options.native_density;
        density.chosen = start.native_density;
        density.setting = start.hardware_acceleration;
        density.unattended = options.unattended;
        density.capture = !options.capture_video.empty();
        // The driver the native-density record names under this engine's
        // version, which holds from one build of it to the next. The game
        // writes no scale-level key, so no rung is remembered for it.
        if (const auto driver = renderer_state::native_density_driver(
                renderer_host.records().records(), OA_ENGINE_VERSION
            ))
            density.record_driver = std::string(*driver);
        const bool native_density = decide_window_density(density).native;
        bool create_full = options.start_full_screen && !sized;
        int width = options.window_resolution ? options.match_width
                    : sized                   ? static_cast<int>(screen.width)
                                              : static_cast<int>(fallback.width);
        int height = options.window_resolution ? options.match_height
                     : sized                   ? static_cast<int>(screen.height)
                                               : static_cast<int>(fallback.height);
        std::optional<int> place_x;
        std::optional<int> place_y;
        bool keep_saved_window = false;
        if constexpr (!kTouchFirst) {
            std::optional<SavedWindow> saved_place;
            try {
                saved_place = read_window_place_file(preference_file(options.preferences_file));
            } catch (const std::exception& error) {
                std::cerr << "open-annihilation: the window's place was not read: " << error.what()
                          << '\n';
            }
            // -df is full screen for this run, and -d a window; neither is remembered.
            const PlaceOverride override_mode =
                options.launch.display_option == 0 ? PlaceOverride::none
                : options.start_full_screen        ? PlaceOverride::full_screen
                                                   : PlaceOverride::window;
            RestoredPlace restored = restore_window_place(
                saved_place ? &*saved_place : nullptr,
                override_mode,
                options.start_full_screen,
                connected_displays()
            );
            // --resolution names this run's size and leaves the placing to the window system.
            if (options.window_resolution) {
                restored.sized = false;
                restored.positioned = false;
            }
            create_full = restored.full_screen;
            // A set screen size with nothing remembered still opens as a window,
            // then takes the size, as it always has.
            if (!saved_place && sized)
                create_full = false;
            if (restored.sized) {
                width = static_cast<int>(restored.width);
                height = static_cast<int>(restored.height);
                keep_saved_window = true;
            }
            if (restored.positioned) {
                place_x = restored.x;
                place_y = restored.y;
            }
        }
        window = create_game_window(
            "Open Annihilation",
            width,
            height,
            game_window_flags(create_full, native_density),
            place_x,
            place_y
        );
        if (window == nullptr)
            throw std::runtime_error(std::string("SDL_CreateWindow: ") + SDL_GetError());
        // Full screen shows the size through a display mode of it, or draws
        // the match at it and scales it to the screen (run_full_screen_method);
        // the runtime follows the size applied. A remembered window keeps its
        // own size; the screen size is what full screen uses later.
        if (sized) {
            full_screen.screen_width = screen.width;
            full_screen.screen_height = screen.height;
        }
        if (sized && !keep_saved_window)
            take_screen_size(window, screen, options.start_full_screen, run_full_screen_method());
        if constexpr (!kTouchFirst) {
            const bool command_line =
                options.launch.display_option != 0 || options.window_resolution;
            try {
                bind_window_place(
                    preference_file(options.preferences_file), !options.unattended && !command_line
                );
            } catch (const std::exception& error) {
                std::cerr << "open-annihilation: the window's place was not kept: " << error.what()
                          << '\n';
            }
            set_window_place_watcher(&remember_open_window);
        }
        report_window_size(window, desktop, steam_game_mode);
        set_window_icon(window);
        renderer_host.create(window, start_faults(options));
        // A check's OA_PIXEL_DENSITY grows the drawing surface now, after the
        // size is chosen and before the first frame, so the setup and the
        // menu lay out from the points the window opened at.
        apply_asked_pixel_density(window);
        TierRequest request;
        request.flag = options.hardware_acceleration;
        request.force_capable = options.force_capable;
        request.players_own_profile = !options.preferences_file.has_value();
        request.setting = start.hardware_acceleration;
        renderer_host.decide_start_tier(request);
        // The platform finishes the window once it shows the renderer's
        // view; the desktop has no such hook.
        if (const PlatformHooks& hooks = platform_hooks(); hooks.window_ready != nullptr)
            hooks.window_ready(hooks.context, window);
        // Something is on screen as soon as the renderer exists. The setup or
        // the menu replaces this frame; start-up work after here can run
        // with the window already up.
        launch_frame().window = window;
        launch_frame().renderer = renderer_host.renderer();
        draw_launch_frame();
        oa::platform::log_files::note_timing_span("display and renderer probe", probe_began);
    }

    /// Ends the run's renderer records cleanly, deleting the sentinel, and
    /// closes the window and SDL. It runs at every exit through main, the
    /// fatal error's among them, as the error unwinds.
    ~HostDisplay() {
        if constexpr (!kTouchFirst)
            remember_open_window(window);
        set_window_place_watcher(nullptr);
        release_pointer(window);
        renderer_host.finish_records();
        renderer_host.destroy();
        if (window != nullptr)
            SDL_DestroyWindow(window);
        window = nullptr;
        if (active)
            SDL_Quit();
    }
};

/// Plays the movies the game starts with, the logo then the intro, unless a
/// switch skips them.
///
/// @param options the parsed command line, with the mod profile played
/// @param host the window the movies show in; null for a headless run
void play_intro(const Options& options, HostDisplay* host) {
    const uint32_t intro_began = oa::platform::tick_ms();
    if (host == nullptr) {
        play_intro_movies(options, nullptr, nullptr, nullptr, nullptr);
        oa::platform::log_files::note_timing_span("intro", intro_began);
        return;
    }
    // Alt+Enter switches full screen during the movies as it does in the game.
    play_intro_movies(
        options,
        host->window,
        host->renderer_host.renderer(),
        &host->renderer_host,
        &host->full_screen
    );
    oa::platform::log_files::note_timing_span("intro", intro_began);
}

/// Writes each game file and listing a run looks up to a file, one a line,
/// and, for a path a mutator serves or hides, which mutator, from whichever
/// thread looks it up (--trace-lookups).
class LookupLog {
  public:

    /// Opens the log.
    ///
    /// @param file the log file, replaced
    explicit LookupLog(const fs::path& file) : out_(file, std::ios::binary | std::ios::trunc) {
        if (!out_)
            throw std::runtime_error("cannot write the lookup log " + path_to_utf8(file));
    }

    /// The observer that writes to this log.
    ///
    /// @return the observer
    oa::LookupObserver observer() {
        oa::LookupObserver watch{this, nullptr, nullptr};
        watch.looked_up = [](void* context, std::string_view name) {
            auto& self = *static_cast<LookupLog*>(context);
            const oa::base::threads::LockGuard guard(self.lock_);
            self.out_ << name << '\n';
        };
        watch.mutator_layer_used =
            [](void* context, std::string_view path, std::string_view owner, bool hidden) {
                auto& self = *static_cast<LookupLog*>(context);
                const oa::base::threads::LockGuard guard(self.lock_);
                self.out_ << path << '\t';
                if (hidden)
                    self.out_ << "hidden by mutator ";
                else
                    self.out_ << "mutator ";
                self.out_ << owner << '\n';
            };
        return watch;
    }

  private:

    oa::base::threads::Mutex lock_;
    std::ofstream out_;
};

/// What the start needs where the platform brings game files in: the
/// import's folders, the backups setting, what the start's recovery found
/// and whether the Game files screen may open.
struct GameFilesStart {
    /// The platform's hooks are installed, and the game folder and the data
    /// folder are known: what an import left is taken up at the start and
    /// the backups setting is applied after resolution.
    bool installed{};
    /// The screen may be offered as well: the player did not ask for the
    /// notice instead (--no-game-files-screen).
    bool offered{};
    game_files::ImportPaths paths{};            ///< the game folder, staging and state
    oa::platform::preferences::Values values{}; ///< the preferences, for the backups and the mod
    bool backed_up{};                           ///< the game files are kept in device backups
    game_files::RecoveryResult recovery{};      ///< what the start's recovery found
};

/// Prepares the start where the platform brings game files in: the
/// import's folders, the backups setting from the preferences, and the
/// recovery of an import a stop or a change for this start left (renames
/// only), with or without the screen (--no-game-files-screen). Without the
/// hooks nothing is done.
///
/// @param options the parsed command line
/// @return what the start needs; nothing installed without the hooks
GameFilesStart start_game_files(const Options& options) {
    GameFilesStart start;
    const GameFilesHooks& hooks = game_files_hooks();
    if (!game_files_import_offered(hooks))
        return start;
    std::string game_folder;
    if (!hooks.game_folder(hooks.context, &game_folder) || game_folder.empty())
        return start;
    fs::path data_folder;
    if (options.data_dir) {
        data_folder = *options.data_dir;
    } else {
        try {
            data_folder = oa::platform::preferences::data_directory();
        } catch (const std::exception& error) {
            std::cerr << "open-annihilation: the game files cannot be brought in: " << error.what()
                      << '\n';
            return start;
        }
    }
    start.installed = true;
    start.offered = !options.no_game_files_screen;
    start.paths = game_files::import_paths(path_from_utf8(game_folder), data_folder);
    // An unattended run reads only a preferences file it was given.
    if (!options.unattended || options.preferences_file)
        start.values = oa::platform::preferences::load(preference_file(options.preferences_file));
    start.backed_up =
        oa::ui::engine_settings::read_settings(start.values, {}, false).game_files_backed_up;
    start.recovery = game_files::recover_import(hooks, start.paths, start.backed_up);
    return start;
}

/// The game's display and command line, as the folder chooser's hooks reach them.
struct ChooserDisplay {
    HostDisplay* display{};   ///< the game's display, opened on first use
    const Options* options{}; ///< the parsed command line the display opens with
};

/// Returns the folder chooser's hooks over the game's display: the window opens on first use
/// (HostDisplay::initialize), and one that cannot open is reported and gives no window.
///
/// @param chooser the display and command line; outlives the hooks
/// @return the hooks
FolderChooserDisplayHooks display_hooks(ChooserDisplay& chooser) {
    FolderChooserDisplayHooks hooks;
    hooks.context = &chooser;
    hooks.window = [](void* context) -> SDL_Window* {
        auto& place = *static_cast<ChooserDisplay*>(context);
        if (!place.display->initialized) {
            try {
                place.display->initialize(*place.options);
            } catch (const std::exception& error) {
                std::cerr << "open-annihilation: the window cannot open: " << error.what() << '\n';
                return nullptr;
            }
        }
        return place.display->window;
    };
    hooks.renderer = [](void* context) -> SDL_Renderer* {
        return static_cast<ChooserDisplay*>(context)->display->renderer_host.renderer();
    };
    hooks.host = [](void* context) -> RendererHost* {
        return &static_cast<ChooserDisplay*>(context)->display->renderer_host;
    };
    return hooks;
}

[[nodiscard]] std::optional<GameDirectory> compose_from_options(const Options& options);

/// The player's folder, or empty when it cannot be resolved.
///
/// @param options the parsed command line
/// @return the folder
[[nodiscard]] fs::path resolved_user_folder(const Options& options) {
    try {
        const fs::path preferences = preference_file(options.preferences_file);
        std::error_code error;
        const oa::platform::preferences::Values values =
            fs::exists(preferences, error) ? oa::platform::preferences::load(preferences)
                                           : oa::platform::preferences::Values{};
        std::string note;
        return own_user_folder(
            options.user_folder, options.preferences_file, preferences, values, note
        );
    } catch (const std::exception&) {
        return {};
    }
}

/// Frames drawn while the iCloud check holds an archive read.
std::atomic<int>& icloud_wait_draws() {
    static std::atomic<int> draws{0};
    return draws;
}

/// Marks the check's archive as not on this computer. Every other file is
/// asked of the system.
bool icloud_wait_residence(const fs::path& path, oa::platform::FileResidence& residence, void*) {
    std::error_code error;
    if (!fs::is_regular_file(path, error) || error)
        return false;
    residence.dataless = path.filename() == "totala1.hpi";
    return true;
}

/// Holds the archive read until the window has drawn twice more, and fails
/// when those frames never come. A read on the drawing thread cannot draw.
void hold_icloud_archive_read(const fs::path&, void*) {
    const int began = icloud_wait_draws().load(std::memory_order_acquire);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (icloud_wait_draws().load(std::memory_order_acquire) < began + 2) {
        if (std::chrono::steady_clock::now() > deadline)
            throw std::runtime_error("icloud wait: the window froze while an archive was read");
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }
}

/// Asks macOS to fetch the player's folders, including Games and Expansions,
/// without waiting. The music folder is asked for when music starts.
void start_player_evicted_fetch(const Options& options) {
    const fs::path user = resolved_user_folder(options);
    if (user.empty())
        return;
    std::vector<fs::path> roots;
    for (const std::string_view name :
         {"Mods", "Maps", "Languages", "Mutators", "Games", "Expansions"})
        roots.push_back(user / std::string(name));
    oa::platform::start_startup_evicted_fetch(std::move(roots));
}

/// Plants one evicted archive and holds its read for the iCloud check.
void arm_icloud_wait(const Options& options) {
    plant_icloud_wait_copy(resolved_user_folder(options));
    icloud_wait_draws().store(0, std::memory_order_release);
    set_archive_residence_probe(icloud_wait_residence, nullptr);
    set_evicted_archive_hold(hold_icloud_archive_read, nullptr);
}

/// Runs the Game files screen until the game folder resolves: the window
/// opens first, once, and stays for the game. Each PLAY resolves the folder
/// again; a refused folder opens the screen again with the reason.
///
/// @param options the parsed command line
/// @param display the game's display, initialised here when it is not yet
/// @param start the import's folders, the preferences and the recovery
/// @param[in,out] needed why there is no folder; resolution fills it again
/// @param[out] game_directory the folder, once it resolves
/// @return true to go on with the start; false when the screen was closed
bool run_game_files_until_resolved(
    const Options& options,
    HostDisplay& display,
    GameFilesStart& start,
    GameFilesNeeded& needed,
    std::optional<GameDirectory>& game_directory
) {
    while (!game_directory && needed.needed && !needed.chooser) {
        if (!display.initialized)
            display.initialize(options);
        GameFilesScreenRequest request;
        request.window = display.window;
        request.renderer = display.renderer_host.renderer();
        request.host = &display.renderer_host;
        request.entry = GameFilesEntry::first_run;
        request.paths = start.paths;
        request.recovery = start.recovery;
        request.needed = needed;
        request.mod.folder = chosen_mod_directory(options.mod_dir, options.base_game, start.values);
        request.mod.profile_file = options.mod_file;
        request.mod.accept_unimplemented_hacks = options.accept_unimplemented_hacks;
        request.mod.preferences = &start.values;
        request.version = std::string("v") + OA_ENGINE_VERSION;
        request.preferences_file = options.preferences_file;
        request.user_folder = options.user_folder;
        request.players_own_profile = !options.preferences_file.has_value();
        if constexpr (self_checks_built)
            if (options.check_game_files)
                request.check = game_files_check_hooks(options);
        if (run_game_files_screen(request) == GameFilesEnd::quit)
            return false;
        // The continue banner shows once.
        start.recovery = {};
        // An import that finished wrote the managed folders and removed the
        // staged game, so the start plays that copy. The check keeps the
        // staged folder and resolves it as before.
        if (!options.check_game_files) {
            const fs::path user = resolved_user_folder(options);
            if (!user.empty() && managed_base_present(user, oa::data::content_bundles::bundles())) {
                if (std::optional<GameDirectory> managed = compose_from_options(options)) {
                    game_directory = std::move(*managed);
                    needed = {};
                }
            }
        }
        if (!game_directory)
            game_directory = find_game_directory(options, &needed);
        if (!game_directory && !options.check_game_files) {
            const fs::path user = resolved_user_folder(options);
            if (!user.empty() && managed_base_present(user, oa::data::content_bundles::bundles())) {
                needed.needed = false;
                std::cerr << "open-annihilation: the managed copy could not be mounted\n";
            }
        }
    }
    return true;
}

/// The status run_once returns when SWITCH on the settings' Switch Mod
/// question ended the run: main() starts the game afresh for the mod stored.
constexpr int kSoftRestartStatus = -1;

/// What a run a switch of the mod in the Library ended leaves for the next.
struct RestartCarry {
    /// The Library it showed, to open again; null for none.
    std::shared_ptr<const Runtime::LibraryReturn> library{};
    /// What the progress dialog says while the switch applies; empty for none.
    std::string line{};
    /// The window as the switch began, the Library with the line over it,
    /// which shows until the Library opens again; null for none.
    std::shared_ptr<const renderer::Surface> picture{};
};

/// The switches --check-mod-switch makes: four rounds of the mods it takes
/// turns with.
constexpr uint32_t kModSwitches = 13;

/// The mods --check-mod-switch takes turns with: No Mod and two test profiles.
constexpr std::size_t kModSwitchTurns = 3;

/// The growth of the host heap's bytes in use --check-mod-switch allows
/// between two runs that play the same mod, in bytes. Runs that play the
/// same mod hold the same blocks, to within the length of a few strings
/// and what the allocator's own bookkeeping varies by from one machine to
/// the next (up to 100 KiB on the regression Mac), so a leak of 22 KiB or
/// more a switch exceeds it over the six switches between the runs
/// compared.
constexpr uint64_t kModSwitchHeapNoise = uint64_t{128} * 1024;

/// What the host heap holds as each run of --check-mod-switch starts on the
/// main menu, and the verdict on them.
class ModSwitchMemory {
  public:

    /// Paints the progress dialog off the screen before the first run is
    /// sampled. The dialog keeps its fonts, the glyphs it drew and the app
    /// icon from its first showing, when a step of a start lasts longer than
    /// long_work_show_after_ms, for the rest of the process. A start slowed
    /// by a loaded machine would otherwise first show it in a later round,
    /// and the heap would step up by that much once, as if a switch kept
    /// memory. The heap it takes is logged.
    ///
    /// @param window the game's window
    /// @param renderer its renderer
    void warm_progress_dialog(SDL_Window* window, SDL_Renderer* renderer) {
        oa::platform::HostHeapUse before{};
        const bool known = oa::platform::sample_host_heap_use(&before);
        if (!oa::app::warm_progress_dialog(window, renderer)) {
            std::cout << "mod switch check: the progress dialog could not be painted ahead\n";
            return;
        }
        oa::platform::HostHeapUse after{};
        if (known && oa::platform::sample_host_heap_use(&after) && after.bytes >= before.bytes)
            std::cout << "mod switch check: the progress dialog, painted before the first run, "
                         "keeps "
                      << (after.bytes - before.bytes) / 1024 << " KiB\n";
        else
            std::cout << "mod switch check: the progress dialog was painted before the first run\n";
    }

    /// Notes the host heap's use as a run starts on the main menu, with the
    /// working set beside it.
    ///
    /// @param run the soft restarts before it
    void sample(uint32_t run) {
        oa::platform::HostHeapUse heap{};
        if (!oa::platform::sample_host_heap_use(&heap))
            heap_known_ = false;
        oa::platform::MemorySample sample{};
        if (!oa::platform::sample_process_memory(nullptr, &sample))
            sample.working_set = 0;
        if (samples_.size() <= run)
            samples_.resize(std::size_t{run} + 1);
        samples_[run] = heap.bytes;
        std::cout << "mod switch check: run " << run << ", heap in use " << heap.bytes / 1024
                  << " KiB in " << heap.blocks << " blocks, working set "
                  << sample.working_set / 1024 << " KiB\n";
    }

    /// Compares each run of the last round of switches with the run of the
    /// second round that played the same mod, six switches earlier: none may
    /// hold more than kModSwitchHeapNoise more on the host heap, as a run
    /// would that kept what an earlier run allocated. The first start is
    /// left out: it holds blocks of its own, which the first switch frees.
    /// So is the first round after it: on some machines the allocator is
    /// still warming up there, and a run of that round holds up to 100 KiB
    /// less than every later run of the same mod. The working set is not
    /// compared, since it counts the free memory the allocator keeps, which
    /// grows over the switches by an amount that changes from one start of
    /// the check to the next.
    ///
    /// @return 0 when the heap's use stayed level, or the system does not
    ///     report it; 1 when it grew, or not every switch was made
    [[nodiscard]] int verdict() const {
        if (samples_.size() != std::size_t{kModSwitches} + 1) {
            std::cout << "mod switch check: FAILED, " << samples_.size() << " runs instead of "
                      << kModSwitches + 1 << '\n';
            return 1;
        }
        if (!heap_known_) {
            std::cout << "mod switch check: the system reports no heap in use\n";
            return 0;
        }
        uint64_t worst = 0;
        for (std::size_t run = samples_.size() - kModSwitchTurns; run < samples_.size(); ++run) {
            const uint64_t settled = samples_[1 + kModSwitchTurns + (run - 1) % kModSwitchTurns];
            worst = std::max(worst, samples_[run] > settled ? samples_[run] - settled : 0);
        }
        std::cout << "mod switch check: " << kModSwitches
                  << " switches; the last round holds at most " << worst / 1024
                  << " KiB more on the heap than the second round after the first start\n";
        if (worst > kModSwitchHeapNoise) {
            std::cout << "mod switch check: FAILED, the heap's use grows with the switches\n";
            return 1;
        }
        std::cout << "mod switch check: passed\n";
        return 0;
    }

  private:

    std::vector<uint64_t> samples_; ///< heap bytes in use, one for each run, from the first
    bool heap_known_{true};         ///< every run's sample reported the heap's use
};

/// Returns a run's exit status: under --check-game-files, the Game files
/// check's verdict on it.
///
/// @param options the parsed command line
/// @param status the run's status so far
/// @return the status the process ends with
int game_files_status([[maybe_unused]] const Options& options, int status) {
    if constexpr (self_checks_built)
        if (options.check_game_files)
            return finish_game_files_check(options, status);
    return status;
}

void publish_managed_copy(
    const Options& options, const fs::path& installation, bool game_dir_named
);

/// Reports whether this start mounts the player's copy.
///
/// `--game-dir` and `--archive` keep today's folder. A scripted run that
/// names neither `--user-folder` nor `--preferences-file` does too, so it
/// never plays the copy in the player's own folder. Every other start does.
///
/// @param options the parsed command line
/// @return true when the copy is mounted
[[nodiscard]] bool mount_managed_copy(const Options& options) {
    if (!options.game_dir.empty() || !options.archives.empty())
        return false;
    if (options.user_folder || options.preferences_file)
        return true;
    const char* ci = SDL_getenv("CI");
    const char* video = SDL_GetHint(SDL_HINT_VIDEO_DRIVER);
    const bool scripted = options.unattended || options.headless_check ||
                          unattended_environment(
                              ci == nullptr ? std::string_view{} : std::string_view{ci},
                              video == nullptr ? std::string_view{} : std::string_view{video}
                          );
    return !scripted;
}

/// Mounts the player's copy of the chosen base game and expansions.
///
/// The preferences are read again on every call, so the soft restart a mod
/// switch uses picks up a choice saved since the previous start. A problem
/// finding the player's folder leaves the start to the game folder, unless
/// `--content` named the bundles.
///
/// @param options the parsed command line
/// @return the composed folders, or nullopt when no copy is recorded
[[nodiscard]] std::optional<GameDirectory> compose_from_options(const Options& options) {
    oa::platform::preferences::Values values;
    fs::path user;
    try {
        const fs::path preferences = preference_file(options.preferences_file);
        std::error_code failure;
        if (fs::exists(preferences, failure) && !failure)
            values = oa::platform::preferences::load(preferences);
        std::string note;
        user = own_user_folder(
            options.user_folder, options.preferences_file, preferences, values, note
        );
    } catch (const std::exception&) {
        if (!options.content.empty())
            throw;
        return std::nullopt;
    }
    ManagedMount request{};
    request.user_folder = std::move(user);
    request.content = options.content;
    request.mod_dir = options.mod_dir;
    request.base_game = options.base_game;
    request.mod_file = options.mod_file;
    request.accept_unimplemented_hacks = options.accept_unimplemented_hacks;
    return compose_managed_content(request, values);
}

/// Stops a start that has no game to play once the setup is done, and says why.
///
/// Such a start plays the player's copy, so the message says what is wrong
/// with it (managed_copy_problem), or that the player's folder could not be
/// found. main() writes it to the log and shows it. --check-game-files ends
/// with its own verdict instead.
///
/// @param options the parsed command line
/// @return the status --check-game-files ends with
/// @throws std::runtime_error with the message, for every other start
int stop_without_copy(const Options& options) {
    if constexpr (self_checks_built)
        if (options.check_game_files)
            return finish_game_files_check(options, 1);
    oa::platform::preferences::Values values;
    fs::path user;
    try {
        const fs::path preferences = preference_file(options.preferences_file);
        std::error_code failure;
        if (fs::exists(preferences, failure) && !failure)
            values = oa::platform::preferences::load(preferences);
        std::string note;
        user = own_user_folder(
            options.user_folder, options.preferences_file, preferences, values, note
        );
    } catch (const std::exception& failure) {
        throw std::runtime_error(
            std::string(
                "Open Annihilation could not find its own folder, where it keeps its "
                "copy of the game: "
            ) +
            failure.what()
        );
    }
    ManagedMount request{};
    request.user_folder = std::move(user);
    request.content = options.content;
    throw std::runtime_error(managed_copy_problem(request, values));
}

/// Compares two names without regard to ASCII case.
///
/// @param left one name
/// @param right the other
/// @return true when every letter matches
bool same_ascii(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size())
        return false;
    for (std::size_t at = 0; at < left.size(); ++at) {
        const auto fold = [](unsigned char character) noexcept {
            return character >= 'A' && character <= 'Z'
                       ? static_cast<unsigned char>(character - 'A' + 'a')
                       : character;
        };
        if (fold(static_cast<unsigned char>(left[at])) !=
            fold(static_cast<unsigned char>(right[at])))
            return false;
    }
    return true;
}

/// Returns the largest whole number written in a player-count list.
///
/// @param text the OTA numplayers text, such as "2, 4"
/// @return the largest number, or 0 when there is none
int32_t largest_count(std::string_view text) noexcept {
    int32_t best = 0;
    int32_t current = 0;
    bool digits = false;
    for (const char character : text) {
        if (character >= '0' && character <= '9') {
            digits = true;
            const int32_t digit = character - '0';
            if (current > (std::numeric_limits<int32_t>::max() - digit) / 10)
                current = std::numeric_limits<int32_t>::max();
            else
                current = current * 10 + digit;
        } else if (digits) {
            if (current > best)
                best = current;
            current = 0;
            digits = false;
        }
    }
    if (digits && current > best)
        best = current;
    return best;
}

/// Returns how many players a parsed map is for.
///
/// The permitted counts win. Without them, the largest Network schema's
/// start positions do.
///
/// @param metadata the map's OTA
/// @return the count, or 0 when the file names none
int32_t map_players(const oa::formats::ota::MapMetadata& metadata) noexcept {
    const int32_t listed = largest_count(metadata.permitted_player_counts);
    if (listed > 0)
        return listed;
    int32_t best = 0;
    for (const oa::formats::ota::Schema& schema : metadata.schemas) {
        bool network = false;
        for (const std::string_view type : oa::formats::ota::multiplayer_schema_types)
            if (same_ascii(schema.type, type))
                network = true;
        if (!network)
            continue;
        const auto count = static_cast<int32_t>(schema.start_positions.size());
        if (count > best)
            best = count;
    }
    return best;
}

/// One map a launch or the content list can name.
struct CatalogueMap {
    std::string key{};
    std::string shown{};
    std::string stem{};
    int32_t players = 0;
    std::string size{};
};

/// One mod or expansion the content list names.
struct CatalogueNamed {
    std::string id{};
    std::string name{};
};

/// Returns the player's own folder, or nothing when it cannot be chosen.
///
/// @param options the run
/// @return the folder
std::optional<fs::path> launch_user_folder(const Options& options) {
    try {
        const fs::path preferences = preference_file(options.preferences_file);
        std::error_code failure;
        oa::platform::preferences::Values values;
        if (fs::exists(preferences, failure) && !failure)
            values = oa::platform::preferences::load(preferences);
        std::string note;
        return own_user_folder(
            options.user_folder, options.preferences_file, preferences, values, note
        );
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

/// Turns "has not copied <id>" into a launch refusal that names the field.
///
/// @param message the message the managed copy reported
/// @return the refusal line
std::string missing_copy_line(std::string_view message) {
    const auto& net = netgame_context().options;
    constexpr std::string_view marker = "Open Annihilation has not copied ";
    const auto at = message.find(marker);
    std::string id;
    if (at != std::string_view::npos) {
        auto rest = message.substr(at + marker.size());
        if (!rest.empty() && rest.back() == '.')
            rest.remove_suffix(1);
        id = std::string(rest);
    }
    const std::string prefix = net.launch_source + ": ";
    if (!id.empty() && id == net.launch_content.base_game)
        return prefix + "content.base-game: \"" + id + "\" is not an installed base game";
    if (!id.empty())
        return prefix + "content.expansions: \"" + id + "\" is not an installed expansion";
    return prefix + "content.base-game: " + std::string(message);
}

/// Mounts the managed copy, and names a missing bundle as a launch refusal.
///
/// @param options the run
/// @return the composed folders, or nullopt when no copy is recorded
std::optional<GameDirectory> compose_for_run(const Options& options) {
    try {
        return compose_from_options(options);
    } catch (const netgame::LaunchRefusal&) {
        throw;
    } catch (const std::runtime_error& error) {
        if (netgame_context().options.launch_content_applied)
            throw netgame::LaunchRefusal(missing_copy_line(error.what()));
        throw;
    }
}

/// Resolves content.mod to a folder or a profile file before the game mounts.
///
/// A path that is already there is used as it is. An id is matched to a mod
/// in the player's Mods folder, and, when --game-dir was named, to one in
/// that game folder. A missing mod is refused before a window opens.
///
/// @param[in,out] options the run; mod_file or mod_dir is set when one matches
void resolve_launch_mod(Options& options) {
    auto& net = netgame_context().options;
    if (!net.launch_mod_pending)
        return;
    const std::string named = net.launch_content.mod;
    const fs::path as_path = path_from_utf8(named);
    std::error_code failure;
    if (fs::exists(as_path, failure) && !failure) {
        if (fs::is_regular_file(as_path, failure) && !failure)
            options.mod_file = as_path;
        else if (fs::is_directory(as_path, failure) && !failure)
            options.mod_dir = as_path;
        else
            throw netgame::LaunchRefusal(
                net.launch_source + ": content.mod: \"" + named + "\" is not an installed mod"
            );
        net.launch_mod_pending = false;
        return;
    }
    std::vector<fs::path> folders;
    if (const std::optional<fs::path> user = launch_user_folder(options)) {
        try {
            const std::vector<fs::path> mods =
                list_mods_in(*user / std::string(user_mods_folder_name));
            folders.insert(folders.end(), mods.begin(), mods.end());
        } catch (const std::exception&) {
        }
    }
    if (options.game_dir_named) {
        try {
            const std::vector<fs::path> mods = list_mod_folders(options.game_dir);
            folders.insert(folders.end(), mods.begin(), mods.end());
        } catch (const std::exception&) {
        }
    }
    std::optional<fs::path> by_id;
    std::optional<fs::path> by_filename;
    for (const fs::path& folder : folders) {
        const ModSummary summary = read_mod_summary(folder);
        const std::string filename = path_to_utf8(folder.filename());
        if (!summary.id.empty() && summary.id == named && !by_id)
            by_id = folder;
        if (filename == named && !by_filename)
            by_filename = folder;
    }
    const std::optional<fs::path> found = by_id ? by_id : by_filename;
    if (!found)
        throw netgame::LaunchRefusal(
            net.launch_source + ": content.mod: \"" + named + "\" is not an installed mod"
        );
    options.mod_dir = *found;
    net.launch_mod_pending = false;
}

/// Refuses a --game-dir launch whose named bundles are not in that folder.
///
/// A start without --game-dir is checked when the managed copy is mounted.
///
/// @param options the run, with game_dir filled
void require_installed_copies(const Options& options) {
    const auto& net = netgame_context().options;
    if (!net.launch_content_applied || !options.game_dir_named)
        return;
    const std::vector<std::string> copied = copied_content_bundles(options.game_dir);
    const auto missing = [&copied](const std::string& id) {
        return std::find(copied.begin(), copied.end(), id) == copied.end();
    };
    const std::string prefix = net.launch_source + ": ";
    if (!net.launch_content.base_game.empty() && missing(net.launch_content.base_game))
        throw netgame::LaunchRefusal(
            prefix + "content.base-game: \"" + net.launch_content.base_game +
            "\" is not an installed base game"
        );
    for (const std::string& id : net.launch_content.expansions)
        if (missing(id))
            throw netgame::LaunchRefusal(
                prefix + "content.expansions: \"" + id + "\" is not an installed expansion"
            );
}

/// Reads the installed maps, mods and expansions.
///
/// @param options the run
/// @param directory the game folder
/// @param[out] maps the maps, sorted by key
/// @param[out] mods the mods, sorted by id
/// @param[out] expansions the installed expansions, sorted by id
void read_installed_content(
    const Options& options,
    const GameDirectory& directory,
    std::vector<CatalogueMap>& maps,
    std::vector<CatalogueNamed>& mods,
    std::vector<CatalogueNamed>& expansions
) {
    std::vector<fs::path> archives = options.archives;
    if (archives.empty())
        archives = directory.archives;
    if (!archives.empty()) {
        oa::AssetStore assets(options.game_folders);
        for (const fs::path& candidate : archives) {
            const fs::path archive =
                candidate.is_absolute() ? candidate : options.game_dir / candidate;
            assets.mount(archive);
        }

        struct ListedNames {
            oa::data::campaign::MapList list{};
            char* names = nullptr;

            ~ListedNames() {
                std::free(names);
                oa::data::campaign::map_clear_list_cache(list, nullptr);
            }
        } listed;

        auto files = oa::data::campaign::campaign_asset_files(assets);
        files.translate = nullptr;
        const int32_t count = oa::data::campaign::map_build_multiplayer_list(
            listed.list, files, {}, &listed.names, false, false
        );
        const char* name = listed.names;
        for (int32_t index = 0; name != nullptr && index < count;
             ++index, name += std::strlen(name) + 1) {
            CatalogueMap row;
            row.key = name;
            row.stem = name;
            row.shown = name;
            try {
                const auto bytes = assets.read(std::string("maps/") + row.key + ".ota").bytes;
                const auto parsed = oa::formats::ota::parse(
                    std::string_view(reinterpret_cast<const char*>(bytes.data()), bytes.size())
                );
                if (parsed.ok() && parsed.metadata) {
                    if (!parsed.metadata->mission_name.empty())
                        row.shown = parsed.metadata->mission_name;
                    row.size = parsed.metadata->map_size;
                    row.players = map_players(*parsed.metadata);
                }
            } catch (const std::exception&) {
            }
            maps.push_back(std::move(row));
        }
    }
    if (const std::optional<fs::path> user = launch_user_folder(options)) {
        try {
            MapPacks packs(*user / "Maps");
            for (const PackMap& map : packs.maps()) {
                CatalogueMap row;
                row.key = map.name;
                row.stem = map.stem;
                row.shown = map.title.empty() ? map.name : map.title;
                row.players = map.players;
                row.size = map.size;
                maps.push_back(std::move(row));
            }
        } catch (const std::exception&) {
        }
        try {
            const std::vector<fs::path> folders =
                list_mods_in(*user / std::string(user_mods_folder_name));
            for (const fs::path& folder : folders) {
                const ModSummary summary = read_mod_summary(folder);
                CatalogueNamed row;
                row.id = summary.id.empty() ? path_to_utf8(folder.filename()) : summary.id;
                row.name = summary.title.empty() ? row.id : summary.title;
                mods.push_back(std::move(row));
            }
        } catch (const std::exception&) {
        }
    }
    try {
        const std::vector<fs::path> folders = list_mod_folders(options.game_dir);
        for (const fs::path& folder : folders) {
            const ModSummary summary = read_mod_summary(folder);
            CatalogueNamed row;
            row.id = summary.id.empty() ? path_to_utf8(folder.filename()) : summary.id;
            row.name = summary.title.empty() ? row.id : summary.title;
            const bool seen =
                std::any_of(mods.begin(), mods.end(), [&row](const CatalogueNamed& have) {
                    return have.id == row.id;
                });
            if (!seen)
                mods.push_back(std::move(row));
        }
    } catch (const std::exception&) {
    }
    if (!options.game_dir.empty()) {
        for (const std::string& id : copied_content_bundles(options.game_dir)) {
            const oa::data::content_bundles::Bundle* bundle =
                oa::data::content_bundles::find_bundle(id);
            if (bundle == nullptr || bundle->kind != oa::data::content_bundles::Kind::expansion)
                continue;
            CatalogueNamed row;
            row.id = id;
            row.name = bundle->english_name.empty() ? id : std::string(bundle->english_name);
            expansions.push_back(std::move(row));
        }
    }
    std::sort(maps.begin(), maps.end(), [](const CatalogueMap& left, const CatalogueMap& right) {
        return left.key < right.key;
    });
    std::sort(
        mods.begin(), mods.end(), [](const CatalogueNamed& left, const CatalogueNamed& right) {
            return left.id < right.id;
        }
    );
    std::sort(
        expansions.begin(),
        expansions.end(),
        [](const CatalogueNamed& left, const CatalogueNamed& right) { return left.id < right.id; }
    );
}

/// Finds the one map a launch names, by key and then by the name players see.
///
/// @param asked the file's map
/// @param maps the installed maps
/// @param[out] why why none was chosen, when the return is null
/// @return the map, or null
const CatalogueMap* resolve_catalogue_map(
    std::string_view asked, const std::vector<CatalogueMap>& maps, std::string& why
) {
    std::vector<const CatalogueMap*> exact;
    for (const CatalogueMap& map : maps)
        if (map.key == asked)
            exact.push_back(&map);
    if (exact.size() == 1)
        return exact[0];
    if (exact.size() > 1) {
        why = "matches more than one map";
        return nullptr;
    }
    const bool keyed = asked.find('@') != std::string_view::npos;
    std::vector<const CatalogueMap*> keys;
    for (const CatalogueMap& map : maps)
        if (same_ascii(map.key, asked))
            keys.push_back(&map);
    if (keys.size() == 1)
        return keys[0];
    if (keys.size() > 1 || keyed) {
        why = keys.empty() ? "is not an installed map" : "matches more than one map";
        return nullptr;
    }
    std::vector<const CatalogueMap*> shown;
    for (const CatalogueMap& map : maps)
        if (same_ascii(map.shown, asked) || same_ascii(map.stem, asked))
            shown.push_back(&map);
    if (shown.size() == 1)
        return shown[0];
    why = shown.empty() ? "is not an installed map" : "matches more than one map";
    return nullptr;
}

/// Prints the installed content, or checks a host's launch map, before a window.
///
/// @param options the run
/// @param directory the game folder
/// @return 0 when the content list was printed; nullopt to keep starting
std::optional<int>
publish_launch_catalogue(const Options& options, const GameDirectory& directory) {
    auto& net = netgame_context().options;
    auto& block = netgame_context().launch.block;
    const bool named_map = net.launch_planned && block.hosting != 0 && block.mission[0] != '\0';
    if (!net.list_content && !named_map)
        return std::nullopt;
    if (!net.list_content)
        require_installed_copies(options);
    std::vector<CatalogueMap> maps;
    std::vector<CatalogueNamed> mods;
    std::vector<CatalogueNamed> expansions;
    read_installed_content(options, directory, maps, mods, expansions);
    if (net.list_content) {
        oa::formats::json::JsonWriter writer;
        writer.begin_object();
        writer.key("maps");
        writer.begin_array();
        for (const CatalogueMap& map : maps) {
            writer.begin_object();
            writer.key("shown-name");
            writer.string(map.shown);
            writer.key("key");
            writer.string(map.key);
            writer.key("players");
            writer.integer(map.players);
            writer.key("size");
            writer.string(map.size);
            writer.end_object();
        }
        writer.end_array();
        writer.key("mods");
        writer.begin_array();
        for (const CatalogueNamed& mod : mods) {
            writer.begin_object();
            writer.key("id");
            writer.string(mod.id);
            writer.key("name");
            writer.string(mod.name);
            writer.end_object();
        }
        writer.end_array();
        writer.key("expansions");
        writer.begin_array();
        for (const CatalogueNamed& expansion : expansions) {
            writer.begin_object();
            writer.key("id");
            writer.string(expansion.id);
            writer.key("name");
            writer.string(expansion.name);
            writer.end_object();
        }
        writer.end_array();
        writer.end_object();
        // The standard output the process was started with, even where the
        // log took the streams.
        automation::write_started_output(writer.text());
        return 0;
    }
    std::string asked;
    for (const char character : block.mission) {
        if (character == '\0')
            break;
        asked.push_back(character);
    }
    std::string why;
    const CatalogueMap* chosen = resolve_catalogue_map(asked, maps, why);
    if (chosen == nullptr)
        throw netgame::LaunchRefusal(launch_place("map") + ": \"" + asked + "\" " + why);
    if (chosen->key.size() >= sizeof block.mission)
        throw netgame::LaunchRefusal(launch_place("map") + ": is longer than 63 characters");
    oa::base::text::copy_terminated(block.mission, chosen->key);
    return std::nullopt;
}

/// Makes a launch file the system opened the game with this start's launch, as -c FILE does.
///
/// macOS hands the game the file it was started for once SDL's video has
/// started, as a drop of the file, which the watch on SDL's events reads into
/// network play's waiting launches (mod_install_watch.cpp); the events the
/// system holds are pumped first, so that it arrives now. The launch's
/// content then chooses the game's folders, the map check runs before the
/// runtime starts, and the intro is skipped. A start that names a launch of
/// its own leaves the file waiting, and the running game refuses it. A file
/// that arrives later is the running game's (follow_opened_launches).
/// --check-opened-launch start opens its file in the game here, as the
/// system opens one.
///
/// Throws netgame::LaunchRefusal, naming the file and the field, when the
/// launch cannot be used.
///
/// @param[in,out] options the run, its content and intro among them
void take_launch_opened_at_start(Options& options) {
    const auto& net = netgame_context().options;
    if (net.opened_launch_check == OpenedLaunchCheck::start) {
        // SDL keeps the pointer: the text lives as long as the process.
        static std::string dropped;
        dropped = net.opened_launch_check_file;
        SDL_Event drop{};
        drop.type = SDL_EVENT_DROP_FILE;
        drop.drop.data = dropped.c_str();
        if (!SDL_PushEvent(&drop))
            std::cerr << "open-annihilation: opened-launch check: the file could not be opened: "
                      << SDL_GetError() << '\n';
    }
    SDL_PumpEvents();
    if (net.launch_planned || !netgame::opened_launch_waiting())
        return;
    const std::optional<netgame::OpenedLaunch> opened = netgame::take_opened_launch();
    if (!opened)
        return;
    std::cerr << "open-annihilation: a launch file opened with the game is this start's launch: "
              << opened->source << '\n';
    put_launch_in_place(netgame_context(), netgame::read_opened_launch_plan(*opened));
    adopt_launch_run(options);
    resolve_launch_mod(options);
    options.skip_intro = true;
}

/// Tells whether the window waits for the launch's checks.
///
/// --list-content opens no window at all. A launch, named on the command
/// line, piped in or opened with the game, opens it once the launch's
/// content and map are found installed (publish_launch_catalogue), so that a
/// launch the game cannot use is refused before any window opens.
///
/// @return true for a listing or a launch
[[nodiscard]] bool window_waits_for_launch() {
    const auto& net = netgame_context().options;
    return net.list_content || net.launch_planned;
}

/// Refuses a --mutator value the game cannot apply, before a window opens.
///
/// A malformed value was already refused while the command line was read.
/// An id that is not installed, an option the manifest does not have, a
/// value outside its choices, and a list the manifests refuse (one id
/// twice, a prerequisite that is not above it, or two that conflict) end
/// the run with status 2. With no player folder, an id is refused the
/// same way and is not looked up. `--no-mutators` returns without reading
/// any mutator, so a list that is not installed does not stop the run.
///
/// @param options the parsed command line
void refuse_unusable_mutators(const Options& options) {
    if (options.mutators.empty() || options.no_mutators)
        return;
    const std::optional<fs::path> player = launch_user_folder(options);
    fs::path mutators_folder;
    if (player)
        mutators_folder = *player / std::string(oa::data::mutator_library::mutators_folder_name);
    const std::string launch_source = netgame_context().options.launch_source;
    std::vector<oa::data::mutators::PipelineMutator> listed;
    std::vector<std::string> versions;
    listed.reserve(options.mutators.size());
    versions.reserve(options.mutators.size());
    for (std::size_t index = 0; index < options.mutators.size(); ++index) {
        const std::string& value = options.mutators[index];
        const bool from_file =
            index < options.mutator_fields.size() && !options.mutator_fields[index].empty();
        const std::string prefix = from_file
                                       ? launch_source + ": " + options.mutator_fields[index] + ": "
                                       : std::string("--mutator: ");
        std::string error;
        const std::optional<oa::data::mutators::MutatorChoice> choice =
            oa::data::mutators::parse_choice(value, &error);
        if (!choice) {
            if (from_file)
                throw netgame::LaunchRefusal(prefix + error);
            throw std::runtime_error(prefix + error);
        }
        if (!choice->by_path && !player) {
            throw netgame::LaunchRefusal(
                prefix + "no player folder can be worked out for \"" + choice->target + "\""
            );
        }
        const std::string version = index < options.mutator_versions.size()
                                        ? options.mutator_versions[index]
                                        : std::string{};
        oa::data::mutators::ResolvedMutator resolved;
        if (!resolve_run_mutator(value, version, mutators_folder, &resolved, &error))
            throw netgame::LaunchRefusal(prefix + error);
        oa::data::mutators::PipelineMutator entry;
        entry.package = resolved.package_folder;
        entry.manifest = resolved.manifest;
        entry.options = resolved.options;
        versions.push_back(oa::formats::oamut::version_text(resolved.manifest.version));
        listed.push_back(std::move(entry));
    }
    const std::string refusal = oa::data::mutators::list_refusal(listed, versions);
    if (!refusal.empty()) {
        const bool from_file =
            !options.mutator_fields.empty() && !options.mutator_fields.front().empty();
        throw netgame::LaunchRefusal(
            (from_file ? launch_source + ": content.mutators: " : std::string("--mutator: ")) +
            refusal
        );
    }
}

/// Refuses a launch brain the game cannot seat, before a window opens.
///
/// An id that is not kebab-case was already refused while the launch was
/// read. One that is not installed, or whose manifest does not read, ends
/// the run with status 2 and names the id. An empty id is the built-in
/// player and is not looked up. With no player folder, a named id is
/// refused the same way.
///
/// @param options the parsed command line
void refuse_unusable_brains(const Options& options) {
    namespace launch = oa::ui::frontend_multiplayer::launch;
    const auto& net = netgame_context().options;
    if (!net.launch_planned)
        return;
    const launch::LaunchBlock& block = netgame_context().launch.block;
    const int32_t count = std::min(block.computer_count, launch::kLaunchComputerLimit);
    bool named = false;
    for (int32_t index = 0; index < count; ++index) {
        if (block.computers[index].brain[0] != '\0')
            named = true;
    }
    if (!named)
        return;
    bool from_option = false;
    for (const auto& replaced : net.launch_replaced) {
        if (replaced.first == "computer-players" && replaced.second.starts_with("--"))
            from_option = true;
    }
    oa::data::brain_library::Listing listing;
    if (const std::optional<fs::path> player = launch_user_folder(options)) {
        listing = oa::data::brain_library::list_installed(
            *player / std::string(oa::data::brain_library::brains_folder_name)
        );
    }
    for (int32_t index = 0; index < count; ++index) {
        const char* id = block.computers[index].brain;
        if (id[0] == '\0')
            continue;
        if (oa::data::brain_library::find(listing, id) != nullptr)
            continue;
        const std::string field = "computer-players[" + std::to_string(index + 1) + "].brain";
        const std::string place =
            from_option ? std::string("--computer-player brain") : launch_place(field);
        throw netgame::LaunchRefusal(place + ": " + id + " is not installed");
    }
}

/// Runs the game once: finds the game folders and their profile, with the
/// Game files screen where the platform brings game files in and none is
/// usable, opens the window the first time, plays the intro movies the
/// first time, mounts the archives and runs a runtime on them until it ends.
///
/// Throws std::runtime_error when the game folder cannot be played.
///
/// @param options the parsed command line, with restarts set
/// @param extension the extensions' combined hooks
/// @param display the window and its renderer, opened once
/// @param game_files the import's folders, the preferences and the recovery,
///     where the platform brings game files in
/// @param lookup_log --trace-lookups' log; null without it
/// @param switch_memory --check-mod-switch's working sets
/// @param[in,out] carried what the run before left for this run: the Library
///     to open again; what this run leaves for the next
/// @return the run's exit status, or kSoftRestartStatus when SWITCH ended it
int run_once(
    Options options,
    const Extension& extension,
    HostDisplay& display,
    GameFilesStart& game_files,
    LookupLog* lookup_log,
    ModSwitchMemory& switch_memory,
    RestartCarry& carried
) {
    options.game_dir_named = !options.game_dir.empty();
    start_player_evicted_fetch(options);
    resolve_launch_mod(options);
    refuse_unusable_mutators(options);
    refuse_unusable_brains(options);
    // Captured before game_dir is filled with the folder this start plays:
    // the second compose, after a copy made on this start, still runs.
    const bool mount_copy = mount_managed_copy(options);
    // A soft restart reads the settings the switch saved: the mod the Game
    // files screen would play and the backups setting.
    if (options.restarts > 0 && game_files.installed &&
        (!options.unattended || options.preferences_file)) {
        game_files.values =
            oa::platform::preferences::load(preference_file(options.preferences_file));
        game_files.backed_up = oa::ui::engine_settings::read_settings(game_files.values, {}, false)
                                   .game_files_backed_up;
    }
    // The game folder's profile, --mod's or the mod folder's or its own,
    // is resolved while the folder is inspected, before any archive is
    // mounted, so that one the engine cannot use stops the run with its
    // errors.
    if (options.check_folder_chooser) {
        ChooserDisplay chooser{&display, &options};
        const bool passed =
            run_folder_chooser_key_check(options, display_hooks(chooser), options.snapshot);
        return passed ? 0 : 1;
    }
    // A start that does not name --game-dir mounts the player's copy. The
    // choice is read from the preferences on every start, including the soft
    // restart a mod switch uses, so a change of choice is what the next start
    // mounts. --game-dir keeps the scan of the named folder.
    // A listing starts no video. Any other start brings SDL's video up
    // first, which hands over a launch file the system opened the game with:
    // that file's launch is this start's, taken before the folder is
    // resolved, which the launch's content chooses.
    const bool listing = netgame_context().options.list_content;
    if (!options.headless_check && !listing)
        display.start_video(options);
    if (options.restarts == 0 && display.video_started)
        take_launch_opened_at_start(options);
    // The window is up before the folder is resolved, so a slow look at the
    // copy does not leave the screen blank. A run nobody watches can ask no
    // dialog, so that look leaves this thread and the window keeps drawing.
    // A listing, and a launch until its content and map are checked, keep
    // the window closed.
    if (!options.headless_check && !display.initialized && !window_waits_for_launch())
        display.initialize(options);
    if (options.check_window_place) {
        if (!display.initialized)
            display.initialize(options);
        const bool command_line = options.launch.display_option != 0 || options.window_resolution;
        return check_saved_window_place(
            display.window, preference_file(options.preferences_file), command_line
        );
    }
    std::optional<GameDirectory> game_directory;
    // Language, the game and the copy are the first thing a new player sees.
    // While they will, the system's folder dialog, the in-engine locator and
    // the Game files screen stay closed: those were what showed before the
    // language step. The look itself still runs on the side thread when a
    // run nobody watches has the window up. A listing shows none of them,
    // and no setup either: it asks nothing.
    const bool setup_now = !listing && setup_before_game_due(options, mount_copy);
    const bool dialogs_closed = setup_now || listing;
    GameFilesNeeded needed;
    const uint32_t folder_began = oa::platform::tick_ms();
    const auto resolve_folder = [&] {
        if (mount_copy)
            game_directory = compose_for_run(options);
        if (!game_directory)
            game_directory = find_game_directory(
                options,
                dialogs_closed
                    ? nullptr
                    : ((game_files.offered || folder_chooser_offered(options)) ? &needed : nullptr),
                dialogs_closed
            );
    };
    const bool folder_aside = display.initialized && (options.unattended || options.headless_check);
    // An interactive start composes the managed copy aside. That compose
    // reads the archives, which is what iCloud holds. A folder dialog stays
    // on this thread, inside find_game_directory.
    const bool compose_aside = display.initialized && !folder_aside && mount_copy;
    if (options.check_icloud_wait && !compose_aside)
        throw std::runtime_error("--check-icloud-wait draws in a window");
    if (compose_aside)
        apply_stored_interface_language(options);
    if (options.check_icloud_wait)
        arm_icloud_wait(options);
    if (compose_aside) {
        bool saw_icloud = false;
        std::string published_line;
        // A close while iCloud holds a read ends the game: the read cannot be
        // stopped, and waiting for it is what made players force quit.
        const LongWorkResult waited = run_long_work(
            display.window,
            display.renderer_host.renderer(),
            &display.renderer_host,
            "Open Annihilation",
            "Loading the game's archives",
            [&](oa::ui::paint::Canvas& canvas) {
                if (options.check_icloud_wait) {
                    icloud_wait_draws().fetch_add(1, std::memory_order_release);
                    if (archive_wait_line() == icloud_download_line)
                        saw_icloud = true;
                }
                fill_progress_behind(canvas, nullptr);
            },
            [&](LongWork& work) {
                const ArchiveWait progress{
                    [](void* context, std::string_view key) {
                        static_cast<LongWork*>(context)->use_line(key);
                    },
                    [](void* context, int32_t done, int32_t whole) {
                        auto& job = *static_cast<LongWork*>(context);
                        job.whole.store(whole, std::memory_order_release);
                        job.done.store(done, std::memory_order_release);
                    },
                    [](void* context) {
                        return static_cast<LongWork*>(context)->stop.load(
                            std::memory_order_acquire
                        );
                    },
                    &work,
                };
                set_archive_wait(&progress);
                struct Clear {
                    ~Clear() { set_archive_wait(nullptr); }
                } clear;
                game_directory = compose_for_run(options);
                published_line = work.line_key({});
            },
            long_work_show_after_ms,
            LongWorkClose::ends_game
        );
        if (options.check_icloud_wait)
            require_icloud_download_lines();
        // Only macOS keeps a file's bytes in iCloud. Elsewhere the archive
        // the check marks is read at once, and the line never names iCloud.
        if (options.check_icloud_wait && !oa::platform::host_reports_dataless_files()) {
            if (!published_line.empty())
                throw std::runtime_error(
                    "icloud wait: the line changed to \"" + published_line +
                    "\" on a system that keeps no file in iCloud"
                );
            std::cout << "icloud wait: this system keeps no file in iCloud; the line stays\n";
        } else if (options.check_icloud_wait) {
            const int draws = icloud_wait_draws().load(std::memory_order_acquire);
            if (!waited.shown || !waited.bar || draws < 2 || !saw_icloud)
                throw std::runtime_error(
                    "icloud wait: the window did not keep drawing the iCloud line (" +
                    std::to_string(draws) + " frames)"
                );
            const std::string shown(oa::data::languages::interface_text(icloud_download_line));
            std::cout << "icloud wait: " << shown << '\n';
            if (shown == icloud_download_line &&
                oa::data::languages::interface_language().tag != "en" &&
                oa::data::languages::interface_language().tag != "system")
                throw std::runtime_error("icloud wait: the line was not in the player's language");
        }
    }
    if (folder_aside) {
        // A folder dialog stays on this thread. A run nobody watches can look
        // aside, and the progress dialog covers a look that lasts.
        const std::string_view folder_line =
            options.restarts == 0          ? std::string_view("Loading the game's archives")
            : options.restart_line.empty() ? std::string_view("Applying your changes")
                                           : std::string_view(options.restart_line);
        std::ignore = run_long_work(
            display.window,
            display.renderer_host.renderer(),
            &display.renderer_host,
            "Open Annihilation",
            folder_line,
            [](oa::ui::paint::Canvas& canvas) { fill_progress_behind(canvas, nullptr); },
            [&](LongWork&) { resolve_folder(); }
        );
    } else if (!compose_aside) {
        resolve_folder();
    } else if (!game_directory) {
        // The copy did not mount. A folder dialog, when one is due, stays
        // on this thread.
        resolve_folder();
    }
    oa::platform::log_files::note_timing_span("game folder", folder_began);
    if (options.check_icloud_wait) {
        if (!game_directory || game_directory->archives.empty())
            throw std::runtime_error("icloud wait: the folder step did not continue");
        std::cout << "icloud wait: continued\n";
        return 0;
    }
    if (listing && !game_directory)
        throw std::runtime_error(
            "--list-content: no Total Annihilation folder is known; name it with --game-dir PATH"
        );
    if (!setup_now) {
        // Without a usable folder where the in-engine chooser is asked for, it picks one.
        if (!game_directory && needed.needed && needed.chooser) {
            ChooserDisplay chooser{&display, &options};
            const FolderChooserDisplayHooks hooks = display_hooks(chooser);
            if (!run_folder_chooser_until_resolved(options, hooks, needed, game_directory))
                return 0;
        }
        // Without a usable folder, the Game files screen brings one in.
        if (!run_game_files_until_resolved(options, display, game_files, needed, game_directory))
            return game_files_status(options, 0);
        if (!game_directory)
            return game_files_status(options, 1);
    }
    // The game files are kept out of device backups unless the player put
    // them back in, with or without the screen.
    if (game_files.installed)
        game_files::apply_backup_setting(
            game_files_hooks(), game_files.paths, game_files.backed_up
        );
    // A folder that held the demo's installer is played from the folder
    // its archive was unpacked to, and remembered as chosen. A folder found
    // on this machine is remembered too, and the main menu says where it was
    // found.
    if (game_directory) {
        options.game_dir = game_directory->installation;
        if (game_directory->source == GameDirectorySource::chosen ||
            game_directory->source == GameDirectorySource::found)
            options.remember_game_dir = game_directory->path;
        if (game_directory->source == GameDirectorySource::found && game_directory->found_from)
            options.found_install_notice = found_install_notice(*game_directory->found_from);
        if (!fs::is_directory(options.game_dir))
            throw std::runtime_error(
                "game directory does not exist: " + path_to_utf8(options.game_dir) +
                " (name it with --game-dir PATH)"
            );
    }
#if OA_SELF_CHECKS
    if (options.check_renderer_proof)
        return renderer_proof::run(options.game_dir, options.snapshot);
#endif
    // Quit leaves before the silent copy. Skip with no folder leaves too,
    // without opening a dialog. A copy that finished here is the one the
    // start plays, so the silent copy does not run again.
    bool copied_on_setup = false;
    if (setup_now) {
        if (!display.initialized)
            display.initialize(options);
        fs::path found_install;
        if (game_directory && game_directory->source != GameDirectorySource::managed)
            found_install = game_directory->installation;
        const SetupBefore setup = run_setup_before_game(
            options,
            SetupDisplay{display.window, display.renderer_host.renderer(), &display.renderer_host},
            mount_copy,
            found_install
        );
        if (setup == SetupBefore::quit)
            return game_files_status(options, 0);
        copied_on_setup = setup == SetupBefore::copied;
        if (setup == SetupBefore::skipped && !game_directory)
            return game_files_status(options, 0);
    }
    // The setup shows while the copy is missing or not finished. A start
    // here with no folder has a copy it cannot mount, or no player's folder,
    // and says why rather than ending without a word.
    if (!game_directory && !copied_on_setup)
        return stop_without_copy(options);
    if (!copied_on_setup)
        publish_managed_copy(options, options.game_dir, options.game_dir_named);
    // A copy made on this start is what the start plays. --game-dir is left
    // on the folder it named.
    if (mount_copy) {
        if (std::optional<GameDirectory> copied = compose_for_run(options))
            game_directory = std::move(copied);
        if (!game_directory)
            return stop_without_copy(options);
        options.game_dir = game_directory->installation;
    }
    if (!game_directory)
        return game_files_status(options, 1);
    options.managed_content = game_directory->source == GameDirectorySource::managed;
    options.game_folders = game_directory->folders;
    if (options.game_folders.empty())
        options.game_folders = {options.game_dir};
    options.layered_mod_folder.clear();
    if (!game_directory->mod_folder.empty()) {
        std::error_code absolute_error;
        const fs::path absolute = fs::absolute(game_directory->mod_folder, absolute_error);
        options.layered_mod_folder =
            (absolute_error ? game_directory->mod_folder : absolute).lexically_normal();
    }
    options.mod_profile = game_directory->profile;
    // --archive names the archives and skips the inspection; a --mod
    // profile still sets the layout the data is read by.
    if (!options.archives.empty() && !options.mod_file.empty()) {
        auto resolved = resolve_folder_profile(
            options.game_folders,
            {{}, options.mod_file, options.accept_unimplemented_hacks, nullptr}
        );
        if (!resolved.errors.empty()) {
            std::string message = "the mod profile cannot be used:";
            for (const auto& error : resolved.errors)
                message += "\n  " + error;
            throw std::runtime_error(message);
        }
        options.mod_profile = std::move(resolved.profile);
        game_directory->profile_warnings = std::move(resolved.warnings);
    }
    if (options.mod_profile) {
        // Its limits size the game's tables from the start, and its
        // rules reach every match.
        report_mod_profile(*options.mod_profile, game_directory->profile_warnings, std::cout);
    } else if (!options.layered_mod_folder.empty()) {
        // Expansion bundles add folders and are not a mod. Only the mod
        // folder is reported when it has no profile.
        std::cout << "open-annihilation: the mod folder "
                  << path_to_utf8(options.layered_mod_folder)
                  << " holds no oamod.yaml; its files are layered over the game folder, "
                     "which plays by 3.1c's own rules\n";
    }
    // Every later read of game data uses the profile's layout.
    oa::data::defs::use_data_layout(data_layout_of(options.mod_profile.get()));
    if (const std::optional<int> listed = publish_launch_catalogue(options, *game_directory))
        return *listed;
    if (!options.headless_check && !display.initialized)
        display.initialize(options);
    // The Game files check's management route runs its pass over the
    // installed folder before the game starts.
    if constexpr (self_checks_built)
        if (options.check_game_files && options.game_files_route == GameFilesRoute::manage &&
            !run_game_files_manage_check(options, display.window, display.renderer_host.renderer()))
            return finish_game_files_check(options, 1);
    // While the setup will still show, the movies wait until its last step,
    // and the main menu's hum waits for them (Runtime::play_menu_voice). A
    // start that does not show it plays them here, before the archives are
    // mounted and the runtime opens the main menu, which starts the hum.
    if (options.restarts == 0 && setup_shows_over_menu(options))
        options.intro_after_setup = true;
    else if (options.restarts == 0)
        play_intro(options, options.headless_check ? nullptr : &display);
    oa::base::float_precision::restore_program_float_control();
    oa::AssetStore assets(options.game_folders);
    if (lookup_log != nullptr)
        assets.observe_lookups(lookup_log->observer());
    auto archives = options.archives;
    if (archives.empty())
        archives = std::move(game_directory->archives);
    if (archives.empty())
        throw std::runtime_error("no game archives were selected");
    const uint32_t mount_began = oa::platform::tick_ms();
    // A switch made in the Library leaves the window as it began, and it
    // stays, with no dialog over it.
    const std::shared_ptr<const renderer::Surface> restart_picture = options.restart_picture;
    const bool mount_shown = display.window != nullptr && !options.headless_check;
    const std::string_view mount_line =
        options.restarts == 0          ? std::string_view("Loading the game's archives")
        : options.restart_line.empty() ? std::string_view("Applying your changes")
                                       : std::string_view(options.restart_line);
    std::ignore = run_long_work(
        mount_shown ? display.window : nullptr,
        mount_shown ? display.renderer_host.renderer() : nullptr,
        mount_shown ? &display.renderer_host : nullptr,
        "Open Annihilation",
        mount_line,
        [&restart_picture](oa::ui::paint::Canvas& canvas) {
            fill_progress_behind(canvas, restart_picture.get());
        },
        [&](LongWork& work) {
            const std::size_t count = archives.size();
            const int32_t whole =
                count > static_cast<std::size_t>(std::numeric_limits<int32_t>::max())
                    ? std::numeric_limits<int32_t>::max()
                    : static_cast<int32_t>(count);
            work.whole.store(whole > 0 ? whole : 1, std::memory_order_release);
            work.done.store(0, std::memory_order_release);
            int32_t mounted = 0;
            for (const auto& candidate : archives) {
                work.done.store(mounted, std::memory_order_release);
                const auto archive =
                    candidate.is_absolute() ? candidate : options.game_dir / candidate;
                assets.mount(archive);
                if (mounted < whole)
                    ++mounted;
            }
            work.done.store(whole > 0 ? whole : 1, std::memory_order_release);
        },
        long_work_show_after_ms,
        LongWorkClose::after_job,
        restart_picture ? LongWorkLook::behind_only : LongWorkLook::dialog
    );
    oa::platform::log_files::note_timing_span("mounting", mount_began);
    if (game_directory->demo.outcome == DemoOutcome::ready)
        std::cout << "open-annihilation: " << describe_ready(game_directory->demo) << '\n';
    // A capture starts before the runtime, which starts the menu's
    // sound, so that its own sound device opens first and sets the mix.
    std::unique_ptr<VideoCapture> capture;
    if (!options.capture_video.empty() && options.restarts == 0) {
        int width = 0;
        int height = 0;
        if (!SDL_GetWindowSizeInPixels(display.window, &width, &height))
            throw std::runtime_error(std::string("SDL window size: ") + SDL_GetError());
        capture = std::make_unique<VideoCapture>(options.capture_video, width, height);
    }
    // The Game files check reads its options again for its verdict, and
    // --check-mod-switch its own, once the runtime, which takes them, has run.
    std::optional<Options> checked_options;
    if (options.check_game_files)
        checked_options = options;
    const bool check_mod_switch = options.check_mod_switch;
    const uint32_t restarts = options.restarts;
    const bool window_shown = display.window != nullptr && !options.headless_check;
    // The runtime holds hundreds of kilobytes of game state, so it lives
    // on the heap: the main thread's stack is 1 MiB on Windows.
    const auto runtime = std::make_unique<Runtime>(
        std::move(options),
        assets,
        extension,
        display.window,
        display.renderer_host.renderer(),
        &display.renderer_host
    );
    if constexpr (!kTouchFirst)
        runtime->attach_window_place();

    // Destroyed before the runtime, so a remember after it writes the file itself.
    struct DetachWindowPlace {
        ~DetachWindowPlace() { detach_window_place_values(); }
    } detach_window_place;

    runtime->take_video_capture(std::move(capture));
    runtime->take_full_screen_switch(display.full_screen);
    if (check_mod_switch && restarts == 0)
        switch_memory.warm_progress_dialog(display.window, display.renderer_host.renderer());
    runtime->take_library_return(std::move(carried.library));
    if (check_mod_switch)
        switch_memory.sample(restarts);
    const int status = runtime->run();
    if (!runtime->soft_restart_requested()) {
        if (checked_options)
            return game_files_status(*checked_options, status);
        return check_mod_switch && status == 0 ? switch_memory.verdict() : status;
    }
    // The next runtime takes over the window as Alt+Enter left it, and the
    // renderer without this runtime's textures; this runtime and its
    // archives go before the next are made.
    display.full_screen = runtime->full_screen_switch();
    carried.library = runtime->library_return();
    carried.line = runtime->restart_line();
    carried.picture = runtime->switch_picture();
    // The switch's picture, or its dialog, shows at once, and stays while the
    // game reloads.
    if (carried.picture && window_shown)
        show_still_frame(
            display.window, display.renderer_host.renderer(), &display.renderer_host, *carried.picture
        );
    else if (!carried.line.empty() && window_shown)
        show_work_frame(
            display.window,
            display.renderer_host.renderer(),
            &display.renderer_host,
            oa::data::languages::interface_text("Open Annihilation"),
            carried.line
        );
    runtime->release_renderer_textures();
    // The characters each game font draws are kept by the address of its
    // glyphs, and this runtime's fonts go with it: the next mod's fonts may
    // be loaded at the same addresses.
    oa::ui::frontend_renderer::forget_gui_font_characters();
    oa::ui::gadget_render::forget_font_characters();
    return kSoftRestartStatus;
}

// Sends the game's standard output and standard error to a log in `folder`.
// When the log cannot be opened they stay where they were, and standard error
// says why.
void start_log_in(const fs::path& folder) {
    if (!oa::platform::log_files::begin(folder) && !oa::platform::log_files::lines_held_for_log())
        std::cerr << "open-annihilation: cannot open a log in " << path_to_utf8(folder)
                  << "; the output stays here\n";
}

// Sends the game's standard output and standard error to the logs folder in
// the per-user folder. Without a per-user folder they stay where they were.
void start_log() {
    try {
        start_log_in(oa::platform::preferences::data_directory() / "logs");
    } catch (const std::exception& error) {
        if (!oa::platform::log_files::lines_held_for_log())
            std::cerr << "open-annihilation: no log: " << error.what() << '\n';
    }
}

/// The longest a second start waits for the running copy to take the mod
/// packages it handed over, in milliseconds.
constexpr uint32_t kHandoffWaitMs = 3000;
/// How often it looks whether they were taken, in milliseconds.
constexpr uint32_t kHandoffPollMs = 50;
/// The slices a wait between renames pumps SDL's events in, in milliseconds.
constexpr uint32_t kWaitSliceMs = 50;

/// Waits between tries of a rename while the window shows, keeping it
/// answering: SDL's events are pumped every slice.
///
/// @param milliseconds how long
void wait_pumping(void*, uint32_t milliseconds) {
    while (milliseconds > 0) {
        const uint32_t slice = std::min(milliseconds, kWaitSliceMs);
        SDL_PumpEvents();
        oa::base::threads::sleep_ms(slice);
        milliseconds -= slice;
    }
}

/// Returns the game's data folder: --data-dir, else the platform's.
///
/// @param options the parsed command line
/// @return the folder; nothing when there is none
std::optional<fs::path> data_folder_of(const Options& options) {
    if (options.data_dir)
        return *options.data_dir;
    try {
        return oa::platform::preferences::data_directory();
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

/// Settles what a stopped install left in each kind's folder of the player's
/// own folder, before each run resolves the mod folder: a stop mid-change
/// may have moved the folder the settings play. The discard folders it found
/// are kept for the runtime to delete.
///
/// @param options the parsed command line
void recover_package_installs(const Options& options) {
    try {
        const fs::path preferences = preference_file(options.preferences_file);
        std::error_code error;
        const oa::platform::preferences::Values values =
            fs::exists(preferences, error) ? oa::platform::preferences::load(preferences)
                                           : oa::platform::preferences::Values{};
        std::string note;
        const fs::path folder = own_user_folder(
            options.user_folder, options.preferences_file, preferences, values, note
        );
        for (const package_install::PackageKind& kind : package_install::package_kinds()) {
            const auto recovery =
                package_install::recover_changes(kind, folder / std::string(kind.root_folder));
            package_install::keep_discards(recovery.discards);
        }
    } catch (const std::exception& error) {
        std::cerr << "open-annihilation: package install: nothing settled: " << error.what()
                  << '\n';
    }
}

/// Writes what a file-type registration did, one log line each.
///
/// @param done what the registration or withdrawal did
void log_file_types(const oa::platform::file_types::Registration& done) {
    for (const auto& line : done.lines)
        std::cerr << "open-annihilation: file types: " << line << '\n';
    if (!done.error.empty())
        std::cerr << "open-annihilation: file types: " << done.error << '\n';
}

/// Tells whether a registration did not take, so a withdrawal must not run
/// over whatever another copy or another program left.
///
/// @param done what the registration did
/// @return true when this system does not register, the registration failed,
///     or it skipped
bool file_types_not_applied(const oa::platform::file_types::Registration& done) {
    if (!done.supported || !done.error.empty())
        return true;
    for (const auto& line : done.lines)
        if (line.rfind("skipped:", 0) == 0)
            return true;
    return false;
}

/// The file types whose preference is not the word off. A missing file, a
/// missing key, or a failure to read the preferences leaves all seven on.
///
/// @return the types to register
oa::platform::file_types::TypeSet enabled_file_types() {
    namespace file_types = oa::platform::file_types;
    file_types::TypeSet enabled;
    try {
        const fs::path preferences = preference_file(std::nullopt);
        std::error_code error;
        if (!fs::exists(preferences, error) || error)
            return enabled;
        const oa::platform::preferences::Values values =
            oa::platform::preferences::load(preferences);
        const auto off = [&values](std::string_view key) {
            const auto found = values.find(std::string{key});
            return found != values.end() && found->second == "off";
        };
        namespace key = oa::ui::engine_settings::key;
        if (off(key::file_type_oamod))
            enabled.mod = false;
        if (off(key::file_type_oalang))
            enabled.language = false;
        if (off(key::file_type_oamap))
            enabled.map = false;
        if (off(key::file_type_oareg))
            enabled.registry = false;
        if (off(key::file_type_oamut))
            enabled.mutator = false;
        if (off(key::file_type_oabrain))
            enabled.brain = false;
        if (off(key::file_type_oalaunch))
            enabled.launch = false;
    } catch (const std::exception&) {
        return {};
    }
    return enabled;
}

/// Registers the types in `enabled` and, where that took, withdraws the rest.
///
/// @param enabled the types the player left on
void apply_file_type_set(oa::platform::file_types::TypeSet enabled) {
    namespace file_types = oa::platform::file_types;
    const auto executable = file_types::running_executable();
    if (!executable)
        return;
    const auto done = file_types::register_file_types(*executable, window_icon_png(), {}, enabled);
    log_file_types(done);
    if (file_types_not_applied(done))
        return;
    log_file_types(file_types::withdraw_file_types(enabled.complement()));
}

/// Makes this copy of the game the opener of the file types the player left
/// on, where the system takes it at run time; only a start someone plays
/// does: no check, test or unattended run, and none that names its own
/// preferences, player's or data folder, or draws no window. A type left
/// off is withdrawn then, not at the next start after this one.
///
/// @param options the parsed command line
void register_file_types_for_player(const Options& options) {
    if (options.headless_check || options.unattended || options.preferences_file ||
        options.user_folder || options.data_dir)
        return;
    if (const auto driver = oa::platform::environment_value("SDL_VIDEO_DRIVER");
        driver && (*driver == "dummy" || *driver == "offscreen"))
        return;
    apply_file_type_set(enabled_file_types());
}

/// Hands the files a start carries to the copy of the game already running,
/// when one holds the instance lock, or takes the lock for this copy's life
/// and its hand-off folder. Only a start someone plays does.
///
/// @param options the parsed command line
/// @param[out] lock the instance lock, when this copy takes it
/// @return true when the packages were handed over and this start ends
bool hand_over_or_lock(const Options& options, std::unique_ptr<package_install::FileLock>& lock) {
    if (options.headless_check || options.unattended || options.preferences_file)
        return false;
    const auto data = data_folder_of(options);
    if (!data)
        return false;
    const fs::path handoff = *data / std::string(package_install::handoff_folder_name);
    lock = package_install::take_instance_lock(
        *data / std::string(package_install::instance_lock_name)
    );
    if (lock) {
        package_install::set_handoff_folder(handoff);
        return false;
    }
    // Another copy runs: a start that carries files hands them to it.
    if (options.open_files.empty())
        return false;
    const auto written = package_install::hand_files_over(handoff, options.open_files);
    if (written.empty())
        return false;
#ifdef _WIN32
    // The running copy may bring its window forward.
    AllowSetForegroundWindow(ASFW_ANY);
#endif
    for (uint32_t waited = 0; waited < kHandoffWaitMs; waited += kHandoffPollMs) {
        std::error_code error;
        if (std::none_of(written.begin(), written.end(), [&error](const fs::path& request) {
                return fs::exists(request, error);
            }))
            break;
        oa::base::threads::sleep_ms(kHandoffPollMs);
    }
    if (!oa::platform::log_files::lines_held_for_log() ||
        !oa::platform::log_files::current_file().empty())
        std::cerr << "open-annihilation: the files went to the copy already running\n";
    return true;
}

// A fatal error goes to the log, and to the terminal the game was started
// from; a game started from the desktop has no terminal, so it shows in an
// error box instead.
void report_fatal(const std::string& message) {
    const auto line = "open-annihilation: " + message + "\n";
    if (!oa::platform::log_files::current_file().empty())
        std::fputs(line.c_str(), stderr);
    // Status lines own standard output. With no log yet, the message would
    // land on that stream, so it is kept off it.
    if (oa::platform::log_files::lines_held_for_log() &&
        oa::platform::log_files::current_file().empty())
        return;
    // With neither a terminal nor a box, the log holds the message.
    if (!oa::platform::log_files::write_to_terminal(line))
        std::ignore = SDL_ShowSimpleMessageBox(
            SDL_MESSAGEBOX_ERROR, "Open Annihilation", message.c_str(), nullptr
        );
}

/// Copies known game files into the player's folder when this start should.
///
/// --check-game-files drives the screen's own copy and is left to it. A
/// named --game-dir is used as it is. A run nobody is watching uses a copy
/// already there, or stops with a message. The installation is only read.
///
/// @param options the parsed command line
/// @param installation the folder the game will play from
/// @param game_dir_named true when the command line named --game-dir
void publish_managed_copy(
    const Options& options, const fs::path& installation, bool game_dir_named
) {
    if (options.check_game_files)
        return;
    ManagedCopyRequest request;
    request.installation = installation;
    request.game_dir_named = game_dir_named;
    request.unattended = options.unattended || options.headless_check;
    request.news = &std::cerr;
    try {
        const fs::path preferences = preference_file(options.preferences_file);
        std::error_code error;
        const oa::platform::preferences::Values values =
            fs::exists(preferences, error) ? oa::platform::preferences::load(preferences)
                                           : oa::platform::preferences::Values{};
        std::string note;
        request.user_folder = own_user_folder(
            options.user_folder, options.preferences_file, preferences, values, note
        );
        request.user_folder_known = true;
    } catch (const std::exception&) {
        request.user_folder_known = false;
    }
    ensure_managed_copy(request);
}

} // namespace

void note_launch_frame() {
    LaunchFrame& shown = launch_frame();
    if (shown.shown)
        return;
    shown.shown = true;
    oa::platform::log_files::note_timing("window first frame");
}

void pump_launch_window() {
    LaunchFrame& shown = launch_frame();
    if (!shown.shown || shown.released || shown.renderer == nullptr)
        return;
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
            std::ignore = SDL_PushEvent(&event);
            break;
        }
    }
    draw_launch_frame();
}

void release_launch_window() {
    launch_frame().released = true;
}

} // namespace oa::app

using namespace oa::app;

int main(int argc, char** argv) {
    const uint32_t process_started_ms = oa::platform::tick_ms();
    // --version prints one line and leaves before the log folder, the
    // preferences, the game data or a window are touched. Other options on
    // the same line are not read.
    if (version_argument_present(argc, argv))
        write_startup_line(running_engine_version_line());
#if defined(SDL_PLATFORM_MACOS) && SDL_VERSION_ATLEAST(3, 4, 0)
    // A held key repeats while text is typed, as on the other systems,
    // rather than opening macOS's accents menu. SDL reads the hint once, as
    // its video first starts, for the folder dialog or the window; an
    // environment variable of the hint's name decides instead.
    std::ignore = SDL_SetHint(SDL_HINT_MAC_PRESS_AND_HOLD, "0");
#endif
    // The folder the program keeps its log beside, asked of the system rather
    // than of SDL: SDL_GetBasePath() answers with nothing where its own path
    // discovery cannot run, and this wants a folder here regardless.
    error_log_folder = oa::platform::error_log_directory(oa::platform::program_directory().c_str());
    std::set_new_handler(handle_out_of_memory);
    oa::base::float_precision::program_float_control().hooks.changed = report_float_control_change;
    namespace automation = oa::app::automation;
    try {
        // Every registered extension fills its table before the command
        // line is parsed; the runtime gets the table that combines them.
        const ExtensionList extensions(registered_extensions());
        const Extension& extension = extensions.combined();
        // A launch piped in on standard input is read when no launch file is
        // named (cat host.oalaunch | open-annihilation).
        allow_piped_launch();
        Options parsed = parse_options(argc, argv, extension);
        // The Game files screen's check installs its scripted platform
        // before anything reads the hooks.
        if constexpr (self_checks_built)
            if (parsed.check_game_files)
                install_game_files_check(parsed);
        // --print-profile prints the resolved mod profile and stops.
        if (parsed.print_profile) {
            const int status = print_mod_profile(
                parsed.mod_file,
                parsed.mod_dir.empty() ? parsed.game_dir : parsed.mod_dir,
                parsed.accept_unimplemented_hacks,
                std::cout,
                std::cerr
            );
            // The version follows the profile, so a reader of the profile's
            // JSON still finds it at the start of the output.
            std::cout << running_engine_version_line() << '\n';
            return status;
        }
        if (!parsed.check_content_copy.empty())
            return run_content_copy_check(parsed.check_content_copy);
        if (!parsed.check_installer.empty())
            return run_installer_check(parsed.check_installer);
        // --status-stdout keeps the standard output the process was started
        // with, then the log takes both streams, so the status lines are the
        // only text on that output. --log-dir names the folder for any run.
        // A game started for play, from a terminal or the desktop, logs to the
        // logs folder. Checks, benchmarks and other scripted runs keep their
        // output where it goes, and so does a run whose output another
        // program captures, such as a test or a script. --list-content keeps
        // that standard output too, for its listing, whatever takes the log.
        if (automation::status_stdout_wanted()) {
            automation::capture_status_stdout();
            oa::platform::log_files::hold_lines_for_log();
        } else if (netgame_context().options.list_content) {
            automation::capture_status_stdout();
        }
        if (parsed.log_dir)
            start_log_in(*parsed.log_dir);
        else if (
            automation::status_stdout_wanted() || (!parsed.headless_check && !parsed.unattended &&
                                                   !oa::platform::log_files::output_captured())
        )
            start_log();
        oa::platform::log_files::note_timing_at("process start", process_started_ms);
        if (automation::status_following())
            automation::begin_status_lines(running_engine_version_line());
        // The files the start carries are opened once the main menu shows,
        // unless another copy already runs and takes them.
        std::unique_ptr<package_install::FileLock> instance_lock;
        if (hand_over_or_lock(parsed, instance_lock)) {
            const int code = automation::status_exit_code(0);
            automation::finish_status_lines(code);
            return code;
        }
        for (const auto& file : parsed.open_files)
            package_install::post_opened_file(file);
        register_file_types_for_player(parsed);
        // Where the platform brings game files in, what a stopped import or
        // a change waiting for this start left is taken up before the
        // folder is looked for.
        GameFilesStart game_files = start_game_files(parsed);
        // The content check plays the player's copy, so Core Contingency's
        // maps can leave the skirmish list. --game-dir is only the source of
        // that copy: an unattended start would refuse to make it.
        if (parsed.check_library_content && !parsed.game_dir.empty()) {
            Options seeding = parsed;
            seeding.unattended = false;
            seeding.headless_check = false;
            publish_managed_copy(seeding, parsed.game_dir, false);
            parsed.game_dir.clear();
        }
        // The window, its renderer and the lookup log last the whole
        // process: the window opens where the game opens it, or early for
        // the Game files screen, and only once, and a switch of the mod from
        // the settings finds the game folders, mounts the archives and
        // builds the runtime afresh on them (a soft restart).
        HostDisplay display;
        std::unique_ptr<LookupLog> lookup_log;
        if (!parsed.trace_lookups.empty())
            lookup_log = std::make_unique<LookupLog>(parsed.trace_lookups);
        ModSwitchMemory switch_memory;
        // What a switch of the mod in the Library left, for the next run.
        RestartCarry carried;
        for (uint32_t restarts = 0;; ++restarts) {
            // What a stop, or the change between runs, left in the player's
            // Mods folder is settled before the mod folder is looked for.
            recover_package_installs(parsed);
            auto options = parsed;
            options.restarts = restarts;
            options.restart_line = std::exchange(carried.line, {});
            options.restart_picture = std::exchange(carried.picture, {});
            options.native_density_windows = kNativeDensityWindows;
            // A soft restart returns to the main menu without the movies.
            if (restarts > 0)
                options.skip_intro = true;
            const int status = run_once(
                std::move(options),
                extension,
                display,
                game_files,
                lookup_log.get(),
                switch_memory,
                carried
            );
            // A change to the mod played is made now, with the runtime and
            // its archives gone: before the next run reads the folder, or,
            // when the player quits, before the game ends, so that an update
            // held until they left the mod is not lost.
            package_install::ChangeOptions change{};
            change.hooks.wait = wait_pumping;
            if (status != kSoftRestartStatus) {
                package_install::finish_pending_change(change);
                const int code = automation::status_exit_code(status);
                automation::finish_status_lines(code);
                return code;
            }
            package_install::finish_pending_change(change);
        }
    } catch (const netgame::LaunchRefusal& error) {
        // A launch refused once the status lines began ends them with an
        // exit line, before the refusal is reported.
        automation::finish_refused_status_lines(kLaunchRefusedStatus);
        report_fatal(error.what());
        return kLaunchRefusedStatus;
    } catch (const fs::filesystem_error& error) {
        // A path longer than the system opens is reported with its length,
        // the limit and what to do.
        const auto reason = error.code() == std::errc::filename_too_long
                                ? path_length_problem(error.path1())
                                : std::string();
        automation::finish_status_lines(1);
        report_fatal(
            reason.empty() ? std::string(error.what()) : path_to_utf8(error.path1()) + ": " + reason
        );
        return 1;
    } catch (const std::exception& error) {
        automation::finish_status_lines(1);
        report_fatal(error.what());
        return 1;
    }
}

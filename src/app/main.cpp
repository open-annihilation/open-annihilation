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
#include "screen_size.hpp"
#include "oa/app/extension_list.hpp"
#include "oa/app/full_screen.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/game_files_hooks.hpp"
#include "oa/app/game_files_import.hpp"
#include "oa/app/input_hints.hpp"
#include "oa/app/mod_install.hpp"
#include "oa/app/mod_install/handoff.hpp"
#include "oa/app/mod_install/inbox.hpp"
#include "oa/app/mod_profile_loader.hpp"
#include "oa/app/platform_hooks.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/app/video_capture.hpp"
#include "oa/app/window_icon.hpp"
#include "oa/base/float_precision.hpp"
#include "oa/base/threads.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/media/intro_player.hpp"
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
#include <cinttypes>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
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

#ifndef OA_ENGINE_VERSION
#error "OA_ENGINE_VERSION names the engine's version, which the renderer records are written under"
#endif
#ifndef OA_NATIVE_DENSITY_WINDOWS
#error "OA_NATIVE_DENSITY_WINDOWS (0 or 1) says whether the window opens at native density"
#endif
#ifndef OA_TOUCH_FIRST
#error "OA_TOUCH_FIRST (0 or 1) says whether the touch controls are on from the start"
#endif

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

// The folder ErrorLog.txt goes in, found once at start-up so that the
// out-of-memory report does not have to look for it.
std::string error_log_folder;

/// Reports running out of memory and ends the process.
///
/// Appends the out-of-memory message to ErrorLog.txt beside the application
/// (error_log_folder), shows it in an "Open Annihilation" error box, then
/// exits at once with status 3 (kOutOfMemoryExitStatus).
[[noreturn]] void handle_out_of_memory() {
    // The process ends whatever happens: a log that cannot be written still
    // leaves the box, and a box that cannot be shown the log.
    std::ignore = oa::platform::append_error_log(
        error_log_folder.c_str(), oa::platform::out_of_memory_message
    );
    // The box needs the pointer, which full screen keeps on the window.
    release_pointer(SDL_GetGrabbedWindow());
    std::ignore = SDL_ShowSimpleMessageBox(
        SDL_MESSAGEBOX_ERROR, "Open Annihilation", oa::platform::out_of_memory_message, nullptr
    );
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
    place.engine_version = OA_ENGINE_VERSION;
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

struct HostDisplay {
    SDL_Window* window = nullptr;
    // The window's renderer, made by walking SDL's render drivers, and what
    // the probe found of it.
    RendererHost renderer_host{};
    bool active = false;
    /// initialize has run: the window opens once, early for the Game files
    /// screen or where the game opens it.
    bool initialized = false;
    // The mode Alt+Enter last asked for while the intro movies play.
    FullScreenSwitch full_screen{};

    /// Starts SDL's video and sound and opens the window, at the size
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
    /// (shown_screen_size). A build
    /// whose touch controls are on from the start sets their input hints
    /// before SDL starts (set_input_hints), and the
    /// platform's window_ready hook, when there is one, is told once the
    /// window and its renderer are made.
    ///
    /// Throws std::runtime_error when SDL, the window or the renderer fails.
    /// It runs at most once (initialized).
    ///
    /// @param options the parsed command line
    void initialize(const Options& options) {
        initialized = true;
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
        // version. The game writes no scale-level key, so no rung is
        // remembered for it.
        if (const auto driver = renderer_state::native_density_driver(
                renderer_host.records().records(), place.engine_version
            ))
            density.record_driver = std::string(*driver);
        window = SDL_CreateWindow(
            "Open Annihilation",
            options.window_resolution ? options.match_width
            : sized                   ? screen.width
                                      : fallback.width,
            options.window_resolution ? options.match_height
            : sized                   ? screen.height
                                      : fallback.height,
            game_window_flags(
                options.start_full_screen && !sized, decide_window_density(density).native
            )
        );
        if (window == nullptr)
            throw std::runtime_error(std::string("SDL_CreateWindow: ") + SDL_GetError());
        // Full screen shows the size through a display mode of it, or draws
        // the match at it and scales it to the screen (run_full_screen_method);
        // the runtime follows the size applied.
        if (sized) {
            take_screen_size(window, screen, options.start_full_screen, run_full_screen_method());
            full_screen.screen_width = screen.width;
            full_screen.screen_height = screen.height;
        }
        report_window_size(window, desktop, steam_game_mode);
        set_window_icon(window);
        renderer_host.create(window, start_faults(options));
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
    }

    /// Ends the run's renderer records cleanly, deleting the sentinel, and
    /// closes the window and SDL. It runs at every exit through main, the
    /// fatal error's among them, as the error unwinds.
    ~HostDisplay() {
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

void play_intro_file(
    const Options& options, const fs::path& path, bool snapshot, HostDisplay* host
) {
    if (!fs::exists(path)) {
        std::cerr << "intro missing: " << path_to_utf8(path) << '\n';
        return;
    }
    auto opened = oa::media::IntroPlayer::open(path);
    if (!opened) {
        std::cerr << "intro skip " << path_to_utf8(path.filename()) << ": " << opened.error << '\n';
        return;
    }
    oa::media::PlaybackOptions playback;
    playback.headless_check = options.headless_check;
    playback.frame_limit = options.frame_limit.value_or(0);
    playback.play_audio = !options.headless_check && !options.mute;
    if (host != nullptr) {
        playback.window = host->window;
        playback.renderer = host->renderer_host.renderer();
        // Alt+Enter switches full screen during the movies as it does in the
        // game.
        playback.hooks.context = host;
        // The movie player does not hand its events on, so whether Alt+Enter
        // took one does not matter. A lost device is noted for after the
        // movie.
        playback.hooks.window_event = [](void* context, const SDL_Event& event) {
            auto& display = *static_cast<HostDisplay*>(context);
            if (display.renderer_host.take_event(event))
                return;
            std::ignore = take_full_screen_event(display.window, display.full_screen, event);
        };
    }
    if (snapshot)
        playback.snapshot_path = options.snapshot;
    auto result = opened.player->play(playback);
    // A device lost while the movie played is made again before what follows.
    if (host != nullptr)
        host->renderer_host.service();
    if (!result.ok()) {
        std::cerr << "intro " << path_to_utf8(path.filename()) << ": " << result.error << '\n';
        return;
    }
    std::cout << "intro " << path_to_utf8(path.filename()) << ": decoded " << result.decoded_frames
              << " frame(s)" << (result.skipped ? ", skipped\n" : "\n");
}

/// Plays the movies the game starts with, the logo then the intro, unless a
/// switch skips them.
///
/// @param options the parsed command line, with the mod profile played
/// @param host the window the movies show in; null for a headless run
void play_intro(const Options& options, HostDisplay* host) {
    // The game's -c, -n and -y switches skip the movies too. This startup
    // path runs before the preferences load, so the PlayMovie preference
    // does not bring them back; in 3.1c the movies still play under those
    // switches while PlayMovie is set.
    if (options.skip_intro || options.launch.skip_intro != 0)
        return;
    // Frontend state 0 plays the logo movie (Data/1.zrb, the publisher's
    // logo, in 3.1c), then state 1 the intro (Data/2.zrb), then state 2
    // loads MAINMENU.GUI. The mod profile names each movie's file
    // (media.movies), and an empty name plays none there. A mod folder's
    // movie replaces the game folder's.
    const oa::AssetStore folders(options.game_folders);
    const auto play = [&](std::string_view name, bool snapshot) {
        if (name.empty())
            return;
        const auto found = folders.loose_file(std::string("Data/") + std::string(name));
        play_intro_file(
            options, found ? *found : options.game_dir / "Data" / std::string(name), snapshot, host
        );
    };
    const oa::data::mod_profile::MediaMovies base_movies{};
    const auto& movies = options.mod_profile ? options.mod_profile->media.movies : base_movies;
    play(movies.logo, false);
    play(movies.intro, true);
}

/// Writes each game file and listing a run looks up to a file, one a line,
/// from whichever thread looks it up (--trace-lookups).
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
        return {this, [](void* context, std::string_view name) {
                    auto& self = *static_cast<LookupLog*>(context);
                    const oa::base::threads::LockGuard guard(self.lock_);
                    self.out_ << name << '\n';
                }};
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
        request.players_own_profile = !options.preferences_file.has_value();
        if (options.check_game_files)
            request.check = game_files_check_hooks(options);
        if (run_game_files_screen(request) == GameFilesEnd::quit)
            return false;
        // The continue banner shows once.
        start.recovery = {};
        game_directory = find_game_directory(options, &needed);
    }
    return true;
}

/// The status run_once returns when SWITCH on the settings' Switch Mod
/// question ended the run: main() starts the game afresh for the mod stored.
constexpr int kSoftRestartStatus = -1;

/// The switches --check-mod-switch makes.
constexpr uint32_t kModSwitches = 10;

/// The mods --check-mod-switch takes turns with: No Mod and two test profiles.
constexpr std::size_t kModSwitchTurns = 3;

/// The growth of the host heap's bytes in use --check-mod-switch allows
/// between two runs that play the same mod, in bytes. Runs that play the
/// same mod hold the same blocks, to within the length of a few strings, so
/// a leak of 11 KiB or more a switch exceeds it over the six switches or
/// more between the runs compared.
constexpr uint64_t kModSwitchHeapNoise = uint64_t{64} * 1024;

/// What the host heap holds as each run of --check-mod-switch starts on the
/// main menu, and the verdict on them.
class ModSwitchMemory {
  public:

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

    /// Compares each run of the last round of switches with the first run
    /// after the first start that played the same mod: none may hold more
    /// than kModSwitchHeapNoise more on the host heap, as a run would that
    /// kept what an earlier run allocated. The first start is left out: it
    /// holds blocks of its own, which the first switch frees. The
    /// working set is not compared, since it counts the free memory the
    /// allocator keeps, which grows over the switches by an amount that
    /// changes from one start of the check to the next.
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
            const uint64_t first = samples_[1 + (run - 1) % kModSwitchTurns];
            worst = std::max(worst, samples_[run] > first ? samples_[run] - first : 0);
        }
        std::cout << "mod switch check: " << kModSwitches
                  << " switches; the last round holds at most " << worst / 1024
                  << " KiB more on the heap than the first round after the first start\n";
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
/// @return the run's exit status, or kSoftRestartStatus when SWITCH ended it
int run_once(
    Options options,
    const Extension& extension,
    HostDisplay& display,
    GameFilesStart& game_files,
    LookupLog* lookup_log,
    ModSwitchMemory& switch_memory
) {
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
    GameFilesNeeded needed;
    auto game_directory = find_game_directory(
        options, (game_files.offered || folder_chooser_offered(options)) ? &needed : nullptr
    );
    // Without a usable folder where the in-engine chooser is asked for, it picks one.
    if (!game_directory && needed.needed && needed.chooser) {
        ChooserDisplay chooser{&display, &options};
        const FolderChooserDisplayHooks hooks = display_hooks(chooser);
        if (!run_folder_chooser_until_resolved(options, hooks, needed, game_directory))
            return 0;
    }
    // Without a usable folder, the Game files screen brings one in.
    if (!run_game_files_until_resolved(options, display, game_files, needed, game_directory))
        return options.check_game_files ? finish_game_files_check(options, 0) : 0;
    if (!game_directory)
        return options.check_game_files ? finish_game_files_check(options, 1) : 1;
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
    options.game_folders = game_directory->folders;
    if (options.game_folders.empty())
        options.game_folders = {options.game_dir};
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
    } else if (options.game_folders.size() > 1) {
        std::cout << "open-annihilation: the mod folder "
                  << path_to_utf8(options.game_folders.front())
                  << " holds no oamod.yaml; its files are layered over the game folder, "
                     "which plays by 3.1c's own rules\n";
    }
    // Every later read of game data uses the profile's layout.
    oa::data::defs::use_data_layout(data_layout_of(options.mod_profile.get()));
    if (!options.headless_check && !display.initialized)
        display.initialize(options);
    // The Game files check's management route runs its pass over the
    // installed folder before the game starts.
    if (options.check_game_files && options.game_files_route == GameFilesRoute::manage &&
        !run_game_files_manage_check(options, display.window, display.renderer_host.renderer()))
        return finish_game_files_check(options, 1);
    if (options.restarts == 0)
        play_intro(options, options.headless_check ? nullptr : &display);
    // The movies present on the game's renderer.
    oa::base::float_precision::restore_program_float_control();
    oa::AssetStore assets(options.game_folders);
    if (lookup_log != nullptr)
        assets.observe_lookups(lookup_log->observer());
    auto archives = options.archives;
    if (archives.empty())
        archives = std::move(game_directory->archives);
    if (archives.empty())
        throw std::runtime_error("no game archives were selected");
    for (const auto& candidate : archives) {
        const auto archive = candidate.is_absolute() ? candidate : options.game_dir / candidate;
        assets.mount(archive);
    }
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
    runtime->take_video_capture(std::move(capture));
    runtime->take_full_screen_switch(display.full_screen);
    if (check_mod_switch)
        switch_memory.sample(restarts);
    const int status = runtime->run();
    if (!runtime->soft_restart_requested()) {
        if (checked_options)
            return finish_game_files_check(*checked_options, status);
        return check_mod_switch && status == 0 ? switch_memory.verdict() : status;
    }
    // The next runtime takes over the window as Alt+Enter left it, and the
    // renderer without this runtime's textures; this runtime and its
    // archives go before the next are made.
    display.full_screen = runtime->full_screen_switch();
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
    if (!oa::platform::log_files::begin(folder))
        std::cerr << "open-annihilation: cannot open a log in " << path_to_utf8(folder)
                  << "; the output stays here\n";
}

// Sends the game's standard output and standard error to the logs folder in
// the per-user folder. Without a per-user folder they stay where they were.
void start_log() {
    try {
        start_log_in(oa::platform::preferences::data_directory() / "logs");
    } catch (const std::exception& error) {
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

/// Settles what a stopped install of a mod package left in the player's
/// Mods folder, before each run resolves the mod folder: a stop mid-change
/// may have moved the folder of the mod the settings play. The discard
/// folders it found are kept for the runtime to delete.
///
/// @param options the parsed command line
void recover_mod_installs(const Options& options) {
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
        const auto recovery =
            mod_install::recover_changes(folder / std::string(user_mods_folder_name));
        mod_install::keep_discards(recovery.discards);
    } catch (const std::exception& error) {
        std::cerr << "open-annihilation: mod install: nothing settled: " << error.what() << '\n';
    }
}

/// Makes this copy of the game the opener of .oamod files for the player,
/// where the system takes it at run time; only a start someone plays does:
/// no check, test or unattended run, and none that names its own
/// preferences, player's or data folder, or draws no window.
///
/// @param options the parsed command line
void register_mod_files(const Options& options) {
    if (options.headless_check || options.unattended || options.preferences_file ||
        options.user_folder || options.data_dir)
        return;
    if (const auto driver = oa::platform::environment_value("SDL_VIDEO_DRIVER");
        driver && (*driver == "dummy" || *driver == "offscreen"))
        return;
    namespace file_types = oa::platform::file_types;
    const auto executable = file_types::running_executable();
    if (!executable)
        return;
    const auto done = file_types::register_mod_file_type(*executable, window_icon_png());
    for (const auto& line : done.lines)
        std::cerr << "open-annihilation: file types: " << line << '\n';
    if (!done.error.empty())
        std::cerr << "open-annihilation: file types: " << done.error << '\n';
}

/// Hands the mod packages a start carries to the copy of the game already
/// running, when one holds the instance lock, or takes the lock for this
/// copy's life and its hand-off folder. Only a start someone plays does.
///
/// @param options the parsed command line
/// @param[out] lock the instance lock, when this copy takes it
/// @return true when the packages were handed over and this start ends
bool hand_over_or_lock(const Options& options, std::unique_ptr<mod_install::FileLock>& lock) {
    if (options.headless_check || options.unattended || options.preferences_file)
        return false;
    const auto data = data_folder_of(options);
    if (!data)
        return false;
    const fs::path handoff = *data / std::string(mod_install::handoff_folder_name);
    lock = mod_install::take_instance_lock(*data / std::string(mod_install::instance_lock_name));
    if (lock) {
        mod_install::set_handoff_folder(handoff);
        return false;
    }
    // Another copy runs: a start that carries packages hands them to it.
    if (options.install_mods.empty())
        return false;
    const auto written = mod_install::hand_files_over(handoff, options.install_mods);
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
    std::cerr << "open-annihilation: the mod packages went to the copy already running\n";
    return true;
}

// A fatal error goes to the log, and to the terminal the game was started
// from; a game started from the desktop has no terminal, so it shows in an
// error box instead.
void report_fatal(const std::string& message) {
    const auto line = "open-annihilation: " + message + "\n";
    if (!oa::platform::log_files::current_file().empty())
        std::fputs(line.c_str(), stderr);
    // With neither a terminal nor a box, the log holds the message.
    if (!oa::platform::log_files::write_to_terminal(line))
        std::ignore = SDL_ShowSimpleMessageBox(
            SDL_MESSAGEBOX_ERROR, "Open Annihilation", message.c_str(), nullptr
        );
}

} // namespace
} // namespace oa::app

using namespace oa::app;

int main(int argc, char** argv) {
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
    try {
        // Every registered extension fills its table before the command
        // line is parsed; the runtime gets the table that combines them.
        const ExtensionList extensions(registered_extensions());
        const Extension& extension = extensions.combined();
        const Options parsed = parse_options(argc, argv, extension);
        // The Game files screen's check installs its scripted platform
        // before anything reads the hooks.
        if (parsed.check_game_files)
            install_game_files_check(parsed);
        // --print-profile prints the resolved mod profile and stops.
        if (parsed.print_profile)
            return print_mod_profile(
                parsed.mod_file,
                parsed.mod_dir.empty() ? parsed.game_dir : parsed.mod_dir,
                parsed.accept_unimplemented_hacks,
                std::cout,
                std::cerr
            );
        // A game started for play, from a terminal or the desktop, logs to the
        // logs folder. Checks, benchmarks and other scripted runs keep their
        // output where it goes, and so does a run whose output another
        // program captures, such as a test or a script. --log-dir names the
        // folder for any run.
        if (parsed.log_dir)
            start_log_in(*parsed.log_dir);
        else if (
            !parsed.headless_check && !parsed.unattended &&
            !oa::platform::log_files::output_captured()
        )
            start_log();
        // The mod packages the start carries are installed once the main
        // menu shows, unless another copy already runs and takes them.
        std::unique_ptr<mod_install::FileLock> instance_lock;
        if (hand_over_or_lock(parsed, instance_lock))
            return 0;
        for (const auto& file : parsed.install_mods)
            mod_install::post_mod_file(file);
        register_mod_files(parsed);
        // Where the platform brings game files in, what a stopped import or
        // a change waiting for this start left is taken up before the
        // folder is looked for.
        GameFilesStart game_files = start_game_files(parsed);
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
        for (uint32_t restarts = 0;; ++restarts) {
            // What a stop, or the change between runs, left in the player's
            // Mods folder is settled before the mod folder is looked for.
            recover_mod_installs(parsed);
            auto options = parsed;
            options.restarts = restarts;
            options.native_density_windows = kNativeDensityWindows;
            // A soft restart returns to the main menu without the movies.
            if (restarts > 0)
                options.skip_intro = true;
            const int status = run_once(
                std::move(options), extension, display, game_files, lookup_log.get(), switch_memory
            );
            if (status != kSoftRestartStatus)
                return status;
            // A change to the mod played is made now, with the runtime and
            // its archives gone, before the next run reads the folder.
            mod_install::ChangeOptions change{};
            change.hooks.wait = wait_pumping;
            mod_install::finish_pending_change(change);
        }
    } catch (const fs::filesystem_error& error) {
        // A path longer than the system opens is reported with its length,
        // the limit and what to do.
        const auto reason = error.code() == std::errc::filename_too_long
                                ? path_length_problem(error.path1())
                                : std::string();
        report_fatal(
            reason.empty() ? std::string(error.what()) : path_to_utf8(error.path1()) + ": " + reason
        );
        return 1;
    } catch (const std::exception& error) {
        report_fatal(error.what());
        return 1;
    }
}

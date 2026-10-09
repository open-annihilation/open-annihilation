// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The game directory host over SDL's native folder dialog and message boxes,
// with the platform's default folder and advice from its hooks, the folders
// found on this machine and the in-engine chooser's place.
#include "folder_chooser_screen.hpp"
#include "mod_install_watch.hpp"
#include "oa/app/app.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/game_files_hooks.hpp"
#include "oa/app/platform_hooks.hpp"
#include "oa/platform/game_installs.hpp"
#include "oa/platform/machine.hpp"
#include <SDL3/SDL.h>
#include <array>
#include <atomic>
#include <iostream>
#include <string>
#include <tuple>

#ifndef OA_NATIVE_FOLDER_DIALOG
#error "OA_NATIVE_FOLDER_DIALOG (0 or 1) says whether the build offers the system's folder dialog"
#endif

namespace oa::app {
namespace {

// The build asks for the game folder with the system's folder dialog (the
// OA_NATIVE_FOLDER_DIALOG build option; on for the desktop).
constexpr bool kNativeFolderDialog = OA_NATIVE_FOLDER_DIALOG != 0;
constexpr const char* kMessageTitle = "Open Annihilation";
#if OA_NATIVE_FOLDER_DIALOG
constexpr const char* kDialogTitle = "Choose your Total Annihilation folder";
constexpr Uint32 kDialogPollMs = 50;
#endif

// Video runs only while the dialogs are up, and the hints set for them are
// dropped with it; HostDisplay starts its own.
struct NativeDialogs {
    bool video_started = false;
    bool video_failed = false;
    std::string video_error;

    ~NativeDialogs() {
        if (video_started)
            SDL_QuitSubSystem(SDL_INIT_VIDEO);
        // SDL refuses only a hint it does not hold, which has nothing to
        // reset.
        if (video_started || video_failed) {
            std::ignore = SDL_ResetHint(SDL_HINT_MAC_BACKGROUND_APP);
            std::ignore = SDL_ResetHint(SDL_HINT_NO_SIGNAL_HANDLERS);
        }
    }
};

#if OA_NATIVE_FOLDER_DIALOG
// SDL answers inside the call on macOS, on a worker thread on Windows and for
// zenity, and from event pumping for the desktop portal: the fields are
// written before `done` is released and read after it is acquired.
struct DialogWait {
    std::atomic<bool> done{};
    bool failed = false;
    std::string path;
    std::string error;
};

void SDLCALL folder_chosen(void* userdata, const char* const* filelist, int) {
    auto* wait = static_cast<DialogWait*>(userdata);
    if (filelist == nullptr) {
        wait->failed = true;
        wait->error = SDL_GetError();
    } else if (filelist[0] != nullptr) {
        // zenity reports a cancel as one empty path.
        wait->path = filelist[0];
    }
    wait->done.store(true, std::memory_order_release);
}
#endif

bool start_video(NativeDialogs& dialogs) {
    if (dialogs.video_started || dialogs.video_failed)
        return dialogs.video_started;
    // Both hints are comforts: refused, the dialogs still open with SDL's
    // own behaviour.
    // From macOS 14 SDL leaves a terminal-launched process in the background,
    // and the panel with it.
    std::ignore = SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "0");
    // Nothing reads SDL's quit event while a dialog is open, so Ctrl+C keeps
    // ending the process.
    std::ignore = SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "1");
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        dialogs.video_failed = true;
        dialogs.video_error = SDL_GetError();
        return false;
    }
    dialogs.video_started = true;
    // A .oamod file the game was opened with arrives in that first event
    // loop, and the dialog's stop of SDL's video would drop it.
    watch_opened_files();
    // macOS finishes launching, and brings the app forward, in the first
    // event loop; the panel must not be that loop.
    SDL_PumpEvents();
    return true;
}

#if OA_NATIVE_FOLDER_DIALOG
FolderPick pick_folder(void* context, const fs::path& start, fs::path* chosen, std::string* error) {
    auto& dialogs = *static_cast<NativeDialogs*>(context);
    if (!start_video(dialogs)) {
        *error = dialogs.video_error;
        return FolderPick::unavailable;
    }
    const char* driver = SDL_GetCurrentVideoDriver();
    if (driver != nullptr && unattended_environment({}, driver)) {
        *error = std::string("the ") + driver + " video driver has no display";
        return FolderPick::unavailable;
    }
    const SDL_PropertiesID properties = SDL_CreateProperties();
    if (properties == 0) {
        *error = SDL_GetError();
        return FolderPick::unavailable;
    }
    // A property SDL cannot store leaves the dialog with SDL's own title,
    // starting folder or choice of one folder; it still opens.
    std::ignore =
        SDL_SetStringProperty(properties, SDL_PROP_FILE_DIALOG_TITLE_STRING, kDialogTitle);
    const auto location = dialog_location(start);
    if (!location.empty()) {
        std::ignore = SDL_SetStringProperty(
            properties, SDL_PROP_FILE_DIALOG_LOCATION_STRING, location.c_str()
        );
    }
    std::ignore = SDL_SetBooleanProperty(properties, SDL_PROP_FILE_DIALOG_MANY_BOOLEAN, false);
    DialogWait wait;
    SDL_ShowFileDialogWithProperties(SDL_FILEDIALOG_OPENFOLDER, folder_chosen, &wait, properties);
    SDL_DestroyProperties(properties);
    // A worker thread may still hold `wait`, so the loop cannot end early.
    // Pumping delivers the desktop portal's answer.
    while (!wait.done.load(std::memory_order_acquire)) {
        SDL_PumpEvents();
        SDL_Delay(kDialogPollMs);
    }
    if (wait.failed) {
        *error = wait.error;
#if !defined(SDL_PLATFORM_WINDOWS) && !defined(SDL_PLATFORM_APPLE)
        *error += "\nThe folder dialog needs xdg-desktop-portal with a GTK or KDE backend, or "
                  "zenity.";
#endif
        return FolderPick::unavailable;
    }
    if (wait.path.empty())
        return FolderPick::cancelled;
    *chosen = path_from_utf8(wait.path);
    return FolderPick::chosen;
}
#endif

void tell_user(void* context, Notice kind, std::string_view text) {
    const std::string message(text);
    std::cerr << message << '\n';
    // Without video the box below fails, and that is reported.
    std::ignore = start_video(*static_cast<NativeDialogs*>(context));
    const SDL_MessageBoxFlags flags =
        kind == Notice::information ? SDL_MESSAGEBOX_INFORMATION : SDL_MESSAGEBOX_WARNING;
    if (!SDL_ShowSimpleMessageBox(flags, kMessageTitle, message.c_str(), nullptr))
        std::cerr << "open-annihilation: message box: " << SDL_GetError() << '\n';
}

/// Shows a notice with one button and waits for it to be pressed, before
/// the game's window exists or after; the text goes to the error output too.
///
/// @param context the dialogs
/// @param kind information or warning
/// @param text the notice's text
/// @param button the button's label
/// @return false when the notice could not be shown
bool ask_user(void* context, Notice kind, std::string_view text, std::string_view button) {
    const std::string message(text);
    const std::string label(button);
    std::cerr << message << '\n';
    // Without video the box below fails, and that is reported.
    std::ignore = start_video(*static_cast<NativeDialogs*>(context));
    // Return and Escape press the one button too.
    const std::array<SDL_MessageBoxButtonData, 1> buttons{{
        {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT | SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT,
         0,
         label.c_str()},
    }};
    SDL_MessageBoxData data{};
    data.flags = kind == Notice::information ? SDL_MESSAGEBOX_INFORMATION : SDL_MESSAGEBOX_WARNING;
    data.title = kMessageTitle;
    data.message = message.c_str();
    data.numbuttons = static_cast<int>(buttons.size());
    data.buttons = buttons.data();
    int pressed = -1;
    if (!SDL_ShowMessageBox(&data, &pressed)) {
        std::cerr << "open-annihilation: message box: " << SDL_GetError() << '\n';
        return false;
    }
    return true;
}

// The dialogs, and where the demo's archive is unpacked.
struct NativeHost {
    NativeDialogs dialogs;
    // Empty when none is known; `data_folder_problem` then says why.
    fs::path data_folder;
    std::string data_folder_problem;
    // The mod folder and profile each candidate folder is inspected with.
    ModChoice mod;
    /// The preferences the stored folder and the mod come from; the mod choice points here.
    oa::platform::preferences::Values values{};
};

// Without the folder dialog in the build, the host offers none
// (find_game_directory), and nothing that reaches this asks SDL for one.
FolderPick
pick_native_folder(void* context, const fs::path& start, fs::path* chosen, std::string* error) {
#if OA_NATIVE_FOLDER_DIALOG
    return pick_folder(&static_cast<NativeHost*>(context)->dialogs, start, chosen, error);
#else
    std::ignore = context;
    std::ignore = start;
    std::ignore = chosen;
    *error = "this build offers no folder dialog";
    return FolderPick::unavailable;
#endif
}

void tell_native_user(void* context, Notice kind, std::string_view text) {
    tell_user(&static_cast<NativeHost*>(context)->dialogs, kind, text);
}

/// Shows the missing-folder notice with its look-again button (ask_user).
///
/// @param context the native host
/// @param kind information or warning
/// @param text the notice's text
/// @param button the button's label
/// @return false when the notice could not be shown
bool ask_native_user(void* context, Notice kind, std::string_view text, std::string_view button) {
    return ask_user(&static_cast<NativeHost*>(context)->dialogs, kind, text, button);
}

GameInstall inspect(void* context, const fs::path& folder) {
    const auto& host = *static_cast<NativeHost*>(context);
    auto install = inspect_game_install(folder, host.data_folder, demo_1997, host.mod);
    if (install.demo.outcome == DemoOutcome::unpack_failed && host.data_folder.empty())
        install.demo.problem += " (" + host.data_folder_problem + ")";
    return install;
}

[[nodiscard]] std::string_view text_or_empty(const char* text) {
    return text == nullptr ? std::string_view{} : std::string_view(text);
}

/// Looks for the platform's default game folder (PlatformHooks's
/// default_game_folder); the desktop has none.
///
/// @param[out] folder the folder, when there is one
/// @return true when the platform gave one
bool platform_folder(fs::path* folder) {
    const PlatformHooks& hooks = platform_hooks();
    if (hooks.default_game_folder == nullptr)
        return false;
    std::string found;
    if (!hooks.default_game_folder(hooks.context, &found) || found.empty())
        return false;
    *folder = path_from_utf8(found);
    return true;
}

/// Looks for the platform's default game folder again, for the
/// missing-folder notice's look-again button.
///
/// @param[out] folder the folder, when there is one
/// @return true when the platform gave one
bool find_native_platform_default(void*, fs::path* folder) {
    return platform_folder(folder);
}

// Fills the request with the platform's default game folder, the advice it
// gives without one and the label of the notice's look-again button
// (platform_hooks()); the desktop has none of them.
void take_platform_folder(GameDirectoryRequest& request) {
    const PlatformHooks& hooks = platform_hooks();
    fs::path folder;
    if (platform_folder(&folder))
        request.platform_default = std::move(folder);
    if (hooks.missing_game_folder_advice != nullptr)
        request.platform_advice =
            std::string(text_or_empty(hooks.missing_game_folder_advice(hooks.context)));
    if (hooks.game_folder_check_again != nullptr)
        request.check_again_label =
            std::string(text_or_empty(hooks.game_folder_check_again(hooks.context)));
}

/// Tells whether nobody watches a run: --unattended and the scripted runs, CI, or a dummy or
/// offscreen video driver.
///
/// @param options parsed command line
/// @return true when no dialog can be answered
[[nodiscard]] bool unattended_run(const Options& options) {
    return options.unattended ||
           unattended_environment(
               text_or_empty(SDL_getenv("CI")), text_or_empty(SDL_GetHint(SDL_HINT_VIDEO_DRIVER))
           );
}

/// Prepares the inspection of candidate folders: the preferences, the mod folder and profile,
/// and the data folder the demo is unpacked to.
///
/// An unattended run reads stored choices only from a preferences file it was given, never
/// from the player's own. A mod folder chosen earlier that is gone is dropped, and the game
/// folder plays as it is; one named with --mod-dir must exist.
///
/// @param options parsed command line
/// @param unattended nobody watches the run
/// @param tell_gone_mod say that a mod folder chosen earlier is gone (once, at the start)
/// @param[out] native the host to prepare
void prepare_native_host(
    const Options& options, bool unattended, bool tell_gone_mod, NativeHost& native
) {
    if (!unattended || options.preferences_file)
        native.values = oa::platform::preferences::load(preference_file(options.preferences_file));
    native.mod.folder = chosen_mod_directory(options.mod_dir, options.base_game, native.values);
    std::error_code missing;
    if (options.mod_dir.empty() && !native.mod.folder.empty() &&
        !fs::is_directory(native.mod.folder, missing)) {
        const auto text = "The mod folder chosen earlier can no longer be found:\n\n" +
                          path_to_utf8(native.mod.folder) +
                          "\n\nThe game starts without it; choose a mod again in the "
                          "Open Annihilation settings.";
        if (tell_gone_mod && unattended)
            std::cerr << "open-annihilation: " << text << '\n';
        else if (tell_gone_mod)
            tell_user(&native.dialogs, Notice::information, text);
        native.mod.folder.clear();
    }
    native.mod.profile_file = options.mod_file;
    native.mod.accept_unimplemented_hacks = options.accept_unimplemented_hacks;
    native.mod.preferences = &native.values;
    if (options.data_dir) {
        native.data_folder = *options.data_dir;
    } else {
        try {
            native.data_folder = oa::platform::preferences::data_directory();
        } catch (const std::exception& error) {
            native.data_folder_problem = error.what();
        }
    }
}

} // namespace

std::optional<GameDirectory> find_game_directory(const Options& options, GameFilesNeeded* needed) {
    GameDirectoryRequest request;
    request.argument = options.game_dir;
    request.choose = options.choose_game_dir;
    request.archives_named = !options.archives.empty();
    request.unattended = unattended_run(options);
    NativeHost native;
    prepare_native_host(options, request.unattended, true, native);
    if (request.argument.empty())
        request.stored = stored_game_directory(native.values);
    take_platform_folder(request);
    // The Game files screen takes the place of the missing-folder notice
    // where the platform brings game files in, unless the player asked for
    // the notice; a scripted run never opens it, but for its own check.
    request.import_offered = needed != nullptr && game_files_import_offered(game_files_hooks()) &&
                             !options.no_game_files_screen &&
                             (!request.unattended || options.check_game_files);
    // A run someone watches that names no folder looks where Steam, Heroic,
    // Lutris and Bottles put the game, and may show the in-engine chooser,
    // before the system's dialog in Steam's Game Mode. Scripted runs resolve
    // as they always have.
    if (!request.unattended && request.argument.empty()) {
        request.found = oa::platform::game_installs::find_candidates(
            oa::platform::game_installs::default_search_roots()
        );
        request.chooser_offered = needed != nullptr && folder_chooser_offered(options);
        request.chooser_first = oa::platform::running_in_steam_game_mode();
    }
    GameDirectoryHost host{
        &native, kNativeFolderDialog ? pick_native_folder : nullptr, tell_native_user, inspect
    };
    host.ask = ask_native_user;
    host.find_platform_default = find_native_platform_default;
    const auto resolved = resolve_game_directory(request, host, needed);
    return resolved;
}

FolderPick
pick_game_folder_with_dialog(const fs::path& start, fs::path* chosen, std::string* error) {
#if OA_NATIVE_FOLDER_DIALOG
    NativeDialogs dialogs;
    // Windows reports a choice with no folder on disk behind it (This PC, a
    // library) as a failure, so a failure opens the dialog once more.
    FolderPick pick = pick_folder(&dialogs, start, chosen, error);
    if (pick == FolderPick::unavailable) {
        error->clear();
        pick = pick_folder(&dialogs, start, chosen, error);
    }
    return pick;
#else
    std::ignore = start;
    std::ignore = chosen;
    *error = "this build offers no folder dialog";
    return FolderPick::unavailable;
#endif
}

std::optional<GameDirectory>
take_chosen_folder(const Options& options, const fs::path& folder, std::string* problem) {
    NativeHost native;
    prepare_native_host(options, unattended_run(options), false, native);
    const GameDirectoryHost host{&native, nullptr, tell_native_user, inspect};
    return take_picked_folder(host, folder, problem);
}

} // namespace oa::app

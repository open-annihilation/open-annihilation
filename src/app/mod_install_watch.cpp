// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The watch on SDL's events that copies each file the system opens in the
// game into the inbox, and reads each launch file into network play's
// waiting launches.

#include "mod_install_watch.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/app/netgame/opened_launch.hpp"
#include "oa/app/package_install.hpp"
#include "oa/app/package_install/inbox.hpp"
#include "oa/app/platform_hooks.hpp"

#include <SDL3/SDL.h>

#include <array>
#include <exception>
#include <iostream>
#include <optional>
#include <string>
#include <utility>

namespace oa::app {

namespace {

/// The queued drop events taken at once when the watch starts.
constexpr int kQueuedDrops = 16;

/// Reads a dropped file into network play's waiting launches when it is a
/// launch file: a .oalaunch file, or one whose text begins a launch, as
/// macOS's `open -f` hands one over. It is read at once, through the
/// platform's take_opened_file where it has one, so that a temporary file
/// removed later is still read.
///
/// @param file the file
/// @return true when it was a launch file
bool take_launch(const fs::path& file) {
    const PlatformHooks& hooks = platform_hooks();
    fs::path readable = file;
    std::string copy;
    std::string why;
    const bool brought =
        hooks.take_opened_file != nullptr &&
        hooks.take_opened_file(hooks.context, path_to_utf8(file).c_str(), &copy, &why);
    if (brought)
        readable = path_from_utf8(copy);
    std::optional<netgame::OpenedLaunch> opened = netgame::read_opened_launch(readable);
    if (brought && hooks.release_opened_file != nullptr && readable != file)
        hooks.release_opened_file(hooks.context, copy.c_str());
    if (!opened)
        return false;
    // A refusal names the file the system opened, not the platform's copy.
    opened->source = path_to_utf8(file);
    std::cerr << "open-annihilation: a launch file was opened in the game: " << opened->source
              << '\n';
    netgame::post_opened_launch(std::move(*opened));
    return true;
}

/// Copies a dropped file the game opens into the inbox, a replay among them,
/// and reads a launch file into network play's waiting launches; any other
/// file is logged and left.
///
/// @param event the event
void take_drop(const SDL_Event& event) {
    if (event.type != SDL_EVENT_DROP_FILE || event.drop.data == nullptr)
        return;
    try {
        const fs::path file = path_from_utf8(event.drop.data);
        if (package_install::opens_file(file) || package_install::opens_replay(file)) {
            package_install::post_opened_file(file);
            return;
        }
        if (take_launch(file))
            return;
        std::cerr
            << "open-annihilation: a file opened in the game is none Open Annihilation opens: "
            << event.drop.data << '\n';
    } catch (const std::exception& failure) {
        std::cerr << "open-annihilation: a file opened in the game is lost: " << failure.what()
                  << '\n';
    }
}

/// SDL's event watch: copies each dropped file as it is queued.
///
/// @param event the event
/// @return true, which SDL ignores for a watch
bool SDLCALL take_dropped_file(void*, SDL_Event* event) {
    if (event != nullptr)
        take_drop(*event);
    return true;
}

} // namespace

void watch_opened_files() noexcept {
    SDL_RemoveEventWatch(take_dropped_file, nullptr);
    if (!SDL_AddEventWatch(take_dropped_file, nullptr))
        std::cerr << "open-annihilation: files opened in the game cannot be watched: "
                  << SDL_GetError() << '\n';
    // The drops already queued, which the watch did not see.
    std::array<SDL_Event, kQueuedDrops> queued{};
    const int count = SDL_PeepEvents(
        queued.data(), kQueuedDrops, SDL_PEEKEVENT, SDL_EVENT_DROP_FILE, SDL_EVENT_DROP_FILE
    );
    for (int index = 0; index < count; ++index)
        take_drop(queued[static_cast<std::size_t>(index)]);
}

} // namespace oa::app

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Which install layout this machine can use. This build knows the
// self-check folder only; each platform's own route is filled in later.
// Nothing here writes a file or starts a program.
#pragma once

#include "oa/app/update/build.hpp"
#include "oa/app/update/plan.hpp"
#include "oa/data/engine_feed/feed.hpp"

#include <filesystem>
#include <optional>
#include <string>

namespace oa::app::update {

/// What the machine, and the options of this run, say about an install.
///
/// A test passes any platform's facts. `this_machine` fills the ones this
/// process can see, and leaves the option, the test feed and the offer's
/// way for the caller to set.
struct RouteFacts {
    std::filesystem::path executable;                    ///< the program to start again
    std::filesystem::path program_folder;                ///< the folder that holds that program
    bool folder_writable{false};                         ///< the program folder can be written
    std::string platform;                                ///< the package platform name
    std::optional<std::filesystem::path> install_folder; ///< `--update-install-folder`, when set
    bool test_feed{false};                               ///< this run fetched a test feed
    oa::data::engine_feed::Way way{
        oa::data::engine_feed::Way::none
    }; ///< how the release is offered
};

/// The route a download may use, or why it may not.
struct Where {
    std::optional<Route> route;    ///< set when this install can be replaced
    std::filesystem::path install; ///< the folder the route replaces
    std::filesystem::path program; ///< the program to start afterwards
    std::string why;               ///< why there is no route; empty when there is one
};

/// The route for this build on these facts.
///
/// A release offered on its page has none. A development build has none
/// unless a test feed is in use. Otherwise the self-check install folder
/// selects `check-folder`. Every platform is still without a route of its
/// own: `why` is "no route for this platform yet".
///
/// @param build the build that is running
/// @param facts what the machine and this run say
/// @return the route, or why there is none
[[nodiscard]] Where find_route(const ThisBuild& build, const RouteFacts& facts);

/// The facts this process can see for itself.
///
/// The executable is this process's own file. The program folder is the one
/// passed in. The install folder, the test feed and the offer's way are left
/// unset, for the caller to fill before `find_route`.
///
/// @param program_folder the folder that holds the program
/// @return the facts
[[nodiscard]] RouteFacts this_machine(const std::filesystem::path& program_folder);

} // namespace oa::app::update

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The install route. Linux uses a versions folder. The self-check folder
// still wins when it is named. Nothing is installed.

#include "oa/app/update/route.hpp"

#include "oa/test/check.hpp"

#include <filesystem>
#include <string>
#include <string_view>
#include <system_error>

namespace {

namespace fs = std::filesystem;
namespace update = oa::app::update;
namespace feed = oa::data::engine_feed;

update::ThisBuild release_build() {
    update::ThisBuild build;
    build.version_text = "0.0.1";
    build.development = false;
    build.package = "linux-x86_64";
    return build;
}

update::RouteFacts facts_for(std::string_view platform) {
    update::RouteFacts facts;
    facts.executable = fs::path("/programs/open-annihilation");
    facts.program_folder = fs::path("/programs");
    facts.folder_writable = true;
    facts.platform = std::string(platform);
    facts.way = feed::Way::in_place;
    return facts;
}

void every_other_platform_has_no_route() {
    const std::string_view platforms[] = {
        "macos-universal",
        "windows-x64",
        "windows-arm64",
        "windows-x86",
        "ios",
        "android",
        "unknown",
    };
    const update::ThisBuild build = release_build();
    for (const std::string_view platform : platforms) {
        const update::Where where = update::find_route(build, facts_for(platform));
        OA_CHECK(!where.route.has_value());
        OA_CHECK(where.why == "no route for this platform yet");
        update::RouteFacts read_only = facts_for(platform);
        read_only.folder_writable = false;
        const update::Where unwritable = update::find_route(build, read_only);
        OA_CHECK(!unwritable.route.has_value());
        OA_CHECK(unwritable.why == "no route for this platform yet");
    }
}

void linux_uses_a_versions_folder() {
    const std::string_view platforms[] = {"linux-x86_64", "linux-arm64", "linux-armhf"};
    const update::ThisBuild build = release_build();
    for (const std::string_view platform : platforms) {
        update::RouteFacts facts = facts_for(platform);
        facts.folder_writable = false;
        const update::Where where = update::find_route(build, facts);
        OA_CHECK(where.route.has_value());
        OA_CHECK(*where.route == update::Route::linux_versions);
        OA_CHECK(where.install == facts.program_folder);
        OA_CHECK(where.program == facts.executable);
        OA_CHECK(where.why.empty());
    }

    update::RouteFacts laid_out = facts_for("linux-x86_64");
    laid_out.program_folder = fs::path("/home/player/oa/versions/0.8.6");
    laid_out.executable = laid_out.program_folder / "open-annihilation";
    laid_out.launcher_present = true;
    const update::Where above = update::find_route(build, laid_out);
    OA_CHECK(above.route.has_value());
    OA_CHECK(*above.route == update::Route::linux_versions);
    OA_CHECK(above.install == fs::path("/home/player/oa"));
    OA_CHECK(above.program == laid_out.executable);

    laid_out.launcher_present = false;
    const update::Where flat = update::find_route(build, laid_out);
    OA_CHECK(flat.install == laid_out.program_folder);
}

void a_system_copy_is_not_replaced() {
    const update::ThisBuild build = release_build();
    const fs::path blocked[] = {
        fs::path("/opt"),
        fs::path("/opt/open-annihilation"),
        fs::path("/usr"),
        fs::path("/usr/local/oa"),
        fs::path("/snap/oa"),
        fs::path("/var/lib/flatpak"),
        fs::path("/var/lib/flatpak/app/oa"),
    };
    for (const fs::path& folder : blocked) {
        update::RouteFacts facts = facts_for("linux-x86_64");
        facts.program_folder = folder;
        const update::Where where = update::find_route(build, facts);
        OA_CHECK(!where.route.has_value());
        OA_CHECK(where.why == update::whole_system_copy);
    }
    const fs::path allowed[] = {
        fs::path("/optima/oa"),
        fs::path("/usr-local/oa"),
        fs::path("/var/lib/flatpak-extra/oa"),
        fs::path("/home/player/oa"),
    };
    for (const fs::path& folder : allowed) {
        update::RouteFacts facts = facts_for("linux-x86_64");
        facts.program_folder = folder;
        const update::Where where = update::find_route(build, facts);
        OA_CHECK(where.route.has_value());
        OA_CHECK(*where.route == update::Route::linux_versions);
    }

    update::RouteFacts owned = facts_for("linux-x86_64");
    owned.program_folder = fs::path("/home/player/oa");
    owned.owned_by_player = false;
    OA_CHECK(update::find_route(build, owned).why == update::whole_system_copy);
    owned.owned_by_player = true;
    owned.appimage = true;
    OA_CHECK(update::find_route(build, owned).why == update::whole_system_copy);
    owned.appimage = false;
    owned.flatpak = true;
    OA_CHECK(update::find_route(build, owned).why == update::whole_system_copy);
}

void check_folder_uses_the_option() {
    update::RouteFacts facts = facts_for("windows-x64");
    facts.install_folder = fs::path("/tmp/oa-v13-install");
    const update::Where where = update::find_route(release_build(), facts);
    OA_CHECK(where.route.has_value());
    OA_CHECK(*where.route == update::Route::check_folder);
    OA_CHECK(where.install == fs::path("/tmp/oa-v13-install"));
    OA_CHECK(where.program == facts.executable);
    OA_CHECK(where.why.empty());
}

void a_development_build_has_no_route_without_a_test_feed() {
    update::ThisBuild build = release_build();
    build.development = true;
    update::RouteFacts facts = facts_for("linux-x86_64");
    facts.install_folder = fs::path("/tmp/oa-v13-install");
    const update::Where refused = update::find_route(build, facts);
    OA_CHECK(!refused.route.has_value());
    OA_CHECK(refused.why == "a development build has no route");

    facts.test_feed = true;
    const update::Where allowed = update::find_route(build, facts);
    OA_CHECK(allowed.route.has_value());
    OA_CHECK(*allowed.route == update::Route::check_folder);

    facts.install_folder.reset();
    const update::Where linux = update::find_route(build, facts);
    OA_CHECK(linux.route.has_value());
    OA_CHECK(*linux.route == update::Route::linux_versions);

    facts.platform = "windows-x64";
    const update::Where no_platform = update::find_route(build, facts);
    OA_CHECK(!no_platform.route.has_value());
    OA_CHECK(no_platform.why == "no route for this platform yet");
}

void a_page_is_never_a_route() {
    update::RouteFacts facts = facts_for("macos-universal");
    facts.way = feed::Way::page_only;
    facts.test_feed = true;
    facts.install_folder = fs::path("/tmp/oa-v13-install");
    const update::Where where = update::find_route(release_build(), facts);
    OA_CHECK(!where.route.has_value());
    OA_CHECK(where.why == "the release is offered on its page");
}

void this_machine_names_the_executable() {
    const fs::path folder = fs::temp_directory_path() / "oa-v13-route";
    std::error_code error;
    fs::create_directories(folder, error);
    const update::RouteFacts facts = update::this_machine(folder);
    OA_CHECK(facts.program_folder == folder);
    OA_CHECK(!facts.executable.empty());
    OA_CHECK(!facts.platform.empty());
    OA_CHECK(!facts.install_folder.has_value());
    OA_CHECK(!facts.test_feed);
    OA_CHECK(facts.way == feed::Way::none);
    OA_CHECK(!facts.launcher_present);
    OA_CHECK(facts.owned_by_player);
    fs::remove_all(folder, error);
}

} // namespace

int main() {
    every_other_platform_has_no_route();
    linux_uses_a_versions_folder();
    a_system_copy_is_not_replaced();
    check_folder_uses_the_option();
    a_development_build_has_no_route_without_a_test_feed();
    a_page_is_never_a_route();
    this_machine_names_the_executable();
    return oa::test::check_exit_status();
}

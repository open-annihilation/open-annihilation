// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The install route. Windows has the installer and the folder. The
// self-check folder still wins when it is set. Nothing is installed.

#include "oa/app/update/route.hpp"

#include "oa/test/check.hpp"

#include <filesystem>
#include <fstream>
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

void every_platform_has_no_route() {
    const std::string_view platforms[] = {
        "macos-universal",
        "linux-x86_64",
        "linux-arm64",
        "linux-armhf",
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

void check_folder_uses_the_option() {
    update::RouteFacts facts = facts_for("windows-x64");
    facts.install_folder = fs::path("/tmp/oa-v13-install");
    facts.uninstall_beside = true;
    facts.install_location = "/programs";
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
    update::RouteFacts windows = facts_for("windows-x64");
    windows.uninstall_beside = true;
    windows.install_location = "/programs";
    const update::Where development = update::find_route(build, windows);
    OA_CHECK(!development.route.has_value());
    OA_CHECK(development.why == "a development build has no route");

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

    update::RouteFacts windows = facts_for("windows-x64");
    windows.way = feed::Way::page_only;
    windows.uninstall_beside = true;
    windows.install_location = "/programs";
    const update::Where page = update::find_route(release_build(), windows);
    OA_CHECK(!page.route.has_value());
    OA_CHECK(page.why == "the release is offered on its page");
}

void windows_routes() {
    OA_CHECK(!update::same_install_folder("", ""));
    OA_CHECK(!update::same_install_folder("", "C:/Games"));
    OA_CHECK(
        update::same_install_folder(
            "C:/Games/Extra/../Open Annihilation/", "C:\\Games\\Open Annihilation"
        )
    );
    OA_CHECK(
        update::same_install_folder("\"C:/Games/Open Annihilation\"", "c:/games/open annihilation")
    );

    const update::ThisBuild build = release_build();
    update::RouteFacts facts = facts_for("windows-x64");
    facts.program_folder = fs::path("C:/Games/Open Annihilation");
    facts.uninstall_beside = true;
    facts.install_location = "C:/Games/Extra/../Open Annihilation/";
    const update::Where installer = update::find_route(build, facts);
    OA_CHECK(installer.route.has_value());
    OA_CHECK(*installer.route == update::Route::windows_installer);
    OA_CHECK(installer.install == facts.program_folder);
    OA_CHECK(installer.why.empty());

    facts.install_location = "C:/Games/Other";
    const update::Where mismatch = update::find_route(build, facts);
    OA_CHECK(mismatch.route.has_value());
    OA_CHECK(*mismatch.route == update::Route::windows_folder);

    facts.install_location = "C:/Games/Open Annihilation";
    facts.uninstall_beside = false;
    const update::Where no_uninstaller = update::find_route(build, facts);
    OA_CHECK(no_uninstaller.route.has_value());
    OA_CHECK(*no_uninstaller.route == update::Route::windows_folder);

    facts.uninstall_beside = true;
    facts.install_location.clear();
    const update::Where no_key = update::find_route(build, facts);
    OA_CHECK(no_key.route.has_value());
    OA_CHECK(*no_key.route == update::Route::windows_folder);

    facts.install_location = "C:/Games/Open Annihilation";
    facts.folder_writable = false;
    const update::Where read_only = update::find_route(build, facts);
    OA_CHECK(read_only.route.has_value());
    OA_CHECK(*read_only.route == update::Route::windows_installer);

    update::RouteFacts win95 = facts_for("windows-95");
    win95.program_folder = fs::path("C:/Games/Open Annihilation");
    win95.folder_writable = false;
    const update::Where old = update::find_route(build, win95);
    OA_CHECK(old.route.has_value());
    OA_CHECK(*old.route == update::Route::windows_folder);
}

void this_machine_names_the_executable() {
    const fs::path folder = fs::temp_directory_path() / "oa-v13-route";
    std::error_code error;
    fs::remove_all(folder, error);
    fs::create_directories(folder, error);
    {
        std::ofstream uninstaller(folder / "Uninstall.exe", std::ios::binary);
        uninstaller << "MZ";
    }
    const update::RouteFacts facts = update::this_machine(folder);
    OA_CHECK(facts.program_folder == folder);
    OA_CHECK(!facts.executable.empty());
    OA_CHECK(!facts.platform.empty());
    OA_CHECK(!facts.install_folder.has_value());
    OA_CHECK(!facts.test_feed);
    OA_CHECK(facts.way == feed::Way::none);
    OA_CHECK(facts.folder_writable);
    OA_CHECK(facts.uninstall_beside);
#if !defined(_WIN32)
    OA_CHECK(facts.install_location.empty());
    fs::permissions(
        folder, fs::perms::owner_read | fs::perms::owner_exec, fs::perm_options::replace, error
    );
    const update::RouteFacts locked = update::this_machine(folder);
    OA_CHECK(!locked.folder_writable);
    fs::permissions(folder, fs::perms::owner_all, fs::perm_options::replace, error);
#endif
    fs::remove_all(folder, error);
}

} // namespace

int main() {
    every_platform_has_no_route();
    check_folder_uses_the_option();
    a_development_build_has_no_route_without_a_test_feed();
    a_page_is_never_a_route();
    windows_routes();
    this_machine_names_the_executable();
    return oa::test::check_exit_status();
}

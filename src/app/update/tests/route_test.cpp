// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The install route. Windows has the installer and the folder. Linux uses
// a versions folder. The self-check folder still wins when it is set.
// Nothing is installed.

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

void every_other_platform_has_no_route() {
    const std::string_view platforms[] = {
        "macos-universal",
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
    const update::Where linux = update::find_route(build, facts);
    OA_CHECK(linux.route.has_value());
    OA_CHECK(*linux.route == update::Route::linux_versions);

    facts.platform = "windows-x64";
    const update::Where on_windows = update::find_route(build, facts);
    OA_CHECK(on_windows.route.has_value());
    OA_CHECK(*on_windows.route == update::Route::windows_folder);
    OA_CHECK(on_windows.why.empty());

    facts.platform = "macos-universal";
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

update::RouteFacts macos_bundle_facts() {
    update::RouteFacts facts = facts_for("macos-universal");
    facts.executable =
        fs::path("/Applications/Open Annihilation.app/Contents/MacOS/open-annihilation");
    facts.bundle = fs::path("/Applications/Open Annihilation.app");
    facts.signing_team = "TEAMID1234";
    facts.parent_writable = true;
    facts.self_check = false;
    return facts;
}

void a_translocated_app_has_no_route() {
    update::RouteFacts facts = macos_bundle_facts();
    facts.translocated = true;
    const update::Where where = update::find_route(release_build(), facts);
    OA_CHECK(!where.route.has_value());
    OA_CHECK(!where.installer);
    OA_CHECK(where.why == "macOS is running this copy from a protected place");
}

void an_ad_hoc_app_has_no_route() {
    update::RouteFacts facts = macos_bundle_facts();
    facts.signing_team.clear();
    facts.test_feed = true;
    const update::Where where = update::find_route(release_build(), facts);
    OA_CHECK(!where.route.has_value());
    OA_CHECK(where.why == "an ad hoc signed build does not update in place");
}

void a_team_is_not_compared_by_the_game() {
    update::ThisBuild build = release_build();
    build.package = "macos-universal";
    update::RouteFacts facts = macos_bundle_facts();
    facts.signing_team = "OTHERTEAM1";
    const update::Where where = update::find_route(build, facts);
    OA_CHECK(where.route.has_value());
    OA_CHECK(*where.route == update::Route::macos_bundle);
    OA_CHECK(!where.installer);
    OA_CHECK(where.install == facts.bundle);
    OA_CHECK(where.program == facts.executable);
    OA_CHECK(where.why.empty());
}

void an_unwritable_folder_uses_the_installer() {
    update::RouteFacts facts = macos_bundle_facts();
    facts.parent_writable = false;
    const update::Where where = update::find_route(release_build(), facts);
    OA_CHECK(where.route.has_value());
    OA_CHECK(*where.route == update::Route::macos_bundle);
    OA_CHECK(where.installer);
    OA_CHECK(where.install == facts.bundle);
    OA_CHECK(where.why.empty());
}

void a_writable_folder_uses_the_zip() {
    const update::Where where = update::find_route(release_build(), macos_bundle_facts());
    OA_CHECK(where.route.has_value());
    OA_CHECK(*where.route == update::Route::macos_bundle);
    OA_CHECK(!where.installer);
    OA_CHECK(where.install == macos_bundle_facts().bundle);
}

void a_self_check_ad_hoc_build_may_update() {
    update::ThisBuild build = release_build();
    build.development = true;
    update::RouteFacts facts = macos_bundle_facts();
    facts.signing_team.clear();
    facts.self_check = true;
    facts.test_feed = true;
    const update::Where where = update::find_route(build, facts);
    OA_CHECK(where.route.has_value());
    OA_CHECK(*where.route == update::Route::macos_bundle);
    OA_CHECK(!where.installer);
    OA_CHECK(where.install == facts.bundle);

    facts.test_feed = false;
    const update::Where no_feed = update::find_route(build, facts);
    OA_CHECK(!no_feed.route.has_value());
    OA_CHECK(no_feed.why == "a development build has no route");
}

void a_check_can_be_told_there_is_no_route() {
    update::ThisBuild build = release_build();
    build.development = true;
    update::RouteFacts macos = macos_bundle_facts();
    macos.self_check = true;
    macos.test_feed = true;
    macos.no_install_route = true;
    const update::Where bundle = update::find_route(build, macos);
    OA_CHECK(!bundle.route.has_value());
    OA_CHECK(!bundle.installer);
    OA_CHECK(bundle.why == "this check has no install route");

    update::RouteFacts linux = facts_for("linux-x86_64");
    linux.test_feed = true;
    linux.no_install_route = true;
    linux.install_folder = fs::path("/tmp/oa-v08-install");
    const update::Where folder = update::find_route(build, linux);
    OA_CHECK(!folder.route.has_value());
    OA_CHECK(folder.why == "this check has no install route");
}

void the_self_check_folder_still_wins_on_macos() {
    update::RouteFacts facts = macos_bundle_facts();
    facts.install_folder = fs::path("/tmp/oa-v08-install");
    const update::Where where = update::find_route(release_build(), facts);
    OA_CHECK(where.route.has_value());
    OA_CHECK(*where.route == update::Route::check_folder);
    OA_CHECK(!where.installer);
    OA_CHECK(where.install == fs::path("/tmp/oa-v08-install"));
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
    OA_CHECK(facts.bundle.empty());
    OA_CHECK(!facts.translocated);
    OA_CHECK(facts.self_check);
    OA_CHECK(facts.folder_writable);
    OA_CHECK(facts.uninstall_beside);
    OA_CHECK(!facts.launcher_present);
    OA_CHECK(facts.owned_by_player);
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
    every_other_platform_has_no_route();
    linux_uses_a_versions_folder();
    a_system_copy_is_not_replaced();
    a_translocated_app_has_no_route();
    an_ad_hoc_app_has_no_route();
    a_team_is_not_compared_by_the_game();
    an_unwritable_folder_uses_the_installer();
    a_writable_folder_uses_the_zip();
    a_self_check_ad_hoc_build_may_update();
    a_check_can_be_told_there_is_no_route();
    the_self_check_folder_still_wins_on_macos();
    check_folder_uses_the_option();
    a_development_build_has_no_route_without_a_test_feed();
    a_page_is_never_a_route();
    windows_routes();
    this_machine_names_the_executable();
    return oa::test::check_exit_status();
}

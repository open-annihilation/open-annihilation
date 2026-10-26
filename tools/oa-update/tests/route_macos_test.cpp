// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The macOS swap, decided from signatures a test fills in, and the
// operations that follow. Nothing here signs a bundle or starts one.

#include "oa/update_helper/routes.hpp"

#include "oa/app/update/plan.hpp"
#include "oa/test/check.hpp"
#include "oa/update_helper/paths.hpp"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

namespace {

namespace fs = std::filesystem;
namespace helper = oa::update_helper;
namespace update = oa::app::update;

struct Scratch {
    fs::path path;

    explicit Scratch(std::string_view name) {
        const auto ticks = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path() /
               (std::string("oa-v08-") + std::string(name) + "-" + std::to_string(ticks));
        fs::create_directories(path);
    }

    ~Scratch() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }

    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
};

helper::BundleSignature team_of(std::string_view team, bool valid, bool notarized) {
    helper::BundleSignature signature;
    signature.team = std::string(team);
    signature.valid = valid;
    signature.notarized = notarized;
    return signature;
}

void the_same_team_may_swap() {
    const helper::MacosDecision decision = helper::macos_may_swap(
        team_of("TEAMID1234", true, true), team_of("TEAMID1234", true, true), false
    );
    OA_CHECK(decision.go);
    OA_CHECK(decision.why.empty());
}

void another_team_is_refused() {
    const helper::MacosDecision decision = helper::macos_may_swap(
        team_of("TEAMID1234", true, true), team_of("OTHERTEAM1", true, true), false
    );
    OA_CHECK(!decision.go);
    OA_CHECK(decision.why == "the new bundle is signed by another team");
}

void an_invalid_signature_is_refused() {
    const helper::MacosDecision decision = helper::macos_may_swap(
        team_of("TEAMID1234", true, true), team_of("TEAMID1234", false, true), false
    );
    OA_CHECK(!decision.go);
    OA_CHECK(decision.why == "the new bundle's signature is not valid");
}

void a_bundle_that_is_not_notarized_is_refused() {
    const helper::MacosDecision decision = helper::macos_may_swap(
        team_of("TEAMID1234", true, true), team_of("TEAMID1234", true, false), false
    );
    OA_CHECK(!decision.go);
    OA_CHECK(decision.why == "the new bundle is not notarized");
}

void an_ad_hoc_release_is_refused() {
    const helper::MacosDecision decision =
        helper::macos_may_swap(team_of("", true, false), team_of("", true, false), false);
    OA_CHECK(!decision.go);
    OA_CHECK(decision.why == "an ad hoc signed build does not update in place");
}

void an_ad_hoc_self_check_may_swap() {
    const helper::MacosDecision decision =
        helper::macos_may_swap(team_of("", true, false), team_of("", true, false), true);
    OA_CHECK(decision.go);
    OA_CHECK(decision.why.empty());
}

void a_mixed_ad_hoc_signature_is_refused() {
    const helper::MacosDecision decision =
        helper::macos_may_swap(team_of("TEAMID1234", true, true), team_of("", true, false), true);
    OA_CHECK(!decision.go);
    OA_CHECK(decision.why == "an ad hoc signed build does not update in place");
}

void an_invalid_ad_hoc_self_check_is_refused() {
    const helper::MacosDecision decision =
        helper::macos_may_swap(team_of("", true, false), team_of("", false, false), true);
    OA_CHECK(!decision.go);
    OA_CHECK(decision.why == "the new bundle's signature is not valid");
}

fs::path make_app(const fs::path& path) {
    fs::create_directories(path / "Contents" / "MacOS");
    std::ofstream game(path / "Contents" / "MacOS" / "game");
    game << "game";
    return path;
}

struct LaidOut {
    fs::path install;
    fs::path staged;
    fs::path application;
    update::Plan plan;
};

LaidOut lay_out(const Scratch& scratch) {
    LaidOut laid;
    laid.install = make_app(scratch.path / "Applications" / "Game.app");
    laid.staged = scratch.path / "stage";
    laid.application = make_app(laid.staged / "Open-Annihilation" / "Open Annihilation.app");
    fs::create_directories(scratch.path / "updates");
    laid.plan.route = update::Route::macos_bundle;
    laid.plan.install = helper::utf8_of(laid.install);
    laid.plan.updates = helper::utf8_of(scratch.path / "updates");
    laid.plan.file = helper::utf8_of(scratch.path / "Open-Annihilation.pkg");
    return laid;
}

void the_same_volume_renames() {
    const Scratch scratch("same");
    const LaidOut laid = lay_out(scratch);
    std::string why = "uncleared";
    const std::vector<helper::Operation> operations =
        helper::macos_bundle_operations(laid.plan, laid.staged, helper::MacosVolume::same, &why);
    const fs::path previous = scratch.path / "updates" / "previous" / "Game.app";
    OA_CHECK(why.empty());
    OA_CHECK(operations.size() == 2);
    OA_CHECK(operations[0].kind == helper::Operation::Kind::rename);
    OA_CHECK(operations[0].from == laid.install);
    OA_CHECK(operations[0].to == previous);
    OA_CHECK(operations[1].kind == helper::Operation::Kind::rename);
    OA_CHECK(operations[1].from == laid.application);
    OA_CHECK(operations[1].to == laid.install);
}

void a_cross_volume_move_copies_both_ways() {
    const Scratch scratch("exdev");
    const LaidOut laid = lay_out(scratch);
    std::string why;
    const std::vector<helper::Operation> operations =
        helper::macos_bundle_operations(laid.plan, laid.staged, helper::MacosVolume::exdev, &why);
    const fs::path previous = scratch.path / "updates" / "previous" / "Game.app";
    OA_CHECK(why.empty());
    OA_CHECK(operations.size() == 3);
    OA_CHECK(operations[0].kind == helper::Operation::Kind::copy_tree);
    OA_CHECK(operations[0].from == laid.install);
    OA_CHECK(operations[0].to == previous);
    OA_CHECK(operations[1].kind == helper::Operation::Kind::remove_tree);
    OA_CHECK(operations[1].path == laid.install);
    OA_CHECK(operations[2].kind == helper::Operation::Kind::copy_tree);
    OA_CHECK(operations[2].from == laid.application);
    OA_CHECK(operations[2].to == laid.install);
}

void an_older_kept_bundle_is_removed_first() {
    const Scratch scratch("kept");
    const LaidOut laid = lay_out(scratch);
    const fs::path previous = scratch.path / "updates" / "previous" / "Game.app";
    fs::create_directories(previous);
    std::string why;
    const std::vector<helper::Operation> operations =
        helper::macos_bundle_operations(laid.plan, laid.staged, helper::MacosVolume::same, &why);
    OA_CHECK(why.empty());
    OA_CHECK(operations.size() == 3);
    OA_CHECK(operations[0].kind == helper::Operation::Kind::remove_tree);
    OA_CHECK(operations[0].path == previous);
}

void the_installer_is_opened() {
    const Scratch scratch("pkg");
    LaidOut laid = lay_out(scratch);
    laid.plan.installer = true;
    std::string why = "uncleared";
    const std::vector<helper::Operation> operations =
        helper::macos_bundle_operations(laid.plan, {}, helper::MacosVolume::same, &why);
    OA_CHECK(why.empty());
    OA_CHECK(operations.size() == 1);
    OA_CHECK(operations[0].kind == helper::Operation::Kind::run_and_wait);
    OA_CHECK(operations[0].program == "/usr/bin/open");
    OA_CHECK(operations[0].arguments.size() == 1);
    OA_CHECK(operations[0].arguments[0] == laid.plan.file);
    OA_CHECK(operations[0].inverse_program.empty());
}

void a_missing_installer_is_refused() {
    update::Plan plan;
    plan.installer = true;
    std::string why;
    const std::vector<helper::Operation> operations =
        helper::macos_bundle_operations(plan, {}, helper::MacosVolume::same, &why);
    OA_CHECK(operations.empty());
    OA_CHECK(why == "the installer is missing");
}

void two_applications_are_refused() {
    const Scratch scratch("two");
    LaidOut laid = lay_out(scratch);
    make_app(laid.staged / "Open-Annihilation" / "Other.app");
    std::string why;
    const std::vector<helper::Operation> operations =
        helper::macos_bundle_operations(laid.plan, laid.staged, helper::MacosVolume::same, &why);
    OA_CHECK(operations.empty());
    OA_CHECK(why == "the staged package is not one application");
}

void a_missing_bundle_is_refused() {
    const Scratch scratch("missing");
    LaidOut laid = lay_out(scratch);
    laid.plan.install = helper::utf8_of(scratch.path / "absent.app");
    std::string why;
    const std::vector<helper::Operation> operations =
        helper::macos_bundle_operations(laid.plan, laid.staged, helper::MacosVolume::same, &why);
    OA_CHECK(operations.empty());
    OA_CHECK(why == "the installed bundle is missing");
}

} // namespace

void test_macos_route() {
    the_same_team_may_swap();
    another_team_is_refused();
    an_invalid_signature_is_refused();
    a_bundle_that_is_not_notarized_is_refused();
    an_ad_hoc_release_is_refused();
    an_ad_hoc_self_check_may_swap();
    a_mixed_ad_hoc_signature_is_refused();
    an_invalid_ad_hoc_self_check_is_refused();
    the_same_volume_renames();
    a_cross_volume_move_copies_both_ways();
    an_older_kept_bundle_is_removed_first();
    the_installer_is_opened();
    a_missing_installer_is_refused();
    two_applications_are_refused();
    a_missing_bundle_is_refused();
}

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The receipt the helper leaves for the game: its names, the outcome a
// failed run leaves, which outcomes hold a release, a round trip through the
// file, and the receipts that are refused. Also the state file's quit
// marker and how a watched start ended. A scratch folder holds the files.

#include "oa/app/update/plan.hpp"
#include "oa/app/update/receipt.hpp"

#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace fs = std::filesystem;
namespace update = oa::app::update;

std::vector<uint8_t> bytes_of(std::string_view text) {
    return {text.begin(), text.end()};
}

update::Receipt sample() {
    update::Receipt receipt;
    receipt.action = update::Action::install;
    receipt.outcome = update::Outcome::failed_restored;
    receipt.from_version = "0.8.7";
    receipt.to_version = "0.8.8";
    receipt.release = 12;
    receipt.started_at = 1793000000;
    receipt.finished_at = 1793000042;
    receipt.steps.push_back({update::Step::wait, update::StepResult::done, {}});
    receipt.steps.push_back({update::Step::verify, update::StepResult::done, {}});
    receipt.steps.push_back(
        {update::Step::swap, update::StepResult::failed, "another program had it open"}
    );
    receipt.steps.push_back({update::Step::undo, update::StepResult::done, {}});
    receipt.steps.push_back({update::Step::start, update::StepResult::done, "0.8.7"});
    return receipt;
}

bool same(const update::Receipt& left, const update::Receipt& right) {
    if (left.receipt != right.receipt || left.action != right.action ||
        left.reason != right.reason || left.outcome != right.outcome ||
        left.from_version != right.from_version || left.to_version != right.to_version ||
        left.release != right.release || left.started_at != right.started_at ||
        left.finished_at != right.finished_at || left.shown != right.shown ||
        left.steps.size() != right.steps.size())
        return false;
    for (std::size_t index = 0; index < left.steps.size(); ++index) {
        if (left.steps[index].step != right.steps[index].step ||
            left.steps[index].result != right.steps[index].result ||
            left.steps[index].detail != right.steps[index].detail)
            return false;
    }
    return true;
}

void names() {
    OA_CHECK(
        std::string_view(update::outcome_name(update::Outcome::failed_restored)) ==
        "failed-restored"
    );
    OA_CHECK(
        std::string_view(update::outcome_name(update::Outcome::installs_next_start)) ==
        "installs-next-start"
    );
    OA_CHECK(update::parse_outcome("recovery-incomplete") == update::Outcome::recovery_incomplete);
    OA_CHECK(update::parse_outcome("roll-back-failed") == update::Outcome::roll_back_failed);
    OA_CHECK(!update::parse_outcome("failed"));
    OA_CHECK(std::string_view(update::step_name(update::Step::installer)) == "installer");
    OA_CHECK(update::parse_step("restore") == update::Step::restore);
    OA_CHECK(!update::parse_step("download"));
}

void outcomes_of_a_failed_run() {
    OA_CHECK(
        update::failure_outcome(update::Action::install, false, true) ==
        update::Outcome::not_installed
    );
    OA_CHECK(
        update::failure_outcome(update::Action::install, true, true) ==
        update::Outcome::failed_restored
    );
    OA_CHECK(
        update::failure_outcome(update::Action::install, true, false) ==
        update::Outcome::recovery_incomplete
    );
    OA_CHECK(
        update::failure_outcome(update::Action::roll_back, false, true) ==
        update::Outcome::roll_back_failed
    );
    OA_CHECK(
        update::failure_outcome(update::Action::roll_back, true, true) ==
        update::Outcome::roll_back_failed
    );
    OA_CHECK(
        update::failure_outcome(update::Action::roll_back, true, false) ==
        update::Outcome::roll_back_incomplete
    );

    OA_CHECK(update::failed_outcome(update::Outcome::not_installed));
    OA_CHECK(update::failed_outcome(update::Outcome::cancelled));
    OA_CHECK(update::failed_outcome(update::Outcome::running));
    OA_CHECK(!update::failed_outcome(update::Outcome::installed));
    OA_CHECK(!update::failed_outcome(update::Outcome::rolled_back));
    OA_CHECK(!update::failed_outcome(update::Outcome::opened_installer));

    OA_CHECK(update::holds_release(update::Outcome::failed_restored));
    OA_CHECK(update::holds_release(update::Outcome::not_installed));
    OA_CHECK(update::holds_release(update::Outcome::cancelled));
    OA_CHECK(update::holds_release(update::Outcome::recovery_incomplete));
    OA_CHECK(!update::holds_release(update::Outcome::installed));
    // A failed roll back leaves the release that was running; nothing to hold.
    OA_CHECK(!update::holds_release(update::Outcome::roll_back_failed));
}

void round_trip() {
    const update::Receipt written = sample();
    const std::string text = update::receipt_text(written);
    std::string error;
    const std::optional<update::Receipt> read = update::read_receipt(bytes_of(text), &error);
    OA_CHECK(read.has_value());
    OA_CHECK(error.empty());
    OA_CHECK(read && same(*read, written));
    // An install writes no reason, and a roll back does.
    OA_CHECK(text.find("\"reason\"") == std::string::npos);
    update::Receipt back = sample();
    back.action = update::Action::roll_back;
    back.reason = update::RollBackReason::failed_starts;
    back.outcome = update::Outcome::rolled_back;
    back.shown = true;
    const std::optional<update::Receipt> back_read =
        update::read_receipt(bytes_of(update::receipt_text(back)), &error);
    OA_CHECK(back_read && same(*back_read, back));
    // A version never learned stays empty.
    update::Receipt early = sample();
    early.to_version.clear();
    early.outcome = update::Outcome::not_installed;
    const std::optional<update::Receipt> early_read =
        update::read_receipt(bytes_of(update::receipt_text(early)), &error);
    OA_CHECK(early_read && early_read->to_version.empty());
}

void a_long_detail_is_cut() {
    update::Receipt receipt = sample();
    receipt.steps[2].detail = std::string(update::most_step_detail_bytes - 1, 'a') + "éé";
    std::string error;
    const std::optional<update::Receipt> read =
        update::read_receipt(bytes_of(update::receipt_text(receipt)), &error);
    OA_CHECK(read.has_value());
    // The cut lands on a whole character.
    OA_CHECK(read && read->steps[2].detail.size() == update::most_step_detail_bytes - 1);
}

void refuses(std::string_view name, std::string_view json, std::string_view snippet) {
    std::string error;
    const std::optional<update::Receipt> read = update::read_receipt(bytes_of(json), &error);
    if (read.has_value() || error.find(snippet) == std::string::npos)
        std::fprintf(
            stderr, "receipt case %.*s: %s\n", int(name.size()), name.data(), error.c_str()
        );
    OA_CHECK(!read.has_value());
    OA_CHECK(error.find(snippet) != std::string::npos);
}

void refusals() {
    const std::string tail =
        R"("from_version":"0.8.7","to_version":"0.8.8","release":12,"started_at":1,"finished_at":2,"steps":[],"shown":false})";
    refuses(
        "format", R"({"receipt":2,"action":"install","outcome":"installed",)" + tail, "receipt 1"
    );
    refuses("action", R"({"receipt":1,"action":"undo","outcome":"installed",)" + tail, "action");
    refuses("outcome", R"({"receipt":1,"action":"install","outcome":"fine",)" + tail, "outcome");
    refuses(
        "twice",
        R"({"receipt":1,"receipt":1,"action":"install","outcome":"installed",)" + tail,
        "named twice"
    );
    refuses(
        "version",
        R"({"receipt":1,"action":"install","outcome":"installed","from_version":"eight","to_version":"","release":1,"started_at":1,"finished_at":2,"steps":[],"shown":false})",
        "from_version"
    );
    refuses(
        "step",
        R"({"receipt":1,"action":"install","outcome":"installed","from_version":"0.8.7","to_version":"","release":1,"started_at":1,"finished_at":2,"steps":[{"step":"fly","result":"done","detail":""}],"shown":false})",
        "step"
    );
    refuses(
        "result",
        R"({"receipt":1,"action":"install","outcome":"installed","from_version":"0.8.7","to_version":"","release":1,"started_at":1,"finished_at":2,"steps":[{"step":"wait","result":"maybe","detail":""}],"shown":false})",
        "result"
    );
    refuses(
        "release",
        R"({"receipt":1,"action":"install","outcome":"installed","from_version":"0.8.7","to_version":"","release":-1,"started_at":1,"finished_at":2,"steps":[],"shown":false})",
        "release"
    );
    refuses(
        "shown",
        R"({"receipt":1,"action":"install","outcome":"installed",)" +
            tail.substr(0, tail.find(",\"shown\"")) + "}",
        "shown"
    );
    std::string many =
        R"({"receipt":1,"action":"install","outcome":"installed","from_version":"0.8.7","to_version":"","release":1,"started_at":1,"finished_at":2,"steps":[)";
    for (std::size_t index = 0; index <= update::most_receipt_steps; ++index) {
        if (index > 0)
            many += ',';
        many += R"({"step":"wait","result":"done","detail":""})";
    }
    many += R"(],"shown":false})";
    refuses("many", many, "too many");
    refuses("not json", "receipt", "");
}

void file_round_trip() {
    const fs::path folder = oa::test::make_scratch_directory("update-receipt");
    const fs::path path = folder / std::string(update::receipt_file_name);
    OA_CHECK(!update::load_receipt(path).has_value());
    std::string error;
    update::Receipt receipt = sample();
    OA_CHECK(update::write_receipt(path, receipt, &error));
    std::optional<update::Receipt> loaded = update::load_receipt(path);
    OA_CHECK(loaded && same(*loaded, receipt));
    receipt.shown = true;
    OA_CHECK(update::write_receipt(path, receipt, &error));
    loaded = update::load_receipt(path);
    OA_CHECK(loaded && loaded->shown);
    // The temporary file beside it is gone.
    OA_CHECK(!fs::exists(folder / "receipt.json.tmp"));
    {
        std::ofstream broken(path, std::ios::binary | std::ios::trunc);
        broken << "{";
    }
    OA_CHECK(!update::load_receipt(path).has_value());
    OA_CHECK(!update::write_receipt(folder / "missing" / "receipt.json", receipt, &error));
    OA_CHECK(error.find("the receipt") != std::string::npos);
    std::error_code removed;
    fs::remove_all(folder, removed);
}

update::State unconfirmed_state() {
    update::State state;
    state.installed_version = "0.8.8";
    state.installed_release = 12;
    state.kept_version = "0.8.7";
    state.kept_path = "/kept";
    state.preferences_copy = "/kept/preferences.conf";
    state.failed_starts = 1;
    return state;
}

void a_quit_is_not_a_failed_start() {
    update::State state = unconfirmed_state();
    // The start counted at its beginning is taken back when the player quits.
    update::uncount_quit_start(state);
    OA_CHECK(state.failed_starts == 0);
    OA_CHECK(state.quit_before_menu);
    OA_CHECK(update::start_end(state) == update::StartEnd::quit);
    // The count never goes below 0.
    update::uncount_quit_start(state);
    OA_CHECK(state.failed_starts == 0);
    // A new start clears the marker, and a crash then leaves the count.
    OA_CHECK(update::count_failed_start(state) == 1);
    OA_CHECK(!state.quit_before_menu);
    OA_CHECK(update::start_end(state) == update::StartEnd::failed);
    // Reaching the main menu confirms and clears both.
    state.quit_before_menu = true;
    update::confirm_state(state);
    OA_CHECK(!state.quit_before_menu && state.failed_starts == 0);
    OA_CHECK(update::start_end(state) == update::StartEnd::reached_menu);
    // A confirmed state is not uncounted.
    update::uncount_quit_start(state);
    OA_CHECK(!state.quit_before_menu);
    // A state that cannot be read is a failed start.
    OA_CHECK(update::start_end(std::nullopt) == update::StartEnd::failed);
}

void the_quit_marker_in_the_file() {
    update::State state = unconfirmed_state();
    std::string text = update::state_text(state);
    // Written only when set, so a state file keeps the bytes it had.
    OA_CHECK(text.find("quit_before_menu") == std::string::npos);
    update::uncount_quit_start(state);
    text = update::state_text(state);
    OA_CHECK(text.find("\"quit_before_menu\":true") != std::string::npos);
    std::string error;
    const std::optional<update::State> read = update::read_state(bytes_of(text), &error);
    OA_CHECK(read && read->quit_before_menu && read->failed_starts == 0);
    const std::string wrong =
        text.substr(0, text.find("true")) + "\"yes\"" + text.substr(text.find("true") + 4);
    OA_CHECK(!update::read_state(bytes_of(wrong), &error).has_value());
}

} // namespace

int main() {
    names();
    outcomes_of_a_failed_run();
    round_trip();
    a_long_detail_is_cut();
    refusals();
    file_round_trip();
    a_quit_is_not_a_failed_start();
    the_quit_marker_in_the_file();
    return oa::test::check_exit_status();
}

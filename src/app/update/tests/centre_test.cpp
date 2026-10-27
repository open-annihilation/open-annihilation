// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The update centre: the state each set of facts gives, the actions each
// state allows, the words in English, the route's install words, the
// scheduled install, Later kept apart from Skip, a held release, and the
// rate and time left of a download. Nothing is fetched or installed.

#include "oa/app/update/centre.hpp"
#include "oa/app/update/choices.hpp"
#include "oa/app/update/interface_words.hpp"
#include "oa/app/update/offer.hpp"
#include "oa/ui/library/interface_words.hpp"

#include "oa/test/check.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <set>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace update = oa::app::update;
using update::Act;
using update::Stage;

/// A release 0.8.8, number 12, out for 0.8.7, with a 24.5 MB package.
update::CentreInputs offered_inputs() {
    update::CentreInputs inputs;
    inputs.current_version = "0.8.7";
    inputs.check = update::CheckState::offered;
    inputs.offer_version = "0.8.8";
    inputs.offer_release = 12;
    inputs.offer_size = 25690112;
    return inputs;
}

/// The same release, downloading.
update::CentreInputs downloading_inputs() {
    update::CentreInputs inputs = offered_inputs();
    inputs.download = update::DownloadState::downloading;
    inputs.download_release = 12;
    inputs.download_version = "0.8.8";
    inputs.received = 10590617;
    inputs.total = 25690112;
    inputs.bytes_per_second = 215000;
    return inputs;
}

bool has(const std::vector<Act>& actions, Act act) {
    return std::find(actions.begin(), actions.end(), act) != actions.end();
}

/// The caption of one action in the words, or empty.
std::string caption_of(const update::CentreWords& words, Act act) {
    for (const update::ActWords& one : words.actions)
        if (one.act == act)
            return one.caption;
    return {};
}

/// The note of one action in the words, or empty.
std::string note_of(const update::CentreWords& words, Act act) {
    for (const update::ActWords& one : words.actions)
        if (one.act == act)
            return one.note;
    return {};
}

update::CentreWords english(const update::Centre& centre) {
    return update::centre_words(centre, {});
}

void the_check_alone() {
    update::CentreInputs inputs;
    inputs.current_version = "0.8.7";
    OA_CHECK(update::centre_state(inputs).stage == Stage::not_checked);
    OA_CHECK(english(update::centre_state(inputs)).status == "Not checked yet");
    OA_CHECK(update::centre_state(inputs).actions == std::vector<Act>{Act::check_now});

    inputs.mode = update::CheckMode::never;
    OA_CHECK(update::centre_state(inputs).stage == Stage::never);
    OA_CHECK(update::centre_state(inputs).actions.empty());
    OA_CHECK(english(update::centre_state(inputs)).status == "OA never checks");

    inputs.mode = update::CheckMode::automatically;
    inputs.check = update::CheckState::checking;
    OA_CHECK(update::centre_state(inputs).stage == Stage::checking);
    OA_CHECK(update::centre_state(inputs).actions.empty());

    inputs.check = update::CheckState::up_to_date;
    OA_CHECK(update::centre_state(inputs).stage == Stage::up_to_date);
    OA_CHECK(english(update::centre_state(inputs)).status == "Up to date");
}

void check_failures_read_differently() {
    update::CentreInputs inputs;
    inputs.current_version = "0.8.7";
    inputs.check = update::CheckState::failed;
    inputs.check_failure = update::CheckFailure::connect;
    update::Centre centre = update::centre_state(inputs);
    OA_CHECK(centre.stage == Stage::could_not_connect);
    OA_CHECK(english(centre).status == "Could not connect. Try again later.");
    OA_CHECK(centre.actions == std::vector<Act>{Act::retry});
    OA_CHECK(caption_of(english(centre), Act::retry) == "Try again");

    inputs.check_failure = update::CheckFailure::refused;
    centre = update::centre_state(inputs);
    OA_CHECK(centre.stage == Stage::not_verified);
    OA_CHECK(english(centre).status == "The update could not be verified.");
    OA_CHECK(
        english(centre).detail == "Its signature was refused. Nothing was downloaded or changed."
    );
    // Details only: there is no way past a refused signature.
    OA_CHECK(centre.actions == std::vector<Act>{Act::details});

    inputs.check_failure = update::CheckFailure::storage;
    OA_CHECK(update::centre_state(inputs).stage == Stage::could_not_check);
}

void an_offer_is_download_later_skip() {
    const update::CentreInputs inputs = offered_inputs();
    const update::Centre centre = update::centre_state(inputs);
    OA_CHECK(centre.stage == Stage::offered);
    OA_CHECK(centre.target_version == "0.8.8");
    OA_CHECK(centre.target_release == 12);
    OA_CHECK((centre.actions == std::vector<Act>{Act::download, Act::later, Act::skip}));
    const update::CentreWords words = english(centre);
    OA_CHECK(words.status == "0.8.8 is out");
    OA_CHECK(words.detail == "You have 0.8.7. The update is 24.5 MB.");
    OA_CHECK(caption_of(words, Act::download) == "Download update");
    OA_CHECK(caption_of(words, Act::later) == "Later");
    OA_CHECK(caption_of(words, Act::skip) == "Skip this version");
}

void skip_and_later_are_kept_apart() {
    oa::platform::preferences::Values values;
    update::CentreInputs inputs = offered_inputs();
    // Later writes nothing, so the same release is offered at the next start.
    inputs.skipped = update::read_skipped(values);
    OA_CHECK(update::centre_state(inputs).stage == Stage::offered);

    // Skip stays quiet for that release.
    update::write_skipped(values, 12);
    inputs.skipped = update::read_skipped(values);
    update::Centre centre = update::centre_state(inputs);
    OA_CHECK(centre.stage == Stage::skipped);
    OA_CHECK(english(centre).status == "0.8.8 is skipped");
    OA_CHECK((centre.actions == std::vector<Act>{Act::try_version, Act::check_now}));
    OA_CHECK(caption_of(english(centre), Act::try_version) == "Try this version");

    // A newer release asks again.
    inputs.offer_release = 13;
    inputs.offer_version = "0.8.9";
    OA_CHECK(update::centre_state(inputs).stage == Stage::offered);

    // Try this version takes the skip back.
    inputs.offer_release = 12;
    inputs.offer_version = "0.8.8";
    update::clear_skipped(values);
    inputs.skipped = update::read_skipped(values);
    OA_CHECK(update::centre_state(inputs).stage == Stage::offered);
}

void a_download_shows_bytes_rate_and_controls() {
    const update::Centre centre = update::centre_state(downloading_inputs());
    OA_CHECK(centre.stage == Stage::downloading);
    OA_CHECK(centre.percent == 41);
    OA_CHECK(centre.seconds_left.has_value() && *centre.seconds_left == 71U);
    OA_CHECK((centre.actions == std::vector<Act>{Act::pause, Act::cancel_download}));
    const update::CentreWords words = english(centre);
    OA_CHECK(words.status == "Downloading 0.8.8");
    OA_CHECK(words.detail == "10.1 MB of 24.5 MB · 41% · about a minute left");
    OA_CHECK(words.pill == "0.8.8 · 41%");
    OA_CHECK(words.note == "Downloads pause during a match and carry on after it.");
    OA_CHECK(caption_of(words, Act::pause) == "Pause");
    OA_CHECK(caption_of(words, Act::cancel_download) == "Cancel download");
}

void pauses_say_why() {
    update::CentreInputs inputs = downloading_inputs();
    inputs.download = update::DownloadState::paused_by_player;
    update::Centre centre = update::centre_state(inputs);
    OA_CHECK(centre.stage == Stage::paused_by_player);
    OA_CHECK((centre.actions == std::vector<Act>{Act::resume, Act::cancel_download}));
    OA_CHECK(!centre.seconds_left.has_value());
    OA_CHECK(english(centre).status == "Download paused");
    OA_CHECK(english(centre).pill == "0.8.8 · paused");
    OA_CHECK(caption_of(english(centre), Act::resume) == "Resume");

    // A match's pause carries on by itself, so it offers no Resume.
    inputs.download = update::DownloadState::paused_for_match;
    centre = update::centre_state(inputs);
    OA_CHECK(centre.stage == Stage::paused_by_match);
    OA_CHECK(centre.actions == std::vector<Act>{Act::cancel_download});
    OA_CHECK(english(centre).status == "Paused during a match");
    OA_CHECK(english(centre).note == "Downloads pause during a match and carry on after it.");

    inputs.download = update::DownloadState::verifying;
    centre = update::centre_state(inputs);
    OA_CHECK(centre.stage == Stage::verifying);
    OA_CHECK(centre.actions.empty());
    OA_CHECK(english(centre).status == "Checking the download");
}

void ready_is_three_choices_and_scheduling_is_revocable() {
    oa::platform::preferences::Values values;
    update::CentreInputs inputs = downloading_inputs();
    inputs.download = update::DownloadState::ready;
    inputs.received = inputs.total;
    update::Centre centre = update::centre_state(inputs);
    OA_CHECK(centre.stage == Stage::ready);
    OA_CHECK((
        centre.actions == std::vector<Act>{Act::install_on_quit, Act::install_now, Act::keep_ready}
    ));
    update::CentreWords words = english(centre);
    OA_CHECK(words.status == "0.8.8 is ready");
    OA_CHECK(words.detail == "Downloaded and checked · 24.5 MB");
    OA_CHECK(caption_of(words, Act::install_on_quit) == "Install when I quit");
    OA_CHECK(caption_of(words, Act::install_now) == "Install and restart");
    OA_CHECK(caption_of(words, Act::keep_ready) == "Keep it ready");
    OA_CHECK(note_of(words, Act::keep_ready) == "Nothing is scheduled.");
    OA_CHECK(
        note_of(words, Act::install_on_quit) ==
        "It installs as you quit. Change it any time in Settings › Updates."
    );
    // Keep it ready schedules nothing, so quitting installs nothing.
    OA_CHECK(!update::installs_at_quit(values, 12));

    // Install when I quit is stored, shown and installs at quit.
    update::write_scheduled(values, 12);
    inputs.scheduled = update::read_scheduled(values);
    OA_CHECK(update::installs_at_quit(values, 12));
    OA_CHECK(!update::installs_at_quit(values, 13));
    centre = update::centre_state(inputs);
    OA_CHECK(centre.stage == Stage::scheduled);
    OA_CHECK((centre.actions == std::vector<Act>{Act::unschedule, Act::install_now}));
    words = english(centre);
    OA_CHECK(words.status == "0.8.8 installs when you quit");
    OA_CHECK(words.pill == "0.8.8 installs when you quit");
    OA_CHECK(caption_of(words, Act::unschedule) == "Don't install when I quit");

    // Taking it back leaves the release ready and quitting installs nothing.
    update::clear_scheduled(values);
    inputs.scheduled = update::read_scheduled(values);
    OA_CHECK(!update::installs_at_quit(values, 12));
    OA_CHECK(update::centre_state(inputs).stage == Stage::ready);

    // A schedule for another release does not schedule this one.
    update::write_scheduled(values, 11);
    inputs.scheduled = update::read_scheduled(values);
    OA_CHECK(update::centre_state(inputs).stage == Stage::ready);
    values[std::string(update::key::install_on_quit)] = "twelve";
    OA_CHECK(!update::read_scheduled(values).has_value());
}

void the_install_policy() {
    oa::platform::preferences::Values values;
    OA_CHECK(update::read_install_policy(values) == update::InstallPolicy::ask);
    update::write_install_policy(values, update::InstallPolicy::when_i_quit);
    OA_CHECK(values[std::string(update::key::install_policy)] == "when-i-quit");
    OA_CHECK(update::read_install_policy(values) == update::InstallPolicy::when_i_quit);
    values[std::string(update::key::install_policy)] = "always";
    OA_CHECK(update::read_install_policy(values) == update::InstallPolicy::ask);
}

void the_install_words_name_the_route() {
    update::CentreInputs inputs = downloading_inputs();
    inputs.download = update::DownloadState::ready;

    inputs.way = update::InstallWay::windows_installer;
    update::CentreWords words = english(update::centre_state(inputs));
    OA_CHECK(caption_of(words, Act::install_now) == "Install and restart");
    OA_CHECK(note_of(words, Act::install_now) == "Windows asks for permission.");

    inputs.way = update::InstallWay::macos_installer;
    words = english(update::centre_state(inputs));
    OA_CHECK(caption_of(words, Act::install_now) == "Open the installer");
    OA_CHECK(
        note_of(words, Act::install_now) == "Start Open Annihilation from Applications afterwards."
    );

    inputs.way = update::InstallWay::next_start;
    words = english(update::centre_state(inputs));
    OA_CHECK(caption_of(words, Act::install_now) == "Close the game");
    OA_CHECK(note_of(words, Act::install_now) == "It updates as it next starts.");

    inputs.way = update::InstallWay::restart;
    words = english(update::centre_state(inputs));
    OA_CHECK(caption_of(words, Act::install_now) == "Install and restart");
    OA_CHECK(note_of(words, Act::install_now) == "The game closes, installs it and starts again.");

    // A package manager has no install action, and says who updates the copy.
    update::CentreInputs managed = offered_inputs();
    managed.way = update::InstallWay::package_manager;
    managed.manager = "Flathub";
    const update::Centre centre = update::centre_state(managed);
    OA_CHECK(centre.stage == Stage::managed);
    OA_CHECK(centre.actions == std::vector<Act>{Act::check_now});
    OA_CHECK(!has(centre.actions, Act::download) && !has(centre.actions, Act::install_now));
    OA_CHECK(english(centre).detail == "Updated by Flathub");

    // A release this copy can only open the page of.
    update::CentreInputs page = offered_inputs();
    page.way = update::InstallWay::page_only;
    const update::Centre paged = update::centre_state(page);
    OA_CHECK(paged.stage == Stage::page_only);
    OA_CHECK((paged.actions == std::vector<Act>{Act::open_page, Act::later, Act::skip}));
    OA_CHECK(caption_of(english(paged), Act::open_page) == "Open the release page");
}

void install_way_follows_the_route() {
    using update::InstallWay;
    using update::Route;
    OA_CHECK(
        update::install_way(false, "Flathub", Route::linux_versions, false, false, false) ==
        InstallWay::package_manager
    );
    OA_CHECK(
        update::install_way(false, "", std::nullopt, false, false, false) == InstallWay::page_only
    );
    OA_CHECK(
        update::install_way(true, "", std::nullopt, false, false, false) == InstallWay::page_only
    );
    OA_CHECK(
        update::install_way(true, "", Route::windows_folder, false, true, false) ==
        InstallWay::page_only
    );
    OA_CHECK(
        update::install_way(true, "", Route::windows_installer, true, false, false) ==
        InstallWay::windows_installer
    );
    OA_CHECK(
        update::install_way(true, "", Route::windows_folder, false, false, false) ==
        InstallWay::restart
    );
    OA_CHECK(
        update::install_way(true, "", Route::macos_bundle, true, false, false) ==
        InstallWay::macos_installer
    );
    OA_CHECK(
        update::install_way(true, "", Route::macos_bundle, false, false, false) ==
        InstallWay::restart
    );
    OA_CHECK(
        update::install_way(true, "", Route::linux_versions, false, false, true) ==
        InstallWay::next_start
    );
    OA_CHECK(
        update::install_way(true, "", Route::linux_versions, false, false, false) ==
        InstallWay::restart
    );
}

void download_failures_read_differently() {
    update::CentreInputs inputs = downloading_inputs();
    inputs.download = update::DownloadState::failed;

    inputs.download_failure = update::DownloadFailure::connect;
    update::Centre centre = update::centre_state(inputs);
    OA_CHECK(centre.stage == Stage::could_not_connect);
    OA_CHECK(centre.actions == std::vector<Act>{Act::retry});

    inputs.download_failure = update::DownloadFailure::wrong_file;
    centre = update::centre_state(inputs);
    OA_CHECK(centre.stage == Stage::not_verified);
    OA_CHECK(
        english(centre).detail ==
        "The download was not the file the signed list names, so it was deleted."
    );

    inputs.download_failure = update::DownloadFailure::storage;
    inputs.download_reason = "the download could not be stored";
    centre = update::centre_state(inputs);
    OA_CHECK(centre.stage == Stage::download_stopped);
    OA_CHECK((centre.actions == std::vector<Act>{Act::retry, Act::cancel_download}));
    OA_CHECK(english(centre).status == "The download stopped");
    OA_CHECK(english(centre).detail == "the download could not be stored");

    // A cancelled download goes back to the offer.
    inputs.download_failure = update::DownloadFailure::cancelled;
    OA_CHECK(update::centre_state(inputs).stage == Stage::offered);

    // A download of an older release than the one now out is not shown.
    inputs.download_failure = update::DownloadFailure::connect;
    inputs.offer_release = 13;
    inputs.offer_version = "0.8.9";
    OA_CHECK(update::centre_state(inputs).stage == Stage::offered);
}

update::Receipt failed_receipt(update::Outcome outcome) {
    update::Receipt receipt;
    receipt.action = update::Action::install;
    receipt.outcome = outcome;
    receipt.from_version = "0.8.7";
    receipt.to_version = "0.8.8";
    receipt.release = 12;
    receipt.steps.push_back({update::Step::verify, update::StepResult::done, {}});
    receipt.steps.push_back(
        {update::Step::swap, update::StepResult::failed, "another program had it open"}
    );
    receipt.steps.push_back({update::Step::undo, update::StepResult::done, {}});
    return receipt;
}

void install_failures_read_differently_and_hold_the_release() {
    oa::platform::preferences::Values values;
    update::write_held(values, 12);
    OA_CHECK(update::release_held(values, 12));
    OA_CHECK(!update::release_held(values, 13));
    update::CentreInputs inputs = offered_inputs();
    inputs.held = update::read_held(values);

    inputs.receipt = failed_receipt(update::Outcome::failed_restored);
    update::Centre centre = update::centre_state(inputs);
    OA_CHECK(centre.stage == Stage::failed_restored);
    OA_CHECK(centre.steps.size() == 3U);
    OA_CHECK((centre.actions == std::vector<Act>{Act::retry, Act::details}));
    update::CentreWords words = english(centre);
    OA_CHECK(words.status == "The update failed. 0.8.7 was put back.");
    OA_CHECK(words.detail == "0.8.8 waits until you choose to try again.");

    inputs.receipt = failed_receipt(update::Outcome::not_installed);
    centre = update::centre_state(inputs);
    OA_CHECK(centre.stage == Stage::not_installed);
    OA_CHECK(english(centre).status == "Not installed. 0.8.7 is unchanged.");
    OA_CHECK(english(centre).detail == "It stopped before any file moved.");

    inputs.receipt = failed_receipt(update::Outcome::cancelled);
    centre = update::centre_state(inputs);
    OA_CHECK(centre.stage == Stage::install_cancelled);
    OA_CHECK(english(centre).status == "The update was cancelled.");

    inputs.receipt = failed_receipt(update::Outcome::recovery_incomplete);
    centre = update::centre_state(inputs);
    OA_CHECK(centre.stage == Stage::recovery_incomplete);
    OA_CHECK((centre.actions == std::vector<Act>{Act::details, Act::show_log}));
    OA_CHECK(english(centre).status == "Recovery could not finish.");

    // A run left running was cut short: its recovery did not finish.
    inputs.receipt = failed_receipt(update::Outcome::running);
    OA_CHECK(update::centre_state(inputs).stage == Stage::recovery_incomplete);

    // A held release with no receipt still did not install.
    inputs.receipt.reset();
    OA_CHECK(update::centre_state(inputs).stage == Stage::not_installed);

    // A newer release than the held one is offered as usual.
    inputs.offer_release = 13;
    inputs.offer_version = "0.8.9";
    OA_CHECK(update::centre_state(inputs).stage == Stage::offered);

    // Try again lets the held release be offered.
    inputs.offer_release = 12;
    inputs.offer_version = "0.8.8";
    update::clear_held(values);
    inputs.held = update::read_held(values);
    inputs.receipt = failed_receipt(update::Outcome::failed_restored);
    OA_CHECK(update::centre_state(inputs).stage == Stage::offered);
}

void a_roll_back_receipt_has_its_own_words() {
    update::CentreInputs inputs = offered_inputs();
    inputs.held = 12;
    update::Receipt receipt = failed_receipt(update::Outcome::roll_back_failed);
    receipt.action = update::Action::roll_back;
    inputs.receipt = receipt;
    OA_CHECK(
        english(update::centre_state(inputs)).status == "The roll back failed. Nothing was changed."
    );
    inputs.receipt->outcome = update::Outcome::roll_back_incomplete;
    OA_CHECK(
        english(update::centre_state(inputs)).status ==
        "The roll back failed. The game could not be put back."
    );
}

void this_start_after_an_update() {
    update::CentreInputs inputs;
    inputs.current_version = "0.8.8";
    inputs.updated_from = "0.8.7";
    inputs.kept_version = "0.8.7";
    inputs.unconfirmed = true;
    update::Centre centre = update::centre_state(inputs);
    OA_CHECK(centre.stage == Stage::confirming);
    OA_CHECK(centre.actions.empty());
    OA_CHECK(english(centre).status == "Finishing the update to 0.8.8");

    inputs.unconfirmed = false;
    centre = update::centre_state(inputs);
    OA_CHECK(centre.stage == Stage::updated);
    OA_CHECK(centre.came_from == "0.8.7");
    OA_CHECK((centre.actions == std::vector<Act>{Act::whats_new, Act::roll_back}));
    const update::CentreWords words = english(centre);
    OA_CHECK(words.status == "Updated to 0.8.8");
    OA_CHECK(words.detail == "Updated from 0.8.7");
    OA_CHECK(words.note == "0.8.7 is kept. Roll back from Settings › Updates.");
    OA_CHECK(caption_of(words, Act::roll_back) == "Roll back…");

    // A check that finds nothing newer still says this start was the update.
    inputs.check = update::CheckState::up_to_date;
    OA_CHECK(update::centre_state(inputs).stage == Stage::updated);

    update::CentreInputs back;
    back.current_version = "0.8.7";
    back.rolled_back_from = "0.8.8";
    centre = update::centre_state(back);
    OA_CHECK(centre.stage == Stage::rolled_back);
    OA_CHECK(english(centre).status == "Back on 0.8.7");
    OA_CHECK(english(centre).detail == "0.8.8 is not offered again until a newer release.");
}

void handing_over_is_installing() {
    update::CentreInputs inputs = downloading_inputs();
    inputs.download = update::DownloadState::ready;
    inputs.handing_over = true;
    const update::Centre centre = update::centre_state(inputs);
    OA_CHECK(centre.stage == Stage::installing);
    OA_CHECK(centre.actions.empty());
    OA_CHECK(english(centre).status == "Installing 0.8.8");
}

void roll_back_is_offered_only_at_rest() {
    update::CentreInputs inputs = offered_inputs();
    inputs.kept_version = "0.8.6";
    OA_CHECK(has(update::centre_state(inputs).actions, Act::roll_back));
    inputs = downloading_inputs();
    inputs.kept_version = "0.8.6";
    OA_CHECK(!has(update::centre_state(inputs).actions, Act::roll_back));
}

void step_words_in_english() {
    const update::ReceiptStep verify{update::Step::verify, update::StepResult::done, {}};
    OA_CHECK(
        update::step_words(verify, "0.8.7", "0.8.8", {}) ==
        "Checked the package against the signed list"
    );
    const update::ReceiptStep swap{
        update::Step::swap, update::StepResult::failed, "another program had it open"
    };
    OA_CHECK(
        update::step_words(swap, "0.8.7", "0.8.8", {}) ==
        "Could not put the new files in place: another program had it open"
    );
    const update::ReceiptStep swapped{update::Step::swap, update::StepResult::done, {}};
    OA_CHECK(
        update::step_words(swapped, "0.8.7", "0.8.8", {}) == "Put 0.8.8 in place, keeping 0.8.7"
    );
    const update::ReceiptStep started{update::Step::start, update::StepResult::done, "0.8.7"};
    OA_CHECK(update::step_words(started, "0.8.7", "0.8.8", {}) == "Started 0.8.7");
    const update::ReceiptStep undo{update::Step::undo, update::StepResult::done, {}};
    OA_CHECK(update::step_words(undo, "0.8.7", "0.8.8", {}) == "Put every changed file back");
    const update::ReceiptStep watch{update::Step::watch, update::StepResult::failed, {}};
    OA_CHECK(
        update::step_words(watch, "0.8.7", "0.8.8", {}) == "0.8.8 closed twice before the main menu"
    );
    const std::vector<std::string> steps = update::recovery_steps("0.8.7", {});
    OA_CHECK(steps.size() == 3U);
    OA_CHECK(steps[1] == "Install 0.8.7 again from its page, over the copy you have.");
}

void time_left_and_percent() {
    OA_CHECK(update::time_left_words(20, {}) == "less than a minute left");
    OA_CHECK(update::time_left_words(70, {}) == "about a minute left");
    OA_CHECK(update::time_left_words(150, {}) == "about 3 minutes left");
    OA_CHECK(update::time_left_words(3599, {}) == "about 60 minutes left");
    OA_CHECK(update::time_left_words(3600, {}) == "more than an hour left");
    OA_CHECK(update::download_percent(0, 0) == -1);
    OA_CHECK(update::download_percent(0, 10) == 0);
    OA_CHECK(update::download_percent(5, 10) == 50);
    OA_CHECK(update::download_percent(10, 10) == 100);
    OA_CHECK(update::download_percent(11, 10) == 100);
    OA_CHECK(!update::download_seconds_left(0, 100, 0).has_value());
    OA_CHECK(!update::download_seconds_left(0, 0, 10).has_value());
    OA_CHECK(update::download_seconds_left(0, 100, 30) == uint64_t{4});
    OA_CHECK(update::download_seconds_left(100, 100, 30) == uint64_t{0});
}

void the_rate_meter() {
    update::RateMeter meter;
    OA_CHECK(meter.bytes_per_second() == 0U);
    meter.sample(1000, 0);
    meter.sample(1200, 100);
    // Under half a second of samples gives no rate.
    OA_CHECK(meter.bytes_per_second() == 0U);
    meter.sample(2000, 1000);
    OA_CHECK(meter.bytes_per_second() == 1000U);
    meter.sample(3000, 3000);
    OA_CHECK(meter.bytes_per_second() == 1500U);
    // Samples older than the window are dropped.
    meter.sample(20000, 3000 + 17000 * 4);
    meter.sample(21000, 3000 + 18000 * 4);
    OA_CHECK(meter.bytes_per_second() == 4000U);
    // Fewer bytes starts again: a restarted file.
    meter.sample(22000, 10);
    OA_CHECK(meter.bytes_per_second() == 0U);
    meter.reset();
    OA_CHECK(meter.bytes_per_second() == 0U);
}

/// The patterns a run of the words looked up, unfilled.
std::set<std::string>& looked_up() {
    static std::set<std::string> patterns;
    return patterns;
}

/// Records each pattern and shows it in English.
std::string_view record_pattern(void* /*context*/, std::string_view english) {
    looked_up().insert(std::string(english));
    return english;
}

void every_word_is_listed() {
    const oa::ui::library::TextHooks recording{nullptr, record_pattern};
    // Every stage, every way it installs, a kept version and a roll back's receipt.
    std::vector<update::CentreInputs> all;
    all.push_back(offered_inputs());
    update::CentreInputs check = offered_inputs();
    check.offer_version.clear();
    for (const update::CheckState state :
         {update::CheckState::idle,
          update::CheckState::checking,
          update::CheckState::up_to_date,
          update::CheckState::failed}) {
        check.check = state;
        for (const update::CheckFailure kind :
             {update::CheckFailure::connect,
              update::CheckFailure::refused,
              update::CheckFailure::storage}) {
            check.check_failure = kind;
            all.push_back(check);
        }
    }
    check.check = update::CheckState::idle;
    check.mode = update::CheckMode::never;
    all.push_back(check);
    update::CentreInputs skipped = offered_inputs();
    skipped.skipped = 12;
    all.push_back(skipped);
    for (const update::DownloadState state :
         {update::DownloadState::downloading,
          update::DownloadState::paused_by_player,
          update::DownloadState::paused_for_match,
          update::DownloadState::verifying,
          update::DownloadState::ready,
          update::DownloadState::failed}) {
        update::CentreInputs moving = downloading_inputs();
        moving.download = state;
        moving.kept_version = "0.8.6";
        for (const update::DownloadFailure kind :
             {update::DownloadFailure::connect,
              update::DownloadFailure::wrong_file,
              update::DownloadFailure::storage}) {
            moving.download_failure = kind;
            for (const update::InstallWay way :
                 {update::InstallWay::restart,
                  update::InstallWay::windows_installer,
                  update::InstallWay::macos_installer,
                  update::InstallWay::next_start}) {
                moving.way = way;
                all.push_back(moving);
                moving.scheduled = 12;
                all.push_back(moving);
                moving.scheduled.reset();
            }
        }
    }
    update::CentreInputs page = offered_inputs();
    page.way = update::InstallWay::page_only;
    all.push_back(page);
    page.way = update::InstallWay::package_manager;
    all.push_back(page);
    update::CentreInputs handing = downloading_inputs();
    handing.handing_over = true;
    all.push_back(handing);
    update::CentreInputs after;
    after.current_version = "0.8.8";
    after.updated_from = "0.8.7";
    after.kept_version = "0.8.7";
    all.push_back(after);
    after.unconfirmed = true;
    all.push_back(after);
    update::CentreInputs back;
    back.current_version = "0.8.7";
    back.rolled_back_from = "0.8.8";
    all.push_back(back);
    for (const update::Outcome outcome :
         {update::Outcome::running,
          update::Outcome::not_installed,
          update::Outcome::cancelled,
          update::Outcome::failed_restored,
          update::Outcome::recovery_incomplete}) {
        for (const update::Action action : {update::Action::install, update::Action::roll_back}) {
            update::CentreInputs held = offered_inputs();
            held.held = 12;
            held.receipt = failed_receipt(outcome);
            held.receipt->action = action;
            all.push_back(held);
        }
    }
    for (const update::CentreInputs& inputs : all)
        static_cast<void>(update::centre_words(update::centre_state(inputs), recording));
    for (const update::Step step :
         {update::Step::wait,
          update::Step::verify,
          update::Step::unpack,
          update::Step::swap,
          update::Step::installer,
          update::Step::start,
          update::Step::watch,
          update::Step::undo,
          update::Step::restore}) {
        for (const update::StepResult result :
             {update::StepResult::done, update::StepResult::failed}) {
            static_cast<void>(
                update::step_words({step, result, "why"}, "0.8.7", "0.8.8", recording)
            );
        }
    }
    static_cast<void>(update::recovery_steps("0.8.7", recording));
    for (const uint64_t seconds : {uint64_t{10}, uint64_t{70}, uint64_t{300}, uint64_t{4000}})
        static_cast<void>(update::time_left_words(seconds, recording));

    OA_CHECK(looked_up().size() > 80U);
    // Sizes are the Library's, and its list holds their words.
    const auto in = [](const auto& list, const std::string& pattern) {
        return std::find(std::begin(list), std::end(list), pattern) != std::end(list);
    };
    for (const std::string& pattern : looked_up()) {
        const bool listed =
            in(update::interface_words, pattern) || in(oa::ui::library::interface_words, pattern);
        if (!listed)
            std::fprintf(stderr, "not in interface_words: %s\n", pattern.c_str());
        OA_CHECK(listed);
    }
}

} // namespace

int main() {
    the_check_alone();
    check_failures_read_differently();
    an_offer_is_download_later_skip();
    skip_and_later_are_kept_apart();
    a_download_shows_bytes_rate_and_controls();
    pauses_say_why();
    ready_is_three_choices_and_scheduling_is_revocable();
    the_install_policy();
    the_install_words_name_the_route();
    install_way_follows_the_route();
    download_failures_read_differently();
    install_failures_read_differently_and_hold_the_release();
    a_roll_back_receipt_has_its_own_words();
    this_start_after_an_update();
    handing_over_is_installing();
    roll_back_is_offered_only_at_rest();
    step_words_in_english();
    time_left_and_percent();
    the_rate_meter();
    every_word_is_listed();
    return oa::test::check_exit_status();
}

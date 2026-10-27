// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The update centre's state, its words and its actions.
#include "oa/app/update/centre.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app::update {
namespace {

namespace library = oa::ui::library;

// The words. Each is looked up through the caller's hooks, then filled.

constexpr std::string_view not_checked_status = "Not checked yet";
constexpr std::string_view never_status = "OA never checks";
constexpr std::string_view checking_status = "Checking for updates…";
constexpr std::string_view up_to_date_status = "Up to date";
constexpr std::string_view is_out_status = "{version} is out";
constexpr std::string_view skipped_status = "{version} is skipped";
constexpr std::string_view updated_by_detail = "Updated by {name}";
constexpr std::string_view you_have_detail = "You have {current}. The update is {size}.";
constexpr std::string_view stay_note = "Your saved games, settings and mods stay as they are.";
constexpr std::string_view downloading_status = "Downloading {version}";
constexpr std::string_view paused_status = "Download paused";
constexpr std::string_view paused_match_status = "Paused during a match";
constexpr std::string_view match_note = "Downloads pause during a match and carry on after it.";
constexpr std::string_view progress_detail = "{done} of {size} · {percent}%";
constexpr std::string_view moving_pill = "{version} · {percent}%";
constexpr std::string_view paused_pill = "{version} · paused";
constexpr std::string_view verifying_status = "Checking the download";
constexpr std::string_view verifying_pill = "{version} · checking";
constexpr std::string_view ready_status = "{version} is ready";
constexpr std::string_view ready_detail = "Downloaded and checked · {size}";
constexpr std::string_view scheduled_status = "{version} installs when you quit";
constexpr std::string_view installing_status = "Installing {version}";
constexpr std::string_view confirming_status = "Finishing the update to {version}";
constexpr std::string_view updated_status = "Updated to {version}";
constexpr std::string_view updated_detail = "Updated from {from}";
constexpr std::string_view kept_detail = "{version} is kept. Roll back from Settings › Updates.";
constexpr std::string_view back_on_status = "Back on {version}";
constexpr std::string_view not_offered_detail =
    "{version} is not offered again until a newer release.";
constexpr std::string_view could_not_connect_status = "Could not connect. Try again later.";
constexpr std::string_view could_not_check_status = "Could not check";
constexpr std::string_view not_verified_status = "The update could not be verified.";
constexpr std::string_view refused_detail =
    "Its signature was refused. Nothing was downloaded or changed.";
constexpr std::string_view wrong_file_detail =
    "The download was not the file the signed list names, so it was deleted.";
constexpr std::string_view download_stopped_status = "The download stopped";
constexpr std::string_view not_installed_status = "Not installed. {version} is unchanged.";
constexpr std::string_view not_installed_detail = "It stopped before any file moved.";
constexpr std::string_view cancelled_status = "The update was cancelled.";
constexpr std::string_view nothing_changed_detail = "Nothing was changed.";
constexpr std::string_view failed_restored_status = "The update failed. {version} was put back.";
constexpr std::string_view waits_detail = "{version} waits until you choose to try again.";
constexpr std::string_view recovery_status = "Recovery could not finish.";
constexpr std::string_view recovery_detail =
    "Some files were left half-moved. Show details lists the steps that put the game right.";
constexpr std::string_view roll_back_failed_status = "The roll back failed. Nothing was changed.";
constexpr std::string_view roll_back_incomplete_status =
    "The roll back failed. The game could not be put back.";

constexpr std::string_view check_now_caption = "Check now";
constexpr std::string_view download_caption = "Download update";
constexpr std::string_view pause_caption = "Pause";
constexpr std::string_view resume_caption = "Resume";
constexpr std::string_view cancel_download_caption = "Cancel download";
constexpr std::string_view install_on_quit_caption = "Install when I quit";
constexpr std::string_view install_restart_caption = "Install and restart";
constexpr std::string_view windows_asks_note = "Windows asks for permission.";
constexpr std::string_view open_installer_caption = "Open the installer";
constexpr std::string_view applications_note =
    "Start Open Annihilation from Applications afterwards.";
constexpr std::string_view close_game_caption = "Close the game";
constexpr std::string_view next_start_note = "It updates as it next starts.";
constexpr std::string_view keep_ready_caption = "Keep it ready";
constexpr std::string_view keep_ready_note = "Nothing is scheduled.";
constexpr std::string_view unschedule_caption = "Don't install when I quit";
constexpr std::string_view skip_caption = "Skip this version";
constexpr std::string_view later_caption = "Later";
constexpr std::string_view try_version_caption = "Try this version";
constexpr std::string_view retry_caption = "Try again";
constexpr std::string_view details_caption = "Show details";
constexpr std::string_view show_log_caption = "Show the log";
constexpr std::string_view open_page_caption = "Open the release page";
constexpr std::string_view roll_back_caption = "Roll back…";
constexpr std::string_view whats_new_caption = "What's new";

constexpr std::string_view under_a_minute = "less than a minute left";
constexpr std::string_view about_a_minute = "about a minute left";
constexpr std::string_view about_minutes = "about {minutes} minutes left";
constexpr std::string_view over_an_hour = "more than an hour left";

constexpr std::string_view step_wait_done = "Waited for the game to close";
constexpr std::string_view step_wait_failed = "The game did not close: {detail}";
constexpr std::string_view step_verify_done = "Checked the package against the signed list";
constexpr std::string_view step_verify_failed = "The package could not be checked: {detail}";
constexpr std::string_view step_unpack_done = "Unpacked the package";
constexpr std::string_view step_unpack_failed = "The package could not be unpacked: {detail}";
constexpr std::string_view step_swap_done = "Put {to} in place, keeping {from}";
constexpr std::string_view step_swap_failed = "Could not put the new files in place: {detail}";
constexpr std::string_view step_installer_done = "Ran the installer";
constexpr std::string_view step_installer_failed = "The installer did not finish: {detail}";
constexpr std::string_view step_start_done = "Started {version}";
constexpr std::string_view step_start_failed = "Could not start the game: {detail}";
constexpr std::string_view step_watch_done = "{version} reached the main menu";
constexpr std::string_view step_watch_failed = "{version} closed twice before the main menu";
constexpr std::string_view step_undo_done = "Put every changed file back";
constexpr std::string_view step_undo_failed = "Could not put every file back: {detail}";
constexpr std::string_view step_restore_done = "Put {version} back";
constexpr std::string_view step_restore_failed = "Could not put {version} back: {detail}";

constexpr std::string_view recovery_step_log = "Open the log to see which files were left.";
constexpr std::string_view recovery_step_install =
    "Install {version} again from its page, over the copy you have.";
constexpr std::string_view recovery_step_kept =
    "Your saved games, settings and mods are not touched by this.";

/// Seconds in a minute and in an hour, for the time left.
constexpr uint64_t minute_seconds = 60;
constexpr uint64_t hour_seconds = 3600;

/// Tells whether a stage is at rest, so Roll back may be offered beside it.
///
/// @param stage the stage
/// @return true when nothing is under way
bool resting(Stage stage) noexcept {
    switch (stage) {
    case Stage::not_checked:
    case Stage::never:
    case Stage::up_to_date:
    case Stage::updated:
    case Stage::could_not_connect:
    case Stage::could_not_check:
    case Stage::offered:
    case Stage::skipped:
    case Stage::page_only:
    case Stage::managed:
        return true;
    default:
        return false;
    }
}

/// The actions a stage offers, before Roll back.
///
/// @param centre the state, whose stage and way decide
/// @return the actions, the leading one first
std::vector<Act> stage_actions(const Centre& centre) {
    switch (centre.stage) {
    case Stage::not_checked:
    case Stage::up_to_date:
    case Stage::could_not_check:
    case Stage::managed:
        return {Act::check_now};
    case Stage::never:
    case Stage::checking:
    case Stage::verifying:
    case Stage::installing:
    case Stage::confirming:
        return {};
    case Stage::offered:
        if (centre.way == InstallWay::page_only)
            return {Act::open_page, Act::later, Act::skip};
        return {Act::download, Act::later, Act::skip};
    case Stage::page_only:
        return {Act::open_page, Act::later, Act::skip};
    case Stage::skipped:
        return {Act::try_version, Act::check_now};
    case Stage::downloading:
        return {Act::pause, Act::cancel_download};
    case Stage::paused_by_player:
        return {Act::resume, Act::cancel_download};
    case Stage::paused_by_match:
        return {Act::cancel_download};
    case Stage::ready:
        return {Act::install_on_quit, Act::install_now, Act::keep_ready};
    case Stage::scheduled:
        return {Act::unschedule, Act::install_now};
    case Stage::updated:
        return {Act::whats_new};
    case Stage::rolled_back:
        return {Act::show_log};
    case Stage::could_not_connect:
        return {Act::retry};
    case Stage::not_verified:
        return {Act::details};
    case Stage::download_stopped:
        return {Act::retry, Act::cancel_download};
    case Stage::not_installed:
    case Stage::install_cancelled:
    case Stage::failed_restored:
        return {Act::retry, Act::details};
    case Stage::recovery_incomplete:
        return {Act::details, Act::show_log};
    }
    return {};
}

/// The stage a receipt's failure shows.
///
/// @param outcome the receipt's outcome
/// @return the stage; a run left running was cut short, so its recovery did not finish
Stage receipt_stage(Outcome outcome) noexcept {
    switch (outcome) {
    case Outcome::not_installed:
        return Stage::not_installed;
    case Outcome::cancelled:
        return Stage::install_cancelled;
    case Outcome::failed_restored:
    case Outcome::roll_back_failed:
        return Stage::failed_restored;
    case Outcome::running:
    case Outcome::recovery_incomplete:
    case Outcome::roll_back_incomplete:
        return Stage::recovery_incomplete;
    case Outcome::installed:
    case Outcome::installs_next_start:
    case Outcome::opened_installer:
    case Outcome::restart_windows:
    case Outcome::rolled_back:
        return Stage::not_installed;
    }
    return Stage::not_installed;
}

/// Fills the download's bytes, share and time left.
///
/// @param[in,out] centre the state
/// @param inputs what the runtime knows
void fill_bytes(Centre& centre, const CentreInputs& inputs) {
    centre.received = inputs.received;
    centre.total = inputs.total;
    centre.percent = download_percent(inputs.received, inputs.total);
    if (centre.stage == Stage::downloading) {
        centre.bytes_per_second = inputs.bytes_per_second;
        centre.seconds_left =
            download_seconds_left(inputs.received, inputs.total, inputs.bytes_per_second);
    }
}

/// The stage of a download of the release that is out, or nothing when the
/// download shows nothing.
///
/// @param inputs what the runtime knows
/// @return the stage
std::optional<Stage> download_stage(const CentreInputs& inputs) {
    switch (inputs.download) {
    case DownloadState::waiting:
        return std::nullopt;
    case DownloadState::downloading:
        return Stage::downloading;
    case DownloadState::paused_by_player:
        return Stage::paused_by_player;
    case DownloadState::paused_for_match:
        return Stage::paused_by_match;
    case DownloadState::verifying:
        return Stage::verifying;
    case DownloadState::ready:
        if (inputs.scheduled.has_value() && *inputs.scheduled == inputs.download_release)
            return Stage::scheduled;
        return Stage::ready;
    case DownloadState::failed:
        switch (inputs.download_failure) {
        case DownloadFailure::connect:
            return Stage::could_not_connect;
        case DownloadFailure::wrong_file:
            return Stage::not_verified;
        case DownloadFailure::storage:
        case DownloadFailure::unusable:
        case DownloadFailure::none:
            return Stage::download_stopped;
        case DownloadFailure::cancelled:
            return std::nullopt;
        }
        return Stage::download_stopped;
    }
    return std::nullopt;
}

/// The stage the check alone gives.
///
/// @param inputs what the runtime knows
/// @return the stage
Stage check_stage(const CentreInputs& inputs) {
    switch (inputs.check) {
    case CheckState::checking:
        return Stage::checking;
    case CheckState::failed:
        if (inputs.check_failure == CheckFailure::connect)
            return Stage::could_not_connect;
        if (inputs.check_failure == CheckFailure::refused)
            return Stage::not_verified;
        return Stage::could_not_check;
    case CheckState::offered:
        if (inputs.offer_version.empty())
            break;
        if (inputs.way == InstallWay::package_manager)
            return Stage::managed;
        if (inputs.skipped.has_value() && *inputs.skipped >= inputs.offer_release)
            return Stage::skipped;
        if (inputs.way == InstallWay::page_only)
            return Stage::page_only;
        return Stage::offered;
    case CheckState::up_to_date:
    case CheckState::idle:
    case CheckState::cancelled:
        break;
    }
    if (!inputs.updated_from.empty())
        return Stage::updated;
    if (inputs.check == CheckState::up_to_date || inputs.check == CheckState::offered)
        return Stage::up_to_date;
    if (inputs.mode == CheckMode::never)
        return Stage::never;
    return Stage::not_checked;
}

/// Adds one action's words.
///
/// @param[out] words the list
/// @param act the action
/// @param caption the button's words, looked up
/// @param note a line beside it, looked up; empty for none
/// @param text how the words are looked up
void add_act(
    std::vector<ActWords>& words,
    Act act,
    std::string_view caption,
    std::string_view note,
    const library::TextHooks& text
) {
    ActWords added;
    added.act = act;
    added.caption = std::string(library::shown(text, caption));
    if (!note.empty())
        added.note = std::string(library::shown(text, note));
    words.push_back(std::move(added));
}

/// The bytes so far, of the whole, and the share: "10.1 of 24.5 MB · 41%".
///
/// @param centre the state
/// @param text how the words are looked up
/// @return the line
std::string progress_line(const Centre& centre, const library::TextHooks& text) {
    return library::filled(
        text,
        progress_detail,
        {{"done", library::size_text(centre.received, text)},
         {"size", library::size_text(centre.total, text)},
         {"percent", std::to_string(centre.percent < 0 ? 0 : centre.percent)}}
    );
}

} // namespace

int32_t download_percent(uint64_t received, uint64_t total) noexcept {
    if (total == 0)
        return -1;
    if (received >= total)
        return 100;
    // Kept within 64 bits: a total past 2^57 bytes is past any package.
    if (received > UINT64_MAX / 100U)
        return static_cast<int32_t>(received / (total / 100U));
    return static_cast<int32_t>(received * 100U / total);
}

std::optional<uint64_t>
download_seconds_left(uint64_t received, uint64_t total, uint64_t bytes_per_second) noexcept {
    if (bytes_per_second == 0 || total == 0)
        return std::nullopt;
    if (received >= total)
        return 0;
    const uint64_t remaining = total - received;
    return (remaining + bytes_per_second - 1) / bytes_per_second;
}

InstallWay install_way(
    bool replaces_itself,
    std::string_view manager,
    std::optional<Route> route,
    bool installer,
    bool page_only,
    bool next_start
) noexcept {
    if (!replaces_itself)
        return manager.empty() ? InstallWay::page_only : InstallWay::package_manager;
    if (page_only || !route.has_value())
        return InstallWay::page_only;
    switch (*route) {
    case Route::windows_installer:
        return InstallWay::windows_installer;
    case Route::macos_bundle:
        return installer ? InstallWay::macos_installer : InstallWay::restart;
    case Route::linux_versions:
        return next_start ? InstallWay::next_start : InstallWay::restart;
    case Route::check_folder:
    case Route::windows_folder:
        return InstallWay::restart;
    }
    return InstallWay::restart;
}

Centre centre_state(const CentreInputs& inputs) {
    Centre centre;
    centre.current_version = inputs.current_version;
    centre.kept_version = inputs.kept_version;
    centre.way = inputs.way;
    centre.manager = inputs.manager;
    const bool offer_known = !inputs.offer_version.empty() && inputs.offer_release > 0;
    const auto about_offer = [&]() {
        if (!offer_known)
            return;
        centre.target_version = inputs.offer_version;
        centre.target_release = inputs.offer_release;
        centre.total = inputs.offer_size;
    };
    const auto about_download = [&]() {
        centre.target_version = inputs.download_version;
        centre.target_release = inputs.download_release;
    };
    const bool held_failure =
        inputs.held.has_value() && (!offer_known || inputs.offer_release <= *inputs.held);
    const bool download_current = inputs.download_release > 0 &&
                                  (!offer_known || inputs.download_release == inputs.offer_release);
    const std::optional<Stage> downloaded =
        download_current ? download_stage(inputs) : std::optional<Stage>{};

    if (inputs.handing_over) {
        centre.stage = Stage::installing;
        about_download();
    } else if (inputs.unconfirmed) {
        centre.stage = Stage::confirming;
        centre.target_version = inputs.current_version;
        centre.came_from = inputs.updated_from;
    } else if (held_failure) {
        const bool matches = inputs.receipt.has_value() &&
                             inputs.receipt->release == *inputs.held &&
                             failed_outcome(inputs.receipt->outcome);
        centre.stage = matches ? receipt_stage(inputs.receipt->outcome) : Stage::not_installed;
        centre.target_release = *inputs.held;
        if (matches && !inputs.receipt->to_version.empty())
            centre.target_version = inputs.receipt->to_version;
        else if (offer_known && inputs.offer_release == *inputs.held)
            centre.target_version = inputs.offer_version;
        if (matches) {
            centre.receipt_action = inputs.receipt->action;
            centre.steps = inputs.receipt->steps;
        }
    } else if (downloaded.has_value()) {
        centre.stage = *downloaded;
        about_download();
        fill_bytes(centre, inputs);
        if (centre.stage == Stage::download_stopped || centre.stage == Stage::not_verified ||
            centre.stage == Stage::could_not_connect)
            centre.reason = inputs.download_reason;
        centre.refused_file = centre.stage == Stage::not_verified;
    } else if (!inputs.rolled_back_from.empty()) {
        centre.stage = Stage::rolled_back;
        centre.target_version = inputs.current_version;
        centre.came_from = inputs.rolled_back_from;
    } else {
        centre.stage = check_stage(inputs);
        if (centre.stage == Stage::updated) {
            centre.target_version = inputs.current_version;
            centre.came_from = inputs.updated_from;
        } else {
            about_offer();
        }
    }
    centre.actions = stage_actions(centre);
    if (!centre.kept_version.empty() && resting(centre.stage))
        centre.actions.push_back(Act::roll_back);
    return centre;
}

std::string time_left_words(uint64_t seconds, const library::TextHooks& text) {
    if (seconds < minute_seconds)
        return std::string(library::shown(text, under_a_minute));
    if (seconds < minute_seconds + minute_seconds / 2)
        return std::string(library::shown(text, about_a_minute));
    if (seconds >= hour_seconds)
        return std::string(library::shown(text, over_an_hour));
    const uint64_t minutes = (seconds + minute_seconds / 2) / minute_seconds;
    return library::filled(text, about_minutes, {{"minutes", std::to_string(minutes)}});
}

std::string step_words(
    const ReceiptStep& step,
    std::string_view from_version,
    std::string_view to_version,
    const library::TextHooks& text
) {
    const bool done = step.result == StepResult::done;
    const std::string_view detail = step.detail;
    switch (step.step) {
    case Step::wait:
        return done ? std::string(library::shown(text, step_wait_done))
                    : library::filled(text, step_wait_failed, {{"detail", detail}});
    case Step::verify:
        return done ? std::string(library::shown(text, step_verify_done))
                    : library::filled(text, step_verify_failed, {{"detail", detail}});
    case Step::unpack:
        return done ? std::string(library::shown(text, step_unpack_done))
                    : library::filled(text, step_unpack_failed, {{"detail", detail}});
    case Step::swap:
        return done ? library::filled(
                          text, step_swap_done, {{"to", to_version}, {"from", from_version}}
                      )
                    : library::filled(text, step_swap_failed, {{"detail", detail}});
    case Step::installer:
        return done ? std::string(library::shown(text, step_installer_done))
                    : library::filled(text, step_installer_failed, {{"detail", detail}});
    case Step::start:
        return done ? library::filled(
                          text, step_start_done, {{"version", detail.empty() ? to_version : detail}}
                      )
                    : library::filled(text, step_start_failed, {{"detail", detail}});
    case Step::watch:
        return library::filled(
            text, done ? step_watch_done : step_watch_failed, {{"version", to_version}}
        );
    case Step::undo:
        return done ? std::string(library::shown(text, step_undo_done))
                    : library::filled(text, step_undo_failed, {{"detail", detail}});
    case Step::restore:
        return done ? library::filled(text, step_restore_done, {{"version", to_version}})
                    : library::filled(
                          text, step_restore_failed, {{"version", to_version}, {"detail", detail}}
                      );
    }
    return std::string(detail);
}

std::vector<std::string> recovery_steps(std::string_view version, const library::TextHooks& text) {
    std::vector<std::string> steps;
    steps.emplace_back(library::shown(text, recovery_step_log));
    steps.push_back(library::filled(text, recovery_step_install, {{"version", version}}));
    steps.emplace_back(library::shown(text, recovery_step_kept));
    return steps;
}

CentreWords centre_words(const Centre& centre, const library::TextHooks& text) {
    CentreWords words;
    const std::string_view target = centre.target_version;
    const auto with_target = [&](std::string_view pattern) {
        return library::filled(text, pattern, {{"version", target}});
    };
    const auto with_current = [&](std::string_view pattern) {
        return library::filled(text, pattern, {{"version", centre.current_version}});
    };
    const bool roll_back = centre.receipt_action == Action::roll_back;
    switch (centre.stage) {
    case Stage::not_checked:
        words.status = std::string(library::shown(text, not_checked_status));
        break;
    case Stage::never:
        words.status = std::string(library::shown(text, never_status));
        break;
    case Stage::checking:
        words.status = std::string(library::shown(text, checking_status));
        break;
    case Stage::up_to_date:
        words.status = std::string(library::shown(text, up_to_date_status));
        break;
    case Stage::offered:
    case Stage::page_only:
        words.status = with_target(is_out_status);
        if (centre.total > 0)
            words.detail = library::filled(
                text,
                you_have_detail,
                {{"current", centre.current_version},
                 {"size", library::size_text(centre.total, text)}}
            );
        words.note = std::string(library::shown(text, stay_note));
        break;
    case Stage::skipped:
        words.status = with_target(skipped_status);
        break;
    case Stage::managed:
        words.status = with_target(is_out_status);
        words.detail = library::filled(text, updated_by_detail, {{"name", centre.manager}});
        break;
    case Stage::downloading:
        words.status = with_target(downloading_status);
        words.detail = progress_line(centre, text);
        if (centre.seconds_left.has_value())
            words.detail += " · " + time_left_words(*centre.seconds_left, text);
        words.note = std::string(library::shown(text, match_note));
        break;
    case Stage::paused_by_player:
        words.status = std::string(library::shown(text, paused_status));
        words.detail = progress_line(centre, text);
        break;
    case Stage::paused_by_match:
        words.status = std::string(library::shown(text, paused_match_status));
        words.detail = progress_line(centre, text);
        words.note = std::string(library::shown(text, match_note));
        break;
    case Stage::verifying:
        words.status = std::string(library::shown(text, verifying_status));
        break;
    case Stage::ready:
    case Stage::scheduled:
        words.status = with_target(centre.stage == Stage::ready ? ready_status : scheduled_status);
        if (centre.total > 0)
            words.detail = library::filled(
                text, ready_detail, {{"size", library::size_text(centre.total, text)}}
            );
        break;
    case Stage::installing:
        words.status = with_target(installing_status);
        break;
    case Stage::confirming:
        words.status = with_target(confirming_status);
        break;
    case Stage::updated:
        words.status = with_target(updated_status);
        if (!centre.came_from.empty())
            words.detail = library::filled(text, updated_detail, {{"from", centre.came_from}});
        if (!centre.kept_version.empty())
            words.note = library::filled(text, kept_detail, {{"version", centre.kept_version}});
        break;
    case Stage::rolled_back:
        words.status = with_target(back_on_status);
        if (!centre.came_from.empty())
            words.detail =
                library::filled(text, not_offered_detail, {{"version", centre.came_from}});
        break;
    case Stage::could_not_connect:
        words.status = std::string(library::shown(text, could_not_connect_status));
        break;
    case Stage::could_not_check:
        words.status = std::string(library::shown(text, could_not_check_status));
        break;
    case Stage::not_verified:
        words.status = std::string(library::shown(text, not_verified_status));
        words.detail = std::string(
            library::shown(text, centre.refused_file ? wrong_file_detail : refused_detail)
        );
        break;
    case Stage::download_stopped:
        words.status = std::string(library::shown(text, download_stopped_status));
        words.detail = centre.reason;
        break;
    case Stage::not_installed:
        words.status = with_current(not_installed_status);
        words.detail = std::string(library::shown(text, not_installed_detail));
        break;
    case Stage::install_cancelled:
        words.status = std::string(library::shown(text, cancelled_status));
        words.detail = std::string(library::shown(text, nothing_changed_detail));
        break;
    case Stage::failed_restored:
        if (roll_back) {
            words.status = std::string(library::shown(text, roll_back_failed_status));
        } else {
            words.status = with_current(failed_restored_status);
            if (!target.empty())
                words.detail = with_target(waits_detail);
        }
        break;
    case Stage::recovery_incomplete:
        words.status = std::string(
            library::shown(text, roll_back ? roll_back_incomplete_status : recovery_status)
        );
        words.detail = std::string(library::shown(text, recovery_detail));
        break;
    }

    switch (centre.stage) {
    case Stage::downloading:
        words.pill = library::filled(
            text,
            moving_pill,
            {{"version", target},
             {"percent", std::to_string(centre.percent < 0 ? 0 : centre.percent)}}
        );
        break;
    case Stage::paused_by_player:
    case Stage::paused_by_match:
        words.pill = with_target(paused_pill);
        break;
    case Stage::verifying:
        words.pill = with_target(verifying_pill);
        break;
    default:
        words.pill = words.status;
        break;
    }

    for (const Act act : centre.actions) {
        switch (act) {
        case Act::check_now:
            add_act(words.actions, act, check_now_caption, {}, text);
            break;
        case Act::download:
            add_act(words.actions, act, download_caption, {}, text);
            break;
        case Act::pause:
            add_act(words.actions, act, pause_caption, {}, text);
            break;
        case Act::resume:
            add_act(words.actions, act, resume_caption, {}, text);
            break;
        case Act::cancel_download:
            add_act(words.actions, act, cancel_download_caption, {}, text);
            break;
        case Act::install_on_quit:
            add_act(words.actions, act, install_on_quit_caption, {}, text);
            break;
        case Act::install_now:
            switch (centre.way) {
            case InstallWay::windows_installer:
                add_act(words.actions, act, install_restart_caption, windows_asks_note, text);
                break;
            case InstallWay::macos_installer:
                add_act(words.actions, act, open_installer_caption, applications_note, text);
                break;
            case InstallWay::next_start:
                add_act(words.actions, act, close_game_caption, next_start_note, text);
                break;
            case InstallWay::restart:
            case InstallWay::package_manager:
            case InstallWay::page_only:
                add_act(words.actions, act, install_restart_caption, {}, text);
                break;
            }
            break;
        case Act::keep_ready:
            add_act(words.actions, act, keep_ready_caption, keep_ready_note, text);
            break;
        case Act::unschedule:
            add_act(words.actions, act, unschedule_caption, {}, text);
            break;
        case Act::skip:
            add_act(words.actions, act, skip_caption, {}, text);
            break;
        case Act::later:
            add_act(words.actions, act, later_caption, {}, text);
            break;
        case Act::try_version:
            add_act(words.actions, act, try_version_caption, {}, text);
            break;
        case Act::retry:
            add_act(words.actions, act, retry_caption, {}, text);
            break;
        case Act::details:
            add_act(words.actions, act, details_caption, {}, text);
            break;
        case Act::show_log:
            add_act(words.actions, act, show_log_caption, {}, text);
            break;
        case Act::open_page:
            add_act(words.actions, act, open_page_caption, {}, text);
            break;
        case Act::roll_back:
            add_act(words.actions, act, roll_back_caption, {}, text);
            break;
        case Act::whats_new:
            add_act(words.actions, act, whats_new_caption, {}, text);
            break;
        }
    }
    return words;
}

} // namespace oa::app::update

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The update centre: the one state the menu pill, Settings › Updates and the
// Library header read, the words each state shows and the actions it
// allows. The runtime gathers what it knows into CentreInputs; centre_state
// decides the state and centre_words writes it. Download and install are
// separate decisions, a skip is kept apart from Later, and every failure
// has words of its own. Nothing here fetches, installs or opens a window.
#pragma once

#include "oa/app/update/check.hpp"
#include "oa/app/update/checker.hpp"
#include "oa/app/update/download.hpp"
#include "oa/app/update/plan.hpp"
#include "oa/app/update/receipt.hpp"
#include "oa/ui/library/text.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app::update {

/// Where the update centre stands.
enum class Stage : uint8_t {
    not_checked,         ///< no check has run, and nothing is known
    never,               ///< Check for updates is Never; no request is made
    checking,            ///< a check is running
    up_to_date,          ///< the feed verified and offers nothing newer
    offered,             ///< a release is out and has not been downloaded
    skipped,             ///< the release that is out was skipped; it stays quiet
    page_only,           ///< a release is out and this copy can only open its page
    managed,             ///< a release is out and a package manager updates this copy
    downloading,         ///< the package is being fetched
    paused_by_player,    ///< the player paused the download; the bytes stay
    paused_by_match,     ///< a match paused the download; it carries on after
    verifying,           ///< the downloaded file is being checked
    ready,               ///< downloaded and checked; nothing is scheduled
    scheduled,           ///< downloaded and checked; it installs when the player quits
    installing,          ///< the game is handing the release to the helper
    confirming,          ///< this start is the new version and has not reached the main menu
    updated,             ///< this start is the new version; `came_from` names the old one
    rolled_back,         ///< this start put the kept version back; `came_from` names the one left
    could_not_connect,   ///< the check or the download got no answer
    could_not_check,     ///< the check stopped for a reason of this machine's own
    not_verified,        ///< the feed's signature, or the downloaded file, was refused
    download_stopped,    ///< the download stopped for a reason of this machine's own
    not_installed,       ///< the helper stopped before any file moved; this version is unchanged
    install_cancelled,   ///< the player declined the system's permission prompt; nothing changed
    failed_restored,     ///< a step failed and the version that was running was put back
    recovery_incomplete, ///< a step failed and putting the old version back did not finish
};

/// How this copy puts a release in place, which names the install action.
enum class InstallWay : uint8_t {
    restart,           ///< the helper swaps the files and starts the game again
    windows_installer, ///< the release's setup runs; Windows asks for permission
    macos_installer,   ///< macOS Installer puts the release in Applications
    next_start,        ///< a Steam Deck: the release is put in place and starts with the next start
    package_manager,   ///< a package manager updates this copy; there is no install action
    page_only,         ///< this copy cannot install it; the release's page opens
};

/// One action the update centre offers.
enum class Act : uint8_t {
    check_now,       ///< look for a release now
    download,        ///< download the release that is out
    pause,           ///< pause the download
    resume,          ///< carry on after a pause
    cancel_download, ///< stop the download and keep nothing half-downloaded
    install_on_quit, ///< install the ready release when the game quits
    install_now,     ///< install the ready release now; the words name the route
    keep_ready,      ///< keep the ready release; schedule nothing
    unschedule,      ///< take back install when I quit
    skip,            ///< stay quiet about this release until a newer one
    later,           ///< ask again at the next start
    try_version,     ///< take back a skip
    retry,           ///< try the check, the download or the install again
    details,         ///< show what happened, step by step
    show_log,        ///< open the folder that holds the log
    open_page,       ///< open the release's page
    roll_back,       ///< put the kept version back
    whats_new,       ///< show the release's notes
};

/// What the runtime knows, gathered for centre_state.
struct CentreInputs {
    std::string current_version;               ///< this build, without a label
    CheckMode mode{CheckMode::automatically};  ///< Check for updates
    CheckState check{CheckState::idle};        ///< where the check stands
    CheckFailure check_failure{CheckFailure::none}; ///< why a failed check failed
    /// The release that is out: its version. Empty when nothing is offered.
    std::string offer_version;
    int64_t offer_release{};    ///< its release number
    uint64_t offer_size{};      ///< the size of this build's package, in bytes
    std::optional<int64_t> skipped; ///< the highest release number the player skipped
    std::optional<int64_t> held;    ///< a release that did not install and waits for Try again
    std::optional<int64_t> scheduled; ///< the release scheduled to install when the game quits
    DownloadState download{DownloadState::waiting}; ///< where the download stands
    DownloadFailure download_failure{DownloadFailure::none}; ///< why it stopped
    std::string download_reason;  ///< the download's own line, in English
    int64_t download_release{};   ///< the release being downloaded; 0 for none
    std::string download_version; ///< that release's version
    uint64_t received{};          ///< bytes downloaded so far
    uint64_t total{};             ///< bytes the package holds
    uint64_t bytes_per_second{};  ///< how fast it moves; 0 when unknown
    bool handing_over{};          ///< the player chose to install now and the game is closing
    InstallWay way{InstallWay::restart}; ///< how this copy puts a release in place
    std::string manager;          ///< who updates this copy, for InstallWay::package_manager
    std::string updated_from;     ///< this start replaced that version; empty otherwise
    std::string rolled_back_from; ///< this start rolled back from that version; empty otherwise
    bool unconfirmed{};           ///< this build is installed and has not reached the main menu
    std::string kept_version;     ///< the version kept for a roll back; empty for none
    /// The helper's last receipt, when one is held. A failure in it decides
    /// the state while its release is held.
    std::optional<Receipt> receipt;
};

/// The update centre's state, as every screen reads it.
struct Centre {
    Stage stage{Stage::not_checked};       ///< where it stands
    std::string current_version;           ///< this build
    std::string target_version;            ///< the release this state is about; empty for none
    int64_t target_release{};              ///< that release's number; 0 for none
    /// The version this start came from (updated) or left (rolled back).
    std::string came_from;
    std::string kept_version;              ///< the version a roll back would put back
    uint64_t received{};                   ///< bytes downloaded so far
    uint64_t total{};                      ///< bytes of the package
    uint64_t bytes_per_second{};           ///< how fast the download moves; 0 when unknown
    int32_t percent{-1};                   ///< 0 to 100 while bytes are known; -1 otherwise
    std::optional<uint64_t> seconds_left;  ///< the time the download still needs, when known
    InstallWay way{InstallWay::restart};   ///< how the release goes in
    std::string manager;                   ///< who updates this copy
    std::string reason;                    ///< a download's own line, in English, when it stopped
    /// The downloaded file was refused, rather than the feed's signature.
    bool refused_file{};
    Action receipt_action{Action::install}; ///< whether a receipt's failure was an install or a roll back
    std::vector<ReceiptStep> steps;        ///< the receipt's steps, for Show details
    std::vector<Act> actions;              ///< in the order a screen shows them; the first leads
};

/// Decides the update centre's state.
///
/// In order: handing over is installing; an unconfirmed install is
/// confirming; a held release whose receipt failed shows that failure; a
/// download of the release that is out shows the download; a roll back this
/// start shows it; then the check: checking, its failures, a skipped,
/// page-only, managed or offered release, and up to date, which this start's
/// update turns into updated. The actions follow the state, and Roll back is
/// added where a version is kept and nothing is under way.
///
/// @param inputs what the runtime knows
/// @return the state
[[nodiscard]] Centre centre_state(const CentreInputs& inputs);

/// The words of one action.
struct ActWords {
    Act act{Act::check_now}; ///< the action
    std::string caption;     ///< the button's words, in title case
    std::string note;        ///< a line beside it, such as Windows asking; empty for none
};

/// The words one state shows.
struct CentreWords {
    std::string status; ///< the heading, such as "Downloading 0.8.8"
    std::string detail; ///< the line under it, such as "10.1 of 24.5 MB · 41% · about a minute left"
    std::string pill;   ///< the short form the menu pill and a header show
    std::string note;   ///< a line of guidance, such as the match pause; empty for none
    std::vector<ActWords> actions; ///< in the order of Centre::actions
};

/// Writes the words of one state.
///
/// Every pattern is looked up through `text`, then filled. Versions, sizes
/// and the manager are not looked up again. Sizes are size_text's, the
/// Library's, so the offer, the download and Settings name one figure.
///
/// @param centre the state
/// @param text how the words are looked up
/// @return the words
[[nodiscard]] CentreWords
centre_words(const Centre& centre, const oa::ui::library::TextHooks& text);

/// The words of one receipt step, as Show details lists it.
///
/// @param step the step
/// @param from_version the version that was running
/// @param to_version the version being put in place; may be empty
/// @param text how the words are looked up
/// @return the line; a failed step ends with its detail
[[nodiscard]] std::string step_words(
    const ReceiptStep& step,
    std::string_view from_version,
    std::string_view to_version,
    const oa::ui::library::TextHooks& text
);

/// The steps that put an install right when recovery could not finish.
///
/// @param version the version that was running before the update
/// @param text how the words are looked up
/// @return the steps, in order
[[nodiscard]] std::vector<std::string>
recovery_steps(std::string_view version, const oa::ui::library::TextHooks& text);

/// The words of the time a download still needs.
///
/// Under a minute, about a minute, about N minutes up to an hour, and more
/// than an hour.
///
/// @param seconds the time left
/// @param text how the words are looked up
/// @return the words
[[nodiscard]] std::string
time_left_words(uint64_t seconds, const oa::ui::library::TextHooks& text);

/// How this copy puts a release in place.
///
/// A copy that cannot replace itself is updated by its package manager, or
/// opens the page when nobody is named. A release offered on its page, and an
/// install with no route, open the page. A Steam Deck installs at the next
/// start. The Windows installer and the macOS Installer are their own ways.
/// Every other route restarts.
///
/// @param replaces_itself this build may replace itself
/// @param manager who updates this copy; empty names nobody
/// @param route the route this install may use, when it has one
/// @param installer the route runs or opens the system's installer
/// @param page_only the release is offered on its page
/// @param next_start this machine installs at the next start (a Steam Deck or Game Mode)
/// @return the way
[[nodiscard]] InstallWay install_way(
    bool replaces_itself,
    std::string_view manager,
    std::optional<Route> route,
    bool installer,
    bool page_only,
    bool next_start
) noexcept;

/// The share of a download that has arrived.
///
/// @param received bytes so far
/// @param total bytes of the file
/// @return 0 to 100, rounded down; -1 when the total is unknown
[[nodiscard]] int32_t download_percent(uint64_t received, uint64_t total) noexcept;

/// The time a download still needs at its rate.
///
/// @param received bytes so far
/// @param total bytes of the file
/// @param bytes_per_second the rate
/// @return seconds, rounded up; nothing when the rate or the total is unknown
[[nodiscard]] std::optional<uint64_t>
download_seconds_left(uint64_t received, uint64_t total, uint64_t bytes_per_second) noexcept;

} // namespace oa::app::update

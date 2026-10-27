// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The receipt the update helper leaves for the game: each step it took and
// how it went, the outcome, the versions and the times. The helper rewrites
// it after every step, so a run that is cut short still leaves what it had
// done. The game reads it on its next start and shows it once. The bytes are
// JSON. Nothing here moves a file of the install.
#pragma once

#include "oa/app/update/plan.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app::update {

/// The receipt format this build reads and writes.
inline constexpr int64_t receipt_format = 1;

/// The receipt's name in the data folder's updates directory.
inline constexpr std::string_view receipt_file_name = "receipt.json";

/// The most steps one receipt holds. A run that takes more keeps the first ones.
inline constexpr std::size_t most_receipt_steps = 64;

/// The most bytes of one step's detail. A longer detail is cut.
inline constexpr std::size_t most_step_detail_bytes = 400;

/// How a helper run ended.
enum class Outcome : uint8_t {
    running,              ///< the helper has not finished; left so, the run was cut short
    installed,            ///< the new version is in place and was started
    installs_next_start,  ///< the new version is in place and starts the next time the game does
    opened_installer,     ///< the system's installer was opened; it puts the release in place
    restart_windows,      ///< Windows finishes the install when it next starts
    not_installed,        ///< stopped before any file moved; the version that was running is unchanged
    cancelled,            ///< the player declined the system's permission prompt; nothing changed
    failed_restored,      ///< a step failed after files changed, and every change was undone
    recovery_incomplete,  ///< a step failed, and undoing the changes did not finish
    rolled_back,          ///< the kept version was put back
    roll_back_failed,     ///< a roll back failed and the version that was running stayed
    roll_back_incomplete, ///< a roll back failed, and undoing its changes did not finish
};

/// One step the helper takes.
enum class Step : uint8_t {
    wait,      ///< waited for the game to close
    verify,    ///< checked the package against the signed feed
    unpack,    ///< unpacked the package into the staging folder
    swap,      ///< put the new files in place, keeping the old ones
    installer, ///< ran or opened the system's installer
    start,     ///< started the version that is now in place
    watch,     ///< watched that version reach the main menu
    undo,      ///< put back every file a failed step had changed
    restore,   ///< put the kept version back (a roll back)
};

/// How one step went.
enum class StepResult : uint8_t {
    done,   ///< it finished
    failed, ///< it failed; `detail` says why
};

/// One step of a receipt.
struct ReceiptStep {
    Step step{Step::wait};                  ///< which step
    StepResult result{StepResult::done};    ///< how it went
    std::string detail;                     ///< what happened, in English; may be empty
};

/// What the helper did, as the game shows it.
struct Receipt {
    int64_t receipt{receipt_format};          ///< the format, 1
    Action action{Action::install};           ///< an install, or a roll back
    RollBackReason reason{RollBackReason::none}; ///< why a roll back ran; none for an install
    Outcome outcome{Outcome::running};        ///< how the run ended
    std::string from_version;                 ///< the version that was running
    /// The version being put in place: the release for an install, the kept
    /// version for a roll back. Empty when it was never learned.
    std::string to_version;
    int64_t release{};                        ///< the release number the run was for
    int64_t started_at{};                     ///< seconds since 1970
    int64_t finished_at{};                    ///< seconds since 1970; 0 while running
    std::vector<ReceiptStep> steps;           ///< in the order they ran
    /// The game has shown this receipt. It shows a receipt once.
    bool shown{};
};

/// The outcome as the receipt writes it.
///
/// @param outcome the outcome
/// @return its name, such as `failed-restored`; empty for a value this build does not know
[[nodiscard]] const char* outcome_name(Outcome outcome) noexcept;

/// Reads an outcome name.
///
/// @param text the name
/// @return the outcome, or nothing for any other text
[[nodiscard]] std::optional<Outcome> parse_outcome(std::string_view text) noexcept;

/// The step as the receipt writes it.
///
/// @param step the step
/// @return its name, such as `verify`; empty for a value this build does not know
[[nodiscard]] const char* step_name(Step step) noexcept;

/// Reads a step name.
///
/// @param text the name
/// @return the step, or nothing for any other text
[[nodiscard]] std::optional<Step> parse_step(std::string_view text) noexcept;

/// Tells whether an outcome is a failure the player is told about.
///
/// @param outcome the outcome
/// @return true for not installed, cancelled, the failed and incomplete
///     outcomes and a run left running
[[nodiscard]] bool failed_outcome(Outcome outcome) noexcept;

/// Tells whether an outcome holds the release until the player tries it
/// again, so it is not offered or installed by itself.
///
/// @param outcome the outcome
/// @return true for the outcomes of an install that did not put the release in place
[[nodiscard]] bool holds_release(Outcome outcome) noexcept;

/// The outcome a run that stopped with a failed step leaves.
///
/// Nothing changed gives not installed, or the roll back's failure that
/// changed nothing. Changes that were all undone give failed restored, or
/// roll back failed. An undo that did not finish gives the incomplete outcome.
///
/// @param action an install, or a roll back
/// @param files_changed a file of the install had been moved or written
/// @param undo_finished every change was undone
/// @return the outcome
[[nodiscard]] Outcome
failure_outcome(Action action, bool files_changed, bool undo_finished) noexcept;

/// Reads a receipt.
///
/// The bytes are one JSON object of format 1, at most max_plan_bytes. A
/// member this build does not know is ignored. A member named twice, an
/// unknown outcome or step, and more than most_receipt_steps steps refuse it.
///
/// @param bytes the receipt's bytes
/// @param[out] error why the bytes were refused; may be null
/// @return the receipt, or nothing when the bytes are not one
[[nodiscard]] std::optional<Receipt> read_receipt(std::span<const uint8_t> bytes, std::string* error);

/// Writes a receipt in a compact form.
///
/// Members are in the order the format shows them. A detail longer than
/// most_step_detail_bytes is cut, and steps past most_receipt_steps are left out.
///
/// @param receipt the receipt
/// @return the JSON text, with a trailing line end
[[nodiscard]] std::string receipt_text(const Receipt& receipt);

/// Replaces a receipt file whole.
///
/// The new bytes are written beside the file and then take its place, so a
/// reader sees the previous receipt or the whole new one.
///
/// @param path the file; its folder must already exist
/// @param receipt the receipt
/// @param[out] error why the file was not replaced; may be null
/// @return true when the file was replaced
[[nodiscard]] bool
write_receipt(const std::filesystem::path& path, const Receipt& receipt, std::string* error);

/// Reads a receipt file.
///
/// @param path the file
/// @return the receipt, or nothing when it is missing, too large or refused
[[nodiscard]] std::optional<Receipt> load_receipt(const std::filesystem::path& path);

} // namespace oa::app::update

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The helper's log: one line a step, appended, kept under a mebibyte by
// dropping the first half. Beside it, the receipt the game reads on its next
// start: each step and how it went, rewritten after every step, and the
// outcome, written before any version of the game is started.
#pragma once

#include "oa/app/update/receipt.hpp"

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace oa::update_helper {

/// An append-only log under the updates directory.
class Log {
  public:

    /// Opens `<updates>/oa-update.log`, creating it when it is missing.
    ///
    /// @param updates the data folder's updates directory
    /// @param[out] error why the log could not be opened; may be null
    /// @return true when lines can be written
    [[nodiscard]] bool open(const std::filesystem::path& updates, std::string* error);

    /// Appends one line. A newline in `text` becomes a space.
    ///
    /// @param text the line, without a line end
    void line(std::string_view text);

    /// The last line appended, or empty.
    ///
    /// @return the line
    [[nodiscard]] const std::string& last_line() const noexcept { return last_line_; }

    /// Starts the receipt, `<updates>/receipt.json`, and writes it at once.
    ///
    /// The outcome is running until finish names one, and the start time is
    /// the clock now. A receipt that cannot be written is noted in the log
    /// once, and the run carries on.
    ///
    /// @param updates the data folder's updates directory
    /// @param receipt the action, the versions and the release; no steps
    void begin_receipt(const std::filesystem::path& updates, oa::app::update::Receipt receipt);

    /// Records one step in the receipt and writes it. Without a receipt it does nothing.
    ///
    /// @param step the step
    /// @param result how it went
    /// @param detail what happened, in English; empty for nothing more
    void step(
        oa::app::update::Step step, oa::app::update::StepResult result, std::string_view detail = {}
    );

    /// Records the version being put in place, once it is known.
    ///
    /// @param version the version
    void set_to_version(std::string_view version);

    /// Names the outcome and writes the receipt, with the time now.
    ///
    /// A later call names a later outcome: an install that started the new
    /// version is installed, and a roll back after its failed starts
    /// replaces that.
    ///
    /// @param outcome the outcome
    void finish(oa::app::update::Outcome outcome);

    /// Tells whether a receipt was begun and has no outcome yet.
    ///
    /// @return true while the outcome is running
    [[nodiscard]] bool receipt_running() const noexcept;

    /// The receipt's action, or an install when none was begun.
    ///
    /// @return the action
    [[nodiscard]] oa::app::update::Action receipt_action() const noexcept;

  private:

    /// Drops the first half once the file is over a mebibyte.
    void trim();

    /// Writes the receipt, noting a failure in the log once.
    void write_receipt();

    std::filesystem::path path_{};
    bool open_{};
    std::string last_line_{};                         ///< the last line appended
    std::optional<oa::app::update::Receipt> receipt_; ///< the receipt; empty until begun
    std::filesystem::path receipt_path_{};            ///< where it is written
    bool receipt_failed_{};                           ///< a write failed and was noted
};

} // namespace oa::update_helper

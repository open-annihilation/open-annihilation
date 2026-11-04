// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One entry into an expansion, as an extension asks for it. The sentences
// are already in the language the player is reading. The header names no
// other header of the engine: an extension that includes it links
// oa::app::headers and needs no more of the engine's.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace oa::data::languages {
struct Language;
}

namespace oa::app {

class Runtime;

/// Where one entry into an expansion stands.
enum class ExpansionEntryStep : uint8_t {
    ready,   ///< the expansion is on, and the caller may open it
    waiting, ///< a check or a download is still running
    asking,  ///< a question is waiting for an answer
    stayed,  ///< the player stayed on the menu
    failed,  ///< the entry stopped, and the notice says why
};

/// What the caller asks when an entry begins.
///
/// The sentences are already in the language the player is reading. An empty
/// registry uses the first catalogue that lists the package.
struct ExpansionEntryAsk {
    std::string registry{};     ///< registry id; empty searches every catalogue
    std::string key{};          ///< the package id
    std::string question{};     ///< the update question
    std::string update_label{}; ///< the button that takes the update
    std::string later_label{};  ///< the button that stays on the menu
    std::string ok_label{};     ///< the button that closes a failure
    std::string progress{};     ///< the progress line; "{percent}" is replaced
    /// When true, a check that has not answered does not hold the entry. A
    /// result already in hand is still taken, and a missing package is still
    /// fetched. An offer already in hand is declined so the entry can go on.
    /// A player still reads the services setting and the game files. When
    /// false, the entry waits for the menu to settle and for the check to
    /// answer, and that wait ends on its own if the check does not.
    bool skip_unanswered_check{false};
    /// When true, a fetch or a failure is reported on the caller's screen
    /// instead of a question on the menu. When the caller cannot show that
    /// screen, it turns this off and the question is used.
    bool report_on_screen{false};
};

/// What an entry is doing now.
///
/// The flags say which checks ran. A flag left clear means that check was
/// not performed, and a caller must not describe it as done.
struct ExpansionEntryStatus {
    ExpansionEntryStep step{ExpansionEntryStep::waiting}; ///< where it stands
    std::string notice{};       ///< the question, the progress line or the failure
    bool services_known{false}; ///< the update and catalogue settings were read
    bool services_on{false};    ///< update and catalogue checks are not both off
    bool files_known{false};    ///< the start's game-files check was read
    /// The first file of another bundle that differs. Empty when none does.
    std::string mismatched_file{};
    /// Files the check named as matching. Empty when the check only reports
    /// that nothing differed.
    std::vector<std::string> matched_files{};
    bool feed_known{false};        ///< a check of the update feed produced a result
    bool feed_failed{false};       ///< that check failed
    bool update_offered{false};    ///< the feed offers a newer release
    std::string offered_version{}; ///< the offered release's version text
    bool update_declined{false};   ///< the player went on without the update
    bool extension_known{false};   ///< the catalogue's package was considered
    /// The fetch's percent, 0 to 100. -1 when no fetch is showing.
    int32_t extension_percent{-1};
    bool extension_installed{false}; ///< this entry downloaded and installed it
    bool extension_current{false};   ///< the installed package matches the catalogue
    std::string extension_version{}; ///< the catalogue package's version
    /// The entry stopped before any later check. Nothing was contacted after.
    bool stopped{false};
};

/// Starts an entry: the update check, then the package when it is not
/// already in the game folder.
///
/// A call while a check or a download from this entry is running does
/// nothing. The sentences in `ask` are shown as given.
///
/// @param runtime the running game
/// @param ask the package and the sentences
void begin_expansion_entry(Runtime& runtime, const ExpansionEntryAsk& ask);

/// Moves an entry on by one step.
///
/// @param runtime the running game
/// @return where the entry stands
[[nodiscard]] ExpansionEntryStatus advance_expansion_entry(Runtime& runtime);

/// Answers the update question.
///
/// Update starts the download the update centre offers and stays on the
/// menu. Not now, for a player, goes on to the package and does not
/// remember a skip. A headless or unattended run stays on the menu. A call
/// while that question is not open does nothing.
///
/// @param runtime the running game
/// @param take_update true to download the offered release
void answer_expansion_entry(Runtime& runtime, bool take_update);

/// Reports whether an entry is in the middle of a check or a download.
///
/// @param runtime the running game
/// @return true while a new entry would be ignored
[[nodiscard]] bool expansion_entry_busy(const Runtime& runtime);

/// Tells a running entry whether to draw its fetch on the caller's screen.
///
/// @param runtime the running game
/// @param on_screen true to leave the menu question closed
void expansion_entry_show_on_screen(Runtime& runtime, bool on_screen);

/// Returns the language the game shows its text in.
///
/// @param runtime the running game
/// @return the language; English before the language has started
[[nodiscard]] const oa::data::languages::Language& expansion_entry_language(const Runtime& runtime);

} // namespace oa::app

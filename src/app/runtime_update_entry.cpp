// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One entry into an expansion, in this order. The online features: when the
// update check or the catalogue check is off, a player is asked to turn
// them on. The latest Open Annihilation: the entry waits for the update
// check's answer, a newer release is taken (downloaded, installed and
// started again, with the one-shot entry carried) or the player stays, and
// a failed check asks Try again. The mod: switched off as the mod question
// says. The latest package: the expansion's record names the package it was
// installed from, and the catalogue's package is fetched only when it is
// another, the copy is missing, or the record names none. The package is
// mounted where the player is, and a first install that cannot be put in
// use restarts once. The caller opens the expansion once this reports
// ready. A quiet run asks nothing about the online features or the update,
// changes no setting, and logs what a player would have been asked.
#include "oa/app/runtime.hpp"

#include "engine_settings_state.hpp"
#include "package_paths.hpp"
#include "oa_layer.hpp"
#include "update_state.hpp"

#include "oa/app/content/downloads.hpp"
#include "oa/app/content/settings.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/update/build.hpp"
#include "oa/app/update/centre.hpp"
#include "oa/app/update/check.hpp"
#include "oa/base/sha256.hpp"
#include "oa/data/content_bundles.hpp"
#include "oa/platform/files.hpp"
#include "oa/data/content_bundles/content_choice.hpp"
#include "oa/data/content_bundles/record.hpp"
#include "oa/data/registry/trust.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/ui/engine_settings.hpp"
#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/library/text.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <iostream>
#include <optional>
#include <ostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

/// How many polls a finished download may stay invisible before the entry stops.
constexpr int k_entry_polls = 30;

/// How long a check may stay unanswered before the entry records that and goes on.
///
/// A download that reports a new percent starts this wait again. The wait is
/// what stops a feed, a catalogue refresh or a fetch from sitting forever.
constexpr int64_t k_unanswered_ms = 30000;

/// The sentence a missing catalogue uses, before its first letter is raised.
constexpr std::string_view k_no_package = "the catalogue has no such package";

/// The sentence a package that did not install uses.
constexpr std::string_view k_not_installed = "The package could not be installed.";

/// What a quiet run's log line starts with, before the sentence a player would read.
constexpr std::string_view k_would_ask = "open-annihilation: entry: a player would be asked: ";

// The two keys the online features are made of, as expansion_entry.hpp names them.
static_assert(oa::app::kUpdateCheckKey == oa::ui::engine_settings::key::update_check);
static_assert(oa::app::kCatalogueCheckKey == oa::ui::engine_settings::key::content_check);

/// Returns one of the interface's words in the language shown.
///
/// @param english the word, in English
/// @return its translation, or the English
std::string_view shown_word(void* /*context*/, std::string_view english) {
    return oa::data::languages::interface_text(english);
}

/// The fields of a catalogue's package the entry reads.
///
/// They are copied out of the snapshot that lists them: the catalogue
/// worker can publish a new snapshot at any time, which frees the old one's
/// catalogues.
struct ListedPackage {
    std::string registry{};            ///< the registry that lists it
    bool built_in{false};              ///< that registry is one the game ships with
    std::string version{};             ///< the package's version text
    int64_t release{0};                ///< the package's release
    oa::base::sha256::Digest sha256{}; ///< the whole package file's SHA-256
};

/// The record's word for a copy, as the entry's decisions read it.
///
/// @param check what check_installed_package found
/// @return the same state
CopyState copy_state(oa::data::content_bundles::PackageCheck check) noexcept {
    switch (check) {
    case oa::data::content_bundles::PackageCheck::current:
        return CopyState::current;
    case oa::data::content_bundles::PackageCheck::unrecorded:
        return CopyState::unrecorded;
    case oa::data::content_bundles::PackageCheck::changed:
        return CopyState::changed;
    case oa::data::content_bundles::PackageCheck::missing:
        break;
    }
    return CopyState::missing;
}

/// Tells whether the update centre's stage is a failure of the download or
/// the install.
///
/// @param stage the stage
/// @return true for a stage the entry stops at
bool update_failed(update::Stage stage) noexcept {
    switch (stage) {
    case update::Stage::could_not_connect:
    case update::Stage::could_not_check:
    case update::Stage::not_verified:
    case update::Stage::download_stopped:
    case update::Stage::not_installed:
    case update::Stage::install_cancelled:
    case update::Stage::failed_restored:
    case update::Stage::recovery_incomplete:
        return true;
    default:
        return false;
    }
}

/// Raises an ASCII first letter. Any other text is left as it is.
///
/// @param text the sentence
/// @return the sentence, with a small first letter raised
std::string capitalise(std::string_view text) {
    if (text.empty())
        return {};
    const auto first = static_cast<unsigned char>(text.front());
    const char raised =
        first >= 'a' && first <= 'z' ? static_cast<char>(first - 'a' + 'A') : text.front();
    std::string result(1, raised);
    result.append(text.substr(1));
    return result;
}

/// Reports whether `ids` holds `package_id`.
///
/// @param ids the ids
/// @param package_id the id
/// @return true when one of them is `package_id`
bool holds_id(const std::vector<std::string>& ids, std::string_view package_id) {
    for (const std::string& id : ids) {
        if (id == package_id)
            return true;
    }
    return false;
}

/// Reports whether a mod folder is remembered.
///
/// @param values the preferences
/// @return true when the mod key names a folder
bool mod_remembered(const oa::platform::preferences::Values& values) {
    const auto found = values.find(std::string(oa::ui::engine_settings::key::mod_directory));
    return found != values.end() && !found->second.empty();
}

/// The suffix of an archive the game mounts, in lower case, or empty.
///
/// @param path the file
/// @return `.ccx`, `.hpi`, `.ufo` or `.gp3`, or empty
std::string archive_suffix(const fs::path& path) {
    std::string suffix = oa::platform::path_to_utf8(path.extension());
    for (char& character : suffix) {
        if (character >= 'A' && character <= 'Z')
            character = static_cast<char>(character - 'A' + 'a');
    }
    if (suffix == ".ccx" || suffix == ".hpi" || suffix == ".ufo" || suffix == ".gp3")
        return suffix;
    return {};
}

/// Replaces `{percent}` once.
///
/// @param pattern the line, already translated
/// @param percent the whole percent, 0 to 100
/// @return the line to show
std::string fill_percent(std::string_view pattern, int percent) {
    std::string text(pattern);
    constexpr std::string_view token = "{percent}";
    const std::size_t at = text.find(token);
    if (at != std::string::npos)
        text.replace(at, token.size(), std::to_string(percent));
    return text;
}

/// The first archive in an expansion folder, or an empty path.
///
/// @param folder the expansion's folder
/// @return the archive, or empty when the folder has none
fs::path first_archive(const fs::path& folder) {
    std::error_code failure;
    if (!fs::is_directory(folder, failure) || failure)
        return {};
    const fs::directory_iterator end;
    for (fs::directory_iterator cursor(folder, failure); !failure && cursor != end;
         cursor.increment(failure)) {
        std::error_code status;
        if (!cursor->is_regular_file(status) || status)
            continue;
        if (!archive_suffix(cursor->path()).empty())
            return cursor->path();
    }
    return {};
}

/// The whole percent a download is showing, 0 to 100.
///
/// @param item the queued package
/// @return the percent
int download_percent(const content::DownloadView& item) {
    switch (item.state) {
    case content::DownloadState::verifying:
    case content::DownloadState::handing_over:
    case content::DownloadState::installing:
    case content::DownloadState::done:
        return 100;
    default:
        break;
    }
    if (item.target.size == 0 || item.done == 0)
        return 0;
    const uint64_t percent = item.done * 100 / item.target.size;
    if (percent > 100)
        return 100;
    return static_cast<int>(percent);
}

} // namespace

struct Runtime::EntryState {
    /// Where the entry is inside this file. The public step is derived from it.
    enum class Phase : uint8_t {
        idle,         ///< no entry is running
        opening,      ///< the online features and the game files are read next
        services_ask, ///< the question about the online features is up
        update_wait,  ///< the update check has not answered
        update_ask,   ///< the update question is up
        retry_ask,    ///< the failed check's question is up
        updating,     ///< the newer release is downloading
        fetch_wait,   ///< a catalogue refresh is still running
        progress,     ///< the package is being fetched
        stayed,       ///< the player stayed on the menu
        failed,       ///< the entry stopped
        ready,        ///< the package is on
        mod_ask,      ///< a question about switching a mod off
        mod_restart,  ///< the run is ending so the next one can enter
        command_ask,  ///< a notice that the command line named the mod
        leave_ask,    ///< a question about switching a remembered mod on
        restarting,   ///< the run is ending for the update or the new package
    };

    Phase phase{Phase::idle};          ///< where it is
    ExpansionEntryAsk ask{};           ///< the package and the sentences
    bool handling_check{false};        ///< the normal update question stays back
    uint64_t generation{0};            ///< the check generation the entry waits past
    std::string registry_id{};         ///< the catalogue the package was listed in
    bool refresh_asked{false};         ///< a refresh was asked for this entry
    bool download_queued{false};       ///< a download from this entry is in the queue
    uint64_t download_id{0};           ///< that download's id
    int polls{0};                      ///< polls spent waiting for a finished download's files
    int64_t wait_began_ms{0};          ///< when the current unanswered wait started; 0 when none
    QuestionScreen* question{nullptr}; ///< the question on the layer; not owned
    std::string shown_word{};          ///< the word of that question
    std::string notice{};              ///< the question, the progress line or the failure
    bool services_known{false};        ///< the update and catalogue settings were read
    bool services_on{false};           ///< update and catalogue checks are not both off
    bool files_known{false};           ///< the start's game-files check was read
    std::string mismatched_file{};     ///< the first differing file of another bundle
    std::vector<std::string> matched_files{}; ///< files named as matching
    bool feed_known{false};                   ///< a feed check produced a result
    bool feed_failed{false};                  ///< that check failed
    bool update_offered{false};               ///< the feed offers a newer release
    std::string offered_version{};            ///< the offered release's version text
    bool update_declined{false};              ///< a quiet run went on without the update
    std::string running_version{};            ///< this build's version text
    bool extension_known{false};              ///< the catalogue's package was considered
    int32_t extension_percent{-1};            ///< the fetch's percent, or -1
    bool extension_installed{false};          ///< this entry downloaded and installed it
    bool extension_current{false};            ///< the installed package matches
    std::string extension_version{};          ///< the catalogue package's version
    int64_t extension_release{0};             ///< the catalogue package's release
    bool package_checked{false};              ///< the record was compared once
    /// What that compare found.
    oa::data::content_bundles::PackageCheck package_check{
        oa::data::content_bundles::PackageCheck::missing
    };
    bool stopped{false};             ///< a check stopped the entry before any contact
    std::string leave_question{};    ///< the remembered-mod question
    std::string leave_on_label{};    ///< the button that switches it on
    std::string leave_later_label{}; ///< the button that leaves it off
};

struct EntryDriver {
    Runtime& runtime; ///< the runtime this entry belongs to

    /// The entry, which begin has created.
    ///
    /// @return the entry
    Runtime::EntryState& state() const { return *runtime.entry_; }

    /// Closes the question this entry is showing.
    void dismiss() const {
        Runtime::EntryState& current = state();
        QuestionScreen* leaving = current.question;
        if (leaving == nullptr)
            return;
        current.question = nullptr;
        current.shown_word.clear();
        runtime.oa_layer().close(leaving);
    }

    /// Puts one question on the layer, closing the one this entry showed.
    ///
    /// @param word the question's word
    /// @param asked the question
    void put_up(const std::string& word, oa::ui::kit::Question asked) const {
        Runtime::EntryState& current = state();
        dismiss();
        asked.title = "Open Annihilation";
        asked.word = word;
        current.notice = asked.paragraphs.empty() ? std::string() : asked.paragraphs[0].text;
        Runtime* self = &runtime;
        QuestionScreen::Host host;
        host.answer = [self](int32_t button) {
            return Runtime::entry_update_answered(self, button);
        };
        host.tick = [self]() { return Runtime::entry_question_tick(self); };
        host.closed = [self](const QuestionScreen& closing) {
            Runtime::entry_question_closed(self, &closing);
        };
        OaLayer& layer = runtime.oa_layer();
        auto screen = std::make_unique<QuestionScreen>(
            layer, Screen::main_menu, std::move(asked), std::move(host)
        );
        QuestionScreen* taken = screen.get();
        taken->show_on_any_menu();
        layer.show_when_free(std::move(screen));
        current.question = taken;
        current.shown_word = word;
    }

    /// Shows a line with an optional bar, or updates the one already showing
    /// the same word.
    ///
    /// @param word the question's word
    /// @param paragraph the sentence
    /// @param progress the bar, or -1 for none
    /// @param ok_button true for OK
    /// @param cancel_button true for Cancel
    void show(
        const std::string& word,
        const std::string& paragraph,
        int32_t progress,
        bool ok_button,
        bool cancel_button
    ) const {
        Runtime::EntryState& current = state();
        if (current.question != nullptr && current.shown_word == word) {
            oa::ui::kit::Question& asked = current.question->question();
            if (!asked.paragraphs.empty())
                asked.paragraphs[0].text = paragraph;
            asked.progress = progress;
            current.question->changed();
            current.notice = paragraph;
            return;
        }
        oa::ui::kit::Question asked;
        asked.paragraphs.push_back({paragraph, false});
        asked.progress = progress;
        if (ok_button)
            asked.buttons.push_back({current.ask.ok_label, true, "ok"});
        else if (cancel_button)
            asked.buttons.push_back({current.ask.cancel_label, false, "cancel"});
        asked.primary_button = 0;
        asked.cancel_button = 0;
        asked.marked = 0;
        put_up(word, std::move(asked));
    }

    /// Shows a question with two buttons, or updates the one already showing.
    ///
    /// @param word the question's word
    /// @param paragraph the sentence
    /// @param primary the first button
    /// @param cancel the second button
    /// @param primary_id the first button's name
    /// @param cancel_id the second button's name
    void show_pair(
        const std::string& word,
        const std::string& paragraph,
        const std::string& primary,
        const std::string& cancel,
        const std::string& primary_id,
        const std::string& cancel_id
    ) const {
        Runtime::EntryState& current = state();
        if (current.question != nullptr && current.shown_word == word) {
            oa::ui::kit::Question& asked = current.question->question();
            if (!asked.paragraphs.empty())
                asked.paragraphs[0].text = paragraph;
            current.question->changed();
            current.notice = paragraph;
            return;
        }
        oa::ui::kit::Question asked;
        asked.paragraphs.push_back({paragraph, false});
        asked.buttons.push_back({primary, true, primary_id});
        asked.buttons.push_back({cancel, false, cancel_id});
        asked.primary_button = 0;
        asked.cancel_button = 1;
        asked.marked = 0;
        put_up(word, std::move(asked));
    }

    /// Shows one of the questions asked before the package.
    ///
    /// @param which the question
    /// @param version the offered release, for the update question
    void ask_question(EntryQuestion which, std::string_view version) const {
        const EntryQuestionLook look = entry_question(which, state().ask, version);
        show_pair(
            look.word,
            look.paragraph,
            look.primary_label,
            look.cancel_label,
            look.primary_id,
            look.cancel_id
        );
    }

    /// Writes what a player would have been asked to the log.
    ///
    /// @param sentence the question
    void note_question(std::string_view sentence) const {
        std::cerr << k_would_ask << sentence << '\n';
    }

    /// Stops the entry and shows why.
    ///
    /// @param notice the sentence the player reads
    void fail(std::string notice) const {
        Runtime::EntryState& current = state();
        current.phase = Runtime::EntryState::Phase::failed;
        current.handling_check = false;
        current.notice = std::move(notice);
        if (current.ask.report_on_screen)
            return;
        show("expansion-notice", current.notice, -1, true, false);
    }

    /// Stays on the menu. Nothing more is asked.
    void stay() const {
        Runtime::EntryState& current = state();
        current.phase = Runtime::EntryState::Phase::stayed;
        current.handling_check = false;
        current.notice.clear();
        dismiss();
    }

    /// The package is on. The question comes down.
    void succeed() const {
        Runtime::EntryState& current = state();
        current.phase = Runtime::EntryState::Phase::ready;
        current.handling_check = false;
        current.notice.clear();
        // Only a compare or a download this entry finished may be described.
        current.extension_known = current.package_checked || current.download_queued;
        if (current.download_queued)
            current.extension_installed = true;
        else if (
            current.package_checked &&
            current.package_check == oa::data::content_bundles::PackageCheck::current
        )
            current.extension_current = true;
        if (current.extension_version.empty()) {
            if (const std::optional<ListedPackage> package = package_listed()) {
                current.extension_version = package->version;
                current.extension_release = package->release;
            }
        }
        dismiss();
    }

    /// Reports whether nobody can answer this entry.
    ///
    /// @return true for a quiet ask, a headless run and an unattended run
    bool quiet() const { return state().ask.quiet || !facing_player(); }

    /// Who answers this entry, for the decisions in expansion_entry.hpp.
    ///
    /// @return quiet when nobody can answer
    EntryWay way() const { return quiet() ? EntryWay::quiet : EntryWay::player; }

    /// Saves the one-shot entry, or forgets it, and flushes the preferences.
    ///
    /// @param pending true to save it
    void keep_one_shot(bool pending) const {
        runtime.keep_plain_string(kExpansionEnterOnce, pending ? "1" : "");
        try {
            runtime.flush_preferences();
        } catch (const std::exception&) {
            std::cerr << "open-annihilation: the choice could not be saved\n";
        }
    }

    /// Ends the run so the next one enters, once, after a first install
    /// that cannot be put in use where the player is. An entry a restart
    /// already carried stops with `notice` instead.
    ///
    /// @param notice the sentence shown when the run will not restart
    void restart_for_install(const std::string& notice) const {
        Runtime::EntryState& current = state();
        if (current.ask.carried) {
            fail(notice);
            return;
        }
        keep_one_shot(true);
        std::cerr << "open-annihilation: entry: the new package is put in use by starting again\n";
        current.phase = Runtime::EntryState::Phase::restarting;
        current.handling_check = false;
        current.notice.clear();
        dismiss();
        runtime.request_soft_restart();
    }

    /// The folder an installed expansion is mounted from.
    ///
    /// @param bundle the expansion
    /// @return `Expansions/<game>/<id>` under the player's folder
    fs::path expansion_folder(const oa::data::content_bundles::Bundle& bundle) const {
        return runtime.user_folder() /
               std::string(oa::data::content_bundles::managed_expansions_folder) /
               std::string(bundle.extends[0]) / std::string(bundle.id);
    }

    /// Reports whether that folder holds an archive.
    ///
    /// @param folder the expansion's folder
    /// @return true when a regular archive is in it
    bool folder_has_archive(const fs::path& folder) const { return !first_archive(folder).empty(); }

    /// Mounts every archive in the expansion's folder.
    ///
    /// An archive that is already mounted counts. A path is not shown.
    ///
    /// @param folder the expansion's folder
    /// @return true when every archive mounted
    bool mount_archives(const fs::path& folder) const {
        std::error_code failure;
        if (!fs::is_directory(folder, failure) || failure)
            return false;
        int archives = 0;
        int mounted = 0;
        const fs::directory_iterator end;
        for (fs::directory_iterator cursor(folder, failure); !failure && cursor != end;
             cursor.increment(failure)) {
            std::error_code status;
            if (!cursor->is_regular_file(status) || status)
                continue;
            if (archive_suffix(cursor->path()).empty())
                continue;
            ++archives;
            std::string mount_error;
            if (runtime.assets_.try_mount(cursor->path(), &mount_error) ||
                mount_error == "already mounted")
                ++mounted;
        }
        return archives > 0 && mounted == archives;
    }

    /// Turns the expansion on and remembers the choice.
    ///
    /// A run whose command line already named the choice still counts as
    /// ready. The choice is not stored then.
    ///
    /// @param bundle the expansion
    /// @return true when the entry is ready
    bool turn_on(const oa::data::content_bundles::Bundle& bundle) const {
        namespace bundles = oa::data::content_bundles;
        std::vector<std::string> copied = runtime.kept_content_bundles();
        if (!holds_id(copied, bundle.id))
            copied.emplace_back(bundle.id);
        std::vector<std::string_view> views;
        views.reserve(copied.size());
        for (const std::string& id : copied)
            views.emplace_back(id);
        const bundles::ContentChoice now = content_choice_from_preferences(
            runtime.preference_values_, views, runtime.options_.content
        );
        const bundles::ContentChangeResult result = bundles::turn_content_expansion(
            now,
            bundle.id,
            true,
            views,
            runtime.match_running(),
            mod_remembered(runtime.preference_values_),
            std::span<const std::string_view>{}
        );
        if (result.refusal == bundles::ContentRefusal::command_line) {
            succeed();
            return true;
        }
        if (result.refusal == bundles::ContentRefusal::in_match) {
            fail("A match is running.");
            return false;
        }
        if (result.refusal == bundles::ContentRefusal::unknown) {
            fail("That package is not one this game knows.");
            return false;
        }
        if (result.refusal == bundles::ContentRefusal::not_for_game) {
            fail("That package is not for this game.");
            return false;
        }
        if (result.refusal == bundles::ContentRefusal::not_copied) {
            fail(std::string(k_not_installed));
            return false;
        }
        if (!result.applied || !store_content_change(runtime.preference_values_, result))
            remember_content_choice(runtime.preference_values_, result.choice);
        if (result.mod_cleared != bundles::ModClearReason::none && runtime.engine_settings_) {
            runtime.engine_settings_->current.mod_folder.clear();
            runtime.engine_settings_->dropped_mod_folder.clear();
        }
        runtime.preferences_dirty_ = true;
        try {
            runtime.flush_preferences();
        } catch (const std::exception&) {
            std::cerr << "open-annihilation: the choice could not be saved\n";
        }
        succeed();
        return true;
    }

    /// Takes files that are already on disk. A download that is still queued
    /// is not claimed here.
    ///
    /// A copy this entry has just installed whose archives cannot be mounted
    /// where the player is restarts the run once.
    ///
    /// @param bundle the expansion
    /// @return true when the files were claimed, or the entry stopped
    bool claim(const oa::data::content_bundles::Bundle& bundle) const {
        if (holds_id(runtime.kept_content_bundles(), state().ask.key))
            return turn_on(bundle);
        const fs::path user = runtime.user_folder();
        const bool recorded = holds_id(registry_content_bundles(user), state().ask.key) ||
                              holds_id(copied_content_bundles(user), state().ask.key);
        const fs::path folder = expansion_folder(bundle);
        if (!recorded && !folder_has_archive(folder))
            return false;
        if (!mount_archives(folder)) {
            if (state().download_queued)
                restart_for_install(std::string(k_not_installed));
            else
                fail(std::string(k_not_installed));
            return true;
        }
        return turn_on(bundle);
    }

    /// Reports whether any catalogue is still being fetched.
    ///
    /// @return true while a refresh is queued or running
    bool refreshing() const {
        const std::shared_ptr<const content::Snapshot> snapshot =
            runtime.content_service().snapshot();
        if (!snapshot)
            return false;
        for (const content::RegistryView& row : snapshot->registries) {
            if (row.refreshing)
                return true;
        }
        return false;
    }

    /// Asks the catalogues once more, as Check now does: the package a
    /// catalogue listed earlier may no longer be its current one.
    ///
    /// Every entry asks every registry, a quiet one too, so a machine nobody
    /// watches takes a newly published package on its own. One of the
    /// game's own checks asks only the registries the player added
    /// (registry_reached): a check never contacts a built-in registry.
    void ask_catalogues() const {
        const bool own_check = state().ask.own_check;
        if (!own_check) {
            runtime.content_service().refresh(content::RefreshReason::check_now);
            return;
        }
        const std::shared_ptr<const content::Snapshot> snapshot =
            runtime.content_service().snapshot();
        if (!snapshot)
            return;
        for (const content::RegistryView& row : snapshot->registries) {
            const bool built_in = row.registry.origin != oa::data::registry::Origin::added;
            if (!registry_reached(built_in, own_check) || !row.registry.enabled)
                continue;
            runtime.content_service().refresh(
                content::RefreshReason::check_now, row.registry.descriptor.id
            );
        }
    }

    /// Waits for the catalogues' answer about the package.
    ///
    /// Each entry asks once (ask_catalogues) and waits while a catalogue is
    /// fetching, up to the unanswered wait. A catalogue that cannot be
    /// asked, or that fails, leaves what it listed.
    ///
    /// @return true once the catalogues have answered or the wait ran out;
    ///     false while the entry still waits
    bool catalogues_answered() const {
        Runtime::EntryState& current = state();
        if (!current.refresh_asked) {
            current.refresh_asked = true;
            clear_wait();
            ask_catalogues();
        }
        if (refreshing()) {
            current.phase = Runtime::EntryState::Phase::fetch_wait;
            if (!timed_out())
                return false;
            std::cerr << "open-annihilation: entry: the catalogues did not answer\n";
        }
        clear_wait();
        return true;
    }

    /// Queues the Library's download of the package.
    void queue() const {
        Runtime::EntryState& current = state();
        std::string why;
        const std::optional<uint64_t> id = runtime.queue_download(
            current.registry_id, current.ask.key, content::DownloadReason::install, &why
        );
        if (!id) {
            if (why.empty())
                why = std::string(k_no_package);
            fail(capitalise(why));
            return;
        }
        current.download_id = *id;
        current.download_queued = true;
        current.polls = 0;
        show_progress(0);
    }

    /// Shows the fetch, with the percent filled in.
    ///
    /// @param percent the whole percent, 0 to 100
    void show_progress(int percent) const {
        Runtime::EntryState& current = state();
        current.phase = Runtime::EntryState::Phase::progress;
        current.extension_known = true;
        current.extension_percent = percent;
        const std::string line = fill_percent(current.ask.progress, percent);
        if (current.ask.report_on_screen) {
            current.notice = line;
            return;
        }
        const int32_t bar = percent * oa::ui::kit::question_progress_whole / 100;
        show("expansion-fetch", line, bar, false, false);
    }

    /// Watches the download this entry queued. Files are claimed only once
    /// it is done.
    ///
    /// @param bundle the expansion
    void watch(const oa::data::content_bundles::Bundle& bundle) const {
        Runtime::EntryState& current = state();
        const std::vector<content::DownloadView> items = runtime.content_downloads().view();
        const content::DownloadView* found = nullptr;
        for (const content::DownloadView& item : items) {
            if (item.item == current.download_id) {
                found = &item;
                break;
            }
        }
        if (found == nullptr) {
            if (++current.polls >= k_entry_polls)
                fail(std::string(k_not_installed));
            return;
        }
        if (found->state == content::DownloadState::challenge) {
            runtime.content_downloads().cancel(found->item);
            content::DownloadView forced = *found;
            forced.failure = content::DownloadFailure::server_check;
            forced.detail.clear();
            fail(content::failure_message(forced, &oa::data::languages::interface_text));
            return;
        }
        if (found->state == content::DownloadState::failed ||
            found->state == content::DownloadState::cancelled) {
            content::DownloadView forced = *found;
            if (forced.failure == content::DownloadFailure::none)
                forced.failure = content::DownloadFailure::install_failed;
            fail(content::failure_message(forced, &oa::data::languages::interface_text));
            return;
        }
        if (found->state == content::DownloadState::done) {
            if (claim(bundle) || current.phase == Runtime::EntryState::Phase::failed)
                return;
            if (++current.polls >= k_entry_polls)
                fail(std::string(k_not_installed));
            return;
        }
        const int percent = download_percent(*found);
        if (percent != current.extension_percent)
            clear_wait();
        if (timed_out()) {
            clear_wait();
            fail(std::string(k_not_installed));
            return;
        }
        current.polls = 0;
        show_progress(percent);
    }

    /// Reports whether this entry is the player's, rather than a check.
    ///
    /// @return true when the run is not headless and not unattended
    bool facing_player() const {
        return !runtime.options_.headless_check && !runtime.options_.unattended;
    }

    /// Milliseconds on a clock that does not jump when the machine's clock is set.
    ///
    /// @return milliseconds since that clock's epoch
    static int64_t steady_ms() {
        const auto now = std::chrono::steady_clock::now().time_since_epoch();
        return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
    }

    /// Starts the unanswered wait on its first call, then reports when it has run out.
    ///
    /// @return true once `k_unanswered_ms` has passed since the wait started
    bool timed_out() const {
        Runtime::EntryState& current = state();
        const int64_t now = steady_ms();
        if (current.wait_began_ms == 0)
            current.wait_began_ms = now;
        return now - current.wait_began_ms >= k_unanswered_ms;
    }

    /// Forgets the unanswered wait so the next check starts its own.
    void clear_wait() const { state().wait_began_ms = 0; }

    /// The catalogue package this entry asks for, when a snapshot already lists it.
    ///
    /// Only registries the entry reaches count (registry_reached). Does not
    /// ask for a refresh. A hit also records that catalogue's id. The fields
    /// are copied while the snapshot is held.
    ///
    /// @return the package, or nothing when none is listed
    std::optional<ListedPackage> package_listed() const {
        const std::shared_ptr<const content::Snapshot> snapshot =
            runtime.content_service().snapshot();
        if (!snapshot)
            return std::nullopt;
        const Runtime::EntryState& current = state();
        for (const content::Entry& row : snapshot->entries) {
            if (row.package == nullptr || row.package->id != current.ask.key)
                continue;
            if (!current.ask.registry.empty() && row.registry != current.ask.registry)
                continue;
            if (!registry_reached(row.reviewed, current.ask.own_check))
                continue;
            ListedPackage listed;
            listed.registry = row.registry;
            listed.built_in = row.reviewed;
            listed.version = row.package->version;
            listed.release = row.package->release;
            listed.sha256 = row.package->sha256;
            state().registry_id = row.registry;
            return listed;
        }
        return std::nullopt;
    }

    /// Compares the expansion's record with the catalogue's package, once.
    ///
    /// The package's SHA-256 is the whole `.oaexp`'s, which the record names
    /// once an install recorded it. A copy whose archive is gone is missing
    /// whatever its record says.
    ///
    /// @param bundle the expansion
    /// @param package the catalogue's package
    void compare_package(
        const oa::data::content_bundles::Bundle& bundle, const ListedPackage& package
    ) const {
        namespace bundles = oa::data::content_bundles;
        Runtime::EntryState& current = state();
        if (current.package_checked)
            return;
        current.package_checked = true;
        current.extension_version = package.version;
        current.extension_release = package.release;
        const fs::path folder = expansion_folder(bundle);
        const bundles::ContentRecord record = bundles::read_bundle_folder_record(folder, nullptr);
        bundles::PackageCheck check =
            bundles::check_installed_package(bundle, record, package.sha256);
        if (check == bundles::PackageCheck::current && !folder_has_archive(folder))
            check = bundles::PackageCheck::missing;
        current.package_check = check;
    }

    /// Writes what the compare found, and what the entry does about it, to the log.
    ///
    /// @param package the catalogue's package
    void log_package(const ListedPackage& package) const {
        namespace bundles = oa::data::content_bundles;
        const Runtime::EntryState& current = state();
        std::cerr << "open-annihilation: entry: " << current.ask.key << " release "
                  << package.release;
        if (current.package_check == bundles::PackageCheck::current) {
            std::cerr << " is installed\n";
            return;
        }
        switch (current.package_check) {
        case bundles::PackageCheck::missing:
            std::cerr << " is fetched: no copy is installed\n";
            break;
        case bundles::PackageCheck::unrecorded:
            std::cerr << " is fetched: the copy installed names no package\n";
            break;
        default:
            std::cerr << " is fetched: the copy installed is another package\n";
            break;
        }
    }

    /// Reads the online features' settings and the start's game-files check.
    ///
    /// A player asked about the online features waits for the answer. A
    /// quiet run logs the question and goes on as such a run always has: a
    /// headless or unattended run reads neither setting nor the files, and
    /// "-y" stops when both checks are off. A differing file of any bundle
    /// but the one being entered stops a player and "-y" here. The package
    /// being entered is left to the catalogue check.
    void run_opening() const {
        Runtime::EntryState& current = state();
        const OnlineChecks checks = read_online_checks(runtime.preference_values_);
        const ServicesDecision decision = decide_services(checks, way());
        if (decision == ServicesDecision::note)
            note_question(current.ask.services_question);
        if (!facing_player()) {
            current.phase = Runtime::EntryState::Phase::update_wait;
            return;
        }
        current.services_known = true;
        if (decision == ServicesDecision::ask) {
            current.services_on = false;
            current.phase = Runtime::EntryState::Phase::services_ask;
            ask_question(EntryQuestion::services, {});
            return;
        }
        if (!checks.updates && !checks.catalogue) {
            current.services_on = false;
            current.stopped = true;
            fail(
                "The update check and the catalogue check are off. Turn them on from the "
                "Settings page."
            );
            return;
        }
        current.services_on = true;
        current.files_known = true;
        for (const ContentBundleCheck& bundle : runtime.content_check_.bundles) {
            if (bundle.id == current.ask.key)
                continue;
            if (bundle.files_differ == 0)
                continue;
            std::string name;
            if (!bundle.differing.empty())
                name = bundle.differing[0].copy_name;
            if (name.empty())
                name = bundle.english_name;
            current.mismatched_file = name;
            current.stopped = true;
            fail(name + " does not match. Restore it from the Settings page.");
            return;
        }
        current.phase = Runtime::EntryState::Phase::update_wait;
    }

    /// Applies Turn on or Cancel.
    ///
    /// Turn on stores both checks on, tells the catalogues, saves, and goes
    /// on to the game files and the update check.
    ///
    /// @param turn_on true for Turn on
    void apply_services_answer(bool turn_on) const {
        Runtime::EntryState& current = state();
        if (current.phase != Runtime::EntryState::Phase::services_ask)
            return;
        OnlineChecks checks = read_online_checks(runtime.preference_values_);
        const ServicesEffect effect = oa::app::apply_services_answer(
            turn_on ? ServicesAnswer::turn_on : ServicesAnswer::cancel, checks
        );
        if (effect == ServicesEffect::stay) {
            stay();
            return;
        }
        runtime.turn_on_online_checks();
        dismiss();
        run_opening();
        if (current.phase == Runtime::EntryState::Phase::update_wait)
            finish_check();
    }

    /// Records a feed result a check actually produced.
    ///
    /// A remembered feed was not fetched for this entry, so it is not recorded.
    /// A headless or unattended entry records nothing: its screen is not driven.
    ///
    /// @param answered the check's snapshot
    void note_feed(const update::CheckSnapshot& answered) const {
        if (!facing_player() || answered.remembered)
            return;
        Runtime::EntryState& entry = state();
        if (answered.state == update::CheckState::offered && answered.offer.release != nullptr) {
            entry.feed_known = true;
            entry.update_offered = true;
            entry.offered_version = answered.offer.release->version_text;
            return;
        }
        if (answered.state == update::CheckState::up_to_date) {
            entry.feed_known = true;
            entry.update_offered = false;
            return;
        }
        if (answered.state == update::CheckState::failed) {
            entry.feed_known = true;
            entry.feed_failed = true;
        }
    }

    /// What a snapshot says for the decisions in expansion_entry.hpp.
    ///
    /// @param answered the snapshot
    /// @return the answer
    static FeedAnswer feed_answer(const update::CheckSnapshot& answered) noexcept {
        switch (answered.state) {
        case update::CheckState::up_to_date:
            return FeedAnswer::up_to_date;
        case update::CheckState::offered:
            return answered.offer.release != nullptr ? FeedAnswer::newer : FeedAnswer::up_to_date;
        case update::CheckState::failed:
        case update::CheckState::cancelled:
            return FeedAnswer::failed;
        default:
            return FeedAnswer::unanswered;
        }
    }

    /// Goes on to the mod question, then the package.
    void go_on() const {
        Runtime::EntryState& current = state();
        current.handling_check = false;
        clear_wait();
        consider_mod();
    }

    /// Takes the update check's answer.
    ///
    /// A player is asked Update or Cancel about a newer release, and Try
    /// again or Cancel after a failed check. A quiet run logs the update
    /// question and goes on without it.
    ///
    /// @param answered the snapshot
    void take_answer(const update::CheckSnapshot& answered) const {
        Runtime::EntryState& entry = state();
        clear_wait();
        note_feed(answered);
        const FeedAnswer answer = feed_answer(answered);
        if (answer == FeedAnswer::newer && runtime.update_ != nullptr)
            runtime.update_->offered_release = answered.offer.release->release;
        const std::string version =
            answer == FeedAnswer::newer ? answered.offer.release->version_text : std::string();
        switch (decide_update(answer, way())) {
        case UpdateDecision::proceed:
            go_on();
            return;
        case UpdateDecision::note:
            note_question(fill_entry_token(entry.ask.question, "{version}", version));
            entry.update_declined = true;
            go_on();
            return;
        case UpdateDecision::ask_update:
            entry.phase = Runtime::EntryState::Phase::update_ask;
            ask_question(EntryQuestion::update, version);
            return;
        case UpdateDecision::ask_retry:
            entry.feed_known = true;
            entry.feed_failed = true;
            entry.phase = Runtime::EntryState::Phase::retry_ask;
            ask_question(EntryQuestion::retry, {});
            return;
        case UpdateDecision::wait:
            return;
        }
    }

    /// The check did not answer within the unanswered wait. A player is
    /// asked Try again; a quiet run records that and goes on.
    void not_answered() const {
        Runtime::EntryState& entry = state();
        clear_wait();
        if (quiet()) {
            entry.feed_known = facing_player();
            entry.feed_failed = facing_player();
            go_on();
            return;
        }
        update::CheckSnapshot failed;
        failed.state = update::CheckState::failed;
        take_answer(failed);
    }

    /// Takes a result already in hand, or goes on.
    ///
    /// "-y" does not wait for a check that has not answered, and this pass
    /// does not start one. An offer already published, including one read
    /// from a feed stored earlier, is still noted.
    void finish_without_waiting() const {
        runtime.tick_update_check(false);
        const uint64_t recorded = state().generation;
        if (runtime.update_ != nullptr && runtime.update_->checker != nullptr) {
            const update::CheckSnapshot answered = runtime.update_->checker->snapshot();
            if (answered.generation > recorded && answered.state != update::CheckState::checking &&
                answered.state == update::CheckState::offered &&
                answered.offer.release != nullptr) {
                take_answer(answered);
                return;
            }
        }
        go_on();
    }

    /// Reads the check this entry is waiting on.
    ///
    /// A run that will not check goes on in this same pass. Any other run
    /// waits until the main menu has settled, then asks the feed for this
    /// entry and waits for its answer: a snapshot copied while a check is
    /// running is not an answer, a result published before this entry began
    /// is not its question, and for a player neither is a feed stored on an
    /// earlier start. A player whose check does not answer within the
    /// unanswered wait is asked Try again; a quiet run goes on.
    void finish_check() const {
        // Before the settled-menu wait. A quiet run never creates the update
        // state, so that wait would not end, and these runs expect the
        // package on this same pass.
        if (runtime.asked_update() == Runtime::AskedUpdate::pass) {
            go_on();
            return;
        }
        // One of the game's own checks starts no update check of its own
        // against the live feed: only a feed the run named, such as one on
        // this machine, is asked. A result already in hand is still taken.
        if (state().ask.skip_unanswered_check ||
            (state().ask.own_check && !runtime.options_.update_feed.has_value())) {
            finish_without_waiting();
            return;
        }
        runtime.tick_update_check();
        if (!runtime.update_menu_ready()) {
            if (timed_out())
                not_answered();
            return;
        }
        const uint64_t recorded = state().generation;
        const bool player = !quiet();
        // Copy the worker's snapshot, and let the tick log it, before deciding.
        // The copy `tick_update_check` keeps can still say "checking" after the
        // worker has published the offer.
        auto synced = [this]() -> update::CheckSnapshot {
            update::CheckSnapshot live = runtime.update_->checker->snapshot();
            if (runtime.update_->seen_generation != live.generation) {
                runtime.tick_update_check();
                live = runtime.update_->checker->snapshot();
            }
            return live;
        };
        // A result this entry may take: published after it began, finished,
        // and, for a player, fetched rather than read from a stored feed.
        auto answer_of = [&](const update::CheckSnapshot& snapshot) {
            return snapshot.generation > recorded &&
                   snapshot.state != update::CheckState::checking &&
                   !(player && snapshot.remembered);
        };
        if (runtime.update_ != nullptr && runtime.update_->checker != nullptr) {
            const update::CheckSnapshot earlier = synced();
            if (answer_of(earlier)) {
                if (earlier.remembered) {
                    clear_wait();
                    go_on();
                    return;
                }
                take_answer(earlier);
                return;
            }
        }
        runtime.start_update_check(true);
        runtime.tick_update_check();
        if (runtime.update_ == nullptr || runtime.update_->checker == nullptr) {
            // The check cannot start. End the wait instead of sitting on it.
            if (runtime.asked_update() == Runtime::AskedUpdate::pass) {
                go_on();
                return;
            }
            if (timed_out())
                not_answered();
            return;
        }
        const update::CheckSnapshot latest = synced();
        if (!answer_of(latest)) {
            if (runtime.asked_update() == Runtime::AskedUpdate::pass &&
                latest.state != update::CheckState::checking) {
                go_on();
                return;
            }
            if (timed_out())
                not_answered();
            return;
        }
        if (latest.remembered) {
            clear_wait();
            go_on();
            return;
        }
        take_answer(latest);
    }

    /// Applies Update or Cancel. A second apply does nothing.
    ///
    /// Update downloads the release where this copy installs it in place,
    /// with the one-shot entry saved; where it is only offered on its page,
    /// the page opens and the entry stops. There is no answer that enters
    /// on the older version.
    ///
    /// @param take_update true for Update
    void apply_answer(bool take_update) const {
        Runtime::EntryState& current = state();
        if (current.phase != Runtime::EntryState::Phase::update_ask)
            return;
        EntryCarry carry;
        if (!take_update) {
            static_cast<void>(apply_update_answer(UpdateAnswer::cancel, true, carry));
            stay();
            return;
        }
        const bool in_place = runtime.start_update_download();
        const UpdateEffect effect = apply_update_answer(UpdateAnswer::update, in_place, carry);
        if (effect == UpdateEffect::open_page) {
            runtime.open_release_page();
            stay();
            return;
        }
        keep_one_shot(carry.pending);
        dismiss();
        current.phase = Runtime::EntryState::Phase::updating;
        current.handling_check = true;
        current.notice.clear();
        clear_wait();
    }

    /// Applies Try again or Cancel.
    ///
    /// @param try_again true for Try again
    void apply_retry(bool try_again) const {
        Runtime::EntryState& current = state();
        if (current.phase != Runtime::EntryState::Phase::retry_ask)
            return;
        if (apply_retry_answer(try_again ? RetryAnswer::try_again : RetryAnswer::cancel) ==
            RetryEffect::stay) {
            stay();
            return;
        }
        dismiss();
        current.feed_known = false;
        current.feed_failed = false;
        current.handling_check = true;
        current.generation = 0;
        if (runtime.update_ != nullptr && runtime.update_->checker != nullptr)
            current.generation = runtime.update_->checker->snapshot().generation;
        current.phase = Runtime::EntryState::Phase::update_wait;
        clear_wait();
        runtime.start_update_check(true);
    }

    /// Stops the update download this entry started, forgets the one-shot
    /// entry and stays on the menu.
    void cancel_update() const {
        if (state().phase != Runtime::EntryState::Phase::updating)
            return;
        runtime.take_update_action(update::Act::cancel_download);
        keep_one_shot(false);
        stay();
    }

    /// Follows the update this entry started: its progress, then, once the
    /// release is ready, the install that ends the run and starts the new
    /// version with the one-shot entry.
    void watch_update() const {
        Runtime::EntryState& current = state();
        const update::Centre centre = runtime.update_centre();
        const update::CentreWords words =
            update::centre_words(centre, oa::ui::library::TextHooks{nullptr, shown_word});
        if (centre.stage == update::Stage::ready || centre.stage == update::Stage::scheduled) {
            runtime.take_update_action(update::Act::install_now);
            current.phase = Runtime::EntryState::Phase::restarting;
            current.handling_check = false;
            current.notice.clear();
            dismiss();
            return;
        }
        if (centre.stage == update::Stage::installing) {
            current.phase = Runtime::EntryState::Phase::restarting;
            current.handling_check = false;
            dismiss();
            return;
        }
        if (update_failed(centre.stage)) {
            keep_one_shot(false);
            std::string why = words.status;
            if (!words.detail.empty())
                why += ". " + words.detail;
            fail(why);
            return;
        }
        if (centre.stage == update::Stage::page_only || centre.stage == update::Stage::managed) {
            keep_one_shot(false);
            runtime.open_release_page();
            stay();
            return;
        }
        std::string line = words.status;
        if (!words.detail.empty())
            line += "\n" + words.detail;
        const int32_t bar =
            centre.percent >= 0 ? centre.percent * oa::ui::kit::question_progress_whole / 100 : 0;
        show("expansion-updating", line, bar, false, true);
    }

    /// Forgets the one-shot entry and the remembered mod, and saves that.
    void forget_switched_mod() const {
        runtime.keep_plain_string(kExpansionEnterOnce, {});
        runtime.keep_plain_string(kExpansionSwitchedFolder, {});
        runtime.keep_plain_string(kExpansionSwitchedName, {});
        try {
            runtime.flush_preferences();
        } catch (const std::exception&) {
            std::cerr << "open-annihilation: the choice could not be saved\n";
        }
    }

    /// Switches the mod off, remembers it, and ends the run so the next one enters.
    ///
    /// A quiet switch does not save the one-shot: that run enters again on
    /// its own. A switch that fails forgets the one-shot and stays on the menu.
    void switch_off_mod() const {
        Runtime::EntryState& current = state();
        EntryCarry carry;
        const ModEffect effect =
            apply_mod_entry_answer(ModAnswer::switch_off, carry, !current.ask.mod_switch_quietly);
        if (effect != ModEffect::switch_off)
            return;
        const fs::path folder = path_from_utf8(current.ask.mod_folder);
        if (carry.pending)
            runtime.keep_plain_string(kExpansionEnterOnce, "1");
        if (carry.remembered) {
            runtime.keep_plain_string(kExpansionSwitchedFolder, kept_path(folder));
            runtime.keep_plain_string(kExpansionSwitchedName, current.ask.mod_name);
        }
        std::string refusal;
        if (!runtime.switch_to_mod_folder({}, refusal)) {
            forget_switched_mod();
            current.phase = Runtime::EntryState::Phase::stayed;
            current.handling_check = false;
            current.notice.clear();
            return;
        }
        current.phase = Runtime::EntryState::Phase::mod_restart;
        current.handling_check = false;
        current.notice.clear();
    }

    /// Stays on the menu. The mod is not switched off.
    void stay_without_switch() const {
        Runtime::EntryState& current = state();
        current.phase = Runtime::EntryState::Phase::stayed;
        current.handling_check = false;
        current.notice.clear();
    }

    /// The mod question, once the online features and the update have
    /// passed, then the package.
    ///
    /// A run that cannot ask switches a mod off at once. A player is asked.
    void consider_mod() const {
        Runtime::EntryState& current = state();
        if (!current.ask.mod_folder.empty() &&
            (current.ask.mod_switch_quietly || !facing_player())) {
            switch_off_mod();
            return;
        }
        if (!current.ask.mod_question.empty()) {
            current.handling_check = true;
            current.phase = Runtime::EntryState::Phase::mod_ask;
            show_pair(
                "expansion-mod",
                current.ask.mod_question,
                current.ask.mod_switch_label,
                current.ask.mod_cancel_label,
                "switch-off",
                "cancel"
            );
            return;
        }
        consider_package();
    }

    /// Applies Switch off and enter, or Cancel.
    ///
    /// @param switch_off true to switch the mod off
    void apply_mod_answer(bool switch_off) const {
        Runtime::EntryState& current = state();
        if (current.phase != Runtime::EntryState::Phase::mod_ask)
            return;
        EntryCarry carry;
        const ModEffect effect = apply_mod_entry_answer(
            switch_off ? ModAnswer::switch_off : ModAnswer::cancel,
            carry,
            !current.ask.mod_switch_quietly
        );
        if (effect == ModEffect::stay) {
            current.phase = Runtime::EntryState::Phase::stayed;
            current.handling_check = false;
            current.notice.clear();
            return;
        }
        switch_off_mod();
    }

    /// Applies Switch on or Not now. Either answer forgets the remembered mod.
    ///
    /// @param switch_on true to switch the remembered mod on
    void apply_leave_answer(bool switch_on) const {
        Runtime::EntryState& current = state();
        if (current.phase != Runtime::EntryState::Phase::leave_ask)
            return;
        const LeaveEffect effect =
            apply_mod_leave(switch_on ? LeaveAnswer::switch_on : LeaveAnswer::not_now);
        const std::optional<std::string> folder = runtime.plain_string(kExpansionSwitchedFolder);
        if (effect.forget) {
            runtime.keep_plain_string(kExpansionSwitchedFolder, {});
            runtime.keep_plain_string(kExpansionSwitchedName, {});
        }
        current.phase = Runtime::EntryState::Phase::stayed;
        current.handling_check = false;
        current.notice.clear();
        if (!effect.switch_on || !folder.has_value() || folder->empty()) {
            try {
                runtime.flush_preferences();
            } catch (const std::exception&) {
                std::cerr << "open-annihilation: the choice could not be saved\n";
            }
            if (effect.switch_on)
                std::cerr << "open-annihilation: the mod was not switched back on: the folder is "
                             "not there\n";
            return;
        }
        std::error_code missing;
        const fs::path path = path_from_utf8(*folder);
        if (!fs::is_directory(path, missing) || missing) {
            try {
                runtime.flush_preferences();
            } catch (const std::exception&) {
                std::cerr << "open-annihilation: the choice could not be saved\n";
            }
            std::cerr
                << "open-annihilation: the mod was not switched back on: the folder is not there\n";
            return;
        }
        std::string refusal;
        if (!runtime.switch_to_mod_folder(path, refusal)) {
            try {
                runtime.flush_preferences();
            } catch (const std::exception&) {
                std::cerr << "open-annihilation: the choice could not be saved\n";
            }
            std::cerr << "open-annihilation: the mod was not switched back on: " << refusal << '\n';
        }
    }

    /// Brings the package up to date and turns it on.
    ///
    /// The entry waits for the catalogues' answer. The catalogue's package
    /// is fetched and installed when the expansion's record names another
    /// package or none, or no copy is installed; a record that names it
    /// fetches nothing (decide_package_fetch). Only the registries the entry
    /// reaches count: one of the game's own checks never takes the package
    /// from a built-in registry. A copy no such registry lists is used as it
    /// is, and a missing package no such registry lists stops the entry.
    void consider_package() const {
        try {
            namespace bundles = oa::data::content_bundles;
            const bundles::Bundle* bundle = bundles::find_bundle(state().ask.key);
            if (bundle == nullptr) {
                fail("That package is not one this game knows.");
                return;
            }
            if (bundle->kind != bundles::Kind::expansion || bundle->extends.empty()) {
                fail("That package is not for this game.");
                return;
            }
            if (state().download_queued) {
                watch(*bundle);
                return;
            }
            // The game folder itself holds the expansion's files, the known
            // bytes every package of it carries: nothing is fetched.
            if (holds_id(runtime.kept_content_bundles(), state().ask.key)) {
                state().package_checked = true;
                state().package_check = bundles::PackageCheck::current;
                if (const std::optional<ListedPackage> listed = package_listed()) {
                    state().extension_version = listed->version;
                    state().extension_release = listed->release;
                }
                std::cerr << "open-annihilation: entry: " << state().ask.key
                          << " is in the game folder\n";
                turn_on(*bundle);
                return;
            }
            if (!catalogues_answered())
                return;
            const std::optional<ListedPackage> package = package_listed();
            if (!package) {
                // No registry the entry reaches lists the package: a copy
                // installed is used as it is, and a missing one stops
                // (decide_package_fetch).
                if (claim(*bundle))
                    return;
                if (state().phase == Runtime::EntryState::Phase::failed)
                    return;
                fail(capitalise(k_no_package));
                return;
            }
            compare_package(*bundle, *package);
            state().extension_known = true;
            const PackageAction action =
                decide_package_fetch(copy_state(state().package_check), true);
            log_package(*package);
            if (action == PackageAction::use_installed) {
                if (claim(*bundle))
                    return;
                if (state().phase == Runtime::EntryState::Phase::failed)
                    return;
            }
            queue();
        } catch (const std::exception&) {
            fail(std::string(k_not_installed));
        }
    }

    /// Records where the check stood. advance reads the online features and
    /// the files next, and starts the update check once the main menu has
    /// settled, so an entry that begins before that waits.
    ///
    /// @param ask the package and the sentences
    void begin(const ExpansionEntryAsk& ask) const {
        Runtime::EntryState& current = state();
        current.ask = ask;
        current.handling_check = true;
        current.registry_id.clear();
        current.refresh_asked = false;
        current.download_queued = false;
        current.download_id = 0;
        current.polls = 0;
        current.notice.clear();
        current.services_known = false;
        current.services_on = false;
        current.files_known = false;
        current.mismatched_file.clear();
        current.matched_files.clear();
        current.feed_known = false;
        current.feed_failed = false;
        current.update_offered = false;
        current.offered_version.clear();
        current.update_declined = false;
        current.running_version = update::this_build({}).version_text;
        current.extension_known = false;
        current.extension_percent = -1;
        current.extension_installed = false;
        current.extension_current = false;
        current.extension_version.clear();
        current.extension_release = 0;
        current.package_checked = false;
        current.package_check = oa::data::content_bundles::PackageCheck::missing;
        current.stopped = false;
        current.wait_began_ms = 0;
        current.generation = 0;
        if (runtime.update_ != nullptr && runtime.update_->checker != nullptr)
            current.generation = runtime.update_->checker->snapshot().generation;
        // A mod the command line named is never switched off, even when the
        // folder is empty, so the entry can never go on: it says so before
        // anything is asked or changed. A player reads one button. A run
        // that cannot ask stays on the menu and does not restart.
        if (ask.mod_command_line) {
            if (ask.mod_switch_quietly || !facing_player()) {
                stay_without_switch();
                return;
            }
            current.handling_check = true;
            current.phase = Runtime::EntryState::Phase::command_ask;
            show("expansion-command", ask.mod_question, -1, true, false);
            return;
        }
        current.phase = Runtime::EntryState::Phase::opening;
    }

    /// Copies the entry onto the status the caller reads.
    ///
    /// @param[out] status where it stands
    void publish(ExpansionEntryStatus& status) const {
        const Runtime::EntryState& current = state();
        status.notice = current.notice;
        status.services_known = current.services_known;
        status.services_on = current.services_on;
        status.files_known = current.files_known;
        status.mismatched_file = current.mismatched_file;
        status.matched_files = current.matched_files;
        status.feed_known = current.feed_known;
        status.feed_failed = current.feed_failed;
        status.update_offered = current.update_offered;
        status.offered_version = current.offered_version;
        status.update_declined = current.update_declined;
        status.running_version = current.running_version;
        status.extension_known = current.extension_known;
        status.extension_percent = current.extension_percent;
        status.extension_installed = current.extension_installed;
        status.extension_current = current.extension_current;
        status.extension_version = current.extension_version;
        status.extension_release = current.extension_release;
        status.extension_new =
            current.phase == Runtime::EntryState::Phase::ready && current.extension_installed;
        status.stopped = current.stopped;
        switch (current.phase) {
        case Runtime::EntryState::Phase::ready:
            status.step = ExpansionEntryStep::ready;
            break;
        case Runtime::EntryState::Phase::update_ask:
            status.step = ExpansionEntryStep::asking;
            break;
        case Runtime::EntryState::Phase::services_ask:
            status.step = ExpansionEntryStep::services_asking;
            break;
        case Runtime::EntryState::Phase::retry_ask:
            status.step = ExpansionEntryStep::retry_asking;
            break;
        case Runtime::EntryState::Phase::updating:
            status.step = ExpansionEntryStep::updating;
            break;
        case Runtime::EntryState::Phase::mod_ask:
            status.step = ExpansionEntryStep::mod_asking;
            break;
        case Runtime::EntryState::Phase::mod_restart:
        case Runtime::EntryState::Phase::restarting:
            status.step = ExpansionEntryStep::restarting;
            break;
        case Runtime::EntryState::Phase::stayed:
            status.step = ExpansionEntryStep::stayed;
            break;
        case Runtime::EntryState::Phase::failed:
            status.step = ExpansionEntryStep::failed;
            break;
        default:
            status.step = ExpansionEntryStep::waiting;
            break;
        }
    }

    /// Moves the entry on by one step.
    ///
    /// @param[out] status where it stands
    void advance(ExpansionEntryStatus& status) const {
        switch (state().phase) {
        case Runtime::EntryState::Phase::mod_ask:
        case Runtime::EntryState::Phase::command_ask:
        case Runtime::EntryState::Phase::leave_ask:
        case Runtime::EntryState::Phase::mod_restart:
        case Runtime::EntryState::Phase::restarting:
        case Runtime::EntryState::Phase::services_ask:
        case Runtime::EntryState::Phase::update_ask:
        case Runtime::EntryState::Phase::retry_ask:
            publish(status);
            return;
        default:
            break;
        }
        if (state().phase == Runtime::EntryState::Phase::opening)
            run_opening();
        // A stop for services or a bad file must not pump a download or a refresh.
        if (!state().stopped)
            runtime.tick_content();
        switch (state().phase) {
        case Runtime::EntryState::Phase::update_wait:
            finish_check();
            break;
        case Runtime::EntryState::Phase::updating:
            watch_update();
            break;
        case Runtime::EntryState::Phase::fetch_wait:
        case Runtime::EntryState::Phase::progress:
            consider_package();
            break;
        default:
            break;
        }
        publish(status);
    }
};

void Runtime::destroy_entry_state(EntryState* state) noexcept {
    delete state;
}

void Runtime::entry_question_closed(Runtime* runtime, const void* closing) noexcept {
    if (runtime == nullptr || runtime->entry_ == nullptr)
        return;
    if (closing != static_cast<const void*>(runtime->entry_->question))
        return;
    runtime->entry_->question = nullptr;
    runtime->entry_->shown_word.clear();
}

bool Runtime::entry_question_tick(Runtime* runtime) {
    if (runtime == nullptr || runtime->entry_ == nullptr)
        return false;
    if (runtime->match_ == nullptr && runtime->screen_ != Screen::match)
        return false;
    EntryState& current = *runtime->entry_;
    EntryDriver driver(*runtime);
    switch (current.phase) {
    case EntryState::Phase::services_ask:
    case EntryState::Phase::update_ask:
    case EntryState::Phase::retry_ask:
    case EntryState::Phase::progress:
    case EntryState::Phase::fetch_wait:
        current.phase = EntryState::Phase::stayed;
        current.handling_check = false;
        current.notice.clear();
        return true;
    case EntryState::Phase::updating:
        // A match pauses the download; the entry does not wait through it.
        driver.keep_one_shot(false);
        current.phase = EntryState::Phase::stayed;
        current.handling_check = false;
        current.notice.clear();
        return true;
    case EntryState::Phase::mod_ask:
        driver.apply_mod_answer(false);
        return true;
    case EntryState::Phase::command_ask:
        driver.stay_without_switch();
        return true;
    case EntryState::Phase::leave_ask:
        driver.apply_leave_answer(false);
        return true;
    default:
        return false;
    }
}

bool Runtime::entry_update_answered(Runtime* runtime, int32_t button) {
    if (runtime == nullptr || runtime->entry_ == nullptr)
        return true;
    EntryDriver driver(*runtime);
    switch (runtime->entry_->phase) {
    case EntryState::Phase::command_ask:
        driver.stay_without_switch();
        break;
    case EntryState::Phase::mod_ask:
        driver.apply_mod_answer(button == 0);
        break;
    case EntryState::Phase::leave_ask:
        driver.apply_leave_answer(button == 0);
        break;
    case EntryState::Phase::services_ask:
        driver.apply_services_answer(button == 0);
        break;
    case EntryState::Phase::retry_ask:
        driver.apply_retry(button == 0);
        break;
    case EntryState::Phase::updating:
        driver.cancel_update();
        break;
    default:
        driver.apply_answer(button == 0);
        break;
    }
    return true;
}

void Runtime::begin_expansion_entry(const ExpansionEntryAsk& ask) {
    if (expansion_entry_busy())
        return;
    if (entry_ == nullptr)
        entry_.reset(new EntryState());
    EntryDriver driver(*this);
    driver.dismiss();
    driver.begin(ask);
}

ExpansionEntryStatus Runtime::advance_expansion_entry() {
    ExpansionEntryStatus status;
    if (entry_ == nullptr)
        return status;
    EntryDriver driver(*this);
    driver.advance(status);
    return status;
}

void Runtime::answer_expansion_entry(bool take_update) {
    if (entry_ == nullptr)
        return;
    EntryDriver driver(*this);
    driver.apply_answer(take_update);
}

void Runtime::restart_expansion_entry(const std::string& notice) {
    if (entry_ == nullptr || entry_->phase != EntryState::Phase::ready)
        return;
    EntryDriver driver(*this);
    driver.restart_for_install(notice);
}

bool Runtime::expansion_entry_holds_check() const noexcept {
    return entry_ != nullptr && entry_->handling_check;
}

void Runtime::expansion_entry_show_on_screen(bool on_screen) {
    if (entry_ == nullptr)
        return;
    entry_->ask.report_on_screen = on_screen;
}

bool Runtime::expansion_entry_busy() const noexcept {
    if (entry_ == nullptr)
        return false;
    switch (entry_->phase) {
    case EntryState::Phase::opening:
    case EntryState::Phase::services_ask:
    case EntryState::Phase::update_wait:
    case EntryState::Phase::update_ask:
    case EntryState::Phase::retry_ask:
    case EntryState::Phase::updating:
    case EntryState::Phase::fetch_wait:
    case EntryState::Phase::progress:
    case EntryState::Phase::mod_ask:
    case EntryState::Phase::command_ask:
    case EntryState::Phase::leave_ask:
        return true;
    default:
        return false;
    }
}

void Runtime::ask_remembered_mod(
    const std::string& question, const std::string& on_label, const std::string& later_label
) {
    if (expansion_entry_busy())
        return;
    if (entry_ == nullptr)
        entry_.reset(new EntryState());
    EntryDriver driver(*this);
    driver.dismiss();
    EntryState& current = *entry_;
    current.leave_question = question;
    current.leave_on_label = on_label;
    current.leave_later_label = later_label;
    current.phase = EntryState::Phase::leave_ask;
    current.handling_check = true;
    current.extension_percent = -1;
    current.notice = question;
    driver.show_pair("expansion-leave", question, on_label, later_label, "switch-on", "not-now");
}

bool Runtime::remembered_mod_question_open() const noexcept {
    return entry_ != nullptr && entry_->phase == EntryState::Phase::leave_ask;
}

void Runtime::answer_remembered_mod(bool switch_on) {
    if (entry_ == nullptr)
        return;
    EntryDriver driver(*this);
    driver.apply_leave_answer(switch_on);
    driver.dismiss();
}

bool Runtime::expansion_mod_question_open() const noexcept {
    return entry_ != nullptr && entry_->phase == EntryState::Phase::mod_ask;
}

void Runtime::answer_expansion_mod_question(bool switch_off) {
    if (entry_ == nullptr)
        return;
    EntryDriver driver(*this);
    driver.apply_mod_answer(switch_off);
    driver.dismiss();
}

void begin_expansion_entry(Runtime& runtime, const ExpansionEntryAsk& ask) {
    runtime.begin_expansion_entry(ask);
}

ExpansionEntryStatus advance_expansion_entry(Runtime& runtime) {
    return runtime.advance_expansion_entry();
}

void answer_expansion_entry(Runtime& runtime, bool take_update) {
    runtime.answer_expansion_entry(take_update);
}

void restart_expansion_entry(Runtime& runtime, const std::string& notice) {
    runtime.restart_expansion_entry(notice);
}

void expansion_entry_show_on_screen(Runtime& runtime, bool on_screen) {
    runtime.expansion_entry_show_on_screen(on_screen);
}

bool expansion_entry_busy(const Runtime& runtime) {
    return runtime.expansion_entry_busy();
}

const oa::data::languages::Language& expansion_entry_language(const Runtime& runtime) {
    return runtime.shown_language();
}

void ask_remembered_mod(
    Runtime& runtime,
    const std::string& question,
    const std::string& on_label,
    const std::string& later_label
) {
    runtime.ask_remembered_mod(question, on_label, later_label);
}

bool remembered_mod_question_open(const Runtime& runtime) {
    return runtime.remembered_mod_question_open();
}

void answer_remembered_mod(Runtime& runtime, bool switch_on) {
    runtime.answer_remembered_mod(switch_on);
}

bool expansion_mod_question_open(const Runtime& runtime) {
    return runtime.expansion_mod_question_open();
}

void answer_expansion_mod_question(Runtime& runtime, bool switch_off) {
    runtime.answer_expansion_mod_question(switch_off);
}

ActiveMod active_mod(const Runtime& runtime) {
    ActiveMod playing;
    // A Developer Mode change layers a profile over the base game without a
    // chosen mod. Only a profile or a folder this run was given counts.
    const bool profile_set = runtime.options_.mod_profile != nullptr;
    const bool layered = !runtime.options_.layered_mod_folder.empty();
    playing.active = chosen_mod(profile_set, layered);
    if (profile_set) {
        playing.has_profile = true;
        playing.profile_id = runtime.options_.mod_profile->id;
        playing.profile_name = runtime.options_.mod_profile->name;
    }
    if (layered)
        playing.folder = oa::platform::path_to_utf8(runtime.options_.layered_mod_folder);
    else if (playing.active && !runtime.options_.mod_file.empty())
        playing.folder = oa::platform::path_to_utf8(runtime.options_.mod_file.parent_path());
    return playing;
}

void keep_expansion_preference(Runtime& runtime, std::string_view key, std::string_view value) {
    runtime.keep_plain_string(key, value);
}

std::optional<std::string> expansion_preference(const Runtime& runtime, std::string_view key) {
    return runtime.plain_string(key);
}

bool switch_active_mod(Runtime& runtime, std::string_view folder, std::string& refusal) {
    if (folder.empty())
        return runtime.switch_to_mod_folder({}, refusal);
    return runtime.switch_to_mod_folder(path_from_utf8(folder), refusal);
}

bool expansion_will_restart(const Runtime& runtime) noexcept {
    return runtime.soft_restart_requested();
}

void save_expansion_preferences(Runtime& runtime) {
    runtime.flush_preferences();
}

void end_expansion_run(Runtime& runtime) {
    runtime.quit_application(nullptr);
}

uint32_t active_mutator_count(const Runtime& runtime) {
    const oa::data::mutators::Loadout* loadout = runtime.match_loadout();
    const std::size_t applied = loadout != nullptr ? loadout->entries.size() : 0;
    return carried_mutator_count(runtime.room_mutators().size(), applied);
}

} // namespace oa::app

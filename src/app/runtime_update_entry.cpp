// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One entry into an expansion. It asks the update check the player already
// has, and when that check offers a newer game it asks whether to take it
// before going on. When the package is not already in the game folder it
// queues the Library's download, shows that fetch, and mounts the package
// where the player is. The caller opens the expansion once this reports ready.
#include "oa/app/runtime.hpp"

#include "engine_settings_state.hpp"
#include "oa_layer.hpp"
#include "update_state.hpp"

#include "oa/app/content/downloads.hpp"
#include "oa/app/content/settings.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/update/centre.hpp"
#include "oa/app/update/check.hpp"
#include "oa/base/sha256.hpp"
#include "oa/data/content_bundles.hpp"
#include "oa/platform/files.hpp"
#include "oa/data/content_bundles/content_choice.hpp"
#include "oa/data/content_bundles/record.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/ui/engine_settings.hpp"
#include "oa/ui/kit/components_more.hpp"

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

namespace sha = oa::base::sha256;

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

/// The SHA-256 of a file that is exactly `expected` bytes.
///
/// @param path the file
/// @param expected how many bytes it must hold
/// @return the digest, or nothing when the size differs or the file cannot be read
std::optional<sha::Digest> hash_exact(const fs::path& path, uint64_t expected) {
    std::FILE* file = oa::platform::open_file(path, "rb");
    if (file == nullptr)
        return std::nullopt;
    sha::Hasher hasher;
    std::vector<uint8_t> buffer(64 * 1024);
    uint64_t got = 0;
    while (got < expected) {
        const uint64_t room = expected - got;
        std::size_t ask = buffer.size();
        if (static_cast<uint64_t>(ask) > room)
            ask = static_cast<std::size_t>(room);
        const std::size_t read = std::fread(buffer.data(), 1, ask, file);
        if (read == 0) {
            std::fclose(file);
            return std::nullopt;
        }
        sha::update(hasher, std::span<const uint8_t>(buffer.data(), read));
        got += static_cast<uint64_t>(read);
    }
    unsigned char extra = 0;
    const std::size_t more = std::fread(&extra, 1, 1, file);
    std::fclose(file);
    if (more != 0)
        return std::nullopt;
    return sha::finish(hasher);
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
        idle,        ///< no entry is running
        opening,     ///< a player's services and game-files checks
        update_wait, ///< the asked check has not finished
        update_ask,  ///< the update question is up
        fetch_wait,  ///< a catalogue refresh is still running
        progress,    ///< the package is being fetched
        stayed,      ///< the player stayed on the menu
        failed,      ///< the entry stopped
        ready,       ///< the package is on
    };

    Phase phase{Phase::idle};          ///< where it is
    ExpansionEntryAsk ask{};           ///< the package and the sentences
    bool handling_check{false};        ///< the normal update question stays back
    uint64_t generation{0};            ///< the check generation before this ask
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
    bool update_declined{false};              ///< the player went on without the update
    bool extension_known{false};              ///< the catalogue's package was considered
    int32_t extension_percent{-1};            ///< the fetch's percent, or -1
    bool extension_installed{false};          ///< this entry downloaded and installed it
    bool extension_current{false};            ///< the installed package matches
    std::string extension_version{};          ///< the catalogue package's version
    bool extension_compared{false};           ///< the installed archive was compared once
    bool extension_older{false};              ///< that archive is missing a newer catalogue copy
    bool stopped{false};                      ///< a check stopped the entry before any contact
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

    /// Shows one question, or updates the one already showing the same word.
    ///
    /// @param word the question's word
    /// @param paragraph the sentence
    /// @param progress the bar, or -1 for none
    /// @param update_buttons true for Update and Not now
    /// @param ok_button true for OK
    void show(
        const std::string& word,
        const std::string& paragraph,
        int32_t progress,
        bool update_buttons,
        bool ok_button
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
        dismiss();
        oa::ui::kit::Question asked;
        asked.title = "Open Annihilation";
        asked.word = word;
        asked.paragraphs.push_back({paragraph, false});
        asked.progress = progress;
        if (update_buttons) {
            asked.buttons.push_back({current.ask.update_label, true, "update"});
            asked.buttons.push_back({current.ask.later_label, false, "later"});
            asked.primary_button = 0;
            asked.cancel_button = 1;
            asked.marked = 0;
        } else if (ok_button) {
            asked.buttons.push_back({current.ask.ok_label, true, "ok"});
            asked.primary_button = 0;
            asked.cancel_button = 0;
            asked.marked = 0;
        }
        Runtime* self = &runtime;
        QuestionScreen::Host host;
        host.answer = [self](int32_t button) {
            return Runtime::entry_update_answered(self, button);
        };
        host.tick = [self]() { return Runtime::entry_question_tick(self); };
        host.closed = [self](const QuestionScreen&) { Runtime::entry_question_closed(self); };
        OaLayer& layer = runtime.oa_layer();
        auto screen = std::make_unique<QuestionScreen>(
            layer, Screen::main_menu, std::move(asked), std::move(host)
        );
        QuestionScreen* taken = screen.get();
        taken->show_on_any_menu();
        layer.show_when_free(std::move(screen));
        current.question = taken;
        current.shown_word = word;
        current.notice = paragraph;
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
        show("expansion-notice", current.notice, -1, false, true);
    }

    /// The package is on. The question comes down.
    void succeed() const {
        Runtime::EntryState& current = state();
        current.phase = Runtime::EntryState::Phase::ready;
        current.handling_check = false;
        current.notice.clear();
        if (facing_player()) {
            // Only a compare or a download this entry finished may be described.
            current.extension_known = current.extension_compared || current.download_queued;
            if (current.download_queued)
                current.extension_installed = true;
            else if (current.extension_compared && !current.extension_older)
                current.extension_current = true;
            if (current.extension_version.empty()) {
                if (const oa::data::catalogue::Package* package = package_listed())
                    current.extension_version = package->version;
            }
        }
        dismiss();
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
    bool folder_has_archive(const fs::path& folder) const {
        std::error_code failure;
        if (!fs::is_directory(folder, failure) || failure)
            return false;
        const fs::directory_iterator end;
        for (fs::directory_iterator cursor(folder, failure); !failure && cursor != end;
             cursor.increment(failure)) {
            std::error_code status;
            if (!cursor->is_regular_file(status) || status)
                continue;
            if (!archive_suffix(cursor->path()).empty())
                return true;
        }
        return false;
    }

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
            fail(std::string(k_not_installed));
            return true;
        }
        return turn_on(bundle);
    }

    /// The first catalogue that lists the package, when one does.
    ///
    /// @return true when `registry_id` names that catalogue
    bool listed() const {
        const std::shared_ptr<const content::Snapshot> snapshot =
            runtime.content_service().snapshot();
        if (!snapshot)
            return false;
        const Runtime::EntryState& current = state();
        for (const content::Entry& row : snapshot->entries) {
            if (row.package == nullptr || row.package->id != current.ask.key)
                continue;
            if (!current.ask.registry.empty() && row.registry != current.ask.registry)
                continue;
            state().registry_id = row.registry;
            return true;
        }
        return false;
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

    /// Makes sure a catalogue lists the package, asking for one refresh.
    ///
    /// @return true when it is listed
    bool ensure_listed() const {
        if (listed()) {
            clear_wait();
            return true;
        }
        Runtime::EntryState& current = state();
        if (!current.refresh_asked) {
            current.refresh_asked = true;
            runtime.content_service().refresh(content::RefreshReason::check_now);
        }
        if (refreshing()) {
            current.phase = Runtime::EntryState::Phase::fetch_wait;
            if (!timed_out())
                return false;
            // The refresh did not answer. Stop this wait; do not sit on it.
            clear_wait();
            fail(capitalise(k_no_package));
            return false;
        }
        if (listed())
            return true;
        fail(capitalise(k_no_package));
        return false;
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

    /// Reports whether the update check and the catalogue check are both off.
    ///
    /// Off is the first-run choice that turns both the update check and the
    /// catalogue check to never. Either one left on still counts as on.
    ///
    /// @return true when both are never
    bool services_off() const {
        const update::CheckMode updates = update::read_check_mode(runtime.preference_values_);
        const content::CheckForUpdates catalogues =
            content::read_check_for_updates(runtime.preference_values_);
        return updates == update::CheckMode::never && catalogues == content::CheckForUpdates::never;
    }

    /// The catalogue package this entry asks for, when a snapshot already lists it.
    ///
    /// Does not ask for a refresh. A hit also records that catalogue's id.
    ///
    /// @return the package, or null when none is listed
    const oa::data::catalogue::Package* package_listed() const {
        const std::shared_ptr<const content::Snapshot> snapshot =
            runtime.content_service().snapshot();
        if (!snapshot)
            return nullptr;
        const Runtime::EntryState& current = state();
        for (const content::Entry& row : snapshot->entries) {
            if (row.package == nullptr || row.package->id != current.ask.key)
                continue;
            if (!current.ask.registry.empty() && row.registry != current.ask.registry)
                continue;
            state().registry_id = row.registry;
            return row.package;
        }
        return nullptr;
    }

    /// Reports whether the installed archive is older than the catalogue package.
    ///
    /// A missing archive is not older: the caller fetches it as a missing
    /// package. A size that differs is older, and the bytes are not read.
    ///
    /// @param bundle the expansion
    /// @param package the catalogue package
    /// @return true when an archive is there and does not match
    bool archive_older(
        const oa::data::content_bundles::Bundle& bundle, const oa::data::catalogue::Package& package
    ) const {
        const fs::path archive = first_archive(expansion_folder(bundle));
        if (archive.empty())
            return false;
        std::error_code error;
        const uintmax_t size = fs::file_size(archive, error);
        if (error || size != static_cast<uintmax_t>(package.size))
            return true;
        const std::optional<sha::Digest> digest = hash_exact(archive, package.size);
        return !digest || *digest != package.sha256;
    }

    /// Reads the update and catalogue settings and the start's game-files check.
    ///
    /// Both checks off stop here: nothing is refreshed and no feed is asked.
    /// A differing file of any bundle but the one being entered stops here
    /// too. The package being entered is left to the catalogue check.
    void run_opening() const {
        Runtime::EntryState& current = state();
        current.services_known = true;
        if (services_off()) {
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

    /// Claims files that are already present, or queues the download.
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
            if (facing_player()) {
                // "-y" does not wait for a catalogue that has not answered, and
                // then keeps the copy already on disk. A catalogue that has
                // answered with a newer copy is fetched either way.
                const bool straight = state().ask.skip_unanswered_check;
                const oa::data::catalogue::Package* package = package_listed();
                if (package == nullptr && !straight) {
                    if (!ensure_listed())
                        return;
                    package = package_listed();
                }
                if (package != nullptr && !state().extension_compared) {
                    state().extension_version = package->version;
                    state().extension_older = archive_older(*bundle, *package);
                    state().extension_compared = true;
                }
                if (state().extension_older) {
                    state().extension_known = true;
                    queue();
                    return;
                }
            }
            if (claim(*bundle))
                return;
            if (state().phase == Runtime::EntryState::Phase::failed)
                return;
            if (!ensure_listed())
                return;
            queue();
        } catch (const std::exception&) {
            fail(std::string(k_not_installed));
        }
    }

    /// Takes a result already in hand, or goes on to the package.
    ///
    /// A check that has not answered does not hold the entry, and this pass
    /// does not start one. An offer already published, including one read
    /// from a feed stored earlier, is still asked. Anything else considers
    /// the package, so a missing package is still fetched.
    void finish_without_waiting() const {
        runtime.tick_update_check(false);
        const uint64_t recorded = state().generation;
        if (runtime.update_ != nullptr && runtime.update_->checker != nullptr) {
            const update::CheckSnapshot answered = runtime.update_->checker->snapshot();
            if (answered.generation > recorded && answered.state != update::CheckState::checking) {
                if (answered.state == update::CheckState::offered &&
                    answered.offer.release != nullptr) {
                    runtime.update_->offered_release = answered.offer.release->release;
                    // Nobody is at the keyboard for this entry. Decline and go on.
                    if (facing_player() && state().ask.skip_unanswered_check) {
                        note_feed(answered);
                        state().update_declined = true;
                        state().handling_check = false;
                        clear_wait();
                        consider_package();
                        return;
                    }
                    state().phase = Runtime::EntryState::Phase::update_ask;
                    show("expansion-update", state().ask.question, -1, true, false);
                    return;
                }
                state().handling_check = false;
                consider_package();
                return;
            }
        }
        state().handling_check = false;
        consider_package();
    }

    /// Reads the check this entry is waiting on.
    ///
    /// A run that will not check goes on to the package in this same pass.
    /// Any other run waits until the main menu has settled. A snapshot
    /// copied while a check is still running is not an answer: the entry
    /// waits until the check already running, or the one it starts here,
    /// has offered a release, found this one current, or failed. A result
    /// published before this entry began is not its question. An ask that
    /// must not wait for an unanswered check goes on in this pass instead.
    void finish_check() const {
        // Before the settled-menu wait. A quiet run never creates the update
        // state, so that wait would not end, and these checks expect the
        // package on this same pass.
        if (runtime.asked_update() == Runtime::AskedUpdate::pass) {
            state().handling_check = false;
            consider_package();
            return;
        }
        // "-y", and a check run, do not start a feed check that has not
        // answered. A result already in hand is still taken.
        if (state().ask.skip_unanswered_check) {
            finish_without_waiting();
            return;
        }
        runtime.tick_update_check();
        if (!runtime.update_menu_ready()) {
            if (!timed_out())
                return;
            clear_wait();
            state().handling_check = false;
            consider_package();
            return;
        }

        const uint64_t recorded = state().generation;
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
        auto accept = [&](const update::CheckSnapshot& answered) {
            Runtime::EntryState& entry = state();
            clear_wait();
            note_feed(answered);
            if (answered.remembered) {
                entry.handling_check = false;
                consider_package();
                return;
            }
            if (answered.state == update::CheckState::offered &&
                answered.offer.release != nullptr) {
                runtime.update_->offered_release = answered.offer.release->release;
                if (facing_player() && entry.ask.skip_unanswered_check) {
                    entry.update_declined = true;
                    entry.handling_check = false;
                    consider_package();
                    return;
                }
                entry.phase = Runtime::EntryState::Phase::update_ask;
                show("expansion-update", entry.ask.question, -1, true, false);
                return;
            }
            entry.handling_check = false;
            consider_package();
        };

        if (runtime.update_ != nullptr && runtime.update_->checker != nullptr) {
            const update::CheckSnapshot earlier = synced();
            if (earlier.generation > recorded && earlier.state != update::CheckState::checking) {
                accept(earlier);
                return;
            }
        }

        runtime.start_update_check(true);
        runtime.tick_update_check();
        if (runtime.update_ == nullptr || runtime.update_->checker == nullptr) {
            // The check cannot start. End the wait instead of sitting on it.
            if (runtime.asked_update() == Runtime::AskedUpdate::pass || timed_out()) {
                clear_wait();
                state().handling_check = false;
                consider_package();
            }
            return;
        }

        const update::CheckSnapshot latest = synced();
        if (latest.state == update::CheckState::checking) {
            if (!timed_out())
                return;
            // The feed was asked and did not answer. Say so, then go on.
            clear_wait();
            state().feed_known = true;
            state().feed_failed = true;
            state().handling_check = false;
            consider_package();
            return;
        }
        if (latest.generation <= recorded) {
            if (runtime.asked_update() != Runtime::AskedUpdate::pass) {
                if (!timed_out())
                    return;
                clear_wait();
                state().handling_check = false;
                consider_package();
                return;
            }
            state().handling_check = false;
            consider_package();
            return;
        }
        accept(latest);
    }

    /// Applies Update or Not now. A second apply does nothing.
    ///
    /// @param take_update true to download the offered release
    void apply_answer(bool take_update) const {
        Runtime::EntryState& current = state();
        if (current.phase != Runtime::EntryState::Phase::update_ask)
            return;
        if (take_update) {
            runtime.take_update_action(update::Act::download);
            current.handling_check = false;
            current.phase = Runtime::EntryState::Phase::stayed;
            current.notice.clear();
            return;
        }
        current.handling_check = false;
        current.notice.clear();
        if (!facing_player()) {
            current.phase = Runtime::EntryState::Phase::stayed;
            return;
        }
        current.update_declined = true;
        consider_package();
    }

    /// Records where the check stood. advance starts one once the main menu
    /// has settled, so an entry that begins before that waits.
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
        current.extension_known = false;
        current.extension_percent = -1;
        current.extension_installed = false;
        current.extension_current = false;
        current.extension_version.clear();
        current.extension_compared = false;
        current.extension_older = false;
        current.stopped = false;
        current.wait_began_ms = 0;
        current.generation = 0;
        if (runtime.update_ != nullptr && runtime.update_->checker != nullptr)
            current.generation = runtime.update_->checker->snapshot().generation;
        // A check keeps the path it has today. A player reads the services
        // and the game files before that path starts.
        current.phase = facing_player() ? Runtime::EntryState::Phase::opening
                                        : Runtime::EntryState::Phase::update_wait;
    }

    /// Moves the entry on by one step.
    ///
    /// @param[out] status where it stands
    void advance(ExpansionEntryStatus& status) const {
        if (state().phase == Runtime::EntryState::Phase::opening)
            run_opening();
        // A stop for services or a bad file must not pump a download or a refresh.
        if (!state().stopped)
            runtime.tick_content();
        switch (state().phase) {
        case Runtime::EntryState::Phase::update_wait:
            finish_check();
            break;
        case Runtime::EntryState::Phase::fetch_wait:
        case Runtime::EntryState::Phase::progress:
            consider_package();
            break;
        case Runtime::EntryState::Phase::update_ask:
            // A headless or unattended run has nobody to press Not now.
            if (!facing_player() && timed_out()) {
                clear_wait();
                apply_answer(false);
                dismiss();
            }
            break;
        default:
            break;
        }
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
        status.extension_known = current.extension_known;
        status.extension_percent = current.extension_percent;
        status.extension_installed = current.extension_installed;
        status.extension_current = current.extension_current;
        status.extension_version = current.extension_version;
        status.stopped = current.stopped;
        switch (current.phase) {
        case Runtime::EntryState::Phase::ready:
            status.step = ExpansionEntryStep::ready;
            break;
        case Runtime::EntryState::Phase::update_ask:
            status.step = ExpansionEntryStep::asking;
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
};

void Runtime::destroy_entry_state(EntryState* state) noexcept {
    delete state;
}

void Runtime::entry_question_closed(Runtime* runtime) noexcept {
    if (runtime == nullptr || runtime->entry_ == nullptr)
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
    switch (current.phase) {
    case EntryState::Phase::update_ask:
    case EntryState::Phase::progress:
    case EntryState::Phase::fetch_wait:
        current.phase = EntryState::Phase::stayed;
        current.handling_check = false;
        current.notice.clear();
        return true;
    default:
        return false;
    }
}

bool Runtime::entry_update_answered(Runtime* runtime, int32_t button) {
    if (runtime == nullptr || runtime->entry_ == nullptr)
        return true;
    EntryDriver driver(*runtime);
    driver.apply_answer(button == 0);
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
    driver.dismiss();
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
    case EntryState::Phase::update_wait:
    case EntryState::Phase::update_ask:
    case EntryState::Phase::fetch_wait:
    case EntryState::Phase::progress:
        return true;
    default:
        return false;
    }
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

void expansion_entry_show_on_screen(Runtime& runtime, bool on_screen) {
    runtime.expansion_entry_show_on_screen(on_screen);
}

bool expansion_entry_busy(const Runtime& runtime) {
    return runtime.expansion_entry_busy();
}

const oa::data::languages::Language& expansion_entry_language(const Runtime& runtime) {
    return runtime.shown_language();
}

} // namespace oa::app

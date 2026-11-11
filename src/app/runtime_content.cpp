// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The content service and the download queue the runtime owns from start to quit.
#include "oa/app/runtime.hpp"

#include "map_packs.hpp"

#include "oa/app/content/downloads.hpp"
#include "oa/app/content/service.hpp"
#include "oa/app/content/settings.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/package_install/inbox.hpp"
#include "oa/app/package_install/origin.hpp"
#include "oa/app/package_install/replay_release.hpp"
#include "oa/platform/preferences.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <exception>
#include <filesystem>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <ostream>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

/// Writes one content line to the error stream.
///
/// @param line the line, without a line end
void content_log(void*, std::string_view line) {
    std::cerr << "open-annihilation: content: " << line << '\n';
}

/// Tells whether two texts are the same, ignoring ASCII case.
///
/// @param left one text
/// @param right the other
/// @return true when they match
bool same_ascii(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto left_byte = static_cast<unsigned char>(left[index]);
        const auto right_byte = static_cast<unsigned char>(right[index]);
        if (std::tolower(left_byte) != std::tolower(right_byte))
            return false;
    }
    return true;
}

/// The built-in registries folder beside the program, or empty.
///
/// @return the folder; empty when the program's directory is not known
std::filesystem::path builtin_registries_folder() {
    // SDL caches this string for the process. The caller does not free it.
    const char* const base = SDL_GetBasePath();
    if (base == nullptr)
        return {};
    return path_from_utf8(base) / path_from_utf8(content::builtin_registries_folder_name);
}

/// The form the installer's outcomes use for a path.
///
/// Absolute, then canonical as far as the path exists, then lexically normal,
/// so the file handed over and the outcome name the same path.
///
/// @param file the path
/// @return its form
std::filesystem::path compared_path(const std::filesystem::path& file) {
    std::error_code error;
    std::filesystem::path form = std::filesystem::absolute(file, error);
    if (error)
        form = file;
    std::filesystem::path canonical = std::filesystem::weakly_canonical(form, error);
    if (!error)
        form = std::move(canonical);
    return form.lexically_normal();
}

} // namespace

struct Runtime::ContentState {
    content::Service service;
    /// The player's own folder, where a replay's release is kept beside Mods.
    std::filesystem::path player_folder;
    bool developer_mode = false;
    std::unique_ptr<content::Downloads> downloads;
    std::string language;
    std::mutex handed_mutex;
    /// A file handed to the installer, and the queue item it belongs to.
    std::vector<std::pair<std::filesystem::path, uint64_t>> handed;
    /// Failed items whose missing release has already refreshed the catalogues.
    std::vector<uint64_t> offered_refreshed;

    /// Makes the service from options already filled in.
    ///
    /// @param options the service's options
    /// @param developer Developer mode at the moment the service is made
    ContentState(content::ServiceOptions options, bool developer)
        : service(std::move(options)), developer_mode(developer) {}

    /// Gives a finished download to the installer and remembers which item it was.
    ///
    /// Called on the download worker. It does not call back into the queue.
    ///
    /// @param context the content state
    /// @param item the queue item
    /// @param file the package file
    /// @param origin where it came from, and why the player asked
    /// @return true when the installer took the file
    static bool hand_over(
        void* context,
        uint64_t item,
        const std::filesystem::path& file,
        const content::InstallOrigin& origin
    ) {
        auto* state = static_cast<ContentState*>(context);
        oa::app::package_install::Origin record;
        record.kind = oa::app::package_install::OriginKind::catalogue;
        record.registry = origin.registry;
        record.catalogue_id = origin.key;
        record.release = origin.release;
        record.sha256 = origin.sha256;
        if (origin.beside) {
            // Beside the release in use. The ordinary installer would ask to
            // replace that folder, or would list the copy as an update.
            std::string reason;
            const std::optional<std::filesystem::path> kept =
                oa::app::package_install::install_replay_release(
                    state->player_folder, file, record, &reason
                );
            if (!kept)
                std::cerr << "open-annihilation: content: " << reason << '\n';
            return kept.has_value();
        }
        oa::app::package_install::post_package_file(file, record);
        std::lock_guard<std::mutex> guard(state->handed_mutex);
        state->handed.emplace_back(compared_path(file), item);
        return true;
    }

    /// Passes each registry's stored install ID to the queue. None is made.
    ///
    /// @param values the preference values
    void pass_install_ids(const std::map<std::string, std::string>& values) {
        if (!downloads)
            return;
        const std::shared_ptr<const content::Snapshot> snapshot = service.snapshot();
        if (!snapshot)
            return;
        for (const content::RegistryView& view : snapshot->registries) {
            const content::InstallId id =
                content::read_install_id(values, view.registry.descriptor.id);
            std::optional<std::string> held;
            if (id.state == content::InstallIdState::on)
                held = id.text;
            downloads->set_install_id(view.registry.descriptor.id, std::move(held));
        }
    }
};

/// The install outcome that matches what the installer reported.
///
/// @param result what became of the package
/// @return the queue's outcome
static content::InstallOutcome install_outcome(oa::app::package_install::OutcomeResult result) {
    switch (result) {
    case oa::app::package_install::OutcomeResult::installed:
        return content::InstallOutcome::installed;
    case oa::app::package_install::OutcomeResult::refused:
        return content::InstallOutcome::refused;
    case oa::app::package_install::OutcomeResult::failed:
        return content::InstallOutcome::failed;
    case oa::app::package_install::OutcomeResult::set_aside:
        return content::InstallOutcome::set_aside;
    }
    return content::InstallOutcome::failed;
}

void Runtime::destroy_content_state(ContentState* state) noexcept {
    delete state;
}

void Runtime::start_content() {
    if (content_)
        return;
    content::ServiceOptions options;
    if (options_.data_dir) {
        options.data_folder = *options_.data_dir;
    } else {
        try {
            options.data_folder = oa::platform::preferences::data_directory();
        } catch (const std::exception& error) {
            std::cerr << "open-annihilation: content: " << error.what() << '\n';
        }
    }
    options.player_folder = user_folder();
    options.builtin_folder = builtin_registries_folder();
    options.developer_mode = developer_mode();
    options.automatic = !options_.headless_check && !options_.unattended;
    options.check = content::read_check_for_updates(preference_values_);
    options.log = content_log;
    const bool developer = options.developer_mode;
    const std::filesystem::path data_folder = options.data_folder;
    const std::filesystem::path player_folder = options.player_folder;
    content_.reset(new ContentState(std::move(options), developer));
    content_->player_folder = player_folder;
    content_->service.start();
    content::DownloadsOptions queue_options;
    queue_options.data_folder = data_folder;
    queue_options.language = std::string(shown_language().tag);
    queue_options.handoff = ContentState::hand_over;
    queue_options.handoff_context = content_.get();
    queue_options.log = content_log;
    content_->language = queue_options.language;
    content_->downloads = std::make_unique<content::Downloads>(std::move(queue_options));
    content_->pass_install_ids(preference_values_);
    const std::shared_ptr<const content::Snapshot> snapshot = content_->service.snapshot();
    if (snapshot)
        content_->downloads->start(*snapshot);
    note_pack_catalogue();
}

void Runtime::tick_content() {
    if (!content_ || !content_->downloads)
        return;
    const bool developer = developer_mode();
    if (developer != content_->developer_mode) {
        content_->developer_mode = developer;
        content_->service.set_developer_mode(developer);
    }
    content_->downloads->set_match_running(match_ != nullptr);
    const std::string tag(shown_language().tag);
    if (tag != content_->language) {
        content_->language = tag;
        content_->downloads->set_language(tag);
    }
    for (const oa::app::package_install::PackageOutcome& outcome :
         oa::app::package_install::take_package_outcomes()) {
        const std::filesystem::path form = compared_path(outcome.file);
        std::optional<uint64_t> item;
        {
            std::lock_guard<std::mutex> guard(content_->handed_mutex);
            for (auto it = content_->handed.begin(); it != content_->handed.end(); ++it) {
                if (it->first == form) {
                    item = it->second;
                    content_->handed.erase(it);
                    break;
                }
            }
        }
        if (!item)
            continue;
        content_->downloads->report_install(*item, install_outcome(outcome.result), outcome.reason);
    }
    std::vector<uint64_t> still_offered;
    for (const content::DownloadView& item : content_->downloads->view()) {
        if (item.state != content::DownloadState::failed ||
            item.failure != content::DownloadFailure::not_offered)
            continue;
        const bool seen =
            std::find(
                content_->offered_refreshed.begin(), content_->offered_refreshed.end(), item.item
            ) != content_->offered_refreshed.end();
        if (!seen)
            content_->service.refresh(content::RefreshReason::check_now);
        still_offered.push_back(item.item);
    }
    content_->offered_refreshed = std::move(still_offered);
    note_pack_catalogue();
}

std::optional<uint64_t> Runtime::content_catalogue_generation() const {
    if (content_ == nullptr)
        return std::nullopt;
    const std::shared_ptr<const content::Snapshot> snapshot = content_->service.snapshot();
    if (snapshot == nullptr)
        return std::nullopt;
    return snapshot->generation;
}

Runtime::CatalogueMapTable Runtime::catalogue_map_table(const PackMap& map) const {
    CatalogueMapTable table;
    table.unlisted = true;
    if (content_ == nullptr)
        return table;
    const std::shared_ptr<const content::Snapshot> snapshot = content_->service.snapshot();
    if (snapshot == nullptr)
        return table;
    const oa::data::catalogue::Package* package = nullptr;
    for (const content::Entry& entry : snapshot->entries) {
        if (entry.package == nullptr || entry.package->kind != oa::data::catalogue::Kind::oamap)
            continue;
        if (!same_ascii(entry.registry, map.registry) || !same_ascii(entry.package->id, map.id) ||
            entry.package->release != map.release)
            continue;
        package = entry.package;
        break;
    }
    if (package == nullptr)
        return table;
    table.unlisted = false;
    const oa::data::catalogue::MapEntry* found = nullptr;
    for (const oa::data::catalogue::MapEntry& listed : package->maps) {
        if (listed.map == map.name) {
            found = &listed;
            break;
        }
        if (found == nullptr && listed.stem == map.stem)
            found = &listed;
    }
    if (found == nullptr)
        return table;
    table.named = true;
    table.rows = found->compatible;
    return table;
}

Runtime::CatalogueMapFit
Runtime::catalogue_map_fits(const PackMap& map, std::string_view key) const {
    const CatalogueMapTable table = catalogue_map_table(map);
    if (table.unlisted)
        return CatalogueMapFit::unlisted;
    // The release names its maps. One it does not name is left out, whatever
    // the key: the catalogue has said which maps this release holds.
    if (!table.named)
        return CatalogueMapFit::unfit;
    // An empty key, or a key this map's table never recorded, was not
    // checked when the pack was published. The files decide.
    if (key.empty())
        return CatalogueMapFit::unchecked;
    for (const auto& [game, fitting] : table.rows)
        if (game == key)
            return fitting ? CatalogueMapFit::fits : CatalogueMapFit::unfit;
    return CatalogueMapFit::unchecked;
}

content::Service& Runtime::content_service() {
    if (!content_)
        start_content();
    return content_->service;
}

std::optional<std::string> Runtime::content_install_id(std::string_view registry) {
    bool changed = false;
    std::optional<std::string> id =
        content::ensure_install_id(preference_values_, registry, changed);
    if (changed) {
        preferences_dirty_ = true;
        flush_preferences();
    }
    return id;
}

std::optional<uint64_t> Runtime::queue_download(
    std::string_view registry,
    std::string_view key,
    content::DownloadReason reason,
    std::string* why
) {
    content::Downloads& downloads = content_downloads();
    const std::shared_ptr<const content::Snapshot> snapshot = content_->service.snapshot();
    if (!snapshot) {
        if (why != nullptr)
            *why = "the catalogue has no such package";
        return std::nullopt;
    }
    std::optional<content::DownloadTarget> target =
        content::download_target(*snapshot, registry, key, why);
    if (!target)
        return std::nullopt;
    if (target->install_id == data::registry::InstallIdUse::required) {
        const std::optional<std::string> id = content_install_id(registry);
        if (!id) {
            if (why != nullptr)
                *why = content::failure_text(content::DownloadFailure::install_id_off);
            return std::nullopt;
        }
        downloads.set_install_id(registry, id);
    }
    return downloads.queue(std::move(*target), reason, why);
}

content::Downloads& Runtime::content_downloads() {
    if (!content_)
        start_content();
    return *content_->downloads;
}

void Runtime::pause_content_for_match() {
    if (!content_)
        start_content();
    if (content_ && content_->downloads)
        content_->downloads->set_match_running(true);
}

void Runtime::content_install_ids_changed() {
    if (!content_)
        start_content();
    if (content_)
        content_->pass_install_ids(preference_values_);
}

std::filesystem::path Runtime::content_downloads_folder() const {
    std::filesystem::path data;
    if (options_.data_dir) {
        data = *options_.data_dir;
    } else {
        try {
            data = oa::platform::preferences::data_directory();
        } catch (const std::exception&) {
            return {};
        }
    }
    if (data.empty())
        return {};
    return content::downloads_folder(data);
}

content::EmptyResult Runtime::empty_content_downloads() {
    const std::filesystem::path folder = content_downloads_folder();
    std::vector<std::filesystem::path> in_use;
    if (content_ && content_->downloads)
        in_use = content_->downloads->files_in_use();
    if (folder.empty())
        return {};
    return content::empty_downloads(folder, in_use);
}

} // namespace oa::app

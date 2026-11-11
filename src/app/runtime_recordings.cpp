// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The content a recording or a film was made with. The recording and the
// film's frames are unchanged. The note sits beside the recording, or in the
// film's folder, so an older build still plays them.

#include "oa/app/content/updates.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/runtime.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/base/sha256.hpp"
#include "oa/data/mod_profile.hpp"
#include "oa/data/mutators/loadout.hpp"
#include "oa/data/saves/content_record.hpp"

#include <fstream>
#include <string>
#include <vector>

namespace oa::app {
namespace {

namespace saves = oa::data::saves;

/// Writes a note. A failure leaves no note, and the recording or film still stands.
///
/// @param path the note
/// @param text the note's text
void write_note(const fs::path& path, const std::string& text) {
    if (text.empty())
        return;
    std::ofstream out(path, std::ios::binary);
    if (!out)
        return;
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

/// The note this run writes.
///
/// @param choice the content this run plays
/// @param mod the mod's name, or empty
/// @param played the mutators the match is playing, in list order
/// @param brains the brains that played, in player order
/// @return the note's text
std::string note_text(
    const oa::data::content_bundles::ContentChoice& choice,
    std::string_view mod,
    const std::vector<saves::ContentMutator>& played,
    const std::vector<saves::ContentBrain>& brains
) {
    saves::ContentRecord record =
        saves::content_record_from(choice.base_game, choice.expansions, std::string(mod));
    record.mutators = played;
    record.brains = brains;
    return saves::content_record_text(record);
}

/// The mod's name as the player sees it.
///
/// @param profile the mounted mod, or null
/// @return its name, its id, or empty
std::string mod_label(const oa::data::mod_profile::ModProfile* profile) {
    if (profile == nullptr || profile->id.empty())
        return {};
    return profile->name.empty() ? profile->id : profile->name;
}

} // namespace

void Runtime::note_content_beside(const fs::path& path) const {
    if (path.empty())
        return;
    fs::path note = path;
    note += saves::content_note_suffix;
    write_note(note, content_note_text());
}

std::string Runtime::content_note_text() const {
    return note_text(
        content_choice(), mod_label(mod_profile()), played_mutators(), played_brains()
    );
}

oa::data::saves::ContentRecord Runtime::played_content_record() const {
    const oa::data::content_bundles::ContentChoice choice = content_choice();
    saves::ContentRecord record =
        saves::content_record_from(choice.base_game, choice.expansions, mod_label(mod_profile()));
    record.mutators = played_mutators();
    record.brains = played_brains();
    return record;
}

bool Runtime::person_plays() const noexcept {
    return !options_.unattended && !options_.headless_check;
}

bool Runtime::records_games_by_setting() const {
    return person_plays() && replays_record_setting(preference_values_);
}

oa::sim::match_runtime::Match* Runtime::running_match() noexcept {
    return match_.get();
}

const oa::sim::match_runtime::Match* Runtime::running_match() const noexcept {
    return match_.get();
}

oa::data::campaign::SessionKind Runtime::running_session_kind() const {
    return match_session_kind();
}

const std::string& Runtime::played_map_name() const {
    return selected_map_name_runtime_;
}

Runtime::InstalledPackageFacts Runtime::installed_package(
    std::string_view kind, std::string_view id, std::string_view version
) const {
    InstalledPackageFacts facts;
    if (user_folder_.empty())
        return facts;
    for (const oa::app::content::InstalledPackage& package :
         oa::app::content::list_installed(user_folder_)) {
        if (package.kind == nullptr || package.kind->name != kind || package.key != id)
            continue;
        if (!version.empty() && package.version != version)
            continue;
        facts.found = true;
        facts.version = package.version;
        facts.folder = package.folder;
        if (package.origin.has_value() && package.origin->sha256.has_value()) {
            const auto hex = oa::base::sha256::to_hex(*package.origin->sha256);
            facts.sha256.assign(hex.data(), hex.size());
        }
        break;
    }
    return facts;
}

void Runtime::note_film_content(const fs::path& folder) {
    if (folder.empty())
        return;
    const std::string key = path_to_utf8(folder);
    if (film_content_noted_ == key)
        return;
    write_note(
        folder / std::string(saves::film_content_name),
        note_text(content_choice(), mod_label(mod_profile()), played_mutators(), played_brains())
    );
    film_content_noted_ = key;
}

std::string Runtime::folder_digest_text(const fs::path& folder) const {
    // Each folder is read again only when it changes.
    static oa::data::mutators::FolderDigests digests;
    std::string problem;
    const auto digest = digests.digest(folder, &problem);
    if (!digest.has_value()) {
        std::cerr << "open-annihilation: the folder " << path_to_utf8(folder)
                  << " cannot be digested for the recording: " << problem << '\n';
        return {};
    }
    const auto hex = oa::base::sha256::to_hex(*digest);
    return std::string(hex.data(), hex.size());
}

std::string Runtime::played_mod_digest() const {
    const auto* profile = mod_profile();
    if (profile == nullptr)
        return {};
    const InstalledPackageFacts installed =
        installed_package("oamod", profile->id, profile->version);
    // The folder this run layered over the game's files: the installed package's own, or
    // another copy of the mod.
    std::error_code error;
    const bool another_copy =
        !options_.layered_mod_folder.empty() &&
        (!installed.found || !fs::equivalent(options_.layered_mod_folder, installed.folder, error));
    if (!another_copy && !installed.sha256.empty())
        return installed.sha256;
    const fs::path& folder = another_copy ? options_.layered_mod_folder : installed.folder;
    if (!folder.empty()) {
        std::string text = folder_digest_text(folder);
        if (!text.empty())
            return text;
    }
    // A profile that no folder holds is named by its own hash.
    const auto hex = oa::base::sha256::to_hex(profile->full_hash);
    return std::string(hex.data(), hex.size());
}

} // namespace oa::app

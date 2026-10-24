// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The content a recording or a film was made with. The recording and the
// film's frames are unchanged. The note sits beside the recording, or in the
// film's folder, so an older build still plays them.

#include "oa/app/game_directory.hpp"
#include "oa/app/runtime.hpp"
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
    write_note(
        note,
        note_text(content_choice(), mod_label(mod_profile()), played_mutators(), played_brains())
    );
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

} // namespace oa::app

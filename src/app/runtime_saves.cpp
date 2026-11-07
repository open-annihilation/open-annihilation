// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The content a save was made with, the question shown when loading one that
// needs other content, and the load the next start finishes after TURN ON
// AND LOAD.

#include "engine_settings_state.hpp"
#include "oa_layer.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/app/runtime.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/saves/content_record.hpp"
#include "oa/ui/frontend_dialogs/content_question.hpp"
#include "oa/ui/kit/components_more.hpp"

#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace saves = oa::data::saves;
namespace dialogs = oa::ui::frontend_dialogs;

/// Reads a note file. A missing file is a file from before the note.
///
/// @param path the note
/// @return its text, or empty when it cannot be read
std::string read_note(const fs::path& path) {
    std::ifstream in(path);
    if (!in)
        return {};
    std::string text;
    text.reserve(512);
    std::string line;
    while (text.size() < 65536 && std::getline(in, line)) {
        text += line;
        text += '\n';
    }
    return text;
}

/// The note beside a save or a recording.
///
/// @param path the save or the recording
/// @return the note's path
fs::path beside(const fs::path& path) {
    fs::path note = path;
    note += saves::content_note_suffix;
    return note;
}

/// Views of ids a vector owns.
///
/// @param ids the ids
/// @return views of them
std::vector<std::string_view> views_of(const std::vector<std::string>& ids) {
    std::vector<std::string_view> views;
    views.reserve(ids.size());
    for (const std::string& id : ids)
        views.emplace_back(id);
    return views;
}

/// The name a bundle shows, in the language shown.
///
/// @param id the bundle id
/// @return the name; an unknown id as it was given
std::string shown_bundle_name(std::string_view id) {
    return std::string(oa::data::languages::interface_text(saves::bundle_english_name(id)));
}

/// Joins the names of bundles, in the language shown.
///
/// @param ids the bundle ids
/// @return the names, joined with ", "
std::string shown_bundle_names(const std::vector<std::string>& ids) {
    std::string line;
    for (const std::string& id : ids) {
        if (!line.empty())
            line += ", ";
        line += shown_bundle_name(id);
    }
    return line;
}

/// Joins the names of the note's expansions, in the language shown.
///
/// @param record the note
/// @return the names, or None when there are none
std::string expansion_line(const saves::ContentRecord& record) {
    if (!record.present || record.expansions.empty())
        return std::string(oa::data::languages::interface_text("None"));
    return shown_bundle_names(record.expansions);
}

/// One column of the comparison.
///
/// @param record the note
/// @return the names the question shows
dialogs::ContentColumn column_of(const saves::ContentRecord& record) {
    dialogs::ContentColumn column;
    column.base_game = record.present ? shown_bundle_name(record.base_game) : std::string{};
    column.expansions = expansion_line(record);
    column.mod = record.mod.empty() ? std::string(oa::data::languages::interface_text("No Mod"))
                                    : record.mod;
    return column;
}

/// The note this run would write.
///
/// @param choice the content this run plays
/// @param mod the mod's name, or empty
/// @return the note
saves::ContentRecord
current_record(const oa::data::content_bundles::ContentChoice& choice, std::string_view mod) {
    return saves::content_record_from(choice.base_game, choice.expansions, std::string(mod));
}

/// The mod's name as the player sees it.
///
/// @param profile the mounted mod, or null
/// @return its name, its id, or empty
std::string mod_name(const oa::data::mod_profile::ModProfile* profile) {
    if (profile == nullptr || profile->id.empty())
        return {};
    return profile->name.empty() ? profile->id : profile->name;
}

/// What loading `path` asks.
///
/// @param path the save, the recording, or a film folder
/// @param film true when `path` is a film folder
/// @param choice the content this run plays
/// @param game_dir the game's folder
/// @return the gap
saves::ContentGap gap_for(
    const fs::path& path,
    bool film,
    const oa::data::content_bundles::ContentChoice& choice,
    const fs::path& game_dir
) {
    const fs::path note = film ? path / std::string(saves::film_content_name) : beside(path);
    const saves::ContentRecord saved = saves::content_record_from_text(read_note(note));
    const std::vector<std::string> copied = copied_content_bundles(game_dir);
    const std::vector<std::string_view> copied_ids = views_of(copied);
    saves::ContentGap gap = saves::content_gap(saved, choice, copied_ids);
    // The question names the bundles in the language shown.
    if (!gap.focus_ids.empty())
        gap.focus = shown_bundle_names(gap.focus_ids);
    return gap;
}

/// The first sentence of the question.
///
/// @param title the file's name
/// @param gap what loading asks
/// @return the sentence, or empty when nothing is asked
std::string sentence_of(std::string_view title, const saves::ContentGap& gap) {
    if (!gap.needed)
        return {};
    const auto question = dialogs::content_switch_question({}, {}, title, gap);
    return question.paragraphs.empty() ? std::string{} : question.paragraphs[0].text;
}

/// An `.oademo` replay is compared from its manifest, not from the note beside it.
///
/// @param path the recording
/// @return true when the file is an `.oademo`
bool manifest_replay(const fs::path& path) {
    return path_to_utf8(path.extension()) == ".oademo";
}

} // namespace

std::string Runtime::content_switch_sentence(const fs::path& path) const {
    if (manifest_replay(path))
        return {};
    return sentence_of(
        path_to_utf8(path.filename()), gap_for(path, false, content_choice(), options_.game_dir)
    );
}

bool Runtime::offer_content_switch(const fs::path& path, std::string_view title, bool recording) {
    if (manifest_replay(path))
        return false;
    const auto choice = content_choice();
    const saves::ContentGap gap = gap_for(path, false, choice, options_.game_dir);
    if (!gap.needed)
        return false;
    const saves::ContentRecord saved = saves::content_record_from_text(read_note(beside(path)));
    dialogs::ContentColumn saved_column = column_of(saved);
    dialogs::ContentColumn now_column = column_of(current_record(choice, mod_name(mod_profile())));
    oa::ui::kit::Question question =
        dialogs::content_switch_question(saved_column, now_column, title, gap);
    int32_t turn_on = -1;
    for (int32_t index = 0; index < static_cast<int32_t>(question.buttons.size()); ++index) {
        if (question.buttons[static_cast<std::size_t>(index)].id == dialogs::turn_on_button_id)
            turn_on = index;
    }
    const std::string stored = path_to_utf8(path);
    const bool as_recording = recording;
    const saves::ContentGap kept = gap;
    QuestionScreen::Host answers;
    answers.answer = [this, turn_on, stored, as_recording, kept](int32_t button) {
        if (button != turn_on)
            return true;
        oa::data::content_bundles::ContentChangeResult result;
        result.choice = kept.next;
        result.applied = true;
        if (kept.clears_mod)
            result.mod_cleared = oa::data::content_bundles::ModClearReason::base_game_chosen;
        if (!store_content_change(preference_values_, result))
            return true;
        if (kept.clears_mod && engine_settings_) {
            engine_settings_->current.mod_folder.clear();
            engine_settings_->dropped_mod_folder.clear();
        }
        const std::string key =
            std::string(as_recording ? saves::pending_demo_key : saves::pending_load_key);
        preference_values_[key] = stored;
        preferences_dirty_ = true;
        flush_preferences();
        request_soft_restart();
        return true;
    };
    OaLayer& layer = oa_layer();
    layer.push(std::make_unique<QuestionScreen>(layer, screen_, std::move(question), answers));
    return true;
}

void Runtime::take_pending_content() {
    const auto take = [this](std::string_view key) {
        const auto found = preference_values_.find(std::string(key));
        if (found == preference_values_.end() || found->second.empty())
            return std::string{};
        std::string value = found->second;
        preference_values_.erase(found);
        preferences_dirty_ = true;
        return value;
    };
    const std::string demo = take(saves::pending_demo_key);
    const std::string save = take(saves::pending_load_key);
    if (!demo.empty() || !save.empty())
        flush_preferences();
    pending_demo_path_ = demo;
    pending_save_path_ = save;
}

} // namespace oa::app

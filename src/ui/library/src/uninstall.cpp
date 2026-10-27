// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// UNINSTALL's rules, its question and its notices, written from the
// patterns in text.hpp.
#include "oa/ui/library/uninstall.hpp"

#include "oa/ui/library/prerequisites.hpp"
#include "oa/ui/library/text.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::ui::library {

namespace {

/// Tells whether a queue item has not ended: it waits, downloads, is checked
/// or installed, or waits for the player to leave the mod being played.
///
/// @param phase the item's phase
/// @return true until it has failed or is installed
bool unfinished(QueueItem::Phase phase) {
    return phase != QueueItem::Phase::failed && phase != QueueItem::Phase::installed;
}

/// The name a works-with entry shows: the base game's own words, the name
/// when one is known, otherwise the id as written.
///
/// @param item the works-with entry
/// @param text how texts are looked up
/// @return the name
std::string works_with_name(const WorksWith& item, const TextHooks& text) {
    if (item.id == supported_base)
        return std::string(shown(text, words::base_supported));
    if (!item.name.empty())
        return item.name;
    return item.id;
}

/// Makes a notice of one paragraph and OK, which every key that answers
/// a question answers.
///
/// @param title its title
/// @param body its paragraph
/// @param text how texts are looked up
/// @return the notice
oa::ui::kit::Question
one_button_notice(std::string title, std::string body, const TextHooks& text) {
    oa::ui::kit::Question notice;
    notice.title = std::move(title);
    notice.paragraphs.push_back({std::move(body), false});
    notice.buttons.push_back(
        {std::string(shown(text, words::uninstall_ok)), true, std::string(uninstall_ok_button_id)}
    );
    notice.cancel_button = 0;
    notice.primary_button = 0;
    notice.marked = 0;
    return notice;
}

} // namespace

std::string uninstall_off_reason(const Library& library, const Entry& entry) {
    if (entry.installed == nullptr)
        return {};
    if (entry.id.kind == Kind::map_pack && library.inputs.in_match)
        return std::string(shown(library.text, words::uninstall_off_in_game));
    if (entry.queued != nullptr && unfinished(entry.queued->phase))
        return std::string(shown(library.text, words::uninstall_off_queued));
    return {};
}

UninstallStep uninstall_step(const Library& library, const Entry& entry) {
    if (entry.installed == nullptr || !uninstall_off_reason(library, entry).empty())
        return UninstallStep::off;
    if (entry.id.kind == Kind::mod && entry.installed->playing)
        return UninstallStep::switch_off_first;
    if (entry.id.kind == Kind::language && entry.installed->in_use)
        return UninstallStep::language_first;
    return UninstallStep::ask;
}

oa::ui::kit::Question
uninstall_question(const Entry& entry, std::string_view folder, const TextHooks& text) {
    const std::string name(entry_name(entry));
    const bool kept = entry.installed != nullptr && entry.installed->kept.has_value();
    oa::ui::kit::Question question;
    question.title = filled(text, words::uninstall_title, {{"name", name}});
    question.paragraphs.push_back(
        {filled(
             text,
             kept ? words::uninstall_ask_kept : words::uninstall_ask,
             {{"name", name}, {"folder", folder}}
         ),
         false}
    );
    question.paragraphs.push_back(
        {std::string(
             shown(text, entry.listing != nullptr ? words::uninstall_listed : words::uninstall_own)
         ),
         false}
    );
    question.buttons.push_back(
        {std::string(shown(text, words::action_cancel)),
         false,
         std::string(uninstall_cancel_button_id)}
    );
    question.buttons.push_back(
        {std::string(shown(text, words::action_uninstall)), true, std::string(uninstall_button_id)}
    );
    question.cancel_button = 0;
    question.primary_button = 1;
    question.marked = 0;
    return question;
}

oa::ui::kit::Question uninstall_question(
    const Library& library, const Entry& entry, std::string_view folder, const TextHooks& text
) {
    oa::ui::kit::Question question = uninstall_question(entry, folder, text);
    if (question.paragraphs.empty())
        return question;
    if (entry.id.kind == Kind::key_layout && layout_in_use(library, entry)) {
        question.paragraphs.insert(
            question.paragraphs.end() - 1,
            {filled(
                 text, words::uninstall_layout_in_use, {{"name", std::string(entry_name(entry))}}
             ),
             false}
        );
        return question;
    }
    if (entry.id.kind == Kind::brain && entry.is_default) {
        question.paragraphs.insert(
            question.paragraphs.end() - 1,
            {std::string(shown(text, words::uninstall_default_brain)), false}
        );
        return question;
    }
    if (entry.id.kind != Kind::mutator)
        return question;
    std::vector<std::string> among;
    for (const Installed& item : library.inputs.installed) {
        if (item.kind == Kind::mutator)
            among.push_back(item.key);
    }
    const std::vector<Need> graph = needs_of(library);
    const std::vector<std::string> dependants = dependants_of(graph, among, entry.id.key);
    std::vector<std::string> names;
    for (const std::string& id : dependants) {
        if (id == entry.id.key)
            continue;
        std::string dependant_name = id;
        for (const Entry& other : library.entries) {
            if (other.id.kind == Kind::mutator && other.id.key == id &&
                other.installed != nullptr) {
                dependant_name = std::string(entry_name(other));
                break;
            }
        }
        bool known = false;
        for (const std::string& have : names) {
            if (have == dependant_name) {
                known = true;
                break;
            }
        }
        if (!known)
            names.push_back(std::move(dependant_name));
    }
    if (names.empty())
        return question;
    const std::string paragraph =
        names.size() == 1
            ? filled(text, words::uninstall_needed_by_one, {{"name", names.front()}})
            : filled(text, words::uninstall_needed_by_many, {{"names", join_names(text, names)}});
    question.paragraphs.insert(question.paragraphs.end() - 1, {paragraph, false});
    return question;
}

oa::ui::kit::Question
uninstall_notice(const Entry& entry, UninstallStep step, const TextHooks& text) {
    const std::string name(entry_name(entry));
    const std::string_view pattern = step == UninstallStep::language_first
                                         ? words::uninstall_language
                                         : words::uninstall_playing;
    return one_button_notice(
        filled(text, words::uninstall_title, {{"name", name}}),
        filled(text, pattern, {{"name", name}}),
        text
    );
}

oa::ui::kit::Question
uninstall_failed_notice(std::string_view name, UninstallProblem problem, const TextHooks& text) {
    std::string_view reason;
    switch (problem) {
    case UninstallProblem::busy:
        reason = words::uninstall_failed_busy;
        break;
    case UninstallProblem::changed:
        reason = words::uninstall_failed_changed;
        break;
    case UninstallProblem::link:
        reason = words::uninstall_failed_link;
        break;
    case UninstallProblem::outside:
        reason = words::uninstall_failed_outside;
        break;
    case UninstallProblem::not_moved:
        reason = words::uninstall_failed_not_moved;
        break;
    }
    return one_button_notice(
        filled(text, words::uninstall_title, {{"name", name}}),
        filled(text, words::uninstall_failed, {{"name", name}, {"reason", shown(text, reason)}}),
        text
    );
}

std::string details_note(const Library& library) {
    std::string note = playing_note(library);
    const Entry* entry = selected_entry(library);
    if (entry == nullptr)
        return note;
    const std::string off = uninstall_off_reason(library, *entry);
    if (!off.empty()) {
        if (!note.empty())
            note += ' ';
        note += off;
    }
    const std::string turning = turn_on_note(library);
    if (!turning.empty()) {
        if (!note.empty())
            note += ' ';
        note += turning;
    }
    if (entry->not_compatible) {
        std::vector<std::string> names;
        if (const MutatorFacts* facts = mutator_facts(*entry)) {
            for (const WorksWith& item : facts->works_with)
                names.push_back(works_with_name(item, library.text));
        }
        if (!note.empty())
            note += ' ';
        note += filled(
            library.text, words::note_not_compatible, {{"names", join_names(library.text, names)}}
        );
    }
    if (entry->id.kind == Kind::mutator && entry->state == State::get) {
        const GetPlan plan = get_plan(library, *entry);
        std::string added;
        if (!plan.blocked.empty() && (entry->can_act || plan.blocked != entry->blocked))
            added = plan.blocked;
        else if (plan.blocked.empty() && plan.steps.size() > 1) {
            std::vector<std::string> names;
            for (std::size_t index = 0; index + 1 < plan.steps.size(); ++index)
                names.push_back(plan.steps[index].name);
            added = filled(
                library.text,
                names.size() == 1 ? words::note_downloads_first : words::note_download_first,
                {{"names", join_names(library.text, names)},
                 {"name", std::string(entry_name(*entry))}}
            );
        }
        if (!added.empty()) {
            if (!note.empty())
                note += ' ';
            note += added;
        }
    }
    return note;
}

} // namespace oa::ui::library

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// "Load with the built-in player?": the words are looked up, the places
// filled, and the kit question returned. Nothing here is shown.
#include "oa/ui/brain_picker/prompts.hpp"

#include "oa/ui/brain_picker/text.hpp"
#include "oa/ui/library/text.hpp"

#include <string>
#include <string_view>

namespace oa::ui::brain_picker {
namespace {

namespace kit = oa::ui::kit;
namespace library = oa::ui::library;

/// One button.
///
/// @param caption the words
/// @param accent true for the primary look
/// @param id the name to automation
/// @return the button
kit::QuestionButton button_of(std::string caption, bool accent, std::string_view id) {
    kit::QuestionButton made;
    made.caption = std::move(caption);
    made.accent = accent;
    made.id = std::string(id);
    return made;
}

} // namespace

kit::Question
load_built_in_question(std::span<const MissingBrain> missing, const library::TextHooks& hooks) {
    kit::Question question;
    question.title = std::string(library::shown(hooks, words::load_built_in_title));
    question.paragraphs.reserve(missing.size() + 1);
    for (const MissingBrain& brain : missing) {
        const std::string seat_number = std::to_string(brain.player + 1);
        question.paragraphs.push_back(
            {library::filled(
                 hooks,
                 words::saved_with_missing,
                 {{"name", brain.name}, {"version", brain.version}, {"n", seat_number}}
             ),
             false}
        );
    }
    question.paragraphs.push_back(
        {std::string(library::shown(hooks, words::built_in_takes_seats)), false}
    );
    question.buttons.push_back(
        button_of(std::string(library::shown(hooks, words::back)), false, back_id)
    );
    question.buttons.push_back(button_of(
        std::string(library::shown(hooks, words::load_built_in_button)), true, load_built_in_id
    ));
    question.cancel_button = 0;
    question.primary_button = 1;
    question.marked = 1;
    return question;
}

} // namespace oa::ui::brain_picker

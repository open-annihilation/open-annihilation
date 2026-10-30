// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The four mutator questions: one mutator and several, a download before and
// after it stops, the texts fitting at every class, and a lookup that translates.
#include "oa/test/check.hpp"
#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/mutator_picker/prompts.hpp"
#include "oa/ui/mutator_picker/text.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace {

namespace picker = oa::ui::mutator_picker;
namespace kit = oa::ui::kit;

/// The paragraph texts, in order.
///
/// @param question the question
/// @return the texts
std::vector<std::string> paragraphs_of(const kit::Question& question) {
    std::vector<std::string> texts;
    for (const kit::Paragraph& paragraph : question.paragraphs)
        texts.push_back(paragraph.text);
    return texts;
}

/// The buttons' names to automation.
///
/// @param question the question
/// @return the names, including the body
std::vector<std::string> names_of(const kit::Question& question) {
    const kit::DisplayList list = kit::question_list(question, nullptr);
    std::vector<std::string> names;
    for (const kit::Control& control : list.controls)
        names.push_back(control.name);
    return names;
}

/// Requires the question's text to fit at every class, within that class's height.
///
/// @param question the question
void fits(const kit::Question& question) {
    for (const kit::SizeClass size_class :
         {kit::SizeClass::compact, kit::SizeClass::regular, kit::SizeClass::large}) {
        kit::Question sized = question;
        sized.size_class = size_class;
        const kit::PlacedQuestion placed = kit::place_question(sized, nullptr);
        OA_CHECK(!placed.cut);
        OA_CHECK(placed.height <= kit::metrics_of(size_class).greatest_notice_height);
    }
}

/// One mutator that stopped, and several.
void start_without() {
    const picker::Failed slip{"Slip", "the ridge gave way"};
    const kit::Question one =
        picker::start_without_question(std::span<const picker::Failed>(&slip, 1));
    OA_CHECK(one.title == "Start without Slip?");
    OA_CHECK(
        paragraphs_of(one) ==
        std::vector<std::string>(
            {"Slip stopped before the game could start. Nothing it changed is kept, and the "
             "other mutators are unaffected."}
        )
    );
    OA_CHECK(one.failure == "Slip: the ridge gave way");
    OA_CHECK(one.progress < 0);
    OA_CHECK(one.buttons.size() == 2);
    OA_CHECK(
        one.buttons[0].caption == "Back" && one.buttons[0].id == "back" && !one.buttons[0].accent
    );
    OA_CHECK(
        one.buttons[1].caption == "Start without it" && one.buttons[1].id == "start-without" &&
        one.buttons[1].accent
    );
    OA_CHECK(one.cancel_button == 0 && one.primary_button == 1 && one.marked == 1);
    OA_CHECK(kit::question_word_of(one) == "prompt");
    OA_CHECK(
        names_of(one) ==
        std::vector<std::string>({"prompt.back", "prompt.start-without", "prompt.body"})
    );
    fits(one);

    const picker::Failed lantern{"Lantern", "the wick was wet"};
    const picker::Failed unnamed{"", "the list was refused"};
    const picker::Failed many_failures[] = {slip, lantern, unnamed};
    const kit::Question many = picker::start_without_question(many_failures);
    OA_CHECK(many.title == "Start without them?");
    OA_CHECK(
        paragraphs_of(many)[0].find("Slip and Lantern stopped before the game could start") !=
        std::string::npos
    );
    OA_CHECK(
        many.failure == "Slip: the ridge gave way Lantern: the wick was wet the list was refused"
    );
    OA_CHECK(
        many.buttons[1].caption == "Start without them" && many.buttons[1].id == "start-without"
    );
    OA_CHECK(many.marked == 1 && many.primary_button == 1 && many.cancel_button == 0);
    fits(many);
}

/// The crash question, for one name and for several.
void forget_set() {
    const std::string lantern = "Lantern";
    const kit::Question one =
        picker::forget_set_question(std::span<const std::string>(&lantern, 1));
    OA_CHECK(one.title == "Forget the last set?");
    OA_CHECK(paragraphs_of(one).size() == 2);
    OA_CHECK(paragraphs_of(one)[0].find("Lantern") != std::string::npos);
    OA_CHECK(paragraphs_of(one)[0].find("nothing is uninstalled") != std::string::npos);
    OA_CHECK(paragraphs_of(one)[1] == "The game's log names them, so their authors can be told.");
    OA_CHECK(one.failure.empty());
    OA_CHECK(
        one.buttons[0].caption == "Keep it" && one.buttons[0].id == "keep-it" &&
        !one.buttons[0].accent
    );
    OA_CHECK(
        one.buttons[1].caption == "Forget it" && one.buttons[1].id == "forget-it" &&
        one.buttons[1].accent
    );
    OA_CHECK(one.cancel_button == 0 && one.primary_button == 1 && one.marked == 1);
    OA_CHECK(
        names_of(one) ==
        std::vector<std::string>({"prompt.keep-it", "prompt.forget-it", "prompt.body"})
    );
    fits(one);

    const std::string names[] = {"Lantern", "Slip"};
    const kit::Question two = picker::forget_set_question(names);
    OA_CHECK(paragraphs_of(two)[0].find("Lantern and Slip") != std::string::npos);
    fits(two);
}

/// The download question before a failure and after one.
void get_mutators() {
    picker::GetProgress progress;
    progress.name = "Lantern";
    progress.index = 1;
    progress.total = 2;
    progress.progress = 400;
    const kit::Question running = picker::get_mutators_question(progress);
    OA_CHECK(running.title == "Get mutators");
    OA_CHECK(paragraphs_of(running) == std::vector<std::string>({"Downloading Lantern · 1 of 2"}));
    OA_CHECK(running.progress == 400);
    OA_CHECK(running.failure.empty());
    OA_CHECK(running.buttons.size() == 1);
    OA_CHECK(running.buttons[0].caption == "Cancel" && running.buttons[0].id == "cancel");
    OA_CHECK(running.cancel_button == 0 && running.primary_button == 0 && running.marked == 0);
    OA_CHECK(names_of(running) == std::vector<std::string>({"prompt.cancel", "prompt.body"}));
    fits(running);

    progress.problem = "the ridge did not answer.";
    const kit::Question stopped = picker::get_mutators_question(progress);
    OA_CHECK(stopped.failure == "the ridge did not answer. Retry tries again.");
    OA_CHECK(stopped.buttons.size() == 2);
    OA_CHECK(
        stopped.buttons[1].caption == "Retry" && stopped.buttons[1].id == "retry" &&
        stopped.buttons[1].accent
    );
    OA_CHECK(stopped.cancel_button == 0 && stopped.primary_button == 1 && stopped.marked == 1);
    OA_CHECK(stopped.progress == 400);
    OA_CHECK(
        names_of(stopped) ==
        std::vector<std::string>({"prompt.cancel", "prompt.retry", "prompt.body"})
    );
    fits(stopped);
}

/// Machines whose results differ: one, and several, including a missing release.
void did_not_start() {
    const picker::Differs kai{"Kai", "Lantern", "2", "1", "2"};
    const kit::Question one =
        picker::did_not_start_question(std::span<const picker::Differs>(&kai, 1));
    OA_CHECK(one.title == "The game did not start");
    OA_CHECK(paragraphs_of(one).size() == 2);
    OA_CHECK(
        paragraphs_of(one)[0].find("Kai's mutators gave a different result") != std::string::npos
    );
    OA_CHECK(paragraphs_of(one)[1] == "Kai's GET MUTATORS button fetches release 2.");
    OA_CHECK(one.failure == "Lantern: release 2 here, release 1 on Kai's machine.");
    OA_CHECK(one.buttons.size() == 1);
    OA_CHECK(one.buttons[0].caption == "OK" && one.buttons[0].id == "ok" && one.buttons[0].accent);
    OA_CHECK(one.cancel_button == 0 && one.primary_button == 0 && one.marked == 0);
    OA_CHECK(names_of(one) == std::vector<std::string>({"prompt.ok", "prompt.body"}));
    fits(one);

    const picker::Differs missing{"Ren", "Slip", "2", "", ""};
    const picker::Differs skipped{"Kai", "", "2", "1", ""};
    const picker::Differs several[] = {kai, missing, skipped};
    const kit::Question many = picker::did_not_start_question(several);
    OA_CHECK(paragraphs_of(many)[0].find("Kai and Ren's mutators") != std::string::npos);
    OA_CHECK(
        many.failure ==
        "Lantern: release 2 here, release 1 on Kai's machine. Slip: Ren does not have it."
    );
    OA_CHECK(paragraphs_of(many).size() == 2);
    fits(many);
}

/// A lookup replaces the English pattern before its places are filled.
void translated() {
    const picker::Failed slip{"Slip", "the ridge gave way"};
    oa::ui::library::TextHooks hooks;
    hooks.shown = [](void*, std::string_view english) -> std::string_view {
        if (english == picker::words::start_without_one)
            return "Sans {name} ?";
        if (english == picker::words::failure_line)
            return "{name} — {reason}";
        return english;
    };
    const kit::Question asked =
        picker::start_without_question(std::span<const picker::Failed>(&slip, 1), hooks);
    OA_CHECK(asked.title == "Sans Slip ?");
    OA_CHECK(asked.failure == "Slip — the ridge gave way");
}

} // namespace

int main() {
    start_without();
    forget_set();
    get_mutators();
    did_not_start();
    translated();
    return oa::test::check_exit_status();
}

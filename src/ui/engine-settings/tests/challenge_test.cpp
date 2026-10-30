// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The download check: its words for each status, with and without a browser,
// its buttons, the focus it starts on, Enter and Escape, a finger within
// reach, and every part inside the window at Compact, Regular and Large.

#include "oa/ui/engine_settings/challenge.hpp"

#include "oa/test/check.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/theme.hpp"

#include <cstdint>
#include <string>
#include <vector>

namespace {

namespace settings = oa::ui::engine_settings;
namespace kit = oa::ui::kit;

/// Returns a waiting check for a made-up registry.
///
/// @param browser true when this computer can open the page
/// @return the challenge
settings::ChallengeWindow waiting_window(bool browser) {
    settings::ChallengeWindow window;
    window.registry_name = "Example Registry";
    window.code = "HXQ7-4KP2";
    window.address = "downloads.example.net/verify";
    window.status = settings::ChallengeWindow::Status::waiting;
    window.can_open_browser = browser;
    return window;
}

/// Returns the control of a name.
///
/// @param list the display list
/// @param name the name
/// @return the control; null when the list has none
const kit::Control* named(const kit::DisplayList& list, const char* name) {
    return kit::control_of(list, kit::control_named(list, name));
}

/// Returns the text item that draws a control.
///
/// @param list the display list
/// @param id the control
/// @return the item; null when none draws it as text
const kit::Item* text_of(const kit::DisplayList& list, kit::ControlId id) {
    for (const kit::Item& item : list.items)
        if (item.role == kit::Role::text && item.control == id)
            return &item;
    return nullptr;
}

void test_words() {
    const settings::ChallengeText waiting = settings::challenge_text(waiting_window(true));
    OA_CHECK(waiting.title == "Downloads");
    OA_CHECK(waiting.heading == "Check that you're a person");
    OA_CHECK(
        waiting.body ==
        "Before more downloads, Example Registry needs to check this isn't an automated program. "
        "Open the check on this computer, or go to downloads.example.net/verify on any device "
        "and enter:"
    );
    OA_CHECK(waiting.code == "HXQ7-4KP2");
    OA_CHECK(waiting.status == "Waiting for the check. This updates by itself.");
    OA_CHECK(waiting.buttons.size() == 2);
    OA_CHECK(waiting.buttons[0].caption == "Cancel");
    OA_CHECK(waiting.buttons[1].caption == "Open the check");

    const settings::ChallengeText phone = settings::challenge_text(waiting_window(false));
    OA_CHECK(
        phone.body ==
        "Before more downloads, Example Registry needs to check this isn't an automated program. "
        "Go to downloads.example.net/verify on another device and enter:"
    );
    OA_CHECK(phone.buttons.size() == 1);
    OA_CHECK(phone.buttons[0].caption == "Cancel");

    settings::ChallengeWindow expired = waiting_window(true);
    expired.status = settings::ChallengeWindow::Status::expired;
    const settings::ChallengeText ran_out = settings::challenge_text(expired);
    OA_CHECK(ran_out.body == waiting.body);
    OA_CHECK(ran_out.status == "The check ran out. Try again for a new code.");
    OA_CHECK(ran_out.buttons.size() == 2);
    OA_CHECK(ran_out.buttons[0].caption == "Cancel");
    OA_CHECK(ran_out.buttons[1].caption == "Try again");

    settings::ChallengeWindow passed = waiting_window(true);
    passed.status = settings::ChallengeWindow::Status::passed;
    const settings::ChallengeText done = settings::challenge_text(passed);
    OA_CHECK(done.body == waiting.body);
    OA_CHECK(done.status.empty());
    OA_CHECK(done.buttons.size() == 1);
    OA_CHECK(done.buttons[0].caption == "Cancel");

    settings::ChallengeWindow passed_phone = waiting_window(false);
    passed_phone.status = settings::ChallengeWindow::Status::passed;
    const settings::ChallengeText done_phone = settings::challenge_text(passed_phone);
    OA_CHECK(done_phone.body == phone.body);
    OA_CHECK(done_phone.buttons.size() == 1);
}

void test_focus_and_keys() {
    settings::ChallengeModel waiting;
    waiting.window = waiting_window(true);
    settings::challenge_settle(waiting);
    OA_CHECK(waiting.interaction.focused == settings::challenge_open_control);
    OA_CHECK(waiting.interaction.focus_shown);
    OA_CHECK(settings::challenge_key(waiting, kit::Key::enter) == settings::ChallengeAction::open);
    OA_CHECK(
        settings::challenge_key(waiting, kit::Key::escape) == settings::ChallengeAction::cancel
    );
    OA_CHECK(settings::challenge_key(waiting, kit::Key::left) == settings::ChallengeAction::redraw);
    OA_CHECK(waiting.interaction.focused == settings::challenge_cancel_control);
    OA_CHECK(
        settings::challenge_key(waiting, kit::Key::enter) == settings::ChallengeAction::cancel
    );

    settings::ChallengeModel again;
    again.window = waiting_window(true);
    again.window.status = settings::ChallengeWindow::Status::expired;
    settings::challenge_settle(again);
    OA_CHECK(again.interaction.focused == settings::challenge_try_again_control);
    OA_CHECK(
        settings::challenge_key(again, kit::Key::enter) == settings::ChallengeAction::try_again
    );
    OA_CHECK(settings::challenge_key(again, kit::Key::escape) == settings::ChallengeAction::cancel);

    settings::ChallengeModel only_cancel;
    only_cancel.window = waiting_window(false);
    settings::challenge_settle(only_cancel);
    OA_CHECK(only_cancel.interaction.focused == settings::challenge_cancel_control);
    OA_CHECK(
        settings::challenge_key(only_cancel, kit::Key::enter) == settings::ChallengeAction::cancel
    );
}

void test_pointer() {
    settings::ChallengeModel model;
    model.window = waiting_window(true);
    settings::challenge_settle(model);
    const kit::DisplayList list = settings::challenge_list(model);
    const kit::Control* open = named(list, "challenge.open");
    OA_CHECK(open != nullptr);
    const int32_t x = open->rect.x + open->rect.width / 2;
    const int32_t y = open->rect.y + open->rect.height / 2;
    OA_CHECK(settings::challenge_pointer_down(model, {x, y}) == settings::ChallengeAction::redraw);
    OA_CHECK(settings::challenge_pointer_up(model, {x, y}) == settings::ChallengeAction::open);

    settings::challenge_settle(model);
    const int32_t finger_y = open->rect.y + open->rect.height + 3;
    OA_CHECK(
        settings::challenge_finger_down(model, {x, finger_y}, 6) ==
        settings::ChallengeAction::redraw
    );
    OA_CHECK(
        settings::challenge_pointer_up(model, {x, finger_y}) == settings::ChallengeAction::open
    );
    OA_CHECK(
        settings::challenge_finger_down(model, {x, finger_y}, 2) == settings::ChallengeAction::none
    );
}

void test_layout() {
    const kit::SizeClass classes[] = {
        kit::SizeClass::compact, kit::SizeClass::regular, kit::SizeClass::large
    };
    for (const kit::SizeClass size_class : classes) {
        settings::ChallengeModel model;
        model.window = waiting_window(true);
        model.size_class = size_class;
        settings::challenge_settle(model);
        const int32_t width = settings::challenge_width(size_class);
        const int32_t height = settings::challenge_height(model.window, size_class);
        const kit::Rect bounds{0, 0, width, height};
        const kit::DisplayList list = settings::challenge_list(model);
        OA_CHECK(kit::name_problem(list).empty());
        OA_CHECK(width == kit::metrics_of(size_class).notice_width);
        for (const kit::Item& item : list.items) {
            kit::Rect rect = item.rect;
            if (item.role == kit::Role::focus_ring)
                rect = kit::grown(rect, kit::compact_metrics.focus_inset);
            OA_CHECK(kit::wholly_in(rect, bounds));
        }
        for (const kit::Control& control : list.controls)
            OA_CHECK(kit::wholly_in(control.rect, bounds));

        const kit::Control* code = named(list, "challenge.code");
        const kit::Control* status = named(list, "challenge.status");
        const kit::Control* open = named(list, "challenge.open");
        const kit::Control* cancel = named(list, "challenge.cancel");
        OA_CHECK(code != nullptr && status != nullptr && open != nullptr && cancel != nullptr);
        OA_CHECK(named(list, "challenge.try-again") == nullptr);
        OA_CHECK(code->kind == kit::ControlKind::area && !code->enabled && !code->focusable);
        OA_CHECK(code->text == "HXQ7-4KP2");
        OA_CHECK(status->kind == kit::ControlKind::area && !status->enabled && !status->focusable);
        OA_CHECK(status->text == "Waiting for the check. This updates by itself.");
        OA_CHECK(open->kind == kit::ControlKind::button && open->text == "Open the check");
        OA_CHECK(cancel->kind == kit::ControlKind::button && cancel->text == "Cancel");
        const kit::Item* code_text = text_of(list, settings::challenge_code_control);
        OA_CHECK(code_text != nullptr);
        OA_CHECK(code_text->font == kit::FontRole::regular);
        OA_CHECK(code_text->align == kit::Align::centre);
        OA_CHECK(code_text->colour == kit::colour::accent);
        OA_CHECK(code_text->tracking == settings::challenge_code_tracking);
        OA_CHECK(code_text->text == "HXQ7-4KP2");
    }

    settings::ChallengeModel expired;
    expired.window = waiting_window(true);
    expired.window.status = settings::ChallengeWindow::Status::expired;
    settings::challenge_settle(expired);
    const kit::DisplayList expired_list = settings::challenge_list(expired);
    OA_CHECK(kit::name_problem(expired_list).empty());
    const kit::Control* try_again = named(expired_list, "challenge.try-again");
    OA_CHECK(try_again != nullptr && try_again->text == "Try again");
    OA_CHECK(named(expired_list, "challenge.open") == nullptr);
    const kit::Control* expired_status = named(expired_list, "challenge.status");
    OA_CHECK(
        expired_status != nullptr &&
        expired_status->text == "The check ran out. Try again for a new code."
    );
}

} // namespace

int main() {
    test_words();
    test_focus_and_keys();
    test_pointer();
    test_layout();
    return oa::test::check_exit_status();
}

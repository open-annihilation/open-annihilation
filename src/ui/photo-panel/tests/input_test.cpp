// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The panel opens with its first control focused and no ring. Tab shows the
// ring. Until then an arrow, Space and Return are not taken, and a point on
// the battlefield is not taken. A point on the panel is.
//
// oa-ui-photo-panel-input-test

#include "oa/test/check.hpp"
#include "oa/ui/kit/input.hpp"
#include "oa/ui/photo_panel/input.hpp"
#include "block_font.hpp"

#include <initializer_list>
#include <tuple>
#include <utility>

namespace {

namespace kit = oa::ui::kit;
namespace panel = oa::ui::photo_panel;

panel::Layout placed_panel() {
    panel::Model model;
    model.width = 1280;
    model.height = 720;
    model.size_class = kit::SizeClass::regular;
    model.head = "F9 or Esc leaves";
    model.clock = "12:34";
    return panel::layout_of(model, photo_panel_test::block_fonts());
}

} // namespace

int main() {
    const panel::Model model = [] {
        panel::Model filled;
        filled.width = 1280;
        filled.height = 720;
        filled.size_class = kit::SizeClass::regular;
        filled.head = "F9 or Esc leaves";
        filled.clock = "12:34";
        return filled;
    }();
    const panel::Layout placed = panel::layout_of(model, photo_panel_test::block_fonts());
    kit::Interaction interaction;
    kit::focus_opened(interaction, placed.list, false);
    OA_CHECK(!interaction.focus_shown);
    OA_CHECK(!kit::focus_ring_visible(interaction));
    const kit::Control* first = kit::control_of(placed.list, interaction.focused);
    OA_CHECK(first != nullptr && first->name == "photo.leave");

    const panel::Answer down = panel::key_event(interaction, model, placed, kit::Key::down);
    OA_CHECK(!down.consumed);
    OA_CHECK(!interaction.focus_shown);
    const panel::Answer space = panel::key_event(interaction, model, placed, kit::Key::space);
    OA_CHECK(!space.consumed);
    const panel::Answer entered = panel::key_event(interaction, model, placed, kit::Key::enter);
    OA_CHECK(!entered.consumed);
    OA_CHECK(entered.press.command == panel::Command::none);

    const panel::Answer tab = panel::key_event(interaction, model, placed, kit::Key::tab);
    OA_CHECK(tab.consumed);
    OA_CHECK(!tab.dismiss_toast);
    OA_CHECK(interaction.focus_shown);
    OA_CHECK(kit::focus_ring_visible(interaction));
    const kit::Control* next = kit::control_of(placed.list, interaction.focused);
    OA_CHECK(next != nullptr && next->name == "photo.section.camera");

    const panel::Answer battlefield = panel::pointer_event(
        interaction, model, placed, oa::app::ScreenInputKind::pointer_move, kit::Point{40, 360}
    );
    OA_CHECK(!battlefield.consumed);
    OA_CHECK(!kit::focus_ring_visible(interaction));

    // The pointer hid the ring: Return, Space and the arrows are photo
    // mode's again, and press nothing on the panel.
    for (const kit::Key key : {kit::Key::enter, kit::Key::space, kit::Key::left, kit::Key::down}) {
        const panel::Answer hidden = panel::key_event(interaction, model, placed, key);
        OA_CHECK(!hidden.consumed);
        OA_CHECK(hidden.press.command == panel::Command::none);
        OA_CHECK(!kit::focus_ring_visible(interaction));
    }
    const kit::Control* kept = kit::control_of(placed.list, interaction.focused);
    OA_CHECK(kept != nullptr && kept->name == "photo.section.camera");
    const panel::Answer shown_again = panel::key_event(interaction, model, placed, kit::Key::tab);
    OA_CHECK(shown_again.consumed);
    OA_CHECK(kit::focus_ring_visible(interaction));
    const kit::Control* moved = kit::control_of(placed.list, interaction.focused);
    OA_CHECK(moved != nullptr && moved->name == "photo.zoom");

    const panel::Layout again = placed_panel();
    kit::Interaction fresh;
    kit::focus_opened(fresh, again.list, false);
    const panel::Answer escape = panel::key_event(fresh, model, again, kit::Key::escape);
    OA_CHECK(escape.consumed && escape.leave && escape.dismiss_toast);

    const kit::Point on_panel{
        again.panel.x + again.panel.width / 2, again.panel.y + again.panel.height / 2
    };
    const panel::Answer press =
        panel::pointer_event(fresh, model, again, oa::app::ScreenInputKind::pointer_down, on_panel);
    OA_CHECK(press.consumed);
    std::ignore =
        panel::pointer_event(fresh, model, again, oa::app::ScreenInputKind::pointer_up, on_panel);

    // A press near a slider's end lands on its last stop, and near its
    // start on its first: the nearest stop, not the one left of the point.
    const kit::Control* between = nullptr;
    for (const kit::Control& control : again.list.controls)
        if (control.name == "photo.between")
            between = &control;
    OA_CHECK(between != nullptr);
    if (between != nullptr) {
        const int32_t middle = between->rect.y + between->rect.height / 2;
        const kit::Point near_end{between->rect.x + between->rect.width - 3, middle};
        const kit::Point near_start{between->rect.x + 2, middle};
        for (const auto& [at, stop] :
             {std::pair{near_end, model.between_stops - 1}, std::pair{near_start, 0}}) {
            std::ignore = panel::pointer_event(
                fresh, model, again, oa::app::ScreenInputKind::pointer_down, at
            );
            const panel::Answer up =
                panel::pointer_event(fresh, model, again, oa::app::ScreenInputKind::pointer_up, at);
            OA_CHECK(up.press.command == panel::Command::between);
            OA_CHECK(up.press.value == stop);
            OA_CHECK(up.dismiss_toast);
        }
    }

    const kit::Control* modes = nullptr;
    for (const kit::Control& control : again.list.controls)
        if (control.name == "photo.time-mode")
            modes = &control;
    OA_CHECK(modes != nullptr);
    if (modes != nullptr) {
        const int32_t middle = modes->rect.y + modes->rect.height / 2;
        const kit::Point freeze_at{modes->rect.x + modes->rect.width - 4, middle};
        const kit::Point run_at{modes->rect.x + 4, middle};
        std::ignore = panel::pointer_event(
            fresh, model, again, oa::app::ScreenInputKind::pointer_down, freeze_at
        );
        const panel::Answer froze = panel::pointer_event(
            fresh, model, again, oa::app::ScreenInputKind::pointer_up, freeze_at
        );
        OA_CHECK(froze.press.command == panel::Command::freeze);
        OA_CHECK(froze.dismiss_toast);
        std::ignore = panel::pointer_event(
            fresh, model, again, oa::app::ScreenInputKind::pointer_down, run_at
        );
        const panel::Answer ran =
            panel::pointer_event(fresh, model, again, oa::app::ScreenInputKind::pointer_up, run_at);
        OA_CHECK(ran.press.command == panel::Command::run);
        panel::Model shared = model;
        shared.slow_lock = "Shared game";
        const kit::Point slow_at{modes->rect.x + modes->rect.width / 2, middle};
        std::ignore = panel::pointer_event(
            fresh, shared, again, oa::app::ScreenInputKind::pointer_down, slow_at
        );
        const panel::Answer slowed = panel::pointer_event(
            fresh, shared, again, oa::app::ScreenInputKind::pointer_up, slow_at
        );
        OA_CHECK(slowed.press.command == panel::Command::none);
        OA_CHECK(slowed.dismiss_toast);
    }

    return oa::test::check_exit_status();
}

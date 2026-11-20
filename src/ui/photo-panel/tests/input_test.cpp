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

#include <algorithm>
#include <cstdio>
#include <initializer_list>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

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

/// Tells whether a control's centre lies in the part a press can reach.
bool centre_shown(const kit::Control& control) {
    const kit::Point mid{
        control.rect.x + control.rect.width / 2, control.rect.y + control.rect.height / 2
    };
    if (control.clip.width <= 0 || control.clip.height <= 0)
        return kit::contains(control.rect, mid);
    return kit::contains(control.clip, mid);
}

/// Opens one section and scrolls its header to the top of the rows.
void open_only(panel::Model& model, int32_t section) {
    model.camera_open = section == panel::kSectionCamera;
    model.time_open = section == panel::kSectionTime;
    model.look_open = section == panel::kSectionLook;
    model.capture_open = section == panel::kSectionCapture;
    const int32_t row = std::max(model.touch_row, int32_t{44});
    const int32_t gap = std::max(int32_t{2}, row / 11);
    int before = 0;
    if (section == panel::kSectionTime)
        before = 1;
    else if (section == panel::kSectionLook)
        before = 2;
    else if (section == panel::kSectionCapture)
        before = 3;
    model.scroll = before * (row + gap);
    model.reveal = kit::no_control;
}

/// Scrolls the focused row into the view, as the host does once the ring shows.
void reveal_focus(
    panel::Model& model, const kit::Fonts& fonts, panel::Layout& placed, kit::ControlId id
) {
    model.reveal = id;
    model.scroll = placed.scroll;
    placed = panel::layout_of(model, fonts);
    model.scroll = placed.scroll;
}

/// The D-pad's walk: Tab shows the ring, Down moves, and A opens a closed section.
void walk_pad(int32_t width, int32_t height) {
    const kit::Fonts fonts = photo_panel_test::block_fonts();
    panel::Model model;
    model.width = width;
    model.height = height;
    model.size_class = kit::size_class_of(width, height);
    model.touch = true;
    model.touch_row = 66;
    model.camera_open = true;
    model.time_open = false;
    model.look_open = false;
    model.capture_open = false;
    model.head = "Leave";
    model.clock = "12:34";
    model.key_line = "A Take photo · B Leave · X Freeze · Y Hide panel";
    panel::Layout placed = panel::layout_of(model, fonts);
    kit::Interaction interaction;
    kit::focus_opened(interaction, placed.list, false);
    std::ignore = panel::key_event(interaction, model, placed, kit::Key::tab);
    reveal_focus(model, fonts, placed, interaction.focused);

    std::vector<std::string> saw;
    for (int step = 0; step < 40; ++step) {
        const kit::Control* here = kit::control_of(placed.list, interaction.focused);
        if (here == nullptr)
            break;
        // The layout is made again below, which frees here's list: keep its name.
        const std::string here_name = here->name;
        saw.emplace_back(here_name);
        const bool header = here_name.rfind("photo.section.", 0) == 0 && !here->expanded;
        if (header) {
            const panel::Answer opened =
                panel::key_event(interaction, model, placed, kit::Key::enter);
            OA_CHECK(opened.press.command == panel::Command::toggle_section);
            open_only(model, opened.press.value);
            placed = panel::layout_of(model, fonts);
            model.scroll = placed.scroll;
            const kit::Control* stayed = kit::control_of(placed.list, interaction.focused);
            OA_CHECK(stayed != nullptr && centre_shown(*stayed));
        }
        if (here_name == "photo.take")
            break;
        const kit::ControlId before = interaction.focused;
        const kit::Key key = kit::focus_ring_visible(interaction) ? kit::Key::down : kit::Key::tab;
        std::ignore = panel::key_event(interaction, model, placed, key);
        if (interaction.focused == before)
            break;
        if (kit::focus_ring_visible(interaction))
            reveal_focus(model, fonts, placed, interaction.focused);
        const kit::Control* landed = kit::control_of(placed.list, interaction.focused);
        OA_CHECK(landed != nullptr && centre_shown(*landed));
    }
    for (int step = 0; step < 40; ++step) {
        const kit::ControlId before = interaction.focused;
        const kit::Key key = kit::focus_ring_visible(interaction) ? kit::Key::up : kit::Key::tab;
        std::ignore = panel::key_event(interaction, model, placed, key);
        if (interaction.focused == before)
            break;
        if (kit::focus_ring_visible(interaction))
            reveal_focus(model, fonts, placed, interaction.focused);
        const kit::Control* landed = kit::control_of(placed.list, interaction.focused);
        if (landed == nullptr)
            break;
        OA_CHECK(centre_shown(*landed));
        saw.emplace_back(landed->name);
    }

    const char* stops[] = {
        "photo.leave",        "photo.section.camera", "photo.zoom",
        "photo.turn",         "photo.tilt",           "photo.classic",
        "photo.follow",       "photo.section.time",   "photo.time-mode",
        "photo.speed",        "photo.step",           "photo.between",
        "photo.section.look", "photo.depth",          "photo.look",
        "photo.vignette",     "photo.roundness",      "photo.grain",
        "photo.grain-size",   "photo.frame",          "photo.selection",
        "photo.health",       "photo.guides",         "photo.section.capture",
        "photo.size",         "photo.smooth",         "photo.copy-to-clipboard",
        "photo.player-names", "photo.copy",           "photo.take",
    };
    for (const char* name : stops) {
        const bool found = std::find(saw.begin(), saw.end(), name) != saw.end();
        if (!found) {
            std::fprintf(stderr, "input: the pad missed %s at %dx%d\n", name, width, height);
            for (const std::string& seen : saw)
                std::fprintf(stderr, "  %s\n", seen.c_str());
        }
        OA_CHECK(found);
    }

    // The walk folds Camera. Lay it open again and step the slider with Right.
    model.camera_open = true;
    model.time_open = false;
    model.look_open = false;
    model.capture_open = false;
    model.scroll = 0;
    model.reveal = panel::kZoom;
    placed = panel::layout_of(model, fonts);
    const kit::Control* zoom = nullptr;
    for (const kit::Control& control : placed.list.controls)
        if (control.name == "photo.zoom")
            zoom = &control;
    OA_CHECK(zoom != nullptr);
    if (zoom != nullptr) {
        kit::Interaction slider;
        kit::focus_opened(slider, placed.list, true);
        slider.focused = zoom->id;
        slider.focus_shown = true;
        const panel::Answer right = panel::key_event(slider, model, placed, kit::Key::right);
        OA_CHECK(right.press.command == panel::Command::set_zoom);
        OA_CHECK(right.press.value == model.zoom_stop + 1);
    }
}

/// A tap just inside the view, after half a row of scroll, hits the cut row.
void cut_row_stays(int32_t width, int32_t height) {
    const kit::Fonts fonts = photo_panel_test::block_fonts();
    panel::Model model;
    model.width = width;
    model.height = height;
    model.size_class = kit::size_class_of(width, height);
    model.touch = true;
    model.touch_row = 66;
    model.camera_open = true;
    model.time_open = false;
    model.look_open = false;
    model.capture_open = false;
    model.head = "Leave";
    model.key_line = "A Take photo · B Leave · X Freeze · Y Hide panel";
    const int32_t limit = panel::layout_of(model, fonts).rows.limit;
    const int32_t half = model.touch_row / 2;
    OA_CHECK(limit >= half);
    model.scroll = half;
    const panel::Layout placed = panel::layout_of(model, fonts);
    const kit::Point under{placed.rows.view.x + placed.rows.view.width / 2, placed.rows.view.y + 2};
    kit::Interaction finger;
    std::ignore = panel::pointer_event(
        finger, model, placed, oa::app::ScreenInputKind::pointer_down, under, 22
    );
    const panel::Answer tapped = panel::pointer_event(
        finger, model, placed, oa::app::ScreenInputKind::pointer_up, under, 22
    );
    OA_CHECK(tapped.press.command != panel::Command::leave);
    OA_CHECK(tapped.press.command != panel::Command::none);
    OA_CHECK(!tapped.leave);
}

/// A finger on each Size stop's centre, where automation lists it, chooses
/// that size, and a locked stop chooses nothing. At a phone's Larger rows
/// the four stops wrap onto two rows of two; elsewhere they share one row.
/// The rows are scrolled so the stop pressed lies in the view.
void size_strip_by_finger(int32_t width, int32_t height, int32_t row, bool wraps) {
    const kit::Fonts fonts = photo_panel_test::block_fonts();
    panel::Model model;
    model.width = width;
    model.height = height;
    model.size_class = kit::size_class_of(width, height);
    model.touch = true;
    model.touch_row = row;
    model.camera_open = false;
    model.time_open = false;
    model.look_open = false;
    model.capture_open = true;
    model.head = "Leave";
    model.size_locks[3] = "Not enough memory";
    model.size_reason = "4x: Not enough memory";
    const char* parts[] = {"screen", "2", "3", "4"};
    int32_t first_row = -1;
    bool second_row = false;
    for (int32_t index = 0; index < 4; ++index) {
        const std::string name = std::string("photo.size.") + parts[index];
        model.scroll = 0;
        panel::Layout placed = panel::layout_of(model, fonts);
        const auto find_part = [&](const panel::Layout& layout) {
            kit::AutomationEntry found{};
            for (const kit::AutomationEntry& entry :
                 kit::automation_parts(layout.list, kit::Interaction{}))
                if (entry.name == name)
                    found = entry;
            return found;
        };
        kit::AutomationEntry part = find_part(placed);
        OA_CHECK(part.part && part.rect.width >= row && part.rect.height >= row);
        if (!part.part)
            continue;
        // Scroll the stop's top to the view's top, then find it again.
        model.scroll = part.rect.y - placed.rows.view.y;
        placed = panel::layout_of(model, fonts);
        part = find_part(placed);
        const kit::Point at{part.rect.x + part.rect.width / 2, part.rect.y + part.rect.height / 2};
        OA_CHECK(kit::contains(placed.rows.view, at));
        if (first_row < 0)
            first_row = part.rect.y + placed.scroll;
        if (part.rect.y + placed.scroll > first_row)
            second_row = true;
        kit::Interaction finger;
        std::ignore = panel::pointer_event(
            finger, model, placed, oa::app::ScreenInputKind::pointer_down, at, 12
        );
        const panel::Answer chosen = panel::pointer_event(
            finger, model, placed, oa::app::ScreenInputKind::pointer_up, at, 12
        );
        if (index < 3) {
            OA_CHECK(chosen.press.command == panel::Command::set_size);
            OA_CHECK(chosen.press.value == index + 1);
        } else {
            OA_CHECK(chosen.press.command == panel::Command::none);
        }
    }
    OA_CHECK(second_row == wraps);
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

    const kit::Control* sizes = nullptr;
    for (const kit::Control& control : again.list.controls)
        if (control.name == "photo.size")
            sizes = &control;
    OA_CHECK(sizes != nullptr);
    if (sizes != nullptr) {
        const int32_t middle = sizes->rect.y + sizes->rect.height / 2;
        const int32_t stops = 4;
        const int32_t level_w = std::max((sizes->rect.width - (stops - 1)) / stops, int32_t{1});
        const kit::Point two_at{sizes->rect.x + level_w + level_w / 2, middle};
        std::ignore = panel::pointer_event(
            fresh, model, again, oa::app::ScreenInputKind::pointer_down, two_at
        );
        const panel::Answer sized =
            panel::pointer_event(fresh, model, again, oa::app::ScreenInputKind::pointer_up, two_at);
        OA_CHECK(sized.press.command == panel::Command::set_size);
        OA_CHECK(sized.press.value == 2);
        panel::Model locked = model;
        locked.size_locks[3] = "Shared game";
        const kit::Point four_at{sizes->rect.x + sizes->rect.width - 2, middle};
        std::ignore = panel::pointer_event(
            fresh, locked, again, oa::app::ScreenInputKind::pointer_down, four_at
        );
        const panel::Answer refused = panel::pointer_event(
            fresh, locked, again, oa::app::ScreenInputKind::pointer_up, four_at
        );
        OA_CHECK(refused.press.command == panel::Command::none);
        OA_CHECK(refused.dismiss_toast);
    }

    // A finger takes the nearest control within reach: the centre, and a few
    // points outside the edge. The time strip's centre is Slow.
    const auto finger_tap =
        [](const panel::Model& panel_model, const panel::Layout& layout, kit::Point at) {
            kit::Interaction finger;
            std::ignore = panel::pointer_event(
                finger, panel_model, layout, oa::app::ScreenInputKind::pointer_down, at, 12
            );
            return panel::pointer_event(
                finger, panel_model, layout, oa::app::ScreenInputKind::pointer_up, at, 12
            );
        };
    const auto touch_model = [](bool camera, bool time, bool look, bool capture) {
        panel::Model touch;
        touch.width = 852;
        touch.height = 393;
        touch.size_class = kit::SizeClass::regular;
        touch.touch = true;
        touch.touch_row = 44;
        touch.camera_open = camera;
        touch.time_open = time;
        touch.look_open = look;
        touch.capture_open = capture;
        touch.head = "Leave";
        touch.hint_label = "Show panel";
        touch.clock = "12:34";
        return touch;
    };
    const auto control_named = [](const panel::Layout& layout,
                                  const char* name) -> const kit::Control* {
        for (const kit::Control& control : layout.list.controls)
            if (control.name == name)
                return &control;
        return nullptr;
    };
    const auto centre_of = [](const kit::Control& control) {
        return kit::Point{
            control.rect.x + control.rect.width / 2, control.rect.y + control.rect.height / 2
        };
    };

    const panel::Model camera = touch_model(true, false, false, false);
    const panel::Layout camera_layout = panel::layout_of(camera, photo_panel_test::block_fonts());
    const kit::Control* header = control_named(camera_layout, "photo.section.camera");
    OA_CHECK(header != nullptr && header->rect.height >= 44);
    if (header != nullptr) {
        const kit::Point mid = centre_of(*header);
        const panel::Answer at_centre = finger_tap(camera, camera_layout, mid);
        OA_CHECK(at_centre.press.command == panel::Command::toggle_section);
        OA_CHECK(at_centre.press.value == panel::kSectionCamera);
        const panel::Answer outside =
            finger_tap(camera, camera_layout, kit::Point{header->rect.x - 4, mid.y});
        OA_CHECK(outside.press.command == panel::Command::toggle_section);
        OA_CHECK(outside.consumed);
    }

    // The look's own rows come first, so Show selection is scrolled into view.
    panel::Model looking = touch_model(false, false, true, false);
    looking.reveal = panel::kSelection;
    const panel::Layout look_layout = panel::layout_of(looking, photo_panel_test::block_fonts());
    const kit::Control* selection = control_named(look_layout, "photo.selection");
    OA_CHECK(selection != nullptr && selection->rect.height >= 44);
    if (selection != nullptr) {
        const kit::Point mid = centre_of(*selection);
        OA_CHECK(
            finger_tap(looking, look_layout, mid).press.command == panel::Command::toggle_selection
        );
        const panel::Answer outside =
            finger_tap(looking, look_layout, kit::Point{selection->rect.x - 4, mid.y});
        OA_CHECK(outside.press.command == panel::Command::toggle_selection);
    }

    const panel::Model timing = touch_model(false, true, false, false);
    const panel::Layout time_layout = panel::layout_of(timing, photo_panel_test::block_fonts());
    const kit::Control* strip = control_named(time_layout, "photo.time-mode");
    OA_CHECK(strip != nullptr && strip->rect.height >= 44);
    if (strip != nullptr) {
        const kit::Point mid = centre_of(*strip);
        OA_CHECK(finger_tap(timing, time_layout, mid).press.command == panel::Command::slow);
        const kit::Point past{strip->rect.x + strip->rect.width + 4, mid.y};
        OA_CHECK(finger_tap(timing, time_layout, past).press.command == panel::Command::freeze);
    }

    const kit::Control* take = control_named(camera_layout, "photo.take");
    const kit::Control* leave = control_named(camera_layout, "photo.leave");
    OA_CHECK(take != nullptr && leave != nullptr);
    if (take != nullptr) {
        const kit::Point mid = centre_of(*take);
        OA_CHECK(
            finger_tap(camera, camera_layout, mid).press.command == panel::Command::take_photo
        );
        const panel::Answer outside =
            finger_tap(camera, camera_layout, kit::Point{take->rect.x - 4, mid.y});
        OA_CHECK(outside.press.command == panel::Command::take_photo);
    }
    if (leave != nullptr) {
        const kit::Point mid = centre_of(*leave);
        OA_CHECK(finger_tap(camera, camera_layout, mid).press.command == panel::Command::leave);
        const panel::Answer outside =
            finger_tap(camera, camera_layout, kit::Point{leave->rect.x - 4, mid.y});
        OA_CHECK(outside.press.command == panel::Command::leave);
        OA_CHECK(outside.leave);
    }

    // A release the system cancels, far from every control, presses nothing.
    if (take != nullptr) {
        kit::Interaction finger;
        const kit::Point mid = centre_of(*take);
        std::ignore = panel::pointer_event(
            finger, camera, camera_layout, oa::app::ScreenInputKind::pointer_down, mid, 12
        );
        const panel::Answer cancelled = panel::pointer_event(
            finger,
            camera,
            camera_layout,
            oa::app::ScreenInputKind::pointer_up,
            kit::Point{-1, -1},
            12
        );
        OA_CHECK(cancelled.press.command == panel::Command::none);
    }

    walk_pad(852, 393);
    walk_pad(667, 375);
    walk_pad(1180, 820);
    cut_row_stays(852, 393);
    cut_row_stays(667, 375);
    size_strip_by_finger(852, 393, 44, false);
    size_strip_by_finger(852, 393, 66, true);
    size_strip_by_finger(1180, 820, 66, false);

    const kit::Control* grades = nullptr;
    for (const kit::Control& control : again.list.controls)
        if (control.name == "photo.look")
            grades = &control;
    OA_CHECK(grades != nullptr && grades->parts.size() == 6);
    if (grades != nullptr) {
        const kit::Point bleach_at{
            grades->rect.x + grades->rect.width - 2, grades->rect.y + grades->rect.height - 2
        };
        std::ignore = panel::pointer_event(
            fresh, model, again, oa::app::ScreenInputKind::pointer_down, bleach_at
        );
        const panel::Answer graded = panel::pointer_event(
            fresh, model, again, oa::app::ScreenInputKind::pointer_up, bleach_at
        );
        OA_CHECK(graded.press.command == panel::Command::set_preset);
        OA_CHECK(graded.press.value == 5);
        kit::Interaction keys;
        keys.focused = grades->id;
        keys.focus_shown = true;
        const panel::Answer right = panel::key_event(keys, model, again, kit::Key::right);
        OA_CHECK(right.press.command == panel::Command::set_preset);
        OA_CHECK(right.press.value == 1);
    }

    // Names too long for one row wrap the chips onto rows. Automation lists
    // each chip where it is drawn, and a press there chooses that chip.
    {
        panel::Model worded = model;
        worded.preset_labels = {"Keine", "1997", "Abendlicht", "Arktis", "Schwarzweiss", "Bleich"};
        const panel::Layout wrapped = panel::layout_of(worded, photo_panel_test::block_fonts());
        const char* grade_words[] = {"none", "1997", "dusk", "arctic", "noir", "bleach"};
        kit::Interaction resting;
        const std::vector<kit::AutomationEntry> entries =
            kit::automation_parts(wrapped.list, resting);
        int32_t first_row = -1;
        bool second_row = false;
        for (int32_t index = 0; index < 6; ++index) {
            const std::string name = std::string("photo.look.") + grade_words[index];
            const kit::AutomationEntry* chip = nullptr;
            for (const kit::AutomationEntry& entry : entries)
                if (entry.name == name)
                    chip = &entry;
            OA_CHECK(chip != nullptr && chip->part);
            if (chip == nullptr)
                continue;
            if (first_row < 0)
                first_row = chip->rect.y;
            if (chip->rect.y > first_row)
                second_row = true;
            const kit::Point at{
                chip->rect.x + chip->rect.width / 2, chip->rect.y + chip->rect.height / 2
            };
            kit::Interaction pressing;
            std::ignore = panel::pointer_event(
                pressing, worded, wrapped, oa::app::ScreenInputKind::pointer_down, at
            );
            const panel::Answer chosen = panel::pointer_event(
                pressing, worded, wrapped, oa::app::ScreenInputKind::pointer_up, at
            );
            OA_CHECK(chosen.press.command == panel::Command::set_preset);
            OA_CHECK(chosen.press.value == index);
        }
        OA_CHECK(second_row);
    }

    // A slider of 101 stops: Page Up and Page Down move ten, Home and End go
    // to the ends, and a key at an end sends nothing but is still the panel's.
    const kit::Control* vignette = nullptr;
    for (const kit::Control& control : again.list.controls)
        if (control.name == "photo.vignette")
            vignette = &control;
    OA_CHECK(vignette != nullptr);
    if (vignette != nullptr) {
        panel::Model slider_model = model;
        slider_model.vignette_stop = 40;
        kit::Interaction slider_keys;
        slider_keys.focused = vignette->id;
        slider_keys.focus_shown = true;
        const panel::Answer paged_up =
            panel::key_event(slider_keys, slider_model, again, kit::Key::page_up);
        OA_CHECK(paged_up.consumed && paged_up.press.command == panel::Command::set_vignette);
        OA_CHECK(paged_up.press.value == 50);
        const panel::Answer paged_down =
            panel::key_event(slider_keys, slider_model, again, kit::Key::page_down);
        OA_CHECK(paged_down.consumed && paged_down.press.value == 30);
        const panel::Answer went_home =
            panel::key_event(slider_keys, slider_model, again, kit::Key::home);
        OA_CHECK(went_home.consumed && went_home.press.value == 0);
        const panel::Answer went_end =
            panel::key_event(slider_keys, slider_model, again, kit::Key::end);
        OA_CHECK(went_end.consumed && went_end.press.value == 100);
        slider_model.vignette_stop = 100;
        const panel::Answer past_end =
            panel::key_event(slider_keys, slider_model, again, kit::Key::page_up);
        OA_CHECK(past_end.consumed && past_end.press.command == panel::Command::none);
        // Away from a slider, Page Up is photo mode's.
        kit::Interaction on_head;
        on_head.focused = first != nullptr ? first->id : kit::no_control;
        on_head.focus_shown = true;
        const panel::Answer zoomed =
            panel::key_event(on_head, slider_model, again, kit::Key::page_up);
        OA_CHECK(!zoomed.consumed);
    }

    return oa::test::check_exit_status();
}

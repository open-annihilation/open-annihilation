// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/hud/command_buttons.hpp"
#include "oa/ui/hud/order_panel.hpp"

#include "oa/ui/gui_input/gadget_panel.hpp"
#include "oa/ui/gui_input/tab_order.hpp"

#include <cstdint>
#include <cstring>
#include <utility>
#include <vector>

namespace oa::ui::hud {
namespace {

struct CommandButton {
    const char* word{};
    uint8_t order{};
    bool special{};
};

// In the order the names are tried: UNLOAD before LOAD, which it holds.
constexpr CommandButton kCommandButtons[] = {
    {"MOVE", armed_order::move, false},
    {"STOP", armed_order::default_order, false},
    {"ATTACK", armed_order::attack, false},
    {"BLAST", armed_order::blast, false},
    {"DEFEND", armed_order::defend, false},
    {"REPAIR", armed_order::repair, true},
    {"PATROL", armed_order::patrol, false},
    {"RECLAIM", armed_order::reclaim, true},
    {"CAPTURE", armed_order::capture, true},
    {"UNLOAD", armed_order::unload, true},
    {"LOAD", armed_order::load, false},
};

/// Arms the order the pointer gives (Game.pointer_command).
void set_armed_order(Game& game, uint8_t order) noexcept {
    game.pointer_command = order;
}

/// Clears the drag box bit of Game.pointer_flags, keeping the others.
void clear_drag_box(Game& game) noexcept {
    game.pointer_flags = static_cast<uint8_t>(game.pointer_flags & ~kPointerFlagDragBox);
}

} // namespace

uint8_t armed_order_of(const Game& game) noexcept {
    return game.pointer_command;
}

bool order_panel_command(Game& game, const char* name, int16_t status, const HudEvents& events) {
    if (name == nullptr)
        return false;
    const CommandButton* button = nullptr;
    for (const auto& candidate : kCommandButtons)
        if (std::strstr(name, candidate.word) != nullptr) {
            button = &candidate;
            break;
        }
    if (button == nullptr)
        return false;
    const bool stop = button->order == armed_order::default_order;
    set_armed_order(game, status != 0 && !stop ? button->order : armed_order::default_order);
    clear_drag_box(game);
    if (stop && events.apply_standing_order != nullptr)
        events.apply_standing_order(events.user, kStopOrderTag, 0);
    play_sound(events, button->special ? kSpecialOrdersSound : kImmediateOrdersSound);
    return true;
}

bool panel_unit_vanished(const World& world) noexcept {
    const auto id = world.game.panel_unit_id;
    return id != 0 && id < world.unit_slot_count && world.units[id].type_index == 0;
}

namespace {

/// Whether the button is one the keyboard focus visits: every button the
/// panel shows and a click would press, whatever the side or mod names it.
bool panel_stop(const PanelButton& button) noexcept {
    return button.active && !button.grayed && button.width > 0 && button.height > 0;
}

/// The panel as gadgets, so the menus' order can read it.
///
/// The root stays inactive. A button that is not a stop is inactive too, so
/// the minimap, the bars and a grayed button are left out. A stop is a button.
///
/// The menus' arrow step reads a column at x 0 as one it has not placed yet,
/// and lines up no later button with a column after it. A menu never sits at
/// the screen's left edge, but the panel does: its first gadget, the font,
/// and its left build cells are at x 0. Each button would keep a column of
/// its own, and Down from Orders would pass the cells and land on Move. The
/// gadgets move right together until the leftmost is at x 1. The distances
/// between them, and so the tab order, stay as they are.
std::vector<ui::gui_layout::Gadget> gadgets_of(std::span<const PanelButton> buttons) {
    int32_t leftmost = 1;
    for (std::size_t index = 1; index < buttons.size(); ++index) {
        if (buttons[index].x < leftmost)
            leftmost = buttons[index].x;
    }
    const int32_t shift = 1 - leftmost;
    std::vector<ui::gui_layout::Gadget> gadgets;
    gadgets.reserve(buttons.size());
    for (std::size_t index = 0; index < buttons.size(); ++index) {
        const PanelButton& button = buttons[index];
        ui::gui_layout::Gadget gadget;
        gadget.common.name = button.name != nullptr ? button.name : "";
        gadget.common.x = static_cast<int16_t>(button.x + shift);
        gadget.common.y = button.y;
        gadget.common.width = button.width;
        gadget.common.height = button.height;
        gadget.common.attributes = static_cast<int32_t>(button.attributes);
        gadget.common.common_attributes = static_cast<int8_t>(button.attributes);
        if (index == 0) {
            gadget.common.type = ui::gui_layout::GadgetType::panel;
            gadget.common.active = 0;
            gadget.fields = ui::gui_layout::PanelFields{};
        } else {
            const bool stop = panel_stop(button);
            gadget.common.type = ui::gui_layout::GadgetType::button;
            gadget.common.active = stop ? int8_t{1} : int8_t{0};
            ui::gui_layout::ButtonFields fields;
            fields.grayed_out = button.grayed;
            gadget.fields = fields;
        }
        gadgets.push_back(std::move(gadget));
    }
    return gadgets;
}

/// The next or previous stop, wrapping. The order is the menus' tab order.
int32_t tab_land(std::span<const int32_t> stops, int32_t focus, bool forward) {
    if (stops.empty())
        return focus;
    int32_t landed = forward ? stops.front() : stops.back();
    for (std::size_t at = 0; at < stops.size(); ++at) {
        if (stops[at] != focus)
            continue;
        if (forward)
            landed = stops[(at + 1U) % stops.size()];
        else
            landed = stops[at == 0 ? stops.size() - 1U : at - 1U];
        break;
    }
    return landed;
}

/// The stop focus_nearest lands on, or `focus` when it is not among the stops.
int32_t nearest_stop(
    std::span<const PanelButton> buttons,
    std::span<const int32_t> stops,
    int32_t focus,
    ui::gui_input::FocusDirection direction
) {
    bool found = false;
    for (const int32_t stop : stops) {
        if (stop == focus)
            found = true;
    }
    if (!found || focus <= 0 || static_cast<std::size_t>(focus) >= buttons.size())
        return focus;
    if (buttons.size() > ui::gui_layout::kGadgetCapacity)
        return focus;
    ui::gui_layout::Layout layout;
    layout.gadgets = gadgets_of(buttons);
    ui::gui_input::GadgetPanel panel;
    ui::gui_input::init_gadget_panel(panel);
    auto* owner =
        ui::gui_input::load_panel(panel, layout, "orders", ui::gui_input::panel_flag::no_draw);
    if (owner == nullptr)
        return focus;
    // load_panel focuses the first stop. The arrow starts from the one asked.
    owner->focus = focus;
    ui::gui_input::focus_nearest(panel, direction);
    return owner->focus;
}

} // namespace

std::vector<int32_t> panel_button_stops(std::span<const PanelButton> buttons) {
    if (buttons.empty())
        return {};
    const std::vector<ui::gui_layout::Gadget> gadgets = gadgets_of(buttons);
    return ui::gui_input::tab_order(gadgets);
}

PanelFocusStep step_panel_button_focus(
    std::span<const PanelButton> buttons, int32_t focus, PanelFocusMove move, bool ring_shown
) {
    const std::vector<int32_t> stops = panel_button_stops(buttons);
    PanelFocusStep step;
    step.index = focus;
    if (move == PanelFocusMove::press) {
        if (!ring_shown)
            return step;
        for (const int32_t stop : stops) {
            if (stop != focus)
                continue;
            step.press = true;
            return step;
        }
        return step;
    }
    if (move == PanelFocusMove::tab_next || move == PanelFocusMove::tab_previous) {
        const int32_t landed = tab_land(stops, focus, move == PanelFocusMove::tab_next);
        step.index = landed;
        step.moved = landed != focus;
        return step;
    }
    if (!ring_shown)
        return step;
    ui::gui_input::FocusDirection direction = ui::gui_input::FocusDirection::next;
    switch (move) {
    case PanelFocusMove::left:
        direction = ui::gui_input::FocusDirection::previous;
        break;
    case PanelFocusMove::right:
        direction = ui::gui_input::FocusDirection::next;
        break;
    case PanelFocusMove::up:
        direction = ui::gui_input::FocusDirection::up;
        break;
    case PanelFocusMove::down:
        direction = ui::gui_input::FocusDirection::down;
        break;
    default:
        return step;
    }
    const int32_t landed = nearest_stop(buttons, stops, focus, direction);
    step.index = landed;
    step.moved = landed != focus;
    return step;
}

} // namespace oa::ui::hud

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// SINGLE.GUI when a side has a campaign, and when neither side has one.
#include "oa/app/runtime.hpp"
#include "oa/ui/frontend_dialogs.hpp"

#include <SDL3/SDL.h>

#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace oa::app {

namespace {

namespace dialogs = oa::ui::frontend_dialogs;

void expect(bool condition, std::string_view what) {
    if (!condition)
        throw std::runtime_error("single player offers check: " + std::string(what));
}

} // namespace

void Runtime::check_single_player_offers() {
    const auto grayed = [this](std::string_view name) {
        const auto* gadget = widget(name);
        const auto* button = gadget != nullptr
                                 ? std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget->fields)
                                 : nullptr;
        return button != nullptr && button->grayed_out;
    };
    const auto shown = [this](std::string_view name) {
        const auto* gadget = widget(name);
        return gadget != nullptr && gadget->common.active != 0;
    };
    const auto press = [this](std::string_view name) {
        const auto* gadget = widget(name);
        expect(gadget != nullptr, "the screen lacks " + std::string(name));
        bool running = true;
        SDL_Event event{};
        event.button.x = static_cast<float>(gadget->common.x + gadget->common.width / 2);
        event.button.y = static_cast<float>(gadget->common.y + gadget->common.height / 2);
        event.button.button = SDL_BUTTON_LEFT;
        event.button.clicks = 1;
        event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        handle_sdl_event(event, running);
        event.type = SDL_EVENT_MOUSE_BUTTON_UP;
        handle_sdl_event(event, running);
    };

    exercise_click(menu::resource_name(menu::Button::single_player));
    expect(screen_ == Screen::single_player, "SINGLE did not open SINGLE.GUI");

    const bool campaigns = side_has_campaign(0) || side_has_campaign(1);
    const bool new_greyed = grayed("NewCamp");
    const bool new_shown = shown("NewCamp");
    const bool any_shown = shown("AnyMsn");
    expect(new_shown, "New Campaign is not shown");
    expect(
        new_greyed == !campaigns, "New Campaign is not greyed exactly when there is no campaign"
    );
    if (!campaigns)
        expect(!any_shown, "Any Mission is shown when there is no campaign");
    else
        expect(!any_shown || offers_any_mission(), "Any Mission shows without its screen");

    if (!campaigns) {
        press("NewCamp");
        expect(
            screen_ == Screen::single_player && dialogs::dialog_count() == 0,
            "a greyed New Campaign took a press"
        );
        const auto* gadget = widget("NewCamp");
        const auto* button = gadget != nullptr
                                 ? std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget->fields)
                                 : nullptr;
        expect(button != nullptr && button->quick_key != 0, "New Campaign has no key");
        bool running = true;
        SDL_Event key{};
        key.type = SDL_EVENT_KEY_DOWN;
        key.key.key = static_cast<SDL_Keycode>(static_cast<unsigned char>(button->quick_key));
        handle_sdl_event(key, running);
        expect(
            screen_ == Screen::single_player && dialogs::dialog_count() == 0,
            "New Campaign's key opened something while it was greyed"
        );
    } else {
        exercise_click(entry::resource_name(entry::Button::new_campaign));
        expect(screen_ == Screen::new_campaign, "New Campaign did not open NEWGAME.GUI");
        exercise_click(entry::resource_name(entry::Button::previous_menu));
        expect(screen_ == Screen::single_player, "Previous Menu did not return to SINGLE.GUI");
    }

    std::cout << "single player offers check: campaigns " << (campaigns ? "yes" : "no")
              << ", new campaign " << (new_greyed ? "greyed" : "offered") << ", any mission "
              << (any_shown ? "shown" : "hidden") << '\n';
}

} // namespace oa::app

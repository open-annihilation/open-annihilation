// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// One answer for the move a focus key fired, and the handoff of the
// dispatcher's own repeats to the package screens.
#include "oa/app/runtime.hpp"

#include "oa/ui/key_bindings/key_codes.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <span>
#include <vector>

namespace oa::app {
namespace {

namespace keys = oa::ui::key_bindings;

/// The modifier bits a chord names, as a key event carries them.
///
/// Primary is the command key on this platform and Ctrl on the others.
///
/// @param bits the chord's modifier bits
/// @return the bits a key event carries
uint16_t event_modifiers(uint8_t bits) {
    uint16_t mods = 0;
    if ((bits & keys::shift) != 0)
        mods = static_cast<uint16_t>(mods | static_cast<uint16_t>(SDL_KMOD_LSHIFT));
    if ((bits & keys::ctrl) != 0)
        mods = static_cast<uint16_t>(mods | static_cast<uint16_t>(SDL_KMOD_LCTRL));
    if ((bits & keys::alt) != 0)
        mods = static_cast<uint16_t>(mods | static_cast<uint16_t>(SDL_KMOD_LALT));
    if ((bits & keys::meta) != 0)
        mods = static_cast<uint16_t>(mods | static_cast<uint16_t>(SDL_KMOD_LGUI));
    if ((bits & keys::primary) != 0) {
#ifdef __APPLE__
        mods = static_cast<uint16_t>(mods | static_cast<uint16_t>(SDL_KMOD_LGUI));
#else
        mods = static_cast<uint16_t>(mods | static_cast<uint16_t>(SDL_KMOD_LCTRL));
#endif
    }
    return mods;
}

/// A key press for a chord, or an event no screen is offered when the chord
/// is not a key.
///
/// @param chord the chord that matched
/// @param map the keyboard the bindings are using
/// @return the press, with type 0 when the chord names no key
SDL_Event key_press_for(const keys::Chord& chord, const keys::KeyboardMap& keyboard) {
    SDL_Event event{};
    uint32_t code = 0;
    uint16_t scancode = chord.scancode;
    switch (chord.kind) {
    case keys::TriggerKind::named: {
        const std::span<const keys::NamedKeySpec> named = keys::named_keys();
        if (chord.named >= named.size())
            return event;
        code = named[chord.named].key_code;
        scancode = named[chord.named].scancode;
        break;
    }
    case keys::TriggerKind::place:
        code = keyboard.event_key_code(chord.scancode);
        break;
    case keys::TriggerKind::character: {
        char32_t printed = chord.character;
        if (printed >= U'A' && printed <= U'Z')
            printed = printed - U'A' + U'a';
        if (printed == 0 || printed > 0x10FFFF)
            return event;
        code = static_cast<uint32_t>(printed);
        break;
    }
    case keys::TriggerKind::mouse:
    case keys::TriggerKind::wheel:
    case keys::TriggerKind::pad:
    case keys::TriggerKind::modifier_alone:
        return event;
    }
    if (code == 0)
        return event;
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.key = static_cast<SDL_Keycode>(code);
    event.key.scancode = static_cast<SDL_Scancode>(scancode);
    event.key.mod = static_cast<SDL_Keymod>(event_modifiers(chord.modifiers));
    event.key.down = true;
    event.key.repeat = false;
    return event;
}

/// The move an arrow, Home, End or a page key asks for when the screen did
/// not read the layout.
///
/// @param key the platform key
/// @return the move, or none
MenuMove place_key_move(uint32_t key) {
    if (key == static_cast<uint32_t>(SDLK_LEFT))
        return MenuMove::left;
    if (key == static_cast<uint32_t>(SDLK_RIGHT))
        return MenuMove::right;
    if (key == static_cast<uint32_t>(SDLK_UP))
        return MenuMove::up;
    if (key == static_cast<uint32_t>(SDLK_DOWN))
        return MenuMove::down;
    if (key == static_cast<uint32_t>(SDLK_HOME))
        return MenuMove::home;
    if (key == static_cast<uint32_t>(SDLK_END))
        return MenuMove::end;
    if (key == static_cast<uint32_t>(SDLK_PAGEUP))
        return MenuMove::page_up;
    if (key == static_cast<uint32_t>(SDLK_PAGEDOWN))
        return MenuMove::page_down;
    return MenuMove::none;
}

} // namespace

MenuMoveHit Runtime::menu_move(MenuMoveCount count_as) const {
    const std::span<const keys::ActionEvent> fed = match_action_events();
    std::vector<MenuAction> actions;
    actions.reserve(fed.size());
    for (const keys::ActionEvent& event : fed) {
        MenuMovePhase phase = MenuMovePhase::other;
        if (event.phase == keys::Phase::pressed)
            phase = MenuMovePhase::down;
        else if (event.phase == keys::Phase::repeated)
            phase = MenuMovePhase::repeated;
        else if (event.phase == keys::Phase::released)
            phase = MenuMovePhase::released;
        actions.push_back(MenuAction{event.action, phase});
    }
    return menu_move_fired(actions.data(), actions.size(), count_as);
}

MenuMove Runtime::ring_move(const ScreenInput& input) const {
    if (screen_bound_asked_)
        return screen_bound_move_;
    const MenuMove fired = menu_move(MenuMoveCount::down).move;
    if (fired == MenuMove::next || fired == MenuMove::previous)
        return fired;
    if (input.key == static_cast<uint32_t>(SDLK_TAB))
        return MenuMove::next;
    return place_key_move(input.key);
}

void Runtime::deliver_menu_repeats() {
    if (delivering_menu_repeats_)
        return;
    const std::span<const keys::ActionEvent> live = match_action_events();
    if (live.empty())
        return;
    delivering_menu_repeats_ = true;

    struct StopRepeat {
        bool& open;

        ~StopRepeat() { open = false; }
    } stop_repeat{delivering_menu_repeats_};

    // Put the frame's events back however this returns, including when a
    // screen asked to end the run.
    struct RestoreEvents {
        Runtime* runtime;
        std::vector<keys::ActionEvent> kept;

        ~RestoreEvents() { runtime->swap_action_events(kept); }
    } restore{this, std::vector<keys::ActionEvent>(live.begin(), live.end())};

    const keys::KeyboardMap keyboard = key_map();
    for (const keys::ActionEvent& fed : restore.kept) {
        if (fed.phase != keys::Phase::repeated)
            continue;
        if (!menu_move_is_travel(menu_move_of(fed.action)))
            continue;
        const SDL_Event press = key_press_for(fed.chord, keyboard);
        if (press.type != SDL_EVENT_KEY_DOWN)
            continue;
        // A key whose press no screen read through the layout is left to the
        // platform's repeats, as before: those screens take them already, and
        // a second repeat would move them twice.
        if (menu_repeat_key_ == 0 || static_cast<uint32_t>(press.key.key) != menu_repeat_key_)
            continue;
        std::vector<keys::ActionEvent> one{fed};
        swap_action_events(one);
        if (dispatch_screen_input(press))
            apply_screen_request();
    }
}

} // namespace oa::app

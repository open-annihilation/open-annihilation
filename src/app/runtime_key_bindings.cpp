// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The key bindings' dispatcher. Each key, button and wheel is turned into
// action events after the input method has taken the keys it is composing.
// The events are kept. Nothing acts on them yet. Touch and the pad keep
// their own paths.

#include "oa/app/runtime.hpp"

#include "device_state.hpp"
#include "engine_settings_state.hpp"
#include "oa/app/match_console.hpp"
#include "oa_layer.hpp"

#include "oa/data/input_actions/actions.hpp"
#include "oa/data/mod_profile.hpp"
#include "oa/sim/gameplay_input/order_cursor.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/key_bindings/dispatcher.hpp"
#include "oa/ui/key_bindings/key_codes.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

namespace keys = oa::ui::key_bindings;
namespace actions = oa::data::input_actions;
namespace console = oa::ui::console;

namespace {

/// The bit of a context, or 0 when the table has no such name.
///
/// @param name the context id
/// @return the bit
uint32_t context_bit(std::string_view name) {
    const std::span<const actions::Context> table = actions::contexts();
    for (std::size_t index = 0; index < table.size() && index < 32; ++index) {
        if (table[index].name == name)
            return uint32_t{1} << static_cast<uint32_t>(index);
    }
    return 0;
}

/// The bit of a condition, or 0 when the table has no such name.
///
/// @param name the condition id
/// @return the bit
uint32_t condition_bit(std::string_view name) {
    const std::span<const std::string_view> table = actions::conditions();
    for (std::size_t index = 0; index < table.size() && index < 32; ++index) {
        if (table[index] == name)
            return uint32_t{1} << static_cast<uint32_t>(index);
    }
    return 0;
}

/// The physical modifier bits an event's modifiers carry. Never `primary`.
///
/// @param mods the SDL modifier bits
/// @return the binding bits
uint8_t binding_modifiers(SDL_Keymod mods) {
    const auto value = static_cast<uint32_t>(mods);
    uint8_t bits = 0;
    if ((value & SDL_KMOD_CTRL) != 0)
        bits = static_cast<uint8_t>(bits | keys::ctrl);
    if ((value & SDL_KMOD_SHIFT) != 0)
        bits = static_cast<uint8_t>(bits | keys::shift);
    if ((value & SDL_KMOD_ALT) != 0)
        bits = static_cast<uint8_t>(bits | keys::alt);
    if ((value & SDL_KMOD_GUI) != 0)
        bits = static_cast<uint8_t>(bits | keys::meta);
    return bits;
}

/// The sign of one wheel axis, as one notch.
///
/// @param value the axis
/// @return 1, -1 or 0
int8_t notch_sign(float value) {
    if (value > 0.F)
        return 1;
    if (value < 0.F)
        return -1;
    return 0;
}

/// The keyboard map SDL is using: what events carry, and what keys print.
///
/// @return the map
keys::KeyboardMap keyboard_from_sdl() {
    keys::KeyboardMap map;
    const int limit =
        std::min(static_cast<int>(SDL_SCANCODE_COUNT), static_cast<int>(keys::scancode_limit));
    for (int index = 1; index < limit; ++index) {
        const auto scancode = static_cast<SDL_Scancode>(index);
        const SDL_Keycode event_code = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, true);
        const SDL_Keycode printed_code = SDL_GetKeyFromScancode(scancode, SDL_KMOD_NONE, false);
        char32_t printed = 0;
        if (printed_code > 0) {
            const auto printed_bits = static_cast<uint32_t>(printed_code);
            if ((printed_bits & keys::position_key_code_bit) == 0)
                printed = static_cast<char32_t>(printed_bits);
        }
        map.set(static_cast<uint16_t>(index), static_cast<uint32_t>(event_code), printed);
    }
    return map;
}

/// Which key Primary means on this build.
///
/// @return the platform
keys::Platform platform_here() {
    keys::Platform where;
#ifdef __APPLE__
    where.primary_is_meta = true;
#else
    where.primary_is_meta = false;
#endif
    return where;
}

/// A key, button or wheel as the dispatcher reads it, or nothing for another event.
///
/// @param event the SDL event
/// @param time_ms the time, from the same clock as `advance`
/// @return the input event
std::optional<keys::InputEvent> input_from_sdl(const SDL_Event& event, uint64_t time_ms) {
    keys::InputEvent input;
    input.time_ms = time_ms;
    if (event.type == SDL_EVENT_KEY_DOWN || event.type == SDL_EVENT_KEY_UP) {
        input.kind = keys::InputKind::key;
        input.down = event.key.down;
        input.repeat = event.key.repeat;
        const auto scan = static_cast<uint32_t>(event.key.scancode);
        input.scancode = scan < keys::scancode_limit ? static_cast<uint16_t>(scan) : uint16_t{0};
        input.key_code = static_cast<uint32_t>(event.key.key);
        input.modifiers = binding_modifiers(event.key.mod);
        return input;
    }
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP) {
        input.kind = keys::InputKind::button;
        input.down = event.button.down;
        input.button = event.button.button;
        input.clicks = event.button.clicks;
        input.modifiers = binding_modifiers(device_state::modifiers_held());
        return input;
    }
    if (event.type == SDL_EVENT_MOUSE_WHEEL) {
        float across = event.wheel.x;
        float away = event.wheel.y;
        if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
            across = -across;
            away = -away;
        }
        input.kind = keys::InputKind::wheel;
        input.down = true;
        input.wheel_x = notch_sign(across);
        input.wheel_y = notch_sign(away);
        input.modifiers = binding_modifiers(device_state::modifiers_held());
        return input;
    }
    return std::nullopt;
}

/// Reports a key the devices or the automation endpoint hold.
///
/// @param context unused
/// @param scancode the position
/// @return true while that key is held
bool key_is_down(const void* context, uint16_t scancode) {
    (void)context;
    return device_state::key_held(static_cast<SDL_Scancode>(scancode));
}

} // namespace

struct Runtime::KeyBindingsState {
    keys::Dispatcher dispatcher{};
    keys::Resolved resolved{};
    keys::KeyboardMap keyboard{};
    keys::Platform platform{};
    std::vector<keys::ActionEvent> events{};
    std::vector<std::string> hacks{};
    keys::PointerScheme scheme{keys::PointerScheme::right_click};
    bool number_keys{};
    uint64_t map_generation{};
    bool ready{};

    /// Whether the resolved bindings name this action.
    ///
    /// @param action the action id
    /// @return true when the id is in the bindings
    [[nodiscard]] bool has_action(std::string_view action) const {
        for (const keys::Bound& bound : resolved.actions) {
            if (bound.id == action)
                return true;
        }
        return false;
    }
};

void Runtime::destroy_key_bindings_state(KeyBindingsState* state) noexcept {
    delete state;
}

/// Whether one hack is on: the display rules, the match's console when it
/// already exists, or the profile the match plays by.
///
/// @param id the hack id
/// @return true while that hack is on
bool Runtime::key_binding_hack(std::string_view id) const {
    const oa::data::mod_profile::UiRules& ui = ui_rules();
    const oa::data::mod_profile::ModProfile* profile = mod_profile();
    if (id == "ui.megamap")
        return ui.megamap.enabled;
    if (id == "ui.whiteboard")
        return ui.whiteboard.enabled;
    if (id == "ui.resource-panel")
        return ui.resource_panel.enabled;
    if (id == "ui.selection-shortcuts")
        return ui.selection_shortcuts.enabled;
    if (id == "ui.build-tools")
        return ui.build_tools.enabled;
    if (id == "ui.click-snap")
        return ui.click_snap.enabled;
    if (id == "ui.build-preview")
        return ui.build_preview.enabled;
    if (id == "ui.options-dialog")
        return ui.options_dialog.enabled;
    if (id == "ui.display-modes")
        return ui.display_modes.enabled;
    if (id == "units.build-rotation")
        return profile != nullptr && profile->rules.units.build_rotation.enabled;
    if (id == "console.key-remaps") {
        if (console_ != nullptr)
            return console_->state.key_remaps;
        return profile != nullptr && profile->rules.console.key_remaps.enabled;
    }
    if (id == "teams.alliance-menu-all-game-types") {
        if (console_ != nullptr)
            return console_->state.team_menu_every_game;
        return profile != nullptr && profile->rules.teams.alliance_menu_all_game_types.enabled;
    }
    return false;
}

void Runtime::ensure_key_bindings(bool keymap_changed) {
    if (!key_bindings_state_) {
        std::unique_ptr<KeyBindingsState, void (*)(KeyBindingsState*) noexcept> made{
            new KeyBindingsState, destroy_key_bindings_state
        };
        key_bindings_state_ = std::move(made);
    }
    KeyBindingsState& state = *key_bindings_state_;
    bool map_changed = false;
    if (keymap_changed || state.map_generation == 0) {
        state.keyboard = keyboard_from_sdl();
        state.platform = platform_here();
        ++state.map_generation;
        if (state.map_generation == 0)
            state.map_generation = 1;
        map_changed = true;
    }

    std::vector<std::string> hacks;
    for (const std::string_view id : actions::hack_ids()) {
        if (key_binding_hack(id))
            hacks.emplace_back(id);
    }
    const int32_t face =
        match_ != nullptr ? match_->state().game.interface_type : preferences_.interface_type;
    const keys::PointerScheme scheme = face == oa::sim::gameplay_input::interface_left_click
                                           ? keys::PointerScheme::left_click
                                           : keys::PointerScheme::right_click;
    const bool number_keys = EngineSettingsState::live_switch_alt(*this);
    if (state.ready && !map_changed && state.hacks == hacks && state.scheme == scheme &&
        state.number_keys == number_keys)
        return;

    // The release events would point into the bindings set_bindings replaces.
    (void)state.dispatcher.release_all(SDL_GetTicks());
    state.events.clear();
    keys::ResolveInput input;
    input.keyboard = &state.keyboard;
    input.platform = state.platform;
    input.hacks_on = hacks;
    input.scheme = scheme;
    input.number_keys_select_groups = number_keys;
    keys::Resolved resolved = keys::resolve(input);
    state.dispatcher.set_keyboard(state.keyboard);
    state.dispatcher.set_platform(state.platform);
    state.dispatcher.set_bindings(resolved);
    state.resolved = std::move(resolved);
    state.hacks = std::move(hacks);
    state.scheme = scheme;
    state.number_keys = number_keys;
    state.ready = true;
    rebuild_pointer_roles(state.resolved);
}

keys::Contexts Runtime::input_contexts() {
    keys::Contexts contexts;
    contexts.active = context_bit("global");
    // A modal Open Annihilation screen takes the input, so the match and the
    // menus are not searched under it.
    if (oa_layer().modal_shown(screen_ == Screen::match)) {
        contexts.active |= context_bit("oa-screens");
    } else if (screen_ == Screen::match && match_ != nullptr) {
        contexts.active |= context_bit("match");
        const bool watcher = (current_extension_state() & extension_state::local_watcher) != 0;
        contexts.active |= watcher ? context_bit("watch") : context_bit("play");
        if (chat_composing_ || whiteboard_input_.editing)
            contexts.active |= context_bit("chat");
        if (match_paused_ && (pause_menu_shown() || team_panel_open()))
            contexts.active |= context_bit("panel");
        if (unit_info_panel_.has_value())
            contexts.active |= context_bit("unit-info");
        if (megamap_shown())
            contexts.active |= context_bit("megamap");
        if (whiteboard_on() && device_state::key_held(SDL_SCANCODE_BACKSLASH))
            contexts.active |= context_bit("whiteboard");
        if (match_command_ == MatchCommand::build || pending_build_type_ != 0)
            contexts.active |= context_bit("placing");
        if (match_drag_.has_value())
            contexts.active |= context_bit("box");
    } else if (screen_ != Screen::match) {
        contexts.active |= context_bit("menus");
    }

    contexts.text_entry = chat_composing_ || whiteboard_input_.editing || save_dialog_open();
    if (multiplayer_session())
        contexts.conditions |= condition_bit("multiplayer");
    else if (match_ != nullptr)
        contexts.conditions |= condition_bit("single-player");
    if (match_ != nullptr && (current_extension_state() & extension_state::local_watcher) == 0)
        contexts.conditions |= condition_bit("seat");
    if (has_local_selection())
        contexts.conditions |= condition_bit("selection");
    if (match_ != nullptr) {
        const oa::Game& game = match_->state().game;
        if ((console::console_flags(game) & console::console_flag::developer) != 0)
            contexts.conditions |= condition_bit("developer");
        if ((game.outcome_flags & console::outcome_flag::debug_keys) != 0)
            contexts.conditions |= condition_bit("debug-keys");
    }
    if (engine_settings().wheel_zoom)
        contexts.conditions |= condition_bit("wheel-zoom");
    else
        contexts.conditions |= condition_bit("no-wheel-zoom");
    return contexts;
}

void Runtime::feed_input_actions(const SDL_Event& event) {
    const bool keymap_changed = event.type == SDL_EVENT_KEYMAP_CHANGED;
    ensure_key_bindings(keymap_changed);
    KeyBindingsState& state = *key_bindings_state_;
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        state.events = state.dispatcher.release_all(SDL_GetTicks()).events;
        return;
    }
    const std::optional<keys::InputEvent> input = input_from_sdl(event, SDL_GetTicks());
    if (!input)
        return;
    state.dispatcher.set_contexts(input_contexts());
    state.events = state.dispatcher.feed(*input).events;
}

void Runtime::advance_input_actions() {
    ensure_key_bindings(false);
    KeyBindingsState& state = *key_bindings_state_;
    const keys::Fed fed = state.dispatcher.advance(SDL_GetTicks());
    // An empty frame leaves the last event's action events in place.
    if (!fed.events.empty())
        state.events = fed.events;
}

void Runtime::apply_binding_roles(uint32_t& modifiers, ModifierUse use) const {
    if (use == ModifierUse::keyboard || key_bindings_state_ == nullptr ||
        !key_bindings_state_->ready)
        return;
    const auto adjust = [&](std::string_view action, SDL_Keymod family, SDL_Keymod added) {
        if (!key_bindings_state_->has_action(action))
            return;
        keys::HeldKeys keys_held;
        keys_held.down = key_is_down;
        const bool active = key_bindings_state_->dispatcher.role_active(
            action, binding_modifiers(static_cast<SDL_Keymod>(modifiers)), keys_held
        );
        const auto family_bits = static_cast<uint32_t>(family);
        if (active) {
            if ((modifiers & family_bits) == 0)
                modifiers |= static_cast<uint32_t>(added);
        } else {
            modifiers &= ~family_bits;
        }
    };
    if (use == ModifierUse::order)
        adjust("order.queue-modifier", SDL_KMOD_SHIFT, SDL_KMOD_LSHIFT);
    else if (use == ModifierUse::selection)
        adjust("select.add-modifier", SDL_KMOD_SHIFT, SDL_KMOD_LSHIFT);
    else if (use == ModifierUse::build_button)
        adjust("build.queue-modifier", SDL_KMOD_SHIFT, SDL_KMOD_LSHIFT);
    if (use == ModifierUse::order)
        adjust("order.force-modifier", SDL_KMOD_CTRL, SDL_KMOD_LCTRL);
}

} // namespace oa::app

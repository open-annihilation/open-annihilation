// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What the gamepad's buttons do in each layer: the grips' latches and FORCE,
// the order and build rings, the groups, the game and standing orders
// layers, SELECT ▾ and the menus (docs/controllers.md). Every action ends in
// a function the mouse, the keyboard, the touch controls or, for the
// Library's own buttons, the Library already call. The top screen of the OA
// layer sees an action, and the right stick's motion, before either becomes
// an action or a turn of the wheel.
#include "oa/app/runtime.hpp"
#include "oa_layer.hpp"
#include "pad_state.hpp"
#include "touch_state.hpp"
#include "oa/app/platform_hooks.hpp"
#include "oa/sim/unit_spawn/spawn_runtime.hpp"
#include "oa/ui/engine_settings/dialog.hpp"
#include "oa/ui/pad_controls.hpp"
#include "oa/ui/touch_hud.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <optional>
#include <string_view>
#include <tuple>

namespace oa::app {
namespace {

namespace pc = oa::ui::pad_controls;
namespace hud = oa::ui::touch_hud;

/// Nanoseconds in a millisecond.
constexpr uint64_t kNanosecondsPerMillisecond = 1'000'000;
/// The build ring's slots the builder's page fills, in the page's order: N, NE, SE, S, SW and
/// NW, leaving W and E to PREV and NEXT.
constexpr std::array<uint8_t, 6> kBuildSlots{0, 1, 3, 4, 5, 7};
/// The standing-orders ring's slot of the fire order (N).
constexpr uint8_t kFireOrdersSlot = 0;
/// The standing-orders ring's slot of the move order (NE).
constexpr uint8_t kMoveOrdersSlot = 1;
/// The standing-orders ring's slot of on and off (SE).
constexpr uint8_t kOnOffSlot = 3;
/// The standing-orders ring's slot of SELF-DESTRUCT (S).
constexpr uint8_t kSelfDestructSlot = 4;
/// The standing-orders ring's slot of INFO (W).
constexpr uint8_t kInfoSlot = 6;
/// The standing-orders ring's slot of cloak (NW).
constexpr uint8_t kCloakSlot = 7;
/// The word in the orders page's fire order toggle's name.
constexpr std::string_view kFireOrdersWord = "FIREORD";
/// The word in the orders page's move order toggle's name.
constexpr std::string_view kMoveOrdersWord = "MOVEORD";
/// The word in the orders page's on and off toggle's name.
constexpr std::string_view kOnOffWord = "ONOFF";
/// The word in the orders page's cloak toggle's name.
constexpr std::string_view kCloakWord = "CLOAK";
/// The pages a builder's type has when it has more than one to turn (pages run 1..count-1).
constexpr int kSeveralBuildPages = 3;

/// Returns whether an action reaches the menus through a repeated step, so the
/// top screen sees it there rather than at the press.
///
/// @param action the action
/// @return whether menu_step sends it
bool menu_repeats(pc::Action action) noexcept {
    switch (action) {
    case pc::Action::focus_up:
    case pc::Action::focus_down:
    case pc::Action::focus_left:
    case pc::Action::focus_right:
    case pc::Action::sheet_up:
    case pc::Action::sheet_down:
    case pc::Action::wheel_up:
    case pc::Action::wheel_down:
        return true;
    default:
        return false;
    }
}

/// Returns whether an action is a layer key's, which a press with it held uses.
///
/// @param action the action
/// @return whether it holds a layer
bool layer_key(pc::Action action) noexcept {
    return action == pc::Action::groups_layer || action == pc::Action::game_layer ||
           action == pc::Action::standing_layer;
}

/// Returns the hold delay a binding's hold waits for.
///
/// @param binding the binding
/// @param hold_ms the shared hold delay, milliseconds
/// @return milliseconds
uint32_t hold_delay(const pc::Binding& binding, uint32_t hold_ms) noexcept {
    return binding.action == pc::Action::self_destruct ? pc::self_destruct_hold_ms : hold_ms;
}

/// Returns the latch a grip's binding names.
///
/// @param action queue or add
/// @return the latch
hud::Latch latch_of(pc::Action action) noexcept {
    return action == pc::Action::add || action == pc::Action::add_toggle ? hud::Latch::add
                                                                         : hud::Latch::queue;
}

} // namespace

void PadAccess::press(Runtime& runtime, pc::PadButton button, uint64_t now) {
    auto& state = runtime.pad_state();
    const auto index = static_cast<std::size_t>(button);
    const uint64_t now_ms = now / kNanosecondsPerMillisecond;
    const bool placing_before = placing(runtime);
    // The pointer pad's click holds the pointer still around it.
    if (button == physical(runtime, pc::PadButton::right_pad))
        state.pointer.press(now);
    // A button pressed with a layer key held, or with a button that has a
    // tap and a hold, uses that press: its release gives no tap.
    for (std::size_t other = 0; other < state.down.size(); ++other)
        if (other != index && state.down[other] && state.holds[other].down() &&
            (layer_key(state.pressed[other].action) ||
             state.taps[other].action != pc::Action::none))
            state.holds[other].use();
    // A ring a tap left open is held again by its own button: its release
    // gives the aimed wedge or closes the ring.
    if (state.ring.kind != PadRingKind::none && state.ring.tapped && state.ring.owner == button) {
        state.ring.button = button;
        state.ring.tapped = false;
        state.ring.aimed = state.ring.aim.slot().has_value();
        state.ring.held_ms = now_ms;
        state.pressed[index] = {};
        state.pressed[index].action =
            state.ring.kind == PadRingKind::order ? pc::Action::order_ring : pc::Action::build_ring;
        return;
    }
    const auto context = map_context(runtime);
    const auto now_layer = layer(runtime);
    const pc::Binding binding = pc::binding_for(context, now_layer, button);
    const pc::Binding tap = pc::tap_binding_for(context, now_layer, button);
    state.pressed[index] = binding;
    state.taps[index] = tap;
    state.deferred[index] = false;
    const bool timed =
        tap.action != pc::Action::none || binding.on_hold || binding.action == pc::Action::group;
    if (timed)
        state.holds[index].press(now_ms);
    // A group's tap and hold, and a hold's action, come later.
    if (binding.action == pc::Action::group || binding.on_hold)
        return;
    // FOLLOW waits for the release while R3 can jump the pointer instead.
    if (binding.action == pc::Action::follow && stick_is_pointer(runtime, runtime.pad_settings())) {
        state.deferred[index] = true;
        state.jumped = false;
        return;
    }
    act_press(runtime, button, binding, now);
    note_placement(runtime, placing_before);
}

void PadAccess::release(Runtime& runtime, pc::PadButton button, uint64_t now) {
    auto& state = runtime.pad_state();
    const auto index = static_cast<std::size_t>(button);
    const uint64_t now_ms = now / kNanosecondsPerMillisecond;
    const bool placing_before = placing(runtime);
    if (button == physical(runtime, pc::PadButton::right_pad))
        state.pointer.release(now);
    const pc::Binding binding = state.pressed[index];
    const pc::Binding tap = state.taps[index];
    const bool deferred = state.deferred[index];
    state.pressed[index] = {};
    state.taps[index] = {};
    state.deferred[index] = false;
    pc::HoldEvent event = pc::HoldEvent::none;
    if (state.holds[index].down())
        event = state.holds[index].release(now_ms, hold_delay(binding, hold_ms(runtime)));
    if (binding.action == pc::Action::group) {
        // A group button's tap selects the group (a second tap centres on
        // it); its hold stored the selection.
        const uint8_t group = binding.index != 0 ? binding.index : state.group_aimed.value_or(0);
        if (event == pc::HoldEvent::tap)
            select_group(runtime, group);
    } else if (deferred) {
        if (!state.jumped)
            act_press(runtime, button, binding, now);
        state.jumped = false;
    } else if (event == pc::HoldEvent::tap && tap.action != pc::Action::none) {
        act_tap(runtime, button, tap, now);
    } else if (binding.on_hold) {
        if (event == pc::HoldEvent::hold_ended)
            act_release(runtime, button, binding, now);
    } else {
        act_release(runtime, button, binding, now);
    }
    note_placement(runtime, placing_before);
}

void PadAccess::act_press(
    Runtime& runtime, pc::PadButton button, pc::Binding binding, uint64_t now
) {
    auto& state = runtime.pad_state();
    const uint64_t now_ms = now / kNanosecondsPerMillisecond;
    const bool queue_held = queue_grip_held(runtime);
    const auto start_steps = [&](pc::Action action) {
        // A held D-pad arm steps again after the menu delay, then at the
        // menu interval.
        state.dpad = {};
        std::ignore = state.dpad.repeat.press(now_ms);
        state.dpad.button = button;
        state.dpad.action = action;
        menu_step(runtime, action);
    };
    // A repeated step is offered when it is sent. Everything else is offered
    // once, here, before it becomes a key or a click.
    if (!menu_repeats(binding.action) && runtime.oa_layer_ != nullptr &&
        runtime.oa_layer_->take_pad_action(binding.action))
        return;
    switch (binding.action) {
    case pc::Action::none:
    case pc::Action::force:
    case pc::Action::groups_layer:
    case pc::Action::game_layer:
    case pc::Action::standing_layer:
    case pc::Action::zoom_in:
    case pc::Action::zoom_out:
    case pc::Action::unit_jump:
    case pc::Action::group:
        // Layer keys and held actions act through what is held; groups by
        // their timers.
        return;
    case pc::Action::left_button:
        pointer_button(runtime, true, true, now);
        return;
    case pc::Action::right_button:
        pointer_button(runtime, false, true, now);
        return;
    case pc::Action::queue:
    case pc::Action::add:
        press_latch(runtime, button, latch_of(binding.action), now);
        return;
    case pc::Action::queue_toggle:
    case pc::Action::add_toggle:
        toggle_latch(runtime, latch_of(binding.action), now);
        return;
    case pc::Action::order_ring:
        open_order_ring(runtime, button, now);
        return;
    case pc::Action::build_ring:
        open_build_ring(runtime, button, now);
        return;
    case pc::Action::centre:
        runtime.press_match_action("camera.centre");
        return;
    case pc::Action::follow:
        // With R4 held the previous unit of the selection is followed.
        runtime.press_match_action(queue_held ? "camera.follow-previous" : "camera.follow");
        if (queue_held)
            use_latch(runtime, hud::ActionClass::order);
        return;
    case pc::Action::clear:
        // While text is typed, B leaves it as Escape does.
        if (text_input_on(runtime))
            send_key(runtime, SDLK_ESCAPE);
        else if (in_match(runtime))
            runtime.cancel_or_clear_match_command();
        return;
    case pc::Action::stop:
        runtime.press_match_action("order.stop");
        return;
    case pc::Action::select_type: {
        if (!in_match(runtime))
            return;
        sync_pointer(runtime);
        // Every unit of the type under the pointer, as a double click on it
        // gives; with no own unit there, the primary unit's type.
        const uint16_t unit = runtime.hovered_match_unit_;
        const auto& slots = runtime.match_->world().slots;
        const bool own = unit != 0 && unit < slots.size() && slots[unit].unit != nullptr &&
                         slots[unit].owner_index == runtime.match_local_player_;
        if (own) {
            runtime.select_match_unit(state.pointer_x, state.pointer_y, 2);
            use_latch(runtime, hud::ActionClass::selection);
        } else {
            runtime.press_match_action("select.same-type");
        }
        return;
    }
    case pc::Action::commander: {
        if (!runtime.touch_) {
            runtime.press_match_action("select.commander");
            return;
        }
        // With ADD the commander joins the selection.
        const bool add = runtime.touch_->hud.latches.active(hud::Latch::add);
        runtime.press_match_action(add ? "select.commander-add" : "select.commander");
        if (add)
            use_latch(runtime, hud::ActionClass::selection);
        return;
    }
    case pc::Action::next_unit:
        runtime.press_match_action("camera.next-unit");
        return;
    case pc::Action::next_report:
        runtime.press_match_action("camera.next-report");
        return;
    case pc::Action::select_menu: {
        if (!in_match(runtime))
            return;
        auto& look = runtime.touch_state().hud;
        if (look.sheet == hud::Sheet::select_menu) {
            TouchDispatchAccess::close_sheet(runtime, true);
            state.sheet_focus = -1;
        } else {
            TouchDispatchAccess::open_sheet(runtime, hud::Sheet::select_menu);
            state.sheet_focus = 0;
        }
        runtime.touch_state().hud.sheet_focus = state.sheet_focus;
        return;
    }
    case pc::Action::keys_overlay:
        runtime.toggle_keys_overlay_screen();
        return;
    case pc::Action::unit_info:
        // The unit under the pointer; with R4 held it is pinned. The fallback
        // map's tap of View. The default map's View is the keys overlay.
        sync_pointer(runtime);
        runtime.press_match_action(queue_held ? "game.pin-unit" : "game.unit-info");
        if (queue_held)
            use_latch(runtime, hud::ActionClass::order);
        return;
    case pc::Action::game_menu:
        runtime.press_match_action("game.menu");
        return;
    case pc::Action::previous_page:
        runtime.press_match_action("build.page-previous");
        return;
    case pc::Action::next_page:
        runtime.press_match_action("build.page-next");
        return;
    case pc::Action::chat:
        if (in_match(runtime))
            runtime.open_chat_line();
        return;
    case pc::Action::kill_board:
        runtime.press_match_action("game.kill-board");
        return;
    case pc::Action::pause:
        runtime.press_match_action("game.pause");
        return;
    case pc::Action::health_bars:
        runtime.press_match_action("game.health-bars");
        return;
    case pc::Action::faster:
        runtime.press_match_action("game.speed-up");
        return;
    case pc::Action::slower:
        runtime.press_match_action("game.speed-down");
        return;
    case pc::Action::clear_messages:
        runtime.press_match_action("game.clear-messages");
        return;
    case pc::Action::team_menu: {
        // Tab is the allies menu in a shared game and while the team-menu hack is on; alone it
        // is the next primary unit.
        const bool allies = runtime.keeps_running_inactive() ||
                            runtime.key_binding_hack("teams.alliance-menu-all-game-types");
        runtime.press_match_action(allies ? "game.team-menu" : "select.next-primary");
        return;
    }
    case pc::Action::share_panel:
        // A shared game's share panel. Alone, Shift+Tab is the previous primary unit, unless
        // the team-menu hack has taken that chord for the allies menu.
        if (runtime.keeps_running_inactive())
            runtime.press_match_action("game.share-panel");
        else if (runtime.key_binding_hack("teams.alliance-menu-all-game-types"))
            runtime.press_match_action("game.team-menu");
        else
            runtime.press_match_action("select.previous-primary");
        return;
    case pc::Action::screenshot:
        runtime.press_match_action("game.screenshot");
        return;
    case pc::Action::fire_orders:
        runtime.press_match_action("order.fire-state");
        return;
    case pc::Action::move_orders:
        runtime.press_match_action("order.move-state");
        return;
    case pc::Action::cloak:
        runtime.press_match_action("order.cloak");
        return;
    case pc::Action::on_off:
        runtime.press_match_action("order.on-off");
        return;
    case pc::Action::self_destruct:
        // Its hold ran out: the countdown starts, or stops.
        runtime.play_haptic(Haptic::hold_started);
        runtime.press_match_action("order.self-destruct");
        return;
    case pc::Action::ring_arm:
        give_ring(runtime, true, false);
        return;
    case pc::Action::ring_close:
        close_ring(runtime);
        return;
    case pc::Action::ring_give:
        give_ring(runtime, false, false);
        return;
    case pc::Action::ring_reduce:
        reduce_ring(runtime);
        return;
    case pc::Action::minimap:
        minimap(runtime, true);
        return;
    case pc::Action::sheet_up:
    case pc::Action::sheet_down:
    case pc::Action::focus_up:
    case pc::Action::focus_down:
    case pc::Action::focus_left:
    case pc::Action::focus_right:
        start_steps(binding.action);
        return;
    case pc::Action::sheet_pick:
    case pc::Action::sheet_close:
        sheet_action(runtime, binding.action);
        return;
    case pc::Action::focus_next:
        send_key(runtime, SDLK_TAB);
        return;
    case pc::Action::focus_previous:
        send_key(runtime, SDLK_TAB, SDL_KMOD_LSHIFT);
        return;
    case pc::Action::press: {
        // The settings dialog's focused control takes Space; elsewhere the
        // focused button or the default one takes Return.
        auto* dialog = runtime.engine_settings_dialog();
        const bool focused =
            dialog != nullptr && dialog->focused != oa::ui::engine_settings::no_control;
        send_key(runtime, focused ? SDLK_SPACE : SDLK_RETURN);
        return;
    }
    case pc::Action::back:
        send_key(runtime, SDLK_ESCAPE);
        return;
    case pc::Action::default_button:
        send_key(runtime, SDLK_RETURN);
        return;
    case pc::Action::info:
        send_key(runtime, SDLK_F1);
        return;
    case pc::Action::library_search:
    case pc::Action::library_action:
        // The Library takes these while it is the top screen. MUTATORS… takes
        // Y on the skirmish setup and the campaign launch. On every other
        // screen Y and X do nothing, as they did before they had an action.
        return;
    case pc::Action::wheel_up:
    case pc::Action::wheel_down:
        menu_step(runtime, binding.action);
        return;
    default:
        return;
    }
}

void PadAccess::act_release(
    Runtime& runtime, pc::PadButton button, pc::Binding binding, uint64_t now
) {
    auto& state = runtime.pad_state();
    switch (binding.action) {
    case pc::Action::left_button:
        pointer_button(runtime, true, false, now);
        return;
    case pc::Action::right_button:
        pointer_button(runtime, false, false, now);
        return;
    case pc::Action::queue:
    case pc::Action::add:
        release_latch(runtime, button, now);
        return;
    case pc::Action::order_ring:
    case pc::Action::build_ring:
        release_ring(runtime, button, now);
        return;
    case pc::Action::minimap:
        minimap(runtime, false);
        return;
    case pc::Action::sheet_up:
    case pc::Action::sheet_down:
    case pc::Action::focus_up:
    case pc::Action::focus_down:
    case pc::Action::focus_left:
    case pc::Action::focus_right:
        if (state.dpad.button == button)
            state.dpad = {};
        return;
    default:
        return;
    }
}

void PadAccess::act_tap(Runtime& runtime, pc::PadButton button, pc::Binding binding, uint64_t now) {
    act_press(runtime, button, binding, now);
}

void PadAccess::advance_holds(Runtime& runtime, uint64_t now) {
    auto& state = runtime.pad_state();
    const uint64_t now_ms = now / kNanosecondsPerMillisecond;
    const uint32_t shared = hold_ms(runtime);
    for (std::size_t index = 0; index < state.holds.size(); ++index) {
        auto& timer = runtime.pad_state().holds[index];
        if (!timer.down())
            continue;
        const pc::Binding binding = runtime.pad_state().pressed[index];
        if (timer.advance(now_ms, hold_delay(binding, shared)) != pc::HoldEvent::hold_started)
            continue;
        const auto button = static_cast<pc::PadButton>(index);
        const bool placing_before = placing(runtime);
        if (binding.action == pc::Action::group) {
            store_group(
                runtime, binding.index != 0 ? binding.index : state.group_aimed.value_or(0)
            );
        } else if (binding.on_hold) {
            // A ring opened by a hold (the fallback's R1 and L1) marks it.
            if (binding.action == pc::Action::order_ring ||
                binding.action == pc::Action::build_ring)
                runtime.play_pad_feel(pc::Feel::hold_started);
            act_press(runtime, button, binding, now);
        }
        note_placement(runtime, placing_before);
    }
    // A held D-pad arm's repeated steps.
    auto& dpad = runtime.pad_state().dpad;
    if (dpad.action == pc::Action::none)
        return;
    const auto arm = static_cast<std::size_t>(dpad.button);
    if (arm >= state.down.size() || !state.down[arm]) {
        dpad = {};
        return;
    }
    const pc::Action action = dpad.action;
    for (uint32_t steps = dpad.repeat.advance(now_ms); steps > 0; --steps)
        menu_step(runtime, action);
}

void PadAccess::press_latch(
    Runtime& runtime, pc::PadButton button, hud::Latch latch, uint64_t now
) {
    auto& state = runtime.pad_state();
    const auto index = static_cast<std::size_t>(button);
    // Whether the pointer is on a build button is known at the press, so one
    // grip is QUEUE and x5.
    const hud::Latch chosen = latch == hud::Latch::queue && pointer_on_build_button(runtime)
                                  ? hud::Latch::times_five
                                  : latch;
    runtime.touch_state().hud.latches.press(chosen, now / kNanosecondsPerMillisecond);
    state.latches[index] = chosen;
    runtime.refresh_pointer_modifiers();
}

void PadAccess::release_latch(Runtime& runtime, pc::PadButton button, uint64_t now) {
    auto& state = runtime.pad_state();
    const auto index = static_cast<std::size_t>(button);
    const auto latch = state.latches[index];
    state.latches[index].reset();
    if (!latch)
        return;
    runtime.touch_state().hud.latches.release(
        *latch, now / kNanosecondsPerMillisecond, hold_ms(runtime)
    );
    runtime.refresh_pointer_modifiers();
}

void PadAccess::toggle_latch(Runtime& runtime, hud::Latch latch, uint64_t now) {
    const hud::Latch chosen = latch == hud::Latch::queue && pointer_on_build_button(runtime)
                                  ? hud::Latch::times_five
                                  : latch;
    const uint64_t now_ms = now / kNanosecondsPerMillisecond;
    auto& latches = runtime.touch_state().hud.latches;
    // A press and a release at once: the chip's tap.
    latches.press(chosen, now_ms);
    latches.release(chosen, now_ms, hold_ms(runtime));
    runtime.refresh_pointer_modifiers();
}

void PadAccess::use_latch(Runtime& runtime, hud::ActionClass action) {
    if (!runtime.touch_)
        return;
    runtime.touch_->hud.latches.used(action, latch_mode(runtime));
    runtime.refresh_pointer_modifiers();
}

bool PadAccess::queue_grip_held(const Runtime& runtime) {
    const Runtime::PadState* state = runtime.pad_state_if_made();
    if (state == nullptr)
        return false;
    for (std::size_t index = 0; index < state->down.size(); ++index)
        if (state->down[index] && state->pressed[index].action == pc::Action::queue)
            return true;
    return false;
}

void PadAccess::send_key(Runtime& runtime, SDL_Keycode key, SDL_Keymod mods) {
    SDL_Event event{};
    event.type = SDL_EVENT_KEY_DOWN;
    event.key.timestamp = SDL_GetTicksNS();
    event.key.windowID = runtime.sdl_.window != nullptr ? SDL_GetWindowID(runtime.sdl_.window) : 0;
    event.key.scancode = SDL_GetScancodeFromKey(key, nullptr);
    event.key.key = key;
    event.key.mod = mods;
    event.key.down = true;
    event.key.repeat = false;
    dispatch(runtime, event);
    SDL_Event up = event;
    up.type = SDL_EVENT_KEY_UP;
    up.key.timestamp = SDL_GetTicksNS();
    up.key.down = false;
    dispatch(runtime, up);
}

void PadAccess::select_group(Runtime& runtime, uint8_t group) {
    if (!in_match(runtime) || group < 1 || group > hud::group_ring_slot_count)
        return;
    const bool add =
        runtime.touch_ != nullptr && runtime.touch_->hud.latches.active(hud::Latch::add);
    runtime.select_squad(group, add);
    use_latch(runtime, hud::ActionClass::selection);
}

void PadAccess::store_group(Runtime& runtime, uint8_t group) {
    if (!in_match(runtime) || group < 1 || group > hud::group_ring_slot_count)
        return;
    runtime.play_haptic(Haptic::hold_started);
    runtime.assign_squad(group);
}

void PadAccess::open_order_ring(Runtime& runtime, pc::PadButton button, uint64_t now) {
    if (!in_match(runtime))
        return;
    sync_pointer(runtime);
    auto& state = runtime.pad_state();
    // The touch layer lays the ring out on its viewport.
    std::ignore = ring_viewport(runtime);
    runtime.touch_state().hud.build_ring.reset();
    TouchDispatchAccess::open_radial(runtime, state.pointer_x, state.pointer_y);
    if (!runtime.touch_state().hud.radial) {
        state.ring = {};
        return;
    }
    state.ring = {};
    state.ring.kind = PadRingKind::order;
    state.ring.owner = button;
    state.ring.button = button;
    state.ring.by_pad = true;
    state.ring.held_ms = now / kNanosecondsPerMillisecond;
    state.ring.open_x = runtime.pointer_x_;
    state.ring.open_y = runtime.pointer_y_;
    state.ring.aim.configure(static_cast<uint8_t>(hud::radial_slot_count));
}

void PadAccess::open_build_ring(Runtime& runtime, pc::PadButton button, uint64_t now) {
    if (!in_match(runtime))
        return;
    sync_pointer(runtime);
    auto& touch = runtime.touch_state();
    // A sheet closes first: closing it may load the selection's page again.
    touch.hud.radial.reset();
    if (touch.hud.sheet != hud::Sheet::none)
        TouchDispatchAccess::close_sheet(runtime, true);
    bool factory = false;
    const auto content = build_ring_content(runtime, factory, true);
    auto& state = runtime.pad_state();
    if (!content) {
        state.ring = {};
        return;
    }
    const hud::Point anchor{
        static_cast<int>(std::lround(state.pointer_x)),
        static_cast<int>(std::lround(state.pointer_y))
    };
    runtime.touch_state().hud.build_ring =
        hud::make_build_ring(anchor, *content, ring_viewport(runtime));
    state.ring = {};
    state.ring.kind = PadRingKind::build;
    state.ring.owner = button;
    state.ring.button = button;
    state.ring.by_pad = true;
    state.ring.held_ms = now / kNanosecondsPerMillisecond;
    state.ring.open_x = runtime.pointer_x_;
    state.ring.open_y = runtime.pointer_y_;
    state.ring.aim.configure(static_cast<uint8_t>(hud::build_ring_slot_count));
    state.ring.unit = runtime.selected_match_unit_;
    state.ring.factory = factory;
}

void PadAccess::refresh_rings(Runtime& runtime) {
    auto& state = runtime.pad_state();
    auto* touch = runtime.touch_.get();
    if (touch == nullptr) {
        state.ring = {};
        return;
    }
    // A radial a finger opened is the pad's to aim and pick too.
    if (touch->hud.radial && state.ring.kind == PadRingKind::none) {
        state.ring = {};
        state.ring.kind = PadRingKind::order;
        state.ring.tapped = true;
        state.ring.open_x = runtime.pointer_x_;
        state.ring.open_y = runtime.pointer_y_;
        state.ring.aim.configure(static_cast<uint8_t>(hud::radial_slot_count));
        return;
    }
    if (state.ring.kind == PadRingKind::order) {
        if (!touch->hud.radial)
            state.ring = {};
        return;
    }
    if (state.ring.kind != PadRingKind::build)
        return;
    if (!touch->hud.build_ring) {
        state.ring = {};
        return;
    }
    // The ring follows the selection's page and counts, and closes when its
    // builder is no longer the selection.
    if (!in_match(runtime) || runtime.selected_match_unit_ != state.ring.unit) {
        close_ring(runtime);
        return;
    }
    bool factory = false;
    const auto content = build_ring_content(runtime, factory, false);
    if (!content) {
        close_ring(runtime);
        return;
    }
    const auto anchor = runtime.touch_state().hud.build_ring->anchor;
    runtime.touch_state().hud.build_ring =
        hud::make_build_ring(anchor, *content, ring_viewport(runtime));
    runtime.pad_state().ring.factory = factory;
}

std::optional<hud::BuildRingContent>
PadAccess::build_ring_content(Runtime& runtime, bool& factory, bool open_page) {
    factory = false;
    const uint16_t unit = runtime.selected_match_unit_;
    if (unit == 0 || !runtime.match_hud_ || !runtime.match_)
        return std::nullopt;
    hud::BuildRingContent content{};
    content.gadgets.fill(-1);
    const auto* definition = runtime.definition_for(unit);
    if (definition != nullptr && definition->builder) {
        factory = definition->bm_code == 0;
        // The builder's page shows, as its build button would show it.
        if (open_page && runtime.match_build_page_ <= 0)
            runtime.open_match_build_page(1);
        // The drawer's list is the build page's tiles whatever tab the
        // phone's drawer last showed.
        auto* touch = runtime.touch_.get();
        const auto tab = touch != nullptr ? touch->hud.drawer_tab : hud::DrawerTab::build;
        if (touch != nullptr)
            touch->hud.drawer_tab = hud::DrawerTab::build;
        const auto sheet = runtime.drawer_sheet_gadgets();
        if (touch != nullptr)
            touch->hud.drawer_tab = tab;
        const auto& gadgets = runtime.match_hud_->layout.gadgets;
        const std::size_t count = std::min<std::size_t>(sheet.button_count, kBuildSlots.size());
        for (std::size_t tile = 0; tile < count; ++tile) {
            const int16_t gadget = sheet.buttons[tile];
            if (gadget < 0 || static_cast<std::size_t>(gadget) >= gadgets.size())
                continue;
            const uint8_t slot = kBuildSlots[tile];
            content.kinds[slot] = hud::BuildWedgeKind::build;
            content.gadgets[slot] = gadget;
            content.available[slot] =
                runtime.gadget_command_available(gadgets[static_cast<std::size_t>(gadget)]);
            const uint16_t type = oa::sim::unit_spawn::find_type_index(
                runtime.spawn_type_names_, gadgets[static_cast<std::size_t>(gadget)].common.name
            );
            if (type != 0)
                content.queued[slot] = static_cast<uint16_t>(
                    std::clamp(runtime.match_->queued_build_count(unit, type), 0, 0xffff)
                );
        }
        const bool pages = runtime.builder_gui_page_count() >= kSeveralBuildPages ||
                           sheet.button_count > kBuildSlots.size();
        content.kinds[hud::build_ring_prev_slot] = hud::BuildWedgeKind::prev;
        content.available[hud::build_ring_prev_slot] = pages;
        content.kinds[hud::build_ring_next_slot] = hud::BuildWedgeKind::next;
        content.available[hud::build_ring_next_slot] = pages;
        return content;
    }
    // No builder: the standing orders the loaded orders page has, INFO and
    // SELF-DESTRUCT.
    content.standing_orders = true;
    const auto more = runtime.more_sheet_gadgets();
    const auto& gadgets = runtime.match_hud_->layout.gadgets;
    const auto place = [&](uint8_t slot, hud::BuildWedgeKind kind, std::string_view word) {
        content.kinds[slot] = kind;
        for (std::size_t toggle = 0; toggle < more.toggle_count; ++toggle) {
            const int16_t gadget = more.toggles[toggle];
            if (gadget < 0 || static_cast<std::size_t>(gadget) >= gadgets.size())
                continue;
            if (std::string_view(gadgets[static_cast<std::size_t>(gadget)].common.name)
                    .find(word) == std::string_view::npos)
                continue;
            content.gadgets[slot] = gadget;
            content.available[slot] = true;
            return;
        }
    };
    place(kFireOrdersSlot, hud::BuildWedgeKind::fire_orders, kFireOrdersWord);
    place(kMoveOrdersSlot, hud::BuildWedgeKind::move_orders, kMoveOrdersWord);
    place(kOnOffSlot, hud::BuildWedgeKind::on_off, kOnOffWord);
    place(kCloakSlot, hud::BuildWedgeKind::cloak, kCloakWord);
    content.kinds[kInfoSlot] = hud::BuildWedgeKind::info;
    content.available[kInfoSlot] = true;
    content.kinds[kSelfDestructSlot] = hud::BuildWedgeKind::self_destruct;
    content.available[kSelfDestructSlot] = true;
    return content;
}

void PadAccess::aim_ring(Runtime& runtime, uint64_t now) {
    auto& state = runtime.pad_state();
    auto* touch = runtime.touch_.get();
    if (state.ring.kind == PadRingKind::none || touch == nullptr)
        return;
    int inner = 0;
    int outer = 0;
    if (state.ring.kind == PadRingKind::order && touch->hud.radial) {
        inner = touch->hud.radial->inner_radius;
        outer = touch->hud.radial->outer_radius;
    } else if (state.ring.kind == PadRingKind::build && touch->hud.build_ring) {
        inner = touch->hud.build_ring->inner_radius;
        outer = touch->hud.build_ring->outer_radius;
    } else {
        return;
    }
    const auto context = map_context(runtime);
    const pc::Vec2 aim_stick = stick(runtime, pc::Side::right);
    std::optional<pc::Vec2> offset;
    float dead_zone = pc::pad_ring_dead_zone;
    if (context.trackpads && state.pointer.touched()) {
        // The right thumb's place on its pad, from the pad's centre: no
        // travel is needed.
        const pc::Vec2 thumb = state.pointer.touch();
        offset = pc::Vec2{(thumb.x - 0.5F) * 2.0F, (thumb.y - 0.5F) * 2.0F};
    } else if (std::hypot(aim_stick.x, aim_stick.y) > pc::stick_ring_dead_zone) {
        offset = aim_stick;
        dead_zone = pc::stick_ring_dead_zone;
    } else if (!context.trackpads && outer > 0) {
        // Steam's mouse, or a pointer moved another way: its travel since
        // the ring opened, the hub aiming at nothing.
        offset = pc::Vec2{
            (runtime.pointer_x_ - state.ring.open_x) / static_cast<float>(outer),
            (runtime.pointer_y_ - state.ring.open_y) / static_cast<float>(outer)
        };
        dead_zone = static_cast<float>(inner) / static_cast<float>(outer);
    }
    // A thumb that lifted keeps the wedge it aimed at.
    if (offset) {
        const auto before = state.ring.aim.slot();
        const auto after = state.ring.aim.aim(*offset, dead_zone);
        state.ring.offset = *offset;
        if (after)
            state.ring.aimed = true;
        if (after && after != before)
            runtime.play_pad_feel(pc::Feel::wedge_change);
    }
    // The standing-orders ring's SELF-DESTRUCT acts after a whole second's
    // aim.
    const auto& ring = touch->hud.build_ring;
    if (state.ring.kind != PadRingKind::build || !ring || !ring->standing_orders)
        return;
    const auto slot = state.ring.aim.slot();
    const bool on = slot && *slot < hud::build_ring_slot_count &&
                    ring->wedges[*slot].kind == hud::BuildWedgeKind::self_destruct &&
                    ring->wedges[*slot].available;
    const uint64_t now_ms = now / kNanosecondsPerMillisecond;
    if (!on) {
        state.ring.self_destruct_ms.reset();
        state.ring.self_destruct_fired = false;
        return;
    }
    if (!state.ring.self_destruct_ms) {
        state.ring.self_destruct_ms = now_ms;
        return;
    }
    if (state.ring.self_destruct_fired ||
        now_ms - *state.ring.self_destruct_ms < pc::self_destruct_hold_ms)
        return;
    state.ring.self_destruct_fired = true;
    runtime.play_haptic(Haptic::hold_started);
    close_ring(runtime);
    runtime.press_match_action("order.self-destruct");
}

void PadAccess::release_ring(Runtime& runtime, pc::PadButton button, uint64_t now) {
    auto& state = runtime.pad_state();
    if (state.ring.kind == PadRingKind::none || state.ring.button != button)
        return;
    state.ring.button = pc::PadButton::none;
    const uint64_t now_ms = now / kNanosecondsPerMillisecond;
    const bool quick = now_ms - state.ring.held_ms < hold_ms(runtime);
    // A tap with nothing aimed leaves the ring open for R2, a pad press, A
    // or B.
    if (!state.ring.aim.slot() && !state.ring.aimed && quick) {
        state.ring.tapped = true;
        return;
    }
    if (state.ring.aim.slot())
        give_ring(runtime, false, true);
    else
        close_ring(runtime);
}

void PadAccess::give_ring(Runtime& runtime, bool arm, bool from_release) {
    auto& state = runtime.pad_state();
    auto* touch = runtime.touch_.get();
    const auto slot = state.ring.aim.slot();
    if (touch == nullptr || !slot) {
        // Released with nothing aimed by the ring's own button: it closes.
        if (from_release)
            close_ring(runtime);
        return;
    }
    if (state.ring.kind == PadRingKind::order) {
        if (!touch->hud.radial || *slot >= hud::radial_slot_count) {
            state.ring = {};
            return;
        }
        const auto wedge = touch->hud.radial->wedges[*slot];
        // A greyed wedge gives nothing and the ring stays open.
        if (!wedge.available) {
            state.ring.tapped = state.ring.button == pc::PadButton::none;
            return;
        }
        if (arm) {
            if (const auto order = hud::radial_order(wedge.item)) {
                // Armed, as its button arms it: the next click gives it and
                // a drag gives it over an area. STOP is given at once.
                close_ring(runtime);
                std::ignore = runtime.arm_match_command(hud::order_name(*order), false);
                return;
            }
        }
        // Given at the ring's point, as arming its button and clicking there
        // does; INFO and TYPE act there.
        TouchDispatchAccess::radial_pick(runtime, wedge.item);
        auto& after = runtime.pad_state();
        if (!runtime.touch_state().hud.radial)
            after.ring = {};
        else
            after.ring.tapped = after.ring.button == pc::PadButton::none;
        return;
    }
    if (state.ring.kind != PadRingKind::build || !touch->hud.build_ring ||
        *slot >= hud::build_ring_slot_count) {
        state.ring = {};
        return;
    }
    const auto wedge = touch->hud.build_ring->wedges[*slot];
    if (!wedge.available) {
        if (from_release)
            state.ring.tapped = true;
        return;
    }
    switch (wedge.kind) {
    case hud::BuildWedgeKind::empty:
    case hud::BuildWedgeKind::self_destruct:
        // SELF-DESTRUCT acts after its hold, never on a press.
        if (from_release)
            close_ring(runtime);
        return;
    case hud::BuildWedgeKind::build: {
        if (wedge.gadget < 0)
            return;
        const auto gadget = static_cast<std::size_t>(wedge.gadget);
        if (state.ring.factory) {
            // A factory's ring closes as L1 comes up; a press adds to the
            // queue and keeps it open.
            if (from_release) {
                close_ring(runtime);
                return;
            }
            // The press is a left click on the build button: one more, or five
            // with QUEUE.
            auto& look = runtime.touch_state().hud;
            const bool five = look.latches.active(hud::Latch::queue) &&
                              !look.latches.active(hud::Latch::times_five);
            if (five)
                look.latches.press(
                    hud::Latch::times_five, now_ns(runtime) / kNanosecondsPerMillisecond
                );
            runtime.activate_match_hud(gadget, true);
            if (five) {
                runtime.touch_state().hud.latches.cancel(hud::Latch::times_five);
                use_latch(runtime, hud::ActionClass::order);
            } else if (runtime.touch_state().hud.latches.active(hud::Latch::times_five)) {
                use_latch(runtime, hud::ActionClass::build_button);
            }
            return;
        }
        // A mobile builder's building: its ghost follows the pointer.
        close_ring(runtime);
        runtime.activate_match_hud(gadget, true);
        return;
    }
    case hud::BuildWedgeKind::prev:
    case hud::BuildWedgeKind::next:
        // The page turns and the ring stays open.
        runtime.press_match_action(
            wedge.kind == hud::BuildWedgeKind::next ? "build.page-next" : "build.page-previous"
        );
        if (from_release)
            runtime.pad_state().ring.tapped = true;
        refresh_rings(runtime);
        return;
    case hud::BuildWedgeKind::fire_orders:
    case hud::BuildWedgeKind::move_orders:
    case hud::BuildWedgeKind::on_off:
    case hud::BuildWedgeKind::cloak:
        close_ring(runtime);
        if (wedge.gadget >= 0)
            runtime.activate_match_hud(static_cast<std::size_t>(wedge.gadget), true);
        return;
    case hud::BuildWedgeKind::info:
        close_ring(runtime);
        TouchDispatchAccess::show_unit_info(runtime);
        return;
    }
}

void PadAccess::reduce_ring(Runtime& runtime) {
    auto& state = runtime.pad_state();
    auto* touch = runtime.touch_.get();
    const auto slot = state.ring.aim.slot();
    if (touch == nullptr || state.ring.kind != PadRingKind::build || !state.ring.factory ||
        !touch->hud.build_ring || !slot || *slot >= hud::build_ring_slot_count)
        return;
    const auto wedge = touch->hud.build_ring->wedges[*slot];
    if (wedge.kind != hud::BuildWedgeKind::build || !wedge.available || wedge.gadget < 0)
        return;
    // A right click on the build button: one off, or five with QUEUE.
    auto& look = touch->hud;
    const bool five =
        look.latches.active(hud::Latch::queue) && !look.latches.active(hud::Latch::times_five);
    if (five)
        look.latches.press(hud::Latch::times_five, now_ns(runtime) / kNanosecondsPerMillisecond);
    runtime.activate_match_hud(static_cast<std::size_t>(wedge.gadget), false);
    if (five) {
        runtime.touch_state().hud.latches.cancel(hud::Latch::times_five);
        use_latch(runtime, hud::ActionClass::order);
    } else if (runtime.touch_state().hud.latches.active(hud::Latch::times_five)) {
        use_latch(runtime, hud::ActionClass::build_button);
    }
    runtime.play_haptic(Haptic::queue_reduced);
}

void PadAccess::close_ring(Runtime& runtime) {
    auto& state = runtime.pad_state();
    if (auto* touch = runtime.touch_.get(); touch != nullptr) {
        if (state.ring.kind == PadRingKind::order)
            touch->hud.radial.reset();
        else if (state.ring.kind == PadRingKind::build)
            touch->hud.build_ring.reset();
    }
    state.ring = {};
}

void PadAccess::sheet_action(Runtime& runtime, pc::Action action) {
    auto& state = runtime.pad_state();
    auto& touch = runtime.touch_state();
    if (touch.hud.sheet != hud::Sheet::select_menu) {
        state.sheet_focus = -1;
        touch.hud.sheet_focus = -1;
        return;
    }
    constexpr auto count = static_cast<int>(hud::select_item_count);
    const int focus = state.sheet_focus;
    switch (action) {
    case pc::Action::sheet_up:
        state.sheet_focus = static_cast<int8_t>(focus < 0 ? 0 : (focus + count - 1) % count);
        break;
    case pc::Action::sheet_down:
        state.sheet_focus = static_cast<int8_t>(focus < 0 ? 0 : (focus + 1) % count);
        break;
    case pc::Action::sheet_pick:
        if (focus < 0)
            return;
        state.sheet_focus = -1;
        touch.hud.sheet_focus = -1;
        TouchDispatchAccess::select_item(runtime, static_cast<uint8_t>(focus));
        return;
    case pc::Action::sheet_close:
        state.sheet_focus = -1;
        touch.hud.sheet_focus = -1;
        TouchDispatchAccess::close_sheet(runtime, true);
        return;
    default:
        return;
    }
    touch.hud.sheet_focus = state.sheet_focus;
}

void PadAccess::menu_step(Runtime& runtime, pc::Action action) {
    if (action == pc::Action::wheel_up || action == pc::Action::wheel_down) {
        // One notch of the wheel, towards the top when the stick is pushed up.
        const float notches = action == pc::Action::wheel_up ? 1.0F : -1.0F;
        if (runtime.oa_layer_ != nullptr && runtime.oa_layer_->take_pad_stick(notches))
            return;
    } else if (runtime.oa_layer_ != nullptr && runtime.oa_layer_->take_pad_action(action)) {
        return;
    }
    switch (action) {
    case pc::Action::focus_up:
        send_key(runtime, SDLK_UP);
        return;
    case pc::Action::focus_down:
        send_key(runtime, SDLK_DOWN);
        return;
    case pc::Action::focus_left:
        send_key(runtime, SDLK_LEFT);
        return;
    case pc::Action::focus_right:
        send_key(runtime, SDLK_RIGHT);
        return;
    case pc::Action::sheet_up:
    case pc::Action::sheet_down:
        sheet_action(runtime, action);
        return;
    case pc::Action::wheel_up:
    case pc::Action::wheel_down: {
        // A wheel step at the pointer: the map and mission lists step.
        sync_pointer(runtime);
        const auto& state = runtime.pad_state();
        const float way = action == pc::Action::wheel_up ? 1.0F : -1.0F;
        SDL_Event event{};
        event.type = SDL_EVENT_MOUSE_WHEEL;
        event.wheel.timestamp = SDL_GetTicksNS();
        event.wheel.windowID = 0;
        event.wheel.which = pad_mouse_id;
        event.wheel.x = 0.0F;
        event.wheel.y = way;
        event.wheel.direction = SDL_MOUSEWHEEL_NORMAL;
        event.wheel.mouse_x = state.pointer_x;
        event.wheel.mouse_y = state.pointer_y;
#if SDL_VERSION_ATLEAST(3, 2, 12)
        event.wheel.integer_y = static_cast<Sint32>(way);
#endif
        dispatch(runtime, event);
        return;
    }
    default:
        return;
    }
}

bool PadAccess::text_input_on(const Runtime& runtime) {
    return runtime.chat_composing_ ||
           (runtime.sdl_.window != nullptr && SDL_TextInputActive(runtime.sdl_.window));
}

void PadAccess::note_placement(Runtime& runtime, bool placing_before) {
    if (placing_before || !placing(runtime))
        return;
    runtime.touch_state().dispatch.placement_by_pad = true;
}

bool PadAccess::placing(const Runtime& runtime) {
    return runtime.match_ && runtime.match_command_ == MatchCommand::build &&
           runtime.pending_build_type_ != 0;
}

} // namespace oa::app

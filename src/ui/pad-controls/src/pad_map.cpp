// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The pad's type, the effective scheme, the layers, the left-handed mirror
// and the maps from buttons to actions (pad_controls.hpp).
#include "oa/ui/pad_controls.hpp"

namespace oa::ui::pad_controls {

namespace {

/// The minimum trackpads for the trackpads scheme: a pointer pad and a camera pad.
constexpr uint8_t trackpads_for_pointer = 2;
/// The highest group a button gives in the groups layer; the left trackpad's ring has one more.
constexpr uint8_t last_button_group = 8;
/// The groups on the left trackpad's ring.
constexpr uint8_t ring_group_count = 9;

/// The buttons by role in the order a prompt prefers them when several give one action: the
/// main button of each role first (R2 before A and the right trackpad for a click, B before the
/// D-pad's left arm for closing SELECT ▾).
constexpr std::array<PadButton, 22> prompt_order{
    PadButton::r2,         PadButton::a,         PadButton::right_pad, PadButton::l2,
    PadButton::b,          PadButton::x,         PadButton::y,         PadButton::r1,
    PadButton::l1,         PadButton::r4,        PadButton::l4,        PadButton::r5,
    PadButton::l5,         PadButton::l3,        PadButton::r3,        PadButton::dpad_up,
    PadButton::dpad_right, PadButton::dpad_down, PadButton::dpad_left, PadButton::view,
    PadButton::menu,       PadButton::left_pad,
};

/// The layers chord_for searches, in order.
constexpr std::array<Layer, 8> chord_layers{
    Layer::base,
    Layer::groups,
    Layer::game,
    Layer::standing_orders,
    Layer::order_ring,
    Layer::build_ring,
    Layer::select_sheet,
    Layer::menus,
};

/// Returns a binding of an action alone.
///
/// @param action the action
/// @return the binding
constexpr Binding bind(Action action) noexcept {
    return Binding{action, 0, false};
}

/// Returns a binding of an action that acts once held past the hold delay.
///
/// @param action the action
/// @return the binding
constexpr Binding bind_hold(Action action) noexcept {
    return Binding{action, 0, true};
}

/// Returns a binding of a group.
///
/// @param group the group, 1..8, or 0 for the left trackpad's ring
/// @return the binding
constexpr Binding bind_group(uint8_t group) noexcept {
    return Binding{Action::group, group, false};
}

/// Returns whether a button is one of the four back grips.
///
/// @param button the button, by role
/// @return whether it is
bool is_grip(PadButton button) noexcept {
    return button == PadButton::l4 || button == PadButton::l5 || button == PadButton::r4 ||
           button == PadButton::r5;
}

/// Returns whether a button is a trackpad's press.
///
/// @param button the button, by role
/// @return whether it is
bool is_trackpad(PadButton button) noexcept {
    return button == PadButton::left_pad || button == PadButton::right_pad;
}

/// Returns the physical button that has a role in a map: itself, or its mirror when
/// left-handed (the mirror is its own inverse).
///
/// @param ctx which map applies
/// @param role the button by role
/// @return the physical button
PadButton physical(const MapContext& ctx, PadButton role) noexcept {
    return ctx.left_handed ? mirrored(role) : role;
}

/// Returns a grip's meaning with grips: the latches, FORCE and the layer keys.
///
/// @param role the grip, by role
/// @return its binding
Binding grip_binding(PadButton role) noexcept {
    switch (role) {
    case PadButton::r4:
        return bind(Action::queue);
    case PadButton::l4:
        return bind(Action::add);
    case PadButton::r5:
        return bind(Action::standing_layer);
    case PadButton::l5:
        return bind(Action::groups_layer);
    default:
        return {};
    }
}

/// Returns a D-pad arm's meaning in the base layer of a scheme.
///
/// @param scheme the effective scheme
/// @param role the arm
/// @return its binding
Binding dpad_base_binding(Scheme scheme, PadButton role) noexcept {
    const bool sticks = scheme == Scheme::sticks;
    switch (role) {
    case PadButton::dpad_up:
        return bind(sticks ? Action::zoom_in : Action::commander);
    case PadButton::dpad_right:
        return bind(sticks ? Action::next_report : Action::next_unit);
    case PadButton::dpad_down:
        return bind(sticks ? Action::zoom_out : Action::next_report);
    case PadButton::dpad_left:
        return bind(Action::select_menu);
    default:
        return {};
    }
}

/// Returns a button's meaning in the base layer, by role (a hold's meaning where it has a tap
/// too).
///
/// @param ctx which map applies
/// @param role the button by role
/// @return its binding
Binding base_binding(const MapContext& ctx, PadButton role) noexcept {
    switch (role) {
    case PadButton::a:
    case PadButton::r2:
    case PadButton::right_pad:
        return bind(Action::left_button);
    case PadButton::l2:
        return bind(Action::right_button);
    case PadButton::b:
        return bind(Action::clear);
    case PadButton::x:
        return bind(Action::stop);
    case PadButton::y:
        return bind(Action::select_type);
    case PadButton::view:
        return bind_hold(ctx.fallback ? Action::groups_layer : Action::game_layer);
    case PadButton::menu:
        return ctx.fallback ? bind_hold(Action::game_layer) : bind(Action::game_menu);
    case PadButton::l1:
        return ctx.fallback ? bind_hold(Action::build_ring) : bind(Action::build_ring);
    case PadButton::r1:
        return ctx.fallback ? bind_hold(Action::order_ring) : bind(Action::order_ring);
    case PadButton::l3:
        return bind(Action::centre);
    case PadButton::r3:
        return bind(Action::follow);
    case PadButton::dpad_up:
    case PadButton::dpad_right:
    case PadButton::dpad_down:
    case PadButton::dpad_left:
        return dpad_base_binding(ctx.scheme, role);
    case PadButton::left_pad:
        return bind(Action::minimap);
    default:
        return {};
    }
}

/// Returns a button's meaning in the groups layer, by role: the D-pad's arms and the face
/// buttons clockwise from the top are groups 1 to 8; the left trackpad is the ring of nine.
///
/// @param ctx which map applies
/// @param role the button by role
/// @return its binding
Binding groups_binding(const MapContext& ctx, PadButton role) noexcept {
    switch (role) {
    case PadButton::dpad_up:
        return bind_group(1);
    case PadButton::dpad_right:
        return bind_group(2);
    case PadButton::dpad_down:
        return bind_group(3);
    case PadButton::dpad_left:
        return bind_group(4);
    case PadButton::y:
        return bind_group(5);
    case PadButton::b:
        return bind_group(6);
    case PadButton::a:
        return bind_group(7);
    case PadButton::x:
        return bind_group(8);
    case PadButton::left_pad:
        return ctx.trackpads ? bind_group(0) : Binding{};
    default:
        return {};
    }
}

/// Returns a button's meaning in the game layer, by role. R1 toggles the keys overlay on the
/// default map; the fallback map leaves that slot empty.
///
/// @param ctx which map applies
/// @param role the button by role
/// @return its binding
Binding game_binding(const MapContext& ctx, PadButton role) noexcept {
    switch (role) {
    case PadButton::a:
        return bind(Action::chat);
    case PadButton::b:
        return bind(Action::kill_board);
    case PadButton::x:
        return bind(Action::pause);
    case PadButton::y:
        return bind(Action::health_bars);
    case PadButton::dpad_up:
        return bind(Action::faster);
    case PadButton::dpad_down:
        return bind(Action::slower);
    case PadButton::dpad_left:
        return bind(Action::clear_messages);
    case PadButton::dpad_right:
        return bind(Action::team_menu);
    case PadButton::l1:
        return bind(Action::share_panel);
    case PadButton::r2:
        return bind(Action::screenshot);
    case PadButton::r1:
        return ctx.fallback ? Binding{} : bind(Action::keys_overlay);
    default:
        return {};
    }
}

/// Returns a button's meaning in the standing orders layer, by role: R2, A and a pad press stay
/// clicks (forced, since R5 is held), L2 the right button.
///
/// @param role the button by role
/// @return its binding
Binding standing_binding(PadButton role) noexcept {
    switch (role) {
    case PadButton::y:
        return bind(Action::fire_orders);
    case PadButton::b:
        return bind(Action::move_orders);
    case PadButton::dpad_up:
        return bind(Action::cloak);
    case PadButton::dpad_down:
        return bind(Action::on_off);
    case PadButton::x:
        return bind_hold(Action::self_destruct);
    case PadButton::r2:
    case PadButton::a:
    case PadButton::right_pad:
        return bind(Action::left_button);
    case PadButton::l2:
        return bind(Action::right_button);
    default:
        return {};
    }
}

/// Returns a button's meaning while a ring is open, by role.
///
/// @param build whether it is the build ring (L2 takes one off a factory's queue)
/// @param role the button by role
/// @return its binding
Binding ring_binding(bool build, PadButton role) noexcept {
    switch (role) {
    case PadButton::a:
        return bind(Action::ring_arm);
    case PadButton::b:
        return bind(Action::ring_close);
    case PadButton::r2:
    case PadButton::right_pad:
        return bind(Action::ring_give);
    case PadButton::l2:
        return build ? bind(Action::ring_reduce) : Binding{};
    default:
        return {};
    }
}

/// Returns a button's meaning while SELECT ▾ is open, by role.
///
/// @param role the button by role
/// @return its binding
Binding sheet_binding(PadButton role) noexcept {
    switch (role) {
    case PadButton::dpad_up:
        return bind(Action::sheet_up);
    case PadButton::dpad_down:
        return bind(Action::sheet_down);
    case PadButton::a:
        return bind(Action::sheet_pick);
    case PadButton::b:
    case PadButton::dpad_left:
        return bind(Action::sheet_close);
    default:
        return {};
    }
}

/// Returns a button's meaning in menus and screens other than the match, by role.
/// Y focuses the Library's search and X does its selected entry's first action;
/// on any other screen those two do nothing.
///
/// @param role the button by role
/// @return its binding
Binding menus_binding(PadButton role) noexcept {
    switch (role) {
    case PadButton::dpad_up:
        return bind(Action::focus_up);
    case PadButton::dpad_right:
        return bind(Action::focus_right);
    case PadButton::dpad_down:
        return bind(Action::focus_down);
    case PadButton::dpad_left:
        return bind(Action::focus_left);
    case PadButton::l1:
        return bind(Action::focus_previous);
    case PadButton::r1:
        return bind(Action::focus_next);
    case PadButton::a:
        return bind(Action::press);
    case PadButton::b:
        return bind(Action::back);
    case PadButton::x:
        return bind(Action::library_action);
    case PadButton::y:
        return bind(Action::library_search);
    case PadButton::menu:
        return bind(Action::default_button);
    case PadButton::view:
        return bind(Action::info);
    case PadButton::r2:
    case PadButton::right_pad:
        return bind(Action::left_button);
    case PadButton::l2:
        return bind(Action::right_button);
    default:
        return {};
    }
}

/// Returns the button whose holding gives a layer, by role, in a map: none for the base layer,
/// the rings, SELECT ▾ and the menus, which nothing held gives.
///
/// @param ctx which map applies
/// @param layer the layer
/// @return the layer key by role, or none
PadButton layer_key(const MapContext& ctx, Layer layer) noexcept {
    switch (layer) {
    case Layer::groups:
        return ctx.fallback ? PadButton::view : PadButton::l5;
    case Layer::game:
        return ctx.fallback ? PadButton::menu : PadButton::view;
    case Layer::standing_orders:
        return ctx.fallback ? PadButton::none : PadButton::r5;
    default:
        return PadButton::none;
    }
}

/// Returns whether a layer can be reached in a map.
///
/// @param ctx which map applies
/// @param layer the layer
/// @return false for a held layer no button gives
bool layer_reachable(const MapContext& ctx, Layer layer) noexcept {
    switch (layer) {
    case Layer::groups:
    case Layer::game:
    case Layer::standing_orders:
        return layer_key(ctx, layer) != PadButton::none;
    default:
        return true;
    }
}

/// Returns whether a physical button exists on the pad a map describes, for prompts.
///
/// @param ctx which map applies
/// @param role the button by role
/// @return false for the grips without them and the trackpads without two
bool button_present(const MapContext& ctx, PadButton role) noexcept {
    if (is_grip(role))
        return !ctx.fallback;
    if (is_trackpad(role))
        return ctx.trackpads;
    return true;
}

/// Returns whether a binding gives an action (and group).
///
/// @param binding the binding
/// @param action the action
/// @param index the group for Action::group
/// @return whether it does
bool gives(const Binding& binding, Action action, uint8_t index) noexcept {
    if (binding.action != action)
        return false;
    return action != Action::group || binding.index == index;
}

/// Returns the chord that gives an action in a map, searching every layer, or none.
///
/// @param ctx which map applies
/// @param action the action
/// @param index the group for Action::group
/// @return the chord, or none
std::optional<Chord> find_chord(const MapContext& ctx, Action action, uint8_t index) noexcept {
    for (const Layer layer : chord_layers) {
        if (!layer_reachable(ctx, layer))
            continue;
        const PadButton key_role = layer_key(ctx, layer);
        const PadButton held =
            key_role == PadButton::none ? PadButton::none : physical(ctx, key_role);
        for (const PadButton role : prompt_order) {
            const PadButton button = physical(ctx, role);
            if (!button_present(ctx, role) || button == held)
                continue;
            const Binding binding = binding_for(ctx, layer, button);
            if (gives(binding, action, index))
                return Chord{held, button, false, binding.on_hold};
            const Binding tap = tap_binding_for(ctx, layer, button);
            if (gives(tap, action, index))
                return Chord{held, button, true, false};
        }
    }
    return std::nullopt;
}

} // namespace

PadType pad_type_of(uint16_t vendor, uint16_t product, PadType reported) noexcept {
    if (vendor == valve_vendor && product == steam_deck_product)
        return PadType::steam_deck;
    return reported;
}

Scheme effective_scheme(Scheme chosen, const PadTraits& traits) noexcept {
    if (chosen == Scheme::sticks)
        return Scheme::sticks;
    // Two trackpads give the pointer; a Deck behind Steam Input has Steam's mouse on its
    // right pad.
    const bool deck_behind_steam = traits.type == PadType::steam_deck && traits.steam_input;
    return traits.trackpads >= trackpads_for_pointer || deck_behind_steam ? Scheme::trackpads
                                                                          : Scheme::sticks;
}

Layer layer_for(const Held& held) noexcept {
    if (!held.in_match)
        return Layer::menus;
    if (held.order_ring)
        return Layer::order_ring;
    if (held.build_ring)
        return Layer::build_ring;
    if (held.select_sheet)
        return Layer::select_sheet;
    if (held.groups)
        return Layer::groups;
    if (held.game)
        return Layer::game;
    if (held.standing)
        return Layer::standing_orders;
    return Layer::base;
}

PadButton mirrored(PadButton button) noexcept {
    switch (button) {
    case PadButton::l1:
        return PadButton::r1;
    case PadButton::r1:
        return PadButton::l1;
    case PadButton::l2:
        return PadButton::r2;
    case PadButton::r2:
        return PadButton::l2;
    case PadButton::l3:
        return PadButton::r3;
    case PadButton::r3:
        return PadButton::l3;
    case PadButton::l4:
        return PadButton::r4;
    case PadButton::r4:
        return PadButton::l4;
    case PadButton::l5:
        return PadButton::r5;
    case PadButton::r5:
        return PadButton::l5;
    case PadButton::left_pad:
        return PadButton::right_pad;
    case PadButton::right_pad:
        return PadButton::left_pad;
    case PadButton::left_stick_touch:
        return PadButton::right_stick_touch;
    case PadButton::right_stick_touch:
        return PadButton::left_stick_touch;
    default:
        return button;
    }
}

Binding binding_for(const MapContext& ctx, Layer layer, PadButton button) noexcept {
    const PadButton role = ctx.left_handed ? mirrored(button) : button;
    if (is_grip(role)) {
        // The grips are modifiers and layer keys: the same in every layer of the match, and
        // nothing in the menus or on a pad without them.
        if (ctx.fallback || layer == Layer::menus)
            return {};
        return grip_binding(role);
    }
    switch (layer) {
    case Layer::base:
        return base_binding(ctx, role);
    case Layer::groups:
        return groups_binding(ctx, role);
    case Layer::game:
        return game_binding(ctx, role);
    case Layer::standing_orders:
        return standing_binding(role);
    case Layer::order_ring:
        return ring_binding(false, role);
    case Layer::build_ring:
        return ring_binding(true, role);
    case Layer::select_sheet:
        return sheet_binding(role);
    case Layer::menus:
        return menus_binding(role);
    }
    return {};
}

Binding tap_binding_for(const MapContext& ctx, Layer layer, PadButton button) noexcept {
    if (layer != Layer::base)
        return {};
    const PadButton role = ctx.left_handed ? mirrored(button) : button;
    switch (role) {
    case PadButton::view:
        return bind(Action::unit_info);
    case PadButton::menu:
        return ctx.fallback ? bind(Action::game_menu) : Binding{};
    case PadButton::r1:
        return ctx.fallback ? bind(Action::queue_toggle) : Binding{};
    case PadButton::l1:
        return ctx.fallback ? bind(Action::add_toggle) : Binding{};
    default:
        return {};
    }
}

std::optional<Chord> chord_for(const MapContext& ctx, Action action, uint8_t index) noexcept {
    if (action == Action::none)
        return std::nullopt;
    // FORCE is R5's own while held: its binding names the standing orders layer it also holds.
    if (action == Action::force) {
        if (ctx.fallback)
            return std::nullopt;
        return Chord{PadButton::none, physical(ctx, PadButton::r5), false, false};
    }
    if (const auto chord = find_chord(ctx, action, index))
        return chord;
    // The latches without grips are the fallback's taps, and with grips the grips.
    switch (action) {
    case Action::queue:
        return find_chord(ctx, Action::queue_toggle, 0);
    case Action::add:
        return find_chord(ctx, Action::add_toggle, 0);
    case Action::queue_toggle:
        return find_chord(ctx, Action::queue, 0);
    case Action::add_toggle:
        return find_chord(ctx, Action::add, 0);
    case Action::group:
        // A group no button gives (9) is on the left trackpad's ring.
        if (index > last_button_group && index <= ring_group_count)
            return find_chord(ctx, Action::group, 0);
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

} // namespace oa::ui::pad_controls

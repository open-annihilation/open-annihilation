// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "tick_internal.hpp"

#include "oa/sim/selection.hpp"

#include <cstdint>

namespace oa::sim::match_runtime {
namespace {
// Control code the observer pulse asks the key state about.
constexpr uint32_t control_code_shift = 0xf9;
} // namespace

void TickHost::before_callback() {
    if (dispatched_flags)
        write_flags(*dispatched_slot, *dispatched_flags);
}

void TickHost::after_callback() {
    if (dispatched_flags) {
        *dispatched_flags = weapon_flags(*dispatched_slot);
        if (match.ground_runtime(dispatched_slot->unit_index))
            ground(*dispatched_slot->unit).project_slot();
    }
}

sim::unit_spawn::Slot& TickHost::slot(sim::simulation_state::Unit& u) {
    return match.slots_[u.record.id];
}

sim::simulation_state::Unit& TickHost::unit_view(oa::Unit& record) {
    return match.units_[record.id];
}

sim::ground_orders::GroundRuntime& TickHost::ground(sim::simulation_state::Unit& u) {
    auto& s = slot(u);
    auto* r = match.ground_runtime(s.unit_index);
    if (r)
        return *r;
    match.fault_.note("ground tick has no controller");
    match.spare_ground_ = std::make_unique<sim::ground_orders::GroundRuntime>();
    return *match.spare_ground_;
}

Match::RuntimeOrder& TickHost::owned(sim::simulation_state::Order& order) {
    if (auto* record = match.runtime_order(&order))
        return *record;
    match.fault_.note("order is not owned by match");
    match.spare_order_ = {};
    return match.spare_order_;
}

std::array<uint8_t, 3> TickHost::weapon_flags(sim::unit_spawn::Slot& s) {
    std::array<uint8_t, 3> result{};
    for (std::size_t i = 0; i < 3; ++i)
        result[i] = s.record.weapons[i].flags;
    return result;
}

void TickHost::write_flags(sim::unit_spawn::Slot& s, const std::array<uint8_t, 3>& flags) {
    for (std::size_t i = 0; i < 3; ++i)
        s.record.weapons[i].flags = flags[i];
}

void TickHost::release_tracked_weapons(sim::unit_spawn::Slot& s, std::array<uint8_t, 3>& flags) {
    write_flags(s, flags);
    match.release_tracked_weapons(s, 3);
    flags = weapon_flags(s);
}

sim::ground_orders::UnitView TickHost::view(
    sim::unit_spawn::Slot& s, sim::ground_orders::GroundRuntime& g, std::array<uint8_t, 3>& flags
) {
    return {*s.unit, g.geometry, g.movement, g.navigation, s.record.attach_parent != 0, flags};
}

void TickHost::script(sim::unit_spawn::Slot& s, ScriptFunction function) {
    auto* u = match.instance(s.unit_index);
    if (u && u->script())
        u->script()->call_no_arguments(function, true);
}

uint32_t TickHost::unit_abilities(const oa::Unit& unit) const {
    const auto* def = oa::world_unit_def_of(&match.state(), &unit);
    return def ? def->abilities : 0u;
}

sim::unit_spawn::Slot& TickHost::movement_slot(const sim::unit_movement::Unit& u) {
    const auto index = static_cast<uint16_t>(u.id);
    if (index >= match.slots_.size()) {
        match.fault_.note("movement unit outside pool");
        return match.slots_[0];
    }
    return match.slots_[index];
}

uint32_t TickHost::random_bounded(uint32_t n) {
    return match.random_.bounded(n);
}

void TickHost::tick_script(oa::Unit& record, uint32_t steps) {
    auto& u = unit_view(record);
    match.bridge_->tick_script(slot(u), steps);
}

void TickHost::update_wind_generator(oa::Unit& record) {
    auto& u = unit_view(record);
    auto& s = slot(u);
    if (!(match.fields(s).definition->wind_generator > 0) || !match.wind_.changed)
        return;
    auto* instance = match.instance(s.unit_index);
    if (!instance || !instance->script()) {
        match.fault_.note("wind generator has no script");
        return;
    }
    const auto direction = static_cast<int32_t>(match.wind_.direction);
    const auto speed = std::bit_cast<int32_t>(match.wind_.speed << 4);
    instance->script()->call(ScriptFunction::set_direction, std::span(&direction, 1), false);
    instance->script()->call(ScriptFunction::set_speed, std::span(&speed, 1), false);
}

int32_t TickHost::terrain_height_under(oa::Unit& record) {
    auto& u = unit_view(record);
    return match.sample_terrain_height(u.position[0], u.position[2]);
}

void TickHost::settle_on_ground(oa::Unit& record) {
    auto& u = unit_view(record);
    auto& s = slot(u);
    if (!match.ground_runtime(s.unit_index))
        return;
    auto& g = ground(u);
    auto* object = match.instance(s.unit_index);
    if (!object) {
        match.fault_.note("ground fit without model");
        return;
    }
    sim::unit_movement::GroundClock clock;
    clock.simulation_tick = match.simulation_.tick;
    if (u.type->flags & OA_UNIT_DEF_FLAG_CAN_HOVER) {
        if (!match.input_.uptime_milliseconds) {
            match.fault_.note("floating fit requires clock");
            return;
        }
        for (auto& n : clock.bob_ticks)
            n = sim::unit_movement::scaled_bob_tick(
                match.input_.uptime_milliseconds(), static_cast<uint32_t>(match.input_.clock_scale)
            );
    }
    (void)g.fit_height(match.terrain_, object->model().model(), clock);
}

void TickHost::cancel_search(sim::ground_orders::Navigation& navigation) {
    match.search_.controller.cancel(navigation);
}

void TickHost::play_sound(sim::simulation_state::Unit& u, uint32_t category) {
    before_callback();
    match.services_.command_sound(slot(u), category);
    after_callback();
}

void TickHost::play_sound(sim::simulation_state::Unit& u, uint32_t category, const char* caption) {
    const auto& hooks = match.speech_hooks_;
    if (caption == nullptr || hooks.speak == nullptr) {
        play_sound(u, category);
        return;
    }
    before_callback();
    hooks.speak(hooks.context, slot(u), category, caption);
    after_callback();
}

sim::simulation_state::Unit* TickHost::find_target(sim::simulation_state::Unit& unit) {
    return match.find_automatic_target(unit);
}

bool TickHost::issue_attack(sim::simulation_state::Unit& from, sim::simulation_state::Unit& to) {
    return match.issue_automatic_attack(from, to);
}

void TickHost::local_player_ticked(oa::Player& player) {
    const auto& multiplayer = match.multiplayer;
    if (multiplayer.local_player_ticked) {
        multiplayer.local_player_ticked(multiplayer.context, player);
        return;
    }
    match.fault_.note("local player tick without a multiplayer handler");
}

bool TickHost::is_key_down(uint32_t code) {
    if (code == control_code_shift && match.observer.shift_down != nullptr)
        return match.observer.shift_down(match.observer.context);
    return true;
}

void TickHost::select_next_viewpoint_unit() {
    oa::sim::selection::Hooks hooks{};
    hooks.context = match.observer.context;
    hooks.selection_cleared = match.observer.selection_cleared;
    oa::sim::selection::select_next_viewpoint_unit(match.state(), hooks);
}

void TickHost::follow_next_selected(uint32_t backward) {
    oa::sim::selection::follow_next_selected(match.state(), backward != 0);
}

} // namespace oa::sim::match_runtime

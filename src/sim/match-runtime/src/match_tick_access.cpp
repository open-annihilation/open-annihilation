// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The tests' access to the per-tick work of a match (match_tick_access.hpp),
// through the match's tick host.
#include "match_tick_access.hpp"
#include "tick_internal.hpp"

namespace oa::sim::match_runtime {

static_assert(MatchTickAccess::building_flag == order_building_flag);

MatchTickAccess::MatchTickAccess(Match& match) : host_(std::make_unique<TickHost>(match)) {
}

MatchTickAccess::~MatchTickAccess() = default;

uint32_t MatchTickAccess::dispatch_mission(
    oa::World& world, oa::Unit& record, sim::simulation_state::Order& order, uint32_t events
) {
    return host_->dispatch_mission(world, record, order, events);
}

sim::air::AirHost MatchTickAccess::air_host() {
    return host_->air_host();
}

void MatchTickAccess::stop_building(
    sim::unit_spawn::Slot& builder, sim::simulation_state::Order& order
) {
    host_->stop_building(builder, order);
}

void MatchTickAccess::kill_unit(oa::Unit& record, uint8_t kind) {
    host_->kill_unit(record, kind);
}

void MatchTickAccess::destroy_player_units(uint8_t owner) {
    host_->destroy_player_units(owner);
}

uint32_t MatchTickAccess::movement_rate(
    const sim::unit_movement::Movement& movement, const oa::Unit& unit, const oa::UnitDef& def
) noexcept {
    return match_runtime::movement_rate(movement, unit, def);
}

bool MatchTickAccess::order_building(const sim::simulation_state::Order& order) noexcept {
    return (order.flags & tick_detail::order_building_flag) != 0;
}

void MatchTickAccess::link_target(
    Match& match, sim::simulation_state::Order& order, sim::simulation_state::Unit* target
) {
    auto* entry = match.runtime_order(&order);
    if (entry == nullptr) {
        match.note_fault("order outside the match");
        return;
    }
    match.link_target_observer(*entry, target);
}

MatchTickAccess::ObserverPlace
MatchTickAccess::observer_place(Match& match, const sim::simulation_state::Order& order) {
    return ObserverPlace{match.runtime_order(&order)};
}

bool MatchTickAccess::list_holds(
    Match& match, sim::simulation_state::Unit& unit, ObserverPlace place
) {
    if (place.node == nullptr)
        return false;
    auto* const* head = match.observer_head(unit);
    if (head == nullptr)
        return false;
    std::size_t steps = overlay_order_budget;
    for (auto* cursor = *head; cursor != nullptr && steps-- > 0; cursor = cursor->observer_next) {
        if (cursor == place.node)
            return true;
    }
    return false;
}

} // namespace oa::sim::match_runtime

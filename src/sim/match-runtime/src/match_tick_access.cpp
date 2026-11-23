// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The tests' access to the per-tick work of a match (match_tick_access.hpp),
// through the match's tick host.
#include "match_tick_access.hpp"
#include "tick_internal.hpp"
#include "enhanced_state.hpp"

#include <algorithm>

namespace oa::sim::match_runtime {

static_assert(MatchTickAccess::building_flag == order_building_flag);

sim::ground_orders::SearchRecord MatchTickAccess::active_path_search(const Match& match) {
    return match.search_.controller.active();
}

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

bool MatchTickAccess::observer_lists_valid(Match& match) {
    const auto active = static_cast<size_t>(std::count_if(
        match.orders_.begin(), match.orders_.end(), [](const auto& entry) { return bool(entry); }
    ));
    for (size_t unit = 0; unit < match.units_.size(); ++unit) {
        auto* cursor = match.target_observers_[unit];
        const sim::simulation_state::Order* previous = nullptr;
        size_t remaining = active;
        while (cursor && remaining-- > 0) {
            if (cursor->attack.target != &match.units_[unit] ||
                match.runtime_order(&cursor->order) != cursor)
                return false;
            if (match.enhanced_ && match.enhanced_->order_ownership) {
                const auto* indexed = match.enhanced_->order_ownership->find(&cursor->order);
                if (indexed &&
                    indexed->observer_slot != OrderOwnershipIndex::uncertified_observer &&
                    (indexed->observer_slot != unit || indexed->observer_previous != previous))
                    return false;
            }
            previous = &cursor->order;
            cursor = cursor->observer_next;
        }
        if (cursor)
            return false;
    }
    for (const auto& entry : match.orders_) {
        if (!entry)
            continue;
        if (!entry->attack.target) {
            if (entry->observer_next)
                return false;
            if (match.enhanced_ && match.enhanced_->order_ownership) {
                const auto* indexed = match.enhanced_->order_ownership->find(&entry->order);
                if (indexed &&
                    indexed->observer_slot != OrderOwnershipIndex::uncertified_observer &&
                    (indexed->observer_slot != OrderOwnershipIndex::no_observer ||
                     indexed->observer_previous != nullptr))
                    return false;
            }
            continue;
        }
        auto** head = match.observer_head(*entry->attack.target);
        if (!head)
            return false;
        auto* cursor = *head;
        size_t remaining = active, found = 0;
        while (cursor && remaining-- > 0) {
            found += cursor == entry.get();
            cursor = cursor->observer_next;
        }
        if (cursor || found != 1)
            return false;
    }
    return true;
}

size_t MatchTickAccess::ownership_sequence(
    const Match& match, std::span<const sim::simulation_state::Order*> output
) {
    size_t count = 0;
    for (const auto& entry : match.orders_) {
        if (!entry)
            continue;
        if (count < output.size())
            output[count] = &entry->order;
        ++count;
    }
    return count;
}

MatchTickAccess::ObserverPlace MatchTickAccess::replace_observer_head(
    Match& match, sim::simulation_state::Unit& unit, ObserverPlace place
) {
    auto** head = match.observer_head(unit);
    if (!head)
        return {};
    const ObserverPlace previous{*head};
    *head = static_cast<Match::RuntimeOrder*>(const_cast<void*>(place.node));
    return previous;
}

bool MatchTickAccess::replace_observer_certificate(
    Match& match, const sim::simulation_state::Order& order, const sim::simulation_state::Unit& unit
) {
    if (!match.enhanced_ || !match.enhanced_->order_ownership)
        return false;
    auto* indexed = match.enhanced_->order_ownership->find(&order);
    auto** head = match.observer_head(unit);
    if (!indexed || !head)
        return false;
    indexed->observer_slot = static_cast<uint32_t>(head - match.target_observers_.get());
    return true;
}

bool MatchTickAccess::replace_observer_previous(
    Match& match,
    const sim::simulation_state::Order& order,
    const sim::simulation_state::Order* previous
) {
    if (!match.enhanced_ || !match.enhanced_->order_ownership)
        return false;
    auto* indexed = match.enhanced_->order_ownership->find(&order);
    if (!indexed)
        return false;
    indexed->observer_previous = previous;
    return true;
}

bool MatchTickAccess::reset_order_index(Match& match, size_t unit_slots) {
    if (!match.orders_.empty() || !match.enhanced_ || !match.enhanced_->order_ownership)
        return false;
    match.enhanced_->order_ownership = std::make_unique<OrderOwnershipIndex>(unit_slots);
    return true;
}

size_t MatchTickAccess::shrink_order_capacity(Match& match) {
    match.orders_.shrink_to_fit();
    return match.orders_.capacity();
}

size_t MatchTickAccess::order_capacity(const Match& match) {
    return match.orders_.capacity();
}

} // namespace oa::sim::match_runtime

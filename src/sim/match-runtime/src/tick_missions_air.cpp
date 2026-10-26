// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "tick_internal.hpp"
#include "oa/base/game_math.hpp"

namespace oa::sim::match_runtime {
using base::game_math::truncate_low32;

namespace {

constexpr uint8_t air_evade_kind = 48;      // VTOL_Evade
constexpr uint8_t air_follow_kind = 49;     // VTOL_Follow
constexpr uint8_t air_seek_guard_kind = 63; // VTOL_SeekGuard

constexpr uint32_t event_cancel = 0x2; // raised when the order is torn down
constexpr uint32_t event_target = 0x8; /* ? target lost */
// Raised on the orders that target a unit when the damage reaction runs.
constexpr uint32_t event_target_damaged = 0x10;
// Raised when a weapon slot cannot reach or aim at its target.
constexpr uint32_t event_target_unreachable = 0x1000;
constexpr uint32_t event_weapon = 0x10000;
constexpr uint32_t event_goal = sim::ground_orders::arrived_event |
                                sim::ground_orders::path_failed_event |
                                sim::ground_orders::goal_replaced_event;

constexpr uint32_t wait_goal_or_cancel = event_goal | event_cancel;
constexpr uint32_t wait_attack = event_weapon | event_goal | event_target;
constexpr uint32_t wait_attack_reposition = wait_attack | event_target_unreachable;
constexpr uint32_t wait_attack_or_cancel = wait_attack | event_cancel;
constexpr uint32_t wait_guard = event_goal | event_target_damaged | event_target;
// End AirStrike and AirToGround.
constexpr uint32_t air_strike_abort_events = event_weapon | event_target | event_cancel;
// End AirToAir, AirToGroundHover and VTOL_Evade.
constexpr uint32_t air_attack_abort_events = event_weapon | event_target;

// Command flags bits, kept in OrderState.command_flags.
constexpr uint8_t order_has_target = 0x02;
constexpr uint8_t order_has_destination = 0x04;
constexpr uint8_t order_idle = 0x40; // inherited from the queue head on push-front

// Handler results (see simulation_state primary_orders).
constexpr uint32_t result_restart = 0;
constexpr uint32_t result_next = 1;
constexpr uint32_t result_stay = 2;
constexpr uint32_t result_retry = 3;
constexpr uint32_t result_done = 5;
constexpr uint32_t result_fail = 7;

constexpr int32_t fixed_one = 0x10000;
constexpr uint16_t quarter_turn = 0x4000;
constexpr uint16_t eighth_turn = 0x2000;

constexpr int16_t loiter_arrival = 0x80;
constexpr int32_t map_return_distance = 0x320 * fixed_one;
constexpr int32_t strike_run_in_distance = 0x1e0 * fixed_one;
constexpr int32_t strike_overshoot = 0x8c0 * fixed_one;
constexpr int16_t strike_pass_arrival = 0x3c0;
constexpr int16_t strike_approach_arrival = 0x1e0;
constexpr int32_t strike_breakaway = 0x5a0 * fixed_one;
constexpr int32_t strike_run_extension = 0x3c0;
constexpr float ticks_per_second = 30.0F;
constexpr int32_t dogfight_run_ticks = 0x1e;
constexpr int32_t dogfight_lead_ticks = 0x2d;
constexpr int32_t dogfight_misaligned_step = 0x2d;
constexpr int32_t dogfight_misaligned_limit = 0x5a;
constexpr int16_t dogfight_pursuit_distance = 0xa0;
constexpr uint32_t dogfight_wait_ticks = 0x2d;
// AirToGround: runs pull out three weapon ranges past the point and end
// within a random 128 to 255 world units of it; an aircraft off the map
// waits 30 ticks and runs at the point again.
constexpr int32_t ground_run_pull_out_ranges = 3;
constexpr uint32_t ground_run_pull_out_arrival = 0x80;
constexpr uint32_t ground_run_off_map_wait = 0x1e;
constexpr int16_t hover_arrival = 0x10;
constexpr int32_t hover_unreachable_limit = 2;
constexpr int32_t seek_orbit_margin = 0xa0;
constexpr uint16_t seek_attack_turn = 0x5555;
constexpr uint16_t seek_guard_turn = 0x4000;
constexpr uint16_t seek_turn_jitter = 0x2000;
constexpr uint32_t seek_wait_ticks = 0x1e;

int32_t signed_bits(uint32_t n) {
    return std::bit_cast<int32_t>(n);
}

AttackPoint point_of(const oa::FixedVec3& v) {
    return {static_cast<uint32_t>(v.x), static_cast<uint32_t>(v.y), static_cast<uint32_t>(v.z)};
}

/// Returns a point held as bit patterns as signed 16.16 coordinates.
///
/// @param point Signed 16.16 point as bit patterns.
/// @return The same point.
oa::FixedVec3 fixed_point(const AttackPoint& point) {
    return {signed_bits(point[0]), signed_bits(point[1]), signed_bits(point[2])};
}

/// Returns the bearing of `from` as seen from `to`.
///
/// @param from Signed 16.16 point as bit patterns.
/// @param to Signed 16.16 point as bit patterns.
/// @return Heading in 65536ths of a turn.
uint16_t bearing(const AttackPoint& from, const AttackPoint& to) {
    return base::game_math::direction(signed_bits(from[0] - to[0]), signed_bits(from[2] - to[2]));
}

int32_t horizontal_distance(uint32_t dx, uint32_t dz) {
    return signed_bits(base::game_math::distance(signed_bits(dx), signed_bits(dz)));
}

// `from` moved `magnitude` along heading `angle`, whose unit vector is
// -(sin, cos); y is kept.
AttackPoint forward(const AttackPoint& from, uint16_t angle, int32_t magnitude) {
    auto point = from;
    point[0] -= static_cast<uint32_t>(sim::unit_movement::sine_scaled(angle, magnitude));
    point[2] -= static_cast<uint32_t>(sim::unit_movement::cosine_scaled(angle, magnitude));
    return point;
}

// `from` moved `magnitude` against heading `angle`; y is kept.
AttackPoint backward(const AttackPoint& from, uint16_t angle, int32_t magnitude) {
    auto point = from;
    point[0] += static_cast<uint32_t>(sim::unit_movement::sine_scaled(angle, magnitude));
    point[2] += static_cast<uint32_t>(sim::unit_movement::cosine_scaled(angle, magnitude));
    return point;
}

} // namespace

// Mission handlers of the aircraft attack family, run for one order.
class TickHost::AirAttackMissions {
    TickHost& host;
    sim::unit_spawn::Slot& s;
    Match::RuntimeOrder& record;
    sim::simulation_state::Order& order;
    AttackAdapter adapter;
    oa::World& world;
    oa::Unit& unit;

  public:

    AirAttackMissions(TickHost& h, sim::unit_spawn::Slot& slot, Match::RuntimeOrder& r)
        : host(h), s(slot), record(r), order(r.order), adapter(h, slot, r), world(h.match.state()),
          unit(slot.record) {}

    /// Runs one step of AirStrike: bombers approach, line up a pass over the
    /// destination, release along it, break away, and repeat; a damaged bomber
    /// goes to a repair pad after a pass.
    ///
    /// @param events Events raised on the order since its last step.
    /// @return 1 (next phase), 2 (keep waiting), 0 (restart: a landing was
    ///     queued), 5 (done: an abort event, the target gone or past the leash;
    ///     VTOL_SeekAttack follows when allowed) or 7 (invalid: cannot fly, or
    ///     the OTA GlobalHeader sets no gravity).
    uint32_t air_strike(uint32_t events);
    /// Runs one step of AirToAir: fighters run in on an aligned target, turn
    /// after it while it is ahead, and break off into VTOL_Evade when the
    /// pursuit stalls.
    ///
    /// @param events Events raised on the order since its last step.
    /// @return 1 (next phase), 2 (keep waiting), 0 (restart: VTOL_Evade was
    ///     queued), 5 (done: target lost, an abort event or past the leash) or
    ///     7 (invalid).
    uint32_t air_to_air(uint32_t events);
    /// Runs one step of AirToGround: aircraft fly at a unit or a ground
    /// point, firing their first weapon at it on the way in, pull out past
    /// it, swing round to one side and run at it again until the order ends.
    ///
    /// @param events Events raised on the order since its last step.
    /// @return 1 (next phase), 2 (keep waiting), 0 (restart: a landing was
    ///     queued), 5 (done: an abort event, the unit target gone or past the
    ///     leash; VTOL_SeekAttack follows when allowed) or 7 (invalid).
    uint32_t air_to_ground(uint32_t events);
    /// Runs one step of AirToGround for a gunship under
    /// air.gunships-hover-to-strafe: it closes to half the distance, flies to
    /// weapon range of the point and hovers there facing it, strafing from
    /// side to side and firing at it until the order ends.
    ///
    /// @param events Events raised on the order since its last step.
    /// @return 1 (next phase), 2 (keep waiting), 0 (restart: a landing was
    ///     queued), 5 (done: an abort event, the unit target gone or past the
    ///     leash; VTOL_SeekAttack follows when allowed) or 7 (invalid).
    uint32_t hover_over_ground(uint32_t events);
    /// Runs one step of AirToGroundHover: gunships close to half the distance,
    /// hover at weapon range, then strafe from side to side.
    ///
    /// @param events Events raised on the order since its last step.
    /// @return 1 (next phase), 2 (keep waiting), 0 (restart: a landing was
    ///     queued), 5 (done: target lost, an abort event or past the leash) or
    ///     7 (invalid).
    /// @quirk After two strafes out of reach the order only waits: no
    ///     reposition goal is installed, as in 3.1c.
    uint32_t air_to_ground_hover(uint32_t events);
    /// Runs one step of VTOL_SeekAttack: attacks a given unit, or circles the
    /// destination looking for targets.
    ///
    /// @param events Events raised on the order since its last step.
    /// @return 1 (next phase), 2 (keep waiting), 0 (restart: an attack or a
    ///     landing was queued), 5 (done: no path, or an attack was queued while
    ///     circling) or 7 (invalid).
    uint32_t seek_attack(uint32_t events);
    /// Runs one step of VTOL_SeekGuard: circles the destination until an
    /// allied ground unit comes within sight distance, then guards it.
    ///
    /// @param events Events raised on the order since its last step.
    /// @return 1 (next phase), 2 (keep waiting), 3 (retry later: a guard was
    ///     queued), 0 (restart: a landing was queued), 5 (done: no path) or
    ///     7 (invalid).
    uint32_t seek_guard(uint32_t events);
    /// Runs one step of VTOL_Evade: two legs out to one side of the current
    /// heading, the first as far as weapon range and the second twice as far.
    ///
    /// @param events Events raised on the order since its last step.
    /// @return 1 (next phase), 5 (done: target gone, an abort event, or both
    ///     legs flown) or 7 (invalid).
    uint32_t evade(uint32_t events);

  private:

    const oa::UnitDef& def() const { return *oa::world_unit_def_of(&world, &unit); }

    const oa::UnitDef& def_of(const oa::Unit& other) const {
        return *oa::world_unit_def_of(&world, &other);
    }

    AttackPoint here() const { return point_of(unit.position); }

    oa::Unit& target_record() const { return host.slot(*record.attack.target).record; }

    AttackPoint target_position() const { return point_of(target_record().position); }

    bool can_fly() const {
        return unit.movement != 0 && (def().flags & OA_UNIT_DEF_FLAG_CAN_FLY) != 0;
    }

    // Weapon 1 range in world units; zero without a first weapon.
    int32_t primary_range() const {
        const auto* weapon = host.match.weapons_[s.unit_index].definitions[0];
        return weapon ? weapon->range_world_units : 0;
    }

    // Below three quarters of its health an aircraft breaks off for a repair
    // pad, unless air.no-repair-retreat-flag exempts its type.
    bool health_low() const {
        const auto health = static_cast<uint32_t>(static_cast<int32_t>(unit.health));
        return health < (def().max_damage >> 2) * 3 &&
               !never_retreats_to_repair(host.match.rules(), def().abilities);
    }

    // air.gunships-hover-to-strafe: a type that hovers to attack hovers over
    // the point of its AirToGround as well.
    bool hovers_over_ground() const {
        const auto& rule = host.match.rules().air.gunships_hover_to_strafe;
        return rule.enabled && rule.attack_ground &&
               (def().flags & OA_UNIT_DEF_FLAG_HOVER_ATTACK) != 0;
    }

    bool leash_exceeded() const {
        const auto leash = record.attack.leash;
        if (leash == 0)
            return false;
        const auto distance = base::game_math::distance(
            static_cast<int32_t>(high_word(static_cast<uint32_t>(unit.position.x))) -
                record.attack.origin[0],
            static_cast<int32_t>(high_word(static_cast<uint32_t>(unit.position.z))) -
                record.attack.origin[1]
        );
        return leash <= signed_bits(distance);
    }

    void store_destination(const AttackPoint& point) {
        record.attack.destination = point;
        for (size_t axis = 0; axis < 3; ++axis)
            record.extra.destination[axis] = signed_bits(point[axis]);
    }

    // Wake the order `ticks` from now.
    void wait(uint32_t ticks) {
        order.wait_events |= 1;
        order.wake_tick = host.match.simulation_.tick + ticks;
    }

    /// Removes the order's movement goal: the air driver flies on to no
    /// point, and holds where it is.
    void clear_goal() {
        if (record.extra.goal)
            adapter.clear_goal();
        host.install_air_goal(s, order, nullptr);
    }

    /// Hands the air driver a goal of this order in place of its last one.
    ///
    /// @param goal The goal; it is copied into the order.
    void install_goal(const sim::air::AirGoal& goal) {
        if (record.extra.goal)
            adapter.clear_goal();
        host.install_air_goal(s, order, &goal);
    }

    /// Flies the air driver to a fixed point at cruise height, arriving
    /// anywhere within a radius of it.
    ///
    /// @param point Signed 16.16 point as bit patterns.
    /// @param arrival Arrival radius in world units, kept to 16 bits.
    void point_goal(const AttackPoint& point, int32_t arrival) {
        auto goal = sim::air::air_goal_at_point(&order.raised_events, &unit, fixed_point(point));
        sim::air::air_goal_set_arrival_radius(&goal, static_cast<int16_t>(arrival));
        install_goal(goal);
    }

    /// Flies the air driver to a fixed point at the type's cruise altitude
    /// over the ground there, the unit facing the target, as gunships strafe.
    ///
    /// @param point Signed 16.16 point as bit patterns.
    /// @param arrival Arrival radius in world units.
    void facing_goal(const AttackPoint& point, int16_t arrival) {
        auto goal = sim::air::air_goal_facing_unit(
            &order.raised_events, &unit, &target_record(), fixed_point(point)
        );
        sim::air::air_goal_set_arrival_radius(&goal, arrival);
        sim::air::air_goal_set_altitude(&goal, host.air_host(), def().cruise_alt);
        install_goal(goal);
    }

    /// Flies the air driver after a unit, arriving within a radius of it.
    ///
    /// @param target The unit followed; a flying one is shadowed from the
    ///     first weapon's range along its heading.
    /// @param arrival Arrival radius in world units, kept to 16 bits.
    void follow_goal(oa::Unit& target, int32_t arrival) {
        auto goal =
            sim::air::air_goal_follow_unit(host.air_host(), &order.raised_events, &unit, &target);
        sim::air::air_goal_set_arrival_radius(&goal, static_cast<int16_t>(arrival));
        install_goal(goal);
    }

    /// Flies the air driver after a point that moves by a step every tick;
    /// the goal arrives within 48 world units of the point.
    ///
    /// @param from Signed 16.16 start point as bit patterns.
    /// @param step Signed 16.16 displacement per tick as bit patterns.
    void seek_goal(const AttackPoint& from, const AttackPoint& step) {
        install_goal(
            sim::air::air_goal_seek(
                &order.raised_events, &unit, fixed_point(from), fixed_point(step)
            )
        );
    }

    uint16_t speed_high() const {
        const auto* ground = host.match.ground_runtime(s.unit_index);
        const auto speed = ground ? static_cast<uint32_t>(ground->movement.speed) : 0U;
        return static_cast<uint16_t>(speed >> 16);
    }

    std::array<int32_t, 3> velocity_of(const oa::Unit& other) const {
        const auto* ground =
            host.match.ground_runtime(static_cast<uint16_t>(oa::world_unit_slot(&world, &other)));
        return ground ? ground->movement.velocity : std::array<int32_t, 3>{};
    }

    bool allied(const oa::Unit& from, const oa::Unit& to) const {
        const auto a = from.owner_index;
        const auto b = to.owner_index;
        if (a >= 10 || b >= 10 || !host.match.player_alliances_[a])
            return false;
        return (*host.match.player_alliances_[a])[b] != 0;
    }

    /// Builds an order record for a mission kind from its descriptor, not yet
    /// queued.
    ///
    /// @param kind Mission kind.
    /// @param target Target unit, linked as an observer when the descriptor
    ///     takes one; null for none.
    /// @param destination Signed 16.16 destination as bit patterns, or null.
    /// @return The new order, owned by the match.
    Match::RuntimeOrder&
    new_order(uint8_t kind, sim::simulation_state::Unit* target, const AttackPoint* destination);
    /// Queues an order at the head of its queue, inheriting the idle mark
    /// (order_idle) of the old head.
    ///
    /// @param[in,out] created Order to queue.
    void push_front(Match::RuntimeOrder& created);
    /// Queues an order at the tail of its queue.
    ///
    /// @param[in,out] created Order to queue.
    void append(Match::RuntimeOrder& created);
    bool seek_after_target_lost(uint32_t events, uint32_t abort_events);
    /// Tells whether the unit stands in the off-map bucket.
    ///
    /// @return True when the unit is off the map.
    bool off_map() const;
    /// Steers a unit in the off-map bucket back toward the map centre.
    ///
    /// @return True when the unit was off the map and a goal toward the centre
    ///     was installed.
    bool return_to_map();
    /// Sends a damaged aircraft to a random pad of those its owner's pad
    /// list offers within 0xf00 world units (Match::repair_pads_in_reach)
    /// by queueing VTOL_Landing on it.
    ///
    /// @return True when a pad was found and the landing queued.
    bool seek_repair_pad();
    /// Tells whether the unit faces within a right angle of the bearing from
    /// the target.
    ///
    /// @param target Unit under attack.
    /// @return True when aligned.
    bool aligned_with(const oa::Unit& target) const;
    /// Resolves the guard command for an aircraft: follow an allied live unit.
    ///
    /// @param target Unit to guard.
    /// @return VTOL_Follow for a flying type, Follow_Ground otherwise, or 0 when
    ///     the target is not live, not allied, or the type cannot guard.
    uint8_t guard_kind(const oa::Unit& target) const;
    /// Finds the first ground unit within a radius whose owner is allied with
    /// this unit's owner, in spatial bucket order.
    ///
    /// @param radius Search radius, signed 16.16.
    /// @return The unit, or null when none.
    oa::Unit* first_guard_candidate(int32_t radius);
};

Match::RuntimeOrder& TickHost::AirAttackMissions::new_order(
    uint8_t kind, sim::simulation_state::Unit* target, const AttackPoint* destination
) {
    auto entry = std::make_unique<Match::RuntimeOrder>();
    entry->unit = s.unit;
    entry->order.kind = kind;
    entry->order.wake_tick = 0xffffffffu;
    entry->order.issue_tick = host.match.state().game.tick;
    const auto flags = mission_descriptor_table.at(kind);
    entry->order.preserve_flags = static_cast<uint8_t>(flags);
    entry->extra.command_flags = static_cast<uint8_t>(flags >> 8);
    entry->order.flags = static_cast<uint8_t>(flags >> 16);
    if (destination) {
        entry->attack.destination = *destination;
        for (size_t axis = 0; axis < 3; ++axis)
            entry->extra.destination[axis] = signed_bits((*destination)[axis]);
    } else
        entry->extra.command_flags &= static_cast<uint8_t>(~order_has_destination);
    if (!target)
        entry->extra.command_flags &= static_cast<uint8_t>(~order_has_target);
    auto& created = host.match.adopt_order(std::move(entry));
    if (target && (created.extra.command_flags & order_has_target))
        host.match.link_target_observer(created, target);
    return created;
}

void TickHost::AirAttackMissions::push_front(Match::RuntimeOrder& created) {
    auto& head = (created.order.flags & 4) ? s.unit->secondary : s.unit->primary;
    created.order.next = head;
    if (head)
        created.extra.command_flags |= host.owned(*head).extra.command_flags & order_idle;
    head = &created.order;
}

void TickHost::AirAttackMissions::append(Match::RuntimeOrder& created) {
    created.order.next = nullptr;
    auto* link = (created.order.flags & 4) ? &s.unit->secondary : &s.unit->primary;
    for (size_t steps = 0; *link && steps < overlay_order_budget; ++steps)
        link = &(*link)->next;
    *link = &created.order;
}

// Shared entry test of the attack handlers: on an abort event, or when the
// unit target is gone, units allowed to fire hand over to VTOL_SeekAttack.
// True when the handler must finish.
bool TickHost::AirAttackMissions::seek_after_target_lost(uint32_t events, uint32_t abort_events) {
    const bool may_fire = (unit.flags & OA_UNIT_FLAG_FIRE_ORDER_MASK) != 0;
    if (events & abort_events) {
        if (!order.next && may_fire)
            append(
                new_order(vtol_seek_attack_kind, record.attack.target, &record.attack.destination)
            );
        return true;
    }
    if (record.attack.target || !(record.extra.command_flags & order_has_target))
        return false;
    if (!order.next) {
        const auto position = here();
        append(new_order(vtol_seek_attack_kind, nullptr, &position));
    }
    return true;
}

bool TickHost::AirAttackMissions::off_map() const {
    const auto& projected = host.match.spatial_units_.at(s.unit_index);
    return projected.bucket_linked && !projected.bucket;
}

bool TickHost::AirAttackMissions::return_to_map() {
    if (!off_map())
        return false;
    const auto& game = world.game;
    AttackPoint centre{};
    centre[0] = static_cast<uint32_t>(game.map_pixel_width / 2) << 16;
    centre[2] = static_cast<uint32_t>(game.map_pixel_height / 2) << 16;
    const auto from = here();
    const auto point = forward(from, bearing(from, centre), map_return_distance);
    order.wait_events |= event_goal;
    point_goal(point, loiter_arrival);
    return true;
}

bool TickHost::AirAttackMissions::seek_repair_pad() {
    const auto pads = host.match.repair_pads_in_reach(unit);
    if (pads == 0)
        return false;
    clear_goal();
    auto* pad = host.match.repair_pad_in_reach(unit, host.random(pads));
    push_front(new_order(sim::ground_orders::vtol_landing_kind, &host.unit_view(*pad), nullptr));
    order.wait_events = 0;
    return true;
}

bool TickHost::AirAttackMissions::aligned_with(const oa::Unit& target) const {
    return sim::unit_movement::facing_toward(
        target.position.x, target.position.z, unit.position.x, unit.position.z, unit.heading
    );
}

uint8_t TickHost::AirAttackMissions::guard_kind(const oa::Unit& target) const {
    if (!(target.flags & OA_UNIT_FLAG_LIVE))
        return 0;
    if (!(def().abilities & OA_UNIT_DEF_ABILITY_CAN_GUARD) || !allied(unit, target))
        return 0;
    return (def().flags & OA_UNIT_DEF_FLAG_CAN_FLY) ? air_follow_kind : follow_ground_kind;
}

oa::Unit* TickHost::AirAttackMissions::first_guard_candidate(int32_t radius) {
    struct Search {
        AirAttackMissions* self;
        oa::Unit* found;
    } search{this, nullptr};

    host.match.for_each_unit_in_radius(
        {unit.position.x, unit.position.y, unit.position.z},
        radius,
        [](void* context, sim::unit_spawn::Slot& slot) {
            auto& search = *static_cast<Search*>(context);
            auto& other = slot.record;
            const auto& self = *search.self;
            if (search.found == nullptr && self.allied(other, self.unit) &&
                !(self.def_of(other).flags & OA_UNIT_DEF_FLAG_CAN_FLY) && &other != &self.unit)
                search.found = &other;
        },
        &search
    );
    return search.found;
}

uint32_t TickHost::AirAttackMissions::air_strike(uint32_t events) {
    auto& attack = record.attack;
    if (events & air_strike_abort_events) {
        if (!order.next && (unit.flags & OA_UNIT_FLAG_FIRE_ORDER_MASK))
            append(new_order(vtol_seek_attack_kind, attack.target, &attack.destination));
        return result_done;
    }
    if (attack.target)
        store_destination(target_position());
    else if (record.extra.command_flags & order_has_target) {
        if (!order.next) {
            const auto position = here();
            append(new_order(vtol_seek_attack_kind, nullptr, &position));
        }
        return result_done;
    }
    if (leash_exceeded())
        return result_done;
    const auto destination = attack.destination;
    switch (order.phase) {
    case 0:
        if (!can_fly())
            return result_fail;
        adapter.announce("Attacking");
        host.take_off(s, order);
        return result_next;
    case 1: {
        adapter.reset_weapons();
        adapter.release_weapon_targets(0);
        const auto from = here();
        if (horizontal_distance(destination[0] - from[0], destination[2] - from[2]) <
            strike_run_in_distance) {
            point_goal(
                forward(from, bearing(from, destination), strike_overshoot), strike_pass_arrival
            );
            order.wait_events |= wait_goal_or_cancel;
        }
        return result_next;
    }
    case 2: {
        const auto from = here();
        const auto distance =
            horizontal_distance(destination[0] - from[0], destination[2] - from[2]);
        const auto toward = bearing(from, destination);
        const auto half = distance / 2;
        const auto angle =
            static_cast<uint16_t>(host.random(2 * eighth_turn) + toward - eighth_turn);
        point_goal(forward(from, angle, half), strike_approach_arrival);
        order.wait_events = wait_attack;
        return result_next;
    }
    case 3:
        return result_next;
    case 4: {
        if (events & event_goal)
            return result_next;
        const auto gravity = host.match.scenario_gravity_;
        if (gravity == 0)
            return result_fail;
        const auto fall_ticks =
            base::game_math::square_root(
                static_cast<double>(def().cruise_alt) * 2.0 / static_cast<double>(gravity)
            ) *
            static_cast<double>(ticks_per_second);
        const auto speed = static_cast<int16_t>(speed_high());
        const auto lead = truncate_low32(static_cast<double>(speed) * fall_ticks);
        const auto arrival = static_cast<int16_t>(
            lead + static_cast<int32_t>(static_cast<uint16_t>(def().attack_run_length)) + 1
        );
        if (attack.target)
            follow_goal(target_record(), arrival);
        else
            point_goal(destination, arrival);
        wait(1);
        order.wait_events |= wait_attack;
        return result_stay;
    }
    case 5: {
        adapter.release_weapon_targets(0);
        adapter.assign_ground(destination, 0);
        const auto from = here();
        const auto run = static_cast<int32_t>(
            (static_cast<uint32_t>(static_cast<uint16_t>(def().attack_run_length)) +
             strike_run_extension)
            << 16
        );
        point_goal(forward(from, bearing(from, destination), run), strike_pass_arrival);
        order.wait_events = wait_goal_or_cancel;
        return result_next;
    }
    case 6:
        host.match.stop_weapon(s, 0);
        point_goal(forward(here(), unit.heading, strike_breakaway), loiter_arrival);
        order.wait_events = wait_goal_or_cancel;
        if (health_low()) {
            (void)seek_repair_pad();
            return result_restart;
        }
        order.phase = 3;
        return result_stay;
    default:
        return result_fail;
    }
}

uint32_t TickHost::AirAttackMissions::air_to_air(uint32_t events) {
    auto& attack = record.attack;
    if (seek_after_target_lost(events, air_attack_abort_events))
        return result_done;
    if (return_to_map())
        return result_stay;
    if (leash_exceeded())
        return result_done;
    auto& misaligned = attack.weapon_slot; // counts ticks spent off the target's tail
    if (order.phase == 0) {
        if (!can_fly())
            return result_fail;
        adapter.announce("Attacking");
        host.take_off(s, order);
        wait(1);
        misaligned = 0;
        return result_next;
    }
    if (order.phase != 1)
        return result_fail;
    if (!attack.target)
        return result_done; // a target lost between steps ends the order
    auto& target = target_record();
    adapter.reset_weapons();
    adapter.release_weapon_targets(0);
    adapter.assign_target(*attack.target, 0);
    const auto goal_events = events & event_goal;
    if (goal_events && aligned_with(target)) {
        const auto speed = def().max_velocity;
        const auto run = signed_bits(static_cast<uint32_t>(speed) * dogfight_run_ticks);
        const auto from = forward(here(), unit.heading, run);
        seek_goal(from, forward({}, unit.heading, speed));
        wait(host.random(dogfight_run_ticks) + 0x3c);
        misaligned = 0;
        return result_stay;
    }
    if (goal_events || misaligned >= dogfight_misaligned_limit) {
        clear_goal();
        push_front(new_order(air_evade_kind, attack.target, nullptr));
        misaligned = 0;
        order.wait_events = 0;
        return result_restart;
    }
    if (aligned_with(target))
        misaligned = 0;
    else
        misaligned += dogfight_misaligned_step;
    const auto separation = horizontal_distance(
        static_cast<uint32_t>(unit.position.x) - static_cast<uint32_t>(target.position.x),
        static_cast<uint32_t>(unit.position.z) - static_cast<uint32_t>(target.position.z)
    );
    if (high_word(static_cast<uint32_t>(separation)) > dogfight_pursuit_distance) {
        const auto velocity = velocity_of(target);
        auto from = point_of(target.position);
        from[0] += static_cast<uint32_t>(velocity[0]) * dogfight_lead_ticks;
        from[2] += static_cast<uint32_t>(velocity[2]) * dogfight_lead_ticks;
        // The point starts where the target's velocity takes it in 45 ticks
        // and moves each tick by that velocity plus half the target's top
        // speed along its heading.
        const AttackPoint drift{
            static_cast<uint32_t>(velocity[0]),
            static_cast<uint32_t>(velocity[1]),
            static_cast<uint32_t>(velocity[2])
        };
        seek_goal(from, forward(drift, target.heading, def_of(target).max_velocity / 2));
    }
    wait(dogfight_wait_ticks);
    order.wait_events |= wait_attack;
    return result_stay;
}

uint32_t TickHost::AirAttackMissions::air_to_ground(uint32_t events) {
    if (hovers_over_ground())
        return hover_over_ground(events);
    auto& attack = record.attack;
    const auto range = primary_range();
    if (seek_after_target_lost(events, air_strike_abort_events))
        return result_done;
    // The point follows a target unit; a ground attack keeps the point given.
    if (attack.target)
        store_destination(target_position());
    if (off_map()) {
        wait(ground_run_off_map_wait);
        order.phase = 2;
    }
    if (leash_exceeded())
        return result_done;
    const auto destination = attack.destination;
    switch (order.phase) {
    case 0:
        if (!can_fly())
            return result_fail;
        adapter.announce("Attacking");
        host.take_off(s, order);
        return result_next;
    case 1: {
        adapter.reset_weapons();
        const auto from = here();
        const auto distance =
            horizontal_distance(destination[0] - from[0], destination[2] - from[2]);
        const auto toward = bearing(from, destination);
        const auto half = distance / 2;
        const auto angle =
            static_cast<uint16_t>(host.random(2 * eighth_turn) + toward - eighth_turn);
        point_goal(forward(from, angle, half), loiter_arrival);
        order.wait_events = wait_attack;
        return result_next;
    }
    case 2:
        adapter.release_weapon_targets(0);
        if (attack.target)
            adapter.assign_target(*attack.target, 0);
        else
            adapter.assign_ground(destination, 0);
        point_goal(destination, range);
        order.wait_events = wait_attack;
        return result_next;
    case 3: {
        const auto pull_out =
            signed_bits(static_cast<uint32_t>(range * ground_run_pull_out_ranges) << 16);
        const auto arrival = static_cast<int32_t>(
            host.random(ground_run_pull_out_arrival) + ground_run_pull_out_arrival
        );
        point_goal(forward(destination, bearing(here(), destination), pull_out), arrival);
        order.wait_events = wait_attack_or_cancel;
        return result_next;
    }
    case 4: {
        if (health_low() && seek_repair_pad())
            return result_restart;
        const auto turn = host.random(2) != 0 ? static_cast<uint16_t>(unit.heading + quarter_turn)
                                              : static_cast<uint16_t>(unit.heading - quarter_turn);
        const auto swing = signed_bits(static_cast<uint32_t>(range) << 16);
        point_goal(forward(here(), turn, swing), loiter_arrival);
        order.wait_events = wait_attack_or_cancel;
        return result_next;
    }
    case 5:
        order.phase = 2;
        return result_stay;
    default:
        return result_fail;
    }
}

uint32_t TickHost::AirAttackMissions::hover_over_ground(uint32_t events) {
    auto& attack = record.attack;
    if (seek_after_target_lost(events, air_strike_abort_events))
        return result_done;
    // The point follows a target unit; a ground attack keeps the point given.
    if (attack.target)
        store_destination(target_position());
    if (return_to_map())
        return result_stay;
    if (leash_exceeded())
        return result_done;
    const auto range = primary_range();
    const auto destination = attack.destination;
    auto& side = attack.weapon_slot; // alternates the strafe side
    switch (order.phase) {
    case 0:
        if (!can_fly())
            return result_fail;
        adapter.announce("Attacking");
        host.take_off(s, order);
        return result_next;
    case 1: {
        adapter.reset_weapons();
        const auto from = here();
        const auto distance =
            horizontal_distance(destination[0] - from[0], destination[2] - from[2]);
        const auto toward = bearing(from, destination);
        const auto half = distance / 2;
        const auto angle =
            static_cast<uint16_t>(host.random(2 * eighth_turn) + toward - eighth_turn);
        point_goal(forward(from, angle, half), loiter_arrival);
        order.wait_events = wait_attack;
        return result_next;
    }
    case 2:
        adapter.release_weapon_targets(0);
        if (attack.target)
            adapter.assign_target(*attack.target, 0);
        else
            adapter.assign_ground(destination, 0);
        point_goal(destination, range);
        side = 0;
        order.wait_events = wait_attack;
        return result_next;
    default:
        break;
    }
    // From weapon range on, each step strafes to the other side of the
    // point, two thirds of the range from it, facing it.
    const auto toward = bearing(here(), destination);
    uint16_t angle;
    if (side != 0) {
        angle = static_cast<uint16_t>(toward + eighth_turn);
        side = 0;
    } else {
        angle = static_cast<uint16_t>(toward - eighth_turn);
        side = 1;
    }
    const auto strafe = signed_bits(static_cast<uint32_t>(range * 2 / 3) << 16);
    const auto spot = backward(destination, angle, strafe);
    if (attack.target)
        facing_goal(spot, hover_arrival);
    else {
        auto goal = sim::air::air_goal_at_point(&order.raised_events, &unit, fixed_point(spot));
        sim::air::air_goal_set_arrival_radius(&goal, hover_arrival);
        sim::air::air_goal_set_altitude(&goal, host.air_host(), def().cruise_alt);
        sim::air::air_goal_set_bearing(&goal, bearing(spot, destination));
        install_goal(goal);
    }
    order.wait_events = wait_attack;
    if (health_low() && seek_repair_pad())
        return result_restart;
    return result_stay;
}

uint32_t TickHost::AirAttackMissions::air_to_ground_hover(uint32_t events) {
    auto& attack = record.attack;
    if (seek_after_target_lost(events, air_attack_abort_events))
        return result_done;
    if (return_to_map())
        return result_stay;
    if (leash_exceeded())
        return result_done;
    const auto range = primary_range();
    auto& side = attack.weapon_slot;  // alternates the strafe side
    auto& unreachable = attack.retry; // strafes out of weapon reach
    if (order.phase == 0) {
        if (!can_fly())
            return result_fail;
        adapter.announce("Attacking");
        host.take_off(s, order);
        return result_next;
    }
    if (order.phase > 3)
        return result_fail;
    if (!attack.target)
        return result_done; // a target lost between steps ends the order
    const auto target = target_position();
    switch (order.phase) {
    case 1: {
        adapter.reset_weapons();
        const auto from = here();
        const auto distance = horizontal_distance(target[0] - from[0], target[2] - from[2]);
        const auto toward = bearing(from, target);
        const auto half = distance / 2;
        const auto angle =
            static_cast<uint16_t>(host.random(2 * eighth_turn) + toward - eighth_turn);
        point_goal(forward(from, angle, half), loiter_arrival);
        order.wait_events = wait_attack;
        return result_next;
    }
    case 2:
        adapter.release_weapon_targets(0);
        adapter.assign_target(*attack.target, 0);
        point_goal(target, range);
        side = 0;
        unreachable = 0;
        order.wait_events = wait_attack;
        return result_next;
    default:
        break;
    }
    if (!adapter.can_reach(*attack.target, 0))
        ++unreachable;
    if (unreachable >= hover_unreachable_limit) {
        unreachable = 0;
        const auto angle = static_cast<uint16_t>(host.random(0x10000));
        // The angle comes from the shared stream, but no reposition goal is
        // installed, as in 3.1c.
        (void)forward(target, angle, signed_bits(static_cast<uint32_t>(range) << 16));
        order.wait_events |= wait_attack_reposition;
        return result_stay;
    }
    const auto toward = bearing(here(), target);
    uint16_t angle;
    if (side != 0) {
        angle = static_cast<uint16_t>(toward + eighth_turn);
        side = 0;
    } else {
        angle = static_cast<uint16_t>(toward - eighth_turn);
        side = 1;
    }
    const auto strafe = signed_bits(static_cast<uint32_t>(range * 2 / 3) << 16);
    facing_goal(backward(target, angle, strafe), hover_arrival);
    order.wait_events = wait_attack;
    if (health_low() && seek_repair_pad())
        return result_restart;
    return result_stay;
}

uint32_t TickHost::AirAttackMissions::seek_attack(uint32_t events) {
    if (events & sim::ground_orders::path_failed_event)
        return result_done;
    if (return_to_map())
        return result_stay;
    auto& attack = record.attack;
    auto& circle = attack.weapon_slot; // heading around the destination
    if (order.phase == 0) {
        if (!can_fly())
            return result_fail;
        if (attack.target) {
            if (host.match.issue_automatic_attack(*s.unit, *attack.target)) {
                order.wait_events = 0;
                return result_restart;
            }
            return result_next;
        }
        const auto& destination = attack.destination;
        if (destination[0] == 0 && destination[2] == 0 && destination[1] == 0)
            store_destination(here());
        circle = static_cast<int32_t>(host.random(0x10000));
        attack.retry = circle & 1;
        host.take_off(s, order);
        return result_next;
    }
    if (order.phase != 1)
        return result_fail;
    adapter.reset_weapons();
    if (health_low() && seek_repair_pad())
        return result_restart;
    if (auto* found = host.match.find_automatic_target(*s.unit);
        found && host.match.issue_automatic_attack(*s.unit, *found))
        return result_done;
    if (events & event_goal)
        circle += signed_bits(0u - seek_attack_turn - host.random(seek_turn_jitter));
    const auto orbit =
        signed_bits(static_cast<uint32_t>(primary_range() + seek_orbit_margin) << 16);
    point_goal(forward(attack.destination, static_cast<uint16_t>(circle), orbit), loiter_arrival);
    wait(host.random(seek_wait_ticks) + seek_wait_ticks);
    order.wait_events |= event_goal;
    return result_stay;
}

uint32_t TickHost::AirAttackMissions::seek_guard(uint32_t events) {
    if (events & sim::ground_orders::path_failed_event)
        return result_done;
    if (return_to_map())
        return result_stay;
    auto& attack = record.attack;
    auto& circle = attack.weapon_slot; // heading around the destination
    if (order.phase == 0) {
        if (!can_fly())
            return result_fail;
        const auto& destination = attack.destination;
        if (destination[0] == 0 && destination[2] == 0 && destination[1] == 0)
            store_destination(here());
        circle = static_cast<int32_t>(host.random(0x10000));
        attack.retry = circle & 1;
        return result_next;
    }
    if (order.phase != 1)
        return result_fail;
    if (health_low() && seek_repair_pad())
        return result_restart;
    const auto sight = static_cast<int32_t>(static_cast<uint32_t>(def().sight_distance) << 16);
    if (auto* found = first_guard_candidate(sight)) {
        clear_goal();
        const auto kind = guard_kind(*found);
        push_front(new_order(kind, &host.unit_view(*found), nullptr));
        order.wait_events = 0;
        return result_retry;
    }
    if (events & event_goal)
        circle += signed_bits(0u - seek_guard_turn - host.random(seek_turn_jitter));
    const auto orbit =
        signed_bits(static_cast<uint32_t>(primary_range() + seek_orbit_margin) << 16);
    point_goal(forward(attack.destination, static_cast<uint16_t>(circle), orbit), loiter_arrival);
    wait(seek_wait_ticks);
    order.wait_events |= wait_guard;
    return result_stay;
}

uint32_t TickHost::AirAttackMissions::evade(uint32_t events) {
    const auto range = primary_range();
    if (!record.attack.target || (events & air_attack_abort_events))
        return result_done;
    auto& side = record.attack.weapon_slot; // nonzero turns left
    int32_t reach;
    switch (order.phase) {
    case 0:
        if (!can_fly())
            return result_fail;
        side = static_cast<int32_t>(host.random(2));
        reach = signed_bits(static_cast<uint32_t>(range) << 16);
        break;
    case 1:
        reach = signed_bits(static_cast<uint32_t>(range) << 17);
        break;
    case 2:
        return result_done;
    default:
        return result_fail;
    }
    const auto turn = side != 0 ? static_cast<uint16_t>(unit.heading - quarter_turn)
                                : static_cast<uint16_t>(unit.heading + quarter_turn);
    point_goal(forward(here(), turn, reach), loiter_arrival);
    order.wait_events = wait_attack;
    return result_next;
}

bool TickHost::dispatch_air_attack_mission(
    sim::unit_spawn::Slot& s, sim::simulation_state::Order& order, uint32_t events, uint32_t& result
) {
    const auto kind = order.kind;
    if (kind != air_strike_kind && kind != air_to_air_kind && kind != air_to_ground_kind &&
        kind != air_to_ground_hover_kind && kind != vtol_seek_attack_kind &&
        kind != air_seek_guard_kind && kind != air_evade_kind)
        return false;
    AirAttackMissions missions(*this, s, owned(order));
    if (kind == air_strike_kind)
        result = missions.air_strike(events);
    else if (kind == air_to_air_kind)
        result = missions.air_to_air(events);
    else if (kind == air_to_ground_kind)
        result = missions.air_to_ground(events);
    else if (kind == air_to_ground_hover_kind)
        result = missions.air_to_ground_hover(events);
    else if (kind == vtol_seek_attack_kind)
        result = missions.seek_attack(events);
    else if (kind == air_seek_guard_kind)
        result = missions.seek_guard(events);
    else
        result = missions.evade(events);
    return true;
}

} // namespace oa::sim::match_runtime

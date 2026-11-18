// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Construction, repair, reclaim and capture orders of the ground block of the
// mission table.
#include "ground_missions.hpp"
#include "oa/sim/unit_health/veterancy.hpp"

namespace oa::sim::match_runtime {

namespace {
namespace build {

constexpr uint32_t site_blocked_wait = 0x1e;
constexpr uint32_t no_slot_wait = 300;
constexpr uint32_t build_decloak_hold = 300;
constexpr uint32_t repair_decloak_hold = 0x96;
constexpr uint32_t capture_decloak_hold = 900;
constexpr uint32_t reclaim_decloak_hold = 900;
constexpr uint32_t approach_wait = 0x1e;
constexpr uint32_t approach_jitter = 0x1e;
constexpr uint32_t target_moving_wait = 0xf;
constexpr uint32_t capture_moving_wait = 0x1e;
constexpr uint32_t reclaim_approach_wait = 0xf;
constexpr uint32_t reclaim_retry_wait = 0xf;
constexpr uint32_t nano_step = 2;
constexpr uint8_t all_weapons = 3;
using BusyState = data::match_rules::OrdersWeaponsFreeWhileBusyStates;
constexpr uint8_t reclaim_damage_kind = static_cast<uint8_t>(DeathKind::reclaim);
// A reclaim bite lands once this many ticks of spraying have built up.
constexpr int32_t reclaim_bite_ticks = 15;
constexpr uint32_t reclaim_bite_duration = 0xf;
// The ring a helper keeps around a frame: trunc(sqrt(x * x + 2 * z) * 16).
constexpr double help_ring_scale = 16.0;
// Capture time in ticks: the target's costs weighted and capped, then scaled
// by its health and veterancy.
constexpr float capture_cost_scale = 30.0F;
constexpr float capture_energy_weight = 0.0005F;
constexpr float capture_metal_weight = 1.0F / 140.0F;
constexpr float capture_base_ticks = 150.0F;
constexpr int32_t capture_tick_cap = 0x708;

bool finished(const oa::Unit& unit) {
    return unit.build_remaining == 0.0F;
}

} // namespace build
} // namespace

uint32_t TickHost::GroundMissions::mobile_build() {
    if (events & ground::target_lost_event) {
        speak(ground::speech_failed, "Construction terminated");
        refresh_selection();
        return ground::mission_failed;
    }
    if (events & ground::cancel_event) {
        refresh_selection();
        return ground::mission_done;
    }
    const auto& built = world().unit_defs[static_cast<size_t>(record.construction.type_index)];
    auto& blocked = record.construction.blocked_retries;
    auto& site = record.extra.destination;
    ConstructionAdapter builder(host, s, record);
    // A building placed facing east or west has its footprint's width and
    // depth swapped (units.build-rotation).
    const bool turned = (builder.facing() & 1U) != 0;
    const auto built_x = turned ? built.footprint_z : built.footprint_x;
    const auto built_z = turned ? built.footprint_x : built.footprint_z;
    switch (order.phase) {
    case 0: {
        const auto cell_of = [](int32_t world, int16_t footprint) {
            return static_cast<int16_t>(
                wrapping_add(wrapping_sub(world, footprint * 0x80000), 0x80000) >>
                ground::cell_shift
            );
        };
        const auto cell_x = cell_of(site[0], built_x);
        const auto cell_z = cell_of(site[2], built_z);
        site[0] = (built_x + cell_x * 2) << ground::half_cell_shift;
        site[2] = (built_z + cell_z * 2) << ground::half_cell_shift;
        blocked = 0;
        outline_goal({cell_x, cell_z}, {built_x, built_z});
        order.wait_events = ground::goal_events;
        return ground::next_phase;
    }
    case 1: {
        if ((events & ground::path_failed_event) &&
            build_gap(site, built_x, built_z) >
                static_cast<int32_t>(static_cast<uint16_t>(def().build_distance))) {
            speak(ground::speech_failed, "I can't reach the construction site");
            return ground::mission_failed;
        }
        if (!builder.site_clear()) {
            const auto& kickout = rules().orders.build_site_kickout;
            if (kickout.kickout)
                clear_build_site(host, built_x, built_z, site);
            if (blocked == 0)
                speak(ground::speech_failed, "Waiting for target area to clear");
            else if (blocked > kickout.retry_limit) {
                speak(ground::speech_failed, "Target area was blocked");
                return ground::mission_failed;
            }
            ++blocked;
            wait_ticks(build::site_blocked_wait);
            return ground::keep_waiting;
        }
        occupy_weapons_while_busy(build::BusyState::nanolathe);
        builder.snap_build_height();
        auto* frame = builder.spawn_nanoframe();
        set_target(frame);
        if (!frame) {
            speak(ground::speech_failed, "Unable to create any more units");
            wait_ticks(build::no_slot_wait);
            return ground::keep_waiting;
        }
        speak(ground::speech_build, "Starting construction");
        refresh_selection();
        builder.issue_get_built(*frame);
        start_building_toward(ground::position_of(frame->record));
        return ground::next_phase;
    }
    case 2:
        // A look without the stance ends the order once its frame is gone or
        // finished (by other builders), or runs StartBuilding again.
        return wait_for_build_stance(ground::build_abort_events, [&] {
            auto* frame = target();
            if (!frame)
                return ground::mission_invalid;
            if (build::finished(frame->record))
                return ground::mission_done;
            start_building_toward(ground::position_of(frame->record));
            return ground::keep_waiting;
        });
    case 3: {
        // A lost frame makes the step invalid; the loss event normally ends
        // the order first.
        auto* frame = target();
        if (!frame)
            return ground::mission_invalid;
        (void)builder.construct(*frame, work_rate(def()));
        s.record.decloak_until_tick = tick() + build::build_decloak_hold;
        if (build::finished(frame->record))
            return ground::next_phase;
        wait_ticks(1);
        order.wait_events |= ground::build_abort_events;
        return ground::keep_waiting;
    }
    case 4:
        speak(ground::speech_complete, "Building complete");
        return ground::mission_done;
    default:
        return ground::mission_invalid;
    }
}

uint32_t TickHost::GroundMissions::help_build() {
    if (events & ground::cancel_event) {
        refresh_selection();
        return ground::mission_done;
    }
    auto* frame = target();
    if (!frame) {
        speak(ground::speech_failed, "Construction terminated");
        return ground::mission_failed;
    }
    switch (order.phase) {
    case 0: {
        if (!s.record.movement || !(def().flags & OA_UNIT_DEF_FLAG_BUILDER))
            return ground::mission_invalid;
        const auto& frame_def = def_of(frame->record);
        const double x = frame_def.footprint_x, z = frame_def.footprint_z;
        const auto spread =
            truncate_low32(base::game_math::square_root(x * x + z + z) * build::help_ring_scale);
        ring_goal(
            ground::position_of(frame->record),
            static_cast<int32_t>(static_cast<uint16_t>(def().build_distance)) + spread / 2,
            spread / 2
        );
        order.wait_events = ground::build_goal_events;
        return ground::next_phase;
    }
    case 1:
        if (events & ground::path_failed_event) {
            speak(ground::speech_failed, "I can't get there");
            return ground::mission_failed;
        }
        if (build::finished(frame->record))
            return ground::mission_done;
        occupy_weapons_while_busy(build::BusyState::help_build);
        start_building_toward(ground::position_of(frame->record));
        refresh_selection();
        return ground::next_phase;
    case 2:
        // A look without the stance ends the order once the frame is
        // finished, so that a guard goes on to the factory's next build, or
        // runs StartBuilding again.
        return wait_for_build_stance(ground::build_abort_events, [&] {
            if (build::finished(frame->record))
                return ground::mission_done;
            start_building_toward(ground::position_of(frame->record));
            return ground::keep_waiting;
        });
    case 3:
        (void)ConstructionAdapter(host, s, record).construct(*frame, work_rate(def()));
        s.record.decloak_until_tick = tick() + build::build_decloak_hold;
        if (!build::finished(frame->record)) {
            wait_ticks(1);
            order.wait_events |= ground::build_abort_events;
            return ground::keep_waiting;
        }
        return ground::next_phase;
    case 4:
        speak(ground::speech_complete, "Building complete");
        order.wait_events |= ground::cancel_event;
        return ground::mission_done;
    default:
        return ground::mission_invalid;
    }
}

uint32_t TickHost::GroundMissions::capture() {
    auto* prize = target();
    if (!prize || (events & ground::target_lost_events)) {
        speak(ground::speech_failed, "Capture failed");
        return ground::mission_failed;
    }
    auto& sprayed = record.construction.type_index;
    auto& needed = record.construction.remaining;
    switch (order.phase) {
    case 0: {
        if (!s.record.movement || !(def().abilities & OA_UNIT_DEF_ABILITY_CAN_CAPTURE))
            return ground::mission_invalid;
        const auto& prize_def = def_of(prize->record);
        if (prize_def.abilities & OA_UNIT_DEF_ABILITY_CAN_CAPTURE) {
            speak(ground::speech_failed, "That unit cannot be captured");
            return ground::mission_failed;
        }
        if (!build::finished(prize->record)) {
            speak(ground::speech_failed, "That unit is a cloud of vapor and cannot be captured");
            return ground::mission_failed;
        }
        announce("Capturing");
        const double scale = build::capture_cost_scale;
        const auto costs = truncate_low32(
            static_cast<double>(prize_def.build_cost_energy) * scale *
                build::capture_energy_weight +
            static_cast<double>(prize_def.build_cost_metal) * scale * build::capture_metal_weight +
            build::capture_base_ticks
        );
        const auto capped = costs < build::capture_tick_cap ? costs : build::capture_tick_cap;
        const auto maximum = static_cast<uint32_t>(prize_def.max_damage);
        // A type with no maximum health takes a quotient of 0.
        const auto scaled =
            maximum != 0
                ? (static_cast<uint32_t>(static_cast<int32_t>(prize->record.health)) + maximum) *
                      static_cast<uint32_t>(capped) / (maximum * 2)
                : 0u;
        // The prize's veteran level (kills / 5 in 3.1c) raises the base of 10.
        const auto veterancy =
            sim::unit_health::veteran_capture_level(
                host.match.rules_view(), prize->record.type_index, prize->record.veteran_level
            ) +
            static_cast<uint32_t>(sim::unit_health::capture_base_level);
        needed = static_cast<int32_t>(veterancy * scaled * 10u) / 100;
        occupy_weapons_while_busy(build::BusyState::capture);
        outline_goal(
            {prize->record.cell_x, prize->record.cell_z},
            {prize->record.footprint_x, prize->record.footprint_z}
        );
        order.wait_events = ground::reclaim_goal_events;
        return ground::next_phase;
    }
    case 1:
        if (events & ground::path_failed_event)
            return ground::mission_failed;
        if (!within_build_distance(prize->record))
            return ground::restart_mission;
        start_building_toward(ground::position_of(prize->record));
        return ground::next_phase;
    case 2:
        // The look runs StartBuilding again; a prize that is gone ended the
        // order on the look's step already.
        return wait_for_build_stance(ground::target_lost_events, [&] {
            start_building_toward(ground::position_of(prize->record));
            return ground::keep_waiting;
        });
    case 3:
        speak(ground::speech_work_started);
        return ground::next_phase;
    case 4:
        if (prize->record.movement && (prize->record.flags & OA_UNIT_FLAG_MOVE_RATE_MASK)) {
            stop_building();
            wait_ticks(build::capture_moving_wait);
            return ground::restart_mission;
        }
        if (sprayed >= needed)
            return ground::next_phase;
        host.match.spray_nano(s, *prize, Match::NanoSpray::reclaim);
        s.record.decloak_until_tick = tick() + build::capture_decloak_hold;
        sprayed += build::nano_step;
        wait_ticks(build::nano_step);
        return ground::keep_waiting;
    case 5:
        host.match.capture_unit(host.slot(*prize), s);
        speak(ground::speech_captured);
        return ground::mission_done;
    default:
        return ground::mission_invalid;
    }
}

uint32_t TickHost::GroundMissions::reclaim_unit() {
    auto* victim = target();
    if (!victim || (events & ground::target_lost_events))
        return ground::mission_done;
    auto& bite = record.construction.type_index;
    auto& sprayed = record.construction.remaining;
    switch (order.phase) {
    case 0: {
        uint32_t result = ground::mission_invalid;
        if (s.record.movement && (def().abilities & OA_UNIT_DEF_ABILITY_CAN_RECLAMATE)) {
            if (can_reclaim(victim->record)) {
                announce("Reclaiming");
                occupy_weapons_while_busy(build::BusyState::reclaim_unit);
                return ground::next_phase;
            }
            speak(ground::speech_failed, "That unit cannot be reclaimed");
            result = ground::mission_failed;
        }
        speak(ground::speech_failed, "Reclamation failed");
        return result;
    }
    case 1:
        if (events & ground::arrived_event)
            return ground::next_phase;
        outline_goal(
            {victim->record.cell_x, victim->record.cell_z},
            {victim->record.footprint_x, victim->record.footprint_z}
        );
        order.wait_events |= ground::reclaim_goal_events;
        wait_ticks(build::reclaim_approach_wait);
        bite = reclaim_bite(victim->record, build::reclaim_bite_duration);
        sprayed = 0;
        return ground::keep_waiting;
    case 2:
        if (events & ground::path_failed_event)
            return ground::retry_mission;
        start_building_toward(ground::position_of(victim->record));
        return ground::next_phase;
    case 3:
        // A look without the stance starts the order over for a unit that
        // can no longer be reclaimed (one that took off), so that the first
        // phase refuses it, or runs StartBuilding again.
        return wait_for_build_stance(ground::target_lost_events, [&] {
            if (!can_reclaim(victim->record))
                return ground::restart_mission;
            start_building_toward(ground::position_of(victim->record));
            return ground::keep_waiting;
        });
    case 4:
        speak(ground::speech_work_started);
        return ground::next_phase;
    case 5: {
        const auto dx = ground::sub_fixed(s.record.position.x, victim->record.position.x);
        const auto dz = ground::sub_fixed(s.record.position.z, victim->record.position.z);
        const auto reach =
            static_cast<int32_t>(static_cast<int16_t>(def_of(victim->record).size_radius >> 16)) +
            static_cast<int32_t>(static_cast<uint16_t>(def().build_distance));
        if (ground::squared_high(dz) + ground::squared_high(dx) > reach * reach ||
            !can_reclaim(victim->record)) {
            wait_ticks(build::reclaim_retry_wait);
            stop_building();
            return ground::restart_mission;
        }
        if (sprayed >= build::reclaim_bite_ticks) {
            host.scaled_damage(&s, host.slot(*victim), bite, build::reclaim_damage_kind);
            sprayed = 0;
        }
        s.record.decloak_until_tick = tick() + build::reclaim_decloak_hold;
        host.match.spray_nano(s, *victim, Match::NanoSpray::reclaim);
        wait_ticks(build::nano_step);
        sprayed += build::nano_step;
        return ground::keep_waiting;
    }
    default:
        return ground::mission_invalid;
    }
}

uint32_t TickHost::GroundMissions::repair_unit() {
    auto* patient = target();
    if (!patient) {
        speak(ground::speech_failed, "Repairs unsuccessful.");
        return ground::mission_done;
    }
    if (record.attack.leash != 0) {
        const auto strayed = static_cast<int32_t>(base::game_math::distance(
            static_cast<int16_t>(s.record.position.x >> 16) - record.extra.anchor_x,
            static_cast<int16_t>(s.record.position.z >> 16) - record.extra.anchor_z
        ));
        if (record.attack.leash <= strayed)
            return ground::mission_done;
    }
    if ((patient->record.flags & OA_UNIT_FLAG_OCCUPANCY_MASK) != ground::occupancy_ground) {
        speak(ground::speech_failed, "Repairs unsuccessful.");
        return ground::mission_done;
    }
    const auto repaired = [&] {
        return static_cast<uint32_t>(static_cast<int32_t>(patient->record.health)) >=
               def_of(patient->record).max_damage;
    };
    switch (order.phase) {
    case 0:
        if (!s.record.movement || !(def().flags & OA_UNIT_DEF_FLAG_BUILDER) ||
            !build::finished(patient->record))
            return ground::mission_invalid;
        announce("Repairing");
        return ground::next_phase;
    case 1:
        if (events & ground::path_failed_event)
            return ground::mission_failed;
        if (within_build_distance(patient->record)) {
            occupy_weapons_while_busy(build::BusyState::repair);
            start_building_toward(ground::position_of(patient->record));
            return ground::next_phase;
        }
        outline_goal(
            {patient->record.cell_x, patient->record.cell_z},
            {patient->record.footprint_x, patient->record.footprint_z}
        );
        wait_ticks(random(build::approach_jitter) + build::approach_wait);
        order.wait_events |= ground::build_goal_events;
        return ground::keep_waiting;
    case 2:
        // A look without the stance ends the order once the patient is at
        // full health, or runs StartBuilding again.
        return wait_for_build_stance(ground::repair_step_event, [&] {
            if (repaired())
                return ground::mission_done;
            start_building_toward(ground::position_of(patient->record));
            return ground::keep_waiting;
        });
    case 3:
        if (repaired())
            return ground::next_phase;
        if (patient->record.flags & OA_UNIT_FLAG_MOVE_RATE_MASK) {
            stop_building();
            wait_ticks(build::target_moving_wait);
            return ground::restart_mission;
        }
        s.record.decloak_until_tick = tick() + build::repair_decloak_hold;
        (void)host.nano_repair(s, *patient);
        wait_ticks(1);
        order.wait_events |= ground::repair_step_event;
        return ground::keep_waiting;
    case 4:
        speak(ground::speech_repaired, "Unit repaired");
        return ground::mission_done;
    default:
        return ground::mission_invalid;
    }
}

uint32_t TickHost::GroundMissions::repair_unit_no_move() {
    auto* patient = target();
    if (!patient) {
        speak(ground::speech_failed, "Repairs unsuccessful.");
        return ground::mission_done;
    }
    switch (order.phase) {
    case 0:
        if (!(def().flags & OA_UNIT_DEF_FLAG_BUILDER))
            return ground::mission_invalid;
        if (!build::finished(patient->record) || !(s.record.state_flags & OA_UNIT_STATE_ACTIVE))
            return ground::mission_failed;
        AttackAdapter(host, s, record).release_weapon_targets(build::all_weapons);
        return ground::next_phase;
    case 1:
        if (static_cast<uint32_t>(static_cast<int32_t>(patient->record.health)) <
                def_of(patient->record).max_damage &&
            !(patient->record.flags & OA_UNIT_FLAG_MOVE_RATE_MASK)) {
            s.record.decloak_until_tick = tick() + build::repair_decloak_hold;
            (void)host.nano_repair(s, *patient);
            wait_ticks(1);
            order.wait_events |= ground::repair_step_event;
            return ground::keep_waiting;
        }
        return ground::next_phase;
    case 2:
        speak(ground::speech_repaired, "Unit repaired");
        return ground::mission_done;
    default:
        return ground::mission_invalid;
    }
}

} // namespace oa::sim::match_runtime

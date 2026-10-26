// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "ground_missions.hpp"

namespace oa::sim::match_runtime {

uint32_t TickHost::GroundMissions::reclaim() {
    const auto site = feature_site(record.extra.destination);
    if (!site) {
        speak(ground::speech_failed, "Reclamation failed");
        return ground::mission_failed;
    }
    if (!reclaimable(*site))
        return ground::mission_failed;
    auto& remaining = record.extra.tolerance;
    switch (order.phase) {
    case 0:
        if (!s.record.movement || !(def().abilities & OA_UNIT_DEF_ABILITY_CAN_RECLAMATE))
            return ground::mission_invalid;
        outline_goal({site->cell_x, site->cell_z}, {site->footprint_x, site->footprint_z});
        order.wait_events = ground::goal_events;
        return ground::next_phase;
    case 1: {
        if (events & ground::path_failed_event)
            return ground::mission_failed;
        remaining = truncate_low32(
            (static_cast<double>(site->def->metal) + static_cast<double>(site->def->energy)) *
                ground::reclaim_time_scale +
            ground::reclaim_time_base
        );
        start_building_toward(work_point(*site));
        return ground::next_phase;
    }
    case 2:
        return wait_for_build_stance(0);
    case 3:
        // The display rules may say "working" once: the next steps reclaim
        // in stage 4, which also gets to stage 5 one step sooner.
        if (host.match.display_rules().reclaim_voice_once)
            order.phase = 4;
        speak(ground::speech_work_started);
        [[fallthrough]];
    case 4:
        wait_ticks(ground::reclaim_step);
        remaining -= static_cast<int32_t>(ground::reclaim_step);
        if (remaining <= 0)
            return ground::next_phase;
        s.record.decloak_until_tick = tick() + ground::decloak_hold;
        if (remaining > ground::reclaim_spray_floor)
            for (int32_t spray = 0; spray < ground::reclaim_sprays_per_step; ++spray)
                host.match.spray_nano_feature(s, site->cell_x, site->cell_z, *site->def, true);
        return ground::keep_waiting;
    case 5:
        (void)reclaim_feature_at(record.extra.destination);
        return ground::mission_done;
    default:
        return ground::mission_invalid;
    }
}

uint32_t TickHost::GroundMissions::resurrect() {
    std::optional<ground::FeatureSite> site;
    if (order.phase <= 5) {
        site = feature_site(record.extra.destination);
        if (!site) {
            speak(ground::speech_failed, "Resurrection failed");
            return ground::mission_failed;
        }
        if (!reclaimable(*site))
            return ground::mission_failed;
    }
    auto& type = record.construction.type_index;
    auto& remaining = record.construction.remaining;
    switch (order.phase) {
    case 0:
        if (!s.record.movement || !(def().abilities & OA_UNIT_DEF_ABILITY_CAN_RESURRECT))
            return ground::mission_invalid;
        outline_goal({site->cell_x, site->cell_z}, {site->footprint_x, site->footprint_z});
        order.wait_events = ground::goal_events;
        return ground::next_phase;
    case 1:
        if (events & ground::path_failed_event)
            return ground::mission_failed;
        start_building_toward(work_point(*site));
        return ground::next_phase;
    case 2:
        return wait_for_build_stance(0);
    case 3: {
        // The wreck's name up to its first underscore names the unit type.
        std::string name;
        for (size_t i = 0; i + 1 < 0x40 && i < sizeof(site->def->name); ++i) {
            const auto c = site->def->name[i];
            if (c == '\0' || c == '_')
                break;
            name.push_back(static_cast<char>(std::toupper(static_cast<unsigned char>(c))));
        }
        type = find_loaded_type(host.match.input_, name);
        if (type == 0) {
            // The game's own spelling.
            speak(ground::speech_failed, "Ressurection failed");
            return ground::mission_failed;
        }
        const auto& raised = world().unit_defs[static_cast<size_t>(type)];
        const auto workers = static_cast<int32_t>(static_cast<uint16_t>(def().worker_time)) /
                             ground::worker_ticks_per_second;
        remaining = truncate_low32(
            static_cast<double>(raised.build_time) * ground::resurrect_time_scale /
            static_cast<double>(workers)
        );
        speak(ground::speech_work_started);
        return ground::next_phase;
    }
    case 4:
        if (remaining-- == 0)
            return ground::next_phase;
        host.match.spray_nano_feature(s, site->cell_x, site->cell_z, *site->def, false);
        s.record.decloak_until_tick = tick() + ground::decloak_hold;
        wait_ticks(1);
        return ground::keep_waiting;
    case 5: {
        // A building rises facing as its wreck lies (units.build-rotation).
        const auto& wreck_point = record.extra.destination;
        if (const auto wreck = plot_index(
                wreck_point[0] >> ground::cell_shift, wreck_point[2] >> ground::cell_shift
            ))
            if (const auto* placed = sim::feature_runtime::feature_record(
                    world(), world().plots[feature_origin(*wreck)].feature_record
                ))
                record.construction.facing = host.match.build_facing_of_heading(
                    static_cast<uint16_t>(type), static_cast<uint16_t>(placed->orientation[1])
                );
        auto* raised = ConstructionAdapter(host, s, record).spawn_nanoframe();
        set_target(raised);
        if (!raised) {
            speak(ground::speech_failed, "Unable to create any more units");
            wait_ticks(ground::resurrect_no_slot_wait);
            return ground::keep_waiting;
        }
        const auto& point = record.extra.destination;
        const auto index =
            plot_index(point[0] >> ground::cell_shift, point[2] >> ground::cell_shift);
        if (!index || origin_feature_word(feature_origin(*index)) >= ground::first_reserved_feature)
            return ground::mission_failed;
        const auto origin = feature_origin(*index);
        copy_wreck_orientation(raised->record, origin);
        (void)sim::feature_runtime::clear_plot_feature(
            world(), host.match.feature_host(), *index, false
        );
        // The wreck goes on the other players' machines too, named by its
        // origin plot, when the resurrecting unit is simulated here.
        const auto& multiplayer = host.match.multiplayer;
        if (multiplayer.feature_changed != nullptr &&
            sim::simulation_state::locally_simulated(*s.unit)) {
            const auto width = static_cast<size_t>(host.match.spatial_.terrain_width);
            multiplayer.feature_changed(
                multiplayer.context,
                sim::feature_runtime::FeatureChange::resurrected,
                static_cast<int32_t>(origin % width),
                static_cast<int32_t>(origin / width),
                s.unit_index
            );
        }
        raised->record.build_remaining = 0.0F;
        raised->record.health = 1;
        // The raised unit is finished by the unit that raised it.
        host.match.report_finished(host.slot(*raised).unit_index, s.unit_index);
        ConstructionAdapter(host, s, record).refresh_selected();
        return ground::next_phase;
    }
    case 6: {
        speak(ground::speech_complete, "Resurrection complete");
        auto* raised = target();
        if (raised) {
            if (const auto kind = repair_command_kind(*raised))
                push_order_front(create_order(kind, raised, nullptr, 0, 0, 0));
        }
        return ground::mission_done;
    }
    default:
        return ground::mission_invalid;
    }
}

uint32_t TickHost::GroundMissions::repair_patrol() {
    if (order.phase == 0) {
        if (auto* aimed = target())
            record.extra.destination = ground::position_of(aimed->record);
        clone_patrol();
        return ground::next_phase;
    }
    if (order.phase != 1)
        return ground::mission_invalid;
    if (events & ground::goal_events)
        return ground::rotate_mission;
    circle_goal(record.extra.destination, ground::patrol_arrival);
    wait_ticks(ground::patrol_wait);
    order.wait_events |= ground::goal_events;
    auto* owner = oa::world_unit_owner(&world(), &s.record);
    if (!owner)
        return ground::keep_waiting;
    const auto low = [](float stored, float storage) {
        return static_cast<double>(storage) * ground::reserve_fraction >
               static_cast<double>(stored);
    };
    const int32_t radius = static_cast<int32_t>(static_cast<uint32_t>(def().sight_distance) << 16);
    // orders.con-patrol-guard-options: reclaim only skips the repair and
    // assist search, assist only stops before the reclaim search.
    const auto choice = patrol_choice();
    if (choice != ground::patrol_reclaim_only && !low(owner->energy, owner->energy_storage)) {
        const auto candidates = repair_candidates(radius);
        if (!candidates.empty()) {
            auto& patient = *candidates[random(static_cast<uint32_t>(candidates.size()))];
            if (allied(s.record.owner_index, patient.record.owner_index) &&
                repair_command_kind(patient))
                return queue_repair(patient) ? ground::rotate_mission : ground::retry_later;
        }
    }
    if (choice == ground::patrol_assist_only)
        return ground::keep_waiting;
    if (!low(owner->energy, owner->energy_storage) && !low(owner->metal, owner->metal_storage))
        return ground::keep_waiting;
    const auto features = choose_reclaim_features(ground::position_of(s.record), radius);
    if (!features.found)
        return ground::keep_waiting;
    const auto fits = [](const FeatureChoice& feature, float stored, float storage) {
        return !(
            static_cast<double>(feature.amount) + static_cast<double>(stored) >
            static_cast<double>(storage)
        );
    };
    const FeatureChoice* chosen = nullptr;
    if (features.metal.site && low(owner->metal, owner->metal_storage))
        chosen = &features.metal;
    else if (features.energy.site && low(owner->energy, owner->energy_storage))
        chosen = &features.energy;
    else if (features.metal.site && fits(features.metal, owner->metal, owner->metal_storage))
        chosen = &features.metal;
    else if (features.energy.site && fits(features.energy, owner->energy, owner->energy_storage))
        chosen = &features.energy;
    if (!chosen)
        return ground::keep_waiting;
    install_goal(nullptr);
    push_order_front(create_order(ground::reclaim_kind, nullptr, &*chosen->site, 0, 0, 0));
    install_goal(nullptr);
    order.wait_events = 0;
    return ground::retry_later;
}

uint32_t TickHost::GroundMissions::guard_no_move() {
    if (events & ground::target_lost_events) {
        order.phase = 3;
        return ground::keep_waiting;
    }
    AttackAdapter weapons(host, s, record);
    auto& shots = record.attack.weapon_slot;
    auto& patience = record.attack.retry;
    switch (order.phase) {
    case 0:
        weapons.reset_weapons();
        wait_ticks(ground::guard_wait);
        return ground::next_phase;
    case 1: {
        set_target(weapon_target(0));
        auto* aimed = target();
        if (aimed && (aimed->record.flags & OA_UNIT_FLAG_LIVE)) {
            record.extra.destination = ground::position_of(aimed->record);
            weapons.release_weapon_targets(0);
            weapons.assign_target(*aimed, 0);
            shots = 0;
            patience = static_cast<int32_t>(random(3) + 3);
            return ground::next_phase;
        }
        wait_ticks(ground::guard_wait);
        return ground::keep_waiting;
    }
    case 2: {
        if (events & ground::guard_hold_event)
            shots = 0;
        else
            ++shots;
        auto* aimed = target();
        if (shots <= patience && aimed && weapons.can_reach(*aimed, 0)) {
            order.wait_events |= ground::guard_fire_events;
            return ground::keep_waiting;
        }
        if (static_cast<int32_t>(random(100)) <
            static_cast<int32_t>(ground::guard_give_up_percent)) {
            shots = 0;
            return ground::next_phase;
        }
        return ground::restart_mission;
    }
    case 3: {
        const auto known = known_units_near(
            record.extra.destination, static_cast<int32_t>(ground::guard_search_radius)
        );
        if (known.empty())
            return ground::restart_mission;
        auto* chosen = known[random(static_cast<uint32_t>(known.size()))];
        set_target(chosen);
        weapons.assign_target(*chosen, 0);
        order.phase = 1;
        return ground::keep_waiting;
    }
    default:
        return ground::mission_invalid;
    }
}

uint32_t TickHost::GroundMissions::attack_unit_type() {
    if (order.phase == 0) {
        if (!(def().abilities & OA_UNIT_DEF_ABILITY_CAN_ATTACK))
            return ground::mission_invalid;
        wait_ticks(random(ground::attack_type_wait) + 1);
        return ground::next_phase;
    }
    if (order.phase != 1)
        return ground::mission_invalid;
    sim::simulation_state::Unit* best = nullptr;
    int32_t best_distance = 0x7fffffff;
    const auto wanted = static_cast<uint32_t>(record.construction.type_index);
    auto& state = world();
    for (uint32_t slot = 1; slot < state.unit_slot_count; ++slot) {
        const auto& other = state.units[slot];
        if (other.type_index != wanted || allied(s.record.owner_index, other.owner_index))
            continue;
        auto distance =
            ground::squared_high(ground::sub_fixed(other.position.z, s.record.position.z)) +
            ground::squared_high(ground::sub_fixed(other.position.x, s.record.position.x));
        distance -= static_cast<int32_t>(random(static_cast<uint32_t>(distance / 2)));
        if (distance <= best_distance) {
            best = unit_at(static_cast<int32_t>(slot));
            best_distance = distance;
        }
    }
    if (!best)
        return ground::mission_done;
    auto& attack = create_order(0, nullptr, nullptr, 0, 0, 0);
    const auto kind = AttackAdapter(host, s, attack).morph_attack_command(best);
    apply_descriptor(attack, kind);
    attack.extra.command_flags &= static_cast<uint8_t>(~ground::order_has_point);
    if (attack.extra.command_flags & ground::order_has_target)
        retarget(attack, best);
    push_order_front(attack);
    return ground::restart_mission;
}

uint32_t TickHost::GroundMissions::build_weapon() {
    const auto slot = record.attack.weapon_slot;
    auto& remaining = record.construction.remaining;
    auto& progress = record.construction.blocked_retries;
    if (slot < 0 || slot >= 3)
        return ground::mission_invalid;
    auto& weapons = host.match.weapons_[s.unit_index];
    auto& stockpile = s.record.weapons[static_cast<size_t>(slot)].stockpile;
    switch (order.phase) {
    case 0:
        if (remaining < 1)
            return ground::mission_done;
        if (stockpile >= ground::stockpile_limit) {
            wait_ticks(ground::stockpile_full_wait);
            return ground::keep_waiting;
        }
        progress = 0;
        return ground::next_phase;
    case 1: {
        const auto* weapon = weapons.definitions[static_cast<size_t>(slot)];
        if (!weapon)
            return ground::mission_invalid;
        const auto total = static_cast<int32_t>(weapon->reload_time_ticks);
        const auto step = std::min(progress + ground::stockpile_step, total);
        const auto share = [&](int32_t ticks, float cost) {
            return truncate_low32(
                static_cast<double>(ticks) * static_cast<double>(cost) / static_cast<double>(total)
            );
        };
        const auto energy =
            share(step, weapon->energy_per_shot) - share(progress, weapon->energy_per_shot);
        const auto metal =
            share(step, weapon->metal_per_shot) - share(progress, weapon->metal_per_shot);
        if (!debit(static_cast<float>(energy), static_cast<float>(metal))) {
            wait_ticks(ground::stockpile_short_wait);
            return ground::keep_waiting;
        }
        progress = step;
        if (step >= total)
            return ground::next_phase;
        wait_ticks(static_cast<uint32_t>(ground::stockpile_step));
        return ground::keep_waiting;
    }
    case 2:
        stockpile = static_cast<uint8_t>(stockpile + 1);
        --remaining;
        refresh_panel();
        return ground::restart_mission;
    default:
        return ground::mission_invalid;
    }
}

uint32_t TickHost::GroundMissions::standby_mine() {
    if (order.phase == 0) {
        if (!(s.record.flags & OA_UNIT_FLAG_BUILDING))
            return ground::mission_invalid;
        AttackAdapter(host, s, record).reset_weapons();
        order.wait_events |= ground::standby_mine_event;
        wait_ticks(1);
        return ground::next_phase;
    }
    if (order.phase != 1)
        return ground::mission_invalid;
    auto* victim = host.match.find_automatic_target(*s.unit);
    if (victim &&
        (victim->record.flags & OA_UNIT_FLAG_OCCUPANCY_MASK) == ground::occupancy_ground &&
        (s.record.flags & OA_UNIT_FLAG_FIRE_ORDER_MASK)) {
        push_order_front(self_destruct_order());
        return ground::mission_done;
    }
    order.wait_events |= ground::standby_mine_event;
    wait_ticks(random(ground::mine_wait) + ground::mine_wait);
    return ground::keep_waiting;
}

uint32_t TickHost::GroundMissions::attack_kamikaze() {
    if (events & ground::target_lost_events)
        return ground::mission_done;
    if (auto* aimed = target())
        record.extra.destination = ground::position_of(aimed->record);
    if (order.phase == 0) {
        if (s.record.attach_parent)
            return ground::mission_invalid;
        announce();
        const auto reach = static_cast<uint16_t>(def().kamikaze_distance);
        circle_goal(
            record.extra.destination,
            reach < ground::kamikaze_min_arrival ? ground::kamikaze_min_arrival : reach
        );
        wait_ticks(ground::kamikaze_wait);
        order.wait_events |= ground::goal_events;
        return ground::next_phase;
    }
    if (order.phase != 1)
        return ground::mission_invalid;
    if (events & ground::arrived_event) {
        speak(ground::speech_kamikaze);
        push_order_front(self_destruct_order());
        return ground::mission_done;
    }
    if (events & ground::path_failed_event)
        return ground::mission_failed;
    order.phase = 0;
    return ground::keep_waiting;
}

uint32_t TickHost::GroundMissions::park() {
    if (order.phase == 1) {
        if ((events & ground::arrived_event) || order.next)
            return ground::mission_done;
        wait_ticks(ground::park_wait);
        return ground::restart_mission;
    }
    if (order.phase != 0 || !s.record.movement)
        return ground::mission_invalid;
    if (can_fly()) {
        record.extra.destination = ground::position_of(s.record);
        morph(sim::ground_orders::vtol_move_kind);
        return ground::restart_mission;
    }
    auto spread = static_cast<int32_t>(s.record.footprint_x);
    if (def().min_water_depth >= 0)
        spread += 3;
    const auto cell_x =
        static_cast<int16_t>(static_cast<uint32_t>(s.record.position.x) >> ground::cell_shift);
    const auto cell_z =
        static_cast<int16_t>(static_cast<uint32_t>(s.record.position.z) >> ground::cell_shift);
    outline_goal(
        {static_cast<int16_t>(cell_x - spread * 4), static_cast<int16_t>(cell_z - spread * 3)},
        {static_cast<int16_t>(spread * 8), static_cast<int16_t>(spread * 6)}
    );
    order.wait_events = ground::goal_events;
    return ground::next_phase;
}

uint32_t TickHost::GroundMissions::pickup() {
    auto* cargo = target();
    if (!cargo || (events & ground::target_lost_event)) {
        speak(ground::speech_failed, "Transport mission failed");
        return ground::mission_failed;
    }
    auto& attempts = record.extra.tolerance;
    switch (order.phase) {
    case 0:
        if (!s.record.movement || !can_load())
            return ground::mission_invalid;
        if (cargo->record.footprint_x > int16_t{static_cast<uint8_t>(def().transport_size)}) {
            speak(ground::speech_failed, "Unit is too large to transport");
            return ground::mission_failed;
        }
        announce("Loading unit");
        return ground::next_phase;
    case 1:
    case 3:
        return wait_until_not_busy(ground::target_lost_event);
    case 2:
        start_transport_script(
            ScriptFunction::transport_pickup, {int32_t{cargo->record.id}, 0, 0, 0}
        );
        speak(ground::speech_cargo_loaded);
        ++attempts;
        wait_ticks(ground::transport_script_wait);
        return ground::next_phase;
    case 4:
        if (cargo->record.attach_parent)
            return ground::mission_done;
        if (attempts >= ground::transport_attempts)
            return ground::retry_mission;
        circle_goal(ground::position_of(cargo->record), 0);
        order.wait_events = ground::goal_events | ground::target_lost_event;
        return ground::next_phase;
    case 5:
        install_goal(nullptr);
        return ground::restart_mission;
    default:
        return ground::mission_invalid;
    }
}

uint32_t TickHost::GroundMissions::unload() {
    if (events & ground::target_lost_event) {
        speak(ground::speech_failed, "Unloading process is proceeding non-optimally");
        return ground::mission_failed;
    }
    auto& attempts = record.extra.tolerance;
    switch (order.phase) {
    case 0: {
        if (!s.record.movement || !can_load())
            return ground::mission_invalid;
        set_target(unit_at(link_first_child(s.record)));
        auto* cargo = target();
        if (!cargo)
            return ground::mission_done;
        announce("Unloading");
        // TransportDrop reads the point as X and Z whole units in one word.
        const auto& to = record.extra.destination;
        const auto packed = std::bit_cast<int32_t>(
            (std::bit_cast<uint32_t>(to[0]) & 0xffff0000u) + std::bit_cast<uint32_t>(to[2] >> 16)
        );
        start_transport_script(
            ScriptFunction::transport_drop, {int32_t{cargo->record.id}, packed, 0, 0}
        );
        ++attempts;
        wait_ticks(ground::transport_script_wait);
        return ground::next_phase;
    }
    case 1:
        return wait_until_not_busy(ground::target_lost_event);
    case 2: {
        auto* cargo = target();
        if (!cargo || link_parent(cargo->record) != s.unit_index) {
            speak(ground::speech_cargo_unloaded);
            return ground::mission_done;
        }
        if (attempts >= ground::transport_attempts)
            return ground::retry_mission;
        int32_t reach = 0;
        if (def().flags & OA_UNIT_DEF_FLAG_CAN_HOVER) {
            const auto* bounds = host.match.bounds_for(*s.unit);
            const auto extent_z = bounds ? static_cast<int16_t>(bounds->size_z >> 16) : 0;
            reach = truncate_low32(extent_z * ground::hover_unload_reach);
        }
        circle_goal(record.extra.destination, reach);
        order.wait_events = ground::goal_events | ground::target_lost_event;
        return ground::next_phase;
    }
    case 3:
        return ground::restart_mission;
    default:
        return ground::mission_invalid;
    }
}

uint32_t TickHost::GroundMissions::wait() {
    auto& remaining = record.extra.tolerance;
    const auto radius = record.attack.retry;
    if (radius == 0)
        return wait_order(order.phase, remaining, order.wait_events, order.wake_tick, tick());
    if (!known_units_near(ground::position_of(s.record), radius).empty())
        return ground::mission_done;
    if (remaining < 1)
        return ground::mission_done;
    const auto ticks = random(ground::wait_retry_jitter) + ground::wait_retry_base;
    remaining -= static_cast<int32_t>(ticks);
    wait_ticks(ticks);
    return ground::keep_waiting;
}

void TickHost::retarget(Match::RuntimeOrder& entry, sim::simulation_state::Unit* unit) {
    match.unlink_target_observer(entry);
    entry.construction.target = unit;
    match.link_target_observer(entry, unit);
}

bool TickHost::dispatch_ground_mission(
    sim::unit_spawn::Slot& s, sim::simulation_state::Order& order, uint32_t events, uint32_t& result
) {
    using Handler = uint32_t (GroundMissions::*)();
    Handler handler = nullptr;
    switch (order.kind) {
    case activate_kind:
        handler = &GroundMissions::activate;
        break;
    case attack_chase_kind:
        handler = &GroundMissions::attack_chase;
        break;
    case ground::attack_kamikaze_kind:
        handler = &GroundMissions::attack_kamikaze;
        break;
    case attack_no_move_kind:
        handler = &GroundMissions::attack_no_move;
        break;
    case attack_special_kind:
        handler = &GroundMissions::attack_special;
        break;
    case ground::attack_unit_type_kind:
        handler = &GroundMissions::attack_unit_type;
        break;
    case be_carried_kind:
        handler = &GroundMissions::be_carried;
        break;
    case building_build_kind:
        handler = &GroundMissions::building_build;
        break;
    case ground::build_weapon_kind:
        handler = &GroundMissions::build_weapon;
        break;
    case capture_kind:
        handler = &GroundMissions::capture;
        break;
    case cloak_off_kind:
        handler = &GroundMissions::cloak_off;
        break;
    case cloak_on_kind:
        handler = &GroundMissions::cloak_on;
        break;
    case deactivate_kind:
        handler = &GroundMissions::deactivate;
        break;
    case follow_ground_kind:
        handler = &GroundMissions::follow;
        break;
    case get_built_kind:
        handler = &GroundMissions::get_built;
        break;
    case ground_pickup_kind:
        handler = &GroundMissions::pickup;
        break;
    case ground_unload_kind:
        handler = &GroundMissions::unload;
        break;
    case ground::guard_no_move_kind:
        handler = &GroundMissions::guard_no_move;
        break;
    case help_build_kind:
        handler = &GroundMissions::help_build;
        break;
    case make_selectable_kind:
        handler = &GroundMissions::make_selectable;
        break;
    case mobile_build_kind:
        handler = &GroundMissions::mobile_build;
        break;
    case ground::park_kind:
        handler = &GroundMissions::park;
        break;
    case patrol_kind:
        handler = &GroundMissions::patrol;
        break;
    case qmove_kind:
    case qpatrol_kind:
        handler = &GroundMissions::queued_move;
        break;
    case ground::reclaim_kind:
        handler = &GroundMissions::reclaim;
        break;
    case reclaim_unit_kind:
        handler = &GroundMissions::reclaim_unit;
        break;
    case ground::repair_patrol_kind:
        handler = &GroundMissions::repair_patrol;
        break;
    case repair_unit_kind:
        handler = &GroundMissions::repair_unit;
        break;
    case repair_unit_no_move_kind:
        handler = &GroundMissions::repair_unit_no_move;
        break;
    case ground::resurrect_kind:
        handler = &GroundMissions::resurrect;
        break;
    case self_destruct_kind:
    case self_destruct_fg_kind:
        handler = &GroundMissions::self_destruct;
        break;
    case self_repair_kind:
        handler = &GroundMissions::self_repair;
        break;
    case ground::standby_mine_kind:
        handler = &GroundMissions::standby_mine;
        break;
    case standing_fire_order_kind:
        handler = &GroundMissions::standing_fire_order;
        break;
    case standing_move_order_kind:
        handler = &GroundMissions::standing_move_order;
        break;
    case stop_kind:
        handler = &GroundMissions::stop;
        break;
    case suppress_kind:
        handler = &GroundMissions::suppress;
        break;
    case teleport_kind:
        handler = &GroundMissions::teleport;
        break;
    case wait_kind:
        handler = &GroundMissions::wait;
        break;
    case wait_for_attack_kind:
        handler = &GroundMissions::wait_for_attack;
        break;
    default:
        return false;
    }
    GroundMissions missions(*this, s, order, events);
    result = (missions.*handler)();
    return true;
}

} // namespace oa::sim::match_runtime

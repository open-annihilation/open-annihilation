// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "tick_internal.hpp"

#include <cstdint>

namespace oa::sim::match_runtime {

sim::unit_health::UnitType
TickHost::ConstructionAdapter::health_type(sim::unit_spawn::Slot& s) const {
    const auto& definition = *host.match.fields(s).definition;
    return {
        definition.damage_modifier_fixed,
        static_cast<float>(definition.build_cost_energy),
        definition.build_time,
        s.unit->type->maximum_health,
        static_cast<float>(definition.build_cost_metal)
    };
}

void TickHost::ConstructionAdapter::refresh_selected() {
    if (source.record.owner_index == host.match.world_.viewpoint_player &&
        (source.unit->flags & OA_UNIT_FLAG_SELECTED))
        host.match.selection_.frame_flags |= OA_FRAME_FLAG_REFRESH_ORDER_PANEL;
}

bool TickHost::ConstructionAdapter::site_clear() {
    const auto fx = footprint_x();
    const auto fz = footprint_z();
    const auto cell_x = footprint_origin_cell(record.extra.destination[0], fx);
    const auto cell_z = footprint_origin_cell(record.extra.destination[2], fz);
    // BuildingBuild tests the product's site with the factory's occupancy
    // layer (Unit.flags & OA_UNIT_FLAG_OCCUPANCY_MASK); MobileBuild tests the
    // building's site with layer 1.
    // Factory products are mobiles (ARMPW bm_code=1); the building-site test
    // would reject the pad because the lab's yard map sets plot.flags bit 2
    // on every cell.
    const auto occupancy = record.order.kind == building_build_kind
                               ? static_cast<uint8_t>(source.unit->flags & 3u)
                               : uint8_t{1};
    return host.match.site_clear_for(
        static_cast<uint16_t>(record.construction.type_index),
        cell_x,
        cell_z,
        0,
        occupancy,
        facing()
    );
}

void TickHost::ConstructionAdapter::snap_build_height() {
    const auto index = static_cast<uint16_t>(record.construction.type_index);
    if (index >= host.match.world_.types.size()) {
        host.match.fault_.note("construction type is not loaded");
        return;
    }
    if (host.match.world_.types[index].bm_code != 0)
        return;
    const auto fx = footprint_x();
    const auto fz = footprint_z();
    auto& site = record.extra.destination;
    snap_build_position(site, fx, fz);
    const auto yard = host.match.build_yard(index, facing());
    if (fx < 0 || fz < 0 ||
        yard.size() < static_cast<std::size_t>(fx) * static_cast<std::size_t>(fz)) {
        host.match.fault_.note("building yard map does not cover its footprint");
        return;
    }
    const auto height = sim::spatial_state::footprint_build_height(
        fx,
        fz,
        yard,
        host.match.state().unit_defs[index].water_line,
        footprint_origin_cell(site[0], fx),
        footprint_origin_cell(site[2], fz),
        host.match.spatial_
    );
    site[1] = static_cast<int32_t>(static_cast<uint32_t>(height) << 16);
}

sim::simulation_state::Unit* TickHost::ConstructionAdapter::spawn_nanoframe() {
    sim::unit_spawn::Request request;
    request.player = source.record.owner_index;
    request.type = static_cast<uint16_t>(record.construction.type_index);
    request.position = {
        std::bit_cast<uint32_t>(record.extra.destination[0]),
        std::bit_cast<uint32_t>(record.extra.destination[1]),
        std::bit_cast<uint32_t>(record.extra.destination[2])
    };
    request.finished = false;
    request.state = sim::unit_spawn::ground_occupancy_state;
    request.facing = facing();
    auto* created = host.match.create(request, source.unit_index);
    if (!created)
        return nullptr;
    created->unit->flags |= OA_UNIT_FLAG_LIVE | OA_UNIT_FLAG_SELECTABLE;
    record.construction.target = created->unit;
    return created->unit;
}

void TickHost::ConstructionAdapter::issue_get_built(sim::simulation_state::Unit& nanoframe) {
    (void)host.match.insert_ground_order(
        host.slot(nanoframe).unit_index, get_built_kind, std::nullopt, 0, source.unit_index
    );
}

void TickHost::ConstructionAdapter::start_building(int16_t heading) {
    auto* instance = host.match.instance(source.unit_index);
    bool started = false;
    if (instance && instance->script()) {
        const std::array<int32_t, 4> args{heading, 0, 0, 0};
        started = instance->script()->call(ScriptFunction::start_building, args, false);
    }
    // The other players start it with the heading as its one argument,
    // zero-extended from 16 bits.
    host.match.share_named_script_start(
        source.unit_index, "StartBuilding", 1, {static_cast<uint16_t>(heading), 0, 0, 0}
    );
    if (!started)
        source.record.build_flags = static_cast<uint8_t>(source.record.build_flags | 1u);
    record.order.flags |= order_building_flag;
}

bool TickHost::ConstructionAdapter::build_progress(
    sim::simulation_state::Unit& nanoframe, float rate
) {
    auto& s = host.slot(nanoframe);
    auto type = health_type(s);
    sim::unit_health::Unit builder_proj{
        source.unit_index,
        source.record.state_flags,
        source.record.veteran_level,
        source.unit->health,
        &type
    };
    sim::unit_health::Unit target_proj{
        s.unit_index,
        s.record.state_flags,
        s.record.veteran_level,
        nanoframe.health,
        &type,
        s.record.build_remaining,
        nanoframe.events,
        nanoframe.flags
    };
    target_proj.type_index = s.record.type_index;
    HealthHost health(host.match, source.unit_index);
    // The step's own changes reach the frame before the cancel damage or the
    // completion acts on it, and what those leave, such as a pending death,
    // is what is stored.
    health.bind(target_proj);
    const auto result =
        sim::unit_health::apply_build_progress(builder_proj, target_proj, rate, health);
    health.store();
    nanoframe.health = target_proj.health;
    nanoframe.flags = target_proj.flags;
    nanoframe.events = target_proj.events;
    s.record.build_remaining = target_proj.build_remaining;
    return result.performed;
}

bool TickHost::ConstructionAdapter::construct(sim::simulation_state::Unit& nanoframe, float rate) {
    const bool performed = build_progress(nanoframe, rate);
    if (performed)
        host.match.spray_nano(source, nanoframe, Match::NanoSpray::build);
    return performed;
}

void TickHost::ConstructionAdapter::link_built(sim::simulation_state::Unit& nanoframe) {
    auto& s = host.slot(nanoframe);
    if (!(source.record.flags & OA_UNIT_FLAG_LIVE) || !(s.record.flags & OA_UNIT_FLAG_LIVE))
        return;
    auto type = health_type(s);
    sim::unit_health::Unit builder_proj{
        source.unit_index,
        source.record.state_flags,
        source.record.veteran_level,
        source.unit->health,
        &type
    };
    sim::unit_health::Unit target_proj{
        s.unit_index,
        s.record.state_flags,
        s.record.veteran_level,
        nanoframe.health,
        &type,
        s.record.build_remaining,
        nanoframe.events,
        nanoframe.flags
    };
    target_proj.type_index = s.record.type_index;
    HealthHost health(host.match, source.unit_index);
    health.complete_construction(builder_proj, target_proj);
}

float TickHost::ConstructionAdapter::build_decay_rate(int32_t ticks) const {
    return sim::unit_health::build_decay_rate(health_type(source), ticks);
}

uint8_t TickHost::ConstructionAdapter::facing() const {
    return host.match.build_facing(
        static_cast<uint16_t>(record.construction.type_index), record.construction.facing
    );
}

int16_t TickHost::ConstructionAdapter::footprint_x() const {
    const auto index = static_cast<uint16_t>(record.construction.type_index);
    if (index >= host.match.world_.types.size()) {
        host.match.fault_.note("construction type is not loaded");
        return 0;
    }
    const auto& type = host.match.world_.types[index];
    return (facing() & 1U) != 0 ? type.footprint_z : type.footprint_x;
}

int16_t TickHost::ConstructionAdapter::footprint_z() const {
    const auto index = static_cast<uint16_t>(record.construction.type_index);
    if (index >= host.match.world_.types.size()) {
        host.match.fault_.note("construction type is not loaded");
        return 0;
    }
    const auto& type = host.match.world_.types[index];
    return (facing() & 1U) != 0 ? type.footprint_x : type.footprint_z;
}

bool TickHost::ConstructionAdapter::query_build_pad(sim::ground_orders::Point& pad) {
    pad = {
        signed_word(source.unit->position[0]),
        signed_word(source.unit->position[1]),
        signed_word(source.unit->position[2])
    };
    auto* instance = host.match.instance(source.unit_index);
    if (!instance || !instance->script())
        return true;
    std::array<int32_t, 4> args{-1, 0, 0, 0};
    if (!instance->script()->query(ScriptFunction::query_build_info, args) || args[0] < 0)
        return true;
    record.construction.pad_piece = static_cast<int8_t>(args[0]);
    const auto world = host.match.piece_world_position(source, static_cast<uint32_t>(args[0]));
    pad = {signed_word(world[0]), signed_word(world[1]), signed_word(world[2])};
    return true;
}

void TickHost::ConstructionAdapter::attach_child(
    sim::simulation_state::Unit& child, int8_t piece, uint8_t mode
) {
    host.match.set_carry_link(host.slot(child).unit_index, source.unit_index, piece, mode);
}

void TickHost::ConstructionAdapter::set_activation_mask(uint8_t mask, bool enabled) {
    host.match.set_activation(source, mask, enabled);
    // BuildingBuild waits on INBUILDSTANCE (build_flags bit 0). Activate cob
    // (RequestState → Go → OpenYard) sets it; units with no Activate
    // script still need the bit so BuildingBuild can leave phase 1.
    if (mask == 1 && enabled && (source.record.build_flags & 1) == 0) {
        auto* instance = host.match.instance(source.unit_index);
        if (!instance || !instance->script())
            source.record.build_flags = static_cast<uint8_t>(source.record.build_flags | 1u);
    }
}

} // namespace oa::sim::match_runtime

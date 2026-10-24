// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// ComputerHost over an Match. The match API reports rejected orders by
// throwing; this adapter is the boundary that turns those into false.
#include "oa/sim/ai.hpp"

#include "oa/sim/match_runtime.hpp"
#include "oa/data/mission_types.hpp"
#include "oa/data/unit_definitions.hpp"
#include "oa/sim/gameplay_input/order_cursor.hpp"
#include "oa/sim/weapon_execution/retaliation.hpp"

#include <algorithm>
#include <cstring>
#include <exception>
#include <memory>

namespace oa::sim::ai {
namespace {

using sim::match_runtime::Match;

Match& match_of(void* context) noexcept {
    return *static_cast<Match*>(context);
}

sim::ground_orders::Point point_of(const oa::FixedVec3* at) noexcept {
    return {at->x, at->y, at->z};
}

uint32_t host_random(void* context, uint32_t bound) {
    return match_of(context).random_bounded(bound);
}

uint32_t host_squad_size(void* context, uint8_t player, Squad squad) {
    return static_cast<uint32_t>(
        match_of(context).squad_members(player, static_cast<uint32_t>(squad)).size()
    );
}

uint16_t host_squad_member(void* context, uint8_t player, Squad squad, uint32_t i) {
    const auto members = match_of(context).squad_members(player, static_cast<uint32_t>(squad));
    return i < members.size() ? members[i] : 0;
}

void host_set_squad(void* context, uint16_t unit, Squad squad) {
    try {
        match_of(context).set_unit_squad(unit, static_cast<uint32_t>(squad));
    } catch (const std::exception&) {
    }
}

bool host_allied(void* context, uint8_t player, uint8_t other) {
    return match_of(context).allied(player, other);
}

bool host_primary_order(void* context, uint16_t unit, uint8_t* preserve, uint8_t* queue) {
    auto& match = match_of(context);
    const auto* head = match.orders(unit).primary;
    if (head == nullptr)
        return false;
    *preserve = head->preserve_flags;
    *queue = match.primary_order_command_flags(unit);
    return true;
}

/// Tells whether a unit's secondary order queue holds an order.
bool host_secondary_order(void* context, uint16_t unit) {
    return match_of(context).orders(unit).secondary != nullptr;
}

bool host_unit_visible(void* context, uint8_t player, uint16_t unit) {
    try {
        return match_of(context).unit_visible(player, unit);
    } catch (const std::exception&) {
        return false;
    }
}

const uint8_t* host_strengths(void* context, uint8_t player, uint16_t type) {
    const auto& strengths = match_of(context).strategic_state(player).strengths;
    return type < strengths.size() ? strengths[type].data() : nullptr;
}

bool host_site_clear(void* context, uint16_t type, int32_t x, int32_t z) {
    try {
        return match_of(context).site_clear_for(type, x, z, 0, 1);
    } catch (const std::exception&) {
        return false;
    }
}

bool host_building_site(void* context, uint16_t type, int32_t x, int32_t z) {
    try {
        return match_of(context).building_site_clear(type, x, z, 0);
    } catch (const std::exception&) {
        return false;
    }
}

uint8_t host_cell_metal(void* context, int32_t x, int32_t z) {
    const auto& spatial = match_of(context).spatial();
    if (x < 0 || z < 0 || static_cast<uint32_t>(x) >= spatial.terrain_width ||
        static_cast<uint32_t>(z) >= spatial.terrain_height)
        return 0;
    const auto index =
        static_cast<std::size_t>(z) * spatial.terrain_width + static_cast<std::size_t>(x);
    return index < spatial.plots.size() ? spatial.plots[index].metal : 0;
}

bool host_metal_feature(void* context, int32_t x, int32_t z) {
    const auto& spatial = match_of(context).spatial();
    const auto index =
        static_cast<std::size_t>(z) * spatial.terrain_width + static_cast<std::size_t>(x);
    if (index >= spatial.plots.size())
        return false;
    const auto& plot = spatial.plots[index];
    return plot.feature_word < OA_PLOT_FEATURE_RESERVED && plot.metal_feature &&
           plot.indestructible_feature;
}

bool host_order_move(void* context, uint16_t unit, const oa::FixedVec3* to, bool queue) {
    auto& match = match_of(context);
    return !match.order_refused(match.issue_ground_move(unit, point_of(to), queue));
}

bool host_order_patrol(void* context, uint16_t unit, const oa::FixedVec3* to, bool queue) {
    auto& match = match_of(context);
    return !match.order_refused(match.issue_patrol(unit, point_of(to), queue));
}

/// Gives a squad member the attack command on a target, not queued: in place of
/// its orders, as a player's attack command.
bool host_order_attack(void* context, uint16_t unit, uint16_t target) {
    return match_of(context).issue_attack_command(unit, target, false, nullptr);
}

bool host_order_build(void* context, uint16_t unit, uint16_t type, const oa::FixedVec3* at) {
    auto& match = match_of(context);
    return !match.order_refused(match.issue_mobile_build(unit, type, point_of(at), false));
}

bool host_order_factory(void* context, uint16_t factory, uint16_t type, int32_t count) {
    try {
        match_of(context).queue_factory_build(factory, type, count);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

void host_set_active(void* context, uint16_t unit, bool on) {
    auto& match = match_of(context);
    const auto* record = oa::world_unit_at(&match.state(), unit);
    if (record == nullptr || ((record->state_flags & 1) != 0) == on)
        return;
    try {
        match.toggle_activation(unit, 1);
    } catch (const std::exception&) {
    }
}

/// Tells whether a player sees a point, as Match::point_visible does.
bool host_point_visible(void* context, uint8_t player, const oa::FixedVec3* at) {
    try {
        return match_of(context).point_visible(
            player,
            {static_cast<uint32_t>(at->x),
             static_cast<uint32_t>(at->y),
             static_cast<uint32_t>(at->z)}
        );
    } catch (const std::exception&) {
        return false;
    }
}

/// Returns a player's sightings of other players' units; null for a slot outside the player
/// table.
const sim::detection::Sightings* host_sightings(void* context, uint8_t player) {
    if (player >= OA_PLAYER_COUNT)
        return nullptr;
    return &match_of(context).sightings(player);
}

/// Tells whether a unit's first weapon reaches a point from where the unit stands; false for a
/// missing unit.
bool host_weapon_reaches(void* context, uint16_t unit, const oa::FixedVec3* at) {
    const auto& world = match_of(context).state();
    const auto* shooter = oa::world_unit_at(&world, unit);
    return shooter != nullptr && sim::weapon_execution::slot_reaches_point(
                                     world, *shooter, *at, 0, match_of(context).rules_view()
                                 );
}

/// Orders an attack on a point with the order the Attack command resolves to over open
/// ground, not queued.
bool host_order_attack_point(void* context, uint16_t unit, const oa::FixedVec3* at) {
    auto& match = match_of(context);
    const auto& world = match.state();
    const auto* actor = oa::world_unit_at(&world, unit);
    if (actor == nullptr)
        return false;
    const auto order = sim::gameplay_input::unit_order(
        world, sim::gameplay_input::OrderCommand::attack, *actor, nullptr, at, {}
    );
    const auto kind =
        data::mission_types::index_for_name(sim::gameplay_input::unit_order_name(order));
    if (kind == data::mission_types::unknown_mission)
        return false;
    const auto point = point_of(at);
    return !match.order_refused(match.issue_order(unit, kind, false, 0, &point, 0, 0));
}

/// SurfaceMetal seeds every plot's metal before feature overlays replace it;
/// the first plot without a metal feature still holds it.
int32_t surface_metal_of(const Match& match) noexcept {
    for (const auto& plot : match.spatial().plots)
        if (!plot.metal_feature)
            return plot.metal;
    return 0;
}

ComputerHost make_host(Match& match) noexcept {
    ComputerHost host{};
    host.context = &match;
    host.world = &match.state();
    host.difficulty = match.difficulty();
    host.map_cells_x = static_cast<int32_t>(match.spatial().terrain_width);
    host.map_cells_z = static_cast<int32_t>(match.spatial().terrain_height);
    host.random = host_random;
    host.squad_size = host_squad_size;
    host.squad_member = host_squad_member;
    host.set_squad = host_set_squad;
    host.allied = host_allied;
    host.primary_order = host_primary_order;
    host.secondary_order = host_secondary_order;
    host.unit_visible = host_unit_visible;
    host.strengths = host_strengths;
    host.site_clear = host_site_clear;
    host.building_site = host_building_site;
    host.cell_metal = host_cell_metal;
    host.metal_feature = host_metal_feature;
    host.surface_metal = surface_metal_of(match);
    host.order_move = host_order_move;
    host.order_patrol = host_order_patrol;
    host.order_attack = host_order_attack;
    host.order_build = host_order_build;
    host.order_factory = host_order_factory;
    host.set_active = host_set_active;
    host.point_visible = host_point_visible;
    host.sightings = host_sightings;
    host.weapon_reaches = host_weapon_reaches;
    host.order_attack_point = host_order_attack_point;
    return host;
}

void copy_text(char* out, std::size_t capacity, std::string_view text) noexcept {
    const auto n = text.size() < capacity - 1 ? text.size() : capacity - 1;
    std::memcpy(out, text.data(), n);
    out[n] = '\0';
}

/// Type table from the match's unit definitions and canonical type flags.
bool load_types(ComputerPlayers* state, Match& match) noexcept {
    const auto& world = match.state();
    if (!computer_players_reserve_types(state, world.unit_def_count))
        return false;
    for (uint32_t t = 1; t < world.unit_def_count; ++t) {
        auto& type = state->types[t];
        const auto& def = world.unit_defs[t];
        type.flags = def.flags;
        type.abilities = def.abilities;
        type.bm_code = def.bm_code;
        type.footprint_x = def.footprint_x;
        type.footprint_z = def.footprint_z;
        const auto* source = match.unit_definition(static_cast<uint16_t>(t));
        if (source == nullptr)
            continue;
        copy_text(type.unit_name, sizeof type.unit_name, source->unit_name);
        copy_text(type.side, sizeof type.side, source->side);
        std::size_t used = 0;
        for (const auto& category : source->categories) {
            if (used + category.size() + 2 > sizeof type.categories)
                break;
            if (used != 0)
                type.categories[used++] = ' ';
            std::memcpy(type.categories + used, category.data(), category.size());
            used += category.size();
        }
        type.categories[used] = '\0';
        // The FBI's ai_weight, which the unit header holds.
        const auto* weight_end =
            std::find(std::begin(def.ai_weight), std::end(def.ai_weight), '\0');
        copy_text(
            type.ai_directives,
            sizeof type.ai_directives,
            std::string_view(def.ai_weight, static_cast<std::size_t>(weight_end - def.ai_weight))
        );
        type.makes_metal = source->makes_metal;
        type.min_water_depth = source->min_water_depth;
        type.max_water_depth = def.max_water_depth;
        type.energy_use = def.energy_use;
        type.extracts_metal = source->extracts_metal;
    }
    return true;
}

void destroy_players(ComputerPlayers* state) noexcept {
    computer_players_release(state);
    delete state;
}

} // namespace

ComputerHost match_computer_host(Match& match) noexcept {
    return make_host(match);
}

ComputerPlayers* match_computer_players(Match& match) {
    auto& slot = match.computer_player_state();
    if (!slot) {
        auto* state = new ComputerPlayers{};
        slot = std::shared_ptr<void>(state, [](void* p) {
            destroy_players(static_cast<ComputerPlayers*>(p));
        });
    }
    auto* state = static_cast<ComputerPlayers*>(slot.get());
    state->rules = match.rules_view();
    return state;
}

bool configure_match_computer_players(
    Match& match,
    std::string_view profile,
    std::span<const uint16_t> build_lists,
    bool campaign_session
) {
    auto* state = match_computer_players(match);
    state->downloadables_restricted = campaign_session ? 1 : 0;
    return computer_players_configure(state, profile, build_lists);
}

bool reload_match_computer_profiles(Match& match, std::string_view profile) {
    return computer_players_reload_profile(
        match_computer_players(match), make_host(match), profile
    );
}

void hold_capturer_builds(Match& match, uint8_t player, uint32_t tick) {
    auto& slot = match.computer_player_state();
    if (!slot || player >= OA_PLAYER_COUNT)
        return;
    auto& ai = static_cast<ComputerPlayers*>(slot.get())->players[player];
    if (ai.present)
        ai.commander_build_tick = tick;
}

void write_match_computer_report(
    Match& match, uint8_t player, const ComputerReportPaths& paths, std::FILE* out
) {
    computer_write_report(match_computer_players(match), make_host(match), player, paths, out);
}

struct SetupCap {
    ComputerHost inner{};
    ComputerHost host{};
    match_runtime::SetUpRecord records[OA_PLAYER_COUNT]{};
    uint8_t current{255};
    bool capturing{};
};

uint32_t cap_random(void* context, uint32_t bound) {
    auto* cap = static_cast<SetupCap*>(context);
    uint32_t value = 0;
    if (cap->inner.random != nullptr)
        value = cap->inner.random(cap->inner.context, bound);
    if (cap->capturing && cap->current < OA_PLAYER_COUNT) {
        auto& record = cap->records[cap->current];
        if (record.draw_count < 8) {
            record.draws[record.draw_count].bound = bound;
            record.draws[record.draw_count].value = value;
            ++record.draw_count;
        }
    }
    return value;
}

uint32_t cap_squad_size(void* context, uint8_t player, Squad squad) {
    auto* cap = static_cast<SetupCap*>(context);
    if (cap->inner.squad_size == nullptr)
        return 0;
    return cap->inner.squad_size(cap->inner.context, player, squad);
}

uint16_t cap_squad_member(void* context, uint8_t player, Squad squad, uint32_t index) {
    auto* cap = static_cast<SetupCap*>(context);
    if (cap->inner.squad_member == nullptr)
        return 0;
    return cap->inner.squad_member(cap->inner.context, player, squad, index);
}

void cap_set_squad(void* context, uint16_t unit, Squad squad) {
    auto* cap = static_cast<SetupCap*>(context);
    if (cap->inner.set_squad != nullptr)
        cap->inner.set_squad(cap->inner.context, unit, squad);
}

bool cap_allied(void* context, uint8_t player, uint8_t other) {
    auto* cap = static_cast<SetupCap*>(context);
    if (cap->inner.allied == nullptr)
        return false;
    return cap->inner.allied(cap->inner.context, player, other);
}

bool cap_primary(void* context, uint16_t unit, uint8_t* preserve, uint8_t* queue) {
    auto* cap = static_cast<SetupCap*>(context);
    if (cap->inner.primary_order == nullptr)
        return false;
    return cap->inner.primary_order(cap->inner.context, unit, preserve, queue);
}

bool cap_secondary(void* context, uint16_t unit) {
    auto* cap = static_cast<SetupCap*>(context);
    if (cap->inner.secondary_order == nullptr)
        return false;
    return cap->inner.secondary_order(cap->inner.context, unit);
}

bool cap_visible(void* context, uint8_t player, uint16_t unit) {
    auto* cap = static_cast<SetupCap*>(context);
    if (cap->inner.unit_visible == nullptr)
        return false;
    return cap->inner.unit_visible(cap->inner.context, player, unit);
}

const uint8_t* cap_strengths(void* context, uint8_t player, uint16_t type) {
    auto* cap = static_cast<SetupCap*>(context);
    if (cap->inner.strengths == nullptr)
        return nullptr;
    return cap->inner.strengths(cap->inner.context, player, type);
}

bool cap_site_clear(void* context, uint16_t type, int32_t cell_x, int32_t cell_z) {
    auto* cap = static_cast<SetupCap*>(context);
    if (cap->inner.site_clear == nullptr)
        return false;
    return cap->inner.site_clear(cap->inner.context, type, cell_x, cell_z);
}

bool cap_building_site(void* context, uint16_t type, int32_t cell_x, int32_t cell_z) {
    auto* cap = static_cast<SetupCap*>(context);
    if (cap->inner.building_site == nullptr)
        return false;
    return cap->inner.building_site(cap->inner.context, type, cell_x, cell_z);
}

uint8_t cap_cell_metal(void* context, int32_t cell_x, int32_t cell_z) {
    auto* cap = static_cast<SetupCap*>(context);
    if (cap->inner.cell_metal == nullptr)
        return 0;
    return cap->inner.cell_metal(cap->inner.context, cell_x, cell_z);
}

bool cap_metal_feature(void* context, int32_t cell_x, int32_t cell_z) {
    auto* cap = static_cast<SetupCap*>(context);
    if (cap->inner.metal_feature == nullptr)
        return false;
    return cap->inner.metal_feature(cap->inner.context, cell_x, cell_z);
}

bool cap_order_move(void* context, uint16_t unit, const oa::FixedVec3* to, bool queue) {
    auto* cap = static_cast<SetupCap*>(context);
    if (cap->inner.order_move == nullptr)
        return false;
    return cap->inner.order_move(cap->inner.context, unit, to, queue);
}

bool cap_order_patrol(void* context, uint16_t unit, const oa::FixedVec3* to, bool queue) {
    auto* cap = static_cast<SetupCap*>(context);
    if (cap->inner.order_patrol == nullptr)
        return false;
    return cap->inner.order_patrol(cap->inner.context, unit, to, queue);
}

bool cap_order_attack(void* context, uint16_t unit, uint16_t target) {
    auto* cap = static_cast<SetupCap*>(context);
    if (cap->inner.order_attack == nullptr)
        return false;
    return cap->inner.order_attack(cap->inner.context, unit, target);
}

bool cap_order_build(void* context, uint16_t unit, uint16_t type, const oa::FixedVec3* at) {
    auto* cap = static_cast<SetupCap*>(context);
    if (cap->inner.order_build == nullptr)
        return false;
    return cap->inner.order_build(cap->inner.context, unit, type, at);
}

bool cap_order_factory(void* context, uint16_t factory, uint16_t type, int32_t count) {
    auto* cap = static_cast<SetupCap*>(context);
    if (cap->inner.order_factory == nullptr)
        return false;
    return cap->inner.order_factory(cap->inner.context, factory, type, count);
}

void cap_set_active(void* context, uint16_t unit, bool on) {
    auto* cap = static_cast<SetupCap*>(context);
    if (cap->inner.set_active != nullptr)
        cap->inner.set_active(cap->inner.context, unit, on);
}

bool cap_point_visible(void* context, uint8_t player, const oa::FixedVec3* at) {
    auto* cap = static_cast<SetupCap*>(context);
    if (cap->inner.point_visible == nullptr)
        return false;
    return cap->inner.point_visible(cap->inner.context, player, at);
}

const sim::detection::Sightings* cap_sightings(void* context, uint8_t player) {
    auto* cap = static_cast<SetupCap*>(context);
    if (cap->inner.sightings == nullptr)
        return nullptr;
    return cap->inner.sightings(cap->inner.context, player);
}

bool cap_weapon_reaches(void* context, uint16_t unit, const oa::FixedVec3* at) {
    auto* cap = static_cast<SetupCap*>(context);
    if (cap->inner.weapon_reaches == nullptr)
        return false;
    return cap->inner.weapon_reaches(cap->inner.context, unit, at);
}

bool cap_attack_point(void* context, uint16_t unit, const oa::FixedVec3* at) {
    auto* cap = static_cast<SetupCap*>(context);
    if (cap->inner.order_attack_point == nullptr)
        return false;
    return cap->inner.order_attack_point(cap->inner.context, unit, at);
}

void cap_note_section(void* context, uint8_t player, uint8_t section) {
    auto* cap = static_cast<SetupCap*>(context);
    if (section != section_start || player >= OA_PLAYER_COUNT)
        return;
    cap->current = player;
    cap->capturing = true;
    cap->records[player].draw_count = 0;
    cap->records[player].stream_state = 0;
    cap->records[player].handed_over = false;
    if (cap->inner.context != nullptr)
        cap->records[player].stream_state = static_cast<Match*>(cap->inner.context)->random_state();
}

void arm_setup(SetupCap& cap) noexcept {
    cap.host.context = &cap;
    cap.host.world = cap.inner.world;
    cap.host.difficulty = cap.inner.difficulty;
    cap.host.map_cells_x = cap.inner.map_cells_x;
    cap.host.map_cells_z = cap.inner.map_cells_z;
    cap.host.surface_metal = cap.inner.surface_metal;
    cap.host.random = cap_random;
    cap.host.squad_size = cap_squad_size;
    cap.host.squad_member = cap_squad_member;
    cap.host.set_squad = cap_set_squad;
    cap.host.allied = cap_allied;
    cap.host.primary_order = cap_primary;
    cap.host.secondary_order = cap_secondary;
    cap.host.unit_visible = cap_visible;
    cap.host.strengths = cap_strengths;
    cap.host.site_clear = cap_site_clear;
    cap.host.building_site = cap_building_site;
    cap.host.cell_metal = cap_cell_metal;
    cap.host.metal_feature = cap_metal_feature;
    cap.host.order_move = cap_order_move;
    cap.host.order_patrol = cap_order_patrol;
    cap.host.order_attack = cap_order_attack;
    cap.host.order_build = cap_order_build;
    cap.host.order_factory = cap_order_factory;
    cap.host.set_active = cap_set_active;
    cap.host.point_visible = cap_point_visible;
    cap.host.sightings = cap_sightings;
    cap.host.weapon_reaches = cap_weapon_reaches;
    cap.host.order_attack_point = cap_attack_point;
    cap.host.note_section = cap_note_section;
}

bool any_setup_hook(const Match& match) noexcept {
    for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player) {
        if (match.seat_hooks(player).set_up != nullptr)
            return true;
    }
    return false;
}

void prepare_match_computer_players(Match& match) {
    auto* state = match_computer_players(match);
    if (state->initialized || !load_types(state, match))
        return;
    state->build_lists = match.limits().build_lists;
    for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player) {
        const auto& record = match.state().game.players[player];
        if (record.in_use == 0 || record.status != OA_PLAYER_STATUS_COMPUTER)
            continue;
        try {
            match.refresh_strategic_state(player);
        } catch (const std::exception&) {
        }
    }
    if (!any_setup_hook(match)) {
        (void)computer_players_initialize(state, make_host(match));
        return;
    }
    SetupCap cap;
    cap.inner = make_host(match);
    arm_setup(cap);
    (void)computer_players_initialize(state, cap.host);
    for (uint8_t player = 0; player < OA_PLAYER_COUNT; ++player) {
        const match_runtime::SeatHooks hooks = match.seat_hooks(player);
        if (hooks.set_up != nullptr)
            hooks.set_up(hooks.context, match, player, cap.records[player]);
    }
}

void tick_match_computer_orders(Match& match, uint8_t player) {
    const ComputerHost host = make_host(match);
    // +AI can hand a human to the computer after the start. That player has
    // no controller until this tick builds one.
    computer_players_adopt(match_computer_players(match), host, player);
    computer_player_tick_orders(match_computer_players(match), host, player);
}

} // namespace oa::sim::ai

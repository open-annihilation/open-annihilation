// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The computer player's fake match, shared with the shadow tests.
#pragma once

#include "oa/sim/ai.hpp"

#include <cstdio>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace oa::test::computer_fake {

using namespace oa::sim::ai;

/// Failures from the shared catalog. A test adds this to its own count.
inline int fixture_failures = 0;

enum Type : uint16_t {
    ARMCOM = 1,
    ARMMEX,
    ARMSOLAR,
    ARMLAB,
    ARMPW,
    ARMCK,
    ARMFIG,
    ARMPT,
    ARMLLT,
    CORMEX,
    TYPE_COUNT
};

struct Order {
    std::string kind;
    uint16_t unit{};
    uint16_t arg{};
    oa::FixedVec3 at;
    bool queue{};
};

// Fake match: a World, squads and a scripted random stream.
struct Fake {
    oa::World world{};
    std::vector<oa::Unit> units = std::vector<oa::Unit>(64);
    std::vector<oa::UnitDef> defs = std::vector<oa::UnitDef>(TYPE_COUNT);
    std::map<std::pair<int, int>, std::vector<uint16_t>> squads;
    std::vector<Order> orders;
    std::vector<uint32_t> rolls;
    std::size_t roll = 0;
    uint8_t strength[TYPE_COUNT][3]{};
    bool allies[10][10]{};
    bool holds_order[64]{};        // the unit has a primary order
    bool holds_secondary[64]{};    // the unit has a secondary order
    uint32_t knowledge_tick[10]{}; // the match's Sightings.refreshed_tick

    /// Fills the world from the owned unit and type tables.
    Fake() {
        world.units = units.data();
        world.unit_slot_count = static_cast<uint32_t>(units.size());
        world.unit_defs = defs.data();
        world.unit_def_count = TYPE_COUNT;
        for (int i = 0; i < 10; ++i) {
            allies[i][i] = true;
            world.game.players[i].index = static_cast<uint8_t>(i);
        }
        for (auto& row : strength)
            row[0] = row[1] = row[2] = 50;
    }

    /// Marks one player in use, with a unit range and starting stores.
    void player(int index, uint8_t status, uint32_t first, uint32_t last) {
        auto& p = world.game.players[index];
        p.in_use = 1;
        p.status = status;
        p.first_unit = oa::oa_ref_from_index(first);
        p.last_unit = oa::oa_ref_from_index(last);
        p.energy = 1000.0F;
        p.metal = 500.0F;
        p.energy_storage = 1000.0F;
        p.metal_storage = 500.0F;
        p.energy_produced = 10.0F;
        p.metal_produced = 5.0F;
    }

    /// Places one live selectable unit and returns it.
    oa::Unit& spawn(uint32_t slot, int owner, uint16_t type, int32_t x, int32_t z, uint32_t flags) {
        auto& u = units[slot];
        u = {};
        u.type_index = type;
        u.owner_index = static_cast<uint8_t>(owner);
        u.owner = oa::oa_ref_from_index(static_cast<uint32_t>(owner));
        u.position = {x << 16, 0, z << 16};
        u.flags = OA_UNIT_FLAG_LIVE | OA_UNIT_FLAG_SELECTABLE | flags;
        return u;
    }

    /// Moves a unit between squad lists the way the match swaps and pushes.
    void set_squad(uint16_t slot, int squad) {
        auto& u = units[slot];
        if (u.squad != -1)
            if (u.squad != 0 || true) {
                auto& old = squads[{u.owner_index, u.squad}];
                for (auto& member : old)
                    if (member == slot) {
                        member = old.back();
                        old.pop_back();
                        break;
                    }
            }
        if (squad != -1)
            squads[{u.owner_index, squad}].push_back(slot);
        u.squad = squad;
    }
};

/// Returns the fake a host context points at.
inline Fake* fake_of(void* context) {
    return static_cast<Fake*>(context);
}

/// Fills a host whose calls read and write `fake`.
inline ComputerHost host_for(Fake& fake) {
    ComputerHost host{};
    host.context = &fake;
    host.world = &fake.world;
    host.difficulty = OA_DIFFICULTY_EASY;
    host.map_cells_x = 64;
    host.map_cells_z = 64;
    host.random = [](void* c, uint32_t bound) -> uint32_t {
        auto* f = fake_of(c);
        if (bound < 2)
            return 0;
        const auto value = f->roll < f->rolls.size() ? f->rolls[f->roll] : 0u;
        ++f->roll;
        return value % bound;
    };
    host.squad_size = [](void* c, uint8_t p, Squad s) -> uint32_t {
        return static_cast<uint32_t>(fake_of(c)->squads[{p, static_cast<int>(s)}].size());
    };
    host.squad_member = [](void* c, uint8_t p, Squad s, uint32_t i) -> uint16_t {
        return fake_of(c)->squads[{p, static_cast<int>(s)}].at(i);
    };
    host.set_squad = [](void* c, uint16_t u, Squad s) {
        fake_of(c)->set_squad(u, static_cast<int>(s));
    };
    host.allied = [](void* c, uint8_t p, uint8_t o) { return fake_of(c)->allies[p][o]; };
    host.primary_order = [](void* c, uint16_t u, uint8_t*, uint8_t*) {
        return fake_of(c)->holds_order[u];
    };
    host.secondary_order = [](void* c, uint16_t u) { return fake_of(c)->holds_secondary[u]; };
    host.unit_visible = [](void*, uint8_t, uint16_t) { return true; };
    host.strengths = [](void* c, uint8_t, uint16_t t) -> const uint8_t* {
        return t < TYPE_COUNT ? fake_of(c)->strength[t] : nullptr;
    };
    host.site_clear = [](void*, uint16_t, int32_t x, int32_t z) {
        return x >= 0 && z >= 0 && x < 60 && z < 60;
    };
    host.building_site = host.site_clear;
    host.cell_metal = [](void*, int32_t, int32_t) -> uint8_t { return 0; };
    host.metal_feature = [](void*, int32_t, int32_t) { return false; };
    host.order_move = [](void* c, uint16_t u, const oa::FixedVec3* at, bool q) {
        fake_of(c)->orders.push_back({"move", u, 0, *at, q});
        return true;
    };
    host.order_patrol = [](void* c, uint16_t u, const oa::FixedVec3* at, bool q) {
        fake_of(c)->orders.push_back({"patrol", u, 0, *at, q});
        return true;
    };
    host.order_attack = [](void* c, uint16_t u, uint16_t t) {
        fake_of(c)->orders.push_back({"attack", u, t, {}, false});
        return true;
    };
    host.order_build = [](void* c, uint16_t u, uint16_t t, const oa::FixedVec3* at) {
        fake_of(c)->orders.push_back({"build", u, t, *at, false});
        return true;
    };
    host.order_factory = [](void* c, uint16_t u, uint16_t t, int32_t) {
        fake_of(c)->orders.push_back({"factory", u, t, {}, false});
        return true;
    };
    host.set_active = [](void* c, uint16_t u, bool on) {
        fake_of(c)->orders.push_back({on ? "on" : "off", u, 0, {}, false});
    };
    return host;
}

/// Names one type in the computer player's catalog.
inline void describe(
    ComputerPlayers& state,
    uint16_t id,
    const char* name,
    const char* side,
    const char* categories,
    uint32_t flags,
    int8_t bm_code
) {
    auto& t = state.types[id];
    std::snprintf(t.unit_name, sizeof t.unit_name, "%s", name);
    std::snprintf(t.side, sizeof t.side, "%s", side);
    std::snprintf(t.categories, sizeof t.categories, "%s", categories);
    t.flags = flags;
    t.bm_code = bm_code;
    t.min_water_depth = -10000;
    t.footprint_x = t.footprint_z = 2;
}

/// Fills the catalog the computer-player tests share.
inline void describe_catalog(ComputerPlayers& state) {
    if (!computer_players_reserve_types(&state, TYPE_COUNT)) {
        std::fprintf(stderr, "FAIL: reserve types\n");
        ++fixture_failures;
    }
    describe(state, ARMCOM, "ARMCOM", "ARM", "ARM COMMANDER", OA_UNIT_DEF_FLAG_BUILDER, 1);
    state.types[ARMCOM].abilities = OA_UNIT_DEF_ABILITY_CAN_CAPTURE;
    describe(state, ARMMEX, "ARMMEX", "ARM", "ARM LEVEL1", 0, 0);
    describe(state, ARMSOLAR, "ARMSOLAR", "ARM", "ARM LEVEL1", 0, 0);
    describe(state, ARMLAB, "ARMLAB", "ARM", "ARM PLANT LEVEL1", OA_UNIT_DEF_FLAG_BUILDER, 0);
    describe(state, ARMPW, "ARMPW", "ARM", "ARM KBOT LEVEL1", 0, 1);
    describe(state, ARMCK, "ARMCK", "ARM", "ARM CONSTR LEVEL1", OA_UNIT_DEF_FLAG_BUILDER, 1);
    describe(state, ARMFIG, "ARMFIG", "ARM", "ARM VTOL", OA_UNIT_DEF_FLAG_CAN_FLY, 1);
    describe(state, ARMPT, "ARMPT", "ARM", "ARM SHIP", 0, 1);
    state.types[ARMPT].min_water_depth = 12;
    describe(state, ARMLLT, "ARMLLT", "ARM", "ARM LEVEL1", 0, 0);
    describe(state, CORMEX, "CORMEX", "CORE", "CORE LEVEL1", 0, 0);
}

// The unit types' build lists (UnitDef.build_ids), from type 0 on: a count, then that
// many ids. ARMCOM's holds an id outside the table, which the computer players skip.
constexpr uint16_t kBuildLists[] = {
    0, // type 0
    5,
    ARMSOLAR,
    ARMMEX,
    ARMLAB,
    TYPE_COUNT + 3,
    CORMEX, // ARMCOM
    0,
    0, // ARMMEX, ARMSOLAR
    2,
    ARMPW,
    ARMCK, // ARMLAB
    0,
    0,
    0,
    0,
    0,
    0, // ARMPW to CORMEX
};

} // namespace oa::test::computer_fake

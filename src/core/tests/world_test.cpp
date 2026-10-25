// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Compiled as C++20: World helpers in namespace oa, including const overloads.
#include "oa/core/world.h"

#include <cstdio>
#include <memory>
#include <type_traits>

static_assert(std::is_standard_layout_v<oa::World>);
static_assert(
    std::is_same_v<decltype(oa::world_unit(std::declval<const oa::World*>(), 1u)), const oa::Unit*>
);
static_assert(std::is_same_v<decltype(oa::world_unit(std::declval<oa::World*>(), 1u)), oa::Unit*>);

int main() {
    int failures = 0;
    const auto check = [&](bool condition, const char* what) {
        if (!condition) {
            std::fprintf(stderr, "world: %s\n", what);
            ++failures;
        }
    };
    auto world = std::make_unique<oa::World>();
    oa::Unit units[11]{};
    oa::UnitDef defs[3]{};
    world->units = units;
    world->unit_slot_count = 11;
    world->unit_defs = defs;
    world->unit_def_count = 3;

    units[10].def = oa::oa_ref_from_index(2);
    units[10].owner = oa::world_player_ref_of(world.get(), &world->game.players[0]);
    const oa::World* view = world.get();
    check(oa::world_unit_def_of(view, &units[10]) == &defs[2], "const unit def");
    check(oa::world_unit_owner(view, &units[10]) == &world->game.players[0], "const owner");
    check(oa::world_unit(view, oa::world_unit_ref(view, &units[10])) == &units[10], "round trip");
    check(oa::world_unit_at(view, 11) == nullptr, "const slot bound");

    world->game.weapon_defs[4].flags = OA_WEAPON_FLAG_STOCKPILE;
    check(oa::world_weapon_def(view, 5)->flags == OA_WEAPON_FLAG_STOCKPILE, "weapon def index");
    check(oa::world_weapon_def(view, 1) == &world->game.weapon_defs[0], "const first weapon");
    check(oa::world_weapon_def(view, 256) == &world->game.weapon_defs[255], "const last weapon");
    check(
        oa::world_weapon_def(view, 257) == nullptr && oa::world_weapon_def(view, 0) == nullptr,
        "const weapon bounds"
    );
    check(oa::world_weapon_def_count(view) == OA_WEAPON_DEF_COUNT, "const weapon count");
    check(oa::world_weapon_ref(view, &world->game.weapon_defs[0]) == 1, "const first weapon ref");
    check(oa::world_weapon_ref(view, nullptr) == 0, "const null weapon ref");
    {
        oa::WeaponDef outside{};
        check(oa::world_weapon_ref(view, &outside) == 0, "const weapon outside both tables");
    }
    {
        auto extra = std::make_unique<oa::WeaponDef[]>(745);
        world->extra_weapon_defs = extra.get();
        world->extra_weapon_def_count = 745;
        check(oa::world_weapon_def(view, 257) == &extra[0], "const first extra weapon");
        check(oa::world_weapon_def(view, 1001) == &extra[744], "const last extra weapon");
        check(oa::world_weapon_def(view, 1002) == nullptr, "const past the extra table");
        check(oa::world_weapon_ref(view, &extra[0]) == 257, "const first extra ref");
        check(oa::world_weapon_ref(view, &extra[744]) == 1001, "const last extra ref");
        check(
            oa::world_weapon_def(view, oa::world_weapon_ref(view, &extra[10])) == &extra[10],
            "const extra round trip"
        );
        check(oa::world_weapon_def_count(view) == 1001, "const weapon count with extras");
        world->extra_weapon_defs = nullptr;
        world->extra_weapon_def_count = 0;
    }
    {
        oa::WeaponDef numbered{};
        oa::oa_weapon_def_set_id(&numbered, 300);
        check(
            numbered.weapon_id == 44 && numbered.weapon_id_high == 1 &&
                oa::oa_weapon_def_id(&numbered) == 300,
            "weapon id bytes"
        );
    }
    {
        oa::WorldCapacity capacity{11, 3, 2};
        auto owned =
            std::unique_ptr<oa::World, void (*)(oa::World*)>(oa::world_create(), oa::world_destroy);
        check(owned != nullptr, "created world");
        if (owned != nullptr) {
            check(
                oa::world_alloc_tables(owned.get(), &capacity) &&
                    owned->extra_weapon_defs == nullptr && owned->extra_weapon_def_count == 0,
                "no extra weapons"
            );
            capacity.extra_weapon_defs = 745;
            check(
                oa::world_alloc_tables(owned.get(), &capacity) &&
                    owned->extra_weapon_def_count == 745 && owned->extra_weapon_defs != nullptr &&
                    owned->extra_weapon_defs[0].weapon_id == 0 &&
                    owned->extra_weapon_defs[744].weapon_id_high == 0,
                "allocated extra weapons"
            );
            oa::world_free_tables(owned.get());
            check(
                owned->extra_weapon_defs == nullptr && owned->extra_weapon_def_count == 0,
                "freed extra weapons"
            );
            static oa::Side kept[1];
            owned->extra_sides = kept;
            owned->extra_side_count = 1;
            oa::world_free_tables(owned.get());
            check(
                owned->extra_sides == kept && owned->extra_side_count == 1,
                "free leaves bound sides"
            );
        }
    }
    {
        oa::World plain{};
        const oa::World* sides = &plain;
        check(oa::world_side_count(sides) == 0, "zeroed side count");
        check(oa::world_side(sides, 0) == &plain.game.sides[0], "const side 0");
        check(oa::world_side(sides, 4) == &plain.game.sides[4], "const side 4");
        check(oa::world_side(sides, 5) == nullptr, "const side 5 with no table");
        plain.game.side_count = 3;
        check(oa::world_side(sides, 4) == &plain.game.sides[4], "const side 4 past side_count");
        plain.game.side_count = 7;
        auto extra = std::make_unique<oa::Side[]>(2);
        plain.extra_sides = extra.get();
        plain.extra_side_count = 2;
        check(oa::world_side(&plain, 5) == &extra[0], "first extra side");
        check(oa::world_side(sides, 6) == &extra[1], "const last loaded side");
        check(oa::world_side(sides, 7) == nullptr, "const past the loaded sides");
        plain.extra_side_count = 1;
        check(oa::world_side(sides, 6) == nullptr, "const past the extra table");
        plain.extra_sides = nullptr;
        check(oa::world_side(sides, 5) == nullptr, "const side 5 after unbind");
        check(oa::world_side(sides, 3) == &plain.game.sides[3], "const side under five");
    }
    return failures == 0 ? 0 : 1;
}

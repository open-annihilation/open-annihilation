// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

/* Compiled as C11: World reference helpers. */
#include "oa/core/world.h"

#include <stdio.h>
#include <string.h>

static int failures;

static void check(int condition, const char* what) {
    if (!condition) {
        fprintf(stderr, "world: %s\n", what);
        ++failures;
    }
}

int main(void) {
    static World world;
    static Unit units[21];
    static UnitDef defs[4];
    static FeatureDef features[2];
    static Projectile projectiles[OA_PROJECTILE_CAPACITY];
    uint32_t count = 0;
    Unit* first;

    memset(&world, 0, sizeof world);
    world.units = units;
    world.unit_slot_count = 21;
    world.unit_defs = defs;
    world.unit_def_count = 4;
    world.feature_defs = features;
    world.feature_def_count = 2;
    world.projectiles = projectiles;

    check(oa_ref_from_index(0) == 1, "index 0 is ref 1");
    check(oa_unit_ref_from_slot(0) == 0 && oa_unit_slot_from_ref(0) == 0, "unit slot 0 is null");
    check(oa_unit_ref_from_slot(7) == 8 && oa_unit_slot_from_ref(8) == 7, "unit slot round trip");
    check(world_unit(&world, 0) == NULL, "null unit ref");
    check(world_unit(&world, 1) == &units[0], "unit ref 1 is slot 0");
    check(world_unit(&world, 22) == NULL, "unit ref past the pool");
    check(
        world_unit_at(&world, 20) == &units[20] && world_unit_at(&world, 21) == NULL,
        "unit slot bounds"
    );
    check(
        world_unit_ref(&world, &units[5]) == 6 && world_unit_ref(&world, NULL) == 0, "unit to ref"
    );
    check(world_unit_slot(&world, &units[5]) == 5, "unit to slot");

    units[3].def = 3;
    check(world_unit_def_of(&world, &units[3]) == &defs[2], "unit def ref");
    check(
        world_unit_def(&world, 5) == NULL && world_unit_def(&world, 0) == NULL, "unit def bounds"
    );
    check(world_unit_def_ref(&world, &defs[1]) == 2, "unit def to ref");

    check(
        world_player(&world, 9) == &world.game.players[9] && world_player(&world, 10) == NULL,
        "player bounds"
    );
    check(
        world_player_record(&world, 9) == &world.game.players[9] &&
            world_player_record(&world, 10) == &world.game.no_player &&
            world_player_record(&world, 11) == NULL,
        "player record bounds"
    );
    units[3].owner = world_player_ref_of(&world, &world.game.players[4]);
    check(
        units[3].owner == 5 && world_unit_owner(&world, &units[3]) == &world.game.players[4],
        "unit owner"
    );

    world.game.players[1].first_unit = world_unit_ref(&world, &units[3]);
    world.game.players[1].last_unit = world_unit_ref(&world, &units[4]);
    first = world_player_units(&world, &world.game.players[1], &count);
    check(first == &units[3] && count == 2, "inclusive player range");
    first = world_player_units(&world, &world.game.players[2], &count);
    check(first == NULL && count == 0, "empty player range");

    check(world_weapon_def(&world, 1) == &world.game.weapon_defs[0], "first weapon def");
    check(world_weapon_def(&world, 256) == &world.game.weapon_defs[255], "last weapon def");
    check(
        world_weapon_def(&world, 257) == NULL && world_weapon_def(&world, 0) == NULL,
        "weapon def bounds"
    );
    check(world_weapon_def_count(&world) == OA_WEAPON_DEF_COUNT, "weapon count without extras");
    check(world_weapon_ref(&world, &world.game.weapon_defs[0]) == 1, "first weapon to ref");
    check(world_weapon_ref(&world, &world.game.weapon_defs[255]) == 256, "last weapon to ref");
    check(world_weapon_ref(&world, NULL) == 0, "null weapon to ref");
    {
        WeaponDef outside;
        memset(&outside, 0, sizeof outside);
        check(world_weapon_ref(&world, &outside) == 0, "weapon outside both tables");
    }
    {
        static WeaponDef extra[745];
        memset(extra, 0, sizeof extra);
        world.extra_weapon_defs = extra;
        world.extra_weapon_def_count = 745;
        check(world_weapon_def(&world, 257) == &extra[0], "first extra weapon");
        check(world_weapon_def(&world, 1001) == &extra[744], "last extra weapon");
        check(world_weapon_def(&world, 1002) == NULL, "past the extra table");
        check(world_weapon_ref(&world, &extra[0]) == 257, "first extra to ref");
        check(world_weapon_ref(&world, &extra[744]) == 1001, "last extra to ref");
        check(
            world_weapon_def(&world, world_weapon_ref(&world, &extra[10])) == &extra[10],
            "extra weapon round trip"
        );
        check(world_weapon_def_count(&world) == 1001, "weapon count with extras");
        world.extra_weapon_defs = NULL;
        world.extra_weapon_def_count = 0;
    }
    {
        WeaponDef numbered;
        memset(&numbered, 0, sizeof numbered);
        oa_weapon_def_set_id(&numbered, 300);
        check(
            numbered.weapon_id == 44 && numbered.weapon_id_high == 1 &&
                oa_weapon_def_id(&numbered) == 300,
            "weapon id bytes"
        );
    }
    check(
        world_feature_def(&world, 2) == &features[1] && world_feature_def(&world, 3) == NULL,
        "feature def bounds"
    );
    check(
        world_projectile(&world, OA_PROJECTILE_CAPACITY) ==
            &projectiles[OA_PROJECTILE_CAPACITY - 1],
        "projectile bounds"
    );

    world.game.players[2].economy = 3;
    check(
        world_player_economy(&world, &world.game.players[2]) == &world.player_economy[2],
        "player economy block"
    );
    check(world_player_economy(&world, &world.game.players[3]) == NULL, "absent economy block");

    world.game.players[2].info = 3;
    check(
        world_player_info(&world, &world.game.players[2]) == &world.player_info[2],
        "player info block"
    );
    check(world_player_info(&world, &world.game.players[3]) == NULL, "absent player info");
    world.game.no_player.info = 11;
    check(
        world_player_info(&world, &world.game.no_player) == &world.player_info[10],
        "eleventh player info block"
    );

    {
        static MapPlot plots[6];
        world.game.map_width = 3;
        world.game.map_height = 2;
        check(world_plot(&world, 0, 0) == NULL, "plots not loaded");
        world.plots = plots;
        check(world_plot(&world, 2, 1) == &plots[5], "plot index");
        check(world_plot(&world, 3, 0) == NULL && world_plot(&world, 0, -1) == NULL, "plot bounds");
    }

    {
        World* owned = world_create();
        WorldCapacity capacity = {11, 3, 2, 0};
        check(owned != NULL && owned->units == NULL, "created world has no tables");
        if (owned != NULL) {
            check(
                world_alloc_tables(owned, &capacity) && owned->unit_slot_count == 11 &&
                    owned->units != NULL && owned->projectiles != NULL &&
                    owned->extra_weapon_defs == NULL && owned->extra_weapon_def_count == 0,
                "allocated tables"
            );
            world_free_tables(owned);
            capacity.extra_weapon_defs = 745;
            check(
                world_alloc_tables(owned, &capacity) && owned->extra_weapon_def_count == 745 &&
                    owned->extra_weapon_defs != NULL &&
                    owned->extra_weapon_defs[0].weapon_id == 0 &&
                    owned->extra_weapon_defs[744].weapon_id_high == 0,
                "allocated extra weapons"
            );
            world_free_tables(owned);
            check(
                owned->units == NULL && owned->unit_slot_count == 0 &&
                    owned->extra_weapon_defs == NULL && owned->extra_weapon_def_count == 0,
                "freed tables"
            );
            world_destroy(owned);
        }
    }

    {
        static Side extra_sides[2];
        memset(extra_sides, 0, sizeof extra_sides);
        extra_sides[0].side_index = 5;
        extra_sides[1].side_index = 6;
        check(world_side_count(&world) == 0, "zeroed world side count");
        check(world_side(&world, 0) == &world.game.sides[0], "side 0 of an empty count");
        check(world_side(&world, 4) == &world.game.sides[4], "side 4 of an empty count");
        check(world_side(&world, 5) == NULL, "side 5 with no table");
        world.game.side_count = 3;
        check(world_side(&world, 2) == &world.game.sides[2], "side under five past the count");
        check(world_side(&world, 4) == &world.game.sides[4], "side 4 past side_count");
        check(world_side(&world, 5) == NULL, "side 5 while the count is under five");
        world.game.side_count = 7;
        world.extra_sides = extra_sides;
        world.extra_side_count = 2;
        check(world_side(&world, 5) == &extra_sides[0], "first extra side");
        check(world_side(&world, 6) == &extra_sides[1], "last loaded side");
        check(world_side(&world, 7) == NULL, "past the loaded sides");
        world.extra_side_count = 1;
        check(world_side(&world, 6) == NULL, "past the extra table");
        world.extra_sides = NULL;
        check(world_side(&world, 5) == NULL, "side 5 after the table is cleared");
        check(world_side(&world, 0) == &world.game.sides[0], "side 0 still inline");
        {
            World* owned = world_create();
            if (owned != NULL) {
                owned->extra_sides = extra_sides;
                owned->extra_side_count = 2;
                world_free_tables(owned);
                check(
                    owned->extra_sides == extra_sides && owned->extra_side_count == 2,
                    "free leaves bound sides"
                );
                owned->extra_sides = NULL;
                world_destroy(owned);
            }
        }
    }
    return failures == 0 ? 0 : 1;
}

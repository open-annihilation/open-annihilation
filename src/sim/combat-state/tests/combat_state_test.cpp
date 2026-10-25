// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/combat_state.hpp"
#include <cstring>
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <vector>
#include <string_view>
#define CHECK(x)                                                                                   \
    do {                                                                                           \
        if (!(x))                                                                                  \
            throw std::runtime_error(#x);                                                          \
    } while (false)
using namespace oa::sim::combat_state;

struct Geometry final : SpawnGeometryHost {
    std::vector<uint8_t> query, aim;
    int32_t maximum{-1};

    std::array<int32_t, 3> query_weapon_world(uint8_t slot) override {
        query.push_back(slot);
        return {0, 0, 10 + slot};
    }

    std::array<int32_t, 3> aim_from_world(uint8_t slot) override {
        aim.push_back(slot);
        return {0, 0, 2};
    }

    void set_max_reload_time(int32_t value) override { maximum = value; }
};

// A host whose AimFrom callback clears the slot's definition.
struct ClearingGeometry final : SpawnGeometryHost {
    UnitWeapons* weapons{};
    std::vector<uint8_t> query;
    int32_t maximum{-1};

    std::array<int32_t, 3> query_weapon_world(uint8_t slot) override {
        query.push_back(slot);
        return {};
    }

    std::array<int32_t, 3> aim_from_world(uint8_t slot) override {
        weapons->slots[slot].definition = nullptr;
        return {};
    }

    void set_max_reload_time(int32_t value) override { maximum = value; }
};

struct TargetFixture final : TargetSearchHost {
    std::vector<TargetUnit> nearby;
    std::vector<uint32_t> random_values;
    size_t random_index{};
    bool reachable{true}, override_flag{};
    int32_t radius{123};

    int32_t search_radius(const TargetSource&, const TargetSearchRequest&) override {
        return radius;
    }

    std::span<const TargetUnit>
    gather_nearby(uint8_t, const std::array<uint32_t, 3>&, int32_t) override {
        return nearby;
    }

    uint32_t random_bounded(uint32_t limit) override {
        const auto value = random_index < random_values.size() ? random_values[random_index++] : 0U;
        return limit == 0 ? 0 : value % limit;
    }

    bool weapon_can_reach(const TargetSource&, const TargetUnit&, uint8_t) override {
        return reachable;
    }

    bool global_target_override() const noexcept override { return override_flag; }
};

struct AttackFixture final : AttackHost {
    bool valid_attack{true}, valid_move{true}, commit{true};
    std::vector<AttackOrderRequest> requests;

    uint8_t resolve_order(
        uint8_t kind, const AttackSource&, const TargetUnit*, const std::array<uint32_t, 3>*
    ) override {
        return (kind == attack_order_kind ? valid_attack : valid_move)
                   ? static_cast<uint8_t>(kind + 10)
                   : 0;
    }

    bool commit_orders(const AttackSource&, std::span<const AttackOrderRequest> orders) override {
        requests.assign(orders.begin(), orders.end());
        return commit;
    }
};

struct IntelligenceFixture final : IntelligenceHost {
    std::vector<TargetUnit> units;

    bool unit_active(UnitIdentity id) override { return id != 99; }

    const TargetUnit* resolve(UnitIdentity id) override {
        for (auto& u : units)
            if (u.identity == id)
                return &u;
        return nullptr;
    }
};

void wide_weapon_registry_follows_the_table() {
    WeaponRegistry registry;
    CHECK(registry.capacity() == 256);
    CHECK(registry.records().size() == 256);
    CHECK(install_weapon_text(registry, "[W300]{id=300;}[W1000]{id=1000;}", false, 1024) == 2);
    CHECK(registry.capacity() == 1001);
    CHECK(registry.find("W1000")->registry_index == 1000);
    CHECK(std::strcmp(registry.records()[300].key, "W300") == 0);
    CHECK(oa::oa_weapon_def_id(&registry.records()[300]) == 300);
    CHECK(&registry.definition(1000) == registry.find("W1000"));
    registry.install_damage_override(1000, "ARMCOM", 12);
    CHECK(damage_against(registry.definition(1000), "ARMCOM") == 12);
    WeaponRegistry copy(registry);
    CHECK(copy.capacity() == 1001);
    CHECK(std::strcmp(copy.records()[300].key, "W300") == 0);
    WeaponRegistry assigned;
    assigned = registry;
    CHECK(assigned.capacity() == 1001);
    CHECK(assigned.find("W1000")->registry_index == 1000);
    WeaponRegistry limited;
    CHECK(install_weapon_text(limited, "[W300]{id=300;}[W1000]{id=1000;}") == 0);
    CHECK(limited.capacity() == 256);
    CHECK(limited.find("W300") == nullptr);
}

int main() {
    std::array<WeaponDefinition, 3> d{};
    d[0].reload_time_ticks = 30;
    d[1].reload_time_ticks = 61;
    d[1].registry_index = 1;
    d[2].reload_time_ticks = 1;
    UnitWeapons u;
    for (size_t i = 0; i < 3; ++i) {
        u.definitions[i] = &d[i];
        u.slots[i].stockpile = 0xff;
        u.slots[i].flags = 0xaf;
    }
    std::array<SlotGeometry, 3> g{{{0, 9}, {10, 7}, {-4, -3}}};
    const auto r = initialize_weapon_slots(u, g);
    CHECK(r.maximum_reload_milliseconds == 2033);
    CHECK(u.slots[0].muzzle_offset == -11);
    CHECK(u.slots[1].muzzle_offset == 3);
    CHECK(u.slots[2].muzzle_offset == -1);
    CHECK(u.slots[0].flags == 0xb0);
    CHECK(u.slots[1].flags == 0xb6);
    CHECK(u.slots[2].flags == 0xb8);
    for (size_t i = 0; i < 3; ++i) {
        CHECK(u.slots[i].definition == &d[i]);
        CHECK(u.slots[i].stockpile == 0);
    }
    u.slots[0].flags = 0;
    u.definitions[0] = &d[0];
    CHECK(initialize_weapon_slots(u, g).maximum_reload_milliseconds == 2033);
    CHECK(u.slots[0].flags == 0x10);
    g[0] = {0x7fffffff, 0};
    const auto overflow_result = initialize_weapon_slots(u, g);
    CHECK(overflow_result.maximum_reload_milliseconds == 2033);
    CHECK(u.slots[0].muzzle_offset == -1610612738);
    WeaponRegistry registry;
    CHECK(install_weapon_text(registry, "[ARM_LASER]{id=5; reloadtime=1;}") == 1);
    CHECK(install_weapon_text(registry, "[D_GUN]{id=9; reloadtime=2.05;}") == 1);
    WeaponRegistry parsed;
    CHECK(
        install_weapon_text(
            parsed,
            "[DEFAULT]{id=0;}"
            "[PLASMA]{id= +17suffix; reloadtime=1.5; range=640; paralyzer=1; ballistic=1;"
            " explosiongaf=FX; explosionart=explode3; accuracy=80; tolerance=500;"
            " pitchtolerance=40; burst=3; burstrate=0.5;}"
        ) == 2
    );
    CHECK(parsed.find("plasma")->registry_index == 17);
    CHECK(parsed.find("plasma")->reload_time_ticks == 45);
    CHECK(parsed.find("plasma")->range_world_units == 640);
    CHECK(parsed.find("plasma")->flags == (weapon_paralyzer_flag | weapon_ballistic_flag));
    CHECK(parsed.find("plasma")->explosion_gaf == "FX");
    CHECK(parsed.find("plasma")->explosion_art == "explode3");
    CHECK(parsed.find("plasma")->accuracy == 80);
    CHECK(parsed.find("plasma")->tolerance == 500);
    CHECK(parsed.find("plasma")->pitch_tolerance == 40);
    CHECK(parsed.find("plasma")->burst == 3);
    CHECK(parsed.find("plasma")->burst_rate_ticks == 15);
    CHECK(parsed.find("default")->burst == 0);
    CHECK(parsed.find("default")->burst_rate_ticks == 0);
    // The record the slot was loaded into is kept for Game.weapon_defs.
    CHECK(std::string_view(parsed.records()[17].key) == "PLASMA");
    CHECK(parsed.records()[17].reload_time == 45 && parsed.records()[17].weapon_id == 17);
    CHECK(parsed.records()[18].key[0] == '\0' && parsed.records()[18].weapon_id == 18);
    // A missing range is 0x7fff; a range key with no number is 0, as the
    // game reads it.
    WeaponRegistry ranges;
    CHECK(
        install_weapon_text(ranges, "[FAR]{id=1;} [EMPTY]{id=2; range=;} [WORD]{id=3; range=x;}") ==
        3
    );
    CHECK(ranges.find("far")->range_world_units == 0x7fff);
    CHECK(ranges.find("empty")->range_world_units == 0);
    CHECK(ranges.find("word")->range_world_units == 0);
    const char* smoky = "[SMOKY]{id=12; endsmoke=1; smoketrail=1; smokedelay=0.1;"
                        " waterexplosiongaf=FX; waterexplosionart=h2oboom1;"
                        " lavaexplosiongaf=FX; lavaexplosionart=lavasplashsm;}";
    WeaponRegistry smoke_weapons;
    CHECK(install_weapon_text(smoke_weapons, smoky) == 1);
    // endsmoke is flag bit 10, smokedelay seconds * 30 into smoke_delay_ticks.
    CHECK(smoke_weapons.find("smoky")->flags == (weapon_end_smoke_flag | weapon_smoke_trail_flag));
    CHECK(smoke_weapons.find("smoky")->smoke_delay_ticks == 3);
    CHECK(smoke_weapons.find("smoky")->water_explosion_art == "h2oboom1");
    // On a lava world the water explosion is the lava one.
    WeaponRegistry lava_weapons;
    CHECK(install_weapon_text(lava_weapons, smoky, true) == 1);
    CHECK(lava_weapons.find("smoky")->water_explosion_art == "lavasplashsm");
    WeaponRegistry impact_weapons;
    CHECK(
        install_weapon_text(
            impact_weapons,
            "[QUAKE]{id=13; soundhit=xplolrg1; soundwater=splslrg; shakemagnitude=24;"
            " shakeduration=2500; [DAMAGE]{default=50; ARMCOM=5; corcom=7;}}"
        ) == 1
    );
    // shakeduration seconds * 30 keeps all 32 truncated bits in
    // shake_duration_ticks (75000 overflows the word-sized tick fields).
    CHECK(impact_weapons.find("quake")->soundhit == "xplolrg1");
    CHECK(impact_weapons.find("quake")->soundwater == "splslrg");
    CHECK(impact_weapons.find("quake")->shake_magnitude == 24);
    CHECK(impact_weapons.find("quake")->shake_duration_ticks == 75000);
    CHECK(damage_against(*impact_weapons.find("quake"), "armcom") == 5);
    CHECK(damage_against(*impact_weapons.find("quake"), "CORCOM") == 7);
    CHECK(damage_against(*impact_weapons.find("quake"), "armpw") == 50);
    WeaponRegistry edge;
    CHECK(install_weapon_text(edge, "[EDGE]{id=8; burst=65537; burstrate=0.04;}") == 1);
    CHECK(edge.find("edge")->burst == 1);
    CHECK(edge.find("edge")->burst_rate_ticks == 1);
    // The D-gun's booleans, noexplode (bit 22) among them, and the two other
    // flags only some weapons set: shellweapon (bit 2) and noradar (bit 6).
    WeaponRegistry dgun_weapons;
    CHECK(
        install_weapon_text(
            dgun_weapons,
            "[ARM_DISINTEGRATOR]{id=22; lineofsight=1; turret=1; soundtrigger=1; beamweapon=1;"
            " noexplode=1; commandfire=1; startsmoke=1;}"
            "[QUIET_SHELL]{id=23; shellweapon=1; noradar=1;}"
        ) == 2
    );
    CHECK(
        dgun_weapons.find("arm_disintegrator")->flags ==
        (weapon_line_of_sight_flag | weapon_turret_flag | weapon_sound_trigger_flag |
         weapon_beam_flag | weapon_no_explode_flag | weapon_commandfire_flag |
         weapon_start_smoke_flag)
    );
    CHECK(dgun_weapons.find("quiet_shell")->flags == (weapon_shell_flag | weapon_no_radar_flag));
    // A later section with the same ID clears the bit its zero names.
    CHECK(install_weapon_text(dgun_weapons, "[ARM_DISINTEGRATOR]{id=22; noexplode=0;}") == 1);
    CHECK((dgun_weapons.find("arm_disintegrator")->flags & weapon_no_explode_flag) == 0);
    CHECK(accuracy_spread(0, 100, 100, 0) == 0);
    CHECK(accuracy_spread(80, 100, 100, 0) == 80);
    CHECK(accuracy_spread(0, 50, 100, 0) == 1024);
    CHECK(accuracy_spread(0, 50, 100, 2) == 512);
    CHECK(accuracy_spread(0, 50, 100, 1) == 1024);
    CHECK(accuracy_spread(0, 50, 100, -3) == 1024);
    CHECK(accuracy_spread(0, 50, 100, 3) == 341);
    CHECK(accelerate_projectile(0, 100, 30) == 30);
    CHECK(accelerate_projectile(90, 100, 30) == 100);
    CHECK(accelerate_projectile(100, 100, 30) == 100);
    // The comparisons are unsigned: a negative speed is above any maximum,
    // and an addition that wraps past zero lands below it.
    CHECK(accelerate_projectile(-1, 100, 30) == -1);
    CHECK(accelerate_projectile(50, -2, 0x7fffffff) == static_cast<int32_t>(0x80000031u));
    CHECK(accelerate_projectile(50, 100, -10) == 40);
    CHECK(turn_toward_angle(0, 1000, 100) == 100);
    CHECK(turn_toward_angle(0, 50, 100) == 50);
    CHECK(registry.find("arm_laser")->reload_time_ticks == 30);
    CHECK(!registry.find("missing"));
    const std::array<std::string_view, 3> names{"Arm_Laser", "missing", "d_gun"};
    Geometry host;
    UnitWeapons spawned;
    spawned.slots[0].flags = 0x80;
    spawned.slots[1].flags = 0x40;
    spawned.slots[2].flags = 0x20;
    const auto spawn = initialize_spawn_combat(spawned, registry, names, host);
    CHECK(spawn.resolved_nondefault_weapon);
    CHECK(spawned.slots[0].flags == 0x92);
    CHECK(spawned.slots[1].flags == 0x54);
    CHECK(spawned.slots[2].flags == 0x3a);
    CHECK(spawned.slots[0].muzzle_offset == 10);
    CHECK(spawned.slots[1].muzzle_offset == 11);
    CHECK(spawned.slots[2].muzzle_offset == 12);
    CHECK(host.maximum == 2033);
    CHECK(host.query == std::vector<uint8_t>({0, 1, 2}) && host.aim == host.query);
    UnitWeapons cleared;
    ClearingGeometry clearing;
    clearing.weapons = &cleared;
    const auto stopped = initialize_spawn_combat(cleared, registry, names, clearing);
    CHECK(stopped.definition_cleared && clearing.query == std::vector<uint8_t>({0}));
    CHECK(clearing.maximum == -1 && cleared.slots[1].definition == nullptr);
    std::array<uint32_t, 1> category_mask{1U << 3};
    TargetSource source;
    source.identity = 10;
    source.owner = 1;
    source.owner_present = true;
    source.owner_status = 2;
    source.type_flags = source_range_check_bypass_flag;
    source.preferred_category_masks[0] = category_mask;
    source.position = {0, 0, 0};
    source.weapon_flags[0] = 0;
    TargetFixture targets;
    targets.nearby = {
        {20, 2, {0x10000, 0, 0}, OA_UNIT_FLAG_LIVE, 0, 3, candidate_type_auto_target_flag},
        {30, 3, {0x20000, 0, 0}, OA_UNIT_FLAG_LIVE, 0, 4, candidate_type_auto_target_flag}
    };
    // Candidate selection index then randomized-distance score, repeated. Outside
    // the category mask wins even though its score is larger.
    targets.random_values = {0, 0, 0, 7};
    CHECK(select_automatic_target(source, {0, false}, targets) == 30);
    source.weapon_flags[0] = weapon_paralyzer_low_byte_flag;
    targets.random_index = 0;
    targets.nearby[1].candidate_flags = candidate_paralyzed_flag;
    CHECK(select_automatic_target(source, {0, false}, targets) == 20);
    constexpr uint32_t standing_fire_order_return_fire = 1U << 20; // standing fire order 1
    AttackSource attacker{
        10,
        standing_move_order_manoeuvre | standing_fire_order_return_fire,
        {0x00070001U, 2, 0x00090003U},
        123
    };
    AttackFixture attacks;
    CHECK(issue_attack_order(attacker, targets.nearby[0], false, attacks));
    CHECK(attacks.requests.size() == 2 && attacks.requests[0].kind == move_order_kind + 10);
    CHECK(attacks.requests[0].has_position && attacks.requests[0].position == attacker.position);
    CHECK(attacks.requests[1].kind == attack_order_kind + 10 && attacks.requests[1].target == 20);
    const std::array<uint32_t, 3> zero_position{};
    CHECK(!attacks.requests[1].has_position && attacks.requests[1].position == zero_position);
    CHECK(attacks.requests[1].leash_length == 123);
    CHECK(attacks.requests[1].source_x == 7 && attacks.requests[1].source_z == 9);
    attacks.commit = false;
    CHECK(!issue_attack_order(attacker, targets.nearby[0], false, attacks));
    IntelligenceFixture intelligence;
    // Unit 99 is dead; 20 stands at the centre, 30 five units east, 40 fifty.
    intelligence.units = {
        {20, 0, {0, 0, 0}}, {30, 0, {5u << 16, 0, 0}}, {40, 0, {50u << 16, 0, 0}}
    };
    const std::array<uint32_t, 3> centre{};
    const auto gather = [&](std::initializer_list<uint16_t> seen,
                            std::initializer_list<uint16_t> radar,
                            bool fallback,
                            int32_t radius,
                            std::vector<TargetUnit> found = {}) {
        gather_sightings(seen, radar, fallback, centre, radius, intelligence, found);
        std::vector<UnitIdentity> ids;
        for (const auto& unit : found)
            ids.push_back(unit.identity);
        return ids;
    };
    using Ids = std::vector<UnitIdentity>;
    CHECK(gather({20, 99}, {30}, true, 10) == Ids{20});
    CHECK(gather({99}, {30, 40}, false, 10).empty());
    CHECK(gather({99}, {30, 40}, true, 10) == Ids{30});
    CHECK(gather({40}, {30, 40}, true, 10) == Ids{30});
    // The reach is inclusive.
    CHECK(gather({30}, {}, false, 5) == Ids{30});
    CHECK(gather({30}, {}, false, 4).empty());
    // The radar list stands in only while the output is still empty.
    CHECK(gather({40}, {30}, true, 10, {intelligence.units[0]}) == Ids{20});
    WeaponScoreInput weapons[3]{};
    CHECK(threat_score(false, weapons) == 1);
    CHECK(threat_score(true, weapons) == 11);
    weapons[0] = {true, 200, 80};
    CHECK(threat_score(true, weapons) == 11 + 200 / 100 + 5 + 80 / 0x28);
    CHECK(energy_rate(12.0F, 4.0F, 3.0F, 2.0F, 5.0F) == 12.0F);
    CHECK(energy_rate(0.0F, 4.0F, 3.0F, 2.0F, 5.0F) == -8.0F);
    CHECK(energy_rate(0.0F, 0.0F, 3.0F, 2.0F, 5.0F) == -15.0F);
    CHECK(energy_rate(0.0F, 0.0F, 0.0F, 2.0F, 5.0F) == 0.0F);
    // An extractor starts at 11. The single 0.01 (0x3C23D70A) is
    // just under a hundredth, so 100 metal truncates to 11; the 0.002
    // (0x3B03126F) is just over, so 500 energy then reaches 12; +1 unarmed.
    StrategicType extractor{};
    extractor.extracts_metal = 1.0F;
    extractor.build_cost_metal = 100.0F;
    extractor.build_cost_energy = 500.0F;
    const StrategicRefreshContext calm{};
    CHECK(classify_type(extractor, calm) == 13);
    // A metal maker drawing wind energy (rate -10) is 1 + 10 + 10; three armed
    // weapons take the sum past 100.
    StrategicType maker{};
    maker.makes_metal = 1;
    maker.wind_generator = 10.0F;
    maker.abilities = ability_can_attack;
    for (auto& weapon : maker.weapons)
        weapon = {1, 400, 1000};
    StrategicRefreshContext windy{};
    windy.wind_factor = 1.0F;
    CHECK(classify_type(maker, windy) == 100);
    maker.abilities = 0;
    for (auto& weapon : maker.weapons)
        weapon = {};
    CHECK(classify_type(maker, windy) == 22);
    wide_weapon_registry_follows_the_table();
    std::cout << "combat state tests passed\n";
}

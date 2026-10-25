// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Headless check (--weapon-fire-check TYPE with --headless-check and
// --match-ticks): places finished units of TYPE, orders one to fire its
// first weapon at another and one to fire at TYPE's corpse, and fails
// unless those shots, the damage and the feature hit come from that weapon.
#include "oa/app/runtime.hpp"

#include "oa/core/feature_def.h"
#include "oa/core/game_state.h"
#include "oa/core/map_plot.h"
#include "oa/core/unit.h"
#include "oa/core/weapon_def.h"
#include "oa/core/world.h"
#include "oa/sim/feature_runtime.hpp"
#include "oa/sim/unit_spawn/spawn_runtime.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <utility>

namespace oa::app {
namespace {

constexpr int32_t map_margin = 16;
constexpr int32_t step_toward_middle = 64;
constexpr int32_t feature_search = 16;

[[nodiscard]] uint64_t integer_sqrt(uint64_t value) {
    if (value == 0)
        return 0;
    uint64_t guess = value;
    uint64_t next = (guess + 1) / 2;
    while (next < guess) {
        guess = next;
        next = (guess + value / guess) / 2;
    }
    return guess;
}

[[nodiscard]] int32_t scaled(int64_t component, int32_t pixels, uint64_t length) {
    if (length == 0)
        return 0;
    return static_cast<int32_t>((component * pixels) / static_cast<int64_t>(length));
}

[[nodiscard]] bool on_map(int32_t x, int32_t z, int32_t width, int32_t height) {
    return x >= map_margin && z >= map_margin && x < width - map_margin && z < height - map_margin;
}

[[nodiscard]] bool plot_open(const oa::World& world, int32_t cell_x, int32_t cell_z) {
    const oa::MapPlot* plot = oa::world_plot(&world, cell_x, cell_z);
    return plot != nullptr && plot->feature == oa::sim::feature_runtime::no_feature &&
           plot->ground_unit == 0;
}

[[nodiscard]] bool footprint_open(
    const oa::World& world, int32_t cell_x, int32_t cell_z, int32_t footprint_x, int32_t footprint_z
) {
    for (int32_t row = 0; row < footprint_z; ++row)
        for (int32_t column = 0; column < footprint_x; ++column)
            if (!plot_open(world, cell_x + column, cell_z + row))
                return false;
    return true;
}

[[noreturn]] void fail(const std::string& text) {
    throw std::runtime_error(text);
}

} // namespace

void Runtime::weapon_fire_shot_placed(
    void* context,
    const oa::World& world,
    const oa::Projectile& shot,
    oa::sim::match_runtime::ShotSource source,
    const oa::FixedVec3* aim,
    uint16_t target_unit
) {
    (void)world;
    (void)source;
    (void)aim;
    (void)target_unit;
    auto* runtime = static_cast<Runtime*>(context);
    if (runtime == nullptr || !runtime->weapon_fire_check_)
        return;
    WeaponFireNote noted{};
    noted.source = static_cast<uint16_t>(oa::oa_unit_slot_from_ref(shot.source));
    noted.def = shot.def;
    runtime->weapon_fire_check_->shots.push_back(noted);
}

void Runtime::weapon_fire_shot_detonated(
    void* context, const oa::World& world, const oa::Projectile& shot, uint16_t direct_unit
) {
    (void)world;
    (void)direct_unit;
    auto* runtime = static_cast<Runtime*>(context);
    if (runtime == nullptr || !runtime->weapon_fire_check_)
        return;
    auto& check = *runtime->weapon_fire_check_;
    WeaponFireNote noted{};
    noted.source = static_cast<uint16_t>(oa::oa_unit_slot_from_ref(shot.source));
    noted.def = shot.def;
    const auto pixel_x = static_cast<int32_t>(shot.position.x >> 16);
    const auto pixel_z = static_cast<int32_t>(shot.position.z >> 16);
    noted.cell_x = pixel_x >= 0 ? pixel_x / OA_MAP_CELL_PIXELS : -1;
    noted.cell_z = pixel_z >= 0 ? pixel_z / OA_MAP_CELL_PIXELS : -1;
    if (noted.source == check.unit_a && noted.def == check.weapon_ref)
        check.a_detonated = true;
    check.detonations.push_back(noted);
}

void Runtime::weapon_fire_unit_damaged(
    void* context,
    const oa::World& world,
    uint16_t target,
    uint16_t source,
    int16_t amount,
    uint8_t kind
) {
    (void)world;
    auto* runtime = static_cast<Runtime*>(context);
    if (runtime == nullptr || !runtime->weapon_fire_check_)
        return;
    auto& check = *runtime->weapon_fire_check_;
    WeaponFireNote noted{};
    noted.target = target;
    noted.source = source;
    noted.amount = amount;
    noted.kind = kind;
    noted.after_a_detonation = check.a_detonated;
    check.damage.push_back(noted);
}

void Runtime::begin_weapon_fire_check() {
    if (!match_ || options_.weapon_fire_check.empty())
        fail("weapon fire check needs an active match");
    const std::string& type_name = options_.weapon_fire_check;
    const uint16_t type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, type_name);
    if (type == 0)
        fail("weapon fire check: the game has no type " + type_name);
    auto& world = match_->state();
    if (world.unit_defs == nullptr || type >= world.unit_def_count)
        fail("weapon fire check: the game has no type " + type_name);
    const oa::UnitDef& type_def = world.unit_defs[type];
    const oa::WeaponDef* weapon = oa::world_weapon_def(&world, type_def.weapon1);
    if (weapon == nullptr || type_def.weapon1 == 0)
        fail("weapon fire check: " + type_name + " has no weapon");
    const uint16_t weapon_id = oa::oa_weapon_def_id(weapon);
    const int32_t range = weapon->range;
    if (range <= 0)
        fail("weapon fire check: " + type_name + " has no range");
    if (type_def.corpse < 0 || static_cast<uint32_t>(type_def.corpse) >= world.feature_def_count ||
        world.feature_defs == nullptr)
        fail("weapon fire check: " + type_name + " has no corpse");
    const oa::FeatureDef& corpse = world.feature_defs[type_def.corpse];
    const std::string corpse_name(corpse.name);
    const int32_t footprint_x = corpse.footprint_x > 0 ? corpse.footprint_x : 1;
    const int32_t footprint_z = corpse.footprint_z > 0 ? corpse.footprint_z : 1;

    const oa::sim::unit_spawn::Slot* commander = nullptr;
    const oa::sim::unit_spawn::Slot* first_local = nullptr;
    for (const auto& slot : match_->world().slots) {
        if (slot.unit == nullptr || slot.record.type_index == 0 ||
            slot.record.owner_index != match_local_player_)
            continue;
        if (first_local == nullptr)
            first_local = &slot;
        if (const auto* def = definition_for(slot.unit_index); def != nullptr && def->can_dgun) {
            commander = &slot;
            break;
        }
    }
    if (commander == nullptr)
        commander = first_local;
    if (commander == nullptr)
        fail("weapon fire check needs the local commander");

    const int32_t map_width =
        world.game.map_pixel_width > 0 ? world.game.map_pixel_width : world.game.map_width_world;
    const int32_t pixels_high =
        world.game.map_pixel_height > 0 ? world.game.map_pixel_height : world.game.map_height_world;
    const int32_t home_x = static_cast<int32_t>(commander->unit->position[0] >> 16);
    const int32_t home_z = static_cast<int32_t>(commander->unit->position[2] >> 16);
    int64_t toward_x = static_cast<int64_t>(map_width / 2) - home_x;
    int64_t toward_z = static_cast<int64_t>(pixels_high / 2) - home_z;
    if (toward_x == 0 && toward_z == 0)
        toward_x = 1;
    const uint64_t toward_length = integer_sqrt(
        static_cast<uint64_t>(toward_x * toward_x) + static_cast<uint64_t>(toward_z * toward_z)
    );
    const int32_t half_range = range / 2;
    const auto place_of = [&](int32_t sign) {
        struct Places {
            int32_t ax{}, az{}, tx{}, tz{}, bx{}, bz{}, fx{}, fz{};
            bool fits = false;
        } places;
        places.ax = home_x + scaled(toward_x, step_toward_middle, toward_length);
        places.az = home_z + scaled(toward_z, step_toward_middle, toward_length);
        places.tx = places.ax + scaled(toward_x, half_range, toward_length);
        places.tz = places.az + scaled(toward_z, half_range, toward_length);
        places.bx = places.ax + scaled(-toward_z, range, toward_length) * sign;
        places.bz = places.az + scaled(toward_x, range, toward_length) * sign;
        places.fx = places.bx + scaled(-toward_z, half_range, toward_length) * sign;
        places.fz = places.bz + scaled(toward_x, half_range, toward_length) * sign;
        places.fits = on_map(places.ax, places.az, map_width, pixels_high) &&
                      on_map(places.tx, places.tz, map_width, pixels_high) &&
                      on_map(places.bx, places.bz, map_width, pixels_high) &&
                      on_map(places.fx, places.fz, map_width, pixels_high);
        return places;
    };
    // A continuation cell is measured from its high corner, so a shot only
    // lands in the blast when it arrives from the high side of the wreck.
    const auto faces_high_side = [&](int32_t sign) {
        const int32_t dir_x = scaled(-toward_z, 1024, toward_length) * sign;
        const int32_t dir_z = scaled(toward_x, 1024, toward_length) * sign;
        return dir_x < 0 || dir_z < 0;
    };
    int32_t placed_sign = 0;
    const int32_t side_signs[2] = {1, -1};
    for (const int32_t sign : side_signs) {
        if (!place_of(sign).fits || !faces_high_side(sign))
            continue;
        placed_sign = sign;
        const int32_t dir_x = scaled(-toward_z, 1024, toward_length) * sign;
        const int32_t dir_z = scaled(toward_x, 1024, toward_length) * sign;
        if (dir_x < 0 && dir_z < 0)
            break;
    }
    if (placed_sign == 0) {
        if (place_of(1).fits)
            placed_sign = 1;
        else if (place_of(-1).fits)
            placed_sign = -1;
    }
    if (placed_sign == 0)
        fail("weapon fire check: the map has no room for " + type_name);
    auto places = place_of(placed_sign);
    const int32_t dir_x = scaled(-toward_z, 1024, toward_length) * placed_sign;
    const int32_t dir_z = scaled(toward_x, 1024, toward_length) * placed_sign;

    const auto spawn = [&](uint8_t player, int32_t x, int32_t z) {
        const auto fixed_x = static_cast<uint32_t>(x) << 16;
        const auto fixed_z = static_cast<uint32_t>(z) << 16;
        oa::sim::unit_spawn::Request request;
        request.player = player;
        request.type = type;
        request.finished = true;
        request.state = kGroundOccupancyState;
        request.position = {
            fixed_x, static_cast<uint32_t>(match_->map_height(fixed_x, fixed_z)) << 16, fixed_z
        };
        auto* spawned = match_->create(request);
        if (spawned == nullptr || spawned->unit == nullptr)
            fail("weapon fire check: could not place " + type_name);
        return spawned->unit_index;
    };
    const auto other = static_cast<uint8_t>(match_local_player_ == 0 ? 1 : 0);
    WeaponFireCheck check;
    check.type = type_name;
    check.fired = weapon_id;
    check.weapon_ref = type_def.weapon1;
    check.weapon_damage = weapon->damage_default;
    check.unit_a = spawn(match_local_player_, places.ax, places.az);
    check.unit_t = spawn(other, places.tx, places.tz);
    check.unit_b = spawn(match_local_player_, places.bx, places.bz);
    check.footprint_x = footprint_x;
    check.footprint_z = footprint_z;

    const int32_t wanted_x = places.fx / OA_MAP_CELL_PIXELS;
    const int32_t wanted_z = places.fz / OA_MAP_CELL_PIXELS;
    bool placed = false;
    for (int32_t ring = 0; ring <= feature_search && !placed; ++ring)
        for (int32_t row = -ring; row <= ring && !placed; ++row)
            for (int32_t column = -ring; column <= ring && !placed; ++column) {
                if (ring != 0 && row != -ring && row != ring && column != -ring && column != ring)
                    continue;
                const int32_t cell_x = wanted_x + column;
                const int32_t cell_z = wanted_z + row;
                if (!footprint_open(world, cell_x, cell_z, footprint_x, footprint_z))
                    continue;
                const int32_t middle_x =
                    cell_x * OA_MAP_CELL_PIXELS + footprint_x * OA_MAP_CELL_PIXELS / 2;
                const int32_t middle_z =
                    cell_z * OA_MAP_CELL_PIXELS + footprint_z * OA_MAP_CELL_PIXELS / 2;
                const int64_t from_b_x = static_cast<int64_t>(middle_x) - places.bx;
                const int64_t from_b_z = static_cast<int64_t>(middle_z) - places.bz;
                // Stay inside the weapon's reach, so a nearby open plot is
                // still a plot the unit can hit.
                const int32_t reach = range > map_margin ? range - map_margin : range;
                if (from_b_x * from_b_x + from_b_z * from_b_z > static_cast<int64_t>(reach) * reach)
                    continue;
                if (!console_place_feature(corpse_name.c_str(), cell_x, cell_z))
                    continue;
                const oa::MapPlot* plot = oa::world_plot(&world, cell_x, cell_z);
                if (plot == nullptr)
                    continue;
                check.feature_x = cell_x;
                check.feature_z = cell_z;
                check.feature_word = plot->feature;
                check.feature_plot = static_cast<std::size_t>(plot - world.plots);
                placed = true;
            }
    if (!placed)
        fail("weapon fire check: found no open ground for the corpse of " + type_name);

    if (!match_->issue_attack_command(check.unit_a, check.unit_t, false, nullptr))
        fail("weapon fire check: could not order the attack");
    // The wreck's centre is a cell in from the edge. This weapon's blast is
    // half its area of effect, which does not reach that centre from a shot
    // that stops on the near side. The high corner is the point a continuation
    // cell is measured from, so B fires at the high corner facing it.
    const int32_t model_x = (check.feature_x + footprint_x / 2) * OA_MAP_CELL_PIXELS;
    const int32_t model_z = (check.feature_z + footprint_z / 2) * OA_MAP_CELL_PIXELS;
    const int32_t half_w = footprint_x * OA_MAP_CELL_PIXELS / 2;
    const int32_t half_h = footprint_z * OA_MAP_CELL_PIXELS / 2;
    int32_t aim_x = model_x;
    int32_t aim_z = model_z;
    if (dir_x < 0)
        aim_x = model_x + half_w;
    if (dir_z < 0)
        aim_z = model_z + half_h;
    // An approach along one axis has to stay on the cell that measures from
    // that edge. The edge pixel itself belongs to the next cell.
    if (dir_x < 0 && dir_z >= 0)
        aim_z = model_z - 1;
    if (dir_z < 0 && dir_x >= 0)
        aim_x = model_x - 1;
    const auto fixed_x = static_cast<uint32_t>(aim_x) << 16;
    const auto fixed_z = static_cast<uint32_t>(aim_z) << 16;
    const oa::sim::ground_orders::Point ground{
        static_cast<int32_t>(fixed_x),
        static_cast<int32_t>(static_cast<uint32_t>(match_->map_height(fixed_x, fixed_z)) << 16),
        static_cast<int32_t>(fixed_z)
    };
    if (match_->issue_attack_ground(check.unit_b, ground, false) == nullptr)
        fail("weapon fire check: could not order the attack on the feature");

    weapon_fire_check_ = std::move(check);
    oa::sim::match_runtime::EventHooks hooks{};
    hooks.context = this;
    hooks.shot_placed = weapon_fire_shot_placed;
    hooks.shot_detonated = weapon_fire_shot_detonated;
    hooks.unit_damaged = weapon_fire_unit_damaged;
    const auto listener = match_->add_event_listener(hooks);
    if (listener == 0)
        fail("weapon fire check: the match is not listening");
    weapon_fire_check_->listener = listener;
    std::printf(
        "weapon fire check: %s fires weapon %u\n",
        type_name.c_str(),
        static_cast<unsigned>(weapon_id)
    );
    std::fflush(stdout);
}

void Runtime::finish_weapon_fire_check() {
    if (!weapon_fire_check_ || !match_)
        fail("weapon fire check did not start");
    const WeaponFireCheck check = std::move(*weapon_fire_check_);
    weapon_fire_check_.reset();
    if (check.listener != 0)
        (void)match_->remove_event_listener(check.listener);

    int shots = 0;
    for (const WeaponFireNote& noted : check.shots) {
        if (noted.source != check.unit_a)
            continue;
        if (noted.def != check.weapon_ref)
            fail(
                "weapon fire check: unit A placed a shot of another weapon than " +
                std::to_string(check.fired)
            );
        ++shots;
    }
    std::printf(
        "weapon fire check: unit A placed %d shots of weapon %u\n",
        shots,
        static_cast<unsigned>(check.fired)
    );
    std::fflush(stdout);
    if (shots <= 0)
        fail("weapon fire check: unit A placed no shots of weapon " + std::to_string(check.fired));

    oa::Projectile probe{};
    probe.def = check.weapon_ref;
    const auto* resolved = match_->projectile_weapon(probe);
    if (static_cast<std::size_t>(check.fired) >= weapon_registry_.capacity() ||
        resolved != &weapon_registry_.definition(check.fired))
        fail(
            "weapon fire check: weapon " + std::to_string(check.fired) + " is not the shot's weapon"
        );

    int32_t taken = 0;
    for (const WeaponFireNote& noted : check.damage) {
        if (noted.kind != 1 || noted.source != check.unit_a || noted.target != check.unit_t ||
            !noted.after_a_detonation || noted.amount <= 0)
            continue;
        taken += noted.amount;
    }
    if (taken <= 0)
        fail("weapon fire check: unit T took no damage from unit A");
    std::printf("weapon fire check: unit T took %d damage from unit A\n", static_cast<int>(taken));
    std::fflush(stdout);

    const auto& world = match_->state();
    if ((world.game.console_flags & OA_CONSOLE_FLAG_TREE_DEATH) == 0) {
        std::printf("weapon fire check: weapons damage features only while tree death is set\n");
        std::fflush(stdout);
        fail("weapon fire check: weapons damage features only while tree death is set");
    }
    const uint16_t folded = static_cast<uint16_t>(check.fired % 256);
    if (folded != check.fired) {
        const oa::WeaponDef* other = oa::world_weapon_def(&world, oa::oa_ref_from_index(folded));
        if (other != nullptr && other->damage_default == check.weapon_damage) {
            std::printf(
                "weapon fire check: weapon %u and weapon %u deal the same damage; the feature "
                "hit cannot tell them apart\n",
                static_cast<unsigned>(check.fired),
                static_cast<unsigned>(folded)
            );
            std::fflush(stdout);
            fail(
                "weapon fire check: weapon " + std::to_string(check.fired) + " and weapon " +
                std::to_string(folded) + " deal the same damage"
            );
        }
    }

    bool hit = false;
    for (const WeaponFireNote& noted : check.detonations) {
        if (noted.source != check.unit_b || noted.def != check.weapon_ref)
            continue;
        if (noted.cell_x < check.feature_x || noted.cell_z < check.feature_z)
            continue;
        if (noted.cell_x >= check.feature_x + check.footprint_x ||
            noted.cell_z >= check.feature_z + check.footprint_z)
            continue;
        hit = true;
        break;
    }
    if (!hit)
        fail(
            "weapon fire check: weapon " + std::to_string(check.fired) + " did not hit the feature"
        );

    const oa::MapPlot& plot = world.plots[check.feature_plot];
    const bool gone = plot.feature != check.feature_word;
    int32_t counted = 0;
    if ((plot.flags & OA_PLOT_FLAG_ANIMATING_FEATURE) != 0) {
        // An object wreck keeps its damage on the placed record. The plot's
        // record word is that record's slot, not a damage count.
        const oa::sim::feature_runtime::PlacedFeature* record =
            oa::sim::feature_runtime::feature_record(world, plot.feature_record);
        if (record != nullptr)
            counted = record->damage;
    } else {
        counted = plot.feature_record;
    }
    const bool multiple =
        !gone && check.weapon_damage > 0 && counted > 0 && counted % check.weapon_damage == 0;
    if (!gone && !multiple)
        fail(
            "weapon fire check: the feature's damage is not from weapon " +
            std::to_string(check.fired)
        );
    std::printf(
        "weapon fire check: weapon %u hit the feature at %d,%d: %d damage, weapon damage %d%s\n",
        static_cast<unsigned>(check.fired),
        check.feature_x,
        check.feature_z,
        static_cast<int>(counted),
        static_cast<int>(check.weapon_damage),
        gone ? " and destroyed it" : ""
    );
    std::fflush(stdout);

    std::printf(
        "weapon fire check: tick %llu digest %016llx\n",
        static_cast<unsigned long long>(match_timing_.tick),
        static_cast<unsigned long long>(match_world_digest())
    );
    std::fflush(stdout);
}

} // namespace oa::app

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/data/defs/unit_def_loader.hpp"
#include "oa/base/game_math.hpp"

#include "oa/data/defs/locale.hpp"
#include "oa/data/mission_types.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <span>

namespace oa::data::defs {
namespace {

constexpr uint32_t lookup_text_capacity = 100;
constexpr uint32_t weapon_name_capacity = 0x80;
constexpr uint32_t max_blocks = 1u << 16;
constexpr uint32_t standing_order_mask = 3u;
constexpr uint32_t self_destruct_countdown_mask = 7u;
constexpr uint32_t no_weapon_ref = 1;               // Game.weapon_defs[0]
constexpr uint32_t footprint_cell_fixed = 0x100000; // 16 world units in 16.16

// Replaces one bit with bit 0 of value.
uint32_t with_bit(uint32_t word, uint32_t bit, int32_t value) noexcept {
    return (word & ~bit) | ((static_cast<uint32_t>(value) & 1u) != 0 ? bit : 0u);
}

oa_ref32 weapon_ref(
    const formats::tdf::Block* block, const char* key, std::span<const WeaponDef> weapon_defs
) noexcept {
    char name[weapon_name_capacity];
    formats::tdf::get_string(block, key, name, sizeof name, "");
    const int index = find_weapon_by_name(weapon_defs, name);
    return index < 0 ? no_weapon_ref : static_cast<oa_ref32>(index) + 1u;
}

// Category index of the name, or the name read as a number when no category matches.
int16_t sound_category_of(const SoundCategoryTable* table, const char* name) noexcept {
    for (uint32_t index = 0; index < table->count; ++index)
        if (formats::tdf::compare_nocase(name, table->categories[index].name) == 0)
            return static_cast<int16_t>(index);
    return static_cast<int16_t>(formats::tdf::parse_int(name));
}

// Byte a YardMap character compiles to; any other character separates cells.
struct YardCellCode {
    char symbol{};
    uint8_t cell{};
};

constexpr YardCellCode yard_cell_codes[] = {
    {'.', 0x00},
    {'C', 0x35},
    {'G', 0x8f},
    {'O', 0x2b},
    {'Y', 0x31},
    {'c', 0x2d},
    {'f', 0x6f},
    {'o', 0x2f},
    {'w', 0x37},
    {'y', 0x29},
};

constexpr uint32_t yard_map_text_capacity = 0x400;
constexpr uint32_t yard_map_cell_limit = 1u << 20; // most cells a yard map fills

// A recognised character fills one cell and advances only when another
// character follows, so a short map repeats its last cell. A map with no
// recognised character leaves the cells zero.
void compile_yard_map(
    const char* text, int16_t footprint_x, int16_t footprint_z, uint8_t* cells
) noexcept {
    const char* at = text;
    uint32_t written = 0;
    for (int32_t z = 0; z < footprint_z; ++z) {
        int32_t x = 0;
        while (x < footprint_x) {
            if (*at == '\0')
                return;
            bool recognised = false;
            for (const YardCellCode& code : yard_cell_codes) {
                if (code.symbol == *at) {
                    cells[written] = code.cell;
                    recognised = true;
                    break;
                }
            }
            if (!recognised) {
                ++at;
                continue;
            }
            ++x;
            ++written;
            if (at[1] != '\0')
                ++at;
        }
    }
}

// Half a footprint's extent either side of the origin, from a 32-bit product
// that wraps as 3.1c's did, halved toward zero.
int32_t footprint_min(int16_t cells) noexcept {
    return static_cast<int32_t>(0u - static_cast<uint32_t>(cells) * footprint_cell_fixed) / 2;
}

int32_t footprint_max(int16_t cells) noexcept {
    return static_cast<int32_t>(static_cast<uint32_t>(cells) * footprint_cell_fixed) / 2;
}

int32_t wrapping_sub(int32_t left, int32_t right) noexcept {
    return static_cast<int32_t>(static_cast<uint32_t>(left) - static_cast<uint32_t>(right));
}

void load_yard_map(
    const formats::tdf::Block* block,
    UnitDef& unit,
    UnitDefBlocks* blocks,
    const YardMapRules& rules
) noexcept {
    unit.yard_map = 0;
    if (unit.bm_code != 0 && !rules.mobile_units)
        return;
    char text[yard_map_text_capacity];
    const bool found = formats::tdf::get_string(block, "YardMap", text, sizeof text, "");
    if (rules.skip_without_key && (!found || unit.footprint_x == 0 || unit.footprint_z == 0))
        return;
    const int32_t cells = static_cast<int32_t>(unit.footprint_z) * unit.footprint_x;
    if (cells <= 0 || static_cast<uint32_t>(cells) > yard_map_cell_limit)
        return;
    unit.yard_map = unit_def_blocks_alloc(blocks, static_cast<uint32_t>(cells));
    if (unit.yard_map != 0)
        compile_yard_map(
            text, unit.footprint_x, unit.footprint_z, unit_def_block(blocks, unit.yard_map)
        );
}

} // namespace

void unit_def_blocks_init(UnitDefBlocks* blocks) noexcept {
    blocks->blocks = nullptr;
    blocks->count = 0;
    blocks->capacity = 0;
}

oa_ref32 unit_def_blocks_alloc(UnitDefBlocks* blocks, uint32_t size) noexcept {
    if (blocks->count == blocks->capacity) {
        if (blocks->capacity >= max_blocks)
            return 0;
        const uint32_t grown = blocks->capacity == 0 ? 256u : blocks->capacity * 2u;
        auto* grown_blocks =
            static_cast<UnitDefBlock*>(std::realloc(blocks->blocks, sizeof(UnitDefBlock) * grown));
        if (grown_blocks == nullptr)
            return 0;
        blocks->blocks = grown_blocks;
        blocks->capacity = grown;
    }
    auto* bytes = static_cast<uint8_t*>(std::calloc(size == 0 ? 1 : size, 1));
    if (bytes == nullptr)
        return 0;
    blocks->blocks[blocks->count] = {bytes, size};
    return ++blocks->count;
}

uint8_t* unit_def_block(const UnitDefBlocks* blocks, oa_ref32 ref) noexcept {
    return ref != 0 && ref <= blocks->count ? blocks->blocks[ref - 1u].bytes : nullptr;
}

uint32_t unit_def_block_size(const UnitDefBlocks* blocks, oa_ref32 ref) noexcept {
    return ref != 0 && ref <= blocks->count ? blocks->blocks[ref - 1u].size : 0u;
}

int find_weapon_by_name(std::span<const WeaponDef> weapon_defs, const char* name) noexcept {
    if (name == nullptr || name[0] == '\0')
        return -1;
    for (uint32_t index = 0; index < weapon_defs.size(); ++index)
        if (formats::tdf::compare_nocase(name, weapon_defs[index].key) == 0)
            return static_cast<int>(index);
    return -1;
}

bool load_unit_def(
    const Files* files, const char* path, UnitDef& unit, const UnitDefSources& sources
) noexcept {
    formats::tdf::Document document;
    formats::tdf::document_init(&document);
    if (!load_tdf_file(files, path, &document, nullptr) ||
        !formats::tdf::select_section(&document, "UNITINFO")) {
        formats::tdf::document_free(&document);
        return false;
    }
    const formats::tdf::Block* block = formats::tdf::cursor(&document);
    char text[lookup_text_capacity];

    formats::tdf::get_string(block, "unitname", unit.unit_name, sizeof unit.unit_name, "");
    get_localized_string(block, sources.language, "name", unit.name, sizeof unit.name, nullptr);
    get_localized_string(
        block, sources.language, "description", unit.description, sizeof unit.description, nullptr
    );
    read_unit_texts(block, unit.unit_name, sources.texts);
    formats::tdf::get_string(block, "defaultmissiontype", text, sizeof text, "");
    unit.default_mission_type = static_cast<int8_t>(data::mission_types::index_for_name(text));
    formats::tdf::get_string(
        block, "wpri_badTargetCategory", text, sizeof text, bad_target_category_default
    );
    unit.primary_bad_target_category = category_registry_ref(sources.categories, text);
    formats::tdf::get_string(
        block, "wsec_badTargetCategory", text, sizeof text, bad_target_category_default
    );
    unit.secondary_bad_target_category = category_registry_ref(sources.categories, text);
    formats::tdf::get_string(
        block, "wspe_badTargetCategory", text, sizeof text, bad_target_category_default
    );
    unit.special_bad_target_category = category_registry_ref(sources.categories, text);
    formats::tdf::get_string(
        block, "noChaseCategory", text, sizeof text, bad_target_category_default
    );
    unit.no_chase_category = category_registry_ref(sources.categories, text);
    if (!formats::tdf::get_string(
            block, "objectname", unit.object_name, sizeof unit.object_name, ""
        ))
        std::memcpy(unit.object_name, unit.unit_name, sizeof unit.object_name);

    unit.build_cost_energy = static_cast<float>(formats::tdf::get_int(block, "buildcostenergy", 0));
    unit.build_cost_metal = static_cast<float>(formats::tdf::get_int(block, "buildcostmetal", 0));
    unit.max_velocity = formats::tdf::get_fixed(block, "maxvelocity", 0);
    unit.brake_rate = formats::tdf::get_fixed(block, "brakerate", 0);
    unit.acceleration = formats::tdf::get_fixed(block, "acceleration", 0);
    unit.bank_scale = formats::tdf::get_fixed(block, "bankscale", fixed_unit_scale);
    unit.pitch_scale = formats::tdf::get_fixed(block, "pitchscale", 0);
    unit.damage_modifier = formats::tdf::get_fixed(block, "damagemodifier", fixed_unit_scale);
    const auto doubled_velocity =
        static_cast<int32_t>(static_cast<uint32_t>(unit.max_velocity) << 1);
    unit.move_rate1 = formats::tdf::get_fixed(block, "moverate1", doubled_velocity);
    unit.move_rate2 = formats::tdf::get_fixed(block, "moverate2", doubled_velocity);
    unit.turn_rate = static_cast<int16_t>(formats::tdf::get_int(block, "turnrate", 0));
    unit.water_line = static_cast<int8_t>(formats::tdf::get_int(block, "waterline", 0));
    unit.transport_size = static_cast<int8_t>(formats::tdf::get_int(block, "transportsize", 0));
    unit.transport_capacity =
        static_cast<int8_t>(formats::tdf::get_int(block, "transportcapacity", 0));
    unit.energy_make = static_cast<float>(formats::tdf::get_double(block, "energymake", 0.0));
    unit.energy_use = static_cast<float>(formats::tdf::get_double(block, "energyuse", 0.0));
    unit.metal_make = static_cast<float>(formats::tdf::get_double(block, "metalmake", 0.0));
    unit.extracts_metal = static_cast<float>(formats::tdf::get_double(block, "extractsmetal", 0.0));
    unit.makes_metal = static_cast<int8_t>(formats::tdf::get_int(block, "makesmetal", 0));
    unit.wind_generator = static_cast<float>(formats::tdf::get_double(block, "windgenerator", 0.0));
    unit.tidal_generator =
        static_cast<float>(formats::tdf::get_double(block, "tidalgenerator", 0.0));
    unit.energy_storage = static_cast<float>(formats::tdf::get_double(block, "energystorage", 0.0));
    unit.metal_storage = static_cast<float>(formats::tdf::get_double(block, "metalstorage", 0.0));
    unit.build_time = formats::tdf::get_int(block, "buildtime", 0);
    unit.worker_time = static_cast<int16_t>(formats::tdf::get_int(block, "workertime", 0));
    unit.heal_time = static_cast<int16_t>(formats::tdf::get_int(block, "healtime", 0));
    unit.max_damage = static_cast<uint32_t>(formats::tdf::get_int(block, "maxdamage", 0));
    unit.sight_distance = static_cast<int16_t>(formats::tdf::get_int(block, "sightdistance", 0));
    unit.radar_distance = static_cast<int16_t>(formats::tdf::get_int(block, "radardistance", 0));
    unit.sonar_distance = static_cast<int16_t>(formats::tdf::get_int(block, "sonardistance", 0));
    unit.radar_distance_jam =
        static_cast<int16_t>(formats::tdf::get_int(block, "radardistancejam", 0));
    unit.sonar_distance_jam =
        static_cast<int16_t>(formats::tdf::get_int(block, "sonardistancejam", 0));
    unit.bm_code = static_cast<int8_t>(formats::tdf::get_int(block, "bmcode", 0));

    uint32_t flags = unit.flags;
    const auto move_order = static_cast<uint32_t>(formats::tdf::get_int(
        block, "standingmoveorder", static_cast<int32_t>(standing_order_default)
    ));
    flags = (flags & ~OA_UNIT_DEF_FLAG_MOVE_ORDER_MASK) | (move_order & standing_order_mask);
    const auto fire_order = static_cast<uint32_t>(formats::tdf::get_int(
        block, "standingfireorder", static_cast<int32_t>(standing_order_default)
    ));
    flags = (flags & ~OA_UNIT_DEF_FLAG_FIRE_ORDER_MASK) |
            ((fire_order & standing_order_mask) << OA_UNIT_DEF_FLAG_FIRE_ORDER_SHIFT);
    flags = with_bit(
        flags, OA_UNIT_DEF_FLAG_INIT_CLOAKED, formats::tdf::get_int(block, "init_cloaked", 0)
    );
    flags = with_bit(
        flags, OA_UNIT_DEF_FLAG_DOWNLOADABLE, formats::tdf::get_int(block, "downloadable", 0)
    );
    flags = with_bit(flags, OA_UNIT_DEF_FLAG_BUILDER, formats::tdf::get_int(block, "builder", 0));
    flags = with_bit(flags, OA_UNIT_DEF_FLAG_STEALTH, formats::tdf::get_int(block, "stealth", 0));
    unit.cloak_cost = static_cast<float>(formats::tdf::get_int(block, "cloakcost", 0));
    const auto cloak_cost_whole = static_cast<int32_t>(static_cast<uint64_t>(
        base::game_math::truncate_to_int64(static_cast<double>(unit.cloak_cost))
    ));
    unit.cloak_cost_moving =
        static_cast<float>(formats::tdf::get_int(block, "cloakcostmoving", cloak_cost_whole));
    unit.min_cloak_distance =
        static_cast<int16_t>(formats::tdf::get_int(block, "mincloakdistance", 0));
    unit.build_angle = static_cast<int16_t>(formats::tdf::get_int(block, "buildangle", 0));
    unit.build_distance = static_cast<int16_t>(formats::tdf::get_int(block, "builddistance", 0));
    unit.sort_bias = static_cast<int16_t>(formats::tdf::get_int(block, "sortbias", 0));
    unit.cruise_alt = static_cast<int16_t>(formats::tdf::get_int(block, "cruisealt", 0));
    flags = with_bit(flags, OA_UNIT_DEF_FLAG_Z_BUFFER, formats::tdf::get_int(block, "zbuffer", 0));
    flags =
        with_bit(flags, OA_UNIT_DEF_FLAG_IS_AIRBASE, formats::tdf::get_int(block, "isairbase", 0));
    flags = with_bit(
        flags,
        OA_UNIT_DEF_FLAG_TARGETING_UPGRADE,
        formats::tdf::get_int(block, "istargetingupgrade", 0)
    );
    flags =
        with_bit(flags, OA_UNIT_DEF_FLAG_TELEPORTER, formats::tdf::get_int(block, "teleporter", 0));
    flags = with_bit(
        flags, OA_UNIT_DEF_FLAG_HIDE_DAMAGE, formats::tdf::get_int(block, "hidedamage", 0)
    );
    flags = with_bit(flags, OA_UNIT_DEF_FLAG_SHOOT_ME, formats::tdf::get_int(block, "shootme", 0));
    flags = with_bit(
        flags, OA_UNIT_DEF_FLAG_ARMORED_STATE, formats::tdf::get_int(block, "armoredstate", 0)
    );
    flags = with_bit(
        flags,
        OA_UNIT_DEF_FLAG_ACTIVATE_WHEN_BUILT,
        formats::tdf::get_int(block, "activatewhenbuilt", 0)
    );
    flags = with_bit(flags, OA_UNIT_DEF_FLAG_CAN_FLY, formats::tdf::get_int(block, "canfly", 0));
    flags =
        with_bit(flags, OA_UNIT_DEF_FLAG_CAN_HOVER, formats::tdf::get_int(block, "canhover", 0));
    flags = with_bit(flags, OA_UNIT_DEF_FLAG_UPRIGHT, formats::tdf::get_int(block, "upright", 0));
    flags = with_bit(flags, OA_UNIT_DEF_FLAG_FLOATER, formats::tdf::get_int(block, "floater", 0));
    flags =
        with_bit(flags, OA_UNIT_DEF_FLAG_AMPHIBIOUS, formats::tdf::get_int(block, "amphibious", 0));
    flags =
        with_bit(flags, OA_UNIT_DEF_FLAG_IS_FEATURE, formats::tdf::get_int(block, "isfeature", 0));
    flags =
        with_bit(flags, OA_UNIT_DEF_FLAG_NO_SHADOW, formats::tdf::get_int(block, "noshadow", 0));
    flags = with_bit(
        flags,
        OA_UNIT_DEF_FLAG_IMMUNE_TO_PARALYZER,
        formats::tdf::get_int(block, "immunetoparalyzer", 0)
    );
    flags = with_bit(
        flags, OA_UNIT_DEF_FLAG_HOVER_ATTACK, formats::tdf::get_int(block, "hoverattack", 0)
    );
    flags = with_bit(
        flags, OA_UNIT_DEF_FLAG_ANTI_WEAPONS, formats::tdf::get_int(block, "antiweapons", 0)
    );
    flags = with_bit(flags, OA_UNIT_DEF_FLAG_DIGGER, formats::tdf::get_int(block, "digger", 0));

    uint32_t abilities = unit.abilities;
    abilities = with_bit(
        abilities, OA_UNIT_DEF_ABILITY_ON_OFFABLE, formats::tdf::get_int(block, "onoffable", 0)
    );
    abilities = with_bit(
        abilities,
        OA_UNIT_DEF_ABILITY_MOBILE_STAND_ORDERS,
        formats::tdf::get_int(block, "mobilestandorders", 0)
    );
    abilities = with_bit(
        abilities,
        OA_UNIT_DEF_ABILITY_FIRE_STAND_ORDERS,
        formats::tdf::get_int(block, "firestandorders", 0)
    );
    abilities = with_bit(
        abilities, OA_UNIT_DEF_ABILITY_CAN_STOP, formats::tdf::get_int(block, "canstop", 0)
    );
    abilities = with_bit(
        abilities, OA_UNIT_DEF_ABILITY_CAN_ATTACK, formats::tdf::get_int(block, "canattack", 0)
    );
    abilities = with_bit(
        abilities, OA_UNIT_DEF_ABILITY_CAN_GUARD, formats::tdf::get_int(block, "canguard", 0)
    );
    abilities = with_bit(
        abilities, OA_UNIT_DEF_ABILITY_CAN_PATROL, formats::tdf::get_int(block, "canpatrol", 0)
    );
    abilities = with_bit(
        abilities, OA_UNIT_DEF_ABILITY_CAN_MOVE, formats::tdf::get_int(block, "canmove", 0)
    );
    abilities = with_bit(
        abilities, OA_UNIT_DEF_ABILITY_CAN_LOAD, formats::tdf::get_int(block, "canload", 0)
    );
    abilities = with_bit(
        abilities,
        OA_UNIT_DEF_ABILITY_CAN_RECLAMATE,
        formats::tdf::get_int(block, "canreclamate", 0)
    );
    abilities = with_bit(
        abilities,
        OA_UNIT_DEF_ABILITY_CAN_RESURRECT,
        formats::tdf::get_int(block, "canresurrect", 0)
    );
    abilities = with_bit(
        abilities,
        OA_UNIT_DEF_ABILITY_CAN_REPAIR,
        (abilities & OA_UNIT_DEF_ABILITY_CAN_RECLAMATE) != 0 ? 1 : 0
    );
    abilities = with_bit(
        abilities, OA_UNIT_DEF_ABILITY_CAN_CAPTURE, formats::tdf::get_int(block, "cancapture", 0)
    );
    abilities = with_bit(abilities, OA_UNIT_DEF_ABILITY_CAN_CLOAK, unit.cloak_cost > 0.0F ? 1 : 0);
    abilities = with_bit(
        abilities, OA_UNIT_DEF_ABILITY_CAN_DGUN, formats::tdf::get_int(block, "candgun", 0)
    );
    unit.maneuver_leash_length =
        static_cast<int16_t>(formats::tdf::get_int(block, "maneuverleashlength", 0));
    unit.attack_run_length =
        static_cast<int16_t>(formats::tdf::get_int(block, "attackrunlength", 0));
    flags = with_bit(flags, OA_UNIT_DEF_FLAG_KAMIKAZE, formats::tdf::get_int(block, "kamikaze", 0));
    unit.kamikaze_distance =
        static_cast<int16_t>(formats::tdf::get_int(block, "kamikazedistance", 0));
    abilities = with_bit(
        abilities, OA_UNIT_DEF_ABILITY_NO_RESTRICT, formats::tdf::get_int(block, "norestrict", 0)
    );
    abilities = with_bit(
        abilities,
        OA_UNIT_DEF_ABILITY_SHOW_PLAYER_NAME,
        formats::tdf::get_int(block, "showplayername", 0)
    );
    abilities = with_bit(
        abilities, OA_UNIT_DEF_ABILITY_COMMANDER, formats::tdf::get_int(block, "commander", 0)
    );
    abilities = with_bit(
        abilities,
        OA_UNIT_DEF_ABILITY_CANT_BE_TRANSPORTED,
        formats::tdf::get_int(block, "cantbetransported", 0)
    );
    const char* countdown = formats::tdf::find_value(block, "selfdestructcountdown");
    const uint32_t countdown_value =
        countdown == nullptr ? self_destruct_countdown_default
                             : static_cast<uint32_t>(formats::tdf::parse_int(countdown));
    abilities = (abilities & ~OA_UNIT_DEF_ABILITY_SELF_DESTRUCT_MASK) |
                ((countdown_value & self_destruct_countdown_mask)
                 << OA_UNIT_DEF_ABILITY_SELF_DESTRUCT_SHIFT);
    unit.flags = flags;
    unit.abilities = abilities;

    formats::tdf::get_string(block, "category", text, sizeof text, "");
    register_unit_categories(sources.categories, &unit, text);
    unit.sound_category = formats::tdf::get_string(block, "soundcategory", text, sizeof text, "")
                              ? sound_category_of(sources.sound_categories, text)
                              : int16_t{0};
    unit.corpse = -1;
    if (formats::tdf::get_string(block, "corpse", text, sizeof text, "") &&
        sources.host != nullptr && sources.host->corpse != nullptr)
        unit.corpse = sources.host->corpse(sources.host->context, text);

    unit.move_class = 0;
    if (formats::tdf::get_string(block, "movementclass", text, sizeof text, "")) {
        const int slot = move_class_find(sources.move_classes, text);
        unit.move_class = slot < 0 ? 0u : static_cast<oa_ref32>(slot) + 1u;
    }
    MoveClass own_class;
    move_class_init_defaults(&own_class);
    const MoveClass* move_class = &own_class;
    if (unit.move_class == 0)
        move_class_load(&own_class, block);
    else
        move_class = &sources.move_classes->classes[unit.move_class - 1u];
    unit.footprint_x = move_class->footprint_x;
    unit.footprint_z = move_class->footprint_z;
    unit.max_water_depth = move_class->max_water_depth;
    unit.min_water_depth = move_class->min_water_depth;
    unit.max_slope = move_class->max_slope;
    unit.max_water_slope = move_class->max_water_slope;
    const int64_t slope_divisor = static_cast<int64_t>(unit.max_slope + 1) << 16;
    unit.slope_speed_step =
        static_cast<int32_t>((static_cast<int64_t>(unit.max_velocity) * 0x10000) / slope_divisor);

    unit.weapon1 = weapon_ref(block, "weapon1", sources.weapon_defs);
    unit.weapon2 = weapon_ref(block, "weapon2", sources.weapon_defs);
    unit.weapon3 = weapon_ref(block, "weapon3", sources.weapon_defs);
    unit.explode_as = weapon_ref(block, "explodeas", sources.weapon_defs);
    unit.self_destruct_as = weapon_ref(block, "selfdestructas", sources.weapon_defs);
    const bool armed = unit.weapon1 != no_weapon_ref || unit.weapon2 != no_weapon_ref ||
                       unit.weapon3 != no_weapon_ref;
    unit.flags = with_bit(unit.flags, OA_UNIT_DEF_FLAG_HAS_WEAPONS, armed ? 1 : 0);

    load_yard_map(block, unit, sources.blocks, sources.yard_maps);
    unit.bounds_min_x = footprint_min(unit.footprint_x);
    unit.bounds_min_z = footprint_min(unit.footprint_z);
    unit.bounds_max_x = footprint_max(unit.footprint_x);
    unit.bounds_max_z = footprint_max(unit.footprint_z);
    unit.size_x = wrapping_sub(unit.bounds_max_x, unit.bounds_min_x);
    unit.size_y = wrapping_sub(unit.model_height, unit.bounds_min_y);
    unit.size_z = wrapping_sub(unit.bounds_max_z, unit.bounds_min_z);
    unit.size_radius = static_cast<int32_t>(
                           static_cast<uint32_t>(unit.size_z) + static_cast<uint32_t>(unit.size_x)
                       ) /
                       3;
    formats::tdf::document_free(&document);
    if ((unit.abilities & OA_UNIT_DEF_ABILITY_CAN_CLOAK) != 0 && unit.min_cloak_distance == 0)
        unit.min_cloak_distance = min_cloak_distance_default;
    return true;
}

} // namespace oa::data::defs

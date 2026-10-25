// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/data/defs/weapons.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/base/game_math.hpp"
#include "oa/base/text.hpp"
#include "oa/data/limits.hpp"

#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace oa::data::defs {
namespace {

constexpr std::size_t model_name_capacity = sizeof(WeaponDef::model_name);
constexpr uint32_t max_damage_entries = 4096;
constexpr std::size_t asset_name_capacity = 0x100;

struct BoolKey {
    const char* key{};
    uint32_t flag{};
};

// The game's read order; noautorange is read earlier, next to weapontimer.
constexpr BoolKey flag_keys[] = {
    {"soundtrigger", weapon_flag_sound_trigger},
    {"guidance", weapon_flag_guidance},
    {"tracks", weapon_flag_tracks},
    {"lineofsight", weapon_flag_line_of_sight},
    {"ballistic", weapon_flag_ballistic},
    {"unitsonly", weapon_flag_units_only},
    {"groundbounce", weapon_flag_ground_bounce},
    {"waterweapon", weapon_flag_water_weapon},
    {"toairweapon", weapon_flag_to_air_weapon},
    {"smoketrail", weapon_flag_smoke_trail},
    {"turret", weapon_flag_turret},
    {"selfprop", weapon_flag_self_prop},
    {"propeller", weapon_flag_propeller},
    {"noexplode", weapon_flag_no_explode},
    {"burnblow", weapon_flag_burn_blow},
    {"twophase", weapon_flag_two_phase},
    {"cruise", weapon_flag_cruise},
    {"commandfire", weapon_flag_command_fire},
    {"stockpile", weapon_flag_stockpile},
    {"targetable", weapon_flag_targetable},
    {"interceptor", weapon_flag_interceptor},
    {"beamweapon", weapon_flag_beam_weapon},
    {"shellweapon", weapon_flag_shell_weapon},
    {"dropped", weapon_flag_dropped},
    {"vlaunch", weapon_flag_vlaunch},
    {"meteor", weapon_flag_meteor},
    {"noradar", weapon_flag_no_radar},
    {"paralyzer", weapon_flag_paralyzer},
    {"startsmoke", weapon_flag_start_smoke},
    {"endsmoke", weapon_flag_end_smoke},
};

uint32_t load_u32(const WeaponDef* weapon, std::size_t offset) noexcept {
    uint32_t value;
    std::memcpy(&value, reinterpret_cast<const unsigned char*>(weapon) + offset, sizeof value);
    return value;
}

void store_u32(WeaponDef* weapon, std::size_t offset, uint32_t value) noexcept {
    std::memcpy(reinterpret_cast<unsigned char*>(weapon) + offset, &value, sizeof value);
}

uint32_t flags_of(const WeaponDef* weapon) noexcept {
    return load_u32(weapon, offsetof(WeaponDef, flags));
}

void set_flag(WeaponDef* weapon, uint32_t flag, int32_t value) noexcept {
    const uint32_t flags = flags_of(weapon);
    store_u32(
        weapon,
        offsetof(WeaponDef, flags),
        (flags & ~flag) | ((static_cast<uint32_t>(value) & 1u) != 0 ? flag : 0u)
    );
}

// A scaled TDF double truncated toward zero to 64 bits; callers keep the low
// bits they store.
int64_t scaled(const formats::tdf::Block* section, const char* key, double scale) noexcept {
    return base::game_math::truncate_to_int64(formats::tdf::get_double(section, key, 0.0) * scale);
}

/// Copies a key's text into one of a slot's asset names, or empties the name for a missing key.
void keep_name(
    char (&kept)[weapon_asset_name_capacity], const formats::tdf::Block* section, const char* key
) noexcept {
    formats::tdf::get_string(section, key, kept, sizeof kept, "");
}

int16_t sound_of(
    const WeaponResolver* resolver,
    const formats::tdf::Block* section,
    const char* key,
    char (&kept)[weapon_asset_name_capacity]
) noexcept {
    keep_name(kept, section, key);
    char name[asset_name_capacity];
    if (!formats::tdf::get_string(section, key, name, sizeof name, ""))
        return weapon_no_sound;
    return resolver != nullptr && resolver->sound != nullptr
               ? resolver->sound(resolver->context, name)
               : weapon_no_sound;
}

oa_ref32 animation_of(
    const WeaponResolver* resolver,
    const formats::tdf::Block* section,
    const char* gaf_key,
    const char* art_key,
    char (&kept_gaf)[weapon_asset_name_capacity],
    char (&kept_art)[weapon_asset_name_capacity]
) noexcept {
    keep_name(kept_gaf, section, gaf_key);
    keep_name(kept_art, section, art_key);
    char gaf[asset_name_capacity];
    char art[asset_name_capacity];
    if (!formats::tdf::get_string(section, gaf_key, gaf, sizeof gaf, "") ||
        !formats::tdf::get_string(section, art_key, art, sizeof art, ""))
        return 0;
    return resolver != nullptr && resolver->animation != nullptr
               ? resolver->animation(resolver->context, gaf, art)
               : 0;
}

/// Inserts or updates one DAMAGE override.
///
/// Entries stay in case-insensitive name order; an existing entry is updated
/// only on a byte-exact name match. The map grows from 16 entries by
/// doubling, up to 4096.
///
/// @param[in,out] table override map of one weapon
/// @param unit unit name; cut at 31 characters
/// @param damage damage against that unit
/// @return false when the map is full or an allocation fails
bool set_damage(WeaponDamageTable* table, const char* unit, int32_t damage) noexcept {
    uint32_t first = 0;
    uint32_t length = table->count;
    while (length > 0) {
        const uint32_t half = length / 2;
        if (formats::tdf::compare_nocase(table->entries[first + half].unit, unit) < 0) {
            first += half + 1;
            length -= half + 1;
        } else {
            length = half;
        }
    }
    if (first != table->count && std::strcmp(table->entries[first].unit, unit) == 0) {
        table->entries[first].damage = damage;
        return true;
    }
    if (table->count == table->capacity) {
        if (table->capacity >= max_damage_entries)
            return false;
        const uint32_t grown = table->capacity == 0 ? 16u : table->capacity * 2u;
        auto* entries =
            static_cast<WeaponDamage*>(std::realloc(table->entries, sizeof(WeaponDamage) * grown));
        if (entries == nullptr)
            return false;
        table->entries = entries;
        table->capacity = grown;
    }
    WeaponDamage* slot = &table->entries[first];
    std::memmove(slot + 1, slot, sizeof(WeaponDamage) * (table->count - first));
    oa::base::text::copy_padded(slot->unit, unit, sizeof slot->unit - 1);
    slot->unit[sizeof slot->unit - 1] = '\0';
    slot->damage = damage;
    ++table->count;
    return true;
}

struct FileList {
    char (*names)[path_capacity];
    uint32_t count{};
    uint32_t capacity{};
    bool overflow{};
};

void push_name(void* user, const char* name) {
    auto* list = static_cast<FileList*>(user);
    if (list->count == list->capacity) {
        const uint32_t grown = list->capacity == 0 ? 64u : list->capacity * 2u;
        auto* names = static_cast<char (*)[path_capacity]>(
            std::realloc(list->names, sizeof *list->names * grown)
        );
        if (names == nullptr) {
            list->overflow = true;
            return;
        }
        list->names = names;
        list->capacity = grown;
    }
    oa::base::text::copy_padded(list->names[list->count], name, path_capacity - 1);
    list->names[list->count][path_capacity - 1] = '\0';
    ++list->count;
}

/// Returns the name of the model a weapon slot loaded and owns, WeaponDef.model_name.
///
/// The name is empty when the slot shares a lower slot's model or loaded none.
///
/// @param weapon weapon record of a WeaponTable
/// @return the slot's name buffer, sizeof(WeaponDef::model_name) bytes
char* model_name(WeaponDef* weapon) noexcept {
    return weapon->model_name;
}

/// Gives a weapon slot its model, sharing the model of a lower slot that loaded the same name.
///
/// Otherwise the resolver loads it and the slot records the name (up to 63
/// characters) as its own.
///
/// @param[in,out] table weapon table
/// @param id slot index of the weapon
/// @param name model name, matched case-insensitively
/// @param resolver asset resolver; null, or a null model callback, gives model 0
void share_or_load_model(
    WeaponTable* table, uint32_t id, const char* name, const WeaponResolver* resolver
) noexcept {
    WeaponDef* weapon = &table->defs[id];
    for (uint32_t lower = 0; lower < id; ++lower) {
        if (formats::tdf::compare_nocase(name, model_name(&table->defs[lower])) == 0) {
            model_name(weapon)[0] = '\0';
            weapon->model = table->defs[lower].model;
            return;
        }
    }
    weapon->model = resolver != nullptr && resolver->model != nullptr
                        ? resolver->model(resolver->context, name)
                        : 0;
    oa::base::text::copy_padded(model_name(weapon), name, model_name_capacity - 1);
    model_name(weapon)[model_name_capacity - 1] = '\0';
}

} // namespace

namespace {

/// Loads every top-level section of a parsed weapon document, in order.
///
/// @param[in,out] table weapon table
/// @param[in,out] document parsed TDF; its cursor is moved
/// @param options asset resolver and world options; may be null
/// @return false when any section was rejected
bool load_document(
    WeaponTable* table, formats::tdf::Document* document, const WeaponLoadOptions* options
) noexcept {
    bool all = true;
    for (uint32_t entry = 0;; ++entry) {
        formats::tdf::reset_cursor(document);
        if (!formats::tdf::step_entry(document, entry))
            break;
        all = weapon_load(table, formats::tdf::cursor(document), options) && all;
    }
    return all;
}

} // namespace

const char* weapon_model_name(const WeaponDef* weapon) noexcept {
    return weapon->model_name;
}

uint16_t weapon_id(const WeaponDef* weapon) noexcept {
    return oa_weapon_def_id(weapon);
}

uint32_t weapon_damage_handle(const WeaponDef* weapon) noexcept {
    return weapon->damage_overrides;
}

int32_t weapon_damage_for(
    const WeaponTable* table, const WeaponDef* weapon, const char* unit_name
) noexcept {
    const uint32_t handle = weapon_damage_handle(weapon);
    if (handle != 0 && handle <= table->damage.size()) {
        const WeaponDamageTable* damage = &table->damage[handle - 1];
        for (uint32_t index = 0; index < damage->count; ++index)
            if (formats::tdf::compare_nocase(damage->entries[index].unit, unit_name) == 0)
                return damage->entries[index].damage;
    }
    return weapon->damage_default;
}

namespace {

/// Holds a weapon-type limit to 256..65536.
///
/// @param options load options; null means 3.1c's 256
/// @return the limit weapon_load accepts IDs below
uint32_t weapon_type_limit(const WeaponLoadOptions* options) noexcept {
    uint32_t types = options != nullptr ? options->weapon_types : OA_WEAPON_DEF_COUNT;
    if (types < oa::data::limits::base_weapon_types)
        types = oa::data::limits::base_weapon_types;
    if (types > oa::data::limits::highest_weapon_types)
        types = oa::data::limits::highest_weapon_types;
    return types;
}

/// Grows every weapon vector to `slots` and numbers the new records.
///
/// @param[in,out] table table to grow
/// @param slots new length; ignored when the table is already that long
void grow_weapon_table(WeaponTable* table, uint32_t slots) noexcept {
    if (slots <= table->defs.size())
        return;
    const uint32_t first = static_cast<uint32_t>(table->defs.size());
    table->defs.resize(slots);
    table->damage.resize(slots);
    table->assets.resize(slots);
    table->rule_data.resize(slots);
    for (uint32_t slot = first; slot < slots; ++slot)
        oa_weapon_def_set_id(&table->defs[slot], static_cast<uint16_t>(slot));
}

} // namespace

void weapon_table_init(WeaponTable* table) noexcept {
    table->defs.clear();
    table->damage.clear();
    table->assets.clear();
    table->rule_data.clear();
    table->rejected_ids = 0;
    table->past_limit = 0;
    table->highest_past_limit = -1;
    grow_weapon_table(table, OA_WEAPON_DEF_COUNT);
}

void weapon_table_free(WeaponTable* table) noexcept {
    for (uint32_t slot = 0; slot < table->defs.size(); ++slot) {
        if (model_name(&table->defs[slot])[0] != '\0') {
            table->defs[slot].model = 0;
            model_name(&table->defs[slot])[0] = '\0';
        }
        std::free(table->damage[slot].entries);
        table->damage[slot] = WeaponDamageTable{};
        table->defs[slot].damage_overrides = 0;
    }
}

bool weapon_load(
    WeaponTable* table, const formats::tdf::Block* section, const WeaponLoadOptions* options
) noexcept {
    const int32_t id = formats::tdf::get_int(section, "ID", -1);
    const uint32_t limit = weapon_type_limit(options);
    if (id < 0) {
        ++table->rejected_ids; // a negative or missing ID
        return false;
    }
    if (static_cast<uint32_t>(id) >= limit) {
        ++table->rejected_ids;
        ++table->past_limit;
        if (id > table->highest_past_limit)
            table->highest_past_limit = id;
        return false;
    }
    const uint32_t slot = static_cast<uint32_t>(id);
    grow_weapon_table(table, slot + 1u);
    WeaponDef* weapon = &table->defs[slot];
    const WeaponResolver* resolver = options != nullptr ? options->resolver : nullptr;
    oa::base::text::copy_padded(weapon->key, section->name, sizeof weapon->key - 1);
    weapon->key[sizeof weapon->key - 1] = '\0';
    formats::tdf::get_string(section, "name", weapon->name, sizeof weapon->name, "");
    weapon->weapon_velocity =
        static_cast<oa_fixed>(scaled(section, "weaponvelocity", weapon_velocity_scale));
    weapon->start_velocity =
        static_cast<oa_fixed>(scaled(section, "startvelocity", weapon_velocity_scale));
    weapon->weapon_acceleration =
        static_cast<oa_fixed>(scaled(section, "weaponacceleration", weapon_acceleration_scale));
    weapon->range = formats::tdf::get_int(section, "range", weapon_default_range);
    weapon->coverage = formats::tdf::get_int(section, "coverage", 0);
    weapon->reload_time =
        static_cast<int16_t>(scaled(section, "reloadtime", weapon_ticks_per_second));
    weapon->energy_per_shot =
        static_cast<float>(formats::tdf::get_double(section, "energypershot", 0.0));
    weapon->metal_per_shot =
        static_cast<float>(formats::tdf::get_double(section, "metalpershot", 0.0));
    weapon->area_of_effect =
        static_cast<int16_t>(formats::tdf::get_int(section, "areaofeffect", 0));
    weapon->edge_effectiveness =
        static_cast<float>(formats::tdf::get_double(section, "edgeeffectiveness", 0.0));
    weapon->weapon_timer =
        static_cast<int16_t>(scaled(section, "weapontimer", weapon_ticks_per_second));
    set_flag(weapon, weapon_flag_no_auto_range, formats::tdf::get_int(section, "noautorange", 0));
    weapon->turn_rate = static_cast<int16_t>(scaled(section, "turnrate", weapon_turn_rate_scale));
    weapon->burst = static_cast<int16_t>(formats::tdf::get_int(section, "burst", 0));
    weapon->burst_rate =
        static_cast<int16_t>(scaled(section, "burstrate", weapon_ticks_per_second));
    weapon->spray_angle = static_cast<int16_t>(formats::tdf::get_int(section, "sprayangle", 0));
    weapon->duration = static_cast<int16_t>(scaled(section, "duration", weapon_ticks_per_second));
    weapon->random_decay =
        static_cast<int16_t>(scaled(section, "randomdecay", weapon_ticks_per_second));
    weapon->smoke_delay =
        static_cast<int16_t>(scaled(section, "smokedelay", weapon_ticks_per_second));
    weapon->flight_time =
        static_cast<int16_t>(scaled(section, "flighttime", weapon_ticks_per_second));
    weapon->hold_time = static_cast<int16_t>(scaled(section, "holdtime", weapon_ticks_per_second));
    weapon->min_barrel_angle = static_cast<float>(
        formats::tdf::get_double(section, "minbarrelangle", weapon_default_min_barrel_angle) *
        weapon_degrees_to_radians
    );
    weapon->fire_starter = static_cast<int8_t>(formats::tdf::get_int(section, "firestarter", 0));
    weapon->render_type = static_cast<int8_t>(formats::tdf::get_int(section, "rendertype", 0));
    weapon->color = static_cast<int8_t>(formats::tdf::get_int(section, "color", 0));
    weapon->color2 = static_cast<int8_t>(formats::tdf::get_int(section, "color2", 0));
    for (const BoolKey& flag : flag_keys)
        set_flag(weapon, flag.flag, formats::tdf::get_int(section, flag.key, 0));
    weapon->accuracy = static_cast<int16_t>(formats::tdf::get_int(section, "accuracy", 0));
    weapon->tolerance = static_cast<int16_t>(formats::tdf::get_int(section, "tolerance", 0));
    weapon->pitch_tolerance =
        static_cast<int16_t>(formats::tdf::get_int(section, "pitchtolerance", 0));
    weapon->shake_magnitude = formats::tdf::get_int(section, "shakemagnitude", 0);
    weapon->shake_duration =
        static_cast<int32_t>(scaled(section, "shakeduration", weapon_ticks_per_second));

    if (options != nullptr && options->data_keys != nullptr)
        read_weapon_rule_keys(section, *options->data_keys, table->rule_data[slot]);

    WeaponAssetNames& names = table->assets[slot];
    keep_name(names.model, section, "model");
    char model[asset_name_capacity];
    if (formats::tdf::get_string(section, "model", model, sizeof model, ""))
        share_or_load_model(table, slot, model, resolver);
    else
        weapon->model = 0;
    weapon->explosion_art = animation_of(
        resolver, section, "explosiongaf", "explosionart", names.explosion_gaf, names.explosion_art
    );
    const bool lava = options != nullptr && options->lava_world;
    weapon->water_explosion_art = animation_of(
        resolver,
        section,
        lava ? "lavaexplosiongaf" : "waterexplosiongaf",
        lava ? "lavaexplosionart" : "waterexplosionart",
        names.water_explosion_gaf,
        names.water_explosion_art
    );
    weapon->sound_start = sound_of(resolver, section, "soundstart", names.sound_start);
    weapon->sound_hit = sound_of(resolver, section, "soundhit", names.sound_hit);
    weapon->sound_water = sound_of(resolver, section, "soundwater", names.sound_water);

    const formats::tdf::Block* damage = formats::tdf::find_child(section, "DAMAGE");
    if (damage == nullptr) {
        weapon->damage_default = 0;
        return true;
    }
    weapon->damage_default = static_cast<int16_t>(formats::tdf::get_int(damage, "default", 0));
    for (int32_t index = 0;; ++index) {
        const char* unit = formats::tdf::property_key_at(damage, index);
        if (unit == nullptr)
            break;
        if (formats::tdf::compare_nocase(unit, "default") == 0)
            continue;
        const int32_t value = formats::tdf::get_int(damage, unit, 0);
        if (!set_damage(&table->damage[slot], unit, value))
            return false;
        weapon->damage_overrides = slot + 1u;
    }
    return true;
}

bool load_weapon_text(
    WeaponTable* table, const char* text, uint32_t length, const WeaponLoadOptions* options
) noexcept {
    formats::tdf::Document document;
    formats::tdf::document_init(&document);
    const bool parsed = formats::tdf::parse_text(&document, text, length, false, nullptr);
    const bool loaded = parsed && load_document(table, &document, options);
    formats::tdf::document_free(&document);
    return loaded;
}

uint32_t load_weapon_defs(
    const Files* files, WeaponTable* table, const WeaponLoadOptions* options
) noexcept {
    weapon_table_init(table);
    FileList list{};
    files->list(files->context, directory_name(DataDirectory::weapons), "tdf", push_name, &list);
    uint32_t loaded = 0;
    for (uint32_t index = 0; index < list.count; ++index) {
        char path[path_capacity];
        build_variant_path(
            files,
            path,
            sizeof path,
            directory_name(DataDirectory::weapons),
            list.names[index],
            "tdf",
            options != nullptr ? options->variant : nullptr
        );
        formats::tdf::Document document;
        formats::tdf::document_init(&document);
        if (load_tdf_file(files, path, &document, nullptr) &&
            (document.from_archive || options == nullptr || !options->archive_only)) {
            ++loaded;
            (void)load_document(table, &document, options);
        }
        formats::tdf::document_free(&document);
    }
    std::free(list.names);
    return loaded;
}

} // namespace oa::data::defs

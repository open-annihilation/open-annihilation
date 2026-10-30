// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

/* Weapon definition record (weapon TDF files). */
#ifndef OA_CORE_WEAPON_DEF_H
#define OA_CORE_WEAPON_DEF_H

#include "oa/core/types.h"

/* The slots Game.weapon_defs holds: 3.1c's weapon table. */
#define OA_WEAPON_DEF_COUNT 256

/* Bits of WeaponDef.flags, packed from the TDF boolean of the same name. */
#define OA_WEAPON_FLAG_LINE_OF_SIGHT 0x00000001u
#define OA_WEAPON_FLAG_BALLISTIC 0x00000002u
#define OA_WEAPON_FLAG_SHELL_WEAPON 0x00000004u
#define OA_WEAPON_FLAG_BEAM_WEAPON 0x00000008u
#define OA_WEAPON_FLAG_VLAUNCH 0x00000010u
#define OA_WEAPON_FLAG_METEOR 0x00000020u
#define OA_WEAPON_FLAG_NO_RADAR 0x00000040u
#define OA_WEAPON_FLAG_PARALYZER 0x00000080u
#define OA_WEAPON_FLAG_DROPPED 0x00000100u
#define OA_WEAPON_FLAG_START_SMOKE 0x00000200u
#define OA_WEAPON_FLAG_END_SMOKE 0x00000400u
#define OA_WEAPON_FLAG_SOUND_TRIGGER 0x00000800u
#define OA_WEAPON_FLAG_GUIDANCE 0x00001000u
#define OA_WEAPON_FLAG_TRACKS 0x00002000u
#define OA_WEAPON_FLAG_UNITS_ONLY 0x00004000u
#define OA_WEAPON_FLAG_GROUND_BOUNCE 0x00008000u
#define OA_WEAPON_FLAG_WATER_WEAPON 0x00010000u
#define OA_WEAPON_FLAG_TO_AIR_WEAPON 0x00020000u
#define OA_WEAPON_FLAG_SMOKE_TRAIL 0x00040000u
#define OA_WEAPON_FLAG_TURRET 0x00080000u
#define OA_WEAPON_FLAG_SELF_PROP 0x00100000u
#define OA_WEAPON_FLAG_PROPELLER 0x00200000u
#define OA_WEAPON_FLAG_NO_EXPLODE 0x00400000u
#define OA_WEAPON_FLAG_BURN_BLOW 0x00800000u
#define OA_WEAPON_FLAG_TWO_PHASE 0x01000000u
#define OA_WEAPON_FLAG_CRUISE 0x02000000u
#define OA_WEAPON_FLAG_COMMAND_FIRE 0x04000000u
#define OA_WEAPON_FLAG_NO_AUTO_RANGE 0x08000000u
#define OA_WEAPON_FLAG_STOCKPILE 0x10000000u
#define OA_WEAPON_FLAG_TARGETABLE 0x20000000u
#define OA_WEAPON_FLAG_INTERCEPTOR 0x40000000u

OA_CORE_BEGIN

#pragma pack(push, 1)

/* One weapon TDF record. The game state holds slots 0 to 255 inline; a mod
 * that numbers more keeps the rest in World.extra_weapon_defs. */
typedef struct WeaponDef {
    char key[32]; /* TDF section name */
    char name[64];
    /* The launch behaviour the flags select; the engine leaves it zero and
       reads the flags when a shot is fired. */
    oa_ref32 fire_callback;
    oa_ref32 damage_overrides; /* per-unit DAMAGE table; 0 none */
    oa_fixed weapon_velocity;
    oa_fixed start_velocity;
    oa_fixed weapon_acceleration;
    oa_ref32 model;               /* 3DO object */
    oa_ref32 explosion_art;       /* GAF sequence */
    oa_ref32 water_explosion_art; /* GAF sequence */
    char model_name[64];          /* model this slot loaded and owns; empty when shared */
    float energy_per_shot;
    float metal_per_shot;
    float min_barrel_angle; /* radians */
    int32_t shake_magnitude;
    int32_t shake_duration;
    int16_t damage_default;
    int16_t area_of_effect;
    float edge_effectiveness;
    int32_t range;
    int32_t coverage;
    int16_t reload_time;  /* ticks */
    int16_t weapon_timer; /* ticks */
    int16_t turn_rate;
    int16_t burst;
    int16_t burst_rate; /* ticks */
    int16_t spray_angle;
    int16_t duration;
    int16_t random_decay;
    int16_t sound_start; /* -1 none */
    int16_t sound_hit;   /* -1 none */
    int16_t sound_water; /* -1 none */
    int16_t smoke_delay;
    int16_t flight_time;
    int16_t hold_time;
    uint8_t reserved_after_hold_time[0x4]; /* no known use: zeroed with the table */
    int16_t accuracy;
    int16_t tolerance;
    int16_t pitch_tolerance;
    uint8_t weapon_id; /* the id's low byte; the slot index, written at table init */
    int8_t fire_starter;
    int8_t render_type;
    int8_t color;
    int8_t color2;
    uint8_t weapon_id_high;                     /* the id's high byte; 0 for every id under 256 */
    uint8_t reserved_after_weapon_id_high[0x1]; /* no known use: zeroed with the table */
    uint32_t flags;                             /* WEAPON_FLAG_*; unaligned */
} WeaponDef;

#pragma pack(pop)

OA_ASSERT_SIZE(WeaponDef, 0x115);
OA_ASSERT_OFFSET(WeaponDef, key, 0x0);
OA_ASSERT_OFFSET(WeaponDef, name, 0x20);
OA_ASSERT_OFFSET(WeaponDef, fire_callback, 0x60);
OA_ASSERT_OFFSET(WeaponDef, damage_overrides, 0x64);
OA_ASSERT_OFFSET(WeaponDef, weapon_velocity, 0x68);
OA_ASSERT_OFFSET(WeaponDef, start_velocity, 0x6c);
OA_ASSERT_OFFSET(WeaponDef, weapon_acceleration, 0x70);
OA_ASSERT_OFFSET(WeaponDef, model, 0x74);
OA_ASSERT_OFFSET(WeaponDef, explosion_art, 0x78);
OA_ASSERT_OFFSET(WeaponDef, water_explosion_art, 0x7c);
OA_ASSERT_OFFSET(WeaponDef, model_name, 0x80);
OA_ASSERT_OFFSET(WeaponDef, energy_per_shot, 0xc0);
OA_ASSERT_OFFSET(WeaponDef, metal_per_shot, 0xc4);
OA_ASSERT_OFFSET(WeaponDef, min_barrel_angle, 0xc8);
OA_ASSERT_OFFSET(WeaponDef, shake_magnitude, 0xcc);
OA_ASSERT_OFFSET(WeaponDef, shake_duration, 0xd0);
OA_ASSERT_OFFSET(WeaponDef, damage_default, 0xd4);
OA_ASSERT_OFFSET(WeaponDef, area_of_effect, 0xd6);
OA_ASSERT_OFFSET(WeaponDef, edge_effectiveness, 0xd8);
OA_ASSERT_OFFSET(WeaponDef, range, 0xdc);
OA_ASSERT_OFFSET(WeaponDef, coverage, 0xe0);
OA_ASSERT_OFFSET(WeaponDef, reload_time, 0xe4);
OA_ASSERT_OFFSET(WeaponDef, weapon_timer, 0xe6);
OA_ASSERT_OFFSET(WeaponDef, turn_rate, 0xe8);
OA_ASSERT_OFFSET(WeaponDef, burst, 0xea);
OA_ASSERT_OFFSET(WeaponDef, burst_rate, 0xec);
OA_ASSERT_OFFSET(WeaponDef, spray_angle, 0xee);
OA_ASSERT_OFFSET(WeaponDef, duration, 0xf0);
OA_ASSERT_OFFSET(WeaponDef, random_decay, 0xf2);
OA_ASSERT_OFFSET(WeaponDef, sound_start, 0xf4);
OA_ASSERT_OFFSET(WeaponDef, sound_hit, 0xf6);
OA_ASSERT_OFFSET(WeaponDef, sound_water, 0xf8);
OA_ASSERT_OFFSET(WeaponDef, smoke_delay, 0xfa);
OA_ASSERT_OFFSET(WeaponDef, flight_time, 0xfc);
OA_ASSERT_OFFSET(WeaponDef, hold_time, 0xfe);
OA_ASSERT_OFFSET(WeaponDef, reserved_after_hold_time, 0x100);
OA_ASSERT_OFFSET(WeaponDef, accuracy, 0x104);
OA_ASSERT_OFFSET(WeaponDef, tolerance, 0x106);
OA_ASSERT_OFFSET(WeaponDef, pitch_tolerance, 0x108);
OA_ASSERT_OFFSET(WeaponDef, weapon_id, 0x10a);
OA_ASSERT_OFFSET(WeaponDef, fire_starter, 0x10b);
OA_ASSERT_OFFSET(WeaponDef, render_type, 0x10c);
OA_ASSERT_OFFSET(WeaponDef, color, 0x10d);
OA_ASSERT_OFFSET(WeaponDef, color2, 0x10e);
OA_ASSERT_OFFSET(WeaponDef, weapon_id_high, 0x10f);
OA_ASSERT_OFFSET(WeaponDef, reserved_after_weapon_id_high, 0x110);
OA_ASSERT_OFFSET(WeaponDef, flags, 0x111);

/// Returns the weapon id stored across the record's two id bytes.
///
/// @param weapon record whose id is read
/// @return the low byte with the high byte above it
static inline uint16_t oa_weapon_def_id(const WeaponDef* weapon) {
    return (uint16_t)weapon->weapon_id | (uint16_t)((uint16_t)weapon->weapon_id_high << 8);
}

/// Writes a weapon id into the record's two id bytes.
///
/// @param[in,out] weapon record whose id is written
/// @param id id from 0 to 65535
static inline void oa_weapon_def_set_id(WeaponDef* weapon, uint16_t id) {
    weapon->weapon_id = (uint8_t)(id & 0xffu);
    weapon->weapon_id_high = (uint8_t)((id >> 8) & 0xffu);
}

OA_CORE_END

#endif

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <array>
#include <cstdint>
#include <optional>

namespace oa::sim::unit_movement {
using Fixed = int32_t; // Signed 16.16 world coordinates/speeds.
// Bits of Unit.flags that movement reads and writes; they mirror the core
// bits OA_UNIT_FLAG_OCCUPANCY_MASK and OA_UNIT_FLAG_POSITION_DIRTY.
inline constexpr uint32_t occupancy_mask = 3;
inline constexpr uint32_t position_dirty = 0x10000;
// Bit of Movement.flags: the last collision test refused the next cell.
inline constexpr uint8_t collision_blocked = 4;
// UnitDef.flags bits exempting a unit from the underwater speed penalty:
// hovercraft and floating units (OA_UNIT_DEF_FLAG_CAN_HOVER and
// OA_UNIT_DEF_FLAG_FLOATER).
inline constexpr uint32_t water_speed_exemption_mask = 0x81000;

// The UnitDef fields movement reads.
struct Type {
    Fixed maximum_speed{};   // UnitDef.max_velocity
    uint16_t maximum_turn{}; // UnitDef.turn_rate
    uint32_t flags{};        // UnitDef.flags
};

// The Unit fields movement reads and writes.
struct Unit {
    std::array<Fixed, 3> position{};    // Unit.position
    std::array<int16_t, 2> cell{};      // Unit.cell_x, cell_z
    std::array<int16_t, 2> footprint{}; // Unit.footprint_x, footprint_z
    int16_t id{};                       // Unit.id; the collision query skips this unit
    uint16_t heading{};                 // Unit.heading
    int16_t pitch{};                    // Unit.pitch
    uint32_t flags{};                   // Unit.flags
    Type type{};
};

// The movement object's motion state.
struct Movement {
    std::array<Fixed, 3> velocity{}; // added to the position each tick
    Fixed speed{};
    int16_t turn{};              // heading change of the last turn
    uint32_t last_motion_tick{}; // tick it last moved or changed layer; the bob decays from it
    uint8_t flags{};             // occupancy in bits 0..1, collision_blocked
};

// Only spatial-world operations remain at a required boundary.
struct Host {
    virtual ~Host() = default;
    /// Tests whether the unit's footprint may move into a cell.
    ///
    /// @param unit moving unit
    /// @param cell footprint origin cell to test
    /// @param occupancy occupancy kind the unit will have (Movement.flags bits 0..1)
    /// @return true when the move is not blocked
    virtual bool can_occupy(const Unit& unit, std::array<int16_t, 2> cell, uint8_t occupancy) = 0;
    /// Clears the unit's plot occupancy before it moves.
    ///
    /// @param unit moving unit, still at its old cell
    virtual void remove_occupancy(Unit& unit) = 0;
    /// Registers the unit's plot occupancy at its new cell.
    ///
    /// @param unit moved unit
    virtual void insert_occupancy(Unit& unit) = 0;
    /// Moves the unit's line-of-sight stamp to its new position.
    ///
    /// @param unit moved unit
    virtual void update_spatial_membership(Unit& unit) = 0;
};

/// Returns the signed 1.13 table sine of a heading scaled by a magnitude.
///
/// The heading indexes the 512-entry table with the game's bias; the product is
/// rounded and shifted down 13 bits.
///
/// @param heading 16-bit angle, 65536 per turn
/// @param magnitude signed 16.16 value to scale
/// @return magnitude * sine(heading), signed 16.16, low 32 bits
Fixed sine_scaled(uint16_t heading, Fixed magnitude) noexcept;
/// Returns the table cosine of a heading scaled by a magnitude.
///
/// @param heading 16-bit angle, 65536 per turn
/// @param magnitude signed 16.16 value to scale
/// @return magnitude * cosine(heading), signed 16.16
Fixed cosine_scaled(uint16_t heading, Fixed magnitude) noexcept;
/// Returns the velocity for a heading, pitch and speed.
///
/// Y is sine(pitch) * speed; X and Z are the negated sine and cosine of heading
/// scaled by cosine(pitch) * speed. Ground velocity is this with pitch zero.
///
/// @param heading 16-bit angle, 65536 per turn
/// @param pitch 16-bit angle, 65536 per turn
/// @param speed signed 16.16 world units per tick
/// @return {X, Y, Z} in signed 16.16 world units per tick
std::array<Fixed, 3> aim_velocity(uint16_t heading, uint16_t pitch, Fixed speed) noexcept;
/// Tests whether a facing is within a right angle of the bearing to a point.
///
/// Used by aircraft attack alignment.
///
/// @param from_x signed 16.16 X of the viewer
/// @param from_z signed 16.16 Z of the viewer
/// @param to_x signed 16.16 X of the target
/// @param to_z signed 16.16 Z of the target
/// @param facing 16-bit heading, 65536 per turn
/// @return true when the table dot product of facing and bearing is positive
bool facing_toward(
    int32_t from_x, int32_t from_z, int32_t to_x, int32_t to_z, uint16_t facing
) noexcept;

struct AimAngles {
    uint16_t heading{};
    uint16_t pitch{};
};

/// Returns the heading and pitch along the segment from one point to another.
///
/// Heading is the direction of the negated X/Z delta; pitch is the direction of
/// the negated Y high word over the horizontal distance high word.
///
/// @param from signed 16.16 start point
/// @param to signed 16.16 end point
/// @return 16-bit heading and pitch
AimAngles aim_angles(std::array<Fixed, 3> from, std::array<Fixed, 3> to) noexcept;
/// Turns a unit toward a requested heading delta, clamped to the type turn rate.
///
/// A zero request only clears the stored turn; any other request turns the unit
/// and marks its position dirty.
///
/// @param[in,out] unit unit whose heading changes
/// @param[in,out] movement movement object; its turn records the applied delta
/// @param requested heading delta, 65536 per turn
void turn(Unit& unit, Movement& movement, int16_t requested) noexcept;
/// Changes speed within the slope and water speed limit and rebuilds the ground velocity.
///
/// The limit is the type's maximum speed times a percentage chosen by the pitch
/// bucket (pitch >> 11, clamped to -5..5), halved under water unless the type
/// is a hovercraft or floats.
///
/// @param unit unit whose type, pitch, height and heading are read
/// @param[in,out] movement movement object whose speed and velocity are set
/// @param acceleration signed 16.16 speed change per tick
/// @param sea_level map sea level in whole world units
/// @quirk The limit's product is narrowed to 32 bits between its two 64-bit operations.
void accelerate(Unit& unit, Movement& movement, Fixed acceleration, uint8_t sea_level) noexcept;
/// Returns the distance needed to stop from a speed at a deceleration.
///
/// @param speed signed 16.16 world units per tick
/// @param deceleration signed 16.16 world units per tick per tick
/// @return speed^2 / (2 * deceleration) in 16.16 world units, the square narrowed to 32 bits;
///         nullopt when the doubled deceleration wraps to zero
[[nodiscard]] std::optional<int64_t> braking_distance(Fixed speed, Fixed deceleration) noexcept;
/// Integrates the position of an unattached unit for one tick.
///
/// Adds the velocity. A move within the same cell and occupancy only updates the
/// position. A move into another cell asks the host for a collision test when
/// the unit is simulated here, keeping the stored blocked bit otherwise; an
/// unblocked move updates occupancy and spatial membership in the game's
/// remove/write/insert/dirty/update order, a blocked one clamps X and Z to the
/// current cell interior and drops the speed to half the type maximum. The
/// caller must establish that the unit is unattached.
///
/// @param[in,out] unit unit to move
/// @param[in,out] movement movement object: velocity, speed, occupancy and blocked bits, last motion tick
/// @param tick current game tick, stored as the last motion tick
/// @param local_simulation whether this machine simulates the unit's owner
/// @param host collision test and spatial updates
/// @quirk The cell clamp keeps the game's upper-first signed comparisons, including wraparound.
void integrate_unattached(
    Unit& unit, Movement& movement, uint32_t tick, bool local_simulation, Host& host
);

/// Writes an absolute position, updating occupancy when the cell or occupancy changes.
///
/// Makes no collision query.
///
/// @param[in,out] unit unit to place
/// @param movement movement object (unused)
/// @param position signed 16.16 world position
/// @param occupancy occupancy kind; only bits 0..1 are used
/// @param host spatial updates
void write_position(
    Unit& unit, Movement& movement, std::array<Fixed, 3> position, uint8_t occupancy, Host& host
);
} // namespace oa::sim::unit_movement

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What the battlefield shows between two ticks. A frame drawn a fraction of
// the way from one tick to the next shows each unit's place, heading and
// script-moved pieces, each projectile's flight and each debris piece's fall
// that fraction of the way from the tick before to the current tick, never
// past it. A piece that is spinning follows that spin, including a turn of
// more than half a revolution, so the spin never reads as turning back. "The
// tick before" is the last tick the presentation saw before the
// current one: the tick before it, or, when one frame's clock step ran a
// batch of ticks (above normal speed), the tick before the batch. The poses
// of the tick before are copies the presentation takes as it sees each tick;
// a draw between ticks works on copies of the unit records and model
// instances, and never writes the match's own. The debug grid's random
// numbers are kept from a tick's first draw for its later draws.
#pragma once

#include "oa/core/projectile.h"
#include "oa/core/unit.h"
#include "oa/formats/objects3d.hpp"
#include "oa/present/model/model_draw.hpp"
#include "oa/sim/effect_particles.hpp"
#include "oa/sim/model_runtime/instance.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>
#include <vector>

namespace oa::app {

/// A whole tick in the parts a frame between two ticks is placed by: a frame
/// at whole_tick shows the current tick as it is, one at 0 the tick before.
inline constexpr uint32_t whole_tick = 0x10000;

/// The farthest a unit moves along any axis from one tick the presentation
/// saw to the next and still shows between its two places, in map pixels. A
/// larger move is a jump (a unit placed, loaded or dropped) and shows where
/// it lands.
inline constexpr int32_t unit_jump_pixels = 64;

/// The most ticks one frame's clock step runs (the game loop keeps at most
/// five steps pending). The fraction a frame is drawn at spans the batch of
/// ticks the step ran, so poses seen this many ticks apart or fewer show
/// between each other; poses further apart (a match loaded, or ticks run with
/// no frame drawn) show the later one as it is.
inline constexpr uint32_t most_batch_ticks = 5;

/// Returns the ticks from one tick the presentation saw to the next, when
/// the frames between them may show the way from the one to the other.
///
/// @param seen the tick seen before
/// @param tick the tick seen now
/// @return tick - seen when it is 1 to most_batch_ticks (the difference wraps
///     at 32 bits); 0 for the same tick, an earlier one or one further on
[[nodiscard]] uint32_t batch_ticks(uint32_t seen, uint32_t tick) noexcept;

/// Returns the part of a tick a presentation alpha places a frame at.
///
/// @param alpha 0 for the tick before, 1 for the current tick; values
///        outside 0..1 are clamped, and a value that is no number is a whole tick
/// @return 0 to whole_tick, rounded to the nearest part
[[nodiscard]] uint32_t tick_fraction(float alpha) noexcept;

/// Returns a 16.16 coordinate part of the way from its value at the tick
/// before to its value at the current tick.
///
/// The step between the two wraps at 32 bits, as the match's coordinates
/// do, and its part is rounded toward negative infinity.
///
/// @param previous the value at the tick before
/// @param current the value at the current tick
/// @param fraction the part of the way, 0 to whole_tick
/// @return `previous` at 0, `current` at whole_tick
[[nodiscard]] int32_t blend_fixed(int32_t previous, int32_t current, uint32_t fraction) noexcept;

/// Returns an angle word part of the way along the shorter turn from its
/// value at the tick before to its value at the current tick.
///
/// A turn of exactly half a circle goes the negative way.
///
/// @param previous the angle at the tick before, 65536 a turn
/// @param current the angle at the current tick
/// @param fraction the part of the way, 0 to whole_tick
/// @return `previous` at 0, `current` at whole_tick
[[nodiscard]] int16_t blend_angle(int16_t previous, int16_t current, uint32_t fraction) noexcept;

/// Returns a 16.16 point part of the way from the tick before to the current tick.
///
/// @param previous the point at the tick before
/// @param current the point at the current tick
/// @param fraction the part of the way, 0 to whole_tick
/// @return each coordinate as blend_fixed gives it
[[nodiscard]] FixedVec3
blend_point(const FixedVec3& previous, const FixedVec3& current, uint32_t fraction) noexcept;

/// Returns where a point that moves one step a tick shows part of the way
/// through the ticks since the tick before.
///
/// @param point the point at the current tick
/// @param step the step each tick moves it, 16.16
/// @param ticks the ticks since the tick before (batch_ticks), at least 1
/// @param fraction the part of the way, 0 to whole_tick
/// @return blend_point from `point` less `ticks` steps to `point`
[[nodiscard]] FixedVec3 point_along_step(
    const FixedVec3& point, const FixedVec3& step, uint32_t ticks, uint32_t fraction
) noexcept;

/// Returns an angle word that turns one step a tick part of the way through
/// the ticks since the tick before.
///
/// @param angle the angle at the current tick, 65536 a turn
/// @param step the turn each tick adds
/// @param ticks the ticks since the tick before (batch_ticks), at least 1
/// @param fraction the part of the way, 0 to whole_tick
/// @return `angle` less the part of `ticks` steps the fraction leaves to come
[[nodiscard]] int16_t
angle_along_step(int16_t angle, int16_t step, uint32_t ticks, uint32_t fraction) noexcept;

/// Returns an angle word part of the way from one tick to the next for one
/// axis of a piece.
///
/// When `speed_per_tick` over `ticks` lands on `current`, the angle follows
/// that turn, including one of more than half a revolution. A speed above
/// half a revolution a tick that does not land there shows the current angle.
/// Any other turn takes the shorter way, as blend_angle does.
///
/// @param previous the angle at the tick before, 65536 a turn
/// @param current the angle at the current tick
/// @param speed_per_tick the turn the axis adds each tick; 0 when it is still
/// @param ticks the ticks from the tick before to the current tick
/// @param fraction the part of the way, 0 to whole_tick
/// @return `previous` at 0, `current` at whole_tick
[[nodiscard]] int16_t blend_piece_turn(
    int16_t previous, int16_t current, int32_t speed_per_tick, uint32_t ticks, uint32_t fraction
) noexcept;

/// What a unit's script left of one piece at a tick.
struct PiecePose {
    oa::formats::objects3d::FixedVector3 translation{};
    oa::sim::model_runtime::RotationWords rotation{};
    /// The turn each axis adds per tick, in the rotation word's order
    /// (xy, xz, yz), 65536 a full turn. Zero when that axis is still.
    std::array<int32_t, 3> turn_per_tick{};
};

/// What a draw shows of one unit at a tick: its place, its orientation, the
/// unit carrying it and its pieces' poses.
struct UnitPose {
    FixedVec3 position{};
    uint16_t heading{}; ///< Unit.heading
    int16_t pitch{};    ///< Unit.pitch
    int16_t bank{};     ///< Unit.bank
    oa_ref32 attach_parent{};
    std::vector<PiecePose> pieces; ///< in the instance's piece order
};

/// Tells whether a slot's model instance is its unit's own, as spawn binds
/// it: made for the unit (its owner token) with a piece for each object of
/// its model, or with none (no model, or one whose hierarchy make_instance
/// refused). The motion reads the pieces of an instance only when it is.
///
/// @param instance the slot's model instance
/// @param owner the slot's unit, as spawn gives the instance its owner token
/// @return true when the instance fits the unit
[[nodiscard]] bool
instance_fits_unit(const oa::sim::model_runtime::Instance& instance, const void* owner) noexcept;

/// Takes a unit's pose.
///
/// @param unit the unit record
/// @param instance its model instance
/// @param[out] pose the pose; its piece list keeps its memory
/// @param piece_turns three turn-per-tick words a piece, in xy, xz, yz
///        order; a short span leaves the remaining axes still
void capture_unit_pose(
    const oa::Unit& unit,
    const oa::sim::model_runtime::Instance& instance,
    UnitPose& pose,
    std::span<const int32_t> piece_turns = {}
);

/// Tells whether two poses are the same.
///
/// @param a a pose
/// @param b another pose
/// @return true when every coordinate, angle and piece pose is equal
[[nodiscard]] bool poses_equal(const UnitPose& a, const UnitPose& b) noexcept;

/// Tells whether a unit went on from one pose to the next: neither a jump
/// further than unit_jump_pixels, nor a change of the unit carrying it, nor
/// another piece list.
///
/// @param previous the pose at the tick before
/// @param current the pose at the current tick
/// @return true when a frame between the two may show the unit between them
[[nodiscard]] bool pose_continues(const UnitPose& previous, const UnitPose& current) noexcept;

/// Places copies of a unit's record and model instance part of the way from
/// one pose to the next.
///
/// The record's position, heading, pitch and bank, and each piece's
/// translation and rotation, are blended; every other field and each piece's
/// flags stay as the copies hold them, which are the current tick's. The
/// instance's transforms are left as they were: the caller rebuilds them.
///
/// @param previous the pose at the tick before
/// @param current the pose at the current tick; it has the instance's pieces
/// @param fraction the part of the way, 0 to whole_tick
/// @param[in,out] record a copy of the unit's record
/// @param[in,out] instance a copy of the unit's model instance
/// @param ticks the ticks from the tick before to the current tick; a
///        spinning piece follows its turn per tick over these
void blend_unit_pose(
    const UnitPose& previous,
    const UnitPose& current,
    uint32_t fraction,
    oa::Unit& record,
    oa::sim::model_runtime::Instance& instance,
    uint32_t ticks = 1
);

/// Places the pieces of a copy of a unit's model instance part of the way
/// from one pose to the next: each piece's translation and rotation is
/// blended, a spinning axis along its spin, and its flags stay as the copy
/// holds them.
///
/// @param previous the pose at the tick before
/// @param current the pose at the current tick; it has the instance's pieces
/// @param fraction the part of the way, 0 to whole_tick
/// @param[in,out] instance a copy of the unit's model instance
/// @param ticks the ticks from the tick before to the current tick
void blend_unit_pieces(
    const UnitPose& previous,
    const UnitPose& current,
    uint32_t fraction,
    oa::sim::model_runtime::Instance& instance,
    uint32_t ticks = 1
);

/// One unit slot's poses at the last two ticks the presentation saw, and the
/// copies a draw between those ticks draws the unit from.
struct UnitMotion {
    UnitMotion() = default;
    // The copies' cached image would point into the source's pixels.
    UnitMotion(const UnitMotion&) = delete;
    UnitMotion& operator=(const UnitMotion&) = delete;
    UnitMotion(UnitMotion&&) noexcept = default;
    UnitMotion& operator=(UnitMotion&&) noexcept = default;

    uint32_t instance_generation{}; ///< the slot's SlotRuntime::instance_generation
    uint32_t tick{};                ///< the tick `current` was taken at
    bool seen{};                    ///< `current` holds the unit's pose
    /// `previous` holds the pose of the tick seen before `tick`, at most
    /// most_batch_ticks earlier, and the unit went on from it (pose_continues).
    bool continued{};
    bool moved{}; ///< continued, and the two poses differ
    /// `previous` holds the pose of the tick seen before `tick`, with the
    /// same piece list, and the pieces' poses differ: a unit drawn elsewhere
    /// than its record (on its playout) shows its pieces between them even
    /// when it jumped.
    bool pieces_moved{};
    UnitPose previous{};
    UnitPose current{};
    // The copies a draw between ticks draws, and their own draw state: the
    // match's record, instance and draw state are never touched by them.
    oa::Unit record{};
    oa::sim::model_runtime::Instance instance{};
    oa::present::model::ModelState state{};
    uint64_t blended_draw{}; ///< the draw the copies were placed for; 0 for none
};

// The slot list grows by moving its motions.
static_assert(std::is_nothrow_move_constructible_v<UnitMotion>);

/// Notes a unit's pose at a tick.
///
/// A unit of another instance generation than the one noted starts afresh,
/// with draw state of its own. At a tick up to most_batch_ticks after the
/// one noted (batch_ticks), the noted pose becomes the previous one; at any
/// other new tick the unit has no previous pose. The tick noted already
/// changes nothing, including piece rates changed by a script query later
/// during that same tick. Visible piece changes are compared while capturing
/// the current pose; changing rates alone does not mark a piece as moved.
///
/// @param[in,out] motion the slot's motion
/// @param tick the match's tick
/// @param instance_generation the slot's SlotRuntime::instance_generation
/// @param unit the unit record
/// @param instance its model instance
/// @param piece_turns three turn-per-tick words a piece, in xy, xz, yz
///        order; empty when the unit's pieces are still
void observe_unit(
    UnitMotion& motion,
    uint32_t tick,
    uint32_t instance_generation,
    const oa::Unit& unit,
    const oa::sim::model_runtime::Instance& instance,
    std::span<const int32_t> piece_turns = {}
);

/// Forgets the unit of a slot that holds none: its poses, the copies of its
/// record and model instance and their draw state, with the memory they hold.
///
/// @param[in,out] motion the slot's motion; empty after the call
void forget_unit(UnitMotion& motion) noexcept;

/// What a draw shows of one projectile, and what tells it from another.
struct ShotPose {
    FixedVec3 position{}; ///< Projectile.position, its head
    FixedVec3 origin{};   ///< Projectile.origin, a beam's tail
    uint16_t heading{};
    uint16_t pitch{};
    oa_ref32 def{};
    oa_ref32 source{};
    uint32_t created_tick{};
};

/// The projectile pool as the presentation saw it at the last two ticks, by
/// pool index.
struct ShotFlight {
    uint32_t tick{};          ///< the tick `current` was taken at
    uint32_t previous_tick{}; ///< the tick `previous` was taken at, when continued
    bool seen{};              ///< `current` holds the pool
    /// `previous` holds the pool at the tick seen before `tick`, at most
    /// most_batch_ticks earlier.
    bool continued{};
    std::vector<ShotPose> previous;
    std::vector<ShotPose> current;
    /// By index in `current`: the index in `previous` of the same projectile,
    /// or -1 for none (a projectile fired since, or not found).
    std::vector<int32_t> previous_index;
};

/// Takes a projectile's pose.
///
/// @param shot the projectile
/// @return its pose
[[nodiscard]] ShotPose shot_pose(const oa::Projectile& shot) noexcept;

/// Notes the projectile pool at a tick, and finds each projectile in the
/// pool as it was noted before.
///
/// From the tick before, a projectile is found where the end of the tick
/// moved it from (Projectile.compact_index, its index before the pool closed
/// its gaps) and checked by its weapon, source and tick of creation. Across a
/// batch of ticks, which closed the gaps more than once, it is the nearest
/// unclaimed projectile of the same weapon, source and tick of creation.
///
/// @param[in,out] flight the pool's flight
/// @param tick the match's tick
/// @param shots the live projectiles
void observe_shots(ShotFlight& flight, uint32_t tick, std::span<const oa::Projectile> shots);

/// Returns where a projectile shows part of the way through the current tick.
///
/// A projectile observe_shots found in the pool as noted before shows
/// between its two poses. One fired since shows between its muzzle
/// (Projectile.origin) and its head. Any other shows as it is.
///
/// @param flight the pool's flight, noted at `tick`
/// @param tick the match's tick
/// @param index the projectile's index in the pool, as noted at `tick`
/// @param shot the projectile
/// @param fraction the part of the way, 0 to whole_tick
/// @return the pose to draw
[[nodiscard]] ShotPose presented_shot(
    const ShotFlight& flight,
    uint32_t tick,
    std::size_t index,
    const oa::Projectile& shot,
    uint32_t fraction
) noexcept;

/// What a draw shows of one debris slot at a tick, and what tells one piece
/// from another.
struct DebrisPose {
    bool live{};
    uint16_t unit{};
    uint32_t piece{};
    const oa::formats::objects3d::Model* model{};
    uint32_t object{};
    FixedVec3 position{};
    std::array<int16_t, 3> spin{};
};

/// The debris table as the presentation saw it at the last two ticks, by slot.
struct DebrisFall {
    uint32_t tick{}; ///< the tick `current` was taken at
    bool seen{};     ///< `current` holds the table
    /// `previous` holds the table at the tick seen before `tick`, at most
    /// most_batch_ticks earlier.
    bool continued{};
    std::array<DebrisPose, oa::sim::effect_particles::debris_capacity> previous{};
    std::array<DebrisPose, oa::sim::effect_particles::debris_capacity> current{};
};

/// Notes the debris table at a tick.
///
/// @param[in,out] fall the table's fall
/// @param tick the match's tick
/// @param debris the debris table, at most debris_capacity slots
void observe_debris(
    DebrisFall& fall, uint32_t tick, std::span<const oa::sim::effect_particles::DebrisPiece> debris
);

/// Returns a copy of a debris piece placed part of the way through the current tick.
///
/// A piece its slot held at the tick before shows between its two places
/// and spins; any other shows as it is.
///
/// @param fall the table's fall, noted at `tick`
/// @param tick the match's tick
/// @param slot the piece's slot in the debris table
/// @param piece the piece
/// @param fraction the part of the way, 0 to whole_tick
/// @return the piece to draw
[[nodiscard]] oa::sim::effect_particles::DebrisPiece presented_debris(
    const DebrisFall& fall,
    uint32_t tick,
    std::size_t slot,
    const oa::sim::effect_particles::DebrisPiece& piece,
    uint32_t fraction
) noexcept;

/// The debug grid's random numbers. They come from the grid's own generator,
/// never from the match's streams, so that drawing the grid never changes
/// the game. The numbers the first draw of a tick drew are drawn again in
/// turn by every later draw of the tick, so that a tick drawn more than once
/// shows one grid.
struct DebugGridRandom {
    /// The generator: each step multiplies by `multiplier` and adds
    /// `increment`; a number is the state's bits from `shift` up, under `mask`.
    static constexpr uint32_t multiplier = 214013;
    static constexpr uint32_t increment = 2531011;
    static constexpr uint32_t shift = 16;
    static constexpr uint32_t mask = 0x7fff;
    static constexpr uint32_t first_state = 1;

    uint32_t tick{};
    bool drawn{}; ///< `values` holds the numbers of `tick`
    bool first{}; ///< the draw under way is the tick's first, which takes new numbers
    std::vector<int32_t> values;
    std::size_t next{};          ///< the value a later draw of the tick takes next
    uint32_t state{first_state}; ///< the generator's state

    /// Steps the grid's own generator.
    ///
    /// @return 0 to 32767
    [[nodiscard]] int32_t generate() noexcept {
        state = state * multiplier + increment;
        return static_cast<int32_t>((state >> shift) & mask);
    }
};

/// Starts a draw of the debug grid at a tick: the tick's first draw forgets
/// the numbers kept and takes new ones; a later draw repeats them from the first.
///
/// @param[in,out] kept the numbers kept
/// @param tick the match's tick
void start_debug_grid_draw(DebugGridRandom& kept, uint32_t tick) noexcept;

/// Returns the debug grid's next random number in the draw under way.
///
/// The tick's first draw takes it from `stream` and keeps it; a later draw
/// takes the kept numbers in turn, from the first again once they run out,
/// and 0 when none were kept.
///
/// @param[in,out] kept the numbers kept, the draw started (start_debug_grid_draw)
/// @param stream gives a new number (DebugGridRandom::generate)
/// @return the number
template <typename Stream>
[[nodiscard]] int32_t next_debug_grid_number(DebugGridRandom& kept, Stream&& stream) {
    if (kept.first) {
        const int32_t value = stream();
        kept.values.push_back(value);
        return value;
    }
    if (kept.values.empty())
        return 0;
    const int32_t value = kept.values[kept.next % kept.values.size()];
    ++kept.next;
    return value;
}

} // namespace oa::app

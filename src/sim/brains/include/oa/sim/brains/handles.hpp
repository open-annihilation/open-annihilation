// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A unit handle is a slot and the generation the match already keeps for it.
// One Lua object stands for one slot and generation, so handles work as
// table keys. A later spawn in the same slot is a different handle.
#pragma once

#include <cstdint>

struct lua_State;

namespace oa::sim::match_runtime {
class Match;
}

namespace oa::sim::brains {

struct Seat;

/// The slot and generation one unit handle names.
struct UnitRef {
    /// Unit slot. Slot 0 is the reserved empty slot.
    uint16_t slot{};
    /// Units made in `slot` so far, from the match's slot runtime.
    uint32_t generation{};
};

/// Returns the generation the match keeps for `slot`.
///
/// The count is `SlotRuntime::instance_generation`. A slot outside the pool
/// reads as 0.
///
/// @param match the match that owns the slot
/// @param slot unit slot
/// @return the generation, or 0 outside the pool
[[nodiscard]] uint32_t unit_generation(match_runtime::Match& match, uint16_t slot) noexcept;

/// Returns the handle identity of whatever `slot` holds now.
///
/// @param match the match that owns the slot
/// @param slot unit slot
/// @return the slot with its current generation
[[nodiscard]] UnitRef unit_ref(match_runtime::Match& match, uint16_t slot) noexcept;

/// Reports whether `ref` still names the unit in its slot.
///
/// Current means the slot holds a unit (`type_index` is not 0) and that
/// unit's generation is `ref.generation`.
///
/// @param match the match that owns the slot
/// @param ref handle identity
/// @return true when the slot still holds that unit
[[nodiscard]] bool unit_current(match_runtime::Match& match, UnitRef ref) noexcept;

/// Pushes the one handle object of `ref`.
///
/// The seat keeps that object in a registry table, by slot. A later
/// generation replaces the table entry. The previous object stays reachable
/// when Lua kept it, and it is not the same object. May raise.
///
/// @param state sandbox state
/// @param seat the seat whose registry holds the handle
/// @param ref slot and generation
void push_unit(lua_State* state, Seat& seat, UnitRef ref);

/// Reads a unit handle at `index` without raising.
///
/// A value that is not a unit handle leaves `ref` unchanged and returns
/// false.
///
/// @param state sandbox state
/// @param index stack index
/// @param ref receives the slot and generation when the value is a handle
/// @return true when index holds a unit handle
[[nodiscard]] bool to_unit(lua_State* state, int index, UnitRef* ref) noexcept;

} // namespace oa::sim::brains

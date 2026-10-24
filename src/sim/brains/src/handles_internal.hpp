// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Registry tables, access classes and the helpers unit handles and `me` share.
// Later tickets add getters and methods here; they do not grow a second
// metatable.
#pragma once

#include "oa/sim/brains/handles.hpp"

#include "oa/core/types.h"

#include "lua.hpp"

#include <cstddef>

struct lua_State;

namespace oa {
struct World;
}

namespace oa::sim::brains {

/// Who may read one unit field.
enum class Access : uint8_t {
    always, ///< any handle, including a dead one and another player's
    seen,   ///< the seat's own live unit, or another player's when it may be seen
    own,    ///< the seat's own live unit
};

/// How a seen or own read came out. `dead` is only for the seat's own slot
/// once that generation is gone; `alive` is the field that turns it into false.
enum class Read : uint8_t {
    allow,
    none,
    dead,
};

/// Installs the unit metatable, the handle cache and the unit fields.
///
/// Works when `seat` has no match. May raise.
///
/// @param state sandbox state
/// @param seat the seat being opened
void install_handles(lua_State* state, Seat& seat);

/// Makes `me` read-only and installs the seat's side, squads and economy.
///
/// B01's player, level, difficulty and options stay readable. Works when
/// `seat` has no match. May raise.
///
/// @param state sandbox state
/// @param seat the seat being opened
void install_me(lua_State* state, Seat& seat);

/// Adds a unit getter. A name already used as a getter or a method raises.
///
/// @param state sandbox state
/// @param name field name
/// @param access who may read it
/// @param function the getter; it is called with the handle at index 1
void add_unit_getter(lua_State* state, const char* name, Access access, lua_CFunction function);

/// Adds a unit method. A name already used as a getter or a method raises.
///
/// The method is called only when `access` allows the handle. Otherwise the
/// call returns nil and does not check its other arguments.
///
/// @param state sandbox state
/// @param name method name
/// @param access who may call it
/// @param function the method; the handle is at index 1
void add_unit_method(lua_State* state, const char* name, Access access, lua_CFunction function);

/// Adds a getter of `me`. A name already used as a getter or a method raises.
///
/// @param state sandbox state
/// @param name field name
/// @param function the getter
void add_me_getter(lua_State* state, const char* name, lua_CFunction function);

/// Adds a method of `me`. A name already used as a getter or a method raises.
///
/// @param state sandbox state
/// @param name method name
/// @param function the method; `me` is at index 1
void add_me_method(lua_State* state, const char* name, lua_CFunction function);

/// Reports whether the seat owns the unit `ref` names.
///
/// Owned means the handle is current and `Unit.owner_index` is the seat's
/// player.
///
/// @param seat the seat
/// @param ref handle identity
/// @return true when the seat owns that live unit
[[nodiscard]] bool seat_owns(const Seat& seat, UnitRef ref) noexcept;

/// Reports how `access` applies to `ref` for this seat.
///
/// @param seat the seat
/// @param ref handle identity
/// @param access the field's class
/// @return allow, none, or dead
[[nodiscard]] Read unit_access(const Seat& seat, UnitRef ref, Access access) noexcept;

/// Whether the seat may read a seen field of a unit it does not own.
///
/// True when the seat has full awareness, or when it can see the unit now.
/// An uninstalled seat still reads false. A throw from the match reads false.
extern bool (*seat_may_see)(const Seat& seat, UnitRef ref);

/// Remembers `seat` for the getters and methods of this state.
///
/// @param state sandbox state
/// @param seat the seat
void store_seat(lua_State* state, Seat& seat);

/// Returns the seat stored for this state.
///
/// @param state sandbox state
/// @return the seat, or null when none was stored
[[nodiscard]] Seat* seat_of(lua_State* state) noexcept;

/// Reads a position table at `index`.
///
/// `x` and `z` are required. A missing `y` is 0. A component is a fixed
/// value or a whole number of world units. Raises `<function> expects a
/// position: a table with x and z`, or `<function>: x must be a fixed value
/// or a whole number` (and the same for `y` and `z`).
///
/// @param state sandbox state
/// @param index stack index
/// @param function name used in the error
/// @param out receives the point
/// @return false after a raising call, so the caller returns as well
bool read_position(lua_State* state, int index, const char* function, oa::FixedVec3* out);

/// Pushes `{x = , y = , z = }` as fixed values.
///
/// May raise.
///
/// @param state sandbox state
/// @param point the point
void push_position(lua_State* state, const oa::FixedVec3& point);

/// Raises `<function> expects a unit` when index is not a handle.
///
/// @param state sandbox state
/// @param index stack index
/// @param function name used in the error
/// @param ref receives the handle
/// @return 0 when index is a handle; does not return when it is not
int check_unit(lua_State* state, int index, const char* function, UnitRef* ref);

/// Pushes the bytes of `text` up to `room`, not reading a missing terminator.
///
/// @param state sandbox state
/// @param text the characters
/// @param room how many bytes `text` may hold
void push_text(lua_State* state, const char* text, std::size_t room);

/// The world a seat reads. A match seat uses the match. A tool seat uses the
/// world its calls carry.
///
/// @param seat the seat
/// @return the world, or null when the seat has neither
[[nodiscard]] oa::World* seat_world(const Seat& seat) noexcept;

/// A handle for `slot` in this seat's world.
///
/// A match seat keeps the match's generation. A tool seat's units are
/// generation 0.
///
/// @param seat the seat
/// @param slot unit slot
/// @return the handle
[[nodiscard]] UnitRef seat_unit_ref(const Seat& seat, uint16_t slot) noexcept;

/// Whether `ref` names a unit that is still in this seat's world.
///
/// A match seat uses the match's generation. A tool seat treats a filled
/// slot of generation 0 as current, including a dying unit.
///
/// @param seat the seat
/// @param ref handle identity
/// @return true when the unit is current
[[nodiscard]] bool unit_current_for_seat(const Seat& seat, UnitRef ref) noexcept;

} // namespace oa::sim::brains

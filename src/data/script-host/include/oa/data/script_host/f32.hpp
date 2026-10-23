// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Single-precision values for scripts. One value holds the bits of an IEEE
// binary32 number, the width of the engine's resources and build progress.
#pragma once

#include <cstdint>
#include <optional>

struct lua_State;

namespace oa::data::script_host {

/// Opens the single-precision library and sets the global `f32`.
///
/// A profile's `install` calls this. A sandbox that does not call it has no
/// `f32`. May raise.
///
/// @param state sandbox state
void open_f32_library(lua_State* state);

/// Pushes an f32 value holding `value`.
///
/// Allocates a userdata, so this may raise. The bits are stored as given,
/// including a NaN's payload and a zero's sign.
///
/// @param state sandbox state
/// @param value the float to store
void push_f32(lua_State* state, float value);

/// Reads an f32 value at `index` without raising.
///
/// A value that is not an f32 userdata leaves `value` unchanged and returns
/// false. The library does not have to be open: a foreign userdata is false.
///
/// @param state sandbox state
/// @param index stack index
/// @param value receives the float when the value is an f32
/// @return true when index holds an f32 value
[[nodiscard]] bool to_f32(lua_State* state, int index, float* value) noexcept;

/// Converts an integer to a float in one rounding.
///
/// The integer is converted directly. It is not converted to a double first,
/// which would round a second time above 2^53. The rounding is nearest,
/// ties to even.
///
/// @param value the integer
/// @return the float
[[nodiscard]] float f32_from_integer(int64_t value) noexcept;

/// Converts a double to a float in one rounding.
///
/// The rounding is nearest, ties to even. A NaN becomes the quiet NaN whose
/// bits are 0x7fc00000, so every build stores the same result.
///
/// @param value the double
/// @return the float
[[nodiscard]] float f32_from_double(double value) noexcept;

/// Converts a fixed count to a float.
///
/// The count is divided by 65536. For a count in the fixed range that
/// quotient is exact, and the conversion to float is the one rounding.
///
/// @param count count of 65536ths
/// @return the float
[[nodiscard]] float f32_from_fixed_count(int64_t count) noexcept;

/// Converts a float to a fixed count.
///
/// The float is widened, which is exact, multiplied by 65536 and truncated
/// toward zero, as `fixed(number)` does.
///
/// @param value the float
/// @return the count, or no value when value is not finite or the count is
///         outside the fixed range
[[nodiscard]] std::optional<int64_t> f32_to_fixed_count(float value) noexcept;

} // namespace oa::data::script_host

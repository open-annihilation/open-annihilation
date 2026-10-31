// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

namespace oa::base::game_math {
/// A finite binary floating-point number with a 64-bit significand.
///
/// The game keeps the results of its arctangent, sine and cosine, and some
/// intermediate steps, at this precision before rounding them to a double.
/// The exponent range is wide enough that no value the simulation computes
/// overflows or becomes subnormal. Every operation here is integer arithmetic,
/// so results are the same on every platform and compiler.
struct Extended {
    /// Bit 63 is set unless the value is zero.
    uint64_t significand{};
    /// The value is significand * 2^(exponent - 63).
    int32_t exponent{};
    /// The sign; a zero keeps its sign.
    bool negative{};
};

/// Significand widths a result is rounded to, to nearest with ties to even.
enum class Precision : uint8_t {
    /// 53 bits, the width of a double.
    bits_53 = 53,
    /// 64 bits, the width of Extended.
    bits_64 = 64,
};

/// Converts a finite double exactly.
///
/// @param value finite value
/// @return the same value
[[nodiscard]] Extended to_extended(double value) noexcept;

/// Converts a 32-bit integer exactly.
///
/// @param value integer
/// @return the same value; zero is positive
[[nodiscard]] Extended to_extended(int32_t value) noexcept;

/// Rounds to a double, to nearest with ties to even.
///
/// Values beyond double's range become infinite, and values below its
/// normal range are rounded to its subnormal precision.
///
/// @param value value to round
/// @return the nearest double
[[nodiscard]] double to_double(Extended value) noexcept;

/// Rounds to a narrower significand, keeping the wide exponent range.
///
/// @param value value to round
/// @param precision significand width of the result
/// @return value rounded to nearest, ties to even
[[nodiscard]] Extended round_to(Extended value, Precision precision) noexcept;

/// Changes the sign.
///
/// @param value value to negate
/// @return -value; a zero changes sign too
[[nodiscard]] Extended negate(Extended value) noexcept;

/// Compares two values; zeros of either sign are equal.
///
/// @param a left operand
/// @param b right operand
/// @return -1 when a < b, 0 when a == b, 1 when a > b
[[nodiscard]] int compare(Extended a, Extended b) noexcept;

/// Adds, rounding the exact sum once.
///
/// @param a left operand
/// @param b right operand
/// @param precision significand width of the result
/// @return a + b rounded to nearest, ties to even; an exact zero sum is positive
///         unless both operands are negative zeros
[[nodiscard]] Extended add(Extended a, Extended b, Precision precision) noexcept;

/// Subtracts, rounding the exact difference once.
///
/// @param a left operand
/// @param b right operand
/// @param precision significand width of the result
/// @return a - b rounded as add rounds
[[nodiscard]] Extended subtract(Extended a, Extended b, Precision precision) noexcept;

/// Multiplies, rounding the exact product once.
///
/// @param a left operand
/// @param b right operand
/// @param precision significand width of the result
/// @return a * b rounded to nearest, ties to even
[[nodiscard]] Extended multiply(Extended a, Extended b, Precision precision) noexcept;

/// Multiplies by an integer and rounds the exact product to a double.
///
/// The result is to_double(multiply(factor, to_extended(value),
/// Precision::bits_53)) for every factor and value, worked out without the
/// general operations wherever the product is a normal double.
///
/// @param factor left operand
/// @param value integer right operand
/// @return the product rounded as that expression rounds it; a zero product
///         is negative when exactly one operand is negative, as multiply's is
[[nodiscard]] double multiply_to_double(Extended factor, int32_t value) noexcept;

/// Divides, rounding the exact quotient once.
///
/// @param a dividend
/// @param b divisor; must not be zero
/// @param precision significand width of the result
/// @return a / b rounded to nearest, ties to even
[[nodiscard]] Extended divide(Extended a, Extended b, Precision precision) noexcept;

/// Takes the square root, rounding the exact root once.
///
/// @param value radicand; must not be negative
/// @param precision significand width of the result
/// @return the square root rounded to nearest, ties to even; the root of a
///         zero is that zero
[[nodiscard]] Extended square_root(Extended value, Precision precision) noexcept;

/// Returns the angle of the point (x, y) from the positive x axis.
///
/// @param y finite ordinate
/// @param x finite abscissa
/// @return the exact angle in radians, in [-pi, pi], rounded to a 64-bit
///         significand, nearest with ties to even. A zero y gives a zero of
///         y's sign when x is positive or a positive zero, and pi of y's sign
///         when x is negative or a negative zero; a zero x with a non-zero y
///         gives pi/2 of y's sign.
[[nodiscard]] Extended arctangent(double y, double x) noexcept;

/// Returns an estimate of the angle of the point (x, y) from the positive x
/// axis, quickly.
///
/// @param y finite ordinate
/// @param x finite abscissa
/// @return within 2^-50 of arctangent(y, x); the same on every platform
[[nodiscard]] double approximate_arctangent(double y, double x) noexcept;

/// The sine and cosine of one angle.
struct SineCosine {
    Extended sine{};
    Extended cosine{};
};

/// Returns the sine and cosine of an angle.
///
/// @param radians angle; its magnitude must be below 2^31
/// @return what sine and cosine return for the angle
[[nodiscard]] SineCosine sine_cosine(double radians) noexcept;

/// Returns the sine of an angle.
///
/// @param radians angle; its magnitude must be below 2^31
/// @return the exact sine rounded to a 64-bit significand, nearest with ties
///         to even; the sine of a zero is that zero
[[nodiscard]] Extended sine(double radians) noexcept;

/// Returns the cosine of an angle.
///
/// @param radians angle; its magnitude must be below 2^31
/// @return the exact cosine rounded to a 64-bit significand, nearest with ties
///         to even
[[nodiscard]] Extended cosine(double radians) noexcept;
} // namespace oa::base::game_math

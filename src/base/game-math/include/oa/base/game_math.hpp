// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/base/game_math/extended.hpp"

#include <cstdint>
#include <optional>

namespace oa::base::game_math {
/// Draws a bounded number from the game's shared random generator.
///
/// A Park-Miller style step: state * 16807 - (state / 127773) * (2^31 - 1),
/// with every intermediate wrapping at 32 bits and a non-positive result
/// raised by 2^31 - 1.
///
/// @param[in,out] state shared generator state, owned by the caller
/// @param bound number of possible results
/// @return state % bound, or 0 without advancing the state when bound, read
///         as signed, is below 2
uint32_t random_bounded(uint32_t& state, uint32_t bound) noexcept;
/// Seeds the game's shared random generator; no host randomness is introduced.
///
/// @param[out] state generator state
/// @param input seed value; it is XORed with a fixed mask and forced odd
void seed_random(uint32_t& state, uint32_t input) noexcept;
/// Returns the heading of the planar vector (x, z).
///
/// The angle arctangent(x, z), kept at a 64-bit significand, is multiplied by the
/// game's radians-to-heading constant with the product rounded to a 53-bit
/// significand, then rounded to the nearest integer, ties to even.
///
/// @param x east-west component
/// @param z north-south component
/// @return the heading's low 16 bits, 65536 per turn (COB GET selectors 12
///         and 14, unit steering, terrain slopes and weapon aiming use it)
uint16_t direction(int32_t x, int32_t z) noexcept;

/// Returns the length of the vector (x, y) as the game measures it.
///
/// The magnitudes are divided by the larger one, squared, summed and
/// square-rooted, and the root is multiplied by the larger magnitude. Each of
/// those steps is rounded first to a 64-bit and then to a 53-bit significand,
/// nearest with ties to even at both widths; the second rounding can differ
/// from rounding the exact value once.
///
/// @param x first component; finite
/// @param y second component; finite
/// @return the length; zero when both components are zero
[[nodiscard]] double hypotenuse(double x, double y) noexcept;

/// Returns the angle whose cosine is x, as the game computes it.
///
/// The angle is arctangent(sqrt((1 + x) * (1 - x)), x), with the sum, the
/// difference, the product and the root each rounded to a double and the
/// arctangent kept at a 64-bit significand. x == 1 gives zero and x == -1 gives
/// pi at a 64-bit significand.
///
/// @param x cosine
/// @return the angle in [0, pi], or no value when x is NaN or its magnitude
///         exceeds 1
[[nodiscard]] std::optional<Extended> arccosine(double x) noexcept;

/// Returns the square root of a double, rounded to nearest with ties to even.
///
/// @param value radicand
/// @return the correctly rounded root; NaN for a negative radicand
[[nodiscard]] double square_root(double value) noexcept;

/// A pair of coordinates rotated in their plane, before rounding to integers.
struct RotatedPair {
    double first{};
    double second{};
};

/// Rotates the integer pair (first, second) by an angle word.
///
/// The angle is the word times the game's radians-per-word constant, rounded
/// to a double. Its sine and cosine are kept at a 64-bit significand; each of
/// the four products with a coordinate is rounded to a 53-bit significand, and
/// so is each of the two sums.
///
/// @param first first coordinate
/// @param second second coordinate
/// @param angle angle word, 65536 per turn
/// @return first * cos - second * sin and first * sin + second * cos
[[nodiscard]] RotatedPair rotate_pair(int32_t first, int32_t second, int16_t angle) noexcept;

/// Coefficients for rounding rotated integer coordinates.
///
/// Keep this value unchanged after prepare_integer_rotation returns it, and
/// use it before changing the calling thread's floating-point rounding mode.
struct PreparedIntegerRotation {
    int16_t angle{};         ///< angle word, 65536 per turn
    double sine{};           ///< sine rounded to a double
    double cosine{1.0};      ///< cosine rounded to a double
    bool estimate_allowed{}; ///< double estimates may settle integer rounding
};

/// A rotated pair after rounding each coordinate to a signed 64-bit integer.
struct IntegerPair {
    int64_t first{};
    int64_t second{};
};

/// Prepares an angle for repeated integer rotations on the calling thread.
///
/// Estimates are used only with nearest rounding and binary64 operations.
/// Other floating-point settings keep the ordinary extended products.
///
/// @param angle angle word, 65536 per turn
/// @return coefficients kept unchanged until all their rotations finish
[[nodiscard]] PreparedIntegerRotation prepare_integer_rotation(int16_t angle) noexcept;

/// Rotates a pair and rounds its coordinates as rotate_pair followed by llrint.
///
/// A double estimate settles a coordinate only when its entire error interval
/// falls strictly inside one integer's nearest-rounding interval. Otherwise
/// the ordinary extended products decide both coordinates. This preserves the
/// integer results; floating-point exception flags are not preserved.
///
/// @param first first coordinate
/// @param second second coordinate
/// @param rotation unchanged result of prepare_integer_rotation, under the
///     same floating-point rounding mode
/// @return each rotated coordinate rounded using the current rounding mode
[[nodiscard]] IntegerPair rotate_pair_rounded(
    int32_t first, int32_t second, const PreparedIntegerRotation& rotation
) noexcept;
/// Returns the length of the planar vector (x, z), truncated.
///
/// @param x east-west component
/// @param z north-south component
/// @return planar_length truncated toward zero to 64 bits, low 32 bits kept
///         (COB GET selectors 13 and 15 use it)
uint32_t distance(int32_t x, int32_t z) noexcept;
/// Returns the length of the planar vector (x, z) as a double.
///
/// The length is hypotenuse(x, z).
///
/// @param x east-west component
/// @param z north-south component
/// @return the length before truncation
[[nodiscard]] double planar_length(int32_t x, int32_t z) noexcept;
/// Scales a 16.16 vector by a 16.16 factor.
///
/// @param[out] out scaled vector; may alias in
/// @param in vector to scale, 16.16 fixed point
/// @param scale factor, 16.16 fixed point
/// @quirk Each component uses a 64-bit multiply and an arithmetic shift right by
///        16, so negative results round toward negative infinity.
void scale_vector_fixed(int32_t out[3], const int32_t in[3], int32_t scale) noexcept;
/// Returns the length of (x, y, z) truncated toward zero.
///
/// @param x first component
/// @param y second component
/// @param z third component
/// @return the length truncated to 64 bits, low 32 bits kept
/// @quirk The squares are summed as z^2 + y^2 before x^2, which can land on a
///        different double than x^2 first.
/// @quirk A length past 2^31 wraps negative instead of saturating.
[[nodiscard]] int32_t truncated_length(int32_t x, int32_t y, int32_t z) noexcept;

/// Returns the sum of the high 32 bits of the three signed squares.
///
/// Projectile collision compares this with a range squared.
///
/// @param x first component
/// @param y second component
/// @param z third component
/// @return (x*x >> 32) + (y*y >> 32) + (z*z >> 32)
[[nodiscard]] int32_t squared_magnitude_high(int32_t x, int32_t y, int32_t z) noexcept;

// 8-byte max-heap node. The float is the key; the 32-bit word is the payload.
struct FloatHeapPair {
    uint32_t value{};
    float key{};
};

/// Moves a hole up a max-heap while the parent key is smaller, then writes a pair there.
///
/// @param[in,out] heap heap array
/// @param hole index of the empty slot
/// @param lower subtree root the hole must not climb past
/// @param value payload of the new pair
/// @param key key of the new pair
void float_heap_sift_up(
    FloatHeapPair* heap, int32_t hole, int32_t lower, uint32_t value, float key
) noexcept;

/// Fills a hole in a max-heap with a new pair.
///
/// Pulls the larger child into the hole down to the bottom, then places the
/// pair with float_heap_sift_up.
///
/// @param[in,out] heap heap array
/// @param hole index of the empty slot; also the subtree root the pair may not climb past
/// @param size one past the last occupied index
/// @param value payload of the new pair
/// @param key key of the new pair
/// @quirk On equal child keys the right child is taken.
void float_heap_replace(
    FloatHeapPair* heap, int32_t hole, int32_t size, uint32_t value, float key
) noexcept;

/// Copies the root pair out, then re-fills the root with a new pair.
///
/// @param[in,out] heap heap array
/// @param size element count passed to float_heap_replace
/// @param[out] out receives the root pair; written before the heap changes
/// @param value payload of the new pair
/// @param key key of the new pair
void float_heap_extract(
    FloatHeapPair* heap, int32_t size, FloatHeapPair* out, uint32_t value, float key
) noexcept;

/// Orders pairs into a max-heap.
///
/// Re-places each parent from the last one (size / 2 - 1) down to the root.
///
/// @param[in,out] heap heap array
/// @param size number of pairs
void float_heap_make(FloatHeapPair* heap, int32_t size) noexcept;

/// Moves the root pair to the last slot and re-heaps the others.
///
/// @param[in,out] heap heap array
/// @param size number of pairs, at least 1
void float_heap_pop(FloatHeapPair* heap, int32_t size) noexcept;

/// Truncates a double toward zero to a signed 64-bit integer.
///
/// @param value value to convert
/// @return the integer; INT64_MIN for NaN, an infinity or a value outside
///         [-2^63, 2^63)
[[nodiscard]] constexpr int64_t truncate_to_int64(double value) noexcept {
    constexpr double limit = 9223372036854775808.0; // 2^63
    if (!(value >= -limit && value < limit))
        return INT64_MIN;
    return static_cast<int64_t>(value);
}

/// Converts a double to an integer as the game's float-to-integer steps do:
/// truncated toward zero to 64 bits, the low 32 bits kept.
///
/// A float argument converts to double exactly, so this serves float values
/// too.
///
/// @param value value to convert
/// @return the low 32 bits of truncate_to_int64(value), read as signed; 0 for
///         NaN, an infinity or a value outside [-2^63, 2^63), whose 64-bit
///         result INT64_MIN has a low word of 0
[[nodiscard]] constexpr int32_t truncate_low32(double value) noexcept {
    return static_cast<int32_t>(
        static_cast<uint32_t>(static_cast<uint64_t>(truncate_to_int64(value)))
    );
}
} // namespace oa::base::game_math

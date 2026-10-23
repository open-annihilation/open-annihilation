// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// The integer arithmetic of a format-1 network. A stored q stands for
// q times 2^exponent. Nothing here uses a fractional type.

#include <cstddef>
#include <cstdint>

namespace oa::data::neural {

/// How many operations make one charge unit.
///
/// A network's charge is the operations in one run, divided by this, rounded up.
inline constexpr uint32_t operations_per_charge = 64;

/// Most operations one run may perform, 2^28.
///
/// A kernel runs to its end once started, so this bounds one run. A network
/// over it is refused when it is built.
inline constexpr uint64_t max_operations_per_run = uint64_t{1} << 28;

/// Most bytes of networks one brain may hold, 64 MiB.
///
/// `NetworkSet` enforces this unless a caller passes a smaller cap.
inline constexpr std::size_t max_brain_network_bytes = std::size_t{64} * 1024 * 1024;

/// Moves an integer from one power-of-two scale to another.
///
/// `from` and `to` are exponents: a stored q stands for q times 2^exponent.
/// With d = to - from, a positive d divides by 2^d, rounding halves toward
/// plus infinity. A negative d multiplies by 2^(-d) and holds the result at
/// the signed 64-bit limits. A zero d returns `value`.
///
/// @param value the integer at exponent from
/// @param from the exponent value is stored at
/// @param to the exponent to move it to
/// @return the integer at exponent to
[[nodiscard]] constexpr int64_t rescale(int64_t value, int32_t from, int32_t to) noexcept {
    const int64_t places = static_cast<int64_t>(to) - static_cast<int64_t>(from);
    if (places == 0)
        return value;
    if (places > 0) {
        // Any int64 divided by 2^64 or more, with halves toward plus infinity, is 0.
        if (places >= 64)
            return 0;
        const int shift = static_cast<int>(places);
        const int64_t bias = int64_t{1} << (shift - 1);
        if (value < 0)
            return (value + bias) >> shift;
        const uint64_t widened = static_cast<uint64_t>(value) + static_cast<uint64_t>(bias);
        return static_cast<int64_t>(widened >> static_cast<unsigned>(shift));
    }

    const int64_t shift = -places;
    constexpr int64_t k_min = -9223372036854775807LL - 1;
    constexpr int64_t k_max = 9223372036854775807LL;
    if (value == 0)
        return 0;
    // 1 << 63 does not fit in a positive 64-bit value. -1 << 63 is the minimum.
    if (shift >= 63) {
        if (value > 0)
            return k_max;
        if (value == -1 && shift == 63)
            return k_min;
        return k_min;
    }
    const int amount = static_cast<int>(shift);
    const int64_t positive_limit = k_max >> amount;
    const int64_t negative_limit = k_min >> amount;
    if (value > positive_limit)
        return k_max;
    if (value < negative_limit)
        return k_min;
    // A signed left shift of a negative value is undefined. The checks above
    // make the product representable, so shifting the two's-complement bits
    // as an unsigned value is that product.
    const auto bits = static_cast<uint64_t>(value);
    return static_cast<int64_t>(bits << static_cast<unsigned>(amount));
}

/// Holds a 64-bit value to the int16 range.
///
/// @param value the wide value
/// @return value, or the nearer of -32768 and 32767 when it is outside
[[nodiscard]] constexpr int16_t saturate_int16(int64_t value) noexcept {
    if (value > 32767)
        return 32767;
    if (value < -32768)
        return static_cast<int16_t>(-32768);
    return static_cast<int16_t>(value);
}

/// Holds a 64-bit value to the int32 range.
///
/// @param value the wide value
/// @return value, or the nearer int32 limit when it is outside
[[nodiscard]] constexpr int32_t saturate_int32(int64_t value) noexcept {
    constexpr int64_t k_min = -2147483647LL - 1;
    constexpr int64_t k_max = 2147483647LL;
    if (value > k_max)
        return static_cast<int32_t>(k_max);
    if (value < k_min)
        return static_cast<int32_t>(k_min);
    return static_cast<int32_t>(value);
}

static_assert(rescale(3, 0, 1) == 2);
static_assert(rescale(-3, 0, 1) == -1);
static_assert(rescale(-3, 2, 0) == -12);
static_assert(rescale(1, 0, -63) == 9223372036854775807LL);
static_assert(rescale(-1, 0, -63) == -9223372036854775807LL - 1);
static_assert(max_operations_per_run == 268435456);
static_assert(max_brain_network_bytes == 67108864);

} // namespace oa::data::neural

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/base/game_math.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace oa::base::game_math {
namespace {
// The exact binary64 constants 3.1c uses; do not replace them with ones
// derived from a different pi.
constexpr double radians_to_heading = 10430.37835047;
constexpr double radians_per_angle_word = 9.587379924285e-05;
// How close to a half-integer a heading estimate may come before the exact
// heading decides; far wider than the estimate's error.
constexpr double heading_estimate_margin = 0x1p-28;

// Constants of the game's 32-bit wrapped generator. The seed XOR value is a
// scrambling mask with no known external meaning.
constexpr uint32_t random_multiplier = 16807;
constexpr uint32_t random_quotient_divisor = 127773;
constexpr uint32_t random_modulus = 0x7fffffff;
constexpr uint32_t seed_scrambling_mask = 0x66e29572;
constexpr uint32_t force_odd_seed_bit = 1;

// Separate stores preserve binary64 operations and prevent fused multiply-add.
double rounded(double value) noexcept {
    volatile double result = value;
    return result;
}

// The sine and cosine of the angle of every angle word from 0 to 32768. The
// angle of a negative word is the negative of its magnitude's angle, so its
// sine is negated and its cosine the same.
constexpr std::size_t angle_word_entries = 32769;

struct AngleWordTable {
    std::array<uint64_t, angle_word_entries> sine_significand{};
    std::array<uint64_t, angle_word_entries> cosine_significand{};
    std::array<int16_t, angle_word_entries> sine_exponent{};
    std::array<int16_t, angle_word_entries> cosine_exponent{};
    std::array<uint8_t, angle_word_entries> cosine_negative{};

    AngleWordTable() noexcept {
        for (std::size_t word = 0; word < angle_word_entries; ++word) {
            const double radians = rounded(static_cast<double>(word) * radians_per_angle_word);
            const SineCosine values = sine_cosine(radians);
            sine_significand[word] = values.sine.significand;
            sine_exponent[word] = static_cast<int16_t>(values.sine.exponent);
            cosine_significand[word] = values.cosine.significand;
            cosine_exponent[word] = static_cast<int16_t>(values.cosine.exponent);
            cosine_negative[word] = values.cosine.negative ? 1 : 0;
        }
    }
};

// Built once, on first use; the values never change afterwards.
SineCosine angle_word_sine_cosine(int16_t angle) noexcept {
    static const AngleWordTable table;
    const bool negative = angle < 0;
    const auto word = static_cast<std::size_t>(negative ? -int32_t{angle} : int32_t{angle});
    return {
        {table.sine_significand[word], table.sine_exponent[word], negative},
        {table.cosine_significand[word],
         table.cosine_exponent[word],
         table.cosine_negative[word] != 0}
    };
}
} // namespace

uint32_t random_bounded(uint32_t& state, uint32_t bound) noexcept {
    if (std::bit_cast<int32_t>(bound) < 2)
        return 0;
    state = state * random_multiplier - (state / random_quotient_divisor) * random_modulus;
    if (std::bit_cast<int32_t>(state) < 1)
        state += random_modulus;
    return state % bound;
}

void seed_random(uint32_t& state, uint32_t input) noexcept {
    state = (input ^ seed_scrambling_mask) | force_odd_seed_bit;
}

uint16_t direction(int32_t x, int32_t z) noexcept {
    // An estimate within 2^-35 of the heading settles the nearest integer
    // unless it lies within heading_estimate_margin of a half-integer.
    const double estimate =
        approximate_arctangent(static_cast<double>(x), static_cast<double>(z)) * radians_to_heading;
    const double estimate_lower = std::floor(estimate);
    const double estimate_fraction = estimate - estimate_lower;
    if (std::fabs(estimate_fraction - 0.5) > heading_estimate_margin)
        return static_cast<uint16_t>(
            static_cast<int32_t>(estimate_lower + (estimate_fraction > 0.5 ? 1.0 : 0.0))
        );
    const double angle = to_double(multiply(
        arctangent(static_cast<double>(x), static_cast<double>(z)),
        to_extended(radians_to_heading),
        Precision::bits_53
    ));
    const double lower = std::floor(angle), fraction = angle - lower;
    const auto nearest =
        lower + ((fraction > 0.5 || (fraction == 0.5 && std::fmod(lower, 2.0) != 0)) ? 1.0 : 0.0);
    return static_cast<uint16_t>(static_cast<int32_t>(nearest));
}

std::optional<Extended> arccosine(double x) noexcept {
    if (!(x >= -1.0 && x <= 1.0))
        return std::nullopt;
    if (x == 1.0)
        return to_extended(0.0);
    if (x == -1.0)
        return arctangent(0.0, -1.0);
    // Sum, difference, product and root are each rounded to a double.
    const double sum = rounded(1.0 + x), difference = rounded(1.0 - x);
    return arctangent(square_root(rounded(sum * difference)), x);
}

double square_root(double value) noexcept {
    // IEEE 754 rounds the square root exactly, like the four basic operations.
    return std::sqrt(value);
}

RotatedPair rotate_pair(int32_t first, int32_t second, int16_t angle) noexcept {
    const SineCosine angle_values = angle_word_sine_cosine(angle);
    const Extended sine_value = angle_values.sine, cosine_value = angle_values.cosine;
    const auto product = [](Extended factor, int32_t coordinate) {
        return multiply_to_double(factor, coordinate);
    };
    return {
        rounded(product(cosine_value, first) - product(sine_value, second)),
        rounded(product(sine_value, first) + product(cosine_value, second))
    };
}

double planar_length(int32_t x, int32_t z) noexcept {
    return hypotenuse(static_cast<double>(x), static_cast<double>(z));
}

uint32_t distance(int32_t x, int32_t z) noexcept {
    return static_cast<uint32_t>(static_cast<uint64_t>(planar_length(x, z)));
}

void scale_vector_fixed(int32_t out[3], const int32_t in[3], int32_t scale) noexcept {
    for (int i = 0; i < 3; ++i) {
        const auto product = static_cast<int64_t>(in[i]) * static_cast<int64_t>(scale);
        out[i] = static_cast<int32_t>(product >> 16);
    }
}

int32_t truncated_length(int32_t x, int32_t y, int32_t z) noexcept {
    const auto square = [](int32_t v) {
        return rounded(static_cast<double>(v) * static_cast<double>(v));
    };
    const double sum = rounded(rounded(square(z) + square(y)) + square(x));
    const auto wide = static_cast<int64_t>(square_root(sum));
    return static_cast<int32_t>(static_cast<uint32_t>(static_cast<uint64_t>(wide)));
}

int32_t squared_magnitude_high(int32_t x, int32_t y, int32_t z) noexcept {
    const auto high = [](int32_t value) {
        return static_cast<int32_t>(
            static_cast<uint64_t>(static_cast<int64_t>(value) * static_cast<int64_t>(value)) >> 32
        );
    };
    return high(x) + high(y) + high(z);
}

void float_heap_sift_up(
    FloatHeapPair* heap, int32_t hole, int32_t lower, uint32_t value, float key
) noexcept {
    while (lower < hole && heap[(hole - 1) / 2].key < key) {
        const auto parent = (hole - 1) / 2;
        heap[hole] = heap[parent];
        hole = parent;
    }
    heap[hole].value = value;
    heap[hole].key = key;
}

void float_heap_replace(
    FloatHeapPair* heap, int32_t start, int32_t size, uint32_t value, float key
) noexcept {
    auto hole = start;
    auto child = 0;
    while (true) {
        const auto doubled = hole * 2;
        child = doubled + 2;
        if (size <= child)
            break;
        if (heap[child].key < heap[child - 1].key)
            child = doubled + 1;
        heap[hole] = heap[child];
        hole = child;
    }
    if (child == size) {
        heap[hole] = heap[child - 1];
        hole = hole * 2 + 1;
    }
    float_heap_sift_up(heap, hole, start, value, key);
}

void float_heap_extract(
    FloatHeapPair* heap, int32_t size, FloatHeapPair* out, uint32_t value, float key
) noexcept {
    *out = *heap;
    float_heap_replace(heap, 0, size, value, key);
}

void float_heap_make(FloatHeapPair* heap, int32_t size) noexcept {
    for (auto hole = size / 2; hole > 0;) {
        --hole;
        const auto pair = heap[hole];
        float_heap_replace(heap, hole, size, pair.value, pair.key);
    }
}

void float_heap_pop(FloatHeapPair* heap, int32_t size) noexcept {
    const auto last = heap[size - 1];
    float_heap_extract(heap, size - 1, &heap[size - 1], last.value, last.key);
}
} // namespace oa::base::game_math

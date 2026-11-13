// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/base/game_math.hpp"
#include <bit>
#include <cfenv>
#include <cfloat>
#include <cmath>
#include <cstdint>
#include <iostream>
#include <limits>
#include <source_location>
#include <stdexcept>
#include <string>

namespace {

/// Checks truncate_to_int64 and truncate_low32 at zero, the halves, the
/// 32-bit, 32-bit-plus-one and 64-bit edges, and NaN and the infinities.
void truncation() {
    using oa::base::game_math::truncate_low32;
    using oa::base::game_math::truncate_to_int64;
    constexpr double two_31 = 2147483648.0;
    constexpr double two_32 = 4294967296.0;
    constexpr double two_63 = 9223372036854775808.0;
    constexpr double nan = std::numeric_limits<double>::quiet_NaN();
    constexpr double infinity = std::numeric_limits<double>::infinity();
    static_assert(truncate_to_int64(0.5) == 0 && truncate_to_int64(-0.5) == 0);
    static_assert(truncate_to_int64(1.75) == 1 && truncate_to_int64(-1.75) == -1);
    static_assert(truncate_low32(0.5) == 0 && truncate_low32(-0.5) == 0);
    // 2^31 wraps to INT32_MIN; -2^31 is INT32_MIN itself.
    static_assert(truncate_to_int64(two_31) == 2147483648LL);
    static_assert(truncate_low32(two_31) == INT32_MIN && truncate_low32(-two_31) == INT32_MIN);
    // 2^32 + 1 keeps 1; its negation keeps -1.
    static_assert(truncate_low32(two_32 + 1.0) == 1 && truncate_low32(-(two_32 + 1.0)) == -1);
    // -2^63 is in range; 2^63 is not.
    static_assert(truncate_to_int64(-two_63) == INT64_MIN && truncate_low32(-two_63) == 0);
    static_assert(truncate_to_int64(two_63) == INT64_MIN && truncate_low32(two_63) == 0);
    static_assert(truncate_to_int64(9223372036854774784.0) == 9223372036854774784LL);
    static_assert(truncate_low32(9223372036854774784.0) == static_cast<int32_t>(0xFFFFFC00u));
    static_assert(truncate_to_int64(nan) == INT64_MIN && truncate_low32(nan) == 0);
    static_assert(truncate_to_int64(infinity) == INT64_MIN && truncate_low32(infinity) == 0);
    static_assert(truncate_to_int64(-infinity) == INT64_MIN && truncate_low32(-infinity) == 0);
    // The same at run time, where the compiler cannot fold the conversions.
    volatile double runtime_nan = nan;
    volatile double runtime_wrap = two_32 + 7.9;
    if (truncate_low32(runtime_nan) != 0 || truncate_to_int64(runtime_nan) != INT64_MIN ||
        truncate_low32(runtime_wrap) != 7)
        throw std::runtime_error("run-time truncation differs");
    // A float converts to double exactly.
    if (truncate_low32(3.75F) != 3 || truncate_low32(-3.75F) != -3)
        throw std::runtime_error("float truncation differs");
}

/// Reports a failed rotation check with its source location.
void rotation_check(
    bool passed,
    const char* expression,
    std::source_location location = std::source_location::current()
) {
    if (!passed)
        throw std::runtime_error(
            std::string{location.file_name()} + ":" + std::to_string(location.line()) + ": " +
            expression
        );
}

/// Restores the rounding mode when the rotation checks finish or fail.
struct RestoreRoundingMode {
    int mode{};

    /// Restores the mode that was active before the checks.
    ~RestoreRoundingMode() { std::fesetround(mode); }
};

/// Returns the next signed coordinate from a deterministic local bit stream.
int32_t next_rotation_coordinate(uint32_t& bits) {
    constexpr int first_left_shift = 13;
    constexpr int right_shift = 17;
    constexpr int second_left_shift = 5;
    bits ^= bits << first_left_shift;
    bits ^= bits >> right_shift;
    bits ^= bits << second_left_shift;
    return std::bit_cast<int32_t>(bits);
}

/// Rounds one estimate operation to a double.
double stored_rotation_estimate(double value) {
    volatile double stored = value;
    return stored;
}

/// Checks whether a prepared estimate reaches a half-integer's error margin.
bool rotation_estimate_near_half(
    int32_t first, int32_t second, const oa::base::game_math::PreparedIntegerRotation& rotation
) {
    const double cosine_first =
        stored_rotation_estimate(rotation.cosine * static_cast<double>(first));
    const double sine_second =
        stored_rotation_estimate(rotation.sine * static_cast<double>(second));
    const double sine_first = stored_rotation_estimate(rotation.sine * static_cast<double>(first));
    const double cosine_second =
        stored_rotation_estimate(rotation.cosine * static_cast<double>(second));
    const double estimated_first = stored_rotation_estimate(cosine_first - sine_second);
    const double estimated_second = stored_rotation_estimate(sine_first + cosine_second);
    constexpr double margin_scale = 0x1p-49;
    const double margin = margin_scale * (std::fabs(static_cast<double>(first)) +
                                          std::fabs(static_cast<double>(second)) + 1.0);
    constexpr double half_integer = 0.5;
    return std::fabs(estimated_first - static_cast<double>(std::llrint(estimated_first))) >=
               half_integer - margin ||
           std::fabs(estimated_second - static_cast<double>(std::llrint(estimated_second))) >=
               half_integer - margin;
}

/// Checks integer rotation against the rounded double coordinates and current mode.
oa::base::game_math::IntegerPair check_integer_rotation(
    int32_t first,
    int32_t second,
    const oa::base::game_math::PreparedIntegerRotation& rotation,
    int mode
) {
    const auto coordinates = oa::base::game_math::rotate_pair(first, second, rotation.angle);
    const int64_t expected_first = std::llrint(coordinates.first);
    const int64_t expected_second = std::llrint(coordinates.second);
    const auto actual = oa::base::game_math::rotate_pair_rounded(first, second, rotation);
    const bool equal = actual.first == expected_first && actual.second == expected_second;
    if (!equal)
        std::cerr << "angle " << rotation.angle << ", coordinates " << first << ", " << second
                  << ", mode " << mode << ": expected " << expected_first << ", " << expected_second
                  << ", got " << actual.first << ", " << actual.second << '\n';
    rotation_check(equal, "rotate_pair_rounded equals rotate_pair followed by llrint");
    rotation_check(std::fegetround() == mode, "integer rotation preserves the rounding mode");
    return actual;
}

/// Checks every angle word, integer-coordinate edges and all four rounding modes.
void integer_rotation() {
    const int initial_mode = std::fegetround();
    rotation_check(initial_mode != -1, "the initial rounding mode is available");
    const RestoreRoundingMode restore{initial_mode};
    constexpr int32_t minimum = std::numeric_limits<int32_t>::min();
    constexpr int32_t maximum = std::numeric_limits<int32_t>::max();
    // Equal coordinates at angle 8192 put one result within the error margin
    // of 384199200.5; the sign variants exercise both rotated coordinates.
    constexpr int32_t near_half_coordinate = 271669860;
    constexpr int32_t coordinate_pairs[][2]{
        {0, 0},
        {1, 0},
        {0, -1},
        {1, 1},
        {1, -1},
        {-1, 1},
        {-1, -1},
        {3, 4},
        {-3, 4},
        {65536, 0},
        {0, -65536},
        {65537, -65535},
        {16777217, -16777217},
        {123456789, -987654321},
        {minimum, 0},
        {0, minimum},
        {maximum, 0},
        {0, maximum},
        {minimum, minimum},
        {maximum, maximum},
        {minimum, maximum},
        {maximum, minimum},
        {minimum, 1},
        {1, maximum},
        {maximum, -1},
        {-1, minimum},
        {near_half_coordinate, near_half_coordinate},
        {near_half_coordinate, -near_half_coordinate},
        {-near_half_coordinate, near_half_coordinate},
        {-near_half_coordinate, -near_half_coordinate}
    };
    uint64_t prepared_estimates{};
    uint64_t near_half_estimates{};
    uint64_t wide_results{};
    constexpr int additional_pairs_per_angle = 2;
    for (const int mode : {FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO}) {
        rotation_check(std::fesetround(mode) == 0, "each rounding mode can be selected");
        uint32_t coordinate_bits = 0x91e10da5U;
        for (int32_t word = std::numeric_limits<int16_t>::min();
             word <= std::numeric_limits<int16_t>::max();
             ++word) {
            const auto angle = static_cast<int16_t>(word);
            const auto rotation = oa::base::game_math::prepare_integer_rotation(angle);
            const auto saved_rotation = rotation;
            rotation_check(rotation.angle == angle, "the prepared angle is unchanged");
            rotation_check(
                rotation.estimate_allowed ==
                    (angle != 0 && FLT_EVAL_METHOD == 0 && mode == FE_TONEAREST),
                "nonzero angles allow estimates only with binary64 evaluation and nearest rounding"
            );
            rotation_check(std::fegetround() == mode, "preparation preserves the rounding mode");
            if (rotation.estimate_allowed)
                ++prepared_estimates;
            const auto check_pair = [&](int32_t first, int32_t second) {
                const auto actual = check_integer_rotation(first, second, rotation, mode);
                if (actual.first < minimum || actual.first > maximum || actual.second < minimum ||
                    actual.second > maximum)
                    ++wide_results;
                if (rotation.estimate_allowed &&
                    rotation_estimate_near_half(first, second, rotation))
                    ++near_half_estimates;
            };
            for (const auto& pair : coordinate_pairs)
                check_pair(pair[0], pair[1]);
            for (int pair = 0; pair < additional_pairs_per_angle; ++pair) {
                const auto first = next_rotation_coordinate(coordinate_bits);
                const auto second = next_rotation_coordinate(coordinate_bits);
                check_pair(first, second);
            }
            rotation_check(
                rotation.angle == saved_rotation.angle &&
                    std::bit_cast<uint64_t>(rotation.sine) ==
                        std::bit_cast<uint64_t>(saved_rotation.sine) &&
                    std::bit_cast<uint64_t>(rotation.cosine) ==
                        std::bit_cast<uint64_t>(saved_rotation.cosine) &&
                    rotation.estimate_allowed == saved_rotation.estimate_allowed,
                "rotation calls preserve every prepared coefficient"
            );
        }
    }
    if (FLT_EVAL_METHOD == 0) {
        constexpr uint64_t nonzero_angle_word_count = 65535;
        rotation_check(
            prepared_estimates == nonzero_angle_word_count,
            "all nonzero angle words allow estimates"
        );
        rotation_check(near_half_estimates != 0, "near-half estimates exercise extended products");
    } else {
        rotation_check(prepared_estimates == 0, "wider evaluation keeps extended products");
    }
    rotation_check(wide_results != 0, "rotated coordinates retain values outside 32 bits");
    rotation_check(std::fesetround(initial_mode) == 0, "the initial rounding mode is restored");
}

} // namespace

int main() {
    truncation();
    integer_rotation();
    uint32_t rng_state = 1;
    if (oa::base::game_math::random_bounded(rng_state, 100) != 7 || rng_state != 16807)
        throw std::runtime_error("random first state and draw");
    if (oa::base::game_math::random_bounded(rng_state, 100) != 49 || rng_state != 282475249)
        throw std::runtime_error("random state continuity");
    const auto saved_rng = rng_state;
    for (auto bound : {0u, 1u, 0x80000000u, 0xffffffffu})
        if (oa::base::game_math::random_bounded(rng_state, bound) != 0 || rng_state != saved_rng)
            throw std::runtime_error("bounds below 2, read as signed, do not advance the state");
    rng_state = 0;
    if (oa::base::game_math::random_bounded(rng_state, 2) != 1 || rng_state != 0x7fffffffu)
        throw std::runtime_error("a zero state is raised by 2^31 - 1");
    oa::base::game_math::seed_random(rng_state, 0);
    if (rng_state != 0x66e29573u)
        throw std::runtime_error("seed transform");
    if (oa::base::game_math::direction(1, 0) != 16384 ||
        oa::base::game_math::direction(-1, 0) != 49152 ||
        oa::base::game_math::distance(3, 4) != 5 || oa::base::game_math::distance(0, 0) != 0)
        throw std::runtime_error("direction/distance endpoints");
    int32_t in[3] = {0x20000, -0x10000, 0x30000};
    int32_t out[3]{};
    oa::base::game_math::scale_vector_fixed(out, in, 0x8000);
    if (out[0] != 0x10000 || out[1] != -0x8000 || out[2] != 0x18000)
        throw std::runtime_error("half-scale");
    if (oa::base::game_math::truncated_length(3, 4, 12) != 13)
        throw std::runtime_error("3-4-12 length");
    // The length truncates to 64 bits and the low word is kept, so a length
    // past 2^31 (3719550785 here) wraps negative instead of saturating.
    if (oa::base::game_math::truncated_length(0x7fffffff, 0x7fffffff, 0x7fffffff) != -575416511)
        throw std::runtime_error("low word of the 64-bit length");
    // The squares sum exactly to 456698230^2. Adding z^2 + y^2 first lands on
    // the double nearest that sum; x^2 + y^2 first lands one double lower,
    // whose root truncates to 456698229.
    if (oa::base::game_math::truncated_length(456698220, 93580, 19410) != 456698230)
        throw std::runtime_error("z^2 + y^2 before x^2");
    if (oa::base::game_math::squared_magnitude_high(65536, 65536, 0) != 2)
        throw std::runtime_error("high squares of a unit square");
    if (oa::base::game_math::squared_magnitude_high(-65536, 0, 0) != 1)
        throw std::runtime_error("high square of a negative");
    oa::base::game_math::FloatHeapPair heap[3]{};
    oa::base::game_math::float_heap_sift_up(heap, 0, 0, 1, 1.0F);
    oa::base::game_math::float_heap_sift_up(heap, 1, 0, 2, 3.0F);
    oa::base::game_math::float_heap_sift_up(heap, 2, 0, 3, 2.0F);
    if (heap[0].value != 2 || heap[0].key != 3.0F || heap[1].value != 1 || heap[2].value != 3)
        throw std::runtime_error("max-heap insert");
    oa::base::game_math::float_heap_replace(heap, 0, 3, 4, 0.5F);
    if (heap[0].value != 3 || heap[0].key != 2.0F || heap[1].value != 1 || heap[2].value != 4 ||
        heap[2].key != 0.5F)
        throw std::runtime_error("replace root");
    oa::base::game_math::FloatHeapPair popped[3]{};
    oa::base::game_math::float_heap_sift_up(popped, 0, 0, 1, 1.0F);
    oa::base::game_math::float_heap_sift_up(popped, 1, 0, 2, 3.0F);
    oa::base::game_math::float_heap_sift_up(popped, 2, 0, 3, 2.0F);
    oa::base::game_math::float_heap_extract(popped, 2, &popped[2], popped[2].value, popped[2].key);
    if (popped[0].value != 3 || popped[0].key != 2.0F || popped[1].value != 1 ||
        popped[1].key != 1.0F || popped[2].value != 2 || popped[2].key != 3.0F)
        throw std::runtime_error("extract root");

    // Making the heap re-places parents 1 then 0; each hole sinks to the bottom
    // before the pair climbs back up.
    oa::base::game_math::FloatHeapPair made[5]{
        {10, 1.0F}, {11, 3.0F}, {12, 2.0F}, {13, 5.0F}, {14, 4.0F}
    };
    oa::base::game_math::float_heap_make(made, 5);
    const uint32_t made_values[5]{13, 14, 12, 11, 10};
    const float made_keys[5]{5.0F, 4.0F, 2.0F, 3.0F, 1.0F};
    for (int i = 0; i < 5; ++i)
        if (made[i].value != made_values[i] || made[i].key != made_keys[i])
            throw std::runtime_error("make heap");
    // Popping moves the root to the last slot; the old last pair re-enters
    // the four-element heap from the root.
    oa::base::game_math::float_heap_pop(made, 5);
    const uint32_t popped_values[5]{14, 11, 12, 10, 13};
    const float popped_keys[5]{4.0F, 3.0F, 2.0F, 1.0F, 5.0F};
    for (int i = 0; i < 5; ++i)
        if (made[i].value != popped_values[i] || made[i].key != popped_keys[i])
            throw std::runtime_error("pop heap");
    // On equal keys replacing takes the right child.
    oa::base::game_math::FloatHeapPair tied[3]{{1, 2.0F}, {2, 2.0F}, {3, 2.0F}};
    oa::base::game_math::float_heap_make(tied, 3);
    if (tied[0].value != 3 || tied[1].value != 2 || tied[2].value != 1)
        throw std::runtime_error("right child on a tie");
}

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/base/game_math/extended.hpp"
#include "oa/base/game_math.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>

#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
#include <intrin.h>
#endif

namespace oa::base::game_math {
namespace {
// Unsigned 128-bit integers built from two 64-bit halves.
struct U128 {
    uint64_t high{};
    uint64_t low{};
};

bool is_zero(U128 a) noexcept {
    return (a.high | a.low) == 0;
}

bool less(U128 a, U128 b) noexcept {
    return a.high < b.high || (a.high == b.high && a.low < b.low);
}

U128 plus(U128 a, U128 b) noexcept {
    const uint64_t low = a.low + b.low;
    return {a.high + b.high + (low < a.low ? 1 : 0), low};
}

U128 minus(U128 a, U128 b) noexcept {
    return {a.high - b.high - (a.low < b.low ? 1 : 0), a.low - b.low};
}

U128 shift_left(U128 a, unsigned count) noexcept {
    if (count == 0)
        return a;
    if (count >= 128)
        return {};
    if (count >= 64)
        return {a.low << (count - 64), 0};
    return {(a.high << count) | (a.low >> (64 - count)), a.low << count};
}

U128 shift_right(U128 a, unsigned count) noexcept {
    if (count == 0)
        return a;
    if (count >= 128)
        return {};
    if (count >= 64)
        return {0, a.high >> (count - 64)};
    return {a.high >> count, (a.low >> count) | (a.high << (64 - count))};
}

// Whether any of the lowest `count` bits is set.
bool low_bits_set(U128 a, unsigned count) noexcept {
    if (count == 0)
        return false;
    if (count >= 128)
        return !is_zero(a);
    if (count >= 64)
        return a.low != 0 || (a.high & ((uint64_t{1} << (count - 64)) - 1)) != 0;
    return (a.low & ((uint64_t{1} << count) - 1)) != 0;
}

int leading_zeros(U128 a) noexcept {
    return a.high != 0 ? std::countl_zero(a.high) : 64 + std::countl_zero(a.low);
}

#if defined(__SIZEOF_INT128__)
__extension__ using NativeU128 = unsigned __int128;
#endif

// The full product. Every branch computes the same exact value; the first two
// use a native 128-bit product where the platform has one.
U128 multiply_64(uint64_t a, uint64_t b) noexcept {
#if defined(__SIZEOF_INT128__)
    const NativeU128 product = static_cast<NativeU128>(a) * b;
    return {static_cast<uint64_t>(product >> 64), static_cast<uint64_t>(product)};
#elif defined(_MSC_VER) && (defined(_M_X64) || defined(_M_ARM64))
    return {__umulh(a, b), a * b};
#else
    constexpr uint64_t low_32_bits = 0xffffffff;
    const uint64_t a0 = a & low_32_bits, a1 = a >> 32, b0 = b & low_32_bits, b1 = b >> 32;
    const uint64_t p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    const uint64_t middle = (p00 >> 32) + (p01 & low_32_bits) + (p10 & low_32_bits);
    return {p11 + (p01 >> 32) + (p10 >> 32) + (middle >> 32), (middle << 32) | (p00 & low_32_bits)};
#endif
}

constexpr double two_to_64 = 18446744073709551616.0;

double approximate(U128 a) noexcept {
    return static_cast<double>(a.high) * two_to_64 + static_cast<double>(a.low);
}

// floor(numerator / divisor) and the remainder, for a quotient below 2^64.
// A double estimate, within 2^14 of the quotient, is corrected with exact
// remainders, so the result does not depend on how the estimate rounds.
uint64_t divide_128_by_64(U128 numerator, uint64_t divisor, uint64_t& remainder) noexcept {
    const double estimate = approximate(numerator) / static_cast<double>(divisor);
    uint64_t quotient = estimate >= two_to_64 ? ~uint64_t{0} : static_cast<uint64_t>(estimate);
    for (int pass = 0;; ++pass) {
        const U128 product = multiply_64(quotient, divisor);
        if (less(numerator, product)) {
            const U128 excess = minus(product, numerator);
            const auto steps =
                pass == 0
                    ? static_cast<uint64_t>(approximate(excess) / static_cast<double>(divisor))
                    : 0;
            quotient -= steps + 1;
            continue;
        }
        const U128 rest = minus(numerator, product);
        if (rest.high == 0 && rest.low < divisor) {
            remainder = rest.low;
            return quotient;
        }
        const auto steps =
            pass == 0 ? static_cast<uint64_t>(approximate(rest) / static_cast<double>(divisor)) : 0;
        quotient += steps > 0 ? steps : 1;
    }
}

// A product that stays below 2^128.
U128 multiply_small(U128 a, uint32_t factor) noexcept {
    const U128 low = multiply_64(a.low, factor);
    return {low.high + a.high * factor, low.low};
}

Extended zero(bool negative) noexcept {
    return {0, 0, negative};
}

Extended with_sign(Extended value, bool negative) noexcept {
    value.negative = negative;
    return value;
}

// Rounds (-1)^negative * (value + fraction) * 2^scale to the precision's
// significand width, to nearest with ties to even. `sticky` says whether the
// exact value has a non-zero fraction below bit 0 of `value`; callers that set
// it pass more bits than the precision keeps.
Extended pack(bool negative, U128 value, bool sticky, int32_t scale, Precision precision) noexcept {
    if (is_zero(value))
        return zero(negative);
    const int width = static_cast<int>(precision);
    const int top = 127 - leading_zeros(value);
    int32_t exponent = scale + top;
    uint64_t kept = 0;
    const int discarded = top + 1 - width;
    if (discarded <= 0) {
        kept = shift_left(value, static_cast<unsigned>(-discarded)).low;
    } else {
        const auto count = static_cast<unsigned>(discarded);
        kept = shift_right(value, count).low;
        const bool round_bit = (shift_right(value, count - 1).low & 1) != 0;
        const bool below_round_bit = sticky || low_bits_set(value, count - 1);
        if (round_bit && (below_round_bit || (kept & 1) != 0)) {
            ++kept;
            const bool carried_out =
                width == 64 ? kept == 0 : kept == (uint64_t{1} << static_cast<unsigned>(width));
            if (carried_out) {
                kept = uint64_t{1} << static_cast<unsigned>(width - 1);
                ++exponent;
            }
        }
    }
    return {kept << static_cast<unsigned>(64 - width), exponent, negative};
}

// ---------------------------------------------------------------------------
// Wide values: a 128-bit significand whose operations truncate, used only to
// evaluate the arctangent, sine and cosine well beyond the 64 bits their
// results keep.
struct Wide {
    U128 significand{}; // bit 127 set unless zero
    int32_t exponent{}; // value = significand * 2^(exponent - 127)
    bool negative{};
};

struct WideConstant {
    uint64_t high{};
    uint64_t low{};
    int32_t exponent{};
};

#include "wide_constants.inc"

// Table steps of the arctangent, atan(k / 256), and of the sine and cosine,
// sin(k / 64) and cos(k / 64). The series after a step stop where their next
// term falls below 2^-128 of the result.
constexpr double arctangent_step_count = 256.0;
constexpr unsigned arctangent_step_shift = 8;
constexpr int arctangent_terms = 8;
constexpr int32_t sine_step_shift = 6;
constexpr int sine_terms = 8;
constexpr int cosine_terms = 8;
// Arguments within this bound need no reduction by multiples of pi/2.
constexpr double unreduced_limit = 0.78539816339744828;
constexpr double reciprocal_half_pi = 0.63661977236758138;
// Magnitudes from 2^31 on are outside the supported argument range.
constexpr int32_t maximum_argument_exponent = 31;
// half_pi_fixed holds pi/2 with this many fraction bits.
constexpr int32_t reduction_fraction_bits = 224;

// A table constant rounded to a double, to nearest with ties to even.
constexpr double constant_double(WideConstant constant) {
    if (constant.high == 0)
        return 0.0;
    constexpr uint64_t below_double = 0x7ff, half_of_below = 0x400;
    uint64_t kept = constant.high >> 11;
    const uint64_t rest = constant.high & below_double;
    if (rest > half_of_below || (rest == half_of_below && (constant.low != 0 || (kept & 1) != 0)))
        ++kept;
    int32_t exponent = constant.exponent;
    if (kept == uint64_t{1} << 53) {
        kept >>= 1;
        ++exponent;
    }
    return std::bit_cast<double>(
        (static_cast<uint64_t>(exponent + 1023) << 52) | (kept & 0xfffffffffffffULL)
    );
}

// atan(k / 64) for k = 0..64, as doubles.
constexpr std::size_t estimate_steps = 64;
constexpr std::array<double, estimate_steps + 1> arctangent_estimate_steps = [] {
    std::array<double, estimate_steps + 1> steps{};
    for (std::size_t k = 0; k <= estimate_steps; ++k)
        steps[k] = constant_double(arctangent_steps[k * 4]);
    return steps;
}();
constexpr double pi_estimate = constant_double(pi_constant);
constexpr double half_pi_estimate = constant_double(half_pi_constant);

Wide wide(WideConstant constant, bool negative = false) noexcept {
    return {{constant.high, constant.low}, constant.exponent, negative};
}

Wide wide(Extended value) noexcept {
    return {{value.significand, 0}, value.exponent, value.negative};
}

// The value value * 2^scale, normalised.
Wide normalized(bool negative, U128 value, int32_t scale) noexcept {
    if (is_zero(value))
        return {{}, 0, negative};
    const int zeros = leading_zeros(value);
    return {shift_left(value, static_cast<unsigned>(zeros)), scale + 127 - zeros, negative};
}

Wide wide_negate(Wide value) noexcept {
    value.negative = !value.negative;
    return value;
}

Wide wide_multiply(const Wide& a, const Wide& b) noexcept {
    const bool negative = a.negative != b.negative;
    if (is_zero(a.significand) || is_zero(b.significand))
        return {{}, 0, negative};
    const U128 ll = multiply_64(a.significand.low, b.significand.low);
    const U128 lh = multiply_64(a.significand.low, b.significand.high);
    const U128 hl = multiply_64(a.significand.high, b.significand.low);
    const U128 hh = multiply_64(a.significand.high, b.significand.high);
    uint64_t word1 = ll.high, carry1 = 0;
    word1 += lh.low;
    carry1 += word1 < lh.low ? 1 : 0;
    word1 += hl.low;
    carry1 += word1 < hl.low ? 1 : 0;
    uint64_t word2 = hh.low, carry2 = 0;
    word2 += lh.high;
    carry2 += word2 < lh.high ? 1 : 0;
    word2 += hl.high;
    carry2 += word2 < hl.high ? 1 : 0;
    word2 += carry1;
    carry2 += word2 < carry1 ? 1 : 0;
    const uint64_t word3 = hh.high + carry2;
    if ((word3 >> 63) != 0)
        return {{word3, word2}, a.exponent + b.exponent + 1, negative};
    return {
        {(word3 << 1) | (word2 >> 63), (word2 << 1) | (word1 >> 63)},
        a.exponent + b.exponent,
        negative
    };
}

Wide wide_add(const Wide& a, const Wide& b) noexcept {
    if (is_zero(b.significand))
        return a;
    if (is_zero(a.significand))
        return b;
    const bool a_larger = a.exponent > b.exponent ||
                          (a.exponent == b.exponent && !less(a.significand, b.significand));
    const Wide& large = a_larger ? a : b;
    const Wide& small = a_larger ? b : a;
    const int64_t distance = int64_t{large.exponent} - small.exponent;
    const U128 aligned =
        distance >= 128 ? U128{} : shift_right(small.significand, static_cast<unsigned>(distance));
    if (large.negative == small.negative) {
        const U128 sum = plus(large.significand, aligned);
        if (less(sum, large.significand))
            return {
                {(sum.high >> 1) | (uint64_t{1} << 63), (sum.low >> 1) | (sum.high << 63)},
                large.exponent + 1,
                large.negative
            };
        return {sum, large.exponent, large.negative};
    }
    return normalized(large.negative, minus(large.significand, aligned), large.exponent - 127);
}

Wide wide_subtract(const Wide& a, const Wide& b) noexcept {
    return wide_add(a, wide_negate(b));
}

// a / b to about 2^-124 of the quotient: a double estimate of 1/b refined by
// two Newton steps, r = r * (2 - b * r), then multiplied by a.
Wide wide_divide(const Wide& a, const Wide& b) noexcept {
    const bool negative = a.negative != b.negative;
    if (is_zero(a.significand))
        return {{}, 0, negative};
    const Wide divisor{b.significand, b.exponent, false};
    const Wide two{{uint64_t{1} << 63, 0}, 1, false};
    // 2^128 / significand of b, a 53-bit estimate, scaled to 1 / b.
    const double estimate = two_to_64 / static_cast<double>(b.significand.high);
    Wide reciprocal = wide(to_extended(estimate));
    reciprocal.exponent -= b.exponent + 1;
    for (int step = 0; step < 2; ++step)
        reciprocal =
            wide_multiply(reciprocal, wide_subtract(two, wide_multiply(divisor, reciprocal)));
    Wide quotient = wide_multiply(a, reciprocal);
    quotient.negative = negative;
    return quotient;
}

Extended round_wide(const Wide& value) noexcept {
    return pack(value.negative, value.significand, false, value.exponent - 127, Precision::bits_64);
}

// sum over j of (-1)^j * coefficient[j * stride] * square^j, by Horner's rule.
Wide alternating_series(
    const Wide& square, const WideConstant* coefficients, std::size_t stride, int terms
) noexcept {
    Wide result = wide(coefficients[static_cast<std::size_t>(terms - 1) * stride]);
    for (int j = terms - 2; j >= 0; --j)
        result = wide_add(
            wide(coefficients[static_cast<std::size_t>(j) * stride]),
            wide_negate(wide_multiply(square, result))
        );
    return result;
}

// atan(v) for a small v: v * (1 - v^2/3 + v^4/5 - ...).
Wide small_arctangent(const Wide& value) noexcept {
    return wide_multiply(
        value,
        alternating_series(wide_multiply(value, value), odd_reciprocals.data(), 1, arctangent_terms)
    );
}

// Unsigned 256-bit integers, least significant word first.
using U256 = std::array<uint64_t, 4>;

U256 multiply_256(const U256& a, uint64_t factor) noexcept {
    U256 result{};
    uint64_t carry = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const U128 product = plus(multiply_64(a[i], factor), U128{0, carry});
        result[i] = product.low;
        carry = product.high;
    }
    return result;
}

bool less_256(const U256& a, const U256& b) noexcept {
    for (std::size_t i = a.size(); i-- > 0;)
        if (a[i] != b[i])
            return a[i] < b[i];
    return false;
}

U256 minus_256(const U256& a, const U256& b) noexcept {
    U256 result{};
    uint64_t borrow = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        const uint64_t difference = a[i] - b[i];
        const uint64_t next_borrow = (a[i] < b[i] ? 1 : 0) | (difference < borrow ? 1 : 0);
        result[i] = difference - borrow;
        borrow = next_borrow;
    }
    return result;
}

// value * 2^-reduction_fraction_bits as a Wide, keeping its top 128 bits.
Wide wide_from_fixed(bool negative, const U256& value) noexcept {
    std::size_t word = value.size();
    while (word > 0 && value[word - 1] == 0)
        --word;
    if (word == 0)
        return {{}, 0, negative};
    // The top three words hold the leading 128 bits.
    const uint64_t top_word = value[word - 1];
    const uint64_t next_word = word >= 2 ? value[word - 2] : 0;
    const uint64_t third_word = word >= 3 ? value[word - 3] : 0;
    const auto zeros = static_cast<unsigned>(std::countl_zero(top_word));
    U128 bits{top_word, next_word};
    if (zeros != 0)
        bits = {
            (top_word << zeros) | (next_word >> (64 - zeros)),
            (next_word << zeros) | (third_word >> (64 - zeros))
        };
    const int top = static_cast<int>(word * 64) - 1 - static_cast<int>(zeros);
    return {bits, top - reduction_fraction_bits, negative};
}

// radians - quarter * pi/2 for |radians| below 2^31 and quarter of the same
// sign; the subtraction is exact to 2^-224.
Wide reduce(Extended radians, int64_t quarter) noexcept {
    U256 value{};
    const int shift = radians.exponent - 63 + reduction_fraction_bits;
    const int word = shift / 64, bit = shift % 64;
    value[static_cast<std::size_t>(word)] = radians.significand << bit;
    if (bit != 0 && word + 1 < 4)
        value[static_cast<std::size_t>(word + 1)] = radians.significand >> (64 - bit);
    const uint64_t magnitude =
        quarter < 0 ? uint64_t{0} - static_cast<uint64_t>(quarter) : static_cast<uint64_t>(quarter);
    const U256 multiple = multiply_256(half_pi_fixed, magnitude);
    if (less_256(value, multiple))
        return wide_from_fixed(!radians.negative, minus_256(multiple, value));
    return wide_from_fixed(radians.negative, minus_256(value, multiple));
}

struct WideSineCosine {
    Wide sine{};
    Wide cosine{};
};

// sin and cos of a reduced angle within about pi/4 of zero.
WideSineCosine reduced_sine_cosine(const Wide& angle) noexcept {
    // angle = k/64 + rest, with |rest| at most 1/128: k is |angle| * 64
    // rounded half up, from the significand's top bits.
    const int32_t shift = 127 - angle.exponent - sine_step_shift;
    const uint64_t halves =
        is_zero(angle.significand) || shift > 128
            ? 0
            : shift_right(angle.significand, static_cast<unsigned>(shift - 1)).low;
    const auto step = static_cast<int>((halves + 1) >> 1);
    Wide rest = angle;
    if (step != 0)
        rest = wide_subtract(
            Wide{angle.significand, angle.exponent, false},
            normalized(false, U128{0, static_cast<uint64_t>(step)}, -sine_step_shift)
        );
    if (step != 0 && angle.negative)
        rest = wide_negate(rest);
    const Wide square = wide_multiply(rest, rest);
    const Wide rest_sine =
        wide_multiply(rest, alternating_series(square, &factorial_reciprocals[1], 2, sine_terms));
    const Wide rest_cosine = alternating_series(square, &factorial_reciprocals[0], 2, cosine_terms);
    if (step == 0)
        return {rest_sine, rest_cosine};
    // sin(c + r) = sin c cos r + cos c sin r; cos(c + r) = cos c cos r - sin c sin r.
    const auto index = static_cast<std::size_t>(step);
    const Wide step_sine = wide(sine_steps[index], angle.negative);
    const Wide step_cosine = wide(cosine_steps[index]);
    return {
        wide_add(wide_multiply(step_sine, rest_cosine), wide_multiply(step_cosine, rest_sine)),
        wide_subtract(wide_multiply(step_cosine, rest_cosine), wide_multiply(step_sine, rest_sine))
    };
}

WideSineCosine wide_sine_cosine(double radians) noexcept {
    const Extended value = to_extended(radians);
    const Wide one = wide(factorial_reciprocals[0]);
    if (value.significand == 0 || value.exponent >= maximum_argument_exponent)
        return {{{}, 0, value.negative}, one};
    Wide reduced = wide(value);
    int64_t quarter = 0;
    if (!(radians <= unreduced_limit && radians >= -unreduced_limit)) {
        const double estimate = radians * reciprocal_half_pi;
        quarter = static_cast<int64_t>(estimate + (estimate < 0 ? -0.5 : 0.5));
        reduced = reduce(value, quarter);
    }
    const WideSineCosine result = reduced_sine_cosine(reduced);
    switch (quarter & 3) {
    case 1:
        return {result.cosine, wide_negate(result.sine)};
    case 2:
        return {wide_negate(result.sine), wide_negate(result.cosine)};
    case 3:
        return {wide_negate(result.cosine), result.sine};
    default:
        return result;
    }
}

// floor(sqrt(value)) and value minus its square, for value in [2^126, 2^128).
// A double estimate, within 2^12 of the root, is corrected with exact squares.
uint64_t integer_square_root(U128 value, U128& remainder) noexcept {
    constexpr uint64_t smallest_root = uint64_t{1} << 63, largest_root = ~uint64_t{0};
    const double estimate = std::sqrt(approximate(value));
    uint64_t root = estimate >= two_to_64 ? largest_root : static_cast<uint64_t>(estimate);
    if (root < smallest_root)
        root = smallest_root;
    // One Newton step from the estimate, then single steps.
    const U128 square = multiply_64(root, root);
    if (less(value, square)) {
        const double excess = approximate(minus(square, value)) / (2.0 * static_cast<double>(root));
        root -= static_cast<uint64_t>(excess);
    } else {
        const double shortfall =
            approximate(minus(value, square)) / (2.0 * static_cast<double>(root));
        const auto step = static_cast<uint64_t>(shortfall);
        root = largest_root - root < step ? largest_root : root + step;
    }
    while (less(value, multiply_64(root, root)))
        --root;
    while (root != largest_root && !less(value, multiply_64(root + 1, root + 1)))
        ++root;
    remainder = minus(value, multiply_64(root, root));
    return root;
}
} // namespace

Extended to_extended(double value) noexcept {
    const auto bits = std::bit_cast<uint64_t>(value);
    const bool negative = (bits >> 63) != 0;
    const uint64_t field = (bits >> 52) & 0x7ff;
    uint64_t significand = bits & 0xfffffffffffffULL;
    int32_t scale = 0;
    if (field == 0) {
        if (significand == 0)
            return zero(negative);
        scale = -1074;
    } else {
        significand |= uint64_t{1} << 52;
        scale = static_cast<int32_t>(field) - 1075;
    }
    const int shift = std::countl_zero(significand);
    return {significand << static_cast<unsigned>(shift), scale + 63 - shift, negative};
}

Extended to_extended(int32_t value) noexcept {
    if (value == 0)
        return zero(false);
    const auto bits = static_cast<uint32_t>(value);
    const uint64_t magnitude = value < 0 ? uint64_t{0u - bits} : uint64_t{bits};
    const int shift = std::countl_zero(magnitude);
    return {magnitude << static_cast<unsigned>(shift), 63 - shift, value < 0};
}

double to_double(Extended value) noexcept {
    const uint64_t sign = value.negative ? uint64_t{1} << 63 : 0;
    if (value.significand == 0)
        return std::bit_cast<double>(sign);
    constexpr int32_t minimum_normal_exponent = -1022, maximum_exponent = 1023;
    constexpr uint64_t infinity_bits = 0x7ff0000000000000ULL;
    if (value.exponent >= minimum_normal_exponent) {
        const Extended rounded = round_to(value, Precision::bits_53);
        if (rounded.exponent > maximum_exponent)
            return std::bit_cast<double>(sign | infinity_bits);
        const uint64_t fraction = (rounded.significand >> 11) & 0xfffffffffffffULL;
        return std::bit_cast<double>(
            sign | (static_cast<uint64_t>(rounded.exponent + 1023) << 52) | fraction
        );
    }
    // Below the normal range the result is a multiple of 2^-1074.
    const int32_t shift = 63 - 1074 - value.exponent;
    const uint64_t kept = shift >= 64 ? 0 : value.significand >> shift;
    const bool round_bit =
        shift - 1 < 64 && ((value.significand >> static_cast<unsigned>(shift - 1)) & 1) != 0;
    const bool below_round_bit =
        shift - 1 >= 64
            ? value.significand != 0
            : (value.significand & ((uint64_t{1} << static_cast<unsigned>(shift - 1)) - 1)) != 0;
    const uint64_t result = kept + (round_bit && (below_round_bit || (kept & 1) != 0) ? 1 : 0);
    return std::bit_cast<double>(sign | result);
}

Extended round_to(Extended value, Precision precision) noexcept {
    if (value.significand == 0)
        return value;
    return pack(value.negative, U128{0, value.significand}, false, value.exponent - 63, precision);
}

Extended negate(Extended value) noexcept {
    value.negative = !value.negative;
    return value;
}

int compare(Extended a, Extended b) noexcept {
    const bool a_zero = a.significand == 0, b_zero = b.significand == 0;
    if (a_zero && b_zero)
        return 0;
    if (a_zero)
        return b.negative ? 1 : -1;
    if (b_zero)
        return a.negative ? -1 : 1;
    if (a.negative != b.negative)
        return a.negative ? -1 : 1;
    int magnitude = 0;
    if (a.exponent != b.exponent)
        magnitude = a.exponent < b.exponent ? -1 : 1;
    else if (a.significand != b.significand)
        magnitude = a.significand < b.significand ? -1 : 1;
    return a.negative ? -magnitude : magnitude;
}

Extended add(Extended a, Extended b, Precision precision) noexcept {
    if (b.significand == 0) {
        if (a.significand == 0)
            return zero(a.negative && b.negative);
        return round_to(a, precision);
    }
    if (a.significand == 0)
        return round_to(b, precision);
    const bool a_larger =
        a.exponent > b.exponent || (a.exponent == b.exponent && a.significand >= b.significand);
    const Extended& large = a_larger ? a : b;
    const Extended& small = a_larger ? b : a;
    const int64_t distance = int64_t{large.exponent} - small.exponent;
    // The larger significand sits at bits 126..63, leaving room for a carry
    // and 63 bits for the smaller operand's alignment.
    const U128 x = shift_left(U128{0, large.significand}, 63);
    U128 y = shift_left(U128{0, small.significand}, 63);
    bool sticky = false;
    if (distance >= 128) {
        sticky = true;
        y = {};
    } else {
        sticky = low_bits_set(y, static_cast<unsigned>(distance));
        y = shift_right(y, static_cast<unsigned>(distance));
    }
    U128 result{};
    if (large.negative == small.negative) {
        result = plus(x, y);
    } else {
        result = minus(x, y);
        // The exact difference lies between result - 1 and result.
        if (sticky)
            result = minus(result, U128{0, 1});
        if (is_zero(result) && !sticky)
            return zero(false);
    }
    return pack(large.negative, result, sticky, large.exponent - 126, precision);
}

Extended subtract(Extended a, Extended b, Precision precision) noexcept {
    return add(a, negate(b), precision);
}

Extended multiply(Extended a, Extended b, Precision precision) noexcept {
    const bool negative = a.negative != b.negative;
    if (a.significand == 0 || b.significand == 0)
        return zero(negative);
    return pack(
        negative,
        multiply_64(a.significand, b.significand),
        false,
        a.exponent + b.exponent - 126,
        precision
    );
}

double multiply_to_double(Extended factor, int32_t value) noexcept {
    const bool negative = factor.negative != (value < 0);
    if (factor.significand == 0 || value == 0)
        return negative ? -0.0 : 0.0;
    // The value's magnitude, shifted up to bit 63 as to_extended places it:
    // the product of the significands lies in [2^126, 2^128).
    const auto bits = static_cast<uint32_t>(value);
    const uint64_t magnitude = value < 0 ? uint64_t{0u - bits} : uint64_t{bits};
    const int shift = std::countl_zero(magnitude);
    const U128 product = multiply_64(factor.significand, magnitude << static_cast<unsigned>(shift));
    const auto top = static_cast<int32_t>(product.high >> 63);
    const int32_t exponent = factor.exponent + 63 - shift + top;
    // Below double's normal range to_double rounds the 53-bit product again,
    // and past it gives an infinity; both are left to the general operations.
    constexpr int32_t minimum_normal_exponent = -1022, maximum_exponent = 1023;
    if (exponent < minimum_normal_exponent || exponent >= maximum_exponent)
        return to_double(multiply(factor, to_extended(value), Precision::bits_53));
    // The leading bit to bit 63 of `high`: its top 53 bits are kept, bit 10
    // is the round bit, and the bits below it and `low` are the sticky bits.
    uint64_t high = product.high, low = product.low;
    if (top == 0) {
        high = (high << 1) | (low >> 63);
        low <<= 1;
    }
    constexpr unsigned dropped_bits = 11;
    constexpr uint64_t round_bit = uint64_t{1} << (dropped_bits - 1);
    uint64_t kept = high >> dropped_bits;
    const bool below_round_bit = (high & (round_bit - 1)) != 0 || low != 0;
    if ((high & round_bit) != 0 && (below_round_bit || (kept & 1) != 0))
        ++kept;
    // kept holds the leading bit at bit 52, or 2^53 when rounding carried
    // out; adding it to the exponent field below that bit carries either into
    // the exponent.
    const uint64_t sign = negative ? uint64_t{1} << 63 : 0;
    return std::bit_cast<double>(
        sign | ((static_cast<uint64_t>(exponent + maximum_exponent - 1) << 52) + kept)
    );
}

Extended divide(Extended a, Extended b, Precision precision) noexcept {
    const bool negative = a.negative != b.negative;
    if (a.significand == 0 || b.significand == 0)
        return zero(negative);
    // The significand quotient lies in (1/2, 2). Scaling the dividend by 2^64,
    // or 2^63 when it is at least the divisor, gives a 64-bit quotient; one
    // more quotient bit and the remainder decide the rounding.
    const bool at_least = a.significand >= b.significand;
    const U128 numerator =
        at_least ? U128{a.significand >> 1, a.significand << 63} : U128{a.significand, 0};
    uint64_t remainder = 0;
    const uint64_t quotient = divide_128_by_64(numerator, b.significand, remainder);
    // remainder < divisor < 2^64; twice the remainder decides the next bit.
    const U128 twice{remainder >> 63, remainder << 1};
    const U128 divisor{0, b.significand};
    const bool next_bit = !less(twice, divisor);
    const bool beyond = next_bit ? !is_zero(minus(twice, divisor)) : remainder != 0;
    const U128 value{quotient >> 63, (quotient << 1) | (next_bit ? 1 : 0)};
    const int32_t scale = a.exponent - b.exponent - (at_least ? 64 : 65);
    return pack(negative, value, beyond, scale, precision);
}

Extended square_root(Extended value, Precision precision) noexcept {
    if (value.significand == 0)
        return value;
    // value = significand * 2^power; scaling the significand by 2^63 or 2^64
    // makes the remaining power even and the radicand at least 2^126.
    const int32_t power = value.exponent - 63;
    const unsigned scale_bits = (power & 1) != 0 ? 63 : 64;
    U128 remainder{};
    const uint64_t root =
        integer_square_root(shift_left(U128{0, value.significand}, scale_bits), remainder);
    // The exact root is root + f with 0 <= f < 1; f >= 1/2 exactly when the
    // remainder exceeds root, and f is never exactly 1/2.
    const bool half = less(U128{0, root}, remainder);
    const U128 doubled{root >> 63, (root << 1) | (half ? 1 : 0)};
    return pack(
        false,
        doubled,
        !is_zero(remainder),
        (power - static_cast<int32_t>(scale_bits)) / 2 - 1,
        precision
    );
}

Extended arctangent(double y, double x) noexcept {
    const Extended ordinate = to_extended(y), abscissa = to_extended(x);
    const bool negative = ordinate.negative;
    if (ordinate.significand == 0) {
        if (!abscissa.negative)
            return zero(negative);
        return with_sign(round_wide(wide(pi_constant)), negative);
    }
    if (abscissa.significand == 0)
        return with_sign(round_wide(wide(half_pi_constant)), negative);

    const Extended a = with_sign(ordinate, false), b = with_sign(abscissa, false);
    const bool steep = compare(a, b) > 0;
    const Extended shorter = steep ? b : a, longer = steep ? a : b;
    const double ratio = to_double(shorter) / to_double(longer);
    const auto step = static_cast<int>(ratio * arctangent_step_count + 0.5);
    Wide angle;
    if (step == 0) {
        angle = small_arctangent(wide_divide(wide(shorter), wide(longer)));
    } else {
        // atan(t) = atan(k/32) + atan(u) with u = (32 shorter - k longer) / (32 longer
        // + k shorter), numerator and denominator formed exactly; longer is less
        // than 2^7 times shorter.
        const auto distance = static_cast<unsigned>(longer.exponent - shorter.exponent);
        const U128 n{0, shorter.significand};
        const U128 f = shift_left(U128{0, longer.significand}, distance);
        const U128 n32 = shift_left(n, arctangent_step_shift);
        const U128 f32 = shift_left(f, arctangent_step_shift);
        const U128 kn = multiply_small(n, static_cast<uint32_t>(step));
        const U128 kf = multiply_small(f, static_cast<uint32_t>(step));
        const bool below = less(n32, kf);
        const Wide numerator = normalized(below, below ? minus(kf, n32) : minus(n32, kf), 0);
        const Wide denominator = normalized(false, plus(f32, kn), 0);
        angle = wide_add(
            wide(arctangent_steps[static_cast<std::size_t>(step)]),
            small_arctangent(wide_divide(numerator, denominator))
        );
    }
    if (steep)
        angle = wide_subtract(wide(half_pi_constant), angle);
    if (abscissa.negative)
        angle = wide_subtract(wide(pi_constant), angle);
    angle.negative = negative;
    return round_wide(angle);
}

namespace {
// A positive normal double as its 53-bit significand and the power of two of
// its leading bit.
struct DoubleParts {
    uint64_t significand{};
    int32_t exponent{};
};

DoubleParts parts(double value) noexcept {
    const auto bits = std::bit_cast<uint64_t>(value);
    return {
        (bits & 0xfffffffffffffULL) | (uint64_t{1} << 52),
        static_cast<int32_t>((bits >> 52) & 0x7ff) - 1023
    };
}

bool is_normal_positive(double value) noexcept {
    const auto field = (std::bit_cast<uint64_t>(value) >> 52) & 0x7ff;
    return field != 0 && field != 0x7ff && !std::signbit(value);
}

// Rounds to 64 bits, then to a double: the value stored after a step.
double stored(Extended value) noexcept {
    return to_double(value);
}

// shorter / longer rounded to 64 bits, then to a double. The IEEE quotient q
// is that value unless the exact quotient lies within about 2^-11 units of
// q's last place of a point halfway between q and a neighbour, or q is a
// power of two; the exact remainder decides, and the rare others are divided
// exactly.
double stored_ratio(double shorter, double longer) noexcept {
    const double quotient = shorter / longer;
    if (is_normal_positive(quotient) && is_normal_positive(shorter)) {
        const DoubleParts s = parts(shorter), l = parts(longer), q = parts(quotient);
        // shorter/longer - quotient = (S * 2^k - Q * L) / L units of q's last place.
        const int32_t shift = s.exponent - l.exponent - q.exponent + 52;
        if (q.significand != uint64_t{1} << 52 && shift >= 0 && shift <= 60) {
            const U128 scaled = shift_left(U128{0, s.significand}, static_cast<unsigned>(shift));
            const U128 product = multiply_64(q.significand, l.significand);
            const U128 remainder =
                less(scaled, product) ? minus(product, scaled) : minus(scaled, product);
            // Twice the remainder against the divisor: a half unit is L.
            const U128 twice = shift_left(remainder, 1);
            const U128 divisor{0, l.significand};
            const U128 distance =
                less(twice, divisor) ? minus(divisor, twice) : minus(twice, divisor);
            if (less(U128{0, (l.significand >> 9) + 1}, distance))
                return quotient;
        }
    }
    return stored(divide(to_extended(shorter), to_extended(longer), Precision::bits_64));
}

// sqrt(value) rounded to 64 bits, then to a double, for value in [1, 4). The
// IEEE root r is that value unless the exact root lies within about 2^-10
// units of r's last place of a point halfway between r and a neighbour.
double stored_root(double value) noexcept {
    const double root = std::sqrt(value);
    const DoubleParts v = parts(value), r = parts(root);
    if (r.exponent == 0 && (v.exponent == 0 || v.exponent == 1)) {
        // value * 2^106 against the squares of the halfway points (2R +- 1) * 2^-53.
        const U128 scaled =
            shift_left(U128{0, v.significand}, static_cast<unsigned>(54 + v.exponent));
        const U128 below = multiply_64(2 * r.significand - 1, 2 * r.significand - 1);
        const U128 above = multiply_64(2 * r.significand + 1, 2 * r.significand + 1);
        const U128 margin{0, (2 * r.significand + 1) >> 8};
        const U128 distance_below =
            less(scaled, below) ? minus(below, scaled) : minus(scaled, below);
        const U128 distance_above =
            less(scaled, above) ? minus(above, scaled) : minus(scaled, above);
        if (less(margin, distance_below) && less(margin, distance_above))
            return root;
    }
    return stored(square_root(to_extended(value), Precision::bits_64));
}
} // namespace

double hypotenuse(double x, double y) noexcept {
    // Each step is rounded to 64 bits, then stored as a double. The larger
    // magnitude divided by itself is exactly 1, and so is its square.
    const double a = std::fabs(x), b = std::fabs(y);
    const double largest = b > a ? b : a, smaller = b > a ? a : b;
    if (largest == 0.0)
        return 0.0;
    const Extended large = to_extended(largest);
    if (smaller == 0.0)
        return largest;
    const double ratio = stored_ratio(smaller, largest);
    const Extended ratio_value = to_extended(ratio);
    const double squared = stored(add(
        to_extended(1.0), multiply(ratio_value, ratio_value, Precision::bits_64), Precision::bits_64
    ));
    const double root = stored_root(squared);
    return stored(multiply(to_extended(root), large, Precision::bits_64));
}

double approximate_arctangent(double y, double x) noexcept {
    const double a = std::fabs(y), b = std::fabs(x);
    if (a == 0.0 || b == 0.0)
        return to_double(arctangent(y, x));
    // t = shorter / longer in [0, 1] = k/64 + a remainder whose arctangent
    // is u - u^3/3 + ... for u = (t - k/64) / (1 + t k/64), |u| <= 1/128.
    const bool steep = a > b;
    const double t = steep ? b / a : a / b;
    const auto step = static_cast<std::size_t>(t * static_cast<double>(estimate_steps) + 0.5);
    const double c = static_cast<double>(step) / static_cast<double>(estimate_steps);
    const double u = (t - c) / (1.0 + t * c);
    const double square = u * u;
    const double series =
        u *
        (1.0 + square * (-1.0 / 3.0 + square * (1.0 / 5.0 + square * (-1.0 / 7.0 + square / 9.0))));
    double angle = arctangent_estimate_steps[step] + series;
    if (steep)
        angle = half_pi_estimate - angle;
    if (std::signbit(x))
        angle = pi_estimate - angle;
    return std::signbit(y) ? -angle : angle;
}

SineCosine sine_cosine(double radians) noexcept {
    const WideSineCosine result = wide_sine_cosine(radians);
    return {round_wide(result.sine), round_wide(result.cosine)};
}

Extended sine(double radians) noexcept {
    return round_wide(wide_sine_cosine(radians).sine);
}

Extended cosine(double radians) noexcept {
    return round_wide(wide_sine_cosine(radians).cosine);
}
} // namespace oa::base::game_math

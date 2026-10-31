// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The game's floating-point functions and the 64-bit-significand arithmetic
// under them. The basic operations at 53 bits must agree with IEEE 754
// doubles on random operands, and the product with an integer rounded to a
// double with the general multiply and rounding. The sine and cosine of every angle word, and
// the arctangents, must be the exact values rounded to 64 bits, to nearest
// with ties to even. Over fixed sample sets the results are pinned by
// digest: the heading, length, square root and rotation results are 3.1c's
// own, and the 64-bit arctangents, arccosines, sines and cosines are the
// correctly rounded values.
#include "oa/base/game_math.hpp"
#include "oa/base/game_math/extended.hpp"
#include "oa/base/sha256.hpp"

#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace {

namespace gm = oa::base::game_math;
namespace sha256 = oa::base::sha256;
using gm::Extended;
using gm::Precision;

int failures = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition);     \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

bool same(Extended a, Extended b) {
    if (a.significand == 0 || b.significand == 0)
        return a.significand == b.significand && a.negative == b.negative;
    return a.significand == b.significand && a.exponent == b.exponent && a.negative == b.negative;
}

bool same_bits(double a, double b) {
    return std::bit_cast<uint64_t>(a) == std::bit_cast<uint64_t>(b);
}

Extended value(uint64_t significand, int32_t exponent, bool negative = false) {
    return {significand, exponent, negative};
}

// The sample sets: a SplitMix64 sequence per set.
struct Sequence {
    uint64_t state{};

    uint64_t next() {
        uint64_t z = (state += 0x9e3779b97f4a7c15ULL);
        z = (z ^ (z >> 30)) * 0xbf58476d1ce4e5b9ULL;
        z = (z ^ (z >> 27)) * 0x94d049bb133111ebULL;
        return z ^ (z >> 31);
    }
};

// A 32-bit integer whose magnitude is spread over every scale.
int32_t spread_int(uint64_t r) {
    return std::bit_cast<int32_t>(static_cast<uint32_t>(r)) >> static_cast<unsigned>(r >> 59);
}

// A double with a random sign, significand and an exponent in [low, high].
double spread_double(uint64_t r, uint64_t s, int low, int high) {
    const auto span = static_cast<uint64_t>(high - low + 1);
    const int exponent = low + static_cast<int>((s >> 32) % span);
    const uint64_t bits = (r & 0xfffffffffffffULL) |
                          (static_cast<uint64_t>(exponent + 1023) << 52) | ((s & 1ULL) << 63);
    return std::bit_cast<double>(bits);
}

constexpr int grid_limit = 64;
constexpr int heading_samples = 1 << 18;
constexpr int arctangent_samples = 1 << 18;
constexpr int hypotenuse_samples = 1 << 18;
constexpr int hypotenuse_stream_pairs = 1 << 20;
constexpr int float_pair_samples = 1 << 16;
constexpr int arccosine_samples = 1 << 18;
constexpr int square_root_samples = 1 << 18;
constexpr int random_points_per_angle = 4;

struct Digest {
    sha256::Hasher hasher{};

    template <class T>
    void add(T item) {
        const auto bytes = std::bit_cast<std::array<uint8_t, sizeof(T)>>(item);
        sha256::update(hasher, bytes);
    }

    void add(Extended item) {
        add(item.significand);
        add(item.significand != 0 ? item.exponent : int32_t{0});
        add(static_cast<uint8_t>(item.negative ? 1 : 0));
    }

    std::string hex() const {
        const auto text = sha256::to_hex(sha256::finish(hasher));
        return {text.begin(), text.end()};
    }
};

void check_digest(const Digest& digest, std::string_view expected, const char* what) {
    const std::string actual = digest.hex();
    if (actual != expected) {
        std::fprintf(
            stderr,
            "%s digest %s, expected %.*s\n",
            what,
            actual.c_str(),
            static_cast<int>(expected.size()),
            expected.data()
        );
        ++failures;
    }
}

void check_conversions() {
    CHECK(same(gm::to_extended(1.0), value(0x8000000000000000ULL, 0)));
    CHECK(same(gm::to_extended(-0.75), value(0xc000000000000000ULL, -1, true)));
    CHECK(same(gm::to_extended(int32_t{-2147483647 - 1}), value(0x8000000000000000ULL, 31, true)));
    CHECK(same(
        gm::to_extended(std::bit_cast<double>(uint64_t{1})), value(0x8000000000000000ULL, -1074)
    ));
    CHECK(gm::to_extended(-0.0).negative && gm::to_extended(-0.0).significand == 0);
    // Rounding to a double: ties go to the even significand.
    CHECK(same_bits(gm::to_double(value(0x8000000000000400ULL, 0)), 1.0));
    CHECK(same_bits(gm::to_double(value(0x8000000000000c00ULL, 0)), 1.0 + 0x1p-51));
    CHECK(same_bits(gm::to_double(value(0x8000000000000401ULL, 0)), 1.0 + 0x1p-52));
    // Beyond the range: infinite; below it: rounded to the subnormal spacing.
    CHECK(std::isinf(gm::to_double(value(0x8000000000000000ULL, 1024))));
    CHECK(same_bits(gm::to_double(value(0xc000000000000000ULL, -1075)), 0x1p-1074));
    CHECK(same_bits(gm::to_double(value(0x8000000000000000ULL, -1075)), 0.0));
    CHECK(same(
        gm::round_to(value(0xffffffffffffffffULL, 3), Precision::bits_53),
        value(0x8000000000000000ULL, 4)
    ));
}

void check_compare() {
    CHECK(gm::compare(gm::to_extended(0.0), gm::to_extended(-0.0)) == 0);
    CHECK(gm::compare(gm::to_extended(-1.0), gm::to_extended(0.0)) < 0);
    CHECK(gm::compare(value(0x8000000000000001ULL, 0), gm::to_extended(1.0)) > 0);
    CHECK(gm::compare(value(0x8000000000000001ULL, 0, true), gm::to_extended(-1.0)) < 0);
}

// At 53 bits each operation must give the IEEE double result.
void check_double_operations() {
    Sequence sequence{11};
    int mismatches = 0;
    for (int i = 0; i < 100000; ++i) {
        const double a = spread_double(sequence.next(), sequence.next(), -30, 30);
        const double b = spread_double(sequence.next(), sequence.next(), -30, 30);
        const Extended x = gm::to_extended(a), y = gm::to_extended(b);
        const Precision p = Precision::bits_53;
        if (!same_bits(gm::to_double(gm::add(x, y, p)), a + b) ||
            !same_bits(gm::to_double(gm::subtract(x, y, p)), a - b) ||
            !same_bits(gm::to_double(gm::multiply(x, y, p)), a * b) ||
            !same_bits(gm::to_double(gm::divide(x, y, p)), a / b) ||
            !same_bits(
                gm::to_double(gm::square_root(gm::to_extended(std::fabs(a)), p)),
                std::sqrt(std::fabs(a))
            ))
            ++mismatches;
    }
    CHECK(mismatches == 0);
    // Exact cancellation gives a positive zero.
    CHECK(same(
        gm::add(gm::to_extended(2.5), gm::to_extended(-2.5), Precision::bits_64),
        gm::to_extended(0.0)
    ));
    CHECK(same(
        gm::add(gm::to_extended(-0.0), gm::to_extended(-0.0), Precision::bits_64),
        gm::to_extended(-0.0)
    ));
}

void check_wide_operations() {
    // 1/3 at 64 bits: the bits past the 64th are 1010..., so it rounds up.
    const Extended third =
        gm::divide(gm::to_extended(1.0), gm::to_extended(3.0), Precision::bits_64);
    CHECK(same(third, value(0xaaaaaaaaaaaaaaabULL, -2)));
    // sqrt(2) at 64 bits: the bit past the 64th is 0, so it rounds down.
    CHECK(same(
        gm::square_root(gm::to_extended(2.0), Precision::bits_64), value(0xb504f333f9de6484ULL, 0)
    ));
    // (1 + 2^-63) * (1 + 2^-63) = 1 + 2^-62 + 2^-126 rounds to 1 + 2^-62.
    const Extended near_one = value(0x8000000000000001ULL, 0);
    CHECK(
        same(gm::multiply(near_one, near_one, Precision::bits_64), value(0x8000000000000002ULL, 0))
    );
    // 1 + 2^-64 is a tie at 64 bits and goes to the even significand, 1.
    CHECK(same(
        gm::add(gm::to_extended(1.0), gm::to_extended(0x1p-64), Precision::bits_64),
        gm::to_extended(1.0)
    ));
    CHECK(same(
        gm::add(gm::to_extended(1.0), gm::to_extended(0x1p-64 + 0x1p-100), Precision::bits_64),
        value(0x8000000000000001ULL, 0)
    ));
}

// multiply_to_double gives what rounding multiply's 53-bit product to a
// double gives: for the sine and cosine of every angle word against
// coordinates at every scale, for factors across the exponent range (normal,
// subnormal and infinite products), at ties and next to them, and for zeros.
void check_integer_products() {
    const auto general = [](Extended factor, int32_t integer) {
        return gm::to_double(gm::multiply(factor, gm::to_extended(integer), Precision::bits_53));
    };
    int mismatches = 0;
    const auto compare = [&](Extended factor, int32_t integer) {
        if (!same_bits(gm::multiply_to_double(factor, integer), general(factor, integer)))
            ++mismatches;
    };
    std::array<int32_t, 2 + 31 * 6> edges{0, -2147483647 - 1};
    std::size_t edge = 2;
    for (unsigned bit = 0; bit < 31; ++bit) {
        const int32_t power = int32_t{1} << bit;
        for (const int32_t near : {power, power - 1, power + 1})
            if (near > 0) {
                edges[edge++] = near;
                edges[edge++] = -near;
            }
    }
    Sequence sequence{12};
    for (int word = -32768; word <= 32767; ++word) {
        const double radians = static_cast<double>(word) * 9.587379924285e-05;
        const gm::SineCosine values = gm::sine_cosine(radians);
        for (const Extended factor : {values.sine, values.cosine}) {
            for (std::size_t i = 0; i < edge; ++i)
                compare(factor, edges[i]);
            for (int i = 0; i < 8; ++i)
                compare(factor, spread_int(sequence.next()));
        }
    }
    constexpr uint64_t leading_bit = uint64_t{1} << 63;
    for (int i = 0; i < 200000; ++i) {
        const uint64_t r = sequence.next(), s = sequence.next();
        // Exponents from far below double's range to past it.
        const auto exponent = static_cast<int32_t>((r >> 8) % 2400) - 1200;
        compare(value(s | leading_bit, exponent, (r & 1) != 0), spread_int(sequence.next()));
    }
    // Ties: an odd q and an odd m whose product lies in [2^53, 2^54), so that
    // the bits of q * 2^k * m past the 53rd are exactly one half; and the
    // factors one unit either side of each.
    for (int i = 0; i < 20000; ++i) {
        const uint64_t r = sequence.next(), s = sequence.next();
        const uint64_t odd_factor = ((s >> 1) & 0x7f) | 1;
        constexpr uint64_t tie_low = uint64_t{1} << 53, tie_high = uint64_t{1} << 54;
        const uint64_t least = (tie_low + odd_factor - 1) / odd_factor;
        const uint64_t most = (tie_high - 1) / odd_factor;
        uint64_t quotient = (least + (s >> 8) % (most - least)) | 1;
        if (quotient > most)
            quotient -= 2;
        const Extended tie = value(
            quotient << std::countl_zero(quotient),
            static_cast<int32_t>((r >> 8) & 63) - 32,
            (r & 1) != 0
        );
        const auto integer = static_cast<int32_t>(odd_factor << ((r >> 16) & 7));
        compare(tie, integer);
        compare(tie, -integer);
        compare(value(tie.significand + 1, tie.exponent, tie.negative), integer);
        if (tie.significand != leading_bit)
            compare(value(tie.significand - 1, tie.exponent, tie.negative), -integer);
    }
    CHECK(mismatches == 0);
    // Zeros keep multiply's sign.
    CHECK(same_bits(gm::multiply_to_double(gm::to_extended(-0.0), 5), -0.0));
    CHECK(same_bits(gm::multiply_to_double(gm::to_extended(-0.0), -5), 0.0));
    CHECK(same_bits(gm::multiply_to_double(gm::to_extended(-2.0), 0), -0.0));
    CHECK(same_bits(gm::multiply_to_double(gm::to_extended(2.0), 0), 0.0));
    CHECK(same_bits(gm::multiply_to_double(gm::to_extended(0.75), -2147483647 - 1), -0x1.8p30));
}

void check_named_values() {
    // pi and pi/2 at 64 bits.
    CHECK(same(gm::arctangent(0.0, -1.0), value(0xc90fdaa22168c235ULL, 1)));
    CHECK(same(gm::arctangent(-0.0, -0.0), value(0xc90fdaa22168c235ULL, 1, true)));
    CHECK(same(gm::arctangent(5.0, 0.0), value(0xc90fdaa22168c235ULL, 0)));
    CHECK(same(gm::arctangent(-0.0, 3.0), gm::to_extended(-0.0)));
    CHECK(same(gm::arctangent(1.0, 1.0), value(0xc90fdaa22168c235ULL, -1)));
    // atan2(601, 16629) = 2^-5 * 0x93f8cf851ebe715f.7f5f...: just below a tie.
    CHECK(same(gm::arctangent(601.0, 16629.0), value(0x93f8cf851ebe715fULL, -5)));
    // atan2(30037, 464177) lies just above a tie.
    CHECK(same(gm::arctangent(30037.0, 464177.0), value(0x84578f5dc1905953ULL, -4)));
    CHECK(same(gm::sine(-0.0), gm::to_extended(-0.0)));
    CHECK(same(gm::cosine(0.0), gm::to_extended(1.0)));
    // The angle word -32345 lies in the third quarter turn.
    const double angle = -32345.0 * 9.587379924285e-05;
    CHECK(same(gm::sine(angle), value(0xa610f14ce81e3bfbULL, -5, true)));
    CHECK(same(gm::cosine(angle), value(0xffca1d57274ed147ULL, -1, true)));
    CHECK(!gm::arccosine(1.0000000000000002).has_value());
    CHECK(!gm::arccosine(std::nan("")).has_value());
    CHECK(same(*gm::arccosine(1.0), gm::to_extended(0.0)));
    CHECK(same(*gm::arccosine(-1.0), value(0xc90fdaa22168c235ULL, 1)));
}

void check_game_functions() {
    // Each step of the length is rounded twice. For the 52-165-173 triangle
    // that gives 173 plus one unit in the last place; rounding each step once
    // gives 173 minus one, which truncates to 172.
    CHECK(same_bits(gm::hypotenuse(52.0, 165.0), 173.00000000000003));
    CHECK(gm::distance(52, 165) == 173);
    CHECK(gm::distance(990, -312) == 1038);
    CHECK(same_bits(gm::hypotenuse(-4.0, -148052582.0), 0x1.1a634cc000002p+27));
    CHECK(same_bits(gm::hypotenuse(0.0, -0.0), 0.0));
    CHECK(gm::direction(0, 1) == 0 && gm::direction(1, 0) == 16384);
    CHECK(gm::direction(0, -1) == 32768 && gm::direction(-1, 0) == 49152);
    CHECK(gm::direction(1, 1) == 8192);
    const gm::RotatedPair quarter = gm::rotate_pair(65536, 0, 16384);
    CHECK(std::fabs(quarter.first) < 1e-6 && std::fabs(quarter.second - 65536.0) < 1e-6);
}

double nearest_even(double v) {
    const double low = std::floor(v), fraction = v - low;
    return low + ((fraction > 0.5 || (fraction == 0.5 && std::fmod(low, 2.0) != 0)) ? 1.0 : 0.0);
}

int32_t rounded_coordinate(double v) {
    const double n = nearest_even(v);
    return n >= -2147483648.0 && n <= 2147483647.0 ? static_cast<int32_t>(n) : INT32_MIN;
}

void check_sample_sets() {
    {
        // Headings and their arctangents: a grid of small vectors, then
        // random ones.
        Digest headings, arctangents;
        double worst_estimate = 0.0;
        auto add = [&](int32_t x, int32_t z) {
            headings.add(gm::direction(x, z));
            const Extended angle = gm::arctangent(static_cast<double>(x), static_cast<double>(z));
            arctangents.add(angle);
            const double estimate =
                gm::approximate_arctangent(static_cast<double>(x), static_cast<double>(z));
            worst_estimate = std::fmax(worst_estimate, std::fabs(estimate - gm::to_double(angle)));
        };
        for (int x = -grid_limit; x <= grid_limit; ++x)
            for (int z = -grid_limit; z <= grid_limit; ++z)
                add(x, z);
        Sequence sequence{1};
        for (int i = 0; i < heading_samples; ++i) {
            const int32_t x = spread_int(sequence.next());
            add(x, spread_int(sequence.next()));
        }
        CHECK(worst_estimate <= 0x1p-51);
        check_digest(
            headings, "89b9333179c134419e97d665448f8f2ec6b51f935236ca6654e743cf6b930238", "heading"
        );
        check_digest(
            arctangents,
            "5e2ec188ee3eafdd1c3a7cd353c776855254b3dc179e2bb1a44e8cf3f0133eb3",
            "heading arctangent"
        );
    }
    {
        // Arctangents of doubles of many magnitudes, with signed zeros.
        Digest digest;
        Sequence sequence{2};
        for (int i = 0; i < arctangent_samples; ++i) {
            const uint64_t a = sequence.next(), b = sequence.next(), c = sequence.next(),
                           d = sequence.next();
            double y = spread_double(a, b, -40, 40), x = spread_double(c, d, -40, 40);
            if ((b & 0xff) == 0)
                y = 0.0;
            else if ((b & 0xff) == 1)
                y = -0.0;
            if ((d & 0xff) == 0)
                x = 0.0;
            else if ((d & 0xff) == 1)
                x = -0.0;
            digest.add(gm::arctangent(y, x));
        }
        check_digest(
            digest, "1c772922dcae0905d3e3ae4f882c73c0e328698bf40a75f1b476c84e99463ddb", "arctangent"
        );
    }
    {
        // Lengths: a grid, random integer pairs, then pairs of floats.
        Digest digest;
        auto add = [&](double a, double b) {
            digest.add(std::bit_cast<uint64_t>(gm::hypotenuse(a, b)));
        };
        for (int x = -grid_limit; x <= grid_limit; ++x)
            for (int z = -grid_limit; z <= grid_limit; ++z)
                add(x, z);
        Sequence sequence{3};
        for (int i = 0; i < hypotenuse_stream_pairs; ++i) {
            const int32_t a = spread_int(sequence.next()), b = spread_int(sequence.next());
            if (i < hypotenuse_samples)
                add(a, b);
        }
        for (int i = 0; i < float_pair_samples; ++i) {
            const uint64_t p = sequence.next(), q = sequence.next(), s = sequence.next(),
                           t = sequence.next();
            const auto a = static_cast<float>(spread_double(p, q, -20, 20));
            const auto b = static_cast<float>(spread_double(s, t, -20, 20));
            add(a, b);
        }
        check_digest(
            digest, "453f9a31d58a892c63c1776b8d69437dd402ff5a63b326fd0f73c0f968b4e3a8", "hypotenuse"
        );
    }
    {
        // Arccosines, many near 1 or -1; outside the domain is one 0xff byte.
        Digest digest;
        Sequence sequence{4};
        for (int i = 0; i < arccosine_samples; ++i) {
            const uint64_t p = sequence.next(), q = sequence.next();
            double x = spread_double(p, q, -60, -1);
            if ((q & 0xf) == 0)
                x = std::bit_cast<double>((0x3fefffffffffffffULL - (p & 0xffff)) | ((q & 2) << 62));
            const auto angle = gm::arccosine(x);
            if (angle)
                digest.add(*angle);
            else
                digest.add(uint8_t{0xff});
        }
        check_digest(
            digest, "819dfcbca4d254f0150f9e9b7e48e59bafa03a27dcb82d8419ba29181419b0ce", "arccosine"
        );
    }
    {
        Digest digest;
        Sequence sequence{5};
        for (int i = 0; i < square_root_samples; ++i) {
            const uint64_t p = sequence.next(), q = sequence.next();
            digest.add(
                std::bit_cast<uint64_t>(gm::square_root(spread_double(p, q & ~1ULL, -60, 60)))
            );
        }
        check_digest(
            digest,
            "a87a88c1bbec993263ed69761c35260d5f51c8a5cb1bd6ba6adc8c033261cdbb",
            "square root"
        );
    }
    {
        // Every angle word: its sine and cosine, and the rotation of fixed
        // and random points.
        constexpr std::array<std::array<int32_t, 2>, 12> points{{
            {0, 0},
            {65536, 0},
            {0, 65536},
            {65536, 65536},
            {-1, 1},
            {12345678, -7654321},
            {0x7fffffff, 0},
            {0, -0x7fffffff},
            {1000000, 1000000},
            {-3, 7},
            {0x00a00000, 0x00500000},
            {-0x01000000, 0x00800000},
        }};
        Digest trigonometry, rotations;
        Sequence sequence{6};
        for (int word = -32768; word <= 32767; ++word) {
            const auto angle = static_cast<int16_t>(word);
            const double radians = static_cast<double>(angle) * 9.587379924285e-05;
            const gm::SineCosine values = gm::sine_cosine(radians);
            trigonometry.add(values.sine);
            trigonometry.add(values.cosine);
            for (std::size_t i = 0; i < points.size() + random_points_per_angle; ++i) {
                int32_t first = 0, second = 0;
                if (i < points.size()) {
                    first = points[i][0];
                    second = points[i][1];
                } else {
                    first = spread_int(sequence.next());
                    second = spread_int(sequence.next());
                }
                // The game leaves a pair unchanged at angle word 0.
                if (angle != 0) {
                    const gm::RotatedPair rotated = gm::rotate_pair(first, second, angle);
                    first = rounded_coordinate(rotated.first);
                    second = rounded_coordinate(rotated.second);
                }
                rotations.add(first);
                rotations.add(second);
            }
        }
        check_digest(
            trigonometry,
            "507241d01343b129d0a3595027f474047d0a98e273f032ea363d17e6ca2af1dc",
            "sine and cosine"
        );
        check_digest(
            rotations,
            "b20f656ac5794a19f6aecd8965415a927d532625e2576af082945c4e4557dd64",
            "rotation"
        );
    }
}

} // namespace

int main() {
    check_conversions();
    check_compare();
    check_double_operations();
    check_wide_operations();
    check_integer_products();
    check_named_values();
    check_game_functions();
    check_sample_sets();
    if (failures != 0) {
        std::fprintf(stderr, "%d checks failed\n", failures);
        return 1;
    }
    return 0;
}

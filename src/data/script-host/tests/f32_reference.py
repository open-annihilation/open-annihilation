# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Golden binary32 values for the script host's f32 library. Run with python3 -I.
# The stdout is the table in f32_test.cpp, from the BEGIN line through the
# END line. Every finite value is a fractions.Fraction. Rounding to binary32
# is nearest, ties to even, with subnormals and overflow to infinity. Nothing
# here uses binary floating point, and nothing calls the engine.

import sys
from fractions import Fraction

COUNT_MIN = -(1 << 47)
COUNT_MAX = (1 << 47) - 1
CANONICAL_NAN = 0x7FC00000
OVERFLOW_AT = Fraction(2**128 - 2**103)

rows = []
seen = set()


def add(op, left, right, result):
    row = (op, str(left), str(right), str(result))
    if row in seen:
        return
    for part in row:
        if any(ch in part for ch in "\"\\\n\r{};"):
            print(f"unsafe golden text {part!r}", file=sys.stderr)
            sys.exit(1)
    seen.add(row)
    rows.append(row)


def floor_log2(frac):
    """Largest e with 2^e <= frac. frac is positive."""
    numerator = frac.numerator
    denominator = frac.denominator
    estimate = numerator.bit_length() - denominator.bit_length()

    def at_most(power):
        if power >= 0:
            return (denominator << power) <= numerator
        return denominator <= (numerator << -power)

    if at_most(estimate + 1):
        return estimate + 1
    if at_most(estimate):
        return estimate
    return estimate - 1


def round_ties_even(quotient, remainder, denominator):
    twice = remainder * 2
    if twice > denominator or (twice == denominator and (quotient & 1) == 1):
        return quotient + 1
    return quotient


def round_binary32(sign, magnitude):
    """Bits of sign and a non-negative magnitude, rounded to binary32."""
    sign_bit = (sign & 1) << 31
    if magnitude == 0:
        return sign_bit
    if magnitude >= OVERFLOW_AT:
        return sign_bit | 0x7F800000
    exponent = floor_log2(magnitude)
    if exponent >= -126:
        scaled = magnitude / (Fraction(2) ** (exponent - 23))
        quotient, remainder = divmod(scaled.numerator, scaled.denominator)
        quotient = round_ties_even(quotient, remainder, scaled.denominator)
        if quotient == (1 << 24):
            quotient = 1 << 23
            exponent += 1
        if exponent > 127:
            return sign_bit | 0x7F800000
        return sign_bit | ((exponent + 127) << 23) | (quotient & 0x7FFFFF)
    scaled = magnitude * (Fraction(2) ** 149)
    quotient, remainder = divmod(scaled.numerator, scaled.denominator)
    quotient = round_ties_even(quotient, remainder, scaled.denominator)
    if quotient >= (1 << 23):
        return sign_bit | (1 << 23)
    return sign_bit | quotient


class Decoded:
    def __init__(self, kind, sign=0, magnitude=None):
        self.kind = kind
        self.sign = sign
        self.magnitude = magnitude


def decode(bits):
    bits &= 0xFFFFFFFF
    sign = (bits >> 31) & 1
    exponent = (bits >> 23) & 0xFF
    fraction = bits & 0x7FFFFF
    if exponent == 255:
        if fraction == 0:
            return Decoded("inf", sign)
        return Decoded("nan", sign)
    if exponent == 0:
        if fraction == 0:
            return Decoded("zero", sign)
        return Decoded("num", sign, Fraction(fraction) * (Fraction(2) ** -149))
    magnitude = Fraction((1 << 23) + fraction, 1 << 23) * (Fraction(2) ** (exponent - 127))
    return Decoded("num", sign, magnitude)


def pack(sign, magnitude):
    return round_binary32(sign, magnitude)


def negate(value):
    if value.kind == "nan":
        return value
    return Decoded(value.kind, value.sign ^ 1, value.magnitude)


def add_values(left, right):
    if left.kind == "nan" or right.kind == "nan":
        return CANONICAL_NAN
    if left.kind == "inf" or right.kind == "inf":
        if left.kind == "inf" and right.kind == "inf" and left.sign != right.sign:
            return CANONICAL_NAN
        if left.kind == "inf":
            return (left.sign << 31) | 0x7F800000
        return (right.sign << 31) | 0x7F800000
    if left.kind == "zero" and right.kind == "zero":
        if left.sign == 1 and right.sign == 1:
            return 0x80000000
        return 0
    if left.kind == "zero":
        return encode(right)
    if right.kind == "zero":
        return encode(left)
    total = signed_fraction(left) + signed_fraction(right)
    if total == 0:
        return 0
    if total < 0:
        return pack(1, -total)
    return pack(0, total)


def encode(value):
    if value.kind == "nan":
        return CANONICAL_NAN
    if value.kind == "inf":
        return (value.sign << 31) | 0x7F800000
    if value.kind == "zero":
        return value.sign << 31
    return pack(value.sign, value.magnitude)


def signed_fraction(value):
    if value.sign:
        return -value.magnitude
    return value.magnitude


def sub_values(left, right):
    return add_values(left, negate(right))


def mul_values(left, right):
    if left.kind == "nan" or right.kind == "nan":
        return CANONICAL_NAN
    sign = left.sign ^ right.sign
    zero_times_inf = (left.kind == "inf" and right.kind == "zero") or (
        right.kind == "inf" and left.kind == "zero"
    )
    if zero_times_inf:
        return CANONICAL_NAN
    if left.kind == "inf" or right.kind == "inf":
        return (sign << 31) | 0x7F800000
    if left.kind == "zero" or right.kind == "zero":
        return sign << 31
    return pack(sign, left.magnitude * right.magnitude)


def div_values(left, right):
    if left.kind == "nan" or right.kind == "nan":
        return CANONICAL_NAN
    sign = left.sign ^ right.sign
    if left.kind == "inf" and right.kind == "inf":
        return CANONICAL_NAN
    if left.kind == "zero" and right.kind == "zero":
        return CANONICAL_NAN
    if right.kind == "zero":
        return (sign << 31) | 0x7F800000
    if left.kind == "inf":
        return (sign << 31) | 0x7F800000
    if left.kind == "zero" or right.kind == "inf":
        return sign << 31
    return pack(sign, left.magnitude / right.magnitude)


def from_integer(number):
    if number == 0:
        return 0
    if number < 0:
        return pack(1, Fraction(-number))
    return pack(0, Fraction(number))


def from_fixed_count(count):
    if count == 0:
        return 0
    if count < 0:
        return pack(1, Fraction(-count, 65536))
    return pack(0, Fraction(count, 65536))


def trunc_toward_zero(frac):
    if frac >= 0:
        return frac.numerator // frac.denominator
    positive = -frac
    return -(positive.numerator // positive.denominator)


def to_fixed_count(bits):
    value = decode(bits)
    if value.kind in ("nan", "inf"):
        return None
    if value.kind == "zero":
        return 0
    scaled = value.magnitude * 65536
    if value.sign:
        scaled = -scaled
    if scaled >= (1 << 63) or scaled < -(1 << 63):
        return None
    count = trunc_toward_zero(scaled)
    if count < COUNT_MIN or count > COUNT_MAX:
        return None
    return count


def encode_binary64(sign, magnitude):
    """Bits of an exact binary64 value. magnitude is a positive exact double."""
    sign_bit = (sign & 1) << 63
    exponent = floor_log2(magnitude)
    if exponent < -1022:
        scaled = magnitude * (Fraction(2) ** 1074)
        quotient = int(scaled)
        if scaled != quotient:
            print(f"double subnormal is not exact {magnitude}", file=sys.stderr)
            sys.exit(1)
        return sign_bit | quotient
    scaled = magnitude / (Fraction(2) ** (exponent - 52))
    if scaled != int(scaled):
        print(f"double is not exact {magnitude}", file=sys.stderr)
        sys.exit(1)
    quotient = int(scaled)
    if quotient == (1 << 53):
        quotient = 1 << 52
        exponent += 1
    if not (1 << 52) <= quotient < (1 << 53):
        print(f"double significand {quotient}", file=sys.stderr)
        sys.exit(1)
    return sign_bit | ((exponent + 1023) << 52) | (quotient & ((1 << 52) - 1))


def hex_float(bits):
    sign = "-" if (bits >> 63) else ""
    exponent = (bits >> 52) & 0x7FF
    fraction = bits & ((1 << 52) - 1)
    if exponent == 0x7FF:
        if fraction == 0:
            return "-inf" if sign else "inf"
        return "nan"
    if exponent == 0:
        if fraction == 0:
            return "-0" if sign else "0"
        digits = f"{fraction:013x}".rstrip("0")
        return f"{sign}0x0.{digits}p-1022"
    digits = f"{fraction:013x}".rstrip("0")
    body = f"0x1.{digits}" if digits else "0x1"
    return f"{sign}{body}p{exponent - 1023:+d}"


def bits8(bits):
    return f"{bits & 0xFFFFFFFF:08x}"


def require(condition, detail):
    if not condition:
        print(detail, file=sys.stderr)
        sys.exit(1)


def self_check():
    require(floor_log2(Fraction(1)) == 0, "log2 1")
    require(floor_log2(Fraction(3, 2)) == 0, "log2 1.5")
    require(floor_log2(Fraction(1, 3)) == -2, "log2 1/3")
    require(floor_log2(Fraction(2) ** -126) == -126, "log2 min normal")
    require(floor_log2(Fraction(2) ** -149) == -149, "log2 min sub")
    require(round_binary32(0, Fraction(1)) == 0x3F800000, "1")
    require(round_binary32(0, Fraction(1, 2)) == 0x3F000000, "0.5")
    require(round_binary32(1, Fraction(1)) == 0xBF800000, "-1")
    require(round_binary32(0, Fraction(2) ** -149) == 0x00000001, "min sub")
    require(round_binary32(0, Fraction(2) ** -150) == 0, "tie to +0")
    require(round_binary32(1, Fraction(2) ** -150) == 0x80000000, "tie to -0")
    require(round_binary32(0, Fraction(1) + Fraction(2) ** -24) == 0x3F800000, "tie stays 1")
    one_plus = Fraction(1) + Fraction(2) ** -23 + Fraction(2) ** -24
    require(round_binary32(0, one_plus) == 0x3F800002, "tie rounds up")
    max_finite = (Fraction(2) - Fraction(2) ** -23) * (Fraction(2) ** 127)
    require(round_binary32(0, max_finite) == 0x7F7FFFFF, "max finite")
    require(round_binary32(0, max_finite + Fraction(2) ** 102) == 0x7F7FFFFF, "below overflow")
    require(round_binary32(0, OVERFLOW_AT) == 0x7F800000, "overflow tie")
    midpoint = (Fraction(2) ** -126) - (Fraction(2) ** -150)
    require(round_binary32(0, midpoint) == 0x00800000, "subnormal tie to normal")
    require(from_integer(1 << 24) == 0x4B800000, "2^24")
    require(from_integer((1 << 24) + 1) == 0x4B800000, "2^24+1 tie")
    require(from_integer((1 << 24) + 2) == 0x4B800001, "2^24+2")
    require(from_integer((1 << 24) + 3) == 0x4B800002, "2^24+3 tie")
    require(from_integer(-(1 << 63)) == 0xDF000000, "int64 min")
    require(from_integer((1 << 63) - 1) == 0x5F000000, "int64 max")
    # Above 2^53 a conversion through binary64 can round twice. This integer
    # sits just below a binary32 tie whose even side is the higher value, so
    # the one rounding is the lower value.
    twice_rounded = (1 << 60) + (1 << 37) + (1 << 36) - 1
    require(from_integer(twice_rounded) == 0x5D800001, "int above 2^53")
    for bits in (
        0x00000001,
        0x007FFFFF,
        0x00800000,
        0x3F800000,
        0x3F800001,
        0x33800000,
        0x7F7FFFFF,
    ):
        value = decode(bits)
        require(pack(value.sign, value.magnitude) == bits, f"round trip {bits:#x}")
    one = decode(0x3F800000)
    half_ulp = decode(0x33800000)
    next_one = decode(0x3F800001)
    require(add_values(one, one) == 0x40000000, "1+1")
    require(add_values(one, half_ulp) == 0x3F800000, "1 + 2^-24")
    require(add_values(next_one, half_ulp) == 0x3F800002, "odd tie")
    require(add_values(decode(0x80000000), decode(0x80000000)) == 0x80000000, "-0 + -0")
    require(add_values(decode(0), decode(0x80000000)) == 0, "+0 + -0")
    require(sub_values(decode(0x80000000), decode(0)) == 0x80000000, "-0 - +0")
    require(div_values(decode(0x00000001), decode(0x40000000)) == 0, "min sub / 2")
    require(div_values(decode(0x80000001), decode(0x40000000)) == 0x80000000, "-min sub / 2")
    require(div_values(one, decode(0)) == 0x7F800000, "1/0")
    require(div_values(decode(0xBF800000), decode(0)) == 0xFF800000, "-1/0")
    require(mul_values(decode(0x7F800000), decode(0)) == CANONICAL_NAN, "inf * 0")
    require(sub_values(decode(0x7F800000), decode(0x7F800000)) == CANONICAL_NAN, "inf - inf")
    tie_count = ((1 << 24) + 1) * 65536
    require(from_fixed_count(tie_count) == 0x4B800000, "fixed tie")
    require(to_fixed_count(0x7FC00000) is None, "nan tofixed")
    require(to_fixed_count(0x7F800000) is None, "inf tofixed")
    require(to_fixed_count(0x5F000000) is None, "2^63 tofixed")
    # 2^47 is exact in binary32: exponent 47, bias 127, field 174.
    two_47 = (47 + 127) << 23
    require(decode(two_47).magnitude == Fraction(2) ** 47, "2^47 bits")
    require(to_fixed_count(two_47) is None, "2^47 tofixed")


def main():
    self_check()
    operands = [
        0x00000000,
        0x80000000,
        0x3F800000,
        0xBF800000,
        0x40000000,
        0x3F000000,
        0x33800000,
        0x3F800001,
        0x00000001,
        0x80000001,
        0x00800000,
        0x7F7FFFFF,
        0x7F800000,
        0xFF800000,
        0x7FC00000,
    ]
    operations = (
        ("add", add_values),
        ("sub", sub_values),
        ("mul", mul_values),
        ("div", div_values),
    )
    arithmetic = 0
    for name, function in operations:
        for left in operands:
            for right in operands:
                add(name, bits8(left), bits8(right), bits8(function(decode(left), decode(right))))
                arithmetic += 1
    if arithmetic < 200:
        print(f"only {arithmetic} arithmetic rows", file=sys.stderr)
        sys.exit(1)

    integers = [
        0,
        1,
        -1,
        2,
        -2,
        3,
        -3,
        10,
        -10,
        100,
        -100,
        100000,
        -100000,
        (1 << 23) - 1,
        1 << 23,
        (1 << 23) + 1,
        -(1 << 23) - 1,
        (1 << 24) - 1,
        1 << 24,
        (1 << 24) + 1,
        (1 << 24) + 2,
        (1 << 24) + 3,
        (1 << 24) + 4,
        -((1 << 24) + 1),
        -((1 << 24) + 3),
        (1 << 25) + 3,
        (1 << 31) - 1,
        1 << 31,
        -(1 << 31),
        (1 << 31) + (1 << 8),
        (1 << 53) - 1,
        1 << 53,
        (1 << 53) + 1,
        (1 << 53) + (1 << 29),
        (1 << 53) + (1 << 29) + 1,
        (1 << 53) + (1 << 30),
        -((1 << 53) + 1),
        -((1 << 53) + (1 << 29)),
        1 << 62,
        (1 << 62) + (1 << 39),
        (1 << 62) + (1 << 39) + 1,
        -(1 << 62),
        (1 << 63) - 1,
        (1 << 63) - (1 << 40),
        (1 << 63) - (1 << 39),
        -(1 << 63),
        16777217,
        -16777217,
        33554433,
        -33554433,
        123456789,
        -123456789,
        65536,
        -65536,
        65537,
        -65537,
        (1 << 40) + 5,
        -((1 << 40) + 5),
        (1 << 50) + (1 << 27) + 1,
        (1 << 16) + 1,
        42,
        -42,
        (1 << 24) + 5,
        (1 << 24) + 6,
        (1 << 24) + 7,
        999999,
        -999999,
        (1 << 60) + (1 << 37) + (1 << 36) - 1,
        (1 << 60) + (1 << 37) + (1 << 36),
        (1 << 60) + (1 << 37),
    ]
    if len(integers) < 60:
        print(f"only {len(integers)} integers", file=sys.stderr)
        sys.exit(1)
    for number in integers:
        add("i64", str(number), "", bits8(from_integer(number)))

    doubles = [
        ("0", 0),
        ("-0", 0x80000000),
        ("inf", 0x7F800000),
        ("-inf", 0xFF800000),
        ("nan", CANONICAL_NAN),
    ]
    magnitudes = [
        Fraction(1),
        Fraction(1, 2),
        Fraction(2),
        Fraction(3),
        Fraction(1) + Fraction(2) ** -24,
        Fraction(1) + Fraction(2) ** -23,
        Fraction(1) + Fraction(2) ** -23 + Fraction(2) ** -24,
        Fraction(1) + Fraction(2) ** -52,
        Fraction(2) ** -149,
        Fraction(2) ** -150,
        Fraction(3) * Fraction(2) ** -150,
        Fraction(2) ** -126,
        (Fraction(2) ** -126) - (Fraction(2) ** -150),
        (Fraction(2) - Fraction(2) ** -23) * (Fraction(2) ** 127),
        (Fraction(2) - Fraction(2) ** -23) * (Fraction(2) ** 127) + Fraction(2) ** 102,
        OVERFLOW_AT,
        Fraction(2) ** 127,
        Fraction(2) ** 128,
        Fraction(2) ** 200,
        Fraction(2) ** 24 + 1,
        Fraction(2) ** 24 + 3,
        Fraction(10),
        Fraction(1, 8),
        Fraction(5, 4),
        Fraction(2) ** -1074,
        Fraction(2) ** -1022,
        Fraction(1, 65536),
        Fraction(65536),
        Fraction(2) ** 47,
        Fraction(2) ** 31,
        Fraction(2) ** 31 - Fraction(2) ** 8,
        Fraction(123456789, 65536),
        Fraction(2) ** -10 + Fraction(2) ** -40,
        Fraction(3, 2),
        Fraction(7, 2),
    ]
    finite_doubles = []
    for magnitude in magnitudes:
        if magnitude == 0:
            continue
        finite_doubles.append((0, magnitude))
        finite_doubles.append((1, magnitude))
    # Keep a deterministic 35 finite values, both signs already included.
    finite_doubles = finite_doubles[:35]
    if len(doubles) + len(finite_doubles) < 40:
        print(
            f"only {len(doubles) + len(finite_doubles)} doubles",
            file=sys.stderr,
        )
        sys.exit(1)
    for token, bits in doubles:
        add("f64", token, "", bits8(bits))
    for sign, magnitude in finite_doubles:
        token = hex_float(encode_binary64(sign, magnitude))
        add("f64", token, "", bits8(pack(sign, magnitude)))

    fixed_counts = [
        0,
        1,
        -1,
        2,
        -2,
        65535,
        -65535,
        65536,
        -65536,
        65537,
        -65537,
        32768,
        -32768,
        98304,
        -98304,
        104857,
        -104857,
        157285,
        (1 << 24) * 65536,
        ((1 << 24) + 1) * 65536,
        ((1 << 24) + 2) * 65536,
        ((1 << 24) + 3) * 65536,
        -((1 << 24) + 1) * 65536,
        (1 << 31) - 1,
        -(1 << 31),
        (1 << 31),
        2147483647,
        -2147483647 - 1,
        3 * 65536 + 1,
        -3 * 65536 - 1,
        100000,
        -100000,
        COUNT_MAX,
        COUNT_MIN,
        (1 << 46),
        -(1 << 46),
        16777217,
        -16777217,
        (1 << 20) + 3,
        5 * 65536 + 32768,
        7,
        -9,
        42,
        (1 << 30) + 12345,
        9 * 65536 + 3,
    ]
    if len(fixed_counts) < 40:
        print(f"only {len(fixed_counts)} fixed counts", file=sys.stderr)
        sys.exit(1)
    for count in fixed_counts:
        add("fixed", str(count), "", bits8(from_fixed_count(count)))

    tofixed_bits = [
        0x00000000,
        0x80000000,
        0x3F800000,
        0xBF800000,
        0x3F000000,
        0xBF000000,
        0x3FC00000,
        0xBFC00000,
        0x3F800001,
        0xBF800001,
        0x00000001,
        0x80000001,
        0x00800000,
        0x37800000,
        0x3F000001,
        0x4B800000,
        0x4B800001,
        0x42C80000,
        0xC2C80000,
        0x4F000000,
        0x4EFFFFFF,
        0xCF000000,
        0x5F000000,
        0x7F7FFFFF,
        0xFF7FFFFF,
        0x7F800000,
        0xFF800000,
        0x7FC00000,
        0xFFC00000,
        0x7F800001,
        0x40400000,
        0xC0400000,
        0x3EAAAAAB,
        0x501502F9,
        0x3F8CCCCD,
        0x322BCC77,
        0x43000000,
        0xC3000000,
        0x3F801000,
        0x2C800000,
    ]
    if len(tofixed_bits) < 40:
        print(f"only {len(tofixed_bits)} tofixed rows", file=sys.stderr)
        sys.exit(1)
    for bits in tofixed_bits:
        count = to_fixed_count(bits)
        add("tofixed", bits8(bits), "", "error" if count is None else str(count))

    print(f"{len(rows)} rows", file=sys.stderr)
    print("// H02-F32-GOLDEN-BEGIN")
    for op, left, right, result in rows:
        print(f'    {{"{op}", "{left}", "{right}", "{result}"}},')
    print("// H02-F32-GOLDEN-END")


if __name__ == "__main__":
    main()

# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Outputs of the integer network runner's fixed cases. Run with:
#   python3 -I src/data/neural/tests/neural_reference.py
# The stdout, from N02-NEURAL-REFERENCE-BEGIN through
# N02-NEURAL-REFERENCE-END, is pasted into neural_test.cpp. The arithmetic
# is whole numbers only, the same rules as the network file. Nothing here
# calls the engine.

import hashlib
import sys

INT64_MIN = -1 << 63
INT64_MAX = (1 << 63) - 1


def rescale(value, src, dst):
    """Move value from exponent src to exponent dst, in 64 bits."""
    places = dst - src
    if places == 0:
        return value
    if places > 0:
        if places >= 64:
            return 0
        bias = 1 << (places - 1)
        return (value + bias) >> places
    shift = -places
    result = value << shift
    if result > INT64_MAX:
        return INT64_MAX
    if result < INT64_MIN:
        return INT64_MIN
    return result


def sat16(value):
    if value > 32767:
        return 32767
    if value < -32768:
        return -32768
    return value


def dense(samples, weights, bias, in_exp, weight_exp, out_exp):
    """weights is a list of rows. bias is a list, or None."""
    out = []
    for row, row_weights in enumerate(weights):
        total = 0 if bias is None else bias[row]
        for sample, weight in zip(samples, row_weights):
            total += sample * weight
        out.append(sat16(rescale(total, in_exp + weight_exp, out_exp)))
    return out


def relu(samples):
    return [sample if sample >= 0 else 0 for sample in samples]


def leaky_relu(samples, alpha):
    out = []
    for sample in samples:
        if sample >= 0:
            out.append(sample)
        else:
            out.append((sample * alpha) >> 15)
    return out


def held_sum(left, right):
    total = left + right
    return sat16(total)


def add(left, left_exp, right, right_exp, out_exp):
    out = []
    for sample, other in zip(left, right):
        out.append(
            held_sum(rescale(sample, left_exp, out_exp), rescale(other, right_exp, out_exp))
        )
    return out


def concat_rank1(left, left_exp, right, right_exp, out_exp):
    out = [sat16(rescale(sample, left_exp, out_exp)) for sample in left]
    out.extend(sat16(rescale(sample, right_exp, out_exp)) for sample in right)
    return out


def concat_axis1(left_rows, left_exp, right_rows, right_exp, out_exp):
    """Rank 2, axis 1. Each row is a list. The right side is one column."""
    out = []
    for left_row, right_value in zip(left_rows, right_rows):
        out.extend(sat16(rescale(sample, left_exp, out_exp)) for sample in left_row)
        out.append(sat16(rescale(right_value, right_exp, out_exp)))
    return out


def flatten(samples):
    return list(samples)


def argmax(samples):
    best = 0
    for element, sample in enumerate(samples):
        if sample > samples[best]:
            best = element
    return best


def embedding(indexes, table, width):
    out = []
    for row in indexes:
        if row < 0 or row >= len(table) // width:
            raise RuntimeError(f"row {row}")
        start = row * width
        out.extend(table[start : start + width])
    return out


class Lcg:
    """The sequence the mixed network's test writes. State starts at 1."""

    def __init__(self):
        self.state = 1

    def next_u32(self):
        self.state = (self.state * 1664525 + 1013904223) & 0xFFFFFFFF
        return self.state

    def next_i32(self):
        word = self.next_u32()
        if word >= 0x80000000:
            return word - 0x100000000
        return word

    def next_i16(self):
        word = (self.next_u32() >> 16) & 0xFFFF
        if word >= 0x8000:
            return word - 0x10000
        return word

    def next_i8(self):
        word = (self.next_u32() >> 24) & 0xFF
        if word >= 0x80:
            return word - 0x100
        return word


def row_of(value, rows):
    reduced = value % rows
    if reduced < 0:
        reduced += rows
    return reduced


def mixed_outputs():
    """The mixed network in neural_test.cpp, from the same sequence."""
    stream = Lcg()
    weights8 = [stream.next_i8() for _ in range(32)]
    weights16 = [stream.next_i16() for _ in range(256)]
    bias = [stream.next_i32() for _ in range(8)]
    raw = [stream.next_i16() for _ in range(64)]

    tokens = [row_of(value, 16) for value in raw[0:16]]
    feat = raw[16:48]
    side = raw[48:56]
    tail = raw[56:64]

    embedded = embedding(tokens, weights8, 2)
    flat = flatten(embedded)
    mixed = add(flat, 0, feat, 1, 0)
    rows = [weights16[row * 32 : (row + 1) * 32] for row in range(8)]
    hidden = dense(mixed, rows, bias, 0, 0, 0)
    activated = relu(hidden)
    leaked = leaky_relu(activated, 3277)
    body = add(leaked, 0, side, 1, 0)
    joined = concat_rank1(body, 0, tail, 2, 0)
    pick = argmax(joined)
    return joined, pick


def digest_of(joined, pick):
    raw = bytearray()
    for value in joined:
        bits = value & 0xFFFF
        raw.append(bits & 0xFF)
        raw.append((bits >> 8) & 0xFF)
    bits = pick & 0xFFFFFFFF
    raw.append(bits & 0xFF)
    raw.append((bits >> 8) & 0xFF)
    raw.append((bits >> 16) & 0xFF)
    raw.append((bits >> 24) & 0xFF)
    return hashlib.sha256(raw).hexdigest()


def cpp_array(kind, name, values):
    if isinstance(values, int):
        return f"constexpr {kind} {name} = {values};"
    body = ", ".join(str(value) for value in values)
    return f"constexpr {kind} {name}[] = {{{body}}};"


def expect(actual, hand, label):
    if actual != hand:
        print(f"{label}: got {actual}, hand {hand}", file=sys.stderr)
        sys.exit(1)


def main():
    expect(rescale(3, 0, 1), 2, "rescale tie")
    expect(rescale(-3, 0, 1), -1, "rescale negative tie")
    expect(dense([2, 3], [[1, 0], [4, -1]], None, 0, 0, 0), [2, 5], "dense shift 0")
    expect(dense([2, 3], [[1, 0], [4, -1]], [10, -2], 0, 0, 0), [12, 3], "dense bias")
    # 7*400 + -3*200 = 2200; (2200 + 256) >> 9 = 4
    # 7*-400 + -3*100 = -3100; (-3100 + 256) >> 9 = -6
    expect(
        dense([7, -3], [[400, 200], [-400, 100]], None, 0, 0, 9),
        [4, -6],
        "dense shift 9",
    )
    # bias added before the shift: 2300 and -3150, both still shift to 4 and -6
    expect(
        dense([7, -3], [[400, 200], [-400, 100]], [100, -50], 0, 0, 9),
        [4, -6],
        "dense shift 9 bias",
    )
    expect(relu([-2, 0, 5, -32768]), [0, 0, 5, 0], "relu")
    # -1*3277 >> 15 = -1; -10*3277 = -32770 >> 15 = -2; -32768*3277 >> 15 = -3277
    expect(leaky_relu([5, 0, -1, -10, -32768], 3277), [5, 0, -1, -2, -3277], "leaky")
    expect(add([2, -3, 3, -3], 0, [1, 4, 1, 0], 1, 0), [4, 5, 5, -3], "add")
    # R(3, 0, 1) + R(1, 0, 1) = 2 + 1; R(-3, 0, 1) + R(1, 0, 1) = -1 + 1
    expect(add([3, -3], 0, [1, 1], 0, 1), [3, 0], "add round")
    expect(concat_rank1([3, -3], 0, [8, 1], 1, 0), [3, -3, 16, 2], "concat")
    expect(
        concat_axis1([[1, 2], [3, 4]], 0, [1, -1], 2, 0),
        [1, 2, 4, 3, 4, -4],
        "concat axis",
    )
    expect(flatten([1, -2, 3, 4, -5, 6]), [1, -2, 3, 4, -5, 6], "flatten")
    expect(argmax([1, 4, 4, -2]), 1, "argmax")
    expect(
        embedding([2, 0, 1], [1, -2, 3, 4, -5, 6], 2),
        [-5, 6, 1, -2, 3, 4],
        "embed i8",
    )
    expect(
        embedding([1, 0, 1], [1000, -1000, 7, 8], 2),
        [7, 8, 1000, -1000, 7, 8],
        "embed i16",
    )

    joined, pick = mixed_outputs()
    digest = digest_of(joined, pick)
    lines = [
        "// N02-NEURAL-REFERENCE-BEGIN",
        cpp_array("int16_t", "k_dense_shift0", [2, 5]),
        cpp_array("int16_t", "k_dense_shift0_bias", [12, 3]),
        cpp_array("int16_t", "k_dense_shift9", [4, -6]),
        cpp_array("int16_t", "k_dense_shift9_bias", [4, -6]),
        cpp_array("int16_t", "k_relu", [0, 0, 5, 0]),
        cpp_array("int16_t", "k_leaky", [5, 0, -1, -2, -3277]),
        cpp_array("int16_t", "k_add", [4, 5, 5, -3]),
        cpp_array("int16_t", "k_add_round", [3, 0]),
        cpp_array("int16_t", "k_concat", [3, -3, 16, 2]),
        cpp_array("int16_t", "k_concat_axis", [1, 2, 4, 3, 4, -4]),
        cpp_array("int16_t", "k_flat", [1, -2, 3, 4, -5, 6]),
        cpp_array("int32_t", "k_argmax", 1),
        cpp_array("int16_t", "k_embed_i8", [-5, 6, 1, -2, 3, 4]),
        cpp_array("int16_t", "k_embed_i16", [7, 8, 1000, -1000, 7, 8]),
        cpp_array("int16_t", "k_mixed_cat", joined),
        cpp_array("int32_t", "k_mixed_pick", pick),
        f'constexpr std::string_view pinned_mixed_digest = "{digest}";',
        "// N02-NEURAL-REFERENCE-END",
    ]
    sys.stdout.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()

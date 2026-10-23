# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# Outputs of the integer network runner's fixed cases. Run with:
#   python3 -I src/data/neural/tests/neural_reference.py
# The stdout, from N02-NEURAL-REFERENCE-BEGIN through
# N02-NEURAL-REFERENCE-END, and from N06-NEURAL-REFERENCE-BEGIN through
# N06-NEURAL-REFERENCE-END, is pasted into neural_test.cpp. The arithmetic
# is whole numbers only, the same rules as the network file. Nothing here
# calls the engine. The tanh, sigmoid and exponent tables come from
# tables_reference.py.

import hashlib
import sys
from pathlib import Path

# python3 -I leaves the script's directory off sys.path. The tables live beside this file.
sys.path.insert(0, str(Path(__file__).resolve().parent))
import tables_reference

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


def conv2d(
    samples,
    channels,
    height,
    width,
    weights,
    kernels,
    kernel_h,
    kernel_w,
    stride_h,
    stride_w,
    pad_h,
    pad_w,
    bias,
    in_exp,
    weight_exp,
    out_exp,
):
    """samples is [c, h, w]. weights is [k, c, kernel_h, kernel_w]. A miss reads 0."""
    out_h = (height + 2 * pad_h - kernel_h) // stride_h + 1
    out_w = (width + 2 * pad_w - kernel_w) // stride_w + 1
    out = []
    for kernel in range(kernels):
        for out_y in range(out_h):
            for out_x in range(out_w):
                total = 0 if bias is None else bias[kernel]
                for channel in range(channels):
                    for kernel_y in range(kernel_h):
                        for kernel_x in range(kernel_w):
                            in_y = out_y * stride_h + kernel_y - pad_h
                            in_x = out_x * stride_w + kernel_x - pad_w
                            sample = 0
                            if 0 <= in_y < height and 0 <= in_x < width:
                                sample = samples[(channel * height + in_y) * width + in_x]
                            weight_at = (
                                ((kernel * channels + channel) * kernel_h + kernel_y) * kernel_w
                                + kernel_x
                            )
                            total += sample * weights[weight_at]
                out.append(sat16(rescale(total, in_exp + weight_exp, out_exp)))
    return out


def maxpool(samples, channels, height, width, kernel_h, kernel_w, stride_h, stride_w):
    """The largest value in each window. The window stays inside the input."""
    out_h = (height - kernel_h) // stride_h + 1
    out_w = (width - kernel_w) // stride_w + 1
    out = []
    for channel in range(channels):
        for out_y in range(out_h):
            for out_x in range(out_w):
                best = None
                for kernel_y in range(kernel_h):
                    for kernel_x in range(kernel_w):
                        in_y = out_y * stride_h + kernel_y
                        in_x = out_x * stride_w + kernel_x
                        sample = samples[(channel * height + in_y) * width + in_x]
                        if best is None or sample > best:
                            best = sample
                out.append(best)
    return out


def activation_index(value, exponent):
    """clamp(R(value, exponent, -8) + 2048, 0, 4095)."""
    scaled = rescale(value, exponent, -8)
    indexed = scaled + 2048
    if indexed < 0:
        return 0
    if indexed > 4095:
        return 4095
    return indexed


def tanh_activation(samples, exponent):
    table = tables_reference.tanh_table()
    return [table[activation_index(sample, exponent)] for sample in samples]


def sigmoid_activation(samples, exponent):
    table = tables_reference.sigmoid_table()
    return [table[activation_index(sample, exponent)] for sample in samples]


def softmax(samples, exponent):
    """q is EXP[d] for d <= 4095 and 0 otherwise. The division is whole numbers."""
    table = tables_reference.exp_table()
    peak = samples[0]
    for sample in samples[1:]:
        if sample > peak:
            peak = sample
    terms = []
    for sample in samples:
        distance = rescale(peak - sample, exponent, -8)
        if distance > 4095:
            terms.append(0)
        else:
            terms.append(table[distance])
    total = sum(terms)
    return [(term * 32767 + total // 2) // total for term in terms]


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


def grid_outputs():
    """Sixteen runs of the grid network in neural_test.cpp.

    The sequence starts again at 1. Weights come first, then each picture.
    The picture's exponent is -30 and the convolutions shift the sums down,
    so a picture changes the scores. The second convolution is 1x1.
    """
    stream = Lcg()
    conv_w = [stream.next_i8() for _ in range(54)]
    conv2_w = [stream.next_i8() for _ in range(4)]
    dense_w = [stream.next_i8() for _ in range(512)]
    dense_rows = [dense_w[row * 128 : (row + 1) * 128] for row in range(4)]
    scores = []
    picks = []
    for _ in range(16):
        picture = [stream.next_i16() for _ in range(3 * 16 * 16)]
        convolved = conv2d(picture, 3, 16, 16, conv_w, 2, 3, 3, 1, 1, 1, 1, None, -30, 0, -18)
        activated = relu(convolved)
        pooled = maxpool(activated, 2, 16, 16, 2, 2, 2, 2)
        second = conv2d(pooled, 2, 8, 8, conv2_w, 2, 1, 1, 1, 1, 0, 0, None, -18, 0, -12)
        squashed = tanh_activation(second, -12)
        flat = flatten(squashed)
        hidden = dense(flat, dense_rows, None, -15, 0, -12)
        gated = sigmoid_activation(hidden, -12)
        choice = softmax(gated, -15)
        scores.extend(choice)
        picks.append(argmax(gated))
    return scores, picks


def digest_bytes(scores, picks):
    """Little-endian int16 scores, then one little-endian int32 pick per run."""
    raw = bytearray()
    per_run = len(scores) // len(picks)
    for run, pick in enumerate(picks):
        for value in scores[run * per_run : (run + 1) * per_run]:
            bits = value & 0xFFFF
            raw.append(bits & 0xFF)
            raw.append((bits >> 8) & 0xFF)
        bits = pick & 0xFFFFFFFF
        raw.append(bits & 0xFF)
        raw.append((bits >> 8) & 0xFF)
        raw.append((bits >> 16) & 0xFF)
        raw.append((bits >> 24) & 0xFF)
    return hashlib.sha256(raw).hexdigest()


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
    # 1*1 + 4*1 = 5. Outside the 2x2 window is not read.
    expect(
        conv2d([1, 2, 3, 4], 1, 2, 2, [1, 0, 0, 1], 1, 2, 2, 1, 1, 0, 0, None, 0, 0, 0),
        [5],
        "conv stride 1",
    )
    # The same sum moved to exponent 1: (5 + 1) >> 1 = 3.
    expect(
        conv2d([1, 2, 3, 4], 1, 2, 2, [1, 0, 0, 1], 1, 2, 2, 1, 1, 0, 0, None, 0, 0, 1),
        [3],
        "conv shift",
    )
    # Four 2x2 windows of ones: 14, 22, 46, 54.
    picture = list(range(1, 17))
    expect(
        conv2d(picture, 1, 4, 4, [1, 1, 1, 1], 1, 2, 2, 2, 2, 0, 0, None, 0, 0, 0),
        [14, 22, 46, 54],
        "conv stride 2",
    )
    # Pad 1, kernel 1..9. Corner windows read 0 outside the 2x2.
    # (0,0) = 5+12+24+36 = 77. (0,1) = 4+10+21+32 = 67.
    # (1,0) = 2+6+15+24 = 47. (1,1) = 1+4+12+20 = 37.
    expect(
        conv2d(
            [1, 2, 3, 4],
            1,
            2,
            2,
            [1, 2, 3, 4, 5, 6, 7, 8, 9],
            1,
            3,
            3,
            1,
            1,
            1,
            1,
            None,
            0,
            0,
            0,
        ),
        [77, 67, 47, 37],
        "conv pad",
    )
    # int16 kernel, bias -4 added before the scale: 1*2 + 4*3 - 4 = 10.
    expect(
        conv2d([1, 2, 3, 4], 1, 2, 2, [2, 0, 0, 3], 1, 2, 2, 1, 1, 0, 0, [-4], 0, 0, 0),
        [10],
        "conv bias",
    )
    # Stride 2, pad 1, bias 10, diagonal kernel. Windows read 0 past the edge.
    # 6+10, 3+10, 7+10, 14+10.
    expect(
        conv2d(
            [1, 2, 3, 4, 5, 6, 7, 8, 9],
            1,
            3,
            3,
            [1, 0, 0, 0, 1, 0, 0, 0, 1],
            1,
            3,
            3,
            2,
            2,
            1,
            1,
            [10],
            0,
            0,
            0,
        ),
        [16, 13, 17, 24],
        "conv stride 2 pad bias",
    )
    expect(maxpool(picture, 1, 4, 4, 2, 2, 2, 2), [6, 8, 14, 16], "pool 2")
    # 3x3 stride 1. The -1 is in the first window and is not the largest.
    expect(
        maxpool([1, 2, 3, 4, 5, 6, 7, 8, 9, 0, -1, 10, 11, 12, 13, 14], 1, 4, 4, 3, 3, 1, 1),
        [9, 10, 13, 14],
        "pool 3",
    )
    # Exponent 0: x -8 and 8 clamp to the ends, 0 is the centre, 1 is index 2304.
    expect(tanh_activation([-8, 8, 0, 1], 0), [-32768, 32767, 0, 24956], "tanh")
    expect(sigmoid_activation([-8, 8, 0, 1], 0), [11, 32757, 16384, 23955], "sigmoid")
    # Exponent -8 leaves the stored value as the offset from 2048. 1 is index 2049.
    expect(tanh_activation([1], -8), [128], "tanh index")
    expect(sigmoid_activation([1], -8), [16416], "sigmoid index")
    # Four equal inputs. q = 2^30, S = 2^32, each output is 32769 / 4 = 8192.
    expect(softmax([5, 5, 5, 5], 0), [8192, 8192, 8192, 8192], "softmax equal")
    # The peak's distance is 0. The others are 100 << 8 = 25600, past 4095, so 0.
    # (2^30 * 32767 + 2^29) / 2^30 = 32767.
    expect(softmax([100, 0, 0], 0), [32767, 0, 0], "softmax peak")
    # d(20) = 10 << 8 = 2560. d(0) = 30 << 8 = 7680, which is past 4095, so exactly 0.
    gap = tables_reference.exp_table()[2560]
    total = (1 << 30) + gap
    expect(
        softmax([30, 20, 0], 0),
        [
            ((1 << 30) * 32767 + total // 2) // total,
            (gap * 32767 + total // 2) // total,
            0,
        ],
        "softmax far",
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
    scores, picks = grid_outputs()
    grid_digest = digest_bytes(scores, picks)
    lines.extend(
        [
            "// N06-NEURAL-REFERENCE-BEGIN",
            cpp_array("int16_t", "k_conv_stride1", [5]),
            cpp_array("int16_t", "k_conv_shift", [3]),
            cpp_array("int16_t", "k_conv_stride2", [14, 22, 46, 54]),
            cpp_array("int16_t", "k_conv_pad", [77, 67, 47, 37]),
            cpp_array("int16_t", "k_conv_bias", [10]),
            cpp_array("int16_t", "k_conv_stride2_pad", [16, 13, 17, 24]),
            cpp_array("int16_t", "k_pool2", [6, 8, 14, 16]),
            cpp_array("int16_t", "k_pool3", [9, 10, 13, 14]),
            cpp_array("int16_t", "k_tanh", [-32768, 32767, 0, 24956]),
            cpp_array("int16_t", "k_sigmoid", [11, 32757, 16384, 23955]),
            cpp_array("int16_t", "k_tanh_index", [128]),
            cpp_array("int16_t", "k_sigmoid_index", [16416]),
            cpp_array("int16_t", "k_softmax_equal", [8192, 8192, 8192, 8192]),
            cpp_array("int16_t", "k_softmax_peak", [32767, 0, 0]),
            cpp_array(
                "int16_t",
                "k_softmax_far",
                [
                    ((1 << 30) * 32767 + total // 2) // total,
                    (gap * 32767 + total // 2) // total,
                    0,
                ],
            ),
            cpp_array("int16_t", "k_grid_scores", scores),
            cpp_array("int32_t", "k_grid_picks", picks),
            f'constexpr std::string_view pinned_grid_digest = "{grid_digest}";',
            "// N06-NEURAL-REFERENCE-END",
        ]
    )
    sys.stdout.write("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()

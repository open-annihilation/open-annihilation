#!/usr/bin/env python3
# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

"""QR Code encoder for the registration address: byte mode, level M, versions 1 to 10.

qr_matrix(text) returns rows of booleans, True where a module is dark. Pass qr_version
to force a version; the text is refused when it does not fit. --matrix prints one row
of 0 and 1 per line, which the archive check compares with the rendered frame.
"""

import argparse
import sys

ECC_PER_BLOCK_M = [None, 10, 16, 26, 18, 24, 16, 18, 22, 22, 26]
BLOCKS_M = [None, 1, 1, 1, 2, 2, 4, 4, 4, 5, 5]
FORMAT_BITS_M = 0  # level M's two format bits


def raw_data_modules(ver):
    """Returns how many modules of a version hold data and error correction."""
    result = (16 * ver + 128) * ver + 64
    if ver >= 2:
        num_align = ver // 7 + 2
        result -= (25 * num_align - 10) * num_align - 55
        if ver >= 7:
            result -= 36
    return result


def data_codewords(ver):
    """Returns how many data codewords a version holds at level M."""
    return raw_data_modules(ver) // 8 - ECC_PER_BLOCK_M[ver] * BLOCKS_M[ver]


def alignment_positions(ver):
    """Returns the alignment-pattern centres of a version."""
    if ver == 1:
        return []
    num_align = ver // 7 + 2
    step = (ver * 8 + num_align * 3 + 5) // (num_align * 4 - 4) * 2
    result = [6]
    pos = ver * 4 + 17 - 7
    tail = []
    for _ in range(num_align - 1):
        tail.insert(0, pos)
        pos -= step
    return result + tail


def gf_mul(x, y):
    """Multiplies two bytes in the QR field."""
    z = 0
    for i in range(7, -1, -1):
        z = (z << 1) ^ ((z >> 7) * 0x11D)
        z ^= ((y >> i) & 1) * x
    return z & 0xFF


def rs_divisor(degree):
    """Returns the Reed-Solomon generator of `degree`."""
    result = [0] * (degree - 1) + [1]
    root = 1
    for _ in range(degree):
        for j in range(len(result)):
            result[j] = gf_mul(result[j], root)
            if j + 1 < len(result):
                result[j] ^= result[j + 1]
        root = gf_mul(root, 0x02)
    return result


def rs_remainder(data, divisor):
    """Returns the Reed-Solomon remainder of `data` divided by `divisor`."""
    result = [0] * len(divisor)
    for b in data:
        factor = b ^ result.pop(0)
        result.append(0)
        for i, coef in enumerate(divisor):
            result[i] ^= gf_mul(coef, factor)
    return result


def qr_matrix(text, qr_version=None):
    """Returns the dark-module matrix of `text` at level M.

    With qr_version omitted, the smallest version from 1 to 10 that holds the
    text is used. A given version is used as it is, and a text that does not
    fit is refused.
    """
    data = text.encode("utf-8")
    if qr_version is None:
        for ver in range(1, 11):
            cap = data_codewords(ver)
            if 4 + 8 + len(data) * 8 <= cap * 8:
                break
        else:
            raise ValueError("text too long for versions 1-10")
    else:
        ver = qr_version
        if not 1 <= ver <= 10:
            raise ValueError("version must be from 1 to 10")
        cap = data_codewords(ver)
        if 4 + 8 + len(data) * 8 > cap * 8:
            raise ValueError("text too long for the requested version")
    # Data bits.
    bits = []

    def put(val, n):
        for i in range(n - 1, -1, -1):
            bits.append((val >> i) & 1)

    put(0b0100, 4)
    put(len(data), 8)
    for b in data:
        put(b, 8)
    cap_bits = cap * 8
    put(0, min(4, cap_bits - len(bits)))
    put(0, (8 - len(bits) % 8) % 8)
    pad = 0xEC
    while len(bits) < cap_bits:
        put(pad, 8)
        pad ^= 0xEC ^ 0x11
    codewords = [int("".join(map(str, bits[i : i + 8])), 2) for i in range(0, len(bits), 8)]
    # Error correction and interleaving.
    nblocks, ecclen = BLOCKS_M[ver], ECC_PER_BLOCK_M[ver]
    raw = raw_data_modules(ver) // 8
    nshort = nblocks - raw % nblocks
    short_len = raw // nblocks
    div = rs_divisor(ecclen)
    blocks, k = [], 0
    for i in range(nblocks):
        dat = codewords[k : k + short_len - ecclen + (0 if i < nshort else 1)]
        k += len(dat)
        ecc = rs_remainder(dat, div)
        if i < nshort:
            dat = dat + [None]
        blocks.append(dat + ecc)
    final = []
    for i in range(len(blocks[0])):
        for j, blk in enumerate(blocks):
            if i != short_len - ecclen or j >= nshort:
                final.append(blk[i])
    # Modules.
    size = ver * 4 + 17
    mod = [[False] * size for _ in range(size)]
    fn = [[False] * size for _ in range(size)]

    def setf(x, y, dark):
        mod[y][x] = dark
        fn[y][x] = True

    for i in range(size):
        setf(6, i, i % 2 == 0)
        setf(i, 6, i % 2 == 0)

    def finder(cx, cy):
        for dy in range(-4, 5):
            for dx in range(-4, 5):
                d = max(abs(dx), abs(dy))
                x, y = cx + dx, cy + dy
                if 0 <= x < size and 0 <= y < size:
                    setf(x, y, d not in (2, 4))

    finder(3, 3)
    finder(size - 4, 3)
    finder(3, size - 4)
    ap = alignment_positions(ver)
    n = len(ap)
    for i in range(n):
        for j in range(n):
            if (i == 0 and j == 0) or (i == 0 and j == n - 1) or (i == n - 1 and j == 0):
                continue
            for dy in range(-2, 3):
                for dx in range(-2, 3):
                    setf(ap[i] + dx, ap[j] + dy, max(abs(dx), abs(dy)) != 1)

    def draw_format(mask):
        dat = (FORMAT_BITS_M << 3) | mask
        rem = dat
        for _ in range(10):
            rem = (rem << 1) ^ ((rem >> 9) * 0x537)
        b = ((dat << 10) | rem) ^ 0x5412
        bit = lambda i: ((b >> i) & 1) != 0
        for i in range(0, 6):
            setf(8, i, bit(i))
        setf(8, 7, bit(6))
        setf(8, 8, bit(7))
        setf(7, 8, bit(8))
        for i in range(9, 15):
            setf(14 - i, 8, bit(i))
        for i in range(0, 8):
            setf(size - 1 - i, 8, bit(i))
        for i in range(8, 15):
            setf(8, size - 15 + i, bit(i))
        setf(8, size - 8, True)

    draw_format(0)  # reserve the format areas
    # Place the codewords.
    i = 0
    allbits = []
    for cw in final:
        for b in range(7, -1, -1):
            allbits.append((cw >> b) & 1)
    right = size - 1
    while right >= 1:
        if right == 6:
            right = 5
        for vert in range(size):
            for j in range(2):
                x = right - j
                upward = ((right + 1) & 2) == 0
                y = size - 1 - vert if upward else vert
                if not fn[y][x] and i < len(allbits):
                    mod[y][x] = allbits[i] == 1
                    i += 1
        right -= 2
    base = [row[:] for row in mod]

    def masked(m):
        out = [row[:] for row in base]
        for y in range(size):
            for x in range(size):
                if fn[y][x]:
                    continue
                inv = [
                    (x + y) % 2 == 0,
                    y % 2 == 0,
                    x % 3 == 0,
                    (x + y) % 3 == 0,
                    (x // 3 + y // 2) % 2 == 0,
                    x * y % 2 + x * y % 3 == 0,
                    (x * y % 2 + x * y % 3) % 2 == 0,
                    ((x + y) % 2 + x * y % 3) % 2 == 0,
                ][m]
                if inv:
                    out[y][x] = not out[y][x]
        return out

    def penalty(g):
        p = 0
        for lines in (g, [list(c) for c in zip(*g)]):
            for row in lines:
                run, color = 0, None
                for v in row:
                    if v == color:
                        run += 1
                    else:
                        if run >= 5:
                            p += run - 2
                        run, color = 1, v
                if run >= 5:
                    p += run - 2
                s = "".join("1" if v else "0" for v in row)
                p += 40 * (s.count("10111010000") + s.count("00001011101"))
        for y in range(size - 1):
            for x in range(size - 1):
                if g[y][x] == g[y][x + 1] == g[y + 1][x] == g[y + 1][x + 1]:
                    p += 3
        dark = sum(v for row in g for v in row)
        p += abs(dark * 20 - size * size * 10) // (size * size) * 10
        return p

    best = None
    for m in range(8):
        g = masked(m)
        mod[:] = g
        draw_format(m)
        g = [row[:] for row in mod]
        pen = penalty(g)
        if best is None or pen < best[0]:
            best = (pen, g)
    return best[1]


def main(argv):
    """Prints the module matrix of --matrix at --qr-version."""
    parser = argparse.ArgumentParser(description="Print a QR Code module matrix, level M.")
    parser.add_argument("--matrix", metavar="TEXT", help="text to encode; one row of 0 and 1 is printed per module row")
    parser.add_argument(
        "--qr-version",
        type=int,
        help="version from 1 to 10; omit to use the smallest version that holds the text",
    )
    args = parser.parse_args(argv)
    if args.matrix is None:
        parser.error("give --matrix TEXT")
    try:
        matrix = qr_matrix(args.matrix, args.qr_version)
    except ValueError as error:
        print(f"qr: {error}", file=sys.stderr)
        return 1
    for row in matrix:
        print("".join("1" if dark else "0" for dark in row))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))

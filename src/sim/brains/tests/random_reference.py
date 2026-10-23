# SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
# SPDX-License-Identifier: GPL-3.0-only

# The brain's random seed and the first ten math.random(1, 100) draws.
# Run with python3 -I. The stdout is the table in random_test.cpp.
# set_seed, next_word and project are Lua 5.4's xoshiro256**, copied from
# src/data/script-host/tests/random_reference.py.

import hashlib

MASK = (1 << 64) - 1


def rotate_left(bits, places):
    bits &= MASK
    return ((bits << places) | (bits >> (64 - places))) & MASK


def next_word(word):
    state0, state1, state2, state3 = word
    mixed2 = (state2 ^ state0) & MASK
    mixed3 = (state3 ^ state1) & MASK
    drawn = (rotate_left((state1 * 5) & MASK, 7) * 9) & MASK
    return drawn, [
        (state0 ^ mixed3) & MASK,
        (state1 ^ mixed2) & MASK,
        (mixed2 ^ ((state1 << 17) & MASK)) & MASK,
        rotate_left(mixed3, 45),
    ]


def set_seed(first, second):
    word = [first & MASK, 0xFF, second & MASK, 0]
    for _step in range(16):
        _drawn, word = next_word(word)
    return word


def project(drawn, limit, word):
    drawn &= MASK
    limit &= MASK
    if (limit & (limit + 1)) == 0:
        return drawn & limit, word
    mask = limit
    mask |= mask >> 1
    mask |= mask >> 2
    mask |= mask >> 4
    mask |= mask >> 8
    mask |= mask >> 16
    mask |= mask >> 32
    mask &= MASK
    while True:
        drawn &= mask
        if drawn <= limit:
            return drawn, word
        drawn, word = next_word(word)


def brain_seed(match_seed, player, brain_id, version):
    # Match seed as 8 little-endian bytes, zero-extended from 32 bits, then
    # the player, the id, a zero byte and the version.
    material = (
        int(match_seed).to_bytes(8, "little")
        + bytes([player])
        + brain_id.encode("utf-8")
        + b"\x00"
        + version.encode("utf-8")
    )
    digest = hashlib.sha256(material).digest()
    return int.from_bytes(digest[:8], "little")


def ten_bounded(first, second):
    word = set_seed(first, second)
    values = []
    for _step in range(10):
        drawn, word = next_word(word)
        projected, word = project(drawn, 99, word)
        values.append(projected + 1)
    return values


CASES = (
    (12345, 1, "steady-commander", "1.2"),
    (0, 0, "steady-commander", "1.2"),
    (12345, 2, "steady-commander", "1.2"),
    (12345, 1, "steady-commander", "1.3"),
)


def main():
    for match_seed, player, brain_id, version in CASES:
        seed = brain_seed(match_seed, player, brain_id, version)
        draws = ", ".join(str(value) for value in ten_bounded(seed, 0))
        print(
            f'    {{{match_seed}u, {player}u, "{brain_id}", "{version}", '
            f"0x{seed:016x}ULL, {{{draws}}}}},"
        )


if __name__ == "__main__":
    main()

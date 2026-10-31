# Game maths

Integer and floating-point helpers whose results must match 3.1c bit for bit:
the game's arctangent, sine, cosine, arccosine, vector length and square
root, the unit-script coordinate queries built on them, fixed-point vector
scaling, 3D lengths, the shared random generator and the float-keyed max-heap
the computer player uses to rank metal spots by distance.

## Floating-point functions

3.1c keeps the results of its arctangent, sine and cosine, and some steps in
between, with a 64-bit significand, while its other arithmetic rounds every
result to a 53-bit significand. `oa/base/game_math/extended.hpp` gives that
arithmetic: `Extended`, a finite number with a 64-bit significand and a wide
exponent range, and its addition, subtraction, multiplication, division and
square root, each rounded once to 53 or 64 bits, to nearest with ties to
even. All of it is integer arithmetic, so it gives the same results with
every compiler, processor and maths library.

- `arctangent(y, x)`, `sine`, `cosine` and `sine_cosine` return the exact
  value rounded to a 64-bit significand. They evaluate a series with a
  128-bit significand after an exact reduction of the argument, which leaves
  far more precision than the rounding needs.
- `direction(x, z)` is the game's heading of a vector: the arctangent times
  the radians-to-heading constant, rounded to 53 bits, then to the nearest
  integer. Unit steering, terrain slopes, weapon aiming and COB GET
  selectors 12 and 14 use it.
- `rotate_pair` rotates a coordinate pair by an angle word: each product of
  a coordinate with the sine or cosine and each sum is rounded to 53 bits.
  `multiply_to_double` rounds each product straight from the 128-bit product
  of the significands, to the double `to_double(multiply(..., bits_53))`
  gives; where the product is not a normal double it takes that path.
- `hypotenuse(x, y)` is the game's vector length: the magnitudes divided by
  the larger one, squared and summed, square-rooted and multiplied back, with
  every step rounded to 64 bits and then again to 53. The second rounding
  can differ from rounding once: the 52-165-173 triangle measures
  173.00000000000003, where rounding each step once gives
  172.99999999999997. `planar_length` and `distance` (COB GET selectors 13
  and 15) use it.
- `arccosine(x)` is the arctangent of sqrt((1 + x)(1 - x)) and x, those steps
  rounded to doubles; it returns no value outside [-1, 1], where the game's
  result is NaN.
- `square_root` is the IEEE 754 square root, which every platform rounds
  exactly.

`truncated_length` sums its squares as z^2 + y^2 before x^2 and truncates the
root toward zero to 64 bits, keeping the low 32 bits.

The game converts a float or a double to an integer by truncating it toward
zero to 64 bits; where it keeps an `int`, it keeps the low 32 bits of that.
`truncate_to_int64` and `truncate_low32` are those two conversions, shared by
every module that makes them. A NaN, an infinity or a value outside
[-2^63, 2^63) gives INT64_MIN, whose low 32 bits are 0. The `game-math` test
checks both at the half, 2^31, 2^32 + 1 and 2^63 edges, and at NaN and the
infinities.

The `game-math-extended` test checks the 53-bit operations against IEEE 754
doubles, `multiply_to_double` against the general multiply and rounding (the
sine and cosine of every angle word at every scale of coordinate, factors
across the exponent range, ties and their neighbours), the sine and cosine of
every angle word and the rotation of points by every angle word, and pins the
results over fixed samples of headings, arctangents, lengths, arccosines and
square roots by digest.

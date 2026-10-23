# Network files

`oa-formats-oann` reads, checks and writes a learned network, a `.oann`
file. A brain's `nets/*.oann` is this format. The integer runner builds
from the `Model` this module returns, and a tool that trains a network
writes the same bytes back. Target `oa-formats-oann`, header
`oa/formats/oann/oann.hpp`, namespace `oa::formats::oann`.

The file is little-endian. Format 1 is the only format. A stored integer
`q` stands for `q × 2^exponent`. No fractional arithmetic is used to run
a network. A damaged or hostile file is refused before anything runs it.

## Entry points

- `read` decodes one file from a byte span and then checks it. The span
  is the whole file. A refusal names the tensor or layer and the byte
  offset of the field that broke the rule, and does not read past the
  span. `problem` is set only when nothing is returned.
- `check` reports the first rule a model breaks, in the order
  [Rules](#rules) lists them, or nothing. For a model that was not read
  from bytes, the offset is where that field would sit in the written
  file.
- `write` checks, then writes the one byte form. `write` of a model
  `read` accepted gives those bytes back.
- `element_count` is the product of the first `rank` dimensions.
  `element_bytes` is 1, 2 or 4. `kind_name` and `parse_kind` are the
  thirteen names below, compared byte for byte. `describe` prints
  `byte <offset>: <message>`.

## State

None. Callers pass bytes and a `Model` and get values back. Nothing here
is installed for the rest of the process, and nothing opens a file.

## Invariants

- `format_version` is 1. A later format is refused.
- One model has exactly one byte form. Constants sit in tensor-table
  order, each at the first multiple of 4 at or after the end of the one
  before, padding bytes are 0, and nothing follows the data block.
- The file is at most 64 MiB (`max_file_bytes`, 67,108,864). A tensor
  has at most 16,777,216 elements (`max_tensor_elements`, 2^24). The
  dense, conv2d and embedding weights together have at most 8,388,608
  elements (`max_weights`, 2^23).
- Counts, lengths and offsets are checked against the bytes that remain
  before a constant's bytes are copied. A tensor's `data` is not
  allocated before that check.

## Byte layout

Header, 16 bytes (`header_bytes`); then the tensor table (64 bytes an
entry, `tensor_entry_bytes`), the layer table (32 bytes an entry,
`layer_entry_bytes`) and the data block. The file's length is exactly
`16 + 64 × tensor_count + 32 × layer_count + data_bytes`. Nothing follows.

| Offset | Bytes | Field | Rule |
| --- | --- | --- | --- |
| 0 | 4 | magic | `4F 41 4E 4E` (`OANN`) |
| 4 | 2 | format | 1 |
| 6 | 2 | flags | 0 |
| 8 | 2 | tensor_count | 2 to 4,096 |
| 10 | 2 | layer_count | 1 to 1,024 |
| 12 | 4 | data_bytes | the data block's length |

Tensor entry, 64 bytes:

| Offset | Bytes | Field | Rule |
| --- | --- | --- | --- |
| 0 | 32 | name | 1 to 31 bytes of `a`-`z`, `0`-`9` and `_`, starting with a letter, then NUL bytes to the end of the field; unique in the file |
| 32 | 1 | role | 1 input, 2 output, 3 activation, 4 constant |
| 33 | 1 | type | 1 int8, 2 int16, 3 int32 |
| 34 | 1 | exponent | signed: a stored integer `q` stands for `q × 2^exponent`; -48 to 16 |
| 35 | 1 | rank | 1 to 4 |
| 36 | 16 | dims | four u32; the first `rank` are 1 to 65,535, the rest 0 |
| 52 | 4 | offset | a constant's byte offset in the data block; 0 for every other role |
| 56 | 4 | bytes | a constant's length: elements × the type's size; 0 for every other role |
| 60 | 4 | reserved | 0 |

A tensor's elements are the product of its dims, at most 16,777,216
(2^24), stored row-major (the last dim varies fastest), each
little-endian. Inputs and activations are int16. Outputs are int16,
except that an argmax layer's output is int32. Constants (weights,
biases, embedding tables) are any type their layer allows. Constants'
data appear in the data block in tensor-table order, the first at
offset 0 and each later one at the first multiple of 4 at or after the
end of the one before. Padding bytes are 0, and `data_bytes` is the end
of the last constant rounded up to a multiple of 4.

Layer entry, 32 bytes. An unused tensor index is `0xFFFF` (`no_tensor`).

| Offset | Bytes | Field | Rule |
| --- | --- | --- | --- |
| 0 | 1 | kind | 1 dense, 2 conv2d, 3 maxpool, 4 relu, 5 leaky_relu, 6 tanh, 7 sigmoid, 8 add, 9 concat, 10 flatten, 11 softmax, 12 argmax, 13 embedding |
| 1 | 1 | input_count | as the kind needs (below) |
| 2 | 8 | inputs | four u16 tensor indexes; unused ones `0xFFFF` |
| 10 | 2 | output | a tensor index |
| 12 | 2 | weights | a tensor index, or `0xFFFF` |
| 14 | 2 | bias | a tensor index, or `0xFFFF` |
| 16 | 1 | kernel_h | conv2d and maxpool: 1 to 15; otherwise 0 |
| 17 | 1 | kernel_w | as kernel_h |
| 18 | 1 | stride_h | conv2d and maxpool: 1 to 15; otherwise 0 |
| 19 | 1 | stride_w | as stride_h |
| 20 | 1 | pad_h | conv2d: 0 to kernel_h - 1; otherwise 0 |
| 21 | 1 | pad_w | conv2d: 0 to kernel_w - 1; otherwise 0 |
| 22 | 1 | axis | concat: 0 to rank - 1; otherwise 0 |
| 23 | 1 | reserved | 0 |
| 24 | 2 | alpha | leaky_relu: the slope for negative inputs in 32768ths, 0 to 32767; otherwise 0 |
| 26 | 6 | reserved | 0 |

Reading also refuses, before the rules below, a header cut short, a
magic other than `OANN`, a format other than 1, flags other than 0, a
count outside its range (checked before the declared length, so a count
of 4,097 fails even when the body is short), a declared length past 64
MiB, a file shorter or longer than its parts, a name that fills its 32
bytes or holds a byte outside the set or a non-zero byte after its first
NUL, a non-zero reserved byte, a non-constant whose offset or length is
not 0, a constant that does not start at its canonical place, a constant
that runs past the data block, a padding byte that is not 0, and a data
block longer than the constants it holds.

## Rules

`check` reports the first of these that fails.

1. The tensor count is 2 to 4,096 (`max_tensors`).
2. The layer count is 1 to 1,024 (`max_layers`).
3. Each tensor, in table order: the name is 1 to 31 bytes of `a`-`z`,
   `0`-`9` and `_`, starts with a letter, and is unique; the role is
   input, output, activation or constant; the type is int8, int16 or
   int32; the exponent is -48 to 16; the rank is 1 to 4; the first
   `rank` dimensions are 1 to 65,535 and the rest are 0; the element
   count is at most 16,777,216; a constant's `data` is that many
   elements and every other role's `data` is empty; an input or an
   activation is int16.
4. The written file is at most 67,108,864 bytes. The length is the
   header, the two tables and the data block, with each constant padded
   as the byte layout says.
5. Each layer, in table order. The kind is one of the thirteen. The
   input count is what the kind needs, each used slot names a tensor,
   and each unused slot is `0xFFFF`. The output names a tensor. Weights
   and a bias are present only when the kind has them (a dense or
   conv2d bias may be absent). Kernel and stride are 1 to 15 for conv2d
   and maxpool and 0 otherwise. Pad is 0 to kernel − 1 for conv2d and 0
   otherwise. Axis is 0 except for concat, whose range is that kind's
   row. The slope is 0 to 32767 for leaky_relu and 0 otherwise. Then
   that kind's row below. An argmax output's role, type, shape,
   exponent and "read by no layer" are part of the argmax row, so a
   later layer that reads it is reported there.
6. The graph, in this order. A layer's inputs are role-input tensors,
   or tensors an earlier layer wrote. Every activation and every output
   is written by exactly one layer (a second writer, then a tensor
   nobody writes). Inputs and constants are never written. Every input
   and every activation is read by at least one layer. A constant is
   read only as a layer's weights or bias, and every constant is used.
   There are 1 to 16 inputs and 1 to 16 outputs (`max_ports`). The
   weights of all dense, conv2d and embedding layers together hold at
   most 8,388,608 elements, counted once per tensor; the layer that
   crosses the limit is the one reported.

Each kind's tensors. `in` is the input, `w` the weights, `b` the bias,
`out` the output, and `e` a tensor's exponent.

| Kind | Inputs | Weights, bias | Output | Scales |
| --- | --- | --- | --- | --- |
| dense | 1, any shape, `n` elements | w int8 or int16, rank 2 `[m, n]`; b int32 `[m]` or none | rank 1 `[m]` | `e(b) = e(in) + e(w)`; `s = e(out) - e(in) - e(w)` is 0 to 62 |
| conv2d | 1, rank 3 `[c, h, w]` | w int8 or int16 `[k, c, kernel_h, kernel_w]`; b int32 `[k]` or none | `[k, oh, ow]`, `oh = (h + 2·pad_h - kernel_h) / stride_h + 1` (whole-number division; `h + 2·pad_h >= kernel_h`), `ow` alike | as dense |
| maxpool | 1, `[c, h, w]` | none; pads 0 | `[c, oh, ow]`, `oh = (h - kernel_h) / stride_h + 1`, `h >= kernel_h`, `ow` alike | `e(out) = e(in)` |
| relu, leaky_relu | 1 | none | same rank and dims as in | `e(out) = e(in)` |
| tanh, sigmoid | 1 | none | same dims | `e(out) = -15` |
| add | 2, same dims | none | same dims | any |
| concat | 2 to 4, same rank, dims equal except `axis` | none | dims equal, `axis` the sum | any |
| flatten | 1 | none | rank 1, the same element count | `e(out) = e(in)` |
| softmax | 1 | none | same dims | `e(out) = -15` |
| argmax | 1 | none | int32, rank 1 `[1]`, role output, read by no layer | `e(out) = 0` |
| embedding | 1, `n` elements, `e(in) = 0` | w int8 or int16 `[v, d]`; no bias | rank 2 `[n, d]` | `e(out) = e(w)` |

An input tensor is int16. An output is int16, except an argmax output,
which is int32.

## Arithmetic

This is the meaning of format 1. The runner implements it. This module
does not.

- `R(x, from, to)` moves the integer `x` from exponent `from` to
  exponent `to`, in 64 bits. With `d = to - from`: when `d > 0`,
  `(x + 2^(d-1)) >> d`, an arithmetic shift (halves round up, towards
  plus infinity); when `d = 0`, `x`; when `d < 0`, `x × 2^(-d)`, held
  at the 64-bit limits when it would pass them. `sat16` and `sat32`
  hold a 64-bit value to the int16 and int32 range.
- dense: `out[j] = sat16(R(b[j] + Σ_i in[i]·w[j][i], e(in) + e(w), e(out)))`,
  the sum in 64 bits (no sum of allowed sizes can pass 2^55), `b[j]` 0
  without a bias.
- conv2d: the same sum over `c`, `ky`, `kx` of
  `in[c][y·stride_h + ky - pad_h][x·stride_w + kx - pad_w] · w[o][c][ky][kx]`,
  a position outside the input reading 0.
- maxpool: the largest value in the window. relu: `max(x, 0)`.
  leaky_relu: `x` when `x >= 0`, else `(x · alpha) >> 15`.
- tanh and sigmoid: the index
  `i = clamp(R(x, e(in), -8) + 2048, 0, 4095)` into a table of 4,096
  int16 values, `TANH[i]` = `tanh((i - 2048) / 256) × 32768` and
  `SIGMOID[i]` = `32768 / (1 + exp(-(i - 2048) / 256))`, each rounded
  half to even and held to 32767. The tables of tanh, sigmoid and
  softmax are N06's (`src/data/neural/*`). No floating point is used
  to run a network.
  Little-endian SHA-256: tanh `cb7f633a633e5280f6e1f1f6e8ce0b8de29df3c80895e21f8f43ec3638081b58`, sigmoid `c4a892ac4c7782b3990d864b2ef6880662da2b77ba7144cc82cfc060781810b7`, exp `887bdd7a27c48be6ffd340ddffbce39dbfa80104546267a23b4921f3c2328e67`.
- add: `sat16(R(a, e(a), e(out)) + R(b, e(b), e(out)))`. concat: each
  element `sat16(R(x, e(x), e(out)))`, the inputs placed in order along
  `axis`. flatten: the same values.
- softmax: `m` the largest input; for each element
  `d = R(m - x, e(in), -8)`; `q = EXP[d]` when `d <= 4095`, else 0,
  where `EXP[i]` is `2^30 × exp(-i / 256)` rounded half to even
  (uint32); `S` the sum of every `q` in 64 bits; the output
  `(q × 32767 + S / 2) / S`, whole-number division.
- argmax: the 0-based index of the largest element; the first of equal
  ones.
- embedding: row `r = in[i]` (exponent 0, so the stored number);
  `0 <= r < v`, or the run stops with an error naming the layer and
  the row; the output's row `i` is `w[r]`, each element widened to
  int16.

## Tests

`formats-oann` builds one network with one layer of each kind, writes
it, reads it back field by field, and writes it again byte for byte.
The SHA-256 of that file is pinned; it moves only when format 1's byte
form changes. Each rule above is broken once, from that network, and
the message's start is checked. Every prefix of the pinned file is
refused, and so are a count past its limit, a dimension product past
2^24, an offset or a data length past the end, an unaligned constant,
a padding byte, one byte after the data block, an upper-case name and
two equal names. A network whose weights hold exactly 8,388,608
elements is accepted, and one more element is refused. The test does
not open a file.

## Limitations

- One byte form only. Padding, order and bytes after the data block are
  refused, so two writings of one model cannot differ.
- No layer kinds beyond these thirteen.
- Format 1 has no metadata block.
- This module does not run a network.

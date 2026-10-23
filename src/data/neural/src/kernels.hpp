// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// The layer kernels this build runs. Each one reads and writes plain spans.
// Sums are int64, and a product is formed in int64 from widened operands.
// Nothing here allocates, throws, or uses a fractional type.

#include "oa/data/neural/arithmetic.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace oa::data::neural {

/// One input of a concat, already pointing at its elements.
struct ConcatInput {
    const int16_t* values{}; ///< the elements, row-major
    int8_t exponent{};       ///< the input's exponent
    uint32_t along{};        ///< the length of the concat axis
};

/// Runs a dense layer.
///
/// `out[j]` is the bias, or 0, plus the sum over i of `in[i] * w[j][i]`,
/// moved from the sum's exponent to the output's and held to int16. The sum
/// is int64. An allowed network cannot overflow it, so the order of the
/// additions does not change a bit. `Weight` is int8_t or int16_t.
///
/// @param input the n inputs
/// @param weights m by n, row-major
/// @param bias m biases, or an empty span when the layer has none
/// @param output the m outputs
/// @param input_exponent the input's exponent
/// @param weight_exponent the weights' exponent
/// @param output_exponent the output's exponent
template <typename Weight>
void dense(
    std::span<const int16_t> input,
    std::span<const Weight> weights,
    std::span<const int32_t> bias,
    std::span<int16_t> output,
    int32_t input_exponent,
    int32_t weight_exponent,
    int32_t output_exponent
) noexcept;

/// Writes the positive inputs and zero for the rest.
///
/// @param input the layer's input
/// @param output the same number of elements
void relu(std::span<const int16_t> input, std::span<int16_t> output) noexcept;

/// Keeps a non-negative input and scales a negative one.
///
/// A negative x becomes (x * alpha) divided by 32768, rounding toward minus
/// infinity. `alpha` is the slope in 32768ths, 0 to 32767.
///
/// @param input the layer's input
/// @param output the same number of elements
/// @param alpha the slope in 32768ths
void leaky_relu(std::span<const int16_t> input, std::span<int16_t> output, int32_t alpha) noexcept;

/// Adds two tensors into the output's scale.
///
/// Each element is the two rescaled values added, then held to int16.
///
/// @param left the first input
/// @param left_exponent the first input's exponent
/// @param right the second input
/// @param right_exponent the second input's exponent
/// @param output one element per input element
/// @param output_exponent the output's exponent
void add(
    std::span<const int16_t> left,
    int32_t left_exponent,
    std::span<const int16_t> right,
    int32_t right_exponent,
    std::span<int16_t> output,
    int32_t output_exponent
) noexcept;

/// Joins inputs along one axis, each element moved to the output's scale.
///
/// The inputs are placed in the order given. The last dimension varies fastest.
///
/// @param inputs the inputs, in placement order
/// @param rank the shared rank, 1 to 4
/// @param axis the axis that grows, 0 to rank - 1
/// @param output_dims the output shape; the first rank entries are used
/// @param output_exponent the output's exponent
/// @param output the joined elements
void concat(
    std::span<const ConcatInput> inputs,
    uint8_t rank,
    uint8_t axis,
    const std::array<uint32_t, 4>& output_dims,
    int8_t output_exponent,
    std::span<int16_t> output
) noexcept;

/// Copies elements into a rank-1 tensor of the same scale.
///
/// @param input the layer's input
/// @param output the same elements
void flatten(std::span<const int16_t> input, std::span<int16_t> output) noexcept;

/// Writes the 0-based index of the largest element.
///
/// The first of equal values wins. An empty input writes 0.
///
/// @param input the layer's input
/// @param[out] index the chosen element
void argmax(std::span<const int16_t> input, int32_t& index) noexcept;

/// Copies table rows named by the input into the output.
///
/// Each stored index is a row. A row below 0 or not less than `rows` stops
/// the call and is written to `bad_row`. Table elements are widened to int16.
/// `Weight` is int8_t or int16_t.
///
/// @param indexes one row number per output row
/// @param table `rows` by `width`, row-major
/// @param rows how many rows the table holds
/// @param width how many elements each row holds
/// @param output `indexes.size()` by `width`
/// @param[out] bad_row the row that was out of range, when this returns false
/// @return false when a row is out of range
template <typename Weight>
[[nodiscard]] bool embedding(
    std::span<const int16_t> indexes,
    std::span<const Weight> table,
    uint32_t rows,
    uint32_t width,
    std::span<int16_t> output,
    int32_t& bad_row
) noexcept;

/// Runs a convolution. A position outside the input reads 0.
///
/// The sum is the bias, or 0, plus each in-window sample times its weight,
/// in int64. The sum moves from the product's exponent to the output's and
/// is held to int16. `Weight` is int8_t or int16_t. There is no read past
/// the input: a coordinate outside it contributes nothing.
///
/// @param input rank 3, `[channels, height, width]`, row-major
/// @param input_dims the input shape; the first three entries are used
/// @param weights `[kernels, channels, kernel_h, kernel_w]`, row-major
/// @param bias one bias per kernel, or an empty span when the layer has none
/// @param output `[kernels, out_height, out_width]`
/// @param output_dims the output shape; the first three entries are used
/// @param kernel_h the window height, 1 to 15
/// @param kernel_w the window width, 1 to 15
/// @param stride_h the vertical step, 1 to 15
/// @param stride_w the horizontal step, 1 to 15
/// @param pad_h how many rows of zeros sit above the input
/// @param pad_w how many columns of zeros sit to the left of the input
/// @param input_exponent the input's exponent
/// @param weight_exponent the weights' exponent
/// @param output_exponent the output's exponent
template <typename Weight>
void conv2d(
    std::span<const int16_t> input,
    const std::array<uint32_t, 4>& input_dims,
    std::span<const Weight> weights,
    std::span<const int32_t> bias,
    std::span<int16_t> output,
    const std::array<uint32_t, 4>& output_dims,
    uint8_t kernel_h,
    uint8_t kernel_w,
    uint8_t stride_h,
    uint8_t stride_w,
    uint8_t pad_h,
    uint8_t pad_w,
    int32_t input_exponent,
    int32_t weight_exponent,
    int32_t output_exponent
) noexcept;

/// Writes the largest value in each window.
///
/// The window stays inside the input. Nothing is read from outside it.
///
/// @param input rank 3, `[channels, height, width]`
/// @param input_dims the input shape; the first three entries are used
/// @param output one element per window
/// @param output_dims the output shape; the first three entries are used
/// @param kernel_h the window height, 1 to 15
/// @param kernel_w the window width, 1 to 15
/// @param stride_h the vertical step, 1 to 15
/// @param stride_w the horizontal step, 1 to 15
void maxpool(
    std::span<const int16_t> input,
    const std::array<uint32_t, 4>& input_dims,
    std::span<int16_t> output,
    const std::array<uint32_t, 4>& output_dims,
    uint8_t kernel_h,
    uint8_t kernel_w,
    uint8_t stride_h,
    uint8_t stride_w
) noexcept;

/// Looks each input up in the tanh table.
///
/// The index is clamp(R(x, input_exponent, -8) + 2048, 0, 4095). The table
/// holds the rounded tanh of that argument, already on the output's scale.
///
/// @param input the layer's input
/// @param output one table value per input
/// @param input_exponent the input's exponent
void tanh(
    std::span<const int16_t> input, std::span<int16_t> output, int32_t input_exponent
) noexcept;

/// Looks each input up in the sigmoid table.
///
/// The index is the same one tanh uses. The table holds the rounded sigmoid
/// of that argument, already on the output's scale.
///
/// @param input the layer's input
/// @param output one table value per input
/// @param input_exponent the input's exponent
void sigmoid(
    std::span<const int16_t> input, std::span<int16_t> output, int32_t input_exponent
) noexcept;

/// Writes a softmax over the whole input.
///
/// The distance of each element from the largest is moved to exponent -8.
/// A distance above 4095 contributes 0; every other distance reads the
/// exponent table. The outputs share one 64-bit sum and a whole-number
/// division, `(q * 32767 + sum / 2) / sum`.
///
/// @param input the layer's input
/// @param output one share per input, on the output's scale
/// @param input_exponent the input's exponent
void softmax(
    std::span<const int16_t> input, std::span<int16_t> output, int32_t input_exponent
) noexcept;

} // namespace oa::data::neural

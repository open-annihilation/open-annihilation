// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The integer runner: rescale, one small network of each format-1 kind, a
// mixed network and a grid network pinned against neural_reference.py, and
// the refusals. The pinned digests are the same on every build and move
// only with the arithmetic.

#include "oa/data/neural/arithmetic.hpp"
#include "oa/data/neural/network.hpp"

#include "oa/base/sha256.hpp"
#include "oa/formats/oann/oann.hpp"
#include "oa/test/check.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

// The generated activation tables. The same text is compiled into the runner.
#include "../src/tables.inc"

using oa::data::neural::Network;
using oa::data::neural::NetworkSet;
using oa::data::neural::RunError;
using oa::data::neural::max_brain_network_bytes;
using oa::data::neural::max_operations_per_run;
using oa::data::neural::rescale;
using oa::data::neural::saturate_int16;
using oa::data::neural::saturate_int32;
using oa::formats::oann::ElementType;
using oa::formats::oann::Layer;
using oa::formats::oann::LayerKind;
using oa::formats::oann::Model;
using oa::formats::oann::Problem;
using oa::formats::oann::Role;
using oa::formats::oann::Tensor;
using oa::formats::oann::check;
using oa::formats::oann::no_tensor;
using oa::formats::oann::read;
using oa::formats::oann::write;

// The printout of neural_reference.py, from the BEGIN line through the END
// line. The digest is SHA-256 of the mixed cat output as little-endian int16
// values followed by the pick as a little-endian int32. It is the same on
// every build and moves only when the arithmetic changes.
// clang-format off
// N02-NEURAL-REFERENCE-BEGIN
constexpr int16_t k_dense_shift0[] = {2, 5};
constexpr int16_t k_dense_shift0_bias[] = {12, 3};
constexpr int16_t k_dense_shift9[] = {4, -6};
constexpr int16_t k_dense_shift9_bias[] = {4, -6};
constexpr int16_t k_relu[] = {0, 0, 5, 0};
constexpr int16_t k_leaky[] = {5, 0, -1, -2, -3277};
constexpr int16_t k_add[] = {4, 5, 5, -3};
constexpr int16_t k_add_round[] = {3, 0};
constexpr int16_t k_concat[] = {3, -3, 16, 2};
constexpr int16_t k_concat_axis[] = {1, 2, 4, 3, 4, -4};
constexpr int16_t k_flat[] = {1, -2, 3, 4, -5, 6};
constexpr int32_t k_argmax = 1;
constexpr int16_t k_embed_i8[] = {-5, 6, 1, -2, 3, 4};
constexpr int16_t k_embed_i16[] = {7, 8, 1000, -1000, 7, 8};
constexpr int16_t k_mixed_cat[] = {32767, -29006, 22384, -22982, -21485, 32767, 32767, -1957, -32768, 31460, 32767, -32768, -32768, -936, -1760, -32768};
constexpr int32_t k_mixed_pick = 0;
constexpr std::string_view pinned_mixed_digest = "dde635fa4de824150a1ca292e896a61063c8ffe599b797579fd576f6d7deacfb";
// N02-NEURAL-REFERENCE-END
// N06-NEURAL-REFERENCE-BEGIN
constexpr int16_t k_conv_stride1[] = {5};
constexpr int16_t k_conv_shift[] = {3};
constexpr int16_t k_conv_stride2[] = {14, 22, 46, 54};
constexpr int16_t k_conv_pad[] = {77, 67, 47, 37};
constexpr int16_t k_conv_bias[] = {10};
constexpr int16_t k_conv_stride2_pad[] = {16, 13, 17, 24};
constexpr int16_t k_pool2[] = {6, 8, 14, 16};
constexpr int16_t k_pool3[] = {9, 10, 13, 14};
constexpr int16_t k_tanh[] = {-32768, 32767, 0, 24956};
constexpr int16_t k_sigmoid[] = {11, 32757, 16384, 23955};
constexpr int16_t k_tanh_index[] = {128};
constexpr int16_t k_sigmoid_index[] = {16416};
constexpr int16_t k_softmax_equal[] = {8192, 8192, 8192, 8192};
constexpr int16_t k_softmax_peak[] = {32767, 0, 0};
constexpr int16_t k_softmax_far[] = {32766, 1, 0};
constexpr int16_t k_grid_scores[] = {5730, 5730, 5730, 15576, 8192, 8192, 8192, 8192, 8192, 8192, 8192, 8192, 8192, 8192, 8192, 8192, 5730, 5730, 5730, 15576, 5730, 5730, 5730, 15576, 5730, 5730, 5730, 15576, 8192, 8192, 8192, 8192, 5730, 5730, 5730, 15576, 8192, 8192, 8192, 8192, 5730, 5730, 5730, 15576, 11977, 11977, 4406, 4406, 9729, 9729, 3579, 9729, 4406, 11977, 4406, 11977, 5730, 5730, 5730, 15576, 5730, 5730, 5730, 15576};
constexpr int32_t k_grid_picks[] = {3, 0, 0, 0, 3, 3, 3, 0, 3, 0, 3, 0, 0, 1, 3, 3};
constexpr std::string_view pinned_grid_digest = "eaf045a125e072e763f3d79aead6be32a4115d83b92600fb248d5f89c8ea09e6";
constexpr std::string_view k_tanh_sha256 = "cb7f633a633e5280f6e1f1f6e8ce0b8de29df3c80895e21f8f43ec3638081b58";
constexpr std::string_view k_sigmoid_sha256 = "c4a892ac4c7782b3990d864b2ef6880662da2b77ba7144cc82cfc060781810b7";
constexpr std::string_view k_exp_sha256 = "887bdd7a27c48be6ffd340ddffbce39dbfa80104546267a23b4921f3c2328e67";
// N06-NEURAL-REFERENCE-END
// clang-format on

// Entries at 0, 1, 2047, 2048, 2049, 4094 and 4095, from tables_reference.py.
static_assert(tanh_table[0] == -32768);
static_assert(tanh_table[1] == -32768);
static_assert(tanh_table[2047] == -128);
static_assert(tanh_table[2048] == 0);
static_assert(tanh_table[2049] == 128);
static_assert(tanh_table[4094] == 32767);
static_assert(tanh_table[4095] == 32767);
static_assert(sigmoid_table[0] == 11);
static_assert(sigmoid_table[1] == 11);
static_assert(sigmoid_table[2047] == 16352);
static_assert(sigmoid_table[2048] == 16384);
static_assert(sigmoid_table[2049] == 16416);
static_assert(sigmoid_table[4094] == 32757);
static_assert(sigmoid_table[4095] == 32757);
static_assert(exp_table[0] == 1073741824U);
static_assert(exp_table[1] == 1069555701U);
static_assert(exp_table[2047] == 361610U);
static_assert(exp_table[2048] == 360200U);
static_assert(exp_table[2049] == 358796U);
static_assert(exp_table[4094] == 122U);
static_assert(exp_table[4095] == 121U);

constexpr int64_t k_int64_min = -9223372036854775807LL - 1;
constexpr int64_t k_int64_max = 9223372036854775807LL;
constexpr int32_t k_int32_min = -2147483647 - 1;
constexpr int32_t k_int32_max = 2147483647;

// The mixed network's sequence. State starts at 1. The multiplier and the
// increment are the ones neural_reference.py uses.
struct Sequence {
    uint32_t state = 1;

    uint32_t next_u32() {
        state = state * 1664525U + 1013904223U;
        return state;
    }

    int32_t next_i32() {
        const uint32_t word = next_u32();
        if (word < 0x80000000U)
            return static_cast<int32_t>(word);
        return static_cast<int32_t>(static_cast<int64_t>(word) - 4294967296LL);
    }

    int16_t next_i16() {
        const uint32_t word = (next_u32() >> 16U) & 0xFFFFU;
        if (word < 0x8000U)
            return static_cast<int16_t>(word);
        return static_cast<int16_t>(static_cast<int32_t>(word) - 65536);
    }

    int8_t next_i8() {
        const uint32_t word = (next_u32() >> 24U) & 0xFFU;
        if (word < 128U)
            return static_cast<int8_t>(word);
        return static_cast<int8_t>(static_cast<int16_t>(word) - 256);
    }
};

int16_t row_of(int16_t value, int16_t rows) {
    int32_t reduced = static_cast<int32_t>(value) % rows;
    if (reduced < 0)
        reduced += rows;
    return static_cast<int16_t>(reduced);
}

void push_i8(std::vector<uint8_t>& data, int value) {
    data.push_back(static_cast<uint8_t>(static_cast<int8_t>(value)));
}

void push_i16(std::vector<uint8_t>& data, int value) {
    const auto bits = static_cast<uint16_t>(static_cast<int16_t>(value));
    data.push_back(static_cast<uint8_t>(bits));
    data.push_back(static_cast<uint8_t>(bits >> 8U));
}

void push_i32(std::vector<uint8_t>& data, int32_t value) {
    const auto bits = static_cast<uint32_t>(value);
    data.push_back(static_cast<uint8_t>(bits));
    data.push_back(static_cast<uint8_t>(bits >> 8U));
    data.push_back(static_cast<uint8_t>(bits >> 16U));
    data.push_back(static_cast<uint8_t>(bits >> 24U));
}

Tensor valued(
    std::string name,
    Role role,
    int8_t exponent,
    std::initializer_list<uint32_t> dims,
    ElementType type = ElementType::int16
) {
    Tensor tensor;
    tensor.name = std::move(name);
    tensor.role = role;
    tensor.type = type;
    tensor.exponent = exponent;
    tensor.rank = static_cast<uint8_t>(dims.size());
    uint8_t axis = 0;
    for (const uint32_t dim : dims) {
        tensor.dims[axis] = dim;
        ++axis;
    }
    return tensor;
}

Tensor constant_i8(
    std::string name,
    int8_t exponent,
    std::initializer_list<uint32_t> dims,
    std::initializer_list<int> values
) {
    Tensor tensor = valued(std::move(name), Role::constant, exponent, dims, ElementType::int8);
    for (const int value : values)
        push_i8(tensor.data, value);
    return tensor;
}

Tensor constant_i16(
    std::string name,
    int8_t exponent,
    std::initializer_list<uint32_t> dims,
    std::initializer_list<int> values
) {
    Tensor tensor = valued(std::move(name), Role::constant, exponent, dims, ElementType::int16);
    for (const int value : values)
        push_i16(tensor.data, value);
    return tensor;
}

Tensor constant_i32(
    std::string name,
    int8_t exponent,
    std::initializer_list<uint32_t> dims,
    std::initializer_list<int32_t> values
) {
    Tensor tensor = valued(std::move(name), Role::constant, exponent, dims, ElementType::int32);
    for (const int32_t value : values)
        push_i32(tensor.data, value);
    return tensor;
}

Layer make_layer(
    LayerKind kind,
    std::initializer_list<uint16_t> inputs,
    uint16_t output,
    uint16_t weights = no_tensor,
    uint16_t bias = no_tensor
) {
    Layer layer;
    layer.kind = kind;
    layer.input_count = static_cast<uint8_t>(inputs.size());
    layer.inputs.fill(no_tensor);
    uint8_t slot = 0;
    for (const uint16_t input : inputs) {
        layer.inputs[slot] = input;
        ++slot;
    }
    layer.output = output;
    layer.weights = weights;
    layer.bias = bias;
    return layer;
}

Layer spatial(
    LayerKind kind,
    std::initializer_list<uint16_t> inputs,
    uint16_t output,
    uint8_t kernel_h,
    uint8_t kernel_w,
    uint8_t stride_h,
    uint8_t stride_w,
    uint8_t pad_h,
    uint8_t pad_w,
    uint16_t weights = no_tensor,
    uint16_t bias = no_tensor
) {
    Layer layer = make_layer(kind, inputs, output, weights, bias);
    layer.kernel_h = kernel_h;
    layer.kernel_w = kernel_w;
    layer.stride_h = stride_h;
    layer.stride_w = stride_w;
    layer.pad_h = pad_h;
    layer.pad_w = pad_w;
    return layer;
}

std::optional<Network> load_network(const Model& model) {
    Problem problem;
    const auto bytes = write(model, problem);
    if (!bytes) {
        std::fprintf(stderr, "write: %s\n", problem.message.c_str());
        OA_CHECK(false);
        return std::nullopt;
    }
    const auto back = read(std::span<const uint8_t>(bytes->data(), bytes->size()), problem);
    if (!back) {
        std::fprintf(stderr, "read: %s\n", problem.message.c_str());
        OA_CHECK(false);
        return std::nullopt;
    }
    auto network = Network::build(*back, problem);
    if (!network) {
        std::fprintf(stderr, "build: %s\n", problem.message.c_str());
        OA_CHECK(false);
        return std::nullopt;
    }
    OA_CHECK(Network::memory_bytes_of(model) == network->memory_bytes());
    return network;
}

void fill(Network& network, std::size_t port, std::initializer_list<int16_t> values) {
    const std::span<int16_t> samples = network.input(port);
    OA_CHECK(samples.size() == values.size());
    std::size_t element = 0;
    for (const int16_t value : values) {
        if (element < samples.size())
            samples[element] = value;
        ++element;
    }
}

std::string digest_of(std::span<const uint8_t> bytes) {
    const auto text = oa::base::sha256::to_hex(oa::base::sha256::digest_of(bytes));
    return std::string(text.begin(), text.end());
}

void expect_message(const Problem& problem, const char* want) {
    if (problem.message != want)
        std::fprintf(stderr, "message: %s\n", problem.message.c_str());
    OA_CHECK(problem.message == want);
}

void expect_i16(std::span<const int16_t> got, std::span<const int16_t> want, const char* label) {
    OA_CHECK(got.size() == want.size());
    if (got.size() != want.size())
        return;
    for (std::size_t element = 0; element < got.size(); ++element) {
        if (got[element] != want[element]) {
            std::fprintf(
                stderr,
                "%s[%zu] got %d want %d\n",
                label,
                element,
                static_cast<int>(got[element]),
                static_cast<int>(want[element])
            );
        }
        OA_CHECK(got[element] == want[element]);
    }
}

bool ran(Network& network) {
    const RunError error = network.run();
    if (error.failed) {
        std::fprintf(
            stderr,
            "run failed at layer %u row %d\n",
            static_cast<unsigned>(error.layer),
            static_cast<int>(error.row)
        );
        OA_CHECK(false);
        return false;
    }
    return true;
}

void test_rescale() {
    // Halves round toward plus infinity. R(3, 0, 1) = (3 + 1) >> 1 = 2.
    // R(-3, 0, 1) = (-3 + 1) >> 1 = -1.
    OA_CHECK(rescale(3, 0, 1) == 2);
    OA_CHECK(rescale(-3, 0, 1) == -1);
    OA_CHECK(rescale(5, 3, 3) == 5);
    OA_CHECK(rescale(k_int64_min, -4, -4) == k_int64_min);
    // A left shift of 2: 6 * 4 = 24, and -3 * 4 = -12.
    OA_CHECK(rescale(6, 3, 1) == 24);
    OA_CHECK(rescale(-3, 2, 0) == -12);
    // Held at the 64-bit limits. 1 << 63 does not fit; -1 << 63 is the minimum.
    OA_CHECK(rescale(1, 0, -63) == k_int64_max);
    OA_CHECK(rescale(-1, 0, -63) == k_int64_min);
    OA_CHECK(rescale(2, 0, -62) == k_int64_max);
    OA_CHECK(rescale(k_int64_max, 0, -1) == k_int64_max);
    OA_CHECK(rescale(int64_t{1} << 62, 0, -1) == k_int64_max);
    OA_CHECK(rescale(k_int64_min, 0, -1) == k_int64_min);
    // (5 + 2) >> 2 = 1. (-5 + 2) >> 2 = -1. A shift of 64 or more is 0.
    OA_CHECK(rescale(5, 0, 2) == 1);
    OA_CHECK(rescale(-5, 0, 2) == -1);
    OA_CHECK(rescale(5, -48, 16) == 0);
    OA_CHECK(rescale(-1, -48, 16) == 0);

    OA_CHECK(saturate_int16(0) == 0);
    OA_CHECK(saturate_int16(32767) == 32767);
    OA_CHECK(saturate_int16(32768) == 32767);
    OA_CHECK(saturate_int16(-32768) == -32768);
    OA_CHECK(saturate_int16(-32769) == -32768);
    OA_CHECK(saturate_int16(k_int64_max) == 32767);
    OA_CHECK(saturate_int16(k_int64_min) == -32768);
    OA_CHECK(saturate_int32(0) == 0);
    OA_CHECK(saturate_int32(k_int32_max) == k_int32_max);
    OA_CHECK(saturate_int32(static_cast<int64_t>(k_int32_max) + 1) == k_int32_max);
    OA_CHECK(saturate_int32(k_int32_min) == k_int32_min);
    OA_CHECK(saturate_int32(static_cast<int64_t>(k_int32_min) - 1) == k_int32_min);
    OA_CHECK(saturate_int32(k_int64_max) == k_int32_max);
    OA_CHECK(saturate_int32(k_int64_min) == k_int32_min);
}

void test_dense() {
    // Shift 0, no bias. in [2, 3], weights [[1, 0], [4, -1]].
    // 1*2 + 0*3 = 2. 4*2 + -1*3 = 5.
    {
        Model model;
        model.tensors.push_back(valued("in", Role::input, 0, {2}));
        model.tensors.push_back(constant_i8("w", 0, {2, 2}, {1, 0, 4, -1}));
        model.tensors.push_back(valued("out", Role::output, 0, {2}));
        model.layers.push_back(make_layer(LayerKind::dense, {0}, 2, 1));
        auto network = load_network(model);
        if (!network)
            return;
        fill(*network, 0, {2, 3});
        if (!ran(*network))
            return;
        expect_i16(network->output(0), k_dense_shift0, "dense0");
    }
    // The same products plus bias [10, -2]: 12 and 3.
    {
        Model model;
        model.tensors.push_back(valued("in", Role::input, 0, {2}));
        model.tensors.push_back(constant_i8("w", 0, {2, 2}, {1, 0, 4, -1}));
        model.tensors.push_back(constant_i32("b", 0, {2}, {10, -2}));
        model.tensors.push_back(valued("out", Role::output, 0, {2}));
        model.layers.push_back(make_layer(LayerKind::dense, {0}, 3, 1, 2));
        auto network = load_network(model);
        if (!network)
            return;
        fill(*network, 0, {2, 3});
        if (!ran(*network))
            return;
        expect_i16(network->output(0), k_dense_shift0_bias, "dense0b");
    }
    // Shift 9, no bias. Sums are 7*400 + -3*200 = 2200 and
    // 7*-400 + -3*100 = -3100. (2200 + 256) >> 9 = 4.
    // (-3100 + 256) >> 9 = -6.
    {
        Model model;
        model.tensors.push_back(valued("in", Role::input, 0, {2}));
        model.tensors.push_back(constant_i16("w", 0, {2, 2}, {400, 200, -400, 100}));
        model.tensors.push_back(valued("out", Role::output, 9, {2}));
        model.layers.push_back(make_layer(LayerKind::dense, {0}, 2, 1));
        auto network = load_network(model);
        if (!network)
            return;
        fill(*network, 0, {7, -3});
        if (!ran(*network))
            return;
        expect_i16(network->output(0), k_dense_shift9, "dense9");
    }
    // Bias [100, -50] is added before the shift: 2300 and -3150, still 4 and -6.
    {
        Model model;
        model.tensors.push_back(valued("in", Role::input, 0, {2}));
        model.tensors.push_back(constant_i16("w", 0, {2, 2}, {400, 200, -400, 100}));
        model.tensors.push_back(constant_i32("b", 0, {2}, {100, -50}));
        model.tensors.push_back(valued("out", Role::output, 9, {2}));
        model.layers.push_back(make_layer(LayerKind::dense, {0}, 3, 1, 2));
        auto network = load_network(model);
        if (!network)
            return;
        fill(*network, 0, {7, -3});
        if (!ran(*network))
            return;
        expect_i16(network->output(0), k_dense_shift9_bias, "dense9b");
    }
}

void test_relu_and_leaky() {
    Model relu_model;
    relu_model.tensors.push_back(valued("in", Role::input, 0, {4}));
    relu_model.tensors.push_back(valued("out", Role::output, 0, {4}));
    relu_model.layers.push_back(make_layer(LayerKind::relu, {0}, 1));
    auto relu_network = load_network(relu_model);
    if (!relu_network)
        return;
    fill(*relu_network, 0, {-2, 0, 5, -32768});
    if (!ran(*relu_network))
        return;
    expect_i16(relu_network->output(0), k_relu, "relu");

    // alpha 3277, in 32768ths. -1*3277 >> 15 = -1.
    // -10*3277 = -32770, and -32770 >> 15 = -2.
    // -32768*3277 >> 15 = -3277.
    Model leaky_model;
    leaky_model.tensors.push_back(valued("in", Role::input, 0, {5}));
    leaky_model.tensors.push_back(valued("out", Role::output, 0, {5}));
    Layer leaky = make_layer(LayerKind::leaky_relu, {0}, 1);
    leaky.alpha = 3277;
    leaky_model.layers.push_back(leaky);
    auto leaky_network = load_network(leaky_model);
    if (!leaky_network)
        return;
    fill(*leaky_network, 0, {5, 0, -1, -10, -32768});
    if (!ran(*leaky_network))
        return;
    expect_i16(leaky_network->output(0), k_leaky, "leaky");
}

void test_add_and_concat() {
    // Exponents 0 and 1, output exponent 0. The second input is doubled:
    // 2+2, -3+8, 3+2, -3+0.
    {
        Model model;
        model.tensors.push_back(valued("a", Role::input, 0, {4}));
        model.tensors.push_back(valued("b", Role::input, 1, {4}));
        model.tensors.push_back(valued("out", Role::output, 0, {4}));
        model.layers.push_back(make_layer(LayerKind::add, {0, 1}, 2));
        auto network = load_network(model);
        if (!network)
            return;
        fill(*network, 0, {2, -3, 3, -3});
        fill(*network, 1, {1, 4, 1, 0});
        if (!ran(*network))
            return;
        expect_i16(network->output(0), k_add, "add");
    }
    // Output exponent 1. R(3, 0, 1) + R(1, 0, 1) = 2 + 1 = 3.
    // R(-3, 0, 1) + R(1, 0, 1) = -1 + 1 = 0.
    {
        Model model;
        model.tensors.push_back(valued("a", Role::input, 0, {2}));
        model.tensors.push_back(valued("b", Role::input, 0, {2}));
        model.tensors.push_back(valued("out", Role::output, 1, {2}));
        model.layers.push_back(make_layer(LayerKind::add, {0, 1}, 2));
        auto network = load_network(model);
        if (!network)
            return;
        fill(*network, 0, {3, -3});
        fill(*network, 1, {1, 1});
        if (!ran(*network))
            return;
        expect_i16(network->output(0), k_add_round, "add-round");
    }
    // Rank 1. The second input's exponent is 1, so 8 and 1 become 16 and 2.
    {
        Model model;
        model.tensors.push_back(valued("a", Role::input, 0, {2}));
        model.tensors.push_back(valued("b", Role::input, 1, {2}));
        model.tensors.push_back(valued("out", Role::output, 0, {4}));
        model.layers.push_back(make_layer(LayerKind::concat, {0, 1}, 2));
        auto network = load_network(model);
        if (!network)
            return;
        fill(*network, 0, {3, -3});
        fill(*network, 1, {8, 1});
        if (!ran(*network))
            return;
        expect_i16(network->output(0), k_concat, "concat");
    }
    // Rank 2, axis 1. The second input's exponent is 2, so 1 and -1 become
    // 4 and -4. The rows join as [1, 2, 4] and [3, 4, -4].
    {
        Model model;
        model.tensors.push_back(valued("a", Role::input, 0, {2, 2}));
        model.tensors.push_back(valued("b", Role::input, 2, {2, 1}));
        model.tensors.push_back(valued("out", Role::output, 0, {2, 3}));
        Layer layer = make_layer(LayerKind::concat, {0, 1}, 2);
        layer.axis = 1;
        model.layers.push_back(layer);
        auto network = load_network(model);
        if (!network)
            return;
        fill(*network, 0, {1, 2, 3, 4});
        fill(*network, 1, {1, -1});
        if (!ran(*network))
            return;
        expect_i16(network->output(0), k_concat_axis, "concat-axis");
    }
}

void test_flatten_and_argmax() {
    // Flatten keeps every value, row-major, last dimension fastest.
    Model flat_model;
    flat_model.tensors.push_back(valued("in", Role::input, 0, {2, 3}));
    flat_model.tensors.push_back(valued("out", Role::output, 0, {6}));
    flat_model.layers.push_back(make_layer(LayerKind::flatten, {0}, 1));
    auto flat_network = load_network(flat_model);
    if (!flat_network)
        return;
    fill(*flat_network, 0, {1, -2, 3, 4, -5, 6});
    if (!ran(*flat_network))
        return;
    expect_i16(flat_network->output(0), k_flat, "flat");

    // The largest value is 4. The first of the two is element 1.
    Model model;
    model.tensors.push_back(valued("in", Role::input, 0, {4}));
    model.tensors.push_back(valued("pick", Role::output, 0, {1}, ElementType::int32));
    model.layers.push_back(make_layer(LayerKind::argmax, {0}, 1));
    auto network = load_network(model);
    if (!network)
        return;
    fill(*network, 0, {1, 4, 4, -2});
    if (!ran(*network))
        return;
    OA_CHECK(network->output(0).empty());
    OA_CHECK(network->output_index(0) == k_argmax);
    OA_CHECK(network->output_index(1) == 0);
}

void test_embedding() {
    {
        Model model;
        model.tensors.push_back(valued("ids", Role::input, 0, {3}));
        model.tensors.push_back(constant_i8("w", 0, {3, 2}, {1, -2, 3, 4, -5, 6}));
        model.tensors.push_back(valued("out", Role::output, 0, {3, 2}));
        model.layers.push_back(make_layer(LayerKind::embedding, {0}, 2, 1));
        auto network = load_network(model);
        if (!network)
            return;
        fill(*network, 0, {2, 0, 1});
        if (!ran(*network))
            return;
        expect_i16(network->output(0), k_embed_i8, "embed8");
    }
    {
        Model model;
        model.tensors.push_back(valued("ids", Role::input, 0, {3}));
        model.tensors.push_back(constant_i16("w", 0, {2, 2}, {1000, -1000, 7, 8}));
        model.tensors.push_back(valued("out", Role::output, 0, {3, 2}));
        model.layers.push_back(make_layer(LayerKind::embedding, {0}, 2, 1));
        auto network = load_network(model);
        if (!network)
            return;
        fill(*network, 0, {1, 0, 1});
        if (!ran(*network))
            return;
        expect_i16(network->output(0), k_embed_i16, "embed16");
    }
}

void test_embedding_row() {
    // Flatten is layer 0, so the embedding that stops is layer 1.
    // The table has 4 rows. Row -1 and row 4 are both out of range.
    Model model;
    model.tensors.push_back(valued("ids", Role::input, 0, {1}));
    model.tensors.push_back(valued("flat", Role::activation, 0, {1}));
    model.tensors.push_back(constant_i8("w", 0, {4, 1}, {1, 2, 3, 4}));
    model.tensors.push_back(valued("out", Role::output, 0, {1, 1}));
    model.layers.push_back(make_layer(LayerKind::flatten, {0}, 1));
    model.layers.push_back(make_layer(LayerKind::embedding, {1}, 3, 2));
    auto network = load_network(model);
    if (!network)
        return;
    const std::span<int16_t> samples = network->input(0);
    OA_CHECK(samples.size() == 1);
    if (samples.size() != 1)
        return;

    samples[0] = -1;
    RunError error = network->run();
    OA_CHECK(error.failed);
    OA_CHECK(error.layer == 1);
    OA_CHECK(error.row == -1);

    samples[0] = 4;
    error = network->run();
    OA_CHECK(error.failed);
    OA_CHECK(error.layer == 1);
    OA_CHECK(error.row == 4);

    samples[0] = 3;
    error = network->run();
    OA_CHECK(!error.failed);
    OA_CHECK(network->output(0).size() == 1);
    if (network->output(0).size() == 1)
        OA_CHECK(network->output(0)[0] == 4);
}

Model counted_model() {
    // input [4] int16 is 8 bytes, int8 weights [3, 4] are 12, int32 bias [3]
    // is 12, the activation [3] int16 is 6 and the output [3] int16 is 6.
    // 8 + 12 + 12 + 6 + 6 = 44.
    // dense is 3 * 4 = 12 operations, relu is 3, together 15.
    // ceil(15 / 64) = 1 charge unit.
    Model model;
    model.tensors.push_back(valued("in", Role::input, 0, {4}));
    model.tensors.push_back(constant_i8("w", 0, {3, 4}, {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0}));
    model.tensors.push_back(constant_i32("b", 0, {3}, {0, 0, 0}));
    model.tensors.push_back(valued("h", Role::activation, 0, {3}));
    model.tensors.push_back(valued("out", Role::output, 0, {3}));
    model.layers.push_back(make_layer(LayerKind::dense, {0}, 3, 1, 2));
    model.layers.push_back(make_layer(LayerKind::relu, {3}, 4));
    return model;
}

void test_counts() {
    const Model model = counted_model();
    OA_CHECK(Network::memory_bytes_of(model) == 44);
    auto network = load_network(model);
    if (!network)
        return;
    OA_CHECK(network->memory_bytes() == 44);
    OA_CHECK(network->operations() == 15);
    OA_CHECK(network->charge_units() == 1);
}

void test_mixed() {
    // embedding 32, flatten 32, add 32, dense 8*32 = 256, relu 8,
    // leaky_relu 8, add 8, concat 16, argmax 16. Total 408.
    // ceil(408 / 64) = 7.
    // Bytes: tok 32, emb_w 32, emb 64, flat 64, feat 64, mixed 64,
    // dense_w 512, dense_b 32, hid 16, rel 16, lky 16, side 16, body 16,
    // tail 16, cat 32, pick 4. Total 996.
    Sequence stream;
    Model model;
    model.tensors.push_back(valued("tok", Role::input, 0, {16}));
    Tensor table = valued("embw", Role::constant, 0, {16, 2}, ElementType::int8);
    for (int element = 0; element < 32; ++element)
        push_i8(table.data, stream.next_i8());
    model.tensors.push_back(std::move(table));
    model.tensors.push_back(valued("emb", Role::activation, 0, {16, 2}));
    model.tensors.push_back(valued("flat", Role::activation, 0, {32}));
    model.tensors.push_back(valued("feat", Role::input, 1, {32}));
    model.tensors.push_back(valued("mixed", Role::activation, 0, {32}));
    Tensor weights = valued("densew", Role::constant, 0, {8, 32}, ElementType::int16);
    for (int element = 0; element < 256; ++element)
        push_i16(weights.data, stream.next_i16());
    model.tensors.push_back(std::move(weights));
    Tensor bias = valued("denseb", Role::constant, 0, {8}, ElementType::int32);
    for (int element = 0; element < 8; ++element)
        push_i32(bias.data, stream.next_i32());
    model.tensors.push_back(std::move(bias));
    model.tensors.push_back(valued("hid", Role::activation, 0, {8}));
    model.tensors.push_back(valued("rel", Role::activation, 0, {8}));
    model.tensors.push_back(valued("lky", Role::activation, 0, {8}));
    model.tensors.push_back(valued("side", Role::input, 1, {8}));
    model.tensors.push_back(valued("body", Role::activation, 0, {8}));
    model.tensors.push_back(valued("tail", Role::input, 2, {8}));
    model.tensors.push_back(valued("cat", Role::output, 0, {16}));
    model.tensors.push_back(valued("pick", Role::output, 0, {1}, ElementType::int32));

    model.layers.push_back(make_layer(LayerKind::embedding, {0}, 2, 1));
    model.layers.push_back(make_layer(LayerKind::flatten, {2}, 3));
    model.layers.push_back(make_layer(LayerKind::add, {3, 4}, 5));
    model.layers.push_back(make_layer(LayerKind::dense, {5}, 8, 6, 7));
    model.layers.push_back(make_layer(LayerKind::relu, {8}, 9));
    Layer leaky = make_layer(LayerKind::leaky_relu, {9}, 10);
    leaky.alpha = 3277;
    model.layers.push_back(leaky);
    model.layers.push_back(make_layer(LayerKind::add, {10, 11}, 12));
    model.layers.push_back(make_layer(LayerKind::concat, {12, 13}, 14));
    model.layers.push_back(make_layer(LayerKind::argmax, {14}, 15));

    OA_CHECK(Network::memory_bytes_of(model) == 996);
    auto network = load_network(model);
    if (!network)
        return;
    OA_CHECK(network->operations() == 408);
    OA_CHECK(network->charge_units() == 7);
    OA_CHECK(network->memory_bytes() == 996);
    OA_CHECK(network->inputs().size() == 4);
    OA_CHECK(network->inputs()[0].name == "tok");
    OA_CHECK(network->inputs()[0].elements == 16);
    OA_CHECK(network->inputs()[1].exponent == 1);
    OA_CHECK(network->outputs().size() == 2);
    OA_CHECK(!network->outputs()[0].index);
    OA_CHECK(network->outputs()[1].index);
    OA_CHECK(network->outputs()[1].name == "pick");

    std::array<int16_t, 64> raw{};
    for (int16_t& sample : raw)
        sample = stream.next_i16();
    const std::span<int16_t> tokens = network->input(0);
    const std::span<int16_t> feat = network->input(1);
    const std::span<int16_t> side = network->input(2);
    const std::span<int16_t> tail = network->input(3);
    OA_CHECK(tokens.size() == 16);
    OA_CHECK(feat.size() == 32);
    OA_CHECK(side.size() == 8);
    OA_CHECK(tail.size() == 8);
    if (tokens.size() != 16 || feat.size() != 32 || side.size() != 8 || tail.size() != 8)
        return;
    for (std::size_t element = 0; element < 16; ++element)
        tokens[element] = row_of(raw[element], 16);
    for (std::size_t element = 0; element < 32; ++element)
        feat[element] = raw[16 + element];
    for (std::size_t element = 0; element < 8; ++element) {
        side[element] = raw[48 + element];
        tail[element] = raw[56 + element];
    }
    if (!ran(*network))
        return;
    expect_i16(network->output(0), k_mixed_cat, "mixed");
    OA_CHECK(network->output(1).empty());
    OA_CHECK(network->output_index(0) == 0);
    OA_CHECK(network->output_index(1) == k_mixed_pick);

    std::vector<uint8_t> bytes;
    for (const int16_t value : network->output(0)) {
        const auto bits = static_cast<uint16_t>(value);
        bytes.push_back(static_cast<uint8_t>(bits));
        bytes.push_back(static_cast<uint8_t>(bits >> 8U));
    }
    const auto pick_bits = static_cast<uint32_t>(network->output_index(1));
    bytes.push_back(static_cast<uint8_t>(pick_bits));
    bytes.push_back(static_cast<uint8_t>(pick_bits >> 8U));
    bytes.push_back(static_cast<uint8_t>(pick_bits >> 16U));
    bytes.push_back(static_cast<uint8_t>(pick_bits >> 24U));
    const auto text = oa::base::sha256::to_hex(
        oa::base::sha256::digest_of(std::span<const uint8_t>(bytes.data(), bytes.size()))
    );
    const std::string hex(text.begin(), text.end());
    if (hex != pinned_mixed_digest)
        std::fprintf(stderr, "sha256 %s\n", hex.c_str());
    OA_CHECK(hex == pinned_mixed_digest);
}

void append_i16_bytes(std::vector<uint8_t>& bytes, int16_t value) {
    const auto bits = static_cast<uint16_t>(value);
    bytes.push_back(static_cast<uint8_t>(bits));
    bytes.push_back(static_cast<uint8_t>(bits >> 8U));
}

void append_i32_bytes(std::vector<uint8_t>& bytes, int32_t value) {
    const auto bits = static_cast<uint32_t>(value);
    bytes.push_back(static_cast<uint8_t>(bits));
    bytes.push_back(static_cast<uint8_t>(bits >> 8U));
    bytes.push_back(static_cast<uint8_t>(bits >> 16U));
    bytes.push_back(static_cast<uint8_t>(bits >> 24U));
}

void test_tables() {
    // Little-endian bytes of the three generated tables.
    std::vector<uint8_t> tanh_bytes;
    std::vector<uint8_t> sigmoid_bytes;
    std::vector<uint8_t> exp_bytes;
    tanh_bytes.reserve(tanh_table.size() * 2);
    sigmoid_bytes.reserve(sigmoid_table.size() * 2);
    exp_bytes.reserve(exp_table.size() * 4);
    for (const int16_t value : tanh_table)
        append_i16_bytes(tanh_bytes, value);
    for (const int16_t value : sigmoid_table)
        append_i16_bytes(sigmoid_bytes, value);
    for (const uint32_t value : exp_table)
        append_i32_bytes(exp_bytes, static_cast<int32_t>(value));
    const std::string tanh_hex = digest_of(tanh_bytes);
    const std::string sigmoid_hex = digest_of(sigmoid_bytes);
    const std::string exp_hex = digest_of(exp_bytes);
    if (tanh_hex != k_tanh_sha256)
        std::fprintf(stderr, "tanh sha256 %s\n", tanh_hex.c_str());
    if (sigmoid_hex != k_sigmoid_sha256)
        std::fprintf(stderr, "sigmoid sha256 %s\n", sigmoid_hex.c_str());
    if (exp_hex != k_exp_sha256)
        std::fprintf(stderr, "exp sha256 %s\n", exp_hex.c_str());
    OA_CHECK(tanh_hex == k_tanh_sha256);
    OA_CHECK(sigmoid_hex == k_sigmoid_sha256);
    OA_CHECK(exp_hex == k_exp_sha256);
}

void test_conv() {
    // Stride 1, no padding, no bias, int8. 1*1 + 4*1 = 5.
    {
        Model model;
        model.tensors.push_back(valued("in", Role::input, 0, {1, 2, 2}));
        model.tensors.push_back(constant_i8("w", 0, {1, 1, 2, 2}, {1, 0, 0, 1}));
        model.tensors.push_back(valued("out", Role::output, 0, {1, 1, 1}));
        model.layers.push_back(spatial(LayerKind::conv2d, {0}, 2, 2, 2, 1, 1, 0, 0, 1));
        auto network = load_network(model);
        if (!network)
            return;
        fill(*network, 0, {1, 2, 3, 4});
        if (!ran(*network))
            return;
        expect_i16(network->output(0), k_conv_stride1, "conv-s1");
    }
    // The same sum at output exponent 1: (5 + 1) >> 1 = 3.
    {
        Model model;
        model.tensors.push_back(valued("in", Role::input, 0, {1, 2, 2}));
        model.tensors.push_back(constant_i8("w", 0, {1, 1, 2, 2}, {1, 0, 0, 1}));
        model.tensors.push_back(valued("out", Role::output, 1, {1, 1, 1}));
        model.layers.push_back(spatial(LayerKind::conv2d, {0}, 2, 2, 2, 1, 1, 0, 0, 1));
        auto network = load_network(model);
        if (!network)
            return;
        fill(*network, 0, {1, 2, 3, 4});
        if (!ran(*network))
            return;
        expect_i16(network->output(0), k_conv_shift, "conv-shift");
    }
    // Stride 2, no padding, no bias, int8. Each window is four ones:
    // 14, 22, 46 and 54.
    {
        Model model;
        model.tensors.push_back(valued("in", Role::input, 0, {1, 4, 4}));
        model.tensors.push_back(constant_i8("w", 0, {1, 1, 2, 2}, {1, 1, 1, 1}));
        model.tensors.push_back(valued("out", Role::output, 0, {1, 2, 2}));
        model.layers.push_back(spatial(LayerKind::conv2d, {0}, 2, 2, 2, 2, 2, 0, 0, 1));
        auto network = load_network(model);
        if (!network)
            return;
        fill(*network, 0, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
        if (!ran(*network))
            return;
        expect_i16(network->output(0), k_conv_stride2, "conv-s2");
    }
    // Padding 1, stride 1, no bias, int8. The kernel is 1..9. A corner
    // reads 0 outside the 2x2. (0,0) = 77, (0,1) = 67, (1,0) = 47, (1,1) = 37.
    {
        Model model;
        model.tensors.push_back(valued("in", Role::input, 0, {1, 2, 2}));
        model.tensors.push_back(constant_i8("w", 0, {1, 1, 3, 3}, {1, 2, 3, 4, 5, 6, 7, 8, 9}));
        model.tensors.push_back(valued("out", Role::output, 0, {1, 2, 2}));
        model.layers.push_back(spatial(LayerKind::conv2d, {0}, 2, 3, 3, 1, 1, 1, 1, 1));
        auto network = load_network(model);
        if (!network)
            return;
        fill(*network, 0, {1, 2, 3, 4});
        if (!ran(*network))
            return;
        expect_i16(network->output(0), k_conv_pad, "conv-pad");
    }
    // Stride 1, no padding, int16 weights, bias -4. 1*2 + 4*3 - 4 = 10.
    {
        Model model;
        model.tensors.push_back(valued("in", Role::input, 0, {1, 2, 2}));
        model.tensors.push_back(constant_i16("w", 0, {1, 1, 2, 2}, {2, 0, 0, 3}));
        model.tensors.push_back(constant_i32("b", 0, {1}, {-4}));
        model.tensors.push_back(valued("out", Role::output, 0, {1, 1, 1}));
        model.layers.push_back(spatial(LayerKind::conv2d, {0}, 3, 2, 2, 1, 1, 0, 0, 1, 2));
        auto network = load_network(model);
        if (!network)
            return;
        fill(*network, 0, {1, 2, 3, 4});
        if (!ran(*network))
            return;
        expect_i16(network->output(0), k_conv_bias, "conv-bias");
    }
    // Stride 2, padding 1, int16 weights, bias 10. The diagonal kernel
    // reads 0 past the 3x3. The windows are 6, 3, 7 and 14, plus 10.
    {
        Model model;
        model.tensors.push_back(valued("in", Role::input, 0, {1, 3, 3}));
        model.tensors.push_back(constant_i16("w", 0, {1, 1, 3, 3}, {1, 0, 0, 0, 1, 0, 0, 0, 1}));
        model.tensors.push_back(constant_i32("b", 0, {1}, {10}));
        model.tensors.push_back(valued("out", Role::output, 0, {1, 2, 2}));
        model.layers.push_back(spatial(LayerKind::conv2d, {0}, 3, 3, 3, 2, 2, 1, 1, 1, 2));
        auto network = load_network(model);
        if (!network)
            return;
        fill(*network, 0, {1, 2, 3, 4, 5, 6, 7, 8, 9});
        if (!ran(*network))
            return;
        expect_i16(network->output(0), k_conv_stride2_pad, "conv-s2-pad");
    }
}

void test_maxpool() {
    // 2x2 stride 2. The windows are 6, 8, 14 and 16.
    {
        Model model;
        model.tensors.push_back(valued("in", Role::input, 0, {1, 4, 4}));
        model.tensors.push_back(valued("out", Role::output, 0, {1, 2, 2}));
        model.layers.push_back(spatial(LayerKind::maxpool, {0}, 1, 2, 2, 2, 2, 0, 0));
        auto network = load_network(model);
        if (!network)
            return;
        fill(*network, 0, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
        if (!ran(*network))
            return;
        expect_i16(network->output(0), k_pool2, "pool2");
    }
    // 3x3 stride 1. The -1 sits in the first window and is not the largest.
    // The windows are 9, 10, 13 and 14.
    {
        Model model;
        model.tensors.push_back(valued("in", Role::input, 0, {1, 4, 4}));
        model.tensors.push_back(valued("out", Role::output, 0, {1, 2, 2}));
        model.layers.push_back(spatial(LayerKind::maxpool, {0}, 1, 3, 3, 1, 1, 0, 0));
        auto network = load_network(model);
        if (!network)
            return;
        fill(*network, 0, {1, 2, 3, 4, 5, 6, 7, 8, 9, 0, -1, 10, 11, 12, 13, 14});
        if (!ran(*network))
            return;
        expect_i16(network->output(0), k_pool3, "pool3");
    }
}

void test_tanh_and_sigmoid() {
    // Exponent 0. -8 and 8 clamp to the ends of the table. 0 is the centre.
    // 1 lands at index 2304, between the centre and the high end.
    {
        Model model;
        model.tensors.push_back(valued("in", Role::input, 0, {4}));
        model.tensors.push_back(valued("out", Role::output, -15, {4}));
        model.layers.push_back(make_layer(LayerKind::tanh, {0}, 1));
        auto network = load_network(model);
        if (!network)
            return;
        fill(*network, 0, {-8, 8, 0, 1});
        if (!ran(*network))
            return;
        expect_i16(network->output(0), k_tanh, "tanh");
    }
    {
        Model model;
        model.tensors.push_back(valued("in", Role::input, 0, {4}));
        model.tensors.push_back(valued("out", Role::output, -15, {4}));
        model.layers.push_back(make_layer(LayerKind::sigmoid, {0}, 1));
        auto network = load_network(model);
        if (!network)
            return;
        fill(*network, 0, {-8, 8, 0, 1});
        if (!ran(*network))
            return;
        expect_i16(network->output(0), k_sigmoid, "sigmoid");
    }
    // Exponent -8 keeps the stored value as the offset from 2048.
    // 1 is index 2049.
    {
        Model model;
        model.tensors.push_back(valued("in", Role::input, -8, {1}));
        model.tensors.push_back(valued("out", Role::output, -15, {1}));
        model.layers.push_back(make_layer(LayerKind::tanh, {0}, 1));
        auto network = load_network(model);
        if (!network)
            return;
        fill(*network, 0, {1});
        if (!ran(*network))
            return;
        expect_i16(network->output(0), k_tanh_index, "tanh-index");
    }
    {
        Model model;
        model.tensors.push_back(valued("in", Role::input, -8, {1}));
        model.tensors.push_back(valued("out", Role::output, -15, {1}));
        model.layers.push_back(make_layer(LayerKind::sigmoid, {0}, 1));
        auto network = load_network(model);
        if (!network)
            return;
        fill(*network, 0, {1});
        if (!ran(*network))
            return;
        expect_i16(network->output(0), k_sigmoid_index, "sigmoid-index");
    }
}

void test_softmax() {
    // Four equal inputs. q = 2^30, S = 2^32, and each share is 32769 / 4 = 8192.
    {
        Model model;
        model.tensors.push_back(valued("in", Role::input, 0, {4}));
        model.tensors.push_back(valued("out", Role::output, -15, {4}));
        model.layers.push_back(make_layer(LayerKind::softmax, {0}, 1));
        auto network = load_network(model);
        if (!network)
            return;
        fill(*network, 0, {5, 5, 5, 5});
        if (!ran(*network))
            return;
        expect_i16(network->output(0), k_softmax_equal, "softmax-equal");
    }
    // 100 against zeros. The others are 100 << 8 = 25600, past 4095, so 0.
    // (2^30 * 32767 + 2^29) / 2^30 = 32767.
    {
        Model model;
        model.tensors.push_back(valued("in", Role::input, 0, {3}));
        model.tensors.push_back(valued("out", Role::output, -15, {3}));
        model.layers.push_back(make_layer(LayerKind::softmax, {0}, 1));
        auto network = load_network(model);
        if (!network)
            return;
        fill(*network, 0, {100, 0, 0});
        if (!ran(*network))
            return;
        expect_i16(network->output(0), k_softmax_peak, "softmax-peak");
    }
    // d(20) = 10 << 8 = 2560. d(0) = 30 << 8 = 7680, past 4095, so exactly 0.
    {
        Model model;
        model.tensors.push_back(valued("in", Role::input, 0, {3}));
        model.tensors.push_back(valued("out", Role::output, -15, {3}));
        model.layers.push_back(make_layer(LayerKind::softmax, {0}, 1));
        auto network = load_network(model);
        if (!network)
            return;
        fill(*network, 0, {30, 20, 0});
        if (!ran(*network))
            return;
        expect_i16(network->output(0), k_softmax_far, "softmax-far");
    }
}

void test_grid() {
    // conv 2*16*16*3*3*3 = 13824, relu 512, pool 2*8*8*2*2 = 512,
    // conv 2*8*8*2*1*1 = 256, tanh 128, flatten 128, dense 4*128 = 512,
    // sigmoid 4, softmax 16, argmax 4. Total 15896.
    // Bytes: picture 1536, conv weights 54, conv 1024, relu 1024, pool 256,
    // conv weights 4, conv 256, tanh 256, flat 256, dense weights 512,
    // dense 8, sigmoid 8, scores 8, pick 4. Total 5206.
    Sequence stream;
    Model model;
    model.tensors.push_back(valued("pic", Role::input, -30, {3, 16, 16}));
    Tensor conv_w = valued("cw", Role::constant, 0, {2, 3, 3, 3}, ElementType::int8);
    for (int element = 0; element < 54; ++element)
        push_i8(conv_w.data, stream.next_i8());
    model.tensors.push_back(std::move(conv_w));
    model.tensors.push_back(valued("conv", Role::activation, -18, {2, 16, 16}));
    model.tensors.push_back(valued("rel", Role::activation, -18, {2, 16, 16}));
    model.tensors.push_back(valued("pool", Role::activation, -18, {2, 8, 8}));
    Tensor conv2_w = valued("cw2", Role::constant, 0, {2, 2, 1, 1}, ElementType::int8);
    for (int element = 0; element < 4; ++element)
        push_i8(conv2_w.data, stream.next_i8());
    model.tensors.push_back(std::move(conv2_w));
    model.tensors.push_back(valued("conv2", Role::activation, -12, {2, 8, 8}));
    model.tensors.push_back(valued("th", Role::activation, -15, {2, 8, 8}));
    model.tensors.push_back(valued("flat", Role::activation, -15, {128}));
    Tensor dense_w = valued("dw", Role::constant, 0, {4, 128}, ElementType::int8);
    for (int element = 0; element < 512; ++element)
        push_i8(dense_w.data, stream.next_i8());
    model.tensors.push_back(std::move(dense_w));
    model.tensors.push_back(valued("den", Role::activation, -12, {4}));
    model.tensors.push_back(valued("sig", Role::activation, -15, {4}));
    model.tensors.push_back(valued("scores", Role::output, -15, {4}));
    model.tensors.push_back(valued("pick", Role::output, 0, {1}, ElementType::int32));

    model.layers.push_back(spatial(LayerKind::conv2d, {0}, 2, 3, 3, 1, 1, 1, 1, 1));
    model.layers.push_back(make_layer(LayerKind::relu, {2}, 3));
    model.layers.push_back(spatial(LayerKind::maxpool, {3}, 4, 2, 2, 2, 2, 0, 0));
    model.layers.push_back(spatial(LayerKind::conv2d, {4}, 6, 1, 1, 1, 1, 0, 0, 5));
    model.layers.push_back(make_layer(LayerKind::tanh, {6}, 7));
    model.layers.push_back(make_layer(LayerKind::flatten, {7}, 8));
    model.layers.push_back(make_layer(LayerKind::dense, {8}, 10, 9));
    model.layers.push_back(make_layer(LayerKind::sigmoid, {10}, 11));
    model.layers.push_back(make_layer(LayerKind::softmax, {11}, 12));
    model.layers.push_back(make_layer(LayerKind::argmax, {11}, 13));

    OA_CHECK(Network::memory_bytes_of(model) == 5206);
    auto network = load_network(model);
    if (!network)
        return;
    OA_CHECK(network->operations() == 15896);
    OA_CHECK(network->memory_bytes() == 5206);
    OA_CHECK(network->inputs().size() == 1);
    OA_CHECK(network->outputs().size() == 2);
    OA_CHECK(network->outputs()[1].index);

    std::vector<int16_t> scores;
    std::vector<int32_t> picks;
    scores.reserve(64);
    picks.reserve(16);
    for (int run = 0; run < 16; ++run) {
        const std::span<int16_t> picture = network->input(0);
        OA_CHECK(picture.size() == 768);
        if (picture.size() != 768)
            return;
        for (int16_t& sample : picture)
            sample = stream.next_i16();
        if (!ran(*network))
            return;
        const std::span<const int16_t> got = network->output(0);
        OA_CHECK(got.size() == 4);
        if (got.size() != 4)
            return;
        for (const int16_t value : got)
            scores.push_back(value);
        picks.push_back(network->output_index(1));
    }
    expect_i16(scores, k_grid_scores, "grid");
    OA_CHECK(picks.size() == 16);
    if (picks.size() != 16)
        return;
    for (std::size_t run = 0; run < picks.size(); ++run)
        OA_CHECK(picks[run] == k_grid_picks[run]);

    std::vector<uint8_t> bytes;
    for (std::size_t run = 0; run < picks.size(); ++run) {
        for (std::size_t element = 0; element < 4; ++element)
            append_i16_bytes(bytes, scores[run * 4 + element]);
        append_i32_bytes(bytes, picks[run]);
    }
    const std::string hex = digest_of(bytes);
    if (hex != pinned_grid_digest)
        std::fprintf(stderr, "grid sha256 %s\n", hex.c_str());
    OA_CHECK(hex == pinned_grid_digest);
}

void test_every_kind() {
    // One model holds every format-1 kind, so build refuses none of them.
    Model model;
    model.tensors.push_back(valued("pic", Role::input, 0, {1, 4, 4}));
    model.tensors.push_back(constant_i8("kern", 0, {1, 1, 1, 1}, {1}));
    model.tensors.push_back(valued("seen", Role::activation, 0, {1, 4, 4}));
    model.tensors.push_back(valued("rel", Role::activation, 0, {1, 4, 4}));
    model.tensors.push_back(valued("pool", Role::activation, 0, {1, 2, 2}));
    model.tensors.push_back(valued("curved", Role::activation, -15, {1, 2, 2}));
    model.tensors.push_back(valued("flat", Role::activation, -15, {4}));
    model.tensors.push_back(constant_i8("dw", 0, {2, 4}, {1, 0, 0, 0, 0, 1, 0, 0}));
    model.tensors.push_back(constant_i32("db", -15, {2}, {0, 0}));
    model.tensors.push_back(valued("den", Role::activation, -15, {2}));
    model.tensors.push_back(valued("leaked", Role::activation, -15, {2}));
    model.tensors.push_back(valued("sig", Role::activation, -15, {2}));
    model.tensors.push_back(valued("side", Role::input, -15, {2}));
    model.tensors.push_back(valued("sum", Role::activation, -15, {2}));
    model.tensors.push_back(valued("tail", Role::input, -15, {2}));
    model.tensors.push_back(valued("cat", Role::activation, -15, {4}));
    model.tensors.push_back(valued("ids", Role::input, 0, {2}));
    model.tensors.push_back(constant_i8("rows", 0, {4, 1}, {1, 2, 3, 4}));
    model.tensors.push_back(valued("emb", Role::activation, 0, {2, 1}));
    model.tensors.push_back(valued("wide", Role::output, 0, {2}));
    model.tensors.push_back(valued("scores", Role::output, -15, {4}));
    model.tensors.push_back(valued("pick", Role::output, 0, {1}, ElementType::int32));

    model.layers.push_back(spatial(LayerKind::conv2d, {0}, 2, 1, 1, 1, 1, 0, 0, 1));
    model.layers.push_back(make_layer(LayerKind::relu, {2}, 3));
    model.layers.push_back(spatial(LayerKind::maxpool, {3}, 4, 2, 2, 2, 2, 0, 0));
    model.layers.push_back(make_layer(LayerKind::tanh, {4}, 5));
    model.layers.push_back(make_layer(LayerKind::flatten, {5}, 6));
    model.layers.push_back(make_layer(LayerKind::dense, {6}, 9, 7, 8));
    Layer leaky = make_layer(LayerKind::leaky_relu, {9}, 10);
    leaky.alpha = 3277;
    model.layers.push_back(leaky);
    model.layers.push_back(make_layer(LayerKind::sigmoid, {10}, 11));
    model.layers.push_back(make_layer(LayerKind::add, {11, 12}, 13));
    model.layers.push_back(make_layer(LayerKind::concat, {13, 14}, 15));
    model.layers.push_back(make_layer(LayerKind::embedding, {16}, 18, 17));
    model.layers.push_back(make_layer(LayerKind::flatten, {18}, 19));
    model.layers.push_back(make_layer(LayerKind::softmax, {15}, 20));
    model.layers.push_back(make_layer(LayerKind::argmax, {15}, 21));

    auto network = load_network(model);
    if (!network)
        return;
    fill(*network, 0, {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16});
    fill(*network, 1, {1, -1});
    fill(*network, 2, {2, 3});
    fill(*network, 3, {1, 0});
    if (!ran(*network))
        return;
    OA_CHECK(network->output(0).size() == 2);
    OA_CHECK(network->output(1).size() == 4);
    OA_CHECK(network->outputs().size() == 3);
    OA_CHECK(network->outputs()[2].index);
}

void test_refusals() {
    Model empty;
    Problem problem;
    OA_CHECK(!Network::build(empty, problem).has_value());
    expect_message(problem, "the model has 0 tensors, and needs 2 to 4096");

    // 1024 relu layers of 64*64*65 elements. 1024 * 266240 = 272629760,
    // which is over 2^28.
    Model heavy;
    heavy.tensors.push_back(valued("in", Role::input, 0, {64, 64, 65}));
    for (int layer_number = 0; layer_number < 1024; ++layer_number) {
        const bool last = layer_number + 1 == 1024;
        heavy.tensors.push_back(valued(
            last ? "out" : "h" + std::to_string(layer_number),
            last ? Role::output : Role::activation,
            0,
            {64, 64, 65}
        ));
        heavy.layers.push_back(make_layer(
            LayerKind::relu,
            {static_cast<uint16_t>(layer_number)},
            static_cast<uint16_t>(layer_number + 1)
        ));
    }
    OA_CHECK(uint64_t{1024} * 64U * 64U * 65U == 272629760);
    if (const std::optional<Problem> found = check(heavy)) {
        std::fprintf(stderr, "heavy check: %s\n", found->message.c_str());
        OA_CHECK(false);
    } else {
        OA_CHECK(!Network::build(heavy, problem).has_value());
        expect_message(problem, "a run would take 272629760 operations; a run may take 268435456");
    }

    auto tiny = load_network(counted_model());
    if (!tiny)
        return;
    NetworkSet held;
    OA_CHECK(held.add("steady-commander", *tiny, problem));
    OA_CHECK(!held.add("steady-commander", *tiny, problem));
    expect_message(problem, "network steady-commander is already held");
    OA_CHECK(held.find("steady-commander") != nullptr);
    OA_CHECK(held.find("other") == nullptr);
    OA_CHECK(held.memory_bytes() == 44);
    OA_CHECK(max_brain_network_bytes == 67108864);

    // A 1 MiB cap. [512, 512] int16 is 524288 bytes, and the relu output
    // matches it, so the network is exactly 1048576 bytes.
    Model wide_model;
    wide_model.tensors.push_back(valued("in", Role::input, 0, {512, 512}));
    wide_model.tensors.push_back(valued("out", Role::output, 0, {512, 512}));
    wide_model.layers.push_back(make_layer(LayerKind::relu, {0}, 1));
    auto wide = load_network(wide_model);
    if (!wide)
        return;
    OA_CHECK(wide->memory_bytes() == 1048576);
    NetworkSet capped{std::size_t{1024} * 1024};
    OA_CHECK(capped.add("wide", *wide, problem));
    OA_CHECK(capped.memory_bytes() == 1048576);
    OA_CHECK(!capped.add("steady-commander", *tiny, problem));
    expect_message(problem, "networks would take 1048620 bytes; a brain may hold 1048576");
    OA_CHECK(capped.find("wide") != nullptr);
    OA_CHECK(capped.find("steady-commander") == nullptr);
    OA_CHECK(capped.memory_bytes() == 1048576);
}

} // namespace

int main() {
    test_rescale();
    test_dense();
    test_relu_and_leaky();
    test_add_and_concat();
    test_flatten_and_argmax();
    test_embedding();
    test_embedding_row();
    test_counts();
    test_mixed();
    test_tables();
    test_conv();
    test_maxpool();
    test_tanh_and_sigmoid();
    test_softmax();
    test_grid();
    test_every_kind();
    test_refusals();
    return oa::test::check_exit_status();
}

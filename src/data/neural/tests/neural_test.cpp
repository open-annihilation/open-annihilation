// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The integer runner: rescale, one small network of each kind this build
// runs, a mixed network pinned against neural_reference.py, and the refusals.
// The pinned digest is the same on every build and moves only with the
// arithmetic.

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
// clang-format on

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

void test_refusals() {
    Model empty;
    Problem problem;
    OA_CHECK(!Network::build(empty, problem).has_value());
    expect_message(problem, "the model has 0 tensors, and needs 2 to 4096");

    Model conv;
    conv.tensors.push_back(valued("grid", Role::input, 0, {1, 2, 2}));
    conv.tensors.push_back(constant_i8("kern", 0, {1, 1, 1, 1}, {1}));
    conv.tensors.push_back(valued("seen", Role::output, 0, {1, 2, 2}));
    Layer conv_layer = make_layer(LayerKind::conv2d, {0}, 2, 1);
    conv_layer.kernel_h = 1;
    conv_layer.kernel_w = 1;
    conv_layer.stride_h = 1;
    conv_layer.stride_w = 1;
    conv.layers.push_back(conv_layer);
    if (const std::optional<Problem> found = check(conv)) {
        std::fprintf(stderr, "conv check: %s\n", found->message.c_str());
        OA_CHECK(false);
    } else {
        OA_CHECK(!Network::build(conv, problem).has_value());
        expect_message(problem, "layer 0 (conv2d) is not run by this build");
    }

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
    test_refusals();
    return oa::test::check_exit_status();
}

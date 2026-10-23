// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A network with one layer of each kind, written and read back, and one
// refusal for every format rule. The pinned sha256 is the all-kinds file.

#include "oa/formats/oann/oann.hpp"

#include "oa/base/sha256.hpp"
#include "oa/test/check.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

using oa::formats::oann::ElementType;
using oa::formats::oann::Layer;
using oa::formats::oann::LayerKind;
using oa::formats::oann::Model;
using oa::formats::oann::Problem;
using oa::formats::oann::Role;
using oa::formats::oann::Tensor;
using oa::formats::oann::check;
using oa::formats::oann::element_bytes;
using oa::formats::oann::element_count;
using oa::formats::oann::header_bytes;
using oa::formats::oann::kind_name;
using oa::formats::oann::layer_entry_bytes;
using oa::formats::oann::max_dimension;
using oa::formats::oann::max_file_bytes;
using oa::formats::oann::max_tensor_elements;
using oa::formats::oann::max_weights;
using oa::formats::oann::no_tensor;
using oa::formats::oann::parse_kind;
using oa::formats::oann::read;
using oa::formats::oann::tensor_entry_bytes;
using oa::formats::oann::write;

// Pinned sha256 of the all-kinds network. It moves only when format 1's byte form changes.
constexpr std::string_view k_all_kinds_sha256 =
    "ef392a263b9fdf7901a16bd8ec985ad833569267e165b7b6c80354bcc60a9f33";

std::span<const uint8_t> view_of(const std::vector<uint8_t>& bytes) {
    return std::span<const uint8_t>(bytes.data(), bytes.size());
}

Tensor make_tensor(
    std::string name,
    Role role,
    ElementType type,
    int8_t exponent,
    uint8_t rank,
    std::initializer_list<uint32_t> dims,
    uint8_t seed
) {
    Tensor tensor;
    tensor.name = std::move(name);
    tensor.role = role;
    tensor.type = type;
    tensor.exponent = exponent;
    tensor.rank = rank;
    uint8_t axis = 0;
    for (const uint32_t dim : dims) {
        tensor.dims[axis] = dim;
        ++axis;
    }
    if (role == Role::constant) {
        const std::size_t bytes =
            static_cast<std::size_t>(element_count(tensor) * element_bytes(type));
        tensor.data.resize(bytes);
        for (std::size_t index = 0; index < bytes; ++index)
            tensor.data[index] = static_cast<uint8_t>(seed + index * 17U);
    }
    return tensor;
}

Tensor constant_of(
    std::string name,
    ElementType type,
    int8_t exponent,
    std::initializer_list<uint32_t> dims,
    uint8_t seed
) {
    return make_tensor(
        std::move(name),
        Role::constant,
        type,
        exponent,
        static_cast<uint8_t>(dims.size()),
        dims,
        seed
    );
}

Tensor valued(std::string name, Role role, int8_t exponent, std::initializer_list<uint32_t> dims) {
    // An output of shape [1] is the argmax port and is int32. Every other
    // valued tensor is int16. Callers that need a different type set it.
    const bool single =
        role == Role::output && exponent == 0 && dims.size() == 1 && *dims.begin() == 1;
    return make_tensor(
        std::move(name),
        role,
        single ? ElementType::int32 : ElementType::int16,
        exponent,
        static_cast<uint8_t>(dims.size()),
        dims,
        0
    );
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

// Thirteen layers, one of each kind, with every tensor read or written as the
// format requires. Indexes below are the tensor table order.
Model all_kinds() {
    Model model;
    // 0 tokens, 1 emb_w, 2 emb, 3 flat, 4 dense1_w, 5 dense1_b, 6 dense1,
    // 7 relu1, 8 leaky1, 9 side, 10 sum1, 11 tanh1, 12 sig1, 13 soft1,
    // 14 choice, 15 grid, 16 conv1_w, 17 conv1_b, 18 conv1, 19 pool1,
    // 20 grid2, 21 joined.
    model.tensors.push_back(valued("tokens", Role::input, 0, {2}));
    model.tensors.push_back(constant_of("emb_w", ElementType::int8, 0, {4, 3}, 3));
    model.tensors.push_back(valued("emb", Role::activation, 0, {2, 3}));
    model.tensors.push_back(valued("flat", Role::activation, 0, {6}));
    model.tensors.push_back(constant_of("dense1_w", ElementType::int8, 0, {4, 6}, 5));
    model.tensors.push_back(constant_of("dense1_b", ElementType::int32, 0, {4}, 7));
    model.tensors.push_back(valued("dense1", Role::activation, 0, {4}));
    model.tensors.push_back(valued("relu1", Role::activation, 0, {4}));
    model.tensors.push_back(valued("leaky1", Role::activation, 0, {4}));
    model.tensors.push_back(valued("side", Role::input, 0, {4}));
    model.tensors.push_back(valued("sum1", Role::activation, 0, {4}));
    model.tensors.push_back(valued("tanh1", Role::activation, -15, {4}));
    model.tensors.push_back(valued("sig1", Role::activation, -15, {4}));
    model.tensors.push_back(valued("soft1", Role::activation, -15, {4}));
    model.tensors.push_back(make_tensor("choice", Role::output, ElementType::int32, 0, 1, {1}, 0));
    model.tensors.push_back(valued("grid", Role::input, 0, {1, 4, 4}));
    model.tensors.push_back(constant_of("conv1_w", ElementType::int8, 0, {2, 1, 3, 3}, 9));
    model.tensors.push_back(constant_of("conv1_b", ElementType::int32, 0, {2}, 11));
    model.tensors.push_back(valued("conv1", Role::activation, 0, {2, 4, 4}));
    model.tensors.push_back(valued("pool1", Role::activation, 0, {2, 2, 2}));
    model.tensors.push_back(valued("grid2", Role::input, 0, {2, 2, 1}));
    model.tensors.push_back(valued("joined", Role::output, 0, {2, 2, 3}));

    model.layers.push_back(make_layer(LayerKind::embedding, {0}, 2, 1));
    model.layers.push_back(make_layer(LayerKind::flatten, {2}, 3));
    model.layers.push_back(make_layer(LayerKind::dense, {3}, 6, 4, 5));
    model.layers.push_back(make_layer(LayerKind::relu, {6}, 7));
    Layer leaky = make_layer(LayerKind::leaky_relu, {7}, 8);
    leaky.alpha = 16384;
    model.layers.push_back(leaky);
    model.layers.push_back(make_layer(LayerKind::add, {8, 9}, 10));
    model.layers.push_back(make_layer(LayerKind::tanh, {10}, 11));
    model.layers.push_back(make_layer(LayerKind::sigmoid, {11}, 12));
    model.layers.push_back(make_layer(LayerKind::softmax, {12}, 13));
    model.layers.push_back(make_layer(LayerKind::argmax, {13}, 14));
    Layer conv = make_layer(LayerKind::conv2d, {15}, 18, 16, 17);
    conv.kernel_h = 3;
    conv.kernel_w = 3;
    conv.stride_h = 1;
    conv.stride_w = 1;
    conv.pad_h = 1;
    conv.pad_w = 1;
    model.layers.push_back(conv);
    Layer pool = make_layer(LayerKind::maxpool, {18}, 19);
    pool.kernel_h = 2;
    pool.kernel_w = 2;
    pool.stride_h = 2;
    pool.stride_w = 2;
    model.layers.push_back(pool);
    Layer concat = make_layer(LayerKind::concat, {19, 20}, 21);
    concat.axis = 2;
    model.layers.push_back(concat);
    return model;
}

uint32_t tensor_at(uint32_t index, uint32_t field) {
    return header_bytes + index * tensor_entry_bytes + field;
}

bool same_tensor(const Tensor& got, const Tensor& want) {
    return got.name == want.name && got.role == want.role && got.type == want.type &&
           got.exponent == want.exponent && got.rank == want.rank && got.dims == want.dims &&
           got.data == want.data;
}

bool same_layer(const Layer& got, const Layer& want) {
    return got.kind == want.kind && got.input_count == want.input_count &&
           got.inputs == want.inputs && got.output == want.output && got.weights == want.weights &&
           got.bias == want.bias && got.kernel_h == want.kernel_h &&
           got.kernel_w == want.kernel_w && got.stride_h == want.stride_h &&
           got.stride_w == want.stride_w && got.pad_h == want.pad_h && got.pad_w == want.pad_w &&
           got.axis == want.axis && got.alpha == want.alpha;
}

void expect(const Model& model, std::string_view prefix) {
    const std::optional<Problem> found = check(model);
    if (!found || !found->message.starts_with(prefix)) {
        std::fprintf(stderr, "expected prefix: %s\n", std::string(prefix).c_str());
        if (found)
            std::fprintf(
                stderr, "actual message: %s (byte %u)\n", found->message.c_str(), found->offset
            );
        else
            std::fprintf(stderr, "actual message: (accepted)\n");
        OA_CHECK(false);
        return;
    }
    Problem problem;
    const std::optional<std::vector<uint8_t>> encoded = write(model, problem);
    OA_CHECK(!encoded.has_value());
    OA_CHECK(problem.message == found->message);
    OA_CHECK(problem.offset == found->offset);
}

void test_round_trip() {
    const Model model = all_kinds();
    Problem problem;
    const std::optional<std::vector<uint8_t>> first = write(model, problem);
    OA_CHECK(first.has_value());
    if (!first)
        return;
    const std::string hex = [&] {
        const auto text = oa::base::sha256::to_hex(oa::base::sha256::digest_of(view_of(*first)));
        return std::string(text.begin(), text.end());
    }();
    if (hex != k_all_kinds_sha256)
        std::fprintf(stderr, "sha256 %s\n", hex.c_str());
    OA_CHECK(hex == k_all_kinds_sha256);

    const std::optional<Model> back = read(view_of(*first), problem);
    OA_CHECK(back.has_value());
    if (!back)
        return;
    OA_CHECK(back->tensors.size() == model.tensors.size());
    OA_CHECK(back->layers.size() == model.layers.size());
    if (back->tensors.size() != model.tensors.size() || back->layers.size() != model.layers.size())
        return;
    for (std::size_t index = 0; index < model.tensors.size(); ++index)
        OA_CHECK(same_tensor(back->tensors[index], model.tensors[index]));
    for (std::size_t index = 0; index < model.layers.size(); ++index)
        OA_CHECK(same_layer(back->layers[index], model.layers[index]));

    const std::optional<std::vector<uint8_t>> second = write(*back, problem);
    OA_CHECK(second.has_value());
    OA_CHECK(second == first);

    OA_CHECK(kind_name(LayerKind::dense) == "dense");
    OA_CHECK(kind_name(LayerKind::embedding) == "embedding");
    OA_CHECK(parse_kind("leaky_relu") == LayerKind::leaky_relu);
    OA_CHECK(!parse_kind("Dense").has_value());
    OA_CHECK(element_bytes(ElementType::int8) == 1);
    OA_CHECK(element_bytes(ElementType::int16) == 2);
    OA_CHECK(element_bytes(ElementType::int32) == 4);
    OA_CHECK(element_count(model.tensors[3]) == 6);
    const std::string described = oa::formats::oann::describe(Problem{"the magic is not OANN", 0});
    OA_CHECK(described == "byte 0: the magic is not OANN");
}

void test_rules() {
    const Model model = all_kinds();

    {
        Model broken = model;
        broken.tensors.resize(1);
        expect(broken, "the model has 1 tensor, and needs 2 to 4096");
    }
    {
        Model broken = model;
        broken.layers.clear();
        expect(broken, "the model has 0 layers, and needs 1 to 1024");
    }
    {
        Model broken = model;
        broken.tensors[0].name.clear();
        expect(broken, "tensor 0: the name is empty");
        const auto found = check(broken);
        OA_CHECK(found.has_value() && found->offset == tensor_at(0, 0));
    }
    {
        Model broken = model;
        broken.tensors[0].name = std::string(32, 'a');
        expect(broken, "tensor 0 (aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa): the name is 32 bytes");
    }
    {
        Model broken = model;
        broken.tensors[0].name = "Tokens";
        expect(broken, "tensor 0 (Tokens): the name has a byte outside a-z, 0-9 and _");
    }
    {
        Model broken = model;
        broken.tensors[9].name = "1side";
        expect(broken, "tensor 9 (1side): the name starts with '1', and must start with a letter");
    }
    {
        Model broken = model;
        broken.tensors[9].name = "_side";
        expect(broken, "tensor 9 (_side): the name starts with '_', and must start with a letter");
    }
    {
        Model broken = model;
        broken.tensors[9].name = "tokens";
        expect(broken, "tensor 9 (tokens): the name is the same as tensor 0 (tokens)");
    }
    {
        Model broken = model;
        broken.tensors[0].role = static_cast<Role>(0);
        expect(
            broken,
            "tensor 0 (tokens): the role is 0, and must be input, output, activation or constant"
        );
    }
    {
        Model broken = model;
        broken.tensors[1].type = static_cast<ElementType>(0);
        expect(broken, "tensor 1 (emb_w): the type is 0, and must be int8, int16 or int32");
    }
    {
        Model broken = model;
        broken.tensors[0].exponent = -49;
        expect(broken, "tensor 0 (tokens): the exponent is -49, and must be -48 to 16");
    }
    {
        Model broken = model;
        broken.tensors[0].exponent = 17;
        expect(broken, "tensor 0 (tokens): the exponent is 17, and must be -48 to 16");
    }
    {
        Model broken = model;
        broken.tensors[0].rank = 0;
        expect(broken, "tensor 0 (tokens): the rank is 0, and must be 1 to 4");
    }
    {
        Model broken = model;
        broken.tensors[0].rank = 5;
        expect(broken, "tensor 0 (tokens): the rank is 5, and must be 1 to 4");
    }
    {
        Model broken = model;
        broken.tensors[0].dims[0] = 0;
        expect(broken, "tensor 0 (tokens): dim 0 is 0, and must be 1 to 65535");
    }
    {
        Model broken = model;
        broken.tensors[0].dims[0] = max_dimension + 1;
        expect(broken, "tensor 0 (tokens): dim 0 is 65536, and must be 1 to 65535");
    }
    {
        Model broken = model;
        broken.tensors[0].dims[1] = 1;
        expect(broken, "tensor 0 (tokens): dim 1 is 1, and must be 0");
    }
    {
        Model broken = model;
        broken.tensors[0].rank = 2;
        broken.tensors[0].dims[0] = 65535;
        broken.tensors[0].dims[1] = 257;
        expect(
            broken, "tensor 0 (tokens): the tensor has 16842495 elements, and the limit is 16777216"
        );
    }
    {
        Model broken = model;
        broken.tensors[4].data.resize(20);
        expect(
            broken,
            "tensor 4 (dense1_w): a constant needs int8, int16 or int32 data of 24 bytes, the "
            "entry says 20"
        );
    }
    {
        Model broken = model;
        broken.tensors[0].data = {1, 2};
        expect(
            broken, "tensor 0 (tokens): an input carries 2 bytes of data, and only a constant does"
        );
    }
    {
        Model broken = model;
        broken.tensors[0].type = ElementType::int8;
        expect(broken, "tensor 0 (tokens): an input needs int16, and the type is int8");
    }
    {
        Model broken = model;
        broken.tensors[7].type = ElementType::int32;
        expect(broken, "tensor 7 (relu1): an activation needs int16, and the type is int32");
    }
    {
        // One input, one int32 constant of 2^24 elements and one output.
        // 16 + 64*3 + 32 + 67108864 = 67109104, just past the file limit.
        // The constant's shape is legal, so the file-size rule is the one that fires.
        Model broken;
        broken.tensors.push_back(valued("one", Role::input, 0, {1}));
        broken.tensors.push_back(
            make_tensor("bulk", Role::constant, ElementType::int32, 0, 3, {256, 256, 256}, 1)
        );
        broken.tensors.push_back(valued("two", Role::output, 0, {2}));
        broken.layers.push_back(make_layer(LayerKind::relu, {0}, 2));
        expect(broken, "the network is 67109104 bytes, and the limit is 67108864");
    }

    {
        Model broken = model;
        broken.layers[0].kind = static_cast<LayerKind>(0);
        expect(broken, "layer 0: the kind is 0, and must be one of the thirteen");
    }
    {
        Model broken = model;
        broken.layers[2].input_count = 2;
        expect(broken, "layer 2 (dense): needs 1 input, the entry says 2");
    }
    {
        Model broken = model;
        broken.layers[2].inputs[0] = no_tensor;
        expect(broken, "layer 2 (dense): input 0 is none, and must name a tensor");
    }
    {
        Model broken = model;
        broken.layers[2].inputs[0] = 4000;
        expect(broken, "layer 2 (dense): input 0 is 4000, and the model has 22 tensors");
    }
    {
        Model broken = model;
        broken.layers[2].inputs[1] = 0;
        expect(broken, "layer 2 (dense): input slot 1 is 0, and an unused slot is none");
    }
    {
        Model broken = model;
        broken.layers[2].output = 4000;
        expect(broken, "layer 2 (dense): the output is 4000, and the model has 22 tensors");
    }
    {
        Model broken = model;
        broken.layers[2].weights = no_tensor;
        expect(broken, "layer 2 (dense): needs weights");
    }
    {
        Model broken = model;
        broken.layers[3].weights = 4;
        expect(broken, "layer 3 (relu): names weights, and this layer has none");
    }
    {
        Model broken = model;
        broken.layers[3].bias = 5;
        expect(broken, "layer 3 (relu): names a bias, and this layer has none");
    }
    {
        Model broken = model;
        broken.layers[10].kernel_h = 0;
        expect(broken, "layer 10 (conv2d): the kernel height is 0, and must be 1 to 15");
    }
    {
        Model broken = model;
        broken.layers[3].kernel_h = 1;
        expect(broken, "layer 3 (relu): the kernel height is 1, and must be 0");
    }
    {
        Model broken = model;
        broken.layers[10].stride_h = 0;
        expect(broken, "layer 10 (conv2d): the stride height is 0, and must be 1 to 15");
    }
    {
        Model broken = model;
        broken.layers[11].pad_h = 1;
        expect(broken, "layer 11 (maxpool): the pad height is 1, and must be 0");
    }
    {
        Model broken = model;
        broken.layers[10].pad_h = 3;
        expect(broken, "layer 10 (conv2d): the pad height is 3, and must be 0 to 2");
    }
    {
        Model broken = model;
        broken.layers[3].axis = 1;
        expect(broken, "layer 3 (relu): the axis is 1, and must be 0");
    }
    {
        Model broken = model;
        broken.layers[12].axis = 3;
        expect(broken, "layer 12 (concat): the axis is 3, and must be 0 to 2");
    }
    {
        Model broken = model;
        broken.layers[3].alpha = 1;
        expect(broken, "layer 3 (relu): the slope is 1, and must be 0");
    }
    {
        Model broken = model;
        broken.layers[4].alpha = -1;
        expect(broken, "layer 4 (leaky_relu): the slope is -1, and must be 0 to 32767");
    }

    {
        Model broken = model;
        broken.tensors[4].dims[1] = 5;
        broken.tensors[4].data.assign(20, 1);
        expect(broken, "layer 2 (dense): the weights are [4, 5], and must be [4, 6]");
    }
    {
        Model broken = model;
        broken.tensors[4].type = ElementType::int32;
        broken.tensors[4].data.assign(4U * 6U * 4U, 1);
        expect(broken, "layer 2 (dense): the weights' type is int32, and must be int8 or int16");
    }
    {
        Model broken = model;
        broken.tensors[5].type = ElementType::int16;
        broken.tensors[5].data.assign(8, 1);
        expect(broken, "layer 2 (dense): the bias's type is int16, and must be int32");
    }
    {
        Model broken = model;
        broken.tensors[5].dims[0] = 5;
        broken.tensors[5].data.assign(20, 1);
        expect(broken, "layer 2 (dense): the bias is [5], and must be [4]");
    }
    {
        Model broken = model;
        broken.tensors[5].exponent = -1;
        const auto found = check(broken);
        expect(
            broken,
            "layer 2 (dense): the bias's exponent is -1, the input's and the weights' make 0"
        );
        OA_CHECK(found.has_value() && found->offset == tensor_at(5, 34));
    }
    {
        Model broken = model;
        broken.tensors[4].exponent = -48;
        broken.tensors[5].exponent = -48;
        broken.tensors[6].exponent = 16;
        expect(broken, "layer 2 (dense): the output scale shift is 64, and must be 0 to 62");
    }
    {
        Model broken = model;
        broken.tensors[6].dims[0] = 5;
        expect(broken, "layer 2 (dense): the output is [5], and must be [4]");
    }
    {
        Model broken = model;
        broken.tensors[15].rank = 1;
        broken.tensors[15].dims = {16, 0, 0, 0};
        expect(broken, "layer 10 (conv2d): the input's rank is 1, and must be 3");
    }
    {
        Model broken = model;
        broken.layers[10].kernel_h = 15;
        broken.layers[10].kernel_w = 15;
        broken.layers[10].pad_h = 0;
        broken.layers[10].pad_w = 0;
        broken.tensors[16].dims = {2, 1, 15, 15};
        broken.tensors[16].data.assign(2U * 1U * 15U * 15U, 1);
        expect(
            broken,
            "layer 10 (conv2d): the input height is 4 and the padding is 0, which is shorter than "
            "the kernel height 15"
        );
    }
    {
        Model broken = model;
        broken.tensors[18].dims[1] = 3;
        expect(broken, "layer 10 (conv2d): the output is [2, 3, 4], and must be [2, 4, 4]");
    }
    {
        Model broken = model;
        broken.tensors[17].exponent = 1;
        expect(
            broken,
            "layer 10 (conv2d): the bias's exponent is 1, the input's and the weights' make 0"
        );
    }
    {
        Model broken = model;
        broken.layers[11].kernel_h = 15;
        broken.layers[11].stride_h = 1;
        expect(
            broken,
            "layer 11 (maxpool): the input height is 4, and must be at least the kernel height 15"
        );
    }
    {
        Model broken = model;
        broken.tensors[19].dims[2] = 1;
        expect(broken, "layer 11 (maxpool): the output is [2, 2, 1], and must be [2, 2, 2]");
    }
    {
        Model broken = model;
        broken.tensors[19].exponent = 1;
        expect(broken, "layer 11 (maxpool): the output's exponent is 1, the input's is 0");
    }
    {
        Model broken = model;
        broken.tensors[7].dims[0] = 5;
        expect(broken, "layer 3 (relu): the output is [5], and must match the input [4]");
    }
    {
        Model broken = model;
        broken.tensors[8].exponent = 1;
        expect(broken, "layer 4 (leaky_relu): the output's exponent is 1, the input's is 0");
    }
    {
        Model broken = model;
        broken.tensors[11].exponent = -14;
        expect(broken, "layer 6 (tanh): the output's exponent is -14, and must be -15");
    }
    {
        Model broken = model;
        broken.tensors[12].dims[0] = 3;
        expect(broken, "layer 7 (sigmoid): the output is [3], and must match the input [4]");
    }
    {
        Model broken = model;
        broken.tensors[9].dims[0] = 5;
        expect(broken, "layer 5 (add): input 1 is [5], and must match input 0 [4]");
    }
    {
        Model broken = model;
        broken.tensors[10].dims[0] = 3;
        expect(broken, "layer 5 (add): the output is [3], and must match the inputs [4]");
    }
    {
        Model broken = model;
        broken.tensors[20].dims[0] = 1;
        expect(
            broken, "layer 12 (concat): input 1's dim 0 is 1, and must match input 0's dim 0 of 2"
        );
    }
    {
        Model broken = model;
        broken.tensors[21].dims[2] = 2;
        expect(broken, "layer 12 (concat): the output's dim 2 is 2, and the inputs join to 3");
    }
    {
        Model broken = model;
        broken.tensors[3].dims[0] = 5;
        expect(broken, "layer 1 (flatten): the output is [5], and must be a vector of 6");
    }
    {
        Model broken = model;
        broken.tensors[13].exponent = -14;
        expect(broken, "layer 8 (softmax): the output's exponent is -14, and must be -15");
    }
    {
        Model broken = model;
        broken.tensors[14].type = ElementType::int16;
        expect(broken, "layer 9 (argmax): the output's type is int16, and must be int32");
    }
    {
        Model broken = model;
        broken.tensors[14].dims[0] = 2;
        expect(broken, "layer 9 (argmax): the output is [2], and must be [1]");
    }
    {
        Model broken = model;
        broken.tensors[14].exponent = 1;
        expect(broken, "layer 9 (argmax): the output's exponent is 1, and must be 0");
    }
    {
        Model broken = model;
        broken.tensors[14].role = Role::constant;
        broken.tensors[14].data.assign(4, 1);
        expect(broken, "layer 9 (argmax): the output's role is constant, and must be output");
    }
    {
        Model broken = model;
        broken.layers[12].inputs[0] = 14;
        expect(broken, "layer 9 (argmax): tensor 14 (choice) is read by layer 12 (concat)");
    }
    {
        Model broken = model;
        broken.tensors[0].exponent = 1;
        expect(broken, "layer 0 (embedding): the input's exponent is 1, and must be 0");
    }
    {
        Model broken = model;
        broken.tensors[2].dims[1] = 2;
        expect(broken, "layer 0 (embedding): the output is [2, 2], and must be [2, 3]");
    }
    {
        Model broken = model;
        broken.tensors[2].exponent = 1;
        expect(broken, "layer 0 (embedding): the output's exponent is 1, the weights' is 0");
    }
    {
        Model broken = model;
        broken.tensors.push_back(constant_of("odd", ElementType::int32, 0, {4}, 1));
        broken.layers[3].inputs[0] = static_cast<uint16_t>(broken.tensors.size() - 1);
        expect(broken, "layer 3 (relu): the input's type is int32, and must be int16");
    }

    {
        Model broken = model;
        broken.layers[3].inputs[0] = 8;
        expect(broken, "layer 3 (relu): reads tensor 8 (leaky1), which no earlier layer writes");
    }
    {
        Model broken = model;
        broken.layers[3].output = 6;
        expect(
            broken, "layer 3 (relu): writes tensor 6 (dense1), which layer 2 (dense) already writes"
        );
    }
    {
        Model broken = model;
        broken.tensors[7].role = Role::input;
        expect(broken, "layer 3 (relu): writes tensor 7 (relu1), which is an input");
    }
    {
        Model broken = model;
        broken.tensors.push_back(valued("spare", Role::activation, 0, {1}));
        expect(broken, "tensor 22 (spare): an activation is written by no layer");
    }
    {
        Model broken = model;
        broken.tensors.push_back(valued("spare", Role::output, 0, {4}));
        expect(broken, "tensor 22 (spare): an output is written by no layer");
    }
    {
        Model broken = model;
        broken.layers[4].inputs[0] = 6;
        expect(broken, "tensor 7 (relu1): an activation is read by no layer");
    }
    {
        Model broken = model;
        broken.layers[12].inputs[1] = 19;
        broken.tensors[21].dims[2] = 4;
        expect(broken, "tensor 20 (grid2): an input is read by no layer");
    }
    {
        Model broken = model;
        broken.tensors.push_back(constant_of("spare", ElementType::int16, 0, {2, 2, 2}, 1));
        broken.layers[12].input_count = 3;
        broken.layers[12].inputs[2] = static_cast<uint16_t>(broken.tensors.size() - 1);
        broken.tensors[21].dims[2] = 5;
        expect(
            broken,
            "layer 12 (concat): reads tensor 22 (spare) as an input, and a constant is only read "
            "as weights or a bias"
        );
    }
    {
        Model broken = model;
        broken.tensors.push_back(constant_of("spare", ElementType::int8, 0, {1}, 1));
        expect(broken, "tensor 22 (spare): a constant is used by no layer");
    }
    {
        Model broken = model;
        uint16_t left = 9;
        for (int extra = 0; extra < 13; ++extra) {
            const bool last = extra == 12;
            broken.tensors.push_back(valued("extra" + std::to_string(extra), Role::input, 0, {4}));
            const auto input = static_cast<uint16_t>(broken.tensors.size() - 1);
            broken.tensors.push_back(valued(
                last ? "extraout" : "extrasum" + std::to_string(extra),
                last ? Role::output : Role::activation,
                0,
                {4}
            ));
            const auto output = static_cast<uint16_t>(broken.tensors.size() - 1);
            broken.layers.push_back(make_layer(LayerKind::add, {left, input}, output));
            left = output;
        }
        expect(broken, "the model has 17 inputs, and the limit is 16");
    }
    {
        Model broken = model;
        for (int extra = 0; extra < 15; ++extra) {
            broken.tensors.push_back(valued("more" + std::to_string(extra), Role::output, 0, {4}));
            const auto output = static_cast<uint16_t>(broken.tensors.size() - 1);
            broken.layers.push_back(make_layer(LayerKind::relu, {9}, output));
        }
        expect(broken, "the model has 17 outputs, and the limit is 16");
    }
}

void store_u16(std::vector<uint8_t>& bytes, std::size_t offset, uint16_t value) {
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 8U);
}

void store_u32(std::vector<uint8_t>& bytes, std::size_t offset, uint32_t value) {
    bytes[offset] = static_cast<uint8_t>(value);
    bytes[offset + 1] = static_cast<uint8_t>(value >> 8U);
    bytes[offset + 2] = static_cast<uint8_t>(value >> 16U);
    bytes[offset + 3] = static_cast<uint8_t>(value >> 24U);
}

void expect_bytes(const std::vector<uint8_t>& bytes, std::string_view prefix) {
    Problem problem;
    const std::optional<Model> model = read(view_of(bytes), problem);
    if (model || !problem.message.starts_with(prefix)) {
        std::fprintf(stderr, "bytes expected prefix: %s\n", std::string(prefix).c_str());
        if (model)
            std::fprintf(stderr, "bytes were accepted\n");
        else
            std::fprintf(
                stderr, "bytes message: %s (byte %u)\n", problem.message.c_str(), problem.offset
            );
        OA_CHECK(false);
    }
}

void test_malformed() {
    Problem problem;
    const std::optional<std::vector<uint8_t>> file = write(all_kinds(), problem);
    OA_CHECK(file.has_value());
    if (!file)
        return;

    for (std::size_t length = 0; length < file->size(); ++length) {
        const auto prefix = std::span<const uint8_t>(file->data(), length);
        Problem refusal;
        const std::optional<Model> model = read(prefix, refusal);
        OA_CHECK(!model.has_value());
    }

    {
        auto bytes = *file;
        store_u16(bytes, 8, 4097);
        expect_bytes(bytes, "the tensor count is 4097, and must be 2 to 4096");
    }
    {
        auto bytes = *file;
        store_u16(bytes, 10, 1025);
        expect_bytes(bytes, "the layer count is 1025, and must be 1 to 1024");
    }
    {
        auto bytes = *file;
        bytes[tensor_at(0, 35)] = 2;
        store_u32(bytes, tensor_at(0, 36), 65535);
        store_u32(bytes, tensor_at(0, 40), 257);
        expect_bytes(bytes, "tensor 0 (tokens): the tensor has 16842495 elements");
    }
    {
        auto bytes = *file;
        store_u32(bytes, tensor_at(1, 52), 1000);
        expect_bytes(bytes, "tensor 1 (emb_w): the constant starts at 1000");
    }
    {
        auto bytes = *file;
        const uint32_t data_bytes = bytes[12] | (uint32_t{bytes[13]} << 8U) |
                                    (uint32_t{bytes[14]} << 16U) | (uint32_t{bytes[15]} << 24U);
        store_u32(bytes, 12, data_bytes + 8);
        expect_bytes(
            bytes, "the file has " + std::to_string(file->size()) + " bytes, and the network needs"
        );
    }
    {
        auto bytes = *file;
        store_u32(bytes, tensor_at(4, 52), 13);
        expect_bytes(bytes, "tensor 4 (dense1_w): the constant starts at 13, and its place is 12");
    }
    {
        auto bytes = *file;
        // conv1_w is 18 bytes at data offset 52, so the first padding byte is at 70.
        const std::size_t data_origin =
            header_bytes + 22 * tensor_entry_bytes + 13 * layer_entry_bytes;
        bytes[data_origin + 70] = 1;
        expect_bytes(bytes, "a padding byte is 1, and must be 0");
    }
    {
        auto bytes = *file;
        bytes.push_back(0);
        expect_bytes(
            bytes,
            "the file has " + std::to_string(file->size() + 1) + " bytes, and the network ends at"
        );
    }
    {
        auto bytes = *file;
        bytes[tensor_at(0, 0)] = 'T';
        expect_bytes(bytes, "tensor 0: the name has a byte outside a-z, 0-9 and _");
    }
    {
        auto bytes = *file;
        std::memcpy(bytes.data() + tensor_at(9, 0), bytes.data() + tensor_at(0, 0), 32);
        expect_bytes(bytes, "tensor 9 (tokens): the name is the same as tensor 0 (tokens)");
    }
    {
        auto bytes = *file;
        bytes[tensor_at(0, 7)] = 1;
        expect_bytes(bytes, "tensor 0: a byte after the name is 1, and must be 0");
    }
    {
        auto bytes = *file;
        store_u16(bytes, 6, 1);
        expect_bytes(bytes, "the flags are 1, and must be 0");
    }
    {
        auto bytes = *file;
        bytes[0] = 'X';
        expect_bytes(bytes, "the magic is not OANN");
    }
}

Tensor wide_input() {
    return make_tensor("wide", Role::input, ElementType::int16, 0, 1, {32768}, 0);
}

Tensor wide_weights() {
    return constant_of("wide_w", ElementType::int8, 0, {256, 32768}, 1);
}

Tensor wide_output() {
    return make_tensor("wideout", Role::output, ElementType::int16, 0, 1, {256}, 0);
}

void test_weight_limit() {
    Model exact;
    exact.tensors.push_back(wide_input());
    exact.tensors.push_back(wide_weights());
    exact.tensors.back().data.assign(max_weights, 1);
    exact.tensors.push_back(wide_output());
    exact.layers.push_back(make_layer(LayerKind::dense, {0}, 2, 1));
    OA_CHECK(element_count(exact.tensors[1]) == max_weights);
    OA_CHECK(!check(exact).has_value());
    Problem problem;
    const auto encoded = write(exact, problem);
    OA_CHECK(encoded.has_value());
    if (!encoded)
        return;
    const auto back = read(view_of(*encoded), problem);
    OA_CHECK(back.has_value());

    Model over = exact;
    over.tensors.push_back(make_tensor("tiny", Role::input, ElementType::int16, 0, 1, {1}, 0));
    over.tensors.push_back(constant_of("tiny_w", ElementType::int8, 0, {1, 1}, 2));
    over.tensors.push_back(make_tensor("tinyout", Role::output, ElementType::int16, 0, 1, {1}, 0));
    over.layers.push_back(make_layer(LayerKind::dense, {3}, 5, 4));
    expect(over, "layer 1 (dense): the weights hold 8388609 elements, and the limit is 8388608");
}

} // namespace

int main() {
    test_round_trip();
    test_rules();
    test_malformed();
    test_weight_limit();
    return oa::test::check_exit_status();
}

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// A format-1 network file (.oann): named tensors, a list of layers, and the
// integer values of the constants. Reading checks every rule before a model
// is returned. Nothing here opens a file or uses floating point.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::formats::oann {

/// The only format this module reads.
inline constexpr uint16_t format_version = 1;

/// Length of the file header, in bytes.
inline constexpr uint32_t header_bytes = 16;

/// Length of one tensor-table entry, in bytes.
inline constexpr uint32_t tensor_entry_bytes = 64;

/// Length of one layer-table entry, in bytes.
inline constexpr uint32_t layer_entry_bytes = 32;

/// Most tensors one network may name.
inline constexpr uint16_t max_tensors = 4096;

/// Most layers one network may list.
inline constexpr uint16_t max_layers = 1024;

/// Most input tensors, and most output tensors, in one network.
inline constexpr uint16_t max_ports = 16;

/// Largest length of one tensor dimension.
inline constexpr uint32_t max_dimension = 65535;

/// Most elements one tensor may hold, 2^24.
inline constexpr uint32_t max_tensor_elements = 16777216;

/// Most weight elements of the dense, conv2d and embedding layers together.
inline constexpr uint32_t max_weights = 8388608;

/// Longest file, 64 MiB.
inline constexpr uint32_t max_file_bytes = 67108864;

/// Lowest stored exponent. A stored q stands for q times 2^exponent.
inline constexpr int8_t min_exponent = -48;

/// Highest stored exponent.
inline constexpr int8_t max_exponent = 16;

/// The tensor index that means "no tensor" in an unused slot.
inline constexpr uint16_t no_tensor = 0xFFFF;

/// What a tensor is for in the network.
enum class Role : uint8_t {
    input = 1,      ///< given to the network; int16; never written
    output = 2,     ///< produced by the network; int16, or int32 for argmax
    activation = 3, ///< written by one layer for a later layer; int16
    constant = 4,   ///< weights, a bias or an embedding table, kept in the file
};

/// The integer stored for one element.
enum class ElementType : uint8_t {
    int8 = 1,  ///< one byte
    int16 = 2, ///< two bytes, little-endian
    int32 = 3, ///< four bytes, little-endian
};

/// One layer of format 1.
enum class LayerKind : uint8_t {
    dense = 1,
    conv2d = 2,
    maxpool = 3,
    relu = 4,
    leaky_relu = 5,
    tanh = 6,
    sigmoid = 7,
    add = 8,
    concat = 9,
    flatten = 10,
    softmax = 11,
    argmax = 12,
    embedding = 13,
};

/// One named tensor.
///
/// `data` is a constant's little-endian elements, with no padding of its own.
/// Every other role leaves it empty.
struct Tensor {
    std::string name;               ///< 1 to 31 bytes: a letter, then a-z, 0-9 and _
    Role role{};                    ///< input, output, activation or constant
    ElementType type{};             ///< int8, int16 or int32
    int8_t exponent{};              ///< q stands for q times 2^exponent
    uint8_t rank{};                 ///< 1 to 4
    std::array<uint32_t, 4> dims{}; ///< the first rank are the shape; the rest are 0
    std::vector<uint8_t> data;      ///< a constant's elements; empty otherwise
};

/// One layer, in the order the network runs.
struct Layer {
    LayerKind kind{};                 ///< which of the thirteen layers
    uint8_t input_count{};            ///< how many of `inputs` are used
    std::array<uint16_t, 4> inputs{}; ///< tensor indexes; unused slots are no_tensor
    uint16_t output{};                ///< tensor this layer writes
    uint16_t weights{};               ///< a constant, or no_tensor
    uint16_t bias{};                  ///< a constant, or no_tensor
    uint8_t kernel_h{};               ///< conv2d and maxpool: 1 to 15; otherwise 0
    uint8_t kernel_w{};               ///< as kernel_h
    uint8_t stride_h{};               ///< conv2d and maxpool: 1 to 15; otherwise 0
    uint8_t stride_w{};               ///< as stride_h
    uint8_t pad_h{};                  ///< conv2d: 0 to kernel_h - 1; otherwise 0
    uint8_t pad_w{};                  ///< conv2d: 0 to kernel_w - 1; otherwise 0
    uint8_t axis{};                   ///< concat: 0 to rank - 1; otherwise 0
    int16_t alpha{};                  ///< leaky_relu slope in 32768ths, 0 to 32767; otherwise 0
};

/// A network that has been read, or one a caller built to be written.
struct Model {
    std::vector<Tensor> tensors; ///< tensor table, in file order
    std::vector<Layer> layers;   ///< layer table; layers run in this order
};

/// One rule a file or a model broke.
struct Problem {
    std::string message; ///< which entry, and what is wrong
    uint32_t offset{};   ///< file offset of the field, or where it would be written
};

/// Reads a network and checks every rule.
///
/// The span is the whole file. A refusal does not read past it. `problem` is
/// set only when nothing is returned.
///
/// @param bytes the file
/// @param[out] problem the broken rule, when the file is refused
/// @return the network, or nothing when a rule fails
[[nodiscard]] std::optional<Model> read(std::span<const uint8_t> bytes, Problem& problem);

/// Reports the first rule a model breaks.
///
/// The order is the order the module's README lists the rules. The offset is
/// where the field would sit in the written file.
///
/// @param model the network
/// @return the broken rule, or nothing when every rule holds
[[nodiscard]] std::optional<Problem> check(const Model& model);

/// Checks a model and writes its one byte form.
///
/// A model check refuses is not written. `problem` is set only then.
///
/// @param model the network
/// @param[out] problem the broken rule, when nothing is written
/// @return the file, or nothing when a rule fails
[[nodiscard]] std::optional<std::vector<uint8_t>> write(const Model& model, Problem& problem);

/// Counts the elements of a tensor.
///
/// This is the product of the first rank dimensions. The count is 0 when the
/// rank is not 1 to 4, and the largest 64-bit value when the product would
/// pass it.
///
/// @param tensor the tensor
/// @return the element count
[[nodiscard]] inline uint64_t element_count(const Tensor& tensor) noexcept {
    if (tensor.rank < 1 || tensor.rank > 4)
        return 0;
    uint64_t count = 1;
    for (uint8_t axis = 0; axis < tensor.rank; ++axis) {
        const uint64_t dim = tensor.dims[axis];
        if (dim != 0 && count > ~uint64_t{0} / dim)
            return ~uint64_t{0};
        count *= dim;
    }
    return count;
}

/// Returns the size of one element.
///
/// @param type the element type
/// @return 1, 2 or 4, or 0 when the type is not one of those
[[nodiscard]] inline uint32_t element_bytes(ElementType type) noexcept {
    switch (type) {
    case ElementType::int8:
        return 1;
    case ElementType::int16:
        return 2;
    case ElementType::int32:
        return 4;
    }
    return 0;
}

/// Returns the file's name of a layer kind.
///
/// @param kind the kind
/// @return "dense" through "embedding", or an empty view when the kind is not one of them
[[nodiscard]] inline std::string_view kind_name(LayerKind kind) noexcept {
    switch (kind) {
    case LayerKind::dense:
        return "dense";
    case LayerKind::conv2d:
        return "conv2d";
    case LayerKind::maxpool:
        return "maxpool";
    case LayerKind::relu:
        return "relu";
    case LayerKind::leaky_relu:
        return "leaky_relu";
    case LayerKind::tanh:
        return "tanh";
    case LayerKind::sigmoid:
        return "sigmoid";
    case LayerKind::add:
        return "add";
    case LayerKind::concat:
        return "concat";
    case LayerKind::flatten:
        return "flatten";
    case LayerKind::softmax:
        return "softmax";
    case LayerKind::argmax:
        return "argmax";
    case LayerKind::embedding:
        return "embedding";
    }
    return {};
}

/// Reads a layer kind's name.
///
/// @param name "dense" through "embedding", compared byte for byte
/// @return the kind, or nothing when the name is not one of them
[[nodiscard]] inline std::optional<LayerKind> parse_kind(std::string_view name) noexcept {
    for (uint8_t index = 1; index <= static_cast<uint8_t>(LayerKind::embedding); ++index) {
        const auto kind = static_cast<LayerKind>(index);
        if (kind_name(kind) == name)
            return kind;
    }
    return std::nullopt;
}

/// Prints a problem as `byte <offset>: <message>`.
///
/// @param problem the broken rule
/// @return that one line, without a line break
[[nodiscard]] inline std::string describe(const Problem& problem) {
    return "byte " + std::to_string(problem.offset) + ": " + problem.message;
}

} // namespace oa::formats::oann

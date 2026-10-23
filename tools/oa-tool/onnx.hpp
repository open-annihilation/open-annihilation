// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A reader for the ONNX messages oa-tool converts. The bytes are the
// protocol-buffer wire format. Nothing here links a protobuf library.

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace oa::tool {

/// Longest ONNX file read_onnx accepts, 256 MiB.
inline constexpr std::size_t max_onnx_bytes = std::size_t{256} * 1024 * 1024;

/// Deepest nesting of messages read_onnx accepts.
inline constexpr int max_onnx_depth = 16;

/// One dimension of a model input or output.
///
/// A numbered dimension sets `value`. A named dimension, such as the batch,
/// sets `param` and leaves `value` empty.
struct OnnxDim {
    std::optional<int64_t> value; ///< the length, when the dimension is a number
    std::string param;            ///< the name, when the dimension is not a number
};

/// One input or output of the graph.
struct OnnxValue {
    std::string name;          ///< the tensor's name in the model
    int32_t elem_type{};       ///< 1 float, 6 int32, 7 int64; 0 when the model omits it
    std::vector<OnnxDim> dims; ///< the shape, batch dimension included
};

/// One constant tensor.
///
/// Float tensors fill `data`. Int32 and int64 tensors fill `ints` and leave
/// `data` empty. An external tensor is refused before a model is returned.
struct OnnxTensor {
    std::string name;          ///< the tensor's name; empty when the model omits it
    std::vector<int64_t> dims; ///< the shape, with no batch dimension added
    int32_t data_type{};       ///< 1 float, 6 int32, 7 int64
    std::vector<float> data;   ///< float elements, in row-major order
    std::vector<int64_t> ints; ///< int32 or int64 elements, in row-major order
};

/// One attribute of a node.
///
/// Only the fields the model writes are set. A list that the model omits is
/// empty, and a singular value it omits is empty.
struct OnnxAttribute {
    std::string name;                 ///< the attribute's name
    std::optional<float> f;           ///< a single float
    std::optional<int64_t> i;         ///< a single integer
    std::optional<std::string> s;     ///< a single string
    std::optional<OnnxTensor> tensor; ///< a single tensor
    std::vector<float> floats;        ///< a list of floats
    std::vector<int64_t> ints;        ///< a list of integers
};

/// One node of the graph, in the model's order.
struct OnnxNode {
    std::string name;                 ///< the node's name; may be empty
    std::string op_type;              ///< the operator, such as "Gemm"
    std::string domain;               ///< empty or "ai.onnx" for the default domain
    std::vector<std::string> inputs;  ///< tensors the node reads, including an empty omitted one
    std::vector<std::string> outputs; ///< tensors the node writes
    std::vector<OnnxAttribute> attributes; ///< the node's attributes
};

/// One imported operator set.
struct OnnxOpset {
    std::string domain; ///< empty or "ai.onnx" for the default domain
    int64_t version{};  ///< the opset version
};

/// An ONNX model reduced to the messages the converter reads.
struct OnnxModel {
    int64_t ir_version{};                 ///< the file's ir_version; 0 when it is omitted
    std::vector<OnnxOpset> opsets;        ///< the imported operator sets
    std::vector<OnnxNode> nodes;          ///< the graph's nodes, in file order
    std::vector<OnnxTensor> initializers; ///< the graph's constants
    std::vector<OnnxValue> inputs;        ///< the graph's inputs
    std::vector<OnnxValue> outputs;       ///< the graph's outputs
};

/// Reads an ONNX model from the protocol-buffer wire format.
///
/// The file is at most 256 MiB. A message nested more than 16 deep is
/// refused, a varint longer than 10 bytes is refused, and a length that runs
/// past the bytes that remain is refused. An unknown field is skipped by its
/// wire type. A group, wire type 3 or 4, is refused. A tensor whose data lives
/// in another file is refused. `problem` is set only when nothing is returned.
///
/// @param bytes the file
/// @param[out] problem the broken rule, when the file is refused
/// @return the model, or nothing when a rule fails
[[nodiscard]] std::optional<OnnxModel>
read_onnx(std::span<const uint8_t> bytes, std::string& problem);

} // namespace oa::tool

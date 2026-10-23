// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The ONNX wire reader. A file is one protocol-buffer message. Field numbers
// are the ones ONNX 1.16 uses; the words of that schema are not copied here.
// Attribute field 6 is a nested graph. It is walked only so a message nested
// too deep is refused, and the nodes inside it are not imported.

#include "onnx.hpp"

#include <cstdint>
#include <cstring>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace oa::tool {
namespace {

/// One cursor over a message, and the reason a file was refused.
class Reader {
  public:

    /// Binds a cursor to one message.
    ///
    /// @param bytes the message
    /// @param[out] problem where a refusal is written
    Reader(std::span<const uint8_t> bytes, std::string& problem)
        : bytes_(bytes), problem_(&problem) {}

    /// Reads a model message.
    ///
    /// @param[out] model the model
    /// @param depth how deep this message is, counting the model as 1
    /// @return false when the message is refused
    bool parse_model(OnnxModel& model, int depth) {
        if (!enter(depth))
            return false;
        while (!done()) {
            uint64_t field = 0;
            uint32_t wire = 0;
            if (!key(field, wire))
                return false;
            if (field == 1 && wire == 0) {
                uint64_t version = 0;
                if (!varint(version))
                    return false;
                model.ir_version = static_cast<int64_t>(version);
            } else if (field == 7 && wire == 2) {
                Reader nested;
                if (!slice(nested))
                    return false;
                if (!nested.parse_graph(model, depth + 1))
                    return refuse_from(nested);
            } else if (field == 8 && wire == 2) {
                Reader nested;
                if (!slice(nested))
                    return false;
                OnnxOpset opset;
                if (!nested.parse_opset(opset, depth + 1))
                    return refuse_from(nested);
                model.opsets.push_back(std::move(opset));
            } else if (!skip(wire)) {
                return false;
            }
        }
        return true;
    }

    /// Reads a graph message into a model, or into a graph that is discarded.
    ///
    /// @param[out] model receives the graph's nodes, constants, inputs and outputs
    /// @param depth how deep this message is
    /// @return false when the message is refused
    bool parse_graph(OnnxModel& model, int depth) {
        if (!enter(depth))
            return false;
        while (!done()) {
            uint64_t field = 0;
            uint32_t wire = 0;
            if (!key(field, wire))
                return false;
            if (field == 1 && wire == 2) {
                Reader nested;
                if (!slice(nested))
                    return false;
                OnnxNode node;
                if (!nested.parse_node(node, depth + 1))
                    return refuse_from(nested);
                model.nodes.push_back(std::move(node));
            } else if (field == 5 && wire == 2) {
                Reader nested;
                if (!slice(nested))
                    return false;
                OnnxTensor tensor;
                if (!nested.parse_tensor(tensor, depth + 1))
                    return refuse_from(nested);
                model.initializers.push_back(std::move(tensor));
            } else if (field == 11 && wire == 2) {
                Reader nested;
                if (!slice(nested))
                    return false;
                OnnxValue value;
                if (!nested.parse_value(value, depth + 1))
                    return refuse_from(nested);
                model.inputs.push_back(std::move(value));
            } else if (field == 12 && wire == 2) {
                Reader nested;
                if (!slice(nested))
                    return false;
                OnnxValue value;
                if (!nested.parse_value(value, depth + 1))
                    return refuse_from(nested);
                model.outputs.push_back(std::move(value));
            } else if (!skip(wire)) {
                return false;
            }
        }
        return true;
    }

  private:

    Reader() = default;

    /// Refuses a message nested deeper than the limit.
    ///
    /// @param depth how deep this message is
    /// @return false when the depth is past the limit
    bool enter(int depth) {
        if (depth > max_onnx_depth)
            return fail("a message nests " + std::to_string(depth) + " deep");
        return true;
    }

    /// Reports whether the cursor is at the end of this message.
    ///
    /// @return true when no byte remains
    bool done() const { return at_ >= bytes_.size(); }

    /// Records a refusal.
    ///
    /// @param text the reason
    /// @return false
    bool fail(std::string text) {
        if (problem_ != nullptr && problem_->empty())
            *problem_ = std::move(text);
        return false;
    }

    /// Copies a nested reader's refusal.
    ///
    /// @param nested the reader that refused
    /// @return false
    bool refuse_from(const Reader& nested) {
        if (problem_ != nullptr && problem_->empty() && nested.problem_ != nullptr)
            *problem_ = *nested.problem_;
        return false;
    }

    /// Reads a field key.
    ///
    /// @param[out] field the field number
    /// @param[out] wire the wire type
    /// @return false when the key is not one this reader accepts
    bool key(uint64_t& field, uint32_t& wire) {
        uint64_t tag = 0;
        if (!varint(tag))
            return false;
        wire = static_cast<uint32_t>(tag & 7U);
        field = tag >> 3U;
        if (field == 0)
            return fail("a field number is 0");
        if (wire == 3 || wire == 4)
            return fail(
                "a group (wire type " + std::to_string(wire) +
                ") is not a field this reader accepts"
            );
        if (wire > 5)
            return fail("a field's wire type is " + std::to_string(wire));
        return true;
    }

    /// Reads a varint of at most 10 bytes.
    ///
    /// The 10th byte has one significant bit. A continuation there, or a
    /// wider value, is an 11th byte and is refused.
    ///
    /// @param[out] value the bits of the varint
    /// @return false when the varint is cut off or too long
    bool varint(uint64_t& value) {
        value = 0;
        for (int index = 0; index < 10; ++index) {
            if (at_ >= bytes_.size())
                return fail("a varint ended early");
            const uint8_t piece = bytes_[at_++];
            if (index == 9 && ((piece & 0x80U) != 0 || (piece & 0x7fU) > 1))
                return fail("a varint is longer than 10 bytes");
            value |= static_cast<uint64_t>(piece & 0x7fU) << static_cast<unsigned>(index * 7);
            if ((piece & 0x80U) == 0)
                return true;
        }
        return fail("a varint is longer than 10 bytes");
    }

    /// Takes a fixed run of bytes.
    ///
    /// @param count how many bytes
    /// @param[out] out those bytes
    /// @return false when the message ends first
    bool take(std::size_t count, std::span<const uint8_t>& out) {
        if (count > bytes_.size() - at_)
            return fail("the message ended inside a field");
        out = bytes_.subspan(at_, count);
        at_ += count;
        return true;
    }

    /// Opens a length-delimited slice as its own reader.
    ///
    /// @param[out] nested a reader over the slice, sharing the refusal text
    /// @return false when the length runs past the bytes that remain
    bool slice(Reader& nested) {
        uint64_t length = 0;
        if (!varint(length))
            return false;
        if (length > bytes_.size() - at_)
            return fail("a length runs past the bytes that remain");
        nested.bytes_ = bytes_.subspan(at_, static_cast<std::size_t>(length));
        nested.at_ = 0;
        nested.problem_ = problem_;
        at_ += static_cast<std::size_t>(length);
        return true;
    }

    /// Skips a field this reader does not store.
    ///
    /// A length-delimited field is skipped as bytes. That does not nest, so a
    /// packed list does not count toward the depth.
    ///
    /// @param wire the field's wire type
    /// @return false when the field is cut off
    bool skip(uint32_t wire) {
        if (wire == 0) {
            uint64_t ignored = 0;
            return varint(ignored);
        }
        if (wire == 1) {
            std::span<const uint8_t> ignored;
            return take(8, ignored);
        }
        if (wire == 5) {
            std::span<const uint8_t> ignored;
            return take(4, ignored);
        }
        if (wire == 2) {
            Reader nested;
            return slice(nested);
        }
        return fail("a field's wire type is " + std::to_string(wire));
    }

    /// Appends one integer from a varint or a packed run of varints.
    ///
    /// @param wire the field's wire type
    /// @param[out] values the integers so far
    /// @return false when the field is cut off
    bool take_varints(uint32_t wire, std::vector<int64_t>& values) {
        if (wire == 0) {
            uint64_t value = 0;
            if (!varint(value))
                return false;
            values.push_back(static_cast<int64_t>(value));
            return true;
        }
        if (wire != 2)
            return skip(wire);
        Reader packed;
        if (!slice(packed))
            return false;
        while (!packed.done()) {
            uint64_t value = 0;
            if (!packed.varint(value))
                return refuse_from(packed);
            values.push_back(static_cast<int64_t>(value));
        }
        return true;
    }

    /// Appends floats from a 32-bit field or a packed run of them.
    ///
    /// @param wire the field's wire type
    /// @param[out] values the floats so far
    /// @return false when the field is cut off or its length is not a multiple of 4
    bool take_floats(uint32_t wire, std::vector<float>& values) {
        if (wire == 5) {
            std::span<const uint8_t> bytes;
            if (!take(4, bytes))
                return false;
            values.push_back(load_f32(bytes.data()));
            return true;
        }
        if (wire != 2)
            return skip(wire);
        Reader packed;
        if (!slice(packed))
            return false;
        if (packed.bytes_.size() % 4 != 0) {
            return fail(
                "a packed field is " + std::to_string(packed.bytes_.size()) +
                " bytes, and float elements are 4"
            );
        }
        for (std::size_t index = 0; index < packed.bytes_.size(); index += 4)
            values.push_back(load_f32(packed.bytes_.data() + index));
        return true;
    }

    /// Reads a string field.
    ///
    /// @param wire the field's wire type
    /// @param[out] text the bytes of the string
    /// @return false when the field is cut off
    bool take_string(uint32_t wire, std::string& text) {
        if (wire != 2)
            return skip(wire);
        Reader nested;
        if (!slice(nested))
            return false;
        text.assign(reinterpret_cast<const char*>(nested.bytes_.data()), nested.bytes_.size());
        return true;
    }

    /// Reads one imported operator set.
    ///
    /// @param[out] opset the operator set
    /// @param depth how deep this message is
    /// @return false when the message is refused
    bool parse_opset(OnnxOpset& opset, int depth) {
        if (!enter(depth))
            return false;
        while (!done()) {
            uint64_t field = 0;
            uint32_t wire = 0;
            if (!key(field, wire))
                return false;
            if (field == 1 && wire == 2) {
                if (!take_string(wire, opset.domain))
                    return false;
            } else if (field == 2 && wire == 0) {
                uint64_t version = 0;
                if (!varint(version))
                    return false;
                opset.version = static_cast<int64_t>(version);
            } else if (!skip(wire)) {
                return false;
            }
        }
        return true;
    }

    /// Reads one node.
    ///
    /// @param[out] node the node
    /// @param depth how deep this message is
    /// @return false when the message is refused
    bool parse_node(OnnxNode& node, int depth) {
        if (!enter(depth))
            return false;
        while (!done()) {
            uint64_t field = 0;
            uint32_t wire = 0;
            if (!key(field, wire))
                return false;
            if (field == 1 && wire == 2) {
                std::string input;
                if (!take_string(wire, input))
                    return false;
                node.inputs.push_back(std::move(input));
            } else if (field == 2 && wire == 2) {
                std::string output;
                if (!take_string(wire, output))
                    return false;
                node.outputs.push_back(std::move(output));
            } else if (field == 3 && wire == 2) {
                if (!take_string(wire, node.name))
                    return false;
            } else if (field == 4 && wire == 2) {
                if (!take_string(wire, node.op_type))
                    return false;
            } else if (field == 5 && wire == 2) {
                Reader nested;
                if (!slice(nested))
                    return false;
                OnnxAttribute attribute;
                if (!nested.parse_attribute(attribute, depth + 1))
                    return refuse_from(nested);
                node.attributes.push_back(std::move(attribute));
            } else if (field == 7 && wire == 2) {
                if (!take_string(wire, node.domain))
                    return false;
            } else if (!skip(wire)) {
                return false;
            }
        }
        return true;
    }

    /// Reads one attribute.
    ///
    /// Field 6 is a graph. Parsing it enforces the depth bound. Its nodes are
    /// thrown away: a .oann has no subgraphs.
    ///
    /// @param[out] attribute the attribute
    /// @param depth how deep this message is
    /// @return false when the message is refused
    bool parse_attribute(OnnxAttribute& attribute, int depth) {
        if (!enter(depth))
            return false;
        while (!done()) {
            uint64_t field = 0;
            uint32_t wire = 0;
            if (!key(field, wire))
                return false;
            if (field == 1 && wire == 2) {
                if (!take_string(wire, attribute.name))
                    return false;
            } else if (field == 2) {
                std::vector<float> values;
                if (!take_floats(wire, values))
                    return false;
                if (!values.empty())
                    attribute.f = values.back();
            } else if (field == 3 && wire == 0) {
                uint64_t value = 0;
                if (!varint(value))
                    return false;
                attribute.i = static_cast<int64_t>(value);
            } else if (field == 4 && wire == 2) {
                std::string text;
                if (!take_string(wire, text))
                    return false;
                attribute.s = std::move(text);
            } else if (field == 5 && wire == 2) {
                Reader nested;
                if (!slice(nested))
                    return false;
                OnnxTensor tensor;
                if (!nested.parse_tensor(tensor, depth + 1))
                    return refuse_from(nested);
                attribute.tensor = std::move(tensor);
            } else if (field == 6 && wire == 2) {
                Reader nested;
                if (!slice(nested))
                    return false;
                OnnxModel discarded;
                if (!nested.parse_graph(discarded, depth + 1))
                    return refuse_from(nested);
            } else if (field == 7) {
                if (!take_floats(wire, attribute.floats))
                    return false;
            } else if (field == 8) {
                if (!take_varints(wire, attribute.ints))
                    return false;
            } else if (!skip(wire)) {
                return false;
            }
        }
        return true;
    }

    /// Reads one tensor and refuses one whose data lives in another file.
    ///
    /// @param[out] tensor the tensor
    /// @param depth how deep this message is
    /// @return false when the message is refused
    bool parse_tensor(OnnxTensor& tensor, int depth) {
        if (!enter(depth))
            return false;
        std::vector<float> floats;
        std::vector<int64_t> int32s;
        std::vector<int64_t> int64s;
        std::vector<uint8_t> raw;
        bool external = false;
        while (!done()) {
            uint64_t field = 0;
            uint32_t wire = 0;
            if (!key(field, wire))
                return false;
            if (field == 1) {
                if (!take_varints(wire, tensor.dims))
                    return false;
            } else if (field == 2 && wire == 0) {
                uint64_t data_type = 0;
                if (!varint(data_type))
                    return false;
                tensor.data_type = static_cast<int32_t>(data_type);
            } else if (field == 4) {
                if (!take_floats(wire, floats))
                    return false;
            } else if (field == 5) {
                if (!take_varints(wire, int32s))
                    return false;
            } else if (field == 7) {
                if (!take_varints(wire, int64s))
                    return false;
            } else if (field == 8 && wire == 2) {
                if (!take_string(wire, tensor.name))
                    return false;
            } else if (field == 9 && wire == 2) {
                Reader nested;
                if (!slice(nested))
                    return false;
                raw.assign(nested.bytes_.begin(), nested.bytes_.end());
            } else if (field == 14 && wire == 0) {
                uint64_t location = 0;
                if (!varint(location))
                    return false;
                external = location == 1;
            } else if (!skip(wire)) {
                return false;
            }
        }
        if (external) {
            const std::string& name = tensor.name.empty() ? "unnamed" : tensor.name;
            return fail(
                "tensor " + name +
                " keeps its data in another file; export with the data inside the model"
            );
        }
        return finish_tensor(tensor, floats, int32s, int64s, raw);
    }

    /// Fills a tensor from its raw bytes when those are present, otherwise from
    /// the typed lists, and checks the shape.
    ///
    /// @param[in,out] tensor the tensor, with its name, type and dimensions set
    /// @param floats float elements from the typed list
    /// @param int32s int32 elements from the typed list
    /// @param int64s int64 elements from the typed list
    /// @param raw little-endian elements, used when the list is not empty
    /// @return false when the type, the shape or the byte count does not match
    bool finish_tensor(
        OnnxTensor& tensor,
        const std::vector<float>& floats,
        const std::vector<int64_t>& int32s,
        const std::vector<int64_t>& int64s,
        const std::vector<uint8_t>& raw
    ) {
        const std::string name = tensor.name.empty() ? "unnamed" : tensor.name;
        int64_t count = 1;
        for (const int64_t dim : tensor.dims) {
            if (dim < 0)
                return fail("tensor " + name + ": a dimension is negative");
            if (dim != 0 && count > (INT64_C(0x7fffffffffffffff) / dim))
                return fail("tensor " + name + ": the shape is too large");
            count *= dim;
        }
        const bool have_raw = !raw.empty();
        std::size_t got = 0;
        if (tensor.data_type == 1) {
            if (have_raw) {
                if (raw.size() % 4 != 0)
                    return fail("tensor " + name + ": float data is not a multiple of 4 bytes");
                tensor.data.reserve(raw.size() / 4);
                for (std::size_t index = 0; index < raw.size(); index += 4)
                    tensor.data.push_back(load_f32(raw.data() + index));
            } else {
                tensor.data = floats;
            }
            got = tensor.data.size();
        } else if (tensor.data_type == 6) {
            if (have_raw) {
                if (raw.size() % 4 != 0)
                    return fail("tensor " + name + ": int32 data is not a multiple of 4 bytes");
                tensor.ints.reserve(raw.size() / 4);
                for (std::size_t index = 0; index < raw.size(); index += 4)
                    tensor.ints.push_back(load_i32(raw.data() + index));
            } else {
                tensor.ints = int32s;
            }
            got = tensor.ints.size();
        } else if (tensor.data_type == 7) {
            if (have_raw) {
                if (raw.size() % 8 != 0)
                    return fail("tensor " + name + ": int64 data is not a multiple of 8 bytes");
                tensor.ints.reserve(raw.size() / 8);
                for (std::size_t index = 0; index < raw.size(); index += 8)
                    tensor.ints.push_back(load_i64(raw.data() + index));
            } else {
                tensor.ints = int64s;
            }
            got = tensor.ints.size();
        } else if (have_raw || !floats.empty() || !int32s.empty() || !int64s.empty()) {
            return fail(
                "tensor " + name + ": the data type is " + std::to_string(tensor.data_type)
            );
        }
        if (count > 0 && static_cast<int64_t>(got) != count) {
            return fail(
                "tensor " + name + ": the shape has " + std::to_string(count) +
                " elements and the data has " + std::to_string(got)
            );
        }
        if (count == 0 && got != 0) {
            return fail(
                "tensor " + name + ": the shape has 0 elements and the data has " +
                std::to_string(got)
            );
        }
        return true;
    }

    /// Reads one input or output.
    ///
    /// @param[out] value the input or output
    /// @param depth how deep this message is
    /// @return false when the message is refused
    bool parse_value(OnnxValue& value, int depth) {
        if (!enter(depth))
            return false;
        while (!done()) {
            uint64_t field = 0;
            uint32_t wire = 0;
            if (!key(field, wire))
                return false;
            if (field == 1 && wire == 2) {
                if (!take_string(wire, value.name))
                    return false;
            } else if (field == 2 && wire == 2) {
                Reader nested;
                if (!slice(nested))
                    return false;
                if (!nested.parse_type(value, depth + 1))
                    return refuse_from(nested);
            } else if (!skip(wire)) {
                return false;
            }
        }
        return true;
    }

    /// Reads a type message and, when it holds a tensor type, that type.
    ///
    /// @param[in,out] value receives the element type and the shape
    /// @param depth how deep this message is
    /// @return false when the message is refused
    bool parse_type(OnnxValue& value, int depth) {
        if (!enter(depth))
            return false;
        while (!done()) {
            uint64_t field = 0;
            uint32_t wire = 0;
            if (!key(field, wire))
                return false;
            if (field == 1 && wire == 2) {
                Reader nested;
                if (!slice(nested))
                    return false;
                if (!nested.parse_tensor_type(value, depth + 1))
                    return refuse_from(nested);
            } else if (!skip(wire)) {
                return false;
            }
        }
        return true;
    }

    /// Reads a tensor type.
    ///
    /// @param[in,out] value receives the element type and the shape
    /// @param depth how deep this message is
    /// @return false when the message is refused
    bool parse_tensor_type(OnnxValue& value, int depth) {
        if (!enter(depth))
            return false;
        while (!done()) {
            uint64_t field = 0;
            uint32_t wire = 0;
            if (!key(field, wire))
                return false;
            if (field == 1 && wire == 0) {
                uint64_t elem = 0;
                if (!varint(elem))
                    return false;
                value.elem_type = static_cast<int32_t>(elem);
            } else if (field == 2 && wire == 2) {
                Reader nested;
                if (!slice(nested))
                    return false;
                if (!nested.parse_shape(value, depth + 1))
                    return refuse_from(nested);
            } else if (!skip(wire)) {
                return false;
            }
        }
        return true;
    }

    /// Reads a shape.
    ///
    /// @param[in,out] value receives one dimension per field
    /// @param depth how deep this message is
    /// @return false when the message is refused
    bool parse_shape(OnnxValue& value, int depth) {
        if (!enter(depth))
            return false;
        while (!done()) {
            uint64_t field = 0;
            uint32_t wire = 0;
            if (!key(field, wire))
                return false;
            if (field == 1 && wire == 2) {
                Reader nested;
                if (!slice(nested))
                    return false;
                OnnxDim dim;
                if (!nested.parse_dim(dim, depth + 1))
                    return refuse_from(nested);
                value.dims.push_back(std::move(dim));
            } else if (!skip(wire)) {
                return false;
            }
        }
        return true;
    }

    /// Reads one dimension.
    ///
    /// @param[out] dim the dimension
    /// @param depth how deep this message is
    /// @return false when the message is refused
    bool parse_dim(OnnxDim& dim, int depth) {
        if (!enter(depth))
            return false;
        while (!done()) {
            uint64_t field = 0;
            uint32_t wire = 0;
            if (!key(field, wire))
                return false;
            if (field == 1 && wire == 0) {
                uint64_t length = 0;
                if (!varint(length))
                    return false;
                dim.value = static_cast<int64_t>(length);
            } else if (field == 2 && wire == 2) {
                if (!take_string(wire, dim.param))
                    return false;
            } else if (!skip(wire)) {
                return false;
            }
        }
        return true;
    }

    /// Reads a little-endian float.
    ///
    /// @param bytes four bytes
    /// @return the float
    static float load_f32(const uint8_t* bytes) {
        const uint32_t bits =
            static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8U) |
            (static_cast<uint32_t>(bytes[2]) << 16U) | (static_cast<uint32_t>(bytes[3]) << 24U);
        float value = 0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    /// Reads a little-endian int32, widened to int64.
    ///
    /// @param bytes four bytes
    /// @return the integer
    static int64_t load_i32(const uint8_t* bytes) {
        const uint32_t bits =
            static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8U) |
            (static_cast<uint32_t>(bytes[2]) << 16U) | (static_cast<uint32_t>(bytes[3]) << 24U);
        return static_cast<int32_t>(bits);
    }

    /// Reads a little-endian int64.
    ///
    /// @param bytes eight bytes
    /// @return the integer
    static int64_t load_i64(const uint8_t* bytes) {
        uint64_t bits = 0;
        for (int index = 0; index < 8; ++index)
            bits |= static_cast<uint64_t>(bytes[index]) << static_cast<unsigned>(index * 8);
        return static_cast<int64_t>(bits);
    }

    std::span<const uint8_t> bytes_{};
    std::size_t at_{};
    std::string* problem_ = nullptr;
};

} // namespace

std::optional<OnnxModel> read_onnx(std::span<const uint8_t> bytes, std::string& problem) {
    problem.clear();
    if (bytes.size() > max_onnx_bytes) {
        problem = "the file is larger than 256 MiB";
        return std::nullopt;
    }
    // An empty buffer is a prefix of every file. A truncation of every length
    // has to be refused, and an empty message would otherwise be well formed.
    if (bytes.empty()) {
        problem = "the message ended inside a field";
        return std::nullopt;
    }
    Reader reader(bytes, problem);
    OnnxModel model;
    if (!reader.parse_model(model, 1))
        return std::nullopt;
    return model;
}

} // namespace oa::tool

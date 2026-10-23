// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Reads one format-1 network from a byte span. Counts, offsets and lengths
// are checked against the bytes that remain before any constant is copied.

#include "oa/formats/oann/oann.hpp"

#include "coding.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace oa::formats::oann {
namespace {

using coding::layer_field;
using coding::load_u16;
using coding::load_u32;
using coding::signed_byte;
using coding::signed_word;
using coding::tensor_field;

/// Sets the problem and reports that reading stopped.
///
/// @param problem the problem to set
/// @param offset file offset of the field
/// @param message what is wrong
/// @return nothing
std::optional<Model> refuse(Problem& problem, uint32_t offset, std::string message) {
    problem = Problem{std::move(message), offset};
    return std::nullopt;
}

/// Reports a header the file does not contain.
///
/// @param bytes how many bytes the file has
/// @param problem the problem to set
/// @return nothing
std::optional<Model> short_header(std::size_t bytes, Problem& problem) {
    uint32_t offset = 0;
    const char* field = "magic";
    if (bytes >= 12) {
        offset = 12;
        field = "data length";
    } else if (bytes >= 10) {
        offset = 10;
        field = "layer count";
    } else if (bytes >= 8) {
        offset = 8;
        field = "tensor count";
    } else if (bytes >= 6) {
        offset = 6;
        field = "flags";
    } else if (bytes >= 4) {
        offset = 4;
        field = "format";
    }
    return refuse(problem, offset, std::string("the file ends before the ") + field);
}

/// Returns whether a name byte is a letter, a digit or an underscore.
///
/// @param byte the byte
/// @return true when the byte may appear in a name
bool name_byte(uint8_t byte) noexcept {
    return (byte >= 'a' && byte <= 'z') || (byte >= '0' && byte <= '9') || byte == '_';
}

/// Reads a 32-byte name into the tensor.
///
/// @param field the 32 bytes
/// @param index the tensor's place
/// @param problem the problem to set on failure
/// @param[out] name the name, when it is well formed
/// @return false when the field is refused
bool read_name(const uint8_t* field, std::size_t index, Problem& problem, std::string& name) {
    const uint32_t origin = tensor_field(index, 0);
    std::size_t length = 0;
    while (length < 32 && field[length] != 0)
        ++length;
    if (length == 32) {
        problem = Problem{
            "tensor " + std::to_string(index) +
                ": the name fills its 32 bytes, and must end with a zero",
            origin
        };
        return false;
    }
    for (std::size_t byte = 0; byte < length; ++byte) {
        if (!name_byte(field[byte])) {
            problem = Problem{
                "tensor " + std::to_string(index) + ": the name has a byte outside a-z, 0-9 and _",
                origin + static_cast<uint32_t>(byte)
            };
            return false;
        }
    }
    for (std::size_t byte = length + 1; byte < 32; ++byte) {
        if (field[byte] != 0) {
            problem = Problem{
                "tensor " + std::to_string(index) + ": a byte after the name is " +
                    std::to_string(field[byte]) + ", and must be 0",
                origin + static_cast<uint32_t>(byte)
            };
            return false;
        }
    }
    if (length == 0) {
        problem = Problem{"tensor " + std::to_string(index) + ": the name is empty", origin};
        return false;
    }
    name.assign(length, '\0');
    for (std::size_t byte = 0; byte < length; ++byte)
        name[byte] = static_cast<char>(field[byte]);
    return true;
}

/// Copies one constant after its offset and length are inside the data block.
///
/// @param data the data block
/// @param data_origin file offset of the data block
/// @param[in,out] cursor the next canonical constant offset
/// @param tensor the constant
/// @param index the tensor's place
/// @param stored_offset the offset field
/// @param stored_bytes the length field
/// @param problem the problem to set on failure
/// @return false when the constant is out of place or the padding is not zero
bool take_constant(
    std::span<const uint8_t> data,
    uint32_t data_origin,
    uint64_t& cursor,
    Tensor& tensor,
    std::size_t index,
    uint32_t stored_offset,
    uint32_t stored_bytes,
    Problem& problem
) {
    const std::string label = "tensor " + std::to_string(index) + " (" + tensor.name + ")";
    if (stored_offset != cursor) {
        problem = Problem{
            label + ": the constant starts at " + std::to_string(stored_offset) +
                ", and its place is " + std::to_string(cursor),
            tensor_field(index, 52)
        };
        return false;
    }
    const uint64_t end = cursor + stored_bytes;
    if (end < cursor || end > data.size()) {
        problem = Problem{
            label + ": the constant runs to " + std::to_string(end) + ", past the data of " +
                std::to_string(data.size()) + " bytes",
            tensor_field(index, 56)
        };
        return false;
    }
    const uint64_t aligned = (end + 3U) & ~uint64_t{3};
    if (aligned > data.size()) {
        problem = Problem{
            label + ": the constant's padding runs to " + std::to_string(aligned) +
                ", past the data of " + std::to_string(data.size()) + " bytes",
            tensor_field(index, 56)
        };
        return false;
    }
    // The length is inside the block, so the copy cannot pass the span.
    tensor.data.assign(
        data.begin() + static_cast<std::size_t>(stored_offset),
        data.begin() + static_cast<std::size_t>(end)
    );
    for (uint64_t pad = end; pad < aligned; ++pad) {
        if (data[static_cast<std::size_t>(pad)] != 0) {
            problem = Problem{
                "a padding byte is " + std::to_string(data[static_cast<std::size_t>(pad)]) +
                    ", and must be 0",
                data_origin + static_cast<uint32_t>(pad)
            };
            return false;
        }
    }
    cursor = aligned;
    return true;
}

} // namespace

std::optional<Model> read(std::span<const uint8_t> bytes, Problem& problem) {
    if (bytes.size() > max_file_bytes) {
        return refuse(
            problem,
            0,
            "the file is " + std::to_string(bytes.size()) + " bytes, and the limit is " +
                std::to_string(max_file_bytes)
        );
    }
    if (bytes.size() < header_bytes)
        return short_header(bytes.size(), problem);

    const uint8_t* header = bytes.data();
    if (header[0] != 'O' || header[1] != 'A' || header[2] != 'N' || header[3] != 'N')
        return refuse(problem, 0, "the magic is not OANN");

    const uint16_t format = load_u16(header + 4);
    if (format != format_version) {
        return refuse(
            problem,
            4,
            "the format is " + std::to_string(format) + ", and must be " +
                std::to_string(format_version)
        );
    }
    const uint16_t flags = load_u16(header + 6);
    if (flags != 0) {
        return refuse(problem, 6, "the flags are " + std::to_string(flags) + ", and must be 0");
    }

    const uint16_t tensor_count = load_u16(header + 8);
    if (tensor_count < 2 || tensor_count > max_tensors) {
        return refuse(
            problem,
            8,
            "the tensor count is " + std::to_string(tensor_count) + ", and must be 2 to " +
                std::to_string(max_tensors)
        );
    }
    const uint16_t layer_count = load_u16(header + 10);
    if (layer_count < 1 || layer_count > max_layers) {
        return refuse(
            problem,
            10,
            "the layer count is " + std::to_string(layer_count) + ", and must be 1 to " +
                std::to_string(max_layers)
        );
    }
    const uint32_t data_bytes = load_u32(header + 12);
    const uint64_t declared = uint64_t{header_bytes} + uint64_t{tensor_entry_bytes} * tensor_count +
                              uint64_t{layer_entry_bytes} * layer_count + data_bytes;
    if (declared > max_file_bytes) {
        return refuse(
            problem,
            12,
            "the network declares " + std::to_string(declared) + " bytes, and the limit is " +
                std::to_string(max_file_bytes)
        );
    }
    if (bytes.size() < declared) {
        return refuse(
            problem,
            static_cast<uint32_t>(bytes.size()),
            "the file has " + std::to_string(bytes.size()) + " bytes, and the network needs " +
                std::to_string(declared)
        );
    }
    if (bytes.size() > declared) {
        return refuse(
            problem,
            static_cast<uint32_t>(declared),
            "the file has " + std::to_string(bytes.size()) + " bytes, and the network ends at " +
                std::to_string(declared)
        );
    }

    Model model;
    model.tensors.resize(tensor_count);
    model.layers.resize(layer_count);

    const uint32_t data_origin = static_cast<uint32_t>(
        header_bytes + uint64_t{tensor_entry_bytes} * tensor_count +
        uint64_t{layer_entry_bytes} * layer_count
    );
    uint64_t cursor = 0;
    for (std::size_t index = 0; index < tensor_count; ++index) {
        const uint8_t* entry = bytes.data() + tensor_field(index, 0);
        Tensor& tensor = model.tensors[index];
        if (!read_name(entry, index, problem, tensor.name))
            return std::nullopt;
        tensor.role = static_cast<Role>(entry[32]);
        tensor.type = static_cast<ElementType>(entry[33]);
        tensor.exponent = signed_byte(entry[34]);
        tensor.rank = entry[35];
        for (uint8_t axis = 0; axis < 4; ++axis)
            tensor.dims[axis] = load_u32(entry + 36 + axis * 4U);
        const uint32_t stored_offset = load_u32(entry + 52);
        const uint32_t stored_bytes = load_u32(entry + 56);
        const uint32_t reserved = load_u32(entry + 60);
        if (reserved != 0) {
            return refuse(
                problem,
                tensor_field(index, 60),
                "tensor " + std::to_string(index) + " (" + tensor.name +
                    "): the reserved bytes are not zero"
            );
        }
        if (tensor.role != Role::constant) {
            if (stored_offset != 0) {
                return refuse(
                    problem,
                    tensor_field(index, 52),
                    "tensor " + std::to_string(index) + " (" + tensor.name + "): the offset is " +
                        std::to_string(stored_offset) +
                        ", and must be 0 when the tensor is not a constant"
                );
            }
            if (stored_bytes != 0) {
                return refuse(
                    problem,
                    tensor_field(index, 56),
                    "tensor " + std::to_string(index) + " (" + tensor.name + "): the length is " +
                        std::to_string(stored_bytes) +
                        ", and must be 0 when the tensor is not a constant"
                );
            }
            continue;
        }
        const auto data = bytes.subspan(data_origin, data_bytes);
        if (!take_constant(
                data, data_origin, cursor, tensor, index, stored_offset, stored_bytes, problem
            ))
            return std::nullopt;
    }
    if (cursor != data_bytes) {
        return refuse(
            problem,
            data_origin + static_cast<uint32_t>(cursor),
            "the data block is " + std::to_string(data_bytes) +
                " bytes, and the constants end at " + std::to_string(cursor)
        );
    }

    for (std::size_t index = 0; index < layer_count; ++index) {
        const uint8_t* entry = bytes.data() + layer_field(tensor_count, index, 0);
        Layer& layer = model.layers[index];
        layer.kind = static_cast<LayerKind>(entry[0]);
        layer.input_count = entry[1];
        for (uint8_t slot = 0; slot < 4; ++slot)
            layer.inputs[slot] = load_u16(entry + 2 + slot * 2U);
        layer.output = load_u16(entry + 10);
        layer.weights = load_u16(entry + 12);
        layer.bias = load_u16(entry + 14);
        layer.kernel_h = entry[16];
        layer.kernel_w = entry[17];
        layer.stride_h = entry[18];
        layer.stride_w = entry[19];
        layer.pad_h = entry[20];
        layer.pad_w = entry[21];
        layer.axis = entry[22];
        if (entry[23] != 0) {
            return refuse(
                problem,
                layer_field(tensor_count, index, 23),
                "layer " + std::to_string(index) + ": the reserved byte is " +
                    std::to_string(entry[23]) + ", and must be 0"
            );
        }
        layer.alpha = signed_word(load_u16(entry + 24));
        for (uint8_t reserved_byte = 0; reserved_byte < 6; ++reserved_byte) {
            if (entry[26 + reserved_byte] != 0) {
                return refuse(
                    problem,
                    layer_field(tensor_count, index, 26 + reserved_byte),
                    "layer " + std::to_string(index) + ": a reserved byte is " +
                        std::to_string(entry[26 + reserved_byte]) + ", and must be 0"
                );
            }
        }
    }

    if (const std::optional<Problem> found = check(model)) {
        problem = *found;
        return std::nullopt;
    }
    return model;
}

} // namespace oa::formats::oann

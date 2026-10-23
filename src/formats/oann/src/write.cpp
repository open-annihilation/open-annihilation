// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Writes a checked model in format 1's one byte form: header, tensor table,
// layer table, then constants in table order padded to a multiple of 4.

#include "oa/formats/oann/oann.hpp"

#include "coding.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

namespace oa::formats::oann {
namespace {

using coding::layer_field;
using coding::store_u16;
using coding::store_u32;
using coding::tensor_field;

/// Appends the constants in tensor-table order, with zero padding.
///
/// @param model the network, already checked
/// @return the data block
std::vector<uint8_t> data_block(const Model& model) {
    std::vector<uint8_t> data;
    for (const Tensor& tensor : model.tensors) {
        if (tensor.role != Role::constant)
            continue;
        while ((data.size() & 3U) != 0)
            data.push_back(0);
        data.insert(data.end(), tensor.data.begin(), tensor.data.end());
    }
    while ((data.size() & 3U) != 0)
        data.push_back(0);
    return data;
}

} // namespace

std::optional<std::vector<uint8_t>> write(const Model& model, Problem& problem) {
    if (const std::optional<Problem> found = check(model)) {
        problem = *found;
        return std::nullopt;
    }

    const std::vector<uint8_t> data = data_block(model);
    const std::size_t tensor_count = model.tensors.size();
    const std::size_t layer_count = model.layers.size();
    const std::size_t total = header_bytes + tensor_entry_bytes * tensor_count +
                              layer_entry_bytes * layer_count + data.size();
    std::vector<uint8_t> bytes(total, 0);

    bytes[0] = 'O';
    bytes[1] = 'A';
    bytes[2] = 'N';
    bytes[3] = 'N';
    store_u16(bytes.data() + 4, format_version);
    store_u16(bytes.data() + 8, static_cast<uint16_t>(tensor_count));
    store_u16(bytes.data() + 10, static_cast<uint16_t>(layer_count));
    store_u32(bytes.data() + 12, static_cast<uint32_t>(data.size()));

    uint32_t cursor = 0;
    for (std::size_t index = 0; index < tensor_count; ++index) {
        const Tensor& tensor = model.tensors[index];
        uint8_t* entry = bytes.data() + tensor_field(index, 0);
        for (std::size_t byte = 0; byte < tensor.name.size(); ++byte)
            entry[byte] = static_cast<uint8_t>(tensor.name[byte]);
        entry[32] = static_cast<uint8_t>(tensor.role);
        entry[33] = static_cast<uint8_t>(tensor.type);
        entry[34] = static_cast<uint8_t>(tensor.exponent);
        entry[35] = tensor.rank;
        for (uint8_t axis = 0; axis < 4; ++axis)
            store_u32(entry + 36 + axis * 4U, tensor.dims[axis]);
        if (tensor.role == Role::constant) {
            store_u32(entry + 52, cursor);
            store_u32(entry + 56, static_cast<uint32_t>(tensor.data.size()));
            const uint32_t end = cursor + static_cast<uint32_t>(tensor.data.size());
            cursor = (end + 3U) & ~uint32_t{3};
        }
    }

    for (std::size_t index = 0; index < layer_count; ++index) {
        const Layer& layer = model.layers[index];
        uint8_t* entry = bytes.data() + layer_field(tensor_count, index, 0);
        entry[0] = static_cast<uint8_t>(layer.kind);
        entry[1] = layer.input_count;
        for (uint8_t slot = 0; slot < 4; ++slot)
            store_u16(entry + 2 + slot * 2U, layer.inputs[slot]);
        store_u16(entry + 10, layer.output);
        store_u16(entry + 12, layer.weights);
        store_u16(entry + 14, layer.bias);
        entry[16] = layer.kernel_h;
        entry[17] = layer.kernel_w;
        entry[18] = layer.stride_h;
        entry[19] = layer.stride_w;
        entry[20] = layer.pad_h;
        entry[21] = layer.pad_w;
        entry[22] = layer.axis;
        store_u16(entry + 24, static_cast<uint16_t>(layer.alpha));
    }

    for (std::size_t byte = 0; byte < data.size(); ++byte)
        bytes
            [header_bytes + tensor_entry_bytes * tensor_count + layer_entry_bytes * layer_count +
             byte] = data[byte];
    return bytes;
}

} // namespace oa::formats::oann

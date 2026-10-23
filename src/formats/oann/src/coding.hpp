// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Little-endian fields and the places they occupy in a format-1 file.
// Shared by the reader and the writer so a model has one byte form.

#include "oa/formats/oann/oann.hpp"

#include <cstddef>
#include <cstdint>

namespace oa::formats::oann {
namespace coding {

/// Reads a 16-bit little-endian value.
///
/// The caller has already checked that two bytes are present.
///
/// @param bytes the first of the two bytes
/// @return the value
[[nodiscard]] inline uint16_t load_u16(const uint8_t* bytes) noexcept {
    return static_cast<uint16_t>(bytes[0] | (static_cast<uint32_t>(bytes[1]) << 8U));
}

/// Reads a 32-bit little-endian value.
///
/// The caller has already checked that four bytes are present.
///
/// @param bytes the first of the four bytes
/// @return the value
[[nodiscard]] inline uint32_t load_u32(const uint8_t* bytes) noexcept {
    return static_cast<uint32_t>(bytes[0]) | (static_cast<uint32_t>(bytes[1]) << 8U) |
           (static_cast<uint32_t>(bytes[2]) << 16U) | (static_cast<uint32_t>(bytes[3]) << 24U);
}

/// Writes a 16-bit little-endian value.
///
/// @param bytes the first of the two bytes
/// @param value the value
inline void store_u16(uint8_t* bytes, uint16_t value) noexcept {
    bytes[0] = static_cast<uint8_t>(value);
    bytes[1] = static_cast<uint8_t>(value >> 8U);
}

/// Writes a 32-bit little-endian value.
///
/// @param bytes the first of the four bytes
/// @param value the value
inline void store_u32(uint8_t* bytes, uint32_t value) noexcept {
    bytes[0] = static_cast<uint8_t>(value);
    bytes[1] = static_cast<uint8_t>(value >> 8U);
    bytes[2] = static_cast<uint8_t>(value >> 16U);
    bytes[3] = static_cast<uint8_t>(value >> 24U);
}

/// Reads a signed byte stored as two's complement.
///
/// @param byte the stored byte
/// @return the value, -128 to 127
[[nodiscard]] inline int8_t signed_byte(uint8_t byte) noexcept {
    if (byte < 128)
        return static_cast<int8_t>(byte);
    return static_cast<int8_t>(static_cast<int16_t>(byte) - 256);
}

/// Reads a signed 16-bit value stored as two's complement.
///
/// @param word the stored value
/// @return the value, -32768 to 32767
[[nodiscard]] inline int16_t signed_word(uint16_t word) noexcept {
    if (word < 32768)
        return static_cast<int16_t>(word);
    return static_cast<int16_t>(static_cast<int32_t>(word) - 65536);
}

/// Returns the file offset of a field in a tensor entry.
///
/// The caller has already kept the tensor count inside its limit, so the
/// offset fits in 32 bits.
///
/// @param index the tensor's place in the table
/// @param field the field's offset inside the entry
/// @return the file offset
[[nodiscard]] inline uint32_t tensor_field(std::size_t index, uint32_t field) noexcept {
    return static_cast<uint32_t>(header_bytes + index * tensor_entry_bytes + field);
}

/// Returns the file offset of a field in a layer entry.
///
/// The caller has already kept both counts inside their limits.
///
/// @param tensor_count how many tensor entries precede the layer table
/// @param index the layer's place in the table
/// @param field the field's offset inside the entry
/// @return the file offset
[[nodiscard]] inline uint32_t
layer_field(std::size_t tensor_count, std::size_t index, uint32_t field) noexcept {
    return static_cast<uint32_t>(
        header_bytes + tensor_count * tensor_entry_bytes + index * layer_entry_bytes + field
    );
}

/// Returns the data block a model would write, counting each constant's bytes.
///
/// Padding takes each later constant to the next multiple of 4. The largest
/// 64-bit value means the sum does not fit.
///
/// @param model the network
/// @return the data block's length
[[nodiscard]] inline uint64_t data_block_bytes(const Model& model) noexcept {
    uint64_t cursor = 0;
    for (const Tensor& tensor : model.tensors) {
        if (tensor.role != Role::constant)
            continue;
        const uint64_t end = cursor + tensor.data.size();
        if (end < cursor)
            return ~uint64_t{0};
        const uint64_t aligned = (end + 3U) & ~uint64_t{3};
        cursor = aligned;
    }
    return cursor;
}

/// Returns the length of the file a model would write.
///
/// @param model the network
/// @return the file length, or the largest 64-bit value when it does not fit
[[nodiscard]] inline uint64_t encoded_bytes(const Model& model) noexcept {
    const uint64_t data = data_block_bytes(model);
    if (data == ~uint64_t{0})
        return data;
    const uint64_t tables = uint64_t{header_bytes} +
                            uint64_t{tensor_entry_bytes} * model.tensors.size() +
                            uint64_t{layer_entry_bytes} * model.layers.size();
    if (tables > ~uint64_t{0} - data)
        return ~uint64_t{0};
    return tables + data;
}

} // namespace coding
} // namespace oa::formats::oann

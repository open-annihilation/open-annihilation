// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Records written into a keyframe with the bytes between their fields zero.
// A record made on the stack and then copied in carries in its padding what
// the stack held there, which differs from one run of a replay to the next,
// so two images of the same state would differ there.

#include "oa/sim/match_runtime/keyframe.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

namespace oa::sim::match_runtime::keyframe {

/// A run of bytes in a record that no field holds: padding the compiler leaves between fields.
struct Padding {
    std::size_t offset{}; ///< where the run starts in the record
    std::size_t size{};   ///< how many bytes
};

/// Adds up a record's padding, to check that its fields and its padding fill it.
///
/// @param padding The runs of bytes in the record that no field holds.
/// @return How many bytes they take.
constexpr std::size_t padding_bytes(std::span<const Padding> padding) noexcept {
    std::size_t bytes = 0;
    for (const Padding& gap : padding)
        bytes += gap.size;
    return bytes;
}

/// Writes trivially copyable records into the current section with their padding zero.
///
/// Two records with the same fields are then the same bytes.
///
/// @tparam T The records' type.
/// @param[in,out] out The writer.
/// @param items The records.
/// @param padding The runs of bytes in a record that no field holds.
template <class T>
void write_records(Writer* out, std::span<const T> items, std::span<const Padding> padding) {
    static_assert(std::is_trivially_copyable_v<T>);
    for (const T& item : items) {
        T copy = item;
        for (const Padding& gap : padding)
            std::memset(reinterpret_cast<uint8_t*>(&copy) + gap.offset, 0, gap.size);
        out->bytes(&copy, sizeof copy);
    }
}

} // namespace oa::sim::match_runtime::keyframe

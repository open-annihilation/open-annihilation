// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Canonical bytes for one plain Lua value, and the way back. A saved game
// stores the bytes; this module does not read or write the save itself.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

struct lua_State;

namespace oa::data::script_host {

/// Most bytes one plain value may encode to, header included.
inline constexpr std::size_t plain_data_limit = std::size_t{1} << 20;

/// Most nested tables one plain value may contain, the outermost counted.
inline constexpr int plain_data_depth_limit = 64;

/// Bytes of one plain value. Empty until a successful `assign` or encode.
///
/// The caller of `encode_plain_data` and `push_plain_data` keeps this object
/// outside any frame a Lua raise can skip. A raise does not run destructors,
/// so a `PlainData` that lives in that frame would be left half-destroyed.
class PlainData {
  public:

    /// Returns the bytes from the last successful store.
    ///
    /// The span is valid until the next `assign`, `clear`, or destruction.
    ///
    /// @return the stored bytes, empty when nothing is stored
    [[nodiscard]] std::span<const uint8_t> bytes() const noexcept;

    /// Drops the stored bytes.
    void clear() noexcept;

    /// Replaces the stored bytes with a copy of `bytes`.
    ///
    /// A span longer than `plain_data_limit`, and a copy the allocator
    /// refuses, leave the stored bytes as they were.
    ///
    /// @param bytes bytes to copy; may be empty
    /// @return false when the copy is not stored
    [[nodiscard]] bool assign(std::span<const uint8_t> bytes) noexcept;

  private:

    std::vector<uint8_t> stored_;
};

/// Why a value or a byte string is not plain data.
///
/// `where` is the place in the value, from the top, such as `saved.units[3]`.
/// A string key that is a name is a dot and that name. Any other string key
/// is `["…"]`, with quotes and bytes outside printable ASCII escaped. An
/// integer key is in brackets. `message` says what is wrong there.
struct PlainDataProblem {
    std::string where;
    std::string message;
};

/// Writes a problem as `where: message`.
///
/// Either part alone is written when the other is empty.
///
/// @param problem the problem to write
/// @return the sentence
[[nodiscard]] std::string describe(const PlainDataProblem& problem);

/// Encodes the value at `index` into canonical plain-data bytes.
///
/// The same value gives the same bytes on every build, whatever order its
/// tables were filled in. Plain data is nil (only at the top), booleans,
/// integers, floats, strings, fixed counts and tables of those. A float key
/// Lua has already stored as an integer is an integer key. A shared table
/// is written twice. Nil inside a table cannot appear.
///
/// Refuses, without charging and without changing `out`, a function, a
/// coroutine, userdata other than fixed, a table with a metatable, a cycle,
/// a key that is not an integer or a string, more than `plain_data_depth_limit`
/// levels, a string past the string limit, a fixed count outside the fixed
/// range, or output past `plain_data_limit`. `problem.where` names the place.
///
/// The walk makes no call that can raise. It may use containers, and it
/// destroys them before it charges. The charge is `units_for_bytes` of the
/// encoded size plus `element_charge` for every value written, keys included.
/// That charge may raise when it spends the allowance. Keep `out` and
/// `problem` outside the frame a raise can skip.
///
/// @param state sandbox state, inside a phase
/// @param index stack index of the value
/// @param[out] out replaced with the bytes when the call returns true
/// @param[out] problem set when the call returns false; cleared on success
/// @return false when the value is not plain data
[[nodiscard]] bool
encode_plain_data(lua_State* state, int index, PlainData& out, PlainDataProblem& problem);

/// Checks plain-data bytes without using Lua.
///
/// Accepts only bytes `encode_plain_data` could have written: the header,
/// one value, tags, lengths that fit in the input, sorted unique keys,
/// depth, the size limit, string and fixed ranges, and no trailing bytes.
/// A length is compared with the bytes still left before any of those bytes
/// are read.
///
/// @param bytes bytes to check; may be empty
/// @param[out] problem set when the call returns false; cleared on success
/// @return false when `bytes` is not one plain value
[[nodiscard]] bool
check_plain_data(std::span<const uint8_t> bytes, PlainDataProblem& problem) noexcept;

/// Pushes the value `bytes` encodes.
///
/// Checks first. Damaged bytes raise `the saved data is damaged:` and
/// `describe` of the problem, and push nothing. A value that passes is
/// one stack value. Tables are created with sizes taken from their entry
/// counts. The charge matches encoding: `units_for_bytes` of the encoded
/// size, plus `element_charge` for every value, charged as the value is
/// built. Building a string or a table may also raise a memory error.
///
/// No object with a destructor is alive inside this function across a call
/// that can raise. The caller keeps any such object outside this call.
///
/// @param state sandbox state, inside a phase
/// @param bytes bytes `check_plain_data` can accept
void push_plain_data(lua_State* state, std::span<const uint8_t> bytes);

} // namespace oa::data::script_host

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Canonical bytes for one plain Lua value. Encoding reads the value with
// calls that do not raise, drops every container, and only then charges.
// Checking never touches Lua. Pushing checks, then builds the value.

#include "oa/data/script_host/plain_data.hpp"

#include "oa/data/script_host/fixed.hpp"
#include "oa/data/script_host/script_host.hpp"

#include "lua.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::data::script_host {
namespace {

constexpr char header_text[] = {'O', 'A', 'P', 'D'};
constexpr std::size_t header_bytes = 5;
constexpr uint8_t format_version = 1;

constexpr uint8_t tag_nil = 0x00;
constexpr uint8_t tag_false = 0x01;
constexpr uint8_t tag_true = 0x02;
constexpr uint8_t tag_integer = 0x03;
constexpr uint8_t tag_float = 0x04;
constexpr uint8_t tag_string = 0x05;
constexpr uint8_t tag_fixed = 0x06;
constexpr uint8_t tag_table = 0x07;

constexpr uint64_t canonical_nan_bits = 0x7ff8000000000000ull;
constexpr std::size_t number_bytes = 8;
constexpr std::size_t damage_text_bytes = 480;

static_assert(plain_data_limit == 1048576);
static_assert(plain_data_depth_limit == 64);
static_assert(sizeof(lua_Integer) == number_bytes);
static_assert(sizeof(lua_Number) == number_bytes);
static_assert(string_length_limit == 65536);

/// True when this build stores a multi-byte integer least significant byte first.
bool host_little_endian() noexcept {
    const uint16_t one = 1;
    unsigned char raw[2] = {};
    std::memcpy(raw, &one, sizeof raw);
    return raw[0] == 1;
}

/// IEEE bits of `value`, the sign in the high bit, whatever this build's order is.
uint64_t bits_of_double(double value) noexcept {
    unsigned char raw[number_bytes] = {};
    std::memcpy(raw, &value, number_bytes);
    if (!host_little_endian()) {
        for (std::size_t i = 0; i < number_bytes / 2; ++i)
            std::swap(raw[i], raw[number_bytes - 1 - i]);
    }
    uint64_t bits = 0;
    for (std::size_t i = 0; i < number_bytes; ++i)
        bits |= static_cast<uint64_t>(raw[i]) << (8 * i);
    return bits;
}

/// The double whose IEEE bits are `bits`.
double double_from_bits(uint64_t bits) noexcept {
    unsigned char raw[number_bytes] = {};
    for (std::size_t i = 0; i < number_bytes; ++i)
        raw[i] = static_cast<unsigned char>((bits >> (8 * i)) & 0xffu);
    if (!host_little_endian()) {
        for (std::size_t i = 0; i < number_bytes / 2; ++i)
            std::swap(raw[i], raw[number_bytes - 1 - i]);
    }
    double value = 0;
    std::memcpy(&value, raw, number_bytes);
    return value;
}

/// True when the exponent is all ones and the fraction is not zero.
bool nan_bits(uint64_t bits) noexcept {
    const uint64_t exponent = (bits >> 52) & 0x7ffu;
    const uint64_t fraction = bits & 0x000fffffffffffffull;
    return exponent == 0x7ffu && fraction != 0;
}

/// Quiet NaN every build writes, or `bits` when `bits` is not a NaN.
uint64_t canonical_float_bits(uint64_t bits) noexcept {
    if (nan_bits(bits))
        return canonical_nan_bits;
    return bits;
}

/// Negative when `left` is first in byte order, zero when the bytes match.
int compare_bytes(std::string_view left, std::string_view right) noexcept {
    const std::size_t shared = std::min(left.size(), right.size());
    for (std::size_t i = 0; i < shared; ++i) {
        const auto left_byte = static_cast<unsigned char>(left[i]);
        const auto right_byte = static_cast<unsigned char>(right[i]);
        if (left_byte < right_byte)
            return -1;
        if (left_byte > right_byte)
            return 1;
    }
    if (left.size() < right.size())
        return -1;
    if (left.size() > right.size())
        return 1;
    return 0;
}

/// True when `text` is a Lua name: a letter or `_`, then letters, digits and `_`.
bool plain_name(std::string_view text) noexcept {
    if (text.empty())
        return false;
    const auto first = static_cast<unsigned char>(text[0]);
    const bool letter = (first >= 'A' && first <= 'Z') || (first >= 'a' && first <= 'z');
    if (!letter && first != '_')
        return false;
    for (char raw : text) {
        const auto byte = static_cast<unsigned char>(raw);
        const bool later_letter = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
        const bool digit = byte >= '0' && byte <= '9';
        if (!later_letter && !digit && byte != '_')
            return false;
    }
    return true;
}

/// Appends a string key the way a message names it.
void append_string_path(std::string& where, std::string_view text) {
    if (plain_name(text)) {
        where.push_back('.');
        where.append(text);
        return;
    }
    where += "[\"";
    constexpr char hex[] = "0123456789abcdef";
    for (char raw : text) {
        const auto byte = static_cast<unsigned char>(raw);
        if (byte == '\\' || byte == '"') {
            where.push_back('\\');
            where.push_back(static_cast<char>(byte));
        } else if (byte < 0x20 || byte > 0x7e) {
            where += "\\x";
            where.push_back(hex[byte >> 4]);
            where.push_back(hex[byte & 0x0f]);
        } else {
            where.push_back(static_cast<char>(byte));
        }
    }
    where += "\"]";
}

/// Appends an integer key in brackets.
void append_integer_path(std::string& where, int64_t key) {
    where.push_back('[');
    where += std::to_string(key);
    where.push_back(']');
}

/// Copies `text` into `out`, cutting it so the result stays inside `size`.
void copy_capped(char* out, std::size_t size, std::string_view text) noexcept {
    if (out == nullptr || size == 0)
        return;
    const std::size_t keep = text.size() < size - 1 ? text.size() : size - 1;
    if (keep > 0)
        std::memcpy(out, text.data(), keep);
    out[keep] = '\0';
}

/// A cursor that refuses a read past the bytes it was given.
struct Reader {
    const uint8_t* cursor = nullptr;
    const uint8_t* stop = nullptr;

    /// Bytes still unread.
    ///
    /// @return the unread count
    [[nodiscard]] std::size_t remaining() const noexcept {
        return static_cast<std::size_t>(stop - cursor);
    }

    /// Moves past `count` bytes when they are all still unread.
    ///
    /// @param count bytes to skip
    /// @return false when fewer than `count` bytes remain
    [[nodiscard]] bool skip(std::size_t count) noexcept {
        if (count > remaining())
            return false;
        cursor += count;
        return true;
    }

    /// Points `out` at the next `count` bytes and moves past them.
    ///
    /// @param count bytes to take
    /// @param[out] out set to the first of those bytes when the call returns true
    /// @return false when fewer than `count` bytes remain
    [[nodiscard]] bool take(std::size_t count, const uint8_t*& out) noexcept {
        if (count > remaining())
            return false;
        out = cursor;
        cursor += count;
        return true;
    }

    /// Reads one byte.
    ///
    /// @param[out] out the byte
    /// @return false when no byte remains
    [[nodiscard]] bool u8(uint8_t& out) noexcept {
        const uint8_t* data = nullptr;
        if (!take(1, data))
            return false;
        out = data[0];
        return true;
    }

    /// Reads a little-endian `uint32_t`.
    ///
    /// @param[out] out the value
    /// @return false when fewer than 4 bytes remain
    [[nodiscard]] bool u32(uint32_t& out) noexcept {
        const uint8_t* data = nullptr;
        if (!take(4, data))
            return false;
        out = static_cast<uint32_t>(data[0]) | (static_cast<uint32_t>(data[1]) << 8) |
              (static_cast<uint32_t>(data[2]) << 16) | (static_cast<uint32_t>(data[3]) << 24);
        return true;
    }

    /// Reads a little-endian `uint64_t`.
    ///
    /// @param[out] out the value
    /// @return false when fewer than 8 bytes remain
    [[nodiscard]] bool u64(uint64_t& out) noexcept {
        const uint8_t* data = nullptr;
        if (!take(number_bytes, data))
            return false;
        uint64_t bits = 0;
        for (std::size_t i = 0; i < number_bytes; ++i)
            bits |= static_cast<uint64_t>(data[i]) << (8 * i);
        out = bits;
        return true;
    }
};

/// Records a refusal and returns false.
bool refuse(PlainDataProblem& problem, const std::string& where, const char* message) {
    problem.where = where;
    problem.message = message;
    return false;
}

/// True when `where` already ends at the value being read.
bool ends_early(PlainDataProblem& problem, const std::string& where) {
    return refuse(problem, where, "the saved data ends early");
}

enum class KeyKind : uint8_t { none, integer, string };

/// The previous key of one table, used to require a strict canonical order.
struct PreviousKey {
    KeyKind kind = KeyKind::none;
    int64_t whole = 0;
    std::string text;
};

/// Reads one key and refuses a key that is missing, repeated, or out of order.
bool check_key(
    Reader& reader, PreviousKey& previous, const std::string& where, PlainDataProblem& problem
) {
    uint8_t tag = 0;
    if (!reader.u8(tag))
        return ends_early(problem, where);
    if (tag == tag_integer) {
        uint64_t bits = 0;
        if (!reader.u64(bits))
            return ends_early(problem, where);
        const auto whole = static_cast<int64_t>(bits);
        if (previous.kind == KeyKind::string)
            return refuse(problem, where, "the table keys are not in order");
        if (previous.kind == KeyKind::integer) {
            if (whole == previous.whole)
                return refuse(problem, where, "the table has a repeated key");
            if (whole < previous.whole)
                return refuse(problem, where, "the table keys are not in order");
        }
        previous.kind = KeyKind::integer;
        previous.whole = whole;
        previous.text.clear();
        return true;
    }
    if (tag == tag_string) {
        uint32_t length = 0;
        if (!reader.u32(length))
            return ends_early(problem, where);
        if (static_cast<std::size_t>(length) > reader.remaining())
            return ends_early(problem, where);
        if (length > string_length_limit)
            return refuse(problem, where, "a string is longer than 65536 bytes");
        const uint8_t* data = nullptr;
        if (!reader.take(length, data))
            return ends_early(problem, where);
        const std::string_view text(reinterpret_cast<const char*>(data), length);
        if (previous.kind == KeyKind::string) {
            const int order = compare_bytes(previous.text, text);
            if (order == 0)
                return refuse(problem, where, "the table has a repeated key");
            if (order > 0)
                return refuse(problem, where, "the table keys are not in order");
        }
        previous.kind = KeyKind::string;
        previous.text.assign(text);
        return true;
    }
    return refuse(problem, where, "a key must be an integer or a string");
}

/// Checks one value. `top` allows nil, which only the outermost value may be.
bool check_value(
    Reader& reader, int depth, bool top, std::string& where, PlainDataProblem& problem
) {
    if (depth > plain_data_depth_limit)
        return refuse(problem, where, "the saved data is nested more than 64 deep");
    uint8_t tag = 0;
    if (!reader.u8(tag))
        return ends_early(problem, where);
    switch (tag) {
    case tag_nil:
        if (!top)
            return refuse(problem, where, "only the top value may be nil");
        return true;
    case tag_false:
    case tag_true:
        return true;
    case tag_integer:
        if (!reader.skip(number_bytes))
            return ends_early(problem, where);
        return true;
    case tag_float: {
        uint64_t bits = 0;
        if (!reader.u64(bits))
            return ends_early(problem, where);
        if (nan_bits(bits) && bits != canonical_nan_bits)
            return refuse(problem, where, "a NaN is not in canonical form");
        return true;
    }
    case tag_string: {
        uint32_t length = 0;
        if (!reader.u32(length))
            return ends_early(problem, where);
        if (static_cast<std::size_t>(length) > reader.remaining())
            return ends_early(problem, where);
        if (length > string_length_limit)
            return refuse(problem, where, "a string is longer than 65536 bytes");
        if (!reader.skip(length))
            return ends_early(problem, where);
        return true;
    }
    case tag_fixed: {
        uint64_t bits = 0;
        if (!reader.u64(bits))
            return ends_early(problem, where);
        const auto count = static_cast<int64_t>(bits);
        if (count < fixed_count_min || count > fixed_count_max)
            return refuse(problem, where, "a fixed count is outside the fixed range");
        return true;
    }
    case tag_table: {
        uint32_t entries = 0;
        if (!reader.u32(entries))
            return ends_early(problem, where);
        if (static_cast<std::size_t>(entries) > reader.remaining())
            return ends_early(problem, where);
        PreviousKey previous;
        const std::size_t mark = where.size();
        for (uint32_t entry = 0; entry < entries; ++entry) {
            if (!check_key(reader, previous, where, problem))
                return false;
            if (previous.kind == KeyKind::integer)
                append_integer_path(where, previous.whole);
            else
                append_string_path(where, previous.text);
            if (!check_value(reader, depth + 1, false, where, problem))
                return false;
            where.resize(mark);
        }
        return true;
    }
    default:
        return refuse(problem, where, "the value has an unknown tag");
    }
}

/// Checks `bytes` and may throw `std::bad_alloc` while it names a place.
bool check_collected(std::span<const uint8_t> bytes, PlainDataProblem& problem) {
    if (bytes.size() > plain_data_limit)
        return refuse(problem, "saved", "the saved data is larger than 1 MiB");
    if (bytes.size() < header_bytes)
        return ends_early(problem, "saved");
    if (bytes[0] != static_cast<uint8_t>(header_text[0]) ||
        bytes[1] != static_cast<uint8_t>(header_text[1]) ||
        bytes[2] != static_cast<uint8_t>(header_text[2]) ||
        bytes[3] != static_cast<uint8_t>(header_text[3]) || bytes[4] != format_version)
        return refuse(problem, "saved", "the header is not plain data");
    Reader reader{bytes.data() + header_bytes, bytes.data() + bytes.size()};
    std::string where = "saved";
    if (!check_value(reader, 1, true, where, problem))
        return false;
    if (reader.remaining() != 0)
        return refuse(problem, "saved", "bytes follow the value");
    problem.where.clear();
    problem.message.clear();
    return true;
}

/// Appends `data` when the result would still fit in `plain_data_limit`.
bool append_raw(
    std::vector<uint8_t>& out,
    const uint8_t* data,
    std::size_t count,
    const std::string& where,
    PlainDataProblem& problem
) {
    if (count > plain_data_limit || out.size() > plain_data_limit - count)
        return refuse(problem, where, "the saved data is larger than 1 MiB");
    if (count > 0)
        out.insert(out.end(), data, data + count);
    return true;
}

/// Appends one tag byte.
bool append_tag(
    std::vector<uint8_t>& out, uint8_t tag, const std::string& where, PlainDataProblem& problem
) {
    return append_raw(out, &tag, 1, where, problem);
}

/// Appends a little-endian `uint32_t`.
bool append_u32(
    std::vector<uint8_t>& out, uint32_t value, const std::string& where, PlainDataProblem& problem
) {
    const uint8_t raw[4] = {
        static_cast<uint8_t>(value),
        static_cast<uint8_t>(value >> 8),
        static_cast<uint8_t>(value >> 16),
        static_cast<uint8_t>(value >> 24),
    };
    return append_raw(out, raw, 4, where, problem);
}

/// Appends a little-endian two's-complement 64-bit integer.
bool append_u64(
    std::vector<uint8_t>& out, uint64_t value, const std::string& where, PlainDataProblem& problem
) {
    uint8_t raw[number_bytes] = {};
    for (std::size_t i = 0; i < number_bytes; ++i)
        raw[i] = static_cast<uint8_t>((value >> (8 * i)) & 0xffu);
    return append_raw(out, raw, number_bytes, where, problem);
}

/// One table key, and the stack slot that holds a copy of its value.
struct SavedKey {
    KeyKind kind = KeyKind::none;
    int64_t whole = 0;
    std::string text;
    int anchor = 0;
};

/// Integer keys first, ascending, then string keys in byte order.
bool key_before(const SavedKey& first, const SavedKey& second) noexcept {
    if (first.kind != second.kind)
        return first.kind == KeyKind::integer;
    if (first.kind == KeyKind::integer)
        return first.whole < second.whole;
    return compare_bytes(first.text, second.text) < 0;
}

/// The walk that fills one buffer. Destroyed before the phase is charged.
struct EncodeWalk {
    lua_State* state = nullptr;
    std::vector<uint8_t> out;
    uint64_t values = 0;
    PlainDataProblem* problem = nullptr;
    std::vector<const void*> path;
    std::string where;
};

/// Restores the Lua stack when the walk returns or throws.
struct StackRestore {
    lua_State* state = nullptr;
    int top = 0;

    ~StackRestore() {
        if (state != nullptr)
            lua_settop(state, top);
    }
};

/// Records `message` at the walk's current place and returns false.
bool fail_walk(EncodeWalk& walk, const char* message) {
    return refuse(*walk.problem, walk.where, message);
}

/// Writes one integer value, which a key uses as well as a number.
bool write_integer(EncodeWalk& walk, int64_t whole) {
    if (!append_tag(walk.out, tag_integer, walk.where, *walk.problem))
        return false;
    if (!append_u64(walk.out, static_cast<uint64_t>(whole), walk.where, *walk.problem))
        return false;
    ++walk.values;
    return true;
}

/// Writes one string value.
bool write_string(EncodeWalk& walk, std::string_view text) {
    if (text.size() > string_length_limit)
        return fail_walk(walk, "a string is longer than 65536 bytes");
    if (!append_tag(walk.out, tag_string, walk.where, *walk.problem))
        return false;
    if (!append_u32(walk.out, static_cast<uint32_t>(text.size()), walk.where, *walk.problem))
        return false;
    if (!append_raw(
            walk.out,
            reinterpret_cast<const uint8_t*>(text.data()),
            text.size(),
            walk.where,
            *walk.problem
        ))
        return false;
    ++walk.values;
    return true;
}

/// Reads a key at `index` into `key` without converting it.
bool read_key(EncodeWalk& walk, int index, SavedKey& key) {
    const int type = lua_type(walk.state, index);
    if (type == LUA_TNUMBER) {
        if (lua_isinteger(walk.state, index) != 0) {
            int is_number = 0;
            const lua_Integer whole = lua_tointegerx(walk.state, index, &is_number);
            if (is_number == 0)
                return fail_walk(walk, "a float key cannot be saved");
            key.kind = KeyKind::integer;
            key.whole = static_cast<int64_t>(whole);
            return true;
        }
        return fail_walk(walk, "a float key cannot be saved");
    }
    if (type == LUA_TSTRING) {
        std::size_t length = 0;
        const char* text = lua_tolstring(walk.state, index, &length);
        if (text == nullptr)
            return fail_walk(walk, "a string key cannot be saved");
        if (length > string_length_limit)
            return fail_walk(walk, "a string is longer than 65536 bytes");
        key.kind = KeyKind::string;
        key.text.assign(text, length);
        return true;
    }
    if (type == LUA_TBOOLEAN)
        return fail_walk(walk, "a boolean key cannot be saved");
    if (type == LUA_TTABLE)
        return fail_walk(walk, "a table key cannot be saved");
    if (type == LUA_TFUNCTION)
        return fail_walk(walk, "a function key cannot be saved");
    if (type == LUA_TTHREAD)
        return fail_walk(walk, "a coroutine key cannot be saved");
    if (type == LUA_TUSERDATA || type == LUA_TLIGHTUSERDATA)
        return fail_walk(walk, "a userdata key cannot be saved");
    return fail_walk(walk, "a key of this kind cannot be saved");
}

bool write_value(EncodeWalk& walk, int index, int depth);

/// Writes a table: integer keys ascending, then string keys in byte order.
bool write_table(EncodeWalk& walk, int index, int depth) {
    if (lua_checkstack(walk.state, 4) == 0)
        return fail_walk(walk, "the saved data is too large to read");
    if (lua_getmetatable(walk.state, index) != 0) {
        lua_pop(walk.state, 1);
        return fail_walk(walk, "a table with a metatable cannot be saved");
    }
    const void* identity = lua_topointer(walk.state, index);
    for (const void* seen : walk.path) {
        if (seen == identity)
            return fail_walk(walk, "a table that contains itself cannot be saved");
    }
    walk.path.push_back(identity);

    const int base = lua_gettop(walk.state);
    std::vector<SavedKey> keys;
    lua_pushnil(walk.state);
    while (lua_next(walk.state, index) != 0) {
        if (lua_checkstack(walk.state, 2) == 0) {
            lua_settop(walk.state, base);
            walk.path.pop_back();
            return fail_walk(walk, "the saved data is too large to read");
        }
        SavedKey found;
        if (keys.size() >= static_cast<std::size_t>(std::numeric_limits<int>::max()) ||
            base >= std::numeric_limits<int>::max() - static_cast<int>(keys.size()) - 1) {
            lua_settop(walk.state, base);
            walk.path.pop_back();
            return fail_walk(walk, "the saved data is too large to read");
        }
        if (!read_key(walk, -2, found)) {
            lua_settop(walk.state, base);
            walk.path.pop_back();
            return false;
        }
        // The copy has to sit under the key. lua_next needs the key at the top.
        lua_pushvalue(walk.state, -1);
        const int anchor_at = base + static_cast<int>(keys.size()) + 1;
        lua_rotate(walk.state, anchor_at, 1);
        lua_pop(walk.state, 1);
        found.anchor = anchor_at;
        keys.push_back(std::move(found));
    }

    std::sort(keys.begin(), keys.end(), key_before);
    if (keys.size() > std::numeric_limits<uint32_t>::max()) {
        lua_settop(walk.state, base);
        walk.path.pop_back();
        return fail_walk(walk, "the saved data is larger than 1 MiB");
    }
    if (!append_tag(walk.out, tag_table, walk.where, *walk.problem) ||
        !append_u32(walk.out, static_cast<uint32_t>(keys.size()), walk.where, *walk.problem)) {
        lua_settop(walk.state, base);
        walk.path.pop_back();
        return false;
    }
    ++walk.values;

    const std::size_t mark = walk.where.size();
    for (const SavedKey& saved : keys) {
        if (saved.kind == KeyKind::integer)
            append_integer_path(walk.where, saved.whole);
        else
            append_string_path(walk.where, saved.text);
        const bool wrote_key = saved.kind == KeyKind::integer ? write_integer(walk, saved.whole)
                                                              : write_string(walk, saved.text);
        if (!wrote_key || !write_value(walk, saved.anchor, depth + 1)) {
            lua_settop(walk.state, base);
            walk.path.pop_back();
            return false;
        }
        walk.where.resize(mark);
    }
    lua_settop(walk.state, base);
    walk.path.pop_back();
    return true;
}

/// Writes the value at `index`. `depth` is 1 for the outermost value.
bool write_value(EncodeWalk& walk, int index, int depth) {
    if (depth > plain_data_depth_limit)
        return fail_walk(walk, "the saved data is nested more than 64 deep");
    if (lua_checkstack(walk.state, 4) == 0)
        return fail_walk(walk, "the saved data is too large to read");
    const int type = lua_type(walk.state, index);
    switch (type) {
    case LUA_TNIL:
        if (depth != 1)
            return fail_walk(walk, "only the top value may be nil");
        if (!append_tag(walk.out, tag_nil, walk.where, *walk.problem))
            return false;
        ++walk.values;
        return true;
    case LUA_TBOOLEAN: {
        const uint8_t tag = lua_toboolean(walk.state, index) != 0 ? tag_true : tag_false;
        if (!append_tag(walk.out, tag, walk.where, *walk.problem))
            return false;
        ++walk.values;
        return true;
    }
    case LUA_TNUMBER: {
        if (lua_isinteger(walk.state, index) != 0) {
            int is_number = 0;
            const lua_Integer whole = lua_tointegerx(walk.state, index, &is_number);
            if (is_number == 0)
                return fail_walk(walk, "a number cannot be saved");
            return write_integer(walk, static_cast<int64_t>(whole));
        }
        int is_number = 0;
        const lua_Number number = lua_tonumberx(walk.state, index, &is_number);
        if (is_number == 0)
            return fail_walk(walk, "a number cannot be saved");
        const uint64_t bits = canonical_float_bits(bits_of_double(number));
        if (!append_tag(walk.out, tag_float, walk.where, *walk.problem) ||
            !append_u64(walk.out, bits, walk.where, *walk.problem))
            return false;
        ++walk.values;
        return true;
    }
    case LUA_TSTRING: {
        std::size_t length = 0;
        const char* text = lua_tolstring(walk.state, index, &length);
        if (text == nullptr)
            return fail_walk(walk, "a string cannot be saved");
        return write_string(walk, std::string_view(text, length));
    }
    case LUA_TUSERDATA: {
        int64_t count = 0;
        if (!to_fixed(walk.state, index, &count))
            return fail_walk(walk, "userdata cannot be saved");
        if (count < fixed_count_min || count > fixed_count_max)
            return fail_walk(walk, "a fixed count is outside the fixed range");
        if (!append_tag(walk.out, tag_fixed, walk.where, *walk.problem) ||
            !append_u64(walk.out, static_cast<uint64_t>(count), walk.where, *walk.problem))
            return false;
        ++walk.values;
        return true;
    }
    case LUA_TTABLE:
        return write_table(walk, index, depth);
    case LUA_TFUNCTION:
        return fail_walk(walk, "a function cannot be saved");
    case LUA_TTHREAD:
        return fail_walk(walk, "a coroutine cannot be saved");
    case LUA_TLIGHTUSERDATA:
        return fail_walk(walk, "userdata cannot be saved");
    case LUA_TNONE:
        return fail_walk(walk, "there is no value to save");
    default:
        return fail_walk(walk, "a value of this kind cannot be saved");
    }
}

/// Encodes into `out` and reports the charge. Containers die with this call.
bool encode_collected(
    lua_State* state, int index, PlainData& out, PlainDataProblem& problem, uint64_t& charge
) {
    StackRestore restore{state, lua_gettop(state)};
    EncodeWalk walk;
    walk.state = state;
    walk.problem = &problem;
    walk.where = "saved";
    const uint8_t header[header_bytes] = {
        static_cast<uint8_t>(header_text[0]),
        static_cast<uint8_t>(header_text[1]),
        static_cast<uint8_t>(header_text[2]),
        static_cast<uint8_t>(header_text[3]),
        format_version,
    };
    if (!append_raw(walk.out, header, header_bytes, walk.where, problem))
        return false;
    const int absolute = lua_absindex(state, index);
    if (!write_value(walk, absolute, 1))
        return false;
    if (!out.assign(walk.out))
        return refuse(problem, "saved", "the saved data could not be stored");
    charge = units_for_bytes(walk.out.size()) + element_charge * walk.values;
    problem.where.clear();
    problem.message.clear();
    return true;
}

/// Skips one value already known to be well formed. Returns null when it is not.
const uint8_t* skip_value(const uint8_t* cursor, const uint8_t* stop) noexcept {
    if (cursor == nullptr || cursor >= stop)
        return nullptr;
    const uint8_t tag = *cursor;
    ++cursor;
    switch (tag) {
    case tag_nil:
    case tag_false:
    case tag_true:
        return cursor;
    case tag_integer:
    case tag_float:
    case tag_fixed:
        if (static_cast<std::size_t>(stop - cursor) < number_bytes)
            return nullptr;
        return cursor + number_bytes;
    case tag_string: {
        if (static_cast<std::size_t>(stop - cursor) < 4)
            return nullptr;
        const uint32_t length =
            static_cast<uint32_t>(cursor[0]) | (static_cast<uint32_t>(cursor[1]) << 8) |
            (static_cast<uint32_t>(cursor[2]) << 16) | (static_cast<uint32_t>(cursor[3]) << 24);
        cursor += 4;
        if (static_cast<std::size_t>(length) > static_cast<std::size_t>(stop - cursor))
            return nullptr;
        return cursor + length;
    }
    case tag_table: {
        if (static_cast<std::size_t>(stop - cursor) < 4)
            return nullptr;
        const uint32_t entries =
            static_cast<uint32_t>(cursor[0]) | (static_cast<uint32_t>(cursor[1]) << 8) |
            (static_cast<uint32_t>(cursor[2]) << 16) | (static_cast<uint32_t>(cursor[3]) << 24);
        cursor += 4;
        if (static_cast<std::size_t>(entries) > static_cast<std::size_t>(stop - cursor))
            return nullptr;
        for (uint32_t entry = 0; entry < entries; ++entry) {
            cursor = skip_value(cursor, stop);
            cursor = skip_value(cursor, stop);
            if (cursor == nullptr)
                return nullptr;
        }
        return cursor;
    }
    default:
        return nullptr;
    }
}

/// Raises the damaged-data sentence. Does not return.
void raise_damaged(lua_State* state, const char* text) {
    luaL_error(state, "%s", text);
}

/// Pushes one value and charges one element. `cursor` is the tag.
const uint8_t* push_value(lua_State* state, const uint8_t* cursor, const uint8_t* stop) {
    charge_work(state, element_charge);
    if (cursor == nullptr || cursor >= stop) {
        raise_damaged(state, "the saved data is damaged: saved: the saved data ends early");
        return nullptr;
    }
    const uint8_t tag = *cursor;
    ++cursor;
    switch (tag) {
    case tag_nil:
        lua_pushnil(state);
        return cursor;
    case tag_false:
        lua_pushboolean(state, 0);
        return cursor;
    case tag_true:
        lua_pushboolean(state, 1);
        return cursor;
    case tag_integer: {
        if (static_cast<std::size_t>(stop - cursor) < number_bytes) {
            raise_damaged(state, "the saved data is damaged: saved: the saved data ends early");
            return nullptr;
        }
        uint64_t bits = 0;
        for (std::size_t i = 0; i < number_bytes; ++i)
            bits |= static_cast<uint64_t>(cursor[i]) << (8 * i);
        cursor += number_bytes;
        lua_pushinteger(state, static_cast<lua_Integer>(static_cast<int64_t>(bits)));
        return cursor;
    }
    case tag_float: {
        if (static_cast<std::size_t>(stop - cursor) < number_bytes) {
            raise_damaged(state, "the saved data is damaged: saved: the saved data ends early");
            return nullptr;
        }
        uint64_t bits = 0;
        for (std::size_t i = 0; i < number_bytes; ++i)
            bits |= static_cast<uint64_t>(cursor[i]) << (8 * i);
        cursor += number_bytes;
        lua_pushnumber(state, double_from_bits(bits));
        return cursor;
    }
    case tag_string: {
        if (static_cast<std::size_t>(stop - cursor) < 4) {
            raise_damaged(state, "the saved data is damaged: saved: the saved data ends early");
            return nullptr;
        }
        const uint32_t length =
            static_cast<uint32_t>(cursor[0]) | (static_cast<uint32_t>(cursor[1]) << 8) |
            (static_cast<uint32_t>(cursor[2]) << 16) | (static_cast<uint32_t>(cursor[3]) << 24);
        cursor += 4;
        if (static_cast<std::size_t>(length) > static_cast<std::size_t>(stop - cursor)) {
            raise_damaged(state, "the saved data is damaged: saved: the saved data ends early");
            return nullptr;
        }
        lua_pushlstring(state, reinterpret_cast<const char*>(cursor), length);
        return cursor + length;
    }
    case tag_fixed: {
        if (static_cast<std::size_t>(stop - cursor) < number_bytes) {
            raise_damaged(state, "the saved data is damaged: saved: the saved data ends early");
            return nullptr;
        }
        uint64_t bits = 0;
        for (std::size_t i = 0; i < number_bytes; ++i)
            bits |= static_cast<uint64_t>(cursor[i]) << (8 * i);
        cursor += number_bytes;
        push_fixed(state, static_cast<int64_t>(bits));
        return cursor;
    }
    case tag_table: {
        if (static_cast<std::size_t>(stop - cursor) < 4) {
            raise_damaged(state, "the saved data is damaged: saved: the saved data ends early");
            return nullptr;
        }
        const uint32_t entries =
            static_cast<uint32_t>(cursor[0]) | (static_cast<uint32_t>(cursor[1]) << 8) |
            (static_cast<uint32_t>(cursor[2]) << 16) | (static_cast<uint32_t>(cursor[3]) << 24);
        cursor += 4;
        int integer_keys = 0;
        int string_keys = 0;
        const uint8_t* scan = cursor;
        for (uint32_t entry = 0; entry < entries; ++entry) {
            if (scan >= stop) {
                raise_damaged(state, "the saved data is damaged: saved: the saved data ends early");
                return nullptr;
            }
            if (*scan == tag_integer)
                ++integer_keys;
            else
                ++string_keys;
            scan = skip_value(scan, stop);
            scan = skip_value(scan, stop);
            if (scan == nullptr) {
                raise_damaged(state, "the saved data is damaged: saved: the saved data ends early");
                return nullptr;
            }
        }
        if (lua_checkstack(state, 4) == 0) {
            luaL_error(state, "the saved data is too large to restore");
            return nullptr;
        }
        lua_createtable(state, integer_keys, string_keys);
        for (uint32_t entry = 0; entry < entries; ++entry) {
            cursor = push_value(state, cursor, stop);
            cursor = push_value(state, cursor, stop);
            lua_rawset(state, -3);
        }
        return cursor;
    }
    default:
        raise_damaged(state, "the saved data is damaged: saved: the value has an unknown tag");
        return nullptr;
    }
}

} // namespace

std::span<const uint8_t> PlainData::bytes() const noexcept {
    return {stored_.data(), stored_.size()};
}

void PlainData::clear() noexcept {
    stored_.clear();
}

bool PlainData::assign(std::span<const uint8_t> bytes) noexcept {
    if (bytes.size() > plain_data_limit)
        return false;
    try {
        std::vector<uint8_t> copy(bytes.begin(), bytes.end());
        stored_.swap(copy);
    } catch (const std::bad_alloc&) {
        return false;
    }
    return true;
}

std::string describe(const PlainDataProblem& problem) {
    if (problem.where.empty())
        return problem.message;
    if (problem.message.empty())
        return problem.where;
    return problem.where + ": " + problem.message;
}

bool encode_plain_data(lua_State* state, int index, PlainData& out, PlainDataProblem& problem) {
    if (state == nullptr)
        return refuse(problem, "saved", "there is no value to save");
    uint64_t charge = 0;
    bool ok = false;
    try {
        ok = encode_collected(state, index, out, problem, charge);
    } catch (const std::bad_alloc&) {
        return refuse(problem, "saved", "the saved data could not be stored");
    }
    if (!ok)
        return false;
    charge_work(state, charge);
    return true;
}

bool check_plain_data(std::span<const uint8_t> bytes, PlainDataProblem& problem) noexcept {
    try {
        return check_collected(bytes, problem);
    } catch (const std::bad_alloc&) {
        problem.where = "saved";
        problem.message = "the saved data could not be checked";
        return false;
    }
}

void push_plain_data(lua_State* state, std::span<const uint8_t> bytes) {
    if (state == nullptr)
        return;
    char text[damage_text_bytes] = {};
    bool damaged = false;
    try {
        PlainDataProblem problem;
        if (!check_plain_data(bytes, problem)) {
            const std::string sentence =
                std::string("the saved data is damaged: ") + describe(problem);
            copy_capped(text, sizeof text, sentence);
            damaged = true;
        }
    } catch (const std::bad_alloc&) {
        copy_capped(
            text,
            sizeof text,
            "the saved data is damaged: saved: the saved data could not be checked"
        );
        damaged = true;
    }
    if (damaged)
        raise_damaged(state, text);
    charge_work(state, units_for_bytes(bytes.size()));
    if (bytes.size() < header_bytes) {
        raise_damaged(state, "the saved data is damaged: saved: the saved data ends early");
        return;
    }
    const uint8_t* cursor = bytes.data() + header_bytes;
    const uint8_t* stop = bytes.data() + bytes.size();
    cursor = push_value(state, cursor, stop);
    if (cursor != stop)
        raise_damaged(state, "the saved data is damaged: saved: bytes follow the value");
}

} // namespace oa::data::script_host

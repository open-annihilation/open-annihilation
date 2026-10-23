// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Single-precision values for scripts. An f32 value holds one binary32
// number. Two f32 values, or an f32 and an integer, use float arithmetic.
// An f32 and a Lua float use double arithmetic and the result is a Lua
// float. A NaN produced by an operation or by narrowing a double is stored
// as the quiet NaN 0x7fc00000, so every build agrees. A value made with
// from_bits keeps the bits it was given.

#include "oa/data/script_host/f32.hpp"

#include "oa/data/script_host/fixed.hpp"

#include "lua.hpp"

#include <cstdint>
#include <cstring>

extern "C" int oa_script_host_number_text(char* buffer, std::size_t size, double value);

namespace oa::data::script_host {
namespace {

char f32_meta_key = 0;

constexpr uint32_t canonical_nan_bits = 0x7fc00000u;

static_assert(sizeof(float) == sizeof(uint32_t));
static_assert(sizeof(lua_Integer) >= sizeof(int64_t));

/// True when the bits are a NaN of either sign.
bool nan_bits(uint32_t bits) noexcept {
    return (bits & 0x7fffffffu) > 0x7f800000u;
}

/// True when the bits are an infinity of either sign.
bool inf_bits(uint32_t bits) noexcept {
    return (bits & 0x7fffffffu) == 0x7f800000u;
}

/// The bits of a float. A NaN becomes the canonical quiet NaN.
uint32_t bits_of_result(float value) noexcept {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    if (nan_bits(bits))
        return canonical_nan_bits;
    return bits;
}

/// The float with these bits, including a NaN payload and a zero's sign.
float float_of_bits(uint32_t bits) noexcept {
    float value = 0;
    std::memcpy(&value, &bits, sizeof value);
    return value;
}

/// Reads the stored bits. False when index is not an f32 value.
bool load_f32(lua_State* state, int index, uint32_t* bits) noexcept {
    if (state == nullptr || bits == nullptr)
        return false;
    if (lua_type(state, index) != LUA_TUSERDATA)
        return false;
    if (lua_checkstack(state, 3) == 0)
        return false;
    if (lua_getmetatable(state, index) == 0)
        return false;
    lua_pushlightuserdata(state, &f32_meta_key);
    lua_rawget(state, LUA_REGISTRYINDEX);
    const int match = lua_rawequal(state, -1, -2);
    lua_pop(state, 2);
    if (match == 0)
        return false;
    const void* memory = lua_touserdata(state, index);
    if (memory == nullptr || lua_rawlen(state, index) < sizeof(uint32_t))
        return false;
    std::memcpy(bits, memory, sizeof(uint32_t));
    return true;
}

/// Pushes an f32 value holding these bits. May raise.
void push_bits(lua_State* state, uint32_t bits) {
    if (lua_checkstack(state, 3) == 0) {
        luaL_error(state, "f32: the single-precision library is not open");
        return;
    }
    lua_pushlightuserdata(state, &f32_meta_key);
    if (lua_rawget(state, LUA_REGISTRYINDEX) != LUA_TTABLE) {
        lua_pop(state, 1);
        luaL_error(state, "f32: the single-precision library is not open");
        return;
    }
    void* memory = lua_newuserdatauv(state, sizeof(uint32_t), 0);
    std::memcpy(memory, &bits, sizeof bits);
    lua_insert(state, -2);
    lua_setmetatable(state, -2);
}

enum class OperandKind : uint8_t { f32_value, integer, number, fixed, other };

/// One arithmetic operand. Every member is trivial, so a raise may pass it.
struct Operand {
    OperandKind kind{OperandKind::other};
    uint32_t bits{};
    int64_t whole{};
    double number{};
};

void read_operand(lua_State* state, int index, Operand* operand) {
    uint32_t bits = 0;
    if (load_f32(state, index, &bits)) {
        operand->kind = OperandKind::f32_value;
        operand->bits = bits;
        return;
    }
    int64_t count = 0;
    if (to_fixed(state, index, &count)) {
        operand->kind = OperandKind::fixed;
        operand->whole = count;
        return;
    }
    if (lua_type(state, index) == LUA_TNUMBER) {
        if (lua_isinteger(state, index) != 0) {
            operand->kind = OperandKind::integer;
            operand->whole = static_cast<int64_t>(lua_tointeger(state, index));
            return;
        }
        operand->kind = OperandKind::number;
        operand->number = lua_tonumber(state, index);
        return;
    }
    operand->kind = OperandKind::other;
}

int raise_fixed(lua_State* state, const char* verb) {
    return luaL_error(
        state,
        "f32: cannot %s a fixed value; f32 arithmetic takes f32 values and numbers; "
        "convert a fixed value with f32(x)",
        verb
    );
}

int raise_type(lua_State* state, const char* verb, int index) {
    const char* type_name = luaL_typename(state, index);
    return luaL_error(
        state, "f32: cannot %s a %s", verb, type_name != nullptr ? type_name : "value"
    );
}

/// The operand as a float. An integer is converted once, not through a double.
float operand_float(const Operand& operand) noexcept {
    if (operand.kind == OperandKind::integer)
        return f32_from_integer(operand.whole);
    return float_of_bits(operand.bits);
}

/// The operand as a double. An f32 widens exactly.
double operand_double(const Operand& operand) noexcept {
    if (operand.kind == OperandKind::number)
        return operand.number;
    return static_cast<double>(float_of_bits(operand.bits));
}

enum class Arithmetic : uint8_t { add, subtract, multiply, divide };

const char* arithmetic_verb(Arithmetic which) noexcept {
    if (which == Arithmetic::subtract)
        return "subtract";
    if (which == Arithmetic::multiply)
        return "multiply";
    if (which == Arithmetic::divide)
        return "divide";
    return "add";
}

/// Float operation of two floats. One rounding, with no fused multiply-add.
float float_operation(Arithmetic which, float left, float right) noexcept {
    if (which == Arithmetic::subtract)
        return left - right;
    if (which == Arithmetic::multiply)
        return left * right;
    if (which == Arithmetic::divide)
        return left / right;
    return left + right;
}

/// Double operation. The result is a Lua float, not an f32.
double double_operation(Arithmetic which, double left, double right) noexcept {
    if (which == Arithmetic::subtract)
        return left - right;
    if (which == Arithmetic::multiply)
        return left * right;
    if (which == Arithmetic::divide)
        return left / right;
    return left + right;
}

int meta_arithmetic(lua_State* state, Arithmetic which) {
    const char* verb = arithmetic_verb(which);
    Operand left;
    Operand right;
    read_operand(state, 1, &left);
    read_operand(state, 2, &right);
    if (left.kind == OperandKind::fixed || right.kind == OperandKind::fixed)
        return raise_fixed(state, verb);
    if (left.kind == OperandKind::other)
        return raise_type(state, verb, 1);
    if (right.kind == OperandKind::other)
        return raise_type(state, verb, 2);
    if (left.kind != OperandKind::f32_value && right.kind != OperandKind::f32_value)
        return raise_type(state, verb, 1);
    if (left.kind == OperandKind::number || right.kind == OperandKind::number) {
        const double widened = double_operation(which, operand_double(left), operand_double(right));
        lua_pushnumber(state, widened);
        return 1;
    }
    const float result = float_operation(which, operand_float(left), operand_float(right));
    push_bits(state, bits_of_result(result));
    return 1;
}

int meta_order(lua_State* state, bool or_equal) {
    Operand left;
    Operand right;
    read_operand(state, 1, &left);
    read_operand(state, 2, &right);
    if (left.kind == OperandKind::fixed || right.kind == OperandKind::fixed)
        return raise_fixed(state, "compare");
    if (left.kind == OperandKind::other)
        return raise_type(state, "compare", 1);
    if (right.kind == OperandKind::other)
        return raise_type(state, "compare", 2);
    if (left.kind != OperandKind::f32_value && right.kind != OperandKind::f32_value)
        return raise_type(state, "compare", 1);
    bool holds = false;
    if (left.kind == OperandKind::number || right.kind == OperandKind::number) {
        const double left_wide = operand_double(left);
        const double right_wide = operand_double(right);
        holds = or_equal ? left_wide <= right_wide : left_wide < right_wide;
    } else {
        const float left_float = operand_float(left);
        const float right_float = operand_float(right);
        holds = or_equal ? left_float <= right_float : left_float < right_float;
    }
    lua_pushboolean(state, holds ? 1 : 0);
    return 1;
}

/// True when the host's number text is an optional sign and digits.
bool integral_spelling(const char* text) noexcept {
    if (text == nullptr || text[0] == '\0')
        return false;
    const char* cursor = text;
    if (*cursor == '-')
        ++cursor;
    if (*cursor == '\0')
        return false;
    for (; *cursor != '\0'; ++cursor) {
        if (*cursor < '0' || *cursor > '9')
            return false;
    }
    return true;
}

int push_converted(lua_State* state, int index) {
    uint32_t bits = 0;
    if (load_f32(state, index, &bits)) {
        push_bits(state, bits);
        return 1;
    }
    int64_t count = 0;
    if (to_fixed(state, index, &count)) {
        push_f32(state, f32_from_fixed_count(count));
        return 1;
    }
    if (lua_type(state, index) != LUA_TNUMBER)
        return luaL_error(state, "f32 takes a number, a fixed value or an f32");
    if (lua_isinteger(state, index) != 0) {
        push_f32(state, f32_from_integer(static_cast<int64_t>(lua_tointeger(state, index))));
        return 1;
    }
    push_f32(state, f32_from_double(lua_tonumber(state, index)));
    return 1;
}

/// True when index 1 is an f32. Raises, then returns false, otherwise.
bool require_f32(lua_State* state, uint32_t* bits, const char* method) {
    if (load_f32(state, 1, bits))
        return true;
    luaL_error(state, "f32:%s expected an f32 value", method);
    return false;
}

extern "C" {

static int meta_add(lua_State* state) {
    return meta_arithmetic(state, Arithmetic::add);
}

static int meta_sub(lua_State* state) {
    return meta_arithmetic(state, Arithmetic::subtract);
}

static int meta_mul(lua_State* state) {
    return meta_arithmetic(state, Arithmetic::multiply);
}

static int meta_div(lua_State* state) {
    return meta_arithmetic(state, Arithmetic::divide);
}

static int meta_unm(lua_State* state) {
    uint32_t bits = 0;
    if (!load_f32(state, 1, &bits))
        return raise_type(state, "negate", 1);
    const float negated = -float_of_bits(bits);
    push_bits(state, bits_of_result(negated));
    return 1;
}

static int meta_eq(lua_State* state) {
    uint32_t left_bits = 0;
    uint32_t right_bits = 0;
    if (!load_f32(state, 1, &left_bits) || !load_f32(state, 2, &right_bits)) {
        lua_pushboolean(state, 0);
        return 1;
    }
    const bool same = float_of_bits(left_bits) == float_of_bits(right_bits);
    lua_pushboolean(state, same ? 1 : 0);
    return 1;
}

static int meta_lt(lua_State* state) {
    return meta_order(state, false);
}

static int meta_le(lua_State* state) {
    return meta_order(state, true);
}

static int meta_tostring(lua_State* state) {
    uint32_t bits = 0;
    if (!require_f32(state, &bits, "tostring"))
        return 0;
    char text[64];
    const int length =
        oa_script_host_number_text(text, sizeof text, static_cast<double>(float_of_bits(bits)));
    if (length < 0 || static_cast<std::size_t>(length) + 3 > sizeof text)
        return luaL_error(state, "f32: tostring failed");
    // Lua adds ".0" when the host text is an integral spelling. An f32 prints
    // the same characters as tostring of its widened value.
    if (integral_spelling(text)) {
        text[length] = '.';
        text[length + 1] = '0';
        text[length + 2] = '\0';
    }
    lua_pushstring(state, text);
    return 1;
}

static int method_bits(lua_State* state) {
    uint32_t bits = 0;
    if (!require_f32(state, &bits, "bits"))
        return 0;
    lua_pushinteger(state, static_cast<lua_Integer>(bits));
    return 1;
}

static int method_tonumber(lua_State* state) {
    uint32_t bits = 0;
    if (!require_f32(state, &bits, "tonumber"))
        return 0;
    lua_pushnumber(state, static_cast<double>(float_of_bits(bits)));
    return 1;
}

static int method_tofixed(lua_State* state) {
    uint32_t bits = 0;
    if (!require_f32(state, &bits, "tofixed"))
        return 0;
    const float value = float_of_bits(bits);
    int64_t count = 0;
    bool fitted = false;
    {
        const std::optional<int64_t> got = f32_to_fixed_count(value);
        fitted = got.has_value();
        if (fitted)
            count = *got;
    }
    if (!fitted) {
        char spelling[64];
        oa_script_host_number_text(spelling, sizeof spelling, static_cast<double>(value));
        if (nan_bits(bits) || inf_bits(bits))
            return luaL_error(state, "f32:tofixed: %s is not a finite number", spelling);
        return luaL_error(
            state,
            "f32:tofixed: %s is outside the range of fixed numbers "
            "(-2147483648 to 2147483647.99998)",
            spelling
        );
    }
    push_fixed(state, count);
    return 1;
}

static int method_isnan(lua_State* state) {
    uint32_t bits = 0;
    if (!require_f32(state, &bits, "isnan"))
        return 0;
    lua_pushboolean(state, nan_bits(bits) ? 1 : 0);
    return 1;
}

static int lib_call(lua_State* state) {
    if (lua_gettop(state) < 2)
        return luaL_error(state, "f32 takes a number, a fixed value or an f32");
    return push_converted(state, 2);
}

static int lib_from_bits(lua_State* state) {
    if (lua_type(state, 1) != LUA_TNUMBER || lua_isinteger(state, 1) == 0)
        return luaL_error(state, "f32.from_bits takes an integer from 0 to 4294967295");
    const lua_Integer raw = lua_tointeger(state, 1);
    if (raw < 0 || static_cast<uint64_t>(raw) > uint64_t{4294967295u})
        return luaL_error(state, "f32.from_bits takes an integer from 0 to 4294967295");
    push_bits(state, static_cast<uint32_t>(raw));
    return 1;
}

} // extern "C"

} // namespace

float f32_from_integer(int64_t value) noexcept {
    // Directly. A double in between would round a second time above 2^53.
    return static_cast<float>(value);
}

float f32_from_double(double value) noexcept {
    const float narrowed = static_cast<float>(value);
    uint32_t bits = 0;
    std::memcpy(&bits, &narrowed, sizeof bits);
    if (!nan_bits(bits))
        return narrowed;
    return float_of_bits(canonical_nan_bits);
}

float f32_from_fixed_count(int64_t count) noexcept {
    // The division is exact for every count in the fixed range. The cast
    // is the one rounding.
    return static_cast<float>(count / 65536.0);
}

std::optional<int64_t> f32_to_fixed_count(float value) noexcept {
    return fixed_from_number(static_cast<double>(value));
}

bool to_f32(lua_State* state, int index, float* value) noexcept {
    uint32_t bits = 0;
    if (!load_f32(state, index, &bits))
        return false;
    std::memcpy(value, &bits, sizeof *value);
    return true;
}

void push_f32(lua_State* state, float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    push_bits(state, bits);
}

void open_f32_library(lua_State* state) {
    lua_newtable(state);
    lua_pushcfunction(state, method_bits);
    lua_setfield(state, -2, "bits");
    lua_pushcfunction(state, method_tonumber);
    lua_setfield(state, -2, "tonumber");
    lua_pushcfunction(state, method_tofixed);
    lua_setfield(state, -2, "tofixed");
    lua_pushcfunction(state, method_isnan);
    lua_setfield(state, -2, "isnan");

    lua_newtable(state);
    lua_pushvalue(state, -2);
    lua_setfield(state, -2, "__index");
    lua_pushcfunction(state, meta_add);
    lua_setfield(state, -2, "__add");
    lua_pushcfunction(state, meta_sub);
    lua_setfield(state, -2, "__sub");
    lua_pushcfunction(state, meta_mul);
    lua_setfield(state, -2, "__mul");
    lua_pushcfunction(state, meta_div);
    lua_setfield(state, -2, "__div");
    lua_pushcfunction(state, meta_unm);
    lua_setfield(state, -2, "__unm");
    lua_pushcfunction(state, meta_eq);
    lua_setfield(state, -2, "__eq");
    lua_pushcfunction(state, meta_lt);
    lua_setfield(state, -2, "__lt");
    lua_pushcfunction(state, meta_le);
    lua_setfield(state, -2, "__le");
    lua_pushcfunction(state, meta_tostring);
    lua_setfield(state, -2, "__tostring");
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "__metatable");

    lua_pushlightuserdata(state, &f32_meta_key);
    lua_pushvalue(state, -2);
    lua_rawset(state, LUA_REGISTRYINDEX);
    lua_pop(state, 1);
    lua_pop(state, 1);

    lua_newtable(state);
    lua_pushcfunction(state, lib_from_bits);
    lua_setfield(state, -2, "from_bits");

    lua_newtable(state);
    lua_pushcfunction(state, lib_call);
    lua_setfield(state, -2, "__call");
    lua_pushboolean(state, 0);
    lua_setfield(state, -2, "__metatable");
    lua_setmetatable(state, -2);
    lua_setglobal(state, "f32");
}

} // namespace oa::data::script_host

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Heading and length through a sandbox, against the engine's own geometry.

#include "lua.hpp"
#include "oa/base/game_math.hpp"
#include "oa/data/script_host/geometry.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/test/check.hpp"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace host = oa::data::script_host;
namespace game_math = oa::base::game_math;

namespace {

constexpr int32_t k_counts[] = {
    0,          1,           -1,        3,          4,
    -4,         12,          100,       -100,       65535,
    -65535,     32768,       -32768,    65536,      -65536,
    196608,     262144,      786432,    2147418112, -2147483647 - 1,
    2147483647, -2147483647, 456698220, 93580,      19410,
};

constexpr int32_t k_length[] = {
    0,
    3,
    4,
    -4,
    12,
    65536,
    2147483647,
    -2147483647 - 1,
};

constexpr int32_t k_scale[] = {0, 65536, -65536, 2147483647};

static_assert(sizeof k_counts / sizeof k_counts[0] >= 20);
static_assert(sizeof k_length / sizeof k_length[0] >= 8);
static_assert(sizeof k_scale / sizeof k_scale[0] >= 4);

struct Package {
    std::map<std::string, std::vector<uint8_t>> files;
};

bool read_package(void* context, std::string_view path, std::vector<uint8_t>& bytes) {
    auto* package = static_cast<Package*>(context);
    const auto found = package->files.find(std::string(path));
    if (found == package->files.end())
        return false;
    bytes = found->second;
    return true;
}

void install_geometry(void*, lua_State* state) {
    host::open_geometry_library(state);
}

host::Profile geometry_profile() {
    host::Profile profile = host::mutator_profile();
    profile.name = "test";
    profile.install = install_geometry;
    return profile;
}

std::optional<host::Sandbox>
open_box(Package& package, host::ScriptError& error, host::Profile profile) {
    host::OpenOptions options;
    options.package_id = "sample";
    options.profile = profile;
    options.sources.context = &package;
    options.sources.read = read_package;
    auto sandbox = host::Sandbox::open(options, error);
    OA_CHECK(sandbox.has_value());
    return sandbox;
}

std::string list_of(const int32_t* values, int count) {
    std::string text = "{";
    for (int index = 0; index < count; ++index) {
        if (index != 0)
            text += ", ";
        text += std::to_string(values[index]);
    }
    text += "}";
    return text;
}

std::string values_source() {
    const int count_n = static_cast<int>(sizeof k_counts / sizeof k_counts[0]);
    const int length_n = static_cast<int>(sizeof k_length / sizeof k_length[0]);
    const int scale_n = static_cast<int>(sizeof k_scale / sizeof k_scale[0]);
    std::string source = R"lua(
local parts = {}
local function add(n)
  parts[#parts + 1] = string.format("%d", n)
end
local function raw(n)
  return fixed.raw(n)
end
local function whole_of(count)
  if count % 65536 ~= 0 then return nil end
  local whole = count // 65536
  if whole < -32768 or whole > 32767 then return nil end
  return whole
end
local function same_pair(fn, x, z)
  local wx, wz = whole_of(x), whole_of(z)
  if wx == nil or wz == nil then return end
  local from_raw = fn(raw(x), raw(z))
  local from_int = fn(wx, wz)
  local from_fixed = fn(fixed(wx), fixed(wz))
  assert(from_raw == from_int)
  assert(from_raw == from_fixed)
end
local function same_length(x, y, z)
  local wx, wy, wz = whole_of(x), whole_of(y), whole_of(z)
  if wx == nil or wy == nil or wz == nil then return end
  local from_raw = geometry.length(raw(x), raw(y), raw(z))
  local from_int = geometry.length(wx, wy, wz)
  local from_fixed = geometry.length(fixed(wx), fixed(wy), fixed(wz))
  assert(from_raw == from_int)
  assert(from_raw == from_fixed)
end
local function same_scale(x, y, z, factor)
  local wx, wy, wz, wf = whole_of(x), whole_of(y), whole_of(z), whole_of(factor)
  if wx == nil or wy == nil or wz == nil or wf == nil then return end
  local ax, ay, az = geometry.scale(raw(x), raw(y), raw(z), raw(factor))
  local bx, by, bz = geometry.scale(wx, wy, wz, wf)
  local cx, cy, cz = geometry.scale(fixed(wx), fixed(wy), fixed(wz), fixed(wf))
  assert(ax == bx and ay == by and az == bz)
  assert(ax == cx and ay == cy and az == cz)
end
)lua";
    source += "local counts = " + list_of(k_counts, count_n) + "\n";
    source += "local lengths = " + list_of(k_length, length_n) + "\n";
    source += "local scales = " + list_of(k_scale, scale_n) + "\n";
    source += R"lua(
for i = 1, #counts do
  for j = 1, #counts do
    local x, z = counts[i], counts[j]
    add(geometry.direction(raw(x), raw(z)))
    add(geometry.planar_length(raw(x), raw(z)):raw())
    same_pair(geometry.direction, x, z)
    same_pair(geometry.planar_length, x, z)
  end
end
for i = 1, #lengths do
  for j = 1, #lengths do
    for k = 1, #lengths do
      local x, y, z = lengths[i], lengths[j], lengths[k]
      add(geometry.length(raw(x), raw(y), raw(z)):raw())
      same_length(x, y, z)
    end
  end
end
add(geometry.length(raw(456698220), raw(93580), raw(19410)):raw())
for i = 1, #scales do
  for j = 1, #scales do
    for k = 1, #scales do
      for n = 1, #scales do
        local x, y, z, factor = scales[i], scales[j], scales[k], scales[n]
        local a, b, c = geometry.scale(raw(x), raw(y), raw(z), raw(factor))
        add(a:raw())
        add(b:raw())
        add(c:raw())
        same_scale(x, y, z, factor)
      end
    end
  end
end
local a, b, c = geometry.scale(raw(131072), raw(-65536), raw(196608), raw(32768))
add(a:raw())
add(b:raw())
add(c:raw())
assert(f32 == nil)
assert(geometry.direction(3, 4) == geometry.direction(fixed(3), fixed(4)))
assert(geometry.planar_length(3, 4) == geometry.planar_length(fixed(3), fixed(4)))
assert(geometry.length(3, 4, 12) == geometry.length(fixed(3), fixed(4), fixed(12)))
local sx, sy, sz = geometry.scale(3, 4, 12, 2)
local tx, ty, tz = geometry.scale(fixed(3), fixed(4), fixed(12), fixed(2))
assert(sx == tx and sy == ty and sz == tz)
return table.concat(parts, ",")
)lua";
    return source;
}

void fill_expected(std::vector<int64_t>& expected) {
    for (const int32_t x : k_counts) {
        for (const int32_t z : k_counts) {
            expected.push_back(static_cast<int64_t>(game_math::direction(x, z)));
            expected.push_back(static_cast<int64_t>(game_math::distance(x, z)));
        }
    }
    for (const int32_t x : k_length) {
        for (const int32_t y : k_length) {
            for (const int32_t z : k_length) {
                expected.push_back(static_cast<int64_t>(game_math::truncated_length(x, y, z)));
            }
        }
    }
    expected.push_back(static_cast<int64_t>(game_math::truncated_length(456698220, 93580, 19410)));
    for (const int32_t x : k_scale) {
        for (const int32_t y : k_scale) {
            for (const int32_t z : k_scale) {
                for (const int32_t factor : k_scale) {
                    const int32_t in[3] = {x, y, z};
                    int32_t scaled[3] = {0, 0, 0};
                    game_math::scale_vector_fixed(scaled, in, factor);
                    expected.push_back(static_cast<int64_t>(scaled[0]));
                    expected.push_back(static_cast<int64_t>(scaled[1]));
                    expected.push_back(static_cast<int64_t>(scaled[2]));
                }
            }
        }
    }
    const int32_t sample_in[3] = {131072, -65536, 196608};
    int32_t sample_out[3] = {0, 0, 0};
    game_math::scale_vector_fixed(sample_out, sample_in, 32768);
    expected.push_back(static_cast<int64_t>(sample_out[0]));
    expected.push_back(static_cast<int64_t>(sample_out[1]));
    expected.push_back(static_cast<int64_t>(sample_out[2]));
}

void read_text(void* context, lua_State* state, int results) {
    auto* out = static_cast<std::string*>(context);
    if (results == 1 && lua_type(state, -1) == LUA_TSTRING && lua_tostring(state, -1) != nullptr)
        *out = lua_tostring(state, -1);
}

bool run_source(
    host::Sandbox& sandbox,
    Package& package,
    const std::string& source,
    host::ScriptError& error,
    const host::CallHooks* hooks
) {
    package.files["mutator.lua"] = std::vector<uint8_t>(source.begin(), source.end());
    return sandbox.run_file("mutator.lua", "plan", hooks, error);
}

void compare_values() {
    OA_CHECK(game_math::truncated_length(0x7fffffff, 0x7fffffff, 0x7fffffff) == -575416511);
    OA_CHECK(game_math::truncated_length(3, 4, 12) == 13);
    OA_CHECK(game_math::truncated_length(456698220, 93580, 19410) == 456698230);
    OA_CHECK(game_math::direction(1, 0) == 16384);
    OA_CHECK(game_math::distance(3, 4) == 5);
    int32_t half_in[3] = {0x20000, -0x10000, 0x30000};
    int32_t half_out[3] = {0, 0, 0};
    game_math::scale_vector_fixed(half_out, half_in, 0x8000);
    OA_CHECK(half_out[0] == 0x10000);
    OA_CHECK(half_out[1] == -0x8000);
    OA_CHECK(half_out[2] == 0x18000);

    std::vector<int64_t> expected;
    fill_expected(expected);
    OA_CHECK(expected.size() >= 300);

    Package package;
    host::ScriptError error;
    auto sandbox = open_box(package, error, geometry_profile());
    if (!sandbox)
        return;
    std::string shown;
    host::CallHooks hooks;
    hooks.context = &shown;
    hooks.read = read_text;
    if (!run_source(*sandbox, package, values_source(), error, &hooks)) {
        std::fprintf(
            stderr, "geometry line %d: %s\n", static_cast<int>(error.line), error.message.c_str()
        );
        OA_CHECK(false);
        return;
    }
    std::size_t index = 0;
    const char* cursor = shown.data();
    const char* end = shown.data() + shown.size();
    while (cursor < end) {
        int64_t parsed = 0;
        const std::from_chars_result got = std::from_chars(cursor, end, parsed);
        if (got.ec != std::errc{} || index >= expected.size() || parsed != expected[index]) {
            std::fprintf(
                stderr,
                "geometry value %zu got %lld want %lld\n",
                index,
                static_cast<long long>(parsed),
                index < expected.size() ? static_cast<long long>(expected[index]) : 0LL
            );
            OA_CHECK(false);
            return;
        }
        ++index;
        cursor = got.ptr;
        if (cursor < end && *cursor == ',')
            ++cursor;
    }
    OA_CHECK(index == expected.size());
}

const char* refusal_source() {
    return R"lua(
local function message(err)
  local text = tostring(err)
  return text:match("^.-:%d+: (.*)$") or text
end
local function expect(fn, wanted)
  local ok, err = pcall(fn)
  assert(not ok, wanted)
  local got = message(err)
  assert(got == wanted, got)
end

geometry.extra = 1
assert(geometry.extra == 1)
assert(math.type(geometry.direction(0, 1)) == "integer")
assert(geometry.direction(-32768, 32767) == geometry.direction(fixed(-32768), fixed(32767)))
local edge = geometry.length(fixed.raw(2147483647), fixed.raw(-2147483648), 0)
assert(type(edge:raw()) == "number")
assert(geometry.planar_length(-32768, 0):raw() ==
  geometry.planar_length(fixed.raw(-2147483648), 0):raw())

local outside = " is outside -32768 to 32767.99998"
expect(function() return geometry.direction(32768, 0) end,
  "geometry.direction: argument 1" .. outside)
expect(function() return geometry.direction(0, 32768) end,
  "geometry.direction: argument 2" .. outside)
expect(function() return geometry.direction(-32769, 0) end,
  "geometry.direction: argument 1" .. outside)
expect(function() return geometry.length(0, 32768, 0) end,
  "geometry.length: argument 2" .. outside)
expect(function() return geometry.scale(0, 0, 0, 32768) end,
  "geometry.scale: argument 4" .. outside)
expect(function() return geometry.planar_length(fixed.raw(2147483648), 0) end,
  "geometry.planar_length: argument 1" .. outside)
expect(function() return geometry.planar_length(0, fixed.raw(-2147483649)) end,
  "geometry.planar_length: argument 2" .. outside)

local needed = " must be a fixed value or an integer"
expect(function() return geometry.direction(1.5, 0) end,
  "geometry.direction: argument 1" .. needed)
expect(function() return geometry.direction(1.0, 0) end,
  "geometry.direction: argument 1" .. needed)
expect(function() return geometry.direction(0, 1.5) end,
  "geometry.direction: argument 2" .. needed)
expect(function() return geometry.length(0, true, 0) end,
  "geometry.length: argument 2" .. needed)
expect(function() return geometry.scale(0, 0, 0, "1") end,
  "geometry.scale: argument 4" .. needed)
expect(function() return geometry.planar_length({}, 0) end,
  "geometry.planar_length: argument 1" .. needed)
expect(function() return geometry.direction(0) end,
  "geometry.direction: argument 2" .. needed)
expect(function() return geometry.direction(0, nil) end,
  "geometry.direction: argument 2" .. needed)
expect(function() return geometry.length(0, 0) end,
  "geometry.length: argument 3" .. needed)
return true
)lua";
}

void check_refusals() {
    Package package;
    host::ScriptError error;
    auto sandbox = open_box(package, error, geometry_profile());
    if (!sandbox)
        return;
    if (!run_source(*sandbox, package, refusal_source(), error, nullptr)) {
        std::fprintf(
            stderr, "refusal line %d: %s\n", static_cast<int>(error.line), error.message.c_str()
        );
        OA_CHECK(false);
    }
}

void mutator_has_no_geometry() {
    Package package;
    host::ScriptError error;
    auto sandbox = open_box(package, error, host::mutator_profile());
    if (!sandbox)
        return;
    std::string shown;
    host::CallHooks hooks;
    hooks.context = &shown;
    hooks.read = read_text;
    if (!run_source(*sandbox, package, "return type(geometry)\n", error, &hooks)) {
        std::fprintf(stderr, "mutator: %s\n", error.message.c_str());
        OA_CHECK(false);
        return;
    }
    OA_CHECK(shown == "nil");
}

} // namespace

int main() {
    compare_values();
    check_refusals();
    mutator_has_no_geometry();
    return oa::test::check_exit_status();
}

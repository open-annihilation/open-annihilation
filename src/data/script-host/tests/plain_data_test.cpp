// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Plain data: canonical bytes, a round trip, the refusals, and the charge.
// Every Lua value is built in a sandbox whose profile is named "test".

#include "lua.hpp"
#include "oa/data/script_host/f32.hpp"
#include "oa/data/script_host/fixed.hpp"
#include "oa/data/script_host/plain_data.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/test/check.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace host = oa::data::script_host;

namespace {

constexpr uint8_t nil_bytes[] = {0x4f, 0x41, 0x50, 0x44, 0x01, 0x00};

constexpr uint8_t true_bytes[] = {0x4f, 0x41, 0x50, 0x44, 0x01, 0x02};

constexpr uint8_t zero_bytes[] = {
    0x4f, 0x41, 0x50, 0x44, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
};

constexpr uint8_t minus_one_bytes[] = {
    0x4f, 0x41, 0x50, 0x44, 0x01, 0x03, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff
};

constexpr uint8_t one_and_a_half_bytes[] = {
    0x4f, 0x41, 0x50, 0x44, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf8, 0x3f
};

constexpr uint8_t negative_zero_bytes[] = {
    0x4f, 0x41, 0x50, 0x44, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80
};

constexpr uint8_t nan_bytes[] = {
    0x4f, 0x41, 0x50, 0x44, 0x01, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xf8, 0x7f
};

constexpr uint8_t empty_text_bytes[] = {0x4f, 0x41, 0x50, 0x44, 0x01, 0x05, 0x00, 0x00, 0x00, 0x00};

constexpr uint8_t ab_bytes[] = {
    0x4f, 0x41, 0x50, 0x44, 0x01, 0x05, 0x02, 0x00, 0x00, 0x00, 'a', 'b'
};

// fixed(1) is 65536 counts of 65536ths.
constexpr uint8_t fixed_one_bytes[] = {
    0x4f, 0x41, 0x50, 0x44, 0x01, 0x06, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00
};

constexpr uint8_t empty_table_bytes[] = {
    0x4f, 0x41, 0x50, 0x44, 0x01, 0x07, 0x00, 0x00, 0x00, 0x00
};

constexpr uint8_t list_bytes[] = {
    0x4f, 0x41, 0x50, 0x44, 0x01, 0x07, 0x02, 0x00, 0x00, 0x00, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x02, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x03, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// Integer key 3, then "a", then "b".
constexpr uint8_t mixed_bytes[] = {
    0x4f, 0x41, 0x50, 0x44, 0x01, 0x07, 0x03, 0x00, 0x00, 0x00, 0x03, 0x03, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x02, 0x05, 0x01, 0x00, 0x00, 0x00, 'a',
    0x03, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x05, 0x01, 0x00, 0x00,
    0x00, 'b',  0x03, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

// Header, table header, 15 strings of 65,536 and one tail. An integer key is 9 bytes.
constexpr std::size_t big_plain(std::size_t tail) {
    constexpr std::size_t key_bytes = 9;
    constexpr std::size_t fixed_part = 10;
    const std::size_t long_entry = key_bytes + 5 + 65536;
    const std::size_t tail_entry = key_bytes + 5 + tail;
    return fixed_part + 15 * long_entry + tail_entry;
}

static_assert(big_plain(65302) == 1048576);
static_assert(big_plain(65303) == 1048577);
static_assert(host::plain_data_limit == 1048576);
static_assert(host::plain_data_depth_limit == 64);

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

void install_test(void*, lua_State* state) {
    host::open_f32_library(state);
}

extern "C" int make_foreign(lua_State* state) {
    void* memory = lua_newuserdatauv(state, sizeof(int), 0);
    if (memory != nullptr)
        std::memset(memory, 0, sizeof(int));
    return 1;
}

void register_foreign(void*, lua_State* state) {
    lua_pushcfunction(state, make_foreign);
    lua_setglobal(state, "foreign");
}

host::Profile test_profile() {
    host::Profile profile = host::mutator_profile();
    profile.name = "test";
    profile.install = install_test;
    return profile;
}

std::optional<host::Sandbox> open_test(Package& package, host::ScriptError& error) {
    host::OpenOptions options;
    options.package_id = "sample";
    options.profile = test_profile();
    options.sources.context = &package;
    options.sources.read = read_package;
    auto sandbox = host::Sandbox::open(options, error);
    OA_CHECK(sandbox.has_value());
    if (!sandbox)
        return std::nullopt;
    if (!sandbox->run_host("host", nullptr, register_foreign, error)) {
        std::fprintf(stderr, "%s\n", host::describe(error).c_str());
        OA_CHECK(false);
        return std::nullopt;
    }
    return sandbox;
}

bool run_chunk(
    host::Sandbox& sandbox, Package& package, const char* source, host::ScriptError& error
) {
    const std::size_t length = std::strlen(source);
    package.files["mutator.lua"] = std::vector<uint8_t>(source, source + length);
    const bool ran = sandbox.run_file("mutator.lua", "plan", nullptr, error);
    if (!ran)
        std::fprintf(stderr, "%s\n", host::describe(error).c_str());
    return ran;
}

struct Job {
    const char* name = "value";
    host::PlainData data;
    host::PlainDataProblem problem;
    bool ok = false;
};

void encode_job(void* context, lua_State* state) {
    auto* job = static_cast<Job*>(context);
    lua_getglobal(state, job->name);
    job->ok = host::encode_plain_data(state, -1, job->data, job->problem);
    lua_pop(state, 1);
}

bool encode_named(host::Sandbox& sandbox, const char* name, Job& job, host::ScriptError& error) {
    job.name = name;
    job.ok = false;
    const bool ran = sandbox.run_host("host", &job, encode_job, error);
    if (!ran)
        std::fprintf(stderr, "%s\n", host::describe(error).c_str());
    return ran;
}

void expect_bytes(std::span<const uint8_t> got, const uint8_t* expected, std::size_t size) {
    const bool match =
        got.size() == size && (size == 0 || std::memcmp(got.data(), expected, size) == 0);
    if (!match) {
        std::fprintf(stderr, "plain data bytes differ (got %zu, expected %zu)\n", got.size(), size);
        const std::size_t shown = got.size() < 48 ? got.size() : 48;
        for (std::size_t i = 0; i < shown; ++i)
            std::fprintf(stderr, "%02x", got[i]);
        std::fprintf(stderr, "\n");
    }
    OA_CHECK(match);
}

void append_u32(std::vector<uint8_t>& out, uint32_t value) {
    out.push_back(static_cast<uint8_t>(value));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 24));
}

void append_i64(std::vector<uint8_t>& out, int64_t value) {
    const auto bits = static_cast<uint64_t>(value);
    for (int shift = 0; shift < 8; ++shift)
        out.push_back(static_cast<uint8_t>((bits >> (8 * shift)) & 0xffu));
}

void append_header(std::vector<uint8_t>& out) {
    const uint8_t header[] = {0x4f, 0x41, 0x50, 0x44, 0x01};
    out.insert(out.end(), header, header + sizeof header);
}

bool host_little_endian() {
    const uint16_t one = 1;
    unsigned char raw[2] = {};
    std::memcpy(raw, &one, sizeof raw);
    return raw[0] == 1;
}

uint64_t bits_of_double(double value) {
    unsigned char raw[8] = {};
    std::memcpy(raw, &value, 8);
    if (!host_little_endian()) {
        for (int i = 0; i < 4; ++i) {
            const unsigned char swap = raw[i];
            raw[i] = raw[7 - i];
            raw[7 - i] = swap;
        }
    }
    uint64_t bits = 0;
    for (int i = 0; i < 8; ++i)
        bits |= static_cast<uint64_t>(raw[i]) << (8 * i);
    return bits;
}

bool type_is(lua_State* state, int index, const char* expected) {
    const int absolute = lua_absindex(state, index);
    lua_getglobal(state, "math");
    lua_getfield(state, -1, "type");
    lua_remove(state, -2);
    lua_pushvalue(state, absolute);
    if (lua_pcall(state, 1, 1, 0) != LUA_OK) {
        lua_pop(state, 1);
        return false;
    }
    const char* text = lua_tolstring(state, -1, nullptr);
    const bool match = text != nullptr && std::strcmp(text, expected) == 0;
    lua_pop(state, 1);
    return match;
}

bool field_integer(lua_State* state, int table, const char* name, lua_Integer expected) {
    lua_getfield(state, table, name);
    int is_number = 0;
    const lua_Integer whole = lua_tointegerx(state, -1, &is_number);
    const bool match = is_number != 0 && lua_isinteger(state, -1) != 0 && whole == expected;
    lua_pop(state, 1);
    return match;
}

bool slot_integer(lua_State* state, int table, int slot, lua_Integer expected) {
    lua_rawgeti(state, table, slot);
    int is_number = 0;
    const lua_Integer whole = lua_tointegerx(state, -1, &is_number);
    const bool match = is_number != 0 && lua_isinteger(state, -1) != 0 && whole == expected;
    lua_pop(state, 1);
    return match;
}

bool field_bool(lua_State* state, int table, const char* name, bool expected) {
    lua_getfield(state, table, name);
    const bool match =
        lua_type(state, -1) == LUA_TBOOLEAN && (lua_toboolean(state, -1) != 0) == expected;
    lua_pop(state, 1);
    return match;
}

bool field_text(lua_State* state, int table, const char* name, const char* expected) {
    lua_getfield(state, table, name);
    std::size_t length = 0;
    const char* text = lua_tolstring(state, -1, &length);
    const bool match = text != nullptr && length == std::strlen(expected) &&
                       std::memcmp(text, expected, length) == 0;
    lua_pop(state, 1);
    return match;
}

bool field_bits(lua_State* state, int table, const char* name, uint64_t expected) {
    lua_getfield(state, table, name);
    int is_number = 0;
    const double value = lua_tonumberx(state, -1, &is_number);
    const bool match =
        is_number != 0 && lua_isinteger(state, -1) == 0 && bits_of_double(value) == expected;
    lua_pop(state, 1);
    return match;
}

bool field_fixed(lua_State* state, int table, const char* name, int64_t expected) {
    lua_getfield(state, table, name);
    int64_t count = 0;
    const bool match = host::to_fixed(state, -1, &count) && count == expected;
    lua_pop(state, 1);
    return match;
}

struct RoundTrip {
    host::PlainData first;
    host::PlainData second;
    host::PlainDataProblem problem;
    bool encoded = false;
    bool again = false;
};

void inspect_round_trip(lua_State* state, int index) {
    const int value = lua_absindex(state, index);
    OA_CHECK(lua_type(state, value) == LUA_TTABLE);
    OA_CHECK(field_bool(state, value, "flag", true));
    OA_CHECK(field_bool(state, value, "no", false));
    OA_CHECK(field_integer(state, value, "whole", 0));
    OA_CHECK(field_integer(state, value, "negative", -1));
    lua_getfield(state, value, "whole");
    OA_CHECK(type_is(state, -1, "integer"));
    lua_pop(state, 1);
    lua_getfield(state, value, "fraction");
    OA_CHECK(type_is(state, -1, "float"));
    OA_CHECK(lua_isinteger(state, -1) == 0);
    OA_CHECK(bits_of_double(lua_tonumberx(state, -1, nullptr)) == 0x3ff8000000000000ull);
    lua_pop(state, 1);
    lua_getfield(state, value, "negzero");
    OA_CHECK(type_is(state, -1, "float"));
    OA_CHECK(bits_of_double(lua_tonumberx(state, -1, nullptr)) == 0x8000000000000000ull);
    lua_pop(state, 1);
    OA_CHECK(field_bits(state, value, "missing", 0x7ff8000000000000ull));
    OA_CHECK(field_bits(state, value, "huge", 0x7ff0000000000000ull));
    OA_CHECK(field_bits(state, value, "below", 0xfff0000000000000ull));
    OA_CHECK(field_text(state, value, "blank", ""));
    OA_CHECK(field_text(state, value, "text", "ab"));
    OA_CHECK(field_fixed(state, value, "count", 65536));
    lua_getfield(state, value, "list");
    OA_CHECK(slot_integer(state, -1, 1, 1));
    OA_CHECK(slot_integer(state, -1, 2, 2));
    lua_pop(state, 1);
    lua_getfield(state, value, "inner");
    OA_CHECK(field_integer(state, -1, "a", 2));
    OA_CHECK(field_integer(state, -1, "b", 1));
    OA_CHECK(slot_integer(state, -1, 3, 1) == false);
    lua_rawgeti(state, -1, 3);
    OA_CHECK(lua_type(state, -1) == LUA_TBOOLEAN && lua_toboolean(state, -1) != 0);
    lua_pop(state, 2);
    OA_CHECK(field_text(state, value, "x-y", "z"));
    lua_rawgeti(state, value, -1);
    OA_CHECK(lua_type(state, -1) == LUA_TBOOLEAN && lua_toboolean(state, -1) == 0);
    lua_pop(state, 1);

    lua_getfield(state, value, "pair");
    lua_rawgeti(state, -1, 1);
    lua_rawgeti(state, -2, 2);
    OA_CHECK(lua_rawequal(state, -1, -2) == 0);
    OA_CHECK(slot_integer(state, -1, 1, 7));
    OA_CHECK(slot_integer(state, -2, 1, 7));
    lua_pop(state, 3);
}

void round_work(void* context, lua_State* state) {
    auto* trip = static_cast<RoundTrip*>(context);
    lua_getglobal(state, "value");
    trip->encoded = host::encode_plain_data(state, -1, trip->first, trip->problem);
    lua_pop(state, 1);
    if (!trip->encoded)
        return;
    host::push_plain_data(state, trip->first.bytes());
    inspect_round_trip(state, -1);
    trip->again = host::encode_plain_data(state, -1, trip->second, trip->problem);
    lua_pop(state, 1);
}

void exact_bytes() {
    struct Sample {
        const char* source;
        const uint8_t* bytes;
        std::size_t size;
    };

    const Sample samples[] = {
        {"value = nil\n", nil_bytes, sizeof nil_bytes},
        {"value = true\n", true_bytes, sizeof true_bytes},
        {"value = 0\n", zero_bytes, sizeof zero_bytes},
        {"value = -1\n", minus_one_bytes, sizeof minus_one_bytes},
        {"value = 1.5\n", one_and_a_half_bytes, sizeof one_and_a_half_bytes},
        {"value = -0.0\n", negative_zero_bytes, sizeof negative_zero_bytes},
        {"value = 0/0\n", nan_bytes, sizeof nan_bytes},
        {"value = \"\"\n", empty_text_bytes, sizeof empty_text_bytes},
        {"value = \"ab\"\n", ab_bytes, sizeof ab_bytes},
        {"value = fixed(1)\n", fixed_one_bytes, sizeof fixed_one_bytes},
        {"value = {}\n", empty_table_bytes, sizeof empty_table_bytes},
        {"value = {1, 2}\n", list_bytes, sizeof list_bytes},
        {"value = {b = 1, a = 2, [3] = true}\n", mixed_bytes, sizeof mixed_bytes},
    };
    for (const Sample& sample : samples) {
        Package package;
        host::ScriptError error;
        auto sandbox = open_test(package, error);
        if (!sandbox)
            return;
        OA_CHECK(run_chunk(*sandbox, package, sample.source, error));
        Job job;
        OA_CHECK(encode_named(*sandbox, "value", job, error));
        if (!job.ok)
            std::fprintf(stderr, "%s\n", host::describe(job.problem).c_str());
        OA_CHECK(job.ok);
        expect_bytes(job.data.bytes(), sample.bytes, sample.size);
        host::PlainDataProblem problem;
        OA_CHECK(host::check_plain_data(job.data.bytes(), problem));
    }
}

void same_bytes_whatever_the_fill_order() {
    const char* source = R"lua(
local function fill(which)
  local t = {}
  if which == 1 then
    t.b = 1
    t.a = 2
    t[3] = true
  elseif which == 2 then
    t[3] = true
    t.a = 2
    t.b = 1
  elseif which == 3 then
    t.a = 2
    t.b = 1
    t.extra = 9
    t.extra = nil
    t[3] = true
  elseif which == 4 then
    t[3] = false
    t.b = 1
    t.a = 2
    t[3] = true
  else
    t.b = 1
    t[4] = 1
    t.a = 2
    t[4] = nil
    t[3] = true
  end
  return t
end
one = fill(1)
two = fill(2)
three = fill(3)
four = fill(4)
five = fill(5)
)lua";
    Package package;
    host::ScriptError error;
    auto sandbox = open_test(package, error);
    if (!sandbox)
        return;
    OA_CHECK(run_chunk(*sandbox, package, source, error));
    const char* names[] = {"one", "two", "three", "four", "five"};
    for (const char* name : names) {
        Job job;
        OA_CHECK(encode_named(*sandbox, name, job, error));
        OA_CHECK(job.ok);
        expect_bytes(job.data.bytes(), mixed_bytes, sizeof mixed_bytes);
    }

    Package other_package;
    host::ScriptError other_error;
    auto other = open_test(other_package, other_error);
    if (!other)
        return;
    OA_CHECK(run_chunk(*other, other_package, source, other_error));
    Job again;
    OA_CHECK(encode_named(*other, "one", again, other_error));
    OA_CHECK(again.ok);
    expect_bytes(again.data.bytes(), mixed_bytes, sizeof mixed_bytes);
}

void round_trip() {
    const char* source = R"lua(
local shared = {7}
value = {
  flag = true,
  no = false,
  whole = 0,
  negative = -1,
  fraction = 1.5,
  negzero = -0.0,
  missing = 0/0,
  huge = math.huge,
  below = -math.huge,
  blank = "",
  text = "ab",
  count = fixed(1),
  list = {1, 2},
  inner = {b = 1, a = 2, [3] = true},
  ["x-y"] = "z",
  [-1] = false,
  pair = {shared, shared},
}
)lua";
    Package package;
    host::ScriptError error;
    auto sandbox = open_test(package, error);
    if (!sandbox)
        return;
    OA_CHECK(run_chunk(*sandbox, package, source, error));
    RoundTrip trip;
    OA_CHECK(sandbox->run_host("host", &trip, round_work, error));
    if (!trip.encoded)
        std::fprintf(stderr, "%s\n", host::describe(trip.problem).c_str());
    OA_CHECK(trip.encoded);
    OA_CHECK(trip.again);
    OA_CHECK(trip.first.bytes().size() == trip.second.bytes().size());
    OA_CHECK(
        trip.first.bytes().size() == 0 ||
        std::memcmp(
            trip.first.bytes().data(), trip.second.bytes().data(), trip.first.bytes().size()
        ) == 0
    );
    host::PlainDataProblem problem;
    OA_CHECK(host::check_plain_data(trip.first.bytes(), problem));
}

void expect_refusal(const char* source, const char* name, const char* where, const char* message) {
    Package package;
    host::ScriptError error;
    auto sandbox = open_test(package, error);
    if (!sandbox)
        return;
    OA_CHECK(run_chunk(*sandbox, package, source, error));
    Job job;
    OA_CHECK(encode_named(*sandbox, name, job, error));
    OA_CHECK(!job.ok);
    OA_CHECK(job.problem.where == where);
    OA_CHECK(job.problem.message == message);
    OA_CHECK(job.data.bytes().empty());
    OA_CHECK(sandbox->instructions_used() == 0);
    OA_CHECK(host::describe(job.problem) == std::string(where) + ": " + message);
}

void refusals() {
    expect_refusal(
        "value = { units = { [3] = function() end } }\n",
        "value",
        "saved.units[3]",
        "a function cannot be saved"
    );
    expect_refusal("value = foreign()\n", "value", "saved", "userdata cannot be saved");
    expect_refusal(
        "value = { reading = f32(1) }\n", "value", "saved.reading", "userdata cannot be saved"
    );
    expect_refusal(
        "value = setmetatable({}, {})\n",
        "value",
        "saved",
        "a table with a metatable cannot be saved"
    );
    expect_refusal(
        "local t = {}\nt.self = t\nvalue = t\n",
        "value",
        "saved.self",
        "a table that contains itself cannot be saved"
    );
    expect_refusal("value = {[true] = 1}\n", "value", "saved", "a boolean key cannot be saved");
    expect_refusal("value = {[1.5] = true}\n", "value", "saved", "a float key cannot be saved");

    const char* nest = R"lua(
local function nest(levels)
  local top = {}
  local cursor = top
  for _ = 2, levels do
    local inner = {}
    cursor[1] = inner
    cursor = inner
  end
  return top
end
shallow = nest(64)
deep = nest(65)
)lua";
    Package package;
    host::ScriptError error;
    auto sandbox = open_test(package, error);
    if (!sandbox)
        return;
    OA_CHECK(run_chunk(*sandbox, package, nest, error));
    Job shallow;
    OA_CHECK(encode_named(*sandbox, "shallow", shallow, error));
    OA_CHECK(shallow.ok);
    host::PlainDataProblem checked;
    OA_CHECK(host::check_plain_data(shallow.data.bytes(), checked));
    Job deep;
    OA_CHECK(encode_named(*sandbox, "deep", deep, error));
    OA_CHECK(!deep.ok);
    std::string deep_where = "saved";
    for (int level = 0; level < 64; ++level)
        deep_where += "[1]";
    OA_CHECK(deep.problem.where == deep_where);
    OA_CHECK(deep.problem.message == "the saved data is nested more than 64 deep");
    OA_CHECK(sandbox->instructions_used() == 0);

    const char* sizes = R"lua(
local function big(tail_len)
  local body = string.rep("x", 65536)
  local tail = string.rep("y", tail_len)
  local t = {}
  for i = 1, 15 do
    t[i] = body
  end
  t[16] = tail
  return t
end
fits = big(65302)
over = big(65303)
)lua";
    Package sized;
    host::ScriptError size_error;
    auto size_box = open_test(sized, size_error);
    if (!size_box)
        return;
    OA_CHECK(run_chunk(*size_box, sized, sizes, size_error));
    Job fits;
    OA_CHECK(encode_named(*size_box, "fits", fits, size_error));
    if (!fits.ok)
        std::fprintf(stderr, "%s\n", host::describe(fits.problem).c_str());
    OA_CHECK(fits.ok);
    OA_CHECK(fits.data.bytes().size() == host::plain_data_limit);
    OA_CHECK(host::check_plain_data(fits.data.bytes(), checked));
    const uint64_t large_charge = size_box->instructions_used();
    Job over;
    OA_CHECK(encode_named(*size_box, "over", over, size_error));
    OA_CHECK(!over.ok);
    OA_CHECK(over.problem.where == "saved[16]");
    OA_CHECK(over.problem.message == "the saved data is larger than 1 MiB");
    OA_CHECK(over.data.bytes().empty());
    OA_CHECK(size_box->instructions_used() == 0);
    Job small;
    OA_CHECK(run_chunk(*size_box, sized, "value = nil\n", size_error));
    OA_CHECK(encode_named(*size_box, "value", small, size_error));
    OA_CHECK(small.ok);
    OA_CHECK(large_charge > size_box->instructions_used());
}

void check_refuses_damaged_bytes() {
    host::PlainDataProblem problem;
    for (std::size_t length = 0; length < sizeof mixed_bytes; ++length) {
        const std::span<const uint8_t> prefix(mixed_bytes, length);
        OA_CHECK(!host::check_plain_data(prefix, problem));
    }
    OA_CHECK(
        host::check_plain_data(std::span<const uint8_t>(mixed_bytes, sizeof mixed_bytes), problem)
    );

    std::vector<uint8_t> flipped(mixed_bytes, mixed_bytes + sizeof mixed_bytes);
    flipped[5] = 0x0f;
    OA_CHECK(!host::check_plain_data(flipped, problem));
    OA_CHECK(problem.where == "saved");
    OA_CHECK(problem.message == "the value has an unknown tag");

    std::vector<uint8_t> wrong_version(nil_bytes, nil_bytes + sizeof nil_bytes);
    wrong_version[4] = 2;
    OA_CHECK(!host::check_plain_data(wrong_version, problem));
    OA_CHECK(problem.message == "the header is not plain data");

    // String keys "b" then "a".
    std::vector<uint8_t> unsorted;
    append_header(unsorted);
    unsorted.push_back(0x07);
    append_u32(unsorted, 2);
    unsorted.push_back(0x05);
    append_u32(unsorted, 1);
    unsorted.push_back('b');
    unsorted.push_back(0x03);
    append_i64(unsorted, 1);
    unsorted.push_back(0x05);
    append_u32(unsorted, 1);
    unsorted.push_back('a');
    unsorted.push_back(0x03);
    append_i64(unsorted, 2);
    OA_CHECK(!host::check_plain_data(unsorted, problem));
    OA_CHECK(problem.where == "saved");
    OA_CHECK(problem.message == "the table keys are not in order");

    std::vector<uint8_t> integers_backwards;
    append_header(integers_backwards);
    integers_backwards.push_back(0x07);
    append_u32(integers_backwards, 2);
    integers_backwards.push_back(0x03);
    append_i64(integers_backwards, 2);
    integers_backwards.push_back(0x02);
    integers_backwards.push_back(0x03);
    append_i64(integers_backwards, 1);
    integers_backwards.push_back(0x01);
    OA_CHECK(!host::check_plain_data(integers_backwards, problem));
    OA_CHECK(problem.message == "the table keys are not in order");

    std::vector<uint8_t> repeated;
    append_header(repeated);
    repeated.push_back(0x07);
    append_u32(repeated, 2);
    repeated.push_back(0x03);
    append_i64(repeated, 1);
    repeated.push_back(0x02);
    repeated.push_back(0x03);
    append_i64(repeated, 1);
    repeated.push_back(0x01);
    OA_CHECK(!host::check_plain_data(repeated, problem));
    OA_CHECK(problem.where == "saved");
    OA_CHECK(problem.message == "the table has a repeated key");

    std::vector<uint8_t> long_text;
    append_header(long_text);
    long_text.push_back(0x05);
    append_u32(long_text, 65537);
    long_text.insert(long_text.end(), 65537, static_cast<uint8_t>('q'));
    OA_CHECK(!host::check_plain_data(long_text, problem));
    OA_CHECK(problem.where == "saved");
    OA_CHECK(problem.message == "a string is longer than 65536 bytes");

    std::vector<uint8_t> high_fixed;
    append_header(high_fixed);
    high_fixed.push_back(0x06);
    append_i64(high_fixed, host::fixed_count_max + 1);
    OA_CHECK(!host::check_plain_data(high_fixed, problem));
    OA_CHECK(problem.message == "a fixed count is outside the fixed range");
    std::vector<uint8_t> low_fixed;
    append_header(low_fixed);
    low_fixed.push_back(0x06);
    append_i64(low_fixed, host::fixed_count_min - 1);
    OA_CHECK(!host::check_plain_data(low_fixed, problem));
    OA_CHECK(problem.message == "a fixed count is outside the fixed range");
    std::vector<uint8_t> top_fixed;
    append_header(top_fixed);
    top_fixed.push_back(0x06);
    append_i64(top_fixed, host::fixed_count_max);
    OA_CHECK(host::check_plain_data(top_fixed, problem));
    std::vector<uint8_t> bottom_fixed;
    append_header(bottom_fixed);
    bottom_fixed.push_back(0x06);
    append_i64(bottom_fixed, host::fixed_count_min);
    OA_CHECK(host::check_plain_data(bottom_fixed, problem));

    std::vector<uint8_t> trailing(nil_bytes, nil_bytes + sizeof nil_bytes);
    trailing.push_back(0x00);
    OA_CHECK(!host::check_plain_data(trailing, problem));
    OA_CHECK(problem.where == "saved");
    OA_CHECK(problem.message == "bytes follow the value");

    std::vector<uint8_t> nested_nil;
    append_header(nested_nil);
    nested_nil.push_back(0x07);
    append_u32(nested_nil, 1);
    nested_nil.push_back(0x03);
    append_i64(nested_nil, 1);
    nested_nil.push_back(0x00);
    OA_CHECK(!host::check_plain_data(nested_nil, problem));
    OA_CHECK(problem.where == "saved[1]");
    OA_CHECK(problem.message == "only the top value may be nil");

    std::vector<uint8_t> odd_nan;
    append_header(odd_nan);
    odd_nan.push_back(0x04);
    append_i64(odd_nan, static_cast<int64_t>(0x7ff8000000000001ull));
    OA_CHECK(!host::check_plain_data(odd_nan, problem));
    OA_CHECK(problem.message == "a NaN is not in canonical form");

    std::vector<uint8_t> huge(host::plain_data_limit + 1, 0);
    OA_CHECK(!host::check_plain_data(huge, problem));
    OA_CHECK(problem.message == "the saved data is larger than 1 MiB");
}

void push_only(void* context, lua_State* state) {
    auto* bytes = static_cast<std::vector<uint8_t>*>(context);
    host::push_plain_data(state, *bytes);
}

void push_of_damaged_bytes_fails_the_phase() {
    Package package;
    host::ScriptError error;
    auto sandbox = open_test(package, error);
    if (!sandbox)
        return;
    std::vector<uint8_t> damaged(nil_bytes, nil_bytes + 5);
    OA_CHECK(!sandbox->run_host("host", &damaged, push_only, error));
    OA_CHECK(error.kind == host::ErrorKind::runtime);
    OA_CHECK(error.message == "the saved data is damaged: saved: the saved data ends early");
    OA_CHECK(error.chunk.empty());
    OA_CHECK(error.line == 0);
    OA_CHECK(sandbox->failed());
}

struct PushJob {
    host::PlainData data;
};

void push_saved(void* context, lua_State* state) {
    auto* job = static_cast<PushJob*>(context);
    host::push_plain_data(state, job->data.bytes());
    lua_pop(state, 1);
}

void charges() {
    Package package;
    host::ScriptError error;
    auto sandbox = open_test(package, error);
    if (!sandbox)
        return;
    OA_CHECK(run_chunk(*sandbox, package, "value = nil\n", error));
    Job first;
    OA_CHECK(encode_named(*sandbox, "value", first, error));
    OA_CHECK(first.ok);
    const uint64_t nil_charge = sandbox->instructions_used();
    OA_CHECK(nil_charge == host::units_for_bytes(first.data.bytes().size()) + host::element_charge);
    Job second;
    OA_CHECK(encode_named(*sandbox, "value", second, error));
    OA_CHECK(second.ok);
    OA_CHECK(sandbox->instructions_used() == nil_charge);

    OA_CHECK(run_chunk(*sandbox, package, "value = {b = 1, a = 2, [3] = true}\n", error));
    Job mixed;
    OA_CHECK(encode_named(*sandbox, "value", mixed, error));
    OA_CHECK(mixed.ok);
    OA_CHECK(sandbox->instructions_used() > nil_charge);

    PushJob pushed;
    OA_CHECK(pushed.data.assign(first.data.bytes()));
    OA_CHECK(sandbox->run_host("host", &pushed, push_saved, error));
    OA_CHECK(sandbox->instructions_used() == nil_charge);
}

void assign_respects_the_limit() {
    host::PlainData data;
    const uint8_t byte = 7;
    OA_CHECK(data.assign(std::span<const uint8_t>(&byte, 1)));
    std::vector<uint8_t> huge(host::plain_data_limit + 1, 0);
    OA_CHECK(!data.assign(huge));
    OA_CHECK(data.bytes().size() == 1);
    OA_CHECK(data.bytes()[0] == 7);
    data.clear();
    OA_CHECK(data.bytes().empty());
    host::PlainDataProblem problem;
    problem.where = "saved.units[3]";
    problem.message = "a function cannot be saved";
    OA_CHECK(host::describe(problem) == "saved.units[3]: a function cannot be saved");
}

} // namespace

int main() {
    exact_bytes();
    same_bytes_whatever_the_fill_order();
    round_trip();
    refusals();
    check_refuses_damaged_bytes();
    push_of_damaged_bytes_fails_the_phase();
    charges();
    assign_respects_the_limit();
    return oa::test::check_exit_status();
}

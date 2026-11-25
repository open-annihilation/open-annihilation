// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Charged library calls against Lua's own results, then the loops and ranges
// that have to stop a phase. The clocks are only in this test.

#include "lua.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/test/check.hpp"
#include "oa/test/processor_time.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace host = oa::data::script_host;

namespace {

// The address sanitizer makes every call several times slower, so its builds
// get the suite's timeout scale on the time bounds.
//
// A loop the budget stops spends at most `budget_stop_seconds` of processor
// time. Before the libraries were charged these loops ran for hours; a slow
// Windows machine needs about 7 seconds for the heaviest now.
#if defined(__SANITIZE_ADDRESS__)
constexpr int time_scale = 6;
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
constexpr int time_scale = 6;
#else
constexpr int time_scale = 1;
#endif
#else
constexpr int time_scale = 1;
#endif

constexpr double budget_stop_seconds = 20.0;

struct Case {
    std::string name;
    std::string body;
};

struct Package {
    std::map<std::string, std::vector<uint8_t>> files;
};

struct ReadResult {
    std::vector<char> text;
    std::size_t length = 0;
    bool saw = false;
    bool overflow = false;
};

bool read_package(void* context, std::string_view path, std::vector<uint8_t>& bytes) {
    auto* package = static_cast<Package*>(context);
    const auto found = package->files.find(std::string(path));
    if (found == package->files.end())
        return false;
    bytes = found->second;
    return true;
}

void put(Package& package, std::string path, std::string_view source) {
    package.files[std::move(path)] = std::vector<uint8_t>(source.begin(), source.end());
}

void* counting_allocate(void* /*user*/, void* pointer, size_t /*old_size*/, size_t new_size) {
    if (new_size == 0) {
        std::free(pointer);
        return nullptr;
    }
    return std::realloc(pointer, new_size);
}

void read_result(void* context, lua_State* state, int results) {
    auto* out = static_cast<ReadResult*>(context);
    out->saw = true;
    out->length = 0;
    out->overflow = false;
    if (results < 1 || lua_type(state, -1) != LUA_TSTRING)
        return;
    std::size_t length = 0;
    const char* text = lua_tolstring(state, -1, &length);
    if (text == nullptr)
        return;
    if (length >= out->text.size()) {
        out->overflow = true;
        return;
    }
    std::memcpy(out->text.data(), text, length);
    out->length = length;
}

host::Profile compare_profile() {
    host::Budget budget;
    budget.instructions = 2'000'000;
    budget.memory_bytes = std::size_t{8} << 20;
    return host::Profile{"test", budget, nullptr, nullptr};
}

std::optional<host::Sandbox>
open_with(Package& package, host::ScriptError& error, host::Profile profile) {
    host::OpenOptions options;
    options.package_id = "sample";
    options.profile = profile;
    // These cases are about the instruction budget; the real-time limit has
    // its own test, and a busy machine must not turn a budget stop into a
    // time stop here.
    options.time_limit = std::chrono::minutes(10);
    options.sources.context = &package;
    options.sources.read = read_package;
    auto sandbox = host::Sandbox::open(options, error);
    OA_CHECK(sandbox.has_value());
    return sandbox;
}

const char* preamble = R"lua(
local function show(...)
  local count = select("#", ...)
  local parts = {}
  for index = 1, count do
    local value = select(index, ...)
    local kind = type(value)
    if kind == "string" then
      parts[index] = "string:" .. string.format("%q", value)
    elseif kind == "number" then
      parts[index] = "number:" .. string.format("%.17g", value)
    elseif kind == "boolean" then
      if value then
        parts[index] = "boolean:true"
      else
        parts[index] = "boolean:false"
      end
    elseif kind == "nil" then
      parts[index] = "nil"
    else
      parts[index] = kind
    end
  end
  return table.concat(parts, "\t")
end
local ok, result = pcall(function()
)lua";

const char* postamble = R"lua(
end)
if not ok then
  return "error\t" .. tostring(result)
end
return "values\t" .. result
)lua";

std::string script_for(const std::string& body) {
    return std::string(preamble) + body + postamble;
}

void add_case(std::vector<Case>& cases, std::string name, std::string body) {
    cases.push_back(Case{std::move(name), std::move(body)});
}

void add_cases(std::vector<Case>& cases) {
    const char* rows[][2] = {
        {"byte-one", "return show(string.byte(\"ABC\", 2))"},
        {"byte-range", "return show(string.byte(\"ABC\", 1, 3))"},
        {"byte-negative", "return show(string.byte(\"ABC\", -1))"},
        {"byte-negative-range", "return show(string.byte(\"ABC\", -3, -2))"},
        {"byte-empty-range", "return show(string.byte(\"ABC\", 3, 2))"},
        {"byte-past-end", "return show(string.byte(\"ABC\", 1, 10))"},
        {"byte-zero", "return show(string.byte(\"ABC\", 0))"},
        {"byte-empty-string", "return show(string.byte(\"\", 1, 1))"},
        {"byte-high", "return show(string.byte(string.char(0, 127, 128, 255), 1, 4))"},
        {"byte-default", "return show(string.byte(\"Z\"))"},
        {"byte-only-end", "return show(string.byte(\"ABC\", 2, 2))"},
        {"byte-type", "return show(string.byte(\"ABC\", \"no\"))"},

        {"move-forward",
         "local t = {10, 20, 30, 40}\ntable.move(t, 1, 2, 3)\nreturn show(t[1], t[2], t[3], t[4])"},
        {"move-backward",
         "local t = {10, 20, 30, 40}\ntable.move(t, 3, 4, 2)\nreturn show(t[1], t[2], t[3], t[4])"},
        {"move-other",
         "local a = {1, 2, 3}\nlocal b = {}\ntable.move(a, 1, 3, 2, b)\nreturn show(b[2], b[3], "
         "b[4])"},
        {"move-empty", "local t = {1, 2}\ntable.move(t, 2, 1, 1)\nreturn show(t[1], t[2])"},
        {"move-one", "local t = {7}\ntable.move(t, 1, 1, 2)\nreturn show(t[1], t[2])"},
        {"move-same-spot", "local t = {4, 5}\ntable.move(t, 1, 2, 1)\nreturn show(t[1], t[2])"},
        {"move-not-table", "return show(table.move(1, 1, 1, 1))"},
        {"move-too-many", "return show(table.move({}, 0, 9223372036854775807, 1))"},
        {"move-wrap", "return show(table.move({1}, 1, 2, 9223372036854775807))"},
        {"move-negative", "local t = {9}\ntable.move(t, -1, -1, 1)\nreturn show(t[1])"},

        {"insert-end", "local t = {1, 2}\ntable.insert(t, 3)\nreturn show(t[1], t[2], t[3])"},
        {"insert-middle",
         "local t = {1, 2, 3}\ntable.insert(t, 2, 9)\nreturn show(t[1], t[2], t[3], t[4])"},
        {"insert-front", "local t = {1, 2}\ntable.insert(t, 1, 0)\nreturn show(t[1], t[2], t[3])"},
        {"insert-empty", "local t = {}\ntable.insert(t, \"a\")\nreturn show(t[1])"},
        {"insert-bounds", "return show(table.insert({1}, 3, 1))"},
        {"insert-zero", "return show(table.insert({1}, 0, 1))"},
        {"insert-argc", "return show(table.insert({1}, 1, 1, 1))"},
        {"insert-type", "return show(table.insert(1, 1))"},

        {"remove-last", "local t = {1, 2, 3}\nreturn show(table.remove(t), t[1], t[2], t[3])"},
        {"remove-middle", "local t = {1, 2, 3}\nreturn show(table.remove(t, 2), t[1], t[2], t[3])"},
        {"remove-front", "local t = {1, 2, 3}\nreturn show(table.remove(t, 1), t[1], t[2], t[3])"},
        {"remove-only", "local t = {8}\nreturn show(table.remove(t), t[1])"},
        {"remove-empty", "return show(table.remove({}))"},
        {"remove-bounds", "return show(table.remove({1, 2}, 4))"},
        {"remove-past-end", "local t = {1}\nreturn show(table.remove(t, 2), t[1], t[2])"},

        {"concat-empty", "return show(table.concat({}))"},
        {"concat-one", "return show(table.concat({\"ab\"}))"},
        {"concat-sep", "return show(table.concat({\"a\", \"b\", \"c\"}, \"-\"))"},
        {"concat-range", "return show(table.concat({\"a\", \"b\", \"c\", \"d\"}, \",\", 2, 3))"},
        {"concat-numbers", "return show(table.concat({1, 2, 3}, \":\"))"},
        {"concat-empty-range", "return show(table.concat({\"a\", \"b\"}, \",\", 2, 1))"},
        {"concat-invalid", "return show(table.concat({1, {}, 3}))"},
        {"concat-hole", "return show(table.concat({\"a\", nil, \"c\"}))"},

        {"unpack-all", "return show(table.unpack({10, 20, 30}))"},
        {"unpack-range", "return show(table.unpack({10, 20, 30, 40}, 2, 3))"},
        {"unpack-empty", "return show(table.unpack({1, 2}, 2, 1))"},
        {"unpack-from", "return show(table.unpack({7, 8, 9}, 3))"},
        {"unpack-hole", "return show(table.unpack({1, nil, 3}))"},
        {"unpack-none", "return show(table.unpack({}))"},

        {"sort-reverse",
         "local t = {3, 1, 4, 2}\ntable.sort(t)\nreturn show(t[1], t[2], t[3], t[4])"},
        {"sort-sorted",
         "local t = {1, 2, 3, 4}\ntable.sort(t)\nreturn show(t[1], t[2], t[3], t[4])"},
        {"sort-duplicates",
         "local t = {2, 2, 1, 1, 2}\ntable.sort(t)\nreturn show(t[1], t[2], t[3], t[4], t[5])"},
        {"sort-empty", "local t = {}\ntable.sort(t)\nreturn show(#t)"},
        {"sort-one", "local t = {5}\ntable.sort(t)\nreturn show(t[1])"},
        {"sort-comparator",
         "local t = {1, 3, 2}\ntable.sort(t, function(a, b) return a > b end)\nreturn show(t[1], "
         "t[2], t[3])"},
        {"sort-strings",
         "local t = {\"b\", \"a\", \"c\"}\ntable.sort(t)\nreturn show(t[1], t[2], t[3])"},
        {"sort-invalid",
         "local t = {1, 2, 3}\ntable.sort(t, function() return true end)\nreturn show(t[1])"},

        {"len-ascii", "return show(utf8.len(\"abc\"))"},
        {"len-multibyte", "return show(utf8.len(string.char(0xC3, 0xA9)))"},
        {"len-mixed", "return show(utf8.len(\"a\" .. string.char(0xC3, 0xA9) .. \"b\"))"},
        {"len-empty", "return show(utf8.len(\"\"))"},
        {"len-invalid", "return show(utf8.len(string.char(0x80)))"},
        {"len-range", "return show(utf8.len(\"abcd\", 2, 3))"},
        {"len-lax", "return show(utf8.len(string.char(0xED, 0xA0, 0x80), 1, -1, true))"},

        {"codepoint-ascii", "return show(utf8.codepoint(\"AB\", 1, 2))"},
        {"codepoint-one", "return show(utf8.codepoint(string.char(0xC3, 0xA9)))"},
        {"codepoint-range", "return show(utf8.codepoint(\"abcd\", 2, 3))"},
        {"codepoint-empty", "return show(utf8.codepoint(\"abc\", 3, 2))"},
        {"codepoint-invalid", "return show(utf8.codepoint(string.char(0x80)))"},

        {"offset-next", "return show(utf8.offset(\"abc\", 2))"},
        {"offset-back", "return show(utf8.offset(\"abc\", -1))"},
        {"offset-zero", "return show(utf8.offset(string.char(0xC3, 0xA9), 0, 2))"},
        {"offset-end", "return show(utf8.offset(\"abc\", 4))"},
        {"offset-missing", "return show(utf8.offset(\"abc\", 5))"},
        {"offset-continuation", "return show(utf8.offset(string.char(0xC3, 0xA9), 1, 2))"},

        {"codes-ascii",
         "local parts = {}\nfor position, code in utf8.codes(\"ab\") do\n  parts[#parts + 1] = "
         "show(position, code)\nend\nreturn table.concat(parts, \"|\")"},
        {"codes-multibyte",
         "local text = \"a\" .. string.char(0xC3, 0xA9)\nlocal parts = {}\nfor position, code in "
         "utf8.codes(text) do\n  parts[#parts + 1] = show(position, code)\nend\nreturn "
         "table.concat(parts, \"|\")"},
        {"codes-empty",
         "local n = 0\nfor position, code in utf8.codes(\"\") do\n  n = n + 1\nend\nreturn "
         "show(n)"},
        {"codes-invalid",
         "local n = 0\nfor position, code in utf8.codes(string.char(0x80)) do\n  n = n + "
         "1\nend\nreturn show(n)"},
    };
    for (const char* const* row : rows)
        add_case(cases, row[0], row[1]);

    // The functions copied with a charge inside: each message names the
    // function, counts a method call's arguments as Lua does, and starts
    // with the script's chunk and line.
    const char* messages[][2] = {
        {"format-none", "return show(string.format())"},
        {"format-number-expected", "return show(string.format(\"%d\", \"x\"))"},
        {"format-method", "return show((\"%d\"):format(\"x\"))"},
        {"format-pcall-name", "return show(pcall(string.format))"},
        {"format-no-value", "return show(string.format(\"%d %d\", 1))"},
        {"format-bad-conversion", "return show(string.format(\"%y\", 1))"},
        {"format-bad-specification", "return show(string.format(\"%10.3c\", 65))"},
        {"format-too-long", "return show(string.format(\"%0000000000000000000000000d\", 1))"},
        {"format-q-modifier", "return show(string.format(\"%10q\", \"a\"))"},
        {"format-q-table", "return show(string.format(\"%q\", {}))"},
        {"format-a-modifier", "return show(string.format(\"%5a\", 1.5))"},
        {"format-s-zeros", "return show(string.format(\"%5s\", \"a\\0b\"))"},
        {"format-not-integer", "return show(string.format(\"%d\", 1.5))"},
        {"format-mixed",
         "return show(string.format(\"%5.2f|%-5d|%x|%X|%o|%c|%e|%G|%i|%u|%+3d|%#o|% d\", "
         "3.14159, 42, 255, 255, 8, 65, 12345.678, 0.0001, -7, 3, 5, 8, 9))"},
        {"format-q",
         "return show(string.format(\"%q|%q|%q|%q|%q|%q|%q|%q\", \"a\\n\\0b\\\"\\\\\\1\\0019\", "
         "1/0, -1/0, -9223372036854775807 - 1, 2^53, 0.1, 0/0, true))"},
        {"format-a", "return show(string.format(\"%a|%A|%a|%a\", 1.5, -0.1, 0.0, 1e-310))"},
        {"format-s",
         "return show(string.format(\"%s|%s|%.3s|%10s|%-4s|%s\", nil, true, \"abcdef\", \"xy\", "
         "\"z\", string.rep(\"w\", 120)))"},
        {"format-s-meta",
         "return show(string.format(\"%s\", setmetatable({}, {__tostring = function() return "
         "\"T\" end})))"},
        {"format-percent", "return show(string.format(\"100%% %s\", \"x\"))"},
        {"format-f-wide", "return show(string.format(\"%99.99f\", -1e308))"},

        {"tonumber-base-range", "return show(tonumber(\"10\", 99))"},
        {"tonumber-none", "return show(tonumber())"},
        {"tonumber-base-number", "return show(tonumber(10, 16))"},
        {"tonumber-base-type", "return show(tonumber(\"10\", \"x\"))"},
        {"tonumber-forms",
         "return show(tonumber(\"0x10\"), tonumber(\" 12 \"), tonumber(\"1e2\"), tonumber(\"z\", "
         "36), tonumber(\" -ff \", 16), tonumber(\"8\", 8), tonumber(\"1 2\"), tonumber({}), "
         "tonumber(\"7\", 10.0), tonumber(\"\"))"},

        {"next-none", "return show(next())"},
        {"next-bad-key", "return show(next({}, \"nokey\"))"},
        {"next-type", "return show(next(\"s\"))"},
        {"next-values", "return show(next({}), next({5}, 1), next({5}))"},

        {"rawequal-none", "return show(rawequal(1))"},
        {"rawequal-values",
         "local a = string.rep(\"x\", 100)\nreturn show(rawequal(a, string.rep(\"x\", 100)), "
         "rawequal(\"a\", \"b\"), rawequal(1, 1.0), rawequal({}, {}))"},

        {"warn-none", "return show(warn())"},
        {"warn-table", "return show(warn(\"a\", {}))"},
        {"warn-values", "return show(warn(\"@off\"), warn(\"a\", 1, 2.5))"},

        {"char-range", "return show(utf8.char(-1))"},
        {"char-type", "return show(utf8.char(\"x\"))"},
        {"char-second", "return show(utf8.char(65, 2^31))"},
        {"char-values", "return show(utf8.char(), utf8.char(72, 233, 8364, 0x10FFFF, 0x7FFFFFFF))"},

        {"table-pack", "local t = table.pack(1, nil, \"x\")\nreturn show(t.n, t[1], t[2], t[3])"},
        {"table-pack-empty", "local t = table.pack()\nreturn show(t.n, t[1])"},
    };
    for (const char* const* row : messages)
        add_case(cases, row[0], row[1]);
}

lua_State* open_raw() {
    lua_State* state = lua_newstate(counting_allocate, nullptr);
    OA_CHECK(state != nullptr);
    if (state == nullptr)
        return nullptr;
    luaL_requiref(state, "_G", luaopen_base, 1);
    lua_pop(state, 1);
    luaL_requiref(state, "string", luaopen_string, 1);
    lua_pop(state, 1);
    luaL_requiref(state, "table", luaopen_table, 1);
    lua_pop(state, 1);
    luaL_requiref(state, "utf8", luaopen_utf8, 1);
    lua_pop(state, 1);
    return state;
}

// The chunk has the sandbox's name for it, so an error's "chunk:line:" prefix
// is compared too.
std::string run_raw(lua_State* state, const std::string& source, bool* ran) {
    lua_settop(state, 0);
    if (luaL_loadbufferx(state, source.data(), source.size(), "@mutator.lua", "t") != LUA_OK) {
        *ran = false;
        const char* text = lua_tostring(state, -1);
        std::string message = text != nullptr ? text : "load failed";
        lua_settop(state, 0);
        return message;
    }
    if (lua_pcall(state, 0, 1, 0) != LUA_OK) {
        *ran = false;
        const char* text = lua_tostring(state, -1);
        std::string message = text != nullptr ? text : "call failed";
        lua_settop(state, 0);
        return message;
    }
    *ran = true;
    std::size_t length = 0;
    const char* text = lua_tolstring(state, -1, &length);
    std::string result = text != nullptr ? std::string(text, length) : std::string();
    lua_settop(state, 0);
    return result;
}

void compare_cases(const std::vector<Case>& cases) {
    OA_CHECK(cases.size() >= 60);
    Package package;
    host::ScriptError error;
    auto sandbox = open_with(package, error, compare_profile());
    if (!sandbox)
        return;
    lua_State* raw = open_raw();
    if (raw == nullptr)
        return;
    for (const Case& row : cases) {
        if (sandbox->failed()) {
            sandbox = open_with(package, error, compare_profile());
            if (!sandbox)
                break;
        }
        const std::string source = script_for(row.body);
        bool raw_ran = false;
        const std::string raw_text = run_raw(raw, source, &raw_ran);
        ReadResult read;
        read.text.resize(64 * 1024);
        host::CallHooks hooks;
        hooks.context = &read;
        hooks.read = read_result;
        put(package, "mutator.lua", source);
        const bool ran = sandbox->run_file("mutator.lua", "c-work", &hooks, error);
        if (!ran || !raw_ran || !read.saw || read.overflow) {
            std::fprintf(
                stderr,
                "case %s sandbox %d raw %d saw %d overflow %d kind %d message %s\n",
                row.name.c_str(),
                ran ? 1 : 0,
                raw_ran ? 1 : 0,
                read.saw ? 1 : 0,
                read.overflow ? 1 : 0,
                static_cast<int>(error.kind),
                error.message.c_str()
            );
            OA_CHECK(ran && raw_ran && read.saw && !read.overflow);
            continue;
        }
        const std::string sandbox_text(read.text.data(), read.length);
        if (sandbox_text != raw_text) {
            std::fprintf(
                stderr,
                "case %s\n sandbox [%s]\n raw [%s]\n",
                row.name.c_str(),
                sandbox_text.c_str(),
                raw_text.c_str()
            );
            OA_CHECK(sandbox_text == raw_text);
        }
    }
    lua_close(raw);
}

bool message_has(const host::ScriptError& error, const char* needle) {
    return error.message.find(needle) != std::string::npos;
}

void expect_budget(const char* source) {
    Package package;
    host::ScriptError error;
    auto sandbox = open_with(package, error, host::mutator_profile());
    if (!sandbox)
        return;
    put(package, "mutator.lua", source);
    // The bound is on the processor time this test spends, so other programs
    // sharing the machine do not stretch it. The wall time is printed too.
    const double stop_seconds = budget_stop_seconds * time_scale;
    const oa::test::WorkTimer timer;
    const bool ran = sandbox->run_file("mutator.lua", "tick", nullptr, error);
    const oa::test::WorkTime took = timer.elapsed();
    // A processor time the system cannot give fails the bound.
    const double spent_seconds = took.processor_seconds.value_or(stop_seconds);
    const std::string_view text(source);
    const std::string_view first_line = text.substr(0, text.find('\n'));
    std::printf(
        "stopped in %lld ms (%.0f ms of processor time): %.*s\n",
        static_cast<long long>(took.real.count()),
        spent_seconds * 1000.0,
        static_cast<int>(first_line.size()),
        first_line.data()
    );
    OA_CHECK(!ran);
    OA_CHECK(error.kind == host::ErrorKind::instruction_budget);
    OA_CHECK(message_has(error, "50000000"));
    OA_CHECK(sandbox->instructions_used() >= host::default_instruction_budget);
    OA_CHECK(took.processor_seconds.has_value());
    OA_CHECK(spent_seconds < stop_seconds);
}

void expect_range(const char* source, const char* function, const char* count) {
    Package package;
    host::ScriptError error;
    auto sandbox = open_with(package, error, host::mutator_profile());
    if (!sandbox)
        return;
    put(package, "mutator.lua", source);
    const auto started = std::chrono::steady_clock::now();
    const bool ran = sandbox->run_file("mutator.lua", "tick", nullptr, error);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    OA_CHECK(!ran);
    OA_CHECK(error.kind == host::ErrorKind::runtime);
    OA_CHECK(message_has(error, function));
    OA_CHECK(message_has(error, "past the most of"));
    OA_CHECK(message_has(error, count));
    OA_CHECK(elapsed < std::chrono::seconds(5 * time_scale));
}

void review_and_replaced_calls_stop() {
    // A string of 1 MiB cannot be built, so the original upper loop is the
    // same call on a string of the new cap.
    Package package;
    host::ScriptError error;
    auto sandbox = open_with(package, error, host::mutator_profile());
    if (sandbox) {
        put(package, "mutator.lua", "string.rep(\"x\", 1048576)\n");
        const auto started = std::chrono::steady_clock::now();
        const bool ran = sandbox->run_file("mutator.lua", "tick", nullptr, error);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        OA_CHECK(!ran);
        OA_CHECK(error.kind == host::ErrorKind::string_length);
        OA_CHECK(message_has(error, "1048576"));
        OA_CHECK(message_has(error, "65536"));
        OA_CHECK(elapsed < std::chrono::seconds(5 * time_scale));
    }

    expect_budget(
        "local s = string.rep(\"x\", 65536)\nwhile true do\n  local u = s:upper()\nend\n"
    );
    expect_range("table.move({}, 1, 0x7ffffffffffffffe, 2)\n", "table.move", "9223372036854775806");
    expect_range(
        "local t = setmetatable({}, { __len = function() return 1 << 62 end })\n"
        "table.insert(t, 1, \"x\")\n",
        "table.insert",
        "4611686018427387904"
    );
    expect_range(
        "local t = setmetatable({}, { __len = function() return 1 << 62 end })\n"
        "table.remove(t, 1)\n",
        "table.remove",
        "4611686018427387903"
    );
    expect_range("table.concat({}, \"\", 1, 1 << 62)\n", "table.concat", "4611686018427387904");
    expect_range("table.unpack({}, 1, 1 << 62)\n", "table.unpack", "4611686018427387904");
    expect_budget(
        "local s = string.rep(\"x\", 8192)\nwhile true do\n  string.byte(s, 1, -1)\nend\n"
    );
    expect_budget(
        "local t = {}\nfor i = 1, 128 do\n  t[i] = 129 - i\nend\nwhile true do\n  "
        "table.sort(t)\nend\n"
    );
    expect_budget("local s = string.rep(\"x\", 65536)\nwhile true do\n  utf8.len(s)\nend\n");
    expect_budget(
        "local s = string.rep(\"x\", 8192)\nwhile true do\n  utf8.codepoint(s, 1, -1)\nend\n"
    );
    expect_budget(
        "local s = string.rep(\"x\", 65536)\nwhile true do\n  utf8.offset(s, 65536)\nend\n"
    );
    expect_budget(
        "local s = string.rep(\"x\", 65536)\nwhile true do\n  for position, code in "
        "utf8.codes(s) do end\nend\n"
    );
    expect_budget(
        "local big = string.rep(\"x\", 65536)\n"
        "while true do\n"
        "  pcall(function()\n"
        "    for i = 1, 1000 do\n"
        "      local u = big:upper()\n"
        "    end\n"
        "  end)\n"
        "end\n"
    );
    expect_budget(
        "while true do\n"
        "  (\"a\"):rep(65536):find((\"a\"):rep(32767) .. \"b\", 1, true)\n"
        "end\n"
    );
    expect_budget(
        "local s = \"a\" .. string.rep(\"\\x80\", 65000)\n"
        "local f = utf8.codes(\"a\")\n"
        "while true do\n"
        "  f(s, 1)\n"
        "end\n"
    );
    expect_budget(
        "local t = {}\n"
        "for i = 1, 1000000 do\n"
        "  t[i] = i\n"
        "end\n"
        "while true do\n"
        "  table.sort(t, rawequal)\n"
        "end\n"
    );
    expect_budget(
        "local t = {}\n"
        "for i = 1, 400 do\n"
        "  t[i] = string.rep(\"\\0\", 65535)\n"
        "end\n"
        "while true do\n"
        "  table.sort(t)\n"
        "end\n"
    );
}

// The copied base, string, utf8 and table calls, each in a loop that does a
// lot of work per call.
void copied_calls_stop() {
    // string.format copies 400 texts of 65,000 bytes, then fails on its last
    // argument, which the script catches.
    expect_budget(
        "local big = string.rep(\"x\", 65000)\n"
        "local args = {}\n"
        "for i = 1, 400 do\n"
        "  args[i] = big\n"
        "end\n"
        "args[401] = \"not a number\"\n"
        "local format = string.rep(\"%s\", 400) .. \"%d\"\n"
        "while true do\n"
        "  pcall(string.format, format, table.unpack(args))\n"
        "end\n"
    );
    expect_budget(
        "local t = {}\n"
        "for i = 1, 300000 do\n"
        "  t[i] = i\n"
        "end\n"
        "local function f(...)\n"
        "  while true do\n"
        "    local packed = table.pack(...)\n"
        "  end\n"
        "end\n"
        "f(table.unpack(t))\n"
    );
    expect_budget(
        "local a = string.rep(\"x\", 65536)\n"
        "local b = string.rep(\"x\", 65536)\n"
        "while true do\n"
        "  rawequal(a, b)\n"
        "end\n"
    );
    expect_budget(
        "local t = {}\n"
        "for i = 1, 300000 do\n"
        "  t[i] = \"\"\n"
        "end\n"
        "local function f(...)\n"
        "  while true do\n"
        "    warn(...)\n"
        "  end\n"
        "end\n"
        "f(table.unpack(t))\n"
    );
    expect_budget(
        "local t = {}\n"
        "for i = 1, 30000 do\n"
        "  t[i] = \"\"\n"
        "end\n"
        "local format = string.rep(\"%s\", 30000)\n"
        "local function f(...)\n"
        "  while true do\n"
        "    string.format(format, ...)\n"
        "  end\n"
        "end\n"
        "f(table.unpack(t))\n"
    );
    expect_budget(
        "local t = {}\n"
        "for i = 1, 300000 do\n"
        "  t[i] = i\n"
        "end\n"
        "local function f(...)\n"
        "  while true do\n"
        "    math.max(...)\n"
        "  end\n"
        "end\n"
        "f(table.unpack(t))\n"
    );
    expect_budget(
        "local t = {}\n"
        "for i = 1, 300000 do\n"
        "  t[i * 7 % 300007] = i\n"
        "end\n"
        "while true do\n"
        "  for k in pairs(t) do\n"
        "    break\n"
        "  end\n"
        "end\n"
    );
    expect_budget(
        "local s = string.rep(\" \", 65000) .. \"1\"\nwhile true do\n  tonumber(s)\nend\n"
    );
    // setmetatable installs a copy of the metatable, walking every field.
    expect_budget(
        "local mt = {}\n"
        "for i = 1, 30000 do\n"
        "  mt[i] = i\n"
        "end\n"
        "while true do\n"
        "  setmetatable({}, mt)\n"
        "end\n"
    );
}

/// Runs `setup` and then `loop` as two phases of one sandbox, and returns the
/// count the loop phase spent.
uint64_t loop_charge(const char* setup, const char* loop) {
    Package package;
    host::ScriptError error;
    auto sandbox = open_with(package, error, host::mutator_profile());
    if (!sandbox)
        return 0;
    put(package, "setup.lua", setup);
    put(package, "loop.lua", loop);
    OA_CHECK(sandbox->run_file("setup.lua", "setup", nullptr, error));
    OA_CHECK(sandbox->run_file("loop.lua", "loop", nullptr, error));
    return sandbox->instructions_used();
}

// A string of at most 40 bytes is shared, so whether making one allocates
// depends on when the collector last ran. Those strings are not charged, so
// the same loop costs the same with an empty heap and with many live tables.
void short_strings_cost_the_same() {
    const char* empty = "keep = {}\n";
    const char* full = "keep = {}\nfor i = 1, 20000 do\n  keep[i] = {i}\nend\n";
    const char* short_loop = "local made = 0\n"
                             "for i = 1, 200000 do\n"
                             "  local s = \"k\" .. (i % 500)\n"
                             "  made = made + #s\n"
                             "end\n"
                             "return made\n";
    const uint64_t short_empty = loop_charge(empty, short_loop);
    const uint64_t short_full = loop_charge(full, short_loop);
    std::printf(
        "short strings: %llu units with an empty heap, %llu with 20000 tables\n",
        static_cast<unsigned long long>(short_empty),
        static_cast<unsigned long long>(short_full)
    );
    OA_CHECK(short_empty > 0);
    OA_CHECK(short_empty == short_full);

    // Strings past 40 bytes are always new, and are charged by length.
    const char* long_loop = "local made = 0\n"
                            "local p = string.rep(\"p\", 40)\n"
                            "for i = 1, 200000 do\n"
                            "  local s = p .. (i % 500)\n"
                            "  made = made + #s\n"
                            "end\n"
                            "return made\n";
    const uint64_t long_empty = loop_charge(empty, long_loop);
    const uint64_t long_full = loop_charge(full, long_loop);
    OA_CHECK(long_empty == long_full);
    OA_CHECK(long_empty >= short_empty + 200000);
}

/// Runs `source` at the mutator default and returns the count it spent.
uint64_t workload_charge(const char* source) {
    Package package;
    host::ScriptError error;
    auto sandbox = open_with(package, error, host::mutator_profile());
    if (!sandbox)
        return 0;
    put(package, "mutator.lua", source);
    OA_CHECK(sandbox->run_file("mutator.lua", "tick", nullptr, error));
    return sandbox->instructions_used();
}

// What a mutator does to 400 units of 61 fields: walk each unit's fields with
// pairs, clamp them with math.max and math.min, write them with
// string.format, and sort names. Each stays a small part of the allowance.
void unit_workloads_stay_cheap() {
    const std::string units = "local units = {}\n"
                              "local names = {}\n"
                              "for f = 1, 60 do\n"
                              "  names[f] = \"field\" .. string.format(\"%02d\", f)\n"
                              "end\n"
                              "for i = 1, 400 do\n"
                              "  local u = {unitname = \"RIDGE\" .. i}\n"
                              "  for f = 1, 60 do\n"
                              "    u[names[f]] = i + f\n"
                              "  end\n"
                              "  units[i] = u\n"
                              "end\n";
    const uint64_t setup = workload_charge(units.c_str());
    const std::string walk = units + "for i = 1, 400 do\n"
                                     "  for k, v in pairs(units[i]) do\n"
                                     "    if type(v) == \"number\" then\n"
                                     "      units[i][k] = v * 2\n"
                                     "    end\n"
                                     "  end\n"
                                     "end\n";
    const std::string clamp = units + "for i = 1, 400 do\n"
                                      "  for f = 1, 50 do\n"
                                      "    local k = names[f]\n"
                                      "    units[i][k] = math.max(0, math.min(units[i][k], 300))\n"
                                      "  end\n"
                                      "end\n";
    const std::string format = units + "local out = 0\n"
                                       "for i = 1, 400 do\n"
                                       "  for f = 1, 20 do\n"
                                       "    local text = string.format(\"%s=%d\", names[f], "
                                       "units[i][names[f]])\n"
                                       "    out = out + #text\n"
                                       "  end\n"
                                       "end\n";
    const std::string sort = units + "local list = {}\n"
                                     "for i = 1, 400 do\n"
                                     "  list[i] = units[i].unitname\n"
                                     "end\n"
                                     "table.sort(list)\n"
                                     "local numbers = {}\n"
                                     "for i = 1, 5000 do\n"
                                     "  numbers[i] = (i * 7919) % 5003\n"
                                     "end\n"
                                     "table.sort(numbers)\n";
    const uint64_t walked = workload_charge(walk.c_str()) - setup;
    const uint64_t clamped = workload_charge(clamp.c_str()) - setup;
    const uint64_t formatted = workload_charge(format.c_str()) - setup;
    const uint64_t sorted = workload_charge(sort.c_str()) - setup;
    std::printf(
        "unit workloads: pairs %llu, clamp %llu, format %llu, sort %llu units\n",
        static_cast<unsigned long long>(walked),
        static_cast<unsigned long long>(clamped),
        static_cast<unsigned long long>(formatted),
        static_cast<unsigned long long>(sorted)
    );
    OA_CHECK(walked < host::default_instruction_budget / 8);
    OA_CHECK(clamped < host::default_instruction_budget / 100);
    OA_CHECK(formatted < host::default_instruction_budget / 100);
    OA_CHECK(sorted < host::default_instruction_budget / 100);
}

// setmetatable charges 64 units for making the copy and one unit per field
// it copies. Both phases run fewer instructions than one count step, so the
// difference is the second call's charge alone. A class of 21 fields set on
// 5,000 values stays a small part of the allowance.
void setmetatable_charge_stays_small() {
    const char* once = "local mt = {__index = {}, a = 1, b = 2}\n"
                       "setmetatable({}, mt)\n";
    const char* twice = "local mt = {__index = {}, a = 1, b = 2}\n"
                        "setmetatable({}, mt)\n"
                        "setmetatable({}, mt)\n";
    const uint64_t one = workload_charge(once);
    const uint64_t two = workload_charge(twice);
    OA_CHECK(one == 64 + 3);
    OA_CHECK(two - one == 64 + 3);

    const char* instances = "local Class = {}\n"
                            "Class.__index = Class\n"
                            "for m = 1, 20 do\n"
                            "  Class[\"method\" .. m] = function(self) return self.id + m end\n"
                            "end\n"
                            "local made = {}\n"
                            "for i = 1, 5000 do\n"
                            "  made[i] = setmetatable({id = i}, Class)\n"
                            "end\n"
                            "return made[5000]:method20()\n";
    const uint64_t many = workload_charge(instances);
    std::printf(
        "setmetatable: %llu units for one call, %llu for 5000 values of a 21-field class\n",
        static_cast<unsigned long long>(one),
        static_cast<unsigned long long>(many)
    );
    OA_CHECK(many >= 5000 * (64 + 21));
    OA_CHECK(many < host::default_instruction_budget / 50);
}

void utf8_codes_is_one_function() {
    Package package;
    host::ScriptError error;
    auto sandbox = open_with(package, error, host::mutator_profile());
    if (!sandbox)
        return;
    put(package, "mutator.lua", "same = utf8.codes(\"a\") == utf8.codes(\"b\")\n");
    const bool ran = sandbox->run_file("mutator.lua", "tick", nullptr, error);
    OA_CHECK(ran);
    lua_State* state = sandbox->state();
    lua_getglobal(state, "same");
    OA_CHECK(lua_toboolean(state, -1) == 1);
}

// string.pack, string.packsize and string.unpack are removed, because their
// sizes and alignment differ between builds. Neither the library nor a method
// call on a string reaches them.
void pack_family_is_absent() {
    Package package;
    host::ScriptError error;
    auto sandbox = open_with(package, error, host::mutator_profile());
    if (!sandbox)
        return;
    put(package,
        "mutator.lua",
        "absent = string.pack == nil and string.packsize == nil and string.unpack == nil\n"
        "local ok, message = pcall(function() return (\"i4\"):pack(1) end)\n"
        "method_absent = not ok and string.find(message, \"nil value\", 1, true) ~= nil\n");
    const bool ran = sandbox->run_file("mutator.lua", "tick", nullptr, error);
    OA_CHECK(ran);
    if (!ran)
        return;
    lua_State* state = sandbox->state();
    lua_getglobal(state, "absent");
    OA_CHECK(lua_toboolean(state, -1) == 1);
    lua_getglobal(state, "method_absent");
    OA_CHECK(lua_toboolean(state, -1) == 1);
    lua_pop(state, 2);
}

struct TextResult {
    char text[16]{};
    std::size_t length = 0;
    bool saw = false;
};

void read_text(void* context, lua_State* state, int results) {
    auto* out = static_cast<TextResult*>(context);
    out->saw = results >= 1;
    out->length = 0;
    if (!out->saw || lua_type(state, -1) != LUA_TSTRING)
        return;
    std::size_t length = 0;
    const char* text = lua_tolstring(state, -1, &length);
    if (text == nullptr || length >= sizeof out->text)
        return;
    std::memcpy(out->text, text, length);
    out->length = length;
}

void a_mutator_rewrites_tables() {
    Package package;
    host::ScriptError error;
    auto sandbox = open_with(package, error, host::mutator_profile());
    if (!sandbox)
        return;
    const char* source = "local rows = {}\n"
                         "for i = 1, 400 do\n"
                         "  rows[i] = { text = \"Ridge\" }\n"
                         "end\n"
                         "for i = 1, 400 do\n"
                         "  local text = rows[i].text\n"
                         "  text = string.upper(text)\n"
                         "  text = string.lower(text)\n"
                         "  text = string.reverse(text)\n"
                         "  rows[i].text = text\n"
                         "end\n"
                         "return rows[1].text\n";
    TextResult text;
    host::CallHooks hooks;
    hooks.context = &text;
    hooks.read = read_text;
    put(package, "mutator.lua", source);
    const bool ran = sandbox->run_file("mutator.lua", "tick", &hooks, error);
    OA_CHECK(ran);
    OA_CHECK(text.saw);
    OA_CHECK(text.length == 5);
    OA_CHECK(std::memcmp(text.text, "egdir", 5) == 0);
    OA_CHECK(sandbox->instructions_used() >= 400);
}

} // namespace

int main() {
    std::vector<Case> cases;
    add_cases(cases);
    compare_cases(cases);
    review_and_replaced_calls_stop();
    copied_calls_stop();
    short_strings_cost_the_same();
    unit_workloads_stay_cheap();
    setmetatable_charge_stays_small();
    utf8_codes_is_one_function();
    pack_family_is_absent();
    a_mutator_rewrites_tables();
    return oa::test::check_exit_status();
}

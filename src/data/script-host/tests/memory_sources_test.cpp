// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Package files served from memory. A path matches byte for byte, so a
// different case or a trailing slash is a different file.

#include "lua.hpp"
#include "oa/data/script_host/memory_sources.hpp"
#include "oa/test/check.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace host = oa::data::script_host;

namespace {

struct Text {
    std::string value;
    bool saw = false;
};

void read_text(void* context, lua_State* state, int results) {
    auto* out = static_cast<Text*>(context);
    out->saw = false;
    if (results < 1 || lua_type(state, -1) != LUA_TSTRING)
        return;
    std::size_t length = 0;
    const char* text = lua_tolstring(state, -1, &length);
    if (text == nullptr)
        return;
    out->value.assign(text, length);
    out->saw = true;
}

std::vector<uint8_t> bytes_of(std::string_view text) {
    return std::vector<uint8_t>(text.begin(), text.end());
}

void add_find_and_read_by_exact_path() {
    host::MemorySources sources;
    const auto body = bytes_of("hi\n");
    OA_CHECK(sources.add("lua/util.lua", body));
    OA_CHECK(!sources.add("", body));
    OA_CHECK(sources.size() == 1);
    OA_CHECK(sources.total_bytes() == body.size());

    const auto found = sources.find("lua/util.lua");
    OA_CHECK(found.size() == body.size());
    OA_CHECK(found.size() == 3 && found[0] == 'h' && found[1] == 'i' && found[2] == '\n');
    OA_CHECK(sources.find("Lua/util.lua").empty());
    OA_CHECK(sources.find("lua/util.lua/").empty());
    OA_CHECK(sources.find("lua/other.lua").empty());

    const host::SourceHooks hooks = sources.hooks();
    std::vector<uint8_t> out{1, 2, 3};
    OA_CHECK(hooks.read != nullptr);
    OA_CHECK(hooks.read(hooks.context, "lua/util.lua", out));
    OA_CHECK(out == body);
    out = {9};
    OA_CHECK(!hooks.read(hooks.context, "Lua/util.lua", out));
    OA_CHECK(out.empty());
    out = {9};
    OA_CHECK(!hooks.read(hooks.context, "lua/util.lua/", out));
    OA_CHECK(out.empty());

    OA_CHECK(sources.add("empty.lua", {}));
    OA_CHECK(sources.size() == 2);
    OA_CHECK(sources.find("empty.lua").empty());
    out = {4};
    OA_CHECK(hooks.read(hooks.context, "empty.lua", out));
    OA_CHECK(out.empty());
}

void replacing_a_file() {
    host::MemorySources sources;
    const auto first = bytes_of("one");
    const auto second = bytes_of("b");
    const auto other = bytes_of("zz");
    OA_CHECK(sources.add("lua/a.lua", first));
    OA_CHECK(sources.add("lua/b.lua", other));
    OA_CHECK(sources.size() == 2);
    OA_CHECK(sources.total_bytes() == first.size() + other.size());
    OA_CHECK(sources.add("lua/a.lua", second));
    OA_CHECK(sources.size() == 2);
    OA_CHECK(sources.total_bytes() == second.size() + other.size());
    const auto replaced = sources.find("lua/a.lua");
    OA_CHECK(replaced.size() == 1 && replaced[0] == 'b');
    OA_CHECK(sources.find("lua/b.lua").size() == other.size());

    const host::SourceHooks hooks = sources.hooks();
    std::vector<uint8_t> out;
    OA_CHECK(hooks.read(hooks.context, "lua/a.lua", out));
    OA_CHECK(out == second);
}

void a_sandbox_reads_brain_and_a_required_module() {
    host::MemorySources sources;
    const auto brain = bytes_of("local got = require(\"a.b\")\nreturn got\n");
    const auto module = bytes_of("return \"from-memory\"\n");
    const auto decoy = bytes_of("return \"wrong-case\"\n");
    OA_CHECK(sources.add("brain.lua", brain));
    OA_CHECK(sources.add("lua/a/b.lua", module));
    OA_CHECK(sources.add("Lua/a/b.lua", decoy));
    OA_CHECK(sources.size() == 3);

    host::ScriptError error;
    host::OpenOptions options;
    options.package_id = "steady-commander";
    options.profile = host::mutator_profile();
    options.sources = sources.hooks();
    auto sandbox = host::Sandbox::open(options, error);
    OA_CHECK(sandbox.has_value());
    if (!sandbox)
        return;

    Text text;
    host::CallHooks hooks;
    hooks.context = &text;
    hooks.read = read_text;
    OA_CHECK(sandbox->run_file("brain.lua", "load", &hooks, error));
    OA_CHECK(error.kind == host::ErrorKind::none);
    OA_CHECK(text.saw);
    OA_CHECK(text.value == "from-memory");
}

} // namespace

int main() {
    add_find_and_read_by_exact_path();
    replacing_a_file();
    a_sandbox_reads_brain_and_a_required_module();
    return oa::test::check_exit_status();
}

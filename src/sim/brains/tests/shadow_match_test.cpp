// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A shadow beside the built-in player on the computer-build match: the siege
// section stays level with the built-in player, the sort does not, and the
// set-up record feeds brain.setup_random.

#include "match_fixture.hpp"

#include "oa/core/game_state.h"
#include "oa/sim/brains/seat.hpp"
#include "oa/sim/brains/shadow.hpp"
#include "oa/sim/match_runtime/script.hpp"
#include "oa/sim/state_hash.hpp"

#include "lua.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace brains = oa::sim::brains;
namespace host = oa::data::script_host;

std::string read_text(const char* name) {
    const std::filesystem::path path = std::filesystem::path(OA_STEADY_SHADOW_DIR) / name;
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error(std::string("missing ") + name);
    std::string text((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (text.empty())
        throw std::runtime_error(std::string("empty ") + name);
    return text;
}

struct HeldBrain {
    std::string lua;
    std::string yaml;
};

brains::SeatOutcome seat_package(
    Fixture& fixture,
    const std::string& lua,
    std::string_view yaml,
    bool shadow,
    const char* sections
) {
    std::vector<brains::PackageFile> files;
    files.push_back(
        {"brain.lua",
         std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(lua.data()), lua.size()}}
    );
    if (!yaml.empty()) {
        files.push_back(
            {"oabrain.yaml",
             std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(yaml.data()), yaml.size()}}
        );
    }
    brains::SeatSpec spec;
    spec.player = computer;
    spec.id = "steady-shadow";
    spec.name = "Steady Shadow";
    spec.version = "1.0";
    spec.entry = "brain.lua";
    spec.level = "steady";
    spec.difficulty = OA_DIFFICULTY_MEDIUM;
    spec.files = files;
    spec.shadow = shadow;
    spec.shadow_sections = sections != nullptr ? sections : "";
    return brains::seat_brain(*fixture.match, spec);
}

void require_seated(const brains::SeatOutcome& outcome) {
    if (outcome.result == brains::SeatResult::seated)
        return;
    throw std::runtime_error(std::string("seat: ") + outcome.reason.data());
}

void arm(Fixture& fixture) {
    fixture.spawn(0, vehicle, 8, 8);
    fixture.spawn(computer, vehicle, 40, 40);
    CHECK(sim::ai::configure_match_computer_players(*fixture.match, profile, build_lists, false));
}

uint64_t digest_of(Fixture& fixture) {
    return oa::sim::trace::match_state_hash(
        *fixture.match, oa::base::game_loop::Timing{}, 0, 0, nullptr
    );
}

void step(Fixture& fixture) {
    ++fixture.match->state().game.tick;
    fixture.match->tick();
}

void play_level(Fixture& shadowed, Fixture& plain) {
    CHECK(shadowed.match->random_state() == plain.match->random_state());
    CHECK(digest_of(shadowed) == digest_of(plain));
    for (uint32_t tick = 1; tick <= run_ticks; ++tick) {
        step(shadowed);
        step(plain);
        if (shadowed.match->random_state() != plain.match->random_state())
            throw std::runtime_error("random state at tick " + std::to_string(tick));
        if (digest_of(shadowed) != digest_of(plain))
            throw std::runtime_error("match digest at tick " + std::to_string(tick));
    }
}

void test_task9_stays_level() {
    const std::string lua = read_text("brain.lua");
    const std::string yaml = read_text("oabrain.yaml");
    Fixture shadowed;
    Fixture plain;
    arm(shadowed);
    arm(plain);
    require_seated(seat_package(shadowed, lua, yaml, true, "task9"));
    CHECK(brains::brain_is_shadow(*shadowed.match, computer));
    CHECK(!brains::brain_is_shadow(*plain.match, computer));
    play_level(shadowed, plain);
    const brains::ShadowReport* report = brains::shadow_report(*shadowed.match, computer);
    CHECK(report != nullptr);
    CHECK(report->start == brains::ShadowStart::not_compared);
    CHECK(report->ticks == run_ticks);
    CHECK(report->differ_count == 0);
    CHECK(!report->stopped);
}

void test_sort_differs_at_tick_30() {
    const std::string lua = read_text("brain.lua");
    const std::string yaml = read_text("oabrain.yaml");
    Fixture shadowed;
    arm(shadowed);
    require_seated(seat_package(shadowed, lua, yaml, true, "sort"));
    for (uint32_t tick = 0; tick < run_ticks; ++tick)
        step(shadowed);
    const brains::ShadowReport* report = brains::shadow_report(*shadowed.match, computer);
    CHECK(report != nullptr);
    CHECK(report->differ_count > 0);
    CHECK(report->first_tick == 30);
    CHECK(std::strcmp(report->section, "sort") == 0);
    CHECK(!report->stopped);
}

struct SetupRead {
    const sim::match_runtime::SetUpRecord* record{};
    bool ok{true};
    char message[160]{};
};

void read_setup_draws(void* context, lua_State* state) {
    auto* held = static_cast<SetupRead*>(context);
    if (held == nullptr || held->record == nullptr)
        return;
    lua_getglobal(state, "brain");
    lua_getfield(state, -1, "setup_random");
    if (lua_type(state, -1) != LUA_TFUNCTION) {
        held->ok = false;
        std::snprintf(held->message, sizeof held->message, "setup_random is missing");
        return;
    }
    for (uint8_t draw = 0; draw < held->record->draw_count; ++draw) {
        lua_pushvalue(state, -1);
        lua_pushinteger(state, static_cast<lua_Integer>(held->record->draws[draw].bound));
        if (lua_pcall(state, 1, 1, 0) != 0) {
            held->ok = false;
            const char* text = lua_tostring(state, -1);
            std::snprintf(
                held->message,
                sizeof held->message,
                "%s",
                text != nullptr ? text : "setup_random failed"
            );
            return;
        }
        const lua_Integer value = lua_tointeger(state, -1);
        lua_pop(state, 1);
        if (value != static_cast<lua_Integer>(held->record->draws[draw].value)) {
            held->ok = false;
            std::snprintf(
                held->message, sizeof held->message, "draw %u", static_cast<unsigned>(draw)
            );
            return;
        }
    }
}

void test_setup_record() {
    const std::string lua = read_text("brain.lua");
    const std::string yaml = read_text("oabrain.yaml");
    Fixture fixture;
    arm(fixture);
    require_seated(seat_package(fixture, lua, yaml, true, "task9"));
    step(fixture);
    const sim::match_runtime::SetUpRecord* record =
        brains::seat_setup_record(*fixture.match, computer);
    CHECK(record != nullptr);
    CHECK(record->draw_count == 8);
    oa::sim::match_runtime::SharedRandom copy{1};
    copy.restore(record->stream_state);
    for (uint8_t draw = 0; draw < record->draw_count; ++draw) {
        const uint32_t value = copy.bounded(record->draws[draw].bound);
        CHECK(value == record->draws[draw].value);
    }
    const uint32_t stream = fixture.match->random_state();
    host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, computer);
    CHECK(sandbox != nullptr);
    SetupRead held;
    held.record = record;
    host::ScriptError error;
    CHECK(sandbox->run_host("setup", &held, read_setup_draws, error));
    if (!held.ok)
        throw std::runtime_error(held.message);
    CHECK(fixture.match->random_state() == stream);
}

struct FieldPresent {
    int present{-1};
};

void read_setup_field(void* context, lua_State* state) {
    auto* held = static_cast<FieldPresent*>(context);
    lua_getglobal(state, "brain");
    lua_getfield(state, -1, "setup_random");
    held->present = lua_isnil(state, -1) == 0 ? 1 : 0;
}

void test_other_brains_have_no_setup_random() {
    const char* quiet = "local hooks = {}\n"
                        "function hooks.onTick()\n"
                        "end\n"
                        "return hooks\n";
    Fixture fixture;
    arm(fixture);
    require_seated(seat_package(fixture, quiet, {}, false, nullptr));
    CHECK(!brains::brain_is_shadow(*fixture.match, computer));
    CHECK(brains::brain_seated(*fixture.match, computer));
    step(fixture);
    const sim::match_runtime::SetUpRecord* record =
        brains::seat_setup_record(*fixture.match, computer);
    CHECK(record != nullptr);
    CHECK(record->draw_count == 8);
    host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, computer);
    CHECK(sandbox != nullptr);
    FieldPresent held;
    host::ScriptError error;
    CHECK(sandbox->run_host("read", &held, read_setup_field, error));
    CHECK(held.present == 0);
}

} // namespace

int main() {
    try {
        test_task9_stays_level();
        test_sort_differs_at_tick_30();
        test_setup_record();
        test_other_brains_have_no_setup_random();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "FAIL: %s\n", error.what());
        return 1;
    }
    std::printf("shadow match tests passed\n");
    return 0;
}

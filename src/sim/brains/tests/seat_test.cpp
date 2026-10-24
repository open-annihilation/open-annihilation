// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Seating the made-up brain steady-commander: fits and start, the libraries,
// refusals, and a match that ends with two brains seated.

#include "match_fixture.hpp"

#include "oa/base/sha256.hpp"
#include "oa/core/game_state.h"
#include "oa/core/player.h"
#include "oa/data/script_host/fixed.hpp"
#include "oa/data/script_host/plain_data.hpp"
#include "oa/sim/brains/seat.hpp"
#include "oa/sim/unit_movement/movement.hpp"

#include "lua.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace brains = oa::sim::brains;
namespace host = oa::data::script_host;

struct StoredOption {
    std::string id;
    brains::OptionKind kind{};
    bool toggle{};
    double number{};
    std::string text;
};

struct Package {
    std::string id{"steady-commander"};
    std::string name{"Steady Commander"};
    std::string version{"1"};
    std::string entry{"brain.lua"};
    std::string level{"hard"};
    int32_t difficulty{OA_DIFFICULTY_HARD};
    uint8_t player{computer};
    bool full_awareness{};
    bool with_network{true};
    bool corrupt_network{};
    std::vector<std::pair<std::string, std::string>> files;
    std::vector<StoredOption> options;
};

void add_file(Package& package, std::string path, std::string text) {
    package.files.emplace_back(std::move(path), std::move(text));
}

brains::SeatOutcome
seat_it(Fixture& fixture, Package& package, std::span<const uint8_t> saved = {}) {
    std::vector<brains::PackageFile> files;
    files.reserve(package.files.size());
    for (const auto& held : package.files)
        files.push_back(
            {held.first,
             std::span<const uint8_t>{
                 reinterpret_cast<const uint8_t*>(held.second.data()), held.second.size()
             }}
        );

    std::vector<brains::OptionValue> options;
    options.reserve(package.options.size());
    for (const StoredOption& option : package.options)
        options.push_back({option.id, option.kind, option.toggle, option.number, option.text});

    std::vector<brains::NetworkFile> networks;
    if (package.with_network) {
        std::span<const uint8_t> bytes;
        for (const brains::PackageFile& file : files) {
            if (file.path == "nets/scout.oann")
                bytes = file.bytes;
        }
        auto digest = oa::base::sha256::digest_of(bytes);
        if (package.corrupt_network)
            digest[0] = static_cast<uint8_t>(digest[0] ^ 0x1u);
        networks.push_back({"scout", "nets/scout.oann", digest});
    }

    brains::SeatSpec spec;
    spec.player = package.player;
    spec.id = package.id;
    spec.name = package.name;
    spec.version = package.version;
    spec.entry = package.entry;
    spec.level = package.level;
    spec.difficulty = package.difficulty;
    spec.full_awareness = package.full_awareness;
    spec.options = options;
    spec.files = files;
    spec.networks = networks;
    spec.saved = saved;
    return brains::seat_brain(*fixture.match, spec);
}

std::string_view reason_of(const brains::SeatOutcome& outcome) {
    return std::string_view(outcome.reason.data());
}

void arm(Fixture& fixture) {
    fixture.spawn(0, vehicle, 8, 8);
    fixture.spawn(computer, vehicle, 40, 40);
    CHECK(sim::ai::configure_match_computer_players(*fixture.match, profile, build_lists, false));
}

void play_out(Fixture& fixture) {
    for (uint32_t tick = 0; tick < run_ticks; ++tick) {
        ++fixture.match->state().game.tick;
        fixture.match->tick();
    }
}

void copy_lua_text(lua_State* state, int index, char* dest, std::size_t room) {
    std::size_t length = 0;
    const char* text = lua_tolstring(state, index, &length);
    std::size_t count = 0;
    if (text != nullptr && room > 0) {
        count = length < room - 1 ? length : room - 1;
        for (std::size_t index_byte = 0; index_byte < count; ++index_byte)
            dest[index_byte] = text[index_byte];
    }
    if (room > 0)
        dest[count] = '\0';
}

struct Seen {
    lua_Integer player{-1};
    char level[32]{};
    char difficulty[16]{};
    int hold{-1};
    double waves{};
    char stance[16]{};
    double pace{};
    char brain_kind[16]{};
    char game_kind[16]{};
    char third_kind[16]{};
    char f32_kind[16]{};
    char geometry_kind[16]{};
    char cosine_kind[16]{};
    int sine_ok{};
    int64_t sine{};
    char mark[16]{};
    lua_Integer note{-1};
};

void read_seen(void* context, lua_State* state) {
    auto* seen = static_cast<Seen*>(context);
    lua_getglobal(state, "seen_player");
    if (lua_isinteger(state, -1) != 0)
        seen->player = lua_tointeger(state, -1);
    lua_pop(state, 1);

    lua_getglobal(state, "seen_level");
    copy_lua_text(state, -1, seen->level, sizeof seen->level);
    lua_pop(state, 1);
    lua_getglobal(state, "seen_difficulty");
    copy_lua_text(state, -1, seen->difficulty, sizeof seen->difficulty);
    lua_pop(state, 1);

    lua_getglobal(state, "seen_hold");
    if (lua_isboolean(state, -1) != 0)
        seen->hold = lua_toboolean(state, -1);
    lua_pop(state, 1);
    lua_getglobal(state, "seen_waves");
    if (lua_isnumber(state, -1) != 0)
        seen->waves = lua_tonumber(state, -1);
    lua_pop(state, 1);
    lua_getglobal(state, "seen_stance");
    copy_lua_text(state, -1, seen->stance, sizeof seen->stance);
    lua_pop(state, 1);
    lua_getglobal(state, "seen_pace");
    if (lua_isnumber(state, -1) != 0)
        seen->pace = lua_tonumber(state, -1);
    lua_pop(state, 1);

    lua_getglobal(state, "seen_brain");
    copy_lua_text(state, -1, seen->brain_kind, sizeof seen->brain_kind);
    lua_pop(state, 1);
    lua_getglobal(state, "seen_game");
    copy_lua_text(state, -1, seen->game_kind, sizeof seen->game_kind);
    lua_pop(state, 1);
    lua_getglobal(state, "seen_third");
    copy_lua_text(state, -1, seen->third_kind, sizeof seen->third_kind);
    lua_pop(state, 1);
    lua_getglobal(state, "seen_f32");
    copy_lua_text(state, -1, seen->f32_kind, sizeof seen->f32_kind);
    lua_pop(state, 1);
    lua_getglobal(state, "seen_geometry");
    copy_lua_text(state, -1, seen->geometry_kind, sizeof seen->geometry_kind);
    lua_pop(state, 1);
    lua_getglobal(state, "seen_cosine");
    copy_lua_text(state, -1, seen->cosine_kind, sizeof seen->cosine_kind);
    lua_pop(state, 1);

    lua_getglobal(state, "seen_sine");
    int64_t count = 0;
    if (host::to_fixed(state, -1, &count)) {
        seen->sine_ok = 1;
        seen->sine = count;
    }
    lua_pop(state, 1);

    lua_getglobal(state, "seen_mark");
    copy_lua_text(state, -1, seen->mark, sizeof seen->mark);
    lua_pop(state, 1);
    lua_getglobal(state, "seen_note");
    if (lua_isinteger(state, -1) != 0)
        seen->note = lua_tointeger(state, -1);
    lua_pop(state, 1);
}

const char* ready_script() {
    return R"(local hooks = {}
function hooks.fits(game)
  return true
end
function hooks.start(game, me, third)
  seen_player = me.player
  seen_level = me.level
  seen_difficulty = me.difficulty
  seen_hold = me.options.hold
  seen_waves = me.options.waves
  seen_stance = me.options.stance
  seen_pace = me.options.pace
  seen_brain = type(brain)
  seen_game = type(game)
  seen_third = type(third)
  seen_f32 = type(f32)
  seen_geometry = type(geometry)
  seen_cosine = type(geometry.cosine_scaled)
  seen_sine = geometry.sine_scaled(16384, 1)
  local wave = require("tactics.wave")
  seen_mark = wave.mark
  seen_note = hooks.note
end
hooks.note = 1
return hooks
)";
}

Package ready_package() {
    Package package;
    add_file(package, "brain.lua", ready_script());
    add_file(package, "lua/tactics/wave.lua", "return { mark = \"steady\" }\n");
    add_file(package, "nets/scout.oann", "scout");
    package.options.push_back({"hold", brains::OptionKind::toggle, true, 0.0, {}});
    package.options.push_back({"waves", brains::OptionKind::number, false, 3.0, {}});
    package.options.push_back({"stance", brains::OptionKind::choice, false, 0.0, "hold"});
    package.options.push_back({"pace", brains::OptionKind::choice, false, 2.0, {}});
    return package;
}

void me_game_and_libraries() {
    Fixture fixture;
    Package package = ready_package();
    const brains::SeatOutcome outcome = seat_it(fixture, package);
    CHECK(outcome.result == brains::SeatResult::seated);
    CHECK(brains::brain_seated(*fixture.match, computer));
    host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, computer);
    CHECK(sandbox != nullptr);
    Seen seen;
    host::ScriptError error;
    CHECK(sandbox->run_host("read", &seen, read_seen, error));
    CHECK(seen.player == 1);
    CHECK(std::string_view(seen.level) == "hard");
    CHECK(std::string_view(seen.difficulty) == "hard");
    CHECK(seen.hold == 1);
    CHECK(seen.waves == 3.0);
    CHECK(std::string_view(seen.stance) == "hold");
    CHECK(seen.pace == 2.0);
    CHECK(std::string_view(seen.brain_kind) == "table");
    CHECK(std::string_view(seen.game_kind) == "table");
    CHECK(std::string_view(seen.third_kind) == "nil");
    CHECK(std::string_view(seen.f32_kind) == "table");
    CHECK(std::string_view(seen.geometry_kind) == "table");
    CHECK(std::string_view(seen.cosine_kind) == "function");
    CHECK(seen.sine_ok == 1);
    const int32_t scaled = sim::unit_movement::sine_scaled(16384, 65536);
    CHECK(seen.sine == static_cast<int64_t>(scaled));
    CHECK(std::string_view(seen.mark) == "steady");
    CHECK(seen.note == 1);
}

void fits_true_seats() {
    Fixture fixture;
    Package package;
    add_file(
        package,
        "brain.lua",
        "local hooks = {}\nfunction hooks.fits() return true end\nreturn hooks\n"
    );
    add_file(package, "nets/scout.oann", "scout");
    const brains::SeatOutcome outcome = seat_it(fixture, package);
    CHECK(outcome.result == brains::SeatResult::seated);
    CHECK(brains::brain_seated(*fixture.match, computer));
}

void fits_reason_leaves_the_built_in_player() {
    Fixture fixture;
    Package package;
    add_file(
        package,
        "brain.lua",
        "local hooks = {}\n"
        "function hooks.fits() return \"No build list for this mod's third side\" end\n"
        "return hooks\n"
    );
    add_file(package, "nets/scout.oann", "scout");
    const brains::SeatOutcome outcome = seat_it(fixture, package);
    CHECK(outcome.result == brains::SeatResult::does_not_fit);
    CHECK(reason_of(outcome) == "No build list for this mod's third side");
    CHECK(!brains::brain_seated(*fixture.match, computer));
    CHECK(brains::seat_sandbox(*fixture.match, computer) == nullptr);
    CHECK(fixture.match->seat_hooks(computer).play == nullptr);
    arm(fixture);
    play_out(fixture);
    CHECK(fixture.count(computer, collector) != 0);
}

void fits_nil_fails() {
    Fixture fixture;
    Package package;
    add_file(
        package,
        "brain.lua",
        "local hooks = {}\nfunction hooks.fits() return nil end\nreturn hooks\n"
    );
    add_file(package, "nets/scout.oann", "scout");
    const brains::SeatOutcome outcome = seat_it(fixture, package);
    CHECK(outcome.result == brains::SeatResult::failed);
    CHECK(outcome.error.kind == host::ErrorKind::runtime);
    CHECK(outcome.error.message == "fits must return true or a reason");
    CHECK(reason_of(outcome) == "fits must return true or a reason");
    CHECK(!brains::brain_seated(*fixture.match, computer));
    CHECK(brains::seat_sandbox(*fixture.match, computer) == nullptr);
}

void syntax_error_names_the_module() {
    Fixture fixture;
    Package package;
    add_file(package, "brain.lua", "require(\"util\")\nreturn {}\n");
    add_file(package, "lua/util.lua", "return (\n");
    add_file(package, "nets/scout.oann", "scout");
    const brains::SeatOutcome outcome = seat_it(fixture, package);
    CHECK(outcome.result == brains::SeatResult::failed);
    CHECK(outcome.error.kind == host::ErrorKind::syntax);
    CHECK(outcome.error.chunk == "lua/util.lua");
    CHECK(outcome.error.line != 0);
    CHECK(!brains::brain_seated(*fixture.match, computer));
    CHECK(brains::seat_sandbox(*fixture.match, computer) == nullptr);
}

void entry_returning_a_number_fails() {
    Fixture fixture;
    Package package;
    add_file(package, "brain.lua", "return 1\n");
    add_file(package, "nets/scout.oann", "scout");
    const brains::SeatOutcome outcome = seat_it(fixture, package);
    CHECK(outcome.result == brains::SeatResult::failed);
    CHECK(reason_of(outcome) == "brain.lua must return a table of hooks");
    CHECK(outcome.error.message == "brain.lua must return a table of hooks");
    CHECK(!brains::brain_seated(*fixture.match, computer));
}

void on_tick_must_be_a_function() {
    Fixture fixture;
    Package package;
    add_file(package, "brain.lua", "return { onTick = 5 }\n");
    add_file(package, "nets/scout.oann", "scout");
    const brains::SeatOutcome outcome = seat_it(fixture, package);
    CHECK(outcome.result == brains::SeatResult::failed);
    CHECK(reason_of(outcome) == "brain.lua: onTick must be a function");
    CHECK(outcome.error.chunk == "brain.lua");
    CHECK(!brains::brain_seated(*fixture.match, computer));
}

void missing_entry_is_refused() {
    Fixture fixture;
    Package package;
    package.entry = "missing.lua";
    add_file(package, "nets/scout.oann", "scout");
    const brains::SeatOutcome outcome = seat_it(fixture, package);
    CHECK(outcome.result == brains::SeatResult::refused);
    CHECK(reason_of(outcome) == "missing.lua is not among the package files");
    CHECK(brains::seat_sandbox(*fixture.match, computer) == nullptr);
    CHECK(!brains::brain_seated(*fixture.match, computer));
}

void bad_digest_runs_no_lua() {
    Fixture fixture;
    Package package;
    package.corrupt_network = true;
    add_file(package, "brain.lua", "ran = true\nreturn {}\n");
    add_file(package, "nets/scout.oann", "scout");
    const brains::SeatOutcome outcome = seat_it(fixture, package);
    CHECK(outcome.result == brains::SeatResult::refused);
    CHECK(reason_of(outcome) == "nets/scout.oann does not match its sha256 in oabrain.yaml");
    CHECK(brains::seat_sandbox(*fixture.match, computer) == nullptr);
    CHECK(!brains::brain_seated(*fixture.match, computer));
}

void start_stops_at_the_phase_allowance() {
    Fixture fixture;
    Package package;
    add_file(
        package,
        "brain.lua",
        "local hooks = {}\nfunction hooks.start() while true do end end\nreturn hooks\n"
    );
    add_file(package, "nets/scout.oann", "scout");
    const brains::SeatOutcome outcome = seat_it(fixture, package);
    CHECK(outcome.result == brains::SeatResult::failed);
    CHECK(outcome.error.kind == host::ErrorKind::instruction_budget);
    CHECK(outcome.error.message == "the script ran past its budget of 50000000 instructions");
    CHECK(!brains::brain_seated(*fixture.match, computer));
    CHECK(brains::seat_sandbox(*fixture.match, computer) == nullptr);
}

void local_unused_and_a_second_seat_are_refused() {
    Fixture fixture;
    Package local = ready_package();
    local.player = 0;
    const brains::SeatOutcome local_outcome = seat_it(fixture, local);
    CHECK(local_outcome.result == brains::SeatResult::refused);
    CHECK(reason_of(local_outcome) == "the player is not a computer");

    Package unused = ready_package();
    unused.player = 3;
    const brains::SeatOutcome unused_outcome = seat_it(fixture, unused);
    CHECK(unused_outcome.result == brains::SeatResult::refused);
    CHECK(reason_of(unused_outcome) == "the player is not in the game");

    Package outside = ready_package();
    outside.player = 99;
    const brains::SeatOutcome outside_outcome = seat_it(fixture, outside);
    CHECK(outside_outcome.result == brains::SeatResult::refused);
    CHECK(reason_of(outside_outcome) == "the player is outside 0 to 9");

    Package first = ready_package();
    CHECK(seat_it(fixture, first).result == brains::SeatResult::seated);
    Package second = ready_package();
    const brains::SeatOutcome again = seat_it(fixture, second);
    CHECK(again.result == brains::SeatResult::refused);
    CHECK(reason_of(again) == "the seat already has a brain");
    CHECK(brains::brain_seated(*fixture.match, computer));
}

void seated_brain_skips_the_order_tick() {
    Fixture fixture;
    arm(fixture);
    Package package;
    add_file(package, "brain.lua", "return {}\n");
    add_file(package, "nets/scout.oann", "scout");
    const brains::SeatOutcome outcome = seat_it(fixture, package);
    CHECK(outcome.result == brains::SeatResult::seated);
    CHECK(brains::brain_seated(*fixture.match, computer));
    auto* players = sim::ai::match_computer_players(*fixture.match);
    CHECK(players != nullptr);
    CHECK(players->difficulty_set[computer]);
    CHECK(players->difficulty[computer] == OA_DIFFICULTY_HARD);
    play_out(fixture);
    CHECK(fixture.count(computer, collector) == 0);
    CHECK(
        players->players[computer].sort_countdown ==
        static_cast<int32_t>(sim::ai::sort_interval_ticks)
    );
}

struct HeldBytes {
    std::vector<uint8_t> bytes;
};

void keep_bytes(void* context, std::span<const uint8_t> bytes) {
    static_cast<HeldBytes*>(context)->bytes.assign(bytes.begin(), bytes.end());
}

struct StartsRead {
    const uint8_t* bytes{};
    std::size_t count{};
    lua_Integer starts{-1};
    int ok{};
};

void read_starts(void* context, lua_State* state) {
    auto* box = static_cast<StartsRead*>(context);
    host::push_plain_data(state, std::span<const uint8_t>(box->bytes, box->count));
    lua_getfield(state, -1, "starts");
    if (lua_isinteger(state, -1) != 0) {
        box->starts = lua_tointeger(state, -1);
        box->ok = 1;
    }
    lua_pop(state, 2);
}

struct HeldStart {
    lua_Integer starts{-1};
};

void read_held(void* context, lua_State* state) {
    auto* box = static_cast<HeldStart*>(context);
    lua_getglobal(state, "held_starts");
    if (lua_isinteger(state, -1) != 0)
        box->starts = lua_tointeger(state, -1);
    lua_pop(state, 1);
}

Package script_package(std::string script) {
    Package package;
    package.with_network = false;
    add_file(package, "brain.lua", std::move(script));
    return package;
}

void save_returns_a_table() {
    Fixture fixture;
    Package package = script_package(
        "local brain = {}\n"
        "function brain.save()\n"
        "  return { starts = 3 }\n"
        "end\n"
        "return brain\n"
    );
    CHECK(seat_it(fixture, package).result == brains::SeatResult::seated);
    HeldBytes held;
    brains::SaveHooks hooks;
    hooks.context = &held;
    hooks.accept = keep_bytes;
    CHECK(brains::save_brain_state(*fixture.match, computer, hooks) == brains::SaveOutcome::stored);
    CHECK(!held.bytes.empty());
    CHECK(brains::brain_seated(*fixture.match, computer));
    host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, computer);
    CHECK(sandbox != nullptr);
    StartsRead read;
    read.bytes = held.bytes.data();
    read.count = held.bytes.size();
    host::ScriptError error;
    CHECK(sandbox->run_host("read", &read, read_starts, error));
    CHECK(read.ok == 1);
    CHECK(read.starts == 3);
}

void save_absent_when_the_brain_has_none() {
    Fixture fixture;
    Package package = script_package("return {}\n");
    CHECK(seat_it(fixture, package).result == brains::SeatResult::seated);
    CHECK(brains::save_brain_state(*fixture.match, computer, {}) == brains::SaveOutcome::absent);
    CHECK(brains::brain_seated(*fixture.match, computer));
}

void save_that_raises_hands_the_seat_back() {
    Fixture fixture;
    Package package = script_package(
        "local brain = {}\n"
        "function brain.save()\n"
        "  error(\"no\")\n"
        "end\n"
        "return brain\n"
    );
    CHECK(seat_it(fixture, package).result == brains::SeatResult::seated);
    CHECK(brains::save_brain_state(*fixture.match, computer, {}) == brains::SaveOutcome::failed);
    CHECK(!brains::brain_seated(*fixture.match, computer));
}

void save_over_one_mebibyte_is_omitted() {
    Fixture fixture;
    Package package = script_package(
        "local brain = {}\n"
        "function brain.save()\n"
        "  local parts = {}\n"
        "  for i = 1, 20 do\n"
        "    parts[i] = string.rep(\"x\", 60000)\n"
        "  end\n"
        "  return parts\n"
        "end\n"
        "return brain\n"
    );
    CHECK(seat_it(fixture, package).result == brains::SeatResult::seated);
    CHECK(brains::save_brain_state(*fixture.match, computer, {}) == brains::SaveOutcome::omitted);
    CHECK(brains::brain_seated(*fixture.match, computer));
}

void start_receives_the_saved_state() {
    Fixture fixture;
    Package writer = script_package(
        "local brain = {}\n"
        "function brain.save()\n"
        "  return { starts = 4 }\n"
        "end\n"
        "return brain\n"
    );
    CHECK(seat_it(fixture, writer).result == brains::SeatResult::seated);
    HeldBytes held;
    brains::SaveHooks hooks;
    hooks.context = &held;
    hooks.accept = keep_bytes;
    CHECK(brains::save_brain_state(*fixture.match, computer, hooks) == brains::SaveOutcome::stored);
    fixture.match->simulation().players[2].present = true;
    fixture.match->simulation().players[2].status = OA_PLAYER_STATUS_COMPUTER;
    Package reader = script_package(
        "local brain = {}\n"
        "function brain.start(game, me, saved)\n"
        "  held_starts = -1\n"
        "  if type(saved) == \"table\" and type(saved.starts) == \"number\" then\n"
        "    held_starts = saved.starts\n"
        "  end\n"
        "end\n"
        "return brain\n"
    );
    reader.player = 2;
    CHECK(seat_it(fixture, reader, held.bytes).result == brains::SeatResult::seated);
    host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, 2);
    CHECK(sandbox != nullptr);
    HeldStart seen;
    host::ScriptError error;
    CHECK(sandbox->run_host("read", &seen, read_held, error));
    CHECK(seen.starts == 4);
}

void instruction_totals_cover_load_tick_and_a_stop() {
    Fixture fixture;
    arm(fixture);
    Package quiet;
    add_file(
        quiet,
        "brain.lua",
        "local n = 0\n"
        "for i = 1, 2000 do n = n + i end\n"
        "return {\n"
        "  onTick = function()\n"
        "    local m = 0\n"
        "    for i = 1, 2000 do m = m + i end\n"
        "  end\n"
        "}\n"
    );
    add_file(quiet, "nets/scout.oann", "scout");
    CHECK(seat_it(fixture, quiet).result == brains::SeatResult::seated);
    host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, computer);
    CHECK(sandbox != nullptr);
    const uint64_t loaded = brains::seat_instructions(*fixture.match, computer);
    CHECK(loaded > 0);
    CHECK(loaded == sandbox->instructions_used());
    ++fixture.match->state().game.tick;
    fixture.match->tick();
    sandbox = brains::seat_sandbox(*fixture.match, computer);
    CHECK(sandbox != nullptr);
    const uint64_t ticked = sandbox->instructions_used();
    CHECK(ticked > 0);
    CHECK(brains::seat_instructions(*fixture.match, computer) == loaded + ticked);
    CHECK(brains::seat_instructions(*fixture.match, 0) == 0);
    CHECK(brains::seat_instructions(*fixture.match, 99) == 0);

    Fixture stopping;
    arm(stopping);
    Package breaker;
    add_file(
        breaker,
        "brain.lua",
        "return {\n"
        "  onTick = function()\n"
        "    local n = 0\n"
        "    for i = 1, 2000 do n = n + i end\n"
        "    error(\"breaks\")\n"
        "  end\n"
        "}\n"
    );
    add_file(breaker, "nets/scout.oann", "scout");
    CHECK(seat_it(stopping, breaker).result == brains::SeatResult::seated);
    const uint64_t before = brains::seat_instructions(*stopping.match, computer);
    ++stopping.match->state().game.tick;
    stopping.match->tick();
    CHECK(brains::seat_sandbox(*stopping.match, computer) == nullptr);
    CHECK(brains::seat_instructions(*stopping.match, computer) > before);
}

void two_brains_end_with_the_match() {
    Fixture fixture;
    fixture.match->simulation().players[2].present = true;
    fixture.match->simulation().players[2].status = OA_PLAYER_STATUS_COMPUTER;
    Package first;
    first.player = computer;
    add_file(first, "brain.lua", "return {}\n");
    add_file(first, "nets/scout.oann", "scout");
    Package second = first;
    second.player = 2;
    CHECK(seat_it(fixture, first).result == brains::SeatResult::seated);
    CHECK(seat_it(fixture, second).result == brains::SeatResult::seated);
    CHECK(brains::brain_seated(*fixture.match, computer));
    CHECK(brains::brain_seated(*fixture.match, 2));
    CHECK(brains::seat_sandbox(*fixture.match, computer) != nullptr);
    CHECK(brains::seat_sandbox(*fixture.match, 2) != nullptr);
    fixture.match.reset();
    CHECK(fixture.match == nullptr);
}

} // namespace

int main() {
    try {
        me_game_and_libraries();
        fits_true_seats();
        fits_reason_leaves_the_built_in_player();
        fits_nil_fails();
        syntax_error_names_the_module();
        entry_returning_a_number_fails();
        on_tick_must_be_a_function();
        missing_entry_is_refused();
        bad_digest_runs_no_lua();
        start_stops_at_the_phase_allowance();
        local_unused_and_a_second_seat_are_refused();
        seated_brain_skips_the_order_tick();
        save_returns_a_table();
        save_absent_when_the_brain_has_none();
        save_that_raises_hands_the_seat_back();
        save_over_one_mebibyte_is_omitted();
        start_receives_the_saved_state();
        instruction_totals_cover_load_tick_and_a_stop();
        two_brains_end_with_the_match();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "sim-brains-seat: %s\n", ex.what());
        return 1;
    }
    return 0;
}

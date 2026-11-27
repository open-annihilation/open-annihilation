// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The seat's own units counted by name and by tag, and the match's unit
// limit. Probes run in the seated sandbox and raise on the first wrong
// answer; the turns record what onTick read.

#include "match_fixture.hpp"

#include "internal.hpp"

#include "oa/base/game_loop.hpp"
#include "oa/core/game_state.h"
#include "oa/core/player.h"
#include "oa/data/enhanced_mode.hpp"
#include "oa/sim/brains/handles.hpp"
#include "oa/sim/brains/seat.hpp"
#include "oa/sim/state_hash.hpp"

#include "lua.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <span>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace brains = oa::sim::brains;
namespace host = oa::data::script_host;
namespace enhanced = oa::data::enhanced_mode;

constexpr uint8_t local_player = 0;
constexpr uint16_t revision_one_limit = 10'000;
constexpr uint16_t both_players = 0b11;

// What onTick reads each turn, kept in globals the test reads back.
constexpr const char* recording_brain = R"(local hooks = {}
seen = {}
function hooks.onTick(tick)
  seen[#seen + 1] = tick .. " " .. me:unit_count() .. " " .. me:unit_count_of("ARMCV") ..
    " " .. me:unit_count_of("level1") .. " " .. game.unit_limit
end
return hooks
)";

constexpr const char* quiet_brain = "return {}\n";

// The checks every probe can call. A wrong answer raises with its label.
constexpr const char* prelude = R"(
local function expect(got, want, label)
  if got ~= want then
    error(label .. ": got " .. tostring(got) .. " want " .. tostring(want), 2)
  end
end
local function refused(value, label)
  local ok, message = pcall(me.unit_count_of, me, value)
  if ok then
    error(label .. ": was not refused", 2)
  end
  local usage = "unit_count_of expects a unit name, a tag, a unit record or a type index"
  if not string.find(tostring(message), usage, 1, true) then
    error(label .. ": " .. tostring(message), 2)
  end
end
)";

struct Chunk {
    std::string source;
};

int push_chunk(void* context, lua_State* lua) {
    const auto* chunk = static_cast<const Chunk*>(context);
    if (luaL_loadstring(lua, chunk->source.c_str()) != 0) {
        const char* message = lua_tostring(lua, -1);
        return luaL_error(lua, "%s", message == nullptr ? "probe failed to load" : message);
    }
    return 0;
}

/// Runs `source` after the prelude in the seat's sandbox. A raise fails the test.
void probe(host::Sandbox& sandbox, const std::string& source) {
    Chunk chunk{std::string(prelude) + source};
    host::CallHooks hooks;
    hooks.context = &chunk;
    hooks.push = push_chunk;
    host::ScriptError error;
    if (!sandbox.call("probe", hooks, error))
        throw std::runtime_error(error.message.empty() ? "probe failed" : error.message);
}

/// Seats `script` on the computer player and returns its sandbox.
host::Sandbox& seat(Fixture& fixture, const std::string& script, bool full_awareness = false) {
    const auto bytes =
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(script.data()), script.size()};
    brains::PackageFile file{"brain.lua", bytes};
    brains::SeatSpec spec;
    spec.player = computer;
    spec.id = "unit-counter";
    spec.name = "Unit Counter";
    spec.version = "1";
    spec.entry = "brain.lua";
    spec.level = "medium";
    spec.difficulty = OA_DIFFICULTY_MEDIUM;
    spec.full_awareness = full_awareness;
    spec.files = std::span<const brains::PackageFile>{&file, 1};
    const brains::SeatOutcome outcome = brains::seat_brain(*fixture.match, spec);
    if (outcome.result != brains::SeatResult::seated)
        throw std::runtime_error(
            std::string("seat failed: ") + outcome.reason.data() + " " + outcome.error.message
        );
    host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, computer);
    CHECK(sandbox != nullptr);
    return *sandbox;
}

brains::Seat& seat_of(Fixture& fixture) {
    auto* seated = static_cast<brains::Brains*>(fixture.match->seat_state().get());
    CHECK(seated != nullptr && seated->seats[computer] != nullptr);
    return *seated->seats[computer];
}

struct Pushed {
    brains::Seat* seat{};
    brains::UnitRef ref{};
    const char* name{};
};

void push_handle(void* context, lua_State* lua) {
    const auto* pushed = static_cast<const Pushed*>(context);
    brains::push_unit(lua, *pushed->seat, pushed->ref);
    lua_setglobal(lua, pushed->name);
}

/// Stores a handle of `slot` in the global `name`.
void name_unit(Fixture& fixture, host::Sandbox& sandbox, uint16_t slot, const char* name) {
    Pushed pushed{&seat_of(fixture), {slot, brains::unit_generation(*fixture.match, slot)}, name};
    host::ScriptError error;
    if (!sandbox.run_host("name-unit", &pushed, push_handle, error))
        throw std::runtime_error(error.message);
}

void advance(Fixture& fixture) {
    ++fixture.match->state().game.tick;
    fixture.match->tick();
}

/// Gives each type the Category words the probes count. The relay's line
/// names ARM twice.
void tag_types(Fixture& fixture) {
    fixture.defs[vehicle].categories = {"ARM", "LEVEL1", "CTRL_B", "TANK"};
    fixture.defs[collector].categories = {"ARM", "LEVEL1", "ENERGY", "NOWEAPON"};
    fixture.defs[relay].categories = {"ARM", "LEVEL1", "ARM"};
    fixture.defs[silo].categories = {"ARM", "LEVEL2", "CTRL_F", "WEAPON"};
    fixture.defs[missile].categories = {"ARM", "VTOL"};
}

/// Occupied slots of one player, walked here rather than read from the match's count.
uint32_t walked(Fixture& fixture, uint8_t owner) {
    uint32_t found = 0;
    const oa::World& world = fixture.match->state();
    for (uint32_t slot = 1; slot < world.unit_slot_count; ++slot)
        found += world.units[slot].type_index != 0 && world.units[slot].owner_index == owner;
    return found;
}

uint16_t live_count(Fixture& fixture, uint8_t owner) {
    return fixture.match->state().game.players[owner].unit_count;
}

void own_units_by_name_and_tag() {
    Fixture fixture;
    tag_types(fixture);
    const uint16_t first = fixture.spawn(computer, vehicle, 10, 10).unit_index;
    fixture.spawn(computer, vehicle, 14, 10);
    fixture.spawn(computer, collector, 20, 20);
    fixture.spawn(computer, silo, 30, 30);
    fixture.spawn(local_player, vehicle, 50, 50);
    fixture.spawn(local_player, relay, 54, 50);
    host::Sandbox& sandbox = seat(fixture, quiet_brain);
    name_unit(fixture, sandbox, first, "first");
    CHECK(live_count(fixture, computer) == 4);
    CHECK(walked(fixture, computer) == 4);

    probe(sandbox, R"(
expect(me:unit_count(), 4, "own units")
expect(me:unit_count(), #me:units(), "own units listed")
-- A name, in any case, a record and a type index count one type.
expect(me:unit_count_of("ARMCV"), 2, "name")
expect(me:unit_count_of("armcv"), 2, "name in lower case")
expect(me:unit_count_of(units.ARMCV), 2, "record")
expect(me:unit_count_of(units.ARMCV:index()), 2, "type index")
expect(me:unit_count_of(units.ARMSILO), 1, "silo")
-- A type the seat has none of, though another player does.
expect(me:unit_count_of("ARMRELAY"), 0, "relay")
expect(me:unit_count_of("ARMMAKENUKE"), 0, "missile")
-- A tag sums the types whose Category lines carry it, in any case.
expect(me:unit_count_of("CTRL_B"), 2, "builders tag")
expect(me:unit_count_of("ctrl_b"), 2, "builders tag in lower case")
expect(me:unit_count_of("TANK"), 2, "tank")
expect(me:unit_count_of("ENERGY"), 1, "energy")
expect(me:unit_count_of("level1"), 3, "level 1")
expect(me:unit_count_of("LEVEL2"), 1, "level 2")
expect(me:unit_count_of("CTRL_F"), 1, "factories tag")
expect(me:unit_count_of("ARM"), 4, "side tag")
expect(me:unit_count_of("ALL"), 4, "every type")
expect(me:unit_count_of("all"), 4, "every type in lower case")
-- A tag only types the seat has none of carry.
expect(me:unit_count_of("VTOL"), 0, "aircraft tag")
-- Words no loaded unit carries, and indexes outside the type table.
expect(me:unit_count_of("AIR"), nil, "unknown tag")
expect(me:unit_count_of("CTRL_Q"), nil, "unknown letter tag")
expect(me:unit_count_of(""), nil, "empty word")
expect(me:unit_count_of(string.rep("A", 300)), nil, "word past the registry's length")
expect(me:unit_count_of(0), nil, "reserved type")
expect(me:unit_count_of(#units + 1), nil, "type past the table")
expect(me:unit_count_of(-1), nil, "negative index")
expect(me:unit_count_of(65536), nil, "index past 16 bits")
-- Any other value is refused.
refused(nil, "nil")
refused({}, "table")
refused(true, "boolean")
refused(2.5, "fraction")
refused(first, "unit handle")
)");
}

void counts_follow_changes_within_a_tick() {
    Fixture fixture;
    tag_types(fixture);
    fixture.spawn(computer, vehicle, 10, 10);
    const uint16_t solar = fixture.spawn(computer, collector, 20, 20).unit_index;
    host::Sandbox& sandbox = seat(fixture, quiet_brain);
    probe(sandbox, R"(
expect(me:unit_count_of("ARMCV"), 1, "before")
expect(me:unit_count_of("LEVEL1"), 2, "level 1 before")
)");
    // Same tick: a unit made and one freed since the last count show at once.
    fixture.spawn(computer, vehicle, 14, 10);
    probe(sandbox, R"(
expect(me:unit_count_of("ARMCV"), 2, "made")
expect(me:unit_count_of("TANK"), 2, "made, by tag")
expect(me:unit_count(), 3, "made, all")
)");
    fixture.match->kill_unit(solar, static_cast<uint8_t>(sim::match_runtime::DeathKind::dismissed));
    fixture.spawn(computer, relay, 30, 30);
    probe(sandbox, R"(
expect(me:unit_count_of("ARMSOLAR"), 0, "freed")
expect(me:unit_count_of("ARMRELAY"), 1, "made in its place")
expect(me:unit_count_of("LEVEL1"), 3, "level 1 after")
expect(me:unit_count(), 3, "all after")
)");
    CHECK(live_count(fixture, computer) == walked(fixture, computer));
}

void full_awareness_still_counts_own_units() {
    Fixture fixture;
    tag_types(fixture);
    fixture.spawn(computer, vehicle, 10, 10);
    fixture.spawn(local_player, vehicle, 50, 50);
    fixture.spawn(local_player, relay, 54, 50);
    host::Sandbox& sandbox = seat(fixture, quiet_brain, true);
    probe(sandbox, R"(
expect(me.full_awareness, true, "awareness")
expect(#game.units:list(0), 2, "the other player's units are known")
expect(me:unit_count(), 1, "own units")
expect(me:unit_count_of("ARMCV"), 1, "own vehicles")
expect(me:unit_count_of("ARMRELAY"), 0, "the other player's relay")
expect(me:unit_count_of("ARM"), 1, "own side tag")
)");
}

void dead_and_captured_units_stop_counting() {
    Fixture fixture;
    tag_types(fixture);
    const uint16_t killed = fixture.spawn(computer, vehicle, 10, 10).unit_index;
    auto& taken = fixture.spawn(computer, vehicle, 14, 10);
    fixture.spawn(computer, collector, 20, 20);
    auto& captor = fixture.spawn(local_player, vehicle, 16, 10);
    host::Sandbox& sandbox = seat(fixture, recording_brain);
    advance(fixture);
    probe(sandbox, R"(
expect(seen[#seen], "1 3 2 3 40", "first turn")
)");

    // A unit torn down at once stops counting at once.
    fixture.match->kill_unit(
        killed, static_cast<uint8_t>(sim::match_runtime::DeathKind::dismissed)
    );
    probe(sandbox, R"(
expect(me:unit_count_of("ARMCV"), 1, "after the kill")
expect(me:unit_count(), 2, "all after the kill")
)");

    // A captured unit is copied to the captor and dies as captured. It holds
    // its slot until its death is carried out in the next unit sweep, which
    // runs before that tick's turn.
    const uint16_t taken_slot = taken.unit_index;
    name_unit(fixture, sandbox, taken_slot, "taken");
    fixture.match->capture_unit(taken, captor);
    CHECK(fixture.count(local_player, vehicle) == 2);
    probe(sandbox, R"(
expect(taken.dying, true, "the captured unit is dying")
expect(me:unit_count_of("ARMCV"), 1, "until its slot is freed")
)");
    advance(fixture);
    probe(sandbox, R"(
expect(seen[#seen], "2 1 0 1 40", "the next turn")
expect(taken.alive, false, "the captured unit")
expect(me:unit_count_of("ARMCV"), 0, "captured")
expect(me:unit_count_of("TANK"), 0, "captured, by tag")
expect(me:unit_count(), 1, "all after the capture")
)");
    CHECK(live_count(fixture, computer) == 1);
    CHECK(walked(fixture, computer) == 1);
    CHECK(fixture.count(local_player, vehicle) == 2);
}

/// Places own vehicles until the match refuses one, and returns how many it took.
uint32_t fill_own_slots(Fixture& fixture, uint32_t most) {
    uint32_t placed = 0;
    for (; placed <= most; ++placed) {
        const auto at = [](uint32_t cell) { return (cell * 16 + 8) << 16; };
        const uint32_t cell_x = 2 + (placed % 30) * 2;
        const uint32_t cell_z = 2 + (placed / 30) * 2;
        auto* slot =
            fixture.match->create({computer, vehicle, {at(cell_x), 0, at(cell_z)}, true, 1, 0});
        if (slot == nullptr || slot->unit == nullptr)
            break;
        slot->unit->object_present = true;
    }
    return placed;
}

oa::data::match_rules::MatchRules revision_one() {
    oa::data::match_rules::MatchRules rules;
    enhanced::store_choice(
        rules.setup.enhanced_mode, enhanced::Choice{1, enhanced::revision_features(1)}
    );
    return rules;
}

void limit_with_enhanced_mode_off() {
    {
        // The limit the preferences or a mod profile chose, 40 here, is the
        // range of slots the match gives each player. One more is refused.
        Fixture fixture;
        CHECK(enhanced::features(fixture.match->rules()).empty());
        CHECK(fixture.match->state().game.units_per_player == 40);
        host::Sandbox& sandbox = seat(fixture, quiet_brain);
        probe(sandbox, R"(expect(game.unit_limit, 40, "limit"))");
        CHECK(fill_own_slots(fixture, 40) == 40);
        probe(sandbox, R"(
expect(me:unit_count(), game.unit_limit, "a full range")
expect(me:unit_count_of("ARMCV"), 40, "a full range of vehicles")
)");
    }
    {
        // 3.1c's default of 250.
        Fixture fixture(0, true, {}, 0, 1, {}, 2, false, 250);
        host::Sandbox& sandbox = seat(fixture, quiet_brain);
        probe(sandbox, R"(expect(game.unit_limit, 250, "3.1c's default"))");
    }
}

void limit_under_revision_one() {
    {
        Fixture fixture(
            0, true, revision_one(), 0, 1, {}, 2, false, revision_one_limit, both_players
        );
        CHECK(enhanced::features(fixture.match->rules()).has(enhanced::Feature::capacity));
        CHECK(fixture.match->state().game.units_per_player == revision_one_limit);
        host::Sandbox& sandbox = seat(fixture, quiet_brain);
        fixture.spawn(computer, vehicle, 10, 10);
        probe(sandbox, R"(
expect(game.unit_limit, 10000, "Revision 1's ceiling")
expect(game.unit_limit - me:unit_count(), 9999, "room")
)");
    }
    {
        // A smaller limit under Revision 1 is enforced the same way.
        Fixture fixture(0, true, revision_one(), 0, 1, {}, 2, false, 60, both_players);
        host::Sandbox& sandbox = seat(fixture, quiet_brain);
        CHECK(fill_own_slots(fixture, 60) == 60);
        probe(sandbox, R"(
expect(game.unit_limit, 60, "limit")
expect(me:unit_count(), 60, "a full range")
)");
    }
}

void outside_a_match() {
    brains::Seat seat;
    seat.player = computer;
    const host::Bank bank{
        host::default_tick_allowance, host::default_bank_ticks, host::default_call_time_limit
    };
    host::OpenOptions options;
    options.package_id = "unit-counter";
    options.profile = brains::brain_profile(seat, bank);
    options.sources = seat.sources.hooks();
    options.random_seed = 0;
    host::ScriptError error;
    auto opened = host::Sandbox::open(options, error);
    if (!opened.has_value())
        throw std::runtime_error(error.message.empty() ? "open failed" : error.message);
    host::Sandbox sandbox(std::move(*opened));
    probe(sandbox, R"(
expect(game.in_match, false, "in a match")
expect(game.unit_limit, nil, "limit")
expect(me:unit_count(), 0, "own units")
expect(me:unit_count_of("ARMCV"), nil, "name")
expect(me:unit_count_of("ALL"), nil, "every type")
expect(me:unit_count_of(1), nil, "index")
refused({}, "table")
)");
}

struct Lines {
    std::vector<std::string> lines;
};

void read_seen(void* context, lua_State* lua) {
    auto* held = static_cast<Lines*>(context);
    lua_getglobal(lua, "seen");
    if (!lua_istable(lua, -1)) {
        lua_pop(lua, 1);
        return;
    }
    const auto count = static_cast<lua_Integer>(lua_rawlen(lua, -1));
    for (lua_Integer row = 1; row <= count; ++row) {
        lua_rawgeti(lua, -1, row);
        std::size_t length = 0;
        const char* text = lua_tolstring(lua, -1, &length);
        if (text != nullptr)
            held->lines.emplace_back(text, length);
        lua_pop(lua, 1);
    }
    lua_pop(lua, 1);
}

/// What the recording brain should read after a tick, walked here.
std::string expected_line(Fixture& fixture) {
    uint32_t level1 = 0;
    const oa::World& world = fixture.match->state();
    for (uint32_t slot = 1; slot < world.unit_slot_count; ++slot) {
        const oa::Unit& unit = world.units[slot];
        if (unit.owner_index != computer)
            continue;
        level1 +=
            unit.type_index == vehicle || unit.type_index == collector || unit.type_index == relay;
    }
    return std::to_string(world.game.tick) + " " + std::to_string(walked(fixture, computer)) + " " +
           std::to_string(fixture.count(computer, vehicle)) + " " + std::to_string(level1) + " " +
           std::to_string(world.game.units_per_player);
}

/// Makes, kills and captures the same units in a match at fixed ticks.
void change_units(Fixture& fixture, uint32_t tick, uint16_t first_vehicle, uint16_t captor) {
    if (tick == 10)
        fixture.spawn(computer, vehicle, 30, 10);
    if (tick == 20)
        fixture.spawn(computer, relay, 30, 30);
    if (tick == 30)
        fixture.match->kill_unit(
            first_vehicle, static_cast<uint8_t>(sim::match_runtime::DeathKind::dismissed)
        );
    if (tick == 40) {
        uint16_t taken = 0;
        for (const auto& slot : fixture.match->world().slots)
            if (slot.unit != nullptr && slot.record.owner_index == computer &&
                slot.record.type_index == vehicle) {
                taken = slot.unit_index;
                break;
            }
        CHECK(taken != 0);
        fixture.match->capture_unit(
            fixture.match->spawn_slot(taken), fixture.match->spawn_slot(captor)
        );
    }
}

void counting_leaves_the_match_as_it_was() {
    constexpr uint32_t ticks = 60;
    Fixture counting;
    Fixture quiet;
    tag_types(counting);
    tag_types(quiet);
    uint16_t first[2]{};
    uint16_t captor[2]{};
    Fixture* both[2] = {&counting, &quiet};
    for (int side = 0; side < 2; ++side) {
        first[side] = both[side]->spawn(computer, vehicle, 10, 10).unit_index;
        both[side]->spawn(computer, vehicle, 14, 10);
        both[side]->spawn(computer, collector, 20, 20);
        captor[side] = both[side]->spawn(local_player, vehicle, 50, 50).unit_index;
    }
    host::Sandbox& recorder = seat(counting, recording_brain);
    (void)seat(quiet, quiet_brain);
    std::vector<std::string> expected(ticks + 1);
    for (uint32_t step = 1; step <= ticks; ++step) {
        for (int side = 0; side < 2; ++side)
            change_units(*both[side], step, first[side], captor[side]);
        advance(counting);
        advance(quiet);
        if (counting.match->random_state() != quiet.match->random_state())
            throw std::runtime_error("random diverged at tick " + std::to_string(step));
        expected[step] = expected_line(counting);
    }
    const oa::base::game_loop::Timing timing{};
    CHECK(
        oa::sim::trace::match_state_hash(*counting.match, timing, 0, 0, nullptr) ==
        oa::sim::trace::match_state_hash(*quiet.match, timing, 0, 0, nullptr)
    );
    Lines seen;
    host::ScriptError error;
    CHECK(recorder.run_host("read", &seen, read_seen, error));
    CHECK(seen.lines.size() == ticks);
    for (uint32_t step = 1; step <= ticks; ++step) {
        if (seen.lines[step - 1] != expected[step])
            throw std::runtime_error(
                "tick " + std::to_string(step) + ": read " + seen.lines[step - 1] + ", walked " +
                expected[step]
            );
    }
    // The run made, killed and captured units, so the counts moved.
    CHECK(seen.lines.front() != seen.lines.back());
}

} // namespace

int main() {
    try {
        own_units_by_name_and_tag();
        counts_follow_changes_within_a_tick();
        full_awareness_still_counts_own_units();
        dead_and_captured_units_stop_counting();
        limit_with_enhanced_mode_off();
        limit_under_revision_one();
        outside_a_match();
        counting_leaves_the_match_as_it_was();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "sim-brains-counts: %s\n", ex.what());
        return 1;
    }
    return 0;
}

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The seat's economy as a brain reads it: produced, requested and spent, per
// second, with a full store, an empty store and an idle builder. The made-up
// brain is economy-reader. Its builder makes 25 energy and 1 metal a second,
// uses 1 energy a second while active, and works at 300, as a commander
// does; the energy building it puts up costs 790 energy and 141 metal over a
// build time of 2416, as a Core solar collector does.

#include "match_fixture.hpp"

#include "oa/core/player.h"
#include "oa/core/unit.h"
#include "oa/core/world.h"
#include "oa/sim/ai.hpp"
#include "oa/sim/brains/seat.hpp"

#include "lua.hpp"

#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace brains = oa::sim::brains;
namespace host = oa::data::script_host;

constexpr float builder_energy_income = 25.0F;
constexpr float builder_metal_income = 1.0F;
constexpr float builder_upkeep = 1.0F;
constexpr int32_t building_energy_cost = 790;
constexpr int32_t building_metal_cost = 141;
constexpr int32_t building_time = 2416;
constexpr uint32_t ticks_run = 300;
// The builder works 300 / 30 = 10 a tick, so the building takes 2416 / 10 =
// 242 ticks at full speed.
constexpr double energy_drain_per_second = 790.0 * 300.0 / 2416.0;
constexpr double metal_drain_per_second = 141.0 * 300.0 / 2416.0;

/// The figures of one turn, as the brain read them or as the match holds
/// them after a tick.
struct Figures {
    int64_t tick{};
    uint32_t energy{};
    uint32_t metal{};
    uint32_t energy_produced{};
    uint32_t energy_requested{};
    uint32_t energy_spent{};
    uint32_t metal_produced{};
    uint32_t metal_requested{};
    uint32_t metal_spent{};
};

/// One economy settlement the match ran, from the match's own records.
struct Settlement {
    uint32_t tick{};
    float store_energy_before{};
    float store_metal_before{};
    float energy_wasted{};
    float metal_wasted{};
    oa::Player player{};
    sim::match_runtime::SettledSpending spending{};
};

uint32_t bits_of(float value) {
    uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

std::string load_script() {
    const std::string path = std::string(OA_ECONOMY_READER_DIR) + "/brain.lua";
    std::ifstream input(path);
    if (!input)
        throw std::runtime_error("missing economy-reader");
    std::string text;
    std::string line;
    while (std::getline(input, line)) {
        text.append(line);
        text.push_back('\n');
    }
    CHECK(!text.empty());
    return text;
}

/// Makes the fixture's vehicle a commander-like builder and its collector a
/// solar-collector-like building.
void shape_types(Fixture& fixture) {
    auto& world = fixture.match->state();
    auto& builder = world.unit_defs[vehicle];
    builder.energy_make = builder_energy_income;
    builder.metal_make = builder_metal_income;
    builder.energy_use = builder_upkeep;
    fixture.defs[collector].build_cost_energy = building_energy_cost;
    fixture.defs[collector].build_cost_metal = building_metal_cost;
    fixture.defs[collector].build_time = building_time;
    auto& building = world.unit_defs[collector];
    building.build_cost_energy = static_cast<float>(building_energy_cost);
    building.build_cost_metal = static_cast<float>(building_metal_cost);
    building.build_time = building_time;
}

void seat_reader(Fixture& fixture, const std::string& script, bool build) {
    const auto bytes =
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(script.data()), script.size()};
    brains::PackageFile file{"brain.lua", bytes};
    brains::OptionValue option;
    option.id = "build";
    option.kind = brains::OptionKind::toggle;
    option.toggle = build;
    brains::SeatSpec spec;
    spec.player = computer;
    spec.id = "economy-reader";
    spec.name = "Economy Reader";
    spec.version = "1.0";
    spec.entry = "brain.lua";
    spec.level = "hard";
    spec.difficulty = OA_DIFFICULTY_HARD;
    // The fixture's map is never mapped, and only a brain with full awareness
    // finds a building site on unmapped ground.
    spec.full_awareness = true;
    spec.options = std::span<const brains::OptionValue>{&option, 1};
    spec.files = std::span<const brains::PackageFile>{&file, 1};
    const brains::SeatOutcome outcome = brains::seat_brain(*fixture.match, spec);
    if (outcome.result != brains::SeatResult::seated) {
        throw std::runtime_error(
            std::string("seat failed: ") + outcome.reason.data() + " " + outcome.error.message
        );
    }
}

int64_t field_of(lua_State* lua, int table, const char* name) {
    lua_getfield(lua, table, name);
    const int64_t value = lua_isinteger(lua, -1) != 0 ? lua_tointeger(lua, -1) : -1;
    lua_pop(lua, 1);
    return value;
}

struct Read {
    std::vector<Figures>* rows{};
    std::string ordered;
};

void read_rows(void* context, lua_State* lua) {
    auto* held = static_cast<Read*>(context);
    lua_getglobal(lua, "ordered");
    if (lua_type(lua, -1) == LUA_TSTRING)
        held->ordered = lua_tostring(lua, -1);
    lua_pop(lua, 1);
    lua_getglobal(lua, "rows");
    if (lua_istable(lua, -1) == 0) {
        lua_pop(lua, 1);
        return;
    }
    const int table = lua_gettop(lua);
    const auto count = static_cast<lua_Integer>(lua_rawlen(lua, table));
    for (lua_Integer row = 1; row <= count; ++row) {
        lua_rawgeti(lua, table, row);
        const int at = lua_gettop(lua);
        Figures figures;
        figures.tick = field_of(lua, at, "tick");
        figures.energy = static_cast<uint32_t>(field_of(lua, at, "energy"));
        figures.metal = static_cast<uint32_t>(field_of(lua, at, "metal"));
        figures.energy_produced = static_cast<uint32_t>(field_of(lua, at, "energy_produced"));
        figures.energy_requested = static_cast<uint32_t>(field_of(lua, at, "energy_requested"));
        figures.energy_spent = static_cast<uint32_t>(field_of(lua, at, "energy_spent"));
        figures.metal_produced = static_cast<uint32_t>(field_of(lua, at, "metal_produced"));
        figures.metal_requested = static_cast<uint32_t>(field_of(lua, at, "metal_requested"));
        figures.metal_spent = static_cast<uint32_t>(field_of(lua, at, "metal_spent"));
        held->rows->push_back(figures);
        lua_pop(lua, 1);
    }
    lua_pop(lua, 1);
}

Figures held_figures(Fixture& fixture) {
    const auto& player = fixture.match->state().game.players[computer];
    const auto spending = fixture.match->settled_spending(computer);
    Figures figures;
    figures.tick = fixture.match->state().game.tick;
    figures.energy = bits_of(player.energy);
    figures.metal = bits_of(player.metal);
    figures.energy_produced = bits_of(player.energy_produced);
    figures.energy_requested = bits_of(player.energy_requested);
    figures.energy_spent = bits_of(spending.energy);
    figures.metal_produced = bits_of(player.metal_produced);
    figures.metal_requested = bits_of(player.metal_requested);
    figures.metal_spent = bits_of(spending.metal);
    return figures;
}

bool same_figures(const Figures& read, const Figures& held) {
    return read.energy == held.energy && read.metal == held.metal &&
           read.energy_produced == held.energy_produced &&
           read.energy_requested == held.energy_requested &&
           read.energy_spent == held.energy_spent && read.metal_produced == held.metal_produced &&
           read.metal_requested == held.metal_requested && read.metal_spent == held.metal_spent;
}

/// A match with the builder and the seated reader, run for ticks_run ticks.
struct Run {
    std::vector<Settlement> settlements;
    std::vector<Figures> rows;
    std::string ordered;
};

Run run_reader(
    const std::string& script,
    float energy_store,
    float metal_store,
    bool build,
    float cloak_cost = 0.0F
) {
    // Dry land and a movement class, so the builder can reach its site.
    Fixture fixture(0, false, {}, 1);
    shape_types(fixture);
    CHECK(sim::ai::configure_match_computer_players(*fixture.match, profile, build_lists, false));
    auto& world = fixture.match->state();
    auto& player = world.game.players[computer];
    player.energy = energy_store;
    player.metal = metal_store;
    auto& builder = fixture.spawn(computer, vehicle, 32, 32);
    // Active, as a commander is once built, so it pays its upkeep.
    builder.record.state_flags =
        static_cast<uint8_t>(builder.record.state_flags | OA_UNIT_STATE_ACTIVE);
    if (cloak_cost > 0.0F) {
        world.unit_defs[vehicle].cloak_cost = cloak_cost;
        builder.record.flags |= OA_UNIT_FLAG_CLOAK_RUNNING;
    }
    seat_reader(fixture, script, build);
    CHECK(fixture.match->settled_spending(computer).energy == 0.0F);

    Run run;
    // What the match holds after each tick, which is what the brain reads at
    // its turn in the next tick: the seat's turn comes before the settlement.
    std::vector<Figures> after_tick;
    for (uint32_t step = 0; step < ticks_run; ++step) {
        const uint32_t due = player.next_economy_tick;
        const float energy_before = player.energy;
        const float metal_before = player.metal;
        const double energy_wasted = player.energy_wasted_total;
        const double metal_wasted = player.metal_wasted_total;
        ++world.game.tick;
        fixture.match->tick();
        after_tick.push_back(held_figures(fixture));
        if (player.next_economy_tick != due) {
            Settlement settled;
            settled.tick = world.game.tick;
            settled.store_energy_before = energy_before;
            settled.store_metal_before = metal_before;
            settled.energy_wasted = static_cast<float>(player.energy_wasted_total - energy_wasted);
            settled.metal_wasted = static_cast<float>(player.metal_wasted_total - metal_wasted);
            settled.player = player;
            settled.spending = fixture.match->settled_spending(computer);
            CHECK(settled.spending.stores_left_known);
            CHECK(bits_of(settled.spending.energy_left) == bits_of(player.energy));
            CHECK(bits_of(settled.spending.metal_left) == bits_of(player.metal));
            run.settlements.push_back(settled);
        }
    }

    host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, computer);
    CHECK(sandbox != nullptr);
    Read reading{&run.rows, {}};
    host::ScriptError error;
    CHECK(sandbox->run_host("read", &reading, read_rows, error));
    run.ordered = reading.ordered;

    // Every turn from the second tick on read what the match held at the end
    // of the tick before. The first turn read the match as it was set up.
    std::size_t compared = 0;
    for (const Figures& row : run.rows) {
        if (row.tick < 2)
            continue;
        const auto index = static_cast<std::size_t>(row.tick - 2);
        CHECK(index < after_tick.size());
        if (!same_figures(row, after_tick[index]))
            throw std::runtime_error("turn " + std::to_string(row.tick) + " read other figures");
        ++compared;
    }
    CHECK(compared + 2 >= ticks_run);
    CHECK(run.settlements.size() >= ticks_run / 30 - 1);
    return run;
}

bool close_to(double value, double expected, double room) {
    return std::fabs(value - expected) <= room;
}

/// What the store paid out over a settlement's second: the store before,
/// what came in, less the store after and what overflowed it.
void check_paid_out(const Settlement& settled) {
    const double energy_paid = static_cast<double>(settled.store_energy_before) +
                               static_cast<double>(settled.player.energy_produced) -
                               static_cast<double>(settled.player.energy) -
                               static_cast<double>(settled.energy_wasted);
    const double metal_paid = static_cast<double>(settled.store_metal_before) +
                              static_cast<double>(settled.player.metal_produced) -
                              static_cast<double>(settled.player.metal) -
                              static_cast<double>(settled.metal_wasted);
    if (!close_to(static_cast<double>(settled.spending.energy), energy_paid, 0.01) ||
        !close_to(static_cast<double>(settled.spending.metal), metal_paid, 0.01))
        throw std::runtime_error(
            "tick " + std::to_string(settled.tick) + ": spent " +
            std::to_string(settled.spending.energy) + "/" + std::to_string(settled.spending.metal) +
            ", the stores paid " + std::to_string(energy_paid) + "/" + std::to_string(metal_paid)
        );
}

// Building from a full store: everything requested is paid, so spent is the
// requested figure to the bit, and the commander and solar figures hold:
// 25 produced, 790 * 300 / 2416 + 1 requested, about -74 a second.
void full_store_spends_what_was_requested(const std::string& script) {
    const Run run = run_reader(script, starting_resources, starting_resources, true);
    CHECK(run.ordered == "ARMSOLAR");
    std::size_t building = 0;
    for (const Settlement& settled : run.settlements) {
        check_paid_out(settled);
        CHECK(bits_of(settled.spending.energy) == bits_of(settled.player.energy_requested));
        CHECK(bits_of(settled.spending.metal) == bits_of(settled.player.metal_requested));
        CHECK(settled.player.energy > 0.0F && settled.player.metal > 0.0F);
        if (close_to(
                static_cast<double>(settled.player.energy_requested),
                energy_drain_per_second + static_cast<double>(builder_upkeep),
                0.01
            )) {
            // The building is not finished, so only the builder makes anything.
            CHECK(settled.player.energy_produced == builder_energy_income);
            CHECK(settled.player.metal_produced == builder_metal_income);
            CHECK(close_to(
                static_cast<double>(settled.player.metal_requested), metal_drain_per_second, 0.01
            ));
            CHECK(close_to(
                oa::player_energy_surplus(&settled.player),
                static_cast<double>(builder_energy_income) - energy_drain_per_second -
                    static_cast<double>(builder_upkeep),
                0.01
            ));
            ++building;
        }
    }
    // A whole second of building, at least five times over the 242 ticks.
    CHECK(building >= 5);
}

// Building from an empty energy store: the pool is only the second's
// income, so spent falls short of requested. A builder that owes energy
// builds nothing until a later settlement pays it, so the work goes at
// about a quarter of its speed.
void empty_store_spends_what_came_in(const std::string& script) {
    const Run run = run_reader(script, 0.0F, starting_resources, true);
    CHECK(run.ordered == "ARMSOLAR");
    std::size_t short_seconds = 0;
    for (const Settlement& settled : run.settlements) {
        check_paid_out(settled);
        CHECK(settled.spending.energy <= settled.player.energy_requested);
        CHECK(settled.spending.metal <= settled.player.metal_requested);
        CHECK(
            settled.spending.energy <= settled.store_energy_before + settled.player.energy_produced
        );
        if (close_to(
                static_cast<double>(settled.player.energy_requested),
                energy_drain_per_second + static_cast<double>(builder_upkeep),
                0.01
            )) {
            CHECK(settled.spending.energy < settled.player.energy_requested);
            CHECK(settled.spending.energy > 0.0F);
            ++short_seconds;
        }
    }
    CHECK(short_seconds >= 5);
}

// Both stores empty: once the energy owed is paid, the builder still owes
// metal, so it builds nothing and its energy goes unspent. Spent shows it:
// the upkeep alone, while requested stays at the full rate and the energy
// store fills.
void empty_stores_wait_on_metal(const std::string& script) {
    const Run run = run_reader(script, 0.0F, 0.0F, true);
    CHECK(run.ordered == "ARMSOLAR");
    std::size_t waiting = 0;
    for (const Settlement& settled : run.settlements) {
        check_paid_out(settled);
        CHECK(settled.spending.metal <= settled.player.metal_requested);
        CHECK(settled.spending.metal <= settled.store_metal_before + settled.player.metal_produced);
        if (settled.spending.energy == builder_upkeep &&
            settled.player.energy_requested > builder_upkeep + 1.0F &&
            settled.player.energy > settled.store_energy_before) {
            CHECK(settled.spending.metal < settled.player.metal_requested);
            ++waiting;
        }
    }
    CHECK(waiting >= 5);
}

// An idle builder only pays its upkeep, and pays it whole.
void idle_spends_the_upkeep(const std::string& script) {
    const Run run = run_reader(script, starting_resources, starting_resources, false);
    CHECK(run.ordered.empty());
    for (const Settlement& settled : run.settlements) {
        check_paid_out(settled);
        CHECK(settled.player.energy_requested == builder_upkeep);
        CHECK(settled.spending.energy == builder_upkeep);
        CHECK(settled.player.metal_requested == 0.0F);
        CHECK(settled.spending.metal == 0.0F);
    }
}

// A cloak's upkeep is paid whole from the store as the economy settles, so
// it is spent at once, beside the upkeep the settlement pays.
void cloak_is_spent_at_once(const std::string& script) {
    constexpr float cloak_cost = 50.0F;
    const Run run = run_reader(script, starting_resources, starting_resources, false, cloak_cost);
    CHECK(run.ordered.empty());
    for (const Settlement& settled : run.settlements) {
        check_paid_out(settled);
        CHECK(settled.player.energy_requested == builder_upkeep + cloak_cost);
        CHECK(settled.spending.energy == builder_upkeep + cloak_cost);
        CHECK(settled.spending.metal == 0.0F);
    }
}

// The load's pass settles the economy and then puts the starting stores
// back. Its figures stay, as Player.energy_produced and energy_requested do,
// but the next settlement does not measure from the stores the pass left.
void load_pass_keeps_its_figures() {
    // Dry land and a movement class, as the reader's runs have.
    Fixture fixture(0, false, {}, 1);
    shape_types(fixture);
    auto& world = fixture.match->state();
    auto& player = world.game.players[computer];
    player.energy = player.metal = starting_resources;
    auto& builder = fixture.spawn(computer, vehicle, 32, 32);
    builder.record.state_flags =
        static_cast<uint8_t>(builder.record.state_flags | OA_UNIT_STATE_ACTIVE);
    fixture.match->finish_load();
    const uint32_t due = player.next_economy_tick;
    ++world.game.tick;
    fixture.match->tick();
    CHECK(player.next_economy_tick != due);
    CHECK(player.energy == starting_resources);
    CHECK(player.energy_requested == builder_upkeep);
    CHECK(!fixture.match->settled_spending(computer).stores_left_known);
    CHECK(fixture.match->settled_spending(computer).energy == builder_upkeep);
    CHECK(fixture.run(60, [&] {
        return fixture.match->settled_spending(computer).stores_left_known;
    }));
    CHECK(fixture.match->settled_spending(computer).energy == builder_upkeep);
    CHECK(fixture.match->settled_spending(computer).energy_left == player.energy);
}

} // namespace

int main() {
    try {
        const std::string script = load_script();
        full_store_spends_what_was_requested(script);
        empty_store_spends_what_came_in(script);
        empty_stores_wait_on_metal(script);
        idle_spends_the_upkeep(script);
        cloak_is_spent_at_once(script);
        load_pass_keeps_its_figures();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "sim-brains-economy: %s\n", ex.what());
        return 1;
    }
    return 0;
}

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Events delivered at a seated brain's turn. The made-up brain is
// steady-commander. Hooks append one line per call; the test reads that
// table back with run_host.

#include "match_fixture.hpp"

#include "oa/base/game_loop.hpp"
#include "oa/core/feature_def.h"
#include "oa/core/game_state.h"
#include "oa/core/player.h"
#include "oa/sim/brains/events.hpp"
#include "oa/sim/brains/handles.hpp"
#include "oa/sim/brains/seat.hpp"
#include "oa/sim/ground_orders/orders.hpp"
#include "oa/sim/state_hash.hpp"

#include "lua.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace brains = oa::sim::brains;
namespace host = oa::data::script_host;

struct Package {
    std::string id{"steady-commander"};
    std::string name{"Steady Commander"};
    std::string version{"1"};
    std::string entry{"brain.lua"};
    std::string level{"hard"};
    int32_t difficulty{OA_DIFFICULTY_HARD};
    uint8_t player{computer};
    bool full_awareness{};
    host::Bank bank{
        host::default_tick_allowance, host::default_bank_ticks, host::default_call_time_limit
    };
    std::vector<std::pair<std::string, std::string>> files;
};

void add_file(Package& package, std::string path, std::string text) {
    package.files.emplace_back(std::move(path), std::move(text));
}

brains::SeatOutcome seat_it(Fixture& fixture, Package& package) {
    std::vector<brains::PackageFile> files;
    files.reserve(package.files.size());
    for (const auto& held : package.files)
        files.push_back(
            {held.first,
             std::span<const uint8_t>{
                 reinterpret_cast<const uint8_t*>(held.second.data()), held.second.size()
             }}
        );

    brains::SeatSpec spec;
    spec.player = package.player;
    spec.id = package.id;
    spec.name = package.name;
    spec.version = package.version;
    spec.entry = package.entry;
    spec.level = package.level;
    spec.difficulty = package.difficulty;
    spec.full_awareness = package.full_awareness;
    spec.files = files;
    spec.bank = package.bank;
    return brains::seat_brain(*fixture.match, spec);
}

void advance(Fixture& fixture) {
    ++fixture.match->state().game.tick;
    fixture.match->tick();
}

void advance_n(Fixture& fixture, uint32_t ticks) {
    for (uint32_t passed = 0; passed < ticks; ++passed)
        advance(fixture);
}

uint32_t centre_bits(int32_t cell) {
    return static_cast<uint32_t>(cell * 16 + 8) << 16;
}

int32_t centre_fixed(int32_t cell) {
    return static_cast<int32_t>(centre_bits(cell));
}

sim::ground_orders::Point centre_point(int32_t cell_x, int32_t cell_z) {
    return {centre_fixed(cell_x), 0, centre_fixed(cell_z)};
}

std::string handle_text(uint16_t slot, uint32_t generation) {
    const int64_t id =
        static_cast<int64_t>(generation) * int64_t{65536} + static_cast<int64_t>(slot);
    return "u" + std::to_string(slot) + ":" + std::to_string(id);
}

std::string live_text(Fixture& fixture, uint16_t slot) {
    return handle_text(slot, brains::unit_generation(*fixture.match, slot));
}

void make_computer(Fixture& fixture, uint8_t player) {
    fixture.match->simulation().players[player].present = true;
    fixture.match->simulation().players[player].status = OA_PLAYER_STATUS_COMPUTER;
}

struct LogRead {
    std::vector<std::string>* lines{};
};

void read_log(void* context, lua_State* state) {
    auto* held = static_cast<LogRead*>(context);
    lua_getglobal(state, "log");
    if (lua_istable(state, -1) == 0) {
        lua_pop(state, 1);
        return;
    }
    const int count = static_cast<int>(lua_rawlen(state, -1));
    for (int row = 1; row <= count; ++row) {
        lua_rawgeti(state, -1, row);
        std::size_t length = 0;
        const char* text = lua_tolstring(state, -1, &length);
        if (text != nullptr)
            held->lines->emplace_back(text, length);
        lua_pop(state, 1);
    }
    lua_pop(state, 1);
}

std::vector<std::string> read_lines(host::Sandbox& sandbox) {
    std::vector<std::string> lines;
    LogRead held{&lines};
    host::ScriptError error;
    CHECK(sandbox.run_host("read", &held, read_log, error));
    return lines;
}

int count_prefix(const std::vector<std::string>& lines, std::string_view prefix) {
    int count = 0;
    for (const std::string& line : lines) {
        if (line.size() >= prefix.size() &&
            std::string_view(line).substr(0, prefix.size()) == prefix)
            ++count;
    }
    return count;
}

int line_at(const std::vector<std::string>& lines, const std::string& wanted) {
    for (int row = 0; row < static_cast<int>(lines.size()); ++row) {
        if (lines[static_cast<std::size_t>(row)] == wanted)
            return row;
    }
    return -1;
}

std::string joined(const std::vector<std::string>& lines) {
    std::string text;
    for (const std::string& line : lines)
        text += line + "\n";
    return text;
}

void require_line(const std::vector<std::string>& lines, const std::string& wanted) {
    if (line_at(lines, wanted) >= 0)
        return;
    throw std::runtime_error(wanted + " missing\n" + joined(lines));
}

void require_absent(const std::vector<std::string>& lines, std::string_view prefix) {
    if (count_prefix(lines, prefix) == 0)
        return;
    throw std::runtime_error(std::string(prefix) + " was delivered\n" + joined(lines));
}

const char* recorder_script() {
    return R"(local hooks = {}
log = {}
local function unit_text(unit)
  if unit == nil then
    return "nil"
  end
  return string.format("u%d:%d", unit.slot, unit.id)
end
local function note(line)
  log[#log + 1] = line
end
function hooks.fits()
  return true
end
function hooks.start()
  return true
end
function hooks.onUnitCreated(unit, builder)
  note("created " .. unit_text(unit) .. " " .. unit_text(builder))
end
function hooks.onUnitFinished(unit, builder)
  note("finished " .. unit_text(unit) .. " " .. unit_text(builder))
end
function hooks.onUnitDamaged(unit, attacker, amount, kind)
  note("damaged " .. unit_text(unit) .. " " .. unit_text(attacker) .. " " .. tostring(amount) .. " " .. kind)
end
function hooks.onUnitDestroyed(unit, killer, wreck)
  local wreck_text = "nil"
  if wreck ~= nil then
    wreck_text = wreck.name .. " " .. tostring(wreck.x:raw()) .. " " .. tostring(wreck.z:raw())
  end
  note("destroyed " .. unit_text(unit) .. " " .. unit_text(killer) .. " " .. wreck_text)
end
function hooks.onUnitGiven(unit, copy, from_player, to_player)
  note("given " .. unit_text(unit) .. " " .. unit_text(copy) .. " " .. tostring(from_player) .. " " .. tostring(to_player))
end
function hooks.onUnitIdle(unit)
  note("idle " .. unit_text(unit))
end
function hooks.onCommandDone(unit, order, result)
  note("command " .. unit_text(unit) .. " " .. order .. " " .. result)
end
function hooks.onUnitLoaded(unit, carrier)
  note("loaded " .. unit_text(unit) .. " " .. unit_text(carrier))
end
function hooks.onUnitUnloaded(unit, carrier)
  note("unloaded " .. unit_text(unit) .. " " .. unit_text(carrier))
end
function hooks.onUnitSeen(unit)
  note("seen " .. unit_text(unit))
end
function hooks.onUnitLost(unit)
  note("lost " .. unit_text(unit))
end
function hooks.onPathFailed(unit)
  note("path " .. unit_text(unit))
end
function hooks.onAllianceChanged(player, allied)
  note("alliance " .. tostring(player) .. " " .. (allied and "true" or "false"))
end
function hooks.onPlayerDefeated(player)
  note("defeated " .. tostring(player))
end
function hooks.onTick(tick)
  note("tick " .. tostring(tick))
end
return hooks
)";
}

Package package_with(const char* script, uint8_t player = computer) {
    Package package;
    package.player = player;
    add_file(package, "brain.lua", script);
    return package;
}

Package recording_package(uint8_t player = computer) {
    return package_with(recorder_script(), player);
}

host::Sandbox* seat_ready(Fixture& fixture, Package& package) {
    const brains::SeatOutcome outcome = seat_it(fixture, package);
    CHECK(outcome.result == brains::SeatResult::seated);
    CHECK(brains::brain_seated(*fixture.match, package.player));
    host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, package.player);
    CHECK(sandbox != nullptr);
    return sandbox;
}

void copy_name(char* dest, std::size_t room, const char* text) {
    std::size_t length = 0;
    if (text != nullptr && room > 0) {
        while (text[length] != '\0' && length + 1 < room) {
            dest[length] = text[length];
            ++length;
        }
    }
    if (room > 0)
        dest[length] = '\0';
}

void spawned_unit_reports_created_and_finished() {
    Fixture fixture;
    Package package = recording_package();
    host::Sandbox* sandbox = seat_ready(fixture, package);

    auto& solar = fixture.spawn(computer, collector, 8, 8);
    const std::string solar_text = live_text(fixture, solar.unit_index);
    auto& builder = fixture.spawn(computer, vehicle, 16, 16);
    const std::string builder_text = live_text(fixture, builder.unit_index);

    sim::unit_spawn::Request request;
    request.player = computer;
    request.type = collector;
    request.position = {centre_bits(24), 0, centre_bits(24)};
    request.finished = false;
    request.state = 1;
    auto* frame = fixture.match->create(request, builder.unit_index);
    CHECK(frame != nullptr && frame->unit != nullptr);
    const std::string frame_text = live_text(fixture, frame->unit_index);
    fixture.match->finish_unit(frame->unit_index, builder.unit_index);

    advance(fixture);
    const std::vector<std::string> lines = read_lines(*sandbox);
    const int created_solar = line_at(lines, "created " + solar_text + " nil");
    const int finished_solar = line_at(lines, "finished " + solar_text + " nil");
    const int created_builder = line_at(lines, "created " + builder_text + " nil");
    const int created_frame = line_at(lines, "created " + frame_text + " " + builder_text);
    const int finished_frame = line_at(lines, "finished " + frame_text + " " + builder_text);
    const int tick_line = line_at(lines, "tick 1");
    CHECK(created_solar >= 0 && finished_solar > created_solar);
    CHECK(created_builder > finished_solar);
    CHECK(created_frame > created_builder && finished_frame > created_frame);
    CHECK(tick_line > finished_frame);
    CHECK(fixture.match->state().game.tick == 1);
}

void damage_reports_amount_kind_and_attacker() {
    Fixture fixture;
    Package package = recording_package();
    host::Sandbox* sandbox = seat_ready(fixture, package);

    auto& own = fixture.spawn(computer, collector, 8, 8);
    auto& attacker = fixture.spawn(computer, vehicle, 14, 8);
    auto& enemy = fixture.spawn(0, vehicle, 10, 8);
    const std::string own_text = live_text(fixture, own.unit_index);
    const std::string attacker_text = live_text(fixture, attacker.unit_index);
    const std::string enemy_text = live_text(fixture, enemy.unit_index);

    fixture.match->apply_damage_event(own, &attacker, static_cast<int16_t>(3), 1, 0);
    fixture.match->apply_damage_event(own, &attacker, static_cast<int16_t>(4), 2, 0);
    fixture.match->apply_damage_event(own, nullptr, static_cast<int16_t>(5), 10, 0);
    fixture.match->apply_damage_event(own, &enemy, static_cast<int16_t>(6), 11, 0);
    advance(fixture);
    std::vector<std::string> lines = read_lines(*sandbox);
    require_line(lines, "damaged " + own_text + " " + attacker_text + " 3 weapon");
    require_line(lines, "damaged " + own_text + " " + attacker_text + " 4 paralyze");
    require_line(lines, "damaged " + own_text + " nil 5 heal");
    require_line(lines, "damaged " + own_text + " nil 6 environment");

    while (fixture.match->state().game.tick < 30)
        advance(fixture);
    fixture.match->apply_damage_event(own, &enemy, static_cast<int16_t>(2), 1, 0);
    advance(fixture);
    lines = read_lines(*sandbox);
    require_line(lines, "damaged " + own_text + " " + enemy_text + " 2 weapon");
}

void kill_reports_killer_and_wreck() {
    Fixture fixture;
    Package package = recording_package();
    host::Sandbox* sandbox = seat_ready(fixture, package);

    FeatureDef heap{};
    copy_name(heap.name, sizeof heap.name, "heap");
    heap.footprint_x = heap.footprint_z = 1;
    heap.dead_feature = 0xffff;
    heap.burnt_feature = 0xffff;
    heap.reclamate_feature = 0xffff;
    fixture.match->adopt_feature_defs(std::span<const FeatureDef>(&heap, 1));
    fixture.match->state().unit_defs[collector].corpse = 0;

    auto& attacker = fixture.spawn(computer, vehicle, 14, 14);
    auto& victim = fixture.spawn(computer, collector, 8, 8);
    const std::string attacker_text = live_text(fixture, attacker.unit_index);
    const uint16_t victim_slot = victim.unit_index;
    const uint32_t victim_generation = brains::unit_generation(*fixture.match, victim_slot);
    const int32_t wreck_x = victim.record.position.x;
    const int32_t wreck_z = victim.record.position.z;
    fixture.match->apply_damage_event(victim, &attacker, static_cast<int16_t>(1), 1, 0);
    fixture.match->kill_unit(
        victim_slot, static_cast<uint8_t>(sim::match_runtime::DeathKind::dismissed)
    );

    advance(fixture);
    const std::vector<std::string> lines = read_lines(*sandbox);
    require_line(
        lines,
        "destroyed " + handle_text(victim_slot, victim_generation) + " " + attacker_text +
            " heap " + std::to_string(wreck_x) + " " + std::to_string(wreck_z)
    );
}

void events_arrive_in_order_before_on_tick() {
    Fixture fixture;
    Package package = recording_package();
    host::Sandbox* sandbox = seat_ready(fixture, package);
    auto& solar = fixture.spawn(computer, collector, 8, 8);
    const std::string solar_text = live_text(fixture, solar.unit_index);
    advance(fixture);
    const std::vector<std::string> lines = read_lines(*sandbox);
    const int created_at = line_at(lines, "created " + solar_text + " nil");
    const int finished_at = line_at(lines, "finished " + solar_text + " nil");
    const std::string tick_text = "tick " + std::to_string(fixture.match->state().game.tick);
    const int tick_at = line_at(lines, tick_text);
    CHECK(created_at >= 0 && finished_at > created_at && tick_at > finished_at);
    CHECK(fixture.match->state().game.tick == 1);
}

void event_after_the_turn_waits() {
    Fixture fixture;
    Package package = recording_package();
    host::Sandbox* sandbox = seat_ready(fixture, package);
    auto& earlier = fixture.spawn(computer, collector, 8, 8);
    const std::string earlier_text = live_text(fixture, earlier.unit_index);
    advance(fixture);
    auto& later = fixture.spawn(computer, collector, 20, 20);
    const std::string later_text = live_text(fixture, later.unit_index);
    std::vector<std::string> lines = read_lines(*sandbox);
    CHECK(line_at(lines, "created " + later_text + " nil") < 0);
    CHECK(line_at(lines, "tick 1") >= 0);
    advance(fixture);
    lines = read_lines(*sandbox);
    const int earlier_tick = line_at(lines, "tick 1");
    const int later_created = line_at(lines, "created " + later_text + " nil");
    const int later_tick = line_at(lines, "tick 2");
    const int earlier_created = line_at(lines, "created " + earlier_text + " nil");
    CHECK(earlier_tick >= 0 && later_created > earlier_tick && later_tick > later_created);
    CHECK(earlier_created >= 0 && earlier_created < earlier_tick);
}

void other_players_units_stay_quiet() {
    Fixture fixture;
    Package package = recording_package();
    host::Sandbox* sandbox = seat_ready(fixture, package);
    auto& foreign = fixture.spawn(0, collector, 30, 30);
    fixture.match->apply_damage_event(foreign, nullptr, static_cast<int16_t>(1), 11, 0);
    advance(fixture);
    const std::vector<std::string> lines = read_lines(*sandbox);
    require_absent(lines, "created ");
    require_absent(lines, "finished ");
    require_absent(lines, "damaged ");
    require_line(lines, "tick 1");
}

void enemy_death_follows_the_seen_list() {
    Fixture fixture;
    Package package = recording_package();
    host::Sandbox* sandbox = seat_ready(fixture, package);
    (void)fixture.spawn(computer, collector, 8, 8);
    auto& hidden = fixture.spawn(0, vehicle, 60, 8);
    const uint16_t hidden_slot = hidden.unit_index;
    const uint32_t hidden_generation = brains::unit_generation(*fixture.match, hidden_slot);
    fixture.match->kill_unit(
        hidden_slot, static_cast<uint8_t>(sim::match_runtime::DeathKind::dismissed)
    );
    advance(fixture);
    std::vector<std::string> lines = read_lines(*sandbox);
    require_absent(lines, "destroyed ");

    auto& visible = fixture.spawn(0, vehicle, 10, 8);
    const std::string visible_text = live_text(fixture, visible.unit_index);
    while (fixture.match->state().game.tick < 30)
        advance(fixture);
    fixture.match->kill_unit(
        visible.unit_index, static_cast<uint8_t>(sim::match_runtime::DeathKind::dismissed)
    );
    advance(fixture);
    lines = read_lines(*sandbox);
    require_line(lines, "destroyed " + visible_text + " nil nil");
    CHECK(
        line_at(lines, "destroyed " + handle_text(hidden_slot, hidden_generation) + " nil nil") < 0
    );
    CHECK(count_prefix(lines, "destroyed ") == 1);
}

void full_awareness_reports_every_enemy_death() {
    Fixture fixture;
    Package package = recording_package();
    package.full_awareness = true;
    host::Sandbox* sandbox = seat_ready(fixture, package);
    auto& hidden = fixture.spawn(0, vehicle, 60, 8);
    const std::string hidden_text = live_text(fixture, hidden.unit_index);
    fixture.match->kill_unit(
        hidden.unit_index, static_cast<uint8_t>(sim::match_runtime::DeathKind::dismissed)
    );
    advance(fixture);
    const std::vector<std::string> lines = read_lines(*sandbox);
    require_line(lines, "destroyed " + hidden_text + " nil nil");
}

uint16_t gifted_copy(Fixture& fixture, uint16_t old_slot) {
    uint16_t found = 0;
    for (const auto& slot : fixture.match->world().slots) {
        if (slot.unit == nullptr || slot.unit_index == old_slot)
            continue;
        if (slot.record.owner_index == 0 && slot.record.type_index == vehicle)
            found = slot.unit_index;
    }
    return found;
}

void gift_reports_given_and_not_destroyed() {
    Fixture fixture;
    make_computer(fixture, 0);
    Package giver = recording_package(computer);
    Package receiver = recording_package(0);
    host::Sandbox* giver_sandbox = seat_ready(fixture, giver);
    host::Sandbox* receiver_sandbox = seat_ready(fixture, receiver);

    auto& original = fixture.spawn(computer, vehicle, 18, 18);
    const uint16_t old_slot = original.unit_index;
    const uint32_t old_generation = brains::unit_generation(*fixture.match, old_slot);
    fixture.match->transfer_unit(old_slot, 0);
    const uint16_t copy_slot = gifted_copy(fixture, old_slot);
    CHECK(copy_slot != 0);
    const uint32_t copy_generation = brains::unit_generation(*fixture.match, copy_slot);
    const std::string given = "given " + handle_text(old_slot, old_generation) + " " +
                              handle_text(copy_slot, copy_generation) + " 1 0";

    advance(fixture);
    const std::vector<std::string> giver_lines = read_lines(*giver_sandbox);
    const std::vector<std::string> receiver_lines = read_lines(*receiver_sandbox);
    require_line(giver_lines, given);
    require_line(receiver_lines, given);
    require_absent(giver_lines, "destroyed ");
    require_absent(receiver_lines, "destroyed ");
}

void cancelled_frame_skips_finished() {
    Fixture fixture;
    Package package = recording_package();
    host::Sandbox* sandbox = seat_ready(fixture, package);

    sim::unit_spawn::Request request;
    request.player = computer;
    request.type = collector;
    request.position = {centre_bits(12), 0, centre_bits(12)};
    request.finished = false;
    request.state = 1;
    auto* frame = fixture.match->create(request, 0);
    CHECK(frame != nullptr && frame->unit != nullptr);
    const std::string frame_text = live_text(fixture, frame->unit_index);
    fixture.match->finish_unit(frame->unit_index, frame->unit_index);
    fixture.match->kill_unit(
        frame->unit_index, static_cast<uint8_t>(sim::match_runtime::DeathKind::cancelled)
    );
    advance(fixture);
    const std::vector<std::string> lines = read_lines(*sandbox);
    require_line(lines, "created " + frame_text + " nil");
    require_line(lines, "destroyed " + frame_text + " nil nil");
    require_absent(lines, "finished ");
}

void place_walls(Fixture& fixture, int32_t home_x, int32_t home_z) {
    const int32_t origins[][2] = {
        {home_x - 2, home_z - 2},
        {home_x, home_z - 2},
        {home_x + 2, home_z - 2},
        {home_x - 2, home_z},
        {home_x + 2, home_z},
        {home_x - 2, home_z + 2},
        {home_x, home_z + 2},
        {home_x + 2, home_z + 2},
    };
    for (const auto& origin : origins) {
        auto& wall = fixture.spawn(0, collector, origin[0], origin[1]);
        CHECK(wall.record.cell_x == origin[0] && wall.record.cell_z == origin[1]);
    }
}

void move_order_reports_done_and_idle() {
    Fixture fixture(0, true, {}, 1);
    CHECK(fixture.match->movement_map(1) != nullptr);
    fixture.match->state().unit_defs[vehicle].default_mission_type =
        static_cast<int8_t>(sim::ground_orders::standby_kind);
    Package package = recording_package();
    host::Sandbox* sandbox = seat_ready(fixture, package);

    auto& mover = fixture.spawn(computer, vehicle, 40, 40);
    CHECK(mover.record.cell_x == 40 && mover.record.cell_z == 40);
    CHECK(mover.record.movement);
    CHECK(fixture.match->ground_runtime(mover.unit_index) != nullptr);
    const std::string mover_text = live_text(fixture, mover.unit_index);
    auto& issued = fixture.match->issue_ground_move(mover.unit_index, centre_point(42, 40), false);
    CHECK(!fixture.match->order_refused(issued));

    bool arrived = false;
    for (uint32_t passed = 0; passed < 1200 && !arrived; ++passed) {
        advance(fixture);
        const std::vector<std::string> lines = read_lines(*sandbox);
        arrived = count_prefix(lines, "command " + mover_text + " Move_Ground done") == 1 &&
                  count_prefix(lines, "idle " + mover_text) == 1;
    }
    const std::vector<std::string> lines = read_lines(*sandbox);
    CHECK(arrived);
    CHECK(count_prefix(lines, "command " + mover_text + " Move_Ground done") == 1);
    CHECK(count_prefix(lines, "idle " + mover_text) == 1);
    CHECK(count_prefix(lines, "path ") == 0);
}

void carry_reports_loaded_and_unloaded() {
    Fixture fixture;
    Package package = recording_package();
    host::Sandbox* sandbox = seat_ready(fixture, package);
    auto& parent = fixture.spawn(computer, vehicle, 8, 8);
    auto& child = fixture.spawn(computer, vehicle, 14, 8);
    const std::string parent_text = live_text(fixture, parent.unit_index);
    const std::string child_text = live_text(fixture, child.unit_index);
    fixture.match->set_carry_link(child.unit_index, parent.unit_index, static_cast<int8_t>(-1), 1);
    fixture.match->set_carry_link(child.unit_index, 0, static_cast<int8_t>(-1), 1);
    advance(fixture);
    const std::vector<std::string> lines = read_lines(*sandbox);
    const int loaded_at = line_at(lines, "loaded " + child_text + " " + parent_text);
    // onUnitUnloaded names the carrier the unit left (design: unit, carrier).
    const int unloaded_at = line_at(lines, "unloaded " + child_text + " " + parent_text);
    CHECK(loaded_at >= 0 && unloaded_at > loaded_at);
}

void sight_reports_seen_and_lost() {
    Fixture fixture;
    Package package = recording_package();
    host::Sandbox* sandbox = seat_ready(fixture, package);
    (void)fixture.spawn(computer, collector, 8, 8);
    auto& enemy = fixture.spawn(0, vehicle, 10, 8);
    const std::string enemy_text = live_text(fixture, enemy.unit_index);
    advance_n(fixture, 31);
    std::vector<std::string> lines = read_lines(*sandbox);
    require_line(lines, "seen " + enemy_text);
    require_absent(lines, "lost ");

    const int32_t far_x = centre_fixed(60);
    const int32_t far_z = centre_fixed(8);
    fixture.match->place_unit_at(enemy.unit_index, far_x, 0, far_z, 1);
    advance_n(fixture, 30);
    lines = read_lines(*sandbox);
    require_line(lines, "lost " + enemy_text);
    CHECK(fixture.match->state().game.tick == 61);
}

void blocked_goal_reports_path_failed_once() {
    Fixture fixture(0, true, {}, 1);
    CHECK(fixture.match->movement_map(1) != nullptr);
    Package package = recording_package();
    host::Sandbox* sandbox = seat_ready(fixture, package);
    place_walls(fixture, 8, 8);
    auto& mover = fixture.spawn(computer, vehicle, 8, 8);
    CHECK(mover.record.cell_x == 8 && mover.record.cell_z == 8);
    CHECK(mover.record.movement);
    CHECK(fixture.match->ground_runtime(mover.unit_index) != nullptr);
    const std::string mover_text = live_text(fixture, mover.unit_index);
    auto& issued = fixture.match->issue_ground_move(mover.unit_index, centre_point(20, 8), false);
    CHECK(!fixture.match->order_refused(issued));
    advance_n(fixture, 120);
    const std::vector<std::string> lines = read_lines(*sandbox);
    CHECK(count_prefix(lines, "path " + mover_text) == 1);
}

void open_move_reports_no_path_failed() {
    Fixture fixture(0, true, {}, 1);
    CHECK(fixture.match->movement_map(1) != nullptr);
    Package package = recording_package();
    host::Sandbox* sandbox = seat_ready(fixture, package);
    auto& mover = fixture.spawn(computer, vehicle, 40, 40);
    CHECK(mover.record.cell_x == 40 && mover.record.cell_z == 40);
    auto& issued = fixture.match->issue_ground_move(mover.unit_index, centre_point(42, 40), false);
    CHECK(!fixture.match->order_refused(issued));
    advance_n(fixture, 120);
    const std::vector<std::string> lines = read_lines(*sandbox);
    CHECK(count_prefix(lines, "path ") == 0);
}

void alliance_and_defeat_report_once() {
    Fixture fixture;
    Package package = recording_package();
    host::Sandbox* sandbox = seat_ready(fixture, package);
    advance(fixture);
    std::vector<std::string> lines = read_lines(*sandbox);
    require_absent(lines, "alliance ");
    require_absent(lines, "defeated ");

    std::array<uint8_t, 10> allies{};
    allies[computer] = 1;
    allies[0] = 1;
    fixture.match->configure_player_alliances(computer, allies);
    advance(fixture);
    lines = read_lines(*sandbox);
    CHECK(count_prefix(lines, "alliance ") == 1);
    require_line(lines, "alliance 0 true");

    auto& victim = fixture.spawn(0, collector, 20, 20);
    const uint16_t victim_slot = victim.unit_index;
    const uint32_t victim_generation = brains::unit_generation(*fixture.match, victim_slot);
    fixture.match->kill_unit(
        victim_slot, static_cast<uint8_t>(sim::match_runtime::DeathKind::dismissed)
    );
    advance(fixture);
    lines = read_lines(*sandbox);
    CHECK(count_prefix(lines, "alliance ") == 1);
    CHECK(count_prefix(lines, "defeated ") == 1);
    require_line(lines, "defeated 0");
    require_line(lines, "destroyed " + handle_text(victim_slot, victim_generation) + " nil nil");

    advance(fixture);
    lines = read_lines(*sandbox);
    CHECK(count_prefix(lines, "alliance ") == 1);
    CHECK(count_prefix(lines, "defeated ") == 1);
}

void missing_hook_still_ticks() {
    const char* script = R"(local hooks = {}
log = {}
function hooks.fits()
  return true
end
function hooks.start()
  return true
end
function hooks.onTick(tick)
  log[#log + 1] = "tick " .. tostring(tick)
end
return hooks
)";
    Fixture fixture;
    Package package = package_with(script);
    host::Sandbox* sandbox = seat_ready(fixture, package);
    (void)fixture.spawn(computer, collector, 8, 8);
    advance(fixture);
    const std::vector<std::string> lines = read_lines(*sandbox);
    require_line(lines, "tick 1");
    CHECK(!sandbox->failed());
    CHECK(brains::brain_seated(*fixture.match, computer));
}

void missing_on_tick_is_fine() {
    const char* script = R"(local hooks = {}
log = {}
local function unit_text(unit)
  if unit == nil then
    return "nil"
  end
  return string.format("u%d:%d", unit.slot, unit.id)
end
function hooks.fits()
  return true
end
function hooks.start()
  return true
end
function hooks.onUnitCreated(unit, builder)
  log[#log + 1] = "created " .. unit_text(unit) .. " " .. unit_text(builder)
end
return hooks
)";
    Fixture fixture;
    Package package = package_with(script);
    host::Sandbox* sandbox = seat_ready(fixture, package);
    auto& solar = fixture.spawn(computer, collector, 8, 8);
    const std::string solar_text = live_text(fixture, solar.unit_index);
    advance(fixture);
    advance(fixture);
    const std::vector<std::string> lines = read_lines(*sandbox);
    require_line(lines, "created " + solar_text + " nil");
    require_absent(lines, "tick ");
    CHECK(!sandbox->failed());
    CHECK(brains::brain_seated(*fixture.match, computer));
}

void bank_allows_a_heavy_tick() {
    const char* script = R"(local hooks = {}
log = {}
function hooks.fits()
  return true
end
function hooks.start()
  return true
end
function hooks.onTick(tick)
  log[#log + 1] = "tick " .. tostring(tick)
  if tick % 30 == 0 then
    local total = 0
    for step = 1, 200000 do
      total = total + step
    end
    sink = total
  end
end
return hooks
)";
    Fixture fixture;
    Package package = package_with(script);
    host::Sandbox* sandbox = seat_ready(fixture, package);
    advance_n(fixture, 60);
    const std::vector<std::string> lines = read_lines(*sandbox);
    CHECK(count_prefix(lines, "tick ") == 60);
    CHECK(!sandbox->failed());
    CHECK(brains::brain_seated(*fixture.match, computer));
}

struct DamageStop {
    int stops{};
    uint8_t player{};
    uint32_t tick{};
    host::ErrorKind kind{host::ErrorKind::none};
};

void remember_stop(void* context, uint8_t player, uint32_t tick, const host::ScriptError& error) {
    auto* note = static_cast<DamageStop*>(context);
    note->stops += 1;
    note->player = player;
    note->tick = tick;
    note->kind = error.kind;
}

void looping_damage_hook_stops_the_seat() {
    const char* script = R"(local hooks = {}
function hooks.fits()
  return true
end
function hooks.start()
  return true
end
function hooks.onUnitDamaged()
  while true do
  end
end
return hooks
)";
    Fixture fixture;
    fixture.spawn(0, vehicle, 8, 8);
    fixture.spawn(computer, vehicle, 40, 40);
    CHECK(sim::ai::configure_match_computer_players(*fixture.match, profile, build_lists, false));
    Package package = package_with(script);
    package.bank.call_time_limit = std::chrono::milliseconds{30'000};
    DamageStop note;
    brains::ReportHooks hooks;
    hooks.context = &note;
    hooks.stopped = &remember_stop;
    brains::set_report_hooks(*fixture.match, hooks);
    (void)seat_ready(fixture, package);
    auto& own = fixture.spawn(computer, collector, 8, 8);
    fixture.match->apply_damage_event(own, nullptr, static_cast<int16_t>(1), 11, 0);
    advance(fixture);
    CHECK(note.stops == 1);
    CHECK(note.player == computer);
    CHECK(note.tick == fixture.match->state().game.tick);
    CHECK(note.kind == host::ErrorKind::instruction_budget);
    CHECK(!brains::brain_seated(*fixture.match, computer));
    CHECK(brains::seat_sandbox(*fixture.match, computer) == nullptr);
    CHECK(fixture.match->seat_hooks(computer).play == nullptr);
    auto* players = sim::ai::match_computer_players(*fixture.match);
    CHECK(players != nullptr);
    CHECK(
        players->players[computer].sort_countdown ==
        static_cast<int32_t>(sim::ai::sort_interval_ticks)
    );
    advance(fixture);
    CHECK(
        players->players[computer].sort_countdown ==
        static_cast<int32_t>(sim::ai::sort_interval_ticks) - 1
    );
    CHECK(note.stops == 1);
    CHECK(!brains::brain_seated(*fixture.match, computer));
}

struct CounterRead {
    lua_Integer hits{-1};
};

void read_hits(void* context, lua_State* state) {
    auto* held = static_cast<CounterRead*>(context);
    lua_getglobal(state, "hits");
    if (lua_isinteger(state, -1) != 0)
        held->hits = lua_tointeger(state, -1);
    lua_pop(state, 1);
}

void overflow_drops_events() {
    const char* script = R"(local hooks = {}
hits = 0
function hooks.fits()
  return true
end
function hooks.start()
  return true
end
function hooks.onUnitDamaged()
  hits = hits + 1
end
function hooks.onTick()
end
return hooks
)";
    Fixture fixture;
    auto& own = fixture.spawn(computer, collector, 8, 8);
    Package package = package_with(script);
    package.bank.per_tick = 20'000'000;
    package.bank.ticks = 30;
    package.bank.call_time_limit = std::chrono::milliseconds{30'000};
    host::Sandbox* sandbox = seat_ready(fixture, package);
    for (uint32_t extra = 0; extra < 65537; ++extra)
        fixture.match->apply_damage_event(own, nullptr, static_cast<int16_t>(1), 10, 0);
    advance(fixture);
    CHECK(brains::dropped_events(*fixture.match, computer) == 1);
    CHECK(!sandbox->failed());
    CounterRead held;
    host::ScriptError error;
    CHECK(sandbox->run_host("read", &held, read_hits, error));
    CHECK(held.hits == 65536);
}

void same_stream(Fixture& left, Fixture& right, uint32_t ticks) {
    CHECK(left.match->random_state() == right.match->random_state());
    for (uint32_t passed = 0; passed < ticks; ++passed) {
        advance(left);
        advance(right);
        if (left.match->random_state() != right.match->random_state())
            throw std::runtime_error("random diverged at tick " + std::to_string(passed + 1));
    }
    const oa::base::game_loop::Timing timing{};
    const uint64_t left_hash = oa::sim::trace::match_state_hash(*left.match, timing, 0, 0, nullptr);
    const uint64_t right_hash =
        oa::sim::trace::match_state_hash(*right.match, timing, 0, 0, nullptr);
    CHECK(left_hash == right_hash);
}

void recording_and_empty_hooks_match() {
    Fixture recording;
    Fixture quiet;
    Package recorded = recording_package();
    Package empty = package_with("return {}\n");
    (void)seat_ready(recording, recorded);
    (void)seat_ready(quiet, empty);
    same_stream(recording, quiet, run_ticks);
}

void no_brain_adds_no_listener() {
    Fixture left;
    Fixture right;
    CHECK(left.match->event_listener_count() == 0);
    CHECK(right.match->event_listener_count() == 0);
    same_stream(left, right, 120);
    CHECK(left.match->event_listener_count() == 0);
    CHECK(right.match->event_listener_count() == 0);
}

} // namespace

int main() {
    try {
        spawned_unit_reports_created_and_finished();
        damage_reports_amount_kind_and_attacker();
        kill_reports_killer_and_wreck();
        events_arrive_in_order_before_on_tick();
        event_after_the_turn_waits();
        other_players_units_stay_quiet();
        enemy_death_follows_the_seen_list();
        full_awareness_reports_every_enemy_death();
        gift_reports_given_and_not_destroyed();
        cancelled_frame_skips_finished();
        move_order_reports_done_and_idle();
        carry_reports_loaded_and_unloaded();
        sight_reports_seen_and_lost();
        blocked_goal_reports_path_failed_once();
        open_move_reports_no_path_failed();
        alliance_and_defeat_report_once();
        missing_hook_still_ticks();
        missing_on_tick_is_fine();
        bank_allows_a_heavy_tick();
        looping_damage_hook_stops_the_seat();
        overflow_drops_events();
        recording_and_empty_hooks_match();
        no_brain_adds_no_listener();
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "sim-brains-events: %s\n", ex.what());
        return 1;
    }
    return 0;
}

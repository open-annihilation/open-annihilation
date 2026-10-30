// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Building sites a seated brain judges, beside the match's own checks.
// The made-up brain is order-giver. A load with no match uses steady-commander.

#include "match_fixture.hpp"

#include "oa/data/brain_facts.hpp"
#include "oa/sim/brains/check.hpp"
#include "oa/sim/brains/classic.hpp"
#include "oa/sim/brains/seat.hpp"
#include "oa/sim/match_runtime/construction_orders.hpp"

#include "lua.hpp"

#include <array>
#include <bit>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <span>
#include <string>
#include <string_view>

namespace {

namespace brains = oa::sim::brains;
namespace host = oa::data::script_host;
namespace runtime = oa::sim::match_runtime;

constexpr int32_t sample_cells[][2] = {
    {-1, -1},
    {-1, 0},
    {-1, 2},
    {0, -1},
    {0, 0},
    {0, 2},
    {1, 0},
    {1, 1},
    {2, 2},
    {8, 8},
    {30, 31},
    {61, 61},
    {62, 61},
    {61, 62},
    {62, 62},
    {63, 30},
    {70, 8},
    {8, 70},
};

struct SeatRequest {
    bool full_awareness{};
    bool classic{};
};

struct Flag {
    const char* name{};
    bool present{};
    bool value{};
};

struct TextFlag {
    const char* name{};
    std::string text{};
};

struct Number {
    const char* name{};
    bool present{};
    lua_Integer value{};
};

struct Chunk {
    const char* source{};
};

struct Found {
    bool any{};
    int32_t cell_x{};
    int32_t cell_z{};
    int32_t centre_x{};
    int32_t centre_z{};
};

const char* quiet_brain() {
    return R"(local hooks = {}
function hooks.fits()
    return true
end
function hooks.start()
    return true
end
return hooks
)";
}

int32_t whole_fixed(int32_t world_units) {
    return static_cast<int32_t>(static_cast<int64_t>(world_units) * int64_t{65536});
}

int64_t floor_div(int64_t number, int64_t divisor) {
    int64_t quot = number / divisor;
    const int64_t rest = number % divisor;
    if (rest != 0 && number < 0)
        --quot;
    return quot;
}

int32_t centre_axis(int32_t footprint, int32_t cell) {
    const int64_t sum = static_cast<int64_t>(footprint) + static_cast<int64_t>(cell) * 2;
    const auto bits = static_cast<uint32_t>(sum);
    return static_cast<int32_t>(bits << 19);
}

int32_t anchor_cell(int32_t near, int32_t footprint) {
    const int64_t scaled = floor_div(static_cast<int64_t>(near), int64_t{1} << 19);
    return static_cast<int32_t>(floor_div(scaled - footprint, 2));
}

bool add_cell(int32_t base, int32_t delta, int32_t* out) {
    const int64_t sum = static_cast<int64_t>(base) + delta;
    if (sum > 2147483647 || sum < (int64_t{-2147483647} - 1))
        return false;
    *out = static_cast<int32_t>(sum);
    return true;
}

void turned_footprint(
    const runtime::Match& match, uint16_t type, uint8_t facing, int32_t* across, int32_t* down
) {
    const bool turned = (match.build_facing(type, facing) & 1U) != 0;
    const auto& runtime_type = match.world().types[type];
    *across = static_cast<int32_t>(turned ? runtime_type.footprint_z : runtime_type.footprint_x);
    *down = static_cast<int32_t>(turned ? runtime_type.footprint_x : runtime_type.footprint_z);
}

bool footprint_on_map(
    const runtime::Match& match, uint16_t type, int32_t cell_x, int32_t cell_z, uint8_t facing
) {
    int32_t across = 0;
    int32_t down = 0;
    turned_footprint(match, type, facing, &across, &down);
    const auto width = static_cast<int32_t>(match.spatial().terrain_width);
    const auto height = static_cast<int32_t>(match.spatial().terrain_height);
    return cell_x >= 0 && cell_z >= 0 && width > cell_x + across && height > cell_z + down;
}

bool fog_site(
    runtime::Match& match, uint16_t type, int32_t cell_x, int32_t cell_z, uint8_t facing
) {
    if (!footprint_on_map(match, type, cell_x, cell_z, facing))
        return false;
    runtime::BuildSiteOptions options;
    options.facing = facing;
    return match.building_site(type, cell_x, cell_z, 0, computer, options).has_value();
}

std::array<uint32_t, 3> centre_point(int32_t cell_x, int32_t cell_z, int32_t across, int32_t down) {
    return {
        static_cast<uint32_t>((cell_x * 2 + across) * 0x80000),
        0U,
        static_cast<uint32_t>((cell_z * 2 + down) * 0x80000)
    };
}

/// Mapping and line of sight, as a gated building site. `mapped` fills the
/// viewpoint bit and the computer seat's bit afterwards, which a reset with
/// mapping on has just cleared. The comparisons below treat the two as having
/// explored the same ground.
void show_map(Fixture& fixture, bool mapped) {
    fixture.match->state().game.visibility_flags = static_cast<uint8_t>(3);
    fixture.match->reset_sight_buffers(true);
    if (!mapped)
        return;
    const auto both = static_cast<uint16_t>(1u | (1u << computer));
    for (uint16_t& bits : fixture.match->sight_mutable().player_bits)
        bits = both;
}

host::Sandbox* seat_quiet(Fixture& fixture, const std::string& script, SeatRequest request) {
    const auto bytes =
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(script.data()), script.size()};
    const brains::PackageFile file{"brain.lua", bytes};
    const std::span<const brains::PackageFile> files(&file, 1);
    const std::array<char, 65> fingerprint = brains::classic_fingerprint(files);
    brains::SeatSpec spec;
    spec.player = computer;
    spec.id = "order-giver";
    spec.name = "Order Giver";
    spec.version = "1";
    spec.entry = "brain.lua";
    spec.level = "hard";
    spec.difficulty = OA_DIFFICULTY_HARD;
    spec.full_awareness = request.full_awareness;
    spec.files = files;
    if (request.classic)
        spec.classic_fingerprint_for_tests = fingerprint.data();
    const brains::SeatOutcome outcome = brains::seat_brain(*fixture.match, spec);
    if (outcome.result != brains::SeatResult::seated)
        throw std::runtime_error(std::string("seat refused: ") + outcome.reason.data());
    if (request.classic)
        CHECK(brains::seat_is_classic(*fixture.match, computer));
    else
        CHECK(!brains::seat_is_classic(*fixture.match, computer));
    host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, computer);
    CHECK(sandbox != nullptr);
    return sandbox;
}

int push_chunk(void* context, lua_State* state) {
    const auto* chunk = static_cast<const Chunk*>(context);
    if (chunk == nullptr || chunk->source == nullptr)
        return luaL_error(state, "missing probe");
    if (luaL_loadstring(state, chunk->source) != 0) {
        const char* message = lua_tostring(state, -1);
        if (message == nullptr)
            message = "probe failed to load";
        return luaL_error(state, "%s", message);
    }
    return 0;
}

void run_chunk(host::Sandbox& sandbox, const std::string& source) {
    Chunk chunk{source.c_str()};
    host::CallHooks hooks;
    hooks.context = &chunk;
    hooks.push = push_chunk;
    host::ScriptError error;
    if (!sandbox.call("probe", hooks, error))
        throw std::runtime_error(error.message.empty() ? "probe failed" : error.message);
}

void read_flag(void* context, lua_State* state) {
    auto* flag = static_cast<Flag*>(context);
    lua_getglobal(state, flag->name);
    flag->present = lua_isnil(state, -1) == 0;
    flag->value = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
}

bool flag_of(host::Sandbox& sandbox, const char* name) {
    Flag flag{name, false, false};
    host::ScriptError error;
    CHECK(sandbox.run_host("read-flag", &flag, read_flag, error));
    CHECK(flag.present);
    return flag.value;
}

void read_text(void* context, lua_State* state) {
    auto* flag = static_cast<TextFlag*>(context);
    lua_getglobal(state, flag->name);
    std::size_t length = 0;
    const char* text = lua_tolstring(state, -1, &length);
    if (text != nullptr)
        flag->text.assign(text, length);
    lua_pop(state, 1);
}

std::string text_of(host::Sandbox& sandbox, const char* name) {
    TextFlag flag{name, {}};
    host::ScriptError error;
    CHECK(sandbox.run_host("read-text", &flag, read_text, error));
    return flag.text;
}

void read_number(void* context, lua_State* state) {
    auto* number = static_cast<Number*>(context);
    lua_getglobal(state, number->name);
    number->present = lua_isinteger(state, -1) != 0;
    number->value = lua_tointeger(state, -1);
    lua_pop(state, 1);
}

lua_Integer number_of(host::Sandbox& sandbox, const char* name) {
    Number number{name, false, 0};
    host::ScriptError error;
    CHECK(sandbox.run_host("read-number", &number, read_number, error));
    CHECK(number.present);
    return number.value;
}

bool ask_site(
    host::Sandbox& sandbox,
    const char* type_name,
    int32_t cell_x,
    int32_t cell_z,
    int facing,
    const char* check
) {
    std::string source = "local ok, value = pcall(function() return brain.site(\"";
    source += type_name;
    source += "\", ";
    source += std::to_string(cell_x);
    source += ", ";
    source += std::to_string(cell_z);
    source += ", ";
    source += std::to_string(facing);
    if (check != nullptr) {
        source += ", \"";
        source += check;
        source += "\"";
    }
    source += ") end)\nraised = not ok\n";
    source += "if ok then clear = value and true or false else raised_text = tostring(value) end\n";
    run_chunk(sandbox, source);
    if (flag_of(sandbox, "raised"))
        throw std::runtime_error(text_of(sandbox, "raised_text"));
    return flag_of(sandbox, "clear");
}

Found walk_place(
    runtime::Match& match,
    bool aware,
    uint16_t type,
    int32_t near_x,
    int32_t near_z,
    int32_t radius,
    uint8_t facing
) {
    int32_t across = 0;
    int32_t down = 0;
    turned_footprint(match, type, facing, &across, &down);
    const int32_t anchor_x = anchor_cell(near_x, across);
    const int32_t anchor_z = anchor_cell(near_z, down);
    int32_t ring_limit = 4096;
    const int32_t rings = radius / 16;
    if (rings < ring_limit)
        ring_limit = rings;
    int32_t tried = 0;
    Found found;
    const auto consider = [&](int32_t cell_x, int32_t cell_z) -> bool {
        if (tried >= 4096)
            return true;
        ++tried;
        const bool clear = aware ? match.site_clear_for(type, cell_x, cell_z, 0, 1, facing)
                                 : fog_site(match, type, cell_x, cell_z, facing);
        if (!clear)
            return false;
        found.any = true;
        found.cell_x = cell_x;
        found.cell_z = cell_z;
        found.centre_x = centre_axis(across, cell_x);
        found.centre_z = centre_axis(down, cell_z);
        return true;
    };
    for (int32_t ring = 0; ring <= ring_limit && !found.any; ++ring) {
        int32_t west = 0;
        int32_t east = 0;
        int32_t north = 0;
        int32_t south = 0;
        if (!add_cell(anchor_x, -ring, &west) || !add_cell(anchor_x, ring, &east) ||
            !add_cell(anchor_z, -ring, &north) || !add_cell(anchor_z, ring, &south))
            break;
        if (ring == 0) {
            if (consider(anchor_x, anchor_z))
                break;
            continue;
        }
        bool stop = false;
        for (int32_t cell_x = west; cell_x <= east; ++cell_x) {
            if (consider(cell_x, north)) {
                stop = true;
                break;
            }
        }
        if (stop)
            break;
        for (int32_t cell_z = north + 1; cell_z <= south; ++cell_z) {
            if (consider(east, cell_z)) {
                stop = true;
                break;
            }
        }
        if (stop)
            break;
        for (int32_t cell_x = east - 1; cell_x >= west; --cell_x) {
            if (consider(cell_x, south)) {
                stop = true;
                break;
            }
        }
        if (stop)
            break;
        for (int32_t cell_z = south - 1; cell_z >= north + 1; --cell_z) {
            if (consider(west, cell_z)) {
                stop = true;
                break;
            }
        }
    }
    return found;
}

void expect_place(
    host::Sandbox& sandbox, runtime::Match& match, bool aware, int32_t radius, const Found& expected
) {
    const std::string source =
        std::string("placed = false\nplace_cx = nil\nplace_cz = nil\n") +
        "centre_x = nil\ncentre_z = nil\n" + "local ok, err = pcall(function()\n" +
        "  local cx, cz, x, z = brain.place(\"ARMSOLAR\", {x = 16, z = 16}, " +
        std::to_string(radius) + ", 0)\n" + "  if cx ~= nil then\n" + "    placed = true\n" +
        "    place_cx = cx\n" + "    place_cz = cz\n" + "    centre_x = x:raw()\n" +
        "    centre_z = z:raw()\n" + "  end\n" + "end)\n" + "raised = not ok\n" +
        "raised_text = tostring(err)\n";
    run_chunk(sandbox, source);
    CHECK(!flag_of(sandbox, "raised"));
    CHECK(!sandbox.failed());
    const Found walked =
        walk_place(match, aware, collector, whole_fixed(16), whole_fixed(16), radius, 0);
    CHECK(walked.any == expected.any);
    if (expected.any) {
        CHECK(walked.cell_x == expected.cell_x);
        CHECK(walked.cell_z == expected.cell_z);
        CHECK(walked.centre_x == expected.centre_x);
        CHECK(walked.centre_z == expected.centre_z);
    }
    CHECK(flag_of(sandbox, "placed") == expected.any);
    if (!expected.any)
        return;
    CHECK(number_of(sandbox, "place_cx") == expected.cell_x);
    CHECK(number_of(sandbox, "place_cz") == expected.cell_z);
    CHECK(number_of(sandbox, "centre_x") == expected.centre_x);
    CHECK(number_of(sandbox, "centre_z") == expected.centre_z);
}

void compare_grid(host::Sandbox& sandbox, runtime::Match& match, bool aware, Type type) {
    const char* type_name = type == vehicle ? "ARMCV" : "ARMSOLAR";
    for (const auto& cell : sample_cells) {
        for (int facing = 0; facing < 4; ++facing) {
            const auto face = static_cast<uint8_t>(facing);
            const bool site = ask_site(sandbox, type_name, cell[0], cell[1], facing, nullptr);
            const bool yard = ask_site(sandbox, type_name, cell[0], cell[1], facing, "yard");
            if (aware) {
                CHECK(site == match.site_clear_for(type, cell[0], cell[1], 0, 1, face));
                runtime::BuildSiteOptions options;
                options.facing = face;
                CHECK(yard == match.building_site_clear(type, cell[0], cell[1], 0, options));
            } else {
                runtime::BuildSiteOptions options;
                options.facing = face;
                const bool placed =
                    match.building_site(type, cell[0], cell[1], 0, computer, options).has_value();
                CHECK(site == placed);
                CHECK(yard == placed);
            }
            if (aware && type == vehicle && cell[0] == 0 && cell[1] == 0 && facing == 0) {
                CHECK(site);
                CHECK(!yard);
            }
        }
    }
    CHECK(!sandbox.failed());
}

void aware_site_matches_the_open_checks(const std::string& script) {
    Fixture fixture;
    host::Sandbox* sandbox = seat_quiet(fixture, script, SeatRequest{true, false});
    compare_grid(*sandbox, *fixture.match, true, collector);
    compare_grid(*sandbox, *fixture.match, true, vehicle);
}

void fog_site_matches_the_placing_player(const std::string& script, bool classic) {
    Fixture fixture;
    show_map(fixture, true);
    host::Sandbox* sandbox = seat_quiet(fixture, script, SeatRequest{false, classic});
    compare_grid(*sandbox, *fixture.match, false, collector);
}

void an_enemy_unit_follows_the_seat_sight(const std::string& script) {
    constexpr int32_t cell = 8;
    Fixture hidden;
    show_map(hidden, true);
    hidden.spawn(0, vehicle, cell, cell);
    host::Sandbox* fog = seat_quiet(hidden, script, {});
    int32_t across = 0;
    int32_t down = 0;
    turned_footprint(*hidden.match, collector, 0, &across, &down);
    const std::array<uint32_t, 3> centre = centre_point(cell, cell, across, down);
    CHECK(hidden.match->point_mapped(centre));
    CHECK(!hidden.match->point_visible(computer, centre));
    CHECK(!hidden.match->site_clear_for(collector, cell, cell, 0, 1, 0));
    CHECK(ask_site(*fog, "ARMSOLAR", cell, cell, 0, nullptr));
    runtime::BuildSiteOptions options;
    CHECK(hidden.match->building_site(collector, cell, cell, 0, computer, options).has_value());

    Fixture watched;
    watched.spawn(0, vehicle, cell, cell);
    watched.spawn(computer, vehicle, cell, cell + 4);
    show_map(watched, true);
    host::Sandbox* blocked = seat_quiet(watched, script, {});
    CHECK(watched.match->point_mapped(centre));
    CHECK(watched.match->point_visible(computer, centre));
    CHECK(!ask_site(*blocked, "ARMSOLAR", cell, cell, 0, nullptr));
    CHECK(!watched.match->building_site(collector, cell, cell, 0, computer, options).has_value());

    Fixture open;
    show_map(open, false);
    host::Sandbox* unmapped = seat_quiet(open, script, {});
    CHECK(!open.match->point_mapped(centre));
    CHECK(!ask_site(*unmapped, "ARMSOLAR", cell, cell, 0, nullptr));
    CHECK(!open.match->building_site(collector, cell, cell, 0, computer, options).has_value());

    Fixture aware;
    show_map(aware, true);
    aware.spawn(0, vehicle, cell, cell);
    host::Sandbox* seen = seat_quiet(aware, script, SeatRequest{true, false});
    CHECK(!aware.match->point_visible(computer, centre));
    CHECK(!ask_site(*seen, "ARMSOLAR", cell, cell, 0, nullptr));
}

void a_mobile_type_raises_without_full_awareness(const std::string& script) {
    const auto expect_raise = [&](bool classic) {
        Fixture fixture;
        show_map(fixture, true);
        host::Sandbox* sandbox = seat_quiet(fixture, script, SeatRequest{false, classic});
        const char* body = classic ? "brain.place(\"ARMCV\", {x = 16, z = 16}, 16, 0)\n"
                                   : "brain.site(\"ARMCV\", 4, 4, 0)\n";
        const std::string source = std::string("local ok, err = pcall(function() ") + body +
                                   "end)\nraised = not ok\nraised_text = tostring(err)\n";
        run_chunk(*sandbox, source);
        CHECK(flag_of(*sandbox, "raised"));
        CHECK(!sandbox->failed());
        CHECK(
            text_of(*sandbox, "raised_text").find("site tests buildings; ARMCV is not one") !=
            std::string::npos
        );
    };
    expect_raise(false);
    expect_raise(true);
}

void place_returns_the_first_clear_cell(const std::string& script) {
    Found clear;
    clear.any = true;
    clear.cell_x = 1;
    clear.cell_z = 1;
    clear.centre_x = 2097152;
    clear.centre_z = 2097152;
    Found none;

    Fixture aware;
    host::Sandbox* open = seat_quiet(aware, script, SeatRequest{true, false});
    expect_place(*open, *aware.match, true, 16, clear);
    expect_place(*open, *aware.match, true, 0, none);
    aware.spawn(computer, collector, 1, 1);
    expect_place(*open, *aware.match, true, 16, none);

    Fixture fog;
    show_map(fog, true);
    host::Sandbox* hidden = seat_quiet(fog, script, {});
    expect_place(*hidden, *fog.match, false, 16, clear);

    Fixture unmapped;
    show_map(unmapped, false);
    host::Sandbox* blank = seat_quiet(unmapped, script, {});
    expect_place(*blank, *unmapped.match, false, 16, none);
}

/// The sight cell under `point`, found by which viewpoint bit `point_mapped`
/// reads. Leaves every mapped bit clear.
std::size_t sight_index(runtime::Match& match, const std::array<uint32_t, 3>& point) {
    auto& grid = match.sight_mutable();
    const auto mark = static_cast<uint16_t>(1u << grid.viewpoint_player);
    for (uint16_t& bits : grid.player_bits)
        bits = 0;
    for (std::size_t index = 0; index < grid.player_bits.size(); ++index) {
        grid.player_bits[index] = mark;
        const bool hit = match.point_mapped(point);
        grid.player_bits[index] = 0;
        if (hit)
            return index;
    }
    return grid.player_bits.size();
}

/// The vehicle's build list, filled the way the order tests fill it.
/// `unit:build` counts a type missing from that list as not buildable.
void ready_lists(Fixture& fixture) {
    CHECK(sim::ai::configure_match_computer_players(*fixture.match, profile, build_lists, false));
    sim::ai::prepare_match_computer_players(*fixture.match);
}

/// A brain without full awareness, seated on the computer rather than the
/// viewer, finds a site on ground that seat has explored and orders a
/// building there. Ground only the viewer has mapped stays closed.
void another_seat_builds_on_its_own_ground(const std::string& script) {
    constexpr int32_t own_cell = 8;
    constexpr int32_t viewer_cell = 40;
    Fixture fixture;
    show_map(fixture, false);
    CHECK(fixture.match->state().game.viewpoint_player != computer);
    auto& builder = fixture.spawn(computer, vehicle, 16, 16);
    const uint16_t builder_slot = builder.unit_index;
    int32_t across = 0;
    int32_t down = 0;
    turned_footprint(*fixture.match, collector, 0, &across, &down);
    const auto own_centre = centre_point(own_cell, own_cell, across, down);
    const auto viewer_centre = centre_point(viewer_cell, viewer_cell, across, down);
    const std::size_t own_index = sight_index(*fixture.match, own_centre);
    const std::size_t viewer_index = sight_index(*fixture.match, viewer_centre);
    auto& bits = fixture.match->sight_mutable().player_bits;
    CHECK(own_index < bits.size());
    CHECK(viewer_index < bits.size());
    CHECK(own_index != viewer_index);
    const auto viewpoint = fixture.match->sight_mutable().viewpoint_player;
    bits[own_index] = static_cast<uint16_t>(1u << computer);
    bits[viewer_index] = static_cast<uint16_t>(1u << viewpoint);
    CHECK(fixture.match->player_mapped(computer, own_centre));
    CHECK(!fixture.match->point_mapped(own_centre));
    CHECK(fixture.match->point_mapped(viewer_centre));
    CHECK(!fixture.match->player_mapped(computer, viewer_centre));
    ready_lists(fixture);
    CHECK(fixture.match->player_mapped(computer, own_centre));
    CHECK(!fixture.match->point_mapped(own_centre));

    host::Sandbox* sandbox = seat_quiet(fixture, script, {});
    CHECK(ask_site(*sandbox, "ARMSOLAR", own_cell, own_cell, 0, nullptr));
    CHECK(!ask_site(*sandbox, "ARMSOLAR", viewer_cell, viewer_cell, 0, nullptr));

    const char* source = R"(found = false
ordered = false
site_x = nil
site_z = nil
local builder = nil
me:each_unit(function(unit)
    if builder == nil and unit.finished == true and unit.type ~= nil and unit.type.builder == true
        and unit.type.bmcode == 1 then
        builder = unit
    end
end)
if builder ~= nil and builder.x ~= nil then
    local cx, cz, x, z = brain.place("ARMSOLAR", { x = builder.x, z = builder.z }, 256, 0)
    if x ~= nil then
        found = true
        site_x = x:raw()
        site_z = z:raw()
        ordered = builder:build("ARMSOLAR", { x = x, z = z }, 0) == true
    end
end
)";
    run_chunk(*sandbox, source);
    CHECK(!sandbox->failed());
    CHECK(flag_of(*sandbox, "found"));
    CHECK(flag_of(*sandbox, "ordered"));
    const auto site_x = static_cast<int32_t>(number_of(*sandbox, "site_x"));
    const auto site_z = static_cast<int32_t>(number_of(*sandbox, "site_z"));
    runtime::Match::OrderRecordView record{};
    CHECK(fixture.match->queue_records(builder_slot, false, &record, 1) == 1);
    CHECK(record.kind == runtime::mobile_build_kind);
    CHECK(record.parameter_1 == collector);
    CHECK(record.point[0] == site_x);
    CHECK(record.point[2] == site_z);
    const std::array<uint32_t, 3> ordered_at{
        std::bit_cast<uint32_t>(record.point[0]),
        std::bit_cast<uint32_t>(record.point[1]),
        std::bit_cast<uint32_t>(record.point[2])
    };
    CHECK(fixture.match->player_mapped(computer, ordered_at));
    CHECK(!fixture.match->point_mapped(ordered_at));
}

void outside_a_match_the_checks_are_nil() {
    const char* source = R"(local hooks = {}
function hooks.fits()
    local function zeros(counts)
        return counts.given == 0 and counts.not_own == 0 and counts.dead == 0
            and counts.not_seen == 0 and counts.not_buildable == 0
            and counts.over_limit == 0 and counts.by_engine == 0
    end
    if brain.site("ARMSOLAR", 2, 2) ~= nil then
        return false
    end
    if brain.place("ARMSOLAR", {x = 16, z = 16}, 32) ~= nil then
        return false
    end
    if not zeros(brain.order_counts()) then
        return false
    end
    if not zeros(brain.order_counts("match")) then
        return false
    end
    return true
end
return hooks
)";
    const auto bytes =
        std::span<const uint8_t>{reinterpret_cast<const uint8_t*>(source), std::strlen(source)};
    const brains::PackageFile file{"brain.lua", bytes};
    const std::span<const brains::PackageFile> files(&file, 1);
    brains::SeatSpec spec;
    spec.player = computer;
    spec.id = "steady-commander";
    spec.name = "Steady Commander";
    spec.version = "1";
    spec.entry = "brain.lua";
    spec.level = "hard";
    spec.difficulty = OA_DIFFICULTY_HARD;
    spec.files = files;
    const oa::data::brain_facts::GameFacts facts{};
    const brains::SeatOutcome outcome = brains::check_brain(spec, facts);
    if (outcome.result != brains::SeatResult::seated)
        throw std::runtime_error(std::string("load check: ") + outcome.reason.data());
}

} // namespace

int main() {
    try {
        const std::string script = quiet_brain();
        outside_a_match_the_checks_are_nil();
        aware_site_matches_the_open_checks(script);
        fog_site_matches_the_placing_player(script, false);
        fog_site_matches_the_placing_player(script, true);
        an_enemy_unit_follows_the_seat_sight(script);
        a_mobile_type_raises_without_full_awareness(script);
        place_returns_the_first_clear_cell(script);
        another_seat_builds_on_its_own_ground(script);
    } catch (const std::exception& ex) {
        std::fprintf(stderr, "sim-brains-sites: %s\n", ex.what());
        return 1;
    }
    return 0;
}

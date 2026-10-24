// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// AI profile tables, build lists and strategic strengths. The line walk is
// the engine's. Nothing here applies a weight or chooses a build.
#include "profile_lines.hpp"

#include "definitions.hpp"
#include "handles_internal.hpp"
#include "lua_game.hpp"
#include "read_only.hpp"

#include "oa/core/unit.h"
#include "oa/core/unit_def.h"
#include "oa/data/brain_facts.hpp"
#include "oa/data/limits.hpp"
#include "oa/data/script_host/f32.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/sim/ai.hpp"
#include "oa/sim/match_runtime.hpp"

#include "lua.hpp"

#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <memory>

namespace oa::sim::brains {
namespace {

constexpr const char* profile_denial = "a profile is read-only";
constexpr uint32_t list_room = data::limits::highest_build_list_copy + 1;

char lower_letter(char letter) noexcept {
    const auto value = static_cast<unsigned char>(letter);
    if (value >= 'A' && value <= 'Z')
        return static_cast<char>(value - 'A' + 'a');
    return letter;
}

bool equal_nocase(const char* left, const char* right) noexcept {
    if (left == nullptr || right == nullptr)
        return false;
    while (*left != '\0' && *right != '\0') {
        if (lower_letter(*left) != lower_letter(*right))
            return false;
        ++left;
        ++right;
    }
    return *left == *right;
}

bool same_word(const char* left, const char* right) noexcept {
    if (left == nullptr || right == nullptr)
        return false;
    return std::strcmp(left, right) == 0;
}

const char* word_at(const sim::ai::ProfileLine& line, uint32_t index) noexcept {
    if (index >= line.count || line.words == nullptr || line.words[index] == nullptr)
        return "";
    return line.words[index];
}

bool later_word(const sim::ai::ProfileLine& line, const char* keyword) noexcept {
    for (uint32_t index = 1; index < line.count; ++index)
        if (equal_nocase(word_at(line, index), keyword))
            return true;
    return false;
}

void profile_name(std::string_view path, const char** begin, std::size_t* size) noexcept {
    const char* start = path.data();
    const char* const end = path.data() + path.size();
    if (start == nullptr) {
        *begin = "";
        *size = 0;
        return;
    }
    for (const char* cursor = start; cursor < end; ++cursor)
        if (*cursor == '/' || *cursor == '\\')
            start = cursor + 1;
    std::size_t length = static_cast<std::size_t>(end - start);
    if (length >= 4 && start[length - 4] == '.' && start[length - 3] == 't' &&
        start[length - 2] == 'x' && start[length - 1] == 't')
        length -= 4;
    *begin = start;
    *size = length;
}

void lower_word(char* dest, std::size_t room, const char* text) noexcept {
    std::size_t index = 0;
    if (text != nullptr) {
        while (text[index] != '\0' && index + 1 < room) {
            dest[index] = lower_letter(text[index]);
            ++index;
        }
    }
    dest[index] = '\0';
}

bool unit_named(const Seat& seat, const char* name) noexcept {
    if (seat.match == nullptr || name == nullptr || name[0] == '\0')
        return false;
    const oa::World& world = seat.match->state();
    if (world.unit_defs == nullptr)
        return false;
    for (uint32_t type_index = 1; type_index < world.unit_def_count; ++type_index)
        if (equal_nocase(world.unit_defs[type_index].unit_name, name))
            return true;
    return false;
}

void push_unit_flag(lua_State* state, const Seat& seat, const char* name) {
    if (seat.match == nullptr) {
        lua_pushnil(state);
        return;
    }
    lua_pushboolean(state, unit_named(seat, name) ? 1 : 0);
}

void push_line(lua_State* state, const Seat& seat, const sim::ai::ProfileLine& line) {
    lua_createtable(state, 0, 8);
    lua_pushinteger(state, static_cast<lua_Integer>(line.number));
    lua_setfield(state, -2, "number");

    lua_createtable(state, static_cast<int>(line.count), 0);
    for (uint32_t index = 0; index < line.count; ++index) {
        const char* word = word_at(line, index);
        lua_pushstring(state, word);
        lua_rawseti(state, -2, static_cast<lua_Integer>(index + 1));
    }
    push_read_only(state, profile_denial);
    lua_setfield(state, -2, "words");

    char command[128];
    lower_word(command, sizeof command, word_at(line, 0));
    lua_pushstring(state, command);
    lua_setfield(state, -2, "command");

    if (same_word(command, "plan")) {
        const bool has_word = line.count >= 2;
        lua_pushboolean(state, has_word && equal_nocase(word_at(line, 1), "any") ? 1 : 0);
        lua_setfield(state, -2, "any");
        lua_pushboolean(state, later_word(line, "easy") ? 1 : 0);
        lua_setfield(state, -2, "easy");
        lua_pushboolean(state, later_word(line, "medium") ? 1 : 0);
        lua_setfield(state, -2, "medium");
        lua_pushboolean(state, later_word(line, "hard") ? 1 : 0);
        lua_setfield(state, -2, "hard");
    } else if (same_word(command, "weight")) {
        const char* name = line.count >= 2 ? word_at(line, 1) : "";
        const char* factor_word = line.count >= 3 ? word_at(line, 2) : "";
        lua_pushstring(state, name);
        lua_setfield(state, -2, "name");
        script_host::push_f32(state, static_cast<float>(std::strtod(factor_word, nullptr)));
        lua_setfield(state, -2, "factor");
        push_unit_flag(state, seat, name);
        lua_setfield(state, -2, "unit");
    } else if (same_word(command, "limit")) {
        const char* name = line.count >= 2 ? word_at(line, 1) : "";
        const char* count_word = line.count >= 3 ? word_at(line, 2) : "";
        lua_pushstring(state, name);
        lua_setfield(state, -2, "name");
        const auto count = static_cast<int32_t>(std::strtol(count_word, nullptr, 10));
        lua_pushinteger(state, static_cast<lua_Integer>(count));
        lua_setfield(state, -2, "count");
        push_unit_flag(state, seat, name);
        lua_setfield(state, -2, "unit");
    }
    push_read_only(state, profile_denial);
}

struct LineBuild {
    lua_State* state{};
    Seat* seat{};
    int lines{};
    uint32_t words{};
};

void visit_line(void* context, const sim::ai::ProfileLine& line) {
    auto* build = static_cast<LineBuild*>(context);
    if (build == nullptr || build->state == nullptr || build->seat == nullptr)
        return;
    push_line(build->state, *build->seat, line);
    const lua_Integer slot = luaL_len(build->state, build->lines) + 1;
    lua_rawseti(build->state, build->lines, slot);
    build->words += line.count;
}

/// One pass over the profile's lines for one difficulty keyword.
///
/// The plan flag starts matched. A plan line replaces it. Weight and limit
/// lines are kept only while it is matched. The flag is not kept after this
/// call, and a downloadable type's own lines are not added: the built-in
/// player does both, and a brain that wants them does them itself.
extern "C" int profile_select(lua_State* state) {
    const char* keyword = luaL_checkstring(state, 2);
    const char* field = nullptr;
    if (same_word(keyword, "easy"))
        field = "easy";
    else if (same_word(keyword, "medium"))
        field = "medium";
    else if (same_word(keyword, "hard"))
        field = "hard";
    else
        return luaL_error(state, "select expects easy, medium or hard");

    lua_getfield(state, 1, "lines");
    const int lines = lua_absindex(state, -1);
    lua_newtable(state);
    const int result = lua_absindex(state, -1);
    lua_newtable(state);
    const int weights = lua_absindex(state, -1);
    lua_newtable(state);
    const int limits = lua_absindex(state, -1);

    bool matched = true;
    lua_Integer weight_count = 0;
    lua_Integer limit_count = 0;
    const lua_Integer count = luaL_len(state, lines);
    for (lua_Integer slot = 1; slot <= count; ++slot) {
        lua_geti(state, lines, slot);
        const int row = lua_absindex(state, -1);
        lua_getfield(state, row, "command");
        const char* command = lua_tostring(state, -1);
        lua_pop(state, 1);
        if (command != nullptr && same_word(command, "plan")) {
            lua_getfield(state, row, "any");
            const bool any = lua_toboolean(state, -1) != 0;
            lua_pop(state, 1);
            lua_getfield(state, row, field);
            const bool named = lua_toboolean(state, -1) != 0;
            lua_pop(state, 1);
            matched = any || named;
            lua_pop(state, 1);
        } else if (matched && command != nullptr && same_word(command, "weight")) {
            ++weight_count;
            lua_rawseti(state, weights, weight_count);
        } else if (matched && command != nullptr && same_word(command, "limit")) {
            ++limit_count;
            lua_rawseti(state, limits, limit_count);
        } else {
            lua_pop(state, 1);
        }
    }
    lua_setfield(state, result, "limits");
    lua_setfield(state, result, "weights");
    lua_remove(state, lines);
    return 1;
}

bool downloads_allowed(match_runtime::Match& match) noexcept {
    const std::shared_ptr<void>& slot = match.computer_player_state();
    if (!slot)
        return true;
    const auto* players = static_cast<const sim::ai::ComputerPlayers*>(slot.get());
    return players->downloadables_restricted == 0;
}

uint32_t list_cap(const Seat& seat) noexcept {
    if (seat.match != nullptr) {
        const uint32_t kept = data::limits::build_list_kept(seat.match->limits().build_lists) + 1;
        return kept < list_room ? kept : list_room;
    }
    if (seat.calls.build_kept != 0 && seat.calls.build_kept < list_room)
        return seat.calls.build_kept;
    return list_room;
}

uint32_t collect_run_ids(
    const uint16_t* runs,
    uint32_t run_length,
    uint32_t type_count,
    uint16_t unit_type,
    bool builder,
    uint16_t* out,
    uint32_t cap
) noexcept {
    if (runs == nullptr || out == nullptr || unit_type == 0 || unit_type >= type_count || !builder)
        return 0;
    const uint16_t* run = runs;
    const uint16_t* const end = run + run_length;
    for (uint32_t type_index = 0; type_index < type_count && run < end; ++type_index) {
        const uint32_t count = *run++;
        const auto present = static_cast<uint32_t>(end - run);
        const uint32_t length = count < present ? count : present;
        if (type_index == unit_type) {
            uint32_t kept = 0;
            for (uint32_t slot = 0; slot < length && kept < cap; ++slot) {
                if (run[slot] != 0 && run[slot] < type_count)
                    out[kept++] = run[slot];
            }
            return kept;
        }
        run += length;
    }
    return 0;
}

uint32_t collect_build_ids(
    match_runtime::Match& match, uint16_t unit_type, uint16_t* out, uint32_t cap
) noexcept {
    sim::ai::ComputerPlayers* players = sim::ai::match_computer_players(match);
    if (players == nullptr || players->types == nullptr || players->build_list_runs == nullptr)
        return 0;
    if (unit_type == 0 || unit_type >= players->type_count)
        return 0;
    if ((players->types[unit_type].flags & OA_UNIT_DEF_FLAG_BUILDER) == 0)
        return 0;
    const uint16_t* run = players->build_list_runs;
    const uint16_t* const end = run + players->build_list_run_length;
    for (uint32_t type_index = 0; type_index < players->type_count && run < end; ++type_index) {
        const uint32_t count = *run++;
        const auto present = static_cast<uint32_t>(end - run);
        const uint32_t length = count < present ? count : present;
        if (type_index == unit_type) {
            uint32_t kept = 0;
            for (uint32_t index = 0; index < length && kept < cap; ++index) {
                if (run[index] != 0 && run[index] < players->type_count)
                    out[kept++] = run[index];
            }
            return kept;
        }
        run += length;
    }
    return 0;
}

extern "C" {

int game_ai_profiles(lua_State* state) {
    Seat* seat = seat_of(state);
    if (seat == nullptr)
        return luaL_error(state, "ai_profiles has no seat");
    static const oa::data::brain_facts::GameFacts fallback{};
    const oa::data::brain_facts::GameFacts* facts =
        seat->facts != nullptr ? seat->facts : &fallback;
    lua_createtable(state, static_cast<int>(facts->ai_profiles.size()), 0);
    const int list = lua_gettop(state);
    for (std::size_t index = 0; index < facts->ai_profiles.size(); ++index) {
        const oa::data::brain_facts::ProfileFile& file = facts->ai_profiles[index];
        push_profile(state, *seat, file.path, file.text, false, {});
        lua_rawseti(state, list, static_cast<lua_Integer>(index + 1));
    }
    return 1;
}

int game_parse_profile(lua_State* state) {
    Seat* seat = seat_of(state);
    if (seat == nullptr)
        return luaL_error(state, "parse_profile has no seat");
    std::size_t length = 0;
    const char* text = luaL_checklstring(state, 1, &length);
    push_profile(state, *seat, {}, std::string_view(text, length), false, {});
    return 1;
}

int game_build_list(lua_State* state) {
    Seat* seat = seat_of(state);
    const bool tool_runs =
        seat != nullptr && seat->match == nullptr && seat->calls.build_runs != nullptr;
    if (seat == nullptr || (seat->match == nullptr && !tool_runs)) {
        lua_pushnil(state);
        return 1;
    }
    uint16_t unit_type = 0;
    if (!resolve_unit_type(state, 1, "build_list", &unit_type))
        return 0;
    uint16_t ids[list_room];
    const uint32_t cap = list_cap(*seat);
    uint32_t kept = 0;
    if (seat->match != nullptr) {
        kept = collect_build_ids(*seat->match, unit_type, ids, cap);
    } else {
        const oa::World* world = seat->calls.host.world;
        const uint32_t type_count = world != nullptr ? world->unit_def_count : 0;
        bool builder = false;
        if (world != nullptr && world->unit_defs != nullptr && unit_type < type_count)
            builder = (world->unit_defs[unit_type].flags & OA_UNIT_DEF_FLAG_BUILDER) != 0;
        kept = collect_run_ids(
            seat->calls.build_runs,
            seat->calls.build_run_length,
            type_count,
            unit_type,
            builder,
            ids,
            cap
        );
    }
    lua_getglobal(state, "units");
    const int units = lua_gettop(state);
    lua_createtable(state, static_cast<int>(kept), 0);
    const int list = lua_gettop(state);
    for (uint32_t index = 0; index < kept; ++index) {
        lua_geti(state, units, static_cast<lua_Integer>(ids[index]));
        lua_rawseti(state, list, static_cast<lua_Integer>(index + 1));
    }
    lua_remove(state, units);
    return 1;
}

int brain_strengths(lua_State* state) {
    script_host::charge_work(state, 2);
    Seat* seat = seat_of(state);
    if (seat == nullptr || seat->match == nullptr) {
        lua_pushnil(state);
        return 1;
    }
    uint16_t unit_type = 0;
    if (!resolve_unit_type(state, 1, "strengths", &unit_type))
        return 0;
    if (seat->player >= OA_PLAYER_COUNT) {
        lua_pushnil(state);
        return 1;
    }
    if (seat->calls.host.strengths != nullptr) {
        const uint8_t* triple = nullptr;
        try {
            triple = seat->calls.host.strengths(seat->calls.host.context, seat->player, unit_type);
        } catch (...) {
            triple = nullptr;
        }
        if (triple == nullptr) {
            lua_pushnil(state);
            return 1;
        }
        for (std::size_t index = 0; index < 3; ++index) {
            const auto stored = static_cast<int8_t>(triple[index]);
            lua_pushinteger(state, static_cast<lua_Integer>(stored));
        }
        return 3;
    }
    const auto& table = seat->match->strategic_state(seat->player).strengths;
    if (unit_type >= table.size()) {
        lua_pushnil(state);
        return 1;
    }
    const std::array<uint8_t, 3>& triple = table[unit_type];
    for (std::size_t index = 0; index < triple.size(); ++index) {
        const auto stored = static_cast<int8_t>(triple[index]);
        lua_pushinteger(state, static_cast<lua_Integer>(stored));
    }
    return 3;
}

} // extern "C"

} // namespace

void push_profile(
    lua_State* state,
    Seat& seat,
    std::string_view path,
    std::string_view text,
    bool with_report,
    std::string_view report_path
) {
    lua_createtable(state, 0, 8);
    const int hidden = lua_gettop(state);
    if (!path.empty() && path.data() != nullptr) {
        lua_pushlstring(state, path.data(), path.size());
        lua_setfield(state, hidden, "path");
        const char* name = nullptr;
        std::size_t name_size = 0;
        profile_name(path, &name, &name_size);
        lua_pushlstring(state, name, name_size);
        lua_setfield(state, hidden, "name");
    }
    const bool too_large = text.size() > script_host::string_length_limit;
    if (too_large)
        lua_pushnil(state);
    else if (text.data() == nullptr)
        lua_pushlstring(state, "", 0);
    else
        lua_pushlstring(state, text.data(), text.size());
    lua_setfield(state, hidden, "text");
    lua_pushboolean(state, too_large ? 1 : 0);
    lua_setfield(state, hidden, "too_large");
    if (with_report) {
        if (report_path.empty() || report_path.data() == nullptr)
            lua_pushnil(state);
        else
            lua_pushlstring(state, report_path.data(), report_path.size());
        lua_setfield(state, hidden, "report_path");
    }

    lua_newtable(state);
    const int lines = lua_gettop(state);
    LineBuild build;
    build.state = state;
    build.seat = &seat;
    build.lines = lines;
    build.words = 0;
    sim::ai::computer_profile_lines(text, visit_line, &build);
    push_read_only(state, profile_denial);
    lua_setfield(state, hidden, "lines");
    lua_pushcfunction(state, profile_select);
    lua_setfield(state, hidden, "select");
    script_host::charge_work(
        state,
        script_host::units_for_bytes(text.size()) +
            script_host::element_charge * static_cast<uint64_t>(build.words)
    );
    push_read_only(state, profile_denial);
}

void install_profile(lua_State* state, Seat& seat) {
    static const oa::data::brain_facts::GameFacts fallback{};
    const oa::data::brain_facts::GameFacts* facts = seat.facts != nullptr ? seat.facts : &fallback;
    if (facts->ai_profile.path.empty())
        lua_pushnil(state);
    else
        push_profile(
            state,
            seat,
            facts->ai_profile.path,
            facts->ai_profile.text,
            true,
            facts->ai_profile_report_path
        );
    set_game_field(state, seat, "ai_profile");

    lua_pushcfunction(state, game_ai_profiles);
    set_game_field(state, seat, "ai_profiles");
    lua_pushcfunction(state, game_parse_profile);
    set_game_field(state, seat, "parse_profile");

    lua_getglobal(state, "brain");
    if (lua_type(state, -1) != LUA_TTABLE) {
        lua_pop(state, 1);
        lua_newtable(state);
    }
    if (seat.match == nullptr && seat.calls.build_runs == nullptr) {
        lua_pushnil(state);
        set_game_field(state, seat, "build_list");
        lua_pushnil(state);
        set_game_field(state, seat, "build_list_cap");
        lua_pushnil(state);
        set_game_field(state, seat, "downloadables_allowed");
        lua_pushnil(state);
        lua_setfield(state, -2, "strengths");
        lua_setglobal(state, "brain");
        return;
    }

    lua_pushcfunction(state, game_build_list);
    set_game_field(state, seat, "build_list");
    uint32_t cap = list_room;
    if (seat.match != nullptr)
        cap = data::limits::build_list_kept(seat.match->limits().build_lists) + 1;
    else if (seat.calls.build_kept != 0)
        cap = seat.calls.build_kept;
    lua_pushinteger(state, static_cast<lua_Integer>(cap));
    set_game_field(state, seat, "build_list_cap");
    if (seat.match == nullptr) {
        lua_pushnil(state);
        set_game_field(state, seat, "downloadables_allowed");
        lua_pushnil(state);
        lua_setfield(state, -2, "strengths");
        lua_setglobal(state, "brain");
        return;
    }
    lua_pushboolean(state, downloads_allowed(*seat.match) ? 1 : 0);
    set_game_field(state, seat, "downloadables_allowed");
    lua_pushcfunction(state, brain_strengths);
    lua_setfield(state, -2, "strengths");
    lua_setglobal(state, "brain");
}

void refresh_ai_profile(lua_State* state, Seat& seat) {
    if (state == nullptr || seat.match == nullptr)
        return;
    const oa::sim::ai::ComputerPlayers* players = oa::sim::ai::match_computer_players(*seat.match);
    if (players == nullptr || players->profile_text == nullptr)
        return;
    static const oa::data::brain_facts::GameFacts fallback{};
    const oa::data::brain_facts::GameFacts* facts = seat.facts != nullptr ? seat.facts : &fallback;
    const std::string_view text(players->profile_text, players->profile_length);
    push_profile(state, seat, facts->ai_profile.path, text, true, facts->ai_profile_report_path);
    set_game_field(state, seat, "ai_profile");
}

} // namespace oa::sim::brains

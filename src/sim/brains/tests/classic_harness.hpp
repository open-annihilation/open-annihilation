// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A Classic seat on the computer-player fake, beside the built-in player.
// The built-in player and Classic each keep a log of section markers and of
// the squad assigns that happen while the sort section is running.
#pragma once

#include "oa/sim/brains/seat.hpp"
#include "oa/sim/match_runtime/seat_hooks.hpp"

#include "oa/data/script_host/f32.hpp"
#include "oa/data/unit_definitions.hpp"
#include "oa/test/computer_fake.hpp"

#include "lua.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace oa::test::classic_harness {

/// One squad a sort section assigned, and the unit slot it went to.
struct Assign {
    /// The unit slot the assign named.
    uint16_t slot{};
    /// The squad the unit joined.
    int32_t squad{};
};

/// Markers and sort assigns recorded for one tick.
struct TickLog {
    /// True while the section being recorded is the sort.
    bool in_sort{};
    /// Section names, in the order they were marked.
    std::vector<std::string> markers;
    /// Squad assigns that happened during the sort section.
    std::vector<Assign> assigns;
};

/// The log the built-in player's section callback writes.
inline TickLog* active_builtin = nullptr;
/// The log the Classic section callback writes.
inline TickLog* active_classic = nullptr;
/// The set-up record the scripted stream copies draws into. Null records nothing.
inline oa::sim::match_runtime::SetUpRecord* recording_setup = nullptr;

/// The section name a marker index stands for.
inline const char* section_word(uint8_t section) noexcept {
    switch (section) {
    case 0:
        return "sort";
    case 1:
        return "task1";
    case 2:
        return "task2";
    case 3:
        return "task3";
    case 4:
        return "task4";
    case 5:
        return "task5";
    case 6:
        return "task6";
    case 7:
        return "task7";
    case 8:
        return "task8";
    case 9:
        return "task9";
    case 10:
        return "start";
    default:
        return "other";
    }
}

/// Records one section name and whether it is the sort section.
inline void remember_marker(TickLog* log, const char* name) {
    if (log == nullptr || name == nullptr)
        return;
    log->in_sort = std::strcmp(name, "sort") == 0;
    log->markers.emplace_back(name);
}

/// Records a section marker from the built-in player.
inline void note_builtin_section(void*, uint8_t, uint8_t section) {
    remember_marker(active_builtin, section_word(section));
}

/// Records a section marker from Classic.
inline void note_classic_section(void*, const char* name) {
    remember_marker(active_classic, name);
}

/// Assigns a squad on the built-in player's fake, and records it during a sort.
inline void set_squad_builtin(void* context, uint16_t slot, oa::sim::ai::Squad squad) {
    if (active_builtin != nullptr && active_builtin->in_sort) {
        Assign row;
        row.slot = slot;
        row.squad = static_cast<int32_t>(squad);
        active_builtin->assigns.push_back(row);
    }
    computer_fake::fake_of(context)->set_squad(slot, static_cast<int>(squad));
}

/// Assigns a squad on Classic's fake, and records it during a sort.
inline void set_squad_classic(void* context, uint16_t slot, oa::sim::ai::Squad squad) {
    if (active_classic != nullptr && active_classic->in_sort) {
        Assign row;
        row.slot = slot;
        row.squad = static_cast<int32_t>(squad);
        active_classic->assigns.push_back(row);
    }
    computer_fake::fake_of(context)->set_squad(slot, static_cast<int>(squad));
}

/// The scripted stream, and a copy of each draw whose bound is at least 2.
inline uint32_t capture_random(void* context, uint32_t bound) {
    auto* fake = computer_fake::fake_of(context);
    if (bound < 2)
        return 0;
    const uint32_t rolled = fake->roll < fake->rolls.size() ? fake->rolls[fake->roll] : 0u;
    ++fake->roll;
    const uint32_t value = rolled % bound;
    if (recording_setup != nullptr && recording_setup->draw_count < recording_setup->draws.size()) {
        auto& draw = recording_setup->draws[recording_setup->draw_count];
        draw.bound = bound;
        draw.value = value;
        recording_setup->draw_count = static_cast<uint8_t>(recording_setup->draw_count + 1u);
    }
    return value;
}

/// Clears one tick's markers and assigns.
inline void clear_log(TickLog& log) {
    log.in_sort = false;
    log.markers.clear();
    log.assigns.clear();
}

/// Clears both logs and points the section callbacks at them.
inline void watch(TickLog& builtin, TickLog& classic) {
    clear_log(builtin);
    clear_log(classic);
    active_builtin = &builtin;
    active_classic = &classic;
}

/// The standing move order stored in a unit's flags.
inline uint32_t standing_move(const oa::Unit& unit) noexcept {
    return (unit.flags & OA_UNIT_FLAG_MOVE_ORDER_MASK) >> OA_UNIT_FLAG_MOVE_ORDER_SHIFT;
}

/// The standing fire order stored in a unit's flags.
inline uint32_t standing_fire(const oa::Unit& unit) noexcept {
    return (unit.flags & OA_UNIT_FLAG_FIRE_ORDER_MASK) >> OA_UNIT_FLAG_FIRE_ORDER_SHIFT;
}

/// The standing move and fire bits stored in a unit's flags.
inline uint32_t standing_bits(const oa::Unit& unit) noexcept {
    return unit.flags & (OA_UNIT_FLAG_MOVE_ORDER_MASK | OA_UNIT_FLAG_FIRE_ORDER_MASK);
}

/// Writes each unit's slot into its id, so a handle's slot is that id.
inline void stamp_ids(computer_fake::Fake& fake) {
    for (std::size_t slot = 0; slot < fake.units.size(); ++slot)
        fake.units[slot].id = static_cast<uint16_t>(slot);
}

/// Copies text into a fixed buffer and stops at the first NUL.
inline void copy_text(char* dest, std::size_t room, const char* source) {
    std::size_t index = 0;
    if (source != nullptr) {
        while (index + 1 < room && source[index] != '\0') {
            dest[index] = source[index];
            ++index;
        }
    }
    if (room > 0)
        dest[index] = '\0';
}

/// Copies the catalog onto the fake's unit defs and a parallel definition
/// list, so the seat's type table reads the same numbers.
inline void copy_catalog(
    computer_fake::Fake& fake,
    const oa::sim::ai::ComputerPlayers& state,
    std::vector<oa::data::unit_definitions::UnitDefinition>& definitions
) {
    const uint32_t count = fake.world.unit_def_count;
    definitions.assign(count, oa::data::unit_definitions::UnitDefinition{});
    for (uint32_t index = 0; index < count && index < state.type_count; ++index) {
        const oa::sim::ai::ComputerType& source = state.types[index];
        oa::UnitDef& unit = fake.defs[index];
        copy_text(unit.unit_name, sizeof unit.unit_name, source.unit_name);
        copy_text(unit.side, sizeof unit.side, source.side);
        copy_text(unit.ai_weight, sizeof unit.ai_weight, source.ai_directives);
        unit.flags = source.flags;
        unit.abilities = source.abilities;
        unit.bm_code = source.bm_code;
        unit.makes_metal = source.makes_metal;
        unit.min_water_depth = source.min_water_depth;
        unit.max_water_depth = source.max_water_depth;
        unit.energy_use = source.energy_use;
        unit.footprint_x = source.footprint_x;
        unit.footprint_z = source.footprint_z;
        unit.extracts_metal = source.extracts_metal;

        oa::data::unit_definitions::UnitDefinition& defined = definitions[index];
        defined.unit_name = source.unit_name;
        defined.side = source.side;
        defined.categories.clear();
        std::string word;
        for (const char* cursor = source.categories; *cursor != '\0'; ++cursor) {
            if (*cursor == ' ') {
                if (!word.empty()) {
                    defined.categories.push_back(word);
                    word.clear();
                }
            } else {
                word.push_back(*cursor);
            }
        }
        if (!word.empty())
            defined.categories.push_back(std::move(word));
    }
}

/// The package's paths and bytes, and the file list a seat loads.
struct PackageBytes {
    std::vector<std::string> paths;
    std::vector<std::vector<uint8_t>> bytes;
    std::vector<oa::sim::brains::PackageFile> files;

    /// Rebuilds the file list from the stored paths and bytes.
    void bind() {
        files.clear();
        files.reserve(paths.size());
        for (std::size_t index = 0; index < paths.size(); ++index) {
            files.push_back(
                {paths[index], std::span<const uint8_t>(bytes[index].data(), bytes[index].size())}
            );
        }
    }

    /// Reads every file under a package folder, leaving out `.oa-origin.yaml`.
    void load(const std::filesystem::path& root) {
        paths.clear();
        bytes.clear();
        for (const std::filesystem::directory_entry& entry :
             std::filesystem::recursive_directory_iterator(root)) {
            if (!entry.is_regular_file())
                continue;
            const std::string relative =
                std::filesystem::relative(entry.path(), root).generic_string();
            if (relative == ".oa-origin.yaml")
                continue;
            std::ifstream input(entry.path(), std::ios::binary);
            if (!input)
                throw std::runtime_error("could not read " + relative);
            std::vector<uint8_t> data(
                (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>()
            );
            paths.push_back(relative);
            bytes.push_back(std::move(data));
        }
        bind();
    }
};

/// Refreshes one player's knowledge when thirty ticks have passed since the last.
inline void
refresh_knowledge(oa::sim::ai::ComputerPlayers& state, computer_fake::Fake& fake, uint8_t index) {
    constexpr uint32_t refresh_period = 30;
    auto* knowledge = oa::sim::ai::computer_player_knowledge(&state, index);
    const auto turn = fake.world.game.tick;
    if (knowledge == nullptr || fake.knowledge_tick[index] + refresh_period > turn)
        return;
    oa::sim::ai::computer_knowledge_clear(&state, *knowledge);
    oa::sim::ai::KnowledgeTally tally;
    for (const auto& unit : fake.units) {
        if ((unit.flags & OA_UNIT_FLAG_LIVE) != 0 && unit.owner_index == index &&
            unit.build_remaining == 0.0F)
            oa::sim::ai::computer_knowledge_count(&state, *knowledge, unit, tally);
    }
    oa::sim::ai::computer_knowledge_settle(*knowledge, tally);
    fake.knowledge_tick[index] = turn;
}

/// Runs the built-in order tick for every computer player, then refreshes knowledge.
inline void tick_players(
    oa::sim::ai::ComputerPlayers& state,
    computer_fake::Fake& fake,
    const oa::sim::ai::ComputerHost& host
) {
    for (uint8_t index = 0; index < OA_PLAYER_COUNT; ++index) {
        const auto& player = host.world->game.players[index];
        if (player.in_use == 0)
            continue;
        if (oa::sim::ai::player_has_controller(player) &&
            player.status == OA_PLAYER_STATUS_COMPUTER)
            oa::sim::ai::computer_player_tick_orders(&state, host, index);
        refresh_knowledge(state, fake, index);
    }
}

/// One value read from the seat table by a dotted path.
struct PathRead {
    const char* path{};
    bool ok{};
    bool whole{};
    bool text{};
    lua_Integer number{};
    char words[64]{};
};

/// Walks a dotted path on the seat table and stores the value it finds.
inline void read_path_work(void* context, lua_State* state) {
    auto* box = static_cast<PathRead*>(context);
    if (box == nullptr || box->path == nullptr || lua_type(state, -1) != LUA_TTABLE)
        return;
    const char* cursor = box->path;
    while (*cursor != '\0') {
        if (lua_type(state, -1) != LUA_TTABLE)
            return;
        if (*cursor == '[') {
            ++cursor;
            lua_Integer key = 0;
            while (*cursor >= '0' && *cursor <= '9') {
                key = key * 10 + static_cast<lua_Integer>(*cursor - '0');
                ++cursor;
            }
            if (*cursor == ']')
                ++cursor;
            lua_rawgeti(state, -1, key);
        } else {
            char name[64]{};
            std::size_t length = 0;
            while (*cursor != '\0' && *cursor != '.' && *cursor != '[' &&
                   length + 1 < sizeof name) {
                name[length] = *cursor;
                ++length;
                ++cursor;
            }
            name[length] = '\0';
            lua_getfield(state, -1, name);
        }
        if (*cursor == '.')
            ++cursor;
        lua_remove(state, -2);
    }
    if (lua_isinteger(state, -1) != 0) {
        box->ok = true;
        box->whole = true;
        box->number = lua_tointeger(state, -1);
        return;
    }
    if (lua_type(state, -1) == LUA_TSTRING) {
        std::size_t length = 0;
        const char* text = lua_tolstring(state, -1, &length);
        box->ok = true;
        box->text = true;
        const std::size_t count = length < 63 ? length : 63;
        if (text != nullptr) {
            for (std::size_t index = 0; index < count; ++index)
                box->words[index] = text[index];
        }
        box->words[count] = '\0';
    }
}

/// Reads one dotted path from the seated hooks table.
inline PathRead read_path(oa::sim::brains::ToolSeat& seat, const char* path) {
    PathRead box;
    box.path = path;
    oa::data::script_host::ScriptError error;
    if (!seat.with_seat(&box, read_path_work, error))
        throw std::runtime_error(oa::data::script_host::describe(error));
    return box;
}

/// The fields a direct squad-sort call is given.
struct SortAsk {
    bool building{};
    bool armed{};
    uint32_t flags{};
    bool has_energy{};
    float energy{};
    int32_t min_water{};
    int32_t max_water{};
    const char* rules{};
    bool ok{};
    lua_Integer squad{};
};

/// Calls squads.sort_squad with the fields in the ask.
inline void call_sort_work(void* context, lua_State* state) {
    auto* ask = static_cast<SortAsk*>(context);
    lua_getglobal(state, "require");
    lua_pushliteral(state, "squads");
    if (lua_pcall(state, 1, 1, 0) != LUA_OK)
        return;
    lua_getfield(state, -1, "sort_squad");
    lua_newtable(state);
    lua_pushboolean(state, ask->building ? 1 : 0);
    lua_setfield(state, -2, "building");
    lua_pushboolean(state, ask->armed ? 1 : 0);
    lua_setfield(state, -2, "armed");
    lua_pushinteger(state, 0);
    lua_setfield(state, -2, "squad");
    lua_newtable(state);
    lua_pushinteger(state, static_cast<lua_Integer>(ask->flags));
    lua_setfield(state, -2, "flags");
    if (ask->has_energy)
        oa::data::script_host::push_f32(state, ask->energy);
    else
        lua_pushnil(state);
    lua_setfield(state, -2, "energy_use");
    lua_pushinteger(state, static_cast<lua_Integer>(ask->min_water));
    lua_setfield(state, -2, "min_water_depth");
    lua_pushinteger(state, static_cast<lua_Integer>(ask->max_water));
    lua_setfield(state, -2, "max_water_depth");
    lua_pushstring(state, ask->rules != nullptr ? ask->rules : "base");
    if (lua_pcall(state, 3, 1, 0) != LUA_OK)
        return;
    if (lua_isinteger(state, -1) == 0)
        return;
    ask->ok = true;
    ask->squad = lua_tointeger(state, -1);
}

/// Returns the squad squads.sort_squad chooses for these fields.
inline lua_Integer sort_squad(oa::sim::brains::ToolSeat& seat, const SortAsk& fields) {
    SortAsk ask = fields;
    oa::data::script_host::ScriptError error;
    if (!seat.host("sort", &ask, call_sort_work, error))
        throw std::runtime_error(oa::data::script_host::describe(error));
    if (!ask.ok)
        throw std::runtime_error("sort_squad did not return a squad");
    return ask.squad;
}

/// One set-up draw asked of the seated brain.
struct SetupAsk {
    lua_Integer bound{};
    bool ok{};
    lua_Integer value{};
};

/// Calls brain.setup_random for the bound in the ask.
inline void call_setup_work(void* context, lua_State* state) {
    auto* ask = static_cast<SetupAsk*>(context);
    lua_getglobal(state, "brain");
    lua_getfield(state, -1, "setup_random");
    lua_pushinteger(state, ask->bound);
    if (lua_pcall(state, 1, 1, 0) != LUA_OK)
        return;
    if (lua_isinteger(state, -1) == 0)
        return;
    ask->ok = true;
    ask->value = lua_tointeger(state, -1);
}

/// Returns the integer brain.setup_random draws for this bound.
inline lua_Integer setup_draw(oa::sim::brains::ToolSeat& seat, lua_Integer bound) {
    SetupAsk ask;
    ask.bound = bound;
    oa::data::script_host::ScriptError error;
    if (!seat.host("setup", &ask, call_setup_work, error))
        throw std::runtime_error(oa::data::script_host::describe(error));
    if (!ask.ok)
        throw std::runtime_error("setup_random did not return an integer");
    return ask.value;
}

/// The Lua type of brain.shared_random.
struct KindAsk {
    int kind{LUA_TNIL};
};

/// Reads the Lua type of brain.shared_random.
inline void call_shared_kind(void* context, lua_State* state) {
    auto* ask = static_cast<KindAsk*>(context);
    lua_getglobal(state, "brain");
    lua_getfield(state, -1, "shared_random");
    ask->kind = lua_type(state, -1);
}

/// Returns the Lua type of brain.shared_random, or nil when it is absent.
inline int shared_random_kind(oa::data::script_host::Sandbox& sandbox) {
    KindAsk ask;
    oa::data::script_host::ScriptError error;
    if (!sandbox.run_host("shared", &ask, call_shared_kind, error))
        throw std::runtime_error(oa::data::script_host::describe(error));
    return ask.kind;
}

/// Optional facts a tool seat carries beside the package. Null leaves the
/// medium Classic spec, with no build-list runs and no knowledge record.
struct ClassicBind {
    /// When set, the seat's player index. Otherwise player 1.
    bool use_player{};
    /// The player index `use_player` selects.
    uint8_t player{};
    /// When set, the seat's difficulty number. Otherwise medium.
    bool use_difficulty{};
    /// OA_DIFFICULTY_EASY, MEDIUM or HARD.
    int32_t difficulty{};
    /// When set, the level id. Otherwise "medium".
    bool use_level{};
    /// The level id. A literal, alive for the open call.
    const char* level{};
    /// Build-list runs for a seat with no match: a count and that many ids,
    /// from type 0. Null leaves `build_list` nil.
    const uint16_t* build_runs{};
    /// How many `uint16_t` values `build_runs` holds.
    uint32_t build_run_length{};
    /// Entries a builder's list keeps, including the download entry. 0 keeps
    /// the longest list.
    uint32_t build_kept{};
    /// The fake `host.context` points at, when this seat reads a knowledge record.
    computer_fake::Fake* knowledge_fake{};
    /// The built-in players whose record `me.knowledge` reads. Null leaves it nil.
    oa::sim::ai::ComputerPlayers* knowledge_players{};
};

/// The fake and the built-in players one knowledge callback reads.
///
/// A tool seat's context stays the fake. The callback is keyed by that fake,
/// so two seats on one fake share one record.
struct KnowledgeLink {
    /// The fake the host context must point at.
    computer_fake::Fake* fake{};
    /// The players that hold the record.
    oa::sim::ai::ComputerPlayers* players{};
};

/// The record the open seat's knowledge callback reads. Cleared when a seat
/// opens without one.
inline KnowledgeLink knowledge_link{};

/// Returns the built-in record for the fake this host plays, or null.
inline const oa::sim::ai::ComputerKnowledge*
fake_knowledge(void* context, uint8_t player, uint32_t* refreshed) {
    if (context == nullptr || knowledge_link.players == nullptr || knowledge_link.fake == nullptr)
        return nullptr;
    const auto* host = static_cast<const oa::sim::ai::ComputerHost*>(context);
    if (host->context == nullptr || computer_fake::fake_of(host->context) != knowledge_link.fake)
        return nullptr;
    if (refreshed != nullptr && player < 10)
        *refreshed = knowledge_link.fake->knowledge_tick[player];
    return oa::sim::ai::computer_player_knowledge(knowledge_link.players, player);
}

/// The seat spec that loads this package as Classic.
inline oa::sim::brains::SeatSpec classic_spec(const PackageBytes& package) {
    oa::sim::brains::SeatSpec spec;
    spec.player = 1;
    spec.id = "classic";
    spec.name = "Classic";
    spec.version = "1.0";
    spec.entry = "brain.lua";
    spec.level = "medium";
    spec.difficulty = OA_DIFFICULTY_MEDIUM;
    spec.full_awareness = true;
    spec.files = package.files;
    return spec;
}

/// Opens a tool seat on this package, the host's calls and the given facts.
///
/// `bind` adds the player, the difficulty, build-list runs and the knowledge
/// record. Null leaves those unset. The runs and the players must outlive the
/// seat. Facts are copied into the seat.
inline oa::sim::brains::ToolSeat open_classic(
    const PackageBytes& package,
    const oa::sim::ai::ComputerHost& host,
    const oa::data::brain_facts::GameFacts& facts,
    const std::vector<oa::data::unit_definitions::UnitDefinition>& definitions,
    const oa::sim::match_runtime::SetUpRecord* setup,
    const ClassicBind* bind = nullptr
) {
    oa::sim::brains::ToolSeatSpec spec;
    spec.spec = classic_spec(package);
    spec.calls = oa::sim::brains::seat_calls_from_computer_host(host);
    spec.calls.section = note_classic_section;
    knowledge_link = {};
    if (bind != nullptr) {
        if (bind->use_player)
            spec.spec.player = bind->player;
        if (bind->use_difficulty)
            spec.spec.difficulty = bind->difficulty;
        if (bind->use_level && bind->level != nullptr)
            spec.spec.level = bind->level;
        spec.calls.build_runs = bind->build_runs;
        spec.calls.build_run_length = bind->build_run_length;
        spec.calls.build_kept = bind->build_kept;
        if (bind->knowledge_fake != nullptr && bind->knowledge_players != nullptr) {
            knowledge_link.fake = bind->knowledge_fake;
            knowledge_link.players = bind->knowledge_players;
            spec.calls.knowledge = fake_knowledge;
        }
    }
    spec.facts = facts;
    if (!definitions.empty()) {
        spec.definitions = definitions.data();
        spec.definition_count = definitions.size();
    }
    spec.setup = setup;
    oa::data::script_host::ScriptError error;
    std::optional<oa::sim::brains::ToolSeat> opened = oa::sim::brains::ToolSeat::open(spec, error);
    if (!opened)
        throw std::runtime_error(oa::data::script_host::describe(error));
    return std::move(*opened);
}

/// Runs one Classic tick and throws when the script fails.
inline void play_classic(oa::sim::brains::ToolSeat& seat, uint32_t turn) {
    oa::data::script_host::ScriptError error;
    if (!seat.tick(turn, error))
        throw std::runtime_error(oa::data::script_host::describe(error));
}

/// Failure text from a later `hooks.start`, copied off the Lua stack.
struct LaterStart {
    /// True when `hooks.start` did not run.
    bool failed{};
    /// The Lua error, or why the hooks table could not be called.
    char message[192]{};
};

/// Copies `text` into `box` and marks the later start failed.
inline void note_later_start(LaterStart* box, const char* text) {
    if (box == nullptr || box->failed)
        return;
    box->failed = true;
    std::size_t index = 0;
    if (text != nullptr) {
        while (index + 1 < sizeof box->message && text[index] != '\0') {
            box->message[index] = text[index];
            ++index;
        }
    }
    box->message[index] = '\0';
}

/// Calls `hooks.start(game, me, saved)` on the hooks table the seat loaded.
///
/// `with_seat` leaves that table under `hooks.seat`. `require` only finds
/// `lua/<name>.lua`, and the entry sits at the package root, so this is the
/// call a later open uses. `game` and `me` are the values the seat holds.
/// This harness has no saved bytes, so `saved` is nil.
inline void start_later_work(void* context, lua_State* state) {
    auto* box = static_cast<LaterStart*>(context);
    const int seat = lua_gettop(state);
    if (seat < 2 || lua_type(state, seat) != LUA_TTABLE ||
        lua_type(state, seat - 1) != LUA_TTABLE) {
        note_later_start(box, "the seated hooks are missing");
        return;
    }
    lua_getfield(state, seat - 1, "start");
    if (lua_type(state, -1) != LUA_TFUNCTION) {
        note_later_start(box, "hooks.start is missing");
        return;
    }
    lua_getfield(state, seat, "game");
    lua_getfield(state, seat, "me");
    lua_pushnil(state);
    if (lua_pcall(state, 3, 0, 0) != LUA_OK)
        note_later_start(box, lua_tostring(state, -1));
}

/// Runs the seated entry's `hooks.start` again and throws when it fails.
inline void start_later(oa::sim::brains::ToolSeat& seat) {
    LaterStart box;
    oa::data::script_host::ScriptError error;
    if (!seat.with_seat(&box, start_later_work, error))
        throw std::runtime_error(oa::data::script_host::describe(error));
    if (box.failed) {
        const char* text = box.message[0] != '\0' ? box.message : "hooks.start failed";
        throw std::runtime_error(text);
    }
}

} // namespace oa::test::classic_harness

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The check match against the installed game: its unit types, the commander
// at the centre, the player and base game a brain's start reads, and the
// commander counted by its name and the words of its Category line.

#include "oa/app/brain_check.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/mod_profile_loader.hpp"

#include "oa/core/unit.h"
#include "oa/core/world.h"
#include "oa/data/defs/layout.hpp"
#include "oa/data/script_host/script_host.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/sim/brains/seat.hpp"
#include "oa/sim/match_runtime.hpp"
#include "oa/test/check.hpp"
#include "oa/test/game_data.hpp"
#include "oa/test/scratch_directory.hpp"

#include "lua.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct Scratch {
    fs::path path;

    Scratch() : path(oa::test::make_scratch_directory("T01-app-brain-check")) {}

    ~Scratch() {
        std::error_code error;
        fs::remove_all(path, error);
    }

    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;
};

struct Seen {
    int player{-1};
    std::string mod_id;
    bool found{false};
    int32_t x{};
    int32_t y{};
    int32_t z{};
    int32_t ground{};
    uint8_t owner{};
    std::string type_name;
    std::string commander;
    uint32_t type_count{};
};

void write_text(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    OA_CHECK(static_cast<bool>(out));
}

bool same_ascii(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        const auto fold = [](unsigned char byte) {
            return byte >= 'A' && byte <= 'Z' ? static_cast<unsigned char>(byte - 'A' + 'a') : byte;
        };
        if (fold(static_cast<unsigned char>(left[index])) !=
            fold(static_cast<unsigned char>(right[index])))
            return false;
    }
    return true;
}

void read_globals(void* context, lua_State* state) {
    auto* seen = static_cast<Seen*>(context);
    lua_getglobal(state, "seen_player");
    if (lua_isinteger(state, -1))
        seen->player = static_cast<int>(lua_tointeger(state, -1));
    lua_pop(state, 1);
    lua_getglobal(state, "seen_mod");
    if (lua_isstring(state, -1)) {
        const char* text = lua_tostring(state, -1);
        if (text != nullptr)
            seen->mod_id = text;
    }
    lua_pop(state, 1);
}

void read_match(oa::sim::match_runtime::Match& match, void* context) {
    auto* seen = static_cast<Seen*>(context);
    oa::World& world = match.state();
    seen->type_count = world.unit_def_count;
    if (world.game.side_count > 0) {
        const oa::Side* side_record = oa::world_side(&world, 0);
        if (side_record != nullptr)
            seen->commander = side_record->commander;
    }
    for (uint32_t slot = 1; slot < world.unit_slot_count; ++slot) {
        const oa::Unit& unit = world.units[slot];
        if (unit.type_index == 0)
            continue;
        seen->found = true;
        seen->x = unit.position.x;
        seen->y = unit.position.y;
        seen->z = unit.position.z;
        seen->ground = match.map_height(
            static_cast<uint32_t>(unit.position.x), static_cast<uint32_t>(unit.position.z)
        );
        seen->owner = unit.owner_index;
        if (unit.type_index < world.unit_def_count)
            seen->type_name = world.unit_defs[unit.type_index].unit_name;
        break;
    }
    if (oa::data::script_host::Sandbox* sandbox = oa::sim::brains::seat_sandbox(match, 1)) {
        oa::data::script_host::ScriptError script_error;
        OA_CHECK(sandbox->run_host("read", seen, read_globals, script_error));
    }
}

/// How many unit files the mounted game lists, the same way the check does.
std::size_t count_unit_files(const fs::path& game, const fs::path& mod) {
    oa::app::ModChoice choice;
    choice.folder = mod;
    const oa::app::GameInstall install =
        oa::app::inspect_game_install(game, {}, oa::app::demo_1997, choice);
    OA_CHECK(install.problem.empty());
    OA_CHECK(install.profile_errors.empty());
    const oa::data::defs::DataLayout previous = oa::data::defs::data_layout();
    oa::data::defs::use_data_layout(oa::app::data_layout_of(install.profile.get()));
    oa::AssetStore store(install.folders);
    for (const fs::path& candidate : install.archives) {
        const fs::path archive = candidate.is_absolute() ? candidate : game / candidate;
        store.mount(archive);
    }
    const char* directory = oa::data::defs::directory_name(oa::data::defs::DataDirectory::units);
    const std::vector<std::string> paths =
        store.list_effective(directory, oa::data::defs::unit_file_suffix());
    oa::data::defs::use_data_layout(previous);
    return paths.size();
}

oa::app::InstalledBrain brain_of(const fs::path& scratch) {
    const fs::path folder = scratch / "steady-commander";
    write_text(
        folder / "oabrain.yaml",
        "oabrain: 1\n"
        "id: steady-commander\n"
        "name: \"Steady Commander\"\n"
        "version: \"1.2\"\n"
        "author:\n"
        "  name: \"A. Maker\"\n"
        "description: \"Holds the line.\"\n"
        "tags: [\"classic\"]\n"
        "works-with: [\"ta-3.1c\", \"ridge >= 28\"]\n"
        "levels:\n"
        "  - id: firm\n"
        "    name: \"Firm\"\n"
        "    difficulty: medium\n"
        "entry: \"brain.lua\"\n"
    );
    write_text(
        folder / "brain.lua",
        "return {\n"
        "  start = function()\n"
        "    seen_player = me.player\n"
        "    seen_mod = game.mod.id\n"
        "  end\n"
        "}\n"
    );
    std::string failure;
    const std::optional<oa::app::InstalledBrain> installed =
        oa::app::read_installed_brain(scratch, "steady-commander", failure);
    OA_CHECK(installed.has_value());
    if (!installed)
        std::fprintf(stderr, "brain: %s\n", failure.c_str());
    return installed ? *installed : oa::app::InstalledBrain{};
}

void expect_commander(const Seen& seen) {
    constexpr int32_t centre = static_cast<int32_t>(512u << 16);
    OA_CHECK(seen.found);
    OA_CHECK(seen.owner == 1);
    OA_CHECK(seen.x == centre);
    // Create settles the commander on the map's ground. The height is that
    // ground, in 16.16, which is above the sea on this flat land.
    OA_CHECK(seen.ground > 0);
    OA_CHECK(seen.y == static_cast<int32_t>(static_cast<uint32_t>(seen.ground) << 16));
    OA_CHECK(seen.z == centre);
    OA_CHECK(!seen.commander.empty());
    OA_CHECK(same_ascii(seen.type_name, seen.commander));
}

void base_game_seats_the_commander(const fs::path& game, const oa::app::InstalledBrain& brain) {
    const std::size_t files = count_unit_files(game, {});
    oa::app::BrainCheckRequest request;
    request.game_dir = game;
    request.brain = brain;
    request.choice.id = brain.manifest.id;
    request.choice.level = "firm";
    Seen seen;
    std::string problem;
    const std::optional<oa::app::BrainCheckReport> report =
        oa::app::check_brain_against_game(request, problem, read_match, &seen);
    if (!report)
        std::fprintf(stderr, "brain check: %s\n", problem.c_str());
    OA_CHECK(report.has_value());
    if (!report)
        return;
    OA_CHECK(report->seated);
    OA_CHECK(report->game == "totala");
    OA_CHECK(!report->mod.has_value());
    OA_CHECK(report->level == "firm");
    OA_CHECK(seen.player == 1);
    OA_CHECK(seen.mod_id == "ta-3.1c");
    expect_commander(seen);
    OA_CHECK(seen.type_count > 1);
    OA_CHECK(seen.type_count - 1 + report->warnings.size() == files);
}

void a_unit_file_that_does_not_load_is_a_warning(
    const fs::path& game, const fs::path& scratch, const oa::app::InstalledBrain& brain
) {
    const fs::path mod = scratch / "ridge";
    write_text(
        mod / "oamod.yaml",
        "oamod: 1\n"
        "id: ridge\n"
        "name: \"Ridge\"\n"
        "version: \"28\"\n"
        "requires: {base: ta-3.1c, catalogue: 1}\n"
        "author: {name: unknown}\n"
        "packaging: {revision: 1, date: 2026-10-04, packager: t}\n"
    );
    const oa::data::defs::DataLayout previous = oa::data::defs::data_layout();
    oa::data::defs::use_data_layout(oa::app::data_layout_of(nullptr));
    const std::string units = oa::data::defs::directory_name(oa::data::defs::DataDirectory::units);
    const std::string suffix = oa::data::defs::unit_file_suffix();
    oa::data::defs::use_data_layout(previous);
    write_text(mod / units / ("zzbroken" + suffix), "not a unit\n");

    const std::size_t files = count_unit_files(game, mod);
    oa::app::BrainCheckRequest request;
    request.game_dir = game;
    request.mod_folder = mod;
    request.brain = brain;
    request.choice.id = brain.manifest.id;
    Seen seen;
    std::string problem;
    const std::optional<oa::app::BrainCheckReport> report =
        oa::app::check_brain_against_game(request, problem, read_match, &seen);
    if (!report)
        std::fprintf(stderr, "brain check: %s\n", problem.c_str());
    OA_CHECK(report.has_value());
    if (!report)
        return;
    OA_CHECK(report->seated);
    OA_CHECK(report->mod.has_value());
    if (report->mod)
        OA_CHECK(report->mod->first == "ridge" && report->mod->second == "28");
    std::size_t named = 0;
    for (const std::string& warning : report->warnings) {
        if (warning.find("zzbroken") != std::string::npos) {
            ++named;
            OA_CHECK(warning.find("does not load") != std::string::npos);
        }
    }
    OA_CHECK(named == 1);
    OA_CHECK(seen.type_count > 1);
    OA_CHECK(seen.type_count - 1 + report->warnings.size() == files);
    expect_commander(seen);
}

struct SideSeen {
    int sides{-1};
    std::string side_name;
    bool found{false};
    int32_t x{};
    int32_t z{};
    uint8_t owner{};
    std::string type_name;
};

void read_side_globals(void* context, lua_State* state) {
    auto* seen = static_cast<SideSeen*>(context);
    lua_getglobal(state, "seen_sides");
    if (lua_isinteger(state, -1))
        seen->sides = static_cast<int>(lua_tointeger(state, -1));
    lua_pop(state, 1);
    lua_getglobal(state, "seen_side_name");
    if (lua_isstring(state, -1)) {
        const char* text = lua_tostring(state, -1);
        if (text != nullptr)
            seen->side_name = text;
    }
    lua_pop(state, 1);
}

void read_side_match(oa::sim::match_runtime::Match& match, void* context) {
    auto* seen = static_cast<SideSeen*>(context);
    oa::World& world = match.state();
    for (uint32_t slot = 1; slot < world.unit_slot_count; ++slot) {
        const oa::Unit& unit = world.units[slot];
        if (unit.type_index == 0)
            continue;
        seen->found = true;
        seen->x = unit.position.x;
        seen->z = unit.position.z;
        seen->owner = unit.owner_index;
        if (unit.type_index < world.unit_def_count)
            seen->type_name = world.unit_defs[unit.type_index].unit_name;
        break;
    }
    if (oa::data::script_host::Sandbox* sandbox = oa::sim::brains::seat_sandbox(match, 1)) {
        oa::data::script_host::ScriptError script_error;
        OA_CHECK(sandbox->run_host("read", seen, read_side_globals, script_error));
    }
}

struct Counted {
    std::vector<std::pair<std::string, lua_Integer>> values;
    std::string missing;
    lua_Integer tags{-1};
    uint32_t limit{};
    std::string commander;
};

// Counts its own commander by name, record and the words of its Category
// line, and asks every word of every loaded type's line. A nil count is -1.
constexpr std::string_view counting_brain = R"(local function count(what)
  local found = me:unit_count_of(what)
  if found == nil then
    return -1
  end
  return found
end
return {
  start = function()
    local commander = game.mod.sides[1].commander
    counted = {
      total = me:unit_count(),
      limit = game.unit_limit,
      name = count(commander),
      record = count(units[commander]),
      commander_tag = count("commander"),
      ctrl_c = count("CTRL_C"),
      arm = count("arm"),
      core = count("CORE"),
      vtol = count("VTOL"),
      air = count("AIR"),
      all = count("ALL"),
    }
    local words = {}
    for index = 1, #units do
      for _, word in ipairs(units[index].category) do
        if me:unit_count_of(word) == nil then
          missing = word
        end
        words[string.lower(word)] = true
      end
    end
    tags = 0
    for _ in pairs(words) do
      tags = tags + 1
    end
  end
}
)";

void read_counted(void* context, lua_State* state) {
    auto* counted = static_cast<Counted*>(context);
    lua_getglobal(state, "counted");
    if (lua_istable(state, -1)) {
        for (const char* key :
             {"total",
              "limit",
              "name",
              "record",
              "commander_tag",
              "ctrl_c",
              "arm",
              "core",
              "vtol",
              "air",
              "all"}) {
            lua_getfield(state, -1, key);
            counted->values.emplace_back(
                key, lua_isinteger(state, -1) ? lua_tointeger(state, -1) : -2
            );
            lua_pop(state, 1);
        }
    }
    lua_pop(state, 1);
    lua_getglobal(state, "missing");
    if (lua_isstring(state, -1))
        counted->missing = lua_tostring(state, -1);
    lua_pop(state, 1);
    lua_getglobal(state, "tags");
    if (lua_isinteger(state, -1))
        counted->tags = lua_tointeger(state, -1);
    lua_pop(state, 1);
}

void read_counting_match(oa::sim::match_runtime::Match& match, void* context) {
    auto* counted = static_cast<Counted*>(context);
    counted->limit = match.state().game.units_per_player;
    for (uint32_t slot = 1; slot < match.state().unit_slot_count; ++slot) {
        const oa::Unit& unit = match.state().units[slot];
        if (unit.type_index != 0 && unit.type_index < match.state().unit_def_count)
            counted->commander = match.state().unit_defs[unit.type_index].unit_name;
    }
    if (oa::data::script_host::Sandbox* sandbox = oa::sim::brains::seat_sandbox(match, 1)) {
        oa::data::script_host::ScriptError script_error;
        OA_CHECK(sandbox->run_host("read", counted, read_counted, script_error));
    }
}

lua_Integer counted_value(const Counted& counted, std::string_view key) {
    for (const auto& [name, value] : counted.values)
        if (name == key)
            return value;
    return -3;
}

void counts_read_the_loaded_units(const fs::path& game, const fs::path& scratch) {
    const fs::path folder = scratch / "unit-counter";
    write_text(
        folder / "oabrain.yaml",
        "oabrain: 1\n"
        "id: unit-counter\n"
        "name: \"Unit Counter\"\n"
        "version: \"1\"\n"
        "author:\n"
        "  name: \"A. Maker\"\n"
        "description: \"Counts its own units.\"\n"
        "tags: [\"experimental\"]\n"
        "works-with: [\"ta-3.1c\"]\n"
        "levels:\n"
        "  - id: firm\n"
        "    name: \"Firm\"\n"
        "    difficulty: medium\n"
    );
    write_text(folder / "brain.lua", counting_brain);
    std::string failure;
    const std::optional<oa::app::InstalledBrain> installed =
        oa::app::read_installed_brain(scratch, "unit-counter", failure);
    if (!installed)
        std::fprintf(stderr, "brain: %s\n", failure.c_str());
    OA_CHECK(installed.has_value());
    if (!installed)
        return;
    oa::app::BrainCheckRequest request;
    request.game_dir = game;
    request.brain = *installed;
    request.choice.id = installed->manifest.id;
    request.choice.level = "firm";
    Counted counted;
    std::string problem;
    const std::optional<oa::app::BrainCheckReport> report =
        oa::app::check_brain_against_game(request, problem, read_counting_match, &counted);
    if (!report)
        std::fprintf(stderr, "brain check: %s\n", problem.c_str());
    OA_CHECK(report.has_value());
    if (!report)
        return;
    if (!report->seated)
        std::fprintf(stderr, "unit-counter: %s\n", report->error.c_str());
    OA_CHECK(report->seated);
    // The check match seats the first side's commander, Arm's in the base
    // game. Its Category line carries ARM, commander and CTRL_C, and no
    // aircraft word; the base game has VTOL for aircraft and no AIR.
    OA_CHECK(same_ascii(counted.commander, "ARMCOM"));
    OA_CHECK(counted.limit > 0);
    OA_CHECK(counted_value(counted, "limit") == static_cast<lua_Integer>(counted.limit));
    OA_CHECK(counted_value(counted, "total") == 1);
    OA_CHECK(counted_value(counted, "name") == 1);
    OA_CHECK(counted_value(counted, "record") == 1);
    OA_CHECK(counted_value(counted, "commander_tag") == 1);
    OA_CHECK(counted_value(counted, "ctrl_c") == 1);
    OA_CHECK(counted_value(counted, "arm") == 1);
    OA_CHECK(counted_value(counted, "core") == 0);
    OA_CHECK(counted_value(counted, "vtol") == 0);
    OA_CHECK(counted_value(counted, "air") == -1);
    OA_CHECK(counted_value(counted, "all") == 1);
    if (!counted.missing.empty())
        std::fprintf(stderr, "unit-counter: no count for the tag %s\n", counted.missing.c_str());
    OA_CHECK(counted.missing.empty());
    OA_CHECK(counted.tags > 10);
}

std::optional<fs::path> sides_fixture_of(int argc, char** argv) {
    for (int index = 1; index + 1 < argc; ++index) {
        if (std::string_view(argv[index]) == "--sides-fixture")
            return fs::path(argv[index + 1]);
    }
    return std::nullopt;
}

void a_side_past_the_fifth_seats_its_commander(const fs::path& game, const fs::path& fixture) {
    struct Holder {
        fs::path path;

        Holder() : path(oa::test::make_scratch_directory("T09-app-brain-check")) {}

        ~Holder() {
            std::error_code error;
            fs::remove_all(path, error);
        }

        Holder(const Holder&) = delete;
        Holder& operator=(const Holder&) = delete;
    } holder;

    const fs::path folder = holder.path / "ridge-sixth";
    write_text(
        folder / "oabrain.yaml",
        "oabrain: 1\n"
        "id: ridge-sixth\n"
        "name: \"Ridge Sixth\"\n"
        "version: \"1\"\n"
        "author:\n"
        "  name: \"A. Maker\"\n"
        "description: \"Reads a side past the fifth.\"\n"
        "tags: [\"classic\"]\n"
        "works-with: [\"ridge-sides\"]\n"
        "levels:\n"
        "  - id: firm\n"
        "    name: \"Firm\"\n"
        "    difficulty: medium\n"
        "entry: \"brain.lua\"\n"
    );
    write_text(
        folder / "brain.lua",
        "return {\n"
        "  start = function()\n"
        "    seen_sides = #game.mod.sides\n"
        "    seen_side_name = me.side_name\n"
        "  end\n"
        "}\n"
    );
    std::string failure;
    const std::optional<oa::app::InstalledBrain> installed =
        oa::app::read_installed_brain(holder.path, "ridge-sixth", failure);
    OA_CHECK(installed.has_value());
    if (!installed) {
        std::fprintf(stderr, "brain: %s\n", failure.c_str());
        return;
    }
    oa::app::BrainCheckRequest request;
    request.game_dir = game;
    request.mod_folder = fixture;
    request.brain = *installed;
    request.choice.id = installed->manifest.id;
    request.choice.level = "firm";
    request.side = 6;
    SideSeen seen;
    std::string problem;
    const std::optional<oa::app::BrainCheckReport> report =
        oa::app::check_brain_against_game(request, problem, read_side_match, &seen);
    if (!report)
        std::fprintf(stderr, "brain check: %s\n", problem.c_str());
    OA_CHECK(report.has_value());
    if (!report)
        return;
    OA_CHECK(report->seated);
    OA_CHECK(seen.sides == 7);
    OA_CHECK(seen.side_name == "Ridge6");
    constexpr int32_t centre = static_cast<int32_t>(512u << 16);
    OA_CHECK(seen.found);
    OA_CHECK(seen.owner == 1);
    OA_CHECK(seen.x == centre);
    OA_CHECK(seen.z == centre);
    OA_CHECK(same_ascii(seen.type_name, "RD6COM"));
}

} // namespace

int main(int argc, char** argv) {
    if (!oa::test::game_data_requested(argc, argv))
        return 0;
    const fs::path game = oa::test::require_game_directory("brain check");
    Scratch scratch;
    const oa::app::InstalledBrain brain = brain_of(scratch.path);
    base_game_seats_the_commander(game, brain);
    a_unit_file_that_does_not_load_is_a_warning(game, scratch.path, brain);
    counts_read_the_loaded_units(game, scratch.path);
    if (const std::optional<fs::path> fixture = sides_fixture_of(argc, argv))
        a_side_past_the_fifth_seats_its_commander(game, *fixture);
    return oa::test::check_exit_status();
}

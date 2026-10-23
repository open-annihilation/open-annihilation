// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The facts fact-reader reads back, the read-only game table, and a load
// check with no match. The brain is made up. No game data is mounted.

#include "match_fixture.hpp"

#include "oa/base/sha256.hpp"
#include "oa/core/game_state.h"
#include "oa/core/player.h"
#include "oa/data/brain_facts.hpp"
#include "oa/data/mod_profile.hpp"
#include "oa/data/mod_profile/registry.hpp"
#include "oa/sim/brains/check.hpp"
#include "oa/sim/brains/seat.hpp"

#include "lua.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <fstream>
#include <iostream>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace brains = oa::sim::brains;
namespace facts = oa::data::brain_facts;
namespace host = oa::data::script_host;
namespace played_profile = oa::data::mod_profile;
namespace registry = oa::data::mod_profile::registry;
namespace rules = oa::data::match_rules;

struct StoredOption {
    std::string id;
    brains::OptionKind kind{};
    bool toggle{};
    double number{};
    std::string text;
};

struct Package {
    std::string id{"fact-reader"};
    std::string name{"Fact Reader"};
    std::string version{"1.0"};
    std::string entry{"brain.lua"};
    std::string level{"steady"};
    int32_t difficulty{OA_DIFFICULTY_MEDIUM};
    uint8_t player{computer};
    bool full_awareness{};
    bool with_network{};
    bool corrupt_network{};
    std::vector<std::pair<std::string, std::string>> files;
    std::vector<StoredOption> options;
};

void add_file(Package& package, std::string path, std::string text) {
    package.files.emplace_back(std::move(path), std::move(text));
}

std::string read_packed(const char* name) {
    const std::string path = std::string(OA_FACT_READER_DIR) + "/" + name;
    std::ifstream input(path, std::ios::binary);
    CHECK(input.good());
    input.seekg(0, std::ios::end);
    const auto end = input.tellg();
    CHECK(end >= 0);
    const auto size = static_cast<std::size_t>(end);
    std::string bytes(size, '\0');
    input.seekg(0, std::ios::beg);
    if (size > 0)
        CHECK(static_cast<bool>(input.read(bytes.data(), static_cast<std::streamsize>(size))));
    return bytes;
}

void add_reader(Package& package) {
    add_file(package, "brain.lua", read_packed("brain.lua"));
    add_file(package, "oabrain.yaml", read_packed("oabrain.yaml"));
}

brains::SeatSpec spec_of(
    Package& package,
    std::vector<brains::PackageFile>& files,
    std::vector<brains::OptionValue>& options,
    std::vector<brains::NetworkFile>& networks
) {
    files.clear();
    files.reserve(package.files.size());
    for (const auto& held : package.files)
        files.push_back(
            {held.first,
             std::span<const uint8_t>{
                 reinterpret_cast<const uint8_t*>(held.second.data()), held.second.size()
             }}
        );

    options.clear();
    options.reserve(package.options.size());
    for (const StoredOption& option : package.options)
        options.push_back({option.id, option.kind, option.toggle, option.number, option.text});

    networks.clear();
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
    return spec;
}

brains::SeatOutcome seat_it(Fixture& fixture, Package& package) {
    std::vector<brains::PackageFile> files;
    std::vector<brains::OptionValue> options;
    std::vector<brains::NetworkFile> networks;
    const brains::SeatSpec spec = spec_of(package, files, options, networks);
    return brains::seat_brain(*fixture.match, spec);
}

brains::SeatOutcome check_it(Package& package, const facts::GameFacts& game) {
    std::vector<brains::PackageFile> files;
    std::vector<brains::OptionValue> options;
    std::vector<brains::NetworkFile> networks;
    const brains::SeatSpec spec = spec_of(package, files, options, networks);
    return brains::check_brain(spec, game);
}

std::string_view reason_of(const brains::SeatOutcome& outcome) {
    return std::string_view(outcome.reason.data());
}

std::string global_text(lua_State* state, const char* name) {
    lua_getglobal(state, name);
    std::size_t length = 0;
    const char* text = lua_tolstring(state, -1, &length);
    std::string value;
    if (text != nullptr)
        value.assign(text, length);
    lua_pop(state, 1);
    return value;
}

bool global_bool(lua_State* state, const char* name) {
    lua_getglobal(state, name);
    const bool value = lua_toboolean(state, -1) != 0;
    lua_pop(state, 1);
    return value;
}

double global_number(lua_State* state, const char* name) {
    lua_getglobal(state, name);
    CHECK(lua_isnumber(state, -1) != 0);
    const double value = lua_tonumber(state, -1);
    lua_pop(state, 1);
    return value;
}

struct Read {
    std::string hack_keys;
    lua_Integer hack_count{};
    lua_Integer on_count{};
    lua_Integer limit_count{};
    bool same_game{};
    bool expansions_same{};
    bool mod_same{};
    bool mod_self{};
    bool all_off{};
    bool in_match{};
    bool wave_on{};
    bool hack_same{};
    bool hack_missing{};
    bool has_campaign{};
    bool has_core{};
    bool has_music{};
    bool has_totala{};
    bool has_empty{};
    std::string base;
    std::string engine;
    std::string mode;
    std::string difficulty;
    std::string expansion_text;
    std::string side_names;
    std::string side_prefixes;
    std::string side_commanders;
    std::string mod_id;
    std::string mod_name;
    std::string mod_version;
    std::string squad_rules;
    std::string difficulty_names;
    std::string thresholds;
    std::string name_format;
    std::string mutator_ids;
    std::string mutator_versions;
    std::string surge_type;
    std::string scale_type;
    std::string side_option_type;
    std::string side_option_value;
    std::string write_base;
    std::string write_units;
    std::string write_list;
    double wave_units{};
    double production_1{};
    double production_2{};
    double production_3{};
    double reclaim_1{};
    double reclaim_2{};
    double reclaim_3{};
    double limit_default{};
    double damage_cap{};
    double state_count{};
    double expansion_count{};
    double mutator_count{};
    double scale_value{};
    bool surge_value{};
};

void read_record(void* context, lua_State* state) {
    auto* read = static_cast<Read*>(context);
    read->hack_keys = global_text(state, "hack_keys");
    read->hack_count = static_cast<lua_Integer>(global_number(state, "hack_count"));
    read->on_count = static_cast<lua_Integer>(global_number(state, "on_count"));
    read->limit_count = static_cast<lua_Integer>(global_number(state, "limit_count"));
    read->same_game = global_bool(state, "same_game");
    read->expansions_same = global_bool(state, "expansions_same");
    read->mod_same = global_bool(state, "mod_same");
    read->mod_self = global_bool(state, "mod_self");
    read->all_off = global_bool(state, "all_off");
    read->in_match = global_bool(state, "in_match");
    read->wave_on = global_bool(state, "wave_on");
    read->hack_same = global_bool(state, "hack_same");
    read->hack_missing = global_bool(state, "hack_missing");
    read->has_campaign = global_bool(state, "has_campaign");
    read->has_core = global_bool(state, "has_core");
    read->has_music = global_bool(state, "has_music");
    read->has_totala = global_bool(state, "has_totala");
    read->has_empty = global_bool(state, "has_empty");
    read->base = global_text(state, "base");
    read->engine = global_text(state, "engine");
    read->mode = global_text(state, "mode");
    read->difficulty = global_text(state, "difficulty");
    read->expansion_text = global_text(state, "expansion_text");
    read->side_names = global_text(state, "side_names");
    read->side_prefixes = global_text(state, "side_prefixes");
    read->side_commanders = global_text(state, "side_commanders");
    read->mod_id = global_text(state, "mod_id");
    read->mod_name = global_text(state, "mod_name");
    read->mod_version = global_text(state, "mod_version");
    read->squad_rules = global_text(state, "squad_rules");
    read->difficulty_names = global_text(state, "difficulty_names");
    read->thresholds = global_text(state, "thresholds");
    read->name_format = global_text(state, "name_format");
    read->mutator_ids = global_text(state, "mutator_ids");
    read->mutator_versions = global_text(state, "mutator_versions");
    read->surge_type = global_text(state, "surge_type");
    read->scale_type = global_text(state, "scale_type");
    read->side_option_type = global_text(state, "side_option_type");
    read->side_option_value = global_text(state, "side_option_value");
    read->write_base = global_text(state, "write_base");
    read->write_units = global_text(state, "write_units");
    read->write_list = global_text(state, "write_list");
    read->wave_units = global_number(state, "wave_units");
    read->production_1 = global_number(state, "production_1");
    read->production_2 = global_number(state, "production_2");
    read->production_3 = global_number(state, "production_3");
    read->reclaim_1 = global_number(state, "reclaim_1");
    read->reclaim_2 = global_number(state, "reclaim_2");
    read->reclaim_3 = global_number(state, "reclaim_3");
    read->limit_default = global_number(state, "limit_default");
    read->damage_cap = global_number(state, "damage_cap");
    read->state_count = global_number(state, "state_count");
    read->expansion_count = global_number(state, "expansion_count");
    read->mutator_count = global_number(state, "mutator_count");
    read->scale_value = global_number(state, "scale_value");
    read->surge_value = global_bool(state, "surge_value");
}

Read read_seat(Fixture& fixture, uint8_t player) {
    host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, player);
    CHECK(sandbox != nullptr);
    Read read;
    host::ScriptError error;
    CHECK(sandbox->run_host("read", &read, read_record, error));
    return read;
}

std::string sorted_sim_hacks() {
    std::vector<std::string> ids;
    for (const registry::Entry& entry : registry::table().entries) {
        if (entry.kind == registry::EntryKind::hack && entry.scope == registry::Scope::sim)
            ids.emplace_back(entry.id);
    }
    std::sort(ids.begin(), ids.end());
    std::string joined;
    for (std::size_t index = 0; index < ids.size(); ++index) {
        if (index != 0)
            joined.push_back('|');
        joined += ids[index];
    }
    return joined;
}

struct WalkCounts {
    int sim_hacks{};
    int view_hacks{};
    int limits{};
    bool expect_off{};
};

void walk_registry(void* context, lua_State* state) {
    auto* counts = static_cast<WalkCounts*>(context);
    for (const registry::Entry& entry : registry::table().entries) {
        if (entry.kind != registry::EntryKind::hack && entry.kind != registry::EntryKind::limit)
            continue;
        lua_getglobal(state, "game");
        lua_getfield(state, -1, "mod");
        if (entry.kind == registry::EntryKind::limit)
            lua_getfield(state, -1, "limits");
        else
            lua_getfield(state, -1, "hacks");
        lua_pushlstring(state, entry.id.data(), entry.id.size());
        lua_gettable(state, -2);
        const bool is_table = lua_type(state, -1) == LUA_TTABLE;
        if (entry.kind == registry::EntryKind::hack && entry.scope == registry::Scope::sim) {
            CHECK(is_table);
            if (counts->expect_off) {
                lua_getfield(state, -1, "on");
                CHECK(lua_isboolean(state, -1) != 0);
                CHECK(lua_toboolean(state, -1) == 0);
                lua_pop(state, 1);
            }
            ++counts->sim_hacks;
        } else if (entry.kind == registry::EntryKind::hack) {
            CHECK(lua_isnil(state, -1) != 0);
            ++counts->view_hacks;
        } else {
            CHECK(is_table);
            ++counts->limits;
        }
        lua_pop(state, 4);
    }
}

facts::GameFacts rich_facts() {
    played_profile::ModProfile played;
    played.id = "ridge";
    played.name = "Ridge";
    played.version = "28";
    played.rules.ai.attack_wave_size.enabled = true;
    played.rules.ai.attack_wave_size.units = 10;
    played.rules.ai.squad_assignment.enabled = true;
    played.rules.ai.squad_assignment.rules = rules::AiSquadAssignmentRules::role_squads;
    played.rules.ai.income_multipliers.enabled = true;
    played.rules.ai.income_multipliers.production = {1.25, 0.5, 2.5};
    played.rules.ai.difficulty_names.enabled = true;
    played.rules.ai.difficulty_names.names = {
        rules::AiDifficultyNamesNames::hard,
        rules::AiDifficultyNamesNames::medium,
        rules::AiDifficultyNamesNames::easy,
    };

    facts::GameFacts game;
    game.base = "totala-demo";
    game.expansions = {"campaign", "core-contingency"};
    game.engine = "0.8.6";
    game.mode = facts::GameMode::network;
    game.difficulty = OA_DIFFICULTY_HARD;
    game.mod = facts::mod_facts_of(&played);
    game.mod.sides.push_back({"North", "N", "NCOM"});
    game.mod.sides.push_back({"Vale", "V", "VCOM"});

    facts::OptionFacts surge;
    surge.id = "surge";
    surge.kind = facts::OptionFacts::Kind::toggle;
    surge.toggle = true;
    facts::OptionFacts scale;
    scale.id = "scale";
    scale.kind = facts::OptionFacts::Kind::number;
    scale.number = 1.5;
    facts::MutatorFacts wider;
    wider.id = "wider-waves";
    wider.version = "1.2";
    wider.options.push_back(surge);
    wider.options.push_back(scale);

    facts::OptionFacts side;
    side.id = "side";
    side.kind = facts::OptionFacts::Kind::choice;
    side.text = "north";
    facts::MutatorFacts pick;
    pick.id = "side-pick";
    pick.version = "3";
    pick.options.push_back(side);
    game.mutators.push_back(wider);
    game.mutators.push_back(pick);
    return game;
}

void rich_game_reads_back() {
    Fixture fixture;
    const facts::GameFacts game = rich_facts();
    brains::set_match_game_facts(*fixture.match, game);

    Package first;
    add_reader(first);
    CHECK(seat_it(fixture, first).result == brains::SeatResult::seated);
    const Read read = read_seat(fixture, computer);
    CHECK(read.same_game);
    CHECK(read.expansions_same);
    CHECK(read.mod_same);
    CHECK(read.mod_self);
    CHECK(read.in_match);
    CHECK(read.base == "totala-demo");
    CHECK(read.expansion_text == "campaign|core-contingency");
    CHECK(read.expansion_count == 2);
    CHECK(read.engine == "0.8.6");
    CHECK(read.mode == "network");
    CHECK(read.difficulty == "hard");
    CHECK(read.mod_id == "ridge");
    CHECK(read.mod_name == "Ridge");
    CHECK(read.mod_version == "28");
    CHECK(read.side_names == "North|Vale");
    CHECK(read.side_prefixes == "N|V");
    CHECK(read.side_commanders == "NCOM|VCOM");
    CHECK(read.wave_on);
    CHECK(read.wave_units == 10);
    CHECK(read.squad_rules == "role-squads");
    CHECK(read.production_1 == 1.25);
    CHECK(read.production_2 == 0.5);
    CHECK(read.production_3 == 2.5);
    CHECK(read.reclaim_1 == 0.5);
    CHECK(read.reclaim_2 == 0.7);
    CHECK(read.reclaim_3 == 1.0);
    CHECK(read.difficulty_names == "hard|medium|easy");
    CHECK(read.mutator_count == 2);
    CHECK(read.mutator_ids == "wider-waves|side-pick");
    CHECK(read.mutator_versions == "1.2|3");
    CHECK(read.surge_type == "boolean");
    CHECK(read.surge_value);
    CHECK(read.scale_type == "number");
    CHECK(read.scale_value == 1.5);
    CHECK(read.side_option_type == "string");
    CHECK(read.side_option_value == "north");
    CHECK(read.has_campaign);
    CHECK(read.has_core);
    CHECK(!read.has_music);
    CHECK(!read.has_totala);
    CHECK(!read.has_empty);
    CHECK(read.hack_same);
    CHECK(read.hack_missing);
    CHECK(read.write_base == "game is read-only");
    CHECK(read.write_units == "game is read-only");
    CHECK(read.write_list == "game is read-only");
    CHECK(read.on_count >= 4);

    const std::string order = sorted_sim_hacks();
    CHECK(read.hack_keys == order);

    auto& local = fixture.match->state().game.players[0];
    local.status = OA_PLAYER_STATUS_COMPUTER;
    local.in_use = 1;
    Package second;
    second.player = 0;
    add_reader(second);
    CHECK(seat_it(fixture, second).result == brains::SeatResult::seated);
    const Read other = read_seat(fixture, 0);
    CHECK(other.hack_keys == order);

    WalkCounts counts;
    host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, computer);
    CHECK(sandbox != nullptr);
    host::ScriptError error;
    CHECK(sandbox->run_host("walk", &counts, walk_registry, error));
    CHECK(counts.sim_hacks == read.hack_count);
    CHECK(counts.limits == read.limit_count);
    CHECK(counts.sim_hacks > 0);
    CHECK(counts.limits > 0);
}

void default_game_is_the_base_game() {
    Fixture fixture;
    Package package;
    add_reader(package);
    CHECK(seat_it(fixture, package).result == brains::SeatResult::seated);
    const Read read = read_seat(fixture, computer);
    CHECK(read.base == "totala");
    CHECK(read.mod_id == "ta-3.1c");
    CHECK(read.mod_name == "Total Annihilation 3.1c");
    CHECK(read.mod_version == "3.1c");
    CHECK(read.engine.empty());
    CHECK(read.mode == "skirmish");
    CHECK(read.difficulty == "medium");
    CHECK(read.in_match);
    CHECK(read.expansion_count == 0);
    CHECK(read.expansion_text.empty());
    CHECK(read.mutator_count == 0);
    CHECK(read.all_off);
    CHECK(read.on_count == 0);
    CHECK(!read.wave_on);
    CHECK(read.wave_units == 6);
    CHECK(read.squad_rules == "base");
    CHECK(read.production_1 == 0.5);
    CHECK(read.production_2 == 0.7);
    CHECK(read.production_3 == 1.0);
    CHECK(read.reclaim_1 == 0.5);
    CHECK(read.reclaim_2 == 0.7);
    CHECK(read.reclaim_3 == 1.0);
    CHECK(read.difficulty_names == "easy|medium|hard");
    CHECK(read.limit_default == 250);
    CHECK(read.thresholds == "5|10|15|20|25");
    CHECK(read.damage_cap == 5);
    CHECK(read.state_count == 0);
    CHECK(read.name_format == "AI:%s");
    CHECK(read.write_base == "game is read-only");

    WalkCounts counts;
    counts.expect_off = true;
    host::Sandbox* sandbox = brains::seat_sandbox(*fixture.match, computer);
    CHECK(sandbox != nullptr);
    host::ScriptError error;
    CHECK(sandbox->run_host("walk", &counts, walk_registry, error));
    CHECK(counts.sim_hacks == read.hack_count);
    CHECK(counts.limits == read.limit_count);
}

void check_sees_the_facts_outside_a_match() {
    Fixture fixture;
    CHECK(!fixture.match->seat_state());
    Package package;
    add_reader(package);
    const facts::GameFacts game = rich_facts();
    const brains::SeatOutcome outcome = check_it(package, game);
    CHECK(outcome.result == brains::SeatResult::seated);
    CHECK(reason_of(outcome).empty());
    CHECK(!fixture.match->seat_state());
}

void check_reports_a_reason() {
    Fixture fixture;
    Package package;
    add_file(package, "brain.lua", "return { fits = function() return \"Made for Ridge\" end }\n");
    const brains::SeatOutcome outcome = check_it(package, facts::GameFacts{});
    CHECK(outcome.result == brains::SeatResult::does_not_fit);
    CHECK(reason_of(outcome) == "Made for Ridge");
    CHECK(!fixture.match->seat_state());
}

void check_stops_at_the_phase_allowance() {
    Fixture fixture;
    Package package;
    add_file(package, "brain.lua", "return { fits = function() while true do end end }\n");
    const brains::SeatOutcome outcome = check_it(package, facts::GameFacts{});
    CHECK(outcome.result == brains::SeatResult::failed);
    CHECK(outcome.error.kind == host::ErrorKind::instruction_budget);
    CHECK(outcome.error.message == "the script ran past its budget of 50000000 instructions");
    CHECK(outcome.error.phase == "fits");
    CHECK(!fixture.match->seat_state());
}

void check_refuses_a_bad_network_before_lua() {
    Fixture fixture;
    Package package;
    package.with_network = true;
    package.corrupt_network = true;
    add_file(package, "brain.lua", "return { fits = function() return \"lua ran\" end }\n");
    add_file(package, "nets/scout.oann", "scout");
    const brains::SeatOutcome outcome = check_it(package, facts::GameFacts{});
    CHECK(outcome.result == brains::SeatResult::refused);
    CHECK(reason_of(outcome) == "nets/scout.oann does not match its sha256 in oabrain.yaml");
    CHECK(!fixture.match->seat_state());
}

void check_reads_the_libraries_outside_a_match() {
    // brain.random and brain.after are absent on this branch: B03 is not
    // merged. Outside a match, A04's getters that read the match (side and
    // name) read nil; its methods, such as me:allies, are present.
    Fixture fixture;
    Package package;
    package.player = 7;
    package.options.push_back({"hold", brains::OptionKind::toggle, true, 0, {}});
    add_file(
        package,
        "brain.lua",
        "return { fits = function()\n"
        "  if me.player ~= 7 then return \"player\" end\n"
        "  if me.level ~= \"steady\" then return \"level\" end\n"
        "  if me.difficulty ~= \"medium\" then return \"difficulty\" end\n"
        "  if me.options.hold ~= true then return \"hold\" end\n"
        "  if type(brain) ~= \"table\" then return \"brain\" end\n"
        "  if brain.random ~= nil then return \"random\" end\n"
        "  if brain.after ~= nil then return \"after\" end\n"
        "  if type(f32) ~= \"table\" then return \"f32\" end\n"
        "  if type(geometry) ~= \"table\" then return \"geometry\" end\n"
        "  if type(geometry.sine_scaled) ~= \"function\" then return \"sine\" end\n"
        "  if me.side ~= nil or me.name ~= nil then return \"me extra\" end\n"
        "  return true\n"
        "end }\n"
    );
    const brains::SeatOutcome outcome = check_it(package, facts::GameFacts{});
    CHECK(outcome.result == brains::SeatResult::seated);
    CHECK(!fixture.match->seat_state());
}

} // namespace

int main() {
    try {
        rich_game_reads_back();
        default_game_is_the_base_game();
        check_sees_the_facts_outside_a_match();
        check_reports_a_reason();
        check_stops_at_the_phase_allowance();
        check_refuses_a_bad_network_before_lua();
        check_reads_the_libraries_outside_a_match();
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
    return 0;
}

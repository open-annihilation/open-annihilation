// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The maps hook over a made-up Ridge store. A changed scenario is served
// from the mutator layer and reads back through the session loader. An
// unchanged map is not served. With --data, every installed map is read
// and one profile rewrite is loaded the same way.
#include "oa/data/campaign/campaign_file.hpp"
#include "oa/data/mutators/pipeline.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/formats/tdf.hpp"
#include "oa/test/check.hpp"
#include "oa/test/game_assets.hpp"
#include "oa/test/game_data.hpp"
#include "oa/test/processor_time.hpp"
#include "oa/test/scratch_directory.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef OA_MUTATORS_TEST_DATA
#error OA_MUTATORS_TEST_DATA is the fixture folder
#endif

namespace {

namespace mutators = oa::data::mutators;
namespace oamut = oa::formats::oamut;
namespace tdf = oa::formats::tdf;
namespace defs = oa::data::defs;
namespace missions = oa::data::campaign;
namespace fs = std::filesystem;

const fs::path k_data{OA_MUTATORS_TEST_DATA};

const char* k_valley = R"([GlobalHeader]
{
missionname=Ridge Valley;
planet=Ice;
gravity=112;
minwindspeed=0;
maxwindspeed=100;
tidalstrength=20;
lavaworld=0;
waterdoesdamage=0;
nosealeveltrigger=0;
killmul=1;
timemul=1;
numplayers=2;
waterdamage=0;
[Schema 0]
{
type=Network 1;
aiprofile=DEFAULT;
HumanMetal=1000;
HumanEnergy=1000;
ComputerMetal=1000;
ComputerEnergy=1000;
SurfaceMetal=10;
[units]
{
[unit0]
{
Unitname=RGCOM;
XPos=10;
ZPos=20;
Player=1;
}
}
[features]
{
[feature0]
{
Featurename=Tree;
XPos=5;
ZPos=6;
}
}
[specials]
{
[special0]
{
specialwhat=StartPos;
XPos=100;
ZPos=200;
}
[special1]
{
specialwhat=StartPos2;
XPos=300;
ZPos=400;
}
}
}
[Schema 1]
{
type=Network 2;
aiprofile=OTHER;
HumanMetal=2000;
HumanEnergy=1500;
ComputerMetal=800;
ComputerEnergy=900;
SurfaceMetal=20;
[units]
{
[unit0]
{
Unitname=RGBOT;
XPos=30;
ZPos=40;
Player=2;
}
}
[features]
{
[feature0]
{
Featurename=Rock;
XPos=7;
ZPos=8;
}
}
[specials]
{
[special0]
{
specialwhat=StartPos;
XPos=500;
ZPos=600;
}
[special1]
{
specialwhat=StartPos2;
XPos=700;
ZPos=800;
}
}
}
}
)";

const char* k_other_name = R"([GlobalHeader]
{
missionname=Other Valley;
planet=Ice;
gravity=112;
minwindspeed=0;
maxwindspeed=100;
tidalstrength=20;
lavaworld=0;
waterdoesdamage=0;
nosealeveltrigger=0;
killmul=1;
timemul=1;
numplayers=2;
waterdamage=0;
[Schema 0]
{
type=Network 1;
aiprofile=DEFAULT;
HumanMetal=1000;
HumanEnergy=1000;
ComputerMetal=1000;
ComputerEnergy=1000;
SurfaceMetal=10;
[specials]
{
[special0]
{
specialwhat=StartPos;
XPos=100;
ZPos=200;
}
[special1]
{
specialwhat=StartPos2;
XPos=300;
ZPos=400;
}
}
}
}
)";

const char* k_lower_name = R"([GlobalHeader]
{
missionname=ridge valley;
planet=Ice;
gravity=112;
minwindspeed=0;
maxwindspeed=100;
tidalstrength=20;
lavaworld=0;
waterdoesdamage=0;
nosealeveltrigger=0;
killmul=1;
timemul=1;
numplayers=2;
waterdamage=0;
[Schema 0]
{
type=Network 1;
aiprofile=DEFAULT;
HumanMetal=1000;
HumanEnergy=1000;
ComputerMetal=1000;
ComputerEnergy=1000;
SurfaceMetal=10;
[specials]
{
[special0]
{
specialwhat=StartPos;
XPos=100;
ZPos=200;
}
[special1]
{
specialwhat=StartPos2;
XPos=300;
ZPos=400;
}
}
}
}
)";

struct Scratch {
    fs::path path;

    explicit Scratch(std::string_view name) : path(oa::test::make_scratch_directory(name)) {}

    ~Scratch() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

void write_text(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    OA_CHECK(out.good());
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::vector<uint8_t> read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    OA_CHECK(in.good());
    if (!in.good())
        return {};
    in.seekg(0, std::ios::end);
    const std::streamoff length = in.tellg();
    in.seekg(0);
    if (length <= 0)
        return {};
    std::vector<uint8_t> bytes(static_cast<std::size_t>(length));
    in.read(reinterpret_cast<char*>(bytes.data()), length);
    return bytes;
}

void write_store(const fs::path& root) {
    write_text(root / "maps" / "ridge-valley.ota", k_valley);
    write_text(root / "maps" / "ridgevalley.ota", k_valley);
    write_text(root / "maps" / "other-valley.ota", k_other_name);
    write_text(root / "maps" / "lower-valley.ota", k_lower_name);
    write_text(root / "units" / "RGCOM.fbi", "[UNITINFO]\n{\nUnitName=RGCOM;\n}\n");
    write_text(root / "units" / "RGBOT.fbi", "[UNITINFO]\n{\nUnitName=RGBOT;\n}\n");
    write_text(root / "units" / "RGLAB.fbi", "[UNITINFO]\n{\nUnitName=RGLAB;\n}\n");
    write_text(root / "objects3d" / "RGCOM.3DO", "3DO");
    write_text(root / "objects3d" / "RGBOT.3DO", "3DO");
    write_text(root / "objects3d" / "RGLAB.3DO", "3DO");
    write_text(root / "features" / "ridge.tdf", "[Tree]\n{\n}\n[Rock]\n{\n}\n");
    write_text(root / "ai" / "DEFAULT.txt", "profile=default\n");
    write_text(root / "ai" / "OTHER.txt", "profile=other\n");
    write_text(root / "ai" / "TURBO.txt", "profile=turbo\n");
}

mutators::PipelineMutator load_mutator(const fs::path& package) {
    mutators::PipelineMutator mutator;
    mutator.package = package;
    const std::vector<uint8_t> bytes = read_bytes(package / "oamut.yaml");
    std::vector<oamut::Problem> problems;
    const bool read = oamut::read_manifest(bytes, mutator.manifest, problems);
    OA_CHECK(read);
    return mutator;
}

void write_package(
    const fs::path& package, std::string_view id, std::string_view layers, std::string_view lua
) {
    std::string yaml;
    yaml += "oamut: 1\n";
    yaml += "id: ";
    yaml += id;
    yaml += "\nname: \"Ridge Maps\"\nversion: \"1.0\"\nauthor:\n  name: \"Ridge Maker\"\n";
    yaml += "tags: [\"gameplay\"]\nworks-with: [\"*\"]\nlayers: ";
    yaml += layers;
    yaml += "\n";
    write_text(package / "oamut.yaml", yaml);
    write_text(package / "mutator.lua", lua);
}

mutators::PipelineInput make_input(std::vector<mutators::PipelineMutator> list) {
    mutators::PipelineInput input;
    input.mutators = std::move(list);
    input.mod.id = "ridge";
    input.mod.version = "1.0";
    input.mod.game = "ridge";
    input.engine_version = "0.8.5";
    input.layout = &defs::data_layout();
    input.seed = 1;
    return input;
}

mutators::ChosenMap chosen_from(std::string key, std::string pack, std::vector<uint8_t> bytes) {
    mutators::ChosenMap map;
    map.ota_path = "maps/" + key + ".ota";
    map.key = std::move(key);
    map.pack = std::move(pack);
    map.ota = std::make_shared<const std::vector<uint8_t>>(std::move(bytes));
    return map;
}

mutators::ChosenMap chosen_file(oa::AssetStore& store, std::string key, std::string pack) {
    const oa::AssetData data = store.read("maps/" + key + ".ota");
    return chosen_from(std::move(key), std::move(pack), data.bytes);
}

void release(oa::AssetStore& store) {
    if (store.mutator_layer_mounted())
        store.unmount_mutator_layer();
}

mutators::PipelineResult run_list(
    oa::AssetStore& store,
    const fs::path& scratch,
    std::vector<std::pair<std::string, std::string>> scripts,
    std::string_view layers,
    const mutators::ChosenMap& map
) {
    release(store);
    std::vector<mutators::PipelineMutator> list;
    for (std::size_t index = 0; index < scripts.size(); ++index) {
        const fs::path package =
            scratch / ("pkg-" + std::to_string(index) + "-" + scripts[index].first);
        write_package(package, scripts[index].first, layers, scripts[index].second);
        list.push_back(load_mutator(package));
    }
    mutators::PipelineInput input = make_input(std::move(list));
    input.map = map;
    return mutators::run_pipeline(store, input);
}

mutators::PipelineResult run_one(
    oa::AssetStore& store,
    const fs::path& scratch,
    std::string_view id,
    std::string_view layers,
    std::string_view lua,
    const mutators::ChosenMap& map
) {
    std::vector<std::pair<std::string, std::string>> scripts;
    scripts.emplace_back(std::string(id), std::string(lua));
    return run_list(store, scratch, std::move(scripts), layers, map);
}

const oa::MutatorLayerEntry*
served_entry(const mutators::PipelineResult& result, std::string_view path) {
    for (const oa::MutatorLayerEntry& entry : result.layer.entries) {
        if (entry.path == path && entry.kind == oa::MutatorSourceKind::contents &&
            entry.contents != nullptr)
            return &entry;
    }
    return nullptr;
}

std::string served_text(const mutators::PipelineResult& result, std::string_view path) {
    const oa::MutatorLayerEntry* entry = served_entry(result, path);
    if (entry == nullptr)
        return {};
    return std::string(entry->contents->begin(), entry->contents->end());
}

bool message_has(const mutators::PipelineResult& result, std::string_view fragment) {
    if (result.mutators.empty())
        return false;
    return result.mutators.front().message.find(fragment) != std::string::npos;
}

void expect_refused(const mutators::PipelineResult& result, std::string_view fragment) {
    OA_CHECK(!result.playable());
    OA_CHECK(!result.mutators.empty());
    if (result.mutators.empty())
        return;
    OA_CHECK(result.mutators.front().outcome == mutators::Outcome::failed);
    OA_CHECK(message_has(result, fragment));
    if (!message_has(result, fragment))
        std::fprintf(stderr, "mutator said: %s\n", result.mutators.front().message.c_str());
    OA_CHECK(served_entry(result, "maps/ridge-valley.ota") == nullptr);
}

const tdf::Block* header_of(const tdf::OwnedDocument& document) {
    return tdf::find_child(document.root(), "GlobalHeader");
}

const char* field_of(const tdf::Block* block, const char* key) {
    if (block == nullptr)
        return nullptr;
    return tdf::find_value(block, key);
}

bool starts_nocase(std::string_view text, std::string_view prefix) {
    if (text.size() < prefix.size())
        return false;
    for (std::size_t index = 0; index < prefix.size(); ++index) {
        unsigned char left = static_cast<unsigned char>(text[index]);
        unsigned char right = static_cast<unsigned char>(prefix[index]);
        if (left >= 'A' && left <= 'Z')
            left = static_cast<unsigned char>(left - 'A' + 'a');
        if (right >= 'A' && right <= 'Z')
            right = static_cast<unsigned char>(right - 'A' + 'a');
        if (left != right)
            return false;
    }
    return true;
}

int start_count(const tdf::Block* schema) {
    const tdf::Block* specials = tdf::find_child(schema, "specials");
    int count = 0;
    for (uint32_t index = 0; index < tdf::child_count(specials); ++index) {
        const char* what = field_of(tdf::child_at(specials, index), "specialwhat");
        if (what != nullptr && starts_nocase(what, "StartPos"))
            ++count;
    }
    return count;
}

std::string start_at(const tdf::Block* schema, uint32_t index, const char* key) {
    const tdf::Block* specials = tdf::find_child(schema, "specials");
    uint32_t seen = 0;
    for (uint32_t child = 0; child < tdf::child_count(specials); ++child) {
        const tdf::Block* block = tdf::child_at(specials, child);
        const char* what = field_of(block, "specialwhat");
        if (what == nullptr || !starts_nocase(what, "StartPos"))
            continue;
        if (seen == index) {
            const char* value = field_of(block, key);
            return value != nullptr ? value : std::string{};
        }
        ++seen;
    }
    return {};
}

struct MemoryFiles {
    std::map<std::string, std::string> files;
    std::vector<std::string> messages;
};

std::string fold_path(std::string path) {
    for (char& piece : path) {
        if (piece == '\\')
            piece = '/';
        else if (piece >= 'A' && piece <= 'Z')
            piece = static_cast<char>(piece - 'A' + 'a');
    }
    return path;
}

int32_t memory_size(void* context, const char* path) {
    auto* files = static_cast<MemoryFiles*>(context);
    const auto found = files->files.find(fold_path(path));
    if (found == files->files.end())
        return -1;
    return static_cast<int32_t>(found->second.size());
}

int32_t memory_read(void* context, const char* path, char* buffer, uint32_t capacity) {
    auto* files = static_cast<MemoryFiles*>(context);
    const auto found = files->files.find(fold_path(path));
    if (found == files->files.end())
        return -1;
    const std::size_t count = found->second.size() < capacity ? found->second.size() : capacity;
    std::memcpy(buffer, found->second.data(), count);
    return static_cast<int32_t>(count);
}

void memory_message(void* context, const char* text) {
    static_cast<MemoryFiles*>(context)->messages.emplace_back(text != nullptr ? text : "");
}

missions::CampaignFiles memory_services(MemoryFiles& files) {
    return {
        &files,
        memory_size,
        memory_read,
        nullptr,
        nullptr,
        memory_message,
        nullptr,
        nullptr,
        nullptr
    };
}

bool load_session(MemoryFiles& files, missions::CampaignFile& session, const char* map_name) {
    const missions::CampaignFiles services = memory_services(files);
    missions::CampaignEnv env{&services, nullptr, 0, 2};
    missions::campaign_file_construct(&session, missions::SessionKind::skirmish, &env);
    const bool loaded = missions::campaign_load_mission_info(&session, &env, map_name);
    if (!loaded) {
        for (const std::string& message : files.messages)
            std::fprintf(stderr, "session: %s\n", message.c_str());
    }
    return loaded;
}

void test_header_reads(
    oa::AssetStore& store, const fs::path& scratch, const mutators::ChosenMap& valley
) {
    const char* lua = R"(function mutate(ctx)
  return { maps = function(map, ctx)
    if map.name ~= "Ridge Valley" then error("name") end
    if map.file ~= "ridge-valley" then error("file") end
    if map.planet ~= "Ice" then error("planet") end
    if map.pack ~= nil then error("pack") end
    if map.header.gravity ~= 112 then error("gravity") end
    if map.header.minwindspeed ~= 0 then error("minwind") end
    if map.header.maxwindspeed ~= 100 then error("maxwind") end
    if map.header.tidalstrength ~= 20 then error("tidal") end
    if map.header.lavaworld ~= 0 then error("lava") end
    if map.header.numplayers ~= "2" and map.header.numplayers ~= 2 then error("players") end
    if #map.schemas ~= 2 then error("schemas") end
    local first = map.schemas[1]
    if first.index ~= 0 or first.players ~= 2 or first.type ~= "Network 1" then error("schema") end
    if first.aiprofile ~= "DEFAULT" or first.humanmetal ~= 1000 then error("resources") end
  end }
end
)";
    const mutators::PipelineResult result =
        run_one(store, scratch, "ridge-read", "[\"maps\"]", lua, valley);
    OA_CHECK(result.playable());
    OA_CHECK(served_entry(result, valley.ota_path) == nullptr);
    if (!result.playable() && !result.mutators.empty())
        std::fprintf(stderr, "read: %s\n", result.mutators.front().message.c_str());
}

void test_header_writes(
    oa::AssetStore& store, const fs::path& scratch, const mutators::ChosenMap& valley
) {
    const char* lua = R"(function mutate(ctx)
  return { maps = function(map, ctx)
    map.header.gravity = 50
    map.header.minwindspeed = 1
    map.header.maxwindspeed = 90
    map.header.waterdamage = 3
    map.header.lavaworld = 1
    map.header.waterdoesdamage = 1
    map.header.nosealeveltrigger = 1
    map.header.tidalstrength = 12.5
    map.header.killmul = 2
    map.header.timemul = 0.5
  end }
end
)";
    const mutators::PipelineResult result =
        run_one(store, scratch, "ridge-write", "[\"maps\"]", lua, valley);
    OA_CHECK(result.playable());
    tdf::OwnedDocument document;
    const std::string text = served_text(result, valley.ota_path);
    OA_CHECK(!text.empty());
    OA_CHECK(document.parse(text, nullptr));
    const tdf::Block* header = header_of(document);
    OA_CHECK(std::string(field_of(header, "gravity") ? field_of(header, "gravity") : "") == "50");
    OA_CHECK(
        std::string(field_of(header, "minwindspeed") ? field_of(header, "minwindspeed") : "") == "1"
    );
    OA_CHECK(
        std::string(field_of(header, "maxwindspeed") ? field_of(header, "maxwindspeed") : "") ==
        "90"
    );
    OA_CHECK(
        std::string(field_of(header, "waterdamage") ? field_of(header, "waterdamage") : "") == "3"
    );
    OA_CHECK(
        std::string(field_of(header, "lavaworld") ? field_of(header, "lavaworld") : "") == "1"
    );
    OA_CHECK(
        std::string(
            field_of(header, "waterdoesdamage") ? field_of(header, "waterdoesdamage") : ""
        ) == "1"
    );
    OA_CHECK(
        std::string(
            field_of(header, "nosealeveltrigger") ? field_of(header, "nosealeveltrigger") : ""
        ) == "1"
    );
    OA_CHECK(
        std::string(field_of(header, "tidalstrength") ? field_of(header, "tidalstrength") : "") ==
        "12.5"
    );
    OA_CHECK(std::string(field_of(header, "killmul") ? field_of(header, "killmul") : "") == "2");
    OA_CHECK(std::string(field_of(header, "timemul") ? field_of(header, "timemul") : "") == "0.5");
    OA_CHECK(std::string(field_of(header, "planet") ? field_of(header, "planet") : "") == "Ice");
    OA_CHECK(
        std::string(field_of(header, "missionname") ? field_of(header, "missionname") : "") ==
        "Ridge Valley"
    );
    const tdf::Block* schema = tdf::find_child(header, "Schema 0");
    OA_CHECK(start_count(schema) == 2);
    OA_CHECK(start_at(schema, 0, "XPos") == "100");
    OA_CHECK(start_at(schema, 1, "ZPos") == "400");
}

void test_header_refusals(
    oa::AssetStore& store, const fs::path& scratch, const mutators::ChosenMap& valley
) {
    const auto refuse = [&](const char* id, const char* body, const char* fragment) {
        std::string lua = "function mutate(ctx)\n  return { maps = function(map, ctx)\n    ";
        lua += body;
        lua += "\n  end }\nend\n";
        expect_refused(run_one(store, scratch, id, "[\"maps\"]", lua, valley), fragment);
    };
    refuse(
        "ridge-grav-high",
        "map.header.gravity = 32768",
        "map.header.gravity must be a whole number from 0 to 32767"
    );
    refuse(
        "ridge-grav-frac",
        "map.header.gravity = 1.5",
        "map.header.gravity must be a whole number from 0 to 32767"
    );
    refuse(
        "ridge-grav-text",
        "map.header.gravity = \"no\"",
        "map.header.gravity must be a whole number from 0 to 32767"
    );
    refuse("ridge-lava", "map.header.lavaworld = 2", "map.header.lavaworld must be 0 or 1");
    refuse(
        "ridge-tidal",
        "map.header.tidalstrength = 1001",
        "map.header.tidalstrength must be a number from 0 to 1000"
    );
    refuse("ridge-planet", "map.header.planet = \"Lava\"", "map.header.planet cannot change");
    refuse(
        "ridge-mission", "map.header.missionname = \"No\"", "map.header.missionname cannot change"
    );
    refuse("ridge-players", "map.header.numplayers = 4", "map.header.numplayers cannot change");
    refuse("ridge-header", "map.header = 1", "the map cannot change there");
    refuse("ridge-name", "map.name = \"No\"", "the map cannot change there");
}

void test_schemas(
    oa::AssetStore& store, const fs::path& scratch, const mutators::ChosenMap& valley
) {
    const char* lua = R"(function mutate(ctx)
  return { maps = function(map, ctx)
    map.schemas:set { aiprofile = "TURBO", surfacemetal = 15 }
    map.schemas:scale { humanmetal = 2 }
    map.schemas:where { type = "Network 2" }:set { humanenergy = 2222 }
  end }
end
)";
    const mutators::PipelineResult result =
        run_one(store, scratch, "ridge-schemas", "[\"maps\"]", lua, valley);
    OA_CHECK(result.playable());
    if (!result.playable() && !result.mutators.empty())
        std::fprintf(stderr, "schemas: %s\n", result.mutators.front().message.c_str());
    tdf::OwnedDocument document;
    OA_CHECK(document.parse(served_text(result, valley.ota_path), nullptr));
    const tdf::Block* header = header_of(document);
    const tdf::Block* first = tdf::find_child(header, "Schema 0");
    const tdf::Block* second = tdf::find_child(header, "Schema 1");
    OA_CHECK(
        std::string(field_of(first, "aiprofile") ? field_of(first, "aiprofile") : "") == "TURBO"
    );
    OA_CHECK(
        std::string(field_of(second, "aiprofile") ? field_of(second, "aiprofile") : "") == "TURBO"
    );
    OA_CHECK(
        std::string(field_of(first, "SurfaceMetal") ? field_of(first, "SurfaceMetal") : "") == "15"
    );
    OA_CHECK(
        std::string(field_of(second, "SurfaceMetal") ? field_of(second, "SurfaceMetal") : "") ==
        "15"
    );
    OA_CHECK(
        std::string(field_of(first, "HumanMetal") ? field_of(first, "HumanMetal") : "") == "2000"
    );
    OA_CHECK(
        std::string(field_of(second, "HumanMetal") ? field_of(second, "HumanMetal") : "") == "4000"
    );
    OA_CHECK(
        std::string(field_of(first, "HumanEnergy") ? field_of(first, "HumanEnergy") : "") == "1000"
    );
    OA_CHECK(
        std::string(field_of(second, "HumanEnergy") ? field_of(second, "HumanEnergy") : "") ==
        "2222"
    );
    OA_CHECK(start_count(first) == 2);
    OA_CHECK(std::string(field_of(first, "type") ? field_of(first, "type") : "") == "Network 1");

    const auto refuse = [&](const char* id, const char* body, const char* fragment) {
        std::string script = "function mutate(ctx)\n  return { maps = function(map, ctx)\n    ";
        script += body;
        script += "\n  end }\nend\n";
        expect_refused(run_one(store, scratch, id, "[\"maps\"]", script, valley), fragment);
    };
    refuse(
        "ridge-missing-ai",
        "map.schemas:set { aiprofile = \"MISSING\" }",
        "ai/MISSING.txt is not there"
    );
    refuse(
        "ridge-bad-ai",
        "map.schemas:set { aiprofile = \"bad name\" }",
        "aiprofile must be 1 to 31 letters, digits, _ or -"
    );
    refuse(
        "ridge-metal",
        "map.schemas:set { humanmetal = 1000001 }",
        "map.schemas.humanmetal must be a whole number from 0 to 1000000"
    );
    refuse(
        "ridge-surface",
        "map.schemas:set { surfacemetal = 256 }",
        "map.schemas.surfacemetal must be a whole number from 0 to 255"
    );
    refuse("ridge-type", "map.schemas[1].type = \"Easy\"", "schema.type cannot change");
    refuse(
        "ridge-scale-ai",
        "map.schemas:scale { aiprofile = 2 }",
        "map.schemas.aiprofile cannot be scaled"
    );
}

void test_placed(
    oa::AssetStore& store, const fs::path& scratch, const mutators::ChosenMap& valley
) {
    const char* lua = R"(function mutate(ctx)
  return { maps = function(map, ctx)
    local unit = map.schemas[1].units[1]
    unit.unitname = "RGBOT"
    unit.xpos = 32767
    unit.zpos = 0
    unit.player = 255
    map.schemas[1].features[1].featurename = "Rock"
  end }
end
)";
    const mutators::PipelineResult result =
        run_one(store, scratch, "ridge-placed", "[\"maps\"]", lua, valley);
    OA_CHECK(result.playable());
    if (!result.playable() && !result.mutators.empty())
        std::fprintf(stderr, "placed: %s\n", result.mutators.front().message.c_str());
    tdf::OwnedDocument document;
    OA_CHECK(document.parse(served_text(result, valley.ota_path), nullptr));
    const tdf::Block* schema = tdf::find_child(header_of(document), "Schema 0");
    const tdf::Block* units = tdf::find_child(schema, "units");
    const tdf::Block* unit = tdf::child_at(units, 0);
    OA_CHECK(std::string(field_of(unit, "Unitname") ? field_of(unit, "Unitname") : "") == "RGBOT");
    OA_CHECK(std::string(field_of(unit, "XPos") ? field_of(unit, "XPos") : "") == "32767");
    OA_CHECK(std::string(field_of(unit, "ZPos") ? field_of(unit, "ZPos") : "") == "0");
    OA_CHECK(std::string(field_of(unit, "Player") ? field_of(unit, "Player") : "") == "255");
    const tdf::Block* features = tdf::find_child(schema, "features");
    const tdf::Block* feature = tdf::child_at(features, 0);
    OA_CHECK(
        std::string(field_of(feature, "Featurename") ? field_of(feature, "Featurename") : "") ==
        "Rock"
    );
    OA_CHECK(start_count(schema) == 2);
    OA_CHECK(start_at(schema, 0, "XPos") == "100");
    OA_CHECK(start_at(schema, 1, "XPos") == "300");

    const char* remove_lua = R"(function mutate(ctx)
  return { maps = function(map, ctx)
    map.schemas[1].units[1]:remove()
    map.schemas[1].features[1]:remove()
  end }
end
)";
    const mutators::PipelineResult removed =
        run_one(store, scratch, "ridge-remove", "[\"maps\"]", remove_lua, valley);
    OA_CHECK(removed.playable());
    tdf::OwnedDocument after;
    OA_CHECK(after.parse(served_text(removed, valley.ota_path), nullptr));
    const tdf::Block* gone = tdf::find_child(header_of(after), "Schema 0");
    OA_CHECK(tdf::child_count(tdf::find_child(gone, "units")) == 0);
    OA_CHECK(tdf::child_count(tdf::find_child(gone, "features")) == 0);
    OA_CHECK(start_count(gone) == 2);
    bool removed_unit = false;
    for (const mutators::JournalEntry& entry : removed.journal.entries) {
        if (entry.change == mutators::Change::removed && entry.field == "unitname")
            removed_unit = true;
    }
    OA_CHECK(removed_unit);

    const auto refuse = [&](const char* id, const char* body, const char* fragment) {
        std::string script = "function mutate(ctx)\n  return { maps = function(map, ctx)\n    ";
        script += body;
        script += "\n  end }\nend\n";
        expect_refused(run_one(store, scratch, id, "[\"maps\"]", script, valley), fragment);
    };
    refuse(
        "ridge-no-unit", "map.schemas[1].units[1].unitname = \"NOPE\"", "the unit NOPE is not there"
    );
    refuse(
        "ridge-no-feature",
        "map.schemas[1].features[1].featurename = \"Moss\"",
        "the feature Moss is not there"
    );
    refuse(
        "ridge-xpos",
        "map.schemas[1].units[1].xpos = 32768",
        "a placed unit's xpos must be a whole number from 0 to 32767"
    );
    refuse(
        "ridge-player",
        "map.schemas[1].units[1].player = 0",
        "a placed unit's player must be a whole number from 1 to 255"
    );
    refuse(
        "ridge-add-unit", "map.schemas[1].units[2] = \"RGCOM\"", "a placed unit cannot be added"
    );
    refuse(
        "ridge-add-feature",
        "map.schemas[1].features[2] = \"Tree\"",
        "a placed feature cannot be added"
    );
    refuse("ridge-add-schema", "map.schemas[3] = { }", "a schema cannot be added");
    refuse(
        "ridge-feature-pos",
        "map.schemas[1].features[1].xpos = 1",
        "a placed feature's xpos cannot change"
    );
}

void test_table(oa::AssetStore& store, const fs::path& scratch) {
    const mutators::ChosenMap valley = chosen_file(store, "ridge-valley", "");
    const mutators::ChosenMap plain = chosen_file(store, "ridgevalley", "");
    const mutators::ChosenMap other = chosen_file(store, "other-valley", "");
    const mutators::ChosenMap lower = chosen_file(store, "lower-valley", "");
    const char* name_glob = R"(function mutate(ctx)
  return { maps = { where { name = glob("Ridge*") }:change { gravity = 41 } } }
end
)";
    const mutators::PipelineResult named =
        run_one(store, scratch, "ridge-glob-name", "[\"maps\"]", name_glob, valley);
    OA_CHECK(named.playable());
    tdf::OwnedDocument named_document;
    OA_CHECK(named_document.parse(served_text(named, valley.ota_path), nullptr));
    OA_CHECK(std::string(field_of(header_of(named_document), "gravity")) == "41");
    const mutators::PipelineResult unnamed =
        run_one(store, scratch, "ridge-glob-miss", "[\"maps\"]", name_glob, other);
    OA_CHECK(unnamed.playable());
    OA_CHECK(served_entry(unnamed, other.ota_path) == nullptr);

    const char* file_glob = R"(function mutate(ctx)
  return { maps = { where { file = glob("ridge?valley") }:change { gravity = 42 } } }
end
)";
    const mutators::PipelineResult filed =
        run_one(store, scratch, "ridge-glob-file", "[\"maps\"]", file_glob, valley);
    OA_CHECK(filed.playable());
    tdf::OwnedDocument filed_document;
    OA_CHECK(filed_document.parse(served_text(filed, valley.ota_path), nullptr));
    OA_CHECK(std::string(field_of(header_of(filed_document), "gravity")) == "42");
    const mutators::PipelineResult unfiled =
        run_one(store, scratch, "ridge-glob-file-miss", "[\"maps\"]", file_glob, plain);
    OA_CHECK(unfiled.playable());
    OA_CHECK(served_entry(unfiled, plain.ota_path) == nullptr);

    const char* pattern = R"(function mutate(ctx)
  return { maps = { where { name = pattern("^Ridge") }:change { gravity = 43 } } }
end
)";
    const mutators::PipelineResult matched =
        run_one(store, scratch, "ridge-pattern", "[\"maps\"]", pattern, valley);
    OA_CHECK(matched.playable());
    tdf::OwnedDocument matched_document;
    OA_CHECK(matched_document.parse(served_text(matched, valley.ota_path), nullptr));
    OA_CHECK(std::string(field_of(header_of(matched_document), "gravity")) == "43");
    const mutators::PipelineResult missed =
        run_one(store, scratch, "ridge-pattern-miss", "[\"maps\"]", pattern, lower);
    OA_CHECK(missed.playable());
    OA_CHECK(served_entry(missed, lower.ota_path) == nullptr);

    const char* colour = R"(function mutate(ctx)
  return { maps = { where { name = "Ridge Valley" }:change { colour = 1 } } }
end
)";
    const mutators::PipelineResult bad =
        run_one(store, scratch, "ridge-colour", "[\"maps\"]", colour, valley);
    expect_refused(bad, "the maps change \"colour\" is not a header or schema setting");
    OA_CHECK(bad.mutators.front().cause == mutators::Cause::invalid_plan);

    const char* planet = R"(function mutate(ctx)
  return { maps = { where { file = "ridge-valley" }:change { planet = "Lava" } } }
end
)";
    expect_refused(
        run_one(store, scratch, "ridge-table-planet", "[\"maps\"]", planet, valley),
        "map.header.planet cannot change"
    );

    const char* units = R"(function mutate(ctx)
  return { rules = { where { name = glob("RG*") }:change { maxvelocity = 2 } } }
end
)";
    const mutators::PipelineResult ruled =
        run_one(store, scratch, "ridge-glob-rule", "[\"units\"]", units, valley);
    expect_refused(ruled, "glob() and pattern() match maps only");
    OA_CHECK(ruled.mutators.front().cause == mutators::Cause::unreadable_rules);
}

void test_refusal_loops(
    oa::AssetStore& store, const fs::path& scratch, const mutators::ChosenMap& valley
) {
    // Every refusal is an error the script's own pcall catches. Nothing of a
    // refused write stays behind, and the hook still writes afterwards.
    const char* lua = R"(function mutate(ctx)
  return { maps = function(map, ctx)
    local long = ("x"):rep(1000)
    local schema = map.schemas[1]
    local unit = schema.units[1]
    local feature = schema.features[1]
    local refusals = {
      function() map.header.planet = "Lava" end,
      function() map.header[long] = 1 end,
      function() map.header.gravity = "high" end,
      function() schema.surfacemetal = "not a number at all" end,
      function() schema.type = "Easy" end,
      function() schema[long] = 1 end,
      function() unit.unitname = "A:B" end,
      function() unit.unitname = ("A"):rep(60000) end,
      function() unit.unitname = "NO_SUCH_UNIT" end,
      function() unit.xpos = -1 end,
      function() unit[long] = 1 end,
      function() feature.featurename = "Moss" end,
      function() map.schemas:set { foo = 1 } end,
      function() map.schemas:set { aiprofile = "MISSING" } end,
      function() map.schemas[3] = { } end,
    }
    for round = 1, 200 do
      for _, refused in ipairs(refusals) do
        if pcall(refused) then error("a refused write was kept") end
      end
    end
    local ok, message = pcall(function() unit.unitname = "A:B" end)
    if ok or not string.find(message, "the unit A:B is not there", 1, true) then
      error("unexpected: " .. tostring(message))
    end
    map.header.gravity = 33
  end }
end
)";
    const mutators::PipelineResult result =
        run_one(store, scratch, "ridge-refusals", "[\"maps\"]", lua, valley);
    OA_CHECK(result.playable());
    if (!result.playable() && !result.mutators.empty())
        std::fprintf(stderr, "refusals: %s\n", result.mutators.front().message.c_str());
    tdf::OwnedDocument document;
    OA_CHECK(document.parse(served_text(result, valley.ota_path), nullptr));
    OA_CHECK(
        std::string(
            field_of(header_of(document), "gravity") ? field_of(header_of(document), "gravity") : ""
        ) == "33"
    );
    const tdf::Block* unit = tdf::child_at(
        tdf::find_child(tdf::find_child(header_of(document), "Schema 0"), "units"), 0
    );
    OA_CHECK(std::string(field_of(unit, "Unitname") ? field_of(unit, "Unitname") : "") == "RGCOM");
    OA_CHECK(result.journal.entries.size() == 1);
}

/// Real time each phase of a budget case may run. These cases are about the
/// instruction allowance; the real-time limit has its own test, and a busy
/// machine must not turn a budget stop into a time stop here.
constexpr std::chrono::minutes k_budget_time_limit{10};

/// Most processor time a loop the allowance stops may spend. The heaviest
/// takes under 0.1 s on a fast machine, so a slow one has room. A loop the
/// allowance does not stop fails the cause check when the real-time limit
/// stops it.
constexpr double k_budget_stop_seconds = 20.0;

/// Runs one maps script with a small instruction allowance. The allowance
/// is large enough that the loop's own Lua does not spend it, and small
/// enough that the charged map work does, well before the long real-time
/// limit the case runs under.
void expect_budget_stop(
    oa::AssetStore& store,
    const fs::path& scratch,
    std::string_view id,
    std::string_view body,
    const mutators::ChosenMap& valley
) {
    release(store);
    std::string lua = "function mutate(ctx)\n  return { maps = function(map, ctx)\n";
    lua += body;
    lua += "\n  end }\nend\n";
    const fs::path package = scratch / ("pkg-budget-" + std::string(id));
    write_package(package, id, "[\"maps\"]", lua);
    mutators::PipelineInput input = make_input({load_mutator(package)});
    input.map = valley;
    input.budget.instructions = 1000000;
    input.time_limit = k_budget_time_limit;
    // The bound is on the processor time this test spends, so other programs
    // sharing the machine do not stretch it. The real time is printed too.
    const oa::test::WorkTimer timer;
    const mutators::PipelineResult result = mutators::run_pipeline(store, input);
    const oa::test::WorkTime took = timer.elapsed();
    // A processor time the system cannot give fails the bound.
    const double spent_seconds = took.processor_seconds.value_or(k_budget_stop_seconds);
    std::printf(
        "%.*s stopped in %lld ms (%.0f ms of processor time)\n",
        static_cast<int>(id.size()),
        id.data(),
        static_cast<long long>(took.real.count()),
        spent_seconds * 1000.0
    );
    OA_CHECK(!result.playable());
    OA_CHECK(!result.mutators.empty());
    if (!result.mutators.empty()) {
        const bool budget = result.mutators.front().cause == mutators::Cause::instruction_budget;
        OA_CHECK(budget);
        if (!budget)
            std::fprintf(
                stderr,
                "%.*s: %s\n",
                static_cast<int>(id.size()),
                id.data(),
                result.mutators.front().message.c_str()
            );
    }
    OA_CHECK(took.processor_seconds.has_value());
    OA_CHECK(spent_seconds < k_budget_stop_seconds);
    OA_CHECK(served_entry(result, valley.ota_path) == nullptr);
    release(store);
}

void test_budgets(
    oa::AssetStore& store, const fs::path& scratch, const mutators::ChosenMap& valley
) {
    // A field access costs the lookup and its key's bytes, so a loop of long
    // keys spends the allowance after a few hundred reads.
    expect_budget_stop(
        store,
        scratch,
        "ridge-budget-read",
        "    local key = (\"x\"):rep(65536)\n"
        "    for i = 1, 20000 do local value = map.header[key] end",
        valley
    );
    expect_budget_stop(
        store,
        scratch,
        "ridge-budget-refuse",
        "    local key = (\"x\"):rep(65536)\n"
        "    for i = 1, 20000 do pcall(function() map.header[key] = 1 end) end",
        valley
    );
    expect_budget_stop(
        store,
        scratch,
        "ridge-budget-write",
        "    for i = 1, 1000000 do map.header.gravity = 5 end",
        valley
    );
    expect_budget_stop(
        store,
        scratch,
        "ridge-budget-set",
        "    for i = 1, 1000000 do\n"
        "      map.schemas:set { humanmetal = 5, humanenergy = 5, computermetal = 5,\n"
        "        computerenergy = 5, surfacemetal = 5 }\n"
        "    end",
        valley
    );
    expect_budget_stop(
        store,
        scratch,
        "ridge-budget-unit",
        "    local unit = map.schemas[1].units[1]\n"
        "    for i = 1, 1000000 do pcall(function() unit.unitname = \"NOPE\" end) end",
        valley
    );

    // Each write is journalled in constant time, so many writes stay quick:
    // within 5 s of this program's processor time, which other programs
    // sharing the machine do not stretch. The long real-time limit keeps such
    // a machine from stopping the run; the real time is printed too.
    const char* many = R"(function mutate(ctx)
  return { maps = function(map, ctx)
    for i = 1, 40000 do map.header.gravity = i % 2 end
  end }
end
)";
    release(store);
    const fs::path many_package = scratch / "pkg-many-writes";
    write_package(many_package, "ridge-many-writes", "[\"maps\"]", many);
    mutators::PipelineInput many_input = make_input({load_mutator(many_package)});
    many_input.map = valley;
    many_input.time_limit = k_budget_time_limit;
    const oa::test::WorkTimer timer;
    const mutators::PipelineResult written = mutators::run_pipeline(store, many_input);
    const oa::test::WorkTime took = timer.elapsed();
    constexpr double many_writes_seconds = 5.0;
    const double spent_seconds = took.processor_seconds.value_or(many_writes_seconds);
    std::printf(
        "ridge-many-writes wrote in %lld ms (%.0f ms of processor time)\n",
        static_cast<long long>(took.real.count()),
        spent_seconds * 1000.0
    );
    OA_CHECK(written.playable());
    OA_CHECK(took.processor_seconds.has_value());
    OA_CHECK(spent_seconds < many_writes_seconds);
    std::size_t rows = 0;
    for (const mutators::JournalEntry& entry : written.journal.entries) {
        if (entry.field == "gravity")
            ++rows;
    }
    OA_CHECK(rows == 40000);

    // Past the write limit the mutator fails, even when the script catches
    // the error.
    const char* too_many = R"(function mutate(ctx)
  return { maps = function(map, ctx)
    for i = 1, 70000 do map.header.gravity = i % 2 end
  end }
end
)";
    const mutators::PipelineResult over =
        run_one(store, scratch, "ridge-too-many", "[\"maps\"]", too_many, valley);
    expect_refused(over, "this mutator has changed too many fields");
    const char* caught = R"(function mutate(ctx)
  return { maps = function(map, ctx)
    for i = 1, 70000 do pcall(function() map.header.gravity = i % 2 end) end
  end }
end
)";
    const mutators::PipelineResult kept =
        run_one(store, scratch, "ridge-too-many-caught", "[\"maps\"]", caught, valley);
    expect_refused(kept, "this mutator has changed too many fields");
}

void test_hook_changed_names(
    oa::AssetStore& store, const fs::path& scratch, const mutators::ChosenMap& valley
) {
    // A placed name is checked against the data the mutators above left.
    const char* cloned = R"(function mutate(ctx)
  return {
    units = function(units, ctx)
      units:clone("RGCOM", "RGNEW", { })
    end,
    maps = function(map, ctx)
      map.schemas[1].units[1].unitname = "RGNEW"
    end
  }
end
)";
    const mutators::PipelineResult placed =
        run_one(store, scratch, "ridge-clone-place", "[\"units\", \"maps\"]", cloned, valley);
    OA_CHECK(placed.playable());
    if (!placed.playable() && !placed.mutators.empty())
        std::fprintf(stderr, "clone-place: %s\n", placed.mutators.front().message.c_str());
    tdf::OwnedDocument document;
    OA_CHECK(document.parse(served_text(placed, valley.ota_path), nullptr));
    const tdf::Block* unit = tdf::child_at(
        tdf::find_child(tdf::find_child(header_of(document), "Schema 0"), "units"), 0
    );
    OA_CHECK(std::string(field_of(unit, "Unitname") ? field_of(unit, "Unitname") : "") == "RGNEW");

    const char* removed = R"(function mutate(ctx)
  return {
    units = function(units, ctx)
      units.RGBOT:remove()
    end,
    maps = function(map, ctx)
      map.schemas[1].units[1].unitname = "RGBOT"
    end
  }
end
)";
    expect_refused(
        run_one(store, scratch, "ridge-remove-place", "[\"units\", \"maps\"]", removed, valley),
        "the unit RGBOT is not there"
    );

    const char* gone = R"(function mutate(ctx)
  return {
    units = function(units, ctx)
      ctx.features.Rock:remove()
    end,
    maps = function(map, ctx)
      map.schemas[1].features[1].featurename = "Rock"
    end
  }
end
)";
    expect_refused(
        run_one(
            store,
            scratch,
            "ridge-feature-gone",
            "[\"units\", \"features\", \"maps\"]",
            gone,
            valley
        ),
        "the feature Rock is not there"
    );
}

/// ctx.incompatible in a maps function, even inside pcall, ends that
/// mutator as incompatible and keeps none of its map writes.
void test_declared_in_maps(
    oa::AssetStore& store, const fs::path& scratch, const mutators::ChosenMap& valley
) {
    const char* earlier = R"(function mutate(ctx)
  return { maps = function(map, ctx)
    map.header.gravity = 10
  end }
end
)";
    const char* declares = R"(function mutate(ctx)
  return { maps = function(map, ctx)
    map.header.gravity = 20
    pcall(function() ctx.incompatible("needs a lava map") end)
  end }
end
)";
    std::vector<std::pair<std::string, std::string>> scripts;
    scripts.emplace_back("ridge-earlier", earlier);
    scripts.emplace_back("ridge-declares", declares);
    const mutators::PipelineResult result =
        run_list(store, scratch, std::move(scripts), "[\"maps\"]", valley);
    OA_CHECK(result.mutators.size() == 2);
    if (result.mutators.size() != 2)
        return;
    OA_CHECK(result.mutators[0].outcome == mutators::Outcome::applied);
    OA_CHECK(result.mutators[1].outcome == mutators::Outcome::incompatible);
    OA_CHECK(result.mutators[1].cause == mutators::Cause::declared);
    OA_CHECK(result.mutators[1].phase == mutators::Phase::hooks);
    OA_CHECK(result.mutators[1].message == "needs a lava map");
    if (result.mutators[1].message != "needs a lava map")
        std::fprintf(stderr, "declared: %s\n", result.mutators[1].message.c_str());
    tdf::OwnedDocument document;
    OA_CHECK(document.parse(served_text(result, valley.ota_path), nullptr));
    const char* gravity = field_of(header_of(document), "gravity");
    OA_CHECK(gravity != nullptr && std::string(gravity) == "10");
    const oa::MutatorLayerEntry* entry = served_entry(result, valley.ota_path);
    OA_CHECK(entry != nullptr && entry->owner == "ridge-earlier");
}

void test_order_and_failure(
    oa::AssetStore& store, const fs::path& scratch, const mutators::ChosenMap& valley
) {
    const char* earlier = R"(function mutate(ctx)
  return { maps = function(map, ctx)
    map.header.gravity = 10
  end }
end
)";
    const char* later = R"(function mutate(ctx)
  return { maps = function(map, ctx)
    map.header.gravity = 20
  end }
end
)";
    std::vector<std::pair<std::string, std::string>> scripts;
    scripts.emplace_back("ridge-earlier", earlier);
    scripts.emplace_back("ridge-later", later);
    const mutators::PipelineResult result =
        run_list(store, scratch, std::move(scripts), "[\"maps\"]", valley);
    OA_CHECK(result.playable());
    tdf::OwnedDocument document;
    OA_CHECK(document.parse(served_text(result, valley.ota_path), nullptr));
    OA_CHECK(std::string(field_of(header_of(document), "gravity")) == "20");
    const oa::MutatorLayerEntry* entry = served_entry(result, valley.ota_path);
    OA_CHECK(entry != nullptr && entry->owner == "ridge-later");
    int sets = 0;
    bool saw_ten = false;
    bool saw_twenty = false;
    for (const mutators::JournalEntry& row : result.journal.entries) {
        if (row.field != "gravity" || row.change != mutators::Change::set)
            continue;
        ++sets;
        OA_CHECK(row.record == "map:ridge-valley");
        OA_CHECK(row.collection == mutators::Collection::other);
        OA_CHECK(row.phase == mutators::Phase::hooks);
        if (row.new_text == "10")
            saw_ten = true;
        if (row.new_text == "20")
            saw_twenty = true;
    }
    OA_CHECK(sets == 2);
    OA_CHECK(saw_ten && saw_twenty);
    bool noted = false;
    for (const mutators::Conflict& conflict : result.report.conflicts) {
        if (conflict.kind == mutators::ConflictKind::same_field_hooked &&
            conflict.field == "gravity")
            noted = true;
    }
    OA_CHECK(noted);

    const char* boom = R"(function mutate(ctx)
  return { maps = function(map, ctx)
    map.header.gravity = 10
    error("boom")
  end }
end
)";
    const mutators::PipelineResult failed =
        run_one(store, scratch, "ridge-boom", "[\"maps\"]", boom, valley);
    expect_refused(failed, "boom");
    OA_CHECK(failed.journal.entries.empty());
}

void test_output(
    oa::AssetStore& store, const fs::path& scratch, const mutators::ChosenMap& valley
) {
    const char* same = R"(function mutate(ctx)
  return { maps = function(map, ctx)
    map.header.gravity = 112
  end }
end
)";
    const mutators::PipelineResult unchanged =
        run_one(store, scratch, "ridge-same", "[\"maps\"]", same, valley);
    OA_CHECK(unchanged.playable());
    OA_CHECK(served_entry(unchanged, valley.ota_path) == nullptr);

    const char* pack_lua = R"(function mutate(ctx)
  return { maps = function(map, ctx)
    if map.pack ~= "ridge-maps" then error("pack") end
    if map.file ~= "ridge-valley@ridge-maps" then error("file") end
    map.header.gravity = 60
  end }
end
)";
    const mutators::ChosenMap packed =
        chosen_from("ridge-valley@ridge-maps", "ridge-maps", *valley.ota);
    const mutators::PipelineResult packed_result =
        run_one(store, scratch, "ridge-pack", "[\"maps\"]", pack_lua, packed);
    OA_CHECK(packed_result.playable());
    if (!packed_result.playable() && !packed_result.mutators.empty())
        std::fprintf(stderr, "pack: %s\n", packed_result.mutators.front().message.c_str());
    OA_CHECK(served_entry(packed_result, packed.ota_path) != nullptr);

    const char* twice = R"(function mutate(ctx)
  return { maps = function(map, ctx)
    map.header.gravity = 77
    map.schemas:set { surfacemetal = 9 }
  end }
end
)";
    const mutators::PipelineResult first =
        run_one(store, scratch, "ridge-twice", "[\"maps\"]", twice, valley);
    const mutators::PipelineResult second =
        run_one(store, scratch, "ridge-twice", "[\"maps\"]", twice, valley);
    OA_CHECK(first.playable() && second.playable());
    const oa::MutatorLayerEntry* left = served_entry(first, valley.ota_path);
    const oa::MutatorLayerEntry* right = served_entry(second, valley.ota_path);
    OA_CHECK(left != nullptr && right != nullptr);
    if (left != nullptr && right != nullptr)
        OA_CHECK(*left->contents == *right->contents);
}

void test_session(
    oa::AssetStore& store, const fs::path& scratch, const mutators::ChosenMap& valley
) {
    const char* lua = R"(function mutate(ctx)
  return { maps = function(map, ctx)
    map.header.gravity = 64
    map.schemas:set { aiprofile = "TURBO", humanmetal = 4242, humanenergy = 4343, computermetal = 4444, computerenergy = 4545, surfacemetal = 12 }
  end }
end
)";
    const mutators::PipelineResult result =
        run_one(store, scratch, "ridge-session", "[\"maps\"]", lua, valley);
    OA_CHECK(result.playable());
    const std::string text = served_text(result, valley.ota_path);
    OA_CHECK(!text.empty());
    MemoryFiles files;
    files.files["maps/ridge-valley.ota"] = text;
    files.files["ai/turbo.txt"] = "profile=turbo\n";
    missions::CampaignFile session{};
    OA_CHECK(load_session(files, session, "ridge-valley"));
    OA_CHECK(session.gravity == 64);
    OA_CHECK(session.metal[0] == 4242.f);
    OA_CHECK(session.energy[0] == 4343.f);
    OA_CHECK(session.metal[1] == 4444.f);
    OA_CHECK(session.energy[1] == 4545.f);
    OA_CHECK(session.surface_metal == 12);
    const char* profile = missions::campaign_path(&session, missions::CampaignPath::ai_profile);
    OA_CHECK(profile != nullptr);
    if (profile != nullptr)
        OA_CHECK(
            std::string(profile).find("TURBO") != std::string::npos ||
            std::string(profile).find("turbo") != std::string::npos
        );
    missions::campaign_file_free(&session);
}

void test_package(oa::AssetStore& store, const mutators::ChosenMap& valley) {
    release(store);
    mutators::PipelineInput input = make_input({load_mutator(k_data / "packages" / "turbo-maps")});
    input.map = valley;
    const mutators::PipelineResult result = mutators::run_pipeline(store, input);
    OA_CHECK(result.playable());
    if (!result.playable() && !result.mutators.empty())
        std::fprintf(stderr, "turbo-maps: %s\n", result.mutators.front().message.c_str());
    tdf::OwnedDocument document;
    OA_CHECK(document.parse(served_text(result, valley.ota_path), nullptr));
    OA_CHECK(std::string(field_of(header_of(document), "gravity")) == "50");
    const tdf::Block* schema = tdf::find_child(header_of(document), "Schema 0");
    OA_CHECK(
        std::string(field_of(schema, "aiprofile") ? field_of(schema, "aiprofile") : "") ==
        "TURBOTEST"
    );
    bool profile_file = false;
    for (const oa::MutatorLayerEntry& entry : result.layer.entries) {
        if (entry.path == "ai/TURBOTEST.txt" || entry.path == "ai/turbotest.txt")
            profile_file = true;
    }
    OA_CHECK(profile_file);
    release(store);
}

std::string stem_of(std::string_view path) {
    const auto slash = path.find_last_of('/');
    std::string name(slash == std::string_view::npos ? path : path.substr(slash + 1));
    const auto dot = name.find_last_of('.');
    if (dot != std::string::npos)
        name.resize(dot);
    return name;
}

bool profile_stem(std::string_view name) {
    if (name.empty() || name.size() > 31)
        return false;
    for (const char piece : name) {
        const bool letter = (piece >= 'A' && piece <= 'Z') || (piece >= 'a' && piece <= 'z');
        const bool digit = piece >= '0' && piece <= '9';
        if (!letter && !digit && piece != '_' && piece != '-')
            return false;
    }
    return true;
}

std::string schema_profile(const tdf::Block* header) {
    for (int number = 0; number < 64; ++number) {
        const std::string section = "Schema " + std::to_string(number);
        const tdf::Block* schema = tdf::find_child(header, section.c_str());
        if (schema == nullptr)
            break;
        const char* profile = field_of(schema, "aiprofile");
        if (profile != nullptr && profile[0] != '\0')
            return profile;
    }
    return {};
}

void test_installed(oa::AssetStore& store) {
    const Scratch scratch("D07-maps-data");
    const char* reading = R"(function mutate(ctx)
  return { maps = function(map, ctx)
    local gravity = map.header.gravity
    local count = #map.schemas
    if count >= 1 then
      local schema = map.schemas[1]
      local kind = schema.type
      local players = schema.players
      local profile = schema.aiprofile
      if kind == nil and players < 0 and profile == nil and gravity == nil then error("unread") end
    end
  end }
end
)";
    write_package(scratch.path / "reader", "ridge-reader", "[\"maps\"]", reading);
    const mutators::PipelineMutator reader = load_mutator(scratch.path / "reader");
    const std::vector<std::string> maps = store.list_effective_in_mount_order("maps", "ota", true);
    int read = 0;
    for (const std::string& path : maps) {
        oa::AssetData data = store.read(path);
        const std::string text(data.bytes.begin(), data.bytes.end());
        tdf::OwnedDocument document;
        if (text.empty() || !document.parse(text, nullptr) || header_of(document) == nullptr)
            continue;
        ++read;
        const std::string key = stem_of(path);
        release(store);
        mutators::PipelineInput input = make_input({reader});
        input.map = chosen_from(key, "", data.bytes);
        const mutators::PipelineResult result = mutators::run_pipeline(store, input);
        OA_CHECK(result.playable());
        if (!result.playable() && !result.mutators.empty())
            std::fprintf(stderr, "%s: %s\n", key.c_str(), result.mutators.front().message.c_str());
        OA_CHECK(served_entry(result, "maps/" + key + ".ota") == nullptr);
        release(store);
    }
    OA_CHECK(read >= 1);

    std::vector<std::string> profiles;
    for (const std::string& path : store.list_effective_in_mount_order("ai", "txt", true)) {
        const std::string stem = stem_of(path);
        if (profile_stem(stem))
            profiles.push_back(stem);
    }
    OA_CHECK(profiles.size() >= 2);

    // Every map whose schemas name a profile is given another profile and
    // new resources. A refused or unserved rewrite fails. A map whose
    // installed text loads as a skirmish session must load the rewrite too,
    // with the new profile; a campaign mission does not load as one.
    int rewritten = 0;
    int loaded = 0;
    for (const std::string& path : maps) {
        if (profiles.size() < 2)
            break;
        oa::AssetData data = store.read(path);
        const std::string text(data.bytes.begin(), data.bytes.end());
        tdf::OwnedDocument document;
        if (text.empty() || !document.parse(text, nullptr) || header_of(document) == nullptr)
            continue;
        const std::string current = schema_profile(header_of(document));
        if (current.empty())
            continue;
        std::string replacement;
        for (const std::string& stem : profiles) {
            if (fold_path(stem) == fold_path(current))
                continue;
            replacement = stem;
            break;
        }
        const std::string key = stem_of(path);
        std::string lua = "function mutate(ctx)\n  return { maps = function(map, ctx)\n";
        lua += "    map.schemas:set { aiprofile = \"" + replacement + "\", humanmetal = 4242 }\n";
        lua += "  end }\nend\n";
        write_package(scratch.path / "writer", "ridge-writer", "[\"maps\"]", lua);
        release(store);
        mutators::PipelineInput input = make_input({load_mutator(scratch.path / "writer")});
        input.map = chosen_from(key, "", std::move(data.bytes));
        const mutators::PipelineResult result = mutators::run_pipeline(store, input);
        release(store);
        OA_CHECK(result.playable());
        if (!result.playable()) {
            if (!result.mutators.empty())
                std::fprintf(
                    stderr, "%s: %s\n", key.c_str(), result.mutators.front().message.c_str()
                );
            continue;
        }
        const std::string served = served_text(result, "maps/" + key + ".ota");
        OA_CHECK(!served.empty());
        if (served.empty()) {
            std::fprintf(stderr, "%s: the rewrite was not served\n", key.c_str());
            continue;
        }
        ++rewritten;

        MemoryFiles installed;
        installed.files[fold_path("maps/" + key + ".ota")] = text;
        installed.files[fold_path("ai/" + current + ".txt")] = "profile=installed\n";
        installed.files["ai/default.txt"] = "profile=default\n";
        missions::CampaignFile before{};
        const missions::CampaignFiles before_services = memory_services(installed);
        missions::CampaignEnv before_env{&before_services, nullptr, 0, 0};
        missions::campaign_file_construct(&before, missions::SessionKind::skirmish, &before_env);
        const bool skirmish =
            missions::campaign_load_mission_info(&before, &before_env, key.c_str());
        missions::campaign_file_free(&before);
        if (!skirmish)
            continue;

        MemoryFiles files;
        files.files[fold_path("maps/" + key + ".ota")] = served;
        files.files[fold_path("ai/" + replacement + ".txt")] = "profile=rewrite\n";
        missions::CampaignFile session{};
        const missions::CampaignFiles services = memory_services(files);
        missions::CampaignEnv env{&services, nullptr, 0, 0};
        missions::campaign_file_construct(&session, missions::SessionKind::skirmish, &env);
        const bool read_back = missions::campaign_load_mission_info(&session, &env, key.c_str());
        OA_CHECK(read_back);
        if (!read_back) {
            std::fprintf(stderr, "%s: the rewrite does not load\n", key.c_str());
            missions::campaign_file_free(&session);
            continue;
        }
        const char* profile = missions::campaign_path(&session, missions::CampaignPath::ai_profile);
        const bool named = profile != nullptr &&
                           fold_path(profile).find(fold_path(replacement)) != std::string::npos;
        OA_CHECK(named);
        OA_CHECK(session.metal[0] == 4242.f);
        if (!named || session.metal[0] != 4242.f)
            std::fprintf(stderr, "%s: the rewrite reads back wrong\n", key.c_str());
        else
            ++loaded;
        missions::campaign_file_free(&session);
    }
    std::printf("maps-data: %d rewritten, %d read back as a session\n", rewritten, loaded);
    OA_CHECK(rewritten >= 1);
    OA_CHECK(loaded >= 1);
}

/// One tally for the mutator. A caught write past the limit fails it, and
/// unit writes and map writes add up. A count under the limit is kept.
void test_shared_write_limit(
    oa::AssetStore& store, const fs::path& scratch, const mutators::ChosenMap& valley
) {
    const char* caught_units = R"(function mutate(ctx)
  return { units = function(units, ctx)
    local commander = units.RGCOM
    for i = 1, 70000 do
      pcall(function() commander.maxdamage = (i % 2) + 1 end)
    end
  end }
end
)";
    const mutators::PipelineResult caught =
        run_one(store, scratch, "ridge-caught-units", "[\"units\"]", caught_units, valley);
    expect_refused(caught, "this mutator has changed too many fields");

    const char* both = R"(function mutate(ctx)
  return {
    units = function(units, ctx)
      local commander = units.RGCOM
      for i = 1, 40000 do
        commander.maxdamage = (i % 2) + 1
      end
    end,
    maps = function(map, ctx)
      for i = 1, 30000 do
        pcall(function() map.header.gravity = i % 2 end)
      end
    end
  }
end
)";
    const mutators::PipelineResult crossed =
        run_one(store, scratch, "ridge-units-and-maps", "[\"units\", \"maps\"]", both, valley);
    expect_refused(crossed, "this mutator has changed too many fields");

    const char* within = R"(function mutate(ctx)
  return { units = function(units, ctx)
    local commander = units.RGCOM
    for i = 1, 60000 do
      commander.maxdamage = (i % 2) + 1
    end
  end }
end
)";
    const mutators::PipelineResult kept =
        run_one(store, scratch, "ridge-within-limit", "[\"units\"]", within, valley);
    OA_CHECK(kept.playable());
    std::size_t rows = 0;
    for (const mutators::JournalEntry& entry : kept.journal.entries) {
        if (entry.field == "maxdamage")
            ++rows;
    }
    OA_CHECK(rows == 60000);
}

/// Counts the journal rows whose field name starts with `prefix`.
std::size_t rows_of(const mutators::PipelineResult& result, std::string_view prefix) {
    std::size_t rows = 0;
    for (const mutators::JournalEntry& entry : result.journal.entries) {
        if (entry.field.rfind(prefix, 0) == 0)
            ++rows;
    }
    return rows;
}

/// A clone's fields count on the mutator's tally, and the copy itself does
/// not. A clone refused at the limit fails the mutator with the limit's
/// message, even when the script catches the error.
void test_clone_write_tally(
    oa::AssetStore& store, const fs::path& scratch, const mutators::ChosenMap& valley
) {
    // 100 clones of 40 fields are 4,000 writes, so the 61,537th unit write passes the limit.
    const char* clones_first = R"(function mutate(ctx)
  return { units = function(units, ctx)
    local fields = {}
    for k = 1, 40 do fields["extra" .. k] = k end
    for i = 1, 100 do units:clone("RGCOM", "RGC" .. i, fields) end
    local commander = units.RGCOM
    for i = 1, 62000 do
      commander.maxdamage = (i % 2) + 1
    end
  end }
end
)";
    const mutators::PipelineResult crossed =
        run_one(store, scratch, "ridge-clones-then-writes", "[\"units\"]", clones_first, valley);
    expect_refused(crossed, "this mutator has changed too many fields");

    const char* exact = R"(function mutate(ctx)
  return { units = function(units, ctx)
    local fields = {}
    for k = 1, 40 do fields["extra" .. k] = k end
    for i = 1, 100 do units:clone("RGCOM", "RGC" .. i, fields) end
    local commander = units.RGCOM
    for i = 1, 61536 do
      commander.maxdamage = (i % 2) + 1
    end
  end }
end
)";
    const mutators::PipelineResult kept =
        run_one(store, scratch, "ridge-clones-at-limit", "[\"units\"]", exact, valley);
    OA_CHECK(kept.playable());
    OA_CHECK(rows_of(kept, "extra") == 4000);
    OA_CHECK(rows_of(kept, "maxdamage") == 61536);

    const char* clones_caught = R"(function mutate(ctx)
  return { units = function(units, ctx)
    local commander = units.RGCOM
    for i = 1, 65500 do
      commander.maxdamage = (i % 2) + 1
    end
    local fields = {}
    for k = 1, 40 do fields["extra" .. k] = k end
    for i = 1, 20 do
      pcall(function() units:clone("RGCOM", "RGC" .. i, fields) end)
    end
  end }
end
)";
    const mutators::PipelineResult caught =
        run_one(store, scratch, "ridge-caught-clones", "[\"units\"]", clones_caught, valley);
    expect_refused(caught, "this mutator has changed too many fields");
}

/// True when the layer serves a file that holds `text`.
bool layer_holds(const mutators::PipelineResult& result, std::string_view text) {
    for (const oa::MutatorLayerEntry& entry : result.layer.entries) {
        if (entry.contents == nullptr)
            continue;
        const std::string_view bytes(
            reinterpret_cast<const char*>(entry.contents->data()), entry.contents->size()
        );
        if (bytes.find(text) != std::string_view::npos)
            return true;
    }
    return false;
}

/// A collection a script kept from its units hook writes nothing from its
/// maps hook. The maps phase counts on the mutator's tally but grants no
/// record write.
void test_kept_collections_in_maps(
    oa::AssetStore& store, const fs::path& scratch, const mutators::ChosenMap& valley
) {
    const char* kept_unit = R"(local kept
function mutate(ctx)
  return {
    units = function(units, ctx)
      kept = units.RGCOM
    end,
    maps = function(map, ctx)
      kept.maxdamage = 4242
      map.header.gravity = 7
    end
  }
end
)";
    const mutators::PipelineResult refused =
        run_one(store, scratch, "ridge-kept-unit", "[\"units\", \"maps\"]", kept_unit, valley);
    expect_refused(refused, "does not declare the units layer");
    OA_CHECK(refused.journal.entries.empty());
    OA_CHECK(!layer_holds(refused, "4242"));

    const char* kept_caught = R"(local kept
function mutate(ctx)
  return {
    units = function(units, ctx)
      kept = units.RGCOM
    end,
    maps = function(map, ctx)
      pcall(function() kept.maxdamage = 4242 end)
      map.header.gravity = 7
    end
  }
end
)";
    const mutators::PipelineResult caught =
        run_one(store, scratch, "ridge-kept-caught", "[\"units\", \"maps\"]", kept_caught, valley);
    OA_CHECK(caught.playable());
    OA_CHECK(served_entry(caught, valley.ota_path) != nullptr);
    OA_CHECK(!layer_holds(caught, "4242"));
    OA_CHECK(rows_of(caught, "maxdamage") == 0);
    OA_CHECK(rows_of(caught, "gravity") == 1);

    const char* kept_menus = R"(local kept
function mutate(ctx)
  return {
    units = function(units, ctx)
      kept = ctx.menus
    end,
    maps = function(map, ctx)
      kept:remove("RGBOT")
      map.header.gravity = 7
    end
  }
end
)";
    const mutators::PipelineResult menus = run_one(
        store, scratch, "ridge-kept-menus", "[\"units\", \"menus\", \"maps\"]", kept_menus, valley
    );
    expect_refused(menus, "does not declare the menus layer");
    OA_CHECK(menus.journal.entries.empty());
}

void synthetic_tests() {
    const Scratch scratch("D07-maps");
    write_store(scratch.path / "store");
    oa::AssetStore store(scratch.path / "store");
    const mutators::ChosenMap valley = chosen_file(store, "ridge-valley", "");
    test_header_reads(store, scratch.path, valley);
    test_header_writes(store, scratch.path, valley);
    test_header_refusals(store, scratch.path, valley);
    test_schemas(store, scratch.path, valley);
    test_placed(store, scratch.path, valley);
    test_table(store, scratch.path);
    test_refusal_loops(store, scratch.path, valley);
    test_budgets(store, scratch.path, valley);
    test_shared_write_limit(store, scratch.path, valley);
    test_clone_write_tally(store, scratch.path, valley);
    test_kept_collections_in_maps(store, scratch.path, valley);
    test_hook_changed_names(store, scratch.path, valley);
    test_order_and_failure(store, scratch.path, valley);
    test_declared_in_maps(store, scratch.path, valley);
    test_output(store, scratch.path, valley);
    test_session(store, scratch.path, valley);
    test_package(store, valley);
    release(store);
}

} // namespace

int main(int argc, char** argv) {
    if (oa::test::game_data_requested(argc, argv)) {
        oa::AssetStore store = oa::test::require_game_assets("map hooks");
        test_installed(store);
    } else {
        synthetic_tests();
    }
    return oa::test::check_exit_status();
}

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Side data and the other game-data tables, through made-up Ridge packages
// run by the pipeline. The installed-game case reads every side and every
// build list and writes nothing.
#include "oa/data/mutators/allow_list.hpp"
#include "oa/data/mutators/pipeline.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/test/check.hpp"
#include "oa/test/game_assets.hpp"
#include "oa/test/scratch_directory.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace mutators = oa::data::mutators;
namespace oamut = oa::formats::oamut;
namespace defs = oa::data::defs;
namespace fs = std::filesystem;

struct Scratch {
    fs::path path;

    explicit Scratch(std::string_view name) : path(oa::test::make_scratch_directory(name)) {}

    Scratch(const Scratch&) = delete;
    Scratch& operator=(const Scratch&) = delete;

    ~Scratch() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

void write_bytes(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    OA_CHECK(out.good());
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string fold_path(std::string_view path) {
    std::string folded(path);
    for (char& byte : folded) {
        if (byte == '\\')
            byte = '/';
        else if (byte >= 'A' && byte <= 'Z')
            byte = static_cast<char>(byte - 'A' + 'a');
    }
    return folded;
}

std::size_t count_of(std::string_view text, std::string_view needle) {
    std::size_t count = 0;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t found = text.find(needle, at);
        if (found == std::string_view::npos)
            break;
        ++count;
        at = found + needle.size();
    }
    return count;
}

bool contains(std::string_view text, std::string_view needle) {
    return text.find(needle) != std::string_view::npos;
}

void write_unit(const fs::path& root, std::string_view name, bool builder) {
    std::string text = "[UNITINFO]\n{\nUnitName=";
    text.append(name);
    text.append(";\n");
    if (builder)
        text.append("Builder=1;\n");
    text.append("}\n");
    write_bytes(root / "units" / (std::string(name) + ".fbi"), text);
}

void write_page(const fs::path& root, std::string_view prefix) {
    const char* pages[] = {"GEN.GUI", "MAIN2.GUI", "DL.GUI"};
    for (const char* page : pages)
        write_bytes(root / "guis" / (std::string(prefix) + page), "x\n");
}

struct Corpus {
    std::string side_name{"Ridge"};
    bool gap{false};
    bool variant{false};
};

void write_corpus(const fs::path& root, const Corpus& corpus) {
    write_unit(root, "RIDGECOM", false);
    write_unit(root, "RIDGEFAB", true);
    write_unit(root, "RIDGETANK", false);
    write_unit(root, "RIDGECLONE", false);
    write_unit(root, "RIDGEYARD", true);
    for (int index = 0; index <= 30; ++index) {
        char name[8]{};
        std::snprintf(name, sizeof name, "U%02d", index);
        write_unit(root, name, false);
    }
    write_bytes(root / "anims" / "RID.GAF", "x\n");
    write_bytes(root / "anims" / "RID2.GAF", "x\n");
    write_bytes(root / "fonts" / "RID.FNT", "x\n");
    write_bytes(root / "fonts" / "RID2.FNT", "x\n");
    write_page(root, "RDG");
    write_page(root, "ZZZ");

    std::string sides = "[SIDE0]\n{\nname=";
    sides.append(corpus.side_name);
    sides.append(
        ";\nnameprefix=RDG;\ncommander=RIDGECOM;\nintgaf=RID;\nfont=RID;\nfontgui=plain;\n"
        "[LOGO]\n{\nkeepme=1;\n}\n}\n"
        "[SIDE1]\n{\nname=Other;\nnameprefix=OTH;\ncommander=RIDGECOM;\nintgaf=RID;\nfont=RID;\n"
        "othermark=1;\n}\n"
        "[CANBUILD]\n{\n[RIDGEFAB]\n{\ncanbuild1=RIDGETANK;\n"
    );
    if (corpus.gap)
        sides.append("canbuild3=RIDGECLONE;\n");
    sides.append("}\n}\n");
    write_bytes(root / "gamedata" / "sidedata.tdf", sides);
    write_bytes(
        root / "gamedata" / "moveinfo.tdf",
        "[RIDGE]\n{\nspeed=1;\nsight=4;\n}\n[RIDGE]\n{\nspeed=9;\n}\n[LOSE]\n{\ngone=1;\n}\n"
    );
    write_bytes(root / "gamedata" / "los.tdf", "[RADAR]\n{\nrange=1;\n}\n");
    if (corpus.variant) {
        write_bytes(
            root / "gamedata-ridge" / "sidedata.tdf",
            "[SIDE0]\n{\nname=RidgeMark;\nnameprefix=RDG;\ncommander=RIDGECOM;\nintgaf=RID;\n"
            "font=RID;\n}\n"
            "[SIDE1]\n{\nname=Other;\nnameprefix=OTH;\ncommander=RIDGECOM;\n}\n"
            "[CANBUILD]\n{\n[RIDGEFAB]\n{\ncanbuild1=RIDGECLONE;\n}\n}\n"
        );
    }
}

mutators::PipelineMutator load_package(
    const fs::path& package, std::string_view id, std::string_view layers, std::string_view script
) {
    write_bytes(
        package / "oamut.yaml",
        std::string("oamut: 1\nid: ") + std::string(id) +
            "\nname: \"Ridge Sides\"\nversion: \"1.0\"\nauthor:\n  name: \"Ridge Maker\"\n"
            "tags: [\"gameplay\"]\nworks-with: [\"*\"]\nlayers: [" +
            std::string(layers) + "]\n"
    );
    write_bytes(package / "mutator.lua", script);
    mutators::PipelineMutator mutator;
    mutator.package = package;
    std::ifstream in(package / "oamut.yaml", std::ios::binary);
    OA_CHECK(in.good());
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    const std::vector<uint8_t> bytes(text.begin(), text.end());
    std::vector<oamut::Problem> problems;
    const bool read = oamut::read_manifest(bytes, mutator.manifest, problems);
    if (!read) {
        for (const oamut::Problem& problem : problems) {
            std::fprintf(
                stderr, "%s: %s: %s\n", id.data(), problem.key.c_str(), problem.message.c_str()
            );
        }
    }
    OA_CHECK(read);
    return mutator;
}

mutators::PipelineInput input_of(mutators::PipelineMutator mutator, std::string variant) {
    mutators::PipelineInput input;
    input.mutators.push_back(std::move(mutator));
    input.mod.id = "ridge";
    input.mod.version = "1.0";
    input.mod.game = "ridge";
    input.engine_version = "0.8.5";
    input.layout = &defs::data_layout();
    input.variant = std::move(variant);
    input.seed = 1;
    return input;
}

void release(oa::AssetStore& store) {
    if (store.mutator_layer_mounted())
        store.unmount_mutator_layer();
    OA_CHECK(!store.mutator_layer_mounted());
}

std::string served_text(const oa::MutatorLayerEntry& entry) {
    if (entry.contents == nullptr)
        return {};
    return std::string(entry.contents->begin(), entry.contents->end());
}

const oa::MutatorLayerEntry* find_served(const oa::MutatorLayerSpec& layer, std::string_view path) {
    const std::string wanted = fold_path(path);
    for (const oa::MutatorLayerEntry& entry : layer.entries) {
        if (fold_path(entry.path) == wanted)
            return &entry;
    }
    return nullptr;
}

bool served_contains(
    const oa::MutatorLayerSpec& layer, std::string_view path, std::string_view needle
) {
    const oa::MutatorLayerEntry* entry = find_served(layer, path);
    return entry != nullptr && contains(served_text(*entry), needle);
}

bool has_collection(const mutators::Journal& journal, mutators::Collection collection) {
    for (const mutators::JournalEntry& entry : journal.entries) {
        if (entry.collection == collection)
            return true;
    }
    return false;
}

void show_failure(const mutators::PipelineResult& result) {
    if (!result.refusal.empty())
        std::fprintf(stderr, "refusal: %s\n", result.refusal.c_str());
    for (const mutators::MutatorResult& mutator : result.mutators) {
        if (mutator.outcome != mutators::Outcome::applied)
            std::fprintf(
                stderr,
                "%s outcome %u: %s\n",
                mutator.id.c_str(),
                static_cast<unsigned>(mutator.outcome),
                mutator.message.c_str()
            );
    }
}

mutators::PipelineResult run_script(
    oa::AssetStore& store,
    const fs::path& scratch,
    std::string_view id,
    std::string_view layers,
    std::string_view script,
    std::string variant
) {
    mutators::PipelineInput input =
        input_of(load_package(scratch / std::string(id), id, layers, script), std::move(variant));
    mutators::PipelineResult result = mutators::run_pipeline(store, input);
    release(store);
    return result;
}

void expect_applied(const mutators::PipelineResult& result) {
    OA_CHECK(result.refusal.empty());
    OA_CHECK(result.mutators.size() == 1);
    if (result.mutators.size() != 1 || result.mutators[0].outcome != mutators::Outcome::applied)
        show_failure(result);
    if (result.mutators.size() == 1)
        OA_CHECK(result.mutators[0].outcome == mutators::Outcome::applied);
    OA_CHECK(result.playable());
}

void expect_refused(const mutators::PipelineResult& result, std::string_view needle) {
    OA_CHECK(result.mutators.size() == 1);
    if (result.mutators.size() != 1) {
        show_failure(result);
        return;
    }
    const mutators::MutatorResult& mutator = result.mutators[0];
    OA_CHECK(mutator.outcome == mutators::Outcome::failed);
    OA_CHECK(mutator.phase == mutators::Phase::hooks);
    OA_CHECK(mutator.cause == mutators::Cause::script_error);
    if (!contains(mutator.message, needle)) {
        std::fprintf(
            stderr,
            "expected \"%s\" in \"%s\"\n",
            std::string(needle).c_str(),
            mutator.message.c_str()
        );
    }
    OA_CHECK(contains(mutator.message, needle));
    OA_CHECK(result.layer.entries.empty());
}

constexpr std::string_view k_layers = "\"units\", \"sides\", \"tables\"";

constexpr const char* k_edit = R"lua(
function mutate(ctx)
  return {
    units = function(units, ctx)
      if ctx.sides[1].LOGO ~= nil then error("panel shown") end
      local order = {}
      ctx.sides:each(function(side)
        order[#order + 1] = side.name
      end)
      if #ctx.sides ~= 2 or #order ~= 2 or order[1] ~= "Ridge" or order[2] ~= "Other" then
        error("side order")
      end
      if ctx.sides[1].commander ~= "RIDGECOM" then error("index commander") end
      if ctx.sides.ridge.commander ~= "RIDGECOM" then error("name commander") end
      ctx.sides[1].commander = "ridgeclone"
      if ctx.sides[1].commander ~= "RIDGECLONE" then error("stored commander") end
      ctx.sides[1].nameprefix = "ZZZ"
      ctx.sides[1].intgaf = "RID2"
      ctx.sides[1].font = "RID2"
      ctx.sides[1].name = "Renamed"
      ctx.sides[1].fontgui = "blue"
      ctx.sides:add_canbuild("RIDGEFAB", "RIDGECLONE")
      local listed = ctx.sides:canbuild("RIDGEFAB")
      if listed[1] ~= "RIDGETANK" or listed[2] ~= "RIDGECLONE" or listed[3] ~= nil then
        error("after add_canbuild")
      end
      ctx.sides:remove_canbuild("RIDGEFAB", "RIDGECLONE")
      listed = ctx.sides:canbuild("ridgefab")
      if listed[1] ~= "RIDGETANK" or listed[2] ~= nil then error("after remove_canbuild") end
      ctx.sides:set_canbuild("RIDGEFAB", {"RIDGECLONE", "RIDGETANK"})
      ctx.sides:remove_canbuild("RIDGETANK")
      listed = ctx.sides:canbuild("RIDGEFAB")
      if listed[1] ~= "RIDGECLONE" or listed[2] ~= nil then error("after remove_canbuild all") end
      local empty = ctx.sides:canbuild("RIDGECOM")
      if empty[1] ~= nil then error("non builder list") end
      ctx.sides:add_canbuild("RIDGEYARD", "RIDGETANK")
      ctx.sides:set_canbuild("RIDGEFAB", {"RIDGETANK", "RIDGECLONE"})
      local move = ctx.tables["moveinfo"]
      local names = move:sections()
      if names[1] ~= "RIDGE" or names[2] ~= "RIDGE" or names[3] ~= "LOSE" then
        error("section order")
      end
      local ridge = move["RIDGE"]
      local keys = ridge:keys()
      if keys[1] ~= "speed" or keys[2] ~= "sight" or ridge.speed ~= "1" then
        error("first duplicate")
      end
      ridge.speed = "2"
      move:add_section("ADDED")
      move["ADDED"].mark = "yes"
      move:remove_section("LOSE")
      if ctx.tables["meteor"] ~= nil then error("meteor") end
      local los = ctx.tables["los"]
      local los_path = ctx.tables["gamedata/los.tdf"]
      if los["RADAR"].range ~= "1" or los_path["RADAR"].range ~= "1" then error("los") end
      los["RADAR"].range = "8"
    end
  }
end
)lua";

void test_edit(const fs::path& scratch) {
    oa::AssetStore store(scratch / "game");
    const mutators::PipelineResult result =
        run_script(store, scratch, "ridge-d04-edit", k_layers, k_edit, "");
    expect_applied(result);
    const oa::MutatorLayerEntry* sides = find_served(result.layer, "gamedata/sidedata.tdf");
    OA_CHECK(sides != nullptr);
    if (sides == nullptr)
        return;
    const std::string text = served_text(*sides);
    OA_CHECK(contains(text, "name=Renamed"));
    OA_CHECK(contains(text, "nameprefix=ZZZ"));
    OA_CHECK(contains(text, "commander=RIDGECLONE"));
    OA_CHECK(contains(text, "intgaf=RID2"));
    OA_CHECK(contains(text, "font=RID2"));
    OA_CHECK(contains(text, "fontgui=blue"));
    OA_CHECK(contains(text, "keepme=1"));
    OA_CHECK(contains(text, "othermark=1"));
    OA_CHECK(contains(text, "name=Other"));
    OA_CHECK(contains(text, "nameprefix=OTH"));
    OA_CHECK(!contains(text, "name=Ridge;"));
    OA_CHECK(count_of(text, "[CANBUILD]") == 1);
    OA_CHECK(contains(text, "canbuild1=RIDGETANK"));
    OA_CHECK(contains(text, "canbuild2=RIDGECLONE"));
    OA_CHECK(contains(text, "[RIDGEYARD]"));
    OA_CHECK(!contains(text, "canbuild3"));
    OA_CHECK(served_contains(result.layer, "gamedata/moveinfo.tdf", "speed=2"));
    OA_CHECK(served_contains(result.layer, "gamedata/moveinfo.tdf", "speed=9"));
    OA_CHECK(served_contains(result.layer, "gamedata/moveinfo.tdf", "[ADDED]"));
    OA_CHECK(served_contains(result.layer, "gamedata/moveinfo.tdf", "mark=yes"));
    OA_CHECK(!served_contains(result.layer, "gamedata/moveinfo.tdf", "[LOSE]"));
    OA_CHECK(!served_contains(result.layer, "gamedata/moveinfo.tdf", "speed=1"));
    OA_CHECK(served_contains(result.layer, "gamedata/los.tdf", "range=8"));
    OA_CHECK(has_collection(result.journal, mutators::Collection::sides));
    OA_CHECK(has_collection(result.journal, mutators::Collection::tables));
}

constexpr const char* k_prefix_open = R"lua(
function mutate(ctx)
  return {
    units = function(units, ctx)
      ctx.sides[2].nameprefix = "QQ"
    end
  }
end
)lua";

void test_prefix_without_pages(const fs::path& scratch) {
    oa::AssetStore store(scratch / "game");
    const mutators::PipelineResult result = run_script(
        store, scratch, "ridge-d04-prefix-open", "\"units\", \"sides\"", k_prefix_open, ""
    );
    expect_applied(result);
    OA_CHECK(served_contains(result.layer, "gamedata/sidedata.tdf", "nameprefix=QQ"));
    OA_CHECK(served_contains(result.layer, "gamedata/sidedata.tdf", "keepme=1"));
    OA_CHECK(served_contains(result.layer, "gamedata/sidedata.tdf", "name=Ridge"));
}

void expect_script(
    const fs::path& scratch,
    std::string_view id,
    std::string_view layers,
    std::string_view script,
    std::string_view needle
) {
    oa::AssetStore store(scratch / "game");
    expect_refused(run_script(store, scratch, id, layers, script, ""), needle);
}

void test_refusals(const fs::path& scratch) {
    expect_script(
        scratch,
        "ridge-d04-prefix",
        "\"units\", \"sides\"",
        R"lua(
function mutate(ctx)
  return { units = function(units, ctx) ctx.sides[1].nameprefix = "NOP" end }
end
)lua",
        "the prefix has no guis/NOPGEN.GUI page"
    );
    expect_script(
        scratch,
        "ridge-d04-intgaf",
        "\"units\", \"sides\"",
        R"lua(
function mutate(ctx)
  return { units = function(units, ctx) ctx.sides[1].intgaf = "NO" end }
end
)lua",
        "an intgaf file is missing: anims/NO.GAF"
    );
    expect_script(
        scratch,
        "ridge-d04-font",
        "\"units\", \"sides\"",
        R"lua(
function mutate(ctx)
  return { units = function(units, ctx) ctx.sides[1].font = "NO" end }
end
)lua",
        "a font file is missing: fonts/NO.FNT"
    );
    expect_script(
        scratch,
        "ridge-d04-long-name",
        "\"units\", \"sides\"",
        R"lua(
function mutate(ctx)
  return { units = function(units, ctx) ctx.sides[1].name = "ABCDEFGHIJKLMNOPQRSTUVWXYZ1234" end }
end
)lua",
        "a side name is 1 to 29 characters"
    );
    expect_script(
        scratch,
        "ridge-d04-duplicate",
        "\"units\", \"sides\"",
        R"lua(
function mutate(ctx)
  return { units = function(units, ctx) ctx.sides[2].name = "Ridge" end }
end
)lua",
        "a side already has that name"
    );
    expect_script(
        scratch,
        "ridge-d04-semicolon",
        "\"units\", \"sides\"",
        R"lua(
function mutate(ctx)
  return { units = function(units, ctx) ctx.sides[1].fontgui = "a;b" end }
end
)lua",
        "a value cannot hold ';', braces, brackets or a line break"
    );
    expect_script(
        scratch,
        "ridge-d04-brace",
        "\"units\", \"sides\"",
        R"lua(
function mutate(ctx)
  return { units = function(units, ctx) ctx.sides[1].fontgui = "a{b" end }
end
)lua",
        "a value cannot hold ';', braces, brackets or a line break"
    );
    expect_script(
        scratch,
        "ridge-d04-break",
        "\"units\", \"sides\"",
        "function mutate(ctx)\n"
        "  return { units = function(units, ctx) ctx.sides[1].fontgui = \"a\\nb\" end }\n"
        "end\n",
        "a value cannot hold ';', braces, brackets or a line break"
    );
    expect_script(
        scratch,
        "ridge-d04-add-side",
        "\"units\", \"sides\"",
        R"lua(
function mutate(ctx)
  return { units = function(units, ctx) ctx.sides[3] = { name = "Third" } end }
end
)lua",
        "a mutator cannot add a side"
    );
    expect_script(
        scratch,
        "ridge-d04-remove-side",
        "\"units\", \"sides\"",
        R"lua(
function mutate(ctx)
  return { units = function(units, ctx) ctx.sides[1] = nil end }
end
)lua",
        "a mutator cannot remove a side"
    );
    expect_script(
        scratch,
        "ridge-d04-panel",
        "\"units\", \"sides\"",
        R"lua(
function mutate(ctx)
  return { units = function(units, ctx) ctx.sides[1].LOGO = "x" end }
end
)lua",
        "a side's panels are not changed"
    );
    expect_script(
        scratch,
        "ridge-d04-builder",
        "\"units\", \"sides\"",
        R"lua(
function mutate(ctx)
  return { units = function(units, ctx) ctx.sides:set_canbuild("RIDGECOM", {"RIDGETANK"}) end }
end
)lua",
        "the unit is not a builder"
    );
    expect_script(
        scratch,
        "ridge-d04-missing-unit",
        "\"units\", \"sides\"",
        R"lua(
function mutate(ctx)
  return { units = function(units, ctx) ctx.sides[1].commander = "NOUNIT" end }
end
)lua",
        "there is no unit NOUNIT"
    );
    expect_script(
        scratch,
        "ridge-d04-nil-key",
        "\"units\", \"tables\"",
        R"lua(
function mutate(ctx)
  return { units = function(units, ctx) ctx.tables["moveinfo"]["RIDGE"].speed = nil end }
end
)lua",
        "nil does not remove a table key"
    );
    expect_script(
        scratch,
        "ridge-d04-number",
        "\"units\", \"tables\"",
        R"lua(
function mutate(ctx)
  return { units = function(units, ctx) ctx.tables["moveinfo"]["RIDGE"].speed = 2 end }
end
)lua",
        "a table value is text"
    );
    expect_script(
        scratch,
        "ridge-d04-assign-section",
        "\"units\", \"tables\"",
        R"lua(
function mutate(ctx)
  return { units = function(units, ctx) ctx.tables["moveinfo"]["RIDGE"] = "x" end }
end
)lua",
        "a section is not assigned"
    );
}

constexpr const char* k_gap = R"lua(
function mutate(ctx)
  return {
    units = function(units, ctx)
      ctx.sides:set_canbuild("RIDGEFAB", {"RIDGETANK"})
    end
  }
end
)lua";

void test_gap(const fs::path& scratch) {
    oa::AssetStore store(scratch / "gap");
    const mutators::PipelineResult result =
        run_script(store, scratch, "ridge-d04-gap", "\"units\", \"sides\"", k_gap, "");
    expect_applied(result);
    const oa::MutatorLayerEntry* sides = find_served(result.layer, "gamedata/sidedata.tdf");
    OA_CHECK(sides != nullptr);
    if (sides == nullptr)
        return;
    const std::string text = served_text(*sides);
    OA_CHECK(count_of(text, "[CANBUILD]") == 1);
    OA_CHECK(contains(text, "canbuild1=RIDGETANK"));
    OA_CHECK(!contains(text, "canbuild3"));
}

constexpr const char* k_long_list = R"lua(
function mutate(ctx)
  return {
    units = function(units, ctx)
      local names = {}
      for index = 0, 30 do
        names[#names + 1] = string.format("U%02d", index)
      end
      ctx.sides:set_canbuild("RIDGEFAB", names)
    end
  }
end
)lua";

void test_long_list(const fs::path& scratch) {
    oa::AssetStore store(scratch / "game");
    const mutators::PipelineResult result =
        run_script(store, scratch, "ridge-d04-long", "\"units\", \"sides\"", k_long_list, "");
    expect_applied(result);
    OA_CHECK(served_contains(result.layer, "gamedata/sidedata.tdf", "canbuild31=U30"));
    bool noted = false;
    for (const mutators::Conflict& conflict : result.report.conflicts) {
        if (contains(conflict.detail, "limits.build-list-entries"))
            noted = true;
    }
    OA_CHECK(noted);
}

constexpr const char* k_same = R"lua(
function mutate(ctx)
  return {
    units = function(units, ctx)
      ctx.sides[1].commander = "ridgecom"
    end
  }
end
)lua";

void test_same_text(const fs::path& scratch) {
    oa::AssetStore store(scratch / "game");
    const mutators::PipelineResult result =
        run_script(store, scratch, "ridge-d04-same", "\"units\", \"sides\"", k_same, "");
    expect_applied(result);
    OA_CHECK(result.layer.entries.empty());
    OA_CHECK(result.journal.entries.empty());
}

constexpr const char* k_variant = R"lua(
function mutate(ctx)
  return {
    units = function(units, ctx)
      if ctx.sides[1].name ~= "RidgeMark" then error("variant side") end
      local listed = ctx.sides:canbuild("RIDGEFAB")
      if listed[1] ~= "RIDGETANK" or listed[2] ~= nil then error("plain list") end
      ctx.sides[1].fontgui = "varied"
      ctx.sides:set_canbuild("RIDGEFAB", {"RIDGECOM"})
    end
  }
end
)lua";

void test_variant(const fs::path& scratch) {
    oa::AssetStore store(scratch / "variant");
    const mutators::PipelineResult result =
        run_script(store, scratch, "ridge-d04-variant", "\"units\", \"sides\"", k_variant, "ridge");
    expect_applied(result);
    OA_CHECK(served_contains(result.layer, "gamedata-ridge/sidedata.tdf", "name=RidgeMark"));
    OA_CHECK(served_contains(result.layer, "gamedata-ridge/sidedata.tdf", "fontgui=varied"));
    OA_CHECK(served_contains(result.layer, "gamedata-ridge/sidedata.tdf", "canbuild1=RIDGECLONE"));
    OA_CHECK(served_contains(result.layer, "gamedata/sidedata.tdf", "name=PlainMark"));
    OA_CHECK(served_contains(result.layer, "gamedata/sidedata.tdf", "canbuild1=RIDGECOM"));
    OA_CHECK(!served_contains(result.layer, "gamedata/sidedata.tdf", "fontgui=varied"));
    bool variant_sides = false;
    bool plain_sides = false;
    for (const mutators::JournalEntry& entry : result.journal.entries) {
        if (entry.collection != mutators::Collection::sides)
            continue;
        if (fold_path(entry.path) == "gamedata-ridge/sidedata.tdf")
            variant_sides = true;
        if (fold_path(entry.path) == "gamedata/sidedata.tdf")
            plain_sides = true;
    }
    OA_CHECK(variant_sides);
    OA_CHECK(plain_sides);
}

constexpr const char* k_fail = R"lua(
function mutate(ctx)
  return {
    units = function(units, ctx)
      ctx.sides[1].fontgui = "changed"
      ctx.tables["moveinfo"]["RIDGE"].speed = "3"
      ctx.sides:set_canbuild("RIDGEFAB", {"RIDGECLONE"})
      error("stop")
    end
  }
end
)lua";

void test_failure(const fs::path& scratch) {
    oa::AssetStore store(scratch / "game");
    const mutators::PipelineResult result =
        run_script(store, scratch, "ridge-d04-fail", k_layers, k_fail, "");
    expect_refused(result, "stop");
    OA_CHECK(result.journal.entries.empty());
}

void test_layers(const fs::path& scratch) {
    expect_script(
        scratch,
        "ridge-d04-no-sides",
        "\"units\"",
        R"lua(
function mutate(ctx)
  return { units = function(units, ctx) ctx.sides[1].fontgui = "changed" end }
end
)lua",
        "this mutator does not declare the sides layer"
    );
    expect_script(
        scratch,
        "ridge-d04-no-tables",
        "\"units\"",
        R"lua(
function mutate(ctx)
  return { units = function(units, ctx) ctx.tables["los"]["RADAR"].range = "2" end }
end
)lua",
        "this mutator does not declare the tables layer"
    );
}

constexpr const char* k_read = R"lua(
function mutate(ctx)
  return {
    units = function(units, ctx)
      local side = ctx.sides[1]
      local name = side.name
      local commander = side.commander
      local listed = ctx.sides:canbuild("RIDGEFAB")
      local speed = ctx.tables["moveinfo"]["RIDGE"].speed
      if name ~= "Ridge" or commander ~= "RIDGECOM" or listed[1] ~= "RIDGETANK" or speed ~= "1" then
        error("read")
      end
    end
  }
end
)lua";

void test_read_only(const fs::path& scratch) {
    oa::AssetStore store(scratch / "game");
    const mutators::PipelineResult result =
        run_script(store, scratch, "ridge-d04-read", "\"units\"", k_read, "");
    expect_applied(result);
    OA_CHECK(result.layer.entries.empty());
}

void test_table_paths(const fs::path& scratch) {
    const char* ids[] = {
        "ridge-d04-escape-units",
        "ridge-d04-escape-download",
        "ridge-d04-escape-nested",
        "ridge-d04-escape-parent",
        "ridge-d04-escape-variant",
    };
    const char* requests[] = {
        "units/RGCOM.fbi",
        "download/rglab.tdf",
        "gamedata/sub/x.tdf",
        "../gamedata/los.tdf",
        "gamedata-ridge/los.tdf",
    };
    for (std::size_t index = 0; index < 5; ++index) {
        std::string script = "function mutate(ctx)\n  return {\n";
        script += "    weapons = function(weapons, ctx)\n      local t = ctx.tables[\"";
        script += requests[index];
        script += "\"]\n      t.UNITINFO.footprintx = \"0\"\n";
        script += "      t.UNITINFO.maxdamage = \"-5\"\n    end\n  }\nend\n";
        expect_script(scratch, ids[index], "\"weapons\", \"tables\"", script, requests[index]);
    }
}

void test_sound_and_sidedata(const fs::path& scratch) {
    oa::AssetStore sound_store(scratch / "game");
    const mutators::PipelineResult sound = run_script(
        sound_store,
        scratch,
        "ridge-d04-sound",
        "\"units\"",
        R"lua(
function mutate(ctx)
  return { units = function(units, ctx) local ignored = ctx.tables["sound"] end }
end
)lua",
        ""
    );
    mutators::PathContext context;
    context.layout = &defs::data_layout();
    const mutators::PathVerdict verdict =
        mutators::check_mutable_path("gamedata/sound.tdf", context);
    OA_CHECK(sound.mutators.size() == 1);
    if (sound.mutators.size() == 1) {
        if (!contains(sound.mutators[0].message, verdict.message)) {
            std::fprintf(
                stderr,
                "expected \"%s\" in \"%s\"\n",
                verdict.message.c_str(),
                sound.mutators[0].message.c_str()
            );
        }
        OA_CHECK(contains(sound.mutators[0].message, verdict.message));
        OA_CHECK(sound.mutators[0].outcome == mutators::Outcome::failed);
    }
    OA_CHECK(sound.layer.entries.empty());

    oa::AssetStore side_store(scratch / "game");
    const mutators::PipelineResult sidedata = run_script(
        side_store,
        scratch,
        "ridge-d04-sidedata",
        "\"units\"",
        R"lua(
function mutate(ctx)
  return { units = function(units, ctx) local ignored = ctx.tables["sidedata"] end }
end
)lua",
        ""
    );
    OA_CHECK(sidedata.mutators.size() == 1);
    if (sidedata.mutators.size() == 1) {
        const std::string& message = sidedata.mutators[0].message;
        const auto at = message.rfind("use ctx.sides");
        if (at == std::string::npos || at + std::string("use ctx.sides").size() != message.size())
            std::fprintf(stderr, "sidedata message: %s\n", message.c_str());
        OA_CHECK(
            at != std::string::npos && at + std::string("use ctx.sides").size() == message.size()
        );
    }
}

bool payloads_match(const oa::MutatorLayerSpec& left, const oa::MutatorLayerSpec& right) {
    if (left.entries.size() != right.entries.size())
        return false;
    for (std::size_t index = 0; index < left.entries.size(); ++index) {
        if (fold_path(left.entries[index].path) != fold_path(right.entries[index].path))
            return false;
        const bool left_bytes = left.entries[index].contents != nullptr;
        const bool right_bytes = right.entries[index].contents != nullptr;
        if (left_bytes != right_bytes)
            return false;
        if (left_bytes && *left.entries[index].contents != *right.entries[index].contents)
            return false;
    }
    return true;
}

void test_two_runs(const fs::path& scratch) {
    oa::AssetStore first_store(scratch / "game");
    oa::AssetStore second_store(scratch / "game");
    const mutators::PipelineResult first =
        run_script(first_store, scratch, "ridge-d04-edit", k_layers, k_edit, "");
    const mutators::PipelineResult second =
        run_script(second_store, scratch, "ridge-d04-edit", k_layers, k_edit, "");
    expect_applied(first);
    expect_applied(second);
    OA_CHECK(payloads_match(first.layer, second.layer));
}

constexpr const char* k_installed = R"lua(
function mutate(ctx)
  return {
    units = function(units, ctx)
      local commander = ctx.sides[1].commander
      if commander == nil or commander:lower() ~= "armcom" then error("side commander") end
      for index = 1, #ctx.sides do
        local side = ctx.sides[index]
        local name = side.name
      end
      for name, unit in pairs(units) do
        if unit.builder then
          ctx.sides:canbuild(name)
        end
      end
    end
  }
end
)lua";

void test_installed(oa::AssetStore& store) {
    const fs::path scratch = oa::test::make_scratch_directory("D04-installed");
    const mutators::PipelineResult result =
        run_script(store, scratch, "ridge", "\"units\"", k_installed, "");
    std::error_code error;
    fs::remove_all(scratch, error);
    expect_applied(result);
    for (const oa::MutatorLayerEntry& entry : result.layer.entries)
        OA_CHECK(fold_path(entry.path).find("gamedata") == std::string::npos);
    OA_CHECK(result.layer.entries.empty());
}

void test_side_count_follows_the_limit(const fs::path& scratch) {
    const fs::path root = scratch / "seven";
    write_unit(root, "RIDGECOM", false);
    const char* names[] = {"Arm", "Core", "Ridge2", "Ridge3", "Ridge4", "Ridge5", "Ridge6"};
    std::string sides;
    for (int index = 0; index < 7; ++index) {
        sides += "[SIDE";
        sides += std::to_string(index);
        sides += "]\n{\nname=";
        sides += names[index];
        sides += ";\ncommander=RIDGECOM;\n}\n";
    }
    write_bytes(root / "gamedata" / "sidedata.tdf", sides);
    constexpr std::string_view layers = "\"units\", \"sides\"";
    constexpr const char* wide_script = R"lua(
function mutate(ctx)
  return {
    units = function(units, ctx)
      if #ctx.sides ~= 7 then error("count " .. tostring(#ctx.sides)) end
      if ctx.sides[7].name ~= "Ridge6" then error("name") end
    end
  }
end
)lua";
    constexpr const char* stock_script = R"lua(
function mutate(ctx)
  return {
    units = function(units, ctx)
      if #ctx.sides ~= 5 then error("count " .. tostring(#ctx.sides)) end
    end
  }
end
)lua";
    oa::AssetStore wide_store(root);
    mutators::PipelineInput wide = input_of(
        load_package(scratch / "ridge-t09-sides", "ridge-t09-sides", layers, wide_script), ""
    );
    wide.limits.sides = 7;
    const mutators::PipelineResult wide_result = mutators::run_pipeline(wide_store, wide);
    release(wide_store);
    expect_applied(wide_result);

    oa::AssetStore stock_store(root);
    mutators::PipelineInput stock = input_of(
        load_package(scratch / "ridge-t09-stock", "ridge-t09-stock", layers, stock_script), ""
    );
    const mutators::PipelineResult stock_result = mutators::run_pipeline(stock_store, stock);
    release(stock_store);
    expect_applied(stock_result);
}

void synthetic_tests() {
    Scratch scratch("D04");
    Corpus corpus;
    write_corpus(scratch.path / "game", corpus);
    corpus.gap = true;
    write_corpus(scratch.path / "gap", corpus);
    corpus.gap = false;
    corpus.side_name = "PlainMark";
    corpus.variant = true;
    write_corpus(scratch.path / "variant", corpus);

    test_edit(scratch.path);
    test_prefix_without_pages(scratch.path);
    test_refusals(scratch.path);
    test_gap(scratch.path);
    test_long_list(scratch.path);
    test_same_text(scratch.path);
    test_variant(scratch.path);
    test_failure(scratch.path);
    test_layers(scratch.path);
    test_read_only(scratch.path);
    test_sound_and_sidedata(scratch.path);
    test_table_paths(scratch.path);
    test_two_runs(scratch.path);
    test_side_count_follows_the_limit(scratch.path);
}

} // namespace

int main(int argc, char** argv) {
    if (oa::test::game_data_requested(argc, argv)) {
        oa::AssetStore store = oa::test::require_game_assets("side and table collections");
        test_installed(store);
    } else {
        synthetic_tests();
    }
    return oa::test::check_exit_status();
}

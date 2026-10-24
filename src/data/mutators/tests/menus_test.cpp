// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Build menus edited from mutator hooks, over a made-up Ridge tree.
// The installed-game cases run only when the program is passed --data.
#include "oa/core/unit_def.h"
#include "oa/data/defs/unit_catalog.hpp"
#include "oa/data/defs/unit_records.hpp"
#include "oa/data/mutators/pipeline.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/formats/tdf.hpp"
#include "oa/test/check.hpp"
#include "oa/test/game_assets.hpp"
#include "oa/test/scratch_directory.hpp"
#include "oa/ui/gui_layout.hpp"
#include "oa/ui/hud/order_panel.hpp"

#include <algorithm>
#include <chrono>
#include <ctime>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace {

namespace mutators = oa::data::mutators;
namespace oamut = oa::formats::oamut;
namespace defs = oa::data::defs;
namespace fs = std::filesystem;

struct Kept {
    fs::path root;

    Kept() : root(oa::test::make_scratch_directory("D06-menus")) {}

    ~Kept() {
        std::error_code error;
        fs::remove_all(root, error);
    }

    Kept(const Kept&) = delete;
    Kept& operator=(const Kept&) = delete;
};

std::string fold_copy(std::string_view text) {
    std::string out(text);
    for (char& letter : out) {
        if (letter >= 'A' && letter <= 'Z')
            letter = static_cast<char>(letter - 'A' + 'a');
    }
    return out;
}

void write_bytes(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    OA_CHECK(out.good());
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string read_text(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in.good())
        return {};
    in.seekg(0, std::ios::end);
    const std::streamoff length = in.tellg();
    in.seekg(0);
    if (length <= 0)
        return {};
    std::string text(static_cast<std::size_t>(length), '\0');
    in.read(text.data(), length);
    return text;
}

std::string fbi(std::string_view name, bool builder) {
    std::string text = "[UNITINFO]\r\n{\r\nUnitName=";
    text.append(name);
    text += ";\r\nBuilder=";
    text += builder ? "1" : "0";
    text += ";\r\n}\r\n";
    return text;
}

std::string
gadget(std::string_view section, int id, std::string_view name, std::string_view attribs) {
    std::string text = "[";
    text.append(section);
    text += "]\r\n{\r\n[COMMON]\r\n{\r\nid=";
    text += std::to_string(id);
    text += ";\r\nname=";
    text.append(name);
    text += ";\r\nactive=1;\r\ncommonattribs=";
    text.append(attribs);
    text += ";\r\n}\r\ngrayedout=0;\r\n}\r\n";
    return text;
}

std::string builder_page() {
    std::string text = gadget("PANEL", 0, "RGBLDR", "0");
    text += gadget("FILL", 1, "PREV", "0");
    text += gadget("GADGET2", 1, "NEXT", "0");
    text += gadget("GADGET2", 1, "OK", "0");
    text += gadget("UNIT", 1, "RGONE", "4");
    text += gadget("UNIT", 1, "RGTWO", "4");
    text += gadget("UNIT", 1, "RGTHREE", "-51");
    text += gadget("UNIT", 1, "RGFOUR", "4");
    text += gadget("UNIT", 1, "RGFIVE", "4");
    text += gadget("UNIT", 1, "RGSIX", "4");
    return text;
}

std::string sidedata_text() {
    std::string text = "[CANBUILD]\r\n{\r\n[RGAIFULL]\r\n{\r\n";
    for (int index = 1; index <= 30; ++index) {
        text += "canbuild";
        text += std::to_string(index);
        text += "=RGCB";
        if (index < 10)
            text += "0";
        text += std::to_string(index);
        text += ";\r\n";
    }
    text += "}\r\n[RGBLDR]\r\n{\r\ncanbuild1=RGONE;\r\n}\r\n";
    text += "[RGEMPTY]\r\n{\r\n}\r\n[RGALT]\r\n{\r\n}\r\n}\r\n";
    return text;
}

std::string marked_download() {
    return "[MENUENTRY1]\r\n{\r\nUNITMENU=RGBLDR;\r\nMENU=2;\r\nBUTTON=0;\r\nUNITNAME=RGMARK;\r\n}"
           "\r\n"
           "[MENUENTRY2]\r\n{\r\nUNITMENU=RGBLDR;\r\nMENU=2;\r\nBUTTON=1;\r\nUNITNAME=RGOTHER;\r\n}"
           "\r\n"
           "[FILL]\r\n{\r\nUNITMENU=RGBLDR;\r\nMENU=0;\r\nBUTTON=0;\r\nUNITNAME=RGSIX;\r\n}\r\n"
           "[FILL]\r\n{\r\nUNITMENU=RGBLDR;\r\nMENU=0;\r\nBUTTON=0;\r\nUNITNAME=RGSIX;\r\n}\r\n"
           "[FILL]\r\n{\r\nUNITMENU=RGBLDR;\r\nMENU=0;\r\nBUTTON=0;\r\nUNITNAME=RGSIX;\r\n}\r\n"
           "[TAIL]\r\n{\r\nUNITMENU=RGBLDR;\r\nMENU=0;\r\nBUTTON=0;\r\nUNITNAME=RGSIX;\r\n"
           "TAILKEY=ridgekept;\r\n}\r\n";
}

void write_unit(const fs::path& game, std::string_view name, bool builder) {
    write_bytes(game / "units" / (std::string{name} + ".fbi"), fbi(name, builder));
    write_bytes(game / "objects3d" / (std::string{name} + ".3DO"), "3DO");
}

void plant_world(const fs::path& game) {
    const char* builders[] = {"RGBLDR", "RGEMPTY", "RGALT", "RGAIFULL"};
    for (const char* name : builders)
        write_unit(game, name, true);
    const char* units[] = {
        "RGONE",
        "RGTWO",
        "RGTHREE",
        "RGFOUR",
        "RGFIVE",
        "RGSIX",
        "RGPLANT",
        "RGBASE",
        "RGMARK",
        "RGOTHER",
        "RGSHARED",
        "RGEXTRA1",
        "RGEXTRA2"
    };
    for (const char* name : units)
        write_unit(game, name, false);
    for (int index = 1; index <= 30; ++index) {
        std::string name = "RGCB";
        if (index < 10)
            name += "0";
        name += std::to_string(index);
        write_unit(game, name, false);
    }
    write_bytes(game / "guis" / "RGBLDR1.GUI", builder_page());
    write_bytes(game / "download" / "marked.tdf", marked_download());
    write_bytes(
        game / "download" / "shared.tdf",
        "[A]\r\n{\r\nUNITMENU=RGBLDR;\r\nMENU=2;\r\nBUTTON=2;\r\nUNITNAME=RGSHARED;\r\n}\r\n"
        "[B]\r\n{\r\nUNITMENU=RGALT;\r\nMENU=2;\r\nBUTTON=0;\r\nUNITNAME=RGSHARED;\r\n}\r\n"
    );
    // MENU=8 is player page 7, so "new" on RGEMPTY has nowhere left to go.
    write_bytes(
        game / "download" / "full.tdf",
        "[FULL]\r\n{\r\nUNITMENU=RGEMPTY;\r\nMENU=8;\r\nBUTTON=5;\r\nUNITNAME=RGEXTRA1;\r\n}\r\n"
    );
    write_bytes(game / "gamedata" / "sidedata.tdf", sidedata_text());
    write_bytes(game / "anims" / "RGPLANT_gadget.gaf", "gaf");
}

void write_package(
    const fs::path& packages, std::string_view id, std::string_view layers, std::string_view lua
) {
    const fs::path folder = packages / std::string{id};
    std::string manifest = "oamut: 1\nid: ";
    manifest.append(id);
    manifest += "\nname: \"Ridge ";
    manifest.append(id);
    manifest += "\"\nversion: \"1.0\"\nauthor:\n  name: \"Ridge Maker\"\ntags: [\"gameplay\"]\n";
    manifest += "works-with: [\"*\"]\nlayers: [";
    manifest.append(layers);
    manifest += "]\n";
    write_bytes(folder / "oamut.yaml", manifest);
    write_bytes(folder / "mutator.lua", lua);
}

std::string hook_script(std::string_view which, std::string_view body) {
    std::string lua = "function mutate(ctx)\n  return {\n    ";
    lua.append(which);
    lua += " = function(collection, ctx)\n";
    lua.append(body);
    lua += "\n    end\n  }\nend\n";
    return lua;
}

mutators::PipelineMutator load_package(const fs::path& folder) {
    mutators::PipelineMutator mutator;
    mutator.package = folder;
    const std::string bytes = read_text(folder / "oamut.yaml");
    std::vector<oamut::Problem> problems;
    const bool read = oamut::read_manifest(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()),
        mutator.manifest,
        problems
    );
    OA_CHECK(read);
    return mutator;
}

mutators::PipelineInput input_for(const fs::path& packages, const std::vector<std::string>& ids) {
    mutators::PipelineInput input;
    for (const std::string& id : ids)
        input.mutators.push_back(load_package(packages / id));
    input.mod.id = "ridge";
    input.mod.version = "1.0";
    input.mod.game = "ridge";
    input.engine_version = "0.8.5";
    input.layout = &defs::data_layout();
    input.seed = 1;
    return input;
}

mutators::PipelineResult
run_on(oa::AssetStore& store, const fs::path& packages, const std::vector<std::string>& ids) {
    mutators::PipelineResult result = mutators::run_pipeline(store, input_for(packages, ids));
    if (store.mutator_layer_mounted())
        store.unmount_mutator_layer();
    return result;
}

mutators::PipelineResult
run_packages(const fs::path& game, const fs::path& packages, const std::vector<std::string>& ids) {
    oa::AssetStore store(game);
    return run_on(store, packages, ids);
}

void show_failure(const mutators::PipelineResult& run) {
    if (!run.refusal.empty())
        std::fprintf(stderr, "refusal: %s\n", run.refusal.c_str());
    for (const mutators::MutatorResult& one : run.mutators) {
        if (one.outcome == mutators::Outcome::applied)
            continue;
        std::fprintf(stderr, "%s: %s\n", one.id.c_str(), one.message.c_str());
    }
}

bool applied(const mutators::PipelineResult& run) {
    const bool ok = run.refusal.empty() && !run.mutators.empty() &&
                    run.mutators.front().outcome == mutators::Outcome::applied;
    if (!ok)
        show_failure(run);
    OA_CHECK(ok);
    return ok;
}

bool failed_with(
    const mutators::PipelineResult& run, mutators::Cause cause, std::string_view needle
) {
    const bool ok = !run.mutators.empty() &&
                    run.mutators.front().outcome == mutators::Outcome::failed &&
                    run.mutators.front().cause == cause &&
                    run.mutators.front().message.find(needle) != std::string::npos;
    if (!ok)
        show_failure(run);
    OA_CHECK(ok);
    return ok;
}

const oa::MutatorLayerEntry*
find_served(const mutators::PipelineResult& run, std::string_view needle) {
    const std::string wanted = fold_copy(needle);
    for (const oa::MutatorLayerEntry& entry : run.layer.entries) {
        if (fold_copy(entry.path).find(wanted) != std::string::npos)
            return &entry;
    }
    return nullptr;
}

std::string served_text(const oa::MutatorLayerEntry* entry) {
    if (entry == nullptr || entry->contents == nullptr)
        return {};
    return std::string(entry->contents->begin(), entry->contents->end());
}

bool text_has(std::string_view text, std::string_view needle) {
    return text.find(needle) != std::string_view::npos;
}

std::vector<std::string> id_list(const mutators::PipelineResult& run) {
    std::vector<std::string> ids;
    for (const mutators::MutatorResult& one : run.mutators)
        ids.push_back(one.id);
    return ids;
}

const mutators::Conflict* find_note(const mutators::PipelineResult& run, std::string_view needle) {
    for (const mutators::Conflict& conflict : run.report.conflicts) {
        if (conflict.detail.find(needle) != std::string::npos)
            return &conflict;
    }
    return nullptr;
}

void check_note(const mutators::PipelineResult& run, std::string_view needle) {
    const mutators::Conflict* conflict = find_note(run, needle);
    OA_CHECK(conflict != nullptr);
    if (conflict == nullptr)
        return;
    const std::vector<std::string> ids = id_list(run);
    OA_CHECK(mutators::describe(*conflict, ids) == conflict->detail);
}

// In-memory files the download loader can list. Paths use '\\' so the prefix
// the loader strips leaves the file name.
struct MemoryFile {
    std::string path;
    std::string text;
};

struct MemoryFiles {
    std::vector<MemoryFile> files;

    const MemoryFile* find(const char* path) const {
        const std::string key = fold_slashes(path);
        for (const MemoryFile& file : files) {
            if (fold_slashes(file.path.c_str()) == key)
                return &file;
        }
        return nullptr;
    }

    static std::string fold_slashes(const char* path) {
        std::string key = path != nullptr ? path : "";
        for (char& letter : key) {
            if (letter == '/')
                letter = '\\';
            else if (letter >= 'A' && letter <= 'Z')
                letter = static_cast<char>(letter - 'A' + 'a');
        }
        return key;
    }

    defs::Files view() {
        return defs::Files{
            this,
            [](void* context, const char* path, uint8_t** data, uint32_t* size, bool* archived) {
                const MemoryFile* file = static_cast<MemoryFiles*>(context)->find(path);
                if (file == nullptr)
                    return false;
                *data = static_cast<uint8_t*>(std::malloc(file->text.size() + 1));
                if (*data == nullptr)
                    return false;
                std::memcpy(*data, file->text.data(), file->text.size());
                (*data)[file->text.size()] = 0;
                *size = static_cast<uint32_t>(file->text.size());
                *archived = false;
                return true;
            },
            [](void*, uint8_t* data) { std::free(data); },
            [](void* context, const char* path) {
                return static_cast<MemoryFiles*>(context)->find(path) != nullptr;
            },
            [](void* context,
               const char* directory,
               const char* extension,
               void (*visit)(void*, const char*),
               void* user) {
                const std::string prefix = MemoryFiles::fold_slashes(directory) + "\\";
                std::string suffix = ".";
                suffix += MemoryFiles::fold_slashes(extension);
                for (const MemoryFile& file : static_cast<MemoryFiles*>(context)->files) {
                    const std::string key = MemoryFiles::fold_slashes(file.path.c_str());
                    if (key.rfind(prefix, 0) == 0 && key.size() > suffix.size() &&
                        key.compare(key.size() - suffix.size(), suffix.size(), suffix) == 0 &&
                        key.find('\\', prefix.size()) == std::string::npos)
                        visit(user, file.path.c_str() + prefix.size());
                }
            },
        };
    }
};

struct NamedUnit {
    const char* name{};
    bool builder{false};
};

void set_unit_name(oa::UnitDef& unit, const char* name) {
    std::memset(unit.unit_name, 0, sizeof unit.unit_name);
    std::size_t index = 0;
    for (; name[index] != '\0' && index + 1 < sizeof unit.unit_name; ++index)
        unit.unit_name[index] = name[index];
    unit.unit_name[index] = '\0';
}

std::vector<std::string>
marks_of(const std::vector<MemoryFile>& files, const std::vector<NamedUnit>& units) {
    MemoryFiles memory;
    memory.files = files;
    defs::Files boundary = memory.view();
    defs::UnitDefTables tables;
    defs::unit_def_tables_init(&tables);
    const auto count = static_cast<uint32_t>(units.size() + 1);
    std::vector<std::string> marked;
    if (!defs::unit_def_tables_allocate(&tables, count)) {
        OA_CHECK(false);
        return marked;
    }
    set_unit_name(tables.records[0], "None");
    tables.records[0].type_id = 0;
    for (std::size_t index = 0; index < units.size(); ++index) {
        oa::UnitDef& unit = tables.records[index + 1];
        set_unit_name(unit, units[index].name);
        if (units[index].builder)
            unit.flags = OA_UNIT_DEF_FLAG_BUILDER;
    }
    std::sort(
        tables.records + 1,
        tables.records + count,
        [](const oa::UnitDef& earlier, const oa::UnitDef& later) {
            return fold_copy(earlier.unit_name) < fold_copy(later.unit_name);
        }
    );
    for (uint32_t index = 0; index < count; ++index)
        tables.records[index].type_id = static_cast<uint16_t>(index);
    if (defs::load_build_lists(&boundary, nullptr, &tables) &&
        defs::load_download_menu(&boundary, nullptr, &tables)) {
        for (uint32_t index = 1; index < count; ++index) {
            if ((tables.records[index].flags & OA_UNIT_DEF_FLAG_DOWNLOADABLE) != 0)
                marked.emplace_back(tables.records[index].unit_name);
        }
    } else {
        OA_CHECK(false);
    }
    defs::unit_def_tables_free(&tables);
    std::sort(marked.begin(), marked.end());
    return marked;
}

std::vector<NamedUnit> ridge_units() {
    std::vector<NamedUnit> units = {
        {"RGBLDR", true},
        {"RGEMPTY", true},
        {"RGALT", true},
        {"RGAIFULL", true},
        {"RGONE", false},
        {"RGTWO", false},
        {"RGTHREE", false},
        {"RGFOUR", false},
        {"RGFIVE", false},
        {"RGSIX", false},
        {"RGPLANT", false},
        {"RGBASE", false},
        {"RGMARK", false},
        {"RGOTHER", false},
        {"RGSHARED", false},
        {"RGEXTRA1", false},
        {"RGEXTRA2", false}
    };
    return units;
}

std::vector<std::string> canbuild_names() {
    std::vector<std::string> names;
    for (int index = 1; index <= 30; ++index) {
        std::string name = "RGCB";
        if (index < 10)
            name += "0";
        name += std::to_string(index);
        names.push_back(std::move(name));
    }
    return names;
}

// The extra names stay alive for the pointers in `units`.
struct UnitTable {
    std::vector<std::string> extra;
    std::vector<NamedUnit> units;

    UnitTable() {
        extra = canbuild_names();
        units = ridge_units();
        for (const std::string& name : extra)
            units.push_back(NamedUnit{name.c_str(), false});
    }
};

std::string slash_path(std::string path) {
    for (char& letter : path) {
        if (letter == '/')
            letter = '\\';
    }
    return path;
}

std::vector<MemoryFile> disk_downloads(const fs::path& game) {
    std::vector<MemoryFile> files;
    files.push_back(
        MemoryFile{"gamedata\\sidedata.tdf", read_text(game / "gamedata" / "sidedata.tdf")}
    );
    for (const fs::directory_entry& entry : fs::directory_iterator(game / "download")) {
        MemoryFile file;
        file.path = "download\\" + entry.path().filename().string();
        file.text = read_text(entry.path());
        files.push_back(std::move(file));
    }
    return files;
}

std::vector<MemoryFile>
overlay_downloads(const fs::path& game, const mutators::PipelineResult& run) {
    std::vector<MemoryFile> files = disk_downloads(game);
    for (const oa::MutatorLayerEntry& entry : run.layer.entries) {
        const std::string folded = fold_copy(entry.path);
        if (folded.rfind("download/", 0) != 0 && folded.rfind("download\\", 0) != 0)
            continue;
        const std::string key = fold_copy(slash_path(entry.path));
        if (entry.kind == oa::MutatorSourceKind::hidden) {
            files.erase(
                std::remove_if(
                    files.begin(),
                    files.end(),
                    [&](const MemoryFile& file) { return fold_copy(file.path) == key; }
                ),
                files.end()
            );
            continue;
        }
        if (entry.contents == nullptr)
            continue;
        bool replaced = false;
        for (MemoryFile& file : files) {
            if (fold_copy(file.path) != key)
                continue;
            file.text = served_text(&entry);
            replaced = true;
            break;
        }
        if (!replaced)
            files.push_back(MemoryFile{slash_path(entry.path), served_text(&entry)});
    }
    return files;
}

bool marks_match(
    const std::vector<MemoryFile>& before,
    const std::vector<MemoryFile>& after,
    const UnitTable& table,
    std::string_view dropped
) {
    const std::vector<std::string> earlier = marks_of(before, table.units);
    std::vector<std::string> later = marks_of(after, table.units);
    if (!dropped.empty()) {
        const std::string folded = fold_copy(dropped);
        later.erase(
            std::remove_if(
                later.begin(),
                later.end(),
                [&](const std::string& name) { return fold_copy(name) == folded; }
            ),
            later.end()
        );
        // The dropped name is compared by removing it from both sides when
        // the hook was allowed to clear that one mark.
        std::vector<std::string> trimmed = earlier;
        trimmed.erase(
            std::remove_if(
                trimmed.begin(),
                trimmed.end(),
                [&](const std::string& name) { return fold_copy(name) == folded; }
            ),
            trimmed.end()
        );
        return trimmed == later;
    }
    return earlier == later;
}

bool builder_lists(
    const std::vector<MemoryFile>& files,
    const UnitTable& table,
    const char* builder,
    const char* unit
) {
    MemoryFiles memory;
    memory.files = files;
    defs::Files boundary = memory.view();
    defs::UnitDefTables tables;
    defs::unit_def_tables_init(&tables);
    const auto count = static_cast<uint32_t>(table.units.size() + 1);
    bool listed = false;
    if (!defs::unit_def_tables_allocate(&tables, count))
        return false;
    set_unit_name(tables.records[0], "None");
    for (std::size_t index = 0; index < table.units.size(); ++index) {
        oa::UnitDef& record = tables.records[index + 1];
        set_unit_name(record, table.units[index].name);
        if (table.units[index].builder)
            record.flags = OA_UNIT_DEF_FLAG_BUILDER;
    }
    std::sort(
        tables.records + 1,
        tables.records + count,
        [](const oa::UnitDef& earlier, const oa::UnitDef& later) {
            return fold_copy(earlier.unit_name) < fold_copy(later.unit_name);
        }
    );
    for (uint32_t index = 0; index < count; ++index)
        tables.records[index].type_id = static_cast<uint16_t>(index);
    if (defs::load_build_lists(&boundary, nullptr, &tables) &&
        defs::load_download_menu(&boundary, nullptr, &tables)) {
        const uint16_t wanted = defs::unit_defs_type_id(tables.records, tables.count, unit);
        for (uint32_t index = 1; index < count; ++index) {
            if (fold_copy(tables.records[index].unit_name) != fold_copy(builder))
                continue;
            const uint16_t* ids = defs::unit_def_build_ids(&tables, tables.records[index]);
            for (uint32_t slot = 0; ids != nullptr && slot < tables.records[index].build_id_count;
                 ++slot) {
                if (ids[slot] == wanted)
                    listed = true;
            }
        }
    }
    defs::unit_def_tables_free(&tables);
    return listed;
}

uint16_t known_unit(void*, const char* name) {
    if (name == nullptr || name[0] == '\0')
        return 0;
    return 1;
}

void check_grey_page(const std::string& text) {
    const auto* data = reinterpret_cast<const uint8_t*>(text.data());
    const oa::ui::gui_layout::ParseResult parsed =
        oa::ui::gui_layout::parse(std::span<const uint8_t>(data, text.size()));
    OA_CHECK(parsed.ok());
    if (!parsed.ok())
        return;
    const std::vector<oa::ui::gui_layout::Gadget>& gadgets = parsed.layout->gadgets;
    OA_CHECK(gadgets.size() >= 6);
    if (gadgets.size() < 6)
        return;
    std::vector<std::string> names(gadgets.size());
    std::vector<oa::ui::hud::BuildPageRecord> records(gadgets.size());
    for (std::size_t index = 0; index < gadgets.size(); ++index) {
        names[index] = gadgets[index].common.name;
        records[index].name = names[index].c_str();
        records[index].gadget_type = static_cast<uint8_t>(gadgets[index].common.type);
        records[index].common_attributes =
            static_cast<uint8_t>(gadgets[index].common.common_attributes);
        if (const auto* button =
                std::get_if<oa::ui::gui_layout::ButtonFields>(&gadgets[index].fields))
            records[index].grayed = button->grayed_out;
    }
    const auto greyed = std::find_if(
        records.begin(), records.end(), [](const oa::ui::hud::BuildPageRecord& record) {
            return record.name != nullptr && std::strcmp(record.name, "RGONE") == 0;
        }
    );
    const auto sibling = std::find_if(
        records.begin(), records.end(), [](const oa::ui::hud::BuildPageRecord& record) {
            return record.name != nullptr && std::strcmp(record.name, "RGTWO") == 0;
        }
    );
    const auto negative = std::find_if(
        records.begin(), records.end(), [](const oa::ui::hud::BuildPageRecord& record) {
            return record.name != nullptr && std::strcmp(record.name, "RGTHREE") == 0;
        }
    );
    const auto hidden =
        std::find_if(gadgets.begin(), gadgets.end(), [](const oa::ui::gui_layout::Gadget& gadget) {
            return gadget.common.name == "RGFOUR";
        });
    OA_CHECK(greyed != records.end() && sibling != records.end() && negative != records.end());
    OA_CHECK(hidden != gadgets.end() && hidden->common.active == 0);
    if (greyed == records.end() || sibling == records.end())
        return;
    OA_CHECK(greyed->grayed);
    OA_CHECK((greyed->common_attributes & oa::ui::hud::kCommonUnitButton) == 0);
    OA_CHECK((negative->common_attributes) == 201);
    sibling->grayed = true;
    oa::ui::hud::update_build_button_validity(records.data(), 6, known_unit, nullptr);
    OA_CHECK(greyed->grayed);
    OA_CHECK(!sibling->grayed);
}

int count_token(std::string_view text, std::string_view token) {
    int count = 0;
    for (std::size_t at = 0; (at = text.find(token, at)) != std::string_view::npos;
         at += token.size())
        ++count;
    return count;
}

void check_payloads(
    const mutators::PipelineResult& earlier, const mutators::PipelineResult& later
) {
    OA_CHECK(earlier.layer.entries.size() == later.layer.entries.size());
    if (earlier.layer.entries.size() != later.layer.entries.size())
        return;
    for (std::size_t index = 0; index < earlier.layer.entries.size(); ++index) {
        const oa::MutatorLayerEntry& one = earlier.layer.entries[index];
        const oa::MutatorLayerEntry& other = later.layer.entries[index];
        OA_CHECK(one.path == other.path && one.kind == other.kind);
        const bool same = (one.contents == nullptr && other.contents == nullptr) ||
                          (one.contents != nullptr && other.contents != nullptr &&
                           *one.contents == *other.contents);
        OA_CHECK(same);
    }
}

void test_places(const fs::path& game, const fs::path& packages) {
    write_package(
        packages,
        "ridge-free",
        "\"menus\"",
        hook_script(
            "menus",
            "local place = collection:add(\"RGEMPTY\", \"RGPLANT\")\n"
            "      if place.page ~= 1 or place.slot ~= 1 then error(\"free \"..place.page..\" "
            "\"..place.slot) end"
        )
    );
    mutators::PipelineResult free_run = run_packages(game, packages, {"ridge-free"});
    if (!applied(free_run))
        return;
    const std::string planted = served_text(find_served(free_run, "rgplant.tdf"));
    // Player page 1 is MENU=2 and slot 1 is BUTTON=0.
    OA_CHECK(
        text_has(planted, "MENU=2") && text_has(planted, "BUTTON=0") &&
        text_has(planted, "PLACEHOLDER")
    );
    OA_CHECK(find_served(free_run, "rgplant.fbi") == nullptr);
    OA_CHECK(find_note(free_run, "has no picture") == nullptr);

    write_package(
        packages,
        "ridge-exact",
        "\"menus\"",
        hook_script(
            "menus",
            "local place = collection:add(\"RGEMPTY\", \"RGBASE\", { page = 2, slot = 3 })\n"
            "      if place.page ~= 2 or place.slot ~= 3 then error(\"exact \"..place.page..\" "
            "\"..place.slot) end"
        )
    );
    mutators::PipelineResult exact = run_packages(game, packages, {"ridge-exact"});
    if (applied(exact)) {
        const std::string text = served_text(find_served(exact, "rgbase.tdf"));
        OA_CHECK(text_has(text, "MENU=3") && text_has(text, "BUTTON=2"));
        check_note(exact, "RGBASE's button has no picture on RGEMPTY");
    }

    write_package(
        packages,
        "ridge-page",
        "\"menus\"",
        hook_script(
            "menus",
            "local place = collection:add(\"RGBLDR\", \"RGPLANT\", { page = 1, slot = 1 })\n"
            "      if place.page ~= 2 or place.slot ~= 1 then error(\"page \"..place.page..\" "
            "\"..place.slot) end"
        )
    );
    mutators::PipelineResult bumped = run_packages(game, packages, {"ridge-page"});
    if (applied(bumped))
        check_note(bumped, "taken by the page itself");

    write_package(
        packages,
        "ridge-new",
        "\"menus\"",
        hook_script(
            "menus",
            "local place = collection:add(\"RGBLDR\", \"RGBASE\", { page = \"new\" })\n"
            "      if place.page ~= 2 or place.slot ~= 1 then error(\"new \"..place.page..\" "
            "\"..place.slot) end"
        )
    );
    mutators::PipelineResult fresh = run_packages(game, packages, {"ridge-new"});
    if (applied(fresh)) {
        const std::string text = served_text(find_served(fresh, "rgbase.tdf"));
        OA_CHECK(text_has(text, "MENU=3") && text_has(text, "BUTTON=0"));
    }

    write_package(
        packages,
        "ridge-full",
        "\"menus\"",
        hook_script("menus", "collection:add(\"RGEMPTY\", \"RGPLANT\", { page = \"new\" })")
    );
    failed_with(
        run_packages(game, packages, {"ridge-full"}),
        mutators::Cause::script_error,
        "no free build slot"
    );

    write_package(
        packages,
        "ridge-eight",
        "\"menus\"",
        hook_script("menus", "collection:add(\"RGEMPTY\", \"RGPLANT\", { page = 8, slot = 1 })")
    );
    failed_with(
        run_packages(game, packages, {"ridge-eight"}),
        mutators::Cause::script_error,
        "page must be from 1 to 7"
    );

    write_package(
        packages,
        "ridge-again",
        "\"menus\"",
        hook_script(
            "menus",
            "local place = collection:add(\"RGBLDR\", \"RGONE\")\n"
            "      if place.page ~= 1 or place.slot ~= 1 then error(\"again \"..place.page..\" "
            "\"..place.slot) end"
        )
    );
    mutators::PipelineResult again = run_packages(game, packages, {"ridge-again"});
    if (applied(again))
        OA_CHECK(again.layer.entries.empty());

    write_package(
        packages,
        "ridge-earlier",
        "\"menus\"",
        hook_script("menus", "collection:add(\"RGEMPTY\", \"RGPLANT\", { page = 2, slot = 1 })")
    );
    write_package(
        packages,
        "ridge-later",
        "\"menus\"",
        hook_script(
            "menus",
            "local place = collection:add(\"RGEMPTY\", \"RGBASE\", { page = 2, slot = 1 })\n"
            "      if place.page ~= 1 or place.slot ~= 1 then error(\"later \"..place.page..\" "
            "\"..place.slot) end"
        )
    );
    mutators::PipelineResult both = run_packages(game, packages, {"ridge-earlier", "ridge-later"});
    OA_CHECK(both.mutators.size() == 2);
    if (both.mutators.size() == 2 && both.mutators[0].outcome == mutators::Outcome::applied &&
        both.mutators[1].outcome == mutators::Outcome::applied) {
        const mutators::Conflict* conflict = find_note(both, "taken by ridge-earlier");
        OA_CHECK(conflict != nullptr);
        if (conflict != nullptr) {
            OA_CHECK(
                conflict->mutators.size() == 2 && conflict->mutators[0] == 0 &&
                conflict->mutators[1] == 1
            );
            OA_CHECK(mutators::describe(*conflict, id_list(both)) == conflict->detail);
        }
        OA_CHECK(text_has(served_text(find_served(both, "rgplant.tdf")), "MENU=3"));
        OA_CHECK(text_has(served_text(find_served(both, "rgbase.tdf")), "MENU=2"));
    } else {
        show_failure(both);
    }

    mutators::PipelineResult second = run_packages(game, packages, {"ridge-free"});
    check_payloads(free_run, second);
}

void test_move_remove_style(const fs::path& game, const fs::path& packages) {
    write_package(
        packages,
        "ridge-move",
        "\"menus\"",
        hook_script(
            "menus",
            "local place = collection:move(\"RGBLDR\", \"RGMARK\", { page = 2, slot = 1 })\n"
            "      if place.page ~= 2 or place.slot ~= 1 then error(\"move \"..place.page..\" "
            "\"..place.slot) end"
        )
    );
    mutators::PipelineResult moved = run_packages(game, packages, {"ridge-move"});
    if (applied(moved)) {
        const std::string text = served_text(find_served(moved, "marked.tdf"));
        OA_CHECK(
            text_has(text, "MENU=3") && text_has(text, "BUTTON=0") &&
            text_has(text, "TAILKEY=ridgekept")
        );
    }

    write_package(
        packages,
        "ridge-stuck",
        "\"menus\"",
        hook_script("menus", "collection:move(\"RGBLDR\", \"RGONE\", { page = 2, slot = 1 })")
    );
    failed_with(
        run_packages(game, packages, {"ridge-stuck"}),
        mutators::Cause::script_error,
        "button is part of RGBLDR's page 1"
    );

    write_package(
        packages,
        "ridge-drop-one",
        "\"menus\"",
        hook_script("menus", "collection:remove(\"RGBLDR\", \"RGSHARED\")")
    );
    mutators::PipelineResult one = run_packages(game, packages, {"ridge-drop-one"});
    if (applied(one)) {
        const std::string text = served_text(find_served(one, "shared.tdf"));
        OA_CHECK(text_has(text, "RGALT") && !text_has(text, "RGBLDR"));
    }

    write_package(
        packages,
        "ridge-drop-all",
        "\"menus\"",
        hook_script("menus", "collection:remove(\"RGSHARED\")")
    );
    mutators::PipelineResult all = run_packages(game, packages, {"ridge-drop-all"});
    if (applied(all)) {
        const oa::MutatorLayerEntry* shared = find_served(all, "shared.tdf");
        OA_CHECK(shared != nullptr && shared->kind == oa::MutatorSourceKind::hidden);
    }

    write_package(
        packages,
        "ridge-grey",
        "\"menus\"",
        hook_script(
            "menus",
            "collection:grey(\"RGBLDR\", \"RGONE\")\n"
            "      collection:grey(\"RGBLDR\", \"RGTHREE\")\n"
            "      collection:hide(\"RGBLDR\", \"RGFOUR\")"
        )
    );
    mutators::PipelineResult grey = run_packages(game, packages, {"ridge-grey"});
    if (applied(grey)) {
        const std::string text = served_text(find_served(grey, "rgbldr1.gui"));
        OA_CHECK(text_has(text, "grayedout=1"));
        OA_CHECK(count_token(text, "[GADGET2]") >= 2);
        OA_CHECK(find_served(grey, ".fbi") == nullptr);
        OA_CHECK(find_served(grey, ".tdf") == nullptr);
        check_grey_page(text);
    }

    write_package(
        packages,
        "ridge-grey-download",
        "\"menus\"",
        hook_script("menus", "collection:grey(\"RGBLDR\", \"RGMARK\")")
    );
    failed_with(
        run_packages(game, packages, {"ridge-grey-download"}),
        mutators::Cause::script_error,
        "download entry"
    );

    write_package(
        packages,
        "ridge-grey-again",
        "\"menus\"",
        hook_script(
            "menus",
            "collection:grey(\"RGBLDR\", \"RGONE\")\n"
            "      local place = collection:add(\"RGBLDR\", \"RGONE\")\n"
            "      if place.page ~= 1 or place.slot ~= 1 then error(\"grey add \"..place.page..\" "
            "\"..place.slot) end"
        )
    );
    mutators::PipelineResult grey_add = run_packages(game, packages, {"ridge-grey-again"});
    if (applied(grey_add))
        OA_CHECK(find_served(grey_add, "rgone.tdf") == nullptr);
}

void test_builds_and_notes(const fs::path& game, const fs::path& packages) {
    write_package(
        packages,
        "ridge-builds",
        "\"menus\"",
        hook_script(
            "menus",
            "local list = collection:builds(\"RGBLDR\")\n"
            "      local got = \"\"\n"
            "      for i = 1, #list do\n"
            "        if i > 1 then got = got .. \",\" end\n"
            "        got = got .. list[i]\n"
            "      end\n"
            "      local want = "
            "\"RGONE,RGTWO,RGTHREE,RGFOUR,RGFIVE,RGSIX,RGMARK,RGOTHER,RGSHARED\"\n"
            "      if got ~= want then error(got) end"
        )
    );
    applied(run_packages(game, packages, {"ridge-builds"}));

    write_package(
        packages,
        "ridge-ai",
        "\"menus\"",
        hook_script(
            "menus",
            "collection:add(\"RGAIFULL\", \"RGEXTRA1\")\n"
            "      collection:add(\"RGAIFULL\", \"RGEXTRA2\")"
        )
    );
    mutators::PipelineResult ai = run_packages(game, packages, {"ridge-ai"});
    if (applied(ai)) {
        check_note(ai, "the AI does not see RGEXTRA2 on RGAIFULL: RGAIFULL's list is full");
        OA_CHECK(find_note(ai, "does not see RGEXTRA1") == nullptr);
    }
}

void test_removal_and_marks(
    const fs::path& game, const fs::path& packages, const UnitTable& table
) {
    const std::vector<MemoryFile> before = disk_downloads(game);
    write_package(
        packages,
        "ridge-plant",
        "\"menus\"",
        hook_script("menus", "collection:add(\"RGEMPTY\", \"RGPLANT\")")
    );
    mutators::PipelineResult planted = run_packages(game, packages, {"ridge-plant"});
    if (applied(planted)) {
        const std::vector<MemoryFile> after = overlay_downloads(game, planted);
        OA_CHECK(marks_match(before, after, table, ""));
        OA_CHECK(builder_lists(after, table, "RGEMPTY", "RGPLANT"));
        const std::string text = served_text(find_served(planted, "rgplant.tdf"));
        OA_CHECK(text_has(text, "PLACEHOLDER"));
    }

    write_package(
        packages,
        "ridge-marked",
        "\"menus\"",
        hook_script("menus", "collection:add(\"RGEMPTY\", \"RGMARK\")")
    );
    mutators::PipelineResult marked = run_packages(game, packages, {"ridge-marked"});
    if (applied(marked)) {
        const std::string text = served_text(find_served(marked, "rgmark.tdf"));
        OA_CHECK(text_has(text, "UNITNAME=RGMARK") && !text_has(text, "PLACEHOLDER"));
        OA_CHECK(marks_match(before, overlay_downloads(game, marked), table, ""));
    }

    write_package(
        packages,
        "ridge-remove-unit",
        "\"units\"",
        hook_script(
            "units",
            "collection.RGMARK:remove()\n"
            "      collection.RGONE:remove()"
        )
    );
    mutators::PipelineResult removed = run_packages(game, packages, {"ridge-remove-unit"});
    if (!applied(removed))
        return;
    const std::string download = served_text(find_served(removed, "marked.tdf"));
    OA_CHECK(
        text_has(download, "PLACEHOLDER") && text_has(download, "RGOTHER") &&
        !text_has(download, "RGMARK")
    );
    const std::string page = served_text(find_served(removed, "rgbldr1.gui"));
    OA_CHECK(text_has(page, "grayedout=1"));
    bool saw_removal = false;
    bool saw_grey = false;
    for (const mutators::JournalEntry& entry : removed.journal.entries) {
        if (entry.mutators.size() != 1 || entry.mutators[0] != 0)
            continue;
        if (entry.change == mutators::Change::removed &&
            entry.collection == mutators::Collection::menus)
            saw_removal = true;
        if (entry.field == "grayedout" && entry.record == "RGONE")
            saw_grey = true;
    }
    OA_CHECK(saw_removal && saw_grey);
    const std::vector<std::string> later = marks_of(overlay_downloads(game, removed), table.units);
    OA_CHECK(std::find_if(later.begin(), later.end(), [](const std::string& name) {
                 return fold_copy(name) == "rgother";
             }) == later.end());
    OA_CHECK(std::find_if(later.begin(), later.end(), [](const std::string& name) {
                 return fold_copy(name) == "rgmark";
             }) == later.end());
    // The first place is a placeholder, so the note names no unit as marked.
    check_note(removed, "a placeholder now holds the first place, so no unit gains a mark");
    OA_CHECK(find_note(removed, "as the unit this file marks") == nullptr);
}

void test_layers(const fs::path& game, const fs::path& packages) {
    write_package(
        packages,
        "ridge-units-add",
        "\"units\"",
        hook_script("units", "ctx.menus:add(\"RGEMPTY\", \"RGPLANT\")")
    );
    failed_with(
        run_packages(game, packages, {"ridge-units-add"}), mutators::Cause::script_error, "menus"
    );

    write_package(
        packages,
        "ridge-units-builds",
        "\"units\"",
        hook_script(
            "units",
            "local list = ctx.menus:builds(\"RGBLDR\")\n"
            "      if #list < 1 then error(\"empty builds\") end"
        )
    );
    mutators::PipelineResult builds = run_packages(game, packages, {"ridge-units-builds"});
    if (applied(builds))
        OA_CHECK(builds.layer.entries.empty());

    write_package(
        packages,
        "ridge-no-layer",
        "\"units\"",
        hook_script("menus", "collection:builds(\"RGBLDR\")")
    );
    mutators::PipelineResult undeclared = run_packages(game, packages, {"ridge-no-layer"});
    OA_CHECK(!undeclared.mutators.empty());
    if (!undeclared.mutators.empty()) {
        OA_CHECK(undeclared.mutators.front().outcome == mutators::Outcome::failed);
        OA_CHECK(undeclared.mutators.front().cause == mutators::Cause::undeclared_layer);
        OA_CHECK(undeclared.mutators.front().message.find("menus") != std::string::npos);
    }
}

void test_entry_identity(const fs::path& game, const fs::path& packages) {
    write_package(
        packages,
        "ridge-add-plant",
        "\"menus\"",
        hook_script("menus", "collection:add(\"RGEMPTY\", \"RGPLANT\")")
    );
    write_package(
        packages,
        "ridge-add-base",
        "\"menus\"",
        hook_script("menus", "collection:add(\"RGEMPTY\", \"RGBASE\")")
    );
    mutators::PipelineResult added =
        run_packages(game, packages, {"ridge-add-plant", "ridge-add-base"});
    OA_CHECK(added.mutators.size() == 2);
    if (added.mutators.size() == 2 && added.mutators[0].outcome == mutators::Outcome::applied &&
        added.mutators[1].outcome == mutators::Outcome::applied) {
        const std::string plant = served_text(find_served(added, "rgplant.tdf"));
        const std::string base = served_text(find_served(added, "rgbase.tdf"));
        OA_CHECK(
            text_has(plant, "UNITNAME=RGPLANT") && text_has(plant, "UNITMENU=RGEMPTY") &&
            text_has(plant, "[MENUENTRY1]") && !text_has(plant, "RGBASE")
        );
        OA_CHECK(
            text_has(base, "UNITNAME=RGBASE") && text_has(base, "UNITMENU=RGEMPTY") &&
            text_has(base, "[MENUENTRY1]") && !text_has(base, "RGPLANT")
        );
        check_note(added, "RGBASE's button has no picture on RGEMPTY");
        OA_CHECK(find_note(added, "RGPLANT's button has no picture") == nullptr);
    } else {
        show_failure(added);
    }

    // Both sections use the positional name. The second unit is the one removed.
    write_bytes(
        game / "download" / "twinned.tdf",
        "[MENUENTRY1]\r\n{\r\nUNITMENU=RGBLDR;\r\nMENU=2;\r\nBUTTON=4;\r\nUNITNAME=RGBASE;\r\n}"
        "\r\n"
        "[MENUENTRY1]\r\n{\r\nUNITMENU=RGBLDR;\r\nMENU=2;\r\nBUTTON=5;\r\nUNITNAME=RGPLANT;\r\n}"
        "\r\n"
    );
    write_package(
        packages,
        "ridge-drop-plant",
        "\"units\"",
        hook_script("units", "collection.RGPLANT:remove()")
    );
    mutators::PipelineResult dropped = run_packages(game, packages, {"ridge-drop-plant"});
    if (!applied(dropped))
        return;
    const std::string twinned = served_text(find_served(dropped, "twinned.tdf"));
    OA_CHECK(text_has(twinned, "UNITNAME=RGBASE"));
    OA_CHECK(!text_has(twinned, "RGPLANT"));
    OA_CHECK(!text_has(twinned, "PLACEHOLDER"));
}

void test_unknown(const fs::path& game, const fs::path& packages) {
    write_package(
        packages,
        "ridge-unknown",
        "\"menus\"",
        hook_script("menus", "collection:add(\"RGEMPTY\", \"NOPE\")")
    );
    failed_with(
        run_packages(game, packages, {"ridge-unknown"}),
        mutators::Cause::script_error,
        "unknown unit NOPE"
    );
}

/// The script of a package with a units hook and a menus hook.
std::string two_hooks(std::string_view units_body, std::string_view menus_body) {
    std::string lua = "function mutate(ctx)\n  return {\n    units = function(units, ctx)\n";
    lua.append(units_body);
    lua += "\n    end,\n    menus = function(menus, ctx)\n";
    lua.append(menus_body);
    lua += "\n    end\n  }\nend\n";
    return lua;
}

/// A menus hook that adds `unit` to `builder` at an exact place and checks where it landed.
std::string add_lands(
    std::string_view builder,
    std::string_view unit,
    int page,
    int slot,
    int want_page,
    int want_slot
) {
    std::string lua = "local place = collection:add(\"";
    lua.append(builder);
    lua += "\", \"";
    lua.append(unit);
    lua += "\", { page = " + std::to_string(page) + ", slot = " + std::to_string(slot) + " })\n";
    lua += "      if place.page ~= " + std::to_string(want_page) +
           " or place.slot ~= " + std::to_string(want_slot) +
           " then error(\"landed \"..place.page..\" \"..place.slot) end";
    return lua;
}

bool all_applied(const mutators::PipelineResult& run, std::size_t count) {
    bool ok = run.refusal.empty() && run.mutators.size() == count;
    for (const mutators::MutatorResult& one : run.mutators)
        ok = ok && one.outcome == mutators::Outcome::applied;
    if (!ok)
        show_failure(run);
    OA_CHECK(ok);
    return ok;
}

/// The top-level sections of a served text, with the key asked for.
std::vector<std::pair<std::string, std::string>>
sections_of(const std::string& text, const char* key) {
    std::vector<std::pair<std::string, std::string>> sections;
    oa::formats::tdf::OwnedDocument parsed;
    if (!parsed.parse(text)) {
        OA_CHECK(false);
        return sections;
    }
    const oa::formats::tdf::Block* root = parsed.root();
    for (uint32_t index = 0; index < oa::formats::tdf::child_count(root); ++index) {
        const oa::formats::tdf::Block* section = oa::formats::tdf::child_at(root, index);
        const char* value = oa::formats::tdf::find_value(section, key);
        sections.emplace_back(
            section->name != nullptr ? section->name : "", value != nullptr ? value : ""
        );
    }
    return sections;
}

/// Q64: a slot an earlier mutator emptied is taken exactly by a later one,
/// whether the earlier one emptied it with menus:remove or by removing the
/// unit. Without the removal the later add moves.
void test_reuse(const fs::path& game, const fs::path& packages) {
    write_package(
        packages,
        "ridge-take-slot",
        "\"menus\"",
        hook_script("menus", add_lands("RGALT", "RGPLANT", 1, 1, 1, 1))
    );
    write_package(
        packages,
        "ridge-free-slot",
        "\"menus\"",
        hook_script("menus", "collection:remove(\"RGALT\", \"RGSHARED\")")
    );
    mutators::PipelineResult freed =
        run_packages(game, packages, {"ridge-free-slot", "ridge-take-slot"});
    if (all_applied(freed, 2)) {
        OA_CHECK(find_note(freed, "moved from") == nullptr);
        const std::string shared = served_text(find_served(freed, "shared.tdf"));
        OA_CHECK(text_has(shared, "RGBLDR") && !text_has(shared, "RGALT"));
    }

    write_package(
        packages,
        "ridge-drop-shared",
        "\"units\"",
        hook_script("units", "collection.RGSHARED:remove()")
    );
    mutators::PipelineResult gone =
        run_packages(game, packages, {"ridge-drop-shared", "ridge-take-slot"});
    if (all_applied(gone, 2))
        OA_CHECK(find_note(gone, "moved from") == nullptr);

    write_package(
        packages,
        "ridge-bumped-slot",
        "\"menus\"",
        hook_script("menus", add_lands("RGALT", "RGPLANT", 1, 1, 1, 2))
    );
    mutators::PipelineResult kept = run_packages(game, packages, {"ridge-bumped-slot"});
    if (applied(kept))
        check_note(
            kept, "RGPLANT moved from page 1 slot 1 to page 1 slot 2: taken by the page itself"
        );

    // The same mutator: its menus hook sees the slot its units hook emptied.
    write_package(
        packages,
        "ridge-same-step",
        "\"units\", \"menus\"",
        two_hooks(
            "units.RGSHARED:remove()",
            "local place = menus:add(\"RGALT\", \"RGPLANT\", { page = 1, slot = 1 })\n"
            "      if place.page ~= 1 or place.slot ~= 1 then error(\"same \"..place.page..\" "
            "\"..place.slot) end"
        )
    );
    mutators::PipelineResult same = run_packages(game, packages, {"ridge-same-step"});
    if (applied(same))
        OA_CHECK(find_note(same, "moved from") == nullptr);

    // And through ctx.menus in the units hook itself, right after the removal.
    write_package(
        packages,
        "ridge-same-hook",
        "\"units\", \"menus\"",
        hook_script(
            "units",
            "collection.RGSHARED:remove()\n"
            "      local place = ctx.menus:add(\"RGALT\", \"RGPLANT\", { page = 1, slot = 1 })\n"
            "      if place.page ~= 1 or place.slot ~= 1 then error(\"hook \"..place.page..\" "
            "\"..place.slot) end"
        )
    );
    applied(run_packages(game, packages, {"ridge-same-hook"}));
}

/// A rule's change to a download entry stays when a later mutator edits the
/// same file, and a slot the rule freed is free.
void test_rule_then_menus(const fs::path& game, const fs::path& packages) {
    write_package(
        packages,
        "ridge-rule-button",
        "\"tables\"",
        "function mutate(ctx)\n  return {\n    rules = {\n"
        "      where { unitname = is(\"RGSHARED\"), unitmenu = is(\"RGALT\") }\n"
        "        :on(\"download\", \"tdf\"):change { button = 4 }\n"
        "    }\n  }\nend\n"
    );
    write_package(
        packages,
        "ridge-after-rule",
        "\"menus\"",
        hook_script("menus", add_lands("RGALT", "RGPLANT", 1, 1, 1, 1))
    );
    mutators::PipelineResult moved =
        run_packages(game, packages, {"ridge-rule-button", "ridge-after-rule"});
    if (all_applied(moved, 2))
        OA_CHECK(find_note(moved, "moved from") == nullptr);

    write_package(
        packages,
        "ridge-drop-builder",
        "\"menus\"",
        hook_script("menus", "collection:remove(\"RGBLDR\", \"RGSHARED\")")
    );
    mutators::PipelineResult kept =
        run_packages(game, packages, {"ridge-rule-button", "ridge-drop-builder"});
    if (!all_applied(kept, 2))
        return;
    const std::string shared = served_text(find_served(kept, "shared.tdf"));
    const std::vector<std::pair<std::string, std::string>> buttons = sections_of(shared, "BUTTON");
    OA_CHECK(buttons.size() == 1);
    if (buttons.size() == 1) {
        OA_CHECK(buttons[0].first == "B");
        OA_CHECK(buttons[0].second == "4");
    }
    OA_CHECK(text_has(shared, "UNITMENU=RGALT") && !text_has(shared, "RGBLDR"));
    bool rule_kept = false;
    for (const mutators::JournalEntry& entry : kept.journal.entries) {
        if (entry.phase == mutators::Phase::rules && entry.record == "B" &&
            entry.new_text == std::optional<std::string>{"4"})
            rule_kept = true;
    }
    OA_CHECK(rule_kept);
}

/// A unit or a builder a hook clones can be put in a menu, from the same
/// hook, the same mutator's menus hook or a later mutator. A cloned builder
/// keeps its source's pages, so their slots stay taken.
void test_clones(const fs::path& game, const fs::path& packages) {
    write_package(
        packages,
        "ridge-clone-add",
        "\"units\", \"menus\"",
        hook_script(
            "units",
            "collection:clone(\"RGPLANT\", \"RGCLONE\", {})\n"
            "      ctx.menus:add(\"RGEMPTY\", \"RGCLONE\")"
        )
    );
    mutators::PipelineResult hook = run_packages(game, packages, {"ridge-clone-add"});
    if (applied(hook)) {
        const std::string text = served_text(find_served(hook, "download/rgclone.tdf"));
        OA_CHECK(text_has(text, "UNITNAME=RGCLONE") && text_has(text, "UNITMENU=RGEMPTY"));
    }

    write_package(
        packages,
        "ridge-clone-step",
        "\"units\", \"menus\"",
        two_hooks(
            "units:clone(\"RGPLANT\", \"RGCLONE\", {})",
            "menus:add(\"RGEMPTY\", \"RGCLONE\")\n"
            "      local list = menus:builds(\"RGEMPTY\")\n"
            "      if list[1] ~= \"RGCLONE\" then error(\"builds \"..tostring(list[1])) end"
        )
    );
    applied(run_packages(game, packages, {"ridge-clone-step"}));

    write_package(
        packages,
        "ridge-clone-only",
        "\"units\"",
        hook_script(
            "units",
            "collection:clone(\"RGPLANT\", \"RGCLONE\", {})\n"
            "      collection:clone(\"RGALT\", \"RGALT2\", {})\n"
            "      collection:clone(\"RGBLDR\", \"RGBLDR2\", {})"
        )
    );
    write_package(
        packages,
        "ridge-clone-later",
        "\"menus\"",
        hook_script(
            "menus",
            "collection:add(\"RGEMPTY\", \"RGCLONE\")\n"
            "      collection:add(\"RGALT2\", \"RGPLANT\")\n" +
                add_lands("RGBLDR2", "RGBASE", 1, 1, 2, 1) +
                "\n      collection:grey(\"RGBLDR2\", \"RGONE\")\n"
                "      local list = collection:builds(\"RGBLDR2\")\n"
                "      if list[1] ~= \"RGTWO\" then error(\"clone page \"..tostring(list[1])) end"
        )
    );
    mutators::PipelineResult later =
        run_packages(game, packages, {"ridge-clone-only", "ridge-clone-later"});
    if (!all_applied(later, 2))
        return;
    OA_CHECK(text_has(served_text(find_served(later, "download/rgclone.tdf")), "RGEMPTY"));
    OA_CHECK(text_has(served_text(find_served(later, "download/rgplant.tdf")), "RGALT2"));
    check_note(later, "RGBASE moved from page 1 slot 1 to page 2 slot 1: taken by the page itself");
    const std::string page = served_text(find_served(later, "guis/rgbldr21.gui"));
    OA_CHECK(text_has(page, "grayedout=1"));
    OA_CHECK(served_text(find_served(later, "guis/rgbldr1.gui")).empty());
}

/// Removing an entry from a file of more than five sections leaves the
/// sections past the fifth unread: a placeholder takes the entry's place.
void test_six_sections(const fs::path& game, const fs::path& packages, const UnitTable& table) {
    write_package(
        packages,
        "ridge-drop-other",
        "\"menus\"",
        hook_script("menus", "collection:remove(\"RGBLDR\", \"RGOTHER\")")
    );
    mutators::PipelineResult marked = run_packages(game, packages, {"ridge-drop-other"});
    if (applied(marked)) {
        const std::string text = served_text(find_served(marked, "marked.tdf"));
        const std::vector<std::pair<std::string, std::string>> units =
            sections_of(text, "UNITNAME");
        OA_CHECK(units.size() == 6);
        if (units.size() == 6) {
            OA_CHECK(units[0].second == "RGMARK");
            OA_CHECK(units[1].first == "PLACEHOLDER" && units[1].second.empty());
            OA_CHECK(units[5].first == "TAIL");
        }
        OA_CHECK(marks_match(disk_downloads(game), overlay_downloads(game, marked), table, ""));
    }

    write_bytes(
        game / "download" / "six.tdf",
        "[E1]\r\n{\r\nUNITMENU=RGALT;\r\nMENU=3;\r\nBUTTON=0;\r\nUNITNAME=RGONE;\r\n}\r\n"
        "[E2]\r\n{\r\nUNITMENU=RGALT;\r\nMENU=3;\r\nBUTTON=1;\r\nUNITNAME=RGTWO;\r\n}\r\n"
        "[E3]\r\n{\r\nUNITMENU=RGALT;\r\nMENU=3;\r\nBUTTON=2;\r\nUNITNAME=RGTHREE;\r\n}\r\n"
        "[E4]\r\n{\r\nUNITMENU=RGALT;\r\nMENU=3;\r\nBUTTON=3;\r\nUNITNAME=RGFOUR;\r\n}\r\n"
        "[E5]\r\n{\r\nUNITMENU=RGALT;\r\nMENU=3;\r\nBUTTON=4;\r\nUNITNAME=RGFIVE;\r\n}\r\n"
        "[E6]\r\n{\r\nUNITMENU=RGALT;\r\nMENU=3;\r\nBUTTON=5;\r\nUNITNAME=RGEXTRA2;\r\n}\r\n"
    );
    write_package(
        packages,
        "ridge-drop-two",
        "\"menus\"",
        hook_script("menus", "collection:remove(\"RGALT\", \"RGTWO\")")
    );
    const std::vector<MemoryFile> before = disk_downloads(game);
    OA_CHECK(!builder_lists(before, table, "RGALT", "RGEXTRA2"));
    OA_CHECK(builder_lists(before, table, "RGALT", "RGTWO"));
    mutators::PipelineResult six = run_packages(game, packages, {"ridge-drop-two"});
    if (applied(six)) {
        const std::vector<MemoryFile> after = overlay_downloads(game, six);
        OA_CHECK(!builder_lists(after, table, "RGALT", "RGEXTRA2"));
        OA_CHECK(!builder_lists(after, table, "RGALT", "RGTWO"));
        OA_CHECK(builder_lists(after, table, "RGALT", "RGFIVE"));
        OA_CHECK(marks_match(before, after, table, ""));
        const std::vector<std::pair<std::string, std::string>> units =
            sections_of(served_text(find_served(six, "six.tdf")), "UNITNAME");
        OA_CHECK(units.size() == 6 && units[5].second == "RGEXTRA2");
    }

    // The first entry goes and no other entry of that unit is in the file:
    // a placeholder takes the first place, so RGTWO is not marked.
    write_package(
        packages,
        "ridge-drop-first",
        "\"menus\"",
        hook_script("menus", "collection:remove(\"RGALT\", \"RGONE\")")
    );
    mutators::PipelineResult first = run_packages(game, packages, {"ridge-drop-first"});
    if (applied(first)) {
        const std::vector<MemoryFile> after = overlay_downloads(game, first);
        OA_CHECK(!builder_lists(after, table, "RGALT", "RGEXTRA2"));
        OA_CHECK(marks_match(before, after, table, "RGONE"));
        const std::vector<std::pair<std::string, std::string>> units =
            sections_of(served_text(find_served(first, "six.tdf")), "UNITNAME");
        OA_CHECK(units.size() == 6 && units[0].first == "PLACEHOLDER");
    }
    std::error_code error;
    fs::remove(game / "download" / "six.tdf", error);
}

/// A download file the menus create is journaled as a file added.
void test_added_file(const fs::path& game, const fs::path& packages) {
    write_package(
        packages,
        "ridge-new-file",
        "\"menus\"",
        hook_script("menus", "collection:add(\"RGEMPTY\", \"RGPLANT\")")
    );
    mutators::PipelineResult run = run_packages(game, packages, {"ridge-new-file"});
    if (!applied(run))
        return;
    bool added = false;
    for (const mutators::JournalEntry& entry : run.journal.entries) {
        if (entry.change == mutators::Change::added &&
            entry.collection == mutators::Collection::files &&
            fold_copy(entry.path) == "download/rgplant.tdf")
            added = true;
    }
    OA_CHECK(added);
}

/// A download file created and then emptied in one mutator is not journaled,
/// and the next entry for that unit uses the same path.
void test_created_then_emptied(const fs::path& game, const fs::path& packages) {
    write_package(
        packages,
        "ridge-empty-file",
        "\"menus\"",
        hook_script(
            "menus",
            "collection:add(\"RGEMPTY\", \"RGPLANT\")\n"
            "      collection:remove(\"RGEMPTY\", \"RGPLANT\")"
        )
    );
    mutators::PipelineResult emptied = run_packages(game, packages, {"ridge-empty-file"});
    if (!applied(emptied))
        return;
    for (const mutators::JournalEntry& entry : emptied.journal.entries) {
        OA_CHECK(fold_copy(entry.path).find("rgplant.tdf") == std::string::npos);
    }
    OA_CHECK(find_served(emptied, "rgplant.tdf") == nullptr);

    write_package(
        packages,
        "ridge-reuse-file",
        "\"menus\"",
        hook_script(
            "menus",
            "collection:add(\"RGEMPTY\", \"RGPLANT\")\n"
            "      collection:remove(\"RGEMPTY\", \"RGPLANT\")\n"
            "      collection:add(\"RGEMPTY\", \"RGPLANT\")"
        )
    );
    mutators::PipelineResult reused = run_packages(game, packages, {"ridge-reuse-file"});
    if (!applied(reused))
        return;
    const oa::MutatorLayerEntry* served = find_served(reused, "rgplant.tdf");
    OA_CHECK(served != nullptr);
    if (served == nullptr)
        return;
    OA_CHECK(served->kind != oa::MutatorSourceKind::hidden);
    OA_CHECK(fold_copy(served->path).find("rgplant-2") == std::string::npos);
    OA_CHECK(text_has(served_text(served), "UNITNAME=RGPLANT"));
    bool hidden = false;
    int added_files = 0;
    for (const mutators::JournalEntry& entry : reused.journal.entries) {
        if (fold_copy(entry.path).find("rgplant") == std::string::npos)
            continue;
        if (entry.change == mutators::Change::hidden)
            hidden = true;
        if (entry.change == mutators::Change::added &&
            entry.collection == mutators::Collection::files)
            ++added_files;
        OA_CHECK(fold_copy(entry.path).find("rgplant-2") == std::string::npos);
    }
    OA_CHECK(!hidden);
    OA_CHECK(added_files == 1);
}

/// Refused calls inside pcall leave nothing behind and do not stop the hook.
void test_refusals(const fs::path& game, const fs::path& packages) {
    write_package(
        packages,
        "ridge-refusals",
        "\"menus\"",
        hook_script(
            "menus",
            "local refused = 0\n"
            "      local tries = {\n"
            "        function() return collection:frobnicate_everything_please() end,\n"
            "        function() return collection:add(\"RGEMPTY\", \"NOPE\") end,\n"
            "        function() return collection:add(\"NOPE\", \"RGPLANT\") end,\n"
            "        function() return collection:add(\"RGONE\", \"RGPLANT\") end,\n"
            "        function() return collection:add(\"RGEMPTY\", \"RGPLANT\", { page = 9, slot = "
            "1 }) end,\n"
            "        function() return collection:add(\"RGEMPTY\", \"RGPLANT\", { page = 1, slot = "
            "9 }) end,\n"
            "        function() return collection:add(\"RGEMPTY\", \"RGPLANT\", { page = \"new\" "
            "}) end,\n"
            "        function() return collection:move(\"RGBLDR\", \"RGONE\", { page = 2, slot = 1 "
            "}) end,\n"
            "        function() return collection:move(\"RGBLDR\", \"RGEXTRA1\", { page = 2, slot "
            "= 1 }) end,\n"
            "        function() return collection:grey(\"RGBLDR\", \"RGMARK\") end,\n"
            "        function() return collection:hide(\"RGBLDR\", \"RGEXTRA1\") end,\n"
            "        function() return collection:builds(42) end,\n"
            "        function() return collection:remove() end,\n"
            "        function() collection.add = 1 end,\n"
            "      }\n"
            "      for round = 1, 200 do\n"
            "        for i = 1, #tries do\n"
            "          if not pcall(tries[i]) then refused = refused + 1 end\n"
            "        end\n"
            "      end\n"
            "      if refused ~= 200 * #tries then error(\"refused \"..refused) end"
        )
    );
    mutators::PipelineResult run = run_packages(game, packages, {"ridge-refusals"});
    if (applied(run))
        OA_CHECK(run.layer.entries.empty());
}

/// Runs one package and reports the processor time it took, which a busy
/// machine does not stretch the way it stretches the real time.
mutators::PipelineResult
run_timed(const fs::path& game, const fs::path& packages, const char* id, long& cpu_ms) {
    const std::clock_t cpu = std::clock();
    const auto started = std::chrono::steady_clock::now();
    mutators::PipelineResult run = run_packages(game, packages, {id});
    cpu_ms = static_cast<long>((std::clock() - cpu) * 1000 / CLOCKS_PER_SEC);
    const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started
    );
    std::fprintf(
        stderr,
        "%s: %ld ms of processor time, %lld ms real\n",
        id,
        cpu_ms,
        static_cast<long long>(spent.count())
    );
    return run;
}

/// True for an optimised build. The Visual Studio Debug configuration
/// defines `_DEBUG` and compiles without optimisation; GCC and Clang define
/// `__OPTIMIZE__` only above `-O0`. The Check build type is optimised and
/// does not define `_DEBUG`, so it keeps the tight bound.
constexpr bool optimised_build() {
#if defined(_MSC_VER)
#if defined(_DEBUG)
    return false;
#else
    return true;
#endif
#elif defined(__OPTIMIZE__)
    return true;
#else
    return false;
#endif
}

/// A loop of builds or of removals stops on the instruction allowance, at a
/// small part of the real-time limit's worth of work. The 3 s processor-time
/// bound is for an optimised build: an unoptimised one spends several times
/// that, near the hook's real-time limit, so only the stop is checked there.
void test_budget(const fs::path& game, const fs::path& packages) {
    write_package(
        packages,
        "ridge-builds-loop",
        "\"menus\"",
        hook_script("menus", "for i = 1, 50000000 do collection:builds(\"RGBLDR\") end")
    );
    write_package(
        packages,
        "ridge-remove-loop",
        "\"menus\"",
        hook_script("menus", "for i = 1, 50000000 do collection:remove(\"RGPLANT\") end")
    );
    for (const char* id : {"ridge-builds-loop", "ridge-remove-loop"}) {
        long cpu_ms = 0;
        const mutators::PipelineResult run = run_timed(game, packages, id, cpu_ms);
        OA_CHECK(!run.mutators.empty());
        if (run.mutators.empty())
            continue;
        OA_CHECK(run.mutators.front().cause == mutators::Cause::instruction_budget);
        if (optimised_build())
            OA_CHECK(cpu_ms < 3000);
    }
}

bool path_is_armck_gui(std::string_view path) {
    std::string folded = fold_copy(path);
    for (char& letter : folded) {
        if (letter == '\\')
            letter = '/';
    }
    return folded.rfind("guis/armck", 0) == 0 && folded.size() >= 4 &&
           folded.compare(folded.size() - 4, 4, ".gui") == 0;
}

void test_game_data() {
    oa::AssetStore store = oa::test::require_game_assets("the installed build menus");
    Kept kept;
    const fs::path packages = kept.root / "packages";
    write_package(
        packages,
        "ridge-armcom",
        "\"menus\"",
        hook_script(
            "menus",
            "local list = collection:builds(\"ARMCOM\")\n"
            "      local solar, mex = false, false\n"
            "      for i = 1, #list do\n"
            "        local name = string.lower(list[i])\n"
            "        if name == \"armsolar\" then solar = true end\n"
            "        if name == \"armmex\" then mex = true end\n"
            "      end\n"
            "      if not solar or not mex then error(\"missing commander builds\") end"
        )
    );
    applied(run_on(store, packages, {"ridge-armcom"}));

    // A loop of builds, or of removals, stops on the instruction allowance
    // at a small part of the real-time limit's worth of work.
    write_package(
        packages,
        "ridge-armcom-loop",
        "\"menus\"",
        hook_script("menus", "for i = 1, 50000000 do collection:builds(\"ARMCOM\") end")
    );
    write_package(
        packages,
        "ridge-armsolar-loop",
        "\"menus\"",
        hook_script("menus", "for i = 1, 50000000 do collection:remove(\"ARMSOLAR\") end")
    );
    for (const char* id : {"ridge-armcom-loop", "ridge-armsolar-loop"}) {
        const std::clock_t cpu = std::clock();
        const auto started = std::chrono::steady_clock::now();
        const mutators::PipelineResult loop = run_on(store, packages, {id});
        const long cpu_ms = static_cast<long>((std::clock() - cpu) * 1000 / CLOCKS_PER_SEC);
        const auto spent = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started
        );
        std::fprintf(
            stderr,
            "%s: %ld ms of processor time, %lld ms real\n",
            id,
            cpu_ms,
            static_cast<long long>(spent.count())
        );
        OA_CHECK(!loop.mutators.empty());
        if (loop.mutators.empty())
            continue;
        OA_CHECK(loop.mutators.front().cause == mutators::Cause::instruction_budget);
        if (optimised_build())
            OA_CHECK(cpu_ms < 3000);
    }

    write_package(
        packages, "ridge-read", "\"units\"", hook_script("units", "ctx.menus:builds(\"ARMCOM\")")
    );
    mutators::PipelineResult read = run_on(store, packages, {"ridge-read"});
    if (applied(read))
        OA_CHECK(read.layer.entries.empty());

    // This install's construction kbot names the advanced lab. The aircraft's
    // pages do not.
    write_package(
        packages,
        "ridge-armck",
        "\"menus\"",
        hook_script("menus", "collection:grey(\"ARMCK\", \"ARMALAB\")")
    );
    mutators::PipelineResult painted = run_on(store, packages, {"ridge-armck"});
    if (applied(painted)) {
        OA_CHECK(!painted.layer.entries.empty());
        for (const oa::MutatorLayerEntry& entry : painted.layer.entries)
            OA_CHECK(path_is_armck_gui(entry.path));
    }
}

} // namespace

int main(int argc, char** argv) {
    if (oa::test::game_data_requested(argc, argv)) {
        test_game_data();
        return oa::test::check_exit_status();
    }
    Kept kept;
    const fs::path game = kept.root / "game";
    const fs::path packages = kept.root / "packages";
    plant_world(game);
    UnitTable table;
    test_places(game, packages);
    test_move_remove_style(game, packages);
    test_builds_and_notes(game, packages);
    test_removal_and_marks(game, packages, table);
    test_layers(game, packages);
    test_unknown(game, packages);
    test_entry_identity(game, packages);
    test_reuse(game, packages);
    test_rule_then_menus(game, packages);
    test_clones(game, packages);
    test_six_sections(game, packages, table);
    test_added_file(game, packages);
    test_created_then_emptied(game, packages);
    test_refusals(game, packages);
    test_budget(game, packages);
    return oa::test::check_exit_status();
}

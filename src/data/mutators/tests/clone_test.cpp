// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Clones of made-up Ridge units and weapons through the mutation pipeline.
// Packages are written into a scratch store named for D05. Nothing here runs
// a shell or another program.
#include "oa/data/mutators/pipeline.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {

namespace mutators = oa::data::mutators;
namespace oamut = oa::formats::oamut;
namespace defs = oa::data::defs;
namespace fs = std::filesystem;

const char* k_copyright = "Ridge Holds This Line";
const char* k_version = "9.25";

void expect(bool condition, const std::string& message) {
    if (!condition)
        std::fprintf(stderr, "clone: %s\n", message.c_str());
    OA_CHECK(condition);
}

std::string fold(std::string_view text) {
    std::string folded;
    folded.reserve(text.size());
    for (unsigned char character : text) {
        if (character == '\\')
            character = '/';
        else if (character >= 'A' && character <= 'Z')
            character = static_cast<unsigned char>(character - 'A' + 'a');
        folded.push_back(static_cast<char>(character));
    }
    return folded;
}

bool contains(std::string_view text, std::string_view needle) {
    return fold(text).find(fold(needle)) != std::string::npos;
}

void write_bytes(const fs::path& path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    expect(out.good(), "could not write " + path.string());
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
}

std::string manifest_text(std::string_view id, std::string_view layer) {
    std::string text;
    text += "oamut: 1\n";
    text += "id: ";
    text.append(id);
    text += "\nname: \"Ridge Clone\"\nversion: \"1.0\"\nauthor:\n  name: \"Ridge Maker\"\n";
    text += "tags: [\"gameplay\"]\nworks-with: [\"*\"]\nlayers: [\"";
    text.append(layer);
    text += "\"]\n";
    return text;
}

mutators::PipelineMutator load_package(const fs::path& package) {
    mutators::PipelineMutator mutator;
    mutator.package = package;
    std::ifstream in(package / "oamut.yaml", std::ios::binary);
    expect(in.good(), "missing manifest " + package.string());
    const std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::vector<oamut::Problem> problems;
    const bool read = oamut::read_manifest(
        std::vector<uint8_t>(bytes.begin(), bytes.end()), mutator.manifest, problems
    );
    if (!read) {
        for (const oamut::Problem& problem : problems)
            std::fprintf(stderr, "%s: %s\n", problem.key.c_str(), problem.message.c_str());
    }
    expect(read, "manifest did not parse");
    return mutator;
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

void release(oa::AssetStore& store) {
    if (store.mutator_layer_mounted())
        store.unmount_mutator_layer();
}

const oa::MutatorLayerEntry* find_entry(const oa::MutatorLayerSpec& spec, std::string_view path) {
    const std::string wanted = fold(path);
    for (const oa::MutatorLayerEntry& entry : spec.entries) {
        if (fold(entry.path) == wanted)
            return &entry;
    }
    return nullptr;
}

std::string entry_text(const oa::MutatorLayerEntry* entry) {
    if (entry == nullptr || entry->contents == nullptr)
        return {};
    return std::string(entry->contents->begin(), entry->contents->end());
}

bool has_sentence(const mutators::Journal& journal, std::string_view sentence) {
    for (const mutators::JournalEntry& entry : journal.entries) {
        if (entry.change == mutators::Change::added && entry.new_text.has_value() &&
            *entry.new_text == sentence)
            return true;
    }
    return false;
}

std::string signature(const mutators::PipelineResult& result) {
    std::string text;
    for (const oa::MutatorLayerEntry& entry : result.layer.entries) {
        text.append(fold(entry.path));
        text.push_back('|');
        text.append(std::to_string(static_cast<unsigned>(entry.kind)));
        text.push_back('|');
        if (entry.contents != nullptr) {
            text.append(entry.contents->begin(), entry.contents->end());
        }
        text.push_back('\n');
    }
    text.append("---\n");
    for (const mutators::JournalEntry& entry : result.journal.entries) {
        text.append(std::to_string(static_cast<unsigned>(entry.change)));
        text.push_back('|');
        text.append(entry.path);
        text.push_back('|');
        text.append(entry.record);
        text.push_back('|');
        if (entry.new_text.has_value())
            text.append(*entry.new_text);
        text.push_back('\n');
    }
    return text;
}

void write_unit_game(const fs::path& root) {
    write_bytes(
        root / "units" / "HULL.FBI",
        "[UNITINFO]\n{\nUnitName=HULL;\nObjectname=RidgeHull;\n"
        "Copyright=Ridge Holds This Line;\nVersion=9.25;\nMaxDamage=100;\n"
        "FootprintX=1;\nFootprintZ=1;\n[CANBUILD]\n{\n}\n}\n"
    );
    write_bytes(
        root / "units" / "BARE.FBI",
        "[UNITINFO]\n{\nUnitName=BARE;\nCopyright=Ridge Holds This Line;\n"
        "Version=9.25;\nMaxDamage=40;\nFootprintX=2;\nFootprintZ=2;\n}\n"
    );
    write_bytes(
        root / "units" / "PLAIN.FBI",
        "[UNITINFO]\n{\nUnitName=PLAIN;\nObjectname=PlainHull;\n"
        "Copyright=Ridge Holds This Line;\nVersion=9.25;\nMaxDamage=80;\n"
        "FootprintX=1;\nFootprintZ=1;\n}\n"
    );
    write_bytes(root / "scripts" / "PLAIN.COB", "base-cob-bytes");
    write_bytes(root / "unitpics" / "PLAIN.PCX", "pic-bytes");
    write_bytes(root / "guis" / "PLAIN0.GUI", "gui-page-0");
    write_bytes(root / "guis" / "PLAIN1.GUI", "gui-page-1");
    write_bytes(root / "guis" / "PLAIN2.GUI", "gui-page-2");
    write_bytes(root / "anims" / "PLAIN0.GAF", "gaf-page-0");
    write_bytes(root / "anims" / "PLAIN1.GAF", "gaf-page-1");
    write_bytes(root / "anims" / "PLAIN2.GAF", "gaf-page-2");
    write_bytes(root / "anims" / "PLAIN_gadget.gaf", "gadget-bytes");
    write_bytes(root / "download" / "PLAIN.tdf", "[MENUENTRY]\n{\nUNITNAME=PLAIN;\n}\n");
    write_bytes(root / "objects3d" / "PLAIN.3DO", "model-bytes");
    write_bytes(root / "objects3d" / "RidgeHull.3DO", "hull-model");
    write_bytes(root / "objects3d" / "BARE.3DO", "bare-model");
    write_bytes(root / "objects3d" / "PlainHull.3DO", "plain-model");
}

void write_package(
    const fs::path& package, std::string_view id, std::string_view layer, std::string_view lua
) {
    write_bytes(package / "oamut.yaml", manifest_text(id, layer));
    write_bytes(package / "mutator.lua", lua);
}

void test_units(const fs::path& scratch) {
    const fs::path game = scratch / "units-game";
    const fs::path packages = scratch / "unit-packages";
    write_unit_game(game);
    write_package(
        packages / "ridge-plain-script",
        "ridge-plain-script",
        "files",
        "function mutate(ctx)\n  return { files = { add = { \"files\" } } }\nend\n"
    );
    write_bytes(
        packages / "ridge-plain-script" / "files" / "scripts" / "PLAIN.COB", "layer-cob-bytes"
    );
    write_package(
        packages / "ridge-clone-units",
        "ridge-clone-units",
        "units",
        "function mutate(ctx)\n"
        "  return {\n"
        "    units = function(units, ctx)\n"
        "      units:clone(\"HULL\", \"HULL2\", { maxdamage = 2000 })\n"
        "      if units.HULL2 == nil or units.HULL2.maxdamage ~= 2000 then\n"
        "        error(\"HULL2 missing after clone\")\n"
        "      end\n"
        "      units:clone(\"BARE\", \"BARE2\", {})\n"
        "      units:clone(\"PLAIN\", \"PLAIN2\", { maxdamage = 50 })\n"
        "      units:clone(\"PLAIN2\", \"PLAIN3\", {})\n"
        "      if units.PLAIN3 == nil or units.PLAIN3.maxdamage ~= 50 then\n"
        "        error(\"PLAIN3 missing after clone\")\n"
        "      end\n"
        "    end\n"
        "  }\n"
        "end\n"
    );
    write_package(
        packages / "ridge-tweak-units",
        "ridge-tweak-units",
        "units",
        "function mutate(ctx)\n"
        "  return {\n"
        "    units = function(units, ctx)\n"
        "      if units.HULL2 == nil then error(\"later hook missed HULL2\") end\n"
        "      units.HULL2.maxdamage = 1600\n"
        "    end\n"
        "  }\n"
        "end\n"
    );

    const auto input = [&]() {
        return make_input({
            load_package(packages / "ridge-plain-script"),
            load_package(packages / "ridge-clone-units"),
            load_package(packages / "ridge-tweak-units"),
        });
    };
    oa::AssetStore first(game);
    const mutators::PipelineResult once = mutators::run_pipeline(first, input());
    expect(once.playable(), once.refusal.empty() ? "unit clone did not apply" : once.refusal);
    if (!once.mutators.empty() && once.mutators[0].outcome != mutators::Outcome::applied)
        std::fprintf(stderr, "clone: first mutator: %s\n", once.mutators[0].message.c_str());
    if (once.mutators.size() > 1 && once.mutators[1].outcome != mutators::Outcome::applied)
        std::fprintf(stderr, "clone: clone mutator: %s\n", once.mutators[1].message.c_str());

    const std::string hull = entry_text(find_entry(once.layer, "units/HULL2.FBI"));
    expect(contains(hull, "UnitName=HULL2"), "HULL2 unit name");
    expect(contains(hull, "Objectname=RidgeHull"), "HULL2 keeps the source model");
    expect(contains(hull, k_copyright), "HULL2 copyright");
    expect(contains(hull, std::string("Version=") + k_version), "HULL2 version");
    expect(contains(hull, "1600"), "later hook wrote HULL2");
    expect(!contains(hull, "CANBUILD"), "HULL2 dropped the build list");

    const std::string bare = entry_text(find_entry(once.layer, "units/BARE2.FBI"));
    expect(contains(bare, "UnitName=BARE2"), "BARE2 unit name");
    expect(contains(bare, "Objectname=BARE"), "BARE2 states the default model");
    expect(contains(bare, k_copyright), "BARE2 copyright");
    expect(contains(bare, std::string("Version=") + k_version), "BARE2 version");

    const std::string plain = entry_text(find_entry(once.layer, "units/PLAIN2.FBI"));
    expect(contains(plain, "UnitName=PLAIN2"), "PLAIN2 unit name");
    expect(contains(plain, "Objectname=PlainHull"), "PLAIN2 model");
    expect(contains(plain, "50"), "PLAIN2 field");

    const auto* script = find_entry(once.layer, "scripts/PLAIN2.COB");
    const auto* script3 = find_entry(once.layer, "scripts/PLAIN3.COB");
    expect(script != nullptr && script->contents != nullptr, "PLAIN2 script served");
    expect(
        script3 != nullptr && script3->contents != nullptr, "PLAIN3 script served from the copy"
    );
    if (script != nullptr && script->contents != nullptr) {
        const std::string bytes(script->contents->begin(), script->contents->end());
        expect(bytes == "layer-cob-bytes", "script is the earlier mutator's file");
        expect(bytes != "base-cob-bytes", "script is not the base file");
    }
    if (script != nullptr && script3 != nullptr && script->contents != nullptr &&
        script3->contents != nullptr)
        expect(
            *script->contents == *script3->contents, "a clone of a clone serves the same script"
        );

    const auto expect_bytes = [&](std::string_view path, std::string_view bytes) {
        const oa::MutatorLayerEntry* entry = find_entry(once.layer, path);
        expect(entry != nullptr && entry->contents != nullptr, std::string(path));
        if (entry != nullptr && entry->contents != nullptr) {
            const std::string got(entry->contents->begin(), entry->contents->end());
            expect(got == bytes, std::string(path) + " bytes");
        }
    };
    expect_bytes("unitpics/PLAIN2.PCX", "pic-bytes");
    // Page n of PLAIN2 is guis/PLAIN2<n>.GUI, the name the game probes.
    expect_bytes("guis/PLAIN20.GUI", "gui-page-0");
    expect_bytes("guis/PLAIN21.GUI", "gui-page-1");
    expect_bytes("guis/PLAIN22.GUI", "gui-page-2");
    expect_bytes("anims/PLAIN20.GAF", "gaf-page-0");
    expect_bytes("anims/PLAIN21.GAF", "gaf-page-1");
    expect_bytes("anims/PLAIN22.GAF", "gaf-page-2");
    expect_bytes("unitpics/PLAIN3.PCX", "pic-bytes");
    expect(find_entry(once.layer, "download/PLAIN2.tdf") == nullptr, "download entry was copied");
    expect(find_entry(once.layer, "anims/PLAIN2_gadget.gaf") == nullptr, "gadget art was copied");
    expect(
        find_entry(once.layer, "objects3d/PLAIN2.3DO") == nullptr,
        "model was copied under a new name"
    );
    expect(find_entry(once.layer, "guis/PLAIN23.GUI") == nullptr, "a missing page was served");

    expect(has_sentence(once.journal, "added unit HULL2 (copy of HULL)"), "unit journal");
    expect(has_sentence(once.journal, "added unit BARE2 (copy of BARE)"), "bare journal");
    expect(
        has_sentence(once.journal, "added path scripts/PLAIN2.COB, copy of scripts/PLAIN.COB"),
        "script journal"
    );
    expect(
        has_sentence(once.journal, "added path guis/PLAIN22.GUI, copy of guis/PLAIN2.GUI"),
        "page journal"
    );
    expect(
        has_sentence(once.journal, "added path anims/PLAIN20.GAF, copy of anims/PLAIN0.GAF"),
        "gaf journal"
    );

    const std::string first_signature = signature(once);
    release(first);
    oa::AssetStore second(game);
    const mutators::PipelineResult again = mutators::run_pipeline(second, input());
    expect(again.playable(), "second unit run");
    expect(signature(again) == first_signature, "two runs give identical bytes");
    release(second);
}

void expect_refused(
    const fs::path& game, const fs::path& package, std::string_view needle, std::string_view absent
) {
    oa::AssetStore store(game);
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, make_input({load_package(package)}));
    expect(!result.playable(), std::string(needle) + " was accepted");
    expect(!result.mutators.empty(), "no mutator result");
    if (!result.mutators.empty()) {
        expect(result.mutators[0].outcome == mutators::Outcome::failed, "outcome");
        expect(result.mutators[0].cause == mutators::Cause::script_error, "cause");
        expect(contains(result.mutators[0].message, needle), result.mutators[0].message);
    }
    expect(find_entry(result.layer, absent) == nullptr, std::string(absent) + " was served");
    const std::optional<std::vector<uint8_t>> loaded = store.load_file_contents(absent);
    expect(!loaded.has_value(), std::string(absent) + " is in the store");
    release(store);
}

/// Replace puts a clone whose name matches an existing record, ignoring case, in place.
void test_same_letters_replace_the_unit(const fs::path& game, const fs::path& package) {
    oa::AssetStore store(game);
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, make_input({load_package(package)}));
    if (!result.playable() && !result.mutators.empty())
        std::fprintf(stderr, "clone: %s\n", result.mutators[0].message.c_str());
    expect(
        result.playable(),
        result.refusal.empty() ? "same-letter clone did not apply" : result.refusal
    );

    const std::string unit = entry_text(find_entry(result.layer, "units/PLAIN.FBI"));
    expect(unit.find("UnitName=PLAIN") != std::string::npos, "replaced unit keeps the stored name");
    expect(unit.find("UnitName=plain") == std::string::npos, "the asked spelling is not stored");
    expect(unit.find("Objectname=PlainHull") != std::string::npos, "replaced unit keeps its model");
    expect(unit.find("MaxDamage=80") != std::string::npos, "replaced unit keeps the source fields");
    expect(
        find_entry(result.layer, "scripts/PLAIN.COB") == nullptr, "a replacement copied the script"
    );
    expect(
        find_entry(result.layer, "unitpics/PLAIN.PCX") == nullptr,
        "a replacement copied the picture"
    );
    expect(
        find_entry(result.layer, "objects3d/PLAIN.3DO") == nullptr, "a replacement copied the model"
    );
    expect(has_sentence(result.journal, "replaced unit PLAIN (copy of PLAIN)"), "replace journal");

    bool saw_old = false;
    for (const mutators::JournalEntry& entry : result.journal.entries) {
        if (!entry.new_text.has_value() || *entry.new_text != "replaced unit PLAIN (copy of PLAIN)")
            continue;
        saw_old =
            entry.old_text.has_value() && *entry.old_text == "PLAIN" && entry.record == "PLAIN";
    }
    expect(saw_old, "replace journal names the record that was already there");

    const std::vector<std::string> ids{"ridge-clone-case"};
    bool saw_note = false;
    bool saw_rename = false;
    for (const mutators::Conflict& conflict : result.report.conflicts) {
        if (conflict.kind == mutators::ConflictKind::record_renamed)
            saw_rename = true;
        if (conflict.kind != mutators::ConflictKind::value_replaced)
            continue;
        const std::string line = mutators::describe(conflict, ids);
        if (line == "ridge-clone-case replaced PLAIN from the game on " + conflict.path)
            saw_note = true;
    }
    expect(saw_note, "replace note names the record");
    expect(!saw_rename, "a replacement was noted as a rename");
    release(store);
}

void test_refusals(const fs::path& scratch) {
    const fs::path game = scratch / "units-game";
    const fs::path packages = scratch / "refusal-packages";
    write_package(
        packages / "case",
        "ridge-clone-case",
        "units",
        "function mutate(ctx)\n"
        "  return { units = function(units, ctx)\n"
        "    units:clone(\"PLAIN\", \"plain\", {})\n"
        "  end }\nend\n"
    );
    write_package(
        packages / "long",
        "ridge-clone-long",
        "units",
        "function mutate(ctx)\n"
        "  return { units = function(units, ctx)\n"
        "    units:clone(\"PLAIN\", \"ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEF\", {})\n"
        "  end }\nend\n"
    );
    write_package(
        packages / "dot",
        "ridge-clone-dot",
        "units",
        "function mutate(ctx)\n"
        "  return { units = function(units, ctx)\n"
        "    units:clone(\"PLAIN\", \"PLAIN.2\", {})\n"
        "  end }\nend\n"
    );
    write_package(
        packages / "missing",
        "ridge-clone-missing",
        "units",
        "function mutate(ctx)\n"
        "  return { units = function(units, ctx)\n"
        "    units:clone(\"NOSUCH\", \"MADEUP\", {})\n"
        "  end }\nend\n"
    );
    write_package(
        packages / "field",
        "ridge-clone-field",
        "units",
        "function mutate(ctx)\n"
        "  return { units = function(units, ctx)\n"
        "    units:clone(\"HULL\", \"HULL2\", { footprintx = 0 })\n"
        "  end }\nend\n"
    );
    write_package(
        packages / "boom",
        "ridge-clone-boom",
        "units",
        "function mutate(ctx)\n"
        "  return { units = function(units, ctx)\n"
        "    units:clone(\"HULL\", \"HULL2\", { maxdamage = 2000 })\n"
        "    error(\"boom\")\n"
        "  end }\nend\n"
    );
    test_same_letters_replace_the_unit(game, packages / "case");
    expect_refused(
        game, packages / "long", "not 1 to 31", "units/ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEF.FBI"
    );
    expect_refused(game, packages / "dot", "PLAIN.2", "units/PLAIN.2.FBI");
    expect_refused(game, packages / "missing", "NOSUCH", "units/MADEUP.FBI");
    expect_refused(game, packages / "field", "footprintx", "units/HULL2.FBI");
    expect_refused(game, packages / "boom", "boom", "units/HULL2.FBI");
}

void test_weapon_lowest(const fs::path& scratch) {
    const fs::path game = scratch / "weapon-low-game";
    const fs::path packages = scratch / "weapon-low-packages";
    write_bytes(
        game / "Weapons" / "GUNS.tdf",
        "[GUN]\n{\nID=1;\nname=Gun;\n[DAMAGE]\n{\ndefault=3;\nRIDGEBOT=9;\n}\n}\n"
        "[OTHER]\n{\nID=3;\nname=Other;\n}\n"
    );
    write_bytes(game / "Weapons" / "TAKEN.tdf", "[PARKED]\n{\nID=5;\nname=Parked;\n}\n");
    write_package(
        packages / "ridge-clone-weapons",
        "ridge-clone-weapons",
        "weapons",
        "function mutate(ctx)\n"
        "  return {\n"
        "    weapons = function(weapons, ctx)\n"
        "      weapons:clone(\"GUN\", \"GUN2\", {})\n"
        "      weapons:clone(\"GUN\", \"TAKEN\", {})\n"
        "      weapons:clone(\"GUN\", \"NAMED\", { name = \"Ridge Gun\" })\n"
        "      if weapons.GUN2 == nil or weapons.GUN2.id ~= 2 then\n"
        "        error(\"GUN2 id\")\n"
        "      end\n"
        "    end\n"
        "  }\n"
        "end\n"
    );
    write_package(
        packages / "ridge-read-weapons",
        "ridge-read-weapons",
        "weapons",
        "function mutate(ctx)\n"
        "  return {\n"
        "    weapons = function(weapons, ctx)\n"
        "      if weapons.GUN2 == nil or weapons.GUN2.id ~= 2 then\n"
        "        error(\"later weapon missed GUN2\")\n"
        "      end\n"
        "    end\n"
        "  }\n"
        "end\n"
    );
    oa::AssetStore store(game);
    const mutators::PipelineResult result = mutators::run_pipeline(
        store,
        make_input({
            load_package(packages / "ridge-clone-weapons"),
            load_package(packages / "ridge-read-weapons"),
        })
    );
    expect(
        result.playable(), result.mutators.size() > 0 ? result.mutators[0].message : "weapon run"
    );
    const std::string gun2 = entry_text(find_entry(result.layer, "Weapons/GUN2.tdf"));
    expect(contains(gun2, "ID=2"), "lowest free id is 2");
    expect(contains(gun2, "name=Gun"), "shown name kept");
    expect(contains(gun2, "default=3"), "damage default copied");
    expect(contains(gun2, "RIDGEBOT=9"), "damage row copied");
    expect(find_entry(result.layer, "Weapons/TAKEN-2.tdf") != nullptr, "path collision uses -2");
    const std::string named = entry_text(find_entry(result.layer, "Weapons/NAMED.tdf"));
    expect(contains(named, "name=Ridge Gun"), "fields can change the shown name");
    expect(contains(named, "ID=6"), "later clone skips ids already taken");
    expect(
        has_sentence(result.journal, "added weapon GUN2 (copy of GUN) at id 2"), "weapon journal"
    );
    release(store);
}

void test_weapon_zero(const fs::path& scratch) {
    const fs::path game = scratch / "weapon-zero-game";
    const fs::path package = scratch / "weapon-zero-package";
    write_bytes(
        game / "Weapons" / "ODD.tdf",
        "[ZERO]\n{\nID=;\nname=Empty;\n}\n"
        "[TEXT]\n{\nID=abc;\nname=Text;\n}\n"
        "[HIGH]\n{\nID=300;\nname=High;\n}\n"
        "[GUN]\n{\nID=2;\nname=Gun;\n[DAMAGE]\n{\ndefault=3;\nRIDGEBOT=9;\n}\n}\n"
    );
    write_package(
        package,
        "ridge-clone-zero",
        "weapons",
        "function mutate(ctx)\n"
        "  return { weapons = function(weapons, ctx)\n"
        "    weapons:clone(\"GUN\", \"GUN2\", {})\n"
        "  end }\nend\n"
    );
    oa::AssetStore store(game);
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, make_input({load_package(package)}));
    expect(result.playable(), result.mutators.empty() ? "zero run" : result.mutators[0].message);
    const std::string gun2 = entry_text(find_entry(result.layer, "Weapons/GUN2.tdf"));
    expect(contains(gun2, "ID=1"), "empty and non-numeric ids leave 1 free");
    expect(!contains(gun2, "ID=0"), "slot 0 was taken");
    expect(contains(gun2, "RIDGEBOT=9"), "damage copied beside an out-of-range id");
    expect(has_sentence(result.journal, "added weapon GUN2 (copy of GUN) at id 1"), "id 1 journal");
    release(store);
}

void test_weapon_full(const fs::path& scratch) {
    const fs::path game = scratch / "weapon-full-game";
    const fs::path package = scratch / "weapon-full-package";
    std::string text;
    for (int id = 1; id <= 255; ++id) {
        text += "[W";
        text += std::to_string(id);
        text += "]\n{\nID=";
        text += std::to_string(id);
        text += ";\nname=Full;\n}\n";
    }
    write_bytes(game / "Weapons" / "FULL.tdf", text);
    write_package(
        package,
        "ridge-clone-full",
        "weapons",
        "function mutate(ctx)\n"
        "  return { weapons = function(weapons, ctx)\n"
        "    weapons:clone(\"W1\", \"WNEW\", {})\n"
        "  end }\nend\n"
    );
    oa::AssetStore store(game);
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, make_input({load_package(package)}));
    expect(!result.playable(), "a full id table was accepted");
    expect(!result.mutators.empty(), "full result");
    if (!result.mutators.empty()) {
        expect(
            result.mutators[0].message.find(
                "no free weapon id: 1 to 255 are all used (cloning W1 as WNEW)"
            ) != std::string::npos,
            result.mutators[0].message
        );
    }
    expect(find_entry(result.layer, "Weapons/WNEW.tdf") == nullptr, "refused clone was served");
    release(store);
}

/// Ids 1 to 255 are taken and the limit is 1,024, so the clone receives 256.
void test_weapon_wide_clone_gets_256(const fs::path& scratch) {
    const fs::path game = scratch / "weapon-wide-game";
    const fs::path package = scratch / "weapon-wide-package";
    std::string text;
    for (int id = 1; id <= 255; ++id) {
        text += "[W";
        text += std::to_string(id);
        text += "]\n{\nID=";
        text += std::to_string(id);
        text += ";\nname=Wide;\n}\n";
    }
    write_bytes(game / "Weapons" / "FULL.tdf", text);
    write_package(
        package,
        "ridge-clone-wide",
        "weapons",
        "function mutate(ctx)\n"
        "  return { weapons = function(weapons, ctx)\n"
        "    weapons:clone(\"W1\", \"WNEW\", {})\n"
        "  end }\nend\n"
    );
    oa::AssetStore store(game);
    mutators::PipelineInput input = make_input({load_package(package)});
    input.limits.weapon_types = 1024;
    const mutators::PipelineResult result = mutators::run_pipeline(store, input);
    expect(result.playable(), result.mutators.empty() ? "wide run" : result.mutators[0].message);
    const std::string made = entry_text(find_entry(result.layer, "Weapons/WNEW.tdf"));
    expect(contains(made, "ID=256"), made);
    expect(
        has_sentence(result.journal, "added weapon WNEW (copy of W1) at id 256"), "id 256 journal"
    );
    release(store);
}

/// Ids 1 to 1,023 are taken, so the refusal names that whole range.
void test_weapon_wide_full_refuses(const fs::path& scratch) {
    const fs::path game = scratch / "weapon-wide-full-game";
    const fs::path package = scratch / "weapon-wide-full-package";
    std::string text;
    for (int id = 1; id <= 1023; ++id) {
        text += "[W";
        text += std::to_string(id);
        text += "]\n{\nID=";
        text += std::to_string(id);
        text += ";\nname=Full;\n}\n";
    }
    write_bytes(game / "Weapons" / "FULL.tdf", text);
    write_package(
        package,
        "ridge-clone-wide-full",
        "weapons",
        "function mutate(ctx)\n"
        "  return { weapons = function(weapons, ctx)\n"
        "    weapons:clone(\"W1\", \"WNEW\", {})\n"
        "  end }\nend\n"
    );
    oa::AssetStore store(game);
    mutators::PipelineInput input = make_input({load_package(package)});
    input.limits.weapon_types = 1024;
    const mutators::PipelineResult result = mutators::run_pipeline(store, input);
    expect(!result.playable(), "a table full through 1023 was accepted");
    expect(!result.mutators.empty(), "wide full result");
    if (!result.mutators.empty()) {
        expect(
            result.mutators[0].message.find(
                "no free weapon id: 1 to 1023 are all used (cloning W1 as WNEW)"
            ) != std::string::npos,
            result.mutators[0].message
        );
    }
    expect(
        find_entry(result.layer, "Weapons/WNEW.tdf") == nullptr, "refused wide clone was served"
    );
    release(store);
}

void test_weapon_id_field(const fs::path& scratch) {
    const fs::path game = scratch / "weapon-id-game";
    const fs::path package = scratch / "weapon-id-package";
    write_bytes(game / "Weapons" / "GUNS.tdf", "[GUN]\n{\nID=1;\nname=Gun;\n}\n");
    write_package(
        package,
        "ridge-clone-id",
        "weapons",
        "function mutate(ctx)\n"
        "  return { weapons = function(weapons, ctx)\n"
        "    weapons:clone(\"GUN\", \"GUN2\", { ID = 2 })\n"
        "  end }\nend\n"
    );
    oa::AssetStore store(game);
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, make_input({load_package(package)}));
    expect(!result.playable(), "an ID field was accepted");
    expect(!result.mutators.empty(), "id field result");
    if (!result.mutators.empty()) {
        expect(contains(result.mutators[0].message, "weapons:clone(\"GUN\", \"GUN2\")"), "request");
        expect(contains(result.mutators[0].message, "ID"), "ID refusal");
    }
    expect(find_entry(result.layer, "Weapons/GUN2.tdf") == nullptr, "ID refusal served a file");
    release(store);
}

/// Real time each phase of a budget case may run. These cases are about the
/// instruction allowance; the real-time limit has its own test, and a busy
/// machine must not turn a budget stop into a time stop here.
constexpr std::chrono::minutes k_budget_time_limit{10};

/// Most processor time a loop the allowance stops may spend. The heaviest,
/// the refused weapon loop, takes about 0.2 s on a fast machine and ran past
/// 10 s on a slow one. A loop the allowance does not stop fails the cause
/// check when the real-time limit stops it.
constexpr double k_budget_stop_seconds = 60.0;

/// The allowance is large enough that the loop's own Lua instructions do not
/// spend it, and small enough that the charged clone work does, well before
/// the long real-time limit the case runs under.
void expect_budget_stop(const fs::path& game, const fs::path& package, std::string_view label) {
    oa::AssetStore store(game);
    mutators::PipelineInput input = make_input({load_package(package)});
    input.budget.instructions = 1000000;
    input.time_limit = k_budget_time_limit;
    // The bound is on the processor time this test spends, so other programs
    // sharing the machine do not stretch it. The real time is printed too.
    const std::clock_t processor_started = std::clock();
    const auto started = std::chrono::steady_clock::now();
    const mutators::PipelineResult result = mutators::run_pipeline(store, input);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    const double processor_seconds =
        static_cast<double>(std::clock() - processor_started) / CLOCKS_PER_SEC;
    const auto milliseconds =
        std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
    std::printf(
        "%.*s stopped in %lld ms (%.0f ms of processor time)\n",
        static_cast<int>(label.size()),
        label.data(),
        static_cast<long long>(milliseconds),
        processor_seconds * 1000.0
    );
    expect(!result.playable(), std::string(label) + " applied");
    expect(!result.mutators.empty(), std::string(label) + " result");
    if (!result.mutators.empty()) {
        expect(result.mutators[0].outcome == mutators::Outcome::failed, std::string(label));
        expect(
            result.mutators[0].cause == mutators::Cause::instruction_budget,
            std::string(label) + " cause " +
                std::to_string(static_cast<unsigned>(result.mutators[0].cause))
        );
        expect(
            result.mutators[0].cause != mutators::Cause::time_limit, std::string(label) + " clock"
        );
        expect(result.mutators[0].phase == mutators::Phase::hooks, std::string(label) + " phase");
    }
    expect(
        processor_seconds < k_budget_stop_seconds,
        std::string(label) + " took " + std::to_string(processor_seconds) + " s of processor time"
    );
    expect(result.layer.entries.empty(), std::string(label) + " served a file");
    expect(!store.mutator_layer_mounted(), std::string(label) + " left a layer mounted");
    release(store);
}

void test_clone_budgets(const fs::path& scratch) {
    const fs::path unit_game = scratch / "budget-units-game";
    const fs::path unit_package = scratch / "budget-units-package";
    write_bytes(
        unit_game / "units" / "ONE.FBI",
        "[UNITINFO]\n{\nUnitName=ONE;\nObjectname=One;\n"
        "Copyright=Ridge Holds This Line;\nVersion=9.25;\nMaxDamage=10;\n"
        "FootprintX=1;\nFootprintZ=1;\n}\n"
    );
    write_package(
        unit_package,
        "ridge-clone-budget",
        "units",
        "function mutate(ctx)\n"
        "  return { units = function(units, ctx)\n"
        "    for n = 1, 100000 do\n"
        "      units:clone(\"ONE\", string.format(\"U%d\", n), {})\n"
        "    end\n"
        "  end }\nend\n"
    );
    expect_budget_stop(unit_game, unit_package, "unit clone loop");

    // Each clone copies a 1 MiB script, so the copy's charge spends the
    // allowance after a few dozen clones, long before the loop's own Lua does.
    const fs::path script_game = scratch / "budget-script-game";
    const fs::path script_package = scratch / "budget-script-package";
    write_bytes(
        script_game / "units" / "BULK.FBI",
        "[UNITINFO]\n{\nUnitName=BULK;\nObjectname=Bulk;\n"
        "Copyright=Ridge Holds This Line;\nVersion=9.25;\nMaxDamage=10;\n"
        "FootprintX=1;\nFootprintZ=1;\n}\n"
    );
    write_bytes(script_game / "scripts" / "BULK.COB", std::string(std::size_t{1} << 20, 'c'));
    write_package(
        script_package,
        "ridge-clone-script-budget",
        "units",
        "function mutate(ctx)\n"
        "  return { units = function(units, ctx)\n"
        "    for n = 1, 100000 do\n"
        "      units:clone(\"BULK\", string.format(\"B%d\", n), {})\n"
        "    end\n"
        "  end }\nend\n"
    );
    expect_budget_stop(script_game, script_package, "large script clone loop");

    const fs::path weapon_game = scratch / "budget-weapons-game";
    const fs::path weapon_package = scratch / "budget-weapons-package";
    std::string text;
    for (int id = 1; id <= 255; ++id) {
        text += "[W";
        text += std::to_string(id);
        text += "]\n{\nID=";
        text += std::to_string(id);
        text += ";\nname=Full;\n}\n";
    }
    write_bytes(weapon_game / "Weapons" / "FULL.tdf", text);
    write_package(
        weapon_package,
        "ridge-clone-refuse-budget",
        "weapons",
        "function mutate(ctx)\n"
        "  return { weapons = function(weapons, ctx)\n"
        "    for n = 1, 100000 do\n"
        "      pcall(function()\n"
        "        weapons:clone(\"W1\", string.format(\"N%d\", n), {})\n"
        "      end)\n"
        "    end\n"
        "  end }\nend\n"
    );
    expect_budget_stop(weapon_game, weapon_package, "refused weapon clone loop");
}

void test_page_collision(const fs::path& scratch) {
    const fs::path game = scratch / "collision-game";
    const fs::path package = scratch / "collision-package";
    write_bytes(
        game / "units" / "PLAIN.FBI",
        "[UNITINFO]\n{\nUnitName=PLAIN;\nObjectname=PlainHull;\n"
        "Copyright=Ridge Holds This Line;\nVersion=9.25;\nMaxDamage=80;\n"
        "FootprintX=1;\nFootprintZ=1;\n}\n"
    );
    write_bytes(game / "guis" / "PLAIN0.GUI", "gui-page-0");
    write_bytes(game / "guis" / "PLAIN1.GUI", "gui-page-1");
    write_bytes(game / "guis" / "PLAIN2.GUI", "gui-page-2");
    write_bytes(game / "anims" / "PLAIN2.GAF", "gaf-page-2");
    write_bytes(game / "guis" / "ARMMAIN2.GUI", "screen-bytes");
    write_bytes(game / "anims" / "ARMMAIN2.GAF", "screen-art");
    write_bytes(
        game / "units" / "X.FBI",
        "[UNITINFO]\n{\nUnitName=X;\nObjectname=X;\n"
        "Copyright=Ridge Holds This Line;\nVersion=9.25;\nMaxDamage=8;\n"
        "FootprintX=1;\nFootprintZ=1;\n}\n"
    );
    write_bytes(game / "guis" / "X0.GUI", "x-page-0");
    write_bytes(game / "guis" / "X10.GUI", "x-page-10");
    write_package(
        package,
        "ridge-clone-screen",
        "units",
        "function mutate(ctx)\n"
        "  return { units = function(units, ctx)\n"
        "    units:clone(\"PLAIN\", \"ARMMAIN\", {})\n"
        "  end }\nend\n"
    );
    oa::AssetStore store(game);
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, make_input({load_package(package)}));
    expect(!result.playable(), "a clone onto a screen name was accepted");
    expect(!result.mutators.empty(), "collision result");
    if (!result.mutators.empty()) {
        expect(result.mutators[0].cause == mutators::Cause::script_error, "collision cause");
        expect(
            contains(result.mutators[0].message, "guis/ARMMAIN2.GUI"), result.mutators[0].message
        );
        expect(
            contains(result.mutators[0].message, "the path already exists"),
            result.mutators[0].message
        );
    }
    expect(result.layer.entries.empty(), "the layer served a colliding clone");
    expect(find_entry(result.layer, "guis/ARMMAIN2.GUI") == nullptr, "screen was replaced");
    expect(find_entry(result.layer, "anims/ARMMAIN2.GAF") == nullptr, "screen art was replaced");
    expect(find_entry(result.layer, "units/ARMMAIN.FBI") == nullptr, "colliding unit was served");
    expect(!store.mutator_layer_mounted(), "collision left a layer mounted");
    release(store);

    const fs::path overlap = scratch / "overlap-package";
    write_package(
        overlap,
        "ridge-clone-overlap",
        "units",
        "function mutate(ctx)\n"
        "  return { units = function(units, ctx)\n"
        "    units:clone(\"X\", \"X1\", {})\n"
        "  end }\nend\n"
    );
    oa::AssetStore overlap_store(game);
    const mutators::PipelineResult overlapped =
        mutators::run_pipeline(overlap_store, make_input({load_package(overlap)}));
    expect(!overlapped.playable(), "a clone onto a later page was accepted");
    expect(!overlapped.mutators.empty(), "overlap result");
    if (!overlapped.mutators.empty()) {
        expect(
            contains(overlapped.mutators[0].message, "guis/X10.GUI"), overlapped.mutators[0].message
        );
    }
    expect(overlapped.layer.entries.empty(), "the overlapping clone was served");
    expect(!overlap_store.mutator_layer_mounted(), "overlap left a layer mounted");
    release(overlap_store);
}

} // namespace

int main() {
    const fs::path scratch = oa::test::make_scratch_directory("D05-clone");
    test_units(scratch);
    test_refusals(scratch);
    test_weapon_lowest(scratch);
    test_weapon_zero(scratch);
    test_weapon_full(scratch);
    test_weapon_wide_clone_gets_256(scratch);
    test_weapon_wide_full_refuses(scratch);
    test_weapon_id_field(scratch);
    test_clone_budgets(scratch);
    test_page_collision(scratch);
    std::error_code error;
    fs::remove_all(scratch, error);
    return oa::test::check_exit_status();
}

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Each integrity problem, caused by a change and already present in the base.
// The pipeline case leaves the failing mutator out of a layer that matches
// the run without it.
#include "oa/data/defs/layout.hpp"
#include "oa/data/mutators/integrity.hpp"
#include "oa/data/mutators/pipeline.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/test/check.hpp"

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#ifndef OA_MUTATORS_INTEGRITY_DIR
#error OA_MUTATORS_INTEGRITY_DIR is the fixture folder
#endif

namespace {

namespace mutators = oa::data::mutators;
namespace rules = oa::data::mutation_rules;
namespace oamut = oa::formats::oamut;
namespace fs = std::filesystem;

const fs::path k_root{OA_MUTATORS_INTEGRITY_DIR};

bool holds_all(std::string_view) {
    return true;
}

bool add_doc(rules::DocumentSet& documents, std::string path, std::string_view text) {
    std::string error;
    const bool added = documents.add(std::move(path), text, &error);
    if (!added)
        std::fprintf(stderr, "integrity: %s\n", error.c_str());
    OA_CHECK(added);
    return added;
}

const mutators::IntegrityIssue*
find_problem(const std::vector<mutators::IntegrityIssue>& issues, mutators::Problem problem) {
    for (const mutators::IntegrityIssue& issue : issues) {
        if (issue.problem == problem)
            return &issue;
    }
    return nullptr;
}

bool has_problem(const std::vector<mutators::IntegrityIssue>& issues, mutators::Problem problem) {
    return find_problem(issues, problem) != nullptr;
}

std::vector<mutators::IntegrityIssue> issues_of(const rules::DocumentSet& documents) {
    mutators::IntegrityLimits limits;
    return mutators::integrity_issues(documents, oa::data::defs::data_layout(), limits, holds_all);
}

void expect_filtered(const std::vector<mutators::IntegrityIssue>& base, mutators::Problem problem) {
    const std::vector<mutators::IntegrityIssue> kept =
        mutators::new_issues(base, base, mutators::Journal{}, oa::data::defs::data_layout());
    OA_CHECK(!has_problem(kept, problem));
}

mutators::JournalEntry set_field(uint16_t mutator, std::string record, std::string field) {
    mutators::JournalEntry entry;
    entry.mutators.push_back(mutator);
    entry.phase = mutators::Phase::hooks;
    entry.collection = mutators::Collection::units;
    entry.change = mutators::Change::set;
    entry.record = std::move(record);
    entry.field = std::move(field);
    return entry;
}

void test_weapon_must_hold_its_slot() {
    rules::DocumentSet documents;
    add_doc(documents, "Weapons/first.tdf", "[RGGUN]\n{\nID=1;\n}\n");
    add_doc(documents, "Weapons/later.tdf", "[OTHER]\n{\nID=1;\n}\n");
    add_doc(
        documents,
        "units/RGBOT.fbi",
        "[UNITINFO]\n{\nUnitName=RGBOT;\nWeapon1=RGGUN;\nWeapon2=;\n}\n"
    );
    const std::vector<mutators::IntegrityIssue> found = issues_of(documents);
    const mutators::IntegrityIssue* missing =
        find_problem(found, mutators::Problem::weapon_missing);
    OA_CHECK(missing != nullptr);
    if (missing != nullptr) {
        OA_CHECK(missing->record == "RGBOT");
        OA_CHECK(missing->field == "weapon1");
        OA_CHECK(missing->target == "RGGUN");
        OA_CHECK(missing->severity == mutators::Severity::failure);
        OA_CHECK(missing->message.find("RGGUN") != std::string::npos);
    }
    OA_CHECK(!has_problem(found, mutators::Problem::weapon_missing) || missing->field != "weapon2");
    expect_filtered(found, mutators::Problem::weapon_missing);

    rules::DocumentSet kept;
    add_doc(kept, "Weapons/later.tdf", "[OTHER]\n{\nID=1;\n}\n");
    add_doc(kept, "Weapons/first.tdf", "[RGGUN]\n{\nID=1;\n}\n");
    add_doc(kept, "units/RGBOT.fbi", "[UNITINFO]\n{\nUnitName=RGBOT;\nWeapon1=RGGUN;\n}\n");
    OA_CHECK(!has_problem(issues_of(kept), mutators::Problem::weapon_missing));
}

void test_weapon_id_300_takes_no_slot() {
    rules::DocumentSet documents;
    add_doc(documents, "Weapons/wide.tdf", "[WIDE]\n{\nID=300;\n}\n");
    add_doc(documents, "units/RGBOT.fbi", "[UNITINFO]\n{\nUnitName=RGBOT;\nExplodeAs=WIDE;\n}\n");
    const std::vector<mutators::IntegrityIssue> found = issues_of(documents);
    const mutators::IntegrityIssue* range =
        find_problem(found, mutators::Problem::weapon_id_out_of_range);
    OA_CHECK(range != nullptr);
    if (range != nullptr) {
        OA_CHECK(range->record == "WIDE");
        OA_CHECK(range->field == "id");
        OA_CHECK(range->target == "300");
        OA_CHECK(range->message.find("300") != std::string::npos);
    }
    const mutators::IntegrityIssue* explosion =
        find_problem(found, mutators::Problem::explosion_missing);
    OA_CHECK(explosion != nullptr);
    if (explosion != nullptr) {
        OA_CHECK(explosion->field == "explodeas");
        OA_CHECK(explosion->target == "WIDE");
    }
    expect_filtered(found, mutators::Problem::weapon_id_out_of_range);
    expect_filtered(found, mutators::Problem::explosion_missing);

    rules::DocumentSet bare;
    add_doc(bare, "Weapons/bare.tdf", "[BARE]\n{\n}\n");
    const std::vector<mutators::IntegrityIssue> bare_issues = issues_of(bare);
    const mutators::IntegrityIssue* missing =
        find_problem(bare_issues, mutators::Problem::weapon_id_out_of_range);
    OA_CHECK(missing != nullptr);
    if (missing != nullptr) {
        OA_CHECK(missing->record == "BARE");
        OA_CHECK(missing->message == "BARE has no ID");
    }
}

/// At 1,024 weapon types, 300 holds a slot and 1,024 is outside 0 to 1,023.
void test_weapon_id_300_fits_a_wide_table() {
    mutators::IntegrityLimits limits;
    limits.weapon_types = 1024;
    rules::DocumentSet documents;
    add_doc(documents, "Weapons/wide.tdf", "[WIDE]\n{\nID=300;\n}\n");
    add_doc(documents, "units/RGBOT.fbi", "[UNITINFO]\n{\nUnitName=RGBOT;\nWeapon1=WIDE;\n}\n");
    const std::vector<mutators::IntegrityIssue> found =
        mutators::integrity_issues(documents, oa::data::defs::data_layout(), limits, holds_all);
    OA_CHECK(!has_problem(found, mutators::Problem::weapon_id_out_of_range));
    OA_CHECK(!has_problem(found, mutators::Problem::weapon_missing));

    rules::DocumentSet past;
    add_doc(past, "Weapons/past.tdf", "[PAST]\n{\nID=1024;\n}\n");
    const std::vector<mutators::IntegrityIssue> past_issues =
        mutators::integrity_issues(past, oa::data::defs::data_layout(), limits, holds_all);
    const mutators::IntegrityIssue* range =
        find_problem(past_issues, mutators::Problem::weapon_id_out_of_range);
    OA_CHECK(range != nullptr);
    if (range != nullptr)
        OA_CHECK(range->message.find("outside 0 to 1023") != std::string::npos);
}

void test_corpse_movement_and_model() {
    rules::DocumentSet documents;
    add_doc(documents, "features/corpses/ridge.tdf", "[RGBOT_DEAD]\n{\n}\n");
    add_doc(
        documents,
        "gamedata/moveinfo.tdf",
        "[CLASS0]\n{\nName=RGKBOT;\n}\n[CLASS32]\n{\nName=NO;\n}\n"
    );
    add_doc(
        documents,
        "units/RGBOT.fbi",
        "[UNITINFO]\n{\nUnitName=RGBOT;\nCorpse=NOCORPSE;\nMovementClass=NOCLASS;\n"
        "Objectname=NOMODEL;\n}\n"
    );
    add_doc(
        documents, "units/RGOK.fbi", "[UNITINFO]\n{\nUnitName=RGOK;\nMovementClass=RGKBOT;\n}\n"
    );
    const auto holds = [](std::string_view path) {
        return path != "objects3d/NOMODEL.3DO" && std::string_view{path}.find("scripts/") != 0;
    };
    mutators::IntegrityLimits limits;
    const std::vector<mutators::IntegrityIssue> found =
        mutators::integrity_issues(documents, oa::data::defs::data_layout(), limits, holds);
    OA_CHECK(has_problem(found, mutators::Problem::corpse_missing));
    OA_CHECK(has_problem(found, mutators::Problem::movement_class_missing));
    const mutators::IntegrityIssue* model = find_problem(found, mutators::Problem::model_missing);
    OA_CHECK(model != nullptr);
    if (model != nullptr) {
        OA_CHECK(model->record == "RGBOT");
        OA_CHECK(model->target == "NOMODEL");
        OA_CHECK(model->severity == mutators::Severity::failure);
    }
    const mutators::IntegrityIssue* script = find_problem(found, mutators::Problem::script_missing);
    OA_CHECK(script != nullptr);
    if (script != nullptr)
        OA_CHECK(script->severity == mutators::Severity::warning);
    const mutators::IntegrityIssue* extra =
        find_problem(found, mutators::Problem::too_many_movement_classes);
    OA_CHECK(extra != nullptr);
    if (extra != nullptr)
        OA_CHECK(extra->record == "CLASS32");
    OA_CHECK(
        !has_problem(found, mutators::Problem::movement_class_missing) ||
        find_problem(found, mutators::Problem::movement_class_missing)->record != "RGOK"
    );
    expect_filtered(found, mutators::Problem::corpse_missing);
    expect_filtered(found, mutators::Problem::model_missing);
    expect_filtered(found, mutators::Problem::script_missing);
    expect_filtered(found, mutators::Problem::too_many_movement_classes);
}

void test_sides_and_commander() {
    rules::DocumentSet documents;
    add_doc(
        documents,
        "gamedata/sidedata.tdf",
        "[SIDE0]\n{\nCommander=NOCOM;\n}\n[SIDE5]\n{\nCommander=NOCOM;\n}\n"
    );
    add_doc(documents, "units/RGCOM.fbi", "[UNITINFO]\n{\nUnitName=RGCOM;\n}\n");
    const std::vector<mutators::IntegrityIssue> found = issues_of(documents);
    const mutators::IntegrityIssue* commander =
        find_problem(found, mutators::Problem::side_without_commander);
    OA_CHECK(commander != nullptr);
    if (commander != nullptr) {
        OA_CHECK(commander->record == "SIDE0");
        OA_CHECK(commander->field == "commander");
        OA_CHECK(commander->target == "NOCOM");
    }
    const mutators::IntegrityIssue* extra = find_problem(found, mutators::Problem::too_many_sides);
    OA_CHECK(extra != nullptr);
    if (extra != nullptr)
        OA_CHECK(extra->record == "SIDE5");
    expect_filtered(found, mutators::Problem::side_without_commander);
    expect_filtered(found, mutators::Problem::too_many_sides);

    rules::DocumentSet seven;
    add_doc(
        seven,
        "gamedata/sidedata.tdf",
        "[SIDE0]\n{\nCommander=RGCOM;\n}\n"
        "[SIDE1]\n{\nCommander=RGCOM;\n}\n"
        "[SIDE2]\n{\nCommander=RGCOM;\n}\n"
        "[SIDE3]\n{\nCommander=RGCOM;\n}\n"
        "[SIDE4]\n{\nCommander=RGCOM;\n}\n"
        "[SIDE5]\n{\nCommander=RGCOM;\n}\n"
        "[SIDE6]\n{\nCommander=NOCOM;\n}\n"
        "[SIDE7]\n{\nCommander=RGCOM;\n}\n"
    );
    add_doc(seven, "units/RGCOM.fbi", "[UNITINFO]\n{\nUnitName=RGCOM;\n}\n");
    mutators::IntegrityLimits wide;
    wide.sides = 7;
    const std::vector<mutators::IntegrityIssue> followed =
        mutators::integrity_issues(seven, oa::data::defs::data_layout(), wide, holds_all);
    for (const mutators::IntegrityIssue& issue : followed) {
        if (issue.problem == mutators::Problem::too_many_sides)
            OA_CHECK(issue.record != "SIDE5" && issue.record != "SIDE6");
        if (issue.problem == mutators::Problem::side_without_commander)
            OA_CHECK(issue.record != "SIDE5");
    }
    const mutators::IntegrityIssue* missing_commander = nullptr;
    const mutators::IntegrityIssue* past = nullptr;
    for (const mutators::IntegrityIssue& issue : followed) {
        if (issue.problem == mutators::Problem::side_without_commander && issue.record == "SIDE6")
            missing_commander = &issue;
        if (issue.problem == mutators::Problem::too_many_sides && issue.record == "SIDE7")
            past = &issue;
    }
    OA_CHECK(missing_commander != nullptr);
    OA_CHECK(past != nullptr);
    if (past != nullptr)
        OA_CHECK(past->message.find("past the 7 sides") != std::string::npos);

    rules::DocumentSet removed;
    add_doc(removed, "units/RGCOM.fbi", "[UNITINFO]\n{\nUnitName=RGCOM;\n}\n");
    add_doc(removed, "gamedata/sidedata.tdf", "[SIDE0]\n{\nCommander=RGCOM;\n}\n");
    rules::LoadedDocument* unit = removed.find("units/RGCOM.fbi");
    OA_CHECK(unit != nullptr);
    if (unit != nullptr)
        unit->removed = true;
    const std::vector<mutators::IntegrityIssue> after = issues_of(removed);
    const mutators::IntegrityIssue* gone =
        find_problem(after, mutators::Problem::side_without_commander);
    OA_CHECK(gone != nullptr);
    mutators::Journal journal;
    mutators::JournalEntry entry;
    entry.mutators.push_back(1);
    entry.change = mutators::Change::removed;
    entry.collection = mutators::Collection::units;
    entry.record = "RGCOM";
    journal.entries.push_back(std::move(entry));
    journal.entries.push_back(set_field(0, "SIDE0", "name"));
    const std::vector<mutators::IntegrityIssue> blamed =
        mutators::new_issues({}, after, journal, oa::data::defs::data_layout());
    const mutators::IntegrityIssue* named =
        find_problem(blamed, mutators::Problem::side_without_commander);
    OA_CHECK(named != nullptr);
    if (named != nullptr)
        OA_CHECK(named->mutator == std::optional<uint16_t>{1});
}

void test_download_menu_and_page() {
    rules::DocumentSet documents;
    add_doc(documents, "units/RGLAB.fbi", "[UNITINFO]\n{\nUnitName=RGLAB;\nCopyright=;\n}\n");
    add_doc(documents, "units/RGBOT.fbi", "[UNITINFO]\n{\nUnitName=RGBOT;\n}\n");
    add_doc(
        documents,
        "download/rglab.tdf",
        "[KEPT]\n{\nUNITMENU=;\nUNITNAME=RGBOT;\n}\n"
        "[NEW]\n{\nUNITMENU=RGLAB;\nUNITNAME=NOBODY;\n}\n"
    );
    add_doc(documents, "guis/missing.gui", "[GADGET0]\n{\n}\n");
    rules::LoadedDocument* page = documents.find("guis/missing.gui");
    OA_CHECK(page != nullptr);
    if (page != nullptr)
        page->added = true;
    const auto holds = [](std::string_view path) { return path != "guis/missing.gui"; };
    mutators::IntegrityLimits limits;
    const std::vector<mutators::IntegrityIssue> found =
        mutators::integrity_issues(documents, oa::data::defs::data_layout(), limits, holds);
    const mutators::IntegrityIssue* menu =
        find_problem(found, mutators::Problem::menu_names_missing_unit);
    OA_CHECK(menu != nullptr);
    if (menu != nullptr) {
        OA_CHECK(menu->record == "NEW");
        OA_CHECK(menu->field == "unitname");
        OA_CHECK(menu->target == "NOBODY");
        OA_CHECK(menu->message.find("download/rglab.tdf ") == 0);
    }
    OA_CHECK(has_problem(found, mutators::Problem::gui_edit_names_missing_page));
    // Copyright="" leaves RGLAB present, so no issue names it.
    for (const mutators::IntegrityIssue& issue : found) {
        OA_CHECK(issue.record != "RGLAB");
        OA_CHECK(issue.target != "RGLAB");
        OA_CHECK(issue.message.find("RGLAB") == std::string::npos);
    }
    expect_filtered(found, mutators::Problem::menu_names_missing_unit);
    expect_filtered(found, mutators::Problem::gui_edit_names_missing_page);

    mutators::Journal journal;
    mutators::JournalEntry added;
    added.mutators.push_back(3);
    added.change = mutators::Change::added;
    added.collection = mutators::Collection::menus;
    added.record = "NEW";
    journal.entries.push_back(std::move(added));
    const std::vector<mutators::IntegrityIssue> blamed =
        mutators::new_issues({}, found, journal, oa::data::defs::data_layout());
    const mutators::IntegrityIssue* named =
        find_problem(blamed, mutators::Problem::menu_names_missing_unit);
    OA_CHECK(named != nullptr && named->mutator == std::optional<uint16_t>{3});
}

void test_file_name_and_shared_name() {
    rules::DocumentSet documents;
    add_doc(documents, "units/WRONG.fbi", "[UNITINFO]\n{\nUnitName=RGOTHER;\n}\n");
    add_doc(documents, "units/ONE.fbi", "[UNITINFO]\n{\nUnitName=SAME;\n}\n");
    add_doc(documents, "units/TWO.fbi", "[UNITINFO]\n{\nUnitName=SAME;\n}\n");
    const std::vector<mutators::IntegrityIssue> found = issues_of(documents);
    const mutators::IntegrityIssue* named =
        find_problem(found, mutators::Problem::file_name_differs);
    OA_CHECK(named != nullptr);
    if (named != nullptr) {
        OA_CHECK(named->record == "RGOTHER");
        OA_CHECK(named->field == "unitname");
        OA_CHECK(named->target == "WRONG");
    }
    const mutators::IntegrityIssue* twice = find_problem(found, mutators::Problem::unit_name_twice);
    OA_CHECK(twice != nullptr);
    if (twice != nullptr) {
        OA_CHECK(twice->record == "SAME");
        OA_CHECK(twice->target.find("TWO") != std::string::npos);
    }
    expect_filtered(found, mutators::Problem::file_name_differs);
    expect_filtered(found, mutators::Problem::unit_name_twice);

    rules::DocumentSet empty_name;
    add_doc(empty_name, "units/BARE.fbi", "[UNITINFO]\n{\nUnitName=;\n}\n");
    OA_CHECK(!has_problem(issues_of(empty_name), mutators::Problem::file_name_differs));
}

void test_category_and_sync_limits() {
    rules::DocumentSet few;
    for (int index = 0; index < 3; ++index) {
        const std::string name = "C" + std::to_string(index);
        add_doc(few, "units/" + name + ".fbi", "[UNITINFO]\n{\nUnitName=" + name + ";\n}\n");
    }
    mutators::IntegrityLimits narrow;
    narrow.category_mask_types = 4;
    OA_CHECK(!has_problem(
        mutators::integrity_issues(few, oa::data::defs::data_layout(), narrow, holds_all),
        mutators::Problem::past_category_masks
    ));
    add_doc(few, "units/C3.fbi", "[UNITINFO]\n{\nUnitName=C3;\n}\n");
    const std::vector<mutators::IntegrityIssue> warned =
        mutators::integrity_issues(few, oa::data::defs::data_layout(), narrow, holds_all);
    const mutators::IntegrityIssue* category =
        find_problem(warned, mutators::Problem::past_category_masks);
    OA_CHECK(category != nullptr);
    if (category != nullptr) {
        OA_CHECK(category->severity == mutators::Severity::warning);
        OA_CHECK(category->record.empty());
        OA_CHECK(category->field.empty());
        OA_CHECK(category->target.empty());
    }
    expect_filtered(warned, mutators::Problem::past_category_masks);

    rules::DocumentSet crowd;
    for (int index = 0; index < 1025; ++index) {
        const std::string name = "S" + std::to_string(index);
        add_doc(crowd, "units/" + name + ".fbi", "[UNITINFO]\n{\nUnitName=" + name + ";\n}\n");
    }
    mutators::IntegrityLimits local;
    const std::vector<mutators::IntegrityIssue> soft =
        mutators::integrity_issues(crowd, oa::data::defs::data_layout(), local, holds_all);
    const mutators::IntegrityIssue* warning = find_problem(soft, mutators::Problem::past_unit_sync);
    OA_CHECK(warning != nullptr);
    if (warning != nullptr) {
        OA_CHECK(warning->severity == mutators::Severity::warning);
        OA_CHECK(warning->record.empty());
    }
    mutators::IntegrityLimits shared;
    shared.shared = true;
    const std::vector<mutators::IntegrityIssue> hard =
        mutators::integrity_issues(crowd, oa::data::defs::data_layout(), shared, holds_all);
    const mutators::IntegrityIssue* failure = find_problem(hard, mutators::Problem::past_unit_sync);
    OA_CHECK(failure != nullptr);
    if (failure != nullptr)
        OA_CHECK(failure->severity == mutators::Severity::failure);
    expect_filtered(hard, mutators::Problem::past_unit_sync);
}

void test_unit_and_download_caps() {
    rules::DocumentSet downloads;
    for (int index = 0; index < 4096; ++index)
        add_doc(downloads, "download/d" + std::to_string(index) + ".tdf", "[MENUENTRY]\n{\n}\n");
    OA_CHECK(!has_problem(issues_of(downloads), mutators::Problem::too_many_download_files));
    add_doc(downloads, "download/d4096.tdf", "[MENUENTRY]\n{\n}\n");
    const std::vector<mutators::IntegrityIssue> many = issues_of(downloads);
    OA_CHECK(has_problem(many, mutators::Problem::too_many_download_files));
    expect_filtered(many, mutators::Problem::too_many_download_files);

    mutators::Journal journal;
    mutators::JournalEntry added;
    added.mutators.push_back(2);
    added.change = mutators::Change::added;
    added.collection = mutators::Collection::files;
    added.path = "download/d4096.tdf";
    journal.entries.push_back(set_field(0, "RGBOT", "maxdamage"));
    journal.entries.push_back(std::move(added));
    const std::vector<mutators::IntegrityIssue> blamed =
        mutators::new_issues({}, many, journal, oa::data::defs::data_layout());
    const mutators::IntegrityIssue* named =
        find_problem(blamed, mutators::Problem::too_many_download_files);
    OA_CHECK(named != nullptr && named->mutator == std::optional<uint16_t>{2});
}

void test_blame_follows_the_journal_order() {
    mutators::IntegrityIssue issue;
    issue.problem = mutators::Problem::weapon_missing;
    issue.severity = mutators::Severity::note;
    issue.record = "RGBOT";
    issue.field = "weapon1";
    issue.target = "NOGUN";
    issue.message = "RGBOT's weapon1 names NOGUN, which no weapon holds";

    mutators::Journal unordered;
    unordered.entries.push_back(set_field(1, "RGBOT", "maxdamage"));
    unordered.entries.push_back(set_field(0, "RGBOT", "weapon1"));
    const std::vector<mutators::IntegrityIssue> field_writer = mutators::new_issues(
        {},
        std::span<const mutators::IntegrityIssue>{&issue, 1},
        unordered,
        oa::data::defs::data_layout()
    );
    OA_CHECK(field_writer.size() == 1);
    if (!field_writer.empty())
        OA_CHECK(field_writer[0].mutator == std::optional<uint16_t>{0});

    mutators::Journal removed;
    removed.entries.push_back(set_field(0, "RGBOT", "maxdamage"));
    mutators::JournalEntry drop;
    drop.mutators.push_back(1);
    drop.change = mutators::Change::removed;
    drop.collection = mutators::Collection::weapons;
    drop.record = "NOGUN";
    removed.entries.push_back(std::move(drop));
    const std::vector<mutators::IntegrityIssue> remover = mutators::new_issues(
        {},
        std::span<const mutators::IntegrityIssue>{&issue, 1},
        removed,
        oa::data::defs::data_layout()
    );
    OA_CHECK(!remover.empty() && remover[0].mutator == std::optional<uint16_t>{1});

    mutators::Journal records;
    records.entries.push_back(set_field(0, "RGBOT", "maxdamage"));
    records.entries.push_back(set_field(2, "RGBOT", "buildcostmetal"));
    const std::vector<mutators::IntegrityIssue> later = mutators::new_issues(
        {},
        std::span<const mutators::IntegrityIssue>{&issue, 1},
        records,
        oa::data::defs::data_layout()
    );
    OA_CHECK(!later.empty() && later[0].mutator == std::optional<uint16_t>{2});

    mutators::IntegrityIssue folded = issue;
    folded.record = "rgbot";
    folded.field = "WEAPON1";
    folded.target = "nogun";
    const std::vector<mutators::IntegrityIssue> same = mutators::new_issues(
        std::span<const mutators::IntegrityIssue>{&folded, 1},
        std::span<const mutators::IntegrityIssue>{&issue, 1},
        mutators::Journal{},
        oa::data::defs::data_layout()
    );
    OA_CHECK(same.empty());
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

mutators::PipelineMutator load_mutator(const fs::path& package) {
    mutators::PipelineMutator mutator;
    mutator.package = package;
    const std::vector<uint8_t> bytes = read_bytes(package / "oamut.yaml");
    std::vector<oamut::Problem> problems;
    OA_CHECK(oamut::read_manifest(bytes, mutator.manifest, problems));
    return mutator;
}

mutators::PipelineInput make_input(std::vector<mutators::PipelineMutator> list) {
    mutators::PipelineInput input;
    input.mutators = std::move(list);
    input.mod.id = "ridge";
    input.mod.version = "1.0";
    input.mod.game = "ridge";
    input.engine_version = "0.8.5";
    input.layout = &oa::data::defs::data_layout();
    input.seed = 1;
    return input;
}

bool payloads_match(const oa::MutatorLayerSpec& left, const oa::MutatorLayerSpec& right) {
    if (left.entries.size() != right.entries.size() || left.masks.size() != right.masks.size())
        return false;
    for (std::size_t index = 0; index < left.entries.size(); ++index) {
        const oa::MutatorLayerEntry& one = left.entries[index];
        const oa::MutatorLayerEntry& other = right.entries[index];
        if (one.path != other.path || one.kind != other.kind)
            return false;
        if (one.contents == nullptr || other.contents == nullptr) {
            if (one.contents != nullptr || other.contents != nullptr)
                return false;
        } else if (*one.contents != *other.contents) {
            return false;
        }
    }
    return true;
}

void test_base_missing_name_is_not_new() {
    mutators::IntegrityIssue earlier;
    earlier.problem = mutators::Problem::explosion_missing;
    earlier.severity = mutators::Severity::note;
    earlier.record = "A";
    earlier.field = "explodeas";
    earlier.target = "X";
    mutators::IntegrityIssue later = earlier;
    later.record = "B";
    const std::vector<mutators::IntegrityIssue> kept = mutators::new_issues(
        std::span<const mutators::IntegrityIssue>{&earlier, 1},
        std::span<const mutators::IntegrityIssue>{&later, 1},
        mutators::Journal{},
        oa::data::defs::data_layout()
    );
    OA_CHECK(kept.empty());

    mutators::IntegrityIssue model = earlier;
    model.problem = mutators::Problem::model_missing;
    model.field = "objectname";
    mutators::IntegrityIssue model_copy = model;
    model_copy.record = "B";
    const std::vector<mutators::IntegrityIssue> still = mutators::new_issues(
        std::span<const mutators::IntegrityIssue>{&model, 1},
        std::span<const mutators::IntegrityIssue>{&model_copy, 1},
        mutators::Journal{},
        oa::data::defs::data_layout()
    );
    OA_CHECK(still.size() == 1);
}

void test_clone_of_a_broken_explosion_applies() {
    oa::AssetStore store(k_root / "ridge");
    std::vector<mutators::PipelineMutator> list;
    list.push_back(load_mutator(k_root / "packages" / "ridge-clone-boom"));
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, make_input(std::move(list)));
    OA_CHECK(result.playable());
    if (!result.playable() && !result.mutators.empty())
        std::fprintf(stderr, "clone: %s\n", result.mutators[0].message.c_str());
    // The clone is served and still names the explosion its source lacks.
    OA_CHECK(store.file_size("units/RGBOOM2.fbi") > 0);
    if (store.file_size("units/RGBOOM2.fbi") > 0) {
        const std::vector<uint8_t> bytes = store.read("units/RGBOOM2.fbi").bytes;
        const std::string text(bytes.begin(), bytes.end());
        OA_CHECK(text.find("NOBOOM") != std::string::npos);
    }
    if (store.mutator_layer_mounted())
        store.unmount_mutator_layer();
}

void test_pipeline_leaves_the_failing_mutator_out() {
    oa::AssetStore paired(k_root / "ridge");
    std::vector<mutators::PipelineMutator> both;
    both.push_back(load_mutator(k_root / "packages" / "ridge-bad-gun"));
    both.push_back(load_mutator(k_root / "packages" / "ridge-paint"));
    const mutators::PipelineResult result =
        mutators::run_pipeline(paired, make_input(std::move(both)));
    OA_CHECK(result.mutators.size() == 2);
    if (result.mutators.size() == 2) {
        OA_CHECK(result.mutators[0].outcome == mutators::Outcome::failed);
        OA_CHECK(result.mutators[0].phase == mutators::Phase::integrity);
        OA_CHECK(result.mutators[0].cause == mutators::Cause::integrity);
        OA_CHECK(result.mutators[0].message.find("NOGUN") != std::string::npos);
        OA_CHECK(result.mutators[1].outcome == mutators::Outcome::applied);
        if (result.mutators[0].outcome != mutators::Outcome::failed)
            std::fprintf(stderr, "bad-gun: %s\n", result.mutators[0].message.c_str());
    }
    if (paired.mutator_layer_mounted())
        paired.unmount_mutator_layer();

    oa::AssetStore alone(k_root / "ridge");
    std::vector<mutators::PipelineMutator> paint;
    paint.push_back(load_mutator(k_root / "packages" / "ridge-paint"));
    const mutators::PipelineResult painted =
        mutators::run_pipeline(alone, make_input(std::move(paint)));
    OA_CHECK(painted.playable());
    if (!painted.playable() && !painted.mutators.empty())
        std::fprintf(stderr, "paint: %s\n", painted.mutators[0].message.c_str());
    OA_CHECK(payloads_match(result.layer, painted.layer));
    if (alone.mutator_layer_mounted())
        alone.unmount_mutator_layer();
}

fs::path swift_package() {
    return k_root.parent_path() / "packages" / "ridge-swift";
}

/// The culprit is first and ridge-swift second. The culprit fails the check
/// and the layer matches ridge-swift alone.
void expect_swift_survives(const fs::path& culprit, const char* label) {
    oa::AssetStore paired(k_root / "ridge");
    std::vector<mutators::PipelineMutator> both;
    both.push_back(load_mutator(culprit));
    both.push_back(load_mutator(swift_package()));
    const mutators::PipelineResult result =
        mutators::run_pipeline(paired, make_input(std::move(both)));
    OA_CHECK(result.mutators.size() == 2);
    if (result.mutators.size() == 2) {
        OA_CHECK(result.mutators[0].outcome == mutators::Outcome::failed);
        OA_CHECK(result.mutators[0].phase == mutators::Phase::integrity);
        OA_CHECK(result.mutators[0].cause == mutators::Cause::integrity);
        OA_CHECK(result.mutators[1].outcome == mutators::Outcome::applied);
        if (result.mutators[0].phase != mutators::Phase::integrity)
            std::fprintf(stderr, "%s: %s\n", label, result.mutators[0].message.c_str());
    }
    if (paired.mutator_layer_mounted())
        paired.unmount_mutator_layer();

    oa::AssetStore alone(k_root / "ridge");
    std::vector<mutators::PipelineMutator> swift;
    swift.push_back(load_mutator(swift_package()));
    const mutators::PipelineResult kept =
        mutators::run_pipeline(alone, make_input(std::move(swift)));
    OA_CHECK(kept.playable());
    if (!kept.playable() && !kept.mutators.empty())
        std::fprintf(stderr, "swift: %s\n", kept.mutators[0].message.c_str());
    OA_CHECK(payloads_match(result.layer, kept.layer));
    if (alone.mutator_layer_mounted())
        alone.unmount_mutator_layer();
}

void test_mask_and_added_unit_blame_the_culprit() {
    expect_swift_survives(k_root / "packages" / "ridge-hide-lab", "hide-lab");
    expect_swift_survives(k_root / "packages" / "ridge-hide-guns", "hide-guns");
    expect_swift_survives(k_root / "packages" / "ridge-name-nogun", "name-nogun");
}

void test_blank_copyright_applies() {
    oa::AssetStore store(k_root / "ridge");
    std::vector<mutators::PipelineMutator> list;
    list.push_back(load_mutator(k_root / "packages" / "ridge-blank-copy"));
    const mutators::PipelineResult result =
        mutators::run_pipeline(store, make_input(std::move(list)));
    OA_CHECK(result.playable());
    if (!result.playable() && !result.mutators.empty())
        std::fprintf(stderr, "copyright: %s\n", result.mutators[0].message.c_str());
    for (const mutators::MutatorResult& one : result.mutators)
        OA_CHECK(one.cause != mutators::Cause::integrity);
    if (store.mutator_layer_mounted())
        store.unmount_mutator_layer();
}

void test_renamed_movement_class_is_blamed_on_its_writer() {
    mutators::IntegrityIssue issue;
    issue.problem = mutators::Problem::movement_class_missing;
    issue.severity = mutators::Severity::failure;
    issue.record = "RGBOT";
    issue.field = "movementclass";
    issue.target = "RGKBOT";
    issue.message = "RGBOT's movementclass names RGKBOT, which no movement class holds";
    mutators::Journal journal;
    mutators::JournalEntry renamed = set_field(0, "CLASS0", "name");
    renamed.old_text = "RGKBOT";
    journal.entries.push_back(std::move(renamed));
    journal.entries.push_back(set_field(1, "RGBOT", "maxdamage"));
    const std::vector<mutators::IntegrityIssue> blamed = mutators::new_issues(
        {},
        std::span<const mutators::IntegrityIssue>{&issue, 1},
        journal,
        oa::data::defs::data_layout()
    );
    OA_CHECK(blamed.size() == 1);
    if (!blamed.empty())
        OA_CHECK(blamed[0].mutator == std::optional<uint16_t>{0});
}

} // namespace

int main() {
    test_weapon_must_hold_its_slot();
    test_weapon_id_300_takes_no_slot();
    test_weapon_id_300_fits_a_wide_table();
    test_corpse_movement_and_model();
    test_sides_and_commander();
    test_download_menu_and_page();
    test_file_name_and_shared_name();
    test_category_and_sync_limits();
    test_unit_and_download_caps();
    test_blame_follows_the_journal_order();
    test_base_missing_name_is_not_new();
    test_clone_of_a_broken_explosion_applies();
    test_pipeline_leaves_the_failing_mutator_out();
    test_mask_and_added_unit_blame_the_culprit();
    test_blank_copyright_applies();
    test_renamed_movement_class_is_blamed_on_its_writer();
    return oa::test::check_exit_status();
}

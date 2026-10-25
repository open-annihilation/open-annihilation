// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Weapon ids for weapons a mutator brought. Base weapons keep their ids.
// Each mutator, in list order, keeps the requested ids that are still
// free and then takes ids for its other weapons. A later request for a
// taken id takes another free one. Nothing is written when the table has
// no id left, and the mutator that overflows is the one refused.
#include "oa/data/mutators/weapon_ids.hpp"
#include "oa/formats/tdf.hpp"
#include "oa/test/check.hpp"

#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace mutators = oa::data::mutators;
namespace rules = oa::data::mutation_rules;
namespace tdf = oa::formats::tdf;
namespace defs = oa::data::defs;

bool add_doc(rules::DocumentSet& documents, std::string path, std::string_view text) {
    std::string error;
    const bool added = documents.add(std::move(path), text, &error);
    if (!added)
        std::fprintf(stderr, "weapon ids: %s\n", error.c_str());
    OA_CHECK(added);
    return added;
}

std::string id_text(rules::DocumentSet& documents, std::string_view path, std::string_view weapon) {
    rules::LoadedDocument* loaded = documents.find(path);
    OA_CHECK(loaded != nullptr);
    if (loaded == nullptr)
        return {};
    const tdf::Block* root = loaded->document.get()->root;
    const std::string name{weapon};
    const tdf::Block* section = root != nullptr ? tdf::find_child(root, name.c_str()) : nullptr;
    const char* value = section != nullptr ? tdf::find_value(section, "ID") : nullptr;
    return value != nullptr ? std::string{value} : std::string{};
}

mutators::BroughtWeapon brought_of(std::string name, uint16_t mutator, std::string path) {
    mutators::BroughtWeapon weapon;
    weapon.name = std::move(name);
    weapon.mutator = mutator;
    weapon.path = std::move(path);
    return weapon;
}

void test_request_is_honoured_and_base_id_stays() {
    rules::DocumentSet documents;
    add_doc(documents, "weapons/ridge.tdf", "[RGGUN]\n{\nID=1;\n}\n[RGALPHA]\n{\nID=5;\n}\n");
    std::vector<mutators::BroughtWeapon> brought;
    brought.push_back(brought_of("RGALPHA", 0, "weapons/ridge.tdf"));
    mutators::WeaponIdResult result;
    OA_CHECK(mutators::assign_weapon_ids(documents, defs::data_layout(), brought, result));
    OA_CHECK(result.ok);
    OA_CHECK(brought.size() == 1 && brought[0].assigned_id == 5);
    OA_CHECK(id_text(documents, "weapons/ridge.tdf", "RGALPHA") == "5");
    OA_CHECK(id_text(documents, "weapons/ridge.tdf", "RGGUN") == "1");
}

void test_one_id_goes_to_the_earlier_mutator() {
    rules::DocumentSet documents;
    add_doc(
        documents,
        "weapons/extra.tdf",
        "[RGALPHA]\n{\nID=5;\n}\n[RGBETA]\n{\nID=5;\n}\n[RGGUN]\n{\nID=1;\n}\n"
    );
    std::vector<mutators::BroughtWeapon> brought;
    brought.push_back(brought_of("RGALPHA", 0, "weapons/extra.tdf"));
    brought.push_back(brought_of("RGBETA", 1, "weapons/extra.tdf"));
    mutators::WeaponIdResult result;
    OA_CHECK(mutators::assign_weapon_ids(documents, defs::data_layout(), brought, result));
    OA_CHECK(brought[0].assigned_id == 5);
    OA_CHECK(brought[1].assigned_id == 2);
    OA_CHECK(id_text(documents, "weapons/extra.tdf", "RGBETA") == "2");
    OA_CHECK(id_text(documents, "weapons/extra.tdf", "RGGUN") == "1");
}

void test_same_mutator_orders_by_name() {
    rules::DocumentSet documents;
    add_doc(documents, "Weapons/extra.tdf", "[ZETA]\n{\n}\n[ALPHA]\n{\n}\n");
    std::vector<mutators::BroughtWeapon> brought;
    brought.push_back(brought_of("ZETA", 0, "Weapons/extra.tdf"));
    brought.push_back(brought_of("ALPHA", 0, "Weapons/extra.tdf"));
    mutators::WeaponIdResult result;
    OA_CHECK(mutators::assign_weapon_ids(documents, defs::data_layout(), brought, result));
    OA_CHECK(brought[0].assigned_id == 2);
    OA_CHECK(brought[1].assigned_id == 1);
    OA_CHECK(brought[0].assigned_id != 0);
    OA_CHECK(id_text(documents, "Weapons/extra.tdf", "ALPHA") == "1");
    OA_CHECK(id_text(documents, "Weapons/extra.tdf", "ZETA") == "2");
}

void test_zero_and_out_of_range_are_not_requests() {
    rules::DocumentSet documents;
    add_doc(
        documents,
        "weapons/extra.tdf",
        "[RGZERO]\n{\nID=0;\n}\n[RGWIDE]\n{\nID=300;\n}\n[RGTEXT]\n{\nID=5abc;\n}\n"
        "[RGGUN]\n{\nID=1;\n}\n"
    );
    std::vector<mutators::BroughtWeapon> brought;
    brought.push_back(brought_of("RGWIDE", 0, "weapons/extra.tdf"));
    brought.push_back(brought_of("RGZERO", 0, "weapons/extra.tdf"));
    brought.push_back(brought_of("RGTEXT", 1, "weapons/extra.tdf"));
    mutators::WeaponIdResult result;
    OA_CHECK(mutators::assign_weapon_ids(documents, defs::data_layout(), brought, result));
    // RGTEXT asks for 5. The others take free ids from 1, skipping the base id
    // and skipping 0. RGWIDE is the earlier mutator and sorts first by name
    // against RGZERO, so it takes 2 and RGZERO takes 3.
    OA_CHECK(brought[2].assigned_id == 5);
    OA_CHECK(brought[0].assigned_id == 2);
    OA_CHECK(brought[1].assigned_id == 3);
    OA_CHECK(id_text(documents, "weapons/extra.tdf", "RGGUN") == "1");
    OA_CHECK(id_text(documents, "weapons/extra.tdf", "RGZERO") == "3");
}

/// The earlier mutator has no request, so it takes the lowest free id.
/// The later mutator's request for that id then takes the next free one.
void test_earlier_unrequested_keeps_a_later_request() {
    rules::DocumentSet documents;
    add_doc(
        documents, "weapons/ridge.tdf", "[RGGUN]\n{\nID=1;\n}\n[EARLY]\n{\n}\n[LATE]\n{\nID=2;\n}\n"
    );
    std::vector<mutators::BroughtWeapon> brought;
    brought.push_back(brought_of("EARLY", 0, "weapons/ridge.tdf"));
    brought.push_back(brought_of("LATE", 1, "weapons/ridge.tdf"));
    mutators::WeaponIdResult result;
    OA_CHECK(mutators::assign_weapon_ids(documents, defs::data_layout(), brought, result));
    OA_CHECK(result.ok);
    OA_CHECK(brought[0].assigned_id == 2);
    OA_CHECK(brought[1].assigned_id == 3);
    OA_CHECK(id_text(documents, "weapons/ridge.tdf", "RGGUN") == "1");
    OA_CHECK(id_text(documents, "weapons/ridge.tdf", "EARLY") == "2");
    OA_CHECK(id_text(documents, "weapons/ridge.tdf", "LATE") == "3");
}

/// Base weapons hold 1 to 253, so 254 and 255 are the only free ids.
std::string bases_through_253() {
    std::string text;
    for (int id = 1; id <= 253; ++id) {
        text += "[BASE";
        text += std::to_string(id);
        text += "]\n{\nID=";
        text += std::to_string(id);
        text += ";\n}\n";
    }
    return text;
}

/// The earlier mutator's two weapons take 254 and 255. The later mutator
/// asked for those ids, so it is the one left without an id, and nothing
/// is written.
void test_later_requests_overflow_after_an_earlier_mutator() {
    rules::DocumentSet documents;
    add_doc(
        documents,
        "weapons/full.tdf",
        bases_through_253() + "[EARLYA]\n{\n}\n[EARLYB]\n{\n}\n[LATEA]\n{\nID=254;\n}\n"
                              "[LATEB]\n{\nID=255;\n}\n"
    );
    std::vector<mutators::BroughtWeapon> brought;
    brought.push_back(brought_of("EARLYB", 0, "weapons/full.tdf"));
    brought.push_back(brought_of("EARLYA", 0, "weapons/full.tdf"));
    brought.push_back(brought_of("LATEB", 1, "weapons/full.tdf"));
    brought.push_back(brought_of("LATEA", 1, "weapons/full.tdf"));
    mutators::WeaponIdResult result;
    OA_CHECK(!mutators::assign_weapon_ids(documents, defs::data_layout(), brought, result));
    OA_CHECK(!result.ok);
    OA_CHECK(result.mutator == 1);
    OA_CHECK(result.weapon == "LATEA");
    OA_CHECK(brought[0].assigned_id == 0);
    OA_CHECK(brought[3].assigned_id == 0);
    OA_CHECK(id_text(documents, "weapons/full.tdf", "BASE1") == "1");
    OA_CHECK(id_text(documents, "weapons/full.tdf", "BASE253") == "253");
    OA_CHECK(id_text(documents, "weapons/full.tdf", "LATEA") == "254");
    OA_CHECK(id_text(documents, "weapons/full.tdf", "LATEB") == "255");
}

/// The earlier mutator keeps both of the ids it asked for. The later
/// mutator's weapons then have none left, so that mutator is refused.
void test_earlier_requests_leave_the_later_mutator_to_overflow() {
    rules::DocumentSet documents;
    add_doc(
        documents,
        "weapons/full.tdf",
        bases_through_253() + "[LATEA]\n{\nID=254;\n}\n[LATEB]\n{\nID=255;\n}\n"
                              "[EARLYA]\n{\n}\n[EARLYB]\n{\n}\n"
    );
    std::vector<mutators::BroughtWeapon> brought;
    brought.push_back(brought_of("LATEB", 0, "weapons/full.tdf"));
    brought.push_back(brought_of("LATEA", 0, "weapons/full.tdf"));
    brought.push_back(brought_of("EARLYB", 1, "weapons/full.tdf"));
    brought.push_back(brought_of("EARLYA", 1, "weapons/full.tdf"));
    mutators::WeaponIdResult result;
    OA_CHECK(!mutators::assign_weapon_ids(documents, defs::data_layout(), brought, result));
    OA_CHECK(!result.ok);
    OA_CHECK(result.mutator == 1);
    OA_CHECK(result.weapon == "EARLYA");
    OA_CHECK(brought[2].assigned_id == 0);
    OA_CHECK(brought[3].assigned_id == 0);
    OA_CHECK(id_text(documents, "weapons/full.tdf", "LATEA") == "254");
    OA_CHECK(id_text(documents, "weapons/full.tdf", "EARLYA") == "");
    OA_CHECK(id_text(documents, "weapons/full.tdf", "BASE253") == "253");
}

void test_full_table_writes_nothing() {
    std::string text;
    for (int id = 1; id <= 255; ++id) {
        text += "[BASE";
        text += std::to_string(id);
        text += "]\n{\nID=";
        text += std::to_string(id);
        text += ";\n}\n";
    }
    text += "[RGMORE]\n{\nID=4;\n}\n";
    rules::DocumentSet documents;
    add_doc(documents, "weapons/full.tdf", text);
    std::vector<mutators::BroughtWeapon> brought;
    brought.push_back(brought_of("RGMORE", 3, "weapons/full.tdf"));
    mutators::WeaponIdResult result;
    OA_CHECK(!mutators::assign_weapon_ids(documents, defs::data_layout(), brought, result));
    OA_CHECK(!result.ok);
    OA_CHECK(result.mutator == 3);
    OA_CHECK(result.weapon == "RGMORE");
    OA_CHECK(brought[0].assigned_id == 0);
    OA_CHECK(id_text(documents, "weapons/full.tdf", "RGMORE") == "4");
    OA_CHECK(id_text(documents, "weapons/full.tdf", "BASE1") == "1");
    OA_CHECK(id_text(documents, "weapons/full.tdf", "BASE255") == "255");
}

/// Bases hold 1 to 253, so three weapons with no request take 254, 255 and 256.
void test_wide_limit_gives_256_to_the_third_weapon() {
    rules::DocumentSet documents;
    add_doc(
        documents, "weapons/wide.tdf", bases_through_253() + "[A]\n{\n}\n[B]\n{\n}\n[C]\n{\n}\n"
    );
    std::vector<mutators::BroughtWeapon> brought;
    brought.push_back(brought_of("A", 0, "weapons/wide.tdf"));
    brought.push_back(brought_of("B", 0, "weapons/wide.tdf"));
    brought.push_back(brought_of("C", 0, "weapons/wide.tdf"));
    mutators::WeaponIdResult result;
    OA_CHECK(mutators::assign_weapon_ids(documents, defs::data_layout(), brought, result, 1024));
    OA_CHECK(result.ok);
    OA_CHECK(brought[0].assigned_id == 254);
    OA_CHECK(brought[1].assigned_id == 255);
    OA_CHECK(brought[2].assigned_id == 256);
    OA_CHECK(id_text(documents, "weapons/wide.tdf", "C") == "256");
}

/// 1,000 is inside 1,024 types and is kept. 1,024 is not a request, so that
/// weapon takes the lowest free id instead.
void test_wide_limit_keeps_1000_and_skips_1024() {
    rules::DocumentSet documents;
    add_doc(documents, "weapons/wide.tdf", "[KEEP]\n{\nID=1000;\n}\n[PAST]\n{\nID=1024;\n}\n");
    std::vector<mutators::BroughtWeapon> brought;
    brought.push_back(brought_of("KEEP", 0, "weapons/wide.tdf"));
    brought.push_back(brought_of("PAST", 0, "weapons/wide.tdf"));
    mutators::WeaponIdResult result;
    OA_CHECK(mutators::assign_weapon_ids(documents, defs::data_layout(), brought, result, 1024));
    OA_CHECK(result.ok);
    OA_CHECK(brought[0].assigned_id == 1000);
    OA_CHECK(brought[1].assigned_id == 1);
    OA_CHECK(id_text(documents, "weapons/wide.tdf", "KEEP") == "1000");
    OA_CHECK(id_text(documents, "weapons/wide.tdf", "PAST") == "1");
}

/// Every id from 1 to 1,023 is taken, so the next weapon is refused by name.
void test_wide_table_full_through_1023_refuses() {
    std::string text;
    for (int id = 1; id <= 1023; ++id) {
        text += "[BASE";
        text += std::to_string(id);
        text += "]\n{\nID=";
        text += std::to_string(id);
        text += ";\n}\n";
    }
    text += "[RGMORE]\n{\n}\n";
    rules::DocumentSet documents;
    add_doc(documents, "weapons/full.tdf", text);
    std::vector<mutators::BroughtWeapon> brought;
    brought.push_back(brought_of("RGMORE", 2, "weapons/full.tdf"));
    mutators::WeaponIdResult result;
    OA_CHECK(!mutators::assign_weapon_ids(documents, defs::data_layout(), brought, result, 1024));
    OA_CHECK(!result.ok);
    OA_CHECK(result.mutator == 2);
    OA_CHECK(result.weapon == "RGMORE");
    OA_CHECK(brought[0].assigned_id == 0);
    OA_CHECK(id_text(documents, "weapons/full.tdf", "RGMORE") == "");
    OA_CHECK(id_text(documents, "weapons/full.tdf", "BASE1023") == "1023");
}

} // namespace

int main() {
    test_request_is_honoured_and_base_id_stays();
    test_one_id_goes_to_the_earlier_mutator();
    test_same_mutator_orders_by_name();
    test_zero_and_out_of_range_are_not_requests();
    test_earlier_unrequested_keeps_a_later_request();
    test_later_requests_overflow_after_an_earlier_mutator();
    test_earlier_requests_leave_the_later_mutator_to_overflow();
    test_full_table_writes_nothing();
    test_wide_limit_gives_256_to_the_third_weapon();
    test_wide_limit_keeps_1000_and_skips_1024();
    test_wide_table_full_through_1023_refuses();
    return oa::test::check_exit_status();
}

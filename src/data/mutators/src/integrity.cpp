// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The loaders' rules, walked on the documents the pipeline parsed. A name
// resolves only when the loader would still see it: a weapon only while
// its section holds its slot, a class only in CLASS0 to CLASS31, a side
// only while every earlier side is present.
#include "oa/data/mutators/integrity.hpp"

#include "oa/core/move_class.h"
#include "oa/core/side.h"
#include "oa/data/defs/layout.hpp"
#include "oa/data/defs/unit_catalog.hpp"
#include "oa/formats/tdf.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::data::mutators {
namespace {

namespace defs = oa::data::defs;
namespace rules = oa::data::mutation_rules;
namespace tdf = oa::formats::tdf;

/// The features folder. It is not one of the directories a mod renames.
constexpr std::string_view k_features_folder = "features";

char fold_ascii(char byte) {
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<char>(byte - 'A' + 'a');
    return byte;
}

std::string fold_copy(std::string_view text) {
    std::string out(text.size(), '\0');
    for (std::size_t offset = 0; offset < text.size(); ++offset)
        out[offset] = fold_ascii(text[offset]);
    return out;
}

/// Negative when `left` folds before `right`, without assuming equal length.
int compare_folded(std::string_view left, std::string_view right) {
    const std::size_t count = left.size() < right.size() ? left.size() : right.size();
    for (std::size_t offset = 0; offset < count; ++offset) {
        const char first = fold_ascii(left[offset]);
        const char second = fold_ascii(right[offset]);
        if (first < second)
            return -1;
        if (first > second)
            return 1;
    }
    if (left.size() < right.size())
        return -1;
    if (left.size() > right.size())
        return 1;
    return 0;
}

bool same_text(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t offset = 0; offset < left.size(); ++offset) {
        if (fold_ascii(left[offset]) != fold_ascii(right[offset]))
            return false;
    }
    return true;
}

char fold_path_byte(char byte) {
    if (byte == '\\')
        return '/';
    return fold_ascii(byte);
}

bool same_path(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t offset = 0; offset < left.size(); ++offset) {
        if (fold_path_byte(left[offset]) != fold_path_byte(right[offset]))
            return false;
    }
    return true;
}

std::string_view file_name_of(std::string_view path) {
    const std::size_t slash = path.find_last_of("/\\");
    if (slash == std::string_view::npos)
        return path;
    return path.substr(slash + 1);
}

std::string stem_of(std::string_view path) {
    const std::string_view name = file_name_of(path);
    const std::size_t dot = name.find_last_of('.');
    if (dot == std::string_view::npos)
        return std::string{name};
    return std::string{name.substr(0, dot)};
}

bool file_named(std::string_view path, std::string_view file) {
    return same_text(file_name_of(path), file);
}

const char* value_of(const tdf::Block* block, const char* key) {
    if (block == nullptr)
        return nullptr;
    return tdf::find_value(block, key);
}

/// The text of a field, or empty when the field is missing or blank.
std::string_view text_of(const tdf::Block* block, const char* key) {
    const char* text = value_of(block, key);
    if (text == nullptr)
        return {};
    return text;
}

const tdf::Block* first_named(const tdf::Block* root, const char* name) {
    if (root == nullptr)
        return nullptr;
    const uint32_t count = tdf::child_count(root);
    for (uint32_t index = 0; index < count; ++index) {
        const tdf::Block* child = tdf::child_at(root, index);
        if (child != nullptr && child->name != nullptr && same_text(child->name, name))
            return child;
    }
    return nullptr;
}

/// CLASS or SIDE index, when the name is that word and then digits.
std::optional<uint32_t> numbered(std::string_view name, std::string_view prefix) {
    if (name.size() <= prefix.size() || !same_text(name.substr(0, prefix.size()), prefix))
        return std::nullopt;
    uint32_t value = 0;
    for (std::size_t offset = prefix.size(); offset < name.size(); ++offset) {
        const char digit = name[offset];
        if (digit < '0' || digit > '9')
            return std::nullopt;
        const uint32_t next = value * 10u + static_cast<uint32_t>(digit - '0');
        if (next < value)
            return std::nullopt;
        value = next;
    }
    return value;
}

struct UnitRow {
    std::string name;
    std::string path;
    const tdf::Block* info{};
};

struct WeaponSlot {
    bool held{};
    std::string name;
};

bool count_problem(Problem problem) {
    return problem == Problem::too_many_unit_types || problem == Problem::past_category_masks ||
           problem == Problem::past_unit_sync || problem == Problem::too_many_download_files;
}

Severity severity_of(Problem problem, const IntegrityLimits& limits) {
    if (problem == Problem::script_missing || problem == Problem::past_category_masks)
        return Severity::warning;
    if (problem == Problem::past_unit_sync && !limits.shared)
        return Severity::warning;
    return Severity::failure;
}

void add_issue(
    std::vector<IntegrityIssue>& issues,
    Problem problem,
    const IntegrityLimits& limits,
    std::string record,
    std::string field,
    std::string target,
    std::string path,
    std::string message
) {
    IntegrityIssue issue;
    issue.problem = problem;
    issue.severity = severity_of(problem, limits);
    issue.record = std::move(record);
    issue.field = std::move(field);
    issue.target = std::move(target);
    issue.path = std::move(path);
    issue.message = std::move(message);
    issues.push_back(std::move(issue));
}

std::string names_sentence(
    std::string_view record, std::string_view field, std::string_view target, std::string_view kind
) {
    std::string sentence;
    sentence.append(record);
    sentence += "'s ";
    sentence.append(field);
    sentence += " names ";
    sentence.append(target);
    sentence += ", which no ";
    sentence.append(kind);
    sentence += " holds";
    return sentence;
}

bool unit_named(const std::vector<UnitRow>& units, std::string_view name) {
    for (const UnitRow& unit : units) {
        if (same_text(unit.name, name))
            return true;
    }
    return false;
}

bool feature_named(const std::vector<std::string>& features, std::string_view name) {
    for (const std::string& feature : features) {
        if (same_text(feature, name))
            return true;
    }
    return false;
}

bool weapon_named(const std::vector<WeaponSlot>& slots, std::string_view name) {
    if (name.empty())
        return false;
    for (const WeaponSlot& slot : slots) {
        if (slot.held && same_text(slot.name, name))
            return true;
    }
    return false;
}

std::string_view directory_of(const defs::DataLayout& layout, defs::DataDirectory directory) {
    return layout.directories[static_cast<std::size_t>(directory)];
}

std::vector<std::size_t> files_in(
    const rules::DocumentSet& documents,
    const defs::DataLayout& layout,
    defs::DataDirectory directory,
    std::string_view suffix
) {
    return documents.documents_in(directory_of(layout, directory), suffix, false);
}

void collect_units(
    const rules::DocumentSet& documents, const defs::DataLayout& layout, std::vector<UnitRow>& units
) {
    const std::string suffix = "." + layout.unit_extension;
    const std::vector<std::size_t> listed =
        files_in(documents, layout, defs::DataDirectory::units, suffix);
    units.reserve(listed.size());
    for (const std::size_t index : listed) {
        const rules::LoadedDocument& loaded = documents.at(index);
        const tdf::Block* info = first_named(loaded.document.root(), "UNITINFO");
        if (info == nullptr)
            continue;
        const std::string_view written = text_of(info, "UnitName");
        UnitRow row;
        row.path = loaded.path;
        row.name = written.empty() ? stem_of(loaded.path) : std::string{written};
        row.info = info;
        units.push_back(std::move(row));
    }
}

void collect_weapons(
    const rules::DocumentSet& documents,
    const defs::DataLayout& layout,
    const IntegrityLimits& limits,
    std::vector<WeaponSlot>& slots,
    std::vector<IntegrityIssue>& issues
) {
    const std::vector<std::size_t> listed =
        files_in(documents, layout, defs::DataDirectory::weapons, ".tdf");
    for (const std::size_t index : listed) {
        const rules::LoadedDocument& loaded = documents.at(index);
        const tdf::Block* root = loaded.document.root();
        const uint32_t count = tdf::child_count(root);
        for (uint32_t child = 0; child < count; ++child) {
            const tdf::Block* section = tdf::child_at(root, child);
            if (section == nullptr || section->name == nullptr || section->name[0] == '\0')
                continue;
            const char* written_id = value_of(section, "ID");
            if (written_id == nullptr || written_id[0] == '\0') {
                std::string sentence = section->name;
                sentence += " has no ID";
                add_issue(
                    issues,
                    Problem::weapon_id_out_of_range,
                    limits,
                    section->name,
                    "id",
                    {},
                    loaded.path,
                    std::move(sentence)
                );
                continue;
            }
            const int32_t id = tdf::get_int(section, "ID", -1);
            if (id < 0 || static_cast<uint32_t>(id) >= limits.weapon_types) {
                const std::string id_text = std::to_string(id);
                std::string sentence = section->name;
                sentence += "'s ID is ";
                sentence += id_text;
                sentence += ", which is outside 0 to ";
                sentence += std::to_string(limits.weapon_types - 1);
                add_issue(
                    issues,
                    Problem::weapon_id_out_of_range,
                    limits,
                    section->name,
                    "id",
                    id_text,
                    loaded.path,
                    std::move(sentence)
                );
                continue;
            }
            WeaponSlot& slot = slots[static_cast<std::size_t>(id)];
            slot.held = true;
            slot.name = section->name;
        }
    }
}

void collect_features(const rules::DocumentSet& documents, std::vector<std::string>& features) {
    const std::vector<std::size_t> listed = documents.documents_in(k_features_folder, ".tdf", true);
    for (const std::size_t index : listed) {
        const tdf::Block* root = documents.at(index).document.root();
        const uint32_t count = tdf::child_count(root);
        for (uint32_t child = 0; child < count; ++child) {
            const tdf::Block* section = tdf::child_at(root, child);
            if (section == nullptr || section->name == nullptr || section->name[0] == '\0')
                continue;
            if (!feature_named(features, section->name))
                features.emplace_back(section->name);
        }
    }
}

void read_move_classes(
    const rules::DocumentSet& documents,
    const defs::DataLayout& layout,
    const IntegrityLimits& limits,
    std::array<std::string, OA_MOVE_CLASS_COUNT>& names,
    std::array<uint8_t, OA_MOVE_CLASS_COUNT>& seen,
    std::vector<IntegrityIssue>& issues
) {
    const std::string_view folder = directory_of(layout, defs::DataDirectory::gamedata);
    const std::vector<std::size_t> listed = documents.documents_in(folder, ".tdf", true);
    for (const std::size_t index : listed) {
        const rules::LoadedDocument& loaded = documents.at(index);
        if (!file_named(loaded.path, "moveinfo.tdf"))
            continue;
        const tdf::Block* root = loaded.document.root();
        const uint32_t count = tdf::child_count(root);
        for (uint32_t child = 0; child < count; ++child) {
            const tdf::Block* section = tdf::child_at(root, child);
            if (section == nullptr || section->name == nullptr)
                continue;
            const std::optional<uint32_t> slot = numbered(section->name, "CLASS");
            if (!slot.has_value())
                continue;
            if (*slot >= OA_MOVE_CLASS_COUNT) {
                std::string sentence = section->name;
                sentence += " is past the ";
                sentence += std::to_string(OA_MOVE_CLASS_COUNT);
                sentence += " movement classes the game reads";
                add_issue(
                    issues,
                    Problem::too_many_movement_classes,
                    limits,
                    section->name,
                    {},
                    section->name,
                    loaded.path,
                    std::move(sentence)
                );
                continue;
            }
            const std::size_t place = static_cast<std::size_t>(*slot);
            if (seen[place] != 0)
                continue;
            seen[place] = 1;
            const std::string_view name = text_of(section, "Name");
            if (!name.empty())
                names[place] = std::string{name};
        }
    }
}

bool class_named(
    const std::array<std::string, OA_MOVE_CLASS_COUNT>& names,
    const std::array<uint8_t, OA_MOVE_CLASS_COUNT>& seen,
    std::string_view name
) {
    for (std::size_t index = 0; index < OA_MOVE_CLASS_COUNT; ++index) {
        if (seen[index] != 0 && same_text(names[index], name))
            return true;
    }
    return false;
}

void check_units(
    const std::vector<UnitRow>& units,
    const std::vector<WeaponSlot>& weapons,
    const std::vector<std::string>& features,
    const std::array<std::string, OA_MOVE_CLASS_COUNT>& classes,
    const std::array<uint8_t, OA_MOVE_CLASS_COUNT>& class_seen,
    const IntegrityLimits& limits,
    const std::function<bool(std::string_view)>& store_holds,
    std::vector<IntegrityIssue>& issues
) {
    struct NameUse {
        std::string name;
        std::string path;
    };

    std::vector<NameUse> uses;
    uses.reserve(units.size());
    for (const UnitRow& unit : units) {
        uses.push_back(NameUse{unit.name, unit.path});
        const std::string_view written = text_of(unit.info, "UnitName");
        if (!written.empty() && !same_text(written, stem_of(unit.path))) {
            const std::string stem = stem_of(unit.path);
            std::string sentence = unit.path;
            sentence += "'s file name is not its UnitName ";
            sentence.append(written);
            add_issue(
                issues,
                Problem::file_name_differs,
                limits,
                std::string{written},
                "unitname",
                stem,
                unit.path,
                std::move(sentence)
            );
        }

        const auto weapon_field = [&](const char* field, Problem problem, const char* kind) {
            const std::string_view target = text_of(unit.info, field);
            if (target.empty() || weapon_named(weapons, target))
                return;
            add_issue(
                issues,
                problem,
                limits,
                unit.name,
                fold_copy(field),
                std::string{target},
                unit.path,
                names_sentence(unit.name, fold_copy(field), target, kind)
            );
        };
        weapon_field("weapon1", Problem::weapon_missing, "weapon");
        weapon_field("weapon2", Problem::weapon_missing, "weapon");
        weapon_field("weapon3", Problem::weapon_missing, "weapon");
        weapon_field("explodeas", Problem::explosion_missing, "explosion");
        weapon_field("selfdestructas", Problem::explosion_missing, "explosion");

        const std::string_view corpse = text_of(unit.info, "corpse");
        if (!corpse.empty() && !feature_named(features, corpse)) {
            add_issue(
                issues,
                Problem::corpse_missing,
                limits,
                unit.name,
                "corpse",
                std::string{corpse},
                unit.path,
                names_sentence(unit.name, "corpse", corpse, "feature")
            );
        }
        const std::string_view move = text_of(unit.info, "movementclass");
        if (!move.empty() && !class_named(classes, class_seen, move)) {
            add_issue(
                issues,
                Problem::movement_class_missing,
                limits,
                unit.name,
                "movementclass",
                std::string{move},
                unit.path,
                names_sentence(unit.name, "movementclass", move, "movement class")
            );
        }

        const std::string_view object_field = text_of(unit.info, "objectname");
        const std::string object = object_field.empty() ? unit.name : std::string{object_field};
        const std::string model_path = "objects3d/" + object + ".3DO";
        if (object.empty() || !store_holds(model_path)) {
            add_issue(
                issues,
                Problem::model_missing,
                limits,
                unit.name,
                "objectname",
                object,
                model_path,
                names_sentence(unit.name, "objectname", object, "model")
            );
        }
        const std::string script_path = "scripts/" + unit.name + ".COB";
        if (!store_holds(script_path)) {
            std::string sentence = unit.name;
            sentence += " has no script";
            add_issue(
                issues,
                Problem::script_missing,
                limits,
                unit.name,
                {},
                unit.name,
                script_path,
                std::move(sentence)
            );
        }
    }

    // One pass over indices sorted by folded name, then document order. Each
    // later file of a repeated name is reported, still in document order.
    std::vector<std::size_t> order(uses.size());
    for (std::size_t index = 0; index < order.size(); ++index)
        order[index] = index;
    std::sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        const int compared = compare_folded(uses[left].name, uses[right].name);
        if (compared != 0)
            return compared < 0;
        return left < right;
    });
    std::vector<uint8_t> repeated(uses.size(), 0);
    for (std::size_t place = 1; place < order.size(); ++place) {
        const std::size_t earlier = order[place - 1];
        const std::size_t later = order[place];
        if (same_text(uses[earlier].name, uses[later].name))
            repeated[later] = 1;
    }
    for (std::size_t index = 0; index < uses.size(); ++index) {
        if (repeated[index] == 0)
            continue;
        std::string sentence = uses[index].name;
        sentence += " is the UnitName of two unit files";
        add_issue(
            issues,
            Problem::unit_name_twice,
            limits,
            uses[index].name,
            "unitname",
            uses[index].path,
            uses[index].path,
            std::move(sentence)
        );
    }
}

void check_sides(
    const rules::DocumentSet& documents,
    const defs::DataLayout& layout,
    const std::vector<UnitRow>& units,
    const IntegrityLimits& limits,
    std::vector<IntegrityIssue>& issues
) {
    const std::string_view folder = directory_of(layout, defs::DataDirectory::gamedata);
    const std::vector<std::size_t> listed = documents.documents_in(folder, ".tdf", true);
    for (const std::size_t index : listed) {
        const rules::LoadedDocument& loaded = documents.at(index);
        if (!file_named(loaded.path, "sidedata.tdf"))
            continue;
        const tdf::Block* root = loaded.document.root();
        const uint32_t count = tdf::child_count(root);
        for (uint32_t child = 0; child < count; ++child) {
            const tdf::Block* section = tdf::child_at(root, child);
            if (section == nullptr || section->name == nullptr)
                continue;
            const std::optional<uint32_t> slot = numbered(section->name, "SIDE");
            if (!slot.has_value() || *slot < OA_SIDE_COUNT)
                continue;
            std::string sentence = section->name;
            sentence += " is past the ";
            sentence += std::to_string(OA_SIDE_COUNT);
            sentence += " sides the game reads";
            add_issue(
                issues,
                Problem::too_many_sides,
                limits,
                section->name,
                {},
                section->name,
                loaded.path,
                std::move(sentence)
            );
        }
        for (uint32_t slot = 0; slot < OA_SIDE_COUNT; ++slot) {
            const std::string section_name = "SIDE" + std::to_string(slot);
            const tdf::Block* section = first_named(root, section_name.c_str());
            if (section == nullptr)
                break;
            const std::string_view commander = text_of(section, "commander");
            if (commander.empty()) {
                std::string sentence = section_name;
                sentence += " has no commander";
                add_issue(
                    issues,
                    Problem::side_without_commander,
                    limits,
                    section_name,
                    "commander",
                    {},
                    loaded.path,
                    std::move(sentence)
                );
                continue;
            }
            if (unit_named(units, commander))
                continue;
            add_issue(
                issues,
                Problem::side_without_commander,
                limits,
                section_name,
                "commander",
                std::string{commander},
                loaded.path,
                names_sentence(section_name, "commander", commander, "unit")
            );
        }
    }
}

void check_download(
    const rules::DocumentSet& documents,
    const defs::DataLayout& layout,
    const std::vector<UnitRow>& units,
    const IntegrityLimits& limits,
    std::vector<IntegrityIssue>& issues
) {
    const std::vector<std::size_t> listed =
        files_in(documents, layout, defs::DataDirectory::download, ".tdf");
    if (listed.size() > defs::max_download_files) {
        std::string sentence = "the match has ";
        sentence += std::to_string(listed.size());
        sentence += " download files, past the ";
        sentence += std::to_string(defs::max_download_files);
        sentence += " the game reads";
        add_issue(
            issues, Problem::too_many_download_files, limits, {}, {}, {}, {}, std::move(sentence)
        );
    }
    for (const std::size_t index : listed) {
        const rules::LoadedDocument& loaded = documents.at(index);
        const tdf::Block* root = loaded.document.root();
        const uint32_t count = tdf::child_count(root);
        const uint32_t kept =
            count < defs::download_menu_entries ? count : defs::download_menu_entries;
        for (uint32_t child = 0; child < kept; ++child) {
            const tdf::Block* section = tdf::child_at(root, child);
            if (section == nullptr || section->name == nullptr)
                continue;
            const auto menu_name = [&](const char* field) {
                const std::string_view target = text_of(section, field);
                if (target.empty() || unit_named(units, target))
                    return;
                std::string sentence = loaded.path;
                sentence += ' ';
                sentence += names_sentence(section->name, fold_copy(field), target, "unit");
                add_issue(
                    issues,
                    Problem::menu_names_missing_unit,
                    limits,
                    section->name,
                    fold_copy(field),
                    std::string{target},
                    loaded.path,
                    std::move(sentence)
                );
            };
            // An empty UNITMENU is a placeholder and names nobody.
            menu_name("UNITMENU");
            menu_name("UNITNAME");
        }
    }
}

void check_pages(
    const rules::DocumentSet& documents,
    const defs::DataLayout& layout,
    const IntegrityLimits& limits,
    const std::function<bool(std::string_view)>& store_holds,
    std::vector<IntegrityIssue>& issues
) {
    const std::vector<std::size_t> listed =
        files_in(documents, layout, defs::DataDirectory::guis, ".gui");
    for (const std::size_t index : listed) {
        if (!documents.changed(index))
            continue;
        const rules::LoadedDocument& loaded = documents.at(index);
        if (loaded.removed || store_holds(loaded.path))
            continue;
        std::string sentence = loaded.path;
        sentence += " is not a page the game holds";
        add_issue(
            issues,
            Problem::gui_edit_names_missing_page,
            limits,
            stem_of(loaded.path),
            {},
            loaded.path,
            loaded.path,
            std::move(sentence)
        );
    }
}

void check_counts(
    std::size_t unit_count, const IntegrityLimits& limits, std::vector<IntegrityIssue>& issues
) {
    if (unit_count > static_cast<std::size_t>(defs::max_unit_defs - 1u)) {
        std::string sentence = "the match has ";
        sentence += std::to_string(unit_count);
        sentence += " unit files, past the ";
        sentence += std::to_string(defs::max_unit_defs - 1u);
        sentence += " the game can load";
        add_issue(
            issues, Problem::too_many_unit_types, limits, {}, {}, {}, {}, std::move(sentence)
        );
    }
    // Record 0 of the unit table is reserved, so N files use type ids 1 to N.
    // The masks hold ids 0 to types-1, and the last file already loses its
    // categories when N equals the mask width.
    if (unit_count >= limits.category_mask_types) {
        std::string sentence = "the match has ";
        sentence += std::to_string(unit_count);
        sentence += " unit files, past the ";
        sentence += std::to_string(limits.category_mask_types);
        sentence += " category masks";
        add_issue(
            issues, Problem::past_category_masks, limits, {}, {}, {}, {}, std::move(sentence)
        );
    }
    if (unit_count > unit_sync_limit) {
        std::string sentence = "the match has ";
        sentence += std::to_string(unit_count);
        sentence += " unit files, past the ";
        sentence += std::to_string(unit_sync_limit);
        if (limits.shared)
            sentence += " a shared game can keep in step";
        else
            sentence += " a shared game keeps in step";
        add_issue(issues, Problem::past_unit_sync, limits, {}, {}, {}, {}, std::move(sentence));
    }
}

/// Reference problems the base data already has.
///
/// A weapon, explosion, corpse, movement class or menu unit that nothing
/// holds is the base data's own error when the base already reports the same
/// problem, field and target on any record. The game already plays with that
/// missing name, so the same missing name on another record is not new and
/// blames no mutator. Every other problem still matches the record as well.
bool reference_name(Problem problem) {
    return problem == Problem::weapon_missing || problem == Problem::explosion_missing ||
           problem == Problem::corpse_missing || problem == Problem::movement_class_missing ||
           problem == Problem::menu_names_missing_unit;
}

bool base_has(std::span<const IntegrityIssue> base, const IntegrityIssue& issue) {
    for (const IntegrityIssue& earlier : base) {
        if (earlier.problem != issue.problem)
            continue;
        if (!same_text(earlier.field, issue.field) || !same_text(earlier.target, issue.target))
            continue;
        if (reference_name(issue.problem) || same_text(earlier.record, issue.record))
            return true;
    }
    return false;
}

bool field_write(const JournalEntry& entry, const IntegrityIssue& issue) {
    return entry.change == Change::set && !issue.field.empty() && !issue.record.empty() &&
           same_text(entry.field, issue.field) && same_text(entry.record, issue.record);
}

bool menu_add(const JournalEntry& entry, const IntegrityIssue& issue) {
    return issue.problem == Problem::menu_names_missing_unit && entry.change == Change::added &&
           entry.collection == Collection::menus && !issue.record.empty() &&
           same_text(entry.record, issue.record);
}

bool removed_target(const JournalEntry& entry, const IntegrityIssue& issue) {
    return entry.change == Change::removed && !issue.target.empty() &&
           same_text(entry.record, issue.target);
}

bool renamed_target(const JournalEntry& entry, const IntegrityIssue& issue) {
    if (issue.target.empty() || !entry.old_text.has_value() ||
        !same_text(*entry.old_text, issue.target))
        return false;
    if (entry.change == Change::added)
        return true;
    return entry.change == Change::set &&
           (same_text(entry.field, "unitname") || same_text(entry.field, "name"));
}

bool renumbered_target(const JournalEntry& entry, const IntegrityIssue& issue) {
    return entry.change == Change::set && same_text(entry.field, "id") && !issue.target.empty() &&
           same_text(entry.record, issue.target);
}

bool hid_path(const JournalEntry& entry, const IntegrityIssue& issue) {
    return entry.change == Change::hidden && !issue.path.empty() &&
           same_path(entry.path, issue.path);
}

std::optional<uint16_t> last_named(const JournalEntry& entry) {
    if (entry.mutators.empty())
        return std::nullopt;
    return entry.mutators.back();
}

std::optional<uint16_t> highest_named(const JournalEntry& entry) {
    std::optional<uint16_t> best;
    for (const uint16_t mutator : entry.mutators) {
        if (!best.has_value() || mutator > *best)
            best = mutator;
    }
    return best;
}

void take_higher(std::optional<uint16_t>& best, std::optional<uint16_t> candidate) {
    if (!candidate.has_value())
        return;
    if (!best.has_value() || *candidate > *best)
        best = candidate;
}

bool download_path(const defs::DataLayout& layout, std::string_view path) {
    const std::string folder = fold_copy(directory_of(layout, defs::DataDirectory::download));
    const std::string folded = fold_copy(path);
    return folded == folder || folded.rfind(folder + "/", 0) == 0;
}

std::optional<uint16_t> blame_count(
    Problem problem, const std::vector<JournalEntry>& entries, const defs::DataLayout& layout
) {
    const bool download = problem == Problem::too_many_download_files;
    std::optional<uint16_t> added;
    std::optional<uint16_t> any;
    for (const JournalEntry& entry : entries) {
        take_higher(any, highest_named(entry));
        if (entry.change != Change::added)
            continue;
        const bool relevant =
            download ? (entry.collection == Collection::menus || download_path(layout, entry.path))
                     : entry.collection == Collection::units;
        if (relevant)
            take_higher(added, highest_named(entry));
    }
    if (added.has_value())
        return added;
    return any;
}

std::optional<uint16_t> blame(
    const IntegrityIssue& issue,
    const std::vector<JournalEntry>& entries,
    const defs::DataLayout& layout
) {
    if (count_problem(issue.problem))
        return blame_count(issue.problem, entries, layout);
    std::optional<uint16_t> field;
    for (const JournalEntry& entry : entries) {
        if (field_write(entry, issue) || menu_add(entry, issue))
            field = last_named(entry);
    }
    if (field.has_value())
        return field;
    std::optional<uint16_t> target;
    for (const JournalEntry& entry : entries) {
        if (removed_target(entry, issue) || renamed_target(entry, issue) ||
            renumbered_target(entry, issue) || hid_path(entry, issue))
            target = last_named(entry);
    }
    if (target.has_value())
        return target;
    std::optional<uint16_t> record;
    for (const JournalEntry& entry : entries) {
        if (!issue.record.empty() && same_text(entry.record, issue.record))
            take_higher(record, highest_named(entry));
    }
    return record;
}

} // namespace

std::vector<IntegrityIssue> integrity_issues(
    const rules::DocumentSet& documents,
    const defs::DataLayout& layout,
    const IntegrityLimits& limits,
    const std::function<bool(std::string_view)>& store_holds
) {
    std::vector<IntegrityIssue> issues;
    std::vector<UnitRow> units;
    collect_units(documents, layout, units);
    std::vector<WeaponSlot> weapons(static_cast<std::size_t>(limits.weapon_types));
    collect_weapons(documents, layout, limits, weapons, issues);
    std::vector<std::string> features;
    collect_features(documents, features);
    std::array<std::string, OA_MOVE_CLASS_COUNT> classes{};
    std::array<uint8_t, OA_MOVE_CLASS_COUNT> class_seen{};
    read_move_classes(documents, layout, limits, classes, class_seen, issues);
    check_units(units, weapons, features, classes, class_seen, limits, store_holds, issues);
    check_sides(documents, layout, units, limits, issues);
    check_download(documents, layout, units, limits, issues);
    check_pages(documents, layout, limits, store_holds, issues);
    const std::string unit_suffix = "." + layout.unit_extension;
    const std::size_t unit_files =
        files_in(documents, layout, defs::DataDirectory::units, unit_suffix).size();
    check_counts(unit_files, limits, issues);
    return issues;
}

std::vector<IntegrityIssue> new_issues(
    std::span<const IntegrityIssue> base,
    std::span<const IntegrityIssue> result,
    const Journal& journal,
    const defs::DataLayout& layout
) {
    Journal ordered = journal;
    ordered.order();
    std::vector<IntegrityIssue> fresh;
    fresh.reserve(result.size());
    for (const IntegrityIssue& issue : result) {
        if (base_has(base, issue))
            continue;
        IntegrityIssue kept = issue;
        kept.mutator = blame(kept, ordered.entries, layout);
        fresh.push_back(std::move(kept));
    }
    return fresh;
}

} // namespace oa::data::mutators

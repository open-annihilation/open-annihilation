// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Numbers the weapons a mutator run brought. Base and mod weapons keep their
// ids. Each mutator, in list order, keeps the requested ids that are still
// free, then takes free ids for its other weapons. Nothing is written when
// one weapon cannot be numbered.
#include "oa/data/mutators/weapon_ids.hpp"

#include "oa/formats/tdf.hpp"
#include "oa/formats/tdf/edit.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::data::mutators {
namespace {

namespace rules = oa::data::mutation_rules;
namespace tdf = oa::formats::tdf;
namespace defs = oa::data::defs;

/// Folds one ASCII letter. Other bytes are copied.
char fold_ascii(unsigned char byte) {
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<char>(byte - 'A' + 'a');
    return static_cast<char>(byte);
}

/// Copies a name folded to lower case.
std::string fold_copy(std::string_view text) {
    std::string folded;
    folded.resize(text.size());
    for (std::size_t offset = 0; offset < text.size(); ++offset)
        folded[offset] = fold_ascii(static_cast<unsigned char>(text[offset]));
    return folded;
}

/// Reports whether two names match without regard to ASCII case.
bool same_text(std::string_view left, std::string_view right) {
    if (left.size() != right.size())
        return false;
    for (std::size_t offset = 0; offset < left.size(); ++offset) {
        if (fold_ascii(static_cast<unsigned char>(left[offset])) !=
            fold_ascii(static_cast<unsigned char>(right[offset])))
            return false;
    }
    return true;
}

/// The id a section asks for, when it is from 1 to `weapon_types` - 1.
///
/// A missing field, 0, a value past `weapon_types` - 1 and a value that does
/// not start with a number are not requests. A value such as "5abc" asks for
/// 5, which is how the weapon loader reads it.
bool requested_id(const tdf::Block* section, uint32_t weapon_types, int& request) {
    const char* text = section != nullptr ? tdf::find_value(section, "ID") : nullptr;
    if (text == nullptr)
        return false;
    const int32_t parsed = tdf::parse_int(text);
    if (parsed < 1 || static_cast<uint32_t>(parsed) >= weapon_types)
        return false;
    request = static_cast<int>(parsed);
    return true;
}

/// One brought weapon located in the document set.
struct Placed {
    /// Index into the brought span.
    std::size_t index{};
    /// The document, or null when the section is gone.
    rules::LoadedDocument* loaded{};
    /// The section, or null when the weapon is gone.
    tdf::Block* section{};
    /// True when the section's ID= is a request from 1 to weapon_types - 1.
    bool has_request{};
    /// The requested id, when has_request is set.
    int request{};
};

/// The first weapon section of this name, in document order.
Placed place_weapon(
    rules::DocumentSet& documents,
    const defs::DataLayout& layout,
    std::size_t index,
    const BroughtWeapon& weapon,
    uint32_t weapon_types
) {
    Placed placed;
    placed.index = index;
    const std::size_t weapons = static_cast<std::size_t>(defs::DataDirectory::weapons);
    const std::vector<std::size_t> docs =
        documents.documents_in(layout.directories[weapons], ".tdf", false);
    const auto take = [&](std::size_t doc_index) {
        if (placed.section != nullptr)
            return;
        rules::LoadedDocument& loaded = documents.at(doc_index);
        tdf::Block* root = tdf::editable_root(loaded.document.get());
        if (root == nullptr)
            return;
        tdf::Block* section = tdf::editable_child(root, weapon.name);
        if (section == nullptr)
            return;
        placed.loaded = &loaded;
        placed.section = section;
        placed.has_request = requested_id(section, weapon_types, placed.request);
    };
    for (const std::size_t doc_index : docs) {
        if (same_text(documents.at(doc_index).path, weapon.path))
            take(doc_index);
    }
    for (const std::size_t doc_index : docs)
        take(doc_index);
    return placed;
}

/// True when `section` is one of the brought weapons already placed.
bool is_brought(const std::vector<Placed>& placed, const tdf::Block* section) {
    if (section == nullptr)
        return false;
    for (const Placed& one : placed) {
        if (one.section == section)
            return true;
    }
    return false;
}

/// Marks ids 1 to `weapon_types` - 1 that a base or mod weapon already holds.
void occupy_base(
    const rules::DocumentSet& documents,
    const defs::DataLayout& layout,
    const std::vector<Placed>& placed,
    uint32_t weapon_types,
    std::vector<uint8_t>& used
) {
    const std::size_t weapons = static_cast<std::size_t>(defs::DataDirectory::weapons);
    const std::vector<std::size_t> docs =
        documents.documents_in(layout.directories[weapons], ".tdf", false);
    for (const std::size_t doc_index : docs) {
        const rules::LoadedDocument& loaded = documents.at(doc_index);
        const tdf::Block* root = loaded.document.get()->root;
        if (root == nullptr)
            continue;
        const uint32_t sections = tdf::child_count(root);
        for (uint32_t section_index = 0; section_index < sections; ++section_index) {
            const tdf::Block* section = tdf::child_at(root, section_index);
            if (is_brought(placed, section))
                continue;
            int request = 0;
            if (!requested_id(section, weapon_types, request))
                continue;
            used[static_cast<std::size_t>(request)] = 1;
        }
    }
}

/// Writes the assigned id onto the section when the text is not already it.
bool write_id(Placed& placed, int id) {
    if (placed.loaded == nullptr || placed.section == nullptr)
        return false;
    const std::string text = std::to_string(id);
    const char* current = tdf::find_value(placed.section, "ID");
    if (current != nullptr && std::string_view{current} == text)
        return true;
    const tdf::EditStatus status =
        tdf::set_value(placed.loaded->document.get(), placed.section, "ID", text);
    return status == tdf::EditStatus::ok;
}

} // namespace

bool assign_weapon_ids(
    rules::DocumentSet& documents,
    const defs::DataLayout& layout,
    std::span<BroughtWeapon> brought,
    WeaponIdResult& result,
    uint32_t weapon_types
) {
    result = WeaponIdResult{};
    for (BroughtWeapon& weapon : brought)
        weapon.assigned_id = 0;
    if (brought.empty())
        return true;

    std::vector<Placed> placed;
    placed.reserve(brought.size());
    for (std::size_t index = 0; index < brought.size(); ++index)
        placed.push_back(place_weapon(documents, layout, index, brought[index], weapon_types));

    std::vector<uint8_t> used(static_cast<std::size_t>(weapon_types), 0);
    occupy_base(documents, layout, placed, weapon_types, used);

    std::vector<std::size_t> order(placed.size());
    for (std::size_t index = 0; index < order.size(); ++index)
        order[index] = index;
    std::sort(order.begin(), order.end(), [&](std::size_t left, std::size_t right) {
        const BroughtWeapon& before = brought[placed[left].index];
        const BroughtWeapon& after = brought[placed[right].index];
        if (before.mutator != after.mutator)
            return before.mutator < after.mutator;
        const std::string before_folded = fold_copy(before.name);
        const std::string after_folded = fold_copy(after.name);
        if (before_folded != after_folded)
            return before_folded < after_folded;
        return before.name < after.name;
    });

    // 0 means the weapon still needs an id. -1 means its section is gone.
    std::vector<int> chosen(placed.size(), 0);
    for (std::size_t index = 0; index < placed.size(); ++index) {
        if (placed[index].section == nullptr)
            chosen[index] = -1;
    }
    // One mutator at a time. Its free requests come before its other weapons,
    // and before the next mutator's requests.
    std::size_t group = 0;
    while (group < order.size()) {
        const uint16_t mutator = brought[placed[order[group]].index].mutator;
        std::size_t group_end = group + 1;
        while (group_end < order.size() &&
               brought[placed[order[group_end]].index].mutator == mutator)
            ++group_end;
        for (std::size_t place = group; place < group_end; ++place) {
            const std::size_t index = order[place];
            if (chosen[index] != 0 || !placed[index].has_request)
                continue;
            const int request = placed[index].request;
            if (used[static_cast<std::size_t>(request)] != 0)
                continue;
            chosen[index] = request;
            used[static_cast<std::size_t>(request)] = 1;
        }
        for (std::size_t place = group; place < group_end; ++place) {
            const std::size_t index = order[place];
            if (chosen[index] != 0)
                continue;
            int free_id = 0;
            for (uint32_t candidate = 1; candidate < weapon_types; ++candidate) {
                if (used[static_cast<std::size_t>(candidate)] == 0) {
                    free_id = static_cast<int>(candidate);
                    break;
                }
            }
            if (free_id == 0) {
                const BroughtWeapon& weapon = brought[placed[index].index];
                result.ok = false;
                result.mutator = weapon.mutator;
                result.weapon = weapon.name;
                return false;
            }
            chosen[index] = free_id;
            used[static_cast<std::size_t>(free_id)] = 1;
        }
        group = group_end;
    }

    for (std::size_t index = 0; index < placed.size(); ++index) {
        if (chosen[index] <= 0)
            continue;
        if (!write_id(placed[index], chosen[index])) {
            const BroughtWeapon& weapon = brought[placed[index].index];
            result.ok = false;
            result.mutator = weapon.mutator;
            result.weapon = weapon.name;
            return false;
        }
        brought[placed[index].index].assigned_id = chosen[index];
    }
    return true;
}

} // namespace oa::data::mutators

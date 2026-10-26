// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "tag_actions.hpp"

#include "oa/ui/key_bindings/tags.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {
namespace {

/// Folds one ASCII letter. Any other byte is unchanged.
///
/// @param byte the byte
/// @return the folded byte
unsigned char fold_ascii(unsigned char byte) {
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<unsigned char>(byte - 'A' + 'a');
    return byte;
}

/// True when the name is ALL, ignoring ASCII case.
///
/// @param name the category name
/// @return true for ALL
bool is_all(const char* name) {
    constexpr char all[] = {'a', 'l', 'l'};
    for (const char want : all) {
        if (name == nullptr || *name == '\0')
            return false;
        if (fold_ascii(static_cast<unsigned char>(*name)) != static_cast<unsigned char>(want))
            return false;
        ++name;
    }
    return name != nullptr && *name == '\0';
}

/// True when the mask holds a unit type.
///
/// @param mask the mask
/// @return true when some type bit is set
bool mask_holds_a_type(const oa::data::defs::CategoryMask& mask) {
    if (mask.words == nullptr)
        return false;
    for (uint32_t index = 0; index < mask.word_count; ++index) {
        if (mask.words[index] != 0)
            return true;
    }
    return false;
}

/// True when the action was already given to an earlier word.
///
/// @param seen action ids already kept
/// @param action the id to test
/// @return true when an earlier word has it
bool already_kept(const std::vector<std::string>& seen, std::string_view action) {
    for (const std::string& id : seen) {
        if (id == action)
            return true;
    }
    return false;
}

} // namespace

std::vector<TagWord>
tag_words(const oa::data::defs::CategoryRegistry& registry, std::vector<std::string>& skipped) {
    std::vector<TagWord> words;
    if (registry.entries == nullptr || registry.masks == nullptr)
        return words;
    std::vector<std::string> seen;
    for (uint32_t index = 0; index < registry.count; ++index) {
        const char* name = registry.entries[index].name;
        if (name == nullptr || name[0] == '\0' || is_all(name))
            continue;
        const uint32_t mask_index = registry.entries[index].mask;
        if (mask_index >= registry.count || !mask_holds_a_type(registry.masks[mask_index]))
            continue;
        const std::string word(name);
        const std::string action = oa::ui::key_bindings::tag_action_id(word);
        if (action.empty() || already_kept(seen, action)) {
            skipped.push_back(word);
            continue;
        }
        seen.push_back(action);
        words.push_back(TagWord{word, action});
    }
    return words;
}

} // namespace oa::app

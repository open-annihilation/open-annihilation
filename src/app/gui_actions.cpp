// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A gadget's made gui action, and the registry action whose gadget word the
// name holds.
#include "gui_actions.hpp"

#include "oa/data/input_actions/actions.hpp"

#include <string>
#include <string_view>

namespace oa::app {
namespace {

/// Folds an ASCII letter. Any other byte is unchanged.
///
/// @param byte the byte
/// @return the folded byte
unsigned char fold_byte(unsigned char byte) {
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<unsigned char>(byte - 'A' + 'a');
    return byte;
}

/// Reports whether the name holds the word, ignoring ASCII case.
///
/// @param name the gadget name
/// @param word the registry word
/// @return true when the word is a substring
bool holds_word(std::string_view name, std::string_view word) {
    if (word.empty() || word.size() > name.size())
        return false;
    for (std::size_t start = 0; start + word.size() <= name.size(); ++start) {
        bool match = true;
        for (std::size_t offset = 0; offset < word.size(); ++offset) {
            const auto hay = static_cast<unsigned char>(name[start + offset]);
            const auto needle = static_cast<unsigned char>(word[offset]);
            if (fold_byte(hay) != fold_byte(needle)) {
                match = false;
                break;
            }
        }
        if (match)
            return true;
    }
    return false;
}

/// The registry action whose gadget word is exactly `word`.
///
/// @param word the gadget word, in the registry's spelling
/// @return that action, or null
const oa::data::input_actions::Action* action_for_word(std::string_view word) {
    for (const oa::data::input_actions::Action& action : oa::data::input_actions::actions()) {
        if (action.gadget.size() == word.size() && holds_word(action.gadget, word))
            return &action;
    }
    return nullptr;
}

// The order a button's name is tried. The toggles come first, as MOVEORD
// holds MOVE; UNLOAD comes before LOAD, which it holds; the tabs are last,
// ORDERS then BUILD.
constexpr std::string_view kTrialOrder[] = {
    "FIREORD",
    "MOVEORD",
    "ONOFF",
    "CLOAK",
    "MOVE",
    "STOP",
    "ATTACK",
    "BLAST",
    "DEFEND",
    "REPAIR",
    "PATROL",
    "RECLAIM",
    "CAPTURE",
    "UNLOAD",
    "LOAD",
    "ORDERS",
    "BUILD",
};

} // namespace

std::string gui_key_piece(std::string_view text) {
    std::string piece;
    piece.reserve(text.size());
    for (const char byte : text) {
        const auto value = static_cast<unsigned char>(byte);
        if (value == ' ' || value == '_')
            piece.push_back('-');
        else
            piece.push_back(static_cast<char>(fold_byte(value)));
    }
    return piece;
}

std::string gui_action_id(std::string_view stem, std::string_view name) {
    std::string id = "gui.";
    id += gui_key_piece(stem);
    id += '.';
    id += gui_key_piece(name);
    return id;
}

std::string_view order_action_id(std::string_view name) {
    if (name.empty())
        return {};
    for (const std::string_view word : kTrialOrder) {
        if (!holds_word(name, word))
            continue;
        if (const oa::data::input_actions::Action* action = action_for_word(word))
            return action->id;
    }
    return {};
}

} // namespace oa::app

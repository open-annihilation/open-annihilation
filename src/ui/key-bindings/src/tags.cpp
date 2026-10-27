// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/ui/key_bindings/tags.hpp"

#include "oa/data/input_actions/actions.hpp"
#include "oa/ui/key_bindings/layout.hpp"

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

namespace oa::ui::key_bindings {
namespace {

namespace actions = oa::data::input_actions;

/// The longest suffix `select.tag-` can take and still fit an action id.
constexpr std::size_t k_suffix_limit = 85;

/// A source that is part of the game even with every hack off.
///
/// @param source one source name
/// @return true for 3.1c, OA or data
bool permanent_source(std::string_view source) {
    return source == "3.1c" || source == "OA" || source == "data";
}

/// Reports whether the action has a 3.1c, OA or data source.
///
/// @param action the action
/// @return true when it exists with every hack off
bool any_permanent(const actions::Action& action) {
    for (const std::string_view source : action.source) {
        if (permanent_source(source))
            return true;
    }
    return false;
}

/// The bit of a context, or 0 when the name is not one.
///
/// @param name the context id
/// @return the bit
uint32_t context_bit(std::string_view name) {
    const std::span<const actions::Context> table = actions::contexts();
    for (std::size_t index = 0; index < table.size(); ++index) {
        if (table[index].name == name)
            return uint32_t{1} << static_cast<uint32_t>(index);
    }
    return 0;
}

/// Folds one ASCII letter. Any other byte is unchanged.
///
/// @param byte the byte
/// @return the folded byte
unsigned char fold_ascii(unsigned char byte) {
    if (byte >= 'A' && byte <= 'Z')
        return static_cast<unsigned char>(byte - 'A' + 'a');
    return byte;
}

/// True when the text is exactly Ctrl and one letter.
///
/// @param text a default chord
/// @param letter the letter, folded
/// @return true when the chord is that one key
bool ctrl_letter_chord(std::string_view text, char& letter) {
    constexpr std::string_view prefix = "Ctrl+";
    if (text.size() != prefix.size() + 1 || !text.starts_with(prefix))
        return false;
    const unsigned char last = fold_ascii(static_cast<unsigned char>(text.back()));
    if (last < 'a' || last > 'z')
        return false;
    letter = static_cast<char>(last);
    return true;
}

/// True when a permanent action binds Ctrl and this letter in a layer play meets.
///
/// @param letter the folded letter
/// @return true when the chord is taken
bool letter_taken(char letter) {
    const uint32_t play = context_bit("play");
    for (const actions::Action& action : actions::actions()) {
        if (!any_permanent(action) || !layers_meet(action.context, play))
            continue;
        for (const std::string_view text : action.defaults) {
            char bound = 0;
            if (ctrl_letter_chord(text, bound) && bound == letter)
                return true;
        }
    }
    return false;
}

/// True when the word is `CTRL_` and one letter, ignoring ASCII case.
///
/// @param word the Category word
/// @param letter the letter, folded
/// @return true when the word is that shape
bool ctrl_word(std::string_view word, char& letter) {
    if (word.size() != 6)
        return false;
    constexpr char prefix[] = {'c', 't', 'r', 'l', '_'};
    for (std::size_t index = 0; index < 5; ++index) {
        if (fold_ascii(static_cast<unsigned char>(word[index])) !=
            static_cast<unsigned char>(prefix[index]))
            return false;
    }
    const unsigned char last = fold_ascii(static_cast<unsigned char>(word[5]));
    if (last < 'a' || last > 'z')
        return false;
    letter = static_cast<char>(last);
    return true;
}

/// The folded word, spaces and underscores as hyphens.
///
/// @param word the Category word
/// @return the piece
std::string folded_piece(std::string_view word) {
    std::string piece;
    piece.reserve(word.size());
    for (const char byte : word) {
        const unsigned char value = static_cast<unsigned char>(byte);
        if (value == ' ' || value == '_')
            piece.push_back('-');
        else
            piece.push_back(static_cast<char>(fold_ascii(value)));
    }
    return piece;
}

/// True when the piece is a tag suffix: kebab, no empty part, at most 85 bytes.
///
/// @param piece the folded word
/// @return true when `select.tag-` plus the piece is an action id
bool tag_piece_ok(std::string_view piece) {
    if (piece.empty() || piece.size() > k_suffix_limit)
        return false;
    if (piece.front() == '-' || piece.back() == '-')
        return false;
    bool hyphen = false;
    for (const char byte : piece) {
        if (byte == '-') {
            if (hyphen)
                return false;
            hyphen = true;
            continue;
        }
        hyphen = false;
        const bool digit = byte >= '0' && byte <= '9';
        const bool lower = byte >= 'a' && byte <= 'z';
        if (!digit && !lower)
            return false;
    }
    return true;
}

} // namespace

std::string tag_action_id(std::string_view word) {
    char letter = 0;
    if (ctrl_word(word, letter)) {
        if (letter == 'b')
            return "select.builders";
        if (letter == 'c')
            return "select.commander";
        if (letter == 'f')
            return "select.factories";
        if (letter == 'v')
            return "select.aircraft";
        if (!letter_taken(letter)) {
            std::string category = "select.category-";
            category.push_back(letter);
            return category;
        }
    }
    const std::string piece = folded_piece(word);
    if (!tag_piece_ok(piece))
        return {};
    std::string id = "select.tag-";
    id += piece;
    return id;
}

} // namespace oa::ui::key_bindings

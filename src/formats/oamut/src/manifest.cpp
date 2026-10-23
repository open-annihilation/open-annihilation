// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/formats/oamut/manifest.hpp"

#include "oa/formats/oamod.hpp"
#include "oa/formats/oamut/shared.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::formats::oamut {

/// Tells whether an id is lower-case kebab-case of 1 to 64 bytes.
///
/// The same rule as a catalogue package key.
///
/// @param text the id
/// @return true when it is such an id
bool mutator_id(std::string_view text) noexcept {
    if (text.empty() || text.size() > most_id_bytes || text.front() == '-' || text.back() == '-')
        return false;
    char previous = 0;
    for (const char character : text) {
        const bool word =
            (character >= 'a' && character <= 'z') || (character >= '0' && character <= '9');
        if (!word && character != '-')
            return false;
        if (character == '-' && previous == '-')
            return false;
        previous = character;
    }
    return true;
}

namespace {

namespace oamod = oa::formats::oamod;

/// The top-level keys a manifest may hold.
constexpr std::array<std::string_view, 18> manifest_keys{
    "oamut",
    "id",
    "name",
    "version",
    "author",
    "description",
    "homepage",
    "icon",
    "tags",
    "works-with",
    "requires",
    "conflicts-with",
    "layers",
    "clashes",
    "options",
    "strings",
    "entry",
    "legacy",
};

/// Keys that belong to a mod profile, never to a mutator.
constexpr std::array<std::string_view, 11> profile_keys{
    "identity",
    "layout",
    "limits",
    "script-extensions",
    "data-keys",
    "hacks",
    "settings",
    "media",
    "packaging",
    "supportedGames",
    "requiredExpansions",
};

/// The clash modes, in enum order.
constexpr std::array<std::string_view, 2> clash_mode_names{"add", "replace"};

/// The keys of the author block.
constexpr std::array<std::string_view, 2> author_keys{"name", "email"};

/// The keys of the requires block.
constexpr std::array<std::string_view, 2> requires_keys{"engine", "mutators"};

/// The keys any option may hold before its type is known.
constexpr std::array<std::string_view, 8> option_keys{
    "type",
    "label",
    "default",
    "values",
    "labels",
    "min",
    "max",
    "step",
};

/// The keys of a toggle.
constexpr std::array<std::string_view, 3> toggle_keys{"type", "label", "default"};

/// The keys of a choice.
constexpr std::array<std::string_view, 5> choice_keys{
    "type", "label", "default", "values", "labels"
};

/// The keys of a number option.
constexpr std::array<std::string_view, 6> number_keys{
    "type", "label", "default", "min", "max", "step"
};

/// The keys of one translation.
constexpr std::array<std::string_view, 4> translation_keys{
    "name", "description", "options", "labels"
};

/// The keys of one legacy record.
constexpr std::array<std::string_view, 4> legacy_keys{"name", "prefix", "compatibility", "files"};

/// The layer names, in Layer order.
constexpr std::array<std::string_view, 9> layer_names{
    "files",
    "units",
    "weapons",
    "features",
    "sides",
    "tables",
    "menus",
    "maps",
    "hacks",
};

/// The most bytes of an author's name, as a mod's author name allows.
constexpr std::size_t most_author_name_bytes = 128;

/// The most bytes of an e-mail address, as a mod's author e-mail allows.
constexpr std::size_t most_email_bytes = 254;

/// The most bytes of an option id.
constexpr std::size_t most_option_id_bytes = 32;

/// The most bytes of a label.
constexpr std::size_t most_label_bytes = 64;

/// The most bytes of one legacy original's name.
constexpr std::size_t most_legacy_name_bytes = 128;

/// The most bytes of one compatibility code.
constexpr std::size_t most_code_bytes = 8;

/// The most file names one original lists.
constexpr std::size_t most_legacy_files = 16;

/// The most bytes of one original's file name.
constexpr std::size_t most_file_name_bytes = 255;

/// The shortest and longest language tag, in bytes.
constexpr std::size_t least_language_bytes = 2;
constexpr std::size_t most_language_bytes = 35;

/// The first code point that is not a C0 control character.
constexpr char32_t first_printable = 0x20;

/// The delete character, and the last C1 control character.
constexpr char32_t delete_character = 0x7F;
constexpr char32_t last_c1_control = 0x9F;

/// The line separator and the paragraph separator.
constexpr char32_t line_separator = 0x2028;
constexpr char32_t paragraph_separator = 0x2029;

/// One limb of an exact decimal, base 10^9.
constexpr uint32_t limb_base = 1000000000u;

/// An unsigned integer large enough for a manifest number scaled to an integer.
struct BigInt {
    std::vector<uint32_t> limbs{}; ///< little-endian; empty means zero
};

/// A manifest number scaled to an integer at a shared power of ten.
struct Scaled {
    bool negative{false}; ///< below zero; zero is never negative
    BigInt magnitude{};   ///< the absolute value
};

/// Appends one problem.
///
/// @param problems the problems collected so far
/// @param position where the value starts
/// @param key the path of the value
/// @param message what is wrong
void add_problem(
    std::vector<Problem>& problems,
    oamod::TextPosition position,
    std::string key,
    std::string message
) {
    problems.push_back(Problem{position.line, position.column, std::move(key), std::move(message)});
}

/// Tells whether a key is one of a list.
///
/// @param key the key
/// @param allowed the keys that may appear
/// @return true when `key` is one of them
template <std::size_t count>
bool listed(std::string_view key, const std::array<std::string_view, count>& allowed) {
    for (const std::string_view known : allowed)
        if (known == key)
            return true;
    return false;
}

/// Joins a block path and a child key.
///
/// @param block the parent path, or empty at the top
/// @param key the child key
/// @return the child path
std::string child_path(std::string_view block, std::string_view key) {
    if (block.empty())
        return std::string{key};
    return std::string{block} + "." + std::string{key};
}

/// Joins a list path and a 0-based index.
///
/// @param block the list path
/// @param index the index
/// @return the item path, such as layers[0]
std::string index_path(std::string_view block, std::size_t index) {
    return std::string{block} + "[" + std::to_string(index) + "]";
}

/// Tells whether a byte is an ASCII letter.
///
/// @param character the byte
/// @return true for A-Z or a-z
bool letter(char character) noexcept {
    return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z');
}

/// Tells whether a byte is a decimal digit.
///
/// @param character the byte
/// @return true for 0-9
bool digit(char character) noexcept {
    return character >= '0' && character <= '9';
}

/// Lowers an ASCII letter.
///
/// @param character a byte
/// @return the byte, A-Z made a-z
char ascii_lower(unsigned char character) noexcept {
    if (character >= 'A' && character <= 'Z')
        return static_cast<char>(character - 'A' + 'a');
    return static_cast<char>(character);
}

/// Tells whether two texts are equal ignoring the case of ASCII letters.
///
/// @param left the first text
/// @param right the second text
/// @return true when they fold to the same bytes
bool same_ascii(std::string_view left, std::string_view right) noexcept {
    if (left.size() != right.size())
        return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        if (ascii_lower(static_cast<unsigned char>(left[index])) !=
            ascii_lower(static_cast<unsigned char>(right[index])))
            return false;
    }
    return true;
}

/// Tells whether a text ends with a suffix, ignoring the case of ASCII letters.
///
/// @param text the text
/// @param suffix the suffix
/// @return true when `text` ends with `suffix`
bool ends_ascii(std::string_view text, std::string_view suffix) noexcept {
    if (text.size() < suffix.size())
        return false;
    return same_ascii(text.substr(text.size() - suffix.size()), suffix);
}

/// Decodes UTF-8 into its code points.
///
/// The manifest reader has already refused bytes that are not UTF-8.
///
/// @param text the text
/// @return its code points
std::u32string code_points(std::string_view text) {
    std::u32string characters;
    std::size_t at = 0;
    while (at < text.size()) {
        const auto lead = static_cast<unsigned char>(text[at]);
        char32_t code_point = lead;
        std::size_t length = 1;
        if (lead >= 0xF0) {
            code_point = lead & 0x07;
            length = 4;
        } else if (lead >= 0xE0) {
            code_point = lead & 0x0F;
            length = 3;
        } else if (lead >= 0xC0) {
            code_point = lead & 0x1F;
            length = 2;
        }
        for (std::size_t extra = 1; extra < length && at + extra < text.size(); ++extra) {
            code_point = (code_point << 6) | (static_cast<unsigned char>(text[at + extra]) & 0x3F);
        }
        at += length;
        characters.push_back(code_point);
    }
    return characters;
}

/// Tells whether a character is a control character or a line break.
///
/// A C0 control, delete, a C1 control, a line separator or a paragraph
/// separator.
///
/// @param character the code point
/// @return true when plain text refuses it
bool breaks_line(char32_t character) noexcept {
    return character < first_printable ||
           (character >= delete_character && character <= last_c1_control) ||
           character == line_separator || character == paragraph_separator;
}

/// Tells whether a text holds a control character.
///
/// @param text the text
/// @param allow_line_feed whether U+000A may appear
/// @return true when a control character other than that line feed appears
bool has_control(std::string_view text, bool allow_line_feed) {
    const std::u32string characters = code_points(text);
    for (const char32_t character : characters) {
        if (allow_line_feed && character == U'\n')
            continue;
        if (breaks_line(character))
            return true;
    }
    return false;
}

/// Tells whether a key is a language tag of 2 to 35 bytes.
///
/// The first part is letters. Each later part is letters and digits, joined
/// by one hyphen. The same shape as a catalogue language key.
///
/// @param text the key
/// @return true when the text is such a tag
bool language_tag(std::string_view text) noexcept {
    if (text.size() < least_language_bytes || text.size() > most_language_bytes)
        return false;
    std::size_t index = 0;
    if (!letter(text[index]))
        return false;
    while (index < text.size() && letter(text[index]))
        ++index;
    while (index < text.size()) {
        if (text[index] != '-')
            return false;
        ++index;
        const std::size_t start = index;
        while (index < text.size() && (letter(text[index]) || digit(text[index])))
            ++index;
        if (index == start)
            return false;
    }
    return true;
}

/// Tells whether an e-mail address has the shape a mod's author e-mail has.
///
/// One @ with text on both sides, a dot in the part after it, and no white
/// space or control character, at most most_email_bytes.
///
/// @param text the address
/// @return true when it has that shape
bool email_address(std::string_view text) {
    if (text.empty() || text.size() > most_email_bytes)
        return false;
    for (const unsigned char character : text) {
        if (character <= ' ' || character == 0x7F)
            return false;
    }
    const std::size_t at = text.find('@');
    if (at == 0 || at == std::string_view::npos || text.find('@', at + 1) != std::string_view::npos)
        return false;
    const std::string_view domain = text.substr(at + 1);
    const std::size_t dot = domain.find('.');
    return dot != std::string_view::npos && dot != 0 && domain.back() != '.';
}

/// Tells whether a path is a relative package path.
///
/// `/` separates parts. No part is empty, `.` or `..`. No leading slash or
/// backslash, and no backslash anywhere.
///
/// @param path the path
/// @return true when it has that shape
bool package_path(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.front() == '\\')
        return false;
    if (path.find('\\') != std::string_view::npos)
        return false;
    for (const unsigned char character : path) {
        if (character < 0x20 || character == 0x7F)
            return false;
    }
    std::size_t start = 0;
    while (start <= path.size()) {
        const std::size_t cut = path.find('/', start);
        const std::size_t end = cut == std::string_view::npos ? path.size() : cut;
        const std::string_view part = path.substr(start, end - start);
        if (part.empty() || part == "." || part == "..")
            return false;
        if (cut == std::string_view::npos)
            break;
        start = cut + 1;
    }
    return true;
}

/// Tells whether a path's first folder is one of a list.
///
/// @param path the path
/// @param folder the folder, without a slash
/// @return true when the first part is that folder and more follows
bool under_folder(std::string_view path, std::string_view folder) noexcept {
    if (path.size() <= folder.size() || path[folder.size()] != '/')
        return false;
    return same_ascii(path.substr(0, folder.size()), folder);
}

/// Tells whether a list already holds an id.
///
/// @param ids the ids so far
/// @param id the id
/// @return true when `id` is already there
bool repeats(const std::vector<std::string>& ids, std::string_view id) {
    for (const std::string& earlier : ids)
        if (earlier == id)
            return true;
    return false;
}

/// Drops leading zero limbs.
///
/// @param[in,out] value the integer
void trim_big(BigInt& value) {
    while (!value.limbs.empty() && value.limbs.back() == 0)
        value.limbs.pop_back();
}

/// Makes an integer from a significand.
///
/// @param significand the digits
/// @return the integer
BigInt big_from_u64(uint64_t significand) {
    BigInt value;
    while (significand != 0) {
        value.limbs.push_back(static_cast<uint32_t>(significand % limb_base));
        significand /= limb_base;
    }
    return value;
}

/// Multiplies by a factor below the limb base.
///
/// @param[in,out] value the integer
/// @param factor the factor
void multiply_small(BigInt& value, uint32_t factor) {
    uint64_t carry = 0;
    for (uint32_t& limb : value.limbs) {
        carry += static_cast<uint64_t>(limb) * factor;
        limb = static_cast<uint32_t>(carry % limb_base);
        carry /= limb_base;
    }
    if (carry != 0)
        value.limbs.push_back(static_cast<uint32_t>(carry));
}

/// Multiplies by a power of ten.
///
/// @param[in,out] value the integer
/// @param exponent the power, zero or more
void multiply_pow10(BigInt& value, uint32_t exponent) {
    if (value.limbs.empty() || exponent == 0)
        return;
    const uint32_t whole_limbs = exponent / 9u;
    if (whole_limbs != 0)
        value.limbs.insert(value.limbs.begin(), whole_limbs, 0u);
    const uint32_t rest = exponent % 9u;
    if (rest == 0)
        return;
    uint32_t factor = 1;
    for (uint32_t power = 0; power < rest; ++power)
        factor *= 10u;
    multiply_small(value, factor);
}

/// Compares two unsigned integers.
///
/// @param left the first
/// @param right the second
/// @return negative, zero or positive as `left` is below, equal to or above `right`
int compare_big(const BigInt& left, const BigInt& right) {
    if (left.limbs.size() != right.limbs.size())
        return left.limbs.size() < right.limbs.size() ? -1 : 1;
    for (std::size_t index = left.limbs.size(); index > 0; --index) {
        const uint32_t left_limb = left.limbs[index - 1];
        const uint32_t right_limb = right.limbs[index - 1];
        if (left_limb != right_limb)
            return left_limb < right_limb ? -1 : 1;
    }
    return 0;
}

/// Subtracts, when the first integer is at least the second.
///
/// @param left the first integer
/// @param right the second integer
/// @return the difference
BigInt subtract_big(BigInt left, const BigInt& right) {
    uint32_t borrow = 0;
    for (std::size_t index = 0; index < right.limbs.size() || borrow != 0; ++index) {
        uint64_t have = index < left.limbs.size() ? left.limbs[index] : 0;
        const uint64_t take = (index < right.limbs.size() ? right.limbs[index] : 0) + borrow;
        if (have < take) {
            have += limb_base;
            borrow = 1;
        } else {
            borrow = 0;
        }
        if (index >= left.limbs.size())
            left.limbs.push_back(0);
        left.limbs[index] = static_cast<uint32_t>(have - take);
    }
    trim_big(left);
    return left;
}

/// Adds two unsigned integers.
///
/// @param left the first integer
/// @param right the second integer
/// @return the sum
BigInt add_big(BigInt left, const BigInt& right) {
    uint64_t carry = 0;
    const std::size_t count =
        left.limbs.size() > right.limbs.size() ? left.limbs.size() : right.limbs.size();
    for (std::size_t index = 0; index < count || carry != 0; ++index) {
        if (index >= left.limbs.size())
            left.limbs.push_back(0);
        carry += left.limbs[index];
        if (index < right.limbs.size())
            carry += right.limbs[index];
        left.limbs[index] = static_cast<uint32_t>(carry % limb_base);
        carry /= limb_base;
    }
    return left;
}

/// Scales a manifest number to an integer at `floor_exponent`.
///
/// @param number the number
/// @param floor_exponent the power of ten the integer is counted in
/// @return the scaled number
Scaled scale_number(const oamod::Number& number, int32_t floor_exponent) {
    Scaled scaled;
    scaled.negative = number.negative && number.significand != 0;
    scaled.magnitude = big_from_u64(number.significand);
    if (number.exponent > floor_exponent)
        multiply_pow10(scaled.magnitude, static_cast<uint32_t>(number.exponent - floor_exponent));
    return scaled;
}

/// The positive difference of two scaled numbers, the first at least the second.
///
/// @param high the larger number
/// @param low the smaller number
/// @return the difference as an unsigned integer
BigInt positive_difference(const Scaled& high, const Scaled& low) {
    if (high.negative == low.negative) {
        if (!high.negative)
            return subtract_big(high.magnitude, low.magnitude);
        return subtract_big(low.magnitude, high.magnitude);
    }
    return add_big(high.magnitude, low.magnitude);
}

/// Counts how many steps fit in a span.
///
/// @param span the width, as an integer in the same units as `step`
/// @param step one step, above zero
/// @param limit the most steps that still pass
/// @param[out] count the number of steps, when it divides exactly
/// @return true when `span` is a whole number of steps from 1 to `limit`
bool step_count(const BigInt& span, const BigInt& step, uint32_t limit, uint32_t& count) {
    if (step.limbs.empty())
        return false;
    BigInt left = span;
    count = 0;
    while (!left.limbs.empty()) {
        if (compare_big(left, step) < 0)
            return false;
        if (count == limit)
            return false;
        left = subtract_big(std::move(left), step);
        ++count;
    }
    return count > 0;
}

/// The least exponent among four numbers.
///
/// @param first the first number
/// @param second the second number
/// @param third the third number
/// @param fourth the fourth number
/// @return the least exponent
int32_t lowest_exponent(
    const oamod::Number& first,
    const oamod::Number& second,
    const oamod::Number& third,
    const oamod::Number& fourth
) noexcept {
    int32_t floor = first.exponent;
    if (second.exponent < floor)
        floor = second.exponent;
    if (third.exponent < floor)
        floor = third.exponent;
    if (fourth.exponent < floor)
        floor = fourth.exponent;
    return floor;
}

/// Tells whether a key is one of a span of names.
///
/// @param key the key
/// @param allowed the keys that may appear
/// @return true when `key` is one of them
bool listed_span(std::string_view key, std::span<const std::string_view> allowed) {
    for (const std::string_view known : allowed)
        if (known == key)
            return true;
    return false;
}

/// Reports every key of a mapping that is not one of a span of names.
///
/// @param node the mapping
/// @param allowed the keys that may appear
/// @param block the path of the mapping
/// @param[in,out] problems the problems
void refuse_unknown_span(
    const oamod::Node& node,
    std::span<const std::string_view> allowed,
    std::string_view block,
    std::vector<Problem>& problems
) {
    for (const oamod::Node& entry : node.children) {
        if (entry.key.kind == oamod::KeyKind::string && listed_span(entry.key.text, allowed))
            continue;
        const std::string path = child_path(block, entry.key.text);
        add_problem(
            problems,
            entry.position,
            path,
            entry.key.text + " is not a key of " + std::string{block}
        );
    }
}

/// Reports every key of a mapping that is not one of a list.
///
/// @param node the mapping
/// @param allowed the keys that may appear
/// @param block the path of the mapping
/// @param[in,out] problems the problems
template <std::size_t count>
void refuse_unknown_keys(
    const oamod::Node& node,
    const std::array<std::string_view, count>& allowed,
    std::string_view block,
    std::vector<Problem>& problems
) {
    refuse_unknown_span(
        node, std::span<const std::string_view>{allowed.data(), allowed.size()}, block, problems
    );
}

/// Reads the author block.
///
/// @param node the author value
/// @param[out] author the author
/// @param[in,out] problems the problems
void read_author(const oamod::Node& node, Author& author, std::vector<Problem>& problems) {
    if (node.kind != oamod::NodeKind::mapping) {
        add_problem(
            problems,
            node.position,
            "author",
            "author must be an object with a name and an optional email"
        );
        return;
    }
    refuse_unknown_keys(node, author_keys, "author", problems);
    const oamod::Node* name = oamod::find_entry(node, "name");
    if (name == nullptr || name->kind != oamod::NodeKind::string || name->text.empty() ||
        name->text.size() > most_author_name_bytes) {
        add_problem(
            problems,
            name != nullptr ? name->position : node.position,
            "author.name",
            "author.name must be a string of 1 to " + std::to_string(most_author_name_bytes) +
                " bytes"
        );
    } else {
        author.name = name->text;
    }
    const oamod::Node* email = oamod::find_entry(node, "email");
    if (email == nullptr)
        return;
    if (email->kind != oamod::NodeKind::string || !email_address(email->text)) {
        add_problem(
            problems,
            email->position,
            "author.email",
            "author.email must be an e-mail address such as name@example.com"
        );
    } else {
        author.email = email->text;
    }
}

/// Which list a condition is read for.
enum class ConditionList : uint8_t {
    works_with, ///< a mod, "*", or the base game
    mutators,   ///< a mutator id
};

/// Reads a list of conditions.
///
/// @param node the list
/// @param block the path of the list
/// @param most the most entries
/// @param kind which ids the list accepts
/// @param self_id this mutator's id, when it is known
/// @param self_known whether `self_id` was read
/// @param clash ids that must not appear again, such as requires.mutators; may be null
/// @param[out] conditions the entries that passed
/// @param[in,out] problems the problems
void read_conditions(
    const oamod::Node& node,
    std::string_view block,
    std::size_t most,
    ConditionList kind,
    std::string_view self_id,
    bool self_known,
    const std::vector<Condition>* clash,
    std::vector<Condition>& conditions,
    std::vector<Problem>& problems
) {
    if (node.kind != oamod::NodeKind::sequence) {
        add_problem(
            problems, node.position, std::string{block}, std::string{block} + " must be a list"
        );
        return;
    }
    if (node.children.empty() || node.children.size() > most) {
        add_problem(
            problems,
            node.position,
            std::string{block},
            std::string{block} + " must list 1 to " + std::to_string(most) + " entries (it lists " +
                std::to_string(node.children.size()) + ")"
        );
    }
    std::vector<std::string> seen;
    for (std::size_t index = 0; index < node.children.size(); ++index) {
        const oamod::Node& item = node.children[index];
        const std::string path = index_path(block, index);
        if (item.kind != oamod::NodeKind::string) {
            add_problem(problems, item.position, path, path + " must be a string");
            continue;
        }
        std::string error;
        const std::optional<Condition> parsed = parse_condition(item.text, &error);
        if (!parsed) {
            add_problem(problems, item.position, path, std::move(error));
            continue;
        }
        if (kind == ConditionList::mutators && !mutator_id(parsed->id)) {
            add_problem(
                problems,
                item.position,
                path,
                path + " must name a mutator id of lower-case kebab-case"
            );
            continue;
        }
        if (parsed->id == "*" && node.children.size() != 1) {
            add_problem(problems, item.position, path, "* must be the only entry of works-with");
            continue;
        }
        if (repeats(seen, parsed->id)) {
            add_problem(problems, item.position, path, path + " repeats " + parsed->id);
            continue;
        }
        if (self_known && kind == ConditionList::mutators && parsed->id == self_id) {
            add_problem(problems, item.position, path, path + " names this mutator");
            continue;
        }
        if (clash != nullptr) {
            bool named = false;
            for (const Condition& prerequisite : *clash) {
                if (prerequisite.id == parsed->id) {
                    named = true;
                    break;
                }
            }
            if (named) {
                add_problem(
                    problems,
                    item.position,
                    path,
                    path + " also names a mutator requires.mutators names"
                );
                continue;
            }
        }
        seen.push_back(parsed->id);
        conditions.push_back(*parsed);
    }
}

/// Reads one choice value.
///
/// @param item the value
/// @param path the value's path
/// @param[out] value the value, when it is a number or a string
/// @param[in,out] problems the problems
/// @return true when `value` was set
bool read_choice_value(
    const oamod::Node& item,
    const std::string& path,
    ChoiceValue& value,
    std::vector<Problem>& problems
) {
    if (item.kind == oamod::NodeKind::number) {
        value.number = true;
        value.number_value = item.number;
        value.text = oamod::canonical_number_text(item.number);
        return true;
    }
    if (item.kind == oamod::NodeKind::string && choice_value_text(item.text)) {
        value.text = item.text;
        return true;
    }
    add_problem(
        problems,
        item.position,
        path,
        path + " must be a number or 1 to " + std::to_string(most_choice_value_bytes) +
            " letters, digits, _, - or ."
    );
    return false;
}

/// Tells whether two choice values are the same value.
///
/// @param left the first value
/// @param right the second value
/// @return true when both are the same number or the same string
bool same_choice(const ChoiceValue& left, const ChoiceValue& right) noexcept {
    if (left.number != right.number)
        return false;
    if (left.number)
        return oamod::compare_numbers(left.number_value, right.number_value) == 0;
    return left.text == right.text;
}

/// Reads a label of 1 to 64 bytes with no control character.
///
/// @param node the value
/// @param path the value's path
/// @param[out] label the label, when it passed
/// @param[in,out] problems the problems
/// @return true when `label` was set
bool read_label(
    const oamod::Node& node,
    const std::string& path,
    std::string& label,
    std::vector<Problem>& problems
) {
    if (node.kind != oamod::NodeKind::string || node.text.empty() ||
        node.text.size() > most_label_bytes || has_control(node.text, false)) {
        add_problem(
            problems,
            node.position,
            path,
            path + " must be 1 to " + std::to_string(most_label_bytes) +
                " bytes with no control character"
        );
        return false;
    }
    label = node.text;
    return true;
}

/// Reads the values, labels and default of a choice.
///
/// @param node the option
/// @param block the option's path
/// @param[in,out] option the option; its type is already choice
/// @param[in,out] problems the problems
void read_choice(
    const oamod::Node& node,
    const std::string& block,
    Option& option,
    std::vector<Problem>& problems
) {
    const oamod::Node* values = oamod::find_entry(node, "values");
    if (values == nullptr || values->kind != oamod::NodeKind::sequence ||
        values->children.size() < 2 || values->children.size() > most_choice_values) {
        add_problem(
            problems,
            values != nullptr ? values->position : node.position,
            block + ".values",
            block + ".values must list 2 to " + std::to_string(most_choice_values) + " values"
        );
        return;
    }
    bool uniform = true;
    for (std::size_t index = 0; index < values->children.size(); ++index) {
        const std::string path = block + ".values[" + std::to_string(index) + "]";
        ChoiceValue value;
        if (!read_choice_value(values->children[index], path, value, problems)) {
            uniform = false;
            continue;
        }
        if (!option.values.empty() && value.number != option.values.front().number) {
            add_problem(
                problems,
                values->children[index].position,
                path,
                block + ".values must be all numbers or all strings"
            );
            uniform = false;
            continue;
        }
        bool repeated = false;
        for (const ChoiceValue& earlier : option.values) {
            if (same_choice(earlier, value)) {
                repeated = true;
                break;
            }
        }
        if (repeated) {
            add_problem(
                problems, values->children[index].position, path, path + " repeats a value"
            );
            uniform = false;
            continue;
        }
        option.values.push_back(std::move(value));
    }
    if (!uniform)
        return;
    const oamod::Node* labels = oamod::find_entry(node, "labels");
    if (labels != nullptr) {
        if (labels->kind != oamod::NodeKind::sequence ||
            labels->children.size() != option.values.size()) {
            add_problem(
                problems,
                labels->position,
                block + ".labels",
                block + ".labels must list one label for each value"
            );
        } else {
            bool labels_ok = true;
            std::vector<std::string> read_labels;
            for (std::size_t index = 0; index < labels->children.size(); ++index) {
                const std::string path = block + ".labels[" + std::to_string(index) + "]";
                std::string label;
                if (!read_label(labels->children[index], path, label, problems)) {
                    labels_ok = false;
                    continue;
                }
                read_labels.push_back(std::move(label));
            }
            if (labels_ok)
                option.value_labels = std::move(read_labels);
        }
    }
    const oamod::Node* fallback = oamod::find_entry(node, "default");
    if (fallback == nullptr) {
        add_problem(problems, node.position, block + ".default", block + ".default is required");
        return;
    }
    ChoiceValue fallback_value;
    if (!read_choice_value(*fallback, block + ".default", fallback_value, problems))
        return;
    for (std::size_t index = 0; index < option.values.size(); ++index) {
        if (same_choice(option.values[index], fallback_value)) {
            option.default_index = index;
            return;
        }
    }
    add_problem(
        problems,
        fallback->position,
        block + ".default",
        block + ".default must be one of the values"
    );
}

/// Reads the minimum, maximum, step and default of a number option.
///
/// @param node the option
/// @param block the option's path
/// @param[in,out] option the option; its type is already number
/// @param[in,out] problems the problems
void read_number(
    const oamod::Node& node,
    const std::string& block,
    Option& option,
    std::vector<Problem>& problems
) {
    const oamod::Node* minimum = oamod::find_entry(node, "min");
    const oamod::Node* maximum = oamod::find_entry(node, "max");
    const oamod::Node* step = oamod::find_entry(node, "step");
    const oamod::Node* fallback = oamod::find_entry(node, "default");
    const oamod::Node* fields[] = {minimum, maximum, step, fallback};
    const char* names[] = {"min", "max", "step", "default"};
    bool numbers = true;
    for (std::size_t index = 0; index < 4; ++index) {
        if (fields[index] != nullptr && fields[index]->kind == oamod::NodeKind::number)
            continue;
        add_problem(
            problems,
            fields[index] != nullptr ? fields[index]->position : node.position,
            block + "." + names[index],
            block + "." + names[index] + " must be a number"
        );
        numbers = false;
    }
    if (!numbers)
        return;
    if (oamod::compare_numbers(minimum->number, maximum->number) >= 0) {
        add_problem(problems, minimum->position, block + ".min", block + ".min must be below max");
        return;
    }
    const oamod::Number zero = oamod::number_from_integer(0);
    if (oamod::compare_numbers(step->number, zero) <= 0) {
        add_problem(problems, step->position, block + ".step", block + ".step must be above 0");
        return;
    }
    const int32_t floor =
        lowest_exponent(minimum->number, maximum->number, step->number, fallback->number);
    const Scaled scaled_min = scale_number(minimum->number, floor);
    const Scaled scaled_max = scale_number(maximum->number, floor);
    const Scaled scaled_step = scale_number(step->number, floor);
    const Scaled scaled_default = scale_number(fallback->number, floor);
    uint32_t count = 0;
    if (!step_count(
            positive_difference(scaled_max, scaled_min),
            scaled_step.magnitude,
            static_cast<uint32_t>(most_number_steps),
            count
        )) {
        add_problem(
            problems,
            step->position,
            block + ".step",
            block + ".step must divide the range into a whole number of at most " +
                std::to_string(most_number_steps) + " steps"
        );
        return;
    }
    if (oamod::compare_numbers(fallback->number, minimum->number) < 0 ||
        oamod::compare_numbers(fallback->number, maximum->number) > 0) {
        add_problem(
            problems, fallback->position, block + ".default", block + ".default must lie on a step"
        );
        return;
    }
    uint32_t from_min = 0;
    if (!step_count(
            positive_difference(scaled_default, scaled_min), scaled_step.magnitude, count, from_min
        ) &&
        oamod::compare_numbers(fallback->number, minimum->number) != 0) {
        add_problem(
            problems, fallback->position, block + ".default", block + ".default must lie on a step"
        );
        return;
    }
    option.minimum = minimum->number;
    option.maximum = maximum->number;
    option.step = step->number;
    option.number_default = fallback->number;
}

/// Reads one option.
///
/// @param entry the option entry, holding its id as the key
/// @param[out] option the option, when its id is a Lua name
/// @param[in,out] problems the problems
/// @return true when the id was a Lua name and the body was read, even if a rule failed
bool read_option(const oamod::Node& entry, Option& option, std::vector<Problem>& problems) {
    const std::string block = "options." + entry.key.text;
    if (entry.key.kind != oamod::KeyKind::string || !option_id(entry.key.text)) {
        const bool reserved =
            entry.key.kind == oamod::KeyKind::string && lua_reserved_word(entry.key.text);
        add_problem(
            problems,
            entry.key.position,
            block,
            reserved ? "is a word Lua reserves"
                     : "an option id is a Lua name, [a-z][a-z0-9_]*, of 1 to " +
                           std::to_string(most_option_id_bytes) + " bytes"
        );
        return false;
    }
    option.id = entry.key.text;
    if (entry.kind != oamod::NodeKind::mapping) {
        add_problem(problems, entry.position, block, block + " must be an object");
        return true;
    }
    const oamod::Node* type = oamod::find_entry(entry, "type");
    bool type_known = false;
    if (type == nullptr || type->kind != oamod::NodeKind::string) {
        add_problem(
            problems,
            type != nullptr ? type->position : entry.position,
            block + ".type",
            block + ".type must be toggle, choice or number"
        );
    } else if (type->text == "toggle") {
        option.type = OptionType::toggle;
        type_known = true;
    } else if (type->text == "choice") {
        option.type = OptionType::choice;
        type_known = true;
    } else if (type->text == "number") {
        option.type = OptionType::number;
        type_known = true;
    } else {
        add_problem(
            problems,
            type->position,
            block + ".type",
            block + ".type must be toggle, choice or number"
        );
    }
    if (!type_known)
        refuse_unknown_keys(entry, option_keys, block, problems);
    else if (option.type == OptionType::toggle)
        refuse_unknown_keys(entry, toggle_keys, block, problems);
    else if (option.type == OptionType::choice)
        refuse_unknown_keys(entry, choice_keys, block, problems);
    else
        refuse_unknown_keys(entry, number_keys, block, problems);
    const oamod::Node* label = oamod::find_entry(entry, "label");
    if (label == nullptr) {
        add_problem(problems, entry.position, block + ".label", block + ".label is required");
    } else {
        std::string read;
        if (read_label(*label, block + ".label", read, problems))
            option.label = std::move(read);
    }
    if (!type_known)
        return true;
    if (option.type == OptionType::toggle) {
        const oamod::Node* fallback = oamod::find_entry(entry, "default");
        if (fallback == nullptr || fallback->kind != oamod::NodeKind::boolean) {
            add_problem(
                problems,
                fallback != nullptr ? fallback->position : entry.position,
                block + ".default",
                block + ".default must be true or false"
            );
        } else {
            option.toggle_default = fallback->boolean;
        }
        return true;
    }
    if (option.type == OptionType::choice)
        read_choice(entry, block, option, problems);
    else
        read_number(entry, block, option, problems);
    return true;
}

/// Finds an option by id.
///
/// @param options the options read so far
/// @param id the option id
/// @return the option, or null
const Option* find_option(const std::vector<Option>& options, std::string_view id) noexcept {
    for (const Option& option : options)
        if (option.id == id)
            return &option;
    return nullptr;
}

/// Reads the option labels of one translation.
///
/// @param node the options value
/// @param block the path of the options value
/// @param options the mutator's options
/// @param[out] labels the labels that passed
/// @param[in,out] problems the problems
void read_translated_options(
    const oamod::Node& node,
    const std::string& block,
    const std::vector<Option>& options,
    std::vector<OptionLabel>& labels,
    std::vector<Problem>& problems
) {
    if (node.kind != oamod::NodeKind::mapping) {
        add_problem(problems, node.position, block, block + " must be an object");
        return;
    }
    for (const oamod::Node& entry : node.children) {
        const std::string path = block + "." + entry.key.text;
        if (find_option(options, entry.key.text) == nullptr) {
            add_problem(problems, entry.position, path, path + " names no option");
            continue;
        }
        std::string label;
        if (!read_label(entry, path, label, problems))
            continue;
        labels.push_back(OptionLabel{entry.key.text, std::move(label)});
    }
}

/// Reads the choice labels of one translation.
///
/// @param node the labels value
/// @param block the path of the labels value
/// @param options the mutator's options
/// @param[out] labels the lists that passed
/// @param[in,out] problems the problems
void read_translated_labels(
    const oamod::Node& node,
    const std::string& block,
    const std::vector<Option>& options,
    std::vector<ValueLabels>& labels,
    std::vector<Problem>& problems
) {
    if (node.kind != oamod::NodeKind::mapping) {
        add_problem(problems, node.position, block, block + " must be an object");
        return;
    }
    for (const oamod::Node& entry : node.children) {
        const std::string path = block + "." + entry.key.text;
        const Option* option = find_option(options, entry.key.text);
        if (option == nullptr) {
            add_problem(problems, entry.position, path, path + " names no option");
            continue;
        }
        if (option->type != OptionType::choice) {
            add_problem(problems, entry.position, path, path + " is not a choice");
            continue;
        }
        if (entry.kind != oamod::NodeKind::sequence ||
            entry.children.size() != option->values.size()) {
            add_problem(
                problems, entry.position, path, path + " must list one label for each value"
            );
            continue;
        }
        ValueLabels value_labels;
        value_labels.id = entry.key.text;
        bool labels_ok = true;
        for (std::size_t index = 0; index < entry.children.size(); ++index) {
            const std::string item_path = path + "[" + std::to_string(index) + "]";
            std::string label;
            if (!read_label(entry.children[index], item_path, label, problems)) {
                labels_ok = false;
                continue;
            }
            value_labels.labels.push_back(std::move(label));
        }
        if (labels_ok)
            labels.push_back(std::move(value_labels));
    }
}

/// Reads one translation.
///
/// @param entry the language entry
/// @param options the mutator's options
/// @param own_keys keys of the language block the caller reads itself
/// @param[out] translation the translation
/// @param[in,out] problems the problems
/// @return true when the key was a language tag
bool read_translation(
    const oamod::Node& entry,
    const std::vector<Option>& options,
    std::span<const std::string_view> own_keys,
    Translation& translation,
    std::vector<Problem>& problems
) {
    const std::string block = "strings." + entry.key.text;
    if (entry.key.kind != oamod::KeyKind::string || !language_tag(entry.key.text)) {
        add_problem(
            problems,
            entry.key.position,
            block,
            block + " must be a language tag of " + std::to_string(least_language_bytes) + " to " +
                std::to_string(most_language_bytes) + " bytes"
        );
        return false;
    }
    translation.language = entry.key.text;
    if (entry.kind != oamod::NodeKind::mapping) {
        add_problem(problems, entry.position, block, block + " must be an object");
        return true;
    }
    std::vector<std::string_view> allowed;
    allowed.reserve(translation_keys.size() + own_keys.size());
    for (const std::string_view key : translation_keys)
        allowed.push_back(key);
    for (const std::string_view key : own_keys)
        allowed.push_back(key);
    refuse_unknown_span(entry, allowed, block, problems);
    if (const oamod::Node* name = oamod::find_entry(entry, "name")) {
        std::string label;
        if (read_label(*name, block + ".name", label, problems))
            translation.name = std::move(label);
    }
    if (const oamod::Node* description = oamod::find_entry(entry, "description")) {
        if (description->kind != oamod::NodeKind::string) {
            add_problem(
                problems,
                description->position,
                block + ".description",
                block + ".description must be a string"
            );
        } else if (description->text.empty() || description->text.size() > most_description_bytes) {
            add_problem(
                problems,
                description->position,
                block + ".description",
                block + ".description must be 1 to " + std::to_string(most_description_bytes) +
                    " bytes"
            );
        } else if (has_control(description->text, true)) {
            add_problem(
                problems,
                description->position,
                block + ".description",
                block + ".description must not contain a control character other than a line feed"
            );
        } else {
            translation.description = description->text;
        }
    }
    if (const oamod::Node* option_labels = oamod::find_entry(entry, "options"))
        read_translated_options(
            *option_labels, block + ".options", options, translation.options, problems
        );
    if (const oamod::Node* value_labels = oamod::find_entry(entry, "labels"))
        read_translated_labels(
            *value_labels, block + ".labels", options, translation.labels, problems
        );
    return true;
}

/// Reads one compatibility code.
///
/// @param item the code
/// @param path the code's path
/// @param[out] code the code, when it passed
/// @param[in,out] problems the problems
/// @return true when `code` was set
bool read_code(
    const oamod::Node& item,
    const std::string& path,
    std::string& code,
    std::vector<Problem>& problems
) {
    bool letters = item.kind == oamod::NodeKind::string && !item.text.empty() &&
                   item.text.size() <= most_code_bytes;
    if (letters && item.text != "*") {
        for (const char character : item.text) {
            if (!letter(character) && !digit(character))
                letters = false;
        }
    }
    if (!letters) {
        add_problem(
            problems,
            item.position,
            path,
            path + " must be 1 to " + std::to_string(most_code_bytes) + " letters and digits, or *"
        );
        return false;
    }
    code = item.text;
    return true;
}

/// Reads one legacy record.
///
/// @param item the record
/// @param index its index in the list
/// @param[out] legacy the record
/// @param[in,out] problems the problems
void read_legacy(
    const oamod::Node& item, std::size_t index, Legacy& legacy, std::vector<Problem>& problems
) {
    const std::string block = index_path("legacy", index);
    if (item.kind != oamod::NodeKind::mapping) {
        add_problem(problems, item.position, block, block + " must be an object");
        return;
    }
    refuse_unknown_keys(item, legacy_keys, block, problems);
    const oamod::Node* name = oamod::find_entry(item, "name");
    if (name == nullptr || name->kind != oamod::NodeKind::string || name->text.empty() ||
        name->text.size() > most_legacy_name_bytes || has_control(name->text, false)) {
        add_problem(
            problems,
            name != nullptr ? name->position : item.position,
            block + ".name",
            block + ".name must be 1 to " + std::to_string(most_legacy_name_bytes) +
                " bytes with no control character"
        );
    } else {
        legacy.name = name->text;
    }
    const oamod::Node* prefix = oamod::find_entry(item, "prefix");
    if (prefix == nullptr || prefix->kind != oamod::NodeKind::string ||
        (prefix->text != "{}" && prefix->text != "}{")) {
        add_problem(
            problems,
            prefix != nullptr ? prefix->position : item.position,
            block + ".prefix",
            block + ".prefix must be \"{}\" or \"}{\""
        );
    } else {
        legacy.prefix = prefix->text;
    }
    const oamod::Node* compatibility = oamod::find_entry(item, "compatibility");
    if (compatibility == nullptr || compatibility->kind != oamod::NodeKind::sequence ||
        compatibility->children.empty() || compatibility->children.size() > most_legacy_codes) {
        add_problem(
            problems,
            compatibility != nullptr ? compatibility->position : item.position,
            block + ".compatibility",
            block + ".compatibility must list 1 to " + std::to_string(most_legacy_codes) + " codes"
        );
    } else {
        bool codes_ok = true;
        for (std::size_t code_index = 0; code_index < compatibility->children.size();
             ++code_index) {
            const std::string path = block + ".compatibility[" + std::to_string(code_index) + "]";
            std::string code;
            if (!read_code(compatibility->children[code_index], path, code, problems)) {
                codes_ok = false;
                continue;
            }
            legacy.compatibility.push_back(std::move(code));
        }
        if (!codes_ok)
            legacy.compatibility.clear();
    }
    const oamod::Node* files = oamod::find_entry(item, "files");
    if (files == nullptr || files->kind != oamod::NodeKind::sequence || files->children.empty() ||
        files->children.size() > most_legacy_files) {
        add_problem(
            problems,
            files != nullptr ? files->position : item.position,
            block + ".files",
            block + ".files must list 1 to " + std::to_string(most_legacy_files) + " file names"
        );
    } else {
        bool files_ok = true;
        for (std::size_t file_index = 0; file_index < files->children.size(); ++file_index) {
            const oamod::Node& file = files->children[file_index];
            const std::string path = block + ".files[" + std::to_string(file_index) + "]";
            if (file.kind != oamod::NodeKind::string || file.text.empty() ||
                file.text.size() > most_file_name_bytes || has_control(file.text, false) ||
                file.text.find('/') != std::string::npos ||
                file.text.find('\\') != std::string::npos) {
                add_problem(
                    problems,
                    file.position,
                    path,
                    path + " must be a file name of 1 to " + std::to_string(most_file_name_bytes) +
                        " bytes"
                );
                files_ok = false;
                continue;
            }
            legacy.files.push_back(file.text);
        }
        if (!files_ok)
            legacy.files.clear();
    }
}

/// Reads an options block into a list.
///
/// @param options the options value
/// @param[out] out the options that were read
/// @param[in,out] problems the problems
void read_options_block(
    const oamod::Node& options, std::vector<Option>& out, std::vector<Problem>& problems
) {
    if (options.kind != oamod::NodeKind::mapping) {
        add_problem(problems, options.position, "options", "options must be an object");
        return;
    }
    if (options.children.size() > most_options) {
        add_problem(
            problems,
            options.position,
            "options",
            "options lists " + std::to_string(options.children.size()) + " options; at most " +
                std::to_string(most_options)
        );
    }
    for (const oamod::Node& entry : options.children) {
        Option option;
        if (!read_option(entry, option, problems))
            continue;
        out.push_back(std::move(option));
    }
}

/// Reads a strings block into a list.
///
/// @param strings the strings value
/// @param options the options the translations may name
/// @param own_keys keys of a language block the caller reads itself
/// @param[out] out the translations that were read
/// @param[in,out] problems the problems
void read_strings_block(
    const oamod::Node& strings,
    const std::vector<Option>& options,
    std::span<const std::string_view> own_keys,
    std::vector<Translation>& out,
    std::vector<Problem>& problems
) {
    if (strings.kind != oamod::NodeKind::mapping) {
        add_problem(problems, strings.position, "strings", "strings must be an object");
        return;
    }
    if (strings.children.size() > most_languages) {
        add_problem(
            problems,
            strings.position,
            "strings",
            "strings lists " + std::to_string(strings.children.size()) + " languages; at most " +
                std::to_string(most_languages)
        );
    }
    for (const oamod::Node& entry : strings.children) {
        Translation translation;
        if (!read_translation(entry, options, own_keys, translation, problems))
            continue;
        out.push_back(std::move(translation));
    }
}

/// Reads a works-with list.
///
/// @param list the works-with value
/// @param[out] conditions the entries that passed
/// @param[in,out] problems the problems
void read_works_with_list(
    const oamod::Node& list, std::vector<Condition>& conditions, std::vector<Problem>& problems
) {
    read_conditions(
        list,
        "works-with",
        most_works_with,
        ConditionList::works_with,
        {},
        false,
        nullptr,
        conditions,
        problems
    );
}

/// Reads the header and every block.
///
/// @param root the document
/// @param[out] manifest the manifest
/// @param[in,out] problems the problems
void read_document_manifest(
    const oamod::Node& root, Manifest& manifest, std::vector<Problem>& problems
) {
    for (const oamod::Node& entry : root.children) {
        if (entry.key.kind == oamod::KeyKind::string && listed(entry.key.text, manifest_keys))
            continue;
        if (entry.key.kind == oamod::KeyKind::string && listed(entry.key.text, profile_keys)) {
            add_problem(
                problems,
                entry.position,
                entry.key.text,
                entry.key.text + " belongs in a mod's oamod.yaml; a mutator cannot carry it"
            );
            continue;
        }
        add_problem(
            problems, entry.position, entry.key.text, entry.key.text + " is not a key of oamut.yaml"
        );
    }

    const oamod::Node* format = oamod::find_entry(root, "oamut");
    int64_t format_number = 0;
    if (format == nullptr || format->kind != oamod::NodeKind::number || !format->number.integer ||
        !oamod::integer_value(format->number, format_number) || format_number != format_version) {
        add_problem(
            problems,
            format != nullptr ? format->position : root.position,
            "oamut",
            "oamut must be 1"
        );
    } else {
        manifest.format = format_number;
    }

    const oamod::Node* id = oamod::find_entry(root, "id");
    bool id_known = false;
    if (id == nullptr || id->kind != oamod::NodeKind::string || !mutator_id(id->text)) {
        add_problem(
            problems,
            id != nullptr ? id->position : root.position,
            "id",
            "id must be 1 to " + std::to_string(most_id_bytes) + " bytes of lower-case kebab-case"
        );
    } else {
        manifest.id = id->text;
        id_known = true;
    }

    if (const oamod::Node* name = oamod::find_entry(root, "name")) {
        if (name->kind != oamod::NodeKind::string || name->text.empty() ||
            name->text.size() > most_name_bytes || has_control(name->text, false)) {
            add_problem(
                problems,
                name->position,
                "name",
                "name must be 1 to " + std::to_string(most_name_bytes) +
                    " bytes with no control character"
            );
        } else {
            manifest.name = name->text;
        }
    } else {
        add_problem(problems, root.position, "name", "name is required");
    }

    if (const oamod::Node* version = oamod::find_entry(root, "version")) {
        if (version->kind == oamod::NodeKind::number ||
            (version->kind == oamod::NodeKind::string && !version->quoted)) {
            add_problem(
                problems, version->position, "version", "write the version in quotes, as \"2.0\""
            );
        } else if (version->kind != oamod::NodeKind::string) {
            add_problem(problems, version->position, "version", "version must be a string");
        } else {
            const std::optional<Version> parsed = parse_version(version->text);
            if (!parsed) {
                add_problem(
                    problems,
                    version->position,
                    "version",
                    "version must be a mutator version, such as \"2.0\""
                );
            } else {
                manifest.version = *parsed;
            }
        }
    } else {
        add_problem(problems, root.position, "version", "version is required");
    }

    if (const oamod::Node* author = oamod::find_entry(root, "author"))
        read_author(*author, manifest.author, problems);
    else
        add_problem(problems, root.position, "author", "author is required");

    if (const oamod::Node* description = oamod::find_entry(root, "description")) {
        if (description->kind != oamod::NodeKind::string) {
            add_problem(
                problems, description->position, "description", "description must be a string"
            );
        } else {
            if (description->text.empty() || description->text.size() > most_description_bytes) {
                add_problem(
                    problems,
                    description->position,
                    "description",
                    "description must be 1 to " + std::to_string(most_description_bytes) + " bytes"
                );
            }
            if (has_control(description->text, true)) {
                add_problem(
                    problems,
                    description->position,
                    "description",
                    "description must not contain a control character other than a line feed"
                );
            }
            if (!description->text.empty() && description->text.size() <= most_description_bytes &&
                !has_control(description->text, true))
                manifest.description = description->text;
        }
    }

    const oamod::Node* requires_node = oamod::find_entry(root, "requires");
    const oamod::Node* requires_block = nullptr;
    if (requires_node != nullptr) {
        if (requires_node->kind != oamod::NodeKind::mapping) {
            add_problem(
                problems,
                requires_node->position,
                "requires",
                "requires must be an object with engine and mutators"
            );
        } else {
            requires_block = requires_node;
            refuse_unknown_keys(*requires_node, requires_keys, "requires", problems);
        }
    }

    oamod::PackageKeys keys;
    std::vector<oamod::KeyProblem> key_problems;
    oamod::read_package_keys(root, requires_block, keys, key_problems);
    bool tags_broken = oamod::find_entry(root, "tags") == nullptr;
    for (const oamod::KeyProblem& problem : key_problems) {
        if (problem.path == "tags" || problem.path.rfind("tags[", 0) == 0)
            tags_broken = true;
        add_problem(problems, problem.position, problem.path, problem.message);
    }
    manifest.homepage = std::move(keys.homepage);
    manifest.requires_engine = std::move(keys.requires_engine);
    if (const oamod::Node* tags = oamod::find_entry(root, "tags")) {
        if (!tags_broken && keys.tags.size() > most_tags) {
            add_problem(
                problems,
                tags->position,
                "tags",
                "tags lists " + std::to_string(keys.tags.size()) + " tags; at most " +
                    std::to_string(most_tags)
            );
        } else if (!tags_broken) {
            manifest.tags = std::move(keys.tags);
        }
    } else {
        add_problem(problems, root.position, "tags", "tags is required");
    }

    if (const oamod::Node* icon = oamod::find_entry(root, "icon")) {
        if (icon->kind != oamod::NodeKind::string || !package_path(icon->text) ||
            !ends_ascii(icon->text, ".png")) {
            add_problem(
                problems,
                icon->position,
                "icon",
                "icon must be a relative path ending in .png, with / separators and no empty, '.' "
                "or '..' part"
            );
        } else {
            manifest.icon = icon->text;
        }
    }

    if (const oamod::Node* works = oamod::find_entry(root, "works-with")) {
        read_conditions(
            *works,
            "works-with",
            most_works_with,
            ConditionList::works_with,
            manifest.id,
            id_known,
            nullptr,
            manifest.works_with,
            problems
        );
    } else {
        add_problem(problems, root.position, "works-with", "works-with is required");
    }

    if (requires_block != nullptr) {
        if (const oamod::Node* mutators = oamod::find_entry(*requires_block, "mutators")) {
            read_conditions(
                *mutators,
                "requires.mutators",
                most_required,
                ConditionList::mutators,
                manifest.id,
                id_known,
                nullptr,
                manifest.requires_mutators,
                problems
            );
        }
    }

    if (const oamod::Node* conflicts = oamod::find_entry(root, "conflicts-with")) {
        read_conditions(
            *conflicts,
            "conflicts-with",
            most_conflicts,
            ConditionList::mutators,
            manifest.id,
            id_known,
            &manifest.requires_mutators,
            manifest.conflicts_with,
            problems
        );
    }

    if (const oamod::Node* layers = oamod::find_entry(root, "layers")) {
        if (layers->kind != oamod::NodeKind::sequence || layers->children.empty() ||
            layers->children.size() > layer_names.size()) {
            add_problem(
                problems,
                layers->position,
                "layers",
                "layers must list 1 to " + std::to_string(layer_names.size()) + " layers"
            );
        } else {
            std::vector<Layer> seen;
            for (std::size_t index = 0; index < layers->children.size(); ++index) {
                const oamod::Node& item = layers->children[index];
                const std::string path = index_path("layers", index);
                const std::optional<Layer> layer =
                    item.kind == oamod::NodeKind::string ? parse_layer(item.text) : std::nullopt;
                if (!layer) {
                    add_problem(
                        problems,
                        item.position,
                        path,
                        path + " must name files, units, weapons, features, sides, tables, menus, "
                               "maps or hacks"
                    );
                    continue;
                }
                bool repeated = false;
                for (const Layer earlier : seen) {
                    if (earlier == *layer) {
                        repeated = true;
                        break;
                    }
                }
                if (repeated) {
                    add_problem(problems, item.position, path, path + " repeats " + item.text);
                    continue;
                }
                seen.push_back(*layer);
            }
            manifest.layers = std::move(seen);
        }
    } else {
        add_problem(problems, root.position, "layers", "layers is required");
    }

    if (const oamod::Node* clashes = oamod::find_entry(root, "clashes")) {
        const std::optional<ClashMode> mode = clashes->kind == oamod::NodeKind::string
                                                  ? parse_clash_mode(clashes->text)
                                                  : std::nullopt;
        if (!mode) {
            add_problem(problems, clashes->position, "clashes", "clashes must be add or replace");
        } else {
            manifest.clashes = *mode;
        }
    }

    if (const oamod::Node* options = oamod::find_entry(root, "options"))
        read_options_block(*options, manifest.options, problems);

    if (const oamod::Node* strings = oamod::find_entry(root, "strings"))
        read_strings_block(*strings, manifest.options, {}, manifest.strings, problems);

    manifest.entry = std::string{default_entry};
    if (const oamod::Node* entry = oamod::find_entry(root, "entry")) {
        const bool folder =
            entry->kind == oamod::NodeKind::string &&
            (under_folder(entry->text, "files") || under_folder(entry->text, "rules") ||
             under_folder(entry->text, "lua"));
        if (entry->kind != oamod::NodeKind::string || !package_path(entry->text) ||
            !ends_ascii(entry->text, ".lua") || folder) {
            add_problem(
                problems,
                entry->position,
                "entry",
                "entry must be a relative path ending in .lua, and not under files/, rules/ or lua/"
            );
        } else {
            manifest.entry = entry->text;
        }
    }

    if (const oamod::Node* legacy = oamod::find_entry(root, "legacy")) {
        if (legacy->kind != oamod::NodeKind::sequence) {
            add_problem(
                problems,
                legacy->position,
                "legacy",
                "legacy is a list of records, one for each original"
            );
        } else {
            if (legacy->children.empty() || legacy->children.size() > most_legacy_records) {
                add_problem(
                    problems,
                    legacy->position,
                    "legacy",
                    "legacy must list 1 to " + std::to_string(most_legacy_records) + " records"
                );
            }
            for (std::size_t index = 0; index < legacy->children.size(); ++index) {
                Legacy record;
                read_legacy(legacy->children[index], index, record, problems);
                manifest.legacy.push_back(std::move(record));
            }
        }
    }
}

/// The first line of a description, cut to most_summary_bytes on a character boundary.
///
/// @param description the description
/// @return the summary
std::string cut_summary(std::string_view description) {
    const std::size_t break_at = description.find('\n');
    if (break_at != std::string_view::npos)
        description = description.substr(0, break_at);
    if (description.size() <= most_summary_bytes)
        return std::string{description};
    std::size_t cut = most_summary_bytes;
    while (cut > 0 && (static_cast<unsigned char>(description[cut]) & 0xC0) == 0x80)
        --cut;
    return std::string{description.substr(0, cut)};
}

void relay_add_problem(
    std::vector<Problem>& problems,
    oamod::TextPosition position,
    std::string key,
    std::string message
) {
    add_problem(problems, position, std::move(key), std::move(message));
}

bool relay_package_path(std::string_view path) {
    return package_path(path);
}

bool relay_has_control(std::string_view text, bool allow_line_feed) {
    return has_control(text, allow_line_feed);
}

bool relay_language_tag(std::string_view text) noexcept {
    return language_tag(text);
}

void relay_read_author(const oamod::Node& node, Author& author, std::vector<Problem>& problems) {
    read_author(node, author, problems);
}

} // namespace

namespace shared {

void add_problem(
    std::vector<Problem>& problems,
    oamod::TextPosition position,
    std::string key,
    std::string message
) {
    relay_add_problem(problems, position, std::move(key), std::move(message));
}

void refuse_unknown_keys(
    const oamod::Node& node,
    std::span<const std::string_view> allowed,
    std::string_view block,
    std::vector<Problem>& problems
) {
    refuse_unknown_span(node, allowed, block, problems);
}

bool package_path(std::string_view path) {
    return relay_package_path(path);
}

bool has_control(std::string_view text, bool allow_line_feed) {
    return relay_has_control(text, allow_line_feed);
}

bool language_tag(std::string_view text) noexcept {
    return relay_language_tag(text);
}

void read_author(const oamod::Node& node, Author& author, std::vector<Problem>& problems) {
    relay_read_author(node, author, problems);
}

void read_works_with(
    const oamod::Node& list, std::vector<Condition>& conditions, std::vector<Problem>& problems
) {
    read_works_with_list(list, conditions, problems);
}

void read_options(
    const oamod::Node& mapping, std::vector<Option>& options, std::vector<Problem>& problems
) {
    read_options_block(mapping, options, problems);
}

void read_strings(
    const oamod::Node& mapping,
    const std::vector<Option>& options,
    std::span<const std::string_view> own_keys,
    std::vector<Translation>& strings,
    std::vector<Problem>& problems
) {
    read_strings_block(mapping, options, own_keys, strings, problems);
}

} // namespace shared

bool lua_reserved_word(std::string_view text) noexcept {
    // Lua 5.4's 22 reserved words, in order, matched byte for byte.
    constexpr std::array<std::string_view, 22> reserved{
        "and",      "break",  "do",   "else", "elseif", "end",   "false", "for",
        "function", "goto",   "if",   "in",   "local",  "nil",   "not",   "or",
        "repeat",   "return", "then", "true", "until",  "while",
    };
    for (const std::string_view word : reserved) {
        if (word == text)
            return true;
    }
    return false;
}

bool option_id(std::string_view text) noexcept {
    if (text.empty() || text.size() > most_option_id_bytes || lua_reserved_word(text))
        return false;
    if (text.front() < 'a' || text.front() > 'z')
        return false;
    for (std::size_t index = 1; index < text.size(); ++index) {
        const char character = text[index];
        const bool word =
            (character >= 'a' && character <= 'z') || digit(character) || character == '_';
        if (!word)
            return false;
    }
    return true;
}

bool choice_value_text(std::string_view text) noexcept {
    if (text.empty() || text.size() > most_choice_value_bytes)
        return false;
    for (const char unit : text) {
        const auto byte = static_cast<unsigned char>(unit);
        const bool letter = (byte >= 'A' && byte <= 'Z') || (byte >= 'a' && byte <= 'z');
        const bool digit = byte >= '0' && byte <= '9';
        if (!letter && !digit && byte != '_' && byte != '-' && byte != '.')
            return false;
    }
    return true;
}

const char* layer_name(Layer layer) noexcept {
    return layer_names[static_cast<std::size_t>(layer)].data();
}

const char* clash_mode_name(ClashMode mode) noexcept {
    return clash_mode_names[static_cast<std::size_t>(mode)].data();
}

std::optional<ClashMode> parse_clash_mode(std::string_view text) noexcept {
    for (std::size_t index = 0; index < clash_mode_names.size(); ++index) {
        if (clash_mode_names[index] == text)
            return static_cast<ClashMode>(index);
    }
    return std::nullopt;
}

std::optional<Layer> parse_layer(std::string_view text) noexcept {
    for (std::size_t index = 0; index < layer_names.size(); ++index) {
        if (layer_names[index] == text)
            return static_cast<Layer>(index);
    }
    return std::nullopt;
}

bool read_manifest(
    std::span<const uint8_t> bytes, Manifest& manifest, std::vector<Problem>& problems
) {
    oamod::Node root;
    oamod::ReadError error{};
    std::vector<Problem> found;
    if (!oamod::read_document(bytes, root, error)) {
        const uint32_t line = error.position.line == 0 ? 1 : error.position.line;
        const uint32_t column = error.position.column == 0 ? 1 : error.position.column;
        found.push_back(Problem{line, column, "document", oamod::rule_message(error.rule)});
    } else {
        Manifest read;
        read_document_manifest(root, read, found);
        if (found.empty()) {
            manifest = std::move(read);
            return true;
        }
    }
    problems.insert(problems.end(), found.begin(), found.end());
    return false;
}

std::string summary_of(const Manifest& manifest) {
    return cut_summary(manifest.description);
}

std::string describe(const Problem& problem) {
    return std::string{manifest_file} + ":" + std::to_string(problem.line) + ":" +
           std::to_string(problem.column) + ": " + problem.key + ": " + problem.message;
}

} // namespace oa::formats::oamut

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Legacy rule files. parse_rules reads one file's bytes into rule records
// and a list of diagnostics. It opens nothing, throws nothing of its own,
// and refuses a text longer than max_input_bytes before it allocates.
#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::formats::soft_mutator {

/// The most bytes a rule file may hold. A longer text is refused whole.
inline constexpr size_t max_input_bytes = 1024u * 1024u;
/// The most blocks one file may hold.
inline constexpr uint32_t max_blocks = 4096;
/// The most statements one block may hold, its sections included.
inline constexpr uint32_t max_statements = 4096;
/// The most bytes of one name, field or quoted text.
inline constexpr uint32_t max_text_bytes = 1024;
/// The most diagnostics one parse keeps. The last of them says the list stopped.
inline constexpr uint32_t max_diagnostics = 1000;

/// The folder and filename suffix a block's records are read from.
///
/// `folder` is lower case with `/` separators. `extension` is lower case,
/// without a leading dot, and is a suffix of the filename (`sidedata.tdf`
/// reaches one file, not every file of that folder).
struct Target {
    std::string folder{"units"};  ///< folder under the game data, lower case
    std::string extension{"fbi"}; ///< filename suffix, lower case, no leading dot

    /// Reports whether two targets name the same folder and suffix.
    ///
    /// @param other target compared
    /// @return true when folder and extension are equal
    [[nodiscard]] bool operator==(const Target& other) const noexcept {
        return folder == other.folder && extension == other.extension;
    }
};

/// Which records of the target a block changes.
enum class Selector : uint8_t {
    all,   ///< every record
    name,  ///< the one record of this name
    group, ///< records that meet every need
};

/// How a need compares a field.
enum class NeedKind : uint8_t {
    range,    ///< the field's number is between low and high, inclusive
    equals,   ///< the field's text equals text, ignoring ASCII case
    contains, ///< the field's text contains text, ignoring ASCII case
};

/// One requirement of a group. Every need of the group must hold.
struct Need {
    std::string field; ///< field name, as written
    NeedKind kind{};   ///< how the field is compared
    bool negated{};    ///< true when a leading '!' reverses the comparison
    double low{};      ///< inclusive lower bound of a range
    double high{};     ///< inclusive upper bound of a range
    std::string text;  ///< compared text of an equals or contains need
    uint32_t line{};   ///< line of the statement, from 1
};

/// How a change writes a field.
enum class ChangeKind : uint8_t {
    assign,      ///< write value, as a number
    assign_text, ///< write text
    remove,      ///< clear the field
    append,      ///< append text to the field
    scale,       ///< multiply the field by value
    scale_clamp, ///< multiply by value, then clamp to low..high
    meta,        ///< write value so that it wins over other changes
};

/// One write a block makes. `value` is the assigned number, the factor or
/// the meta value; a text change leaves it 0.
struct Change {
    std::string field; ///< field name as written, or the depth marker
    bool all_depth2{}; ///< true when the field is the depth marker, any case
    ChangeKind kind{}; ///< how the field is written
    double value{};    ///< assigned number, factor or meta value
    double low{};      ///< inclusive lower clamp of a scale_clamp
    double high{};     ///< inclusive upper clamp of a scale_clamp
    std::string text;  ///< text of an assign_text or append, bytes unchanged
    uint32_t line{};   ///< line of the statement, from 1
};

/// One bracket block, with the target in force where the block was written.
struct Block {
    Target target{};             ///< folder and suffix for this block
    Selector selector{};         ///< which records the block changes
    std::string name;            ///< record name; empty unless selector is name
    std::vector<Need> needs;     ///< requirements, in the order written
    std::vector<Change> changes; ///< writes, in the order written
    uint32_t line{};             ///< line of the block's header, from 1
};

/// The blocks of one rule file, in the order written.
struct RuleFile {
    std::vector<Block> blocks; ///< blocks in file order
};

/// How serious a diagnostic is.
enum class Severity : uint8_t {
    warning, ///< the statement or block was kept
    error,   ///< the statement, or the broken block, was dropped
};

/// Why a diagnostic was reported.
enum class DiagnosticCode : uint8_t {
    missing_semicolon,       ///< a kept statement was not closed with ';'
    group_without_asterisks, ///< a header without asterisks holds a group body
    quoted_number,           ///< a change's quoted text is a number
    spaces_in_value,         ///< spaces inside an unquoted value were ignored
    empty_range,             ///< a need's low bound is above its high bound
    group_without_needs,     ///< a group has no needs section, so it matches every record
    repeated_section,        ///< a second needs or changes section was merged
    byte_order_mark,         ///< a leading byte-order mark was skipped
    unquoted_text,           ///< bare words kept as text; the quotes were left off
    empty_value,             ///< nothing after '='; empty text, which is not a removal
    empty_text_need,         ///< "" or !"" in needs; equality with empty text, not presence
    stray_closing_brace,     ///< a '}' outside any block, ignored so later blocks are read

    unknown_section,      ///< not needs or changes, or a section inside a section
    unclosed_block,       ///< a header or brace was never closed
    unexpected_brace,     ///< a brace where a block did not call for one
    directive_in_block,   ///< a folder or extension directive inside a block
    bad_number,           ///< a number the grammar does not allow
    bad_need,             ///< a need that matches no need form
    bad_change,           ///< a change that matches no change form
    need_form_in_changes, ///< a need written where a change belongs
    change_form_in_needs, ///< a change written where a need belongs
    clamp_low_above_high, ///< a scale's low clamp is above its high clamp
    unterminated_string,  ///< a quote was never closed
    block_comment,        ///< a '/*' comment, which is not part of the language
    stray_text,           ///< not a directive, block or statement, or outside a group section
    too_large,            ///< the text is longer than max_input_bytes
    too_many_blocks,      ///< the file holds more than max_blocks blocks
    too_many_statements,  ///< a block holds more than max_statements statements
    text_too_long,        ///< a name, field or quoted text exceeds max_text_bytes
    too_many_diagnostics, ///< the diagnostic list stopped at max_diagnostics
};

/// One finding, with the line it was found on.
struct Diagnostic {
    Severity severity{};   ///< warning or error
    DiagnosticCode code{}; ///< what was found
    uint32_t line{};       ///< line of the finding, from 1
    std::string message;   ///< what was wrong, naming the statement when it has one
};

/// The records and diagnostics of one parse.
struct ParseResult {
    RuleFile rules;                      ///< blocks that were kept
    std::vector<Diagnostic> diagnostics; ///< findings, in the order found

    /// Reports whether the parse found an error.
    ///
    /// @return true when any diagnostic is an error
    [[nodiscard]] bool has_errors() const noexcept;
};

/// Reads one rule file.
///
/// A text longer than max_input_bytes is refused before the file is read.
/// Anything else is read to the end: a bad statement is dropped and the
/// rest of the file is still read. Keywords, section names and the depth
/// marker are matched ignoring ASCII case.
///
/// @param text the file's bytes; they need not be terminated
/// @return the records and the diagnostics
[[nodiscard]] ParseResult parse_rules(std::span<const uint8_t> text);

/// Returns the spelling of a diagnostic code, as its enumerator is written.
///
/// @param code the code
/// @return a static name such as "missing_semicolon"
[[nodiscard]] std::string_view code_name(DiagnosticCode code) noexcept;

/// Reports whether two blocks are the same rule.
///
/// Line numbers are ignored. Field names and the block name are compared
/// ignoring ASCII case, need texts ignoring ASCII case, and change texts
/// exactly. Needs match as a set. Changes match in order after a stable
/// sort by field name folded to lower case, so two writes of one field keep
/// the order they were written in.
///
/// @param left a block
/// @param right a block
/// @return true when the blocks are the same rule
[[nodiscard]] bool equivalent(const Block& left, const Block& right) noexcept;

/// Reports whether two rule files are the same rules, in the same order.
///
/// @param left a file
/// @param right a file
/// @return true when every block is equivalent to the block in that position
[[nodiscard]] bool equivalent(const RuleFile& left, const RuleFile& right) noexcept;

} // namespace oa::formats::soft_mutator

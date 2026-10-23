// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The parts of a mutator manifest a brain manifest reads and writes the same
// way: the author, works-with, options and translations, and the text of an
// author block and an options block. The mutator reader keeps its own rules.
#pragma once

#include "oa/formats/oamut/manifest.hpp"

#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::formats::oamut::shared {

/// Appends one problem.
///
/// @param[in,out] problems the problems collected so far
/// @param position where the value starts
/// @param key the path of the value
/// @param message what is wrong
void add_problem(
    std::vector<Problem>& problems,
    oamod::TextPosition position,
    std::string key,
    std::string message
);

/// Reports every key of a mapping that is not one of a list.
///
/// A key that is not a string is reported too. The message is
/// `<key> is not a key of <block>`, and the path is the block's child.
///
/// @param node the mapping
/// @param allowed the keys that may appear
/// @param block the path of the mapping
/// @param[in,out] problems the problems
void refuse_unknown_keys(
    const oamod::Node& node,
    std::span<const std::string_view> allowed,
    std::string_view block,
    std::vector<Problem>& problems
);

/// Tells whether a path is a relative package path.
///
/// `/` separates parts. No part is empty, `.` or `..`. No leading slash or
/// backslash, and no backslash anywhere.
///
/// @param path the path
/// @return true when it has that shape
[[nodiscard]] bool package_path(std::string_view path);

/// Tells whether a text holds a control character.
///
/// @param text the text
/// @param allow_line_feed whether U+000A may appear
/// @return true when a control character other than that line feed appears
[[nodiscard]] bool has_control(std::string_view text, bool allow_line_feed);

/// Tells whether a key is a language tag of 2 to 35 bytes.
///
/// The first part is letters. Each later part is letters and digits, joined
/// by one hyphen.
///
/// @param text the key
/// @return true when the text is such a tag
[[nodiscard]] bool language_tag(std::string_view text) noexcept;

/// Reads the author block.
///
/// A mapping of `name` (1 to 128 bytes) and an optional `email`. Any other
/// key is refused. The address has one `@`, a dot in the part after it, no
/// space and no control character, and is at most 254 bytes.
///
/// @param node the author value
/// @param[out] author the author; a refused field is left as it was
/// @param[in,out] problems the problems
void read_author(const oamod::Node& node, Author& author, std::vector<Problem>& problems);

/// Reads a works-with list.
///
/// One to 32 conditions, none twice. `*` is the only entry when it appears.
/// Paths are `works-with[i]`, indexes from 0. Entries that pass are appended.
///
/// @param list the works-with value
/// @param[out] conditions the entries that passed
/// @param[in,out] problems the problems
void read_works_with(
    const oamod::Node& list, std::vector<Condition>& conditions, std::vector<Problem>& problems
);

/// Reads an options block.
///
/// A mapping of at most 16 option ids. Each option follows the mutator
/// option rules. Options whose id is a Lua name are appended, even when a
/// later field of that option is refused.
///
/// @param mapping the options value
/// @param[out] options the options that were read
/// @param[in,out] problems the problems
void read_options(
    const oamod::Node& mapping, std::vector<Option>& options, std::vector<Problem>& problems
);

/// Reads a strings block.
///
/// A mapping of at most 16 language tags. Each language follows the mutator
/// translation rules. `own_keys` are keys of a language block that this
/// reader does not interpret and does not refuse; the caller reads them.
/// The mutator reader passes none. Languages whose key is a language tag
/// are appended.
///
/// @param mapping the strings value
/// @param options the options the translations may name
/// @param own_keys keys a language block may hold besides the mutator's
/// @param[out] strings the translations that were read
/// @param[in,out] problems the problems
void read_strings(
    const oamod::Node& mapping,
    const std::vector<Option>& options,
    std::span<const std::string_view> own_keys,
    std::vector<Translation>& strings,
    std::vector<Problem>& problems
);

/// Writes a string as a double-quoted YAML scalar.
///
/// @param text the string
/// @return the quoted scalar, quotes included
[[nodiscard]] std::string quoted(std::string_view text);

/// Writes a condition as a quoted scalar.
///
/// An id alone stays the id. A range is the id, one space and the range
/// text as it was written.
///
/// @param condition the condition
/// @return the quoted text
[[nodiscard]] std::string condition_text(const Condition& condition);

/// Writes the top-level author block.
///
/// `author:` then `name`, then `email` when one was written.
///
/// @param[in,out] out the text
/// @param author the author
void write_author(std::string& out, const Author& author);

/// Writes the options block.
///
/// Nothing is written when `options` is empty. Otherwise `options:` and
/// each option, in order, in the mutator form.
///
/// @param[in,out] out the text
/// @param options the options
void write_options(std::string& out, const std::vector<Option>& options);

} // namespace oa::formats::oamut::shared

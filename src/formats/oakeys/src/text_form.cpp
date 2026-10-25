// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/formats/oakeys/manifest.hpp"

#include "detail.hpp"

#include "oa/formats/oamod.hpp"
#include "oa/formats/oamut/shared.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::formats::oakeys {
namespace {

namespace oamod = oa::formats::oamod;
namespace shared = oa::formats::oamut::shared;

/// The comment that names a paste's base, including the space before the token.
constexpr std::string_view base_prefix = "# oakeys bindings, base ";

static_assert(base_prefix.size() == 24);

/// The same comment with no token after it.
constexpr std::string_view base_head = "# oakeys bindings, base";

/// Where a paste's base comment sits, and the token it names.
struct BaseComment {
    bool present{false}; ///< a base comment was found
    uint32_t line{1};    ///< the 1-based line of the comment
    uint32_t column{1};  ///< the 1-based column of the token
    std::string token{}; ///< the token, empty when none was written
};

/// Finds the first `# oakeys bindings, base` comment.
///
/// The strict reader drops comments, so the base is taken from the bytes.
///
/// @param bytes the text
/// @return the comment, or one that was not present
BaseComment find_base_comment(std::span<const uint8_t> bytes) {
    BaseComment found;
    const auto* data = reinterpret_cast<const char*>(bytes.data());
    std::size_t start = 0;
    uint32_t line = 1;
    while (start <= bytes.size()) {
        std::size_t end = start;
        while (end < bytes.size() && data[end] != '\n')
            ++end;
        std::string_view row{data + start, end - start};
        if (!row.empty() && row.back() == '\r')
            row.remove_suffix(1);
        const std::size_t hash = row.find('#');
        if (!found.present && hash != std::string_view::npos) {
            const std::string_view comment = row.substr(hash);
            if (comment.starts_with(base_prefix) || comment == base_head) {
                found.present = true;
                found.line = line;
                if (comment.starts_with(base_prefix)) {
                    const std::string_view rest = comment.substr(base_prefix.size());
                    std::size_t length = 0;
                    while (length < rest.size() && rest[length] != ' ' && rest[length] != '\t')
                        ++length;
                    found.token.assign(rest.data(), length);
                    found.column = static_cast<uint32_t>(hash + base_prefix.size() + 1);
                } else {
                    found.column = static_cast<uint32_t>(hash + base_head.size() + 1);
                }
            }
        }
        if (end >= bytes.size())
            break;
        start = end + 1;
        ++line;
    }
    return found;
}

/// One problem for a document the strict reader refused.
///
/// A position of 0 is reported as line 1, column 1.
///
/// @param error the reader's error
/// @return the problem, on the key `document`
Problem document_problem(const oamod::ReadError& error) {
    const uint32_t line = error.position.line == 0 ? 1 : error.position.line;
    const uint32_t column = error.position.column == 0 ? 1 : error.position.column;
    return Problem{line, column, "document", oamod::rule_message(error.rule)};
}

} // namespace

std::string write_bindings_text(const Manifest& manifest) {
    std::string out = std::string{base_prefix} + base_name(manifest.base) + "\n";
    out += write_binding_lines(manifest.bindings, 0);
    return out;
}

bool read_bindings_text(
    std::span<const uint8_t> bytes, Manifest& manifest, std::vector<Problem>& problems
) {
    oamod::Node root;
    oamod::ReadError error{};
    if (!oamod::read_document(bytes, root, error)) {
        problems.push_back(document_problem(error));
        return false;
    }
    if (oamod::find_entry(root, "oakeys") != nullptr)
        return read_manifest(bytes, manifest, problems);

    std::vector<Problem> found;
    Manifest read;
    read.base = Base::oa_default;
    const BaseComment comment = find_base_comment(bytes);
    if (comment.present) {
        const std::optional<Base> parsed = parse_base(comment.token);
        if (!parsed) {
            shared::add_problem(
                found,
                oamod::TextPosition{comment.line, comment.column},
                "base",
                "base must be oa-default or none"
            );
        } else {
            read.base = *parsed;
        }
    }
    read_binding_mapping(root, "", read.bindings, found);
    if (!found.empty()) {
        problems.insert(problems.end(), found.begin(), found.end());
        return false;
    }
    manifest = std::move(read);
    return true;
}

} // namespace oa::formats::oakeys

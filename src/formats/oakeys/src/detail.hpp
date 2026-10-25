// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Shared by the manifest reader and the two writers. Not a public header.
#pragma once

#include "oa/formats/oakeys/manifest.hpp"

#include "oa/formats/oamod.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::formats::oakeys {

/// Writes one chord as a flow-sequence item.
///
/// Letters, digits and `+` stay bare, except a chord a strict reader would
/// take as a number, a boolean or null. Anything else is double-quoted.
///
/// @param text the chord
/// @return the scalar, quotes included when it needs them
[[nodiscard]] std::string chord_scalar(std::string_view text);

/// Writes bindings at one indentation, each on one line.
///
/// @param bindings the bindings, in the order to write
/// @param indent how many levels in each line sits
/// @return the lines, each ending in a line break; empty when there are none
[[nodiscard]] std::string
write_binding_lines(const std::vector<BindingEntry>& bindings, int indent);

/// Reads a mapping of action ids to chord lists.
///
/// @param node the mapping
/// @param block `bindings`, or empty when the mapping is the whole text
/// @param[out] bindings the entries that were read
/// @param[in,out] problems the problems
void read_binding_mapping(
    const oa::formats::oamod::Node& node,
    std::string_view block,
    std::vector<BindingEntry>& bindings,
    std::vector<Problem>& problems
);

/// Returns a base's manifest name.
///
/// @param base the base
/// @return oa-default or none
[[nodiscard]] const char* base_name(Base base) noexcept;

/// Reads a base.
///
/// @param text the name, compared byte for byte
/// @return the base, or nothing when the text is not one
[[nodiscard]] std::optional<Base> parse_base(std::string_view text) noexcept;

} // namespace oa::formats::oakeys

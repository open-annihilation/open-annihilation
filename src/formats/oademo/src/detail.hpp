// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

// Shared by the description parser and the archive reader. Not a public header.

#include "oa/formats/oademo/oademo.hpp"

#include <string_view>

namespace oa::formats::oademo {
namespace detail {

/// Why a description or a manifest was refused.
enum class Issue : uint8_t {
    none,    ///< the text was read
    version, ///< the version is not 1
    details, ///< the text is missing a member or is not JSON
};

/// Reads a description.
///
/// @param text the JSON text
/// @param[out] meta the description, when the text was read
/// @param[out] issue why the text was refused
/// @return true when the text was read
[[nodiscard]] bool parse_meta(std::string_view text, Meta& meta, Issue& issue);

/// Reads a manifest.
///
/// @param text the JSON text
/// @param[out] manifest the manifest, when the text was read
/// @param[out] issue why the text was refused
/// @return true when the text was read
[[nodiscard]] bool parse_manifest(std::string_view text, Manifest& manifest, Issue& issue);

/// Rewrites one JSON value as compact JSON.
///
/// @param text one JSON value
/// @return the compact text, or nothing when the text is not one value
[[nodiscard]] std::optional<std::string> canonical_json(std::string_view text);

} // namespace detail
} // namespace oa::formats::oademo

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Reading a catalogue from JSON already parsed, so that the check parses a
// catalogue's bytes once.
#pragma once

#include "oa/data/catalogue/catalogue.hpp"
#include "oa/formats/json.hpp"

#include <optional>
#include <span>
#include <string>

namespace oa::data::catalogue {

/// Reads a catalogue's structure and entries from its parsed JSON.
///
/// Applies the rules read_catalogue applies to the bytes, in the same
/// order, once the bytes have been parsed.
///
/// @param root the parsed catalogue, or nothing when its bytes are not JSON
/// @param[out] out the catalogue; left unchanged when the catalogue is refused
/// @param[out] error why the catalogue was refused; may be null
/// @return true when the catalogue was read
[[nodiscard]] bool read_parsed_catalogue(
    const std::optional<formats::json::Json>& root, Catalogue& out, std::string* error
);

/// Parses a catalogue's bytes as JSON.
///
/// @param bytes the catalogue's bytes
/// @return the parsed value, or nothing when the bytes are not JSON
[[nodiscard]] std::optional<formats::json::Json>
parse_catalogue_json(std::span<const uint8_t> bytes);

} // namespace oa::data::catalogue

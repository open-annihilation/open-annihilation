// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The subset of JSON Schema a tool's arguments are checked against
// (schema.cpp).
#pragma once

#include "oa/formats/json.hpp"

#include <string>

namespace oa::app::mcp {

/// Returns why a value does not match a tool's schema, or empty when it does.
///
/// The value must be an object. The schema may use type, properties,
/// required, additionalProperties, enum, minimum and maximum.
///
/// @param schema the tool's schema object
/// @param value the call's arguments
/// @return the reason, or empty when the arguments match
[[nodiscard]] std::string
schema_rejection(const oa::formats::json::Json& schema, const oa::formats::json::Json& value);

} // namespace oa::app::mcp

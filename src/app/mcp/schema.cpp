// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Checks a tool's arguments against the schema subset schema.hpp names.
#include "schema.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace oa::app::mcp {
namespace {

namespace json = oa::formats::json;

constexpr int kMaxDepth = 16;

/// The name a failure uses: the member's path, or arguments at the root.
///
/// @param path the member's path; empty at the root
/// @return the name
std::string label_of(std::string_view path) {
    return path.empty() ? std::string("arguments") : std::string(path);
}

/// Joins a member onto a path.
///
/// @param path the path so far; empty at the root
/// @param member the member's name
/// @return the member's path
std::string child_path(std::string_view path, std::string_view member) {
    if (path.empty())
        return std::string(member);
    std::string joined(path);
    joined.push_back('.');
    joined.append(member);
    return joined;
}

/// Tells whether two JSON values are the same value.
///
/// @param left one value
/// @param right the other
/// @return true when they are the same
bool same_value(const json::Json& left, const json::Json& right) {
    if (left.type() != right.type())
        return false;
    switch (left.type()) {
    case json::JsonType::null:
        return true;
    case json::JsonType::boolean:
        return left.boolean() == right.boolean();
    case json::JsonType::number:
        return left.number_text() != nullptr && right.number_text() != nullptr &&
               *left.number_text() == *right.number_text();
    case json::JsonType::string:
        return left.string() != nullptr && right.string() != nullptr &&
               *left.string() == *right.string();
    case json::JsonType::array: {
        const auto left_items = left.elements();
        const auto right_items = right.elements();
        if (left_items.size() != right_items.size())
            return false;
        for (std::size_t index = 0; index < left_items.size(); ++index)
            if (!same_value(left_items[index], right_items[index]))
                return false;
        return true;
    }
    case json::JsonType::object: {
        const auto left_names = left.names();
        const auto left_values = left.values();
        const auto right_names = right.names();
        const auto right_values = right.values();
        if (left_names.size() != right_names.size())
            return false;
        for (std::size_t index = 0; index < left_names.size(); ++index) {
            if (left_names[index] != right_names[index])
                return false;
            if (!same_value(left_values[index], right_values[index]))
                return false;
        }
        return true;
    }
    }
    return false;
}

/// Returns why a value is not the schema's type, or empty when it is or the
/// schema names no type.
///
/// @param schema the schema object
/// @param value the value
/// @param path the value's path
/// @return the reason, or empty
std::string
type_rejection(const json::Json& schema, const json::Json& value, std::string_view path) {
    const json::Json* declared = schema.find("type");
    if (declared == nullptr || declared->string() == nullptr)
        return {};
    const std::string& expect = *declared->string();
    bool matches = false;
    if (expect == "object")
        matches = value.type() == json::JsonType::object;
    else if (expect == "array")
        matches = value.type() == json::JsonType::array;
    else if (expect == "string")
        matches = value.type() == json::JsonType::string;
    else if (expect == "boolean")
        matches = value.type() == json::JsonType::boolean;
    else if (expect == "null")
        matches = value.type() == json::JsonType::null;
    else if (expect == "integer")
        matches = value.type() == json::JsonType::number && value.integer().has_value();
    else if (expect == "number")
        matches = value.type() == json::JsonType::number;
    else
        return {};
    if (matches)
        return {};
    return label_of(path) + " must be a " + expect;
}

/// Returns why a value is outside an enum, or empty when it is in it or
/// there is none.
///
/// @param schema the schema object
/// @param value the value
/// @param path the value's path
/// @return the reason, or empty
std::string
enum_rejection(const json::Json& schema, const json::Json& value, std::string_view path) {
    const json::Json* allowed = schema.find("enum");
    if (allowed == nullptr || allowed->type() != json::JsonType::array)
        return {};
    const auto choices = allowed->elements();
    for (const json::Json& choice : choices)
        if (same_value(value, choice))
            return {};
    return label_of(path) + " is not one of the allowed values";
}

/// Returns why a whole number is outside minimum or maximum, or empty.
///
/// A bound that is not a whole number is not applied.
///
/// @param schema the schema object
/// @param value the value
/// @param path the value's path
/// @return the reason, or empty
std::string
range_rejection(const json::Json& schema, const json::Json& value, std::string_view path) {
    const std::optional<int64_t> whole = value.integer();
    if (!whole)
        return {};
    const json::Json* minimum = schema.find("minimum");
    if (minimum != nullptr) {
        const std::optional<int64_t> bound = minimum->integer();
        if (bound && *whole < *bound)
            return label_of(path) + " is below " + std::to_string(*bound);
    }
    const json::Json* maximum = schema.find("maximum");
    if (maximum != nullptr) {
        const std::optional<int64_t> bound = maximum->integer();
        if (bound && *whole > *bound)
            return label_of(path) + " is above " + std::to_string(*bound);
    }
    return {};
}

std::string
reject_value(const json::Json& schema, const json::Json& value, std::string_view path, int depth);

/// Returns why an object's members do not match, or empty.
///
/// @param schema the schema object
/// @param value the object
/// @param path the object's path
/// @param depth how deep the value is nested
/// @return the reason, or empty
std::string object_rejection(
    const json::Json& schema, const json::Json& value, std::string_view path, int depth
) {
    if (value.type() != json::JsonType::object)
        return {};
    const json::Json* required = schema.find("required");
    if (required != nullptr && required->type() == json::JsonType::array) {
        const auto names = required->elements();
        for (const json::Json& required_name : names) {
            const std::string* member = required_name.string();
            if (member == nullptr)
                continue;
            if (value.find(*member) == nullptr)
                return "missing " + child_path(path, *member);
        }
    }
    const json::Json* properties = schema.find("properties");
    const json::Json* extras = schema.find("additionalProperties");
    const bool closed = extras != nullptr && extras->boolean().has_value() && !*extras->boolean();
    if (closed) {
        const auto keys = value.names();
        const auto allowed = properties != nullptr && properties->type() == json::JsonType::object
                                 ? properties->names()
                                 : std::span<const std::string>{};
        for (const std::string& key : keys) {
            bool known = false;
            for (const std::string& allow : allowed)
                if (allow == key)
                    known = true;
            if (!known)
                return "unknown argument " + child_path(path, key);
        }
    }
    if (properties != nullptr && properties->type() == json::JsonType::object) {
        const auto names = properties->names();
        const auto schemas = properties->values();
        for (std::size_t index = 0; index < names.size() && index < schemas.size(); ++index) {
            const json::Json* member = value.find(names[index]);
            if (member == nullptr)
                continue;
            const std::string why =
                reject_value(schemas[index], *member, child_path(path, names[index]), depth + 1);
            if (!why.empty())
                return why;
        }
    }
    return {};
}

std::string
reject_value(const json::Json& schema, const json::Json& value, std::string_view path, int depth) {
    if (depth > kMaxDepth)
        return "the value is nested too deeply";
    if (schema.type() != json::JsonType::object)
        return label_of(path) + " must match the schema";
    if (const std::string why = type_rejection(schema, value, path); !why.empty())
        return why;
    if (const std::string why = enum_rejection(schema, value, path); !why.empty())
        return why;
    if (const std::string why = range_rejection(schema, value, path); !why.empty())
        return why;
    if (const std::string why = object_rejection(schema, value, path, depth); !why.empty())
        return why;
    return {};
}

} // namespace

std::string schema_rejection(const json::Json& schema, const json::Json& value) {
    if (value.type() != json::JsonType::object)
        return "arguments must be an object";
    return reject_value(schema, value, {}, 0);
}

} // namespace oa::app::mcp

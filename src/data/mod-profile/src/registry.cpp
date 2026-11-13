// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The registry's tables, the lookups over them, and the check of a value
// against a parameter's type and bounds.

#include "oa/data/mod_profile/registry.hpp"

#include <array>
#include <regex>
#include <string>

namespace oa::data::mod_profile::registry {

namespace {

#include "registry_table.inc"

/// The first byte of a UTF-8 continuation; every other byte starts a character.
constexpr unsigned char continuation_mask = 0xC0;
constexpr unsigned char continuation_marker = 0x80;

constexpr Registry registry_value{
    catalogue,
    script_index_minimum,
    script_index_maximum,
    base_index_first,
    base_index_last,
    default_fidelity,
    entries,
    parameters,
    literals,
    enum_values,
    presets,
    preset_values,
    constraints,
    area_titles,
};

/// Counts the characters of UTF-8 text.
///
/// @param text the text
/// @return its number of code points
size_t character_count(std::string_view text) noexcept {
    size_t count = 0;
    for (const char c : text) {
        if ((static_cast<unsigned char>(c) & continuation_mask) != continuation_marker)
            ++count;
    }
    return count;
}

/// Writes a bound for a message, as None when there is none.
///
/// @param has whether there is a bound
/// @param bound the bound
/// @return its text
std::string bound_text(bool has, const formats::oamod::Number& bound) {
    return has ? formats::oamod::canonical_number_text(bound) : "None";
}

/// Writes the permitted value spellings for a message.
///
/// @param spec the value's spec
/// @return the values, as a list
std::string values_text(const ValueSpec& spec) {
    std::string text = "[";
    bool first = true;
    for (const std::string_view value : enum_values_of(spec)) {
        if (!first)
            text += ", ";
        first = false;
        text += value;
    }
    return text + "]";
}

/// Tells whether a list holds an item twice, numbers compared by value.
///
/// @param items the items
/// @return true when two are equal
bool repeats(const std::vector<Value>& items) {
    for (size_t left = 0; left < items.size(); ++left) {
        for (size_t right = left + 1; right < items.size(); ++right) {
            if (values_equal(items[left], items[right]))
                return true;
        }
    }
    return false;
}

/// Makes a problem.
///
/// @param path the value's path
/// @param value the value
/// @param message what is wrong
/// @return the problem
Problem problem(const std::string& path, const Value& value, std::string message) {
    return Problem{path, value.position, std::move(message)};
}

} // namespace

const Registry& table() noexcept {
    return registry_value;
}

const Entry* find_entry(std::string_view id) noexcept {
    for (const Entry& entry : entries) {
        if (entry.id == id)
            return &entry;
    }
    return nullptr;
}

std::string_view area_title(std::string_view area) noexcept {
    for (const AreaTitle& named : area_titles) {
        if (named.area == area)
            return named.title;
    }
    return area;
}

std::span<const Parameter> parameters_of(const Entry& entry) noexcept {
    return std::span<const Parameter>{parameters}.subspan(
        entry.first_parameter, entry.parameter_count
    );
}

int find_parameter(const Entry& entry, std::string_view name) noexcept {
    const auto list = parameters_of(entry);
    for (size_t index = 0; index < list.size(); ++index) {
        if (list[index].name == name)
            return static_cast<int>(index);
    }
    return -1;
}

std::span<const Preset> presets_of(const Entry& entry) noexcept {
    return std::span<const Preset>{presets}.subspan(entry.first_preset, entry.preset_count);
}

std::span<const Constraint> constraints_of(const Entry& entry) noexcept {
    return std::span<const Constraint>{constraints}.subspan(
        entry.first_constraint, entry.constraint_count
    );
}

std::span<const std::string_view> enum_values_of(const ValueSpec& spec) noexcept {
    return std::span<const std::string_view>{enum_values}.subspan(
        spec.first_value, spec.value_count
    );
}

Value literal_value(uint16_t literal) {
    const Literal& written = literals.at(literal);
    switch (written.kind) {
    case LiteralKind::boolean:
        return make_boolean(written.boolean);
    case LiteralKind::number:
        return make_number(written.number);
    case LiteralKind::string:
        return make_string(written.text);
    case LiteralKind::list: {
        Value list = make_list();
        for (uint16_t index = 0; index < written.item_count; ++index)
            list.items.push_back(literal_value(static_cast<uint16_t>(written.first_item + index)));
        return list;
    }
    }
    return Value{};
}

std::optional<Problem>
check_value(const ValueSpec& spec, const Value& value, const std::string& path, Value& normal) {
    using formats::oamod::compare_numbers;
    const ValueType type = spec.type;
    const bool list_type = type == ValueType::integer_list || type == ValueType::decimal_list ||
                           type == ValueType::enumeration_list || type == ValueType::string_list ||
                           type == ValueType::enumeration_set;
    if (list_type) {
        if (value.kind != ValueKind::list)
            return problem(path, value, "expected a list, got " + value_text(value));
        const size_t count = value.items.size();
        if (spec.has_length) {
            if (spec.length_minimum == spec.length_maximum && count != spec.length_minimum)
                return problem(
                    path,
                    value,
                    "expected " + std::to_string(spec.length_minimum) + " items, got " +
                        std::to_string(count)
                );
            if (count < spec.length_minimum || count > spec.length_maximum)
                return problem(
                    path,
                    value,
                    "expected " + std::to_string(spec.length_minimum) + "-" +
                        std::to_string(spec.length_maximum) + " items, got " + std::to_string(count)
                );
        }
        ValueSpec item_spec = spec;
        item_spec.type = type == ValueType::integer_list   ? ValueType::integer
                         : type == ValueType::decimal_list ? ValueType::decimal
                         : type == ValueType::string_list  ? ValueType::string
                                                           : ValueType::enumeration;
        Value items = make_list();
        items.position = value.position;
        for (size_t index = 0; index < count; ++index) {
            Value item{};
            if (auto failed = check_value(
                    item_spec, value.items[index], path + "[" + std::to_string(index) + "]", item
                ))
                return failed;
            items.items.push_back(std::move(item));
        }
        if ((spec.distinct || type == ValueType::enumeration_set) && repeats(items.items))
            return problem(path, value, "repeated items in " + value_text(value));
        if (spec.ascending) {
            for (size_t index = 1; index < count; ++index) {
                if (compare_numbers(items.items[index - 1].number, items.items[index].number) >= 0)
                    return problem(path, value, "items must ascend: " + value_text(value));
            }
        }
        if (type == ValueType::enumeration_set) {
            Value ordered = make_list();
            ordered.position = value.position;
            for (const std::string_view member : enum_values_of(spec)) {
                for (const Value& item : items.items) {
                    if (item.text == member) {
                        ordered.items.push_back(item);
                        break;
                    }
                }
            }
            items = std::move(ordered);
        }
        normal = std::move(items);
        return std::nullopt;
    }
    if (type == ValueType::integer_or_none && value.kind == ValueKind::string &&
        value.text == "none") {
        normal = value;
        return std::nullopt;
    }
    if (type == ValueType::boolean) {
        if (value.kind != ValueKind::boolean)
            return problem(path, value, "expected true or false, got " + value_text(value));
        normal = value;
        return std::nullopt;
    }
    if (type == ValueType::integer || type == ValueType::integer_or_none ||
        type == ValueType::decimal) {
        const bool decimal = type == ValueType::decimal;
        if (value.kind != ValueKind::number || (!decimal && !value.number.integer))
            return problem(
                path,
                value,
                std::string(decimal ? "expected a number" : "expected an integer") + ", got " +
                    value_text(value)
            );
        Value number = value;
        if (decimal)
            number.number.integer = false;
        if ((spec.has_minimum && compare_numbers(number.number, spec.minimum) < 0) ||
            (spec.has_maximum && compare_numbers(number.number, spec.maximum) > 0))
            return problem(
                path,
                value,
                value_text(value) + " is outside [" + bound_text(spec.has_minimum, spec.minimum) +
                    ", " + bound_text(spec.has_maximum, spec.maximum) + "]"
            );
        if (spec.has_multiple_of) {
            int64_t whole = 0;
            int64_t step = 0;
            if (!formats::oamod::integer_value(number.number, whole) ||
                !formats::oamod::integer_value(spec.multiple_of, step) || step == 0 ||
                whole % step != 0)
                return problem(
                    path,
                    value,
                    value_text(value) + " is not a multiple of " +
                        formats::oamod::canonical_number_text(spec.multiple_of)
                );
        }
        if (!decimal && spec.value_count != 0) {
            const std::string spelling = formats::oamod::canonical_number_text(number.number);
            bool member = false;
            for (const std::string_view candidate : enum_values_of(spec))
                member = member || candidate == spelling;
            if (!member)
                return problem(
                    path, value, value_text(value) + " is not one of " + values_text(spec)
                );
        }
        normal = std::move(number);
        return std::nullopt;
    }
    if (type == ValueType::string) {
        if (value.kind != ValueKind::string)
            return problem(path, value, "expected a string, got " + value_text(value));
        if (spec.max_length >= 0 &&
            character_count(value.text) > static_cast<size_t>(spec.max_length))
            return problem(
                path, value, "longer than " + std::to_string(spec.max_length) + " characters"
            );
        if (!spec.pattern.empty() &&
            !std::regex_search(
                value.text, std::regex{std::string{spec.pattern}, std::regex::ECMAScript}
            ))
            return problem(
                path, value, value_text(value) + " does not match " + std::string{spec.pattern}
            );
        normal = value;
        return std::nullopt;
    }
    if (type == ValueType::enumeration) {
        bool member = false;
        if (value.kind == ValueKind::string) {
            for (const std::string_view candidate : enum_values_of(spec))
                member = member || candidate == value.text;
        }
        if (!member)
            return problem(path, value, value_text(value) + " is not one of " + values_text(spec));
        normal = value;
        return std::nullopt;
    }
    return problem(path, value, "unknown type");
}

} // namespace oa::data::mod_profile::registry

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Typed ranges for the unit, weapon and movement-class keys the loaders read.
// A value that cannot be stored safely is refused before a match; a value that
// only wraps, or that an older mutator relied on, is kept with a warning.
#pragma once

#include "oa/formats/tdf.hpp"

#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <string_view>

namespace oa::data::field_schema {

/// Which record a key belongs to.
enum class RecordKind : uint8_t {
    unit,          ///< an FBI `[UNITINFO]` key, including movement keys read when no class is named
    weapon,        ///< a weapon-file section key
    weapon_damage, ///< a `[DAMAGE]` entry: `default`, or any unit name
    move_class,    ///< a movement-class section key
};

/// How the loader parses the text.
enum class FieldType : uint8_t {
    integer,    ///< whole number, as `get_int` reads it
    fixed,      ///< 16.16 real number, as `get_fixed` reads it
    decimal,    ///< real number, as `get_double` reads it
    flag,       ///< whole number whose low bit is kept
    text,       ///< text kept as written
    name_ref,   ///< a name of another record
    token_list, ///< category words
    yard_map,   ///< yard-map cells
};

/// How wide the stored result is. `ticks` are seconds times 30.
enum class Storage : uint8_t {
    none,    ///< a flag bit, or text
    int8,    ///< signed 8 bits
    uint8,   ///< unsigned 8 bits
    int16,   ///< signed 16 bits
    uint16,  ///< unsigned 16 bits
    int32,   ///< signed 32 bits
    uint32,  ///< unsigned 32 bits
    float32, ///< a decimal kept in a float
    fixed32, ///< a 16.16 value, or a scaled count kept in 32 bits
    ticks16, ///< seconds times 30, kept in 16 bits
    ticks32, ///< seconds times 30, kept in 32 bits
};

/// How the value is chosen when a file leaves the key out.
enum class DefaultRule : uint8_t {
    constant,        ///< `default_number` or `default_text`
    twice_fixed,     ///< twice the 16.16 value of `default_from`
    whole_of,        ///< the whole number `default_from` reads as
    half_low_byte,   ///< the low byte of `default_from`, halved
    text_of,         ///< the text of `default_from`
    cloaker_distance ///< 80 when `default_from` reads above 0, else 0
};

/// One key the loaders read.
///
/// An infinite bound is no bound. `default_number` is the number a numeric key
/// reads when the file leaves it out and the rule is `constant`; `default_text`
/// is that text for a text, name or token list. Derived defaults use
/// `default_rule` and `default_from` instead.
struct FieldSpec {
    std::string_view key{}; ///< canonical spelling, lower case; empty for any `[DAMAGE]` unit name
    RecordKind record{};
    FieldType type{};
    Storage storage{};
    double refuse_below = -std::numeric_limits<double>::infinity(); ///< refuse a number below this
    double refuse_above = std::numeric_limits<double>::infinity();  ///< refuse a number above this
    double warn_below = -std::numeric_limits<double>::infinity();   ///< warn below this
    double warn_above = std::numeric_limits<double>::infinity();    ///< warn above this
    double default_number{};                                        ///< constant numeric default
    std::string_view default_text{};                                ///< constant text default
    DefaultRule default_rule{};
    std::string_view default_from{}; ///< the other key a derived default reads
    std::string_view why{};          ///< the sentence used when a value is refused or warned about
    std::string_view read_at{};      ///< loader file and line that reads the key
};

/// Returns the keys of one record, in table order.
///
/// @param record which record
/// @return the specs; empty when the kind is unknown
[[nodiscard]] std::span<const FieldSpec> fields(RecordKind record) noexcept;

/// Finds one key, ignoring ASCII case.
///
/// A `[DAMAGE]` unit name is any key other than `default`.
///
/// @param record which record
/// @param key field name; an empty name matches nothing except the unit-name rule
/// @return the spec, or null when the schema does not hold the key
[[nodiscard]] const FieldSpec* find_field(RecordKind record, std::string_view key) noexcept;

/// What a check decided.
enum class Verdict : uint8_t {
    ok,     ///< the loader can keep the value
    warn,   ///< the value is kept, and it wraps or sits past a known threshold
    refuse, ///< the value can crash a match or drop a record
};

/// The result of checking one value.
struct Check {
    Verdict verdict{};  ///< ok, warn or refuse
    std::string reason; ///< empty when the verdict is ok; otherwise key, value, limit and why
};

/// Checks a value written as text, reading it the way the loader would.
///
/// A key the schema does not hold is ok. A numeric field whose text holds no
/// number is refused. A number with text after it is a warning naming what the
/// loader skips, then judged as that number. Text the writer would reject (a
/// semicolon, a line break, a zero byte, `//` or `/*`) is refused.
///
/// @param record which record
/// @param key field name
/// @param text the value as written
/// @param weapon_types the weapon types a weapon `id` is held to; 3.1c's 256
///     unless the caller read a profile's limits.weapon-types
/// @return the verdict and, unless it is ok, a sentence naming the key and the limit
[[nodiscard]] Check check_text(
    RecordKind record, std::string_view key, std::string_view text, uint32_t weapon_types = 256
);

/// Checks a number already parsed.
///
/// A non-finite number in a numeric field is refused. A key the schema does
/// not hold is ok. A non-whole number in an integer field is judged on its
/// whole part.
///
/// @param record which record
/// @param key field name
/// @param value the number
/// @param weapon_types the weapon types a weapon `id` is held to; 3.1c's 256
///     unless the caller read a profile's limits.weapon-types
/// @return the verdict and, unless it is ok, a sentence naming the key and the limit
[[nodiscard]] Check
check_number(RecordKind record, std::string_view key, double value, uint32_t weapon_types = 256);

/// A value read from text, in the form a script sees.
struct Value {
    /// Which member holds the value.
    enum class Kind : uint8_t {
        absent,  ///< the text was missing
        integer, ///< an integer key
        number,  ///< a fixed or decimal key, unscaled
        boolean, ///< a flag's low bit
        text,    ///< text, a name, a token list, a yard map, or an unknown key
    } kind{};
    int64_t integer{}; ///< the integer after the width cast
    double number{};   ///< the real number, or 0 when an integer text held no number
    bool boolean{};    ///< the flag
    std::string text;  ///< the text
};

/// Reads one value the way the loader reads that key.
///
/// An integer is what `parse_int` gives, after the width cast. A fixed or
/// decimal value is `parse_double` of the text, not the scaled count. A flag
/// is the low bit. Text, names and unknown keys are the text itself.
///
/// @param spec the key, or null for an unknown key
/// @param text the value, or null when the file leaves it out
/// @return the value; `absent` when `text` is null
[[nodiscard]] Value read_value(const FieldSpec* spec, const char* text);

/// Returns the value a record holds for a key the block leaves out.
///
/// Derived defaults are computed from the block's other keys with the same
/// getters the loader uses. A key the schema does not hold is `absent`. A
/// `[DAMAGE]` unit entry left out is 0.
///
/// @param record which record
/// @param key field name
/// @param block the record's block, or null when every key is left out
/// @return the default, in `read_value`'s kinds
[[nodiscard]] Value
default_value(RecordKind record, std::string_view key, const oa::formats::tdf::Block* block);

} // namespace oa::data::field_schema

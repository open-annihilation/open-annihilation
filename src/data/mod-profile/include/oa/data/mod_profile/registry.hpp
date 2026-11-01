// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The OAMOD registry as the engine holds it: every meaning a profile can
// name (identity and layout values, limits, unit-script extensions, data
// keys, hacks, visual strings and media), with each parameter's type, bounds,
// 3.1c baseline, default, adjustability and scope, and each hack's presets,
// constraints and whether this engine implements it.
//
// The tables are generated from src/data/mod-profile/registry/hack-registry.yaml
// by tools/gen_mod_registry.py (registry_table.inc); the mod-registry-sync
// test fails when they differ from what the registry gives.
#pragma once

#include "oa/data/mod_profile/value.hpp"
#include "oa/formats/oamod.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace oa::data::mod_profile::registry {

/// What an entry of the registry is.
enum class EntryKind : uint8_t {
    identity,         ///< a value of the profile's identity block
    layout,           ///< a value of the layout block
    limit,            ///< an engine limit, with parameters
    script_extension, ///< a unit-script value mounted at a get or set index
    data_key,         ///< a meaning a key of a mod's unit or weapon files binds to
    hack,             ///< a behaviour, with parameters
    string,           ///< a visual text
    media,            ///< a front-end movie
};

/// The type of a value or parameter.
enum class ValueType : uint8_t {
    boolean,
    integer,
    decimal,
    string,
    enumeration,
    integer_list,
    decimal_list,
    enumeration_list,
    string_list,
    enumeration_set, ///< distinct enumeration values, held in the order the registry lists them
    integer_or_none, ///< an integer, or the string none
};

/// Who may change a parameter.
enum class Adjustable : uint8_t {
    fixed,   ///< the profile only
    install, ///< the player's settings, where a profile binds them
    match,   ///< as install, and the host for one game
};

/// Whether a value is part of the network-play hash.
enum class Scope : uint8_t {
    sim,  ///< every machine must agree; hashed
    view, ///< local display, input, audio or files; not hashed
};

/// Where the original game reads a parameter's setting.
enum class SettingSource : uint8_t {
    none,
    ini,      ///< a Section/Key of its INI file
    registry, ///< a value under its registry key
};

/// How a bound setting's raw value becomes the parameter.
enum class SettingTransform : uint8_t {
    none,
    unit_type_bits, ///< 512 when the value is at most 512, else ((value >> 9) + 1) * 512
};

/// How a constraint compares two parameters.
enum class Comparison : uint8_t {
    less_equal,
    less,
    greater_equal,
    greater,
};

/// Which way a script extension is mounted.
enum class ScriptDirection : uint8_t {
    none,
    get,
    set,
};

/// Which files a data key is read from.
enum class DataFile : uint8_t {
    none,
    unit,   ///< unit definition (FBI) files
    weapon, ///< weapon (TDF) files
};

/// What a literal of the registry holds.
enum class LiteralKind : uint8_t {
    boolean,
    number,
    string,
    list,
};

/// A value written in the registry: a baseline, a default or a preset's value.
struct Literal {
    LiteralKind kind{};
    bool boolean{};                  ///< a boolean's value
    formats::oamod::Number number{}; ///< a number's value
    std::string_view text{};         ///< a string's value
    uint16_t first_item{};           ///< a list's first item, an index into the literals
    uint16_t item_count{};           ///< a list's number of items
};

/// No literal.
inline constexpr uint16_t no_literal = UINT16_MAX;

/// The type and bounds of a value or parameter.
struct ValueSpec {
    ValueType type{};
    Scope scope{};
    bool has_minimum{};
    formats::oamod::Number minimum{}; ///< inclusive; element-wise for lists
    bool has_maximum{};
    formats::oamod::Number maximum{}; ///< inclusive; element-wise for lists
    bool has_multiple_of{};
    formats::oamod::Number multiple_of{};
    bool has_length{};
    uint16_t length_minimum{};     ///< the fewest items a list holds
    uint16_t length_maximum{};     ///< the most items a list holds
    bool ascending{};              ///< a list's items strictly ascend
    bool distinct{};               ///< a list's items differ
    int32_t max_length{-1};        ///< the most characters a string holds; -1 for no bound
    std::string_view pattern{};    ///< a regular expression a string must contain a match of
    uint16_t first_value{};        ///< an enumeration's first value, an index into the enum values
    uint16_t value_count{};        ///< an enumeration's number of values
    uint16_t baseline{no_literal}; ///< the 3.1c value, a literal; no_literal for a data key's
};

/// One parameter of a limit or hack.
struct Parameter {
    std::string_view name{};
    ValueSpec value{};        ///< its type, bounds, scope and baseline
    uint16_t default_value{}; ///< the value while the entry is on, a literal
    Adjustable adjustable{};
    SettingSource setting_source{};
    std::string_view setting_name{}; ///< the INI Section/Key or registry value
    SettingTransform transform{};
    int16_t default_from{-1}; ///< the parameter, within the entry, the default follows; -1 for none
    formats::oamod::Number default_factor{}; ///< what that parameter is multiplied by
    int16_t clamp_low{-1};  ///< the parameter a bound or match value is clamped above; -1 for none
    int16_t clamp_high{-1}; ///< the parameter a bound or match value is clamped below; -1 for none
    std::string_view per_type_key{}; ///< the data-key meaning that overrides it per unit or weapon
    std::string_view unit{};         ///< what a number counts, such as "ticks"; empty for none
};

/// One value of a preset.
struct PresetValue {
    uint16_t parameter{}; ///< the parameter, within the entry
    uint16_t literal{};   ///< its value
};

/// A named, complete set of a limit's or hack's parameter values.
struct Preset {
    std::string_view name{};
    uint16_t first_value{}; ///< an index into the preset values
    uint16_t value_count{};
};

/// A comparison two parameters of an entry must keep once resolved.
struct Constraint {
    uint16_t left{}; ///< a parameter, within the entry
    Comparison comparison{};
    uint16_t right{};        ///< a parameter, within the entry
    std::string_view text{}; ///< as the registry writes it, such as "min <= default"
};

/// One entry of the registry.
struct Entry {
    std::string_view id{};
    EntryKind kind{};
    Scope scope{}; ///< whether the entry's presence is hashed
    bool visual{}; ///< a visual hack
    /// A mutator's hacks block may set this entry's match parameters.
    bool mutator{};
    bool implemented{}; ///< a hack this engine carries out
    /// A hack OA no longer offers: a profile that names it still loads,
    /// with the same hashes, but it changes nothing, and the player's
    /// overrides and the settings leave it out (standard_hacks).
    bool retired{};
    std::string_view key{};     ///< an identity, layout, string or media value's profile key
    ValueSpec value{};          ///< an identity, layout, string, media or data-key value
    uint16_t first_parameter{}; ///< an index into the parameters
    uint16_t parameter_count{};
    int16_t shorthand{-1}; ///< the parameter a scalar sets; -1 for none
    uint16_t first_preset{};
    uint16_t preset_count{};
    uint16_t first_constraint{};
    uint16_t constraint_count{};
    ScriptDirection direction{};      ///< a script extension's direction
    int32_t default_index{};          ///< the index a list-form mount uses
    DataFile file{};                  ///< a data key's files
    std::string_view standard_key{};  ///< the usual spelling of a data key
    std::string_view overrides{};     ///< the ENTRY.PARAM a data key overrides per type
    std::string_view required_hack{}; ///< the hack a data key needs on
    std::string_view summary{};       ///< what the entry does, in one sentence
    /// The part of the game it belongs to, such as "ai"; a hack's is the
    /// first word of its id.
    std::string_view area{};
    /// A hack's name as players see it, in English, such as "Deterministic
    /// Wind"; empty for a retired hack and for every other entry. The
    /// interface shows it through its catalogue
    /// (oa/data/languages/interface_text.hpp).
    std::string_view title{};
};

/// The name players see for one area of the hacks.
struct AreaTitle {
    std::string_view area{};  ///< the area, such as "ui"
    std::string_view title{}; ///< its name, in English, such as "Interface"
};

/// The whole registry.
struct Registry {
    int32_t catalogue{};            ///< the catalogue version profiles require
    int32_t script_index_minimum{}; ///< the lowest get or set index
    int32_t script_index_maximum{}; ///< the highest get or set index
    int32_t base_index_first{};     ///< the first index 3.1c itself uses
    int32_t base_index_last{};      ///< the last index 3.1c itself uses
    std::string_view
        default_fidelity{}; ///< the script-extension fidelity a profile gets without one
    std::span<const Entry> entries{};
    std::span<const Parameter> parameters{};
    std::span<const Literal> literals{};
    std::span<const std::string_view> enum_values{};
    std::span<const Preset> presets{};
    std::span<const PresetValue> preset_values{};
    std::span<const Constraint> constraints{};
    /// Every area of the hacks with its name, in the registry's order.
    std::span<const AreaTitle> area_titles{};
};

/// Returns the registry.
///
/// @return the engine's registry, constant for the life of the program
[[nodiscard]] const Registry& table() noexcept;

/// Finds an entry by its id.
///
/// @param id the entry's id, such as "units.id-reuse-delay"
/// @return the entry, or null when the registry has none
[[nodiscard]] const Entry* find_entry(std::string_view id) noexcept;

/// Returns the name players see for an area of the hacks.
///
/// @param area the area, such as "ui"
/// @return its name in English, such as "Interface"; the area itself when
///     the registry names none
[[nodiscard]] std::string_view area_title(std::string_view area) noexcept;

/// Returns an entry's parameters.
///
/// @param entry an entry of the registry
/// @return its parameters, in the registry's order
[[nodiscard]] std::span<const Parameter> parameters_of(const Entry& entry) noexcept;

/// Finds a parameter of an entry by name.
///
/// @param entry an entry of the registry
/// @param name the parameter's name
/// @return its index within the entry, or -1
[[nodiscard]] int find_parameter(const Entry& entry, std::string_view name) noexcept;

/// Returns an entry's presets.
///
/// @param entry an entry of the registry
/// @return its presets
[[nodiscard]] std::span<const Preset> presets_of(const Entry& entry) noexcept;

/// Returns an entry's constraints.
///
/// @param entry an entry of the registry
/// @return its constraints
[[nodiscard]] std::span<const Constraint> constraints_of(const Entry& entry) noexcept;

/// Returns the values of an enumeration.
///
/// @param spec the value's spec
/// @return its values, in the registry's order
[[nodiscard]] std::span<const std::string_view> enum_values_of(const ValueSpec& spec) noexcept;

/// Makes a value of a literal.
///
/// @param literal an index into the literals
/// @return the value
[[nodiscard]] Value literal_value(uint16_t literal);

/// What is wrong with a value, and where.
struct Problem {
    std::string path{}; ///< the value's path, such as hacks.veterancy.model.default-thresholds[1]
    formats::oamod::TextPosition
        position{}; ///< where the value was written; 0:0 when not in a file
    std::string message{};
};

/// Checks a value against a value spec and makes its normal form: a
/// decimal parameter holds a decimal even when written as an integer, and a
/// set lists its values in the registry's order.
///
/// @param spec the type and bounds
/// @param value the value
/// @param path the value's path, for the problem
/// @param[out] normal the normal form; unchanged on failure
/// @return the problem, or nullopt when the value fits
[[nodiscard]] std::optional<Problem>
check_value(const ValueSpec& spec, const Value& value, const std::string& path, Value& normal);

} // namespace oa::data::mod_profile::registry

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Read-only unit, weapon and feature records for a brain. The values come
// from the match's loaded records. Field readers do not call Lua.
#pragma once

#include "oa/core/feature_def.h"
#include "oa/core/unit_def.h"
#include "oa/core/weapon_def.h"
#include "oa/data/unit_definitions.hpp"

#include <cstdint>
#include <span>
#include <string_view>

struct lua_State;

namespace oa::sim::brains {

struct Seat;

/// One field of a loaded record, in the form a script sees.
///
/// `text` points at storage the caller owns: a record buffer, a definition
/// string, or a static mission name. `listed` is the unit definition whose
/// categories are the list, and it may be null for an empty list.
struct FieldValue {
    /// Which member holds the value.
    enum class Kind : uint8_t {
        integer, ///< a whole number, including a stored tick count
        fixed,   ///< a 16.16 count in `integer`
        real,    ///< a float
        boolean, ///< a flag bit
        text,    ///< characters in `text`
        list,    ///< category names on `listed`
        nil,     ///< a kept field with no value, such as a missing feature link
    };
    Kind kind{Kind::nil};
    int64_t integer{}; ///< the integer, or the 16.16 count when `kind` is fixed
    float real{};
    bool boolean{};
    std::string_view text{};
    const data::unit_definitions::UnitDefinition* listed{};
};

/// Reads one unit field from the loaded record.
///
/// A key the record does not keep, and a key the schema does not have, returns
/// false. The match is `unit` plus `definition` when the load kept typed
/// names there. `definition` may be null; name lists are then empty.
///
/// @param unit the loaded unit record
/// @param definition typed names for that type, or null
/// @param key field name, ignoring ASCII case
/// @param out receives the value when the function returns true
/// @return true when the record keeps the key
[[nodiscard]] bool read_unit_field(
    const oa::UnitDef& unit,
    const data::unit_definitions::UnitDefinition* definition,
    std::string_view key,
    FieldValue& out
) noexcept;

/// Reads one weapon field from the loaded record.
///
/// Sound and art names are not on this record. A key the record does not keep
/// returns false.
///
/// @param weapon the loaded weapon record
/// @param key field name, ignoring ASCII case
/// @param out receives the value when the function returns true
/// @return true when the record keeps the key
[[nodiscard]] bool
read_weapon_field(const oa::WeaponDef& weapon, std::string_view key, FieldValue& out) noexcept;

/// Reads one feature field from the loaded record.
///
/// `featuredead`, `featureburnt` and `featurereclamate` are the name of the
/// feature at that index, or nil when the link is empty or outside `table`.
///
/// @param feature the loaded feature record
/// @param table the match's feature records, so a link can name its target
/// @param key field name, ignoring ASCII case
/// @param out receives the value when the function returns true
/// @return true when the record keeps the key
[[nodiscard]] bool read_feature_field(
    const oa::FeatureDef& feature,
    std::span<const oa::FeatureDef> table,
    std::string_view key,
    FieldValue& out
) noexcept;

/// Why a unit schema key is not read, or null when the record keeps it.
///
/// An unknown name is null as well: it is not an omitted schema key.
///
/// @param key field name, ignoring ASCII case
/// @return the reason, or null
[[nodiscard]] const char* omitted_unit_reason(std::string_view key) noexcept;

/// Why a weapon schema key is not read, or null when the record keeps it.
///
/// An unknown name is null as well: it is not an omitted schema key.
///
/// @param key field name, ignoring ASCII case
/// @return the reason, or null
[[nodiscard]] const char* omitted_weapon_reason(std::string_view key) noexcept;

/// The feature field names, in the order `keys` lists them.
///
/// @return the names; each one is kept
[[nodiscard]] std::span<const std::string_view> feature_field_keys() noexcept;

/// Resolves a unit type from a record, a name or a type index.
///
/// A Lua number that is not an integer is not an index. An unknown name or
/// index raises `<function>: unknown unit type <text>`. Anything else raises
/// `<function> expects a unit type: a record, a name or an index`.
///
/// @param state sandbox state
/// @param index stack index of the value
/// @param function name used in the error
/// @param type receives the type index
/// @return true when `type` was written; does not return when the value is refused
bool resolve_unit_type(lua_State* state, int index, const char* function, uint16_t* type);

/// Installs `units`, `weapons` and `features`, and the unit getter `type`.
///
/// With no match the three collections exist and are empty. May raise.
///
/// @param state sandbox state
/// @param seat the seat being opened
void install_definitions(lua_State* state, Seat& seat);

} // namespace oa::sim::brains

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Developer Mode's list of the standard hacks: its areas, the hacks as the
// dialog shows them, where each row lies, the values each slider steps
// through, and what a press, a drag or a key changes in the chosen
// settings' overrides.

#include "developer.hpp"

#include "geometry.hpp"

#include "oa/base/text/line_break.hpp"
#include "oa/data/mod_profile/overrides.hpp"
#include "oa/data/mod_profile/registry.hpp"
#include "oa/formats/oamod.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <numeric>
#include <string>
#include <utility>
#include <vector>

namespace oa::ui::engine_settings {

namespace mod_profile = oa::data::mod_profile;
namespace registry = oa::data::mod_profile::registry;
namespace oamod = oa::formats::oamod;
using mod_profile::HackOverride;
using mod_profile::HackState;
using mod_profile::ParameterOverride;
using mod_profile::Value;
using mod_profile::ValueKind;
using registry::ValueType;

namespace {

/// Returns a list's item label, its number in its place.
///
/// @param number the item's number, from 1
/// @return the label
std::string item_text(std::size_t number) {
    return geometry::filled("Item {n}", {{"n", std::to_string(number)}});
}

/// The first printable ASCII character.
constexpr unsigned char first_printable = 0x20;
/// The first character past printable ASCII.
constexpr unsigned char past_printable = 0x7F;
/// The lead bytes of UTF-8 sequences of two, three and four bytes.
constexpr unsigned char two_byte_lead = 0xC0;
constexpr unsigned char three_byte_lead = 0xE0;
constexpr unsigned char four_byte_lead = 0xF0;

/// A character the game's fonts lack, in UTF-8, and the ASCII it is written as.
struct AsciiFold {
    std::string_view from; ///< the character
    std::string_view to;   ///< what is written for it
};

/// The characters the registry's summaries use that the game's fonts lack.
constexpr std::array<AsciiFold, 4> ascii_folds{{
    {"\xC2\xB0", " degrees"},
    {"\xC2\xB7", "*"},
    {"\xC3\x97", "x"},
    {"\xC2\xB1", "+/-"},
}};

/// The columns a character of a slider's name keeps beside its value: the
/// small font's widest letter, so that every name fits.
constexpr int32_t name_character_columns = 9;

/// The least decimal step a slider of decimals takes, as a power of ten.
constexpr int32_t least_decimal_exponent = -6;
/// About how many stops a slider of decimals has across its range.
constexpr double decimal_stops = 1000.0;

/// The value an int-or-none parameter holds for none.
constexpr std::string_view none_text = "none";

/// Returns a character with A to Z lowered, for comparing titles.
///
/// @param letter the character
/// @return it lowered, as an unsigned byte
unsigned char folded(char letter) noexcept {
    const auto byte = static_cast<unsigned char>(letter);
    return byte >= 'A' && byte <= 'Z' ? static_cast<unsigned char>(byte - 'A' + 'a') : byte;
}

/// Tells whether one title comes before another alphabetically: compared
/// without regard to the case of A to Z, and as written where that ties.
///
/// @param left a title
/// @param right another
/// @return true when left comes first
bool title_before(std::string_view left, std::string_view right) noexcept {
    const std::size_t common = std::min(left.size(), right.size());
    for (std::size_t at = 0; at < common; ++at)
        if (folded(left[at]) != folded(right[at]))
            return folded(left[at]) < folded(right[at]);
    if (left.size() != right.size())
        return left.size() < right.size();
    return left < right;
}

/// Returns a hack's title.
///
/// @param hack the hack's place among the standard hacks
/// @return its title in English
std::string_view hack_title(std::size_t hack) {
    return mod_profile::standard_hacks()[hack]->title;
}

/// Returns the areas, gathered once from the registry.
///
/// @return each area, alphabetically by its title in English, and its
///     hacks alphabetically by theirs
const std::vector<HackArea>& gathered_areas() {
    static const std::vector<HackArea> areas = [] {
        std::vector<HackArea> found;
        const auto hacks = mod_profile::standard_hacks();
        for (std::size_t index = 0; index < hacks.size(); ++index) {
            const std::string_view name = hacks[index]->area;
            auto at = std::find_if(found.begin(), found.end(), [&](const HackArea& area) {
                return area.name == name;
            });
            if (at == found.end())
                at = found.insert(found.end(), HackArea{name, registry::area_title(name), {}});
            at->hacks.push_back(index);
        }
        std::stable_sort(found.begin(), found.end(), [](const HackArea& a, const HackArea& b) {
            return title_before(a.title, b.title);
        });
        for (HackArea& area : found)
            std::stable_sort(
                area.hacks.begin(), area.hacks.end(), [](std::size_t a, std::size_t b) {
                    return title_before(hack_title(a), hack_title(b));
                }
            );
        return found;
    }();
    return areas;
}

/// Returns the order the list shows the areas in: alphabetically by their
/// titles in the language shown.
///
/// @return the areas' places among gathered_areas, in the list's order
std::vector<std::size_t> shown_area_order() {
    const auto& areas = gathered_areas();
    std::vector<std::size_t> order(areas.size());
    std::iota(order.begin(), order.end(), std::size_t{0});
    std::stable_sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return title_before(
            geometry::shown_text(areas[a].title), geometry::shown_text(areas[b].title)
        );
    });
    return order;
}

/// Returns the order the list shows an area's hacks in: alphabetically by
/// their titles in the language shown.
///
/// @param area the area
/// @return its hacks' places among the standard hacks, in the list's order
std::vector<std::size_t> shown_hack_order(const HackArea& area) {
    std::vector<std::size_t> order = area.hacks;
    std::stable_sort(order.begin(), order.end(), [](std::size_t a, std::size_t b) {
        return title_before(
            geometry::shown_text(hack_title(a)), geometry::shown_text(hack_title(b))
        );
    });
    return order;
}

/// Returns a hack as the profile resolves it.
///
/// @param dialog the dialog
/// @param hack the hack's place among the standard hacks
/// @return its state; off at its baselines when the dialog holds none
const HackState& profile_state(const Dialog& dialog, std::size_t hack) {
    if (hack < dialog.developer.profile.size())
        return dialog.developer.profile[hack];
    static const std::vector<HackState> base = mod_profile::base_hack_states();
    return base[hack];
}

/// Tells whether two parameters' values keep a constraint, as the resolver
/// tells it: a none on either side keeps every constraint.
///
/// @param constraint the constraint
/// @param values the hack's values, by parameter index
/// @return true when it holds
bool constraint_holds(const registry::Constraint& constraint, const std::vector<Value>& values) {
    if (constraint.left >= values.size() || constraint.right >= values.size())
        return true;
    const Value& left = values[constraint.left];
    const Value& right = values[constraint.right];
    if (left.kind != ValueKind::number || right.kind != ValueKind::number)
        return true;
    const int order = oamod::compare_numbers(left.number, right.number);
    switch (constraint.comparison) {
    case registry::Comparison::less_equal:
        return order <= 0;
    case registry::Comparison::less:
        return order < 0;
    case registry::Comparison::greater_equal:
        return order >= 0;
    case registry::Comparison::greater:
        return order > 0;
    }
    return false;
}

/// Tells whether an override fits its hack as the resolver checks one:
/// known parameters, each value fitting its parameter, the hack implemented
/// to be turned on, and its constraints kept.
///
/// @param hack the hack
/// @param profile the hack as the profile resolves it
/// @param override the override
/// @return true when the resolver lays it
bool override_fits(
    const registry::Entry& hack, const HackState& profile, const HackOverride& override
) {
    if (!override.on)
        return true;
    if (!hack.implemented)
        return false;
    const auto parameters = registry::parameters_of(hack);
    for (const ParameterOverride& parameter : override.parameters) {
        const int index = registry::find_parameter(hack, parameter.name);
        if (index < 0)
            return false;
        Value normal{};
        if (registry::check_value(
                parameters[static_cast<std::size_t>(index)].value, parameter.value, {}, normal
            ))
            return false;
    }
    const HackState state = mod_profile::overridden_state(hack, profile, &override);
    for (const registry::Constraint& constraint : registry::constraints_of(hack))
        if (!constraint_holds(constraint, state.values))
            return false;
    return true;
}

/// Returns the override that applies to a hack as the dialog shows it.
///
/// @param dialog the dialog
/// @param hack the hack's place among the standard hacks
/// @return its override while Developer Mode is on and the override fits;
///     null otherwise
const HackOverride* shown_override(const Dialog& dialog, std::size_t hack) {
    if (!dialog.chosen.developer_mode)
        return nullptr;
    const registry::Entry& entry = *mod_profile::standard_hacks()[hack];
    const HackOverride* found = mod_profile::find_override(dialog.chosen.hack_overrides, entry.id);
    if (found == nullptr || !override_fits(entry, profile_state(dialog, hack), *found))
        return nullptr;
    return found;
}

/// Tells whether an area or a hack is open.
///
/// @param flags the open flags, by place
/// @param index the place
/// @return true when its flag is set
bool is_open(const std::vector<uint8_t>& flags, std::size_t index) noexcept {
    return index < flags.size() && flags[index] != 0;
}

/// Opens or closes one of a list of parts, growing the flags to hold it.
///
/// @param[in,out] flags the open flags, by place
/// @param index the place
/// @param count every part there is
/// @param open true to open it
/// @return true when it changed
bool set_open(std::vector<uint8_t>& flags, std::size_t index, std::size_t count, bool open) {
    if (flags.size() < count)
        flags.resize(count, 0);
    if (index >= flags.size() || (flags[index] != 0) == open)
        return false;
    flags[index] = open ? 1 : 0;
    return true;
}

/// Returns the type of one item of a list type.
///
/// @param type a list type
/// @return the type of its items; the type itself for any other
ValueType item_type(ValueType type) noexcept {
    switch (type) {
    case ValueType::integer_list:
        return ValueType::integer;
    case ValueType::decimal_list:
        return ValueType::decimal;
    case ValueType::enumeration_list:
    case ValueType::enumeration_set:
        return ValueType::enumeration;
    case ValueType::string_list:
        return ValueType::string;
    default:
        return type;
    }
}

/// Tells whether a type holds a list of items, each its own row.
///
/// @param type the type
/// @return true for the list types; a set is not one
bool list_type(ValueType type) noexcept {
    return type == ValueType::integer_list || type == ValueType::decimal_list ||
           type == ValueType::enumeration_list || type == ValueType::string_list;
}

/// Makes a decimal number of a whole count of a power of ten.
///
/// @param count the count, either sign
/// @param exponent the power of ten
/// @return the number, its significand without trailing zeros
oamod::Number decimal_number(int64_t count, int32_t exponent) noexcept {
    oamod::Number number{};
    number.integer = false;
    number.negative = count < 0;
    uint64_t significand =
        count < 0 ? 0U - static_cast<uint64_t>(count) : static_cast<uint64_t>(count);
    if (significand == 0)
        return number;
    while (significand % 10U == 0) {
        significand /= 10U;
        ++exponent;
    }
    number.significand = significand;
    number.exponent = exponent;
    return number;
}

/// Reads a whole number.
///
/// @param value the value
/// @param[out] whole the number; left alone unless it is one
/// @return true for a number with no fraction within the profile's bounds
bool whole_number(const Value& value, int64_t& whole) noexcept {
    return value.kind == ValueKind::number && oamod::integer_value(value.number, whole);
}

/// The values a slider of the list steps through.
struct Scale {
    /// Whole numbers, from lowest by step: a number, an int-or-none or a list's length.
    bool integers{};
    int64_t lowest{};          ///< whole numbers: the first stop's value
    int64_t step{1};           ///< whole numbers: between two stops
    int64_t count{1};          ///< whole numbers: their stops, 1 or more
    bool none_first{};         ///< whole numbers: a stop for none comes before them
    std::vector<Value> values; ///< otherwise each stop's value, in order
};

/// Counts a scale's stops.
///
/// @param scale the scale
/// @return 1 or more
int32_t stop_count(const Scale& scale) noexcept {
    const int64_t stops = scale.integers ? scale.count + (scale.none_first ? 1 : 0)
                                         : static_cast<int64_t>(scale.values.size());
    return static_cast<int32_t>(std::clamp<int64_t>(stops, 1, std::numeric_limits<int32_t>::max()));
}

/// Returns a stop's value.
///
/// @param scale the scale
/// @param stop the stop, clamped to the scale's
/// @return the value
Value value_at(const Scale& scale, int32_t stop) {
    int64_t at = std::clamp(stop, int32_t{0}, stop_count(scale) - 1);
    if (scale.integers) {
        if (scale.none_first) {
            if (at == 0)
                return mod_profile::make_string(none_text);
            --at;
        }
        return mod_profile::make_integer(scale.lowest + at * scale.step);
    }
    if (scale.values.empty())
        return Value{};
    return scale.values[static_cast<std::size_t>(at)];
}

/// Returns the stop nearest a value.
///
/// @param scale the scale
/// @param value the value
/// @return the stop, 0 to the last
int32_t stop_nearest(const Scale& scale, const Value& value) {
    if (scale.integers) {
        const int32_t first = scale.none_first ? 1 : 0;
        if (value.kind == ValueKind::string && value.text == none_text)
            return 0;
        int64_t whole = scale.lowest;
        if (!whole_number(value, whole) && value.kind == ValueKind::number)
            whole = static_cast<int64_t>(std::llround(oamod::number_to_double(value.number)));
        const int64_t last = scale.lowest + (scale.count - 1) * scale.step;
        whole = std::clamp(whole, scale.lowest, last);
        return first + static_cast<int32_t>((whole - scale.lowest + scale.step / 2) / scale.step);
    }
    int32_t nearest = 0;
    double nearest_distance = std::numeric_limits<double>::infinity();
    for (std::size_t index = 0; index < scale.values.size(); ++index) {
        const Value& candidate = scale.values[index];
        if (mod_profile::values_equal(candidate, value))
            return static_cast<int32_t>(index);
        if (candidate.kind == ValueKind::number && value.kind == ValueKind::number) {
            const double distance = std::abs(
                oamod::number_to_double(candidate.number) - oamod::number_to_double(value.number)
            );
            if (distance < nearest_distance) {
                nearest_distance = distance;
                nearest = static_cast<int32_t>(index);
            }
        }
    }
    return nearest;
}

/// Returns the values the registry and the profile know for a parameter, or
/// for one item of it: its baseline, its default, its presets' values, the
/// profile's value and the one shown, each once, in that order.
///
/// @param dialog the dialog
/// @param shown the hacks as the dialog shows them
/// @param hack the hack's place among the standard hacks
/// @param parameter the parameter's index within the hack
/// @param item an item of a list, or whole_parameter
/// @return the values
std::vector<Value> known_values(
    const Dialog& dialog,
    const std::vector<HackState>& shown,
    std::size_t hack,
    int32_t parameter,
    int32_t item
) {
    const registry::Entry& entry = *mod_profile::standard_hacks()[hack];
    const registry::Parameter& spec =
        registry::parameters_of(entry)[static_cast<std::size_t>(parameter)];
    std::vector<Value> found;
    const auto add = [&](const Value& value) {
        const Value* candidate = &value;
        if (item >= 0) {
            if (value.kind != ValueKind::list ||
                static_cast<std::size_t>(item) >= value.items.size())
                return;
            candidate = &value.items[static_cast<std::size_t>(item)];
        }
        for (const Value& known : found)
            if (mod_profile::values_equal(known, *candidate))
                return;
        Value kept = *candidate;
        kept.position = {};
        found.push_back(std::move(kept));
    };
    add(registry::literal_value(spec.value.baseline));
    add(registry::literal_value(spec.default_value));
    for (const registry::Preset& preset : registry::presets_of(entry)) {
        const auto values =
            registry::table().preset_values.subspan(preset.first_value, preset.value_count);
        for (const registry::PresetValue& value : values)
            if (value.parameter == static_cast<uint16_t>(parameter))
                add(registry::literal_value(value.literal));
    }
    const HackState& profile = profile_state(dialog, hack);
    if (static_cast<std::size_t>(parameter) < profile.values.size())
        add(profile.values[static_cast<std::size_t>(parameter)]);
    add(shown[hack].values[static_cast<std::size_t>(parameter)]);
    return found;
}

/// Narrows a whole-number range to what a hack's constraints allow one of
/// its parameters, given the others' values.
///
/// @param entry the hack
/// @param parameter the parameter's index within it
/// @param values the hack's values, by parameter index
/// @param[in,out] low the range's lowest value
/// @param[in,out] high the range's highest value
void constrain_whole(
    const registry::Entry& entry,
    int32_t parameter,
    const std::vector<Value>& values,
    int64_t& low,
    int64_t& high
) {
    for (const registry::Constraint& constraint : registry::constraints_of(entry)) {
        const bool left = constraint.left == static_cast<uint16_t>(parameter);
        if (!left && constraint.right != static_cast<uint16_t>(parameter))
            continue;
        const uint16_t other = left ? constraint.right : constraint.left;
        int64_t bound = 0;
        if (other >= values.size() || !whole_number(values[other], bound))
            continue;
        // The parameter on the left reads "parameter op other"; on the right
        // "other op parameter", which bounds it from the other side.
        const bool below = left == (constraint.comparison == registry::Comparison::less_equal ||
                                    constraint.comparison == registry::Comparison::less);
        const bool strict = constraint.comparison == registry::Comparison::less ||
                            constraint.comparison == registry::Comparison::greater;
        if (below)
            high = std::min(high, bound - (strict ? 1 : 0));
        else
            low = std::max(low, bound + (strict ? 1 : 0));
    }
}

/// Returns a whole-number bound of a value spec.
///
/// @param has whether the spec has the bound
/// @param bound the bound
/// @param otherwise what to use without it
/// @return the bound, or `otherwise`
int64_t whole_bound(bool has, const oamod::Number& bound, int64_t otherwise) noexcept {
    int64_t whole = otherwise;
    if (has && !oamod::integer_value(bound, whole))
        whole = static_cast<int64_t>(oamod::number_to_double(bound));
    return whole;
}

/// The widest range a whole-number slider takes when its spec gives no bound.
constexpr int64_t unbounded_reach = 1000;

/// Returns the values a slider of the list steps through.
///
/// @param dialog the dialog
/// @param shown the hacks as the dialog shows them
/// @param hack the hack's place among the standard hacks
/// @param parameter the parameter's index within the hack
/// @param item an item of a list, whole_parameter, or list_length
/// @return the scale
Scale scale_of(
    const Dialog& dialog,
    const std::vector<HackState>& shown,
    std::size_t hack,
    int32_t parameter,
    int32_t item
) {
    const registry::Entry& entry = *mod_profile::standard_hacks()[hack];
    const registry::ValueSpec& spec =
        registry::parameters_of(entry)[static_cast<std::size_t>(parameter)].value;
    const std::vector<Value>& values = shown[hack].values;
    const Value& current = values[static_cast<std::size_t>(parameter)];
    Scale scale{};
    if (item == geometry::list_length) {
        const auto count = static_cast<int64_t>(current.items.size());
        int64_t low = spec.has_length ? spec.length_minimum : 0;
        int64_t high = spec.has_length ? spec.length_maximum : count + 1;
        // An ascending list of whole numbers grows only while there is room
        // above its last item.
        int64_t last = 0;
        if (spec.type == ValueType::integer_list && spec.ascending && spec.has_maximum &&
            !current.items.empty() && whole_number(current.items.back(), last))
            high = std::min(high, count + whole_bound(true, spec.maximum, last) - last);
        high = std::max(high, low);
        scale.integers = true;
        scale.lowest = low;
        scale.count = high - low + 1;
        return scale;
    }
    const ValueType type = item >= 0 ? item_type(spec.type) : spec.type;
    const Value& shown_value = item >= 0 && static_cast<std::size_t>(item) < current.items.size()
                                   ? current.items[static_cast<std::size_t>(item)]
                                   : current;
    switch (type) {
    case ValueType::integer:
    case ValueType::integer_or_none: {
        int64_t whole = 0;
        const bool has_whole = whole_number(shown_value, whole);
        int64_t low = whole_bound(spec.has_minimum, spec.minimum, std::min<int64_t>(whole, 0));
        int64_t high = whole_bound(
            spec.has_maximum, spec.maximum, std::max(low, has_whole ? whole : low) + unbounded_reach
        );
        if (item >= 0 && spec.ascending) {
            int64_t neighbour = 0;
            const auto at = static_cast<std::size_t>(item);
            if (at > 0 && whole_number(current.items[at - 1], neighbour))
                low = std::max(low, neighbour + 1);
            if (at + 1 < current.items.size() && whole_number(current.items[at + 1], neighbour))
                high = std::min(high, neighbour - 1);
        }
        if (item < 0)
            constrain_whole(entry, parameter, values, low, high);
        int64_t step = 1;
        if (spec.has_multiple_of)
            step = std::max<int64_t>(whole_bound(true, spec.multiple_of, 1), 1);
        scale.integers = true;
        scale.none_first = type == ValueType::integer_or_none;
        // The first multiple of the step at or above the lowest value.
        int64_t first = low - ((low % step) + step) % step;
        if (first < low)
            first += step;
        if (high < first) {
            // Nothing between the bounds: the value stays where it is.
            scale.lowest = has_whole ? whole : low;
            scale.count = 1;
            return scale;
        }
        scale.lowest = first;
        scale.count = (high - first) / step + 1;
        return scale;
    }
    case ValueType::decimal: {
        double low = spec.has_minimum ? oamod::number_to_double(spec.minimum) : 0.0;
        double high = spec.has_maximum ? oamod::number_to_double(spec.maximum) : low + 1.0;
        // A power of ten that gives about decimal_stops stops across the range.
        int32_t exponent = 0;
        double unit = 1.0;
        while (unit * 10.0 * decimal_stops <= high - low) {
            unit *= 10.0;
            ++exponent;
        }
        while (unit * decimal_stops > high - low && exponent > least_decimal_exponent) {
            unit /= 10.0;
            --exponent;
        }
        std::vector<Value> candidates;
        const auto first = static_cast<int64_t>(std::ceil(low / unit));
        const auto last = static_cast<int64_t>(std::floor(high / unit));
        for (int64_t count = first; count <= last; ++count)
            candidates.push_back(mod_profile::make_number(decimal_number(count, exponent)));
        for (const Value& known : known_values(dialog, shown, hack, parameter, item))
            if (known.kind == ValueKind::number)
                candidates.push_back(known);
        // Each candidate must fit the parameter and keep the hack's
        // constraints, and a list's ascending order.
        registry::ValueSpec item_spec = spec;
        item_spec.type = ValueType::decimal;
        for (Value& candidate : candidates) {
            Value normal{};
            if (registry::check_value(item_spec, candidate, {}, normal))
                continue;
            if (item >= 0) {
                Value list = current;
                list.items[static_cast<std::size_t>(item)] = normal;
                Value checked{};
                if (registry::check_value(spec, list, {}, checked))
                    continue;
            } else {
                std::vector<Value> trial = values;
                trial[static_cast<std::size_t>(parameter)] = normal;
                bool kept = true;
                for (const registry::Constraint& constraint : registry::constraints_of(entry))
                    kept = kept && constraint_holds(constraint, trial);
                if (!kept)
                    continue;
            }
            scale.values.push_back(std::move(normal));
        }
        std::sort(scale.values.begin(), scale.values.end(), [](const Value& a, const Value& b) {
            return oamod::compare_numbers(a.number, b.number) < 0;
        });
        scale.values.erase(
            std::unique(
                scale.values.begin(),
                scale.values.end(),
                [](const Value& a, const Value& b) {
                    return oamod::compare_numbers(a.number, b.number) == 0;
                }
            ),
            scale.values.end()
        );
        if (scale.values.empty())
            scale.values.push_back(shown_value);
        return scale;
    }
    case ValueType::enumeration:
        for (const std::string_view word : registry::enum_values_of(spec))
            scale.values.push_back(mod_profile::make_string(word));
        return scale;
    case ValueType::string: {
        registry::ValueSpec item_spec = spec;
        item_spec.type = ValueType::string;
        for (const Value& known : known_values(dialog, shown, hack, parameter, item)) {
            Value normal{};
            if (known.kind == ValueKind::string &&
                !registry::check_value(item_spec, known, {}, normal))
                scale.values.push_back(known);
        }
        if (scale.values.empty())
            scale.values.push_back(shown_value);
        return scale;
    }
    default:
        scale.values.push_back(shown_value);
        return scale;
    }
}

/// Returns the item a list grows by: one above the last of an ascending
/// list of whole numbers, the first word a list of distinct words lacks,
/// else a copy of the last.
///
/// @param spec the list's spec
/// @param list the list
/// @return the item
Value next_item(const registry::ValueSpec& spec, const Value& list) {
    int64_t last = 0;
    if (!list.items.empty() && spec.type == ValueType::integer_list && spec.ascending &&
        whole_number(list.items.back(), last))
        return mod_profile::make_integer(last + 1);
    if (spec.type == ValueType::enumeration_list && spec.distinct) {
        for (const std::string_view word : registry::enum_values_of(spec)) {
            const bool held =
                std::any_of(list.items.begin(), list.items.end(), [&](const Value& item) {
                    return item.text == word;
                });
            if (!held)
                return mod_profile::make_string(word);
        }
    }
    if (!list.items.empty())
        return list.items.back();
    if (spec.type == ValueType::integer_list)
        return mod_profile::make_integer(whole_bound(spec.has_minimum, spec.minimum, 0));
    if (spec.type == ValueType::decimal_list)
        return mod_profile::make_number(spec.has_minimum ? spec.minimum : decimal_number(0, 0));
    if (spec.type == ValueType::enumeration_list && spec.value_count > 0)
        return mod_profile::make_string(registry::enum_values_of(spec).front());
    return mod_profile::make_string("");
}

/// Returns a hack's override for changing, made when it has none; one that
/// does not fit is started afresh.
///
/// @param[in,out] dialog the dialog
/// @param hack the hack's place among the standard hacks
/// @return the override
HackOverride& override_of(Dialog& dialog, std::size_t hack) {
    const registry::Entry& entry = *mod_profile::standard_hacks()[hack];
    const HackState& profile = profile_state(dialog, hack);
    for (HackOverride& override : dialog.chosen.hack_overrides) {
        if (override.hack != entry.id)
            continue;
        if (!override_fits(entry, profile, override)) {
            override.on = profile.on;
            override.parameters.clear();
        }
        return override;
    }
    HackOverride& made = dialog.chosen.hack_overrides.emplace_back();
    made.hack = std::string(entry.id);
    made.on = profile.on;
    return made;
}

/// Drops a hack's override when it changes nothing of the profile.
///
/// @param[in,out] dialog the dialog
/// @param hack the hack's place among the standard hacks
void tidy(Dialog& dialog, std::size_t hack) {
    const registry::Entry& entry = *mod_profile::standard_hacks()[hack];
    const HackState& profile = profile_state(dialog, hack);
    std::erase_if(dialog.chosen.hack_overrides, [&](const HackOverride& override) {
        return override.hack == entry.id && override.on == profile.on &&
               override.parameters.empty();
    });
}

/// Turns a hack on or off; off drops the parameters it set.
///
/// @param[in,out] dialog the dialog
/// @param hack the hack's place among the standard hacks
/// @param on true to turn it on
void set_hack_on(Dialog& dialog, std::size_t hack, bool on) {
    HackOverride& override = override_of(dialog, hack);
    override.on = on;
    if (!on)
        override.parameters.clear();
    tidy(dialog, hack);
}

/// Sets one parameter of a hack that is on: the override keeps the value
/// unless it is the one the hack has without it.
///
/// @param[in,out] dialog the dialog
/// @param hack the hack's place among the standard hacks
/// @param parameter the parameter's index within the hack
/// @param value its value
void set_parameter(Dialog& dialog, std::size_t hack, int32_t parameter, Value value) {
    const registry::Entry& entry = *mod_profile::standard_hacks()[hack];
    const std::vector<Value> without = mod_profile::on_values(entry, profile_state(dialog, hack));
    const std::string name{
        registry::parameters_of(entry)[static_cast<std::size_t>(parameter)].name
    };
    HackOverride& override = override_of(dialog, hack);
    override.on = true;
    std::erase_if(override.parameters, [&](const ParameterOverride& set) {
        return set.name == name;
    });
    value.position = {};
    if (!mod_profile::values_equal(value, without[static_cast<std::size_t>(parameter)]))
        override.parameters.push_back(ParameterOverride{name, std::move(value)});
    tidy(dialog, hack);
}

/// Reports whether the chosen settings changed.
///
/// @param dialog the dialog after the event
/// @param before the chosen settings before it
/// @return DialogAction::changed when they differ, else DialogAction::redraw
DialogAction changed_or_redraw(const Dialog& dialog, const EngineSettings& before) {
    return before == dialog.chosen ? DialogAction::redraw : DialogAction::changed;
}

/// Sets a switch of the list: a hack's, a switch parameter's, or one value
/// of a set's.
///
/// @param[in,out] dialog the dialog
/// @param row the row
/// @param on true for On
/// @return what it asks of the host
DialogAction set_toggle(Dialog& dialog, const geometry::ListRow& row, bool on) {
    if (row.locked)
        return DialogAction::redraw;
    const EngineSettings before = dialog.chosen;
    if (row.kind == geometry::ListRowKind::hack) {
        set_hack_on(dialog, row.hack, on);
        return changed_or_redraw(dialog, before);
    }
    if (row.kind != geometry::ListRowKind::toggle)
        return DialogAction::none;
    const std::vector<HackState> shown = shown_hacks(dialog);
    const registry::Entry& entry = *mod_profile::standard_hacks()[row.hack];
    const registry::ValueSpec& spec =
        registry::parameters_of(entry)[static_cast<std::size_t>(row.parameter)].value;
    if (spec.type == ValueType::enumeration_set) {
        const auto words = registry::enum_values_of(spec);
        const Value& held = shown[row.hack].values[static_cast<std::size_t>(row.parameter)];
        Value set = mod_profile::make_list();
        // The set keeps the registry's order of its words.
        for (std::size_t index = 0; index < words.size(); ++index) {
            const bool member =
                std::any_of(held.items.begin(), held.items.end(), [&](const Value& item) {
                    return item.text == words[index];
                });
            const bool kept = static_cast<int32_t>(index) == row.item ? on : member;
            if (kept)
                set.items.push_back(mod_profile::make_string(words[index]));
        }
        set_parameter(dialog, row.hack, row.parameter, std::move(set));
    } else {
        set_parameter(dialog, row.hack, row.parameter, mod_profile::make_boolean(on));
    }
    return changed_or_redraw(dialog, before);
}

/// Sets a slider of the list to a stop.
///
/// @param[in,out] dialog the dialog
/// @param row the slider's row
/// @param stop the stop, clamped to the slider's
/// @return what it asks of the host
DialogAction set_stop(Dialog& dialog, const geometry::ListRow& row, int32_t stop) {
    if (row.locked || row.kind != geometry::ListRowKind::slider)
        return DialogAction::redraw;
    const EngineSettings before = dialog.chosen;
    const std::vector<HackState> shown = shown_hacks(dialog);
    const registry::Entry& entry = *mod_profile::standard_hacks()[row.hack];
    const registry::ValueSpec& spec =
        registry::parameters_of(entry)[static_cast<std::size_t>(row.parameter)].value;
    const Scale scale = scale_of(dialog, shown, row.hack, row.parameter, row.item);
    const Value chosen = value_at(scale, stop);
    const Value& current = shown[row.hack].values[static_cast<std::size_t>(row.parameter)];
    if (row.item == geometry::whole_parameter) {
        set_parameter(dialog, row.hack, row.parameter, chosen);
        return changed_or_redraw(dialog, before);
    }
    Value list = current;
    if (row.item == geometry::list_length) {
        int64_t count = 0;
        if (!whole_number(chosen, count) || count < 0)
            return DialogAction::redraw;
        while (static_cast<int64_t>(list.items.size()) > count)
            list.items.pop_back();
        while (static_cast<int64_t>(list.items.size()) < count)
            list.items.push_back(next_item(spec, list));
    } else {
        const auto at = static_cast<std::size_t>(row.item);
        if (at >= list.items.size())
            return DialogAction::redraw;
        // A list of distinct words swaps the word in with the item that held it.
        if (spec.distinct) {
            for (Value& other : list.items)
                if (mod_profile::values_equal(other, chosen))
                    other = list.items[at];
        }
        list.items[at] = chosen;
    }
    set_parameter(dialog, row.hack, row.parameter, std::move(list));
    return changed_or_redraw(dialog, before);
}

/// Places a text row under a hack.
///
/// @param sized the dialog's sizes at its size class
/// @param[in,out] list the list
/// @param kind id, text or scope
/// @param hack the hack's place among the standard hacks
/// @param area the area's place
/// @param text the line
/// @param[in,out] top the row the line starts at; left under it
void place_text(
    const geometry::Sizes& sized,
    geometry::List& list,
    geometry::ListRowKind kind,
    std::size_t area,
    std::size_t hack,
    std::string text,
    int32_t& top
) {
    const int32_t left = sized.content_left + geometry::hack_text_offset;
    geometry::ListRow& row = list.rows.emplace_back();
    row.kind = kind;
    row.area = area;
    row.hack = hack;
    row.top = top;
    row.height = geometry::list_text_height;
    row.text = std::move(text);
    row.label = {left, top, sized.content_right - left, geometry::list_text_height};
    top += row.height;
}

/// Stands a switch under a name that does not fit beside it, and gives the
/// name the whole of that line. A missing measure leaves the class's row.
///
/// @param[in,out] row the row
/// @param[in,out] switch_rect the switch
/// @param small_width a text's width in the small font; null leaves the row
void drop_switch_under_name(
    geometry::ListRow& row,
    geometry::SourceRect& switch_rect,
    const geometry::TextWidth* small_width
) {
    if (small_width == nullptr || row.label.width <= 0)
        return;
    const int32_t need = (*small_width)(std::string(geometry::shown_text(row.text)));
    if (need <= row.label.width)
        return;
    const int32_t right = switch_rect.x + switch_rect.width;
    const int32_t extra = geometry::label_line_height + 2;
    row.label.width = std::max(right - row.label.x, int32_t{1});
    switch_rect.y += extra;
    row.height += extra;
    if (row.kind == geometry::ListRowKind::hack)
        row.control_area.height += extra;
}

/// Gives a slider's value the width it measures, and the name what remains
/// on the line. A missing measure leaves the class's boxes.
///
/// @param[in,out] row the slider
/// @param small_width a text's width in the small font; null leaves the row
/// @param right the section's right column
void fit_slider_words(
    geometry::ListRow& row, const geometry::TextWidth* small_width, int32_t right
) {
    if (small_width == nullptr)
        return;
    const int32_t gap = geometry::label_gap;
    const int32_t width = std::max(right - row.label.x, int32_t{1});
    const int32_t value_need =
        row.shown.empty() ? 0 : (*small_width)(std::string(geometry::shown_text(row.shown))) + 8;
    const int32_t name_need = (*small_width)(std::string(geometry::shown_text(row.text)));
    int32_t value_columns = std::min(std::max(value_need, int32_t{1}), width);
    int32_t name_columns = name_need;
    if (name_columns + gap + value_columns > width)
        name_columns = std::max(width - gap - value_columns, int32_t{0});
    row.label.width = name_columns;
    const int32_t value_left = row.label.x + name_columns + (name_columns > 0 ? gap : 0);
    row.value.x = value_left;
    row.value.width = std::max(right - value_left, int32_t{0});
}

/// Places a parameter's switch: a switch parameter's, or one of a set's values.
///
/// @param sized the dialog's sizes at its size class
/// @param[in,out] list the list
/// @param left the name's column
/// @param text the name
/// @param top the row's first row
/// @param[in,out] control the next control's number, taken by the row
/// @return the row, for its caller to fill in
geometry::ListRow& place_toggle(
    const geometry::Sizes& sized,
    geometry::List& list,
    int32_t left,
    std::string text,
    int32_t top,
    int32_t& control,
    int32_t switch_columns,
    const geometry::TextWidth* small_width
) {
    const int32_t columns = switch_columns > 0 ? switch_columns : geometry::switch_width;
    geometry::ListRow& row = list.rows.emplace_back();
    row.kind = geometry::ListRowKind::toggle;
    row.control = control++;
    row.top = top;
    row.height = geometry::list_toggle_height;
    row.text = std::move(text);
    row.control_area = {
        sized.content_right - columns, top + 2, columns, geometry::label_line_height
    };
    row.label = {
        left, top + 2, row.control_area.x - geometry::label_gap - left, geometry::label_line_height
    };
    drop_switch_under_name(row, row.control_area, small_width);
    return row;
}

/// Places a parameter's slider: its name and value over its track.
///
/// @param sized the dialog's sizes at its size class
/// @param[in,out] list the list
/// @param left the name's column
/// @param text the name
/// @param top the row's first row
/// @param[in,out] control the next control's number, taken by the row
/// @return the row, for its caller to fill in
geometry::ListRow& place_slider(
    const geometry::Sizes& sized,
    geometry::List& list,
    int32_t left,
    std::string text,
    int32_t top,
    int32_t& control
) {
    geometry::ListRow& row = list.rows.emplace_back();
    row.kind = geometry::ListRowKind::slider;
    row.control = control++;
    row.top = top;
    row.height = geometry::list_slider_height;
    const int32_t width = sized.content_right - left;
    const int32_t name_width =
        std::min(static_cast<int32_t>(text.size()) * name_character_columns, width * 2 / 3);
    row.label = {left, top + 2, name_width, geometry::hint_line_height};
    const int32_t value_left = left + name_width + geometry::label_gap;
    row.value = {value_left, top + 2, sized.content_right - value_left, geometry::hint_line_height};
    row.control_area = {
        left, top + 2 + geometry::hint_line_height + 2, width, geometry::slider_line_height
    };
    row.text = std::move(text);
    return row;
}

/// Places one hack's header and, open, what is under it.
///
/// @param dialog the dialog
/// @param shown the hacks as the dialog shows them
/// @param area the area's place
/// @param hack the hack's place among the standard hacks
/// @param[in,out] list the list
/// @param[in,out] top the row the hack starts at; left under it
/// @param[in,out] control the next control's number
void place_hack(
    const Dialog& dialog,
    const std::vector<HackState>& shown,
    std::size_t area,
    std::size_t hack,
    geometry::List& list,
    int32_t& top,
    int32_t& control,
    int32_t switch_columns,
    const geometry::TextWidth* small_width,
    const geometry::Sizes& sized
) {
    using geometry::ListRow;
    using geometry::ListRowKind;
    const registry::Entry& entry = *mod_profile::standard_hacks()[hack];
    const HackState& state = shown[hack];
    const bool editable = dialog.chosen.developer_mode;
    {
        ListRow& header = list.rows.emplace_back();
        header.kind = ListRowKind::hack;
        header.control = control++;
        header.area = area;
        header.hack = hack;
        header.top = top;
        header.height = geometry::list_header_height;
        header.text = std::string(entry.title);
        header.arrow = {
            sized.content_left + geometry::hack_arrow_offset,
            top + (geometry::list_header_height - geometry::arrow_side) / 2,
            geometry::arrow_side,
            geometry::arrow_side
        };
        const int32_t columns = switch_columns > 0 ? switch_columns : geometry::switch_width;
        header.toggle = {
            sized.content_right - columns, top + 2, columns, geometry::label_line_height
        };
        const int32_t left = sized.content_left + geometry::hack_text_offset;
        header.label = {
            left, top + 2, header.toggle.x - geometry::label_gap - left, geometry::label_line_height
        };
        header.control_area = {
            sized.content_left, top, sized.content_width, geometry::list_header_height
        };
        header.open = is_open(dialog.developer.hacks_open, hack);
        header.on = state.on;
        header.locked = !editable || (!entry.implemented && !state.on);
        drop_switch_under_name(header, header.toggle, small_width);
        top += header.height;
        if (!header.open)
            return;
    }
    top += geometry::list_body_gap;
    place_text(sized, list, ListRowKind::id, area, hack, std::string(entry.id), top);
    for (std::string& line : geometry::summary_lines(entry.summary))
        place_text(sized, list, ListRowKind::text, area, hack, std::move(line), top);
    if (entry.scope == registry::Scope::sim)
        place_text(
            sized, list, ListRowKind::scope, area, hack, std::string(geometry::next_match_text), top
        );
    if (!entry.implemented)
        place_text(
            sized,
            list,
            ListRowKind::text,
            area,
            hack,
            std::string(geometry::not_implemented_text),
            top
        );
    if (!state.on) {
        place_text(
            sized, list, ListRowKind::text, area, hack, std::string(geometry::hack_off_text), top
        );
        top += geometry::list_body_gap;
        return;
    }
    const auto parameters = registry::parameters_of(entry);
    const int32_t left = sized.content_left + geometry::hack_text_offset;
    const int32_t item_left = sized.content_left + geometry::item_text_offset;
    const auto own = [&](ListRow& row, int32_t parameter, int32_t item) {
        row.area = area;
        row.hack = hack;
        row.parameter = parameter;
        row.item = item;
        row.locked = !editable;
    };
    const auto fill_slider = [&](ListRow& row) {
        const Scale scale = scale_of(dialog, shown, hack, row.parameter, row.item);
        const Value& value = state.values[static_cast<std::size_t>(row.parameter)];
        const std::string_view unit = parameters[static_cast<std::size_t>(row.parameter)].unit;
        row.stops = stop_count(scale);
        if (row.item == geometry::list_length) {
            const Value count = mod_profile::make_integer(static_cast<int64_t>(value.items.size()));
            row.stop = stop_nearest(scale, count);
            row.shown = std::to_string(value.items.size());
        } else if (row.item >= 0) {
            const Value& item = value.items[static_cast<std::size_t>(row.item)];
            row.stop = stop_nearest(scale, item);
            row.shown = geometry::list_value_text(item, unit);
        } else {
            row.stop = stop_nearest(scale, value);
            row.shown = geometry::list_value_text(value, unit);
        }
        fit_slider_words(row, small_width, sized.content_right);
    };
    for (std::size_t index = 0; index < parameters.size(); ++index) {
        const registry::Parameter& parameter = parameters[index];
        const auto number = static_cast<int32_t>(index);
        const Value& value = state.values[index];
        const ValueType type = parameter.value.type;
        if (type == ValueType::boolean) {
            ListRow& row = place_toggle(
                sized,
                list,
                left,
                std::string(parameter.name),
                top,
                control,
                switch_columns,
                small_width
            );
            own(row, number, geometry::whole_parameter);
            row.on = value.kind == ValueKind::boolean && value.boolean;
            top += row.height;
            continue;
        }
        if (type == ValueType::enumeration_set || list_type(type)) {
            ListRow& heading = list.rows.emplace_back();
            heading.kind = ListRowKind::heading;
            heading.area = area;
            heading.hack = hack;
            heading.parameter = number;
            heading.top = top;
            heading.height = geometry::list_heading_height;
            heading.text = std::string(parameter.name);
            heading.label = {left, top + 1, sized.content_right - left, geometry::hint_line_height};
            top += heading.height;
        }
        if (type == ValueType::enumeration_set) {
            const auto words = registry::enum_values_of(parameter.value);
            for (std::size_t word = 0; word < words.size(); ++word) {
                ListRow& row = place_toggle(
                    sized,
                    list,
                    item_left,
                    std::string(words[word]),
                    top,
                    control,
                    switch_columns,
                    small_width
                );
                own(row, number, static_cast<int32_t>(word));
                row.on =
                    std::any_of(value.items.begin(), value.items.end(), [&](const Value& item) {
                        return item.text == words[word];
                    });
                top += row.height;
            }
            continue;
        }
        if (list_type(type)) {
            if (parameter.value.has_length &&
                parameter.value.length_minimum != parameter.value.length_maximum) {
                ListRow& row = place_slider(
                    sized, list, item_left, std::string(geometry::items_text), top, control
                );
                own(row, number, geometry::list_length);
                fill_slider(row);
                top += row.height;
            }
            for (std::size_t item = 0; item < value.items.size(); ++item) {
                ListRow& row =
                    place_slider(sized, list, item_left, item_text(item + 1), top, control);
                own(row, number, static_cast<int32_t>(item));
                fill_slider(row);
                top += row.height;
            }
            continue;
        }
        ListRow& row = place_slider(sized, list, left, std::string(parameter.name), top, control);
        own(row, number, geometry::whole_parameter);
        fill_slider(row);
        top += row.height;
    }
    top += geometry::list_body_gap;
}

} // namespace

std::span<const HackArea> developer_areas() {
    return gathered_areas();
}

std::vector<HackState> shown_hacks(const Dialog& dialog) {
    const auto hacks = mod_profile::standard_hacks();
    std::vector<HackState> shown;
    shown.reserve(hacks.size());
    for (std::size_t index = 0; index < hacks.size(); ++index)
        shown.push_back(
            mod_profile::overridden_state(
                *hacks[index], profile_state(dialog, index), shown_override(dialog, index)
            )
        );
    return shown;
}

std::size_t active_hack_count(const Dialog& dialog) {
    const std::vector<HackState> shown = shown_hacks(dialog);
    return static_cast<std::size_t>(
        std::count_if(shown.begin(), shown.end(), [](const HackState& state) { return state.on; })
    );
}

namespace geometry {

bool developer_page(const Dialog& dialog) noexcept {
    // A check's own section takes the place of the list and its footer too.
    const bool own_section =
        dialog.section_hooks != nullptr && dialog.section_hooks->settings != nullptr;
    return dialog.kind == DialogKind::engine && dialog.page == Page::developer && !own_section;
}

bool developer_scrolls_as_one(const Dialog& dialog) noexcept {
    return developer_page(dialog) && dialog.size_class == oa::ui::kit::SizeClass::compact;
}

List place_list(
    const Dialog& dialog,
    int32_t scroll,
    int32_t switch_columns,
    const TextWidth* small_width,
    const TextWidth* regular_width,
    const Sizes* sizes
) {
    List list{};
    const Sizes& sized = sizes != nullptr ? *sizes : sizes_of(dialog);
    const std::vector<HackState> shown = shown_hacks(dialog);
    const auto& areas = gathered_areas();
    // Under the measured rows, not the class's rule, or a longer translation
    // is drawn across those rows.
    int32_t top = developer_list_top(dialog, small_width, &sized, regular_width);
    int32_t control = first_hack_list_control;
    for (const std::size_t area : shown_area_order()) {
        const HackArea& group = areas[area];
        const auto on = static_cast<std::size_t>(std::count_if(
            group.hacks.begin(), group.hacks.end(), [&](std::size_t hack) { return shown[hack].on; }
        ));
        if (dialog.developer.active_only && on == 0)
            continue;
        ListRow& header = list.rows.emplace_back();
        header.kind = ListRowKind::area;
        header.control = control++;
        header.area = area;
        header.hack = group.hacks.front();
        header.top = top;
        header.height = list_header_height;
        header.text = std::string(group.title);
        header.arrow = {
            sized.content_left + area_arrow_offset,
            top + (list_header_height - arrow_side) / 2,
            arrow_side,
            arrow_side
        };
        header.shown = geometry::filled(
            "{on} of {total} on",
            {{"on", std::to_string(on)}, {"total", std::to_string(group.hacks.size())}}
        );
        // The class's count box stays for the English line the tests pin.
        // A translation wider than that box takes the width it measures.
        int32_t count_width = area_count_width;
        if (small_width != nullptr) {
            const int32_t measured = (*small_width)(header.shown);
            if (measured > count_width)
                count_width = measured;
        }
        header.label = {
            sized.content_left + area_text_offset,
            top + 2,
            std::max(sized.content_width - area_text_offset - count_width - label_gap, int32_t{0}),
            label_line_height
        };
        header.value = {sized.content_right - count_width, top + 2, count_width, label_line_height};
        // A translated area name that does not fit beside the count takes the
        // line, and the count stands under it.
        if (regular_width != nullptr) {
            const int32_t title = (*regular_width)(std::string(shown_text(header.text)));
            if (title > header.label.width) {
                const int32_t extra = label_line_height;
                header.label.width = std::max(sized.content_width - area_text_offset, int32_t{1});
                header.value.y += extra;
                header.height += extra;
            }
        }
        header.control_area = {sized.content_left, top, sized.content_width, header.height};
        header.open = is_open(dialog.developer.areas_open, area);
        top += header.height;
        if (!header.open)
            continue;
        for (const std::size_t hack : shown_hack_order(group)) {
            if (dialog.developer.active_only && !shown[hack].on)
                continue;
            place_hack(
                dialog, shown, area, hack, list, top, control, switch_columns, small_width, sized
            );
        }
    }
    list.bottom = top;
    scroll_list(list, scroll);
    return list;
}

void scroll_list(List& list, int32_t by) noexcept {
    const auto lift = [by](SourceRect& rect) {
        if (rect.width > 0 && rect.height > 0)
            rect.y -= by;
    };
    for (ListRow& row : list.rows) {
        row.top -= by;
        lift(row.label);
        lift(row.control_area);
        lift(row.toggle);
        lift(row.value);
        lift(row.arrow);
    }
    list.bottom -= by;
}

int32_t list_scroll_showing(const ScrolledRows& open, int32_t control) noexcept {
    const auto& rows = open.list.rows;
    const auto found = std::find_if(rows.begin(), rows.end(), [control](const ListRow& row) {
        return row.control == control;
    });
    if (found == rows.end())
        return open.scroll;
    // The row's top may come up to the view's first row, its bottom down to
    // the view's last, and for the last row the list's end; a row taller
    // than the view shows its top.
    const SourceRect& seen = open.area.view;
    const int32_t top = found->top + open.scroll;
    const int32_t highest = top - seen.y;
    const int32_t lowest =
        found + 1 == rows.end() ? open.limit : top + found->height - (seen.y + seen.height);
    const int32_t scroll = std::min(std::max(open.scroll, lowest), highest);
    return std::clamp(scroll, int32_t{0}, open.limit);
}

const ListRow* list_row(const List& list, int32_t control) noexcept {
    if (control < first_hack_list_control)
        return nullptr;
    for (const ListRow& row : list.rows)
        if (row.control == control)
            return &row;
    return nullptr;
}

std::string ascii_text(std::string_view text) {
    std::string written;
    std::size_t at = 0;
    while (at < text.size()) {
        const auto byte = static_cast<unsigned char>(text[at]);
        if (byte >= first_printable && byte < past_printable) {
            written += static_cast<char>(byte);
            ++at;
            continue;
        }
        const auto fold =
            std::find_if(ascii_folds.begin(), ascii_folds.end(), [&](const AsciiFold& entry) {
                return text.substr(at).starts_with(entry.from);
            });
        if (fold != ascii_folds.end()) {
            written += fold->to;
            at += fold->from.size();
            continue;
        }
        // Chinese, Japanese and Korean characters stay, drawn in the modern
        // fonts; any other character is one question mark, whatever its
        // length.
        if (const auto read = oa::base::text::break_character(text.substr(at));
            read.bytes > 1 && oa::base::text::is_wide_script(read.character)) {
            written += text.substr(at, read.bytes);
            at += read.bytes;
            continue;
        }
        written += '?';
        at += byte >= four_byte_lead    ? 4
              : byte >= three_byte_lead ? 3
              : byte >= two_byte_lead   ? 2
                                        : 1;
    }
    return written;
}

std::vector<std::string> summary_lines(std::string_view summary) {
    const std::string text = ascii_text(summary);
    std::vector<std::string> lines;
    // A summary with Chinese, Japanese or Korean characters breaks where the
    // line breaker lets it, as every wrap does (oa::base::text::first_row):
    // at its spaces, between Chinese or Japanese characters, and inside a
    // word, between whole characters, only when the word is longer than a
    // line.
    if (oa::base::text::has_wide_script(text)) {
        std::string_view rest = text;
        const auto fits = [](std::string_view start) {
            return start.size() <= summary_line_characters;
        };
        for (;;) {
            while (!rest.empty() && rest.front() == ' ')
                rest.remove_prefix(1);
            if (rest.empty())
                return lines;
            const auto row = oa::base::text::first_row(rest, fits);
            lines.emplace_back(rest.substr(0, row.bytes));
            rest.remove_prefix(row.next);
        }
    }
    // The words, a word longer than a line cut after its last slash that
    // fits, or else where the line ends.
    std::vector<std::string> words;
    std::size_t at = 0;
    while (at < text.size()) {
        while (at < text.size() && text[at] == ' ')
            ++at;
        const std::size_t end = std::min(text.find(' ', at), text.size());
        std::string_view word = std::string_view{text}.substr(at, end - at);
        at = end;
        while (word.size() > summary_line_characters) {
            const std::size_t slash = word.rfind('/', summary_line_characters - 1);
            const std::size_t cut =
                slash == std::string_view::npos || slash == 0 ? summary_line_characters : slash + 1;
            words.emplace_back(word.substr(0, cut));
            word.remove_prefix(cut);
        }
        if (!word.empty())
            words.emplace_back(word);
    }
    std::string line;
    for (const std::string& word : words) {
        if (!line.empty() && line.size() + 1 + word.size() > summary_line_characters) {
            lines.push_back(std::move(line));
            line.clear();
        }
        if (!line.empty())
            line += ' ';
        line += word;
    }
    if (!line.empty())
        lines.push_back(std::move(line));
    return lines;
}

std::string list_value_text(const Value& value, std::string_view unit) {
    switch (value.kind) {
    case ValueKind::number: {
        std::string text = oamod::canonical_number_text(value.number);
        if (!unit.empty())
            text += " " + std::string(unit);
        return ascii_text(text);
    }
    case ValueKind::string:
        return value.text.empty() ? std::string{"\"\""} : ascii_text(value.text);
    case ValueKind::boolean:
        return value.boolean ? std::string{on_text} : std::string{off_text};
    default:
        return {};
    }
}

std::string active_only_text(std::size_t active, std::size_t total) {
    return filled(
        "Show Active Only ({active}/{total})",
        {{"active", std::to_string(active)}, {"total", std::to_string(total)}}
    );
}

} // namespace geometry

namespace developer {

DialogAction toggle_open(Dialog& dialog, const geometry::ListRow& row) {
    bool changed = false;
    if (row.kind == geometry::ListRowKind::area)
        changed =
            set_open(dialog.developer.areas_open, row.area, gathered_areas().size(), !row.open);
    else if (row.kind == geometry::ListRowKind::hack)
        changed = set_open(
            dialog.developer.hacks_open, row.hack, mod_profile::standard_hacks().size(), !row.open
        );
    return changed ? DialogAction::redraw : DialogAction::none;
}

DialogAction release_on(Dialog& dialog, const geometry::ListRow& row, int32_t x) {
    const auto on_half = [x](const geometry::SourceRect& area) {
        return x >= area.x + area.width / 2;
    };
    switch (row.kind) {
    case geometry::ListRowKind::area:
        return toggle_open(dialog, row);
    case geometry::ListRowKind::hack:
        if (x >= row.toggle.x && x < row.toggle.x + row.toggle.width)
            return set_toggle(dialog, row, on_half(row.toggle));
        return toggle_open(dialog, row);
    case geometry::ListRowKind::toggle:
        return set_toggle(dialog, row, on_half(row.control_area));
    default:
        return DialogAction::redraw;
    }
}

DialogAction activate(Dialog& dialog, const geometry::ListRow& row) {
    switch (row.kind) {
    case geometry::ListRowKind::area:
    case geometry::ListRowKind::hack:
        return toggle_open(dialog, row);
    case geometry::ListRowKind::toggle:
        return set_toggle(dialog, row, !row.on);
    default:
        return DialogAction::none;
    }
}

DialogAction step(Dialog& dialog, const geometry::ListRow& row, bool up) {
    switch (row.kind) {
    case geometry::ListRowKind::area:
        return row.open == up ? DialogAction::none : toggle_open(dialog, row);
    case geometry::ListRowKind::hack:
    case geometry::ListRowKind::toggle:
        return set_toggle(dialog, row, up);
    case geometry::ListRowKind::slider:
        return set_stop(dialog, row, row.stop + (up ? 1 : -1));
    default:
        return DialogAction::none;
    }
}

DialogAction drag_to(Dialog& dialog, const geometry::ListRow& row, int32_t column) {
    return set_stop(dialog, row, geometry::stop_at(row.control_area, column, row.stops));
}

DialogAction set_active_only(Dialog& dialog, bool on) noexcept {
    dialog.developer.active_only = on;
    return DialogAction::redraw;
}

DialogAction restore_profile_values(Dialog& dialog) noexcept {
    if (!restore_profile_enabled(dialog) || dialog.chosen.hack_overrides.empty())
        return DialogAction::redraw;
    dialog.chosen.hack_overrides.clear();
    return DialogAction::changed;
}

bool restore_profile_enabled(const Dialog& dialog) noexcept {
    return dialog.chosen.developer_mode;
}

} // namespace developer

} // namespace oa::ui::engine_settings

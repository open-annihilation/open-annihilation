// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A player's overrides of a profile's standard hacks: their states, how an
// override lays over a hack, and the text the settings keep them as.

#include "oa/data/mod_profile/overrides.hpp"

#include "oa/formats/oamod.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

namespace oa::data::mod_profile {

namespace {

/// The top-level key of an effective profile's hacks.
constexpr std::string_view hacks_key = "hacks";

/// The base game's author, as the base-game profile names it: the company
/// that made the game, as its archives' copyright trailer names it
/// (oa/formats/hpi.hpp). It is written as bytes, with no terminating NUL.
constexpr char base_game_author_chars[] = {0x43, 0x61, 0x76, 0x65, 0x64, 0x6f, 0x67,
                                           0x20, 0x45, 0x6e, 0x74, 0x65, 0x72, 0x74,
                                           0x61, 0x69, 0x6e, 0x6d, 0x65, 0x6e, 0x74};
/// The same name as text.
constexpr std::string_view base_game_author{base_game_author_chars, sizeof base_game_author_chars};
static_assert(base_game_author.size() == 21);

/// Returns the registry's standard hacks, gathered once.
///
/// @return every hack that is not retired, in the registry's order
const std::vector<const registry::Entry*>& gathered_hacks() {
    static const std::vector<const registry::Entry*> hacks = [] {
        std::vector<const registry::Entry*> found;
        for (const registry::Entry& entry : registry::table().entries)
            if (entry.kind == registry::EntryKind::hack && !entry.retired)
                found.push_back(&entry);
        return found;
    }();
    return hacks;
}

/// Returns a hack's baselines.
///
/// @param hack a standard hack
/// @return each parameter's 3.1c value, by index
std::vector<Value> baselines(const registry::Entry& hack) {
    std::vector<Value> values;
    for (const registry::Parameter& parameter : registry::parameters_of(hack))
        values.push_back(registry::literal_value(parameter.value.baseline));
    return values;
}

} // namespace

bool operator==(const ParameterOverride& left, const ParameterOverride& right) noexcept {
    return left.name == right.name && values_equal(left.value, right.value);
}

bool operator==(const HackOverride& left, const HackOverride& right) noexcept {
    return left.hack == right.hack && left.on == right.on && left.parameters == right.parameters;
}

std::span<const registry::Entry* const> standard_hacks() {
    return gathered_hacks();
}

std::optional<std::size_t> standard_hack_index(std::string_view id) noexcept {
    const auto& hacks = gathered_hacks();
    for (std::size_t index = 0; index < hacks.size(); ++index)
        if (hacks[index]->id == id)
            return index;
    return std::nullopt;
}

std::vector<HackState> hack_states(const Value& effective) {
    const Value* hacks = find_member(effective, hacks_key);
    std::vector<HackState> states;
    for (const registry::Entry* hack : gathered_hacks()) {
        HackState state{};
        state.values = baselines(*hack);
        const Value* written = hacks != nullptr ? find_member(*hacks, hack->id) : nullptr;
        if (written != nullptr && written->kind == ValueKind::map) {
            state.on = true;
            const auto parameters = registry::parameters_of(*hack);
            for (std::size_t index = 0; index < parameters.size(); ++index)
                if (const Value* value = find_member(*written, parameters[index].name))
                    state.values[index] = *value;
        }
        states.push_back(std::move(state));
    }
    return states;
}

std::vector<HackState> base_hack_states() {
    std::vector<HackState> states;
    for (const registry::Entry* hack : gathered_hacks())
        states.push_back(HackState{false, baselines(*hack)});
    return states;
}

std::vector<Value> on_values(const registry::Entry& hack, const HackState& profile) {
    const auto parameters = registry::parameters_of(hack);
    if (profile.on && profile.values.size() == parameters.size())
        return profile.values;
    std::vector<Value> values;
    for (const registry::Parameter& parameter : parameters)
        values.push_back(registry::literal_value(parameter.default_value));
    return values;
}

HackState overridden_state(
    const registry::Entry& hack, const HackState& profile, const HackOverride* override
) {
    if (override == nullptr)
        return profile;
    if (!override->on)
        return HackState{false, baselines(hack)};
    HackState state{true, on_values(hack, profile)};
    for (const ParameterOverride& parameter : override->parameters) {
        const int index = registry::find_parameter(hack, parameter.name);
        if (index >= 0)
            state.values[static_cast<std::size_t>(index)] = parameter.value;
    }
    return state;
}

const HackOverride*
find_override(std::span<const HackOverride> overrides, std::string_view hack) noexcept {
    for (const HackOverride& override : overrides)
        if (override.hack == hack)
            return &override;
    return nullptr;
}

std::string overrides_text(std::span<const HackOverride> overrides) {
    Value text = make_map();
    for (const HackOverride& override : overrides) {
        if (!override.on || override.parameters.empty()) {
            set_member(text, override.hack, make_boolean(override.on));
            continue;
        }
        Value parameters = make_map();
        for (const ParameterOverride& parameter : override.parameters)
            set_member(parameters, parameter.name, parameter.value);
        set_member(text, override.hack, std::move(parameters));
    }
    return canonical_json(text);
}

std::vector<HackOverride> read_overrides(std::string_view text) {
    std::vector<HackOverride> overrides;
    formats::oamod::Node root{};
    formats::oamod::ReadError error{};
    const std::span<const uint8_t> bytes{
        reinterpret_cast<const uint8_t*>(text.data()), text.size()
    };
    if (!formats::oamod::read_value(bytes, root, error) ||
        root.kind != formats::oamod::NodeKind::mapping)
        return overrides;
    for (const formats::oamod::Node& member : root.children) {
        if (member.key.kind != formats::oamod::KeyKind::string)
            continue;
        if (const registry::Entry* entry = registry::find_entry(member.key.text);
            entry != nullptr && entry->retired)
            continue;
        HackOverride override{};
        override.hack = member.key.text;
        if (member.kind == formats::oamod::NodeKind::boolean) {
            override.on = member.boolean;
        } else if (member.kind == formats::oamod::NodeKind::mapping) {
            override.on = true;
            for (const formats::oamod::Node& parameter : member.children) {
                if (parameter.key.kind != formats::oamod::KeyKind::string)
                    continue;
                Value value = value_from_node(parameter);
                value.position = {};
                override.parameters.push_back(
                    ParameterOverride{parameter.key.text, std::move(value)}
                );
            }
        } else {
            continue;
        }
        overrides.push_back(std::move(override));
    }
    return overrides;
}

std::string base_game_profile_text() {
    return "oamod: 1\n"
           "id: base-game\n"
           "name: \"Total Annihilation 3.1c\"\n"
           "version: \"3.1c\"\n"
           "requires: {base: ta-3.1c, catalogue: " +
           std::to_string(registry::table().catalogue) +
           "}\n"
           "author: {name: " +
           std::string(base_game_author) +
           "}\n"
           "packaging: {revision: 1, date: 2026-10-04, packager: Open Annihilation}\n";
}

} // namespace oa::data::mod_profile

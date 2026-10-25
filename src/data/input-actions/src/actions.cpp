// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The action table and the lookups over it. actions.inc is generated.

#include "oa/data/input_actions/actions.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <span>
#include <string_view>

namespace oa::data::input_actions {

namespace {

/// One editor category, in the catalogue's order. The header does not publish
/// it.
struct Area {
    std::string_view name{};
    uint16_t order{};
    std::string_view title{};
};

/// Two conditions that cannot both hold.
struct ExclusivePair {
    uint8_t first{};
    uint8_t second{};
};

/// An old id and the action that still answers to it.
struct AliasRef {
    std::string_view alias{};
    uint16_t index{};
};

#include "actions.inc"

/// Reports whether the actions are in id order.
///
/// @return true when each id is less than the next
constexpr bool ids_increase() noexcept {
    const size_t count = std::size(k_actions);
    for (size_t index = 1; index < count; ++index) {
        if (!(k_actions[index - 1].id < k_actions[index].id))
            return false;
    }
    return true;
}

static_assert(ids_increase());
static_assert(k_contexts[0].parent == no_parent);
static_assert(k_areas[0].order == uint16_t{1});

} // namespace

std::span<const Action> actions() noexcept {
    return std::span<const Action>(k_actions);
}

const Action* find(std::string_view id_or_alias) noexcept {
    const auto by_id = std::lower_bound(
        std::begin(k_actions),
        std::end(k_actions),
        id_or_alias,
        [](const Action& action, std::string_view key) { return action.id < key; }
    );
    if (by_id != std::end(k_actions) && by_id->id == id_or_alias)
        return &*by_id;
    const auto by_alias = std::lower_bound(
        k_aliases.begin(),
        k_aliases.end(),
        id_or_alias,
        [](const AliasRef& ref, std::string_view key) { return ref.alias < key; }
    );
    if (by_alias == k_aliases.end() || by_alias->alias != id_or_alias)
        return nullptr;
    if (by_alias->index >= std::size(k_actions))
        return nullptr;
    return &k_actions[by_alias->index];
}

std::span<const Context> contexts() noexcept {
    return std::span<const Context>(k_contexts);
}

std::span<const std::string_view> conditions() noexcept {
    return std::span<const std::string_view>(k_conditions);
}

std::span<const Family> families() noexcept {
    return std::span<const Family>(k_families);
}

std::span<const std::string_view> hack_ids() noexcept {
    return std::span<const std::string_view>(k_hack_ids);
}

bool exclusive(uint32_t when_a, uint32_t when_b) noexcept {
    for (const ExclusivePair& pair : k_exclusive) {
        const uint32_t first = uint32_t{1} << pair.first;
        const uint32_t second = uint32_t{1} << pair.second;
        const bool a_has_first = (when_a & first) != 0;
        const bool a_has_second = (when_a & second) != 0;
        const bool b_has_first = (when_b & first) != 0;
        const bool b_has_second = (when_b & second) != 0;
        if ((a_has_first && b_has_second) || (a_has_second && b_has_first))
            return true;
    }
    return false;
}

} // namespace oa::data::input_actions

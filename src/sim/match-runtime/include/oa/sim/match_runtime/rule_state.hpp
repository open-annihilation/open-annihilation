// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// State a mod's rules keep that 3.1c's records have no room for, such as the
// tick each unit slot may be reused at or the facing of each building.
//
// A rule that needs such state keeps it in a table of its own module,
// allocated when the match is built, never in Unit, Game or Player, whose
// layouts are pinned. It adds the table to the match's RuleState while the
// match is built (Match::rule_state), and only when its parameters need the
// state: at its 3.1c baseline a rule adds none. The match digests shared
// tables into its state digest and its trace; a save keeps every table,
// including owner-local tables marked save-only (Match::fold_rule_state).
// With no shared table added and no mutator digest, nothing is digested
// beyond 3.1c's state, so a match without a profile, or with every rule at
// its baseline, digests exactly as before. A mutator digest
// the match holds is folded even when no table was added.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace oa::sim::match_runtime {

/// The most rule-state tables one match keeps.
inline constexpr size_t rule_state_capacity = 16;

/// The most bytes of a table's name, which names its blob in a save.
inline constexpr size_t rule_state_name_bytes = 23;

/// One table of rule state, which its module owns and describes here.
///
/// Its bytes are the state as the module keeps it: fixed-width integers with
/// no padding and no pointers, in the host's little-endian order, which every
/// supported platform shares. They are saved as they are, and digested
/// unless the table is owner-local state excluded from shared match checks.
struct RuleStateTable {
    /// The table's name, 1 to rule_state_name_bytes bytes, unique in the match.
    const char* name{};
    void* context{};
    /// Returns the table's bytes as they are now; null leaves the table empty.
    std::span<const uint8_t> (*bytes)(void* context){};
    /// Replaces the table's state with the bytes a save holds for it.
    /// Returns false when they do not fit the table, which stops the load;
    /// null keeps the state as it is.
    bool (*restore)(void* context, std::span<const uint8_t> bytes){};
    /// Includes the table in shared state and trace digests; false keeps it save-only.
    bool digest{true};
};

/// The rule-state tables of a match, in the order they were added.
struct RuleState {
    std::array<RuleStateTable, rule_state_capacity> tables{};
    uint8_t count{}; ///< tables in use
};

/// Adds a table.
///
/// @param[in,out] state the match's tables
/// @param table the table to add
/// @return false, adding nothing, when the tables are full or the name is
///         empty, too long or already taken
bool add_rule_state(RuleState& state, const RuleStateTable& table) noexcept;

/// Finds a table by name.
///
/// @param state the match's tables
/// @param name the name, matched exactly
/// @return the table, or null
[[nodiscard]] const RuleStateTable*
find_rule_state(const RuleState& state, std::string_view name) noexcept;

/// Counts the tables included in shared state and trace digests.
///
/// @param state the match's tables
/// @return the number whose digest flag is true
[[nodiscard]] uint8_t digested_rule_state_count(const RuleState& state) noexcept;

/// Digests the tables into a 64-bit FNV-1a digest, over bytes: first the
/// profile's sim hash, then each digested table in the order added, as its name's
/// bytes, its byte count as a little-endian 32-bit word, and its bytes,
/// then the mutator digest when one is given.
///
/// @param digest the digest so far
/// @param state the match's tables
/// @param profile_sim_hash the sim hash of the profile the match plays, or
///        empty without one
/// @param mutator_digest the match's mutator digest, or empty when it applied none
/// @return the new digest
[[nodiscard]] uint64_t digest_rule_state(
    uint64_t digest,
    const RuleState& state,
    std::span<const uint8_t> profile_sim_hash,
    std::span<const uint8_t> mutator_digest = {}
) noexcept;

} // namespace oa::sim::match_runtime

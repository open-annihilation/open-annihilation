// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/match_runtime/rule_state.hpp"

namespace oa::sim::match_runtime {
namespace {

/// The prime of the 64-bit FNV-1a digest, as the match-state digest uses.
constexpr uint64_t fnv_prime = 0x100000001b3ull;

/// Folds bytes into a 64-bit FNV-1a digest.
///
/// @param digest the digest so far
/// @param bytes the bytes
/// @return the new digest
uint64_t fold_bytes(uint64_t digest, std::span<const uint8_t> bytes) noexcept {
    for (const uint8_t byte : bytes) {
        digest ^= byte;
        digest *= fnv_prime;
    }
    return digest;
}

/// Returns a table's name as a view, bounded by rule_state_name_bytes + 1.
///
/// @param name the name, or null
/// @return the view; empty for null
std::string_view name_view(const char* name) noexcept {
    size_t length = 0;
    while (name != nullptr && length <= rule_state_name_bytes && name[length] != '\0')
        ++length;
    return name != nullptr ? std::string_view{name, length} : std::string_view{};
}

} // namespace

bool add_rule_state(RuleState& state, const RuleStateTable& table) noexcept {
    const std::string_view name = name_view(table.name);
    if (state.count >= state.tables.size() || name.empty() || name.size() > rule_state_name_bytes ||
        find_rule_state(state, name) != nullptr)
        return false;
    state.tables[state.count++] = table;
    return true;
}

const RuleStateTable* find_rule_state(const RuleState& state, std::string_view name) noexcept {
    for (uint8_t at = 0; at < state.count; ++at) {
        if (name_view(state.tables[at].name) == name)
            return &state.tables[at];
    }
    return nullptr;
}

uint8_t digested_rule_state_count(const RuleState& state) noexcept {
    uint8_t count = 0;
    for (uint8_t at = 0; at < state.count; ++at)
        if (state.tables[at].digest)
            ++count;
    return count;
}

uint64_t digest_rule_state(
    uint64_t digest,
    const RuleState& state,
    std::span<const uint8_t> profile_sim_hash,
    std::span<const uint8_t> mutator_digest
) noexcept {
    digest = fold_bytes(digest, profile_sim_hash);
    for (uint8_t at = 0; at < state.count; ++at) {
        const RuleStateTable& table = state.tables[at];
        if (!table.digest)
            continue;
        const std::string_view name = name_view(table.name);
        digest = fold_bytes(digest, {reinterpret_cast<const uint8_t*>(name.data()), name.size()});
        const std::span<const uint8_t> bytes =
            table.bytes != nullptr ? table.bytes(table.context) : std::span<const uint8_t>{};
        const auto size = static_cast<uint32_t>(bytes.size());
        const std::array<uint8_t, 4> size_bytes{
            static_cast<uint8_t>(size),
            static_cast<uint8_t>(size >> 8),
            static_cast<uint8_t>(size >> 16),
            static_cast<uint8_t>(size >> 24)
        };
        digest = fold_bytes(digest, size_bytes);
        digest = fold_bytes(digest, bytes);
    }
    digest = fold_bytes(digest, mutator_digest);
    return digest;
}

} // namespace oa::sim::match_runtime

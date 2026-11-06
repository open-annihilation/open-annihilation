// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Whether the data a game is playing is one the Boneyards launches and
// reports (boneyards_data.hpp).
#include "boneyards_data.hpp"

#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace oa::app {
namespace {

namespace profiles = oa::data::mod_profile;

DataFactsSource g_source = nullptr;
void* g_source_context = nullptr;

/// The simulation hash a profile resolves to, or nothing when it does not.
///
/// @param text the profile's bytes
/// @param name the name its diagnostics use
/// @param options the settings and overrides it resolves with
/// @return the hash; nothing when the profile is refused
std::optional<oa::base::sha256::Digest> simulation_hash(
    std::string_view text, std::string_view name, const profiles::ResolveOptions& options
) {
    const auto* bytes = reinterpret_cast<const uint8_t*>(text.data());
    const profiles::ResolveResult result =
        profiles::resolve_profile(std::span<const uint8_t>(bytes, text.size()), name, options);
    if (!result.resolution)
        return std::nullopt;
    return result.resolution->profile.sim_hash;
}

DataVerdict allowed_verdict() noexcept {
    return DataVerdict{true, nullptr};
}

DataVerdict refused_verdict() noexcept {
    return DataVerdict{false, kBoneyardsDataRefusal};
}

/// The profile id the facts name, from the field or from an `id:` line.
///
/// @param facts what is known about the data
/// @return the id; empty when none is named
std::string named_profile_id(const DataFacts& facts) {
    if (!facts.profile_id.empty())
        return facts.profile_id;
    std::string_view text = facts.profile_text;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t end = text.find('\n', at);
        const std::size_t stop = end == std::string_view::npos ? text.size() : end;
        std::string_view line = text.substr(at, stop - at);
        if (!line.empty() && line.back() == '\r')
            line.remove_suffix(1);
        constexpr std::string_view key = "id:";
        if (line.size() >= key.size() && line.substr(0, key.size()) == key) {
            std::string_view value = line.substr(key.size());
            while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
                value.remove_prefix(1);
            if (value.size() >= 2 && value.front() == '"' && value.back() == '"')
                value = value.substr(1, value.size() - 2);
            return std::string(value);
        }
        if (end == std::string_view::npos)
            break;
        at = end + 1;
    }
    return {};
}

/// A profile other than the Boneyards' own, or a mod folder with no profile.
///
/// @param facts what is known about the data
/// @return true when the match is not plain 3.1c or the Boneyards' own files
bool foreign_mod(const DataFacts& facts) {
    const std::string id = named_profile_id(facts);
    if (id == "boneyards")
        return false;
    if (!id.empty())
        return true;
    return facts.mod_folder;
}

} // namespace

DataVerdict boneyards_data_verdict(const DataFacts& facts) {
    if (facts.unreadable)
        return refused_verdict();
    // A mutator, a profile other than the Boneyards' own, or a mod folder
    // with no profile is refused even when the simulation hash matches.
    if (facts.mutator_count > 0 || foreign_mod(facts))
        return refused_verdict();
    // The engine's simulation_hash is the profile in play, with every
    // Developer Mode override that changes the simulation already applied.
    if (!facts.run_sim_hash.empty()) {
        const std::string base_text = profiles::base_game_profile_text();
        const std::optional<oa::base::sha256::Digest> base =
            simulation_hash(base_text, profiles::base_game_id, profiles::ResolveOptions{});
        if (!base || facts.run_sim_hash != profiles::digest_text(*base))
            return refused_verdict();
        return allowed_verdict();
    }
    profiles::ResolveOptions options = facts.options;
    if (!facts.developer_mode)
        options.overrides.clear();
    // No profile and no override plays 3.1c. Core Contingency and Battle
    // Tactics are archives on that install; they add no profile.
    if (facts.profile_text.empty() && !facts.shipped_sim_hash && options.overrides.empty())
        return allowed_verdict();

    profiles::ResolveOptions base_options{};
    base_options.accept_unimplemented_hacks = facts.options.accept_unimplemented_hacks;
    const std::string base_text = profiles::base_game_profile_text();
    const std::optional<oa::base::sha256::Digest> base =
        simulation_hash(base_text, profiles::base_game_id, base_options);
    if (!base)
        return refused_verdict();

    std::optional<oa::base::sha256::Digest> played{};
    if (options.overrides.empty() && facts.shipped_sim_hash)
        played = facts.shipped_sim_hash;
    else if (options.overrides.empty())
        played = simulation_hash(
            facts.profile_text, facts.profile_name.empty() ? "profile" : facts.profile_name, options
        );
    else if (facts.profile_text.empty())
        played = simulation_hash(base_text, profiles::base_game_id, options);
    else
        played = simulation_hash(
            facts.profile_text, facts.profile_name.empty() ? "profile" : facts.profile_name, options
        );
    if (!played || *played != *base)
        return refused_verdict();
    return allowed_verdict();
}

void boneyards_set_data_facts_source(DataFactsSource source, void* context) noexcept {
    g_source = source;
    g_source_context = context;
}

DataVerdict boneyards_current_data_verdict() {
    if (g_source == nullptr)
        return allowed_verdict();
    return boneyards_data_verdict(g_source(g_source_context));
}

const char* boneyards_data_refusal_message() noexcept {
    return kBoneyardsDataRefusal;
}

} // namespace oa::app

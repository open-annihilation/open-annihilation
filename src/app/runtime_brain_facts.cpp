// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The facts a brain reads, built on the main thread from the mounted
// content, the profile, the sides and the match's loadout.

#include "oa/app/runtime.hpp"

#include "oa/core/game_state.h"
#include "oa/core/side.h"
#include "oa/data/brain_facts.hpp"
#include "oa/data/campaign/campaign_file.hpp"
#include "oa/data/mutators/loadout.hpp"
#include "oa/formats/oamod.hpp"
#include "oa/formats/oamut/manifest.hpp"

#include <cstdint>
#include <string>

namespace {

std::string text_of(const char* bytes, std::size_t room) {
    std::size_t size = 0;
    while (size < room && bytes[size] != '\0')
        ++size;
    return std::string(bytes, size);
}

} // namespace

namespace oa::app {

oa::data::brain_facts::GameFacts Runtime::brain_game_facts() const {
    oa::data::brain_facts::GameFacts facts;
    if (managed_content_mounted()) {
        const oa::data::content_bundles::ContentChoice choice = content_choice();
        if (!choice.base_game.empty())
            facts.base = choice.base_game;
        facts.expansions = choice.expansions;
    }
    facts.engine = OA_ENGINE_VERSION;
    if (campaign_mission_)
        facts.mode = oa::data::brain_facts::GameMode::campaign;
    else if (multiplayer_session())
        facts.mode = oa::data::brain_facts::GameMode::network;
    else
        facts.mode = oa::data::brain_facts::GameMode::skirmish;
    if (match_)
        facts.difficulty = match_->difficulty();
    else
        facts.difficulty = static_cast<int32_t>(preferences_.difficulty);

    facts.mod = oa::data::brain_facts::mod_facts_of(mod_profile());
    const uint32_t side_count =
        side_table_.count < OA_SIDE_COUNT ? side_table_.count : OA_SIDE_COUNT;
    facts.mod.sides.reserve(side_count);
    for (uint32_t side_index = 0; side_index < side_count; ++side_index) {
        const Side& side = side_table_.sides[side_index];
        oa::data::brain_facts::SideFacts stored;
        stored.name = text_of(side.name, sizeof side.name);
        stored.prefix = text_of(side.name_prefix, sizeof side.name_prefix);
        stored.commander = text_of(side.commander, sizeof side.commander);
        facts.mod.sides.push_back(std::move(stored));
    }

    facts.map_name = selected_map_name_runtime_;
    // brain_game_facts stays const. The map context binds on first use.
    const oa::data::campaign::CampaignFile* map_context =
        const_cast<Runtime*>(this)->match_map_context();
    const char* mission = nullptr;
    if (map_context != nullptr) {
        mission = oa::data::campaign::campaign_path(
            map_context, oa::data::campaign::CampaignPath::mission
        );
    }
    if (mission != nullptr)
        facts.map_path = mission;

    const oa::data::mutators::Loadout* loadout = match_loadout();
    if (loadout == nullptr)
        return facts;
    facts.mutators.reserve(loadout->entries.size());
    for (const oa::data::mutators::LoadoutEntry& entry : loadout->entries) {
        oa::data::brain_facts::MutatorFacts mutator;
        mutator.id = entry.id;
        mutator.version = entry.version;
        mutator.options.reserve(entry.options.size());
        for (const oa::data::mutators::LoadoutOption& option : entry.options) {
            oa::data::brain_facts::OptionFacts stored;
            stored.id = option.id;
            if (option.type == oa::formats::oamut::OptionType::toggle) {
                stored.kind = oa::data::brain_facts::OptionFacts::Kind::toggle;
                stored.toggle = option.toggle;
            } else if (
                option.type == oa::formats::oamut::OptionType::number || option.value.number
            ) {
                stored.kind = oa::data::brain_facts::OptionFacts::Kind::number;
                stored.number = oa::formats::oamod::number_to_double(option.value.number_value);
            } else {
                stored.kind = oa::data::brain_facts::OptionFacts::Kind::choice;
                stored.text = option.value.text;
            }
            mutator.options.push_back(std::move(stored));
        }
        facts.mutators.push_back(std::move(mutator));
    }
    return facts;
}

} // namespace oa::app

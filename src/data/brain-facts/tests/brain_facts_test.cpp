// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// mod_facts_of names the base game for a null profile and for the baseline
// id, copies a ridge profile's own name, and a default GameFacts is the base
// game with nothing switched on.

#include "oa/data/brain_facts.hpp"
#include "oa/data/mod_profile.hpp"
#include "oa/test/check.hpp"

#include "oa/core/game_state.h"

namespace {

using namespace oa::data::brain_facts;

void test_null_profile_is_the_base_game() {
    const ModFacts facts = mod_facts_of(nullptr);
    OA_CHECK(facts.id == "ta-3.1c");
    OA_CHECK(facts.name == "Total Annihilation 3.1c");
    OA_CHECK(facts.version == "3.1c");
    OA_CHECK(!facts.profile.has_value());
    OA_CHECK(facts.sides.empty());
}

void test_base_game_id_keeps_the_base_name() {
    oa::data::mod_profile::ModProfile profile;
    profile.id = "base-game";
    profile.name = "Plain Rules";
    profile.version = "9";
    const ModFacts facts = mod_facts_of(&profile);
    OA_CHECK(facts.id == "ta-3.1c");
    OA_CHECK(facts.name == "Total Annihilation 3.1c");
    OA_CHECK(facts.version == "3.1c");
    OA_CHECK(facts.profile.has_value());
    OA_CHECK(facts.profile->id == "base-game");
    OA_CHECK(facts.profile->name == "Plain Rules");
    OA_CHECK(facts.profile->version == "9");
}

void test_ridge_keeps_its_own_name() {
    oa::data::mod_profile::ModProfile profile;
    profile.id = "ridge";
    profile.name = "Ridge";
    profile.version = "28";
    const ModFacts facts = mod_facts_of(&profile);
    OA_CHECK(facts.id == "ridge");
    OA_CHECK(facts.name == "Ridge");
    OA_CHECK(facts.version == "28");
    OA_CHECK(facts.profile.has_value());
    OA_CHECK(facts.profile->id == "ridge");
    OA_CHECK(facts.profile->version == "28");
}

void test_default_game_is_the_base_game() {
    const GameFacts facts;
    OA_CHECK(facts.base == "totala");
    OA_CHECK(facts.expansions.empty());
    OA_CHECK(facts.engine.empty());
    OA_CHECK(facts.mode == GameMode::skirmish);
    OA_CHECK(facts.difficulty == OA_DIFFICULTY_MEDIUM);
    OA_CHECK(facts.mod.id == "ta-3.1c");
    OA_CHECK(facts.mod.name == "Total Annihilation 3.1c");
    OA_CHECK(facts.mod.version == "3.1c");
    OA_CHECK(!facts.mod.profile.has_value());
    OA_CHECK(facts.mod.sides.empty());
    OA_CHECK(facts.mutators.empty());
}

} // namespace

int main() {
    test_null_profile_is_the_base_game();
    test_base_game_id_keeps_the_base_name();
    test_ridge_keeps_its_own_name();
    test_default_game_is_the_base_game();
    return oa::test::check_exit_status();
}

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Turns a resolved profile into the mod facts a brain reads. The base game
// keeps one name whatever id the baseline profile carries.

#include "oa/data/brain_facts.hpp"

namespace oa::data::brain_facts {

ModFacts mod_facts_of(const data::mod_profile::ModProfile* profile) {
    ModFacts facts;
    if (profile == nullptr)
        return facts;
    if (profile->id != "base-game") {
        facts.id = profile->id;
        facts.name = profile->name;
        facts.version = profile->version;
    }
    facts.profile = *profile;
    return facts;
}

} // namespace oa::data::brain_facts

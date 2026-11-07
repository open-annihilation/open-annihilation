// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Comparing a replay's needs with what is installed. The release fetched is
// the one whose SHA-256 the replay names.

#include "oa/data/replays/content.hpp"

#include "oa/formats/oamod/package_keys.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <utility>

namespace oa::data::replays {
namespace {

namespace oamod = oa::formats::oamod;

/// Reports whether a list holds a text.
bool holds(const std::vector<std::string>& names, std::string_view wanted) {
    return std::find(names.begin(), names.end(), wanted) != names.end();
}

/// The installed mod of an id, or null when none is installed.
const HaveMod* mod_of(const HaveContent& have, std::string_view id) {
    for (const HaveMod& mod : have.mods) {
        if (mod.id == id)
            return &mod;
    }
    return nullptr;
}

/// The installed mod of a display name or an id, or null when none matches.
const HaveMod* mod_named(const HaveContent& have, std::string_view name) {
    if (name.empty())
        return nullptr;
    for (const HaveMod& mod : have.mods) {
        if (mod.name == name || mod.id == name)
            return &mod;
    }
    return nullptr;
}

/// Reports whether the mod being played is the one a name-only need asks for.
bool playing_named(const HaveContent& have, std::string_view name) {
    return !name.empty() && (have.playing_mod_name == name || have.playing_mod_id == name);
}

/// Reports whether the mod being played is this installed release.
bool playing_release(const HaveContent& have, const HaveMod& installed) {
    if (!have.playing_mod_id.empty())
        return have.playing_mod_id == installed.id;
    if (!have.playing_mod_name.empty())
        return have.playing_mod_name == installed.name || have.playing_mod_name == installed.id;
    return false;
}

/// Reports whether an installed mutator is the one the replay names.
bool mutator_matches(const HaveMutator& have, const ContentNeed& need) {
    if (!need.id.empty() && have.id != need.id)
        return false;
    if (need.id.empty())
        return !need.sha256.empty() && !have.sha256.empty() && have.sha256 == need.sha256;
    if (!need.sha256.empty() && !have.sha256.empty() && have.sha256 != need.sha256)
        return false;
    return true;
}

/// Reports whether a mutator the replay names is installed.
bool mutator_installed(const HaveContent& have, const ContentNeed& need) {
    for (const HaveMutator& item : have.mutators) {
        if (mutator_matches(item, need))
            return true;
    }
    return false;
}

/// Reports whether an installed mod is the release the replay names.
bool same_release(const HaveMod& have, const ContentNeed& need) {
    if (have.version != need.version)
        return false;
    if (need.revision.has_value() && have.revision.has_value() && *need.revision != *have.revision)
        return false;
    if (!need.sha256.empty() && !have.sha256.empty() && need.sha256 != have.sha256)
        return false;
    return true;
}

/// The listing for a package digest, or null when none was given.
const ListedRelease* offer_of(std::span<const ListedRelease> listed, std::string_view sha) {
    if (sha.empty())
        return nullptr;
    for (const ListedRelease& offer : listed) {
        if (offer.sha256 == sha)
            return &offer;
    }
    return nullptr;
}

/// The listing for a package id of one kind, or null when none was given.
const ListedRelease*
offer_by_id(std::span<const ListedRelease> listed, std::string_view kind, std::string_view id) {
    if (id.empty())
        return nullptr;
    for (const ListedRelease& offer : listed) {
        if (offer.package_kind == kind && offer.id == id)
            return &offer;
    }
    return nullptr;
}

/// The expansion listing: a catalogue entry of that id wins over a digest
/// the replay did not name, and a listed release wins over a refusal.
const ListedRelease*
expansion_offer(std::span<const ListedRelease> listed, const ContentNeed& need) {
    const ListedRelease* by_id = offer_by_id(listed, "expansion", need.id);
    const ListedRelease* by_sha = offer_of(listed, need.sha256);
    if (by_id != nullptr && by_id->reach == ReleaseReach::listed)
        return by_id;
    if (by_sha != nullptr && by_sha->reach == ReleaseReach::listed)
        return by_sha;
    if (by_id != nullptr)
        return by_id;
    return by_sha;
}

/// Fills the download of a gap from the release the digest names.
void apply_offer(ContentGap& gap, const ListedRelease* offer) {
    gap.fetchable = false;
    if (gap.other_release)
        return;
    if (offer == nullptr || offer->reach == ReleaseReach::unknown)
        return;
    if (offer->reach == ReleaseReach::unlisted) {
        gap.block = GapBlock::unlisted;
        return;
    }
    if (offer->reach == ReleaseReach::offline) {
        // The cached entry's name is what the card says. A digest alone would
        // leave the sentence naming the package by its id.
        if (!offer->id.empty())
            gap.id = offer->id;
        if (gap.kind == GapKind::mutator && !offer->name.empty())
            gap.name = offer->name;
        else if (gap.name.empty() && !offer->name.empty())
            gap.name = offer->name;
        if (gap.kind == GapKind::map && !offer->name.empty())
            gap.pack_name = offer->name;
        if (gap.version.empty() && !offer->version.empty())
            gap.version = offer->version;
        gap.block = GapBlock::offline;
        return;
    }
    gap.fetchable = true;
    gap.registry = offer->registry;
    gap.package_kind = offer->package_kind;
    if (!offer->id.empty())
        gap.id = offer->id;
    if (gap.name.empty())
        gap.name = offer->name;
    if (gap.kind == GapKind::map && !offer->name.empty())
        gap.pack_name = offer->name;
    // A manifest names a mutator by id. The catalogue's name is what the card says.
    if (gap.kind == GapKind::mutator && !offer->name.empty())
        gap.name = offer->name;
    gap.file = offer->file;
    gap.release = offer->release;
    gap.size = offer->size;
}

/// A gap for one need, before the listing is applied.
ContentGap gap_for(GapKind kind, const ContentNeed& need) {
    ContentGap gap;
    gap.kind = kind;
    gap.id = need.id;
    gap.name = need.name.empty() ? need.id : need.name;
    gap.version = need.version;
    gap.revision = need.revision;
    gap.sha256 = need.sha256;
    gap.map_name = need.map_name;
    gap.pack_name = need.pack_name;
    return gap;
}

/// The version an unmet engine requirement names, such as `0.9.2`.
std::string unmet_version(const oamod::EngineRange& range) {
    for (const auto& term : range.terms) {
        if (term.first == oamod::EngineComparison::at_least ||
            term.first == oamod::EngineComparison::above)
            return oamod::engine_version_text(term.second);
    }
    if (range.terms.empty())
        return {};
    return oamod::engine_version_text(range.terms.front().second);
}

/// Appends a map gap unless that pack's digest already has one.
void add_map(
    std::vector<ContentGap>& gaps,
    std::vector<std::string>& packs,
    const ContentNeed& need,
    std::span<const ListedRelease> listed
) {
    if (!need.sha256.empty() && holds(packs, need.sha256))
        return;
    if (!need.sha256.empty())
        packs.push_back(need.sha256);
    ContentGap gap = gap_for(GapKind::map, need);
    apply_offer(gap, offer_of(listed, need.sha256));
    gaps.push_back(std::move(gap));
}

} // namespace

std::vector<ContentGap> compare_needs(
    const Replay& replay, const HaveContent& have, std::span<const ListedRelease> listed
) {
    std::vector<ContentGap> gaps;
    std::vector<std::string> map_packs;
    for (const ContentNeed& need : replay.needs) {
        if (need.kind == "oamod") {
            if (need.id.empty() && need.name.empty())
                continue;
            const HaveMod* installed =
                need.id.empty() ? mod_named(have, need.name) : mod_of(have, need.id);
            // A note that names the mod and no release matches that name.
            const bool name_only = need.id.empty() && need.version.empty();
            if (installed != nullptr && (name_only || same_release(*installed, need))) {
                // A name is enough. A release the manifest names has to be the
                // mod this run is playing, not only a folder under Mods.
                if (name_only || playing_release(have, *installed))
                    continue;
                ContentGap gap = gap_for(GapKind::mod_missing, need);
                gap.play_installed = true;
                gaps.push_back(std::move(gap));
                continue;
            }
            if (installed == nullptr && name_only && playing_named(have, need.name))
                continue;
            if (installed != nullptr) {
                ContentGap gap = gap_for(GapKind::mod_other, need);
                gap.have_version = installed->version;
                gap.have_revision = installed->revision;
                gap.other_release = true;
                apply_offer(gap, offer_of(listed, need.sha256));
                gaps.push_back(std::move(gap));
                continue;
            }
            ContentGap gap = gap_for(GapKind::mod_missing, need);
            apply_offer(gap, offer_of(listed, need.sha256));
            gaps.push_back(std::move(gap));
            continue;
        }
        if (need.kind == "oamap") {
            if (!need.map_name.empty() && holds(have.maps, need.map_name))
                continue;
            if (need.map_name.empty() && need.sha256.empty() && need.id.empty())
                continue;
            add_map(gaps, map_packs, need, listed);
            continue;
        }
        if (need.kind == "oamut") {
            if (mutator_installed(have, need))
                continue;
            if (need.id.empty() && need.sha256.empty())
                continue;
            ContentGap gap = gap_for(GapKind::mutator, need);
            apply_offer(gap, offer_of(listed, need.sha256));
            gaps.push_back(std::move(gap));
            continue;
        }
        if (need.kind == "expansion") {
            if (need.id.empty())
                continue;
            if (holds(have.expansions_on, need.id))
                continue;
            if (holds(have.expansions_have, need.id)) {
                ContentGap gap = gap_for(GapKind::expansion_off, need);
                gap.turn_on = true;
                gaps.push_back(std::move(gap));
                continue;
            }
            // Not installed: fetch one the catalogue offers, and never turn
            // a missing expansion on.
            ContentGap gap = gap_for(GapKind::expansion_missing, need);
            apply_offer(gap, expansion_offer(listed, need));
            gaps.push_back(std::move(gap));
        }
    }
    if (!replay.engine_needs.empty()) {
        const std::optional<oamod::EngineRange> range =
            oamod::parse_engine_range(replay.engine_needs);
        const oamod::EngineVersion engine{have.engine_major, have.engine_minor, have.engine_patch};
        if (range.has_value() && !oamod::engine_range_met(*range, engine)) {
            ContentGap gap;
            gap.kind = GapKind::engine;
            gap.version = unmet_version(*range);
            gap.name = gap.version;
            gaps.push_back(std::move(gap));
        }
    }
    return gaps;
}

} // namespace oa::data::replays

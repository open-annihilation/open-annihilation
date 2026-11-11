// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Comparing a replay's needs with what is installed. The release fetched is
// the one whose SHA-256 the replay names, or, for a mod a `.tad` names with
// no release, the one its catalogue offers.

#include "oa/data/replays/content.hpp"

#include "oa/formats/oamod/package_keys.hpp"

#include <algorithm>
#include <cstddef>
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

/// Reports whether the mod being played is the one a name-only need asks for.
bool playing_named(const HaveContent& have, std::string_view name) {
    return !name.empty() && (have.playing_mod_name == name || have.playing_mod_id == name);
}

/// Reports whether a mod need names no release: no version, revision or digest.
bool names_no_release(const ContentNeed& need) {
    return need.version.empty() && !need.revision.has_value() && need.sha256.empty();
}

/// Reports whether a mod, by its id and name, is the one a need names: by
/// id when the need has one, otherwise by the need's name, which a `.tad`'s
/// note writes as the name the player saw or as the id.
bool names_mod(std::string_view id, std::string_view name, const ContentNeed& need) {
    if (!need.id.empty())
        return id == need.id;
    return !need.name.empty() && (name == need.name || id == need.name);
}

/// Reports whether the mod being played is the one a need that names no
/// release asks for. Any release of it is.
bool playing_any_release(const HaveContent& have, const ContentNeed& need) {
    if (!need.id.empty())
        return !have.playing_mod_id.empty() && have.playing_mod_id == need.id;
    return playing_named(have, need.name);
}

/// The listing of a mod a need names by id or name and no release: the
/// release its catalogue offers now. Null when none was given.
const ListedRelease* mod_offer(std::span<const ListedRelease> listed, const ContentNeed& need) {
    for (const ListedRelease& offer : listed) {
        if (offer.package_kind == "oamod" && names_mod(offer.id, offer.name, need))
            return &offer;
    }
    return nullptr;
}

/// Reports whether the mod being played is this installed release.
///
/// The id (or the name, when no id is playing) has to match. When the playing
/// release's version or digest is known, that has to match too: another
/// release of the same mod is not the one being played.
bool playing_release(const HaveContent& have, const HaveMod& installed) {
    if (!have.playing_mod_id.empty()) {
        if (have.playing_mod_id != installed.id)
            return false;
    } else if (!have.playing_mod_name.empty()) {
        if (have.playing_mod_name != installed.name && have.playing_mod_name != installed.id)
            return false;
    } else {
        return false;
    }
    if (!have.playing_mod_version.empty() && !installed.version.empty() &&
        have.playing_mod_version != installed.version)
        return false;
    if (!have.playing_mod_sha256.empty() && !installed.sha256.empty() &&
        have.playing_mod_sha256 != installed.sha256)
        return false;
    return true;
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
    if (offer == nullptr || offer->reach == ReleaseReach::unknown)
        return;
    if (offer->reach == ReleaseReach::unlisted) {
        gap.block = GapBlock::unlisted;
        gap.registry = offer->registry;
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
    // Another release is installed beside the one in use. It is not the
    // ordinary install, so the button keeps the words for that press.
    gap.fetchable = !gap.other_release;
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

/// A gap that plays an installed mod the replay names and is not the one
/// being played: Watch and the index pass play it from that mod's folder.
ContentGap play_installed_mod(const ContentNeed& need, const HaveMod& installed) {
    ContentGap gap = gap_for(GapKind::mod_missing, need);
    if (gap.id.empty())
        gap.id = installed.id;
    // A note that wrote the id is named as the installed mod names itself.
    if (need.name.empty() || need.name == installed.id)
        gap.name = installed.name.empty() ? installed.id : installed.name;
    gap.play_installed = true;
    gap.play_folder = installed.folder;
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
            // A note names the mod and no release: by the name the player
            // saw, or by its id. Any release of it plays the replay.
            // Several releases of one mod may be installed. The one a
            // manifest names is that release; the first folder of the id is not.
            const bool any_release = names_no_release(need);
            if (any_release && playing_any_release(have, need))
                continue;
            const HaveMod* installed = nullptr;
            const HaveMod* different = nullptr;
            for (const HaveMod& mod : have.mods) {
                if (!names_mod(mod.id, mod.name, need))
                    continue;
                if (any_release || same_release(mod, need)) {
                    installed = &mod;
                    break;
                }
                if (different == nullptr)
                    different = &mod;
            }
            if (installed != nullptr) {
                // A release the manifest names has to be the mod this run is
                // playing, not only a folder under Mods. A mod a note names
                // that is not being played is played from its folder too.
                if (!any_release && playing_release(have, *installed))
                    continue;
                ContentGap gap = play_installed_mod(need, *installed);
                gap.any_release = any_release;
                gaps.push_back(std::move(gap));
                continue;
            }
            if (different != nullptr) {
                ContentGap gap = gap_for(GapKind::mod_other, need);
                gap.have_version = different->version;
                gap.have_revision = different->revision;
                gap.other_release = true;
                apply_offer(gap, offer_of(listed, need.sha256));
                gaps.push_back(std::move(gap));
                continue;
            }
            ContentGap gap = gap_for(GapKind::mod_missing, need);
            if (any_release) {
                // The catalogue's release of that mod is fetched, whichever
                // release the replay was recorded with.
                const ListedRelease* offer = mod_offer(listed, need);
                apply_offer(gap, offer);
                gap.any_release = true;
                gap.version.clear();
                // A note that wrote the id is named as the catalogue names it.
                if (offer != nullptr && !offer->name.empty() && !offer->id.empty() &&
                    need.name == offer->id)
                    gap.name = offer->name;
                if (offer != nullptr && offer->reach == ReleaseReach::listed)
                    gap.sha256 = offer->sha256;
            } else {
                apply_offer(gap, offer_of(listed, need.sha256));
            }
            gaps.push_back(std::move(gap));
            continue;
        }
        if (need.kind == "oamap") {
            if (need.map_name.empty() && need.sha256.empty() && need.id.empty())
                continue;
            // A recording names a pack by the package digest, or by the folder
            // digest when the pack was copied by hand. The digest of oamap.yaml
            // is neither. A map held only by name is not that pack.
            const HaveMapPack* matched = nullptr;
            const HaveMapPack* different = nullptr;
            for (const HaveMapPack& pack : have.map_packs) {
                const bool id_ok = need.id.empty() || pack.id.empty() || pack.id == need.id;
                if (!need.sha256.empty() && pack.sha256 == need.sha256 && id_ok) {
                    // Maps wins when it holds this identity. The kept folder
                    // is the one played only when Maps has no such copy.
                    if (matched == nullptr || (matched->kept && !pack.kept))
                        matched = &pack;
                    continue;
                }
                if (!need.id.empty() && pack.id == need.id && different == nullptr)
                    different = &pack;
            }
            if (!need.sha256.empty() && holds(map_packs, need.sha256))
                continue;
            if (matched != nullptr) {
                // Another release, or a kept pack with nothing beside it.
                if (different != nullptr || matched->kept) {
                    ContentGap gap = gap_for(GapKind::map, need);
                    gap.play_installed = true;
                    gap.play_folder = matched->folder;
                    if (!need.sha256.empty())
                        map_packs.push_back(need.sha256);
                    gaps.push_back(std::move(gap));
                }
                continue;
            }
            if (different != nullptr) {
                ContentGap gap = gap_for(GapKind::map, need);
                gap.other_release = true;
                gap.have_version = different->version;
                apply_offer(gap, offer_of(listed, need.sha256));
                if (!need.sha256.empty())
                    map_packs.push_back(need.sha256);
                gaps.push_back(std::move(gap));
                continue;
            }
            // No pack of this id disagrees. A name the player has is enough,
            // including a stock map and a recording that names no digest.
            if (!need.map_name.empty() && holds(have.maps, need.map_name))
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
    // A `.tad` with no note, or a note that names no mod, plays with the mod
    // its folder names when that mod is installed. A folder no installed mod
    // has is not a gap: the replay plays as it always did.
    if (const std::string folder = folder_mod_id(replay); !folder.empty()) {
        ContentNeed need;
        need.kind = "oamod";
        need.id = folder;
        if (!playing_any_release(have, need)) {
            for (const HaveMod& mod : have.mods) {
                if (mod.id != folder)
                    continue;
                ContentGap gap = play_installed_mod(need, mod);
                gap.any_release = true;
                gaps.push_back(std::move(gap));
                break;
            }
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

std::string folder_mod_id(const Replay& replay) {
    if (replay.file != FileKind::tad)
        return {};
    for (const ContentNeed& need : replay.needs) {
        if (need.kind == "oamod")
            return {};
    }
    const std::string_view path = replay.path;
    const std::size_t slash = path.find('/');
    if (slash == std::string_view::npos || slash == 0)
        return {};
    const std::string_view folder = path.substr(0, slash);
    if (folder.front() == '.' || folder == no_mod_recordings_folder ||
        folder == imported_folder_name || folder == core_prime_folder_name)
        return {};
    if (folder == default_id_recordings_folder)
        return std::string(no_mod_recordings_folder);
    return std::string(folder);
}

} // namespace oa::data::replays

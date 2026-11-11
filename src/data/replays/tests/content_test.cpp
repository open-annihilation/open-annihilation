// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Each row of a replay whose content is missing: the mod, another release of
// it, a map, a mutator, an expansion, a newer engine, a package that is not
// listed, and a registry that cannot be reached. A mod a .tad's note names by
// its name or its id, or its Recordings folder names, is played when it is
// installed and fetched from the catalogue when it is not.

#include "oa/data/replays/content.hpp"
#include "oa/test/check.hpp"

#include <span>
#include <string>
#include <vector>

namespace {

namespace replays = oa::data::replays;

replays::ContentNeed
need_of(std::string kind, std::string id, std::string name, std::string version, std::string sha) {
    replays::ContentNeed need;
    need.kind = std::move(kind);
    need.id = std::move(id);
    need.name = std::move(name);
    need.version = std::move(version);
    need.sha256 = std::move(sha);
    return need;
}

replays::ListedRelease
listed_of(std::string sha, std::string kind, std::string id, std::string name, int64_t release) {
    replays::ListedRelease offer;
    offer.sha256 = std::move(sha);
    offer.reach = replays::ReleaseReach::listed;
    offer.registry = "coreprime";
    offer.package_kind = std::move(kind);
    offer.id = std::move(id);
    offer.name = std::move(name);
    offer.version = "2.8";
    offer.release = release;
    offer.size = 40;
    offer.file = "/v1/p/" + offer.sha256 + "." + offer.package_kind;
    return offer;
}

const replays::ContentGap*
gap_kind(const std::vector<replays::ContentGap>& gaps, replays::GapKind kind) {
    for (const replays::ContentGap& gap : gaps) {
        if (gap.kind == kind)
            return &gap;
    }
    return nullptr;
}

} // namespace

void test_replay_content_rows() {
    constexpr std::string_view ridge_sha =
        "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
    constexpr std::string_view newer_sha =
        "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb";
    constexpr std::string_view pack_sha =
        "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc";
    constexpr std::string_view mutator_sha =
        "dddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddddd";

    replays::HaveContent have;
    have.engine_major = 0;
    have.engine_minor = 8;
    have.engine_patch = 14;

    replays::Replay missing;
    replays::ContentNeed ridge = need_of("oamod", "ridge", "Ridge", "2.8", std::string(ridge_sha));
    ridge.revision = 1;
    missing.needs.push_back(ridge);
    const replays::ListedRelease ridge_offer =
        listed_of(std::string(ridge_sha), "oamod", "ridge", "Ridge", 4);
    const std::vector<replays::ContentGap> missing_gaps = replays::compare_needs(
        missing, have, std::span<const replays::ListedRelease>(&ridge_offer, 1)
    );
    OA_CHECK(missing_gaps.size() == 1);
    if (!missing_gaps.empty()) {
        OA_CHECK(missing_gaps[0].kind == replays::GapKind::mod_missing);
        OA_CHECK(missing_gaps[0].name == "Ridge");
        OA_CHECK(missing_gaps[0].version == "2.8");
        OA_CHECK(missing_gaps[0].revision == 1);
        OA_CHECK(missing_gaps[0].fetchable);
        OA_CHECK(missing_gaps[0].release == 4);
        OA_CHECK(missing_gaps[0].sha256 == ridge_sha);
        OA_CHECK(missing_gaps[0].block == replays::GapBlock::none);
    }

    replays::HaveMod installed;
    installed.id = "ridge";
    installed.name = "Ridge";
    installed.version = "2.9";
    installed.revision = 1;
    installed.sha256 = newer_sha;
    have.mods.push_back(installed);
    const std::vector<replays::ContentGap> other = replays::compare_needs(
        missing, have, std::span<const replays::ListedRelease>(&ridge_offer, 1)
    );
    OA_CHECK(other.size() == 1);
    if (!other.empty()) {
        OA_CHECK(other[0].kind == replays::GapKind::mod_other);
        OA_CHECK(other[0].other_release);
        OA_CHECK(!other[0].fetchable);
        OA_CHECK(other[0].version == "2.8");
        OA_CHECK(other[0].revision == 1);
        OA_CHECK(other[0].have_version == "2.9");
        OA_CHECK(other[0].release == 4);
        OA_CHECK(other[0].file.find(ridge_sha) != std::string::npos);
    }

    have.mods[0].version = "2.8";
    have.mods[0].sha256 = ridge_sha;
    const std::vector<replays::ContentGap> installed_aside =
        replays::compare_needs(missing, have, {});
    OA_CHECK(installed_aside.size() == 1);
    if (!installed_aside.empty()) {
        OA_CHECK(installed_aside[0].play_installed);
        OA_CHECK(!installed_aside[0].fetchable);
        OA_CHECK(!installed_aside[0].other_release);
        OA_CHECK(installed_aside[0].id == "ridge");
        OA_CHECK(installed_aside[0].version == "2.8");
    }
    have.playing_mod_id = "ridge";
    OA_CHECK(replays::compare_needs(missing, have, {}).empty());
    have.playing_mod_id.clear();
    have.mods[0].version = "2.9";
    have.mods[0].sha256 = std::string(newer_sha);
    have.playing_mod_id = "ridge";
    have.playing_mod_version = "2.9";
    have.playing_mod_sha256 = std::string(newer_sha);
    replays::HaveMod kept_release;
    kept_release.id = "ridge";
    kept_release.name = "Ridge";
    kept_release.version = "2.8";
    kept_release.revision = 1;
    kept_release.sha256 = std::string(ridge_sha);
    have.mods.push_back(kept_release);
    const std::vector<replays::ContentGap> kept = replays::compare_needs(
        missing, have, std::span<const replays::ListedRelease>(&ridge_offer, 1)
    );
    OA_CHECK(kept.size() == 1);
    if (!kept.empty()) {
        OA_CHECK(kept[0].play_installed);
        OA_CHECK(!kept[0].other_release);
        OA_CHECK(!kept[0].fetchable);
        OA_CHECK(kept[0].block == replays::GapBlock::none);
        OA_CHECK(kept[0].sha256 == ridge_sha);
    }
    replays::ListedRelease offline_offer = ridge_offer;
    offline_offer.reach = replays::ReleaseReach::offline;
    const std::vector<replays::ContentGap> kept_away = replays::compare_needs(
        missing, have, std::span<const replays::ListedRelease>(&offline_offer, 1)
    );
    OA_CHECK(kept_away.size() == 1);
    if (!kept_away.empty()) {
        OA_CHECK(kept_away[0].play_installed);
        OA_CHECK(kept_away[0].block == replays::GapBlock::none);
    }
    have.mods.erase(have.mods.begin());
    have.playing_mod_id.clear();
    have.playing_mod_version.clear();
    have.playing_mod_sha256.clear();
    const std::vector<replays::ContentGap> only_kept = replays::compare_needs(missing, have, {});
    OA_CHECK(only_kept.size() == 1);
    if (!only_kept.empty())
        OA_CHECK(only_kept[0].play_installed);
    have.mods.clear();

    // Two folders of one id. The 2.8 need plays the 2.8 folder, whichever
    // release is listed first. A hand copy names no digest and still does.
    replays::HaveContent ordered;
    ordered.playing_mod_id = "ridge";
    ordered.playing_mod_version = "2.9";
    ordered.playing_mod_sha256 = std::string(newer_sha);
    replays::HaveMod playing_release;
    playing_release.id = "ridge";
    playing_release.name = "Ridge";
    playing_release.version = "2.9";
    playing_release.revision = 1;
    playing_release.sha256 = std::string(newer_sha);
    playing_release.folder = "Mods/ridge";
    replays::HaveMod kept_folder;
    kept_folder.id = "ridge";
    kept_folder.name = "Ridge";
    kept_folder.version = "2.8";
    kept_folder.revision = 1;
    kept_folder.sha256 = std::string(ridge_sha);
    kept_folder.folder = "Mods/ridge-2.8";
    const auto played_folder = [](const std::vector<replays::ContentGap>& gaps) {
        if (gaps.size() != 1 || !gaps[0].play_installed)
            return std::string{};
        return gaps[0].play_folder;
    };
    ordered.mods = {playing_release, kept_folder};
    OA_CHECK(played_folder(replays::compare_needs(missing, ordered, {})) == "Mods/ridge-2.8");
    ordered.mods = {kept_folder, playing_release};
    OA_CHECK(played_folder(replays::compare_needs(missing, ordered, {})) == "Mods/ridge-2.8");
    kept_folder.sha256.clear();
    kept_folder.folder = "Mods/hand-2.8";
    ordered.mods = {playing_release, kept_folder};
    OA_CHECK(played_folder(replays::compare_needs(missing, ordered, {})) == "Mods/hand-2.8");

    replays::Replay map_replay;
    replays::ContentNeed map = need_of("oamap", "fixture-isles", "", "1", std::string(pack_sha));
    map.map_name = "Twin Rivers";
    map_replay.needs.push_back(map);
    replays::ListedRelease pack =
        listed_of(std::string(pack_sha), "oamap", "fixture-isles", "Fixture Isles", 2);
    pack.version = "1";
    const std::vector<replays::ContentGap> map_gaps =
        replays::compare_needs(map_replay, have, std::span<const replays::ListedRelease>(&pack, 1));
    OA_CHECK(map_gaps.size() == 1);
    if (!map_gaps.empty()) {
        OA_CHECK(map_gaps[0].kind == replays::GapKind::map);
        OA_CHECK(map_gaps[0].map_name == "Twin Rivers");
        OA_CHECK(map_gaps[0].pack_name == "Fixture Isles");
        OA_CHECK(map_gaps[0].fetchable);
        OA_CHECK(map_gaps[0].release == 2);
        OA_CHECK(map_gaps[0].sha256 == pack_sha);
    }
    have.maps.push_back("Twin Rivers");
    OA_CHECK(replays::compare_needs(map_replay, have, {}).empty());
    have.maps.clear();

    replays::HaveMapPack same_pack;
    same_pack.id = "fixture-isles";
    same_pack.name = "Fixture Isles";
    same_pack.version = "1";
    same_pack.sha256 = std::string(pack_sha);
    same_pack.folder = "Maps/fixture-isles";
    same_pack.maps = {"Twin Rivers"};
    have.map_packs.push_back(same_pack);
    OA_CHECK(replays::compare_needs(map_replay, have, {}).empty());

    replays::HaveMapPack other_pack = same_pack;
    other_pack.sha256 = std::string(ridge_sha);
    other_pack.version = "2";
    other_pack.folder = "Maps/fixture-isles-new";
    have.map_packs = {other_pack};
    const std::vector<replays::ContentGap> other_pack_gaps =
        replays::compare_needs(map_replay, have, std::span<const replays::ListedRelease>(&pack, 1));
    OA_CHECK(other_pack_gaps.size() == 1);
    if (!other_pack_gaps.empty()) {
        OA_CHECK(other_pack_gaps[0].kind == replays::GapKind::map);
        OA_CHECK(other_pack_gaps[0].other_release);
        OA_CHECK(!other_pack_gaps[0].fetchable);
        OA_CHECK(other_pack_gaps[0].sha256 == pack_sha);
        OA_CHECK(other_pack_gaps[0].file == pack.file);
        OA_CHECK(other_pack_gaps[0].registry == pack.registry);
        OA_CHECK(other_pack_gaps[0].release == 2);
        OA_CHECK(other_pack_gaps[0].have_version == "2");
    }
    have.maps.push_back("Twin Rivers");
    OA_CHECK(replays::compare_needs(map_replay, have, {}).size() == 1);
    have.maps.clear();

    have.map_packs = {other_pack, same_pack};
    const std::vector<replays::ContentGap> kept_pack_gaps =
        replays::compare_needs(map_replay, have, {});
    OA_CHECK(kept_pack_gaps.size() == 1);
    if (!kept_pack_gaps.empty()) {
        OA_CHECK(kept_pack_gaps[0].play_installed);
        OA_CHECK(!kept_pack_gaps[0].fetchable);
        OA_CHECK(!kept_pack_gaps[0].other_release);
        OA_CHECK(kept_pack_gaps[0].play_folder == "Maps/fixture-isles");
    }
    have.map_packs = {same_pack, other_pack};
    const std::vector<replays::ContentGap> kept_pack_again =
        replays::compare_needs(map_replay, have, {});
    OA_CHECK(kept_pack_again.size() == 1);
    if (!kept_pack_again.empty())
        OA_CHECK(kept_pack_again[0].play_folder == "Maps/fixture-isles");

    replays::HaveMapPack kept_only = same_pack;
    kept_only.kept = true;
    kept_only.folder = "Replay releases/kept";
    have.map_packs = {kept_only};
    const std::vector<replays::ContentGap> kept_alone =
        replays::compare_needs(map_replay, have, {});
    OA_CHECK(kept_alone.size() == 1);
    if (!kept_alone.empty()) {
        OA_CHECK(kept_alone[0].play_installed);
        OA_CHECK(kept_alone[0].play_folder == "Replay releases/kept");
    }
    have.map_packs = {kept_only, same_pack};
    OA_CHECK(replays::compare_needs(map_replay, have, {}).empty());
    have.map_packs = {same_pack, kept_only};
    OA_CHECK(replays::compare_needs(map_replay, have, {}).empty());
    have.map_packs.clear();

    replays::Replay mutator_replay;
    mutator_replay.needs.push_back(
        need_of("oamut", "sturdy", "Sturdy Commanders", "1", std::string(mutator_sha))
    );
    const replays::ListedRelease mutator =
        listed_of(std::string(mutator_sha), "oamut", "sturdy", "Sturdy Commanders", 3);
    const std::vector<replays::ContentGap> mutator_gaps = replays::compare_needs(
        mutator_replay, have, std::span<const replays::ListedRelease>(&mutator, 1)
    );
    OA_CHECK(mutator_gaps.size() == 1);
    if (!mutator_gaps.empty()) {
        OA_CHECK(mutator_gaps[0].kind == replays::GapKind::mutator);
        OA_CHECK(mutator_gaps[0].name == "Sturdy Commanders");
        OA_CHECK(mutator_gaps[0].fetchable);
    }
    replays::HaveMutator sturdy;
    sturdy.id = "sturdy";
    have.mutators.push_back(sturdy);
    OA_CHECK(replays::compare_needs(mutator_replay, have, {}).empty());
    have.mutators.back().sha256 = std::string(mutator_sha);
    OA_CHECK(replays::compare_needs(mutator_replay, have, {}).empty());
    have.mutators.back().sha256 = std::string(ridge_sha);
    const std::vector<replays::ContentGap> other_mutator =
        replays::compare_needs(mutator_replay, have, {});
    OA_CHECK(other_mutator.size() == 1);
    if (!other_mutator.empty())
        OA_CHECK(other_mutator[0].kind == replays::GapKind::mutator);
    have.mutators.clear();

    replays::Replay named_mod;
    replays::ContentNeed ridge_name;
    ridge_name.kind = "oamod";
    ridge_name.name = "Ridge";
    named_mod.needs.push_back(ridge_name);
    const std::vector<replays::ContentGap> name_missing =
        replays::compare_needs(named_mod, have, {});
    OA_CHECK(name_missing.size() == 1);
    if (!name_missing.empty()) {
        OA_CHECK(name_missing[0].kind == replays::GapKind::mod_missing);
        OA_CHECK(name_missing[0].version.empty());
    }
    // The catalogue's Ridge is fetched for a note that names no release. The
    // card names no version, and the download is that release's file.
    replays::ListedRelease ridge_listed =
        listed_of(std::string(newer_sha), "oamod", "ridge", "Ridge", 5);
    ridge_listed.version = "2.9";
    const std::vector<replays::ContentGap> name_offered = replays::compare_needs(
        named_mod, have, std::span<const replays::ListedRelease>(&ridge_listed, 1)
    );
    OA_CHECK(name_offered.size() == 1);
    if (!name_offered.empty()) {
        OA_CHECK(name_offered[0].kind == replays::GapKind::mod_missing);
        OA_CHECK(name_offered[0].fetchable);
        OA_CHECK(name_offered[0].any_release);
        OA_CHECK(name_offered[0].id == "ridge");
        OA_CHECK(name_offered[0].name == "Ridge");
        OA_CHECK(name_offered[0].version.empty());
        OA_CHECK(name_offered[0].sha256 == newer_sha);
        OA_CHECK(name_offered[0].release == 5);
        OA_CHECK(!name_offered[0].play_installed);
    }
    replays::ListedRelease ridge_unlisted;
    ridge_unlisted.package_kind = "oamod";
    ridge_unlisted.name = "Ridge";
    ridge_unlisted.reach = replays::ReleaseReach::unlisted;
    const std::vector<replays::ContentGap> name_unlisted = replays::compare_needs(
        named_mod, have, std::span<const replays::ListedRelease>(&ridge_unlisted, 1)
    );
    OA_CHECK(name_unlisted.size() == 1);
    if (!name_unlisted.empty()) {
        OA_CHECK(name_unlisted[0].kind == replays::GapKind::mod_missing);
        OA_CHECK(name_unlisted[0].block == replays::GapBlock::unlisted);
        OA_CHECK(!name_unlisted[0].fetchable);
    }
    // A note names an installed mod by the name the player saw: it is
    // played from that mod's folder, whichever release is there.
    replays::HaveMod ridge_have;
    ridge_have.id = "ridge";
    ridge_have.name = "Ridge";
    ridge_have.version = "2.8";
    ridge_have.folder = "Mods/ridge";
    have.mods.push_back(ridge_have);
    const auto plays_ridge = [](const std::vector<replays::ContentGap>& gaps) {
        return gaps.size() == 1 && gaps[0].play_installed && gaps[0].play_folder == "Mods/ridge" &&
               gaps[0].id == "ridge" && gaps[0].name == "Ridge" && !gaps[0].fetchable &&
               gaps[0].any_release;
    };
    OA_CHECK(plays_ridge(replays::compare_needs(named_mod, have, {})));
    OA_CHECK(plays_ridge(
        replays::compare_needs(
            named_mod, have, std::span<const replays::ListedRelease>(&ridge_listed, 1)
        )
    ));
    // A note that names the mod by its id.
    replays::Replay id_named;
    replays::ContentNeed ridge_id;
    ridge_id.kind = "oamod";
    ridge_id.name = "ridge";
    id_named.needs.push_back(ridge_id);
    OA_CHECK(plays_ridge(replays::compare_needs(id_named, have, {})));
    // A need that names the id and no release.
    replays::Replay id_need;
    replays::ContentNeed ridge_key;
    ridge_key.kind = "oamod";
    ridge_key.id = "ridge";
    id_need.needs.push_back(ridge_key);
    OA_CHECK(plays_ridge(replays::compare_needs(id_need, have, {})));
    // Being played, it needs nothing, by its name or by its id.
    have.playing_mod_id = "ridge";
    have.playing_mod_name = "Ridge";
    have.playing_mod_version = "2.9";
    OA_CHECK(replays::compare_needs(named_mod, have, {}).empty());
    OA_CHECK(replays::compare_needs(id_named, have, {}).empty());
    OA_CHECK(replays::compare_needs(id_need, have, {}).empty());
    have.playing_mod_id.clear();
    have.playing_mod_name.clear();
    have.playing_mod_version.clear();
    have.mods.clear();
    have.playing_mod_name = "Ridge";
    OA_CHECK(replays::compare_needs(named_mod, have, {}).empty());
    have.playing_mod_name.clear();

    // A .tad with no mod need plays with the mod its Recordings folder names,
    // when that mod is installed. Another folder is no gap.
    replays::Replay in_folder;
    in_folder.file = replays::FileKind::tad;
    in_folder.path = "ridge/2026-10-20 0754 Ashap Plateau.tad";
    replays::ContentNeed folder_map;
    folder_map.kind = "oamap";
    folder_map.map_name = "Ashap Plateau";
    in_folder.needs.push_back(folder_map);
    have.maps.push_back("Ashap Plateau");
    OA_CHECK(replays::folder_mod_id(in_folder) == "ridge");
    OA_CHECK(replays::compare_needs(in_folder, have, {}).empty());
    have.mods.push_back(ridge_have);
    OA_CHECK(plays_ridge(replays::compare_needs(in_folder, have, {})));
    have.playing_mod_id = "ridge";
    OA_CHECK(replays::compare_needs(in_folder, have, {}).empty());
    have.playing_mod_id.clear();
    // A note's mod comes first: the folder is not read.
    replays::Replay noted_in_folder = in_folder;
    replays::ContentNeed other_mod;
    other_mod.kind = "oamod";
    other_mod.name = "Other";
    noted_in_folder.needs.push_back(other_mod);
    OA_CHECK(replays::folder_mod_id(noted_in_folder).empty());
    const std::vector<replays::ContentGap> noted_gaps =
        replays::compare_needs(noted_in_folder, have, {});
    OA_CHECK(noted_gaps.size() == 1);
    if (!noted_gaps.empty()) {
        OA_CHECK(noted_gaps[0].kind == replays::GapKind::mod_missing);
        OA_CHECK(noted_gaps[0].name == "Other");
        OA_CHECK(!noted_gaps[0].play_installed);
    }
    for (const std::string_view path :
         {"default/a.tad",
          "Imported/a.tad",
          "Core Prime/a.tad",
          "a.tad",
          ".replays/a.tad",
          "other/a.tad"}) {
        replays::Replay elsewhere = in_folder;
        elsewhere.path = std::string(path);
        OA_CHECK(replays::compare_needs(elsewhere, have, {}).empty());
    }
    replays::Replay default_id = in_folder;
    default_id.path = "default (mod)/a.tad";
    OA_CHECK(replays::folder_mod_id(default_id) == "default");
    replays::Replay oademo_in_folder = in_folder;
    oademo_in_folder.file = replays::FileKind::oademo;
    OA_CHECK(replays::folder_mod_id(oademo_in_folder).empty());
    OA_CHECK(replays::compare_needs(oademo_in_folder, have, {}).empty());
    have.mods.clear();
    have.maps.clear();

    replays::Replay expansion;
    replays::ContentNeed contingency =
        need_of("expansion", "core-contingency", "Core Contingency", "", "");
    expansion.needs.push_back(contingency);
    have.expansions_have.push_back("core-contingency");
    const std::vector<replays::ContentGap> off = replays::compare_needs(expansion, have, {});
    OA_CHECK(off.size() == 1);
    if (!off.empty()) {
        OA_CHECK(off[0].kind == replays::GapKind::expansion_off);
        OA_CHECK(off[0].turn_on);
        OA_CHECK(!off[0].fetchable);
        OA_CHECK(off[0].name == "Core Contingency");
    }
    have.expansions_on.push_back("core-contingency");
    OA_CHECK(replays::compare_needs(expansion, have, {}).empty());
    have.expansions_on.clear();
    have.expansions_have.clear();

    replays::Replay absent_expansion;
    absent_expansion.needs.push_back(contingency);
    const std::vector<replays::ContentGap> not_installed =
        replays::compare_needs(absent_expansion, have, {});
    OA_CHECK(not_installed.size() == 1);
    if (!not_installed.empty()) {
        OA_CHECK(not_installed[0].kind == replays::GapKind::expansion_missing);
        OA_CHECK(!not_installed[0].turn_on);
        OA_CHECK(!not_installed[0].fetchable);
    }
    replays::ListedRelease expansion_offer;
    expansion_offer.package_kind = "expansion";
    expansion_offer.id = "core-contingency";
    expansion_offer.name = "Core Contingency";
    expansion_offer.reach = replays::ReleaseReach::listed;
    expansion_offer.registry = "coreprime";
    expansion_offer.file = "/v1/files/core-contingency.oaexp";
    expansion_offer.release = 1;
    expansion_offer.size = 4;
    const std::vector<replays::ContentGap> offered = replays::compare_needs(
        absent_expansion, have, std::span<const replays::ListedRelease>(&expansion_offer, 1)
    );
    OA_CHECK(offered.size() == 1);
    if (!offered.empty()) {
        OA_CHECK(offered[0].kind == replays::GapKind::expansion_missing);
        OA_CHECK(offered[0].fetchable);
        OA_CHECK(!offered[0].turn_on);
        OA_CHECK(offered[0].id == "core-contingency");
    }

    replays::Replay engine;
    engine.engine_needs = ">=0.9.2";
    const std::vector<replays::ContentGap> older = replays::compare_needs(engine, have, {});
    OA_CHECK(older.size() == 1);
    if (!older.empty()) {
        OA_CHECK(older[0].kind == replays::GapKind::engine);
        OA_CHECK(older[0].version == "0.9.2");
        OA_CHECK(!older[0].fetchable);
    }
    have.engine_minor = 9;
    have.engine_patch = 2;
    OA_CHECK(replays::compare_needs(engine, have, {}).empty());
    have.engine_minor = 8;
    have.engine_patch = 14;

    replays::ListedRelease unlisted;
    unlisted.sha256 = ridge_sha;
    unlisted.reach = replays::ReleaseReach::unlisted;
    const std::vector<replays::ContentGap> not_listed = replays::compare_needs(
        missing, have, std::span<const replays::ListedRelease>(&unlisted, 1)
    );
    OA_CHECK(not_listed.size() == 1);
    if (!not_listed.empty()) {
        OA_CHECK(not_listed[0].kind == replays::GapKind::mod_missing);
        OA_CHECK(not_listed[0].block == replays::GapBlock::unlisted);
        OA_CHECK(!not_listed[0].fetchable);
        OA_CHECK(not_listed[0].name == "Ridge");
        OA_CHECK(not_listed[0].version == "2.8");
    }

    replays::ListedRelease offline = unlisted;
    offline.reach = replays::ReleaseReach::offline;
    const std::vector<replays::ContentGap> away =
        replays::compare_needs(missing, have, std::span<const replays::ListedRelease>(&offline, 1));
    OA_CHECK(away.size() == 1);
    if (!away.empty()) {
        OA_CHECK(away[0].kind == replays::GapKind::mod_missing);
        OA_CHECK(away[0].block == replays::GapBlock::offline);
        OA_CHECK(!away[0].fetchable);
    }

    replays::ListedRelease newest =
        listed_of(std::string(newer_sha), "oamap", "fixture-isles", "Fixture Isles", 9);
    replays::Replay both;
    both.needs.push_back(map);
    both.needs.push_back(mutator_replay.needs[0]);
    const replays::ListedRelease offers[] = {newest, pack, mutator};
    const std::vector<replays::ContentGap> several = replays::compare_needs(both, have, offers);
    OA_CHECK(several.size() == 2);
    const replays::ContentGap* map_gap = gap_kind(several, replays::GapKind::map);
    const replays::ContentGap* mutator_gap = gap_kind(several, replays::GapKind::mutator);
    OA_CHECK(map_gap != nullptr && mutator_gap != nullptr);
    if (map_gap != nullptr && mutator_gap != nullptr) {
        OA_CHECK(map_gap->fetchable && mutator_gap->fetchable);
        OA_CHECK(map_gap->sha256 == pack_sha);
        OA_CHECK(map_gap->release == 2);
        OA_CHECK(map_gap->release != newest.release);
    }
}

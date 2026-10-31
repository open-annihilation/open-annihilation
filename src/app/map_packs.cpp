// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Map packs installed under Maps: each manifest read once per key, and one
// map checked against the base game.

#include "map_packs.hpp"

#include "oa/app/game_directory.hpp"
#include "oa/app/mod_profile_loader.hpp"
#include "oa/app/package_install/origin.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/base/sha256.hpp"
#include "oa/data/content_bundles.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/data/map_pack/map_name.hpp"
#include "oa/formats/hpi.hpp"

#include <algorithm>
#include <cstdint>
#include <exception>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::app {

namespace fs = std::filesystem;
namespace pack = oa::data::map_pack;
namespace fit = oa::data::map_fit;

namespace {

/// Returns a digest as 64 lower-case hex digits.
///
/// @param digest the digest
/// @return the text
std::string hex_of(const oa::base::sha256::Digest& digest) {
    const auto text = oa::base::sha256::to_hex(digest);
    return std::string(text.data(), text.size());
}

/// Reads a file whole.
///
/// @param file the file
/// @param[out] bytes its bytes
/// @return true when the file was read
bool read_bytes(const fs::path& file, std::vector<uint8_t>& bytes) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return false;
    bytes.assign(std::istreambuf_iterator<char>{in}, {});
    return static_cast<bool>(in) || in.eof();
}

/// Finds a relative path in a folder, each part without case.
///
/// @param folder the folder
/// @param relative the path, '/' between parts
/// @return the file; nothing when a part is missing
std::optional<fs::path> find_entry(const fs::path& folder, std::string_view relative) {
    fs::path at = folder;
    std::string part;
    const auto step = [&](std::string_view name) {
        if (name.empty())
            return true;
        const auto found = entry_without_case(at, name);
        if (!found)
            return false;
        at = *found;
        return true;
    };
    for (const char character : relative) {
        if (character == '/' || character == '\\') {
            if (!step(part))
                return std::nullopt;
            part.clear();
        } else {
            part.push_back(character);
        }
    }
    if (!step(part))
        return std::nullopt;
    return at;
}

/// Returns a file's size, or 0 when it cannot be read.
///
/// @param file the file
/// @return its size in bytes
uint64_t size_of(const fs::path& file) {
    std::error_code error;
    const auto size = fs::file_size(file, error);
    if (error)
        return 0;
    return static_cast<uint64_t>(size);
}

/// A fit that failed before a map could be checked.
///
/// @param detail why
/// @return the fit
fit::Fit store_failure(std::string detail) {
    fit::Fit unfit;
    fit::Failure failure{};
    failure.rule = fit::Rule::complete;
    failure.subject = "the base game";
    failure.detail = std::move(detail);
    unfit.failures.push_back(std::move(failure));
    return unfit;
}

/// One map of a parsed manifest.
///
/// @param folder the pack's folder
/// @param manifest the manifest
/// @param map the map
/// @param key the pack's key
/// @param registry the catalogue registry; empty otherwise
/// @param release the catalogue release; 0 otherwise
/// @return the map
PackMap map_of(
    const fs::path& folder,
    const pack::Manifest& manifest,
    const pack::MapEntry& map,
    const std::string& key,
    const std::string& registry,
    int64_t release
) {
    PackMap listed{};
    listed.name = pack::pack_map_name(map.stem, manifest.id);
    listed.id = manifest.id;
    listed.stem = map.stem;
    listed.title = map.title;
    listed.description = map.description;
    listed.size = map.size;
    listed.pack_name = manifest.name;
    listed.pack_version = manifest.version;
    listed.sha256 = key;
    listed.registry = registry;
    listed.players = map.players;
    listed.release = release;
    listed.folder = folder;
    listed.files = map.files;
    listed.supported_games = manifest.supported_games;
    listed.required_expansions = manifest.required_expansions;
    const std::string terrain = "maps/" + map.stem + ".tnt";
    if (const auto file = find_entry(folder, terrain))
        listed.terrain_bytes = size_of(*file);
    if (!map.preview.empty())
        if (const auto file = find_entry(folder, map.preview))
            listed.preview = *file;
    return listed;
}

} // namespace

MapPacks::MapPacks(fs::path maps_folder) : maps_folder_(std::move(maps_folder)) {
}

void MapPacks::set_manifest_watch(void (*watch)(void*), void* context) {
    watch_ = watch;
    watch_context_ = context;
}

void MapPacks::ensure() const {
    if (!ready_)
        const_cast<MapPacks*>(this)->refresh();
}

std::span<const PackMap> MapPacks::maps() const {
    ensure();
    return maps_;
}

const PackMap* MapPacks::find(std::string_view name) const {
    ensure();
    for (const PackMap& map : maps_)
        if (map.name == name)
            return &map;
    return nullptr;
}

std::vector<std::string> MapPacks::problems() const {
    ensure();
    return problems_;
}

void MapPacks::refresh() {
    maps_.clear();
    problems_.clear();
    ready_ = true;
    std::error_code error;
    if (!fs::is_directory(maps_folder_, error)) {
        cache_.clear();
        return;
    }
    std::vector<std::string> seen;
    for (fs::directory_iterator
             entry{maps_folder_, fs::directory_options::skip_permission_denied, error},
         end;
         !error && entry != end;
         entry.increment(error)) {
        const fs::path path = entry->path();
        const std::string name = path_to_utf8(path.filename());
        if (name.empty() || name.front() == '.')
            continue;
        std::error_code status_error;
        const fs::file_status status = fs::symlink_status(path, status_error);
        if (status_error || !fs::is_directory(status))
            continue;
        seen.push_back(name);
        try {
            const auto origin = package_install::read_origin(path);
            std::string key;
            std::vector<uint8_t> yaml;
            bool yaml_read = false;
            if (origin && origin->sha256) {
                key = hex_of(*origin->sha256);
            } else {
                const auto manifest_path = entry_without_case(path, pack::manifest_file);
                if (!manifest_path || !read_bytes(*manifest_path, yaml)) {
                    problems_.push_back(name + ": its oamap.yaml cannot be read");
                    continue;
                }
                yaml_read = true;
                key = hex_of(oa::base::sha256::digest_of(yaml));
            }
            const auto cached = std::find_if(cache_.begin(), cache_.end(), [&](const auto& item) {
                return item.first == name;
            });
            if (cached != cache_.end() && cached->second.key == key) {
                maps_.insert(maps_.end(), cached->second.maps.begin(), cached->second.maps.end());
                continue;
            }
            if (!yaml_read) {
                const auto manifest_path = entry_without_case(path, pack::manifest_file);
                if (!manifest_path || !read_bytes(*manifest_path, yaml)) {
                    problems_.push_back(name + ": its oamap.yaml cannot be read");
                    if (cached != cache_.end())
                        cache_.erase(cached);
                    continue;
                }
            }
            if (watch_ != nullptr)
                watch_(watch_context_);
            pack::Manifest manifest;
            std::vector<pack::Problem> parse_problems;
            if (!pack::read_manifest(yaml, pack::ManifestUse::package, manifest, parse_problems)) {
                std::string reason = name + ":";
                if (parse_problems.empty())
                    reason += " its oamap.yaml cannot be read";
                for (const pack::Problem& one : parse_problems)
                    reason += " " + pack::describe(one);
                problems_.push_back(std::move(reason));
                if (cached != cache_.end())
                    cache_.erase(cached);
                continue;
            }
            std::string registry;
            int64_t release = 0;
            if (origin && origin->kind == package_install::OriginKind::catalogue) {
                registry = origin->registry;
                release = origin->release;
            }
            Cached kept{};
            kept.key = key;
            for (const pack::MapEntry& map : manifest.maps)
                kept.maps.push_back(map_of(path, manifest, map, key, registry, release));
            maps_.insert(maps_.end(), kept.maps.begin(), kept.maps.end());
            if (cached != cache_.end())
                cached->second = std::move(kept);
            else
                cache_.emplace_back(name, std::move(kept));
        } catch (const std::exception&) {
            problems_.push_back(name + ": its oamap.yaml cannot be read");
        }
    }
    std::erase_if(cache_, [&](const auto& item) {
        return std::find(seen.begin(), seen.end(), item.first) == seen.end();
    });
    std::sort(maps_.begin(), maps_.end(), [](const PackMap& left, const PackMap& right) {
        return left.name < right.name;
    });
}

namespace {

/// Checks one map in a folder against a store that holds the game's content.
///
/// @param store the game's content, mounted; the map's files are laid over it
/// @param folder the map pack's folder, staged or installed
/// @param map the map's manifest entry
/// @param id the pack's id
/// @return every rule the map broke
fit::Fit fit_in_store(
    oa::AssetStore& store, const fs::path& folder, const pack::MapEntry& map, std::string_view id
) {
    oa::PackLayerSpec spec{};
    spec.kind = oa::PackLayerKind::folder;
    spec.location = folder;
    spec.files = fit::layer_files(map, id);
    spec.label = std::string(id);
    std::string error;
    if (!store.mount_pack_layer(std::move(spec), &error))
        return store_failure(error.empty() ? "the map's files cannot be read" : error);
    const oa::data::defs::DataLayout layout{};
    const fit::GameNames names = fit::collect_game_names(store, layout);
    const std::unique_ptr<fit::MapFiles> files = fit::mounted_map_files(store);
    if (!files)
        return store_failure("the map's files cannot be read");
    return fit::check_map(store, names, *files, map, id, layout);
}

/// Tells whether two paths name one folder.
///
/// @param left one path
/// @param right the other
/// @return true when the system finds them the same, or they read the same
bool same_place(const fs::path& left, const fs::path& right) {
    std::error_code error;
    if (fs::equivalent(left, right, error))
        return true;
    return left.lexically_normal() == right.lexically_normal();
}

/// Tells whether a list holds a folder.
///
/// @param folders the folders
/// @param folder the folder
/// @return true when one of them names it
bool listed_folder(std::span<const fs::path> folders, const fs::path& folder) {
    return std::any_of(folders.begin(), folders.end(), [&](const fs::path& listed) {
        return same_place(listed, folder);
    });
}

/// Finds the folder the player's copy of an expansion is in.
///
/// @param user_folder the player's folder
/// @param bundle the expansion
/// @return `Expansions/<base>/<id>`, whose record lists every file of it;
///         nothing when the player has not copied it
std::optional<fs::path>
copied_expansion(const fs::path& user_folder, const oa::data::content_bundles::Bundle& bundle) {
    const fs::path expansions =
        user_folder / std::string(oa::data::content_bundles::managed_expansions_folder);
    std::vector<fs::path> candidates;
    for (const std::string_view game : bundle.extends)
        candidates.push_back(expansions / std::string(game) / std::string(bundle.id));
    if (bundle.extends.empty())
        candidates.push_back(expansions / std::string(bundle.id));
    for (const fs::path& candidate : candidates) {
        const std::vector<std::string> copied = copied_content_bundles(candidate);
        if (std::find(copied.begin(), copied.end(), bundle.id) != copied.end())
            return candidate;
    }
    return std::nullopt;
}

} // namespace

fit::Fit check_base_game_fit(
    const fs::path& game_dir, const fs::path& folder, const pack::MapEntry& map, std::string_view id
) {
    try {
        const GameInstall install = inspect_game_install(game_dir);
        if (!usable(install) || install.folders.empty())
            return store_failure("the base game cannot be read");
        oa::AssetStore store(install.folders);
        for (const fs::path& archive : install.archives) {
            std::string error;
            if (!store.try_mount(archive, &error))
                return store_failure(
                    error.empty() ? "an archive of the base game cannot be read" : error
                );
        }
        return fit_in_store(store, folder, map, id);
    } catch (const std::exception& error) {
        return store_failure(error.what());
    } catch (...) {
        return store_failure("the base game cannot be read");
    }
}

std::vector<fs::path> map_pack_fit_folders(
    std::span<const fs::path> played,
    const fs::path& mod_folder,
    const fs::path& game_dir,
    const fs::path& user_folder,
    std::span<const std::string> required
) {
    namespace bundles = oa::data::content_bundles;
    std::vector<fs::path> folders;
    for (const fs::path& folder : played)
        if (folder.empty() || mod_folder.empty() || !same_place(folder, mod_folder))
            folders.push_back(folder);
    if (folders.empty())
        folders.push_back(game_dir);
    const fs::path base_folder = folders.back();
    folders.pop_back();
    if (!user_folder.empty()) {
        std::vector<std::string_view> asked;
        asked.reserve(required.size());
        for (const std::string& id : required)
            asked.emplace_back(id);
        for (const std::string_view id : bundles::requirement_closure(asked)) {
            const bundles::Bundle* bundle = bundles::find_bundle(id);
            if (bundle == nullptr || bundle->kind != bundles::Kind::expansion)
                continue;
            const std::optional<fs::path> copy = copied_expansion(user_folder, *bundle);
            if (copy && !listed_folder(folders, *copy) && !same_place(*copy, base_folder))
                folders.push_back(*copy);
        }
    }
    folders.push_back(base_folder);
    return folders;
}

fit::Fit check_content_fit(
    std::span<const fs::path> folders,
    const fs::path& folder,
    const pack::MapEntry& map,
    std::string_view id
) {
    if (folders.empty())
        return store_failure("the base game cannot be read");
    if (folders.size() == 1)
        return check_base_game_fit(folders.front(), folder, map, id);
    try {
        // The archives of every folder, mounted as the game mounts a base
        // game and its expansions, with their loose files. An archive that
        // cannot be opened is skipped, as the game skips it.
        oa::AssetStore store(std::vector<fs::path>(folders.begin(), folders.end()));
        std::ignore = store.discover(discovery_plan_of(nullptr));
        if (store.mount_paths().empty())
            return store_failure("the base game cannot be read");
        return fit_in_store(store, folder, map, id);
    } catch (const std::exception& error) {
        return store_failure(error.what());
    } catch (...) {
        return store_failure("the base game cannot be read");
    }
}

namespace {

namespace bundles = oa::data::content_bundles;

/// The old key of Total Annihilation with no mod.
constexpr std::string_view kLegacyBaseKey = "ta-3.1c";

/// The expansion whose tracks do not change the rules. It is left out of the
/// key, and turning it on or off does not change a map's fit.
constexpr std::string_view kMusic = "music";

/// The sentence a catalogue miss uses when no other mod fits.
constexpr std::string_view kCatalogueDetail = "Its catalogue says it does not fit {game}.";

/// Tells whether a list holds an id.
///
/// @param ids the ids
/// @param id the id
/// @return true when it is there
bool id_listed(std::span<const std::string> ids, std::string_view id) {
    for (const std::string& one : ids)
        if (one == id)
            return true;
    return false;
}

/// The base game a missing id stands for.
///
/// @param base_game the id; empty means Total Annihilation
/// @return the id
std::string_view base_id(std::string_view base_game) {
    return base_game.empty() ? bundles::default_base_game_id : base_game;
}

/// The name a player sees for a bundle.
///
/// Total Annihilation drops the version the bundle table carries. Any other
/// id uses the table's name, or the id when the table has none.
///
/// @param id the bundle's id
/// @return the name
std::string bundle_name(std::string_view id) {
    if (id == bundles::default_base_game_id)
        return "Total Annihilation";
    if (const bundles::Bundle* bundle = bundles::find_bundle(id))
        return std::string(bundle->english_name);
    return std::string(id);
}

/// Tells whether an expansion extends a base game.
///
/// @param bundle the expansion
/// @param game the base game's id
/// @return true when it extends that game
bool extends_game(const bundles::Bundle& bundle, std::string_view game) {
    for (const std::string_view extended : bundle.extends)
        if (extended == game)
            return true;
    return false;
}

/// Fills each "{name}" of a text with its value.
///
/// @param text the text
/// @param fields each field's name and value
/// @return the text with its fields filled
std::string filled(
    std::string text, std::initializer_list<std::pair<std::string_view, std::string_view>> fields
) {
    for (const auto& [name, value] : fields) {
        const std::string field = "{" + std::string(name) + "}";
        for (std::size_t at = text.find(field); at != std::string::npos;
             at = text.find(field, at + value.size()))
            text.replace(at, field.size(), value);
    }
    return text;
}

/// The mod a compatible key names, when its last part is `<id>@<release>`.
///
/// @param key the row's name
/// @return `<id>@<release>`; empty when the key names no mod
std::string_view mod_part(std::string_view key) {
    const std::size_t plus = key.rfind('+');
    const std::string_view last = plus == std::string_view::npos ? key : key.substr(plus + 1);
    if (last.find('@') == std::string_view::npos)
        return {};
    return last;
}

/// The id of a mod key, the part before `@`.
///
/// @param mod_key `<id>@<release>`
/// @return the id; the whole key when it has no `@`
std::string_view mod_id(std::string_view mod_key) {
    const std::size_t at = mod_key.find('@');
    return at == std::string_view::npos ? mod_key : mod_key.substr(0, at);
}

/// Another mod a true compatible row names, when it is not the one being played.
///
/// @param rows the map's rows
/// @param mod_key the mod being played; empty when none is
/// @return that mod's id; empty when no other mod fits
std::string other_mod(std::span<const CompatibleFlag> rows, std::string_view mod_key) {
    for (const CompatibleFlag& row : rows) {
        if (!row.fits)
            continue;
        const std::string_view part = mod_part(row.key);
        if (part.empty() || part == mod_key)
            continue;
        return std::string(mod_id(part));
    }
    return {};
}

/// Splits a key on `+`.
///
/// @param key the key
/// @return its parts, in order
std::vector<std::string_view> key_parts(std::string_view key) {
    std::vector<std::string_view> parts;
    std::size_t start = 0;
    for (std::size_t at = key.find('+'); at != std::string_view::npos; at = key.find('+', start)) {
        parts.push_back(key.substr(start, at - start));
        start = at + 1;
    }
    parts.push_back(key.substr(start));
    return parts;
}

} // namespace

std::string map_compatibility_key(
    std::string_view base_game, std::span<const std::string> expansions, std::string_view mod_key
) {
    const std::string_view game = base_id(base_game);
    std::string key(game);
    for (const bundles::Bundle& bundle : bundles::bundles()) {
        if (bundle.kind != bundles::Kind::expansion || bundle.id == kMusic ||
            !extends_game(bundle, game))
            continue;
        if (!id_listed(expansions, bundle.id))
            continue;
        key.push_back('+');
        key.append(bundle.id);
    }
    if (!mod_key.empty()) {
        key.push_back('+');
        key.append(mod_key);
    }
    return key;
}

std::string legacy_compatibility_key(std::string_view base_game, std::string_view mod_key) {
    if (!mod_key.empty())
        return std::string(mod_key);
    if (base_id(base_game) == bundles::default_base_game_id)
        return std::string(kLegacyBaseKey);
    return {};
}

std::optional<bool> compatible_answer(
    std::span<const CompatibleFlag> rows,
    std::string_view base_game,
    std::span<const std::string> expansions_on,
    std::string_view mod_key
) {
    const auto recorded = [&](std::string_view key) -> std::optional<bool> {
        if (key.empty())
            return std::nullopt;
        for (const CompatibleFlag& row : rows)
            if (row.key == key)
                return row.fits;
        return std::nullopt;
    };

    const std::string session = map_compatibility_key(base_game, expansions_on, mod_key);
    if (const std::optional<bool> exact = recorded(session))
        return exact;

    const std::string_view game = base_id(base_game);
    const std::string prefix = std::string(game) + "+";
    int best_count = -1;
    std::optional<bool> best;
    for (const CompatibleFlag& row : rows) {
        if (row.key != game && !row.key.starts_with(prefix))
            continue;
        const std::vector<std::string_view> parts = key_parts(row.key);
        if (parts.empty() || parts.front() != game)
            continue;
        std::string_view named_mod;
        std::size_t expansion_end = parts.size();
        if (parts.back().find('@') != std::string_view::npos) {
            named_mod = parts.back();
            expansion_end = parts.size() - 1;
        }
        if (mod_key.empty()) {
            if (!named_mod.empty())
                continue;
        } else if (named_mod != mod_key) {
            continue;
        }
        bool covered = true;
        int count = 0;
        for (std::size_t index = 1; index < expansion_end; ++index) {
            // Music never changes the rules, so a row that names it matches
            // the same row without it, whether or not music is on.
            if (parts[index] == kMusic)
                continue;
            ++count;
            if (!id_listed(expansions_on, parts[index]))
                covered = false;
        }
        if (!covered || count <= best_count)
            continue;
        best_count = count;
        best = row.fits;
    }
    if (best)
        return best;
    return recorded(legacy_compatibility_key(base_game, mod_key));
}

MapListVerdict map_list_verdict(
    std::span<const std::string> supported_games,
    std::span<const std::string> required_expansions,
    std::string_view base_game,
    std::span<const std::string> expansions_on,
    std::string_view mod_key,
    ListedFit catalogue,
    std::span<const CompatibleFlag> compatible,
    FileFit files
) {
    const std::string_view game = base_id(base_game);
    const bool supports = supported_games.empty() ? game == bundles::default_base_game_id
                                                  : id_listed(supported_games, game);
    if (!supports) {
        MapListVerdict verdict;
        verdict.block = MapListBlock::other_game;
        const bool only_demo =
            supported_games.size() == 1 && supported_games.front() == "totala-demo";
        verdict.demo = only_demo;
        if (!only_demo) {
            const std::string_view shown = supported_games.empty()
                                               ? bundles::default_base_game_id
                                               : std::string_view(supported_games.front());
            verdict.name = bundle_name(shown);
        }
        return verdict;
    }

    for (const bundles::Bundle& bundle : bundles::bundles()) {
        if (bundle.kind != bundles::Kind::expansion || bundle.id == kMusic)
            continue;
        if (!id_listed(required_expansions, bundle.id) || id_listed(expansions_on, bundle.id))
            continue;
        MapListVerdict verdict;
        verdict.block = MapListBlock::needs_expansion;
        verdict.name = std::string(bundle.english_name);
        return verdict;
    }
    for (const std::string& required : required_expansions) {
        if (required == kMusic || id_listed(expansions_on, required) ||
            bundles::find_bundle(required) != nullptr)
            continue;
        MapListVerdict verdict;
        verdict.block = MapListBlock::needs_expansion;
        verdict.name = required;
        return verdict;
    }

    if (catalogue == ListedFit::fits) {
        MapListVerdict verdict;
        verdict.block = MapListBlock::playable;
        return verdict;
    }
    if (catalogue == ListedFit::unfit) {
        MapListVerdict verdict;
        verdict.block = MapListBlock::unfit;
        verdict.final_note = true;
        verdict.name = other_mod(compatible, mod_key);
        if (verdict.name.empty())
            verdict.detail = std::string(kCatalogueDetail);
        return verdict;
    }
    if (!files.known) {
        MapListVerdict verdict;
        verdict.block = MapListBlock::ask_files;
        return verdict;
    }
    if (files.fits) {
        MapListVerdict verdict;
        verdict.block = MapListBlock::playable;
        return verdict;
    }
    MapListVerdict verdict;
    verdict.block = MapListBlock::unfit;
    verdict.final_note = catalogue != ListedFit::unlisted;
    verdict.detail = std::string(files.detail);
    return verdict;
}

MapListVerdict catalogue_map_verdict(
    std::span<const std::string> supported_games,
    std::span<const std::string> required_expansions,
    std::span<const std::pair<std::string, bool>> compatible,
    std::string_view base_game,
    std::span<const std::string> expansions_on,
    std::string_view mod_key,
    bool unkeyed
) {
    // The flags are views of the rows, which outlive the verdict's reading.
    std::vector<CompatibleFlag> flags;
    ListedFit listed = ListedFit::unchecked;
    if (!unkeyed) {
        flags.reserve(compatible.size());
        for (const auto& [key, fits] : compatible)
            flags.push_back(CompatibleFlag{key, fits});
        if (const std::optional<bool> answer =
                compatible_answer(flags, base_game, expansions_on, mod_key))
            listed = *answer ? ListedFit::fits : ListedFit::unfit;
    }
    return map_list_verdict(
        supported_games,
        required_expansions,
        base_game,
        expansions_on,
        unkeyed ? std::string_view{} : mod_key,
        listed,
        flags,
        {}
    );
}

std::string map_list_row_reason(const MapListVerdict& verdict) {
    if (verdict.block == MapListBlock::other_game && verdict.demo)
        return "for the TA demo";
    if (verdict.block == MapListBlock::other_game ||
        (verdict.block == MapListBlock::unfit && !verdict.name.empty()))
        return filled("for {name}", {{"name", verdict.name}});
    if (verdict.block == MapListBlock::needs_expansion)
        return filled("needs {name}", {{"name", verdict.name}});
    if (verdict.block == MapListBlock::unfit)
        return "doesn't fit";
    return {};
}

std::string map_list_explanation(const MapListVerdict& verdict, std::string_view game_name) {
    if (verdict.block == MapListBlock::other_game && verdict.demo)
        return "This map is for the TA demo.";
    if (verdict.block == MapListBlock::other_game ||
        (verdict.block == MapListBlock::unfit && !verdict.name.empty()))
        return filled("This map is for {name}.", {{"name", verdict.name}});
    if (verdict.block == MapListBlock::needs_expansion)
        return filled(
            "Needs {name}, which is off. Turn it on in the Library.", {{"name", verdict.name}}
        );
    if (verdict.block != MapListBlock::unfit)
        return {};
    const std::string reason = filled(
        verdict.detail.empty() ? std::string("it does not fit") : verdict.detail,
        {{"game", game_name}}
    );
    return filled("Doesn't fit {game}: {reason}", {{"game", game_name}, {"reason", reason}});
}

} // namespace oa::app

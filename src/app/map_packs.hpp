// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The map packs installed in the player's Maps folder. Each pack's maps are
// read from its manifest once and kept until the pack's SHA-256 changes.
// Nothing here opens a map's OTA or TNT: a terrain's size is the file's size.
#pragma once

#include "oa/data/map_fit/map_fit.hpp"
#include "oa/data/map_pack/manifest.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

/// One map of an installed map pack, as its manifest lists it.
struct PackMap {
    std::string name{};               ///< `<stem>@<id>`, the name a picker lists
    std::string id{};                 ///< the pack's id
    std::string stem{};               ///< the map's file name without its extension
    std::string title{};              ///< the name players see
    std::string description{};        ///< one line about the map; empty when unwritten
    std::string size{};               ///< the map's width and height, as `<width>x<height>`
    std::string pack_name{};          ///< the pack's name
    std::string pack_version{};       ///< the pack's version
    std::string sha256{};             ///< the pack's key, 64 lower-case hex digits
    std::string registry{};           ///< the catalogue's registry; empty otherwise
    int32_t players{};                ///< how many players the map is for
    int64_t release{};                ///< the catalogue release; 0 otherwise
    uint64_t terrain_bytes{};         ///< the size of the map's TNT file; 0 when it is missing
    std::filesystem::path folder{};   ///< the pack's folder
    std::filesystem::path preview{};  ///< the preview file; empty when the map has none
    std::vector<std::string> files{}; ///< the paths the map loads from the pack
    /// Base games the pack's maps run with, copied from the pack manifest.
    std::vector<std::string> supported_games{};
    /// Expansions that must be on, copied from the pack manifest.
    std::vector<std::string> required_expansions{};
};

/// The map packs in a Maps folder.
///
/// `refresh` reads the folder. A pack's manifest is parsed only when the pack
/// is new or its key changed. The key is the SHA-256 in the pack's origin
/// record, or the SHA-256 of its oamap.yaml when it has no record.
class MapPacks {
  public:

    /// Remembers a Maps folder. Nothing is read until `refresh`.
    ///
    /// @param maps_folder the Maps folder; it need not exist
    explicit MapPacks(std::filesystem::path maps_folder);

    /// Reads Maps again, parsing a manifest only when its pack's key changed.
    ///
    /// A folder whose name starts with '.' is skipped, and so is anything that
    /// is not a folder. A pack whose manifest cannot be read is left out of
    /// `maps` and named by `problems`. A missing Maps folder lists nothing.
    void refresh();

    /// Returns the maps of the last refresh, sorted by name.
    ///
    /// The first call reads the folder.
    ///
    /// @return the maps
    [[nodiscard]] std::span<const PackMap> maps() const;

    /// Returns the map of an exact name.
    ///
    /// The first call reads the folder.
    ///
    /// @param name `<stem>@<id>`
    /// @return the map; null when none has that name
    [[nodiscard]] const PackMap* find(std::string_view name) const;

    /// Returns why packs were left out of the last refresh, one line each.
    ///
    /// The first call reads the folder.
    ///
    /// @return the lines; empty when every pack was read
    [[nodiscard]] std::vector<std::string> problems() const;

    /// Counts each manifest a refresh parses, for a test.
    ///
    /// Hashing a manifest to form a key is not counted. A null function
    /// counts nothing.
    ///
    /// @param watch called with `context` once per manifest parsed
    /// @param context passed to `watch`
    void set_manifest_watch(void (*watch)(void*), void* context);

  private:

    /// Reads the folder when nothing has yet.
    void ensure() const;

    std::filesystem::path maps_folder_{};
    std::vector<PackMap> maps_{};
    std::vector<std::string> problems_{};
    bool ready_{};
    void (*watch_)(void*){};
    void* watch_context_{};

    /// One pack's parsed maps, kept while its key is unchanged.
    struct Cached {
        std::string key{};
        std::vector<PackMap> maps{};
    };

    std::vector<std::pair<std::string, Cached>> cache_{};
};

/// Checks one map in a folder against the base game, with no mod.
///
/// The base store is the game folder's own archives. The map's files are
/// mounted from the folder, and the check uses the base layout. A store that
/// cannot be built is a failure, so the install is refused.
///
/// @param game_dir the base game's folder
/// @param folder the map pack's folder, staged or installed
/// @param map the map's manifest entry
/// @param id the pack's id
/// @return every rule the map broke; a failure when the base game cannot be read
[[nodiscard]] oa::data::map_fit::Fit check_base_game_fit(
    const std::filesystem::path& game_dir,
    const std::filesystem::path& folder,
    const oa::data::map_pack::MapEntry& map,
    std::string_view id
);

/// Lists the folders whose content a map pack's maps are checked against as
/// it installs: where the game finds a map's features.
///
/// They are the folders the game plays without the mod's (the expansions
/// that are on, then the base game), with each expansion the pack's
/// `requiredExpansions` names, and each expansion that one needs, that the
/// player has copied (`Expansions/<base>/<id>` with a record that lists all
/// its files) and that is not listed yet, put before the base game. A mod is
/// never one of them.
///
/// @param played the folders the game plays, highest precedence first: the
///        mod folder when one is layered, the expansions that are on, then the
///        base game; empty means the game folder alone
/// @param mod_folder the mod folder layered over the game; empty when none
/// @param game_dir the base game's folder, used when `played` lists nothing else
/// @param user_folder the player's folder, which holds `Expansions/`; empty for none
/// @param required the expansions the pack needs
/// @return the folders, highest precedence first, the base game last
[[nodiscard]] std::vector<std::filesystem::path> map_pack_fit_folders(
    std::span<const std::filesystem::path> played,
    const std::filesystem::path& mod_folder,
    const std::filesystem::path& game_dir,
    const std::filesystem::path& user_folder,
    std::span<const std::string> required
);

/// Checks one map in a folder against the content of the folders given.
///
/// One folder is the base game alone, checked as check_base_game_fit checks
/// it. With more, their archives, as the game's discovery mounts them from
/// the folders together, and their loose files are the store, so a feature
/// an expansion defines is found as the game finds it. An archive that cannot
/// be opened is skipped, as the game skips it.
///
/// @param folders the base game and its expansions, highest precedence
///        first, as map_pack_fit_folders lists them
/// @param folder the map pack's folder, staged or installed
/// @param map the map's manifest entry
/// @param id the pack's id
/// @return every rule the map broke; a failure when no archive can be read
[[nodiscard]] oa::data::map_fit::Fit check_content_fit(
    std::span<const std::filesystem::path> folders,
    const std::filesystem::path& folder,
    const oa::data::map_pack::MapEntry& map,
    std::string_view id
);

/// The compatibility key of a base game, the expansions that are on, and a mod.
///
/// The base game comes first. An empty base game is Total Annihilation.
/// Each expansion that extends that base game and is on is appended, in the
/// built-in order, as `+<id>`. Music is left out: it does not change the
/// rules, and turning it on or off does not change the key. A mod key is
/// appended last, as `+<mod key>`. An expansion that does not extend the
/// base game is left out, so the demo with any expansions still on is
/// `totala-demo`.
///
/// @param base_game the base game's id; empty means Total Annihilation
/// @param expansions the expansion ids that are on, in any order
/// @param mod_key `<id>@<release>` of the mod being played; empty when none is
/// @return the key, such as `totala+core-contingency+ridge@28`
[[nodiscard]] std::string map_compatibility_key(
    std::string_view base_game, std::span<const std::string> expansions, std::string_view mod_key
);

/// The compatibility key engines before this one look up.
///
/// A mod's key is that mod's `<id>@<release>`. Total Annihilation with no mod
/// is `ta-3.1c`. The demo with no mod has none.
///
/// @param base_game the base game's id; empty means Total Annihilation
/// @param mod_key `<id>@<release>`; empty when no mod is played
/// @return the old key; empty when that game has none
[[nodiscard]] std::string
legacy_compatibility_key(std::string_view base_game, std::string_view mod_key);

/// One row of a map's compatible table.
struct CompatibleFlag {
    std::string_view key{}; ///< the row's name
    bool fits = false;      ///< true when that target fits
};

/// How a catalogue answers for the game being played.
///
/// The new names are tried first (`totala`, `totala+core-contingency`,
/// `totala+<mod>@<release>`, expansions in the built-in order). Music in a
/// row is ignored. A row whose other expansions are all on matches, and the
/// longest such row wins, so a pack
/// published as `totala` still matches when more expansions are on. The old
/// names (`ta-3.1c`, `<mod>@<release>`) are used only when no new name matches,
/// and a new name that is present is never overruled by an old one.
///
/// @param rows the map's compatible rows
/// @param base_game the base game's id; empty means Total Annihilation
/// @param expansions_on the expansion ids that are on
/// @param mod_key `<id>@<release>`; empty when no mod is played
/// @return whether the map fits; nothing when the table does not say
[[nodiscard]] std::optional<bool> compatible_answer(
    std::span<const CompatibleFlag> rows,
    std::string_view base_game,
    std::span<const std::string> expansions_on,
    std::string_view mod_key
);

/// Why a listed map cannot be chosen.
enum class MapListBlock : uint8_t {
    playable,        ///< the map can be played
    other_game,      ///< the pack does not support the base game being played
    needs_expansion, ///< a required expansion is off
    unfit,           ///< the map does not fit the mod, or the files say so
    ask_files,       ///< the catalogue does not say; the files have not been read
};

/// What the catalogue has already said about a map.
enum class ListedFit : uint8_t {
    unlisted,  ///< the catalogue has not listed this release; a file miss is not final
    unchecked, ///< no row for this game; the files decide, and a miss is final
    fits,      ///< the catalogue says the map fits
    unfit,     ///< the catalogue says the map does not fit
};

/// What a file check found, when one has been run.
struct FileFit {
    bool known = false;        ///< a check was run
    bool fits = false;         ///< the files fit
    std::string_view detail{}; ///< the first failure; empty when the files fit
};

/// Why a map is greyed, or that it can be played.
struct MapListVerdict {
    MapListBlock block = MapListBlock::playable;
    /// The pack supports only the demo, so the row reads "for the TA demo".
    bool demo = false;
    /// The other game's name, the expansion's name, or the other mod's id.
    std::string name{};
    /// The file check's or the catalogue's detail, when the row is "doesn't fit".
    std::string detail{};
    /// A miss the log names once. A content block, and a release the catalogue
    /// has not listed, are not.
    bool final_note = false;
};

/// Decides whether a map can be played, and why the list greys it.
///
/// The pack must support the base game, then every required expansion must be
/// on. Music is not required: it does not change the rules. The catalogue is
/// final when it has answered: a fit is playable and a miss is not, and the
/// files are not read either way. A true row that names another mod is the
/// reason "for <id>". When the catalogue has not answered,
/// `files.known` false asks for a file check; a miss then is final unless the
/// release is not listed yet.
///
/// @param supported_games base games the pack supports; empty means Total Annihilation
/// @param required_expansions expansions that must be on
/// @param base_game the base game being played; empty means Total Annihilation
/// @param expansions_on the expansions that are on
/// @param mod_key `<id>@<release>` of the mod being played; empty when none is
/// @param catalogue what the catalogue has said
/// @param compatible the map's compatible rows, read when the catalogue says it does not fit
/// @param files the file check, when one has been run
/// @return the verdict
[[nodiscard]] MapListVerdict map_list_verdict(
    std::span<const std::string> supported_games,
    std::span<const std::string> required_expansions,
    std::string_view base_game,
    std::span<const std::string> expansions_on,
    std::string_view mod_key,
    ListedFit catalogue,
    std::span<const CompatibleFlag> compatible,
    FileFit files
);

/// Decides whether a map a catalogue lists can be played in the game being
/// played, before its pack is fetched, as map_list_verdict decides an
/// installed one: the pack must support the base game, every required
/// expansion must be on, and the map's compatible rows answer for the game
/// as compatible_answer reads them. A mod with no catalogue key reads no
/// rows. When the rows do not answer, the verdict is ask_files: the pack's
/// files decide once it is fetched.
///
/// @param supported_games base games the pack supports; empty means Total Annihilation
/// @param required_expansions expansions that must be on
/// @param compatible the map's compatible rows; empty when the catalogue gives none
/// @param base_game the base game being played; empty means Total Annihilation
/// @param expansions_on the expansions that are on
/// @param mod_key `<id>@<release>` of the mod being played; empty when none is
/// @param unkeyed a mod with no catalogue key is played, so the rows are not read
/// @return the verdict
[[nodiscard]] MapListVerdict catalogue_map_verdict(
    std::span<const std::string> supported_games,
    std::span<const std::string> required_expansions,
    std::span<const std::pair<std::string, bool>> compatible,
    std::string_view base_game,
    std::span<const std::string> expansions_on,
    std::string_view mod_key,
    bool unkeyed
);

/// The short reason a greyed row shows, in English.
///
/// A playable map has none. The words are "for the TA demo", "for {name}",
/// "needs {name}" or "doesn't fit", with the name filled in.
///
/// @param verdict the verdict
/// @return the reason; empty when the map can be played
[[nodiscard]] std::string map_list_row_reason(const MapListVerdict& verdict);

/// The sentence choosing a greyed map shows, in English.
///
/// A pack for another game says which. A required expansion that is off says
/// to turn it on in the Library. A map that does not fit names the other mod,
/// or "Doesn't fit {game}: {reason}".
///
/// @param verdict the verdict
/// @param game_name the game being played, as the player sees it
/// @return the sentence; empty when the map can be played
[[nodiscard]] std::string
map_list_explanation(const MapListVerdict& verdict, std::string_view game_name);

} // namespace oa::app

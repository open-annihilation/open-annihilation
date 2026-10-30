// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The campaign/mission-info object: the loaded campaign TDF, the selected
// mission's OTA header values and the mission placement tables.
#pragma once

#include "oa/data/campaign/directory_list.hpp"
#include "oa/data/match_rules.hpp"

#include "oa/formats/tdf.hpp"

#include <cstddef>
#include <cstdint>

namespace oa {
struct Game;
}

namespace oa::data::campaign {

inline constexpr std::size_t kCampaignNameBytes = 0x100;
inline constexpr std::size_t kCampaignPathBytes = 0x100;
inline constexpr std::size_t kCampaignShortTextBytes = 0x80;
inline constexpr int32_t kMaxCampaignMissions = 256;

// What the object is loaded for (CampaignFile.kind, its first field).
enum class SessionKind : int32_t {
    none = 0,
    campaign = 1,
    skirmish = 2,
    multiplayer = 3,
};

// Indices of CampaignFile.paths, the resolved paths (kCampaignPathBytes each).
enum class CampaignPath : uint32_t {
    campaign = 0,      // camps/<name>.TDF
    mission = 1,       // Maps/<file>.TNT; its size is kept in mission_file_size
    briefing = 2,      // camps/briefs/<brief>.TXT
    narration = 3,     // camps/briefs/<narration>.WAV
    hint = 4,          // camps/hints/<missionhint>.TXT
    glamour = 5,       // <glamour>.PCX
    use_only = 6,      // camps/useonly/<useonlyunits>.TDF
    ai_profile = 7,    // ai/<aiprofile>.txt
    glamour_sound = 8, // camps/briefs/<glamoursound>.WAV
};
inline constexpr uint32_t kCampaignPathCount = 9;

// Bits of MissionUnit.flags.
namespace mission_unit_flag {
inline constexpr uint8_t initial_group_mask = 0x0f;
inline constexpr uint8_t mission_critical = 0x10;
inline constexpr uint8_t ai_ignore = 0x20;
inline constexpr uint8_t ai_priority_target = 0x40;
inline constexpr uint8_t immunity = 0x80;
} // namespace mission_unit_flag

// One schema `units` entry. Strings point into the object's unit string pool
// and are null when the key is absent.
struct MissionUnit {
    const char* unit_name{};       // Unitname
    const char* ident{};           // Ident
    const char* initial_mission{}; // InitialMission
    int32_t x{};                   // XPos << 16
    int32_t y{};                   // YPos << 16
    int32_t z{};                   // ZPos << 16
    int16_t angle{};               // (Angle << 16) / 360
    int16_t health_percent{};      // HealthPercentage, default 100
    int32_t creation_countdown{};
    int16_t build_priority{};
    uint8_t player{}; // 1-based; 0 in the file becomes 1
    uint8_t flags{};  // mission_unit_flag
};

enum class MissionRuleType : int32_t { none = 0, start_position = 1 };

// One schema `specials` entry.
struct MissionRule {
    MissionRuleType type{};
    int32_t index{}; // zero-based start position
    int16_t x{};
    int16_t z{};
};

// One schema `features` entry. An empty name marks an entry skipped for a
// negative coordinate.
struct MissionFeature {
    char name[0x80]{}; // Featurename
    int32_t x{};       // XPos, default -1
    int32_t z{};       // ZPos, default -1
};

// File services. `size` returns -1 when the file is missing; `read` returns
// the number of bytes read or -1. `list` reports bare file names under a
// directory with the given extension. `translate` localises UI text and may
// return null for "no translation"; `message` shows a modal error.
struct CampaignFiles {
    void* context{};
    int32_t (*size)(void* context, const char* path) = nullptr;
    int32_t (*read)(void* context, const char* path, char* buffer, uint32_t capacity) = nullptr;
    void (*list)(
        void* context,
        const char* directory,
        const char* extension,
        void (*visit)(void* visit_context, const char* name),
        void* visit_context
    ) = nullptr;
    const char* (*translate)(void* context, const char* text) = nullptr;
    void (*message)(void* context, const char* text) = nullptr;
    const char* language{}; // optional directory variant ("camps-<language>")
    // Entries a find over the wildcard `pattern` reports other than "." and
    // "..", directories included.
    int32_t (*count)(void* context, const char* pattern) = nullptr;
    // The find walk over the wildcard `pattern`, entry by entry.
    void (*find)(
        void* context,
        const char* pattern,
        void (*visit)(void* user, const FindRecord& record),
        void* user
    ) = nullptr;
    /// Writes, ending in NUL and cut to `capacity`, text naming where the
    /// file comes from, and returns that text's length. Null, or an answer
    /// of -1, means no file is there; the lobby's map hash then keys by path
    /// alone.
    int32_t (*source)(void* context, const char* path, char* out, uint32_t capacity) = nullptr;
};

// The campaign object: the loaded campaign, the bound mission's resolved
// paths and header values, and its placement tables.
struct CampaignFile {
    SessionKind kind{};
    char campaign_name[kCampaignNameBytes]{};
    char paths[kCampaignPathCount][kCampaignPathBytes]{}; // indexed by CampaignPath
    int32_t mission_file_size{};
    oa::formats::tdf::Document campaign;
    char mission_name[kCampaignNameBytes]{};
    char localized_name[kCampaignNameBytes]{};
    char* briefing_text{};
    int32_t mission_index{};
    uint32_t content_hash{}; // map content hash, 0 until computed
    uint32_t header_hash{};  // GlobalHeader body hash
    char description[kCampaignShortTextBytes]{};
    char planet[kCampaignShortTextBytes]{};
    int32_t surface_metal{};
    int32_t min_wind{};
    int32_t max_wind{};
    int32_t gravity{};
    float tidal_strength{}; // -1 when unloaded
    int32_t lava_world{};
    int32_t no_sea_level_trigger{};
    int32_t water_does_damage{};
    int32_t water_damage{};
    float kill_multiplier{};
    float time_multiplier{};
    float metal[10]{};  // per player: [0] HumanMetal, [1] ComputerMetal
    float energy[10]{}; // per player: [0] HumanEnergy, [1] ComputerEnergy
    MissionUnit* units{};
    int32_t unit_count{};
    MissionRule* rules{};
    int32_t rule_count{};
    MissionFeature* features{};
    int32_t feature_count{};
    char memory[kCampaignShortTextBytes]{};
    char num_players[kCampaignShortTextBytes]{};
    // The selected schema and copies of the values the loader also stores in
    // the game block.
    char schema[0x40]{};
    int32_t units_per_player{}; // GlobalHeader maxunits (campaign only)
    int32_t mapping{};
    int32_t line_of_sight{};
    int32_t no_movie{};
};

// What the loaders need from their caller: file services, the game block (may
// be null in tools and tests) and the difficulty the schema is chosen by.
struct CampaignEnv {
    const CampaignFiles* files{};
    Game* game{};
    int32_t difficulty{};   // 0 easy, 1 medium, 2 hard
    int32_t player_count{}; // skirmish/multiplayer schema match; 0 = any
    // The schema type each difficulty's preference order names (ai.difficulty-names).
    match_rules::AiDifficultyNames difficulty_names{};
    /// The installed pack's name for a map, the words players see.
    ///
    /// Called only after that map's file has loaded. Null, or an empty
    /// answer, when this caller has no index or the index has no entry for
    /// the map. The text stays valid through the call that asks for it.
    const char* (*pack_index_name)(void* context, const char* map_name){};
    void* pack_index_context{}; ///< passed to pack_index_name
};

// Game.session_record entries a mission's GlobalHeader sets when the map loads.
inline constexpr int32_t kCampaignCommanderRule = 0; // the game continues after a commander dies
inline constexpr int32_t kCampaignSessionEnabled = 1;

/// Zeroes a campaign object: no campaign, no mission, tidal strength -1.
///
/// @param[out] file campaign object
void campaign_file_init(CampaignFile* file) noexcept;
/// Frees everything a campaign object owns and leaves it reusable.
///
/// @param[in,out] file campaign object
void campaign_file_free(CampaignFile* file) noexcept;

/// Fills the session rules block a campaign mission plays under.
///
/// @param file campaign object with its mission header loaded
/// @param[out] record commander rule 0, the header's mapping and lineofsight, then 1
void campaign_session_record(const CampaignFile* file, int32_t record[4]) noexcept;

/// Constructs the campaign object for a session kind with no campaign loaded.
///
/// @param[out] file campaign object
/// @param kind what the object is loaded for
/// @param env file services, game block and difficulty
void campaign_file_construct(CampaignFile* file, SessionKind kind, const CampaignEnv* env);

/// Loads camps/<name>.TDF and binds mission 0.
///
/// Clears every resolved path first. An empty name unloads.
///
/// @param[in,out] file campaign object
/// @param env file services, game block and difficulty
/// @param name campaign name
/// @return false, after showing the game's message and unloading, when the file is missing
bool campaign_load_file(CampaignFile* file, const CampaignEnv* env, const char* name);

/// Counts the consecutive MISSION<n> sections from MISSION0.
///
/// @param[in,out] file campaign object
/// @return the mission count, at most 256; 0 without a campaign
[[nodiscard]] int32_t campaign_count_missions(CampaignFile* file) noexcept;

/// Writes each mission's own name (its missionname) into rows of kCampaignNameBytes.
///
/// Unnamed missions get a placeholder. Saves name a mission this way and
/// campaign_select_mission finds it by it, in every language;
/// campaign_load_mission_titles gives the names players see.
///
/// @param[in,out] file campaign object
/// @param[out] names rows receiving the names; may be null
/// @param capacity rows in `names`; missions past it are counted but not written
/// @return the mission count
/// @quirk A section that vanishes mid-walk yields zero, as the game does.
int32_t campaign_load_mission_list(
    CampaignFile* file, char (*names)[kCampaignNameBytes], int32_t capacity
) noexcept;

/// Writes the name a mission of the campaign shows under in a language: its
/// "<language>missionname", which wins when present, else its missionname,
/// else the unnamed mission's placeholder, as 3.1c's mission lists show it.
///
/// @param[in,out] file campaign object; its cursor is moved
/// @param index the mission, from 0
/// @param language the language's word, as "German"; null or empty for the
///     mission's own name
/// @param[out] out the name, cut to `capacity - 1` characters; empty on failure
/// @param capacity size of `out` in bytes
/// @return false without a loaded campaign or such a mission
bool campaign_mission_title(
    CampaignFile* file, int32_t index, const char* language, char* out, std::size_t capacity
) noexcept;

/// Writes each mission's name as players see it in a language
/// (campaign_mission_title) into rows of kCampaignNameBytes.
///
/// @param[in,out] file campaign object
/// @param language the language's word; null or empty for the missions' own names
/// @param[out] names rows receiving the names; may be null
/// @param capacity rows in `names`; missions past it are counted but not written
/// @return the mission count
int32_t campaign_load_mission_titles(
    CampaignFile* file, const char* language, char (*names)[kCampaignNameBytes], int32_t capacity
) noexcept;

/// Tests whether a mission index is inside the campaign.
///
/// @param[in,out] file campaign object
/// @param index mission index
/// @return true when index < campaign_count_missions
[[nodiscard]] bool campaign_has_next_mission(CampaignFile* file, int32_t index) noexcept;
/// Steps to the next mission and loads it when one remains.
///
/// @param[in,out] file campaign object
/// @param env file services, game block and difficulty
/// @return false when the bound mission was the last
bool campaign_advance_next_mission(CampaignFile* file, const CampaignEnv* env);
/// Binds a mission by index and loads its mission info.
///
/// @param[in,out] file campaign object
/// @param env file services, game block and difficulty
/// @param index mission index
/// @return false when the mission info does not load
[[nodiscard]] bool campaign_bind_mission(CampaignFile* file, const CampaignEnv* env, int32_t index);

/// Loads the selected mission's OTA header, briefing and placement tables.
///
/// Resolves the campaign's MISSION<n> (campaign sessions, by mission_index) or the named
/// map, reads its GlobalHeader values and briefing text, chooses the schema for the
/// difficulty or player count, and parses the schema's placement tables. Failures show
/// the game's message.
///
/// @param[in,out] file campaign object
/// @param env file services, game block, difficulty and player count
/// @param map_name map to load outside campaigns; ignored in a campaign
/// @return false when the mission or its header cannot be loaded
bool campaign_load_mission_info(CampaignFile* file, const CampaignEnv* env, const char* map_name);

/// Selects a mission for the current session kind.
///
/// Campaigns select by mission display name. Skirmish and multiplayer sessions
/// load the named map. A map of the base game keeps its file name as the name
/// players see, translated when the language is not English. A pack map is
/// named stem\@id; its OTA is read at that name. Players see the name its
/// installed pack gives it (CampaignEnv::pack_index_name). The OTA's
/// missionname is used only when the index has no entry, and another
/// language's own key still comes first. A pack map that gives no title, or
/// whose file is not there, keeps the name the battle room carries.
///
/// @param[in,out] file campaign object
/// @param env file services, game block, difficulty and player count
/// @param name mission display name or map name
/// @return false when no mission matches or it does not load
bool campaign_select_mission(CampaignFile* file, const CampaignEnv* env, const char* name);

/// Frees the unit, rule and feature tables and the briefing text.
///
/// @param[in,out] file campaign object
void campaign_free_owned_blocks(CampaignFile* file) noexcept;

/// Reads the `missionfile` value of MISSION<index>.
///
/// @param[in,out] file campaign object
/// @param index mission index
/// @param[out] out receives the value; empty on failure
/// @param capacity bytes of `out`
/// @return false when the section or value is absent or empty
bool campaign_mission_file(
    CampaignFile* file, int32_t index, char* out, std::size_t capacity
) noexcept;

/// Returns the loaded campaign's name.
///
/// @param file campaign object
/// @return the name, or null when no campaign is loaded
[[nodiscard]] const char* campaign_name_if_loaded(const CampaignFile* file) noexcept;
/// Returns one resolved path of the campaign object.
///
/// @param file campaign object
/// @param path which path
/// @return the path, or null when it is empty
[[nodiscard]] const char* campaign_path(const CampaignFile* file, CampaignPath path) noexcept;
/// Returns the bound mission's briefing text.
///
/// @param file campaign object
/// @return the text, or null when none was read
[[nodiscard]] const char* campaign_briefing_text(const CampaignFile* file) noexcept;
/// Tests whether a mission is bound.
///
/// @param file campaign object
/// @return true when the mission name is not empty
[[nodiscard]] bool campaign_has_mission_name(const CampaignFile* file) noexcept;
/// Returns the session kind the object was set up for (its first word).
///
/// @param file campaign object
/// @return the session kind
[[nodiscard]] SessionKind campaign_kind(const CampaignFile* file) noexcept;
/// Returns the byte size of the bound mission's map file.
///
/// @param file campaign object
/// @return the size; zero while no map is bound, so a nonzero size means a map is selected
[[nodiscard]] int32_t campaign_mission_file_size(const CampaignFile* file) noexcept;
/// Returns the name players see for the bound map.
///
/// A map of the base game shows its file name, translated when a translation
/// exists. A pack map shows the title its OTA gives.
///
/// @param file campaign object
/// @return the name
[[nodiscard]] const char* campaign_localized_name(const CampaignFile* file) noexcept;
/// Returns the bound mission's map name.
///
/// @param file campaign object
/// @return the name, empty while none is bound
[[nodiscard]] const char* campaign_mission_name(const CampaignFile* file) noexcept;
/// Returns the bound mission's position in the campaign's MISSION<n> list.
///
/// @param file campaign object
/// @return the mission index
[[nodiscard]] int32_t campaign_mission_index(const CampaignFile* file) noexcept;
/// Returns the bound mission's planet name, which picks the briefing art.
///
/// @param file campaign object
/// @return the planet name
[[nodiscard]] const char* campaign_planet(const CampaignFile* file) noexcept;

/// Builds "<directory>/<name>.<extension>", preferring the language variant directory.
///
/// "<directory>-<language>/<name>.<extension>" is used when that file exists.
///
/// @param files file services and language; may be null
/// @param[out] out receives the path; empty for an empty name
/// @param capacity bytes of `out`
/// @param directory directory, possibly empty
/// @param name file name; a non-empty extension replaces its own
/// @param extension extension to add
void build_variant_path(
    const CampaignFiles* files,
    char* out,
    std::size_t capacity,
    const char* directory,
    const char* name,
    const char* extension
);

/// Replaces a document with the parsed TDF at a path.
///
/// @param files file services
/// @param[in,out] document document to replace
/// @param path file path
/// @param[out] error why the file did not load: the parse failure of a malformed file,
///     too_large for one over formats::tdf::max_input_bytes, ok for one that is missing or
///     cannot be read; may be null
/// @return false when the file is missing, oversized or malformed
bool load_tdf(
    const CampaignFiles* files,
    oa::formats::tdf::Document* document,
    const char* path,
    oa::formats::tdf::ParseError* error = nullptr
);

/// Picks the schema for a session kind and writes its name.
///
/// Campaigns take the first schema of the difficulty's type in preference order; the
/// types are named by the difficulty names, Easy, Medium and Hard in 3.1c.
/// Skirmish and multiplayer sessions take a multiplayer schema whose start-position
/// count matches `players`, else the one with the most start positions, and leave the
/// document cursor on it.
///
/// @param kind session kind
/// @param[in,out] ota parsed OTA file; its cursor moves
/// @param difficulty 0 easy, 1 medium, 2 hard (campaigns)
/// @param players player count to match; 0 matches any
/// @param[out] schema_name receives "Schema N"; null makes skirmish and multiplayer kinds
///        report the first multiplayer schema without counting start positions
/// @param capacity bytes of `schema_name`
/// @param names which of 3.1c's names each difficulty carries (ai.difficulty-names);
///        3.1c's when left out
/// @return false when no schema matches
/// @quirk The names replace the types the preference order tries, not the order: with
///        Easy and Hard swapped, difficulty 1 tries Medium, Hard, Easy.
bool find_matching_schema(
    SessionKind kind,
    oa::formats::tdf::Document* ota,
    int32_t difficulty,
    int32_t players,
    char* schema_name,
    std::size_t capacity,
    const match_rules::AiDifficultyNames& names = {}
);

/// Selects the schema for a difficulty and leaves the cursor on it.
///
/// The preference order is easy->medium->hard, medium->easy->hard or hard->medium->easy.
///
/// @param[in,out] ota parsed OTA file
/// @param difficulty 0 easy, 1 medium, 2 hard
/// @param[out] schema_name receives "Schema N"
/// @param capacity bytes of `schema_name`
/// @return false when no schema matches
bool select_difficulty_schema(
    oa::formats::tdf::Document* ota, int32_t difficulty, char* schema_name, std::size_t capacity
) noexcept;

/// Parses the schema's units, specials and features into the object's tables.
///
/// @param[in,out] file campaign object
/// @param schema_name "Schema N" section to read
/// @param[in,out] ota parsed OTA file
/// @return false when the tables cannot be read or allocated
bool campaign_parse_mission_data(
    CampaignFile* file, const char* schema_name, oa::formats::tdf::Document* ota
) noexcept;

/// Counts the campaign files, the entries of "camps/*.TDF".
///
/// The language variant is used only when a file of that name opens, which a wildcard
/// never does. The new-campaign screen shows its campaign list for more than two.
///
/// @param files file services
/// @return the count; 0 without a counting service
[[nodiscard]]
int32_t campaign_count_files(const CampaignFiles* files);

/// Lists the campaigns of a side, NUL-separated.
///
/// camps\*.TDF is counted, listed without extensions and sorted by name, then each is
/// loaded; names whose HEADER campaignside is `side` or "ALL" are kept. A name
/// already tried, compared without case, is the same campaign file from a lower
/// layer and is left out. The first copy is the top layer. A file that does not
/// load, malformed or oversized, is left out and reported through `files->message`,
/// once per call; the names after it are listed.
///
/// @param files file services
/// @param side side name
/// @param[out] out receives the names, NUL-separated, ending with an empty name
/// @param capacity bytes of `out`
/// @return the number of names written
int32_t
campaign_load_names(const CampaignFiles* files, const char* side, char* out, std::size_t capacity);

/// Tells whether a side has any campaign, without listing the names.
///
/// The walk is campaign_load_names's: the same files, the same order, the
/// same reports for a file that does not load. It stops once both sides
/// have been found, so a later file is not opened. A file that names "ALL"
/// counts for both sides. A null pointer is left alone.
///
/// @param files file services
/// @param[out] arm set when an Arm or all-sides campaign loads; null to skip
/// @param[out] core set when a Core or all-sides campaign loads; null to skip
void campaign_sides_present(const CampaignFiles* files, bool* arm, bool* core);

} // namespace oa::data::campaign

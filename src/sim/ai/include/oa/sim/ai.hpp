// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Computer player: the per-player controller that sorts units into squads,
// runs one task per squad (factory production, construction, strike groups,
// rallies, air raids, the siege of sighted bases) and chooses builds from the
// AI profile weights/limits.
#pragma once

#include "oa/core/world.h"
#include "oa/data/limits.hpp"
#include "oa/data/match_rules.hpp"
#include "oa/sim/detection.hpp"

#include <cstdint>
#include <cstdio>
#include <span>
#include <string_view>

namespace oa::sim::ai {

inline constexpr uint32_t squad_count = 10;
inline constexpr uint32_t type_categories_bytes = 256;
inline constexpr uint32_t sort_interval_ticks = 30;
inline constexpr int32_t unlimited = -1;

/// Unit.squad values the computer player sorts its units into.
enum class Squad : uint32_t {
    none = 0,
    structures = 1,  // unarmed buildings: factories, economy
    land_strike = 2, // gathered land attack group
    land_army = 3,   // armed land units waiting to join the strike
    builders = 4,    // mobile builders
    armed_structures =
        5, // defences; no task unless ai.squad5-factory-tick gives them the structures task
    naval_strike = 6,
    navy = 7,
    aircraft = 8,
    siege = 9, // the sort assigns none; a unit already in squad 9 stays
};

enum class TaskKind : uint8_t {
    none,
    structures,
    strike,
    rally,
    construction,
    idle,
    air_raid,
    siege, // searches the sighted enemy for the richest area and attacks it
};

/// The type fields the computer player reads, gathered from the unit
/// definitions and their build lists.
struct ComputerType {
    char unit_name[32]{};
    char side[32]{};
    char categories[type_categories_bytes]{}; // space-separated FBI categories
    char ai_directives[64]{};                 // FBI ai_weight: script lines
    uint32_t flags{};                         // OA_UNIT_DEF_FLAG_*
    uint32_t abilities{};                     // OA_UNIT_DEF_ABILITY_*
    int8_t bm_code{};
    int8_t makes_metal{};
    int16_t min_water_depth{};
    int16_t max_water_depth{};
    float energy_use{}; // FBI EnergyUse; negative for a producer
    int16_t footprint_x{};
    int16_t footprint_z{};
    float extracts_metal{};
    uint8_t has_build_list{}; // builders always own a (possibly empty) list
    uint16_t build_count{};
    // build_count type ids in ComputerPlayers::build_id_block; null without a list
    uint16_t* build_ids{};
};

/// One squad task of a controller; ComputerPlayer.tasks holds one per squad.
struct ComputerTask {
    TaskKind kind{};
    Squad squad{};
    uint32_t next_tick{};         // Game.tick the task runs next
    int32_t min_size{};           // strike: size that may attack
    int32_t launch_size{};        // strike: size that always attacks
    int32_t merge_radius{};       // strike: squared world-unit radius per member
    Squad source_squad{};         // strike: squad it recruits from; rally: its strike squad
    int32_t attacking{};          // strike: nonzero while the squad attacks
    oa::FixedVec3 siege_target{}; // siege: the point the squad attacks, 16.16
    oa::FixedVec3 siege_probe{};  // siege: the point the search looks at, 16.16
    oa::FixedVec3 siege_step{};   // siege: the search's step from one look to the next, 16.16
    int32_t siege_weight{};       // siege: sighted base weight around siege_target
};

/// Cell of a metal-bearing feature.
struct MetalSpot {
    int16_t cell_x{};
    int16_t cell_z{};
};

/// Randomised building-placement grid of a computer player's knowledge.
struct PlacementGrid {
    int16_t step_x{};
    int16_t step_z{};
    int16_t phase_x{};
    int16_t phase_z{};
    int32_t margin{};
};

/// Per-player knowledge: profile weights and limits, own counts, base position
/// and build placement state, each table indexed by unit type.
/// The match keeps the knowledge's sightings half in sim::detection::Sightings
/// and rebuilds both halves in one walk.
struct ComputerKnowledge {
    int16_t* owned_counts{};   // finished own units per type
    int8_t* base_weights{};    // the type's weight in the base-position average
    uint8_t* weight_percent{}; // 0..100
    uint32_t* weight_locked{}; // nonzero once a weight line named the type itself
    int32_t* limits{};         // unlimited = -1
    uint32_t* limit_locked{};  // nonzero once a limit line named the type itself
    int32_t builder_count{};   // finished units with a build list
    oa::FixedVec3 base_position{};
    int32_t placement_radius{};
    PlacementGrid land_grid{};
    PlacementGrid water_grid{};
    MetalSpot* metal_spots{};
    uint32_t metal_spot_count{};
};

/// A player's controller (Player.controller). Every player but a mirrored one
/// has a controller, whose weapon-sweep cursor the match holds; only computer
/// players get one here.
struct ComputerPlayer {
    uint8_t present{};
    uint8_t player{};
    int32_t sort_countdown{};          // ticks to the next squad sort
    uint32_t commander_build_tick{};   // set 30..329 ticks ahead when a capturer is hit
    ComputerTask tasks[squad_count]{}; // indexed by squad
    ComputerKnowledge knowledge{};
};

/// Everything the computer players need that is not in oa::World.
struct ComputerPlayers {
    ComputerType* types{}; // type_count entries; index 0 reserved
    uint32_t type_count{};
    // How many CANBUILD entries a builder's list keeps (30 in 3.1c); a list
    // holds one entry more, which only the download menus fill. Set before
    // computer_players_initialize.
    data::limits::BuildLists build_lists{};
    // type_count lists of data::limits::build_list_kept(build_lists) + 1 type
    // ids, one for each type, which ComputerType::build_ids point into.
    uint16_t* build_id_block{};
    // The rules the match plays by (Match::rules_view), the ai.* hacks among
    // them; set before computer_players_initialize and at every pass. Unset,
    // 3.1c's.
    data::match_rules::MatchRulesView rules{};
    ComputerPlayer players[OA_PLAYER_COUNT]{};
    uint8_t initialized{};
    uint8_t downloadables_restricted{}; // options state 1 skips downloadable types
    // The last plan line named the difficulty; kept between passes. It starts
    // matched, so the lines before a profile's first plan line apply.
    bool plan_matches{true};
    // A difficulty chosen for one computer player (0 easy, 1 medium, 2 hard).
    // difficulty_set false leaves that player on the game's difficulty, which
    // is what a zeroed ComputerPlayer would have meant and must not.
    bool difficulty_set[OA_PLAYER_COUNT]{};
    uint8_t difficulty[OA_PLAYER_COUNT]{};
    char* profile_text{};
    uint32_t profile_length{};
    // Every unit type's build list (UnitDef.build_ids: its SIDEDATA CANBUILD
    // entries, then the download menus' entries), as runs of a count and that
    // many type ids, from type 0 on.
    uint16_t* build_list_runs{};
    uint32_t build_list_run_length{};
};

/// Function-pointer boundary to the match runtime. Order calls return false
/// when the runtime rejects the order.
struct ComputerHost {
    void* context{};
    oa::World* world{};
    int32_t difficulty{};  // OA_DIFFICULTY_*
    int32_t map_cells_x{}; // Game.map_width; world width is cells << 4
    int32_t map_cells_z{};
    uint32_t (*random)(void* context, uint32_t bound){};
    uint32_t (*squad_size)(void* context, uint8_t player, Squad squad){};
    uint16_t (*squad_member)(void* context, uint8_t player, Squad squad, uint32_t i){};
    void (*set_squad)(void* context, uint16_t unit, Squad squad){};
    bool (*allied)(void* context, uint8_t player, uint8_t other){};
    /// Head of the unit's primary order queue: false when empty.
    bool (*primary_order)(
        void* context, uint16_t unit, uint8_t* preserve_flags, uint8_t* queue_flags
    ){};
    /// Whether the unit's secondary order queue, where a silo's stockpile builds run, holds an
    /// order; null holds none.
    bool (*secondary_order)(void* context, uint16_t unit){};
    bool (*unit_visible)(void* context, uint8_t player, uint16_t unit){};
    /// Strength triple (strengths[type][0..2]) from the strategic refresh, or
    /// null when the player has none yet.
    const uint8_t* (*strengths)(void* context, uint8_t player, uint16_t type){};
    bool (*site_clear)(void* context, uint16_t type, int32_t cell_x, int32_t cell_z){};
    bool (*building_site)(void* context, uint16_t type, int32_t cell_x, int32_t cell_z){};
    uint8_t (*cell_metal)(void* context, int32_t cell_x, int32_t cell_z){};
    /// Cell holds an indestructible metal-bearing feature.
    bool (*metal_feature)(void* context, int32_t cell_x, int32_t cell_z){};
    int32_t surface_metal{}; // scenario SurfaceMetal
    bool (*order_move)(void* context, uint16_t unit, const oa::FixedVec3* to, bool queue){};
    bool (*order_patrol)(void* context, uint16_t unit, const oa::FixedVec3* to, bool queue){};
    /// Gives a unit the attack command on a target, in place of its orders.
    bool (*order_attack)(void* context, uint16_t unit, uint16_t target){};
    bool (*order_build)(void* context, uint16_t unit, uint16_t type, const oa::FixedVec3* at){};
    bool (*order_factory)(void* context, uint16_t factory, uint16_t type, int32_t count){};
    void (*set_active)(void* context, uint16_t unit, bool on){};
    /// Whether a player sees a point: its line of sight, or where the game keeps no sight
    /// grid, the mapped area; null sees nothing.
    bool (*point_visible)(void* context, uint8_t player, const oa::FixedVec3* at){};
    /// A player's sightings of other players' units; null, or a null result, sights none.
    const sim::detection::Sightings* (*sightings)(void* context, uint8_t player){};
    /// Whether a unit's first weapon reaches a point from where the unit stands; null
    /// reaches nothing.
    bool (*weapon_reaches)(void* context, uint16_t unit, const oa::FixedVec3* at){};
    /// Gives a unit the order the Attack command gives over open ground at a point, in
    /// place of its orders; false when the unit takes no such order.
    bool (*order_attack_point)(void* context, uint16_t unit, const oa::FixedVec3* at){};
    /// Notes the section the built-in player is about to run. Null notes nothing.
    ///
    /// @param context ComputerHost::context
    /// @param player player index, 0 to 9
    /// @param section `section_sort`, a task's squad index, or `section_start`
    void (*note_section)(void* context, uint8_t player, uint8_t section){};
};

/// The squad sort. A task's section is its squad index, 1 to 9.
inline constexpr uint8_t section_sort = 0;
/// The set-up of one computer player's controller, before its first set-up draw.
inline constexpr uint8_t section_start = 10;

/// Stores the AI profile text and the unit types' build lists applied at the next
/// initialisation.
///
/// A computer player's builder chooses from its type's whole build list, the list the
/// game gives the type: its SIDEDATA CANBUILD entries, then the entries the download
/// menus add for it.
///
/// @param[in,out] state computer players; marked uninitialised
/// @param profile ai/<profile>.txt text
/// @param build_lists for each type from 0 on, the number of ids in its list
///        (UnitDef.build_ids) followed by those ids
/// @return false for a null state or when a copy cannot be allocated
bool computer_players_configure(
    ComputerPlayers* state, std::string_view profile, std::span<const uint16_t> build_lists
) noexcept;
/// Allocates a zeroed type table.
///
/// @param[in,out] state computer players; any earlier table is freed
/// @param type_count entries, including the reserved index 0
/// @return false for a null state or when out of memory
bool computer_players_reserve_types(ComputerPlayers* state, uint32_t type_count) noexcept;
/// Frees every table and text the computer players own and clears the state.
///
/// @param[in,out] state computer players; null does nothing
void computer_players_release(ComputerPlayers* state) noexcept;

/// Creates the computer players' controllers as the game starts.
///
/// Loads the build lists, creates a controller and knowledge record for every
/// computer player, applies the profile and each downloadable type's own directives,
/// and scans the metal spots for every player with a controller.
///
/// @param[in,out] state computer players; types must be loaded
/// @param host world, map, difficulty and random stream
/// @return false for a null state, world or type table
bool computer_players_initialize(ComputerPlayers* state, const ComputerHost& host) noexcept;

/// Gives a computer player the controller it would have had at the start.
///
/// +AI hands a human to the computer after the game has started. The next
/// order tick of the machine that simulates that player calls this. A player
/// who already has a controller, or who is not a computer, is left alone, and
/// the other computers' weights are not scaled again.
///
/// @param[in,out] state computer players; nothing before the first initialisation
/// @param host world, map and random stream
/// @param player player index
void computer_players_adopt(
    ComputerPlayers* state, const ComputerHost& host, uint8_t player
) noexcept;

/// Notes the difficulty one computer player plays at.
///
/// Call it before computer_players_initialize. A value outside easy, medium
/// and hard is ignored. A later profile pass takes up a change made after.
///
/// @param[in,out] state computer players; null does nothing
/// @param player player index
/// @param difficulty OA_DIFFICULTY_EASY, MEDIUM or HARD
void computer_players_set_difficulty(
    ComputerPlayers* state, uint8_t player, int32_t difficulty
) noexcept;

/// Returns the difficulty noted for one computer player.
///
/// @param state computer players; null, or a player with none noted, returns fallback
/// @param player player index
/// @param fallback the game's difficulty
/// @return the noted difficulty, or fallback
[[nodiscard]] int32_t
computer_player_difficulty(const ComputerPlayers* state, uint8_t player, int32_t fallback) noexcept;

/// Reloads the AI profile (ReloadAIProfiles).
///
/// Resets every computer player's weight and limit tables, then applies the profile and
/// the downloadable types' directives again from the plan flag the last pass left.
/// Controllers, build grids and metal spots are kept. Before the first initialisation
/// only the profile is stored.
///
/// @param[in,out] state computer players
/// @param host world and difficulty
/// @param profile new ai/<profile>.txt text
/// @return false for a null state or world, or when the copy cannot be allocated
bool computer_players_reload_profile(
    ComputerPlayers* state, const ComputerHost& host, std::string_view profile
) noexcept;

/// Tests whether a player gets a controller (Player.controller) as it starts.
///
/// @param player player record
/// @return true for every player with a status except a mirrored one
[[nodiscard]] inline bool player_has_controller(const oa::Player& player) noexcept {
    return player.status != OA_PLAYER_STATUS_FREE &&
           (player.in_use == 0 || player.status != OA_PLAYER_STATUS_MIRRORED);
}

/// Runs a computer player's order tick: the squad sort every 30 ticks, then every due task.
///
/// @param[in,out] state computer players
/// @param host squads, orders, placement queries, random stream and world
/// @param player player index; nothing for a player without a computer controller
void computer_player_tick_orders(
    ComputerPlayers* state, const ComputerHost& host, uint8_t player
) noexcept;

/// Sums the base weights of the units a player has sighted near a point.
///
/// Every unit on the seen list counts, active or not, whose squared distance from the
/// point on the ground plane (sim::detection::squared_distance_high) is at most the
/// radius squared, the square kept to 32 bits. The weight is the player's base weight
/// for the unit's type (ComputerKnowledge.base_weights), signed.
///
/// @param state computer players, for the type count
/// @param knowledge the player's knowledge
/// @param sightings the player's sightings
/// @param world unit table
/// @param at the point, 16.16 world coordinates
/// @param radius world units
/// @return the sum, with 32-bit wraparound; a type outside the table adds nothing
[[nodiscard]] int32_t computer_sighted_weight(
    const ComputerPlayers* state,
    const ComputerKnowledge& knowledge,
    const sim::detection::Sightings& sightings,
    const oa::World& world,
    const oa::FixedVec3& at,
    int32_t radius
) noexcept;

/// Weighted position sums of the own units counted in one knowledge rebuild.
struct KnowledgeTally {
    float sum_x{};
    float sum_y{};
    float sum_z{};
    float weights{};
};

/// Returns a computer player's knowledge.
///
/// @param state computer players
/// @param player player index
/// @return the knowledge, or null for any other player
[[nodiscard]] ComputerKnowledge*
computer_player_knowledge(ComputerPlayers* state, uint8_t player) noexcept;
/// Starts a knowledge rebuild: no own units counted, none of them build-capable.
///
/// @param state computer players, for the type count
/// @param[in,out] knowledge record being rebuilt
void computer_knowledge_clear(const ComputerPlayers* state, ComputerKnowledge& knowledge) noexcept;
/// Counts one finished own unit the rebuild's walk reports.
///
/// Adds its type to the owned counts, counts it as a builder when it has a build list,
/// and adds its position weighted by its type's base weight.
///
/// @param state computer players and their type table
/// @param[in,out] knowledge record being rebuilt
/// @param unit finished own unit
/// @param[in,out] tally weighted position sums
void computer_knowledge_count(
    const ComputerPlayers* state,
    ComputerKnowledge& knowledge,
    const oa::Unit& unit,
    KnowledgeTally& tally
) noexcept;
/// Ends a knowledge rebuild: the weighted mean position becomes the base position.
///
/// @param[in,out] knowledge record being rebuilt
/// @param tally weighted position sums; zero weight keeps the sums unscaled
void computer_knowledge_settle(ComputerKnowledge& knowledge, const KnowledgeTally& tally) noexcept;

/// Applies an AI profile ("plan", "weight", "limit" lines) to every controller.
///
/// The three directives behave as the console's ai_plan, ai_weight and ai_limit; the
/// plan flag starts matched, so the lines before the first plan line apply.
///
/// @param[in,out] state computer players
/// @param host world and difficulty
/// @param text profile text
void computer_profile_apply(
    ComputerPlayers* state, const ComputerHost& host, std::string_view text
) noexcept;

/// One profile line, as the built-in player's tokenizer stores it.
///
/// `number` counts from 1, one per `\n`, including a line that has no words.
/// A file with no final newline still has that last line. `words` is valid
/// only while the visit runs: the words live in the tokenizer's stack
/// storage. A line is cut into at most 20 words, separated by space, tab,
/// `\r`, `\n`, `\v` or `\f`. `#` ends the line, even inside a word, and that
/// word ends there too. The words of one line share 126 bytes, so a long
/// word is cut and a full line stops early.
struct ProfileLine {
    uint32_t number{};          ///< line number, from 1, one per `\n`
    uint32_t count{};           ///< stored words, 1 to 20
    const char* const* words{}; ///< stored words; valid only during the visit
};

/// Walks a profile text with the built-in player's tokenizer.
///
/// Only a line with at least one word is reported. The first word is the
/// command. Compared ignoring ASCII case, `plan`, `weight` and `limit` are
/// the commands the player applies; anything else, including `//`, is a line
/// and is not applied. A `plan` line matches when it has at least one word
/// after `plan` and either the first of those is `any` (ignoring case; a
/// later `any` counts for nothing) or one of them equals the difficulty
/// keyword `easy`, `medium` or `hard`. A `plan` with no further word matches
/// nothing. The player's plan flag starts matched, so lines before the first
/// `plan` apply, and the player keeps that flag from one pass to the next.
/// `weight <name> <factor>` reads the factor with
/// `static_cast<float>(std::strtod(word, nullptr))` of the third word, which
/// is `""` when the word is missing, so the factor is 0. `limit <name>
/// <count>` reads the count with `static_cast<int32_t>(std::strtol(word,
/// nullptr, 10))`; `-1` is unlimited. This walk does not apply either
/// command, and it does not apply a downloadable type's own lines a second
/// time: the player does that itself.
///
/// @param text profile text, or empty
/// @param visit called once per line that has a word; null does nothing
/// @param context passed through to `visit`
void computer_profile_lines(
    std::string_view text, void (*visit)(void* context, const ProfileLine& line), void* context
) noexcept;

/// Applies a "weight <unit or category> <percent>" line to one player.
///
/// Scales the weight of every unlocked matching type; a unit name matches exactly and
/// locks that type.
///
/// @param[in,out] state computer players
/// @param player player index; nothing without a controller
/// @param token unit name or FBI category ("all" matches every type)
/// @param percent factor applied to the weight percentage, clamped to 0..100
void computer_apply_weight(
    ComputerPlayers* state, uint8_t player, const char* token, float percent
) noexcept;
/// Applies a "limit <unit or category> <count>" line to one player.
///
/// Sets the limit of every unlocked matching type; a unit name matches exactly and
/// locks that type.
///
/// @param[in,out] state computer players
/// @param player player index; nothing without a controller
/// @param token unit name or FBI category ("all" matches every type)
/// @param limit most units of the type; -1 is unlimited
void computer_apply_limit(
    ComputerPlayers* state, uint8_t player, const char* token, int32_t limit
) noexcept;

/// Returns a type's build priority for a player.
///
/// Needs at least 50 energy and 25 metal, the type within its limit, and a strength
/// triple. The economy, attack and priority strengths are shared by the player's metal
/// need, energy need and the rest, then scaled by the type's weight percentage.
///
/// @param state computer players and their type table
/// @param host world and strength triples
/// @param player player index
/// @param type unit type index
/// @return the score; 0 rejects the type
[[nodiscard]] int32_t computer_build_score(
    const ComputerPlayers* state, const ComputerHost& host, uint8_t player, uint16_t type
) noexcept;
/// Picks a type from a builder's list at random, weighted by build score.
///
/// @param state computer players and their type table
/// @param host world, strengths and random stream
/// @param player player index
/// @param builder builder's unit slot
/// @return the type, or 0 when nothing qualifies or the pick belongs to another side
[[nodiscard]] uint16_t computer_pick_build(
    const ComputerPlayers* state, const ComputerHost& host, uint8_t player, uint16_t builder
) noexcept;

/// Map-context paths the weight report prints; null prints as "(null)".
struct ComputerReportPaths {
    const char* terrain{}; // path slot 1, Maps/<map>.TNT
    const char* profile{}; // path slot 7, ai/<profile>.txt
};

/// Writes the "PrintWeights" report of one player.
///
/// Game time, name, controller, terrain, profile and difficulty, then one row per unit
/// type with its limit, strength triple, weight percentage and names. The rows
/// need the player's controller; without one only the header is written.
///
/// @param state computer players and their type table
/// @param host world and strength triples
/// @param player player index
/// @param paths terrain and profile paths; null prints as "(null)"
/// @param out stream written to
void computer_write_report(
    const ComputerPlayers* state,
    const ComputerHost& host,
    uint8_t player,
    const ComputerReportPaths& paths,
    std::FILE* out
) noexcept;

/// Returns the squad a freshly sorted unit joins.
///
/// By 3.1c's rules a building joins the armed structures when armed and the structures
/// otherwise; a mobile unit joins the builders when it builds, the aircraft when it flies,
/// the navy when it needs water (MinWaterDepth above 0), and the land army when armed,
/// leaving any other unit unsorted. By the role-squad rules (ai.squad-assignment) a building
/// joins the structures when it builds or its EnergyUse is 58.5 or more, and the armed
/// structures otherwise; a mobile unit joins the builders, the aircraft, the navy when it
/// needs water, can go 128 deep (MaxWaterDepth) or is amphibious, and otherwise the land
/// army, armed or not.
///
/// @param unit unit being sorted
/// @param type its computer-player fields
/// @param rules which rules sort it; 3.1c's when left out
/// @return the squad, or none
/// @quirk The role-squad EnergyUse test compares the high 16 bits of the float as a signed
///        number above 0x4269, so 58.25 up to just under 58.5 counts as below, and any
///        negative EnergyUse too.
[[nodiscard]] Squad computer_sort_squad(
    const oa::Unit& unit,
    const ComputerType& type,
    data::match_rules::AiSquadAssignmentRules rules =
        data::match_rules::AiSquadAssignmentRules::base
) noexcept;

} // namespace oa::sim::ai

namespace oa::sim::match_runtime {
class Match;
}

namespace oa::sim::ai {
/// Returns the computer-player state owned by a match, creating it on first use.
///
/// @param match match that owns the state
/// @return the state
ComputerPlayers* match_computer_players(sim::match_runtime::Match& match);
/// Returns the host through which a match's computer players read the match and give their
/// orders.
///
/// @param match match the host reads and orders; it must outlive the host
/// @return the host
ComputerHost match_computer_host(sim::match_runtime::Match& match) noexcept;
/// Configures a match's computer players as the session starts (computer_players_configure).
///
/// @param match match that owns the state
/// @param profile ai/<profile>.txt text
/// @param build_lists for each type from 0 on, the number of ids in its list
///        (UnitDef.build_ids) followed by those ids
/// @param campaign_session true in a campaign, where computer players never build a
///        downloadable type
/// @return false when the profile or the lists cannot be stored
[[nodiscard]] bool configure_match_computer_players(
    sim::match_runtime::Match& match,
    std::string_view profile,
    std::span<const uint16_t> build_lists,
    bool campaign_session
);
/// Reloads a match's AI profile (computer_players_reload_profile).
///
/// @param match match that owns the state
/// @param profile new ai/<profile>.txt text
/// @return false when the profile cannot be stored
[[nodiscard]] bool
reload_match_computer_profiles(sim::match_runtime::Match& match, std::string_view profile);
/// Loads the types and initialises a match's computer players before their first tick.
///
/// The game does it as the game starts.
///
/// @param match match that owns the state
void prepare_match_computer_players(sim::match_runtime::Match& match);
/// Runs computer_player_tick_orders for one player of a match.
///
/// @param match match that owns the state
/// @param player player index
void tick_match_computer_orders(sim::match_runtime::Match& match, uint8_t player);

/// Runs one built-in order tick when a brain log is open.
///
/// Return true when this match's tick was run. Return false when this match
/// is not logging, and the usual tick runs.
using LoggedComputerTurn = bool (*)(
    void* context, sim::match_runtime::Match& match, uint8_t player, const ComputerHost& host
);

/// Installs the function `tick_match_computer_orders` asks first.
///
/// A null function restores the usual tick. The function returns false when
/// that match is not logging, so another match is left alone.
///
/// @param turn the function, or null
/// @param context passed to `turn`
void set_logged_computer_turn(LoggedComputerTurn turn, void* context) noexcept;
/// Writes a match player's weight report (computer_write_report).
///
/// @param match match that owns the state
/// @param player player index
/// @param paths terrain and profile paths
/// @param out stream written to
void write_match_computer_report(
    sim::match_runtime::Match& match,
    uint8_t player,
    const ComputerReportPaths& paths,
    std::FILE* out
);
/// Stores the tick until which a hit capturing unit of the player takes no build task.
///
/// @param match match that owns the state
/// @param player player index; nothing when the match has no controller for it
/// @param tick game tick the hold ends
void hold_capturer_builds(sim::match_runtime::Match& match, uint8_t player, uint32_t tick);
} // namespace oa::sim::ai

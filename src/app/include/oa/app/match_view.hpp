// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The table through which a tool reads the running match's units, their
// orders and their types, and the map, the sight grids and the radar
// contacts (runtime_match_view.cpp). The MCP server's unit and map tools
// are the callers. The table only copies; it does not change the match.
// begin_match_watch listens to the match's event hooks and keeps the notes
// pull_match_notes hands on. The notes are what the hooks reported, at the
// match's tick, and the watch adds nothing of its own to the match.
// What a caller is shown is a separate choice: every live unit is copied,
// and shown says whether the local player's view shows it
// (Match::unit_shown). A caller that lists units drops the ones that are
// not shown, unless it was asked for full awareness and the match allows
// it. The map tools make the same choice for another player's sight and
// contacts. apply_give_orders is not part of the table: with --mcp and
// --give-orders it gives a skirmish the orders a headless save/load run
// gives, once, before the tools read.
//
// The header names only the standard library's types, so a caller needs
// nothing more of the engine's to read it.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace oa::app {

class Runtime;

/// One live unit, copied out of the match.
struct UnitRecord {
    uint16_t id{};     ///< the unit's slot; slot 0 is never a unit
    std::string type;  ///< the type's unit name, as the definition spells it (ARMCOM)
    std::string name;  ///< the words the player sees: the description, or the type's name
    std::string side;  ///< the type's side
    uint8_t owner{};   ///< the owning player, 0 to 9
    int32_t x{};       ///< map pixel, the whole part of the unit's position
    int32_t y{};       ///< map pixel
    int32_t z{};       ///< map pixel
    int32_t health{};  ///< hit points the unit has now
    int32_t build{};   ///< how finished the unit is, 0 to 100
    bool selected{};   ///< the player has the unit selected
    bool radar{};      ///< the unit is a radar contact or in sight
    bool cloaked{};    ///< the unit is cloaked
    bool shown{};      ///< the local player's view shows the unit
    std::string order; ///< the primary queue's first order, or Standby when that queue is empty
};

/// One order from a unit's queue, head first.
struct OrderRecord {
    std::string kind;      ///< the mission's name; empty when the kind is not a known mission
    uint16_t target{};     ///< the target unit, or 0 for none
    int32_t x{};           ///< the order's point, map pixel
    int32_t y{};           ///< map pixel
    int32_t z{};           ///< map pixel
    int32_t parameter_1{}; ///< the order's first parameter; a build's product type
    int32_t parameter_2{}; ///< the order's second parameter; a build's remaining count
    uint8_t phase{};       ///< how far the order has got
    uint32_t issue_tick{}; ///< the tick the order was given
    uint8_t facing{};      ///< quarter turns from south a building is placed facing
    bool build_started{};  ///< the order already has the unit it is building
    int32_t seen_x{};      ///< where the order last saw its target, map pixel
    int32_t seen_z{};      ///< map pixel
    std::string build;     ///< the product type's unit name, for a build order; empty otherwise
};

/// A unit's primary and secondary order queues.
struct OrderQueues {
    std::vector<OrderRecord> primary;   ///< the primary queue, head first
    std::vector<OrderRecord> secondary; ///< the secondary queue, head first
    bool primary_truncated{};           ///< the primary queue was longer than the copy
    bool secondary_truncated{};         ///< the secondary queue was longer than the copy
};

/// One unit type, copied out of the match.
struct TypeRecord {
    uint16_t index{};                 ///< the type's index; 0 is not a type
    std::string type;                 ///< the unit name (ARMCOM)
    std::string name;                 ///< the words the player sees
    std::string description;          ///< the definition's description
    std::string side;                 ///< the type's side
    std::vector<std::string> builds;  ///< unit names the type can build, in the build list's order
    std::vector<std::string> weapons; ///< the type's weapons, the definition's key, or its name
    int32_t speed{};                  ///< maximum speed, 16.16 fixed, as the definition stores it
    int32_t sight{};                  ///< sight distance, as the definition stores it
    int32_t radar{};                  ///< radar distance, as the definition stores it
    int32_t sonar{};                  ///< sonar distance, as the definition stores it
};

/// One feature standing on the map, at the plot that holds its origin.
struct MapFeature {
    std::string name;      ///< the feature's name
    int32_t x{};           ///< map pixel of the origin plot's corner
    int32_t z{};           ///< map pixel
    int32_t footprint_x{}; ///< plots across
    int32_t footprint_z{}; ///< plots down
};

/// Metal on one cell of the height grid.
struct MetalSpot {
    int32_t x{};      ///< the cell's column
    int32_t z{};      ///< the cell's row
    int32_t amount{}; ///< the metal of the plots the cell covers, added
};

/// The map a caller asked for, at one cell size.
struct MapInfo {
    std::string name;  ///< the name the player sees; empty when the map has none
    int32_t width{};   ///< map pixels across
    int32_t height{};  ///< map pixels down
    int32_t plots_x{}; ///< plots across
    int32_t plots_z{}; ///< plots down
    int32_t sea{};     ///< the sea level, in height units
    int32_t cell{};    ///< the cell size, in map pixels
    int32_t cells_x{}; ///< cells across
    int32_t cells_z{}; ///< cells down
    std::vector<uint8_t>
        heights; ///< one height per cell, row by row, the plot at the cell's centre
    std::vector<MetalSpot> metal;     ///< cells whose metal is not zero
    std::vector<MapFeature> features; ///< features, origin plots only
    bool features_truncated{};        ///< features was longer than the copy
};

/// Which visibility grid a caller asked for.
enum class VisibilityLayer : uint8_t {
    sight,    ///< ground the player is shown in sight
    explored, ///< ground the player is shown as explored
    radar,    ///< cells holding a radar contact
    sonar,    ///< cells holding a sonar contact
    jammed,   ///< cells holding a jammed unit
};

/// One visibility grid.
///
/// cells is 0 or 1, row by row. minimap is the minimap's own reading of the
/// same cells, for sight and explored: 1 where the minimap shows that layer
/// on. It is empty for the other layers, and when the minimap's grid is not
/// the one asked for.
struct VisibilityGrid {
    int32_t width{};              ///< cells across
    int32_t height{};             ///< cells down
    int32_t cell{};               ///< the cell size, in map pixels
    std::vector<uint8_t> cells;   ///< 0 or 1, row by row
    std::vector<uint8_t> minimap; ///< the minimap's reading; empty when it has none
};

/// One radar or sonar contact, with no identity.
struct RadarContact {
    int32_t x{};  ///< map pixel
    int32_t y{};  ///< map pixel
    int32_t z{};  ///< map pixel
    bool radar{}; ///< a radar contact
    bool sonar{}; ///< a sonar contact
};

/// What a caller may read of the running match's units.
///
/// match_view() sets every entry. context, which every entry is passed
/// back, is the runtime. An entry that reads the match does so on the
/// thread that runs main(), and the records it writes are the caller's.
struct MatchView {
    void* context{};
    /// Tells whether a match is running.
    ///
    /// @param context MatchView::context
    /// @return true while a match is running
    bool (*active)(void* context){};
    /// Returns the match's tick.
    ///
    /// @param context MatchView::context
    /// @return the tick, or 0 while no match is running
    uint32_t (*tick)(void* context){};
    /// Returns the local player, whose view shown is taken from.
    ///
    /// @param context MatchView::context
    /// @return the player, 0 to 9
    uint8_t (*viewer)(void* context){};
    /// Tells whether a caller may read units the local player is not shown.
    ///
    /// A shared match does not allow it. Any other match does.
    ///
    /// @param context MatchView::context
    /// @return true when full awareness is allowed
    bool (*full_awareness_allowed)(void* context){};
    /// Copies every live unit. A unit the view does not show is copied too,
    /// with shown false, so a caller can tell it from a unit that is not there.
    ///
    /// @param context MatchView::context
    /// @param[out] listed replaced by the live units
    void (*units)(void* context, std::vector<UnitRecord>* listed){};
    /// Copies one live unit's primary and secondary queues.
    ///
    /// @param context MatchView::context
    /// @param id the unit's slot
    /// @param[out] queues replaced by the two queues
    /// @return false when id is not a live unit; queues is then empty
    bool (*orders)(void* context, uint16_t id, OrderQueues* queues){};
    /// Copies every unit type the match has loaded.
    ///
    /// @param context MatchView::context
    /// @param[out] catalog replaced by the types
    void (*types)(void* context, std::vector<TypeRecord>* catalog){};
    /// Copies the map at a cell size.
    ///
    /// @param context MatchView::context
    /// @param cell the cell size, in map pixels, at least 1
    /// @param[out] info replaced by the map
    /// @return false when no match is running, or the cell is too small for the map
    bool (*map_info)(void* context, int32_t cell, MapInfo* info){};
    /// Copies one player's visibility grid at a cell size.
    ///
    /// The caller decides whether that player may be read. A cell of 32 is
    /// one sight cell, the cell the minimap's fog is drawn from.
    ///
    /// @param context MatchView::context
    /// @param player the player, 0 to 9
    /// @param layer which grid
    /// @param cell the cell size, in map pixels, at least 1
    /// @param[out] grid replaced by the grid
    /// @return false when it cannot be read
    bool (*visibility)(
        void* context, uint8_t player, VisibilityLayer layer, int32_t cell, VisibilityGrid* grid
    ){};
    /// Copies one player's radar and sonar contacts, without identity.
    ///
    /// A contact is a live unit of another side that the player's picture
    /// marks as a radar or sonar contact. The caller decides whether that
    /// player may be read.
    ///
    /// @param context MatchView::context
    /// @param player the player, 0 to 9
    /// @param[out] listed replaced by the contacts, and empty when the picture is not kept
    /// @param[out] kept set when the picture is the one the match keeps for that player
    /// @return false when no match is running
    bool (*contacts)(
        void* context, uint8_t player, std::vector<RadarContact>* listed, bool* kept
    ){};
};

/// Returns the table through which a caller reads the running match's units.
///
/// @param runtime the running game, which the table's entries read
/// @return the table, every entry set and bound to runtime
[[nodiscard]] MatchView match_view(Runtime& runtime);

/// How many match notes the watch keeps. Older notes are dropped.
inline constexpr std::size_t match_note_keep = 4096;

/// One note the match watch took from a hook, not yet handed on.
struct MatchNote {
    uint32_t tick{};  ///< the match's tick when the hook ran
    std::string kind; ///< the note's kind
    std::string json; ///< the note as one JSON object, with event and tick
};

/// Listens to the running match's hooks: units, orders and who the local
/// player sees. A later call for the same match does nothing. A match that
/// has gone is left alone, and the next match is listened to on its own.
///
/// The hooks only read the match. With no match running, this does nothing.
///
/// @param runtime the running game
void begin_match_watch(Runtime& runtime);

/// Notes a change in the match's life, as the extension's match hook names it.
///
/// The value is MatchEvent. The note's kind is `match` and its change is the
/// hook's word. The watch does not have to be listening: the note is kept
/// either way, so a match ending is not lost with the match.
///
/// @param runtime the running game
/// @param life the hook's value
void note_match_life(Runtime& runtime, uint8_t life);

/// Notes radar contacts the local player has gained since the last sample.
///
/// A contact is another player's unit carrying the radar mark. The note names
/// the place and not the unit. The same contact is noted once, until the mark
/// goes and comes back. The tick is the match's tick now.
///
/// @param runtime the running game
void sample_match_contacts(Runtime& runtime);

/// Hands on the notes taken since the last pull, oldest first, and clears them.
///
/// @param[out] notes replaced by those notes; empty when there were none
void pull_match_notes(std::vector<MatchNote>* notes);

/// Gives a skirmish the orders a headless save/load run gives, once.
///
/// It runs only when the launch asked for those orders. A campaign, a
/// shared match and a replay are left alone. The orders are given on the
/// first pump of a match that already has a live unit of the local
/// player's, and a later pump of the same match does not give them again.
/// A failure is written to the log. A headless run never reaches it, so
/// that run's orders stay the ones it gives itself.
///
/// @param runtime the running game
void apply_give_orders(Runtime& runtime);

} // namespace oa::app

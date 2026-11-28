// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/core/unit.h"
#include "oa/sim/ground_orders/goals.hpp"
#include "oa/sim/ground_orders/navigator.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>

namespace oa::sim::spatial_state {
struct CellClassifier;
struct World;
} // namespace oa::sim::spatial_state

namespace oa::sim::ground_orders {

/// Tests the static footprint without scanning its speed-class perimeter.
///
/// This has the same blocked/passable result as classify_movement_cell with
/// occupancy_before_tick zero: buildings and features block; mobiles do not.
///
/// @param movement movement class footprint, slopes and water limits
/// @param x footprint origin column
/// @param z footprint origin row
/// @param world canonical spatial plots and occupants
/// @return whether the complete static footprint is passable
[[nodiscard]] bool static_flow_passable(
    const sim::spatial_state::CellClassifier& movement,
    int32_t x,
    int32_t z,
    const sim::spatial_state::World& world
) noexcept;

/// Bounded shared-route storage and work. Construction allocates; ticks do not.
struct FlowLimits {
    uint32_t fields{64};
    uint32_t total_cells{2'097'152};
    uint32_t work_per_tick{2048};
    uint32_t route_probes_per_tick{65'536};
    /// Movement class and player pairs whose coarse landmasses are labelled,
    /// each with room for two nearest-node maps; zero labels none, and every
    /// goal is routed as given. With more pairs asking, they take turns.
    uint32_t landmass_slots{};
    /// Landmass work a tick: nodes scanned or expanded and edges tested while
    /// labelling, with sixteen cleared label entries, sixteen cells of a small
    /// landmass labelled as naming none, or eight nodes of a nearest-node
    /// map's pass, counted as one.
    uint32_t landmass_work_per_tick{32'768};
    /// Ninths of a breadth-first level a field spends reaching a coarse node
    /// for each of its player's own ground units in the 3x3 coarse nodes
    /// around it (FlowSampler::ground_units), so that its routes go round
    /// crowds; zero builds every field by distance alone. A map of 2^24 coarse
    /// nodes or more builds by distance alone too.
    uint32_t crowd_weight{};
    /// Ticks a field that met a crowd keeps after it completes before it is
    /// built again around the crowds of that moment, if a request asked for
    /// it since it completed; zero keeps it. Used only with a crowd weight.
    uint32_t crowd_rebuild_ticks{};
};

/// A movement class and a player's knowledge of the static map.
struct FlowContext {
    uint32_t movement_class{}; ///< stable movement-class table index, 0 through 31
    uint32_t player{};         ///< player slot, 0 through 9
    uint32_t static_revision{};
    uint64_t knowledge_epoch{};
    uint32_t footprint_x{};
    uint32_t footprint_z{};
};

/// Reads static passability without treating moving units as walls.
struct FlowSampler {
    void* context{};
    /// Returns 0 blocked, otherwise passable; unknown terrain must not reveal a wall.
    uint8_t (*cell)(
        void* context, uint32_t movement_class, uint32_t player, int32_t x, int32_t z
    ){};
    /// Returns how many of the player's own live ground units stand at the
    /// coarse node (column, row), the same count for the whole tick; asked
    /// only for nodes on the map, and only with a crowd weight. Null adds no
    /// delay.
    uint32_t (*ground_units)(void* context, uint32_t player, int32_t column, int32_t row){};
};

/// Whether a shared route can replace a full per-unit search.
enum class FlowResult : uint8_t { fallback, pending, ready };

/// Cumulative work counters, excluding ordinary full searches.
struct FlowCounters {
    uint64_t requests{}, hits{}, fields_started{}, cells_cleared{}, cells_expanded{};
    uint64_t waits{}, invalidations{}, fallbacks{};
};

/// Shares bounded reverse fields between nearby destinations of one movement class/player.
///
/// A coarse node lies every two map cells. Every connection is checked at
/// footprint-cell resolution, including diagonal corner clearance. Narrow
/// corridors that miss the coarse graph use the ordinary search. Fields are
/// admitted only after a second matching request. Construction/clearing work
/// is divided between ticks; building fields are never evicted by new goals.
/// No field is rooted at a goal that connects to no coarse node, and a field
/// whose root cannot be connected when it is built is given up for 300 ticks.
///
/// With landmass slots, the cache labels the coarse nodes of each movement
/// class and player by landmass: the ground a footprint can walk across, step
/// by step between footprint cells, with ground the player has not seen
/// counted as passable. A unit on a landmass of at least 256 nodes whose goal
/// is blocked, off the map or on another landmass is routed to the node of its
/// own landmass nearest the goal instead, and a field serves only the
/// landmass it was built for. A goal in a passage too narrow for any coarse
/// node is left to the ordinary search. The labels are kept through map
/// changes and labelled again in the background; the work is bounded per tick.
/// With more movement classes and players asking than there are slots, or
/// more landmasses than maps, each takes its turn: a slot held 900 ticks
/// passes on, one every 300 ticks.
///
/// With a crowd weight, a field's breadth-first search puts off each node it
/// discovers by (units * weight + 4) / 9 levels, at most 255, where units are
/// the field's player's own ground units in the 3x3 coarse nodes around it
/// that tick: the node waits in the queue, one level a turn, before it is
/// expanded, so the field's routes lead round crowds where a way of about the
/// same length is free. A field that met a crowd, and that a request asked
/// for since it completed, is built again, from the crowds of that moment,
/// once the crowd rebuild ticks have passed since it completed: one field a
/// refresh period, the longest completed first, when no field the map changed
/// under is due.
///
/// The state is canonical byte storage with no pointers and may be saved, but
/// is owner-local navigation state and must not enter shared simulation hashes.
class FlowCache {
  public:

    /// Allocates bounded storage before simulation ticks.
    ///
    /// Destinations share a field when they fall in the same 96-cell region.
    /// A route's prefix holds at most 32 coarse edges, and a blocked edge
    /// marks its field for the bounded refresh instead of discarding it.
    ///
    /// @param width map width in footprint cells
    /// @param height map height in footprint cells
    /// @param limits maximum fields, total coarse cells and work per tick
    FlowCache(uint32_t width, uint32_t height, FlowLimits limits = {});

    /// Clears at most 65,536 direction bytes and spends the configured expansion budget.
    /// Seed connections consume expansion credits; clearing uses contiguous slices.
    /// A node a crowd puts off spends one credit for each level it waits.
    ///
    /// @param tick deterministic simulation tick
    /// @param static_revision current buildings/features/terrain revision
    /// @param sampler current passability and player knowledge
    void advance(uint32_t tick, uint32_t static_revision, const FlowSampler& sampler) noexcept;

    /// Looks up a circle-goal route, admitting repeated destinations when needed.
    ///
    /// Pending suppresses a full search without consuming its retry timestamp.
    /// Extraction shares a separate per-tick cell-probe budget. Exhaustion
    /// returns pending so large cohorts cannot burst unbounded validation work.
    /// Fine regional fields charge bounded goal-branch nodes to the same budget.
    /// Ready writes at least two points; a route may be a prefix when the output
    /// fills. Every returned edge is checked against current static knowledge.
    /// With landmass slots, a request waits while its movement class and
    /// player are first labelled, or while the nearest-node map its goal
    /// needs is built.
    ///
    /// @param context movement class, player, revisions and footprint
    /// @param unit canonical unit supplying the start cell and position
    /// @param goal circle goal; other shapes fall back
    /// @param[out] points navigator's preallocated route storage
    /// @param[out] count points written, zero unless ready
    /// @param sampler current static passability
    /// @param[out] destination when not null and the route is ready, the
    ///     world point the route leads to: the goal's, or the point of the
    ///     unit's landmass that replaced it
    /// @return ready, pending, or fallback to the ordinary search
    [[nodiscard]] FlowResult route(
        const FlowContext& context,
        const oa::Unit& unit,
        const Goal& goal,
        std::span<RoutePoint> points,
        uint32_t& count,
        const FlowSampler& sampler,
        RoutePoint* destination = nullptr
    ) noexcept;

    /// Returns cumulative bounded-work counters.
    [[nodiscard]] FlowCounters counters() const noexcept;
    /// Returns the static revision restored with the cache.
    [[nodiscard]] uint32_t static_revision() const noexcept;
    /// Tells whether crowds shape this cache's fields.
    ///
    /// @return true with a crowd weight, fields and fewer than 2^24 coarse nodes
    [[nodiscard]] bool crowd_shaped() const noexcept;
    /// Returns canonical little-endian state; no encoding or allocation is needed.
    [[nodiscard]] std::span<const uint8_t> state_bytes() const noexcept;
    /// Restores a validated state of the same map, bounds and format.
    ///
    /// @param bytes saved state, including pending frontiers and admission history
    /// @return false without changes when the bytes are malformed or incompatible
    [[nodiscard]] bool restore(std::span<const uint8_t> bytes) noexcept;

  private:

    /// What a request learned of its unit's landmass and its goal.
    struct LandmassPlacement {
        FlowResult result{FlowResult::ready}; ///< pending to wait, otherwise go on
        uint32_t landmass{};                  ///< the unit's landmass, or none
        bool goal_anchored{};                 ///< the goal connects to a coarse node
    };

    /// Returns where a label slot starts in the state.
    ///
    /// @param index slot index, below landmass_slots_
    /// @return the offset of the slot's metadata; its labels follow
    [[nodiscard]] std::size_t label_slot_offset(uint32_t index) const noexcept;
    /// Returns where a nearest-node slot starts in the state.
    ///
    /// @param index slot index, below nearest_slots_
    /// @return the offset of the slot's metadata; its nodes follow
    [[nodiscard]] std::size_t nearest_slot_offset(uint32_t index) const noexcept;
    /// Returns where the node labels a job is building start in the state.
    ///
    /// @return the offset of the build labels; each footprint cell's state,
    ///     one byte a cell, follows, then the flood queue
    [[nodiscard]] std::size_t build_labels_offset() const noexcept;
    /// Returns the footprint cells of the map, the cells a landmass is
    /// labelled at.
    ///
    /// @return width times height
    [[nodiscard]] uint64_t footprint_cells() const noexcept;
    /// Tells whether the running landmass job reads or writes a label slot.
    ///
    /// @param index label slot index
    /// @return true when the job labels it or builds a map from its labels
    [[nodiscard]] bool job_holds_label_slot(uint32_t index) const noexcept;
    /// Checks the landmass section of saved bytes of this cache's shape.
    ///
    /// @param bytes saved state of this cache's size
    /// @return whether every slot, label, node and the running job are in range
    [[nodiscard]] bool valid_landmasses(std::span<const uint8_t> bytes) noexcept;

    /// Runs the queued landmass work within the tick's landmass budget.
    ///
    /// @param tick deterministic simulation tick
    /// @param revision current static revision
    /// @param sampler current passability and player knowledge
    void advance_landmasses(uint32_t tick, uint32_t revision, const FlowSampler& sampler) noexcept;

    /// Starts the next landmass job: a first labelling, then a nearest-node
    /// map, then a refresh of the labels used longest ago.
    ///
    /// @param tick deterministic simulation tick
    /// @param revision current static revision
    /// @return whether a job was started
    bool start_landmass_job(uint32_t tick, uint32_t revision) noexcept;

    /// Labels landmasses for the running job, spending at most `budget`.
    ///
    /// @param tick deterministic simulation tick
    /// @param sampler current passability and player knowledge
    /// @param[in,out] budget landmass work left this tick
    void label_landmasses(uint32_t tick, const FlowSampler& sampler, uint32_t& budget) noexcept;

    /// Builds the running job's nearest-node map, spending at most `budget`.
    ///
    /// @param[in,out] budget landmass work left this tick
    void spread_nearest_nodes(uint32_t& budget) noexcept;

    /// Finds or asks for a movement class and player's labels.
    ///
    /// @param movement_class movement-class table index
    /// @param player player slot
    /// @param tick deterministic simulation tick
    /// @param[out] waiting true when the labels are being made for the first time
    /// @return the slot, or no slot when none is free
    uint32_t
    find_labels(uint32_t movement_class, uint32_t player, uint32_t tick, bool& waiting) noexcept;

    /// Finds or asks for the nearest-node map of a labelled landmass.
    ///
    /// @param labels label slot index
    /// @param landmass the landmass, its first node in scan order
    /// @param tick deterministic simulation tick
    /// @param[out] waiting true when the map is being made
    /// @return the slot, or no slot when none is free
    uint32_t
    find_nearest(uint32_t labels, uint32_t landmass, uint32_t tick, bool& waiting) noexcept;

    /// Finds the unit's landmass and, when it has at least 256 nodes and the
    /// goal is blocked, off the map or on another landmass, moves the goal's
    /// cell to the nearest node of the unit's own landmass.
    ///
    /// @param context movement class, player and footprint
    /// @param unit canonical unit supplying the start cell
    /// @param[in,out] goal the goal routed to
    /// @param sampler the request's probe-bounded passability
    /// @param exhausted whether the request's probe budget has run out
    /// @return pending to wait, the landmass and whether the goal is anchored
    LandmassPlacement place_goal(
        const FlowContext& context,
        const oa::Unit& unit,
        Goal& goal,
        const FlowSampler& sampler,
        const bool& exhausted
    ) noexcept;

    /// Returns how many levels a crowd puts off a node a field discovers.
    ///
    /// @param sampler the tick's ground unit counts
    /// @param player the field's player slot
    /// @param column the node's column
    /// @param row the node's row
    /// @return (units * crowd weight + 4) / 9, at most 255, where units are
    ///     the player's ground units in the 3x3 nodes around the node
    [[nodiscard]] uint32_t crowd_delay(
        const FlowSampler& sampler, uint32_t player, int32_t column, int32_t row
    ) const noexcept;
    /// Returns where a field's crowd mark lies in the state.
    ///
    /// @param index field index, below fields_
    /// @return the offset of the mark: 1 when the field's build met a crowd
    [[nodiscard]] std::size_t crowd_mark_offset(uint32_t index) const noexcept;
    /// Checks a building or completed field's queue and tree when crowds put
    /// nodes off: the queue holds each node it has not expanded once, and
    /// every discovered node leads to the root through expanded ones.
    ///
    /// @param f the field's metadata in saved bytes
    /// @param discovered the field's discovered nodes
    /// @return whether the queue and tree are ones a build can leave
    [[nodiscard]] bool valid_crowd_frontier(const uint8_t* f, uint32_t discovered) noexcept;

    std::unique_ptr<uint8_t[]> state_{}, restore_marks_{};
    std::size_t state_size_{}, landmass_offset_{}, crowd_offset_{};
    uint32_t width_{}, height_{}, stride_{}, columns_{}, rows_{}, cells_{}, fields_{},
        work_per_tick_{};
    uint32_t route_probes_per_tick_{};
    uint32_t landmass_slots_{}, nearest_slots_{}, landmass_work_per_tick_{};
    uint32_t crowd_weight_{}, crowd_rebuild_ticks_{};
};

} // namespace oa::sim::ground_orders

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/ground_orders/shared_routes.hpp"
#include "footprint_segment.hpp"
#include "oa/sim/spatial_state/cell_classifier.hpp"
#include "oa/sim/spatial_state/movement_footprint.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace oa::sim::ground_orders {
namespace {
/// The saved state's magic and format: format 4 is the fine regional grid with
/// landmass keys and labels, and crowd marks when crowds shape the fields;
/// restore refuses any other.
constexpr uint32_t magic = 0x3246414f, fine_format = 4;
constexpr uint32_t fine_stride = 2;
constexpr uint32_t large_region = 96;
constexpr uint32_t fine_prefix_edges = 32;
constexpr size_t goal_branch_capacity = 512;
constexpr uint32_t fine_anchor_offset = fine_stride + fine_stride / 2;
constexpr uint32_t fine_anchor_probe_bound = 9 * (1 + 3 * fine_anchor_offset);
constexpr uint32_t fine_region_offset = large_region + fine_stride;
constexpr uint32_t fine_region_probe_bound = 1 + 3 * fine_region_offset;
constexpr uint32_t fine_edge_probe_bound = 1 + 3 * fine_stride;
constexpr uint32_t fine_corner_probe_bound =
    1 + 3 * (fine_anchor_offset + fine_prefix_edges * fine_stride + fine_region_offset);
constexpr uint32_t visible_chord_probe_limit = 128;
constexpr std::size_t header_size = 128, admission_size = 48, admission_count = 64;
constexpr std::size_t metadata_size = 84, key_size = 40;
// A key: movement class, player, static revision, knowledge epoch (8 bytes),
// footprint, the goal's region and the landmass the field serves.
constexpr size_t key_revision_offset = 8, key_region_offset = 28, key_landmass_offset = 36;
// An admission entry: its key, the tick it was first asked for and its
// status (0 unused, 1 asked once, 2 admitted).
constexpr size_t admission_tick_offset = 40, admission_status_offset = 44;
// A field's metadata: its phase, its key, the cell of the goal that seeded it,
// its root node, how many direction bytes are cleared, the queue's head and
// tail, the tick it was last asked for, whether the map changed under it and
// since when, and the tick it completed or failed.
constexpr size_t field_key_offset = 4, seed_goal_offset = 44, seed_node_offset = 52;
constexpr size_t cleared_offset = 56, head_offset = 60, tail_offset = 64, last_use_offset = 68;
constexpr size_t dirty_offset = 72, dirty_since_offset = 76, completed_tick_offset = 80;
constexpr uint32_t empty = 0, clearing = 1, building = 2, complete = 3, failed = 4;
// A field's queue holds `cells` entries in a ring: head and tail count every
// entry taken and put, and a node is in it at most once at a time. With
// crowds, an entry's low 24 bits are its node and its high 8 bits the levels
// the node still waits; without, the entry is the node.
constexpr uint32_t queue_node_bits = 24, queue_node_mask = (1u << queue_node_bits) - 1;
/// The most levels a crowd puts a node off, and the divisor of its units
/// times the crowd weight: the weight is in ninths of a level a unit.
constexpr uint32_t max_crowd_delay = 255, crowd_weight_scale = 9;
// The crowd section follows the landmass section: the crowd weight and
// rebuild ticks it was built with, then each field's mark, 1 when its
// build met a crowd.
constexpr size_t crowd_header_size = 8, crowd_rebuild_offset = 4, crowd_mark_size = 4;
constexpr uint32_t refresh_period = 30, completed_refresh_grace = 60;
constexpr uint32_t completed_admission_grace = 60;
/// Ticks a field whose root could not be connected stays given up.
constexpr uint32_t failed_field_retry_ticks = 300;
constexpr uint8_t root = 0xfd, unseen = 0xff;

// The landmass section follows the fields. Its header holds the slot counts
// and the work budget it was built with, then the running job: its kind, the
// slot it works on, its step, its cursor, the queue's head and tail, the
// landmass being flooded, the static revision the job started at and the
// coarse nodes the flood has reached; then the cells and nodes labelled and
// mapped so far. The label slots and nearest-node slots follow, then the
// node labels being built, each footprint cell's state while labelling, and
// the flood's queue of footprint cells.
constexpr size_t landmass_header_size = 64;
constexpr size_t landmass_label_slots_offset = 0, landmass_nearest_slots_offset = 4;
constexpr size_t landmass_work_offset = 8, job_kind_offset = 12, job_slot_offset = 16;
constexpr size_t job_step_offset = 20, job_cursor_offset = 24, job_head_offset = 28;
constexpr size_t job_tail_offset = 32, job_flood_offset = 36, job_revision_offset = 40;
constexpr size_t job_flood_nodes_offset = 44, labelled_cells_offset = 48;
constexpr size_t nearest_nodes_offset = 56;
/// A footprint cell's state while labelling: not yet looked at, blocked, or
/// reached by a flood.
constexpr uint8_t cell_unvisited = 0, cell_blocked = 1, cell_reached = 2;
constexpr uint32_t no_job = 0, labels_job = 1, nearest_job = 2;
// A label job first marks every footprint cell and node label unvisited, then
// scans and floods; a nearest job makes a forward pass, then a backward one.
constexpr uint32_t clearing_step = 0, scanning_step = 1, forward_step = 0, backward_step = 1;
/// The chamfer lengths of a straight and a diagonal step between nodes, and a
/// node no landmass node has reached.
constexpr uint32_t straight_distance = 5, diagonal_distance = 7;
constexpr uint32_t unreached_distance = 0xffffffff;
/// Nodes of a nearest-node map made for one unit of landmass work.
constexpr uint32_t nearest_nodes_per_work = 8;
// A label slot: its phase, movement class, player, the static revision and
// tick its labels were made at, the tick it was last asked for, how many
// times its labels changed, and the tick it was first asked for.
constexpr size_t label_slot_size = 32;
constexpr size_t slot_phase_offset = 0, slot_class_offset = 4, slot_player_offset = 8;
constexpr size_t slot_revision_offset = 12, slot_built_offset = 16, slot_used_offset = 20;
constexpr size_t slot_generation_offset = 24, slot_asked_offset = 28;
constexpr uint32_t slot_empty = 0, slot_asked = 1, slot_labelled = 2;
// A nearest-node slot: its phase, label slot, that slot's generation, the
// landmass, the tick it was last asked for and the tick it was first asked
// for.
constexpr size_t nearest_slot_size = 24;
constexpr size_t nearest_labels_offset = 4, nearest_generation_offset = 8;
constexpr size_t nearest_landmass_offset = 12, nearest_used_offset = 16, nearest_asked_offset = 20;
constexpr uint32_t nearest_empty = 0, nearest_asked = 1, nearest_building = 2, nearest_ready = 3;
/// Node labels that name no landmass: not yet visited, a blocked node, and a
/// node of a landmass below the floor. Every other label is a landmass's
/// first footprint cell in scan order.
constexpr uint32_t label_unvisited = 0xffffffff, label_blocked = 0xfffffffe;
constexpr uint32_t label_small = 0xfffffffd;
/// The landmass of a field key, or of a request, that names none.
constexpr uint32_t no_landmass = 0xffffffff, no_slot = 0xffffffff, no_node = 0xffffffff;
/// Coarse nodes a landmass needs to be named: a unit on a smaller one keeps
/// its goals.
constexpr uint32_t landmass_node_floor = 256;
/// Each nearest-node slot of a label slot.
constexpr uint32_t nearest_slots_per_label_slot = 2;
/// Ticks after labelling before a slot may be labelled again, and the age at
/// which it is labelled again though the map did not change, for ground the
/// player has seen since.
constexpr uint32_t relabel_grace = 300, relabel_period = 900;
/// Ticks a slot stays its owner's after it was last asked for.
constexpr uint32_t slot_keep_ticks = 300;
/// Ticks an owner keeps a slot it has labels or a map in before another that
/// finds every slot in use may take it: the period after which its labels are
/// made again anyway. The slot held longest goes first, and only once no slot
/// of its kind has changed owner for slot_keep_ticks, so with more movement
/// classes and players asking than there are slots each takes its turn, at
/// one labelling a turn.
constexpr uint32_t slot_turn_ticks = relabel_period;
/// Footprint cells made unvisited, or taken off a small landmass's flood
/// queue, for one unit of landmass work.
constexpr uint32_t cleared_entries_per_work = 16;
/// The four steps of a flood between footprint cells: a diagonal step a
/// footprint can take passes through one of them.
constexpr std::array<int32_t, 4> flood_dx{1, 0, -1, 0}, flood_dz{0, 1, 0, -1};
constexpr uint32_t clear_bytes_per_tick = 65'536, clear_slice = 4'096;
constexpr std::array<int32_t, 8> dx{1, 1, 0, -1, -1, -1, 0, 1};
constexpr std::array<int32_t, 8> dz{0, 1, 1, 1, 0, -1, -1, -1};
// Equal-depth parents must favour straight cardinal progress. Angular order
// picks diagonal parents first on some banks and creates large V detours.
constexpr std::array<uint8_t, 8> expansion_order{0, 2, 4, 6, 1, 3, 5, 7};
using Key = std::array<uint8_t, key_size>;

uint32_t get32(const uint8_t* p) noexcept {
    return uint32_t{p[0]} | uint32_t{p[1]} << 8 | uint32_t{p[2]} << 16 | uint32_t{p[3]} << 24;
}

uint64_t get64(const uint8_t* p) noexcept {
    return get32(p) | uint64_t{get32(p + 4)} << 32;
}

void put32(uint8_t* p, uint32_t value) noexcept {
    for (uint32_t i = 0; i < 4; ++i)
        p[i] = static_cast<uint8_t>(value >> (i * 8));
}

void put64(uint8_t* p, uint64_t value) noexcept {
    put32(p, static_cast<uint32_t>(value));
    put32(p + 4, static_cast<uint32_t>(value >> 32));
}

void increment(uint8_t* state, uint32_t counter, uint64_t amount = 1) noexcept {
    auto* p = state + 64 + counter * 8;
    put64(p, get64(p) + amount);
}

/// Restarts one field through the existing bounded clearing and expansion work.
void restart_field(uint8_t* state, uint8_t* selected) noexcept {
    put32(selected, clearing);
    // Keep the movement key, seed destination and last-use tick.
    std::memset(selected + seed_node_offset, 0, last_use_offset - seed_node_offset);
    std::memset(selected + dirty_offset, 0, metadata_size - dirty_offset);
    increment(state, 2);
    increment(state, 6);
}

uint8_t* field(uint8_t* state, uint32_t cells, uint32_t index) noexcept {
    return state + header_size + admission_count * admission_size +
           static_cast<std::size_t>(index) * (metadata_size + static_cast<std::size_t>(cells) * 5);
}

const uint8_t* field(const uint8_t* state, uint32_t cells, uint32_t index) noexcept {
    return field(const_cast<uint8_t*>(state), cells, index);
}

Key make_key(
    const FlowContext& context, const Goal& goal, uint32_t region, uint32_t landmass
) noexcept {
    Key key{};
    put32(key.data(), context.movement_class);
    put32(key.data() + 4, context.player);
    put32(key.data() + key_revision_offset, context.static_revision);
    put64(key.data() + 12, context.knowledge_epoch);
    put32(key.data() + 20, context.footprint_x);
    put32(key.data() + 24, context.footprint_z);
    put32(key.data() + key_region_offset, static_cast<uint32_t>(goal.cell[0]) / region);
    put32(key.data() + key_region_offset + 4, static_cast<uint32_t>(goal.cell[1]) / region);
    put32(key.data() + key_landmass_offset, landmass);
    return key;
}

bool same_key(const uint8_t* a, const Key& b) noexcept {
    return std::memcmp(a, b.data(), key_size) == 0;
}

/// Checks a key's movement class, player and footprint, and that its landmass
/// is none or one of the first `landmasses` footprint cells.
bool valid_key(const uint8_t* key, uint64_t landmasses) noexcept {
    const auto landmass = get32(key + key_landmass_offset);
    return get32(key) < 32 && get32(key + 4) < 10 && get32(key + 20) > 0 && get32(key + 20) <= 64 &&
           get32(key + 24) > 0 && get32(key + 24) <= 64 &&
           (landmass == no_landmass || landmass < landmasses);
}

// Supercover checks forbid diagonal corner cutting. The sampler accounts for
// the entire movement-class footprint, not just the coarse node's centre.
bool line_clear(
    const FlowSampler& sampler, const uint8_t* key, int32_t x, int32_t z, int32_t to_x, int32_t to_z
) noexcept {
    const auto sample = [&](int32_t cx, int32_t cz) {
        return sampler.cell &&
               sampler.cell(sampler.context, get32(key), get32(key + 4), cx, cz) != 0;
    };
    if (!sample(x, z))
        return false;
    const int32_t width = std::abs(to_x - x), height = std::abs(to_z - z);
    const int32_t step_x = x < to_x ? 1 : -1, step_z = z < to_z ? 1 : -1;
    int32_t error = width - height;
    while (x != to_x || z != to_z) {
        const auto twice = error * 2;
        const bool move_x = twice > -height, move_z = twice < width;
        if (move_x && move_z && (!sample(x + step_x, z) || !sample(x, z + step_z)))
            return false;
        if (move_x) {
            error -= height;
            x += step_x;
        }
        if (move_z) {
            error += width;
            z += step_z;
        }
        if (!sample(x, z))
            return false;
    }
    return true;
}

/// Checks the actual fixed-point chord, retaining strict known-only visibility.
bool known_segment_clear(
    const FlowSampler& sampler,
    const uint8_t* key,
    const oa::Unit& unit,
    const FlowContext& context,
    RoutePoint target,
    uint32_t maximum
) noexcept {
    footprint_segment_detail::Segment segment{};
    if (!sampler.cell || !footprint_segment_detail::make_segment(unit, context, target, segment))
        return false;
    uint32_t count{};
    return footprint_segment_detail::visit_origins(
        segment,
        maximum,
        [&](int32_t x, int32_t z) {
            return sampler.cell(sampler.context, get32(key), get32(key + 4), x, z) == 1;
        },
        count
    );
}

// The nearest anchor first, then its eight neighbours. A narrow off-grid
// corridor can have no connection; that request must use the full search.
int32_t connect_anchor(
    uint32_t columns,
    uint32_t rows,
    uint32_t stride,
    int32_t x,
    int32_t z,
    const uint8_t* key,
    const FlowSampler& sampler,
    const uint8_t* directions = nullptr
) noexcept {
    const auto half_stride = static_cast<int32_t>(stride / 2);
    const int32_t centre_x = (x + half_stride) / static_cast<int32_t>(stride);
    const int32_t centre_z = (z + half_stride) / static_cast<int32_t>(stride);
    for (uint32_t i = 0; i < 9; ++i) {
        const int32_t cx = centre_x + (i == 0 ? 0 : dx[i - 1]);
        const int32_t cz = centre_z + (i == 0 ? 0 : dz[i - 1]);
        if (cx < 0 || cz < 0 || static_cast<uint32_t>(cx) >= columns ||
            static_cast<uint32_t>(cz) >= rows)
            continue;
        const auto index = static_cast<uint32_t>(cz) * columns + static_cast<uint32_t>(cx);
        if (directions && directions[index] == unseen)
            continue;
        if (line_clear(
                sampler,
                key,
                x,
                z,
                cx * static_cast<int32_t>(stride),
                cz * static_cast<int32_t>(stride)
            ))
            return static_cast<int32_t>(index);
    }
    return -1;
}

RoutePoint point_at(int32_t x, int32_t z, const FlowContext& context) noexcept {
    return {
        static_cast<int16_t>((x * 2 + static_cast<int32_t>(context.footprint_x)) * 8),
        static_cast<int16_t>((z * 2 + static_cast<int32_t>(context.footprint_z)) * 8)
    };
}
} // namespace

bool static_flow_passable(
    const sim::spatial_state::CellClassifier& movement,
    int32_t x,
    int32_t z,
    const sim::spatial_state::World& world
) noexcept {
    auto classifier = movement;
    classifier.occupancy_before_tick = 0;
    return sim::spatial_state::classify_movement_footprint(classifier, x, z, world) != 0;
}

FlowCache::FlowCache(uint32_t width, uint32_t height, FlowLimits limits)
    : width_(width), height_(height), stride_(fine_stride),
      columns_(width / stride_ + (width % stride_ != 0)),
      rows_(height / stride_ + (height % stride_ != 0)), work_per_tick_(limits.work_per_tick),
      route_probes_per_tick_(limits.route_probes_per_tick) {
    const auto count = uint64_t{columns_} * rows_;
    if (count > 0 && count <= limits.total_cells && count <= std::numeric_limits<uint32_t>::max()) {
        cells_ = static_cast<uint32_t>(count);
        fields_ = std::min(limits.fields, limits.total_cells / cells_);
    }
    if (fields_ != 0 && limits.landmass_slots != 0 && limits.landmass_work_per_tick != 0 &&
        uint64_t{width} * height <= std::numeric_limits<uint32_t>::max() / 4) {
        landmass_slots_ = limits.landmass_slots;
        nearest_slots_ = limits.landmass_slots * nearest_slots_per_label_slot;
        landmass_work_per_tick_ = limits.landmass_work_per_tick;
    }
    landmass_offset_ =
        header_size + admission_count * admission_size +
        static_cast<std::size_t>(fields_) * (metadata_size + static_cast<std::size_t>(cells_) * 5);
    state_size_ = landmass_offset_;
    if (landmass_slots_ != 0) {
        const auto node_bytes = static_cast<std::size_t>(cells_) * 4;
        const auto cells = static_cast<std::size_t>(footprint_cells());
        // The label slots, the nearest-node slots, the node labels being
        // built, each footprint cell's state and the flood queue.
        state_size_ += landmass_header_size + landmass_slots_ * (label_slot_size + node_bytes) +
                       nearest_slots_ * (nearest_slot_size + node_bytes) + node_bytes + cells * 5;
    }
    // A queue entry holds a node in 24 bits beside its delay.
    if (fields_ != 0 && limits.crowd_weight != 0 && cells_ <= queue_node_mask) {
        crowd_weight_ = limits.crowd_weight;
        crowd_rebuild_ticks_ = limits.crowd_rebuild_ticks;
    }
    crowd_offset_ = state_size_;
    if (crowd_weight_ != 0)
        state_size_ += crowd_header_size + static_cast<std::size_t>(fields_) * crowd_mark_size;
    state_ = std::make_unique<uint8_t[]>(state_size_);
    // A mark a coarse node while restoring fields, or a bit a footprint cell
    // (at most four a node) while restoring the landmass flood.
    restore_marks_ = std::make_unique<uint8_t[]>(cells_);
    if (landmass_slots_ != 0) {
        auto* section = state_.get() + landmass_offset_;
        put32(section + landmass_label_slots_offset, landmass_slots_);
        put32(section + landmass_nearest_slots_offset, nearest_slots_);
        put32(section + landmass_work_offset, landmass_work_per_tick_);
    }
    if (crowd_weight_ != 0) {
        put32(state_.get() + crowd_offset_, crowd_weight_);
        put32(state_.get() + crowd_offset_ + crowd_rebuild_offset, crowd_rebuild_ticks_);
    }
    put32(state_.get(), magic);
    put32(state_.get() + 4, fine_format);
    put32(state_.get() + 8, width_);
    put32(state_.get() + 12, height_);
    put32(state_.get() + 16, fields_);
    put32(state_.get() + 20, cells_);
    put32(state_.get() + 24, work_per_tick_);
    put32(state_.get() + 28, stride_);
    put32(state_.get() + 56, route_probes_per_tick_);
    put32(state_.get() + 60, route_probes_per_tick_);
}

void FlowCache::advance(uint32_t tick, uint32_t revision, const FlowSampler& sampler) noexcept {
    auto* state = state_.get();
    const auto stride = stride_;
    const auto previous_tick = get32(state + 32);
    put32(state + 32, tick);
    if (get32(state + 36) != revision) {
        for (uint32_t i = 0; i < fields_; ++i) {
            auto* f = field(state, cells_, i);
            if (get32(f) == empty)
                continue;
            if (get32(f) == failed) {
                increment(state, 6);
                put32(f, empty);
                continue;
            }
            // Returned connections are checked against the current map. An
            // unrelated wreck must not discard an entire useful tree/frontier.
            put32(f + field_key_offset + key_revision_offset, revision);
            if (get32(f + dirty_offset) == 0) {
                put32(f + dirty_offset, 1);
                // The map changed since this cache's last advance. Using
                // that saved boundary also keeps a same-tick load pass and
                // the original match's next regular pass equivalent.
                put32(f + dirty_since_offset, previous_tick);
            }
        }
        for (uint32_t i = 0; i < admission_count; ++i) {
            auto* entry = state + header_size + i * admission_size;
            if (get32(entry + admission_status_offset) != 0)
                put32(entry + key_revision_offset, revision);
        }
        put32(state + 36, revision);
    }
    // A load pass and the regular tick can both call the scheduler. The
    // configured work limit applies once to a simulation tick.
    if (get32(state + 48) != 0 && get32(state + 52) == tick)
        return;
    put32(state + 48, 1);
    put32(state + 52, tick);
    put32(state + 60, route_probes_per_tick_);
    if (fields_ == 0)
        return;
    // A removed obstacle can make a retained route unnecessarily long or open
    // a previously unreachable corridor. Refresh at most one completed dirty
    // tree per period, oldest first. Never interrupt a building frontier, and
    // let every newly completed tree serve routes before it can be refreshed.
    if (tick % refresh_period == 0) {
        uint8_t* oldest = nullptr;
        uint32_t oldest_age = 0;
        for (uint32_t i = 0; i < fields_; ++i) {
            auto* f = field(state, cells_, i);
            // A field whose root could not be connected is given up for a
            // while, then its storage and key are free again.
            if (get32(f) == failed &&
                tick - get32(f + completed_tick_offset) >= failed_field_retry_ticks) {
                put32(f, empty);
                continue;
            }
            if (get32(f) != complete || get32(f + dirty_offset) == 0 ||
                tick - get32(f + completed_tick_offset) < completed_refresh_grace)
                continue;
            const auto age = tick - get32(f + dirty_since_offset);
            if (!oldest || age > oldest_age) {
                oldest = f;
                oldest_age = age;
            }
        }
        // Crowds move on: a field built round them that units asked for
        // since it completed is built again from the crowds of now, the
        // longest completed first, when no dirty field is due. A field nobody
        // asked for keeps its tree and leaves the building to others.
        if (!oldest && crowd_weight_ != 0 && crowd_rebuild_ticks_ != 0)
            for (uint32_t i = 0; i < fields_; ++i) {
                auto* f = field(state, cells_, i);
                const auto age = tick - get32(f + completed_tick_offset);
                if (get32(f) != complete || get32(state + crowd_mark_offset(i)) == 0 ||
                    age < crowd_rebuild_ticks_ || tick - get32(f + last_use_offset) > age)
                    continue;
                if (!oldest || age > oldest_age) {
                    oldest = f;
                    oldest_age = age;
                }
            }
        if (oldest)
            restart_field(state, oldest);
    }
    uint32_t cursor = get32(state + 40) % fields_;
    uint32_t idle = 0, work = 0, clear_budget = clear_bytes_per_tick;
    // Clearing bytes is much cheaper than classifying graph edges. Share a
    // separate byte budget in contiguous slices, rather than charging one
    // search expansion for each byte and delaying large cohorts for seconds.
    while (clear_budget != 0 && work < work_per_tick_ && idle < fields_) {
        const auto field_index = cursor;
        auto* f = field(state, cells_, cursor);
        cursor = (cursor + 1) % fields_;
        if (get32(f) != clearing) {
            ++idle;
            continue;
        }
        idle = 0;
        auto* directions = f + metadata_size;
        const auto clear = get32(f + cleared_offset);
        const auto amount = std::min({cells_ - clear, clear_slice, clear_budget});
        std::memset(directions + clear, unseen, amount);
        put32(f + cleared_offset, clear + amount);
        clear_budget -= amount;
        increment(state, 3, amount);
        if (clear + amount != cells_)
            continue;
        // Seeding performs line tests, so it consumes an expansion credit.
        ++work;
        const auto seed = connect_anchor(
            columns_,
            rows_,
            stride,
            static_cast<int32_t>(get32(f + seed_goal_offset)),
            static_cast<int32_t>(get32(f + seed_goal_offset + 4)),
            f + field_key_offset,
            sampler
        );
        if (seed < 0) {
            put32(f, failed);
            put32(f + completed_tick_offset, tick);
            continue;
        }
        put32(f + seed_node_offset, static_cast<uint32_t>(seed));
        directions[seed] = root;
        auto* queue = directions + cells_;
        put32(queue, static_cast<uint32_t>(seed));
        put32(f + head_offset, 0);
        put32(f + tail_offset, 1);
        put32(f, building);
        // The mark tells of this build only; the root waits for no crowd.
        if (crowd_weight_ != 0)
            put32(state + crowd_mark_offset(field_index), 0);
    }
    idle = 0;
    for (; work < work_per_tick_ && idle < fields_;) {
        const auto field_index = cursor;
        auto* f = field(state, cells_, cursor);
        cursor = (cursor + 1) % fields_;
        if (get32(f) != building) {
            ++idle;
            continue;
        }
        idle = 0;
        ++work;
        auto* directions = f + metadata_size;
        auto* queue = directions + cells_;
        auto head = get32(f + head_offset), tail = get32(f + tail_offset);
        if (head == tail) {
            put32(f, complete);
            put32(f + completed_tick_offset, tick);
            continue;
        }
        const auto entry = get32(queue + static_cast<std::size_t>(head++ % cells_) * 4);
        put32(f + head_offset, head);
        const auto node = crowd_weight_ != 0 ? entry & queue_node_mask : entry;
        // A node a crowd puts off goes to the back of the queue, a level
        // less to wait, at the cost of one credit.
        if (const auto wait = crowd_weight_ != 0 ? entry >> queue_node_bits : 0; wait != 0) {
            put32(
                queue + static_cast<std::size_t>(tail++ % cells_) * 4,
                (wait - 1) << queue_node_bits | node
            );
            put32(f + tail_offset, tail);
            increment(state, 4);
            continue;
        }
        const auto player = get32(f + field_key_offset + 4);
        const int32_t x = static_cast<int32_t>(node % columns_);
        const int32_t z = static_cast<int32_t>(node / columns_);
        for (const auto d : expansion_order) {
            const int32_t nx = x + dx[d], nz = z + dz[d];
            if (nx < 0 || nz < 0 || static_cast<uint32_t>(nx) >= columns_ ||
                static_cast<uint32_t>(nz) >= rows_)
                continue;
            const auto neighbour = static_cast<uint32_t>(nz) * columns_ + static_cast<uint32_t>(nx);
            if (directions[neighbour] != unseen || !line_clear(
                                                       sampler,
                                                       f + field_key_offset,
                                                       x * static_cast<int32_t>(stride),
                                                       z * static_cast<int32_t>(stride),
                                                       nx * static_cast<int32_t>(stride),
                                                       nz * static_cast<int32_t>(stride)
                                                   ))
                continue;
            // A valid field discovers each cell once, so its queue never
            // fills. Keep malformed restored state bounded even if that
            // invariant is violated.
            if (tail - head == cells_) {
                put32(f, failed);
                put32(f + completed_tick_offset, tick);
                break;
            }
            directions[neighbour] = static_cast<uint8_t>((d + 4) % 8);
            const auto delay = crowd_weight_ != 0 ? crowd_delay(sampler, player, nx, nz) : 0;
            if (delay != 0)
                put32(state + crowd_mark_offset(field_index), 1);
            put32(
                queue + static_cast<std::size_t>(tail++ % cells_) * 4,
                delay << queue_node_bits | neighbour
            );
        }
        put32(f + tail_offset, tail);
        increment(state, 4);
        if (head == tail && get32(f) == building) {
            put32(f, complete);
            put32(f + completed_tick_offset, tick);
        }
    }
    put32(state + 40, cursor);
    if (landmass_slots_ != 0)
        advance_landmasses(tick, revision, sampler);
}

std::size_t FlowCache::label_slot_offset(uint32_t index) const noexcept {
    return landmass_offset_ + landmass_header_size +
           static_cast<std::size_t>(index) *
               (label_slot_size + static_cast<std::size_t>(cells_) * 4);
}

std::size_t FlowCache::nearest_slot_offset(uint32_t index) const noexcept {
    return label_slot_offset(landmass_slots_) +
           static_cast<std::size_t>(index) *
               (nearest_slot_size + static_cast<std::size_t>(cells_) * 4);
}

std::size_t FlowCache::build_labels_offset() const noexcept {
    return nearest_slot_offset(nearest_slots_);
}

uint64_t FlowCache::footprint_cells() const noexcept {
    return uint64_t{width_} * height_;
}

uint32_t FlowCache::crowd_delay(
    const FlowSampler& sampler, uint32_t player, int32_t column, int32_t row
) const noexcept {
    if (!sampler.ground_units)
        return 0;
    uint64_t units = 0;
    for (int32_t z = row - 1; z <= row + 1; ++z)
        for (int32_t x = column - 1; x <= column + 1; ++x)
            if (x >= 0 && z >= 0 && static_cast<uint32_t>(x) < columns_ &&
                static_cast<uint32_t>(z) < rows_)
                units += sampler.ground_units(sampler.context, player, x, z);
    // With a weight of one, this many units already wait the most levels.
    units = std::min<uint64_t>(units, uint64_t{max_crowd_delay} * crowd_weight_scale);
    return static_cast<uint32_t>(std::min<uint64_t>(
        (units * crowd_weight_ + crowd_weight_scale / 2) / crowd_weight_scale, max_crowd_delay
    ));
}

std::size_t FlowCache::crowd_mark_offset(uint32_t index) const noexcept {
    return crowd_offset_ + crowd_header_size + static_cast<std::size_t>(index) * crowd_mark_size;
}

bool FlowCache::job_holds_label_slot(uint32_t index) const noexcept {
    const auto* state = state_.get();
    const auto* section = state + landmass_offset_;
    const auto kind = get32(section + job_kind_offset), slot = get32(section + job_slot_offset);
    if (kind == labels_job)
        return slot == index;
    return kind == nearest_job &&
           get32(state + nearest_slot_offset(slot) + nearest_labels_offset) == index;
}

uint32_t FlowCache::find_labels(
    uint32_t movement_class, uint32_t player, uint32_t tick, bool& waiting
) noexcept {
    auto* state = state_.get();
    waiting = false;
    uint32_t free = no_slot, oldest = no_slot, oldest_age = 0;
    uint32_t longest = no_slot, longest_held = 0, newest_held = slot_keep_ticks;
    for (uint32_t i = 0; i < landmass_slots_; ++i) {
        auto* slot = state + label_slot_offset(i);
        const auto phase = get32(slot + slot_phase_offset);
        if (phase == slot_empty) {
            if (free == no_slot)
                free = i;
            continue;
        }
        if (get32(slot + slot_class_offset) == movement_class &&
            get32(slot + slot_player_offset) == player) {
            put32(slot + slot_used_offset, tick);
            waiting = phase == slot_asked;
            return i;
        }
        // A slot nobody asked for lately may serve another class or player;
        // the running job keeps its slot.
        const auto age = tick - get32(slot + slot_used_offset);
        const auto held = tick - get32(slot + slot_asked_offset);
        newest_held = std::min(newest_held, held);
        if (job_holds_label_slot(i))
            continue;
        if (age >= slot_keep_ticks && (oldest == no_slot || age > oldest_age)) {
            oldest = i;
            oldest_age = age;
        }
        if (phase == slot_labelled && held >= slot_turn_ticks &&
            (longest == no_slot || held > longest_held)) {
            longest = i;
            longest_held = held;
        }
    }
    // Every slot in use: the labelled one held longest takes its next owner
    // in turn.
    const auto turn = newest_held >= slot_keep_ticks ? longest : no_slot;
    const auto chosen = free != no_slot ? free : oldest != no_slot ? oldest : turn;
    if (chosen == no_slot)
        return no_slot;
    auto* slot = state + label_slot_offset(chosen);
    // A new owner's labels have a new generation, so no nearest-node map of
    // the previous owner matches them.
    const auto generation = get32(slot + slot_generation_offset) + 1;
    std::memset(slot, 0, label_slot_size);
    put32(slot + slot_phase_offset, slot_asked);
    put32(slot + slot_class_offset, movement_class);
    put32(slot + slot_player_offset, player);
    put32(slot + slot_used_offset, tick);
    put32(slot + slot_generation_offset, generation);
    put32(slot + slot_asked_offset, tick);
    waiting = true;
    return chosen;
}

uint32_t
FlowCache::find_nearest(uint32_t labels, uint32_t landmass, uint32_t tick, bool& waiting) noexcept {
    auto* state = state_.get();
    const auto* section = state + landmass_offset_;
    waiting = false;
    const auto generation = get32(state + label_slot_offset(labels) + slot_generation_offset);
    const auto running = get32(section + job_kind_offset) == nearest_job
                             ? get32(section + job_slot_offset)
                             : no_slot;
    uint32_t free = no_slot, oldest = no_slot, oldest_age = 0;
    uint32_t longest = no_slot, longest_held = 0, newest_held = slot_keep_ticks;
    for (uint32_t i = 0; i < nearest_slots_; ++i) {
        auto* slot = state + nearest_slot_offset(i);
        const auto phase = get32(slot + slot_phase_offset);
        const auto owner = get32(slot + nearest_labels_offset);
        // A map made from labels that have since changed, or given to another
        // owner, matches nothing.
        const bool current = phase != nearest_empty && owner < landmass_slots_ &&
                             get32(slot + nearest_generation_offset) ==
                                 get32(state + label_slot_offset(owner) + slot_generation_offset);
        if (current && owner == labels && get32(slot + nearest_generation_offset) == generation &&
            get32(slot + nearest_landmass_offset) == landmass) {
            put32(slot + nearest_used_offset, tick);
            waiting = phase != nearest_ready;
            return i;
        }
        if (current)
            newest_held = std::min(newest_held, tick - get32(slot + nearest_asked_offset));
        if (i == running)
            continue;
        if (!current) {
            if (free == no_slot)
                free = i;
            continue;
        }
        const auto age = tick - get32(slot + nearest_used_offset);
        if (age >= slot_keep_ticks && (oldest == no_slot || age > oldest_age)) {
            oldest = i;
            oldest_age = age;
        }
        const auto held = tick - get32(slot + nearest_asked_offset);
        if (phase == nearest_ready && held >= slot_turn_ticks &&
            (longest == no_slot || held > longest_held)) {
            longest = i;
            longest_held = held;
        }
    }
    // Every map in use: the finished one held longest takes its next landmass
    // in turn.
    const auto turn = newest_held >= slot_keep_ticks ? longest : no_slot;
    const auto chosen = free != no_slot ? free : oldest != no_slot ? oldest : turn;
    if (chosen == no_slot)
        return no_slot;
    auto* slot = state + nearest_slot_offset(chosen);
    std::memset(slot, 0, nearest_slot_size);
    put32(slot + slot_phase_offset, nearest_asked);
    put32(slot + nearest_labels_offset, labels);
    put32(slot + nearest_generation_offset, generation);
    put32(slot + nearest_landmass_offset, landmass);
    put32(slot + nearest_used_offset, tick);
    put32(slot + nearest_asked_offset, tick);
    waiting = true;
    return chosen;
}

bool FlowCache::start_landmass_job(uint32_t tick, uint32_t revision) noexcept {
    auto* state = state_.get();
    auto* section = state + landmass_offset_;
    const auto start = [&](uint32_t kind, uint32_t slot) {
        put32(section + job_kind_offset, kind);
        put32(section + job_slot_offset, slot);
        put32(section + job_step_offset, kind == labels_job ? clearing_step : forward_step);
        put32(section + job_cursor_offset, 0);
        put32(section + job_head_offset, 0);
        put32(section + job_tail_offset, 0);
        put32(section + job_flood_offset, no_landmass);
        put32(section + job_revision_offset, revision);
        put32(section + job_flood_nodes_offset, 0);
        return true;
    };
    // Labels asked for and never made come first, longest asked first.
    uint32_t best = no_slot, best_age = 0;
    for (uint32_t i = 0; i < landmass_slots_; ++i) {
        const auto* slot = state + label_slot_offset(i);
        const auto age = tick - get32(slot + slot_asked_offset);
        if (get32(slot + slot_phase_offset) == slot_asked && (best == no_slot || age > best_age)) {
            best = i;
            best_age = age;
        }
    }
    if (best != no_slot)
        return start(labels_job, best);
    // Then nearest-node maps asked for whose labels are still the same.
    for (uint32_t i = 0; i < nearest_slots_; ++i) {
        auto* slot = state + nearest_slot_offset(i);
        if (get32(slot + slot_phase_offset) != nearest_asked)
            continue;
        const auto owner = get32(slot + nearest_labels_offset);
        if (owner >= landmass_slots_ ||
            get32(state + label_slot_offset(owner) + slot_phase_offset) != slot_labelled ||
            get32(state + label_slot_offset(owner) + slot_generation_offset) !=
                get32(slot + nearest_generation_offset)) {
            put32(slot + slot_phase_offset, nearest_empty);
            continue;
        }
        const auto age = tick - get32(slot + nearest_asked_offset);
        if (best == no_slot || age > best_age) {
            best = i;
            best_age = age;
        }
    }
    if (best != no_slot) {
        put32(state + nearest_slot_offset(best) + slot_phase_offset, nearest_building);
        return start(nearest_job, best);
    }
    // Then labels in use, made at another static revision or long ago,
    // the oldest first: the map or what the player has seen may have changed.
    for (uint32_t i = 0; i < landmass_slots_; ++i) {
        const auto* slot = state + label_slot_offset(i);
        const auto age = tick - get32(slot + slot_built_offset);
        if (get32(slot + slot_phase_offset) != slot_labelled ||
            tick - get32(slot + slot_used_offset) >= slot_keep_ticks || age < relabel_grace ||
            (get32(slot + slot_revision_offset) == revision && age < relabel_period))
            continue;
        if (best == no_slot || age > best_age) {
            best = i;
            best_age = age;
        }
    }
    if (best != no_slot)
        return start(labels_job, best);
    return false;
}

void FlowCache::label_landmasses(
    uint32_t tick, const FlowSampler& sampler, uint32_t& budget
) noexcept {
    auto* state = state_.get();
    auto* section = state + landmass_offset_;
    const auto index = get32(section + job_slot_offset);
    auto* slot = state + label_slot_offset(index);
    auto* labels = slot + label_slot_size;
    auto* build = state + build_labels_offset();
    auto* cells = build + static_cast<std::size_t>(cells_) * 4;
    const auto total = static_cast<uint32_t>(footprint_cells());
    auto* queue = cells + total;
    const auto movement_class = get32(slot + slot_class_offset);
    const auto player = get32(slot + slot_player_offset);
    const auto at = [](uint8_t* table, uint32_t entry) {
        return table + static_cast<std::size_t>(entry) * 4;
    };
    // The coarse node a footprint cell is the origin of, or no node.
    const auto node_of = [&](uint32_t cell) {
        const auto x = cell % width_, z = cell / width_;
        return x % stride_ == 0 && z % stride_ == 0 ? (z / stride_) * columns_ + x / stride_
                                                    : no_node;
    };
    const auto passable = [&](uint32_t cell) {
        return sampler.cell && sampler.cell(
                                   sampler.context,
                                   movement_class,
                                   player,
                                   static_cast<int32_t>(cell % width_),
                                   static_cast<int32_t>(cell / width_)
                               ) != 0;
    };
    auto cursor = get32(section + job_cursor_offset);
    int64_t left = budget;
    if (get32(section + job_step_offset) == clearing_step) {
        // Every footprint cell first, then every node label.
        const auto entries = total + cells_;
        const auto amount = static_cast<uint32_t>(
            std::min<uint64_t>(entries - cursor, uint64_t(left) * cleared_entries_per_work)
        );
        for (uint32_t entry = cursor; entry < cursor + amount; ++entry) {
            if (entry < total)
                cells[entry] = cell_unvisited;
            else
                put32(at(build, entry - total), label_unvisited);
        }
        cursor += amount;
        left -= std::max<int64_t>(
            1, (amount + cleared_entries_per_work - 1) / cleared_entries_per_work
        );
        if (cursor == entries) {
            put32(section + job_step_offset, scanning_step);
            cursor = 0;
        }
    }
    auto head = get32(section + job_head_offset), tail = get32(section + job_tail_offset);
    auto flood = get32(section + job_flood_offset);
    auto flood_nodes = get32(section + job_flood_nodes_offset);
    // Marks a footprint cell reached by the flood, and its node labelled.
    const auto reach = [&](uint32_t cell) {
        cells[cell] = cell_reached;
        // A flood reaches each cell once, so its queue holds at most every
        // cell; restore refuses a queue that repeats one.
        if (tail < total)
            put32(at(queue, tail++), cell);
        if (const auto node = node_of(cell); node != no_node) {
            put32(at(build, node), flood);
            ++flood_nodes;
        }
    };
    const auto block = [&](uint32_t cell) {
        cells[cell] = cell_blocked;
        if (const auto node = node_of(cell); node != no_node)
            put32(at(build, node), label_blocked);
    };
    bool done = false;
    while (left > 0 && get32(section + job_step_offset) == scanning_step) {
        if (head < tail) {
            // A footprint steps to a neighbouring cell when it can stand on
            // both, so a landmass is the cells joined by such steps.
            const auto cell = get32(at(queue, head++));
            --left;
            const int32_t x = static_cast<int32_t>(cell % width_);
            const int32_t z = static_cast<int32_t>(cell / width_);
            for (size_t d = 0; d < flood_dx.size(); ++d) {
                const int32_t nx = x + flood_dx[d], nz = z + flood_dz[d];
                if (nx < 0 || nz < 0 || static_cast<uint32_t>(nx) >= width_ ||
                    static_cast<uint32_t>(nz) >= height_)
                    continue;
                const auto next = static_cast<uint32_t>(nz) * width_ + static_cast<uint32_t>(nx);
                if (cells[next] != cell_unvisited)
                    continue;
                --left;
                if (passable(next))
                    reach(next);
                else
                    block(next);
            }
            continue;
        }
        if (flood != no_landmass) {
            // The landmass is whole. One of fewer nodes than the floor names
            // none: its cells come off the queue, the last first, sixteen a
            // unit of work, and their nodes are labelled so, however many
            // cells it holds.
            if (flood_nodes < landmass_node_floor && tail != 0) {
                const auto amount = static_cast<uint32_t>(
                    std::min<uint64_t>(tail, uint64_t(left) * cleared_entries_per_work)
                );
                for (uint32_t taken = 0; taken < amount; ++taken)
                    if (const auto node = node_of(get32(at(queue, --tail))); node != no_node) {
                        put32(at(build, node), label_small);
                        --flood_nodes;
                    }
                head = tail;
                put64(
                    section + labelled_cells_offset, get64(section + labelled_cells_offset) + amount
                );
                left -= (amount + cleared_entries_per_work - 1) / cleared_entries_per_work;
                // A flood ends with its queue: none is left without cells.
                if (tail != 0)
                    continue;
            }
            put64(section + labelled_cells_offset, get64(section + labelled_cells_offset) + tail);
            flood = no_landmass;
            flood_nodes = 0;
            head = tail = 0;
            --left;
            continue;
        }
        if (cursor == total) {
            done = true;
            break;
        }
        // Scan order makes each landmass's first footprint cell its name.
        const auto cell = cursor++;
        --left;
        if (cells[cell] != cell_unvisited)
            continue;
        if (!passable(cell)) {
            block(cell);
            continue;
        }
        flood = cell;
        reach(cell);
    }
    put32(section + job_cursor_offset, cursor);
    put32(section + job_head_offset, head);
    put32(section + job_tail_offset, tail);
    put32(section + job_flood_offset, flood);
    put32(section + job_flood_nodes_offset, flood_nodes);
    budget = static_cast<uint32_t>(std::max<int64_t>(left, 0));
    if (!done)
        return;
    // Labels that came out the same keep their generation and their maps.
    const auto bytes = static_cast<std::size_t>(cells_) * 4;
    if (get32(slot + slot_phase_offset) != slot_labelled ||
        std::memcmp(labels, build, bytes) != 0) {
        std::memcpy(labels, build, bytes);
        put32(slot + slot_generation_offset, get32(slot + slot_generation_offset) + 1);
    }
    put32(slot + slot_phase_offset, slot_labelled);
    put32(slot + slot_revision_offset, get32(section + job_revision_offset));
    put32(slot + slot_built_offset, tick);
    put32(section + job_kind_offset, no_job);
}

void FlowCache::spread_nearest_nodes(uint32_t& budget) noexcept {
    auto* state = state_.get();
    auto* section = state + landmass_offset_;
    auto* slot = state + nearest_slot_offset(get32(section + job_slot_offset));
    auto* nodes = slot + nearest_slot_size;
    const auto* labels =
        state + label_slot_offset(get32(slot + nearest_labels_offset)) + label_slot_size;
    const auto landmass = get32(slot + nearest_landmass_offset);
    // The build labels hold each node's distance while a map is made.
    auto* distances = state + build_labels_offset();
    const auto at = [](uint8_t* table, uint32_t node) {
        return table + static_cast<std::size_t>(node) * 4;
    };
    // Takes a neighbour's nearest node when the step from it is shorter.
    const auto relax = [&](uint32_t node, int32_t x, int32_t z, int32_t step_x, int32_t step_z) {
        const int32_t nx = x + step_x, nz = z + step_z;
        if (nx < 0 || nz < 0 || static_cast<uint32_t>(nx) >= columns_ ||
            static_cast<uint32_t>(nz) >= rows_)
            return;
        const auto from = static_cast<uint32_t>(nz) * columns_ + static_cast<uint32_t>(nx);
        const auto reach = get32(at(distances, from));
        if (reach == unreached_distance)
            return;
        const auto length =
            reach + (step_x != 0 && step_z != 0 ? diagonal_distance : straight_distance);
        if (length < get32(at(distances, node))) {
            put32(at(distances, node), length);
            put32(at(nodes, node), get32(at(nodes, from)));
        }
    };
    auto cursor = get32(section + job_cursor_offset);
    auto left = static_cast<uint64_t>(budget) * nearest_nodes_per_work;
    // A forward pass in scan order takes each node's own landmass node or the
    // nearest from the neighbours before it, and a backward pass from those
    // after it: a chamfer distance of 5 a straight step and 7 a diagonal one.
    while (left != 0 && get32(section + job_step_offset) == forward_step) {
        if (cursor == cells_) {
            put32(section + job_step_offset, backward_step);
            cursor = cells_;
            break;
        }
        const auto node = cursor++;
        --left;
        const bool own = get32(labels + static_cast<std::size_t>(node) * 4) == landmass;
        put32(at(nodes, node), own ? node : no_node);
        put32(at(distances, node), own ? 0 : unreached_distance);
        const int32_t x = static_cast<int32_t>(node % columns_);
        const int32_t z = static_cast<int32_t>(node / columns_);
        relax(node, x, z, -1, 0);
        relax(node, x, z, 0, -1);
        relax(node, x, z, -1, -1);
        relax(node, x, z, 1, -1);
    }
    while (left != 0 && get32(section + job_step_offset) == backward_step && cursor != 0) {
        const auto node = --cursor;
        --left;
        const int32_t x = static_cast<int32_t>(node % columns_);
        const int32_t z = static_cast<int32_t>(node / columns_);
        relax(node, x, z, 1, 0);
        relax(node, x, z, 0, 1);
        relax(node, x, z, 1, 1);
        relax(node, x, z, -1, 1);
    }
    put32(section + job_cursor_offset, cursor);
    budget = static_cast<uint32_t>(left / nearest_nodes_per_work);
    if (get32(section + job_step_offset) != backward_step || cursor != 0)
        return;
    put64(section + nearest_nodes_offset, get64(section + nearest_nodes_offset) + cells_);
    put32(slot + slot_phase_offset, nearest_ready);
    put32(section + job_kind_offset, no_job);
}

void FlowCache::advance_landmasses(
    uint32_t tick, uint32_t revision, const FlowSampler& sampler
) noexcept {
    auto* section = state_.get() + landmass_offset_;
    uint32_t budget = landmass_work_per_tick_;
    while (budget != 0) {
        const auto kind = get32(section + job_kind_offset);
        if (kind == no_job) {
            if (!start_landmass_job(tick, revision))
                return;
            continue;
        }
        if (kind == labels_job)
            label_landmasses(tick, sampler, budget);
        else
            spread_nearest_nodes(budget);
    }
}

FlowCache::LandmassPlacement FlowCache::place_goal(
    const FlowContext& context,
    const oa::Unit& unit,
    Goal& goal,
    const FlowSampler& sampler,
    const bool& exhausted
) noexcept {
    auto* state = state_.get();
    const auto tick = get32(state + 32);
    LandmassPlacement placement{FlowResult::ready, no_landmass, false};
    bool waiting = false;
    const auto slot = find_labels(context.movement_class, context.player, tick, waiting);
    if (waiting) {
        placement.result = FlowResult::pending;
        return placement;
    }
    if (slot == no_slot)
        return placement;
    const auto* labels = state + label_slot_offset(slot) + label_slot_size;
    const auto label_of = [&](uint32_t node) {
        return get32(labels + static_cast<std::size_t>(node) * 4);
    };
    const auto names_landmass = [](uint32_t label) { return label < label_small; };
    // The sampler reads the movement class and player from a key's first bytes.
    std::array<uint8_t, 8> sample_key{};
    put32(sample_key.data(), context.movement_class);
    put32(sample_key.data() + 4, context.player);
    const auto unit_node = connect_anchor(
        columns_, rows_, stride_, unit.cell_x, unit.cell_z, sample_key.data(), sampler
    );
    if (exhausted) {
        placement.result = FlowResult::pending;
        return placement;
    }
    // A unit off the coarse nodes, or on a landmass below the floor, keeps
    // its goal.
    if (unit_node < 0 || !names_landmass(label_of(static_cast<uint32_t>(unit_node))))
        return placement;
    const auto landmass = label_of(static_cast<uint32_t>(unit_node));
    placement.landmass = landmass;
    const bool on_map = goal.cell[0] >= 0 && goal.cell[1] >= 0 &&
                        static_cast<uint32_t>(goal.cell[0]) < width_ &&
                        static_cast<uint32_t>(goal.cell[1]) < height_;
    int32_t goal_node = -1;
    if (on_map) {
        const bool blocked =
            !sampler.cell ||
            sampler.cell(
                sampler.context, context.movement_class, context.player, goal.cell[0], goal.cell[1]
            ) == 0;
        if (exhausted) {
            placement.result = FlowResult::pending;
            return placement;
        }
        if (!blocked) {
            goal_node = connect_anchor(
                columns_, rows_, stride_, goal.cell[0], goal.cell[1], sample_key.data(), sampler
            );
            if (exhausted) {
                placement.result = FlowResult::pending;
                return placement;
            }
            // A goal in a passage too narrow for any coarse node, or on a
            // node the labels do not know, is the ordinary search's.
            if (goal_node < 0)
                return placement;
            const auto label = label_of(static_cast<uint32_t>(goal_node));
            if (label == landmass) {
                placement.goal_anchored = true;
                return placement;
            }
            if (label != label_small && !names_landmass(label))
                return placement;
        }
    }
    // The goal is blocked, off the map or on another landmass: route to the
    // node of the unit's landmass nearest it.
    uint32_t look = 0;
    if (goal_node >= 0) {
        look = static_cast<uint32_t>(goal_node);
    } else {
        const auto half = static_cast<int32_t>(stride_ / 2);
        const auto column = std::clamp<int32_t>(
            (int32_t{goal.cell[0]} + half) / static_cast<int32_t>(stride_),
            0,
            static_cast<int32_t>(columns_) - 1
        );
        const auto row = std::clamp<int32_t>(
            (int32_t{goal.cell[1]} + half) / static_cast<int32_t>(stride_),
            0,
            static_cast<int32_t>(rows_) - 1
        );
        look = static_cast<uint32_t>(row) * columns_ + static_cast<uint32_t>(column);
    }
    const auto nearest = find_nearest(slot, landmass, tick, waiting);
    if (waiting) {
        placement.result = FlowResult::pending;
        return placement;
    }
    if (nearest == no_slot)
        return placement;
    const auto target = get32(
        state + nearest_slot_offset(nearest) + nearest_slot_size +
        static_cast<std::size_t>(look) * 4
    );
    if (target >= cells_ || label_of(target) != landmass)
        return placement;
    goal.cell = {
        static_cast<int16_t>((target % columns_) * stride_),
        static_cast<int16_t>((target / columns_) * stride_)
    };
    return placement;
}

bool FlowCache::valid_landmasses(std::span<const uint8_t> bytes) noexcept {
    const auto* section = bytes.data() + landmass_offset_;
    if (std::memcmp(section, state_.get() + landmass_offset_, job_kind_offset) != 0)
        return false;
    const auto total = static_cast<uint32_t>(footprint_cells());
    const auto at = [&](std::size_t offset, uint32_t entry) {
        return get32(bytes.data() + offset + static_cast<std::size_t>(entry) * 4);
    };
    // The footprint cell a coarse node lies on, and the node of a cell that
    // is a node's.
    const auto origin_of = [&](uint32_t node) {
        return (node / columns_) * stride_ * width_ + (node % columns_) * stride_;
    };
    const auto node_of = [&](uint32_t cell) {
        const auto x = cell % width_, z = cell / width_;
        return x % stride_ == 0 && z % stride_ == 0 ? (z / stride_) * columns_ + x / stride_
                                                    : no_node;
    };
    // A landmass is named by its first footprint cell in scan order, which no
    // node of it comes before.
    const auto valid_label = [&](uint32_t label, uint32_t node) {
        return label == label_blocked || label == label_small || label <= origin_of(node);
    };
    const auto kind = get32(section + job_kind_offset);
    const auto slot = get32(section + job_slot_offset), step = get32(section + job_step_offset);
    for (uint32_t i = 0; i < landmass_slots_; ++i) {
        const auto offset = label_slot_offset(i);
        const auto* label_slot = bytes.data() + offset;
        const auto phase = get32(label_slot + slot_phase_offset);
        if (phase > slot_labelled ||
            (phase != slot_empty && (get32(label_slot + slot_class_offset) >= 32 ||
                                     get32(label_slot + slot_player_offset) >= 10)))
            return false;
        if (phase != slot_labelled)
            continue;
        for (uint32_t node = 0; node < cells_; ++node)
            if (!valid_label(at(offset + label_slot_size, node), node))
                return false;
    }
    for (uint32_t i = 0; i < nearest_slots_; ++i) {
        const auto offset = nearest_slot_offset(i);
        const auto* map = bytes.data() + offset;
        const auto phase = get32(map + slot_phase_offset);
        if (phase > nearest_ready ||
            (phase != nearest_empty && (get32(map + nearest_labels_offset) >= landmass_slots_ ||
                                        get32(map + nearest_landmass_offset) >= total)))
            return false;
        // A map is building only while the running job builds it: one left
        // building would keep its requests waiting for good.
        if (phase == nearest_building && (kind != nearest_job || slot != i))
            return false;
        if (phase == nearest_ready)
            for (uint32_t node = 0; node < cells_; ++node) {
                const auto nearest = at(offset + nearest_slot_size, node);
                if (nearest != no_node && nearest >= cells_)
                    return false;
            }
    }
    if (kind == no_job)
        return true;
    const auto cursor = get32(section + job_cursor_offset);
    const auto head = get32(section + job_head_offset), tail = get32(section + job_tail_offset);
    const auto flood = get32(section + job_flood_offset);
    const auto flood_nodes = get32(section + job_flood_nodes_offset);
    const auto build = build_labels_offset();
    const auto cells = build + static_cast<std::size_t>(cells_) * 4, queue = cells + total;
    if (kind > nearest_job || step > 1 || head > tail || tail > total)
        return false;
    if (kind == labels_job) {
        if (slot >= landmass_slots_ ||
            get32(bytes.data() + label_slot_offset(slot) + slot_phase_offset) == slot_empty ||
            cursor > (step == clearing_step ? total + cells_ : total) ||
            (flood != no_landmass && (flood >= total || step != scanning_step || tail == 0)) ||
            (flood == no_landmass && (tail != 0 || flood_nodes != 0)) || flood_nodes > tail)
            return false;
        if (step == clearing_step) {
            // Below the cursor every footprint cell, then every node label,
            // is cleared.
            for (uint32_t entry = 0; entry < cursor; ++entry)
                if (entry < total ? bytes[cells + entry] != cell_unvisited
                                  : at(build, entry - total) != label_unvisited)
                    return false;
            return true;
        }
        // The scan has looked at every cell below its cursor, and each cell's
        // state is unvisited, blocked or reached.
        for (uint32_t cell = 0; cell < total; ++cell)
            if (bytes[cells + cell] > cell_reached ||
                (cell < cursor && bytes[cells + cell] == cell_unvisited))
                return false;
        // A node is labelled when its cell is visited: blocked with its cell,
        // and otherwise for the landmass that reached it.
        uint32_t flood_labels = 0;
        for (uint32_t node = 0; node < cells_; ++node) {
            const auto label = at(build, node);
            const auto state = bytes[cells + origin_of(node)];
            if ((label == label_unvisited) != (state == cell_unvisited) ||
                (label == label_blocked) != (state == cell_blocked) ||
                (label != label_unvisited && !valid_label(label, node)))
                return false;
            flood_labels += flood != no_landmass && label == flood;
        }
        if (flood == no_landmass)
            return true;
        // A flood starts at the cell before the cursor, the first of its
        // queue, and reaches no cell before it; its nodes are labelled for it.
        if (cursor != flood + 1 || at(queue, 0) != flood || flood_labels != flood_nodes)
            return false;
        // The flood's queue holds the cells it reached, each once: a repeated
        // cell would let the flood put more cells than the queue holds.
        auto* queued = restore_marks_.get();
        std::memset(queued, 0, (static_cast<std::size_t>(total) + 7) / 8);
        uint32_t queued_nodes = 0;
        for (uint32_t q = 0; q < tail; ++q) {
            const auto cell = at(queue, q);
            if (cell >= total || cell < flood || bytes[cells + cell] != cell_reached)
                return false;
            const auto bit = static_cast<uint8_t>(1u << (cell % 8));
            if ((queued[cell / 8] & bit) != 0)
                return false;
            queued[cell / 8] |= bit;
            if (const auto node = node_of(cell); node != no_node) {
                if (at(build, node) != flood)
                    return false;
                ++queued_nodes;
            }
        }
        return queued_nodes == flood_nodes;
    }
    if (slot >= nearest_slots_ || flood != no_landmass || tail != 0 || cursor > cells_)
        return false;
    const auto map = nearest_slot_offset(slot);
    if (get32(bytes.data() + map + slot_phase_offset) != nearest_building)
        return false;
    // Every node a pass has reached names a node of the map or none.
    const auto reached = step == forward_step ? cursor : cells_;
    for (uint32_t node = 0; node < reached; ++node) {
        const auto nearest = at(map + nearest_slot_size, node);
        if (nearest != no_node && nearest >= cells_)
            return false;
    }
    return true;
}

FlowResult FlowCache::route(
    const FlowContext& context,
    const oa::Unit& unit,
    const Goal& requested_goal,
    std::span<RoutePoint> points,
    uint32_t& count,
    const FlowSampler& sampler,
    RoutePoint* destination
) noexcept {
    count = 0;
    auto* state = state_.get();
    const auto stride = stride_;
    increment(state, 0);
    const auto fallback = [&]() {
        increment(state, 7);
        return FlowResult::fallback;
    };
    const auto wait = [&]() {
        increment(state, 5);
        return FlowResult::pending;
    };
    // The goal routed to: the requested one, or the node of the unit's own
    // landmass that replaces it.
    Goal goal = requested_goal;
    const auto goal_on_map = [&]() {
        return goal.cell[0] >= 0 && goal.cell[1] >= 0 &&
               static_cast<uint32_t>(goal.cell[0]) < width_ &&
               static_cast<uint32_t>(goal.cell[1]) < height_;
    };
    if (!fields_ || points.size() < 2 || goal.shape != GoalShape::circle ||
        unit.attach_parent != 0 || (landmass_slots_ == 0 && !goal_on_map()) || unit.cell_x < 0 ||
        unit.cell_z < 0 || static_cast<uint32_t>(unit.cell_x) >= width_ ||
        static_cast<uint32_t>(unit.cell_z) >= height_)
        return fallback();
    if (!valid_key(make_key(context, goal, large_region, no_landmass).data(), footprint_cells()) ||
        context.static_revision != get32(state + 36))
        return fallback();
    const auto entry_credits = get32(state + 60);

    struct ProbeBudget {
        const FlowSampler* sampler;
        uint8_t* remaining;
        bool exhausted{};
    } budget{&sampler, state + 60};

    const FlowSampler bounded{
        &budget,
        [](void* c, uint32_t movement_class, uint32_t player, int32_t x, int32_t z) -> uint8_t {
            auto& b = *static_cast<ProbeBudget*>(c);
            const auto remaining = get32(b.remaining);
            if (remaining == 0) {
                b.exhausted = true;
                return 0;
            }
            put32(b.remaining, remaining - 1);
            return b.sampler->cell
                       ? b.sampler->cell(b.sampler->context, movement_class, player, x, z)
                       : 0;
        }
    };
    uint32_t landmass = no_landmass;
    bool goal_anchored = false;
    if (landmass_slots_ != 0) {
        if (entry_credits == 0)
            return wait();
        const auto placement = place_goal(context, unit, goal, bounded, budget.exhausted);
        if (placement.result == FlowResult::pending)
            return wait();
        landmass = placement.landmass;
        goal_anchored = placement.goal_anchored;
        if (!goal_on_map())
            return fallback();
    }
    const auto key = make_key(context, goal, large_region, landmass);
    uint8_t* selected = nullptr;
    for (uint32_t i = 0; i < fields_; ++i) {
        auto* f = field(state, cells_, i);
        if (get32(f) != empty && same_key(f + field_key_offset, key)) {
            selected = f;
            break;
        }
    }
    if (!selected) {
        uint8_t* admitted = nullptr;
        for (uint32_t i = 0; i < admission_count; ++i) {
            auto* entry = state + header_size + i * admission_size;
            if (get32(entry + admission_status_offset) != 0 && same_key(entry, key)) {
                admitted = entry;
                break;
            }
        }
        if (!admitted) {
            const auto cursor = get32(state + 44) % admission_count;
            admitted = state + header_size + cursor * admission_size;
            std::memcpy(admitted, key.data(), key_size);
            put32(admitted + admission_tick_offset, get32(state + 32));
            put32(admitted + admission_status_offset, 1);
            put32(state + 44, (cursor + 1) % admission_count);
            return fallback();
        }
        // Never root a field at a goal no coarse node connects to: its seed
        // would fail, and the destination would hold storage it cannot use.
        if (!goal_anchored) {
            const auto anchor = connect_anchor(
                columns_, rows_, stride, goal.cell[0], goal.cell[1], key.data(), bounded
            );
            if (budget.exhausted)
                return wait();
            if (anchor < 0)
                return fallback();
        }
        // Singleton destinations use ordinary A*. Only repeated regions buy a field.
        put32(admitted + admission_status_offset, 2);
        uint32_t oldest = std::numeric_limits<uint32_t>::max();
        for (uint32_t i = 0; i < fields_; ++i) {
            auto* f = field(state, cells_, i);
            const auto phase = get32(f);
            if (phase == empty) {
                selected = f;
                break;
            }
            // A completed fine field must survive a delayed cohort's poll
            // before another destination can reuse its bounded storage.
            if (phase == complete &&
                get32(state + 32) - get32(f + completed_tick_offset) < completed_admission_grace)
                continue;
            if ((phase == complete || phase == failed) && get32(f + last_use_offset) <= oldest) {
                oldest = get32(f + last_use_offset);
                selected = f;
            }
        }
        if (!selected)
            return fallback();
        std::memset(selected, 0, metadata_size);
        put32(selected, clearing);
        std::memcpy(selected + field_key_offset, key.data(), key_size);
        put32(selected + seed_goal_offset, static_cast<uint32_t>(goal.cell[0]));
        put32(selected + seed_goal_offset + 4, static_cast<uint32_t>(goal.cell[1]));
        increment(state, 2);
    }
    put32(selected + last_use_offset, get32(state + 32));
    const auto phase = get32(selected);
    if (phase == failed)
        return fallback();
    if (phase == clearing)
        return wait();
    if (get32(state + 60) == 0)
        return wait();
    const auto deliver = [&]() {
        if (destination)
            *destination = point_at(goal.cell[0], goal.cell[1], context);
        increment(state, 1);
        return FlowResult::ready;
    };
    // A shared region is a search-work key, not a formation destination.
    // Keep each unit's own unobstructed lane before consulting the regional
    // tree or snapping its start to a coarse anchor. The same probe budget
    // bounds this test, and unknown terrain keeps the sampler's fog rules.
    // Reserve the full supercover cost before trying: a long optional ray
    // must not exhaust a small budget on every tick and starve local prefixes.
    const auto goal_dx = static_cast<uint32_t>(std::abs(int32_t{goal.cell[0]} - unit.cell_x));
    const auto goal_dz = static_cast<uint32_t>(std::abs(int32_t{goal.cell[1]} - unit.cell_z));
    const auto shortcut_probes =
        uint64_t{1} + std::max(goal_dx, goal_dz) + uint64_t{2} * std::min(goal_dx, goal_dz);
    if (shortcut_probes <= get32(state + 60) &&
        line_clear(bounded, key.data(), unit.cell_x, unit.cell_z, goal.cell[0], goal.cell[1])) {
        points[0] = {
            static_cast<int16_t>(unit.position.x >> 16), static_cast<int16_t>(unit.position.z >> 16)
        };
        points[1] = point_at(goal.cell[0], goal.cell[1], context);
        count = 2;
        return deliver();
    }
    if (budget.exhausted)
        return wait();
    const auto* directions = selected + metadata_size;
    auto node = connect_anchor(
        columns_, rows_, stride, unit.cell_x, unit.cell_z, key.data(), bounded, directions
    );
    if (node < 0)
        return budget.exhausted || phase == building ? wait() : fallback();
    std::array<uint32_t, goal_branch_capacity> goal_branch;
    size_t goal_branch_count{};
    {
        // The goal's own branch of the tree, when the root's chord to the
        // goal is blocked.
        const auto seed = get32(selected + seed_node_offset);
        const auto root_x = static_cast<int32_t>(seed % columns_) * static_cast<int32_t>(stride);
        const auto root_z = static_cast<int32_t>(seed / columns_) * static_cast<int32_t>(stride);
        const auto root_dx = static_cast<uint32_t>(std::abs(int32_t(goal.cell[0]) - root_x));
        const auto root_dz = static_cast<uint32_t>(std::abs(int32_t(goal.cell[1]) - root_z));
        const auto root_probes =
            uint64_t{1} + std::max(root_dx, root_dz) + uint64_t{2} * std::min(root_dx, root_dz);
        const auto prefix_probes =
            uint64_t{fine_prefix_edges} *
                (std::max(root_probes, uint64_t{fine_region_probe_bound}) + fine_edge_probe_bound) +
            fine_corner_probe_bound + visible_chord_probe_limit;
        const auto branch_probes =
            root_probes + fine_anchor_probe_bound + goal_branch_capacity + prefix_probes;
        const auto remaining_credits = get32(state + 60);
        const auto spent_credits = entry_credits - remaining_credits;
        // Reserve a whole bounded ancestry and ordinary prefix. Include this
        // call's source work when deciding whether any fresh quota can fit it.
        // Intrinsically smaller quotas retain their ordinary prefix opportunity.
        const bool can_test_root = branch_probes + spent_credits <= route_probes_per_tick_;
        if (can_test_root && root_probes > remaining_credits)
            return wait();
        if (can_test_root &&
            !line_clear(bounded, key.data(), root_x, root_z, goal.cell[0], goal.cell[1])) {
            if (budget.exhausted)
                return wait();
            // A clear root chord needs no ancestry. Reserve the larger branch
            // work only after the chord proves that a goal branch is needed.
            if (branch_probes - root_probes > get32(state + 60))
                return wait();
            auto goal_node = connect_anchor(
                columns_, rows_, stride, goal.cell[0], goal.cell[1], key.data(), bounded, directions
            );
            if (budget.exhausted)
                return wait();
            bool reached_root = false;
            while (goal_node >= 0 && goal_branch_count < goal_branch.size()) {
                const auto remaining = get32(state + 60);
                if (remaining == 0)
                    return wait();
                // Charge each ancestry node, including the root, so bounded
                // membership comparisons share the static-probe work credits.
                put32(state + 60, remaining - 1);
                goal_branch[goal_branch_count++] = static_cast<uint32_t>(goal_node);
                const auto direction = directions[goal_node];
                if (direction == root) {
                    reached_root = true;
                    break;
                }
                if (direction >= 8)
                    break;
                const int32_t parent_x = goal_node % static_cast<int32_t>(columns_) + dx[direction];
                const int32_t parent_z = goal_node / static_cast<int32_t>(columns_) + dz[direction];
                if (parent_x < 0 || parent_z < 0 || static_cast<uint32_t>(parent_x) >= columns_ ||
                    static_cast<uint32_t>(parent_z) >= rows_)
                    break;
                goal_node = parent_z * static_cast<int32_t>(columns_) + parent_x;
            }
            // A distant or undiscovered goal branch must not suppress a useful
            // ordinary prefix from the already discovered part of this tree.
            if (!reached_root)
                goal_branch_count = 0;
        }
    }
    // Validate every coarse edge afresh: newly learned static obstacles cannot
    // leak a stale route, even when the player's knowledge epoch stayed zero.
    points[0] = {
        static_cast<int16_t>(unit.position.x >> 16), static_cast<int16_t>(unit.position.z >> 16)
    };
    count = 1;
    int32_t x =
        static_cast<int32_t>(static_cast<uint32_t>(node) % columns_) * static_cast<int32_t>(stride);
    int32_t z =
        static_cast<int32_t>(static_cast<uint32_t>(node) / columns_) * static_cast<int32_t>(stride);
    const bool has_start_anchor = points[0] != point_at(x, z, context);
    if (has_start_anchor)
        points[count++] = point_at(x, z, context);
    const auto ready = [&]() {
        bool can_prune_corner = !has_start_anchor;
        if (has_start_anchor && count >= 3 &&
            points[2] != point_at(goal.cell[0], goal.cell[1], context)) {
            constexpr int32_t cell_width = 16, half_cell = cell_width / 2;
            const int32_t to_x =
                (int32_t(points[2][0]) - int32_t(context.footprint_x) * half_cell) / cell_width;
            const int32_t to_z =
                (int32_t(points[2][1]) - int32_t(context.footprint_z) * half_cell) / cell_width;
            const auto next_dx = static_cast<uint32_t>(std::abs(to_x - unit.cell_x));
            const auto next_dz = static_cast<uint32_t>(std::abs(to_z - unit.cell_z));
            const auto probes =
                uint64_t{1} + std::max(next_dx, next_dz) + uint64_t{2} * std::min(next_dx, next_dz);
            // The anchor connects the unit to the coarse tree, but is not a
            // required arrival point when its next corner is already visible.
            // A short hook here can otherwise turn a stock unit away from the
            // following leg or pull a whole queue onto one occupied cell.
            if (probes <= get32(state + 60) &&
                line_clear(bounded, key.data(), unit.cell_x, unit.cell_z, to_x, to_z)) {
                std::move(points.begin() + 2, points.begin() + count, points.begin() + 1);
                --count;
                can_prune_corner = true;
            }
        }
        if (can_prune_corner && count >= 3) {
            const int64_t delta_x = int64_t(points[2][0]) - points[0][0];
            const int64_t delta_z = int64_t(points[2][1]) - points[0][1];
            const int64_t px = int64_t(points[1][0]) - points[0][0];
            const int64_t pz = int64_t(points[1][1]) - points[0][1];
            const auto length = delta_x * delta_x + delta_z * delta_z;
            const auto dot = px * delta_x + pz * delta_z;
            const bool collinear_ahead = px * delta_z == pz * delta_x && dot >= 0 && dot <= length;
            const int32_t to_x = (int32_t(points[2][0]) - int32_t(context.footprint_x) * 8) / 16;
            const int32_t to_z = (int32_t(points[2][1]) - int32_t(context.footprint_z) * 8) / 16;
            const auto width = static_cast<uint32_t>(std::abs(to_x - unit.cell_x));
            const auto height = static_cast<uint32_t>(std::abs(to_z - unit.cell_z));
            const auto probes =
                uint64_t{1} + std::max(width, height) + uint64_t{2} * std::min(width, height);
            // One visible chord can remove a genuine BFS bend. Preserve the
            // original anchor shortcut's priority and the other units' credit.
            if (length != 0 && !collinear_ahead && probes <= visible_chord_probe_limit &&
                probes <= get32(state + 60)) {
                if (known_segment_clear(
                        bounded, key.data(), unit, context, points[2], static_cast<uint32_t>(probes)
                    )) {
                    std::move(points.begin() + 2, points.begin() + count, points.begin() + 1);
                    --count;
                }
            }
        }
        return deliver();
    };
    uint8_t previous = unseen;
    for (uint32_t walked = 0; walked < fine_prefix_edges; ++walked) {
        auto direction = directions[node];
        const auto branch_end = goal_branch.begin() + goal_branch_count;
        const auto branch = std::find(goal_branch.begin(), branch_end, static_cast<uint32_t>(node));
        const bool at_requested_anchor = branch != branch_end && branch == goal_branch.begin();
        if (branch != branch_end && branch != goal_branch.begin()) {
            const auto child_direction = directions[*(branch - 1)];
            if (child_direction >= 8) {
                count = 0;
                return fallback();
            }
            // At the shared ancestor, follow the requested goal's branch
            // outward instead of continuing toward the region's first seed.
            direction = static_cast<uint8_t>((child_direction + 4) % 8);
        }
        if (direction == root || at_requested_anchor ||
            (static_cast<uint32_t>(x) / large_region == get32(key.data() + key_region_offset) &&
             static_cast<uint32_t>(z) / large_region ==
                 get32(key.data() + key_region_offset + 4))) {
            if (line_clear(bounded, key.data(), x, z, goal.cell[0], goal.cell[1])) {
                const auto corner = point_at(x, z, context);
                if (points[count - 1] != corner) {
                    if (count == points.size())
                        break;
                    points[count++] = corner;
                }
                const auto finish = point_at(goal.cell[0], goal.cell[1], context);
                if (count == 1 || points[count - 1] != finish) {
                    if (count == points.size())
                        break;
                    points[count++] = finish;
                }
                if (count >= 2) {
                    return ready();
                }
            }
            if (direction == root || at_requested_anchor) {
                count = 0;
                return budget.exhausted ? wait() : fallback();
            }
        }
        if (direction >= 8) {
            count = 0;
            return fallback();
        }
        const int32_t nx = x + dx[direction] * static_cast<int32_t>(stride),
                      nz = z + dz[direction] * static_cast<int32_t>(stride);
        if (!line_clear(bounded, key.data(), x, z, nx, nz)) {
            if (budget.exhausted) {
                count = 0;
                return wait();
            }
            // Preserve the other legal branches and unfinished work. The
            // bounded dirty-field refresh replaces this tree later.
            if (get32(selected + dirty_offset) == 0) {
                put32(selected + dirty_offset, 1);
                put32(selected + dirty_since_offset, get32(state + 32));
            }
            count = 0;
            return fallback();
        }
        if (previous != unseen && previous != direction) {
            if (count == points.size())
                break;
            points[count++] = point_at(x, z, context);
        }
        x = nx;
        z = nz;
        node = (z / static_cast<int32_t>(stride)) * static_cast<int32_t>(columns_) +
               x / static_cast<int32_t>(stride);
        previous = direction;
        if (walked + 1 == fine_prefix_edges || count >= points.size() - 1) {
            if (count < points.size())
                points[count++] = point_at(x, z, context);
            break;
        }
    }
    if (count >= 2) {
        return ready();
    }
    count = 0;
    return fallback();
}

FlowCounters FlowCache::counters() const noexcept {
    const auto* p = state_.get() + 64;
    return {
        get64(p),
        get64(p + 8),
        get64(p + 16),
        get64(p + 24),
        get64(p + 32),
        get64(p + 40),
        get64(p + 48),
        get64(p + 56)
    };
}

uint32_t FlowCache::static_revision() const noexcept {
    return get32(state_.get() + 36);
}

bool FlowCache::crowd_shaped() const noexcept {
    return crowd_weight_ != 0;
}

std::span<const uint8_t> FlowCache::state_bytes() const noexcept {
    return {state_.get(), state_size_};
}

bool FlowCache::restore(std::span<const uint8_t> bytes) noexcept {
    if (bytes.size() != state_size_ || std::memcmp(bytes.data(), state_.get(), 32) != 0 ||
        get32(bytes.data() + 44) >= admission_count ||
        get32(bytes.data() + 56) != route_probes_per_tick_ ||
        get32(bytes.data() + 60) > route_probes_per_tick_ || get32(bytes.data() + 48) > 1 ||
        (fields_ != 0 && get32(bytes.data() + 40) >= fields_))
        return false;
    const auto valid_region = [&](const uint8_t* key) {
        return uint64_t{get32(key + key_region_offset)} * large_region < width_ &&
               uint64_t{get32(key + key_region_offset + 4)} * large_region < height_;
    };
    // A key names a landmass, a footprint cell, only where landmasses are
    // labelled.
    const uint64_t key_landmasses = landmass_slots_ != 0 ? footprint_cells() : 0;
    for (uint32_t i = 0; i < admission_count; ++i) {
        const auto* entry = bytes.data() + header_size + i * admission_size;
        const auto status = get32(entry + admission_status_offset);
        if (status > 2 ||
            (status != 0 && (!valid_key(entry, key_landmasses) || !valid_region(entry))))
            return false;
    }
    for (uint32_t i = 0; i < fields_; ++i) {
        const auto* f = field(bytes.data(), cells_, i);
        const auto* key = f + field_key_offset;
        const auto phase = get32(f);
        const auto head = get32(f + head_offset), tail = get32(f + tail_offset);
        const auto cleared = get32(f + cleared_offset);
        const auto seed_x = get32(f + seed_goal_offset), seed_z = get32(f + seed_goal_offset + 4);
        if (phase > failed || get32(f + dirty_offset) > 1 ||
            (phase != empty && !valid_key(key, key_landmasses)) || cleared > cells_ ||
            head > tail || (crowd_weight_ != 0 ? tail - head > cells_ : tail > cells_))
            return false;
        if (phase != empty && (get32(key + key_revision_offset) != get32(bytes.data() + 36) ||
                               seed_x >= width_ || seed_z >= height_))
            return false;
        if (phase != empty &&
            (!valid_region(key) || seed_x / large_region != get32(key + key_region_offset) ||
             seed_z / large_region != get32(key + key_region_offset + 4)))
            return false;
        if (phase == clearing && (cleared >= cells_ || head != 0 || tail != 0))
            return false;
        if (phase == building || phase == complete) {
            const auto seed = get32(f + seed_node_offset);
            if (seed >= cells_ || cleared != cells_ || tail == 0 ||
                (phase == building && head == tail) || (phase == complete && head != tail))
                return false;
            const auto* directions = f + metadata_size;
            if (directions[seed] != root)
                return false;
            uint32_t discovered = 0, roots = 0;
            for (uint32_t cell = 0; cell < cells_; ++cell) {
                const auto direction = directions[cell];
                if (direction == unseen)
                    continue;
                ++discovered;
                if (direction == root) {
                    ++roots;
                    continue;
                }
                if (direction >= 8)
                    return false;
                const auto x = static_cast<int32_t>(cell % columns_) + dx[direction];
                const auto z = static_cast<int32_t>(cell / columns_) + dz[direction];
                if (x < 0 || z < 0 || static_cast<uint32_t>(x) >= columns_ ||
                    static_cast<uint32_t>(z) >= rows_ ||
                    directions[static_cast<uint32_t>(z) * columns_ + static_cast<uint32_t>(x)] ==
                        unseen)
                    return false;
            }
            if (roots != 1)
                return false;
            if (crowd_weight_ != 0) {
                if (!valid_crowd_frontier(f, discovered))
                    return false;
                continue;
            }
            if (discovered != tail)
                return false;
            std::memset(restore_marks_.get(), 0, cells_);
            const auto* queue = directions + cells_;
            if (get32(queue) != seed)
                return false;
            for (uint32_t q = 0; q < tail; ++q) {
                const auto cell = get32(queue + static_cast<std::size_t>(q) * 4);
                if (cell >= cells_ || directions[cell] == unseen || restore_marks_[cell])
                    return false;
                if (q != 0) {
                    const auto direction = directions[cell];
                    if (direction >= 8)
                        return false;
                    const auto parent_x = static_cast<int32_t>(cell % columns_) + dx[direction];
                    const auto parent_z = static_cast<int32_t>(cell / columns_) + dz[direction];
                    const auto parent = static_cast<uint32_t>(parent_z) * columns_ +
                                        static_cast<uint32_t>(parent_x);
                    if (!restore_marks_[parent])
                        return false;
                }
                restore_marks_[cell] = 1;
            }
        }
    }
    if (landmass_slots_ != 0 && !valid_landmasses(bytes))
        return false;
    if (crowd_weight_ != 0) {
        if (std::memcmp(
                bytes.data() + crowd_offset_, state_.get() + crowd_offset_, crowd_header_size
            ) != 0)
            return false;
        for (uint32_t i = 0; i < fields_; ++i)
            if (get32(bytes.data() + crowd_mark_offset(i)) > 1)
                return false;
    }
    std::memcpy(state_.get(), bytes.data(), state_size_);
    return true;
}

bool FlowCache::valid_crowd_frontier(const uint8_t* f, uint32_t discovered) noexcept {
    const auto head = get32(f + head_offset), tail = get32(f + tail_offset);
    const auto* directions = f + metadata_size;
    const auto* queue = directions + cells_;
    // Each discovered node was put in the queue once, and once more for each
    // level it waited, at most 255; the queue holds no more than were found.
    if (tail < discovered || uint64_t{tail} > uint64_t{discovered} * (max_crowd_delay + 1) ||
        tail - head > discovered)
        return false;
    // A node's marks: waiting in the queue, on the walk being followed, and
    // known to lead to the root.
    constexpr uint8_t queued = 1, walking = 2, rooted = 4;
    std::memset(restore_marks_.get(), 0, cells_);
    for (uint32_t q = head; q != tail; ++q) {
        const auto node = get32(queue + static_cast<std::size_t>(q % cells_) * 4) & queue_node_mask;
        if (node >= cells_ || directions[node] == unseen || restore_marks_[node] != 0)
            return false;
        restore_marks_[node] = queued;
    }
    // The caller found every discovered node's parent on the map and
    // discovered, and the seed the only root.
    const auto parent_of = [&](uint32_t cell) {
        const auto direction = directions[cell];
        const auto x = static_cast<int32_t>(cell % columns_) + dx[direction];
        const auto z = static_cast<int32_t>(cell / columns_) + dz[direction];
        return static_cast<uint32_t>(z) * columns_ + static_cast<uint32_t>(x);
    };
    restore_marks_[get32(f + seed_node_offset)] |= rooted;
    for (uint32_t cell = 0; cell < cells_; ++cell) {
        if (directions[cell] == unseen || (restore_marks_[cell] & rooted) != 0)
            continue;
        // A node is found by expanding its parent, so no parent still waits,
        // and following parents reaches the root without coming round.
        for (auto at = cell; (restore_marks_[at] & rooted) == 0; at = parent_of(at)) {
            if ((restore_marks_[at] & walking) != 0 ||
                (restore_marks_[parent_of(at)] & queued) != 0)
                return false;
            restore_marks_[at] |= walking;
        }
        for (auto at = cell; (restore_marks_[at] & rooted) == 0; at = parent_of(at))
            restore_marks_[at] |= rooted;
    }
    return true;
}
} // namespace oa::sim::ground_orders

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/ground_orders/shared_routes.hpp"
#include "oa/sim/spatial_state/cell_classifier.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>

namespace oa::sim::ground_orders {
namespace {
constexpr uint32_t magic = 0x3246414f, format = 1, stride = 4, region = 32;
constexpr std::size_t header_size = 128, admission_size = 48, admission_count = 64;
constexpr std::size_t metadata_size = 80, key_size = 36;
constexpr uint32_t empty = 0, clearing = 1, building = 2, complete = 3, failed = 4;
constexpr size_t dirty_offset = 68, dirty_since_offset = 72, completed_tick_offset = 76;
constexpr uint32_t refresh_period = 30, completed_refresh_grace = 60;
constexpr uint8_t root = 0xfd, unseen = 0xff;
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
    std::memset(selected + 48, 0, 16);
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

Key make_key(const FlowContext& context, const Goal& goal) noexcept {
    Key key{};
    put32(key.data(), context.movement_class);
    put32(key.data() + 4, context.player);
    put32(key.data() + 8, context.static_revision);
    put64(key.data() + 12, context.knowledge_epoch);
    put32(key.data() + 20, context.footprint_x);
    put32(key.data() + 24, context.footprint_z);
    put32(key.data() + 28, static_cast<uint32_t>(goal.cell[0]) / region);
    put32(key.data() + 32, static_cast<uint32_t>(goal.cell[1]) / region);
    return key;
}

bool same_key(const uint8_t* a, const Key& b) noexcept {
    return std::memcmp(a, b.data(), key_size) == 0;
}

bool valid_key(const uint8_t* key) noexcept {
    return get32(key) < 32 && get32(key + 4) < 10 && get32(key + 20) > 0 && get32(key + 20) <= 64 &&
           get32(key + 24) > 0 && get32(key + 24) <= 64;
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

// The nearest anchor first, then its eight neighbours. A narrow off-grid
// corridor can have no connection; that request must use the full search.
int32_t connect_anchor(
    uint32_t columns,
    uint32_t rows,
    int32_t x,
    int32_t z,
    const uint8_t* key,
    const FlowSampler& sampler,
    const uint8_t* directions = nullptr
) noexcept {
    const int32_t centre_x = (x + 2) / static_cast<int32_t>(stride);
    const int32_t centre_z = (z + 2) / static_cast<int32_t>(stride);
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
    if (x < 0 || z < 0 || movement.footprint_x <= 0 || movement.footprint_z <= 0 ||
        uint64_t{static_cast<uint32_t>(x)} + static_cast<uint32_t>(movement.footprint_x) >=
            world.terrain_width ||
        uint64_t{static_cast<uint32_t>(z)} + static_cast<uint32_t>(movement.footprint_z) >=
            world.terrain_height)
        return false;
    auto classifier = movement;
    classifier.occupancy_before_tick = 0;
    for (int32_t row = 0; row < classifier.footprint_z; ++row)
        for (int32_t column = 0; column < classifier.footprint_x; ++column)
            if (sim::spatial_state::classify_movement_plot(
                    classifier, x + column, z + row, world
                ) == 0)
                return false;
    return true;
}

FlowCache::FlowCache(uint32_t width, uint32_t height, FlowLimits limits)
    : width_(width), height_(height), columns_(width / stride + (width % stride != 0)),
      rows_(height / stride + (height % stride != 0)), work_per_tick_(limits.work_per_tick),
      route_probes_per_tick_(limits.route_probes_per_tick) {
    const auto count = uint64_t{columns_} * rows_;
    if (count > 0 && count <= limits.total_cells && count <= std::numeric_limits<uint32_t>::max()) {
        cells_ = static_cast<uint32_t>(count);
        fields_ = std::min(limits.fields, limits.total_cells / cells_);
    }
    state_size_ =
        header_size + admission_count * admission_size +
        static_cast<std::size_t>(fields_) * (metadata_size + static_cast<std::size_t>(cells_) * 5);
    state_ = std::make_unique<uint8_t[]>(state_size_);
    restore_marks_ = std::make_unique<uint8_t[]>(cells_);
    put32(state_.get(), magic);
    put32(state_.get() + 4, format);
    put32(state_.get() + 8, width_);
    put32(state_.get() + 12, height_);
    put32(state_.get() + 16, fields_);
    put32(state_.get() + 20, cells_);
    put32(state_.get() + 24, work_per_tick_);
    put32(state_.get() + 28, stride);
    put32(state_.get() + 56, route_probes_per_tick_);
    put32(state_.get() + 60, route_probes_per_tick_);
}

void FlowCache::advance(uint32_t tick, uint32_t revision, const FlowSampler& sampler) noexcept {
    auto* state = state_.get();
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
            put32(f + 12, revision);
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
            if (get32(entry + 40) != 0)
                put32(entry + 8, revision);
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
            if (get32(f) != complete || get32(f + dirty_offset) == 0 ||
                tick - get32(f + completed_tick_offset) < completed_refresh_grace)
                continue;
            const auto age = tick - get32(f + dirty_since_offset);
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
        auto* f = field(state, cells_, cursor);
        cursor = (cursor + 1) % fields_;
        if (get32(f) != clearing) {
            ++idle;
            continue;
        }
        idle = 0;
        auto* directions = f + metadata_size;
        const auto clear = get32(f + 52);
        const auto amount = std::min({cells_ - clear, clear_slice, clear_budget});
        std::memset(directions + clear, unseen, amount);
        put32(f + 52, clear + amount);
        clear_budget -= amount;
        increment(state, 3, amount);
        if (clear + amount != cells_)
            continue;
        // Seeding performs line tests, so it consumes an expansion credit.
        ++work;
        const auto seed = connect_anchor(
            columns_,
            rows_,
            static_cast<int32_t>(get32(f + 40)),
            static_cast<int32_t>(get32(f + 44)),
            f + 4,
            sampler
        );
        if (seed < 0) {
            put32(f, failed);
            continue;
        }
        put32(f + 48, static_cast<uint32_t>(seed));
        directions[seed] = root;
        auto* queue = directions + cells_;
        put32(queue, static_cast<uint32_t>(seed));
        put32(f + 56, 0);
        put32(f + 60, 1);
        put32(f, building);
    }
    idle = 0;
    for (; work < work_per_tick_ && idle < fields_;) {
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
        auto head = get32(f + 56), tail = get32(f + 60);
        if (head == tail) {
            put32(f, complete);
            put32(f + completed_tick_offset, tick);
            continue;
        }
        const auto node = get32(queue + static_cast<std::size_t>(head++) * 4);
        put32(f + 56, head);
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
                                                       f + 4,
                                                       x * static_cast<int32_t>(stride),
                                                       z * static_cast<int32_t>(stride),
                                                       nx * static_cast<int32_t>(stride),
                                                       nz * static_cast<int32_t>(stride)
                                                   ))
                continue;
            // A valid field discovers each cell once. Keep malformed restored
            // state bounded even if that invariant is violated.
            if (tail == cells_) {
                put32(f, failed);
                break;
            }
            directions[neighbour] = static_cast<uint8_t>((d + 4) % 8);
            put32(queue + static_cast<std::size_t>(tail++) * 4, neighbour);
        }
        put32(f + 60, tail);
        increment(state, 4);
        if (head == tail && get32(f) == building) {
            put32(f, complete);
            put32(f + completed_tick_offset, tick);
        }
    }
    put32(state + 40, cursor);
}

FlowResult FlowCache::route(
    const FlowContext& context,
    const oa::Unit& unit,
    const Goal& goal,
    std::span<RoutePoint> points,
    uint32_t& count,
    const FlowSampler& sampler
) noexcept {
    count = 0;
    auto* state = state_.get();
    increment(state, 0);
    const auto fallback = [&]() {
        increment(state, 7);
        return FlowResult::fallback;
    };
    if (!fields_ || points.size() < 2 || goal.shape != GoalShape::circle ||
        unit.attach_parent != 0 || goal.cell[0] < 0 || goal.cell[1] < 0 ||
        static_cast<uint32_t>(goal.cell[0]) >= width_ ||
        static_cast<uint32_t>(goal.cell[1]) >= height_ || unit.cell_x < 0 || unit.cell_z < 0 ||
        static_cast<uint32_t>(unit.cell_x) >= width_ ||
        static_cast<uint32_t>(unit.cell_z) >= height_)
        return fallback();
    const auto key = make_key(context, goal);
    if (!valid_key(key.data()) || context.static_revision != get32(state + 36))
        return fallback();
    uint8_t* selected = nullptr;
    for (uint32_t i = 0; i < fields_; ++i) {
        auto* f = field(state, cells_, i);
        if (get32(f) != empty && same_key(f + 4, key)) {
            selected = f;
            break;
        }
    }
    if (!selected) {
        uint8_t* admitted = nullptr;
        for (uint32_t i = 0; i < admission_count; ++i) {
            auto* entry = state + header_size + i * admission_size;
            if (get32(entry + 40) != 0 && same_key(entry, key)) {
                admitted = entry;
                break;
            }
        }
        if (!admitted) {
            const auto cursor = get32(state + 44) % admission_count;
            admitted = state + header_size + cursor * admission_size;
            std::memcpy(admitted, key.data(), key_size);
            put32(admitted + 36, get32(state + 32));
            put32(admitted + 40, 1);
            put32(state + 44, (cursor + 1) % admission_count);
            return fallback();
        }
        // Singleton destinations use ordinary A*. Only repeated regions buy a field.
        put32(admitted + 40, 2);
        uint32_t oldest = std::numeric_limits<uint32_t>::max();
        for (uint32_t i = 0; i < fields_; ++i) {
            auto* f = field(state, cells_, i);
            if (get32(f) == empty) {
                selected = f;
                break;
            }
            if ((get32(f) == complete || get32(f) == failed) && get32(f + 64) <= oldest) {
                oldest = get32(f + 64);
                selected = f;
            }
        }
        if (!selected)
            return fallback();
        std::memset(selected, 0, metadata_size);
        put32(selected, clearing);
        std::memcpy(selected + 4, key.data(), key_size);
        put32(selected + 40, static_cast<uint32_t>(goal.cell[0]));
        put32(selected + 44, static_cast<uint32_t>(goal.cell[1]));
        increment(state, 2);
    }
    put32(selected + 64, get32(state + 32));
    const auto phase = get32(selected);
    if (phase == failed)
        return fallback();
    const auto wait = [&]() {
        increment(state, 5);
        return FlowResult::pending;
    };
    if (phase == clearing)
        return wait();
    if (get32(state + 60) == 0)
        return wait();

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
        increment(state, 1);
        return FlowResult::ready;
    }
    if (budget.exhausted)
        return wait();
    const auto* directions = selected + metadata_size;
    auto node =
        connect_anchor(columns_, rows_, unit.cell_x, unit.cell_z, key.data(), bounded, directions);
    if (node < 0)
        return budget.exhausted || phase == building ? wait() : fallback();
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
    if (points[0] != point_at(x, z, context))
        points[count++] = point_at(x, z, context);
    uint8_t previous = unseen;
    for (uint32_t walked = 0; walked < 128; ++walked) {
        const auto direction = directions[node];
        if (direction == root || (static_cast<uint32_t>(x) / region == get32(key.data() + 28) &&
                                  static_cast<uint32_t>(z) / region == get32(key.data() + 32))) {
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
                    increment(state, 1);
                    return FlowResult::ready;
                }
            }
            if (direction == root) {
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
            put32(selected, empty);
            increment(state, 6);
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
        if (walked == 127 || count >= points.size() - 1) {
            if (count < points.size())
                points[count++] = point_at(x, z, context);
            break;
        }
    }
    if (count >= 2) {
        increment(state, 1);
        return FlowResult::ready;
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
    for (uint32_t i = 0; i < admission_count; ++i) {
        const auto* entry = bytes.data() + header_size + i * admission_size;
        if (get32(entry + 40) > 2 || (get32(entry + 40) != 0 && !valid_key(entry)))
            return false;
    }
    for (uint32_t i = 0; i < fields_; ++i) {
        const auto* f = field(bytes.data(), cells_, i);
        const auto phase = get32(f);
        if (phase > failed || get32(f + dirty_offset) > 1 ||
            (phase != empty && !valid_key(f + 4)) || get32(f + 52) > cells_ ||
            get32(f + 56) > get32(f + 60) || get32(f + 60) > cells_)
            return false;
        if (phase != empty && (get32(f + 12) != get32(bytes.data() + 36) ||
                               get32(f + 40) >= width_ || get32(f + 44) >= height_))
            return false;
        if (phase == clearing &&
            (get32(f + 52) >= cells_ || get32(f + 56) != 0 || get32(f + 60) != 0))
            return false;
        if (phase == building || phase == complete) {
            const auto head = get32(f + 56), tail = get32(f + 60), seed = get32(f + 48);
            if (seed >= cells_ || get32(f + 52) != cells_ || tail == 0 ||
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
            if (roots != 1 || discovered != tail)
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
    std::memcpy(state_.get(), bytes.data(), state_size_);
    return true;
}
} // namespace oa::sim::ground_orders

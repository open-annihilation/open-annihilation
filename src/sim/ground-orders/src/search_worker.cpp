// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/ground_orders/search_worker.hpp"
#include "oa/sim/ground_orders/search_trace.hpp"

#include <bit>
#include <limits>

namespace oa::sim::ground_orders {
namespace {

constexpr int32_t preferred_pick_period = 4;

int32_t wrap_neg(int32_t value) noexcept {
    return static_cast<int32_t>(0u - static_cast<uint32_t>(value));
}

SearchHeap::Payload
pack_node(int16_t x, int16_t z, int32_t g, int32_t f, int16_t extra, int16_t consecutive) {
    SearchHeap::Payload value{};
    value[0] = static_cast<uint32_t>(static_cast<uint16_t>(x)) |
               (static_cast<uint32_t>(static_cast<uint16_t>(z)) << 16);
    value[1] = std::bit_cast<uint32_t>(g);
    value[2] = std::bit_cast<uint32_t>(f);
    value[3] = static_cast<uint32_t>(static_cast<uint16_t>(extra)) |
               (static_cast<uint32_t>(static_cast<uint16_t>(consecutive)) << 16);
    return value;
}

int16_t packed_x(const SearchHeap::Payload& value) noexcept {
    return std::bit_cast<int16_t>(static_cast<uint16_t>(value[0]));
}

int16_t packed_z(const SearchHeap::Payload& value) noexcept {
    return std::bit_cast<int16_t>(static_cast<uint16_t>(value[0] >> 16));
}

int32_t packed_g(const SearchHeap::Payload& value) noexcept {
    return std::bit_cast<int32_t>(value[1]);
}

int16_t packed_extra(const SearchHeap::Payload& value) noexcept {
    return std::bit_cast<int16_t>(static_cast<uint16_t>(value[3]));
}

int16_t packed_run(const SearchHeap::Payload& value) noexcept {
    return std::bit_cast<int16_t>(static_cast<uint16_t>(value[3] >> 16));
}

uint8_t heading_direction(uint16_t heading) noexcept {
    // The heading is zero-extended before the bias and the shift, so the
    // direction depends on the heading alone.
    const auto biased = static_cast<int32_t>(static_cast<uint32_t>(heading) + search_heading_bias);
    return static_cast<uint8_t>(biased >> static_cast<int32_t>(search_heading_shift)) & 7u;
}

void clear_dirty_cells(std::span<SearchMapCell> cells, std::span<uint32_t> dirty) {
    const auto cell_count = static_cast<int32_t>(cells.size());
    auto groups = (cell_count + 0xff) >> 8;
    groups -= 1;
    int32_t group = 0;
    auto clear_eight = [&](int32_t index, bool bounded) {
        for (int n = 8; n != 0; --n) {
            if (!bounded || index < cell_count)
                cells[static_cast<std::size_t>(index)].flags = 0;
            ++index;
        }
    };
    if (groups > 0) {
        do {
            auto bits = dirty[static_cast<std::size_t>(group)];
            if (bits != 0) {
                dirty[static_cast<std::size_t>(group)] = 0;
                auto index = group << 8;
                for (; bits != 0; bits >>= 1) {
                    if ((bits & 1u) != 0)
                        clear_eight(index, false);
                    index += 8;
                }
            }
            ++group;
        } while (group < groups);
    }
    auto bits = dirty[static_cast<std::size_t>(group)];
    if (bits == 0)
        return;
    dirty[static_cast<std::size_t>(group)] = 0;
    auto index = group << 8;
    for (; bits != 0; bits >>= 1) {
        if ((bits & 1u) != 0)
            clear_eight(index, true);
        index += 8;
    }
}

} // namespace

void SearchCellGrid::allocate(int32_t map_width, int32_t map_height) {
    width = map_width;
    height = map_height;
    // 32-bit multiply, then `product + 7` masked with ~7.
    const auto product = static_cast<uint32_t>(map_width) * static_cast<uint32_t>(map_height);
    const auto rounded = (product + uint32_t{7}) & ~uint32_t{7};
    count = rounded;
    // Every cell starts cleared, so a reused grid cannot keep old flags.
    cells.assign(static_cast<std::size_t>(rounded), {});
}

bool crossed_start_goal_axis(
    int32_t start_x, int32_t start_z, int32_t goal_x, int32_t goal_z, int32_t cell_x, int32_t cell_z
) noexcept {
    auto dx_goal = goal_x - start_x;
    auto dx_cell = cell_x - start_x;
    auto dz_cell = cell_z - start_z;
    auto dz_goal = goal_z - start_z;
    if (dx_goal < 0) {
        dx_goal = wrap_neg(dx_goal);
        dx_cell = wrap_neg(dx_cell);
    }
    if (dz_goal < 0) {
        dz_goal = wrap_neg(dz_goal);
        dz_cell = wrap_neg(dz_cell);
    }
    if (dz_cell == 0 && dx_cell > 0)
        return dx_cell <= dx_goal;
    if (dx_cell != dx_goal)
        return false;
    if (dz_cell <= 0)
        return false;
    return dz_cell <= dz_goal;
}

int32_t search_player_credit(int32_t tick_credit, uint16_t player_count) noexcept {
    if (player_count == 0)
        return 0;
    return tick_credit / static_cast<int32_t>(player_count);
}

int32_t
search_rescaled_heuristic(int32_t usage_count, uint16_t units_per_player, int32_t base) noexcept {
    if (units_per_player == 0)
        return base;
    const auto usage = usage_count / static_cast<int32_t>(units_per_player);
    if (usage < 1)
        return base * 6;
    if (usage < 2)
        return base * 3;
    return base;
}

namespace {

int32_t wrap_add(int32_t left, int32_t right) noexcept {
    return static_cast<int32_t>(static_cast<uint32_t>(left) + static_cast<uint32_t>(right));
}

int32_t wrap_sub(int32_t left, int32_t right) noexcept {
    return static_cast<int32_t>(static_cast<uint32_t>(left) - static_cast<uint32_t>(right));
}

} // namespace

SearchPlayerJobState::SearchPlayerJobState() {
    player_heuristic.fill(search_heuristic_scale);
}

void construct_search(
    SearchHeap& heap,
    SearchCellGrid& grid,
    std::vector<uint32_t>& touched,
    SearchPlayerJobState& jobs,
    int32_t world_width,
    int32_t world_height
) {
    heap = SearchHeap{};
    grid.allocate(world_width, world_height);
    // Arithmetic shift of (count + 0xff). Zero words is the empty grid, which
    // clears nothing.
    // A count of 2^31 - 255 cells or more gives no words either; its cells
    // start cleared all the same.
    const auto words = static_cast<int32_t>(grid.count + 0xffu) >> 8;
    if (words <= 0) {
        touched.clear();
    } else {
        touched.assign(static_cast<std::size_t>(words), 0xffffffffu);
        touched.back() = 0;
        auto index = grid.count - 0x100u;
        if (index < grid.count) {
            // index >> 8 stays below words, as index stays below the count.
            do {
                const auto word = static_cast<std::size_t>(index >> 8);
                touched[word] |= 1u << ((index >> 3) & 31u);
                ++index;
            } while (index < grid.count);
        }
        clear_dirty_cells(grid.cells, touched);
    }

    jobs.tick_credit = search_tick_credit;
    jobs.slice_cost = 0;
    jobs.round_robin = 0;
    jobs.active_heuristic = 0;
    jobs.base_heuristic = search_heuristic_scale;
    for (std::size_t player = 0; player < search_player_slots; ++player) {
        jobs.player_heuristic[player] = jobs.base_heuristic;
        jobs.credit[player] = 0;
        jobs.unit_cursor[player] = 0;
    }
}

SearchMapCell SearchWorker::cell(uint32_t x, uint32_t z) const noexcept {
    if (!in_map(x, z))
        return {};
    return at(x, z);
}

bool SearchWorker::in_map(uint32_t x, uint32_t z) const noexcept {
    return x < width_ && z < height_;
}

SearchMapCell& SearchWorker::at(uint32_t x, uint32_t z) {
    return cells_[static_cast<std::size_t>(z) * width_ + x];
}

const SearchMapCell& SearchWorker::at(uint32_t x, uint32_t z) const {
    return cells_[static_cast<std::size_t>(z) * width_ + x];
}

void SearchWorker::install_turn_tables() {
    turn_penalty_ = search_turn_penalty;
    for (std::size_t direction = 0; direction < move_cost_.size(); ++direction)
        move_cost_[direction] = (direction & 1u) ? search_diagonal_cost : search_cardinal_cost;
}

void SearchWorker::mark_dirty(uint32_t index) {
    dirty_[index >> 8] |= 1u << ((index >> 3) & 31u);
}

void SearchWorker::clear_dirty() {
    clear_dirty_cells(cells_, dirty_);
}

void SearchWorker::mark_goal(int32_t x, int32_t z) {
    const auto ux = static_cast<uint32_t>(x);
    const auto uz = static_cast<uint32_t>(z);
    if (!in_map(ux, uz))
        return;
    const auto index = uz * width_ + ux;
    at(ux, uz).flags = search_goal_bit;
    mark_dirty(index);
}

uint32_t SearchWorker::classify(int32_t x, int32_t z) const {
    const auto* map = record_.movement_map;
    const auto ux = static_cast<uint32_t>(x);
    const auto uz = static_cast<uint32_t>(z);
    if (ux >= map->width() || uz >= map->height())
        return 0;
    const auto coarse_x = (static_cast<int32_t>(map->footprint_x()) >> 2) + (x >> 1);
    const auto coarse_z = (static_cast<int32_t>(map->footprint_z()) >> 2) + (z >> 1);
    const auto coarse_w = static_cast<int32_t>(world_width_) >> 1;
    const auto coarse_h = static_cast<int32_t>(world_height_) >> 1;
    if (coarse_x >= coarse_w || coarse_z >= coarse_h)
        return 0;
    const auto index = static_cast<std::size_t>(coarse_w) * static_cast<std::size_t>(coarse_z) +
                       static_cast<std::size_t>(coarse_x);
    if (coarse_x < 0 || coarse_z < 0 || index >= sight_grid_.size())
        return 0;
    const auto bits = static_cast<uint32_t>(sight_grid_[index]);
    if (((1u << (mask_bit_ & 31u)) & bits) == 0)
        return 2;
    return map->cell(ux, uz);
}

int32_t SearchWorker::heuristic(int32_t x, int32_t z) const {
    const auto cost = goal_cost(*record_.goal, x, z);
    const auto product = int64_t(cost) * int64_t(heuristic_scale_);
    return static_cast<int32_t>(product >> 16);
}

void SearchWorker::expand_neighbor(
    const SearchHeap::Payload& parent, uint8_t parent_direction, uint32_t relative_turn
) {
    const auto direction = (static_cast<uint32_t>(parent_direction) + relative_turn) & 7u;
    const auto x = static_cast<int32_t>(packed_x(parent)) + search_direction_x[direction];
    const auto z = static_cast<int32_t>(packed_z(parent)) + search_direction_z[direction];
    const auto ux = static_cast<uint32_t>(x);
    const auto uz = static_cast<uint32_t>(z);
    if (!in_map(ux, uz))
        return;
    auto& cell = at(ux, uz);
    const auto visit = static_cast<SearchCellVisit>(cell.flags & 3u);
    if (visit == SearchCellVisit::unvisited) {
        const auto index = uz * width_ + ux;
        mark_dirty(index);
        const auto classification = classify(x, z);
        if (classification < 1 && (cell.flags & search_seed_bit) == 0) {
            cell.flags = static_cast<uint8_t>(cell.flags | 3u);
            return;
        }
        const auto estimate = heuristic(x, z);
        if (estimate > threshold_)
            cell.flags = static_cast<uint8_t>(cell.flags | 1u);
        else
            cell.flags = static_cast<uint8_t>(cell.flags | 5u);
        cell.predecessor = static_cast<uint8_t>(direction);
        const auto extra = static_cast<int16_t>(
            (1 < static_cast<int32_t>(classification)) ? 0 : search_difficult_extra
        );
        auto g = static_cast<int32_t>(move_cost_[direction]) +
                 static_cast<int32_t>(turn_penalty_[relative_turn]) + packed_g(parent) + extra;
        const auto run =
            relative_turn == 0 ? static_cast<int16_t>(packed_run(parent) + 1) : int16_t{1};
        if (relative_turn != 0 && packed_run(parent) < search_early_run_limit)
            g += search_early_turn_penalty;
        const auto f = g + estimate;
        const auto handle = heap_.insert(
            pack_node(static_cast<int16_t>(x), static_cast<int16_t>(z), g, f, extra, run)
        );
        cell.handle = handle;
        return;
    }
    if (visit != SearchCellVisit::open)
        return;
    auto& node = heap_.payload(cell.handle);
    auto g = static_cast<int32_t>(move_cost_[direction]) +
             static_cast<int32_t>(turn_penalty_[relative_turn]) + packed_extra(node) +
             packed_g(parent);
    if (relative_turn != 0 && packed_run(parent) < search_early_run_limit)
        g += search_early_turn_penalty;
    if (g >= packed_g(node))
        return;
    cell.predecessor = static_cast<uint8_t>(direction);
    const auto delta = g - packed_g(node);
    node[1] = std::bit_cast<uint32_t>(g);
    node[2] = std::bit_cast<uint32_t>(std::bit_cast<int32_t>(node[2]) + delta);
    const auto run = relative_turn == 0 ? static_cast<int16_t>(packed_run(parent) + 1) : int16_t{1};
    node[3] = (node[3] & 0xffffu) | (static_cast<uint32_t>(static_cast<uint16_t>(run)) << 16);
    heap_.decrease_key(cell.handle);
}

uint32_t SearchWorker::expand_best() {
    const auto parent = heap_.peek();
    heap_.pop();
    const auto x = static_cast<uint32_t>(static_cast<int32_t>(packed_x(parent)));
    const auto z = static_cast<uint32_t>(static_cast<int32_t>(packed_z(parent)));
    auto& cell = at(x, z);
    if ((cell.flags & search_goal_bit) != 0) {
        finish_ = {packed_x(parent), packed_z(parent)};
        return 1;
    }
    cell.flags = 2;
    auto relative = -fan_;
    if (relative <= fan_) {
        do {
            expand_neighbor(parent, cell.predecessor, static_cast<uint32_t>(relative) & 7u);
            ++relative;
        } while (relative <= fan_);
    }
    return 0;
}

int32_t SearchWorker::seed_wall_follow(uint32_t step_limit, bool& capped) {
    capped = false;
    auto min_cost = heuristic(start_[0], start_[1]);
    if (classify(start_[0], start_[1]) < 1)
        return min_cost;
    auto current_x = static_cast<int32_t>(start_[0]);
    auto current_z = static_cast<int32_t>(start_[1]);
    const auto seed_limit = static_cast<int32_t>(width_ * height_ * 8u + 8u);
    while (true) {
        if (step_limit != 0 && static_cast<uint32_t>(budget_) >= step_limit) {
            capped = true;
            return min_cost;
        }
        ++budget_;
        if (budget_ > seed_limit)
            return min_cost;
        if (min_cost == 0)
            return 0;
        uint8_t heading = 0;
        const auto dx = nearest_[0] - current_x;
        if (dx < 0)
            heading = 2;
        else if (dx > 0)
            heading = 6;
        else if (nearest_[1] - current_z > 0)
            heading = 4;
        const auto next_x = current_x + search_direction_x[heading];
        const auto next_z = current_z + search_direction_z[heading];
        if (classify(next_x, next_z) >= 1) {
            const auto ux = static_cast<uint32_t>(next_x);
            const auto uz = static_cast<uint32_t>(next_z);
            const auto index = uz * width_ + ux;
            mark_dirty(index);
            auto& cell = at(ux, uz);
            cell.flags = static_cast<uint8_t>(cell.flags | search_seed_bit);
            cell.predecessor = heading;
            if ((cell.flags & search_goal_bit) != 0)
                return 0;
            const auto cost = heuristic(next_x, next_z);
            current_x = next_x;
            current_z = next_z;
            if (cost < min_cost)
                min_cost = cost;
            continue;
        }
        auto left_x = current_x;
        auto left_z = current_z;
        auto right_x = current_x;
        auto right_z = current_z;
        heading = static_cast<uint8_t>(heading + 2) & 7u;
        auto seen = false;
        auto left_dir = heading;
        auto right_dir = heading;
        while (true) {
            if (step_limit != 0 && static_cast<uint32_t>(budget_) >= step_limit) {
                capped = true;
                return min_cost;
            }
            ++budget_;
            if (budget_ > seed_limit)
                return min_cost;
            const auto left_stop = static_cast<uint8_t>(left_dir - 3) & 7u;
            left_dir = static_cast<uint8_t>(left_dir - 2) & 7u;
            auto nx = left_x + search_direction_x[left_dir];
            auto nz = left_z + search_direction_z[left_dir];
            while (classify(nx, nz) < 1) {
                if (left_dir == left_stop)
                    return min_cost;
                left_dir = static_cast<uint8_t>(left_dir + 1) & 7u;
                nx = left_x + search_direction_x[left_dir];
                nz = left_z + search_direction_z[left_dir];
            }
            if (left_x == right_x && left_z == right_z && left_dir == right_dir && seen)
                return min_cost;
            left_x = nx;
            left_z = nz;
            seen = true;
            const auto lux = static_cast<uint32_t>(nx);
            const auto luz = static_cast<uint32_t>(nz);
            const auto lindex = luz * width_ + lux;
            mark_dirty(lindex);
            auto& left_cell = at(lux, luz);
            left_cell.flags = static_cast<uint8_t>(left_cell.flags | search_seed_bit);
            left_cell.predecessor = left_dir;
            if ((left_cell.flags & search_goal_bit) != 0)
                return 0;
            if (crossed_start_goal_axis(current_x, current_z, nearest_[0], nearest_[1], nx, nz)) {
                current_x = nx;
                current_z = nz;
                break;
            }
            const auto left_cost = heuristic(nx, nz);
            if (left_cost < min_cost)
                min_cost = left_cost;
            const auto right_stop = static_cast<uint8_t>(right_dir + 3) & 7u;
            right_dir = static_cast<uint8_t>(right_dir + 2) & 7u;
            auto rx = right_x - search_direction_x[right_dir];
            auto rz = right_z - search_direction_z[right_dir];
            while (classify(rx, rz) < 1) {
                if (right_dir == right_stop)
                    return min_cost;
                right_dir = static_cast<uint8_t>(right_dir - 1) & 7u;
                rx = right_x - search_direction_x[right_dir];
                rz = right_z - search_direction_z[right_dir];
            }
            if (left_x == right_x && left_z == right_z && right_dir == left_dir)
                return min_cost;
            right_x = rx;
            right_z = rz;
            const auto rux = static_cast<uint32_t>(rx);
            const auto ruz = static_cast<uint32_t>(rz);
            const auto rindex = ruz * width_ + rux;
            mark_dirty(rindex);
            auto& right_cell = at(rux, ruz);
            right_cell.flags = static_cast<uint8_t>(right_cell.flags | search_seed_bit);
            right_cell.predecessor = right_dir;
            if ((right_cell.flags & search_goal_bit) != 0)
                return 0;
            if (crossed_start_goal_axis(current_x, current_z, nearest_[0], nearest_[1], rx, rz)) {
                current_x = rx;
                current_z = rz;
                break;
            }
            const auto right_cost = heuristic(rx, rz);
            if (right_cost < min_cost)
                min_cost = right_cost;
        }
    }
}

void SearchWorker::fail_empty() {
    route_.clear();
}

void SearchWorker::publish_route() {
    // The trace reads the predecessor of each cell on the path, and no other.
    route_ = reconstruct_search_path(
        width_,
        height_,
        start_,
        finish_,
        record_.movement_map->footprint_x(),
        record_.movement_map->footprint_z(),
        cells_
    );
}

SearchAdvance SearchWorker::begin(
    SearchRecord record, const SearchBegin& begin, std::span<const uint16_t> sight_grid
) {
    // A refused job walks no cells, so the scan charges it no wall-follow steps.
    budget_ = 0;
    if (!record.unit || !record.navigation || !record.goal || !record.movement_map)
        return SearchAdvance::failed;
    {
        const auto& map = *record.movement_map;
        const auto count = static_cast<std::size_t>(map.width()) * map.height();
        if (map.width() != 0 && count / map.width() != map.height())
            return SearchAdvance::failed;
    }
    record_ = record;
    sight_grid_ = sight_grid;
    mask_bit_ = begin.mask_bit;
    heuristic_scale_ = begin.heuristic_scale;
    order_flags_ = 0;
    route_.clear();
    auto& map = *record_.movement_map;
    world_width_ = begin.world_width ? begin.world_width : map.width();
    world_height_ = begin.world_height ? begin.world_height : map.height();
    const auto next_width = map.width();
    const auto next_height = map.height();
    const auto count = static_cast<std::size_t>(next_width) * next_height;
    const auto dirty_words = static_cast<std::size_t>((static_cast<uint64_t>(count) + 0xff) >> 8);
    if (width_ != next_width || height_ != next_height || cells_.size() != count) {
        width_ = next_width;
        height_ = next_height;
        cells_.assign(count, {});
        dirty_.assign(dirty_words == 0 ? 1 : dirty_words, 0);
    }
    const auto& unit = record_.unit->record;
    start_ = {unit.cell_x, unit.cell_z};
    uint32_t unset_tick = 0;
    map.prepare_search(
        occupancy_rectangle(unit),
        begin.unit_changed_tick ? *begin.unit_changed_tick : unset_tick,
        begin.occupancy,
        begin.current_tick
    );
    install_turn_tables();
    clear_dirty();
    nearest_ = {start_[0], start_[1]};
    auto best = std::numeric_limits<int32_t>::max();
    for_each_goal_cell(*record_.goal, [&](int32_t x, int32_t z) {
        mark_goal(x, z);
        const auto dz = static_cast<int32_t>(start_[1]) - z;
        const auto dx = static_cast<int32_t>(start_[0]) - x;
        const auto distance = dz * dz + dx * dx;
        if (distance < best) {
            nearest_ = {static_cast<int16_t>(x), static_cast<int16_t>(z)};
            best = distance;
        }
    });
    if (goal_contains(*record_.goal, start_[0], start_[1])) {
        order_flags_ |= search_goal_contact_flag;
        if (record_.goal->order)
            record_.goal->order->raised_events |= search_goal_contact_flag;
        fail_empty();
        return SearchAdvance::failed;
    }
    const auto start_cost = heuristic(start_[0], start_[1]);
    if (static_cast<uint32_t>(start_[0]) >= width_ || static_cast<uint32_t>(start_[1]) >= height_) {
        order_flags_ |= search_seed_unresolved_flag;
        if (record_.goal->order)
            record_.goal->order->raised_events |= search_seed_unresolved_flag;
        fail_empty();
        return SearchAdvance::failed;
    }
    bool seed_capped{};
    const auto seed = seed_wall_follow(begin.seed_step_limit, seed_capped);
    // A truncated seed is not an arrival or a failed search. Only ordinary
    // goal-cost zero cells may finish the credited search that follows it.
    threshold_ = seed_capped ? 0 : seed;
    if (!seed_capped && seed == 0) {
        order_flags_ |= search_goal_contact_flag;
        if (record_.goal->order)
            record_.goal->order->raised_events |= search_goal_contact_flag;
    } else if (!seed_capped) {
        order_flags_ |= search_seed_unresolved_flag;
        if (record_.goal->order)
            record_.goal->order->raised_events |= search_seed_unresolved_flag;
        if (seed >= start_cost) {
            fail_empty();
            return SearchAdvance::failed;
        }
    }
    heap_.reset_open_set();
    const auto index = static_cast<uint32_t>(start_[1]) * width_ + static_cast<uint32_t>(start_[0]);
    mark_dirty(index);
    auto& start_cell = at(static_cast<uint32_t>(start_[0]), static_cast<uint32_t>(start_[1]));
    start_cell.flags = static_cast<uint8_t>(start_cell.flags | 1u);
    start_cell.predecessor = heading_direction(unit.heading);
    // The start node's extra (payload word 3) is zero; it is never read, because
    // the start cell is closed on first pop and closed cells are not reopened.
    const auto handle =
        heap_.insert(pack_node(start_[0], start_[1], 0, start_cost, 0, search_start_run));
    start_cell.handle = handle;
    fan_ = search_initial_fan;
    return SearchAdvance::in_progress;
}

SearchAdvance SearchWorker::advance_slice() {
    budget_ = 0;
    if (heap_.exhausted()) {
        fail_empty();
        return SearchAdvance::failed;
    }
    do {
        if (heap_.exhausted())
            return SearchAdvance::in_progress;
        ++budget_;
        if (expand_best() != 0) {
            publish_route();
            return SearchAdvance::succeeded;
        }
        fan_ = search_continue_fan;
    } while (budget_ < search_slice_expansions);
    return SearchAdvance::in_progress;
}

namespace {
void release_job(SearchScheduler& scheduler, const SearchUnitAccess& access) {
    OccupancyRectangle rectangle{};
    uint32_t changed_tick = 0;
    if (access.occupancy && scheduler.controller.active().unit)
        access.occupancy(
            access.context, *scheduler.controller.active().unit, rectangle, changed_tick
        );
    scheduler.controller.complete(rectangle, changed_tick);
}

void publish(
    SearchScheduler& scheduler, const SearchUnitAccess& access, std::span<const RoutePoint> route
) {
    const auto& job = scheduler.controller.active();
    if (access.publish && job.unit && job.navigation)
        access.publish(access.context, *job.unit, *job.navigation, route);
}

// Starts a job for the cursor unit when its navigator asks for a search.
// Returns the extra slice cost: 100 when a job was taken, and one more for
// each step of its wall-follow walk.
int32_t try_start_job(
    SearchScheduler& scheduler,
    sim::simulation_state::Unit& unit,
    uint8_t player,
    uint32_t tick,
    std::span<const uint16_t> sight_grid,
    const SearchUnitAccess& access
) {
    if (unit.record.type_index == 0 || unit.record.movement == 0 || !access.navigator)
        return 0;
    auto* navigation = access.navigator(access.context, unit);
    if (!navigation || !search_ready(*navigation, tick))
        return 0;
    SearchBegin begin;
    MovementMap* map = nullptr;
    begin.current_tick = tick;
    // A unit without a goal or a movement map starts no job.
    if (!navigation->goal || !access.prepare || !access.prepare(access.context, unit, begin, map) ||
        !map)
        return search_slice_expansions;
    auto& jobs = scheduler.jobs;
    jobs.active_heuristic = jobs.player_heuristic[player];
    begin.mask_bit = player;
    begin.heuristic_scale = jobs.active_heuristic;
    const SearchRecord record{&unit, navigation, navigation->goal, map};
    if (!scheduler.controller.begin(record))
        return search_slice_expansions;
    if (scheduler.worker.begin(record, begin, sight_grid) != SearchAdvance::in_progress) {
        publish(scheduler, access, {});
        release_job(scheduler, access);
    }
    // The walk's steps are charged with the start, whether or not the job
    // went on to search.
    return search_slice_expansions + scheduler.worker.budget();
}

// Expands the active job for the rest of the slice. Returns the slice cost.
int32_t continue_job(SearchScheduler& scheduler, const SearchUnitAccess& access) {
    auto& worker = scheduler.worker;
    if (worker.exhausted()) {
        publish(scheduler, access, {});
        release_job(scheduler, access);
        return 0;
    }
    int32_t cost = 0;
    do {
        if (worker.exhausted())
            break;
        ++cost;
        if (worker.expand_best() != 0) {
            worker.publish_route();
            publish(scheduler, access, worker.route());
            release_job(scheduler, access);
            break;
        }
        worker.set_fan(search_continue_fan);
    } while (cost < search_slice_expansions);
    return cost;
}
} // namespace

int32_t scan_player_jobs(
    SearchScheduler& scheduler,
    std::span<sim::simulation_state::Player> players,
    uint16_t player_count,
    uint16_t units_per_player,
    uint32_t tick,
    std::span<const uint16_t> sight_grid,
    const SearchUnitAccess& access
) {
    auto& state = scheduler.jobs;
    if (player_count == 0)
        return 0;

    state.refresh_tick = wrap_add(state.refresh_tick, 1);
    if (search_scale_period <= state.refresh_tick) {
        state.refresh_tick = 0;
        for (std::size_t player = 0; player < search_player_slots; ++player) {
            state.player_heuristic[player] = search_rescaled_heuristic(
                state.job_picks[player], units_per_player, state.base_heuristic
            );
            state.job_picks[player] = 0;
        }
    }

    int32_t budget = 0;
    const auto share = search_player_credit(state.tick_credit, player_count);
    for (uint8_t player = 0; player < search_player_slots; ++player) {
        if (player >= players.size() ||
            !sim::simulation_state::player_slot_active(player, players[player]))
            continue;
        state.credit[player] = wrap_add(state.credit[player], share);
        budget = wrap_add(budget, state.credit[player]);
    }

    std::array<bool, search_player_slots> preferred_asked{};
    int32_t steps = 0;
    while (budget > 0) {
        state.slice_cost = 0;
        if (scheduler.controller.idle()) {
            state.slice_cost = 1;
            bool credited = true;
            for (int turned = 0; state.credit[state.round_robin] < 1; ++turned) {
                if (turned == static_cast<int>(search_player_slots)) {
                    credited = false;
                    break;
                }
                const auto next = static_cast<uint8_t>(state.round_robin + 1u);
                state.round_robin = next > 9 ? uint8_t{0} : next;
            }
            if (!credited)
                break;
            const auto player = state.round_robin;
            state.job_picks[player] = wrap_add(state.job_picks[player], 1);
            if (player >= players.size() || players[player].units.empty())
                break;
            auto& units = players[player].units;
            auto& cursor = state.unit_cursor[player];
            if (static_cast<std::size_t>(cursor) >= units.size())
                break;
            const auto next_cursor =
                static_cast<std::size_t>(cursor) + 1 == units.size() ? uint32_t{0} : cursor + 1;
            auto selected = next_cursor;
            if (access.preferred_unit && !preferred_asked[player] &&
                state.job_picks[player] % preferred_pick_period == 0) {
                preferred_asked[player] = true;
                uint32_t offered{};
                if (access.preferred_unit(access.context, player, tick, offered) &&
                    static_cast<std::size_t>(offered) < units.size() &&
                    units[offered].record.owner_index == player)
                    selected = offered;
            }
            // An out-of-order pick borrows this credit, not the ordinary
            // cursor's next unit. A coincident choice consumes that unit once.
            if (selected == next_cursor)
                cursor = next_cursor;
            state.slice_cost +=
                try_start_job(scheduler, units[selected], player, tick, sight_grid, access);
        } else
            state.slice_cost = continue_job(scheduler, access);
        budget = wrap_sub(budget, state.slice_cost);
        state.credit[state.round_robin] =
            wrap_sub(state.credit[state.round_robin], state.slice_cost);
        ++steps;
    }
    return steps;
}

} // namespace oa::sim::ground_orders

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "computer_internal.hpp"

#include "oa/base/game_math.hpp"
#include "oa/data/match_rules/difficulty_names.hpp"
#include "oa/sim/unit_movement/movement.hpp"
#include "oa/sim/simulation_state.hpp"
#include "oa/sim/unit_health.hpp"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace oa::sim::ai {
namespace {

constexpr uint32_t max_tokens = 20;
constexpr uint32_t token_bytes = 0x7e;
constexpr int8_t base_weight_structure = 40;
constexpr int8_t base_weight_builder = 20;
constexpr uint8_t full_weight = 100;
constexpr int32_t placement_radius_step = 0xa0;
constexpr int32_t placement_attempts = 30;
constexpr int32_t mex_spot_slack = 0xa0;
constexpr uint32_t land_grid_margin = 3;
constexpr uint32_t water_grid_margin = 6;
constexpr float minimum_energy = 50.0F;
constexpr float minimum_metal = 25.0F;
constexpr int32_t energy_storage_cap = 1000;
constexpr int32_t metal_storage_cap = 500;

char lower(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equal_nocase(const char* a, const char* b) noexcept {
    for (; *a != '\0' && *b != '\0'; ++a, ++b)
        if (lower(*a) != lower(*b))
            return false;
    return *a == *b;
}

bool is_space(char c) noexcept {
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\v' || c == '\f';
}

char* duplicate(std::string_view text) noexcept {
    auto* copy = static_cast<char*>(std::malloc(text.size() + 1));
    if (copy != nullptr) {
        std::memcpy(copy, text.data(), text.size());
        copy[text.size()] = '\0';
    }
    return copy;
}

int32_t clamp_percent(int32_t value) noexcept {
    return value < 1 ? 0 : (value < 100 ? value : 100);
}

/// Line tokens: up to twenty whitespace-separated words; '#' ends the line.
struct TokenLine {
    char storage[token_bytes + max_tokens]{};
    const char* tokens[max_tokens]{};
    uint32_t count{};
};

void tokenize(TokenLine& line, const char* begin, const char* end) noexcept {
    line.count = 0;
    char* out = line.storage;
    char* const limit = line.storage + token_bytes;
    while (begin != end) {
        while (begin != end && is_space(*begin))
            ++begin;
        if (begin == end || *begin == '#')
            return;
        if (line.count < max_tokens)
            line.tokens[line.count++] = out;
        while (begin != end && !is_space(*begin) && *begin != '#' && out < limit)
            *out++ = *begin++;
        while (begin != end && !is_space(*begin) && *begin != '#')
            ++begin;
        *out++ = '\0';
        if (out >= line.storage + sizeof line.storage)
            return;
    }
}

const char* token(const TokenLine& line, uint32_t index) noexcept {
    return index < line.count ? line.tokens[index] : "";
}

/// Tests whether a type is named by a profile name.
///
/// @param type the type's computer-player fields
/// @param name unit name or FBI category; "all" matches every type
/// @param exact true compares the unit name only
/// @return true when the unit name matches (exact) or one of its FBI categories does
bool type_matches(const ComputerType& type, const char* name, bool exact) noexcept {
    if (exact)
        return equal_nocase(type.unit_name, name);
    if (equal_nocase(name, "all"))
        return true;
    const char* cursor = type.categories;
    while (*cursor != '\0') {
        while (*cursor == ' ')
            ++cursor;
        const char* start = cursor;
        while (*cursor != '\0' && *cursor != ' ')
            ++cursor;
        const auto length = static_cast<std::size_t>(cursor - start);
        if (length == 0)
            break;
        std::size_t i = 0;
        while (i < length && name[i] != '\0' && lower(start[i]) == lower(name[i]))
            ++i;
        if (i == length && name[i] == '\0')
            return true;
    }
    return false;
}

/// Tests whether a profile name is a unit name rather than a category.
///
/// A unit name resolves to that type and locks it; anything else is a category.
///
/// @param state computer players and their type table
/// @param name name from a weight or limit line
/// @return true when some type has that unit name (case-insensitive)
bool name_is_unit(const ComputerPlayers* state, const char* name) noexcept {
    for (uint32_t t = 1; t < state->type_count; ++t)
        if (equal_nocase(state->types[t].unit_name, name))
            return true;
    return false;
}

bool allocate_knowledge(ComputerKnowledge& k, uint32_t count) noexcept {
    k.owned_counts = static_cast<int16_t*>(std::calloc(count, sizeof(int16_t)));
    k.base_weights = static_cast<int8_t*>(std::calloc(count, sizeof(int8_t)));
    k.weight_percent = static_cast<uint8_t*>(std::calloc(count, sizeof(uint8_t)));
    k.weight_locked = static_cast<uint32_t*>(std::calloc(count, sizeof(uint32_t)));
    k.limits = static_cast<int32_t*>(std::calloc(count, sizeof(int32_t)));
    k.limit_locked = static_cast<uint32_t*>(std::calloc(count, sizeof(uint32_t)));
    return k.owned_counts && k.base_weights && k.weight_percent && k.weight_locked && k.limits &&
           k.limit_locked;
}

/// Frees a player's knowledge tables and metal spots and clears the record.
///
/// @param[in,out] k knowledge record
void release_knowledge(ComputerKnowledge& k) noexcept {
    std::free(k.owned_counts);
    std::free(k.base_weights);
    std::free(k.weight_percent);
    std::free(k.weight_locked);
    std::free(k.limits);
    std::free(k.limit_locked);
    std::free(k.metal_spots);
    k = {};
}

/// Resets a player's per-type tables to their defaults.
///
/// Weights become 100%, limits unlimited and unlocked, owned counts zero, and the base
/// weight 40 for structures plus 20 for build-capable types.
///
/// @param state computer players and their type table
/// @param[in,out] k knowledge record
void reset_tables(const ComputerPlayers* state, ComputerKnowledge& k) noexcept {
    for (uint32_t t = 0; t < state->type_count; ++t) {
        const auto& type = state->types[t];
        int8_t weight = 0;
        if (type.bm_code == 0)
            weight = static_cast<int8_t>(weight + base_weight_structure);
        if (type.has_build_list)
            weight = static_cast<int8_t>(weight + base_weight_builder);
        k.base_weights[t] = weight;
        k.owned_counts[t] = 0;
        k.weight_percent[t] = full_weight;
        k.weight_locked[t] = 0;
        k.limits[t] = unlimited;
        k.limit_locked[t] = 0;
    }
}

/// Seeds a player's randomised build grids.
///
/// Land step 11..20 x 11..13 with margin 3, water step 14..33 x 14..16 with margin 6;
/// phases are centred on the step.
///
/// @param host synced random stream; eight draws
/// @param[in,out] k knowledge record
void seed_grids(const ComputerHost& host, ComputerKnowledge& k) noexcept {
    const auto seed = [&host](PlacementGrid& grid, uint32_t margin, uint32_t x_limit) {
        const auto step = [&](uint32_t limit) {
            const auto roll = host.random(host.context, limit);
            return static_cast<int16_t>(
                static_cast<uint16_t>(roll + static_cast<uint16_t>(margin) + 8u)
            );
        };
        const auto phase = [&](int16_t span) {
            const auto roll = host.random(host.context, static_cast<uint32_t>(span));
            return static_cast<int16_t>(
                static_cast<uint16_t>(roll - static_cast<uint32_t>(span / 2))
            );
        };
        grid.margin = static_cast<int32_t>(margin);
        grid.step_x = step(x_limit);
        grid.step_z = step(3);
        grid.phase_x = phase(grid.step_x);
        grid.phase_z = phase(grid.step_z);
    };
    seed(k.land_grid, land_grid_margin, 10);
    seed(k.water_grid, water_grid_margin, 0x14);
}

/// Builds a player's knowledge record.
///
/// Seeds its build grids, sizes its tables to the unit types and resets them.
///
/// @param state computer players and their type table
/// @param host synced random stream
/// @param[in,out] k knowledge record
/// @return false when out of memory; the record is then released
bool create_knowledge(
    const ComputerPlayers* state, const ComputerHost& host, ComputerKnowledge& k
) noexcept {
    seed_grids(host, k);
    if (!allocate_knowledge(k, state->type_count)) {
        release_knowledge(k);
        return false;
    }
    reset_tables(state, k);
    return true;
}

/// Lists the cells of indestructible metal-bearing features, row by row.
///
/// @param host map size and metal-feature query
/// @param[in,out] k knowledge record; its metal spot list is replaced
void scan_metal_spots(const ComputerHost& host, ComputerKnowledge& k) noexcept {
    std::free(k.metal_spots);
    k.metal_spots = nullptr;
    k.metal_spot_count = 0;
    if (host.metal_feature == nullptr || host.map_cells_x <= 0 || host.map_cells_z <= 0)
        return;
    uint32_t capacity = 0;
    for (int32_t z = 0; z < host.map_cells_z; ++z) {
        for (int32_t x = 0; x < host.map_cells_x; ++x) {
            if (!host.metal_feature(host.context, x, z))
                continue;
            if (k.metal_spot_count == capacity) {
                const auto grown = capacity == 0 ? 64u : capacity * 2u;
                auto* spots =
                    static_cast<MetalSpot*>(std::realloc(k.metal_spots, grown * sizeof(MetalSpot)));
                if (spots == nullptr)
                    return;
                k.metal_spots = spots;
                capacity = grown;
            }
            k.metal_spots[k.metal_spot_count++] = {
                static_cast<int16_t>(x), static_cast<int16_t>(z)
            };
        }
    }
}

/// Applies every "plan", "weight" and "limit" line of an AI script.
///
/// A plan line enables the following weight and limit lines when it names the game's
/// difficulty (by the keyword ai.difficulty-names gives it) or "any"; weight lines apply to every controller, limit lines to every
/// computer player.
///
/// @param state computer players
/// @param host difficulty and players
/// @param text script text
/// @param length bytes of `text`
/// @param[in,out] plan_matches whether the last plan line matched
/// @param only_player a player index, or -1 for every controller
/// @param difficulty a difficulty to match plan lines against, or -1 for the game's
/// @quirk "any" is only recognised as the first word after "plan", tested once per word.
void apply_script(
    ComputerPlayers* state,
    const ComputerHost& host,
    const char* text,
    std::size_t length,
    bool& plan_matches,
    int32_t only_player = -1,
    int32_t difficulty = -1
) noexcept {
    // The keyword the difficulty carries (ai.difficulty-names).
    const auto named = data::match_rules::difficulty_name_index(
        state->rules.rules().ai.difficulty_names, difficulty >= 0 ? difficulty : host.difficulty
    );
    const char* cursor = text;
    const char* const end = text + length;
    while (cursor < end) {
        const char* line_end = cursor;
        while (line_end < end && *line_end != '\n')
            ++line_end;
        TokenLine line{};
        tokenize(line, cursor, line_end);
        cursor = line_end + 1;
        if (line.count == 0)
            continue;
        const char* command = token(line, 0);
        if (equal_nocase(command, "plan")) {
            // Token 1 is re-tested for "any" on every pass.
            plan_matches = false;
            for (uint32_t i = 1; i < line.count; ++i) {
                if (equal_nocase(token(line, 1), "any"))
                    plan_matches = true;
                if (named == OA_DIFFICULTY_EASY && equal_nocase(token(line, i), "easy"))
                    plan_matches = true;
                if (named == OA_DIFFICULTY_MEDIUM && equal_nocase(token(line, i), "medium"))
                    plan_matches = true;
                if (named == OA_DIFFICULTY_HARD && equal_nocase(token(line, i), "hard"))
                    plan_matches = true;
            }
        } else if (equal_nocase(command, "weight")) {
            if (!plan_matches)
                continue;
            const auto percent = static_cast<float>(std::strtod(token(line, 2), nullptr));
            for (uint8_t p = 0; p < OA_PLAYER_COUNT; ++p) {
                if (only_player >= 0 && static_cast<int32_t>(p) != only_player)
                    continue;
                if (state->players[p].present)
                    computer_apply_weight(state, p, token(line, 1), percent);
            }
        } else if (equal_nocase(command, "limit")) {
            if (!plan_matches)
                continue;
            const auto limit = static_cast<int32_t>(std::strtol(token(line, 2), nullptr, 10));
            for (uint8_t p = 0; p < OA_PLAYER_COUNT; ++p) {
                if (only_player >= 0 && static_cast<int32_t>(p) != only_player)
                    continue;
                const auto& player = host.world->game.players[p];
                if (state->players[p].present && player.in_use != 0 &&
                    player.status == OA_PLAYER_STATUS_COMPUTER)
                    computer_apply_limit(state, p, token(line, 1), limit);
            }
        }
    }
}

/// Loads every builder type's list from the unit types' build lists (UnitDef.build_ids).
///
/// A computer player's builder chooses from its type's whole build list: its SIDEDATA
/// CANBUILD entries, then the entries the download menus add for it, as the game holds
/// them. Every builder type gets a list, possibly empty, that takes its type's ids in
/// order, repeats included, up to data::limits::build_list_kept(state->build_lists) + 1
/// entries (31 in 3.1c); an id outside the type table is skipped, and a type the runs
/// do not reach keeps an empty list.
///
/// @param[in,out] state type table, build-list limits and build_list_runs; its list block
///     is allocated anew, and a block that cannot be allocated leaves every list empty
void load_build_lists(ComputerPlayers* state) noexcept {
    const uint32_t capacity = data::limits::build_list_kept(state->build_lists) + 1;
    std::free(state->build_id_block);
    state->build_id_block = static_cast<uint16_t*>(
        std::calloc(std::size_t{state->type_count} * capacity, sizeof(uint16_t))
    );
    for (uint32_t t = 1; t < state->type_count; ++t) {
        auto& type = state->types[t];
        type.build_count = 0;
        type.build_ids = state->build_id_block != nullptr
                             ? state->build_id_block + std::size_t{t} * capacity
                             : nullptr;
        type.has_build_list = (type.flags & OA_UNIT_DEF_FLAG_BUILDER) != 0 ? 1 : 0;
    }
    if (state->build_id_block == nullptr || state->build_list_runs == nullptr)
        return;
    const uint16_t* run = state->build_list_runs;
    const uint16_t* const end = run + state->build_list_run_length;
    for (uint32_t t = 0; t < state->type_count && run < end; ++t) {
        const uint32_t count = *run++;
        const auto present = static_cast<uint32_t>(end - run);
        const uint32_t length = count < present ? count : present;
        auto& type = state->types[t];
        if (t != 0 && type.has_build_list)
            for (uint32_t i = 0; i < length && type.build_count < capacity; ++i)
                if (run[i] != 0 && run[i] < state->type_count)
                    type.build_ids[type.build_count++] = run[i];
        run += length;
    }
}

/// Returns a point, or the point a distance along the way to it when it lies farther.
///
/// @param from start point, 16.16 world coordinates
/// @param to target point, 16.16 world coordinates
/// @param reach largest distance, 16.16 world units
/// @return `to`, or the point `reach` from `from` toward it
oa::FixedVec3
clamp_toward(const oa::FixedVec3& from, const oa::FixedVec3& to, int32_t reach) noexcept {
    int32_t toward[3] = {
        static_cast<int32_t>(static_cast<uint32_t>(to.x) - static_cast<uint32_t>(from.x)),
        static_cast<int32_t>(static_cast<uint32_t>(to.y) - static_cast<uint32_t>(from.y)),
        static_cast<int32_t>(static_cast<uint32_t>(to.z) - static_cast<uint32_t>(from.z)),
    };
    const auto length = base::game_math::truncated_length(toward[0], toward[1], toward[2]);
    if (!(reach < length))
        return to;
    const auto factor =
        static_cast<int32_t>((static_cast<int64_t>(reach) << 16) / static_cast<int64_t>(length));
    base::game_math::scale_vector_fixed(toward, toward, factor);
    return {
        static_cast<int32_t>(static_cast<uint32_t>(from.x) + static_cast<uint32_t>(toward[0])),
        static_cast<int32_t>(static_cast<uint32_t>(from.y) + static_cast<uint32_t>(toward[1])),
        static_cast<int32_t>(static_cast<uint32_t>(from.z) + static_cast<uint32_t>(toward[2]))
    };
}

/// Chooses the metal spot for an extractor with the most metal under its footprint.
///
/// Spots within the radius are taken nearest first; once a buildable one is found,
/// spots more than 160 squared cells farther are no longer tried.
///
/// @param host site and metal queries
/// @param k knowledge record with its metal spots
/// @param type_id unit type to place
/// @param type its computer-player fields (footprint)
/// @param center search centre, 16.16 world coordinates
/// @param radius search radius, cells
/// @param[out] out receives the footprint's top-left cell
/// @return false when no buildable spot has metal
bool place_on_metal(
    const ComputerHost& host,
    const ComputerKnowledge& k,
    uint16_t type_id,
    const ComputerType& type,
    const oa::FixedVec3& center,
    int32_t radius,
    int16_t out[2]
) noexcept {
    if (k.metal_spot_count == 0)
        return false;
    const auto center_x = static_cast<int16_t>(
        static_cast<int32_t>(
            static_cast<uint32_t>(center.x) +
            static_cast<uint32_t>(type.footprint_x) * 0xfff80000u + 0x80000u
        ) >>
        20
    );
    const auto center_z = static_cast<int16_t>(
        static_cast<int32_t>(
            static_cast<uint32_t>(center.z) +
            static_cast<uint32_t>(type.footprint_z) * 0xfff80000u + 0x80000u
        ) >>
        20
    );
    const auto reach =
        static_cast<int32_t>(static_cast<uint32_t>(radius) * static_cast<uint32_t>(radius));
    auto* heap = static_cast<base::game_math::FloatHeapPair*>(
        std::malloc(sizeof(base::game_math::FloatHeapPair) * k.metal_spot_count)
    );
    if (heap == nullptr)
        return false;
    int32_t size = 0;
    for (uint32_t i = 0; i < k.metal_spot_count; ++i) {
        const auto& spot = k.metal_spots[i];
        const auto dz = static_cast<int32_t>(spot.cell_z) - center_z;
        const auto dx = static_cast<int32_t>(spot.cell_x) - center_x;
        const auto distance = dx * dx + dz * dz;
        if (distance <= reach)
            heap[size++] = {
                static_cast<uint32_t>(static_cast<uint16_t>(spot.cell_x)) |
                    (static_cast<uint32_t>(static_cast<uint16_t>(spot.cell_z)) << 16),
                static_cast<float>(-distance)
            };
    }
    if (size >= 2)
        base::game_math::float_heap_make(heap, size);
    int32_t best_metal = 0;
    int32_t first_distance = -1;
    int16_t best[2]{};
    while (size > 0) {
        const auto root = heap[0].value;
        auto cell_x = static_cast<int16_t>(root & 0xffffu);
        auto cell_z = static_cast<int16_t>(root >> 16);
        cell_x = static_cast<int16_t>(cell_x + (type.footprint_x - 3) / -2);
        cell_z = static_cast<int16_t>(cell_z + (type.footprint_z - 3) / -2);
        const auto dz = static_cast<int32_t>(cell_z) - center_z;
        const auto dx = static_cast<int32_t>(cell_x) - center_x;
        const auto distance = dx * dx + dz * dz;
        if (first_distance >= 0 && first_distance + mex_spot_slack < distance)
            break;
        if (host.building_site(host.context, type_id, cell_x, cell_z)) {
            int32_t metal = 0;
            for (int32_t z = 0; z < type.footprint_z; ++z)
                for (int32_t x = 0; x < type.footprint_x; ++x)
                    metal += host.cell_metal(host.context, cell_x + x, cell_z + z);
            if (best_metal < metal) {
                best[0] = cell_x;
                best[1] = cell_z;
                best_metal = metal;
                if (first_distance == -1)
                    first_distance = distance;
            }
        }
        base::game_math::float_heap_pop(heap, size);
        --size;
    }
    std::free(heap);
    if (best_metal == 0)
        return false;
    out[0] = best[0];
    out[1] = best[1];
    return true;
}

/// Chooses a random clear site on the player's build grid near a centre.
///
/// Up to 30 attempts at a random distance and angle within the radius, snapped to the
/// land or water grid and jittered; a site over more than twice the map's surface metal
/// per footprint cell is refused.
///
/// @param host random stream, site and metal queries
/// @param k knowledge record with its grids
/// @param type_id unit type to place
/// @param type its computer-player fields (footprint, water depth)
/// @param center search centre, 16.16 world coordinates
/// @param radius search radius, world units
/// @param[out] out receives the footprint's top-left cell
/// @return false when no attempt succeeded
bool place_on_grid(
    const ComputerHost& host,
    const ComputerKnowledge& k,
    uint16_t type_id,
    const ComputerType& type,
    const oa::FixedVec3& center,
    int32_t radius,
    int16_t out[2]
) noexcept {
    const auto& grid = type.min_water_depth >= 0 ? k.water_grid : k.land_grid;
    const auto fx = static_cast<int32_t>(type.footprint_x);
    const auto fz = static_cast<int32_t>(type.footprint_z);
    const auto metal_ceiling = host.surface_metal * fx * fz * 2;
    for (int32_t attempt = 0; attempt < placement_attempts; ++attempt) {
        const auto distance = host.random(host.context, static_cast<uint32_t>(radius));
        const auto angle = static_cast<uint16_t>(host.random(host.context, 0x10000));
        const auto magnitude = static_cast<int32_t>(distance << 16);
        const auto sx = sim::unit_movement::sine_scaled(angle, magnitude);
        const auto cz = sim::unit_movement::cosine_scaled(angle, magnitude);
        const auto jitter_x =
            host.random(host.context, static_cast<uint32_t>(grid.step_x - fx - grid.margin));
        const auto jitter_z =
            host.random(host.context, static_cast<uint32_t>(grid.step_z - fz - grid.margin));
        const auto cell = [](int32_t position,
                             int32_t offset,
                             int32_t footprint,
                             int16_t step,
                             int16_t phase,
                             uint32_t jitter) {
            const auto raw = static_cast<int16_t>(
                static_cast<int32_t>(
                    static_cast<uint32_t>(position) - static_cast<uint32_t>(offset) +
                    static_cast<uint32_t>(footprint) * 0xfff80000u + 0x80000u
                ) >>
                20
            );
            const auto snapped = step != 0 ? (raw / step) * step : 0;
            return static_cast<int16_t>(snapped + phase + static_cast<int16_t>(jitter));
        };
        const auto cell_x = cell(center.x, sx, fx, grid.step_x, grid.phase_x, jitter_x);
        const auto cell_z = cell(center.z, cz, fz, grid.step_z, grid.phase_z, jitter_z);
        if (!host.site_clear(host.context, type_id, cell_x, cell_z))
            continue;
        int32_t metal = 0;
        for (int32_t z = 0; z < fz; ++z)
            for (int32_t x = 0; x < fx; ++x)
                metal += host.cell_metal(host.context, cell_x + x, cell_z + z);
        if (metal <= metal_ceiling) {
            out[0] = cell_x;
            out[1] = cell_z;
            return true;
        }
    }
    return false;
}

/// Applies each downloadable type's own directives (FBI ai_weight) to every controller.
///
/// Sets the plan flag first, and skips types `locks` marks as set by exact name.
///
/// @param state computer players and their type table
/// @param host difficulty and players
/// @param locks the player's weight or limit lock table
/// @param plan_slot plan flag to use; null uses and sets the shared one
/// @param only_player a player index, or -1 for every controller
/// @param difficulty a difficulty to match, or -1 for the game's
void apply_downloadable_directives(
    ComputerPlayers* state,
    const ComputerHost& host,
    const uint32_t* locks,
    bool* plan_slot = nullptr,
    int32_t only_player = -1,
    int32_t difficulty = -1
) noexcept {
    bool& plan = plan_slot != nullptr ? *plan_slot : state->plan_matches;
    plan = true;
    for (uint32_t t = 1; t < state->type_count; ++t) {
        const auto& type = state->types[t];
        if ((type.flags & OA_UNIT_DEF_FLAG_DOWNLOADABLE) == 0 || locks[t] == 1)
            continue;
        const auto length = std::strlen(type.ai_directives);
        if (length != 0)
            apply_script(state, host, type.ai_directives, length, plan, only_player, difficulty);
    }
}

/// Applies the downloadable types' own directives for the types whose weight the player has not locked.
///
/// @param state computer players and their type table
/// @param host difficulty and players
/// @param player player whose weight locks decide
void apply_downloadable_weights(
    ComputerPlayers* state, const ComputerHost& host, uint8_t player
) noexcept {
    apply_downloadable_directives(state, host, state->players[player].knowledge.weight_locked);
}

/// Applies the downloadable types' own directives for the types whose limit the player has not locked.
///
/// @param state computer players and their type table
/// @param host difficulty and players
/// @param player player whose limit locks decide
void apply_downloadable_limits(
    ComputerPlayers* state, const ComputerHost& host, uint8_t player
) noexcept {
    apply_downloadable_directives(state, host, state->players[player].knowledge.limit_locked);
}

/// Re-applies the profile to a computer player whose chosen difficulty is not the game's.
///
/// A player with none chosen, or the same as the game, is left as the pass just
/// applied it. One that differs has its tables reset and the profile and the
/// downloadable directives applied again, for that player only, at that
/// difficulty. Its profile pass starts from the plan flag the shared pass started
/// from. The downloadable passes repeat once per computer player, as the shared pass
/// compounds them, and use their own plan flag.
///
/// @param state computer players and their type table
/// @param host difficulty and players
/// @param starting_plan the plan flag the shared profile pass started from
void reapply_noted_difficulties(
    ComputerPlayers* state, const ComputerHost& host, bool starting_plan
) noexcept {
    if (state == nullptr || host.world == nullptr)
        return;
    int32_t present_count = 0;
    for (uint8_t index = 0; index < OA_PLAYER_COUNT; ++index) {
        const auto& player = host.world->game.players[index];
        if (state->players[index].present && player.in_use != 0 &&
            player.status == OA_PLAYER_STATUS_COMPUTER)
            ++present_count;
    }
    const auto saved_plan = state->plan_matches;
    for (uint8_t index = 0; index < OA_PLAYER_COUNT; ++index) {
        if (!state->difficulty_set[index] || !state->players[index].present)
            continue;
        const auto noted = static_cast<int32_t>(state->difficulty[index]);
        if (noted == host.difficulty)
            continue;
        reset_tables(state, state->players[index].knowledge);
        ComputerHost noted_host = host;
        noted_host.difficulty = noted;
        bool plan = starting_plan;
        if (state->profile_text != nullptr)
            apply_script(
                state,
                noted_host,
                state->profile_text,
                state->profile_length,
                plan,
                static_cast<int32_t>(index),
                noted
            );
        for (int32_t pass = 0; pass < present_count; ++pass) {
            bool weight_plan = false;
            apply_downloadable_directives(
                state,
                noted_host,
                state->players[index].knowledge.weight_locked,
                &weight_plan,
                static_cast<int32_t>(index),
                noted
            );
            bool limit_plan = false;
            apply_downloadable_directives(
                state,
                noted_host,
                state->players[index].knowledge.limit_locked,
                &limit_plan,
                static_cast<int32_t>(index),
                noted
            );
        }
    }
    state->plan_matches = saved_plan;
}

/// Applies the stored profile, then each computer player's passes over the downloadable types' directives.
///
/// All passes share the plan flag, which the profile pass takes as the last pass left
/// it: matched before the first pass, so the lines before the profile's first plan line
/// apply. Each computer player re-applies the directives twice, once per lock table, to
/// every controller, so weights compound per computer player. A computer player with its
/// own difficulty then has that pass again.
///
/// @param state computer players and their type table
/// @param host difficulty and players
void apply_profile(ComputerPlayers* state, const ComputerHost& host) noexcept {
    const bool starting_plan = state->plan_matches;
    if (state->profile_text != nullptr)
        apply_script(state, host, state->profile_text, state->profile_length, state->plan_matches);
    // Downloadable types carry their own directives (FBI ai_weight). Each
    // computer player re-applies them twice, once per lock table, to every
    // controller, so weights compound per computer player.
    for (uint8_t index = 0; index < OA_PLAYER_COUNT; ++index) {
        if (!state->players[index].present)
            continue;
        apply_downloadable_weights(state, host, index);
        apply_downloadable_limits(state, host, index);
    }
    reapply_noted_difficulties(state, host, starting_plan);
}

/// Gives one computer player the controller the game start gives its computers.
///
/// The profile and the downloadable directives are applied while every other
/// controller is hidden, so their weights are not scaled a second time.
///
/// @param[in,out] state computer players; types and profile already loaded
/// @param host world, map and random stream
/// @param player player index, already a computer
void adopt_handed_player(
    ComputerPlayers* state, const ComputerHost& host, uint8_t player
) noexcept {
    auto& ai = state->players[player];
    release_knowledge(ai.knowledge);
    ai = {};
    if (const auto note = host.note_section)
        note(host.context, player, section_start);
    computer_player_create(ai, player, host.world->game, state->rules.rules().ai);
    if (!create_knowledge(state, host, ai.knowledge)) {
        ai.present = 0;
        return;
    }
    scan_metal_spots(host, ai.knowledge);
    uint8_t saved_present[OA_PLAYER_COUNT];
    for (uint8_t index = 0; index < OA_PLAYER_COUNT; ++index) {
        saved_present[index] = state->players[index].present;
        if (index != player)
            state->players[index].present = 0;
    }
    const bool saved_plan = state->plan_matches;
    // The profile and the downloadable directives name this player only, so a
    // computer seated beside them keeps the weights the start gave it.
    const auto only = static_cast<int32_t>(player);
    if (state->profile_text != nullptr)
        apply_script(
            state, host, state->profile_text, state->profile_length, state->plan_matches, only
        );
    apply_downloadable_directives(
        state, host, state->players[player].knowledge.weight_locked, nullptr, only
    );
    apply_downloadable_directives(
        state, host, state->players[player].knowledge.limit_locked, nullptr, only
    );
    for (uint8_t index = 0; index < OA_PLAYER_COUNT; ++index)
        if (index != player)
            state->players[index].present = saved_present[index];
    state->plan_matches = saved_plan;
}

} // namespace

void computer_profile_lines(
    std::string_view text, void (*visit)(void* context, const ProfileLine& line), void* context
) noexcept {
    if (visit == nullptr || text.data() == nullptr || text.empty())
        return;
    const char* cursor = text.data();
    const char* const end = text.data() + text.size();
    uint32_t number = 1;
    for (;;) {
        const char* line_end = cursor;
        while (line_end < end && *line_end != '\n')
            ++line_end;
        TokenLine tokens;
        tokenize(tokens, cursor, line_end);
        if (tokens.count > 0) {
            ProfileLine line;
            line.number = number;
            line.count = tokens.count;
            line.words = tokens.tokens;
            visit(context, line);
        }
        if (line_end == end)
            break;
        cursor = line_end + 1;
        ++number;
    }
}

int32_t truncate_to_int32(double value) noexcept {
    if (!std::isfinite(value) || value >= 2147483648.0 || value < -2147483648.0)
        return static_cast<int32_t>(0x80000000u);
    return static_cast<int32_t>(value);
}

const ComputerType* computer_type(const ComputerPlayers* state, uint16_t type) noexcept {
    if (state == nullptr || type == 0 || type >= state->type_count)
        return nullptr;
    return &state->types[type];
}

bool computer_players_configure(
    ComputerPlayers* state, std::string_view profile, std::span<const uint16_t> build_lists
) noexcept {
    if (state == nullptr)
        return false;
    std::free(state->profile_text);
    std::free(state->build_list_runs);
    state->profile_text = duplicate(profile);
    state->profile_length = static_cast<uint32_t>(profile.size());
    state->build_list_runs = static_cast<uint16_t*>(
        std::malloc(build_lists.empty() ? sizeof(uint16_t) : build_lists.size_bytes())
    );
    if (state->build_list_runs != nullptr && !build_lists.empty())
        std::memcpy(state->build_list_runs, build_lists.data(), build_lists.size_bytes());
    state->build_list_run_length =
        state->build_list_runs != nullptr ? static_cast<uint32_t>(build_lists.size()) : 0;
    state->initialized = 0;
    return state->profile_text != nullptr && state->build_list_runs != nullptr;
}

bool computer_players_reserve_types(ComputerPlayers* state, uint32_t type_count) noexcept {
    if (state == nullptr)
        return false;
    std::free(state->types);
    state->types = static_cast<ComputerType*>(std::calloc(type_count, sizeof(ComputerType)));
    state->type_count = state->types != nullptr ? type_count : 0;
    return state->types != nullptr;
}

void computer_players_release(ComputerPlayers* state) noexcept {
    if (state == nullptr)
        return;
    for (auto& ai : state->players)
        release_knowledge(ai.knowledge);
    std::free(state->types);
    std::free(state->build_id_block);
    std::free(state->profile_text);
    std::free(state->build_list_runs);
    *state = {};
}

void computer_players_set_difficulty(
    ComputerPlayers* state, uint8_t player, int32_t difficulty
) noexcept {
    if (state == nullptr || player >= OA_PLAYER_COUNT)
        return;
    if (difficulty < OA_DIFFICULTY_EASY || difficulty > OA_DIFFICULTY_HARD)
        return;
    state->difficulty_set[player] = true;
    state->difficulty[player] = static_cast<uint8_t>(difficulty);
}

int32_t computer_player_difficulty(
    const ComputerPlayers* state, uint8_t player, int32_t fallback
) noexcept {
    if (state == nullptr || player >= OA_PLAYER_COUNT || !state->difficulty_set[player])
        return fallback;
    const auto noted = static_cast<int32_t>(state->difficulty[player]);
    if (noted < OA_DIFFICULTY_EASY || noted > OA_DIFFICULTY_HARD)
        return fallback;
    return noted;
}

bool computer_players_initialize(ComputerPlayers* state, const ComputerHost& host) noexcept {
    if (state == nullptr || host.world == nullptr || state->types == nullptr)
        return false;
    load_build_lists(state);
    for (uint8_t index = 0; index < OA_PLAYER_COUNT; ++index) {
        auto& ai = state->players[index];
        release_knowledge(ai.knowledge);
        ai = {};
        const auto& player = host.world->game.players[index];
        if (player.in_use == 0 || player.status != OA_PLAYER_STATUS_COMPUTER)
            continue;
        if (const auto note = host.note_section)
            note(host.context, index, section_start);
        computer_player_create(ai, index, host.world->game, state->rules.rules().ai);
        if (!create_knowledge(state, host, ai.knowledge))
            ai.present = 0;
    }
    apply_profile(state, host);
    for (auto& ai : state->players)
        if (ai.present)
            scan_metal_spots(host, ai.knowledge);
    state->initialized = 1;
    return true;
}

void computer_players_adopt(
    ComputerPlayers* state, const ComputerHost& host, uint8_t player
) noexcept {
    if (state == nullptr || host.world == nullptr || !state->initialized ||
        player >= OA_PLAYER_COUNT || state->players[player].present != 0)
        return;
    const auto& record = host.world->game.players[player];
    if (record.in_use == 0 || record.status != OA_PLAYER_STATUS_COMPUTER)
        return;
    adopt_handed_player(state, host, player);
}

bool computer_players_reload_profile(
    ComputerPlayers* state, const ComputerHost& host, std::string_view profile
) noexcept {
    if (state == nullptr || host.world == nullptr)
        return false;
    std::free(state->profile_text);
    state->profile_text = duplicate(profile);
    state->profile_length = static_cast<uint32_t>(profile.size());
    if (state->profile_text == nullptr)
        return false;
    if (!state->initialized)
        return true;
    for (uint8_t index = 0; index < OA_PLAYER_COUNT; ++index) {
        const auto& player = host.world->game.players[index];
        if (player.in_use != 0 && player.status == OA_PLAYER_STATUS_COMPUTER &&
            state->players[index].present)
            reset_tables(state, state->players[index].knowledge);
    }
    apply_profile(state, host);
    return true;
}

void computer_profile_apply(
    ComputerPlayers* state, const ComputerHost& host, std::string_view text
) noexcept {
    if (state == nullptr || host.world == nullptr)
        return;
    bool plan_matches = true;
    apply_script(state, host, text.data(), text.size(), plan_matches);
}

void computer_apply_weight(
    ComputerPlayers* state, uint8_t player, const char* name, float percent
) noexcept {
    if (state == nullptr || player >= OA_PLAYER_COUNT || !state->players[player].present)
        return;
    auto& k = state->players[player].knowledge;
    const bool exact = name_is_unit(state, name);
    for (uint32_t t = 1; t < state->type_count; ++t) {
        if (!type_matches(state->types[t], name, exact) || k.weight_locked[t] != 0)
            continue;
        const auto scaled = truncate_to_int32(static_cast<double>(k.weight_percent[t]) * percent);
        k.weight_percent[t] = static_cast<uint8_t>(clamp_percent(scaled));
        if (exact)
            k.weight_locked[t] = 1;
    }
}

void computer_apply_limit(
    ComputerPlayers* state, uint8_t player, const char* name, int32_t limit
) noexcept {
    if (state == nullptr || player >= OA_PLAYER_COUNT || !state->players[player].present)
        return;
    auto& k = state->players[player].knowledge;
    const bool exact = name_is_unit(state, name);
    for (uint32_t t = 1; t < state->type_count; ++t) {
        if (!type_matches(state->types[t], name, exact) || k.limit_locked[t] != 0)
            continue;
        k.limits[t] = limit;
        if (exact)
            k.limit_locked[t] = 1;
    }
}

ComputerKnowledge* computer_player_knowledge(ComputerPlayers* state, uint8_t player) noexcept {
    if (state == nullptr || player >= OA_PLAYER_COUNT || !state->players[player].present)
        return nullptr;
    return &state->players[player].knowledge;
}

int32_t computer_sighted_weight(
    const ComputerPlayers* state,
    const ComputerKnowledge& k,
    const sim::detection::Sightings& sightings,
    const oa::World& world,
    const oa::FixedVec3& at,
    int32_t radius
) noexcept {
    const auto reach =
        static_cast<int32_t>(static_cast<uint32_t>(radius) * static_cast<uint32_t>(radius));
    uint32_t sum = 0;
    if (state == nullptr || k.base_weights == nullptr || sightings.seen == nullptr)
        return 0;
    for (uint32_t i = 0; i < sightings.seen_count; ++i) {
        const auto* unit = oa::world_unit_at(&world, sightings.seen[i]);
        if (unit == nullptr || sim::detection::squared_distance_high(at, unit->position) > reach)
            continue;
        if (unit->type_index < state->type_count)
            sum += static_cast<uint32_t>(static_cast<int32_t>(k.base_weights[unit->type_index]));
    }
    return static_cast<int32_t>(sum);
}

void computer_knowledge_clear(const ComputerPlayers* state, ComputerKnowledge& k) noexcept {
    k.builder_count = 0;
    for (uint32_t t = 0; t < state->type_count; ++t)
        k.owned_counts[t] = 0;
}

void computer_knowledge_count(
    const ComputerPlayers* state, ComputerKnowledge& k, const oa::Unit& unit, KnowledgeTally& tally
) noexcept {
    const auto* type = computer_type(state, unit.type_index);
    if (type == nullptr)
        return;
    ++k.owned_counts[unit.type_index];
    if (type->has_build_list)
        ++k.builder_count;
    constexpr float fraction = 1.52587890625e-05F;
    const auto weight = static_cast<double>(k.base_weights[unit.type_index]);
    tally.sum_x =
        static_cast<float>(tally.sum_x + static_cast<double>(unit.position.x) * fraction * weight);
    tally.sum_y =
        static_cast<float>(tally.sum_y + static_cast<double>(unit.position.y) * fraction * weight);
    tally.sum_z =
        static_cast<float>(tally.sum_z + static_cast<double>(unit.position.z) * fraction * weight);
    tally.weights = static_cast<float>(tally.weights + weight);
}

void computer_knowledge_settle(ComputerKnowledge& k, const KnowledgeTally& tally) noexcept {
    auto x = tally.sum_x, y = tally.sum_y, z = tally.sum_z;
    if (tally.weights != 0.0F) {
        x /= tally.weights;
        y /= tally.weights;
        z /= tally.weights;
    }
    k.base_position = {
        truncate_to_int32(static_cast<double>(x) * 65536.0),
        truncate_to_int32(static_cast<double>(y) * 65536.0),
        truncate_to_int32(static_cast<double>(z) * 65536.0)
    };
}

int32_t computer_build_score(
    const ComputerPlayers* state, const ComputerHost& host, uint8_t player, uint16_t type
) noexcept {
    if (state == nullptr || player >= OA_PLAYER_COUNT || !state->players[player].present)
        return 0;
    const auto* info = computer_type(state, type);
    if (info == nullptr)
        return 0;
    const auto& record = host.world->game.players[player];
    const auto& k = state->players[player].knowledge;
    if (record.energy < minimum_energy || record.metal < minimum_metal)
        return 0;
    if (state->downloadables_restricted && (info->flags & OA_UNIT_DEF_FLAG_DOWNLOADABLE) != 0)
        return 0;
    auto energy_cap = truncate_to_int32(record.energy_storage);
    if (energy_cap > energy_storage_cap)
        energy_cap = energy_storage_cap;
    auto metal_cap = truncate_to_int32(record.metal_storage);
    if (metal_cap > metal_storage_cap)
        metal_cap = metal_storage_cap;
    auto energy_need = truncate_to_int32(
        std::fmax(0.0, (static_cast<double>(energy_cap) - record.energy) * 0.125)
    );
    auto metal_need =
        truncate_to_int32(std::fmax(0.0, (static_cast<double>(metal_cap) - record.metal) * 0.25));
    if (oa::player_energy_surplus(&record) < 1.0)
        energy_need += 20;
    if (oa::player_metal_surplus(&record) < 1.0)
        metal_need += 20;
    if (oa::player_energy_produced(&record) < 50.0F)
        energy_need += 100;
    else if (oa::player_energy_produced(&record) < 200.0F)
        energy_need += 10;
    if (oa::player_metal_produced(&record) < 3.0F)
        metal_need += 100;
    else if (oa::player_metal_produced(&record) < 5.0F)
        metal_need += 20;
    const auto metal_share = clamp_percent(metal_need);
    const auto energy_share = clamp_percent(energy_need - metal_share);
    auto base_share = 100 - metal_share - energy_share;
    if (base_share < 1)
        base_share = 0;
    const int32_t* limit_rows[OA_PLAYER_COUNT]{};
    for (uint32_t index = 0; index < OA_PLAYER_COUNT; ++index)
        limit_rows[index] = state->players[index].knowledge.limits;
    if (!sim::unit_health::within_unit_limit_row(
            player, type, static_cast<int32_t>(state->type_count), limit_rows, k.owned_counts[type]
        ))
        return 0;
    const auto* strength = host.strengths(host.context, player, type);
    if (strength == nullptr)
        return 0;
    const auto economy = static_cast<int32_t>(static_cast<int8_t>(strength[1]));
    const auto attack = static_cast<int32_t>(static_cast<int8_t>(strength[2]));
    const auto priority = static_cast<int32_t>(static_cast<int8_t>(strength[0]));
    return (economy * metal_share + attack * energy_share + priority * base_share) *
           static_cast<int32_t>(k.weight_percent[type]) / 10000;
}

uint16_t computer_pick_build(
    const ComputerPlayers* state, const ComputerHost& host, uint8_t player, uint16_t builder
) noexcept {
    const auto* unit = oa::world_unit_at(host.world, builder);
    const auto* type = unit != nullptr ? computer_type(state, unit->type_index) : nullptr;
    if (type == nullptr)
        return 0;
    uint16_t pick = 0;
    uint32_t total = 0;
    for (uint32_t i = 0; i < type->build_count; ++i) {
        const auto candidate = type->build_ids[i];
        const auto score = computer_build_score(state, host, player, candidate);
        if (score <= 0)
            continue;
        total += static_cast<uint32_t>(score);
        if (static_cast<int32_t>(host.random(host.context, total)) < score)
            pick = candidate;
    }
    if (pick == 0)
        return 0;
    const auto* chosen = computer_type(state, pick);
    if (chosen == nullptr || std::strcmp(type->side, chosen->side) != 0)
        return 0;
    return pick;
}

bool computer_place_build(
    const ComputerPlayers* state,
    const ComputerHost& host,
    ComputerPlayer& ai,
    const oa::FixedVec3& builder,
    uint16_t type_id,
    oa::FixedVec3* site
) noexcept {
    const auto* type = computer_type(state, type_id);
    if (type == nullptr)
        return false;
    auto& k = ai.knowledge;
    const auto world_x = host.map_cells_x << 4;
    const auto world_z = host.map_cells_z << 4;
    const auto widest = world_x > world_z ? world_x : world_z;
    if (k.placement_radius < widest)
        k.placement_radius += placement_radius_step;
    // Search centre: the base, or the point that far from the builder toward it.
    const auto center = clamp_toward(
        builder,
        k.base_position,
        static_cast<int32_t>(static_cast<uint32_t>(k.placement_radius) << 16)
    );
    int16_t cell[2]{};
    bool placed = false;
    if (type->extracts_metal != 0.0F &&
        host.surface_metal < static_cast<int32_t>(host.random(host.context, 0xff)))
        placed = place_on_metal(host, k, type_id, *type, center, k.placement_radius << 2, cell);
    else
        placed = place_on_grid(host, k, type_id, *type, center, k.placement_radius, cell);
    if (!placed)
        return false;
    site->x = static_cast<int32_t>(static_cast<uint32_t>(type->footprint_x + cell[0] * 2) << 19);
    site->y = builder.y;
    site->z = static_cast<int32_t>(static_cast<uint32_t>(type->footprint_z + cell[1] * 2) << 19);
    k.placement_radius = 0;
    return true;
}

} // namespace oa::sim::ai

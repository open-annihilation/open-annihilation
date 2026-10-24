// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// brain.grid. Outside a match the function returns nil and does not read
// the call's arguments. Install does not read the seat's match.

#include "lua_grids.hpp"

#include "handles_internal.hpp"
#include "internal.hpp"
#include "lua_networks.hpp"

#include "oa/core/game_state.h"
#include "oa/core/map_plot.h"
#include "oa/core/player.h"
#include "oa/core/unit.h"
#include "oa/core/unit_def.h"
#include "oa/core/world.h"
#include "oa/data/script_host/script_host.hpp"
#include "oa/sim/brains/handles.hpp"
#include "oa/sim/detection.hpp"
#include "oa/sim/match_runtime.hpp"
#include "oa/sim/visibility_state.hpp"

#include "lua.hpp"

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace oa::sim::brains {
namespace {

constexpr uint32_t grid_cap = 128;
constexpr uint32_t cell_cap = grid_cap * grid_cap;
constexpr uint32_t plot_step = 16;
constexpr uint32_t sight_step = 32;
constexpr int32_t share_full = 65536;
constexpr int32_t stored_max = 2147483646;

enum class Layer : uint8_t {
    sight,
    explored,
    water,
    height,
    metal,
    own,
    allies,
    enemies,
    threat,
    radar
};

struct GridScratch {
    uint64_t sum[cell_cap]{};
    uint32_t tally[cell_cap]{};
    uint32_t own[cell_cap]{};
    uint32_t allies[cell_cap]{};
    uint32_t enemies[cell_cap]{};
    uint32_t threat[cell_cap]{};
    uint32_t radar[cell_cap]{};
};

static_assert(sizeof(GridScratch) == 524288, "grid scratch is 512 KiB");

struct GridSource {
    const Seat* seat{};
    match_runtime::Match* match{};
    const oa::World* world{};
    const oa::Unit* units{};
    uint32_t slots{};
    uint32_t span_x{};
    uint32_t span_z{};
    int32_t plot_w{};
    int32_t plot_h{};
    int32_t sight_w{};
    int32_t sight_h{};
    uint8_t sea{};
    uint8_t player{};
    uint32_t size{};
    const uint16_t* coverage{};
    std::size_t coverage_n{};
    const uint16_t* explored{};
    std::size_t explored_n{};
    const uint8_t* contacts{};
    std::size_t contact_n{};
};

const char* layer_names[] = {
    "sight",
    "explored",
    "water",
    "height",
    "metal",
    "own",
    "allies",
    "enemies",
    "threat",
    "radar",
};

bool same_name(const char* held, const char* want) {
    return held != nullptr && want != nullptr && std::strcmp(held, want) == 0;
}

bool unit_layer(Layer layer) {
    return layer == Layer::own || layer == Layer::allies || layer == Layer::enemies ||
           layer == Layer::threat || layer == Layer::radar;
}

bool sight_layer(Layer layer) {
    return layer == Layer::sight || layer == Layer::explored || layer == Layer::water;
}

bool plot_layer(Layer layer) {
    return layer == Layer::height || layer == Layer::metal;
}

uint32_t cell_start(uint32_t index, uint32_t span, uint32_t size) {
    return static_cast<uint32_t>((static_cast<uint64_t>(index) * span) / size);
}

uint32_t cell_of(uint32_t pos, uint32_t span, uint32_t size) {
    if (size == 0 || span == 0)
        return 0;
    uint32_t column = static_cast<uint32_t>((static_cast<uint64_t>(pos) * size) / span);
    if (column >= size)
        column = size - 1;
    while (column + 1 < size && cell_start(column + 1, span, size) <= pos)
        ++column;
    while (column > 0 && cell_start(column, span, size) > pos)
        --column;
    return column;
}

uint32_t cell_middle(uint32_t index, uint32_t span, uint32_t size) {
    const uint32_t start = cell_start(index, span, size);
    const uint32_t next = index + 1 >= size ? span : cell_start(index + 1, span, size);
    return start + (next - start) / 2;
}

int32_t clamp_stored(uint64_t scaled) {
    if (scaled > static_cast<uint64_t>(stored_max))
        return stored_max;
    return static_cast<int32_t>(scaled);
}

int32_t count_stored(uint32_t count) {
    return clamp_stored(static_cast<uint64_t>(count) * static_cast<uint64_t>(share_full));
}

int32_t mean_stored(uint64_t sum, uint32_t tally, int32_t fallback) {
    if (tally == 0)
        return fallback;
    return clamp_stored((sum * static_cast<uint64_t>(share_full)) / tally);
}

uint32_t span_of(int32_t raw) {
    if (raw <= 0)
        return 0;
    return static_cast<uint32_t>(raw);
}

uint64_t area_of(int32_t width, int32_t height) {
    if (width <= 0 || height <= 0)
        return 0;
    return static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
}

void bind_readings(GridSource* source) {
    const auto coverage = source->match->player_coverage(source->player);
    source->coverage = coverage.data();
    source->coverage_n = coverage.size();
    const auto contacts = source->match->player_contact_bits(source->player);
    source->contacts = contacts.data();
    source->contact_n = contacts.size();
    const sim::visibility_state::PlayerSightGrid& grid = source->match->sight();
    source->explored = grid.player_bits.data();
    source->explored_n = grid.player_bits.size();
}

bool sight_on(const GridSource& source, std::size_t index) {
    if (source.coverage == nullptr || index >= source.coverage_n)
        return false;
    return sim::visibility_state::count_in_sight(
        source.coverage[index], sim::visibility_state::SightReading::simulation
    );
}

bool explored_on(const GridSource& source, std::size_t index) {
    if (source.explored == nullptr || index >= source.explored_n || source.player >= 16)
        return false;
    const auto mask = static_cast<uint16_t>(uint16_t{1} << source.player);
    return (source.explored[index] & mask) != 0;
}

int32_t fallback_at(const GridSource& source, Layer layer, uint32_t column, uint32_t row) {
    if (source.span_x == 0 || source.span_z == 0 || source.size == 0)
        return 0;
    const uint32_t mid_x = cell_middle(column, source.span_x, source.size);
    const uint32_t mid_z = cell_middle(row, source.span_z, source.size);
    if (layer == Layer::sight || layer == Layer::explored) {
        if (source.sight_w <= 0 || source.sight_h <= 0)
            return 0;
        const uint32_t cell_x = mid_x / sight_step;
        const uint32_t cell_z = mid_z / sight_step;
        if (cell_x >= static_cast<uint32_t>(source.sight_w) ||
            cell_z >= static_cast<uint32_t>(source.sight_h))
            return 0;
        const std::size_t index =
            static_cast<std::size_t>(cell_z) * static_cast<std::size_t>(source.sight_w) + cell_x;
        const bool on =
            layer == Layer::sight ? sight_on(source, index) : explored_on(source, index);
        return on ? share_full : 0;
    }
    if (source.plot_w <= 0 || source.plot_h <= 0)
        return 0;
    const uint32_t plot_x = mid_x / plot_step;
    const uint32_t plot_z = mid_z / plot_step;
    if (plot_x >= static_cast<uint32_t>(source.plot_w) ||
        plot_z >= static_cast<uint32_t>(source.plot_h))
        return 0;
    const oa::MapPlot* plot =
        oa::world_plot(source.world, static_cast<int32_t>(plot_x), static_cast<int32_t>(plot_z));
    if (plot == nullptr)
        return 0;
    if (layer == Layer::water)
        return plot->height < source.sea ? share_full : 0;
    if (layer == Layer::height)
        return static_cast<int32_t>(plot->height) * share_full;
    if (layer == Layer::metal)
        return static_cast<int32_t>(plot->metal) * share_full;
    return 0;
}

void paint_share(const GridSource& source, GridScratch* scratch, Layer layer, int32_t* dest) {
    const uint32_t size = source.size;
    for (uint32_t row = 0; row < size; ++row) {
        for (uint32_t column = 0; column < size; ++column) {
            const uint32_t at = row * size + column;
            const int32_t fallback =
                scratch->tally[at] == 0 ? fallback_at(source, layer, column, row) : 0;
            dest[at] = mean_stored(scratch->sum[at], scratch->tally[at], fallback);
        }
    }
}

void walk_sight(const GridSource& source, GridScratch* scratch, Layer layer) {
    if (source.sight_w <= 0 || source.sight_h <= 0 || source.span_x == 0 || source.span_z == 0)
        return;
    for (int32_t cell_z = 0; cell_z < source.sight_h; ++cell_z) {
        for (int32_t cell_x = 0; cell_x < source.sight_w; ++cell_x) {
            const uint32_t centre_x = static_cast<uint32_t>(cell_x) * sight_step + sight_step / 2;
            const uint32_t centre_z = static_cast<uint32_t>(cell_z) * sight_step + sight_step / 2;
            if (centre_x >= source.span_x || centre_z >= source.span_z)
                continue;
            const uint32_t column = cell_of(centre_x, source.span_x, source.size);
            const uint32_t row = cell_of(centre_z, source.span_z, source.size);
            const uint32_t at = row * source.size + column;
            scratch->tally[at] += 1;
            const std::size_t index =
                static_cast<std::size_t>(cell_z) * static_cast<std::size_t>(source.sight_w) +
                static_cast<std::size_t>(cell_x);
            bool on = false;
            if (layer == Layer::sight)
                on = sight_on(source, index);
            else if (layer == Layer::explored)
                on = explored_on(source, index);
            if (on)
                scratch->sum[at] += 1;
        }
    }
}

void walk_plots(const GridSource& source, GridScratch* scratch, Layer layer) {
    if (source.plot_w <= 0 || source.plot_h <= 0 || source.span_x == 0 || source.span_z == 0)
        return;
    for (int32_t plot_z = 0; plot_z < source.plot_h; ++plot_z) {
        for (int32_t plot_x = 0; plot_x < source.plot_w; ++plot_x) {
            const uint32_t centre_x = static_cast<uint32_t>(plot_x) * plot_step + plot_step / 2;
            const uint32_t centre_z = static_cast<uint32_t>(plot_z) * plot_step + plot_step / 2;
            if (centre_x >= source.span_x || centre_z >= source.span_z)
                continue;
            const oa::MapPlot* plot = oa::world_plot(source.world, plot_x, plot_z);
            if (plot == nullptr)
                continue;
            const uint32_t column = cell_of(centre_x, source.span_x, source.size);
            const uint32_t row = cell_of(centre_z, source.span_z, source.size);
            const uint32_t at = row * source.size + column;
            scratch->tally[at] += 1;
            if (layer == Layer::water && plot->height < source.sea)
                scratch->sum[at] += 1;
            else if (layer == Layer::height)
                scratch->sum[at] += plot->height;
            else if (layer == Layer::metal)
                scratch->sum[at] += plot->metal;
        }
    }
}

bool contact_hidden(const GridSource& source, uint32_t slot) {
    if (slot > 65535u)
        return true;
    bool in_sight = false;
    try {
        in_sight = source.match->unit_visible(source.player, static_cast<uint16_t>(slot));
    } catch (...) {
        in_sight = true;
    }
    return in_sight;
}

void walk_units(const GridSource& source, GridScratch* scratch) {
    if (source.units == nullptr || source.span_x == 0 || source.span_z == 0)
        return;
    for (uint32_t slot = 0; slot < source.slots; ++slot) {
        const oa::Unit& unit = source.units[slot];
        if (unit.type_index == 0 || !oa::unit_is_live_target(unit.flags))
            continue;
        if (unit.owner_index >= OA_PLAYER_COUNT)
            continue;
        const int32_t raw_x = unit.position.x >> 16;
        const int32_t raw_z = unit.position.z >> 16;
        if (raw_x < 0 || raw_z < 0)
            continue;
        const auto world_x = static_cast<uint32_t>(raw_x);
        const auto world_z = static_cast<uint32_t>(raw_z);
        if (world_x >= source.span_x || world_z >= source.span_z)
            continue;
        const uint32_t column = cell_of(world_x, source.span_x, source.size);
        const uint32_t row = cell_of(world_z, source.span_z, source.size);
        const uint32_t at = row * source.size + column;
        const uint8_t owner = unit.owner_index;
        if (owner == source.player) {
            scratch->own[at] += 1;
            continue;
        }
        if (slot > 65535u)
            continue;
        const UnitRef ref = unit_ref(*source.match, static_cast<uint16_t>(slot));
        if (source.match->allied(source.player, owner)) {
            if (seat_may_see(*source.seat, ref))
                scratch->allies[at] += 1;
            continue;
        }
        if (seat_may_see(*source.seat, ref)) {
            scratch->enemies[at] += 1;
            const oa::UnitDef* def = oa::world_unit_def_of(source.world, &unit);
            if (def != nullptr && (def->flags & OA_UNIT_DEF_FLAG_HAS_WEAPONS) != 0)
                scratch->threat[at] += 1;
        }
        if (source.contacts == nullptr || slot >= source.contact_n)
            continue;
        const uint8_t picture = source.contacts[slot];
        if ((picture & (sim::detection::picture_radar | sim::detection::picture_sonar)) == 0)
            continue;
        if (!contact_hidden(source, slot))
            scratch->radar[at] += 1;
    }
}

void write_counts(const uint32_t* counts, uint32_t size, int32_t* dest) {
    const uint32_t cells = size * size;
    for (uint32_t index = 0; index < cells; ++index)
        dest[index] = count_stored(counts[index]);
}

const uint32_t* counts_for(const GridScratch* scratch, Layer layer) {
    if (layer == Layer::own)
        return scratch->own;
    if (layer == Layer::allies)
        return scratch->allies;
    if (layer == Layer::enemies)
        return scratch->enemies;
    if (layer == Layer::threat)
        return scratch->threat;
    return scratch->radar;
}

void fill_grid(
    GridSource* source,
    GridScratch* scratch,
    int32_t* elements,
    const Layer* layers,
    uint8_t layer_count
) {
    bind_readings(source);
    bool units_walked = false;
    for (uint8_t layer_index = 0; layer_index < layer_count; ++layer_index) {
        if (!unit_layer(layers[layer_index]))
            continue;
        if (!units_walked) {
            std::memset(scratch->own, 0, sizeof scratch->own);
            std::memset(scratch->allies, 0, sizeof scratch->allies);
            std::memset(scratch->enemies, 0, sizeof scratch->enemies);
            std::memset(scratch->threat, 0, sizeof scratch->threat);
            std::memset(scratch->radar, 0, sizeof scratch->radar);
            walk_units(*source, scratch);
            units_walked = true;
        }
    }
    const uint32_t cells = source->size * source->size;
    for (uint8_t layer_index = 0; layer_index < layer_count; ++layer_index) {
        const Layer layer = layers[layer_index];
        int32_t* dest = elements + static_cast<std::size_t>(layer_index) * cells;
        if (unit_layer(layer)) {
            write_counts(counts_for(scratch, layer), source->size, dest);
            continue;
        }
        if ((layer == Layer::sight || layer == Layer::explored) &&
            (source->sight_w <= 0 || source->sight_h <= 0))
            continue;
        std::memset(scratch->sum, 0, sizeof scratch->sum);
        std::memset(scratch->tally, 0, sizeof scratch->tally);
        if (layer == Layer::sight || layer == Layer::explored)
            walk_sight(*source, scratch, layer);
        else
            walk_plots(*source, scratch, layer);
        paint_share(*source, scratch, layer, dest);
    }
}

extern "C" {

static int brain_grid(lua_State* state) {
    Seat* seat = seat_of(state);
    if (seat == nullptr || seat->match == nullptr) {
        lua_pushnil(state);
        return 1;
    }

    if (lua_type(state, 1) != LUA_TTABLE) {
        luaL_error(state, "brain.grid: expects 1 to 10 layer names");
        return 0;
    }
    const lua_Unsigned raw_count = lua_rawlen(state, 1);
    if (raw_count < 1 || raw_count > 10) {
        luaL_error(state, "brain.grid: expects 1 to 10 layer names");
        return 0;
    }
    const auto layer_count = static_cast<uint8_t>(raw_count);
    Layer layers[10];
    bool seen[10] = {};
    bool want_units = false;
    bool want_sight = false;
    bool want_plots = false;
    for (uint8_t index = 0; index < layer_count; ++index) {
        lua_rawgeti(state, 1, static_cast<lua_Integer>(index) + 1);
        if (lua_type(state, -1) != LUA_TSTRING) {
            luaL_error(state, "brain.grid: expects 1 to 10 layer names");
            return 0;
        }
        const char* name = lua_tostring(state, -1);
        int found = -1;
        for (int kind = 0; kind < 10; ++kind) {
            if (same_name(name, layer_names[kind]))
                found = kind;
        }
        if (found < 0) {
            luaL_error(state, "brain.grid: unknown layer '%s'", name != nullptr ? name : "");
            return 0;
        }
        if (seen[found]) {
            luaL_error(state, "brain.grid: layer '%s' is repeated", layer_names[found]);
            return 0;
        }
        seen[found] = true;
        layers[index] = static_cast<Layer>(found);
        lua_pop(state, 1);
        if (unit_layer(layers[index]))
            want_units = true;
        if (sight_layer(layers[index]))
            want_sight = true;
        if (plot_layer(layers[index]))
            want_plots = true;
    }

    uint32_t size = 64;
    if (lua_gettop(state) >= 2 && lua_isnil(state, 2) == 0) {
        if (lua_isinteger(state, 2) == 0) {
            luaL_error(state, "brain.grid: size must be 8, 16, 32, 64 or 128");
            return 0;
        }
        const lua_Integer given = lua_tointeger(state, 2);
        if (given != 8 && given != 16 && given != 32 && given != 64 && given != 128) {
            luaL_error(state, "brain.grid: size must be 8, 16, 32, 64 or 128");
            return 0;
        }
        size = static_cast<uint32_t>(given);
    }

    match_runtime::Match* match = seat->match;
    const oa::World& world = match->state();
    const sim::visibility_state::PlayerSightGrid& sight = match->sight();
    const uint32_t slots = world.units == nullptr ? 0u : world.unit_slot_count;
    const uint64_t plots = area_of(world.game.map_width, world.game.map_height);
    const uint64_t sight_cells = area_of(sight.width, sight.height);
    const uint64_t cells = static_cast<uint64_t>(layer_count) * size * size;
    uint64_t units = (cells + 15u) / 16u;
    if (want_units)
        units += slots;
    if (want_plots)
        units += (plots + 63u) / 64u;
    if (want_sight)
        units += (sight_cells + 63u) / 64u;
    script_host::charge_work(state, units);

    auto* scratch = static_cast<GridScratch*>(lua_newuserdatauv(state, sizeof(GridScratch), 0));
    std::memset(scratch, 0, sizeof(GridScratch));
    const uint32_t dims[4] = {layer_count, size, size, 0};
    int32_t* elements = push_zero_tensor(state, 3, dims);
    if (elements == nullptr)
        return 0;

    GridSource source;
    source.seat = seat;
    source.match = match;
    source.world = &world;
    source.units = world.units;
    source.slots = slots;
    source.span_x = span_of(world.game.map_width_world);
    source.span_z = span_of(world.game.map_height_world);
    source.plot_w = world.game.map_width;
    source.plot_h = world.game.map_height;
    source.sight_w = sight.width;
    source.sight_h = sight.height;
    source.sea = world.game.sea_level;
    source.player = seat->player;
    source.size = size;

    bool failed = false;
    try {
        fill_grid(&source, scratch, elements, layers, layer_count);
    } catch (...) {
        failed = true;
    }
    if (failed) {
        luaL_error(state, "brain.grid: the match could not be read");
        return 0;
    }
    return 1;
}

} // extern "C"

} // namespace

void install_grids(lua_State* state, Seat& seat) {
    (void)seat;
    lua_getglobal(state, "brain");
    if (lua_type(state, -1) != LUA_TTABLE) {
        lua_pop(state, 1);
        lua_newtable(state);
    }
    lua_pushcfunction(state, brain_grid);
    lua_setfield(state, -2, "grid");
    lua_setglobal(state, "brain");
}

} // namespace oa::sim::brains

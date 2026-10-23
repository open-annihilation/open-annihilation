// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Map features on the match's canonical plots: the loader's placement, the
// feature runtime's host calls and the match plots kept in step with it.
#include "oa/sim/match_runtime.hpp"

#include "oa/sim/map_runtime/feature_defs.hpp"
#include "oa/sim/weapon_execution/projectile_pool.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

namespace oa::sim::match_runtime {
namespace {

namespace features = sim::feature_runtime;

// MapPlot.flags bits the feature code owns: the record bit and the placing
// player.
constexpr uint8_t feature_plot_flags =
    OA_PLOT_FLAG_ANIMATING_FEATURE | OA_PLOT_FLAG_PLAYER_FEATURE_MASK;

// A continuation cell keeps z back in the low byte of the record word and x in the high byte.
uint8_t back_x(const MapPlot& plot) noexcept {
    return static_cast<uint8_t>(plot.feature_record >> 8);
}

uint8_t back_z(const MapPlot& plot) noexcept {
    return static_cast<uint8_t>(plot.feature_record & 0xffu);
}

const FeatureDef* table_def(const World& world, uint16_t word) noexcept {
    return word < world.feature_def_count ? &world.feature_defs[word] : nullptr;
}

// The low byte of a feature's metal truncated to a signed 64-bit whole
// number; a value past that range truncates to its least, whose low byte is
// zero.
uint8_t metal_byte(float metal) noexcept {
    constexpr double signed_64_limit = 0x1p63;
    const auto value = static_cast<double>(metal);
    if (!(value >= -signed_64_limit && value < signed_64_limit))
        return 0;
    return static_cast<uint8_t>(static_cast<int64_t>(value));
}

// The origin word of the footprint covering a plot; a continuation whose
// origin lies before the map start has none.
uint16_t origin_word(const World& world, std::size_t index) noexcept {
    const auto& plot = world.plots[index];
    if (plot.feature != features::feature_continuation)
        return plot.feature;
    const auto back =
        static_cast<std::size_t>(back_z(plot)) * static_cast<std::size_t>(world.game.map_width) +
        back_x(plot);
    return back <= index ? world.plots[index - back].feature : features::no_feature;
}

/// Tells whether a plot's feature stops movement: an empty plot or a
/// continuation to nothing never does, a word past the table or a marker
/// always does, and a FeatureDef does when it is blocking.
///
/// @param world Canonical world with its plots and FeatureDef table.
/// @param index Plot index (row-major).
/// @return True when the plot blocks movement.
bool plot_blocks(const World& world, std::size_t index) noexcept {
    const auto word = world.plots[index].feature;
    if (word == features::no_feature)
        return false;
    if (word < OA_PLOT_FEATURE_RESERVED) {
        const auto* def = table_def(world, word);
        return def == nullptr || (def->flags & OA_FEATURE_FLAG_BLOCKING) != 0;
    }
    if (word != features::feature_continuation)
        return true;
    const auto origin = origin_word(world, index);
    if (origin >= OA_PLOT_FEATURE_RESERVED)
        return false;
    const auto* def = table_def(world, origin);
    return def != nullptr && (def->flags & OA_FEATURE_FLAG_BLOCKING) != 0;
}

} // namespace

namespace {

/// Returns the match a feature runtime call is made on.
///
/// @param context FeatureHost::context, the match.
/// @return The match.
Match& feature_match(void* context) noexcept {
    return *static_cast<Match*>(context);
}

/// Finds a mission placement's FeatureDef by name; the app loaded every
/// named one into the table before the match copied it.
///
/// @param context The match.
/// @param name The FeatureDef's name.
/// @return Its index in the match's table, or features::no_feature.
uint16_t find_mission_feature(void* context, const char* name) {
    const auto& world = feature_match(context).state();
    if (world.feature_defs == nullptr)
        return features::no_feature;
    return sim::map_runtime::find_feature_index(
        {world.feature_defs, world.feature_def_count}, name
    );
}

} // namespace

// The feature runtime's calls into the match. They are defined here, in a
// member of the match, which gives them the match's own access.
features::FeatureHost Match::feature_host() noexcept {
    features::FeatureHost host{};
    host.context = this;
    host.random = [](void* context, uint32_t limit) {
        return feature_match(context).random_.bounded(limit);
    };
    host.lcg_random = [](void* context) { return feature_match(context).lcg_rand(); };
    host.sequence_frame =
        [](void* context, oa_ref32 sequence, uint16_t frame, features::FeatureSequenceFrame* out) {
            const auto& lookup = feature_match(context).input_.feature_sequence_frame;
            return lookup && lookup(sequence, frame, *out);
        };
    // The footprint goes to the map listeners.
    host.footprint_changed =
        [](void* context, int16_t cell_x, int16_t cell_z, int16_t width, int16_t height) {
            auto& m = feature_match(context);
            m.project_feature_plots({cell_x, cell_z}, {width, height});
            m.map_listeners_.notify_footprint_changed({cell_x, cell_z}, {width, height});
        };
    host.emit_feature_fx = [](void* context, const FixedVec3* position, uint32_t layer) {
        auto& m = feature_match(context);
        sim::effect_particles::spawn_feature_smoke(
            m.effects(), m.state().game, m.effect_host(), *position, static_cast<uint16_t>(layer)
        );
    };
    host.emit_smoke = [](void* context, const FixedVec3* position, uint32_t layer) {
        auto& m = feature_match(context);
        sim::effect_particles::spawn_white_smoke(
            m.effects(), m.state().game, m.effect_host(), *position, static_cast<uint16_t>(layer)
        );
    };
    host.play_sound = [](void* context, const char* name, const FixedVec3* position) {
        feature_match(context).play_named_sound_at(name, *position);
    };
    host.burn_weapon = [](void* context, oa_ref32 weapon, const FixedVec3* position) {
        feature_match(context).burn_weapon_blast(weapon, *position);
    };
    // Energy, then metal, into the reclaiming unit's economy block; a mod's
    // reclaim multipliers replace the computer players' scaling.
    host.credit_reclaim = [](void* context, Unit* unit, float energy, float metal) {
        auto& m = feature_match(context);
        const auto& income = m.rules().ai.income_multipliers;
        const auto* reclaim = income.enabled ? &income.reclaim : nullptr;
        m.credit_energy(*unit, energy, reclaim);
        m.credit_metal(*unit, metal, reclaim);
    };
    // A weapon hit on a feature that another player's machine settles.
    host.feature_hit_elsewhere = [](void* context,
                                    uint8_t weapon_id,
                                    int32_t cell_x,
                                    int32_t cell_z) {
        const auto& multiplayer = feature_match(context).multiplayer;
        return multiplayer.feature_hit_elsewhere != nullptr &&
               multiplayer.feature_hit_elsewhere(multiplayer.context, weapon_id, cell_x, cell_z);
    };
    // A feature change settled here, for the other players, with the slot
    // of the unit that reclaimed it.
    host.feature_changed = [](void* context,
                              features::FeatureChange change,
                              int32_t cell_x,
                              int32_t cell_z,
                              const Unit* reclaimer) {
        auto& m = feature_match(context);
        const auto& multiplayer = m.multiplayer;
        if (multiplayer.feature_changed == nullptr)
            return;
        const auto slot = reclaimer != nullptr
                              ? static_cast<uint16_t>(world_unit_slot(&m.state(), reclaimer))
                              : uint16_t{0};
        multiplayer.feature_changed(multiplayer.context, change, cell_x, cell_z, slot);
    };
    return host;
}

bool Match::reclaim_feature(oa::Unit& unit, const FixedVec3& point) {
    return features::reclaim_feature(state(), feature_host(), unit, point);
}

void Match::place_saved_feature(
    uint8_t* plot, uint16_t def_index, const uint8_t* position, const uint8_t* orientation
) {
    auto& world = state();
    const auto* first = reinterpret_cast<const uint8_t*>(world.plots);
    if (plot == nullptr || plot < first)
        return;
    const auto index = static_cast<std::size_t>(plot - first) / sizeof(oa::MapPlot);
    const auto word = [](const uint8_t* bytes) {
        return static_cast<uint16_t>(bytes[0] | (bytes[1] << 8));
    };
    const auto dword = [&](const uint8_t* bytes) {
        return static_cast<uint32_t>(word(bytes)) | (static_cast<uint32_t>(word(bytes + 2)) << 16);
    };
    FixedVec3 at{};
    if (position != nullptr) {
        at.x = static_cast<oa_fixed>(dword(position));
        at.y = static_cast<oa_fixed>(dword(position + sizeof(uint32_t)));
        at.z = static_cast<oa_fixed>(dword(position + 2 * sizeof(uint32_t)));
    }
    std::array<int16_t, 3> turn{};
    if (orientation != nullptr)
        for (std::size_t axis = 0; axis < turn.size(); ++axis)
            turn[axis] = static_cast<int16_t>(word(orientation + axis * sizeof(uint16_t)));
    (void)features::place_feature(
        world,
        feature_host(),
        index,
        def_index,
        position != nullptr ? &at : nullptr,
        orientation != nullptr ? turn.data() : nullptr,
        features::no_player
    );
}

void Match::ignite_saved_feature(int32_t cell_x, int32_t cell_z) {
    features::ignite_feature(state(), feature_host(), cell_x, cell_z, false);
}

void Match::restart_saved_feature_sequence(int32_t cell_x, int32_t cell_z, int32_t kind) {
    features::start_feature_sequence(state(), feature_host(), cell_x, cell_z, kind == 1);
}

void Match::adopt_feature_defs(std::span<const oa::FeatureDef> defs) {
    auto& world = state();
    state_.feature_defs.assign(defs.begin(), defs.end());
    world.feature_defs = state_.feature_defs.data();
    world.feature_def_count = static_cast<uint32_t>(state_.feature_defs.size());
    world.game.feature_def_count = static_cast<int32_t>(state_.feature_defs.size());
    input_.feature_defs = state_.feature_defs;
}

void Match::place_map_features() {
    auto& world = state();
    const auto width = world.game.map_width;
    const auto cells =
        static_cast<std::size_t>(width) * static_cast<std::size_t>(world.game.map_height);
    const auto& attributes = input_.map.attributes;
    for (std::size_t index = 0; index < cells; ++index) {
        auto& plot = world.plots[index];
        const auto& resolved = spatial_.plots[index];
        plot.height = index < attributes.size() ? attributes[index].height : 0;
        plot.high_height = resolved.high_height;
        plot.low_height = resolved.low_height;
        plot.metal = resolved.metal;
        plot.feature = features::no_feature;
        plot.flags = static_cast<uint8_t>(features::no_player << features::plot_player_shift);
    }
    if (!features::init_feature_pool(world)) {
        fault_.note("placed-feature pool is smaller than the game's");
        return;
    }
    // The movement maps are built after the placement, so it dispatches
    // nothing; the whole map is projected onto the match plots once it is done.
    auto host = feature_host();
    host.footprint_changed = nullptr;
    for (std::size_t index = 0; index < cells; ++index) {
        const auto word = spatial_.plots[index].feature_word;
        if (word >= OA_PLOT_FEATURE_RESERVED && word != features::feature_continuation &&
            word != features::no_feature)
            (void)features::place_feature(
                world, host, index, word, nullptr, nullptr, features::no_player
            );
    }
    // A resuming savegame places its own features once the match stands.
    if (!input_.resuming_saved_game) {
        for (std::size_t index = 0; index < cells; ++index) {
            const auto word = spatial_.plots[index].feature_word;
            if (word < OA_PLOT_FEATURE_RESERVED && table_def(world, word) != nullptr)
                (void)features::place_feature(
                    world, host, index, word, nullptr, nullptr, features::no_player
                );
        }
        // Words the FeatureDef table does not name keep the loader's footprint.
        for (std::size_t index = 0; index < cells; ++index) {
            const auto& resolved = spatial_.plots[index];
            auto& plot = world.plots[index];
            if (plot.feature != features::no_feature ||
                resolved.feature_word == features::no_feature)
                continue;
            plot.feature = resolved.feature_word;
            if (resolved.feature_word == features::feature_continuation)
                plot.feature_record =
                    static_cast<uint16_t>((resolved.feature_back_x << 8) | resolved.feature_back_z);
        }
        // Then the mission schema's features are placed, and every feature
        // standing paints its metal.
        features::apply_feature_placements(
            world,
            host,
            input_.mission_features.data(),
            static_cast<int32_t>(input_.mission_features.size()),
            find_mission_feature
        );
        paint_feature_metal();
    }
    // The map edges are hidden once every feature is placed.
    features::void_hidden_edges(world, lava_world_ != 0);
    project_feature_plots(
        {0, 0}, {static_cast<int16_t>(width), static_cast<int16_t>(world.game.map_height)}
    );
}

void Match::paint_feature_metal() {
    auto& world = state();
    const auto width = world.game.map_width;
    const auto height = world.game.map_height;
    const auto cells = static_cast<std::size_t>(width) * static_cast<std::size_t>(height);
    for (std::size_t index = 0; index < cells; ++index) {
        const auto* def = table_def(world, world.plots[index].feature);
        if (def == nullptr || def->metal == 0.0F || std::isnan(def->metal) ||
            (def->flags & OA_FEATURE_FLAG_INDESTRUCTIBLE) == 0)
            continue;
        const auto metal = metal_byte(def->metal);
        const auto origin_x = static_cast<int32_t>(index % static_cast<std::size_t>(width));
        const auto origin_z = static_cast<int32_t>(index / static_cast<std::size_t>(width));
        for (int32_t row = 0; row < def->footprint_z; ++row)
            for (int32_t column = 0; column < def->footprint_x; ++column) {
                const auto x = origin_x + column;
                const auto z = origin_z + row;
                if (x >= width || z >= height)
                    continue;
                const auto cell = static_cast<std::size_t>(z) * static_cast<std::size_t>(width) +
                                  static_cast<std::size_t>(x);
                world.plots[cell].metal = metal;
                spatial_.plots[cell].metal = metal;
                if (plot_metal_)
                    plot_metal_[cell].movement_cost = metal;
            }
    }
}

void Match::adopt_plot_metal() {
    const auto& world = state();
    const auto cells = static_cast<std::size_t>(std::max(world.game.map_width, 0)) *
                       static_cast<std::size_t>(std::max(world.game.map_height, 0));
    for (std::size_t index = 0; index < cells; ++index) {
        const auto metal = world.plots[index].metal;
        if (index < spatial_.plots.size())
            spatial_.plots[index].metal = metal;
        if (plot_metal_)
            plot_metal_[index].movement_cost = metal;
    }
}

void Match::project_feature_plots(std::array<int16_t, 2> cell, std::array<int16_t, 2> footprint) {
    const auto& world = state();
    const auto width = world.game.map_width;
    const auto height = world.game.map_height;
    for (int32_t row = 0; row < footprint[1]; ++row) {
        for (int32_t column = 0; column < footprint[0]; ++column) {
            const auto x = cell[0] + column;
            const auto z = cell[1] + row;
            if (x < 0 || z < 0 || x >= width || z >= height)
                continue;
            const auto index = static_cast<std::size_t>(z) * static_cast<std::size_t>(width) +
                               static_cast<std::size_t>(x);
            const auto& from = world.plots[index];
            auto& to = spatial_.plots[index];
            to.flags = static_cast<uint8_t>(
                (to.flags & ~feature_plot_flags) | (from.flags & feature_plot_flags)
            );
            to.feature_word = from.feature;
            const bool continuation = from.feature == features::feature_continuation;
            to.feature_back_x = continuation ? back_x(from) : uint8_t{0};
            to.feature_back_z = continuation ? back_z(from) : uint8_t{0};
            to.blocking_feature = plot_blocks(world, index);
            const auto origin = origin_word(world, index);
            const auto* def =
                origin < OA_PLOT_FEATURE_RESERVED ? table_def(world, origin) : nullptr;
            to.feature_height = def != nullptr ? static_cast<uint8_t>(def->height) : uint8_t{0};
            to.feature_footprint_x = def != nullptr ? def->footprint_x : int16_t{1};
            to.feature_footprint_z = def != nullptr ? def->footprint_z : int16_t{1};
            to.metal_feature = def != nullptr && def->metal != 0.0F;
            to.geo_feature = def != nullptr && (def->flags & OA_FEATURE_FLAG_GEOTHERMAL) != 0;
            to.indestructible_feature =
                def != nullptr && (def->flags & OA_FEATURE_FLAG_INDESTRUCTIBLE) != 0;
        }
    }
}

void Match::project_plot_occupants() {
    // The share of the map's plots at which the written ones are copied by
    // copying every plot: 1 in 8.
    constexpr std::size_t whole_copy_share = 8;
    auto& world = state();
    const auto cells = static_cast<std::size_t>(world.game.map_width) *
                       static_cast<std::size_t>(world.game.map_height);
    const auto count = std::min(cells, spatial_.plots.size());
    const auto copy = [&](std::size_t index) {
        world.plots[index].ground_unit = spatial_.plots[index].ground;
        world.plots[index].air_unit = spatial_.plots[index].air;
    };
    const auto written = spatial_.written_occupant_count;
    if (!occupants_projected_ || spatial_.written_occupants_lost ||
        written >= count / whole_copy_share) {
        for (std::size_t index = 0; index < count; ++index)
            copy(index);
        occupants_projected_ = true;
    } else {
        for (uint32_t entry = 0; entry < written; ++entry)
            if (spatial_.written_occupants[entry] < count)
                copy(spatial_.written_occupants[entry]);
    }
    spatial_.written_occupant_count = 0;
    spatial_.written_occupants_lost = false;
}

bool Match::launch_meteor(
    oa_ref32 weapon, const FixedVec3& position, const FixedVec3& velocity, bool share
) {
    const auto* shot =
        sim::weapon_execution::spawn_free_projectile(state(), weapon, position, velocity);
    if (shot == nullptr)
        return false;
    emit_event<&EventHooks::shot_placed>(state(), *shot, ShotSource::meteor, nullptr, uint16_t{});
    // The weapon's soundstart plays where the shot starts.
    if (const auto* definition = projectile_weapon(*shot))
        play_sound_at(definition->soundstart.c_str(), position);
    if (share) {
        ShotEvent shared{};
        shared.start = position;
        shared.target = velocity;
        shared.weapon_id = oa::world_weapon_def(&state(), weapon)->weapon_id;
        share_shot(shared);
    }
    return true;
}

void Match::burn_weapon_blast(oa_ref32 weapon, const FixedVec3& at) {
    auto* def = oa::world_weapon_def(&state(), weapon);
    if (def == nullptr)
        return;
    oa::Projectile blast{};
    blast.def = weapon;
    blast.position = at;
    blast.owner_index = features::no_player;
    detonate_area(blast, input_.weapons.definition(static_cast<uint8_t>(weapon - 1u)), *def);
}

} // namespace oa::sim::match_runtime

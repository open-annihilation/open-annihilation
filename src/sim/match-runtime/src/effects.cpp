// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/match_runtime.hpp"
#include "oa/core/map_plot.h"

#include <algorithm>
#include <bit>

namespace oa::sim::match_runtime {
namespace {
constexpr std::string_view fx_archive{};
constexpr int32_t cell_shift = 20;            // 16.16 world units per map cell, as a shift
constexpr uint8_t camera_flag_shaking = 0x01; // Game.camera_flags

FixedVec3 fixed_point(const std::array<uint32_t, 3>& position) noexcept {
    return {
        std::bit_cast<int32_t>(position[0]),
        std::bit_cast<int32_t>(position[1]),
        std::bit_cast<int32_t>(position[2])
    };
}

int32_t wrapping_add(int32_t left, int32_t right) noexcept {
    return static_cast<int32_t>(static_cast<uint32_t>(left) + static_cast<uint32_t>(right));
}

/// Adds a blast's screen shake unless the console's noshake option is on:
/// the duration averages with the running one and the magnitudes add to a
/// running shake's, else to zero.
///
/// @param[in,out] game Game record holding the running shake and camera
///     flags.
/// @param magnitude_x Horizontal shake magnitude to add.
/// @param magnitude_y Vertical shake magnitude to add.
/// @param duration Shake duration in ticks, averaged with the running one.
void add_screen_shake(
    oa::Game& game, int32_t magnitude_x, int32_t magnitude_y, int32_t duration
) noexcept {
    if ((game.console_flags & OA_CONSOLE_FLAG_NO_SHAKE) != 0)
        return;
    if ((game.camera_flags & camera_flag_shaking) == 0) {
        game.shake_amplitude_x = 0;
        game.shake_amplitude_y = 0;
    }
    game.shake_duration = wrapping_add(game.shake_duration, duration) / 2;
    game.shake_remaining = game.shake_duration;
    game.shake_amplitude_x = wrapping_add(game.shake_amplitude_x, magnitude_x);
    game.shake_amplitude_y = wrapping_add(game.shake_amplitude_y, magnitude_y);
    if (game.shake_duration > 0)
        game.camera_flags |= camera_flag_shaking;
}
} // namespace

void Match::play_sound_at(const char* name, const FixedVec3& at) {
    if (name == nullptr || name[0] == '\0' || point_sound.play == nullptr)
        return;
    auto& world = state();
    // The map cell is a signed quotient, not a shift.
    constexpr int32_t cell_units = 1 << cell_shift;
    if (oa::world_plot(&world, at.x / cell_units, at.z / cell_units) == nullptr)
        return;
    if (!point_visible(world.game.viewpoint_player, fixed_words(at)))
        return;
    const auto& game = world.game;
    // The point's whole-pixel words, signed.
    const int32_t x = static_cast<int16_t>(at.x >> 16);
    const int32_t y = static_cast<int16_t>(at.y >> 16);
    const int32_t z = static_cast<int16_t>(at.z >> 16);
    const auto camera_x = static_cast<int32_t>(game.camera_x);
    const auto camera_y = static_cast<int32_t>(game.camera_y);
    PointSound sound{at, point_sound_volume_near};
    if (point_sound.spatial != nullptr && point_sound.spatial(point_sound.context)) {
        sound.placed = true;
        sound.x = x - camera_x - game.view_cells_width / 2 * OA_MAP_CELL_PIXELS;
        sound.z = (y >> 1) - z + game.view_cells_height / 2 * OA_MAP_CELL_PIXELS + camera_y;
        sound.min_distance = static_cast<float>(
            (game.view_cells_width + game.view_cells_height) / 2 * OA_MAP_CELL_PIXELS
        );
        sound.max_distance =
            static_cast<float>((game.map_width + game.map_height) * OA_MAP_CELL_PIXELS);
    } else if (
        x < camera_x || z < camera_y || x > camera_x + game.view_cells_width * OA_MAP_CELL_PIXELS ||
        z > camera_y + game.view_cells_height * OA_MAP_CELL_PIXELS
    ) {
        sound.volume = point_sound_volume_far;
    }
    point_sound.play(point_sound.context, name, sound);
}

void Match::play_named_sound_at(const char* name, const FixedVec3& at) {
    if (named_sound.file == nullptr)
        return;
    if (const char* file = named_sound.file(named_sound.context, name))
        play_sound_at(file, at);
}

// A weapon's explosion entry resolves only when both keys are set.
const formats::gaf::Sequence*
Match::weapon_effect_sequence(std::string_view archive, std::string_view entry) const {
    if (archive.empty() || entry.empty() || !input_.effect_sequence)
        return nullptr;
    return input_.effect_sequence(archive, entry);
}

// The water slot (WeaponDef.water_explosion_art) holds the lava art on lava
// worlds: the weapons were loaded with the lava keys there.
const formats::gaf::Sequence*
Match::water_effect_sequence(const sim::combat_state::WeaponDefinition& weapon) const {
    return weapon_effect_sequence(weapon.water_explosion_gaf, weapon.water_explosion_art);
}

int32_t Match::effect_lcg_rand(void* match) {
    auto& seed = static_cast<Match*>(match)->lcg_seed_;
    seed = seed * 214013U + 2531011U;
    return static_cast<int32_t>((seed >> 16) & 0x7fffU);
}

uint32_t Match::effect_synced_rand(void* match, uint32_t bound) {
    return static_cast<Match*>(match)->random_.bounded(bound);
}

int32_t Match::effect_grid_height(void* match, const FixedVec3& position) {
    return static_cast<Match*>(match)->terrain_.height(position.x, position.z);
}

int32_t Match::effect_ground_height(void* match, const FixedVec3& position) {
    const auto& spatial = static_cast<Match*>(match)->spatial_;
    return sim::spatial_state::plot_mean_height_at(
        position.x,
        position.z,
        spatial.plots,
        static_cast<int32_t>(spatial.terrain_width),
        static_cast<int32_t>(spatial.terrain_height)
    );
}

sim::effect_particles::EffectHost Match::effect_host() noexcept {
    return {this, effect_lcg_rand, effect_grid_height, effect_ground_height, effect_synced_rand};
}

bool Match::loaded_primitives(
    const formats::objects3d::Model& model,
    uint32_t object,
    std::vector<sim::effect_particles::PiecePrimitive>& out,
    int32_t& selection_primitive
) const {
    if (!input_.loaded_primitives)
        return false;
    selection_primitive = input_.loaded_primitives(model, object, out);
    return true;
}

void Match::resolve_effect_sequences() {
    if (effect_sequences_resolved_ || !input_.effect_sequence)
        return;
    for (uint32_t i = 0; i < sim::effect_particles::fx_count; ++i) {
        auto& entry = effects_->fx[i];
        if (entry == nullptr)
            entry = input_.effect_sequence(
                fx_archive,
                sim::effect_particles::fx_name(static_cast<sim::effect_particles::Fx>(i))
            );
        effect_sequences_resolved_ = effect_sequences_resolved_ || entry != nullptr;
    }
}

// Frame storage for the explosion flash tiers; each pixel is literal.
void Match::allocate_flash_tiers() {
    for (int32_t tier = 0; tier < sim::effect_particles::flash_tier_count; ++tier) {
        const auto& shape = sim::effect_particles::flash_tier_shapes[tier];
        auto& sequence = flash_tiers_[tier];
        sequence.frames.resize(static_cast<size_t>(shape.count));
        for (int32_t index = 0; index < shape.count; ++index) {
            const auto side = std::max(sim::effect_particles::flash_frame_side(shape, index), 0);
            auto& frame = sequence.frames[static_cast<size_t>(index)];
            frame.pixels.resize(static_cast<size_t>(side) * static_cast<size_t>(side));
            frame.coverage.assign(frame.pixels.size(), 1);
        }
    }
}

size_t Match::particle_count() const noexcept {
    const auto& world = *effects_;
    return static_cast<size_t>(world.trail.live) + world.nano.live + world.flame.live +
           world.wake.live + world.smoke.live;
}

// The worker's nano piece, or its position without a script.
FixedVec3
Match::nano_nozzle(sim::unit_spawn::Slot& worker, const sim::simulation_state::Unit& fallback) {
    if (auto* model = instance(worker.unit_index))
        return fixed_point(model->query_nano_world());
    return fixed_point(worker.unit ? worker.unit->position : fallback.position);
}

void Match::spray_nano(
    sim::unit_spawn::Slot& worker, sim::simulation_state::Unit& target, NanoSpray kind
) {
    const auto nozzle = nano_nozzle(worker, target);
    auto low = fixed_point(target.position);
    auto high = low;
    if (const auto* bounds = bounds_for(target)) {
        low.x += bounds->bounds_min_x;
        if (kind == NanoSpray::build)
            low.y += bounds->bounds_min_y;
        low.z += bounds->bounds_min_z;
        high.x += bounds->bounds_max_x;
        high.y += bounds->model_height;
        high.z += bounds->bounds_max_z;
    }
    if (kind == NanoSpray::reclaim)
        sim::effect_particles::spawn_nano_inward(
            effects(),
            state().game,
            effect_host(),
            {low, high},
            nozzle,
            sim::effect_particles::layer_nano
        );
    else
        sim::effect_particles::spawn_nano_outward(
            effects(),
            state().game,
            effect_host(),
            nozzle,
            {low, high},
            sim::effect_particles::layer_nano
        );
}

void Match::spray_nano_feature(
    sim::unit_spawn::Slot& worker,
    int16_t cell_x,
    int16_t cell_z,
    const FeatureDef& feature,
    bool inward
) {
    if (!worker.unit)
        return;
    const auto nozzle = nano_nozzle(worker, *worker.unit);
    sim::effect_particles::Box box{};
    box.low.x = static_cast<int32_t>(cell_x) << cell_shift;
    box.low.z = static_cast<int32_t>(cell_z) << cell_shift;
    box.low.y =
        static_cast<int32_t>(static_cast<uint32_t>(terrain_.height(box.low.x, box.low.z)) << 16);
    box.high.x = box.low.x + (static_cast<int32_t>(feature.footprint_x) << cell_shift);
    box.high.y = box.low.y + (static_cast<int32_t>(static_cast<uint8_t>(feature.height)) << 16);
    box.high.z = box.low.z + (static_cast<int32_t>(feature.footprint_z) << cell_shift);
    if (inward)
        sim::effect_particles::spawn_nano_inward(
            effects(), state().game, effect_host(), box, nozzle, sim::effect_particles::layer_nano
        );
    else
        sim::effect_particles::spawn_nano_outward(
            effects(), state().game, effect_host(), nozzle, box, sim::effect_particles::layer_nano
        );
}

void Match::spawn_light_puff(const FixedVec3& position) {
    sim::effect_particles::spawn_white_smoke(
        effects(), state().game, effect_host(), position, sim::effect_particles::layer_smoke
    );
}

void Match::projectile_trail_effects(oa::Projectile& shot, int16_t previous_height) {
    auto& world = state();
    const auto* weapon = oa::world_weapon_def(&world, shot.def);
    const auto* definition = projectile_weapon(shot);
    if (weapon == nullptr || definition == nullptr)
        return;
    const auto tick = world.game.tick;
    if ((weapon->flags & OA_WEAPON_FLAG_SMOKE_TRAIL) != 0 && tick < shot.lifetime_tick &&
        shot.created_tick < tick) {
        spawn_light_puff(shot.position);
        shot.created_tick += static_cast<uint16_t>(weapon->smoke_delay);
    }
    const auto sea = static_cast<int16_t>(world.game.sea_level);
    const auto height = static_cast<int16_t>(static_cast<uint32_t>(shot.position.y) >> 16);
    if (!(sea < previous_height && height <= sea))
        return;
    const auto* plot = plot_at(shot.position.x, shot.position.z);
    if (plot == nullptr || plot->high_height >= world.game.sea_level ||
        effects_->no_sea_level_trigger)
        return;
    sim::effect_particles::log_explosion(
        effects(),
        world.game,
        effect_host(),
        shot.position,
        water_effect_sequence(*definition),
        0,
        true
    );
}

bool Match::spawn_weapon_explosion(
    const oa::WeaponDef& weapon, const std::array<uint32_t, 3>& position, bool struck_unit
) {
    const auto& art = input_.weapons.definition(oa::oa_weapon_def_id(&weapon));
    const auto point = fixed_point(position);
    // MapPlot.high_height below sea level under the burst.
    const auto* plot = plot_at(point.x, point.z);
    const bool underwater = plot != nullptr && plot->high_height < state().game.sea_level;
    // A nosealeveltrigger map drops a shot that goes off in the water without
    // striking a unit.
    if (effects_->no_sea_level_trigger && underwater && !struck_unit)
        return false;
    add_screen_shake(
        state().game, weapon.shake_magnitude, weapon.shake_magnitude, weapon.shake_duration
    );
    const auto host = effect_host();
    if (!underwater || struck_unit) {
        play_sound_at(art.soundhit.c_str(), point);
        if ((weapon.flags & OA_WEAPON_FLAG_END_SMOKE) != 0) {
            sim::effect_particles::spawn_white_smoke(
                effects(), state().game, host, point, sim::effect_particles::layer_smoke
            );
            // The puff alone, unless the display rules show the explosion too.
            if (!input_.display.end_smoke_explosion)
                return true;
        }
        sim::effect_particles::log_explosion(
            effects(),
            state().game,
            host,
            point,
            weapon_effect_sequence(art.explosion_gaf, art.explosion_art),
            0,
            underwater
        );
        return true;
    }
    play_sound_at(art.soundwater.c_str(), point);
    sim::effect_particles::log_explosion(
        effects(), state().game, host, point, water_effect_sequence(art), 0, true
    );
    return true;
}
} // namespace oa::sim::match_runtime

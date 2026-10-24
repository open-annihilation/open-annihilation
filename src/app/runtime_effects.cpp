// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/app/runtime.hpp"
#include "match_models.hpp"
#include "renderer_effect_sources.hpp"
#include "renderer_effect_events.hpp"
#include "renderer_weapon_style.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace oa::app {
namespace {
using renderer_effect_sources::Kind;
using renderer_effect_sources::Origin;
using renderer_effect_sources::Source;
constexpr float fixed_scale = 65536.0F;
constexpr float channel_scale = 255.0F;
constexpr float muzzle_radius = 52.0F;
constexpr float projectile_radius = 52.0F;
constexpr float impact_radius = 72.0F;
constexpr float wreck_radius = 64.0F;
constexpr float nano_radius = 48.0F;
constexpr float construction_radius = 72.0F;
constexpr std::size_t maximum_nano_lights = 24;
constexpr uint32_t impact_sprite_retention_ticks = 30;

struct Pulse {
    Source source;
    uint32_t birth{};
    uint32_t duration{renderer_effect_sources::muzzle_flash_ticks};
    bool emg{};
};

struct Flight {
    oa_ref32 def{}, source{};
    uint32_t created{}, lifetime{};
};

struct WreckIdentity {
    int16_t slot{};
    bool captured{};
};

/// Place a light at a canonical fixed-point location.
Source at(const oa::FixedVec3& point, Kind kind, float radius) {
    Source source;
    source.x = static_cast<float>(point.x) / fixed_scale;
    source.height = static_cast<float>(point.y) / fixed_scale;
    source.z = static_cast<float>(point.z) / fixed_scale;
    source.kind = kind;
    source.radius = radius;
    return source;
}

/// Recover fixed-point bits for a source's sight query.
std::array<uint32_t, 3> position(const Source& source) {
    return {
        static_cast<uint32_t>(static_cast<int32_t>(source.x * fixed_scale)),
        static_cast<uint32_t>(static_cast<int32_t>(source.height * fixed_scale)),
        static_cast<uint32_t>(static_cast<int32_t>(source.z * fixed_scale))
    };
}

/// Colour palette ramps directly, using warm light for physical ammunition and plasma sprites.
void weapon_colour(
    Source& source,
    const sim::combat_state::WeaponDefinition& weapon,
    const oa::PaletteBytes& palette
) {
    constexpr uint8_t plasma_render_type = 4;
    const auto index = static_cast<std::size_t>(weapon.color) * 4;
    source.red = palette[index] / channel_scale;
    source.green = palette[index + 1] / channel_scale;
    source.blue = palette[index + 2] / channel_scale;
    constexpr float minimum_colour = .15F;
    if (std::max({source.red, source.green, source.blue}) < minimum_colour || weapon.color == 0 ||
        weapon.rendertype == plasma_render_type) {
        source.red = 1;
        source.green = .58F;
        source.blue = .18F;
    }
}
} // namespace

struct Runtime::MatchEffects {
    const sim::match_runtime::Match* match{};
    renderer_effect_sources::Timeline timeline;
    std::vector<Flight> flights;
    std::vector<Pulse> pulses;
    std::vector<WreckIdentity> wreck_identity;
    std::vector<Source> sources;
    renderer_effect_events::Log events;
    std::vector<renderer_effect_events::Event> emg_impacts;
};

void Runtime::reset_match_effect_sources() {
    match_effects_.reset();
}

void Runtime::observe_match_effect_tick() noexcept try {
    if (!match_)
        return;
    if (!match_effects_ || match_effects_->match != match_.get()) {
        match_effects_ = std::make_shared<MatchEffects>();
        match_effects_->match = match_.get();
    }
    auto& state = *match_effects_;
    const uint32_t tick = match_->simulation().tick;
    const bool seen = state.timeline.seen;
    const auto previous_tick = state.timeline.tick;
    if (seen && tick == previous_tick && state.events.count == 0)
        return;
    const bool reset =
        renderer_effect_sources::observe(state.timeline, tick, match_->wrecks().size());
    if (reset) {
        state.pulses.clear();
        state.emg_impacts.clear();
        state.flights.clear();
        state.wreck_identity.clear();
    }
    state.wreck_identity.resize(match_->wrecks().size());
    std::erase_if(state.pulses, [tick](const Pulse& pulse) {
        // Keep the impact identity after its light fades so the remaining flash sprite
        // cannot recreate a larger generic light at the same place.
        const auto duration = pulse.emg && pulse.source.origin == Origin::impact
                                  ? impact_sprite_retention_ticks
                                  : pulse.duration;
        return renderer_effect_sources::fade(pulse.birth, tick, duration) == 0;
    });
    std::erase_if(state.emg_impacts, [tick](const renderer_effect_events::Event& event) {
        return event.tick > tick || tick - event.tick >= impact_sprite_retention_ticks;
    });
    const auto shots = match_->projectiles();
    std::vector<Flight> flights;
    flights.reserve(shots.size());
    const auto& world = match_->state();
    const auto viewer = match_view_player();
    const bool captured = state.events.count != 0;
    for (std::size_t index = 0; index < state.events.count; ++index) {
        const auto& event = state.events.events[index];
        if (viewer >= OA_PLAYER_COUNT || event.tick > tick)
            continue;
        auto source =
            at(event.position, Kind::weapon, event.impact ? impact_radius : muzzle_radius);
        sim::combat_state::WeaponDefinition colour;
        colour.color = event.colour;
        colour.rendertype = event.render_type;
        weapon_colour(source, colour, match_palette_);
        source.origin = event.impact ? Origin::impact : Origin::muzzle;
        source.intensity = event.impact ? 1.0F : .85F;
        source.heat = event.impact ? 1.0F : .65F;
        constexpr uint32_t impact_ticks = 18;
        uint32_t duration =
            event.impact ? impact_ticks : renderer_effect_sources::muzzle_flash_ticks;
        if (event.emg) {
            renderer_weapon_style::apply_emg_light(source);
            duration = event.impact ? renderer_weapon_style::emg_impact_ticks
                                    : renderer_weapon_style::emg_muzzle_ticks;
        }
        if (!match_->point_shown(viewer, position(source)))
            continue;
        if (event.emg && event.impact) {
            constexpr std::size_t maximum_retained_impacts = 1024;
            if (state.emg_impacts.size() == maximum_retained_impacts)
                state.emg_impacts.erase(state.emg_impacts.begin());
            state.emg_impacts.push_back(event);
            const auto nearby =
                std::find_if(state.pulses.begin(), state.pulses.end(), [&](Pulse& pulse) {
                    return pulse.emg && pulse.source.origin == Origin::impact &&
                           event.tick - pulse.birth < pulse.duration &&
                           renderer_weapon_style::merge_emg_impact(pulse.source, source);
                });
            if (nearby != state.pulses.end()) {
                nearby->birth = event.tick;
                continue;
            }
        }
        if (state.pulses.size() < renderer_effect_sources::maximum_sources)
            state.pulses.push_back({source, event.tick, duration, event.emg});
    }

    state.events.count = 0;
    for (const auto& shot : shots) {
        const auto* weapon = match_->projectile_weapon(shot);
        bool continued = false;
        if (shot.compact_index >= 0 &&
            static_cast<std::size_t>(shot.compact_index) < state.flights.size()) {
            const auto& prior = state.flights[static_cast<std::size_t>(shot.compact_index)];
            const auto* definition = oa::world_weapon_def(&world, shot.def);
            const bool smoking =
                definition && (definition->flags & OA_WEAPON_FLAG_SMOKE_TRAIL) != 0;
            continued = prior.def == shot.def && prior.source == shot.source &&
                        prior.lifetime == shot.lifetime_tick &&
                        (prior.created == shot.created_tick ||
                         (smoking && shot.created_tick >= prior.created));
        }
        if (!reset && !captured && tick - previous_tick == 1 && !continued && weapon &&
            shot.burst_remaining == 0 &&
            state.pulses.size() < renderer_effect_sources::maximum_sources) {
            auto source = at(shot.origin, Kind::weapon, muzzle_radius);
            weapon_colour(source, *weapon, match_palette_);
            source.origin = Origin::muzzle;
            source.intensity = .85F;
            source.heat = .65F;
            const auto* definition = oa::world_weapon_def(&world, shot.def);
            const bool emg = definition && renderer_weapon_style::is_emg(*definition);
            if (emg)
                renderer_weapon_style::apply_emg_light(source);
            if (viewer < OA_PLAYER_COUNT && match_->point_shown(viewer, position(source)))
                state.pulses.push_back(
                    {source,
                     tick,
                     emg ? renderer_weapon_style::emg_muzzle_ticks
                         : renderer_effect_sources::muzzle_flash_ticks,
                     emg}
                );
        }
        flights.push_back({shot.def, shot.source, shot.created_tick, shot.lifetime_tick});
    }
    state.flights = std::move(flights);
    // Retire before a reclaimed feature slot can be reused, even on ticks not rendered.
    const auto wrecks = match_->wrecks();
    std::unordered_set<uint64_t> occupied;
    for (std::size_t remaining = wrecks.size(); remaining > 0; --remaining) {
        const auto index = remaining - 1;
        const auto& wreck = wrecks[index];
        const auto* plot = oa::world_plot(&world, wreck.cell_x, wreck.cell_z);
        auto& identity = state.wreck_identity[index];
        bool present = plot && plot->feature == wreck.feature;
        if (present && (plot->flags & OA_PLOT_FLAG_ANIMATING_FEATURE) != 0) {
            const auto* record = sim::feature_runtime::feature_record(world, plot->feature_record);
            present = record && record->def_index == wreck.feature &&
                      record->cell_x == wreck.cell_x && record->cell_z == wreck.cell_z &&
                      static_cast<uint32_t>(record->model.position.x) == wreck.position[0] &&
                      static_cast<uint32_t>(record->model.position.z) == wreck.position[2];
            if (!identity.captured) {
                identity.slot = plot->feature_record;
                identity.captured = true;
            } else if (identity.slot != plot->feature_record) {
                present = false;
            }
        } else if (identity.captured) {
            present = false;
        }
        // A later corpse at the same cell owns the feature even if both use identical art.
        const auto cell = (static_cast<uint64_t>(static_cast<uint32_t>(wreck.cell_x)) << 32) |
                          static_cast<uint32_t>(wreck.cell_z);
        if (!occupied.insert(cell).second)
            present = false;
        (void)renderer_effect_sources::wreck_heat(state.timeline, index, present, false);
    }
} catch (...) {
    // Optional presentation storage cannot abort a simulation step.
    match_effects_.reset();
}

renderer_effect_events::Capture Runtime::capture_match_effect_events() noexcept {
    observe_match_effect_tick();
    if (!match_ || !match_effects_)
        return {};
    return renderer_effect_events::Capture(
        match_->event_hooks,
        match_effects_->events,
        match_effects_,
        this,
        [](void* context, const sim::match_runtime::EventHooks* hooks) noexcept {
            const auto& runtime = *static_cast<Runtime*>(context);
            return runtime.match_ && &runtime.match_->event_hooks == hooks;
        }
    );
}

bool Runtime::emg_impact_at(const oa::FixedVec3& point) const noexcept {
    if (!match_ || !match_effects_ || match_effects_->match != match_.get())
        return false;
    const uint32_t tick = match_->simulation().tick;
    constexpr int64_t contact_tolerance = 6 * 65536;
    return std::any_of(
        match_effects_->emg_impacts.begin(),
        match_effects_->emg_impacts.end(),
        [&](const renderer_effect_events::Event& event) {
            return event.tick <= tick && tick - event.tick < impact_sprite_retention_ticks &&
                   std::abs(static_cast<int64_t>(event.position.x) - point.x) <=
                       contact_tolerance &&
                   std::abs(static_cast<int64_t>(event.position.y) - point.y) <=
                       contact_tolerance &&
                   std::abs(static_cast<int64_t>(event.position.z) - point.z) <= contact_tolerance;
        }
    );
}

bool Runtime::emg_smoke_at(const oa::FixedVec3& point) const noexcept {
    if (!match_ || !match_effects_ || match_effects_->match != match_.get())
        return false;
    const uint32_t tick = match_->simulation().tick;
    constexpr int64_t smoke_radius = 24 * 65536;
    constexpr int64_t smoke_height = 64 * 65536;
    constexpr int64_t ground_tolerance = 8 * 65536;
    return std::any_of(
        match_effects_->emg_impacts.begin(),
        match_effects_->emg_impacts.end(),
        [&](const renderer_effect_events::Event& event) {
            const int64_t dx = int64_t{point.x} - event.position.x;
            const int64_t dz = int64_t{point.z} - event.position.z;
            const int64_t height = int64_t{point.y} - event.position.y;
            return event.tick <= tick && tick - event.tick < impact_sprite_retention_ticks &&
                   std::abs(dx) <= smoke_radius && std::abs(dz) <= smoke_radius &&
                   dx * dx + dz * dz <= smoke_radius * smoke_radius &&
                   height >= -ground_tolerance && height <= smoke_height;
        }
    );
}

std::span<const renderer_effect_sources::Source> Runtime::match_effect_sources() const noexcept {
    return match_effects_ ? std::span<const Source>(match_effects_->sources)
                          : std::span<const Source>{};
}

std::span<const renderer_effect_sources::Source> Runtime::collect_match_effect_sources() {
    observe_match_effect_tick();
    if (!match_ || !match_effects_)
        return {};
    auto& state = *match_effects_;
    auto& sources = state.sources;
    sources.clear();
    const auto& world = match_->state();
    const auto viewer = match_view_player();
    const uint32_t tick = match_->simulation().tick;
    if (viewer >= OA_PLAYER_COUNT)
        return {};
    constexpr std::array<std::size_t, 6> budgets{16, 16, 16, 24, 24, 24};
    std::array<std::size_t, 6> counts{};
    const auto append = [&](const Source& source) {
        const auto origin = static_cast<std::size_t>(source.origin);
        if (counts[origin] < budgets[origin] &&
            sources.size() < renderer_effect_sources::maximum_sources &&
            match_->point_shown(viewer, position(source))) {
            sources.push_back(source);
            ++counts[origin];
        }
    };
    for (auto it = state.pulses.rbegin(); it != state.pulses.rend(); ++it) {
        const auto& pulse = *it;
        auto source = pulse.source;
        const auto strength = renderer_effect_sources::fade(pulse.birth, tick, pulse.duration);
        if (strength == 0)
            continue;
        source.intensity *= strength;
        source.heat *= strength;
        append(source);
    }
    const auto* models = drawn_match_models();
    const auto shots = match_->projectiles();
    for (std::size_t index = 0; index < shots.size(); ++index) {
        const auto& shot = shots[index];
        const auto* weapon = match_->projectile_weapon(shot);
        if (!weapon || shot.burst_remaining != 0 || (shot.flags & OA_PROJECTILE_FLAG_RETIRED) != 0)
            continue;
        const auto shown =
            models
                ? presented_shot(
                      models->presentation.shots, tick, index, shot, models->presentation.fraction
                  )
                      .position
                : shot.position;
        auto source = at(shown, Kind::weapon, projectile_radius);
        weapon_colour(source, *weapon, match_palette_);
        source.origin = Origin::projectile;
        source.intensity = .55F;
        source.heat = .4F;
        const auto* definition = oa::world_weapon_def(&world, shot.def);
        if (definition && renderer_weapon_style::is_emg(*definition))
            renderer_weapon_style::apply_emg_light(source);
        append(source);
    }
    const auto& effects = match_->effects();
    for (int32_t remaining = effects.explosion_count; remaining > 0; --remaining) {
        const auto& explosion = effects.explosions[remaining - 1];
        if (!explosion.flash.active())
            continue;
        auto source = at(explosion.position, Kind::weapon, impact_radius);
        const bool captured_impact =
            std::any_of(state.pulses.begin(), state.pulses.end(), [&](const Pulse& pulse) {
                const float contact_tolerance =
                    pulse.emg ? renderer_weapon_style::emg_impact_merge_radius : 8;
                return pulse.source.origin == Origin::impact &&
                       std::abs(pulse.source.x - source.x) <= contact_tolerance &&
                       std::abs(pulse.source.height - source.height) <= contact_tolerance &&
                       std::abs(pulse.source.z - source.z) <= contact_tolerance;
            });
        if (captured_impact)
            continue;
        const auto frames = explosion.flash.sequence->frames.size();
        const auto frame = explosion.flash.frame_index;
        const float flash_strength =
            frames > frame ? 1.0F - static_cast<float>(frame) / static_cast<float>(frames) : 0;
        source.red = 1;
        source.green = .5F;
        source.blue = .12F;
        source.origin = Origin::impact;
        source.intensity = flash_strength;
        source.heat = flash_strength;
        append(source);
    }
    const auto wrecks = match_->wrecks();
    for (std::size_t remaining = wrecks.size(); remaining > 0; --remaining) {
        const auto index = remaining - 1;
        const auto& wreck = wrecks[index];
        const float strength =
            renderer_effect_sources::wreck_heat(state.timeline, index, true, true);
        if (strength == 0)
            continue;
        oa::FixedVec3 point{
            static_cast<int32_t>(wreck.position[0]),
            static_cast<int32_t>(wreck.position[1]),
            static_cast<int32_t>(wreck.position[2])
        };
        const auto* plot = oa::world_plot(&world, wreck.cell_x, wreck.cell_z);
        if (plot && (plot->flags & OA_PLOT_FLAG_ANIMATING_FEATURE) != 0)
            if (const auto* record =
                    sim::feature_runtime::feature_record(world, plot->feature_record))
                point = record->model.position;
        auto source = at(point, Kind::wreck, wreck_radius);
        constexpr float wreck_emission_height = 8.0F;
        source.height += wreck_emission_height;
        source.red = 1;
        source.green = .12F + .4F * strength;
        source.blue = .035F * strength;
        source.origin = Origin::wreck;
        source.intensity = .6F * strength;
        source.glow = .8F * strength;
        source.heat = strength;
        append(source);
    }
    if (models) {
        std::size_t count = 0;
        const auto& particles = models->draws.spatial_particles;
        const auto stride = std::max<std::size_t>(1, particles.size() / maximum_nano_lights);
        for (std::size_t index = 0; index < particles.size() && count < maximum_nano_lights;
             index += stride) {
            const auto& particle = particles[index];
            if (!particle.emissive)
                continue;
            auto source = at(particle.position, Kind::nanolathe, nano_radius);
            source.red = .1F;
            source.green = 1;
            source.blue = .24F;
            source.origin = Origin::nano_stream;
            source.intensity = .18F;
            append(source);
            ++count;
        }
        for (const auto& model : models->draws.models) {
            if (model.stand_in >= 0 || !model.model.unit || model.model.unit->build_remaining <= 0)
                continue;
            auto source =
                at(model.shown_position.value_or(model.model.unit->position),
                   Kind::nanolathe,
                   construction_radius);
            source.red = .12F;
            source.green = 1;
            source.blue = .25F;
            source.origin = Origin::construction;
            const float pulse = renderer_effect_sources::construction_pulse(tick);
            source.intensity = .35F * pulse;
            source.glow = .12F * model.model.unit->build_remaining * pulse;
            append(source);
        }
    }
    return sources;
}

} // namespace oa::app

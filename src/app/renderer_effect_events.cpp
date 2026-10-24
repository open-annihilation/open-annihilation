// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "renderer_effect_events.hpp"
#include "renderer_weapon_style.hpp"

namespace oa::app::renderer_effect_events {

Capture::Capture(
    sim::match_runtime::EventHooks& hooks,
    Log& log,
    std::shared_ptr<void> storage,
    void* context,
    bool (*alive)(void*, const sim::match_runtime::EventHooks*) noexcept
) noexcept {
    if (hooks.shot_placed == &Capture::shot_placed)
        return;
    installed_ = &hooks;
    previous_ = hooks;
    log_ = &log;
    storage_ = std::move(storage);
    lifetime_context_ = context;
    alive_ = alive;
    hooks = {
        this, unit_created, unit_finished, shot_placed, shot_detonated, unit_damaged, unit_died
    };
}

Capture::~Capture() {
    if (installed_ && alive_ && alive_(lifetime_context_, installed_) &&
        installed_->context == this)
        *installed_ = previous_;
}

void Capture::note(
    const oa::World& world, const oa::Projectile& shot, bool impact, uint16_t target
) noexcept {
    if (log_->count >= Log::capacity)
        return;
    const auto* weapon = oa::world_weapon_def(&world, shot.def);
    if (!weapon)
        return;
    const auto* source = oa::world_unit(&world, shot.source);
    const auto* definition = source ? oa::world_unit_def_of(&world, source) : nullptr;
    const bool naval = definition && (definition->flags & OA_UNIT_DEF_FLAG_FLOATER) != 0;
    const auto* victim = oa::world_unit_at(&world, target);
    const auto* victim_def = target && victim ? oa::world_unit_def_of(&world, victim) : nullptr;
    const bool naval_target = victim_def && (victim_def->flags & OA_UNIT_DEF_FLAG_FLOATER) != 0;
    const bool water_weapon = (weapon->flags & OA_WEAPON_FLAG_WATER_WEAPON) != 0;
    const int64_t sea = int64_t(world.game.sea_level) * 65536;
    const bool crossed_surface = impact && target == 0 && shot.velocity.y < 0 &&
                                 int64_t(shot.position.y) < sea &&
                                 int64_t(shot.position.y) - shot.velocity.y >= sea;
    log_->events[log_->count++] = {
        impact ? shot.position : shot.origin,
        world.game.tick,
        static_cast<uint8_t>(weapon->color),
        impact,
        static_cast<uint8_t>(weapon->render_type),
        renderer_weapon_style::is_emg(*weapon),
        naval,
        naval_target,
        target != 0,
        water_weapon,
        crossed_surface
    };
}

void Capture::unit_created(void* context, const oa::World& world, uint16_t unit) {
    auto& self = *static_cast<Capture*>(context);
    if (self.previous_.unit_created)
        self.previous_.unit_created(self.previous_.context, world, unit);
}

void Capture::unit_finished(
    void* context, const oa::World& world, uint16_t unit, uint16_t builder
) {
    auto& self = *static_cast<Capture*>(context);
    if (self.previous_.unit_finished)
        self.previous_.unit_finished(self.previous_.context, world, unit, builder);
}

void Capture::shot_placed(
    void* context,
    const oa::World& world,
    const oa::Projectile& shot,
    sim::match_runtime::ShotSource source,
    const oa::FixedVec3* aim,
    uint16_t target
) {
    auto& self = *static_cast<Capture*>(context);
    if (source != sim::match_runtime::ShotSource::meteor && shot.burst_remaining == 0)
        self.note(world, shot, false);
    if (self.previous_.shot_placed)
        self.previous_.shot_placed(self.previous_.context, world, shot, source, aim, target);
}

void Capture::shot_detonated(
    void* context, const oa::World& world, const oa::Projectile& shot, uint16_t target
) {
    auto& self = *static_cast<Capture*>(context);
    self.note(world, shot, true, target);
    if (self.previous_.shot_detonated)
        self.previous_.shot_detonated(self.previous_.context, world, shot, target);
}

void Capture::unit_damaged(
    void* context,
    const oa::World& world,
    uint16_t target,
    uint16_t source,
    int16_t amount,
    uint8_t kind
) {
    auto& self = *static_cast<Capture*>(context);
    if (self.previous_.unit_damaged)
        self.previous_.unit_damaged(self.previous_.context, world, target, source, amount, kind);
}

void Capture::unit_died(
    void* context,
    const oa::World& world,
    uint16_t unit,
    const sim::match_runtime::KillOutcome& outcome,
    bool elsewhere
) {
    auto& self = *static_cast<Capture*>(context);
    if (self.previous_.unit_died)
        self.previous_.unit_died(self.previous_.context, world, unit, outcome, elsewhere);
}

} // namespace oa::app::renderer_effect_events

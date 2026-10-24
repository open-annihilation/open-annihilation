// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Event capture preserves existing observers, lifetime, nesting and same-tick shot colours.
#include "renderer_effect_events.hpp"
#include "renderer_weapon_style.hpp"
#include "oa/sim/match_runtime.hpp"
#include "oa/test/check.hpp"

#include <memory>
#include <cstring>
#include <stdexcept>

namespace {
using namespace oa::app::renderer_effect_events;
using oa::sim::match_runtime::EventHooks;

struct Owner {
    EventHooks hooks;
    bool alive{true};
    int calls[6]{};
};

/// Check the owning match before reading installed hooks.
bool alive(void* context, const EventHooks* hooks) noexcept {
    const auto& owner = *static_cast<Owner*>(context);
    return owner.alive && hooks == &owner.hooks;
}

/// Install probes for every pre-existing recorder callback.
void probes(Owner& owner) {
    owner.hooks.context = &owner;
    owner.hooks.unit_created = [](void* p, const oa::World&, uint16_t unit) {
        OA_CHECK(unit == 2);
        ++static_cast<Owner*>(p)->calls[0];
    };
    owner.hooks.unit_finished = [](void* p, const oa::World&, uint16_t unit, uint16_t builder) {
        OA_CHECK(unit == 2 && builder == 3);
        ++static_cast<Owner*>(p)->calls[1];
    };
    owner.hooks.shot_placed = [](void* p,
                                 const oa::World&,
                                 const oa::Projectile&,
                                 oa::sim::match_runtime::ShotSource,
                                 const oa::FixedVec3* aim,
                                 uint16_t target) {
        OA_CHECK(aim && aim->x == 99 && target == 2);
        ++static_cast<Owner*>(p)->calls[2];
    };
    owner.hooks.shot_detonated =
        [](void* p, const oa::World&, const oa::Projectile&, uint16_t target) {
            OA_CHECK(target == 2);
            ++static_cast<Owner*>(p)->calls[3];
        };
    owner.hooks.unit_damaged = [](void* p,
                                  const oa::World&,
                                  uint16_t target,
                                  uint16_t source,
                                  int16_t amount,
                                  uint8_t kind) {
        OA_CHECK(target == 2 && source == 3 && amount == 4 && kind == 1);
        ++static_cast<Owner*>(p)->calls[4];
    };
    owner.hooks.unit_died = [](void* p,
                               const oa::World&,
                               uint16_t unit,
                               const oa::sim::match_runtime::KillOutcome&,
                               bool elsewhere) {
        OA_CHECK(unit == 2 && elsewhere);
        ++static_cast<Owner*>(p)->calls[5];
    };
}
} // namespace

int main() {
    using namespace oa::app;
    oa::WeaponDef definition{};
    for (const char* key : {"EMG", "emg2", "eMg", "VTOL_EMG"}) {
        std::strcpy(definition.key, key);
        OA_CHECK(renderer_weapon_style::is_emg(definition));
    }
    for (const char* key : {"", "EMG_LARGE", "EMG3", "PLASMA"}) {
        std::strcpy(definition.key, key);
        OA_CHECK(!renderer_weapon_style::is_emg(definition));
    }
    renderer_effect_sources::Source impact;
    impact.origin = renderer_effect_sources::Origin::impact;
    renderer_weapon_style::apply_emg_light(impact);
    OA_CHECK(impact.red > impact.green && impact.green > impact.blue);
    OA_CHECK(impact.heat == 0);
    auto hotspot = impact;
    for (int index = 0; index < 100; ++index)
        OA_CHECK(renderer_weapon_style::merge_emg_impact(hotspot, impact));
    OA_CHECK(hotspot.intensity <= .85F && hotspot.heat <= .3F);
    OA_CHECK(hotspot.intensity > impact.intensity && hotspot.heat > 0);
    auto distant = impact;
    distant.x = 40;
    OA_CHECK(!renderer_weapon_style::merge_emg_impact(hotspot, distant));
    distant = impact;
    distant.height = 30;
    OA_CHECK(!renderer_weapon_style::merge_emg_impact(hotspot, distant));
    for (auto origin :
         {renderer_effect_sources::Origin::projectile, renderer_effect_sources::Origin::muzzle}) {
        auto source = impact;
        source.origin = origin;
        source.heat = 1;
        renderer_weapon_style::apply_emg_light(source);
        OA_CHECK(source.heat == 0 && source.radius < impact.radius);
    }

    auto world = std::make_unique<oa::World>();
    world->game.tick = 42;
    world->game.weapon_defs[0].color = 17;
    world->game.weapon_defs[0].render_type = 4;
    std::strcpy(world->game.weapon_defs[0].key, "VTOL_EMG");
    oa::Projectile shot{};
    shot.def = 1;
    shot.origin = {10, 20, 30};
    shot.position = {40, 50, 60};
    oa::FixedVec3 aim{99, 0, 0};
    oa::sim::match_runtime::KillOutcome outcome;
    auto log = std::make_shared<Log>();
    Owner owner;
    probes(owner);
    const auto prior = owner.hooks;
    {
        Capture capture(owner.hooks, *log, log, &owner, alive);
        Capture nested(owner.hooks, *log, log, &owner, alive);
        const auto& hooks = owner.hooks;
        hooks.unit_created(hooks.context, *world, 2);
        hooks.unit_finished(hooks.context, *world, 2, 3);
        hooks.shot_placed(
            hooks.context, *world, shot, oa::sim::match_runtime::ShotSource::weapon, &aim, 2
        );
        hooks.shot_detonated(hooks.context, *world, shot, 2);
        hooks.unit_damaged(hooks.context, *world, 2, 3, 4, 1);
        hooks.unit_died(hooks.context, *world, 2, outcome, true);
        for (int calls : owner.calls)
            OA_CHECK(calls == 1);
        OA_CHECK(log->count == 2);
        OA_CHECK(log->events[0].position.x == 10 && !log->events[0].impact);
        OA_CHECK(log->events[1].position.x == 40 && log->events[1].impact);
        OA_CHECK(log->events[1].colour == 17 && log->events[1].tick == 42);
        OA_CHECK(log->events[0].emg && log->events[1].emg);
        OA_CHECK(log->events[1].render_type == 4);
        for (std::size_t i = 0; i < Log::capacity + 10; ++i)
            hooks.shot_detonated(hooks.context, *world, shot, 2);
        OA_CHECK(log->count == Log::capacity);
    }
    OA_CHECK(owner.hooks.context == prior.context && owner.hooks.shot_placed == prior.shot_placed);
    owner.hooks.unit_created = [](void*, const oa::World&, uint16_t) {
        throw std::runtime_error("recorder");
    };
    bool caught = false;
    try {
        Capture capture(owner.hooks, *log, log, &owner, alive);
        owner.hooks.unit_created(owner.hooks.context, *world, 2);
    } catch (const std::runtime_error&) {
        caught = true;
    }
    OA_CHECK(caught && owner.hooks.context == prior.context);
    {
        Capture capture(owner.hooks, *log, log, &owner, alive);
        log.reset(); // presentation reset still leaves queue alive and hooks restorable
    }
    OA_CHECK(owner.hooks.context == prior.context);
    log = std::make_shared<Log>();
    {
        Capture capture(owner.hooks, *log, log, &owner, alive);
        owner.alive = false;
        owner.hooks = {}; // match destruction or replacement
    }
    OA_CHECK(owner.hooks.context == nullptr);
    return oa::test::check_exit_status();
}

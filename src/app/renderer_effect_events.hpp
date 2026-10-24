// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include "oa/sim/match_runtime/event_hooks.hpp"
#include <array>
#include <cstddef>
#include <memory>

namespace oa::app::renderer_effect_events {

/// Keep only presentation data from a shot while its canonical record is valid.
struct Event {
    oa::FixedVec3 position{};
    uint32_t tick{};
    uint8_t colour{};
    bool impact{};
    uint8_t render_type{};
    bool emg{};
};

/// Bound storage so callbacks neither allocate nor call back into the match.
struct Log {
    static constexpr std::size_t capacity = 256;
    std::array<Event, capacity> events{};
    std::size_t count{};
};

/// Forward every existing observer while retaining shot events for one simulation step.
class Capture {
  public:

    /// Make an inactive capture when no match can be observed.
    Capture() noexcept = default;
    /// Install forwarding callbacks until this capture ends; nested captures are inactive.
    /// @param hooks current match event callbacks, restored on destruction
    /// @param log bounded visual event queue
    /// @param storage keeps the event queue alive while callbacks refer to it
    /// @param context passed to the lifetime query
    /// @param alive checks whether hooks still belong to the current match without dereferencing them
    Capture(
        sim::match_runtime::EventHooks& hooks,
        Log& log,
        std::shared_ptr<void> storage,
        void* context,
        bool (*alive)(void*, const sim::match_runtime::EventHooks*) noexcept
    ) noexcept;
    /// Restore callbacks unless their match has already been destroyed.
    ~Capture();
    /// Disallow copying because callback context points at this capture.
    Capture(const Capture&) = delete;
    /// Disallow assignment because callback context points at this capture.
    Capture& operator=(const Capture&) = delete;

  private:

    sim::match_runtime::EventHooks* installed_{};
    sim::match_runtime::EventHooks previous_{};
    Log* log_{};
    std::shared_ptr<void> storage_;
    void* lifetime_context_{};
    bool (*alive_)(void*, const sim::match_runtime::EventHooks*) noexcept {};

    /// Append a shot without allocating, retaining its palette colour before retirement.
    void note(const oa::World& world, const oa::Projectile& shot, bool impact) noexcept;
    /// Forward a unit creation.
    static void unit_created(void* context, const oa::World& world, uint16_t unit);
    /// Forward a completed unit.
    static void
    unit_finished(void* context, const oa::World& world, uint16_t unit, uint16_t builder);
    /// Retain a muzzle and forward the original placement callback.
    static void shot_placed(
        void* context,
        const oa::World& world,
        const oa::Projectile& shot,
        sim::match_runtime::ShotSource source,
        const oa::FixedVec3* aim,
        uint16_t target
    );
    /// Retain a coloured impact and forward the original detonation callback.
    static void shot_detonated(
        void* context, const oa::World& world, const oa::Projectile& shot, uint16_t target
    );
    /// Forward damage without changing its arguments.
    static void unit_damaged(
        void* context,
        const oa::World& world,
        uint16_t target,
        uint16_t source,
        int16_t amount,
        uint8_t kind
    );
    /// Forward a death without changing its outcome.
    static void unit_died(
        void* context,
        const oa::World& world,
        uint16_t unit,
        const sim::match_runtime::KillOutcome& outcome,
        bool elsewhere
    );
};

} // namespace oa::app::renderer_effect_events

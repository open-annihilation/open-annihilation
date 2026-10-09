// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What the settings dialog says of the renderer: the Hardware acceleration
// row's status and whether nothing in the game could help the run, and
// whether Vertical sync is out of reach, from what the run knows of its
// renderer, its machine and the match. The graphics card scales the frames
// only with a renderer able to and 2 GiB of memory, and draws the
// battlefield, in the Full tier, only where Full is asked for and nothing
// has kept it to Basic; every other run draws as the game always has.
#pragma once

#include "oa/app/render_policy.hpp"
#include "oa/ui/engine_settings/dialog.hpp"

#include <cstdint>
#include <optional>

namespace oa::app {

/// What the renderer records hold that keeps the graphics card from
/// scaling the frames.
enum class RecordedTrouble : uint8_t {
    none,    ///< nothing
    failure, ///< the graphics driver failed while the game ran
    stopped, ///< the game stopped while using it: a crash or a hang
};

/// Why the Basic tier draws where Full was asked for, which the status's
/// first line says while the graphics card scales the frames.
enum class FullShortfall : uint8_t {
    none,          ///< nothing keeps Full to Basic
    lacks_feature, ///< Full's function test failed or the card cannot make Full's pages
    failed_before, ///< a full-unusable record stands against the driver
    stopped, ///< a call of the Full tier's own failed in this run, or the card refused a frame
    /// The memory guard dropped Full in this run, which setting the setting
    /// to Off and back does not lift.
    too_little_memory,
    cannot_save,          ///< Full's trial record could not be written
    waiting_for_game_end, ///< a shared game or a replay began without Full
};

/// What the run knows of whether the graphics card could scale its frames:
/// the facts the Hardware acceleration row's status and the renderer's
/// locks follow from.
struct AccelerationFacts {
    /// The level asked for: a flag decides, else the Hardware acceleration
    /// setting in effect (hardware_acceleration_asked). Basic and Full ask
    /// for the graphics card.
    oa::ui::engine_settings::HardwareAcceleration asked{
        oa::ui::engine_settings::HardwareAcceleration::off
    };
    /// The level a flag names (Options::hardware_acceleration); empty for
    /// no flag.
    std::optional<oa::ui::engine_settings::HardwareAcceleration> flag{};
    /// --force-capable: the renderer counts as able, and neither the
    /// environment's driver nor SDL's software renderer locks a row.
    bool force_capable{};
    /// SDL_RENDER_DRIVER names a render driver, or the video driver draws no
    /// window (dummy or offscreen).
    bool environment_driver{};
    uint64_t physical_memory{}; ///< bytes; 0 when the system does not say
    /// Whether the renderer was found able to scale and compose the frames;
    /// empty until something has looked at it.
    std::optional<bool> renderer_capable{};
    /// SDL's own software renderer presents the frames: never able, whatever
    /// else is known.
    bool software_renderer{};
    /// The renderer was found unable because it lacks something the
    /// graphics card's scaling needs: a texture limit under 1024, or a
    /// start-up test that drew a known pattern wrongly. Otherwise an unable
    /// renderer has no usable graphics card.
    bool lacks_feature{};
    /// The start-up function test drew a known pattern wrongly: the
    /// renderer is unable, --force-capable or not.
    bool function_test_failed{};
    /// The graphics card stopped scaling the frames for the rest of the run
    /// after a call only it makes failed, or after the renderer failed and
    /// was made again.
    bool driver_failed{};
    /// The memory guard stopped the graphics card scaling the frames for the
    /// rest of the run, which setting the setting to Off and back does not
    /// lift.
    bool memory_dropped{};
    /// The graphics card stopped scaling the frames for the rest of the run
    /// after an error of the game's own, which says nothing of the driver.
    bool engine_error{};
    /// Each change of the wait for the display resets the renderer's device,
    /// and the game does not recover a device such a reset leaves lost.
    bool vertical_sync_resets_device{};
    /// The renderer refused, in this run, to wait for the display.
    bool vertical_sync_refused{};
    bool shared_game{};      ///< a match played with other machines is under way
    bool replay{};           ///< a recorded game is being played back
    bool tier_accelerated{}; ///< the graphics card scales the frames now
    /// The graphics card draws the battlefield now: the Full tier, which
    /// counts as scaling the frames too.
    bool tier_full{};
    /// Why Basic draws where Full was asked for; none while Full draws or
    /// Basic was asked for.
    FullShortfall full_shortfall{FullShortfall::none};
    /// Full's anti-aliasing while it draws: the samples a pixel across the
    /// world target draws with, 1 for none (Runtime::full_supersample),
    /// which the status's second line names (AccelerationStatus::supersample).
    uint8_t full_supersample{1};
    /// The graphics card started at the lowest budget, where nothing smooths
    /// the zoomed-out view.
    bool no_smoothing{};
    /// What the graphics card does on this machine while it is in use.
    oa::ui::engine_settings::AccelerationReach reach{
        oa::ui::engine_settings::AccelerationReach::menus
    };
    /// A record skipped a graphics driver that failed before, at this start.
    bool driver_skipped{};
    /// The records that skipped drivers at this start have been cleared
    /// since, so that the next start tries those drivers again.
    bool skipped_cleared{};
    /// What the records hold against the renderer's own driver: that the
    /// graphics card could not be used with it.
    RecordedTrouble recorded{RecordedTrouble::none};
    /// What the records hold against the drivers skipped before it.
    RecordedTrouble skipped_recorded{RecordedTrouble::none};
    /// The renderer records could not be read after a run that did not end
    /// cleanly, which keeps the start on the processor.
    bool records_unreadable{};
    /// A trial record that guards the graphics card's first use could not
    /// be written, so the card was not tried.
    bool trial_unwritten{};
    /// How many times finer than the window, along each axis, the graphics
    /// card drew the battlefield in the last Full frame: the world target's
    /// factor in use, which the texture limit and the memory guard may hold
    /// under the row's (Runtime::full_supersample); 0 while frames are not
    /// drawn in Full (AccelerationStatus::full_supersample).
    uint8_t full_supersample_drawn{};
};

/// The Hardware acceleration row's status and the renderer's locks, as the
/// settings dialog shows them.
struct AccelerationReport {
    oa::ui::engine_settings::AccelerationStatus status{}; ///< the row's two status lines
    /// Nothing in the game could have the graphics card scale this run's
    /// frames (GameState::acceleration_unavailable).
    bool acceleration_unavailable{};
    /// The renderer cannot wait for the display
    /// (GameState::vertical_sync_unavailable).
    bool vertical_sync_unavailable{};

    friend bool operator==(const AccelerationReport&, const AccelerationReport&) = default;
};

/// Tells whether a machine has 2 GiB, the memory the graphics card's
/// scaling needs: the render policy's threshold,
/// render_policy::smallest_accelerated_memory, 1.75 GiB as the system
/// reports it, so that a machine sold with 2 GB counts. Memory the system
/// does not report counts as less.
///
/// @param physical_memory bytes; 0 when the system does not say
/// @return true at render_policy::smallest_accelerated_memory or more
[[nodiscard]] bool enough_memory_for_acceleration(uint64_t physical_memory) noexcept;

/// Reports what the settings dialog says of the renderer.
///
/// The status is the first that applies: under 2 GiB, whatever the setting
/// or the flags, saying so whether a record skipped a driver; Off, by
/// either, with a driver a record skipped and still holds; Off by a flag;
/// Off by the setting; then, with Basic or Full asked for, an environment
/// that names a driver (unless a flag asks for the card or
/// --force-capable); a shared game or a replay, where a renderer not found
/// unable waits for the match to end, naming the level that then takes
/// effect; a driver that failed in this run; the memory guard's drop in
/// this run; an error of the game's own in this run; a record against the
/// renderer's driver (unless a flag asks for the card), or records that
/// could not be read after an unclean exit, as a failure or as the game
/// having stopped; a renderer found unable after drivers skipped by
/// records, as their records say, or from the next start once those are
/// cleared; a renderer found unable, SDL's software renderer among them
/// (unless --force-capable), as lacking a feature or as no usable graphics
/// card; a trial that could not be written; the step-down's last rung in
/// this run, as frames too slow; while the graphics card scales the
/// frames, Full asked for, which the game cannot draw yet, so Basic is in
/// use in its place; else in use, on another driver where a record skipped
/// one, else with no smoothing where it started at the lowest budget; and
/// otherwise from the next start, which a renderer not yet looked at
/// shows. Acceleration is out of reach on the environment's
/// driver (unless a flag asks for the card or --force-capable), under
/// 2 GiB, or on a renderer found unable (unless --force-capable, which
/// never lifts a failed function test), unless a record skipped a driver
/// at this start or, on an unable renderer, a driver failed in this run; a
/// renderer not yet looked at, a record, and a trial that could not be
/// written leave it within reach, so that the row can try again. While the
/// graphics card draws the battlefield the status is Full in use; while
/// it scales the frames with Full asked for, the status says why Basic
/// draws instead
/// (FullShortfall), in a replay saying so for a shared game's wait, or,
/// where nothing keeps Full to Basic, what the card does as for Basic.
/// Vertical sync is out of reach on SDL's software renderer (unless
/// --force-capable), on a renderer whose device each change resets, and
/// once the renderer refused it.
///
/// @param facts what the run knows
/// @return the status and the locks' facts
[[nodiscard]] AccelerationReport report_acceleration(const AccelerationFacts& facts) noexcept;

/// Returns what the facts the tier is decided from say of the renderer and
/// the run, as the settings dialog's status reads them: the level asked
/// for, by the flag, else by the setting; the flag; --force-capable; the
/// environment's driver or a video driver that draws no window; the
/// machine's memory; the renderer, where there is one, able when probe
/// items 1 to 3 found it capable and the function test did not fail, and
/// lacking a feature for a small texture limit or a failed function test;
/// whether the function test failed;
/// SDL's software renderer; a drop after a driver failure, the memory
/// guard or an error of the game's own; where
/// the records live on disk, a trial that could not be written and records
/// that could not be read after an unclean start; a shared game or a
/// replay the tier waits for; whether the graphics card scales the frames
/// now, and whether it draws the battlefield; where Full was asked for and
/// Basic draws, why (FullShortfall), in the order the tier is decided:
/// its function test failed, a full-unusable
/// record, its drop by a card failure, the memory guard or a trial that
/// could not be written, or a shared game or a replay begun without it;
/// and what the card does at the rung. Vertical sync's facts, Full's
/// anti-aliasing in use and what the records hold against drivers are
/// left for the caller.
///
/// @param inputs the facts the tier is decided from
/// @param rung the rung the accelerated tier draws at
/// @param tier the tier drawing now: standard, accelerated (Basic) or full
/// @return the facts
[[nodiscard]] AccelerationFacts tier_acceleration_facts(
    const render_policy::TierInputs& inputs,
    const render_policy::LadderState& rung,
    render_policy::RenderTier tier
) noexcept;

/// Returns what the graphics card does at a rung of the step-down ladder,
/// which the status's second line says: nothing but the zoomed-out view's
/// smoothing, or nothing at all, with the chrome drawn NEAREST; everything,
/// the zoomed-out view smoothed, above the lowest budget; the interface and
/// the zoomed-in view with the card magnifying; and otherwise the menus and
/// the interface.
///
/// @param rung the rung
/// @return the reach
[[nodiscard]] oa::ui::engine_settings::AccelerationReach
acceleration_reach(const render_policy::LadderState& rung) noexcept;

} // namespace oa::app

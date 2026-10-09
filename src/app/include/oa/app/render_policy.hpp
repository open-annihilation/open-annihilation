// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The render policy: the decisions about the renderer the game presents
// through and the tier each frame is drawn in, as pure functions of plain
// facts. Which renderer to create, and in what order to try SDL's drivers;
// whether a renderer can be accelerated; whether a frame is drawn in the
// standard tier (today's renderer), the accelerated one (Basic) or Full,
// where the graphics card draws the battlefield, and why Full was not
// given where it was asked for; where a machine starts on the ladder of
// rungs and the rung below a buffer the memory guard refuses; whether
// the window opens at the display's own pixel density; how the chrome is
// filtered; and how a texture larger than the renderer allows is split into
// tiles. What a crash or a failure left behind counts for at the
// next start, and the sentinel and the trial through a run, are the
// renderer records' (renderer_records.hpp). Nothing here calls SDL, reads a
// file or a clock, or names a graphics interface: the host hands it what the
// renderer, the machine and the frame pacer report, and acts on what it
// hands back.
#pragma once

#include "oa/ui/engine_settings.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace oa::app::render_policy {

/// The levels of Hardware acceleration, as the setting and the flags name
/// them: Off, Basic and Full.
using oa::ui::engine_settings::HardwareAcceleration;
/// The ways of Menu scaling: Sharp, Whole steps and Unfiltered.
using oa::ui::engine_settings::MenuScaling;

// ---------------------------------------------------------------------------
// Tiers and the command line

/// How a frame is drawn and presented.
enum class RenderTier : uint8_t {
    standard,    ///< today's renderer: the processor draws and scales everything
    accelerated, ///< Basic: the processor draws; the graphics card scales and composes
    /// Full: the processor plans the frame and the graphics card draws the
    /// battlefield from texture pages, the terrain first; what the card
    /// does not draw yet, the processor still draws and the card composes.
    full,
};

/// Tells whether a tier presents through the graphics card: Basic or Full.
///
/// @param tier the tier
/// @return true for every tier but the standard one
[[nodiscard]] constexpr bool card_tier(RenderTier tier) noexcept {
    return tier != RenderTier::standard;
}

/// What the command line asks of hardware acceleration. A flag names a
/// level of the setting and decides it for the run.
enum class AccelerationFlag : uint8_t {
    none,  ///< no flag: the setting decides
    off,   ///< --no-hardware-acceleration or --hardware-acceleration=off
    basic, ///< --hardware-acceleration=basic
    full,  ///< --hardware-acceleration, or --hardware-acceleration=full
};

// ---------------------------------------------------------------------------
// The renderer's facts and its capability

/// The name SDL gives its own software renderer, which is never recorded as
/// failed, never skipped and never capable.
inline constexpr std::string_view software_driver = "software";

/// The texture size a renderer reports when it sets no limit.
inline constexpr uint32_t unlimited_texture_size = 0;

/// The smallest texture limit, in texels, at which a renderer can be
/// accelerated.
inline constexpr uint32_t smallest_capable_texture_size = 1024;

/// The texture limit taken, in texels, for a driver that reports a fixed
/// limit when the device's own limit is not known.
inline constexpr uint32_t unconfirmed_texture_size_cap = 8192;

/// Where a driver's true texture limit comes from.
enum class TextureLimitSource : uint8_t {
    reported,     ///< the limit the renderer reports is the device's own
    fixed_report, ///< the renderer reports a fixed limit; the device's own is read apart
};

/// What the policy needs to know of a render driver. The platform's render
/// probe fills it from the driver's name, so that this code names no
/// graphics interface.
struct DriverTraits {
    bool software{};             ///< SDL's own software renderer
    bool capable_before_vista{}; ///< the one driver that can be accelerated on Windows before Vista
    /// An adapter that cannot be read leaves the renderer not capable,
    /// since a software rasteriser cannot be ruled out.
    bool adapter_required{};
    TextureLimitSource texture_limit_source{TextureLimitSource::reported};
    /// Its device is lost in ordinary use (switching away from exclusive
    /// full screen, locking the screen), so a failure while running is never
    /// recorded against it.
    bool loses_device_in_normal_use{};
    /// The blend is never used on it, since its textures would take the
    /// blend's memory twice over.
    bool blend_excluded{};
};

/// Returns the texture limit a renderer really has.
///
/// @param driver the driver's traits
/// @param reported the limit the renderer reports, in texels; 0 for none
/// @param device the device's own limit when the probe read it, in texels;
///     0 when it could not
/// @return the limit in texels, unlimited_texture_size (0) for none: the
///     reported limit; for a TextureLimitSource::fixed_report driver the
///     device's own limit, no larger than the reported one, when the probe
///     read it, otherwise the reported limit at most
///     unconfirmed_texture_size_cap
[[nodiscard]] uint32_t
texture_limit(const DriverTraits& driver, uint32_t reported, uint32_t device) noexcept;

/// What probe items 1 to 3 learned of a renderer: its driver, its texture
/// limit and its adapter.
struct RendererFacts {
    DriverTraits driver{};
    uint32_t max_texture_size{}; ///< the corrected limit (texture_limit) in texels; 0 for none
    bool adapter_known{};        ///< the probe read the adapter
    bool software_rasteriser{};  ///< the adapter rasterises on the processor
    bool virtual_adapter{};      ///< the adapter is a virtual machine's display adapter
    bool under_wine{}; ///< the game runs under Wine, whose graphics run through another interface
};

/// Whether a renderer can be accelerated, or why not. The order of the
/// enumerators is the order assess_renderer tests them in.
enum class Capability : uint8_t {
    capable,
    software_renderer,   ///< SDL's software renderer
    before_vista_driver, ///< Windows before Vista, on a driver other than the one allowed there
    small_texture_limit, ///< a texture limit under smallest_capable_texture_size
    under_wine,          ///< under Wine
    software_rasteriser, ///< an adapter that rasterises on the processor
    virtual_adapter,     ///< a virtual machine's display adapter
    unknown_adapter,     ///< an adapter that cannot be read, on a driver that needs it read
};

/// Decides whether a renderer can be accelerated, from probe items 1 to 3.
///
/// @param renderer what the probe read
/// @param legacy_windows the game runs on Windows before Vista
/// @param accept_virtual_adapter --accept-virtual-adapter was given, which
///     lifts only Capability::virtual_adapter
/// @return Capability::capable, or the first reason in the enumeration's
///     order that the renderer is not
[[nodiscard]] Capability assess_renderer(
    const RendererFacts& renderer, bool legacy_windows, bool accept_virtual_adapter
) noexcept;

// ---------------------------------------------------------------------------
// Choosing the tier

/// What the start-up function test did.
enum class FunctionTest : uint8_t {
    not_run,         ///< it has not run in this run
    passed,          ///< it drew the known pattern right
    failed,          ///< it drew wrongly or a call failed
    trial_unwritten, ///< it was skipped because its trial record could not be written
};

/// Why acceleration was dropped for the rest of the run.
enum class Drop : uint8_t {
    none,
    /// An accelerated-only call failed, the device was lost or reset, or a
    /// present error rebuilt the renderer.
    driver_failure,
    engine_fault, ///< presenting failed with a render target still set or an invalid renderer
    memory,       ///< the memory guard
    stall,        ///< the present stalled in the accelerated tier
    path_trial_unwritten, ///< an accelerated path's trial record could not be written
};

/// What kind of match is under way, from the moment its loading screen
/// begins.
enum class MatchKind : uint8_t {
    none,        ///< no match, or a match played alone
    shared_game, ///< a match played with other machines
    replay,      ///< a recorded game played back
};

/// The tier's state across a shared game or a replay: whatever leaves the
/// accelerated tier, or Full for Basic, applies at once, and whatever would
/// start either waits for the match to end.
struct SharedMatchGate {
    MatchKind kind{MatchKind::none};
    /// The tier was accelerated as the match's loading screen began, with
    /// the function test passed, and nothing has stopped it since.
    bool accelerated{};
    /// The tier was Full as the match's loading screen began, so its pages
    /// and targets were made then, and nothing has dropped it since.
    bool full{};
};

/// Opens the gate as a match's loading screen begins.
///
/// @param[out] gate the run's gate
/// @param kind the match's kind; MatchKind::none closes nothing
/// @param accelerated_now the tier decided for the loading screen's first
///     frame is accelerated or Full
/// @param full_now that tier is Full
void begin_match(
    SharedMatchGate& gate, MatchKind kind, bool accelerated_now, bool full_now = false
) noexcept;

/// Notes a frame's decision while a match is under way: in a shared game
/// or a replay, any decision for the standard tier, other than a lost
/// device that a reset brings back, keeps the tier standard until the match
/// ends, and any decision for a tier below Full keeps Full away until then.
///
/// @param[in,out] gate the run's gate
/// @param tier the frame's tier
/// @param device_lost the frame is standard only because the device is lost
void note_match_frame(SharedMatchGate& gate, RenderTier tier, bool device_lost) noexcept;

/// Keeps the tier standard until the match ends, as after a device reset
/// destroyed the accelerated tier's textures during a shared game.
///
/// @param[in,out] gate the run's gate
void stop_until_match_end(SharedMatchGate& gate) noexcept;

/// Closes the gate as the match ends.
///
/// @param[out] gate the run's gate
void end_match(SharedMatchGate& gate) noexcept;

/// One gibibyte, in bytes.
inline constexpr uint64_t gibibyte = uint64_t{1} << 30;

/// The least physical memory, in bytes as the system reports it, with which
/// the accelerated tier may run. A machine that reports it counts as having
/// 2 GiB: it is 1.75 GiB, since the firmware and the graphics take their
/// share of a machine's 2 GB before the system reports the rest. Under it,
/// and where the system does not report memory, every frame is drawn in the
/// standard tier whatever the flags, and the function test never runs.
inline constexpr uint64_t smallest_accelerated_memory = 7 * gibibyte / 4;

/// Why Full was dropped to Basic for the rest of the run.
enum class FullDrop : uint8_t {
    none,
    card_failure,    ///< a call only the Full tier makes failed, which is struck against the driver
    function_test,   ///< the Full function test failed: the card lacks a feature Full needs
    memory,          ///< the memory guard, which setting the setting to Off and back does not lift
    trial_unwritten, ///< Full's trial record could not be written, so Full was not tried
    /// The card refused a frame before drawing anything of it: one the Full
    /// tier built wrong, which no driver caused, so nothing is struck.
    frame_refused,
};

/// Everything decide_render_tier reads.
struct TierInputs {
    bool renderer{};       ///< a renderer exists (headless runs have none)
    bool director_frame{}; ///< the frame is a director render's
    /// The machine's physical memory in bytes, as the system reports it; 0
    /// when it does not say.
    uint64_t memory{};
    AccelerationFlag flag{AccelerationFlag::none};
    bool force_capable{};        ///< --force-capable, which only a check passes
    bool render_driver_named{};  ///< the SDL_RENDER_DRIVER environment variable is set
    bool virtual_video_driver{}; ///< the video driver is dummy or offscreen
    bool players_own_profile{};  ///< no --preferences-file was named
    /// The Hardware acceleration setting in effect. Full is drawn as Basic
    /// until the game draws the battlefield on the graphics card.
    HardwareAcceleration setting{HardwareAcceleration::off};
    Capability capability{Capability::capable};
    FunctionTest function_test{FunctionTest::not_run};
    bool accelerated_unusable_record{}; ///< the driver has an accelerated-unusable record
    /// After a start that did not end cleanly, the records file could not
    /// be read.
    bool records_unreadable_after_unclean_start{};
    Drop drop{Drop::none};
    /// Why Full was dropped for the rest of the run, to Basic; setting the
    /// setting to Off and back, or Restore defaults, lifts every drop but
    /// the memory guard's (forget_failures).
    FullDrop full_drop{FullDrop::none};
    /// The driver has a full-unusable record, which keeps Full off it while
    /// Basic stands, unless --hardware-acceleration=full was given.
    bool full_unusable_record{};
    bool device_lost{}; ///< the device is lost until it is reset
    SharedMatchGate match{};
};

/// Why decide_render_tier chose its tier. The order of the enumerators is the
/// order it tests the conditions in.
enum class TierReason : uint8_t {
    accelerated,    ///< every condition holds
    no_renderer,    ///< a headless run
    director_frame, ///< a director render's frame
    /// Physical memory under smallest_accelerated_memory, or not reported,
    /// whatever the flags.
    memory,
    flag_off, ///< --no-hardware-acceleration or --hardware-acceleration=off
    /// SDL_RENDER_DRIVER or a dummy or offscreen video driver, with no flag
    /// that lifts it.
    environment,
    setting_off,           ///< the setting is Off and no flag asks for Basic or Full
    not_capable,           ///< probe items 1 to 3 rejected the renderer
    function_test_failed,  ///< the function test drew wrongly
    records_unreadable,    ///< the records could not be read after an unclean start
    accelerated_unusable,  ///< the driver has an accelerated-unusable record
    dropped,               ///< acceleration was dropped for the run
    device_lost,           ///< the device is lost until it is reset
    waiting_for_match_end, ///< a shared game or a replay, which the tier did not start accelerated
    trial_unwritten,       ///< the function test's trial could not be written, so it was skipped
    function_test_due, ///< every other condition holds: run the function test, then decide again
};

/// Why a frame whose tier is accelerated is Full, or is Basic although Full
/// was asked for. The order of the enumerators is the order
/// decide_render_tier tests the conditions in.
enum class FullReason : uint8_t {
    not_asked, ///< Off or Basic was asked for, or the tier is standard
    full,      ///< Full was asked for and every condition holds
    /// The driver has a full-unusable record and --hardware-acceleration=full
    /// was not given.
    unusable_record,
    dropped, ///< Full was dropped for the run (TierInputs::full_drop)
    /// A shared game or a replay, which the tier did not begin as Full.
    waiting_for_match_end,
};

/// The tier for a frame and the reason for it.
struct TierDecision {
    RenderTier tier{RenderTier::standard};
    TierReason reason{TierReason::no_renderer};
    /// Where the tier is accelerated or full, whether it is Full, and why
    /// not where Full was asked for; FullReason::not_asked otherwise.
    FullReason full{FullReason::not_asked};
};

/// Tells whether the records and the trial live on disk: only the player's
/// own profile keeps them there, and SDL_RENDER_DRIVER keeps none.
///
/// @param players_own_profile no --preferences-file was named
/// @param render_driver_named SDL_RENDER_DRIVER is set
/// @return true when the records are read and written on disk
[[nodiscard]] bool records_on_disk(bool players_own_profile, bool render_driver_named) noexcept;

/// Decides which tier draws a frame.
///
/// Basic and Full share every condition of the accelerated tier, and a
/// frame the conditions allow is Full when Full was asked for
/// (acceleration_asked), Full is ready in this build (full_ready) or
/// --hardware-acceleration=full forced it, the driver has no full-unusable
/// record or that flag was given, Full was not dropped for the run
/// (TierInputs::full_drop), and in a shared game or a replay the tier was
/// Full as its loading screen began; otherwise it is Basic
/// (RenderTier::accelerated), with the first Full reason in FullReason's
/// order in the decision.
///
/// The accelerated tier needs every condition: a renderer, a frame that is
/// not a director render's, physical memory of at least
/// smallest_accelerated_memory as the system reports it, which no flag
/// lifts, no flag that names Off, neither SDL_RENDER_DRIVER nor a dummy or
/// offscreen video driver unless a flag that names Basic or Full or
/// --force-capable lifts them, Basic or Full asked for
/// (acceleration_asked), a capable renderer or --force-capable, a function
/// test that passed, readable records after an unclean start, no
/// accelerated-unusable record unless a flag names Basic or Full, no drop,
/// no lost device, and in a shared game or a replay a tier that was
/// accelerated as its loading screen began. Where the records live in
/// memory a trial cannot fail to be written, so FunctionTest::trial_unwritten
/// counts as not run there.
///
/// @param inputs the run's and the frame's facts
/// @return the tier, and the first reason in TierReason's order that
///     decided it; TierReason::function_test_due when only the function
///     test is missing, which the host runs before it decides again
[[nodiscard]] TierDecision decide_render_tier(const TierInputs& inputs) noexcept;

/// Tells whether the function test may run now: it runs only when the tier
/// could be accelerated but for the test itself, so never under
/// smallest_accelerated_memory or with memory not reported. A test that
/// failed does not run again, nor one whose trial could not be written on
/// disk, until the host sets function_test back to FunctionTest::not_run,
/// as setting the setting to Off and back or Restore defaults does.
///
/// @param inputs the run's and the frame's facts
/// @return true when decide_render_tier answers function_test_due
[[nodiscard]] bool function_test_may_run(const TierInputs& inputs) noexcept;

// ---------------------------------------------------------------------------
// Acting on the tier

/// SDL's names of the video drivers that draw no window.
inline constexpr std::array<std::string_view, 2> windowless_video_drivers{"dummy", "offscreen"};

/// Tells whether a video driver draws no window.
///
/// @param video_driver SDL's name for the video driver
/// @return true for a name of windowless_video_drivers, in any letter case
[[nodiscard]] bool windowless_video_driver(std::string_view video_driver) noexcept;

/// Returns what the command line asks of hardware acceleration.
///
/// @param flag the level a flag names (Options::hardware_acceleration);
///     empty for no flag
/// @return the flag: AccelerationFlag::none for no flag, else the level
[[nodiscard]] AccelerationFlag acceleration_flag(std::optional<HardwareAcceleration> flag) noexcept;

/// Returns the level of hardware acceleration a run asks for: the flag's
/// where one was given, else the setting's.
///
/// @param flag what the command line asks
/// @param setting the Hardware acceleration setting in effect
/// @return Off, Basic or Full
[[nodiscard]] HardwareAcceleration
acceleration_asked(AccelerationFlag flag, HardwareAcceleration setting) noexcept;

/// Tells whether a flag asks for the graphics card: it names Basic or Full.
///
/// @param flag what the command line asks
/// @return true for AccelerationFlag::basic or full
[[nodiscard]] bool flag_asks_for_card(AccelerationFlag flag) noexcept;

/// What the host does before it draws a frame, to make its presentation
/// match the tier decided for the frame.
enum class TierAction : uint8_t {
    none,              ///< the presentation already matches the tier
    run_function_test, ///< run the function test, then decide again
    switch_on,         ///< switch the accelerated presentation on
    switch_off,        ///< switch it off: the frame is drawn in the standard tier
};

/// Returns what the host does before it draws a frame.
///
/// @param decision the tier decided for the frame (decide_render_tier)
/// @param presentation_on the accelerated presentation is on
/// @return TierAction::run_function_test when only the function test is
///     missing, else switch_on or switch_off where the presentation differs
///     from the tier (card_tier: Basic and Full both switch it on), else
///     TierAction::none
[[nodiscard]] TierAction tier_action(const TierDecision& decision, bool presentation_on) noexcept;

/// Forgets what keeps the tier standard, or Basic in place of Full, that
/// setting Hardware acceleration to Off and back, or Restore defaults, lets
/// the run try again: a function test that failed or whose trial could not
/// be written, which then runs again, and a drop of either tier, except the
/// memory guard's.
///
/// @param[in,out] inputs the run's facts
void forget_failures(TierInputs& inputs) noexcept;

/// What runs the start-up function test for step_tier.
struct FunctionTestHooks {
    void* context{};
    /// Runs the function test on the renderer and returns what it found,
    /// FunctionTest::passed or FunctionTest::failed, or
    /// FunctionTest::trial_unwritten when the trial record written before
    /// it could not be, so it did not run. Null runs none, so the frame
    /// stays in the standard tier with the test still due.
    FunctionTest (*run)(void* context){};
};

/// A frame's tier and what the host does before it draws the frame.
struct TierStep {
    TierDecision decision{}; ///< the frame's tier, decided after any function test
    /// TierAction::switch_on, TierAction::switch_off or TierAction::none:
    /// what makes the presentation match the tier.
    TierAction action{TierAction::none};
};

/// Decides which tier draws a frame and what the host does for it, as the
/// game does before each frame: decides the tier
/// (decide_render_tier); where only the function test is missing, runs it
/// through the hooks, keeps its result and decides again; notes the
/// decision in a shared game or a replay (note_match_frame), so that a
/// frame decided standard, other than for a lost device, keeps it standard
/// until the match ends; and says how the presentation must switch to
/// match (tier_action). A test still due because the hooks ran none leaves
/// the frame standard.
///
/// @param[in,out] inputs the run's and the frame's facts; the function
///     test's result and the match's gate are kept in them
/// @param presentation_on the accelerated presentation is on
/// @param test what runs the function test
/// @return the frame's decision and what the host does
[[nodiscard]] TierStep
step_tier(TierInputs& inputs, bool presentation_on, const FunctionTestHooks& test);

// ---------------------------------------------------------------------------
// Creating the renderer

/// The framebuffer hint's value on a video driver that presents SDL's
/// software renderer through the window's own framebuffer.
inline constexpr std::string_view framebuffer_hint_window = "0";

/// The drivers SDL may try and what the walk knows of them.
struct CreationInputs {
    /// SDL's drivers in its own order (SDL_GetRenderDriver).
    std::span<const std::string_view> sdl_order{};
    /// The drivers recorded failed-driver; never read for software.
    std::span<const std::string_view> failed_drivers{};
    /// SDL_RENDER_DRIVER's list, in its order, when the variable is set.
    std::span<const std::string_view> environment_order{};
    bool render_driver_named{}; ///< SDL_RENDER_DRIVER is set
    /// The video driver presents SDL's software renderer through the
    /// window's own framebuffer; otherwise it needs a driver to present it
    /// through, which the framebuffer hint names.
    bool native_window_framebuffer{};
    /// The framebuffer hint takes a comma list of drivers (SDL 3.4 and
    /// later); before that it names the first of them alone.
    bool hint_takes_list{};
};

/// What a walk is for.
enum class WalkKind : uint8_t {
    start,               ///< the start's walk of SDL's order
    rebuild,             ///< a rebuild, from the driver after the one that failed
    environment_start,   ///< SDL_RENDER_DRIVER's start: SDL's own call, once
    environment_rebuild, ///< a rebuild under SDL_RENDER_DRIVER: its later drivers, then software
};

/// Where a walk of the drivers stands.
struct CreationWalk {
    WalkKind kind{WalkKind::start};
    /// A rebuild's driver that failed, which the walk does not try again and
    /// leaves out of the hint's list.
    std::string_view failed_driver{};
    size_t position{};           ///< the next place in the order
    bool records_ignored{};      ///< the second walk, from the top with the records ignored
    bool hardware_missed{};      ///< a hardware driver refused, was skipped by a record or was lost
    bool skipped_by_record{};    ///< the walk skipped a driver because of a record
    bool software_appended{};    ///< an environment rebuild has tried software after its list
    bool attempted{};            ///< an attempt is under way: the next call means it was refused
    bool attempt_was_hardware{}; ///< that attempt was of a hardware driver
    bool finished{};             ///< nothing is left to try
};

/// What to try next.
enum class AttemptKind : uint8_t {
    driver,     ///< create the named driver
    sdl_choice, ///< SDL's own call, with no driver named (SDL_RENDER_DRIVER's start)
    none,       ///< nothing is left: the start or rebuild fails
};

/// One attempt of a walk.
struct Attempt {
    AttemptKind kind{AttemptKind::none};
    std::string_view driver{};      ///< the driver, for AttemptKind::driver
    bool set_framebuffer_hint{};    ///< set the framebuffer hint before creating it
    std::string framebuffer_hint{}; ///< the hint's value; never empty when it is set
    bool records_ignored{};         ///< the walk ignores the records for this run
};

/// Starts the walk of a start: SDL's order, or under SDL_RENDER_DRIVER
/// SDL's own call once.
///
/// @param inputs the drivers and the records
/// @return the walk, before its first attempt
[[nodiscard]] CreationWalk start_creation(const CreationInputs& inputs) noexcept;

/// Starts the walk of a rebuild after a driver failed while running: the
/// drivers after it in SDL's order, then software. Under SDL_RENDER_DRIVER
/// it is the drivers after it in the variable's list, whose names match in
/// any letter case as SDL matches them, never the failed one again, then
/// software.
///
/// @param inputs the drivers and the records
/// @param failed_driver the driver that failed; the walk keeps a view of it
/// @return the walk, before its first attempt
[[nodiscard]] CreationWalk
start_rebuild(const CreationInputs& inputs, std::string_view failed_driver) noexcept;

/// Returns the next attempt of a walk. Calling it again means the attempt
/// it returned was refused.
///
/// Drivers recorded failed-driver are skipped, but never software. Before
/// software, when a hardware driver refused, was skipped by a record or was
/// lost, the framebuffer hint is set: framebuffer_hint_window where the
/// window has a framebuffer of its own, otherwise the drivers of SDL's
/// order that are neither software, recorded nor the rebuild's failed one,
/// as a list, or the first of them alone before SDL 3.4. When that list is
/// empty, or software is refused too, nothing could present: a walk that
/// skipped a driver by a record, and every rebuild, walk SDL's full order
/// again from the top with the records ignored. In that second walk an
/// empty list leaves the hint unset, which is SDL's own choice. Under
/// SDL_RENDER_DRIVER there is no second walk.
///
/// @param[in,out] walk the walk
/// @param inputs the inputs the walk was started with
/// @return the attempt, or AttemptKind::none when nothing is left
[[nodiscard]] Attempt next_attempt(CreationWalk& walk, const CreationInputs& inputs);

// ---------------------------------------------------------------------------
// Present stalls

/// A present that takes longer than this, in nanoseconds, is a stall.
inline constexpr uint64_t stall_present_ns = 2'000'000'000;
/// The stalls within stall_window_ns that act.
inline constexpr uint32_t stalls_that_act = 3;
/// The span the stalls are counted over, in nanoseconds of steady frames:
/// the time of frames that are not steady does not count.
inline constexpr uint64_t stall_window_ns = 10'000'000'000;

/// The stalls seen lately.
struct StallWatch {
    /// The latest stalls' times on the steady frames' clock, oldest first.
    std::array<uint64_t, stalls_that_act> stall_times_ns{};
    uint32_t stalls{}; ///< how many of stall_times_ns hold one
    bool logged{};     ///< the standard tier has logged its stalls
};

/// What the stall rule asks for.
enum class StallAction : uint8_t {
    none,
    log,  ///< the standard tier: log it once and carry on
    drop, ///< the accelerated tier: drop acceleration for the run, unrecorded
};

/// Notes a steady frame's present measure. The host calls it for steady
/// frames alone (steady_frame), with the steady frames' own clock: the sum
/// of their intervals, which stands still while frames are not steady.
///
/// @param[in,out] watch the run's stalls
/// @param steady_ns the frame's time on the steady frames' clock, in
///     nanoseconds
/// @param present_ns the frame's present measure, in nanoseconds
/// @param tier the frame's tier
/// @return StallAction::drop or log at the third stall within
///     stall_window_ns, log only once in a run; otherwise none
[[nodiscard]] StallAction
note_present(StallWatch& watch, uint64_t steady_ns, uint64_t present_ns, RenderTier tier) noexcept;

// ---------------------------------------------------------------------------
// Device resets

/// The device resets within reset_window_ms that make a rebuild.
inline constexpr uint32_t resets_that_rebuild = 3;
/// The span the device resets are counted over, in milliseconds.
inline constexpr uint64_t reset_window_ms = 60'000;

/// The device resets seen lately.
struct ResetWatch {
    /// The latest resets' times, in milliseconds, oldest first.
    std::array<uint64_t, resets_that_rebuild> reset_times_ms{};
    uint32_t resets{}; ///< how many of reset_times_ms hold one
};

/// Notes a device reset. Every reset forgets the engine's textures; the
/// third within reset_window_ms makes a rebuild, and the count then starts
/// again.
///
/// @param[in,out] watch the run's resets
/// @param now_ms when the reset came, in milliseconds on a steady clock
/// @return true at the third reset within reset_window_ms
[[nodiscard]] bool note_device_reset(ResetWatch& watch, uint64_t now_ms) noexcept;

// ---------------------------------------------------------------------------
// Texture formats of the layers

/// The pixel format of a layer's texture.
enum class LayerFormat : uint8_t {
    rgb24,    ///< 3 bytes a pixel, red first
    xrgb8888, ///< 0xXXRRGGBB words
    rgb565,   ///< 16-bit words, for a 16-bit window
    argb8888, ///< 0xAARRGGBB words, opaque and drawn with no blending
};

/// The texture formats of the standard tier's opaque layers.
struct LayerFormats {
    LayerFormat opaque{LayerFormat::xrgb8888};  ///< the match's world, HUD and side textures
    LayerFormat loading{LayerFormat::xrgb8888}; ///< the loading screen and the palette movies
    LayerFormat front_end{LayerFormat::rgb24};  ///< the front end's screens
};

/// Chooses the texture formats of the opaque layers.
///
/// SDL's software renderer keeps today's: XRGB8888, RGB565 for the match's
/// layers on a 16-bit window, and RGB24 for the front end; so does every
/// driver under SDL_RENDER_DRIVER, with XRGB8888 for the match's layers.
/// Every other renderer gets ARGB8888, which hardware drivers upload without
/// converting it; the layers are opaque and drawn with no blending, so no
/// pixel changes.
///
/// @param software the renderer is SDL's software renderer
/// @param rgb565_window the window's pixels are RGB565
/// @param render_driver_named SDL_RENDER_DRIVER is set
/// @return the formats
[[nodiscard]] LayerFormats
layer_formats(bool software, bool rgb565_window, bool render_driver_named) noexcept;

// ---------------------------------------------------------------------------
// The step-down ladder

/// The zoomed-out view's method.
enum class ZoomOutMethod : uint8_t {
    area,  ///< the exact area pass on the processor
    blend, ///< the two-level blend on the graphics card
};

/// The scene pixels the zoomed-out view may draw, in order from the least.
enum class SceneBudget : uint8_t {
    none,    ///< no zoomed-out filtering: below zoom 1 the scene is drawn at the zoom
    reduced, ///< up to 2.25 scene pixels per battlefield pixel
    full,    ///< up to 4 scene pixels per battlefield pixel
};

/// How the graphics card magnifies, in order from the least work.
enum class CardFilter : uint8_t {
    linear,           ///< plain LINEAR, with no prescale target
    prescale_quarter, ///< sharp-bilinear within a quarter of the prescale budget
    prescale_full,    ///< sharp-bilinear within the whole prescale budget
    pixelart,         ///< the renderer's PIXELART scale mode, which needs no prescale target
};

/// Where the accelerated tier stands on the step-down ladder. Each step
/// lowers one member, from the top: Full's rungs while Full draws (its
/// anti-aliasing from 4 to 2 to 1, then Full to Basic), the zoomed-out rungs
/// (method, then budget), magnify off, NEAREST chrome, the card's
/// magnification, and the standard tier.
struct LadderState {
    /// The Full tier draws at the rung; false from the rung Full falls to
    /// Basic down, and on every rung of a run that draws Basic.
    bool full{};
    ZoomOutMethod method{ZoomOutMethod::area};
    SceneBudget budget{SceneBudget::none};
    bool blend_allowed{}; ///< blend is built and allowed on this machine and renderer
    /// Above zoom 1 the card magnifies the scene; false is the magnify-off
    /// rung.
    bool magnify{};
    /// The chrome is filtered at scales that are not whole; false is the
    /// NEAREST-chrome rung.
    bool filtered_chrome{};
    CardFilter card{CardFilter::linear};
    bool standard{}; ///< the last rung: the standard tier for the rest of the run

    friend bool operator==(const LadderState&, const LadderState&) = default;
};

/// Whether a kind of machine has been run on the accelerated tier.
enum class ClassTesting : uint8_t {
    untested, ///< nobody has run this operating system, architecture and driver
    tested,   ///< it has been run, but its weak rows are not yet measured
    measured, ///< it has been run and its weak rows measured
};

/// The most logical processors with which a machine starts at budget none.
inline constexpr uint32_t budget_none_most_processors = 2;
/// The logical processors with which a machine starts at budget reduced.
inline constexpr uint32_t budget_reduced_processors = 3;
/// The most physical memory, in bytes, with which the blend is never used.
inline constexpr uint64_t most_memory_without_blend = 4 * gibibyte;

/// The machine and renderer facts the starting rung is sized from.
struct StartInputs {
    uint32_t processors{1}; ///< logical processors
    /// Physical memory in bytes, as the system reports it; 0 when it does
    /// not say. Only the blend reads it: a start is made only from
    /// smallest_accelerated_memory, and is the same at any memory from there.
    uint64_t memory{};
    bool light_machine{}; ///< oa::platform::light_machine
    bool raspberry_pi{};  ///< a Raspberry Pi
    /// An ARM processor other than Apple silicon and the Raspberry Pi models
    /// that have been run on the accelerated tier.
    bool other_arm{};
    bool legacy_windows{}; ///< Windows before Vista
    ClassTesting run_class{ClassTesting::untested};
    DriverTraits driver{};  ///< the renderer's driver
    bool pixelart{};        ///< probe (c) found the PIXELART scale mode
    bool blend_available{}; ///< blend is built and its half level fits this renderer
};

/// The magnify path has been measured no slower than the standard tier on
/// one thread, which lets a start at budget none rise above magnify off.
inline constexpr bool magnify_measured_at_budget_none = false;
/// A real run on Windows before Vista has held the floor with magnify on,
/// which lets such a start rise above magnify off.
inline constexpr bool magnify_measured_before_vista = false;

/// Returns the scene budget a machine starts at. Memory does not enter it:
/// a start is made only from smallest_accelerated_memory.
///
/// The budget is none with budget_none_most_processors or fewer logical
/// processors, on an ARM processor other than Apple silicon and the
/// Raspberry Pis that have been run, on Windows before Vista and in a class
/// nobody has run; reduced with budget_reduced_processors, and in a class
/// that has been run but not yet measured; full with more processors in a
/// measured class.
///
/// @param machine the machine's and the renderer's facts
/// @return the starting budget
[[nodiscard]] SceneBudget start_budget(const StartInputs& machine) noexcept;

/// Returns the rung a machine starts at, from smallest_accelerated_memory.
///
/// The budget is start_budget's. Before Vista and at budget none the start
/// is the magnify-off rung; no machine starts at the NEAREST-chrome rung by
/// its size. The card uses PIXELART where the probe found it, otherwise the
/// prescale budget, a quarter of it on a light machine or a Pi. The blend
/// is allowed where it is available, on a driver that does not exclude it,
/// with more than most_memory_without_blend. The rung is Basic's until the
/// host marks it Full's (LadderState::full).
///
/// @param machine the machine's and the renderer's facts
/// @return the starting rung
[[nodiscard]] LadderState start_rung(const StartInputs& machine) noexcept;

/// Returns the highest rung a run on this machine may start at, by the
/// remembered rung or otherwise: the machine's own start with the area
/// pass, its budget free to rise to full where the start is above the
/// magnify-off rung. A start at the magnify-off rung rises no higher.
///
/// @param machine the machine's and the renderer's facts
/// @return the ceiling
[[nodiscard]] LadderState start_ceiling(const StartInputs& machine) noexcept;

/// The recorded median frame interval under which a remembered rung starts
/// the next run one rung higher, in percent of the target period.
inline constexpr uint32_t headroom_percent = 60;

/// Returns the rung a run starts at, given the rung a run before it reached.
///
/// The run starts at the remembered rung, never at the standard tier and
/// never above the ceiling, and one rung higher when the frames recorded at
/// it had a median under headroom_percent of the target period.
///
/// @param machine the machine's and the renderer's facts
/// @param remembered the rung recorded for this driver, adapter and engine version
/// @param median_percent the median frame interval recorded at it, in
///     percent of the target period
/// @return the starting rung
[[nodiscard]] LadderState resume_rung(
    const StartInputs& machine, const LadderState& remembered, uint32_t median_percent
) noexcept;

/// Returns the rung one step up the ladder: the lowest rung taken that the
/// ceiling allows back, passing rungs that would change nothing.
///
/// @param state the rung
/// @param ceiling the highest the run may rise to
/// @return the rung one step up, or state when none is left
[[nodiscard]] LadderState step_up(const LadderState& state, const LadderState& ceiling) noexcept;

/// What the host knows of a presented frame's steadiness (steady_frame).
struct FrameSample {
    bool idle{};          ///< paced at the idle rate
    bool window_active{}; ///< the window is shown and has the focus
    bool settling{};      ///< within 2 s of a resize, a mode change or a full-screen switch
    bool match_warming{}; ///< within the match's first 5 s
};

/// Tells whether a frame is steady, so that its sample counts.
///
/// @param sample the frame
/// @return true for a frame at the full rate in an active window, outside
///     the settle time and the match's first seconds
[[nodiscard]] bool steady_frame(const FrameSample& sample) noexcept;

/// A buffer of the accelerated tier's own that the memory guard may refuse
/// before it is made (memory_guard_allows).
enum class AcceleratedBuffer : uint8_t {
    scene,        ///< the magnified scene's texture, with the overlay's texture and buffer
    prescale,     ///< a prescale target: the HUD's, the screens' or the scene's
    card_pages,   ///< the Full tier's texture pages: the terrain atlas, the sprites, the models
    card_targets, ///< the Full tier's render targets: the world, half and shadow targets
};

/// Returns the rung below one that holds no such buffer, where the tier
/// stays when the memory guard refuses to let it make one: magnify off for
/// the scene, the card's magnification one rung lower for a prescale
/// target, and Basic for Full's pages and targets, whose memory Full frees
/// first. The rung never rises.
///
/// @param state the rung
/// @param buffer the buffer refused
/// @return the rung below, or state where it makes no such buffer
[[nodiscard]] LadderState rung_without(const LadderState& state, AcceleratedBuffer buffer) noexcept;

/// Says in a few words what a step down the ladder changed, for the log.
///
/// @param before the rung before the step
/// @param after the rung after it
/// @return the change, the first in the ladder's order where several
///     changed, Full's rungs first, but for budget none and magnify off
///     together, as a clock running behind sheds them; "nothing changed"
///     for the same rung
[[nodiscard]] std::string_view
describe_step(const LadderState& before, const LadderState& after) noexcept;

// ---------------------------------------------------------------------------
// Native pixel density

/// No class of machine, an operating system, a processor and a render
/// driver together, has yet been measured at native density against the
/// standard tier on the same machine and found to cost no more frame time,
/// so the rule (decide_native_density) opens no window at native density
/// by itself; --native-density and the Native pixel density setting still
/// ask for it, and a platform whose windows are at native density asks for
/// it for every window (DensityInputs::platform_native).
inline constexpr bool native_density_measured = false;

/// Everything decide_native_density reads, known before the window opens.
struct DensityInputs {
    /// The platform the game is built for opens its windows at the
    /// display's own pixel density, where it would otherwise scale a
    /// lower-density window softly (the OA_NATIVE_DENSITY_WINDOWS build
    /// option): native density whatever the rest of the rule says, but for
    /// the machine's memory and a flag that names Off.
    bool platform_native{};
    /// --native-density: native density whatever the rest of the rule says,
    /// but for the machine's memory and a flag that names Off.
    bool asked{};
    /// The Native pixel density setting is on: native density whatever the
    /// rest of the rule says, but for the machine's memory and a flag that
    /// names Off, as --native-density.
    bool chosen{};
    /// The machine's physical memory in bytes, as the system reports it; 0
    /// when it does not say.
    uint64_t memory{};
    AccelerationFlag flag{AccelerationFlag::none};
    /// The Hardware acceleration setting read before the window opens.
    HardwareAcceleration setting{HardwareAcceleration::off};
    bool render_driver_named{};  ///< the SDL_RENDER_DRIVER environment variable is set
    bool virtual_video_driver{}; ///< the video driver is dummy or offscreen
    bool unattended{};           ///< a check, a benchmark or another scripted run
    bool capture{};              ///< the run captures video, sized from the window's pixels
    /// The machine's class, with the render driver the native-density
    /// record names, has been measured at native density and costs no more
    /// frame time (native_density_measured).
    bool class_measured{};
    /// The scene budget the machine starts at with the record's driver
    /// (start_budget).
    SceneBudget budget{SceneBudget::none};
    /// The step-down rung remembered for the record's driver, which the run
    /// would start from (resume_rung); none when none is remembered.
    std::optional<LadderState> remembered{};
    /// The native-density record is there: an earlier run on this profile
    /// passed the function test on a hardware driver, ended cleanly above
    /// the magnify-off rung and dropped nothing, under the running engine's
    /// version.
    bool record{};
};

/// Why decide_native_density chose the window's density. The order of the
/// enumerators is the order it tests the conditions in.
enum class DensityReason : uint8_t {
    native, ///< every condition holds
    /// Physical memory under smallest_accelerated_memory, or not reported,
    /// whatever the flags.
    memory,
    flag_off, ///< --no-hardware-acceleration or --hardware-acceleration=off
    platform, ///< the platform opens its windows at native density: native density
    asked,    ///< --native-density: native density
    chosen,   ///< the Native pixel density setting: native density
    /// SDL_RENDER_DRIVER or a dummy or offscreen video driver.
    environment,
    unattended,       ///< an unattended run
    capture,          ///< a video capture
    setting_off,      ///< the setting is Off and no flag asks for Basic or Full
    class_unmeasured, ///< the machine's class has not been measured at native density
    budget_none,      ///< the machine starts at budget none
    remembered_rung,  ///< the remembered rung is at or below the magnify-off rung
    no_record,        ///< no earlier run left the native-density record
};

/// The window's density and the reason for it.
struct DensityDecision {
    /// The window opens at the display's own pixel density; otherwise at
    /// the window system's.
    bool native{};
    DensityReason reason{DensityReason::no_record};
};

/// Decides whether the game's window opens at the display's own pixel
/// density, which is fixed for the run once it opens.
///
/// A window opens at native density only with every condition: physical
/// memory of at least smallest_accelerated_memory, which no flag lifts; no
/// flag that names Off; neither SDL_RENDER_DRIVER nor a dummy or offscreen
/// video driver; a run that is not unattended and captures no video; Basic
/// or Full asked for (acceleration_asked); a class measured at native
/// density; a start above budget none, from a remembered rung, if any,
/// above the magnify-off rung; and the native-density record.
/// A platform whose windows are at native density, --native-density and the
/// Native pixel density setting open it at native density whatever the
/// conditions after a flag that names Off say. Every other window opens at
/// the window system's density, as a first start does.
///
/// @param inputs what is known before the window opens
/// @return the density, and the first reason in DensityReason's order that
///     decided it; DensityReason::native, platform, asked or chosen exactly
///     when it is native
[[nodiscard]] DensityDecision decide_native_density(const DensityInputs& inputs) noexcept;

// ---------------------------------------------------------------------------
// Chrome filtering and the prescale budget

/// The prescale budget B: the most pixels all prescale targets alive at once
/// may hold. A provisional figure, chosen without measurement.
inline constexpr uint64_t prescale_budget_pixels = uint64_t{1} << 23;

/// Returns the pixels a rung's prescale targets may hold together.
///
/// @param card the card's magnification
/// @return prescale_budget_pixels, a quarter of it, or 0 where no prescale
///     target is made
[[nodiscard]] uint64_t prescale_budget(CardFilter card) noexcept;

/// The prescale targets alive at once, charged against the budget.
struct PrescaleBudget {
    uint64_t limit{};   ///< prescale_budget for the rung
    uint64_t charged{}; ///< pixels of the targets alive now
};

/// Returns the factor a prescale target enlarges its source by: the scale
/// rounded up, falling to the largest whole number whose target fits what
/// is left of the budget.
///
/// @param budget the targets alive now
/// @param width the source's width in pixels
/// @param height the source's height in pixels
/// @param scale the scale the source is drawn at, above 0
/// @return the factor, at least 1; 1 means no target: plain LINEAR
[[nodiscard]] uint32_t prescale_factor(
    const PrescaleBudget& budget, uint32_t width, uint32_t height, double scale
) noexcept;

/// Charges a target to the budget.
///
/// @param[in,out] budget the targets alive now
/// @param pixels the target's pixels
/// @return true when it fits, and is charged; false when it does not
bool charge_prescale(PrescaleBudget& budget, uint64_t pixels) noexcept;

/// Returns a freed target's pixels to the budget, as the front end's target
/// is freed during a match.
///
/// @param[in,out] budget the targets alive now
/// @param pixels the target's pixels
void release_prescale(PrescaleBudget& budget, uint64_t pixels) noexcept;

/// How a layer is scaled to the window.
enum class ScaleFilter : uint8_t {
    nearest,        ///< as today
    pixelart,       ///< the renderer's PIXELART scale mode
    sharp_bilinear, ///< NEAREST into a prescale target, then LINEAR
    linear,         ///< plain LINEAR
};

/// Returns how the chrome and the 640x480 screens are scaled.
///
/// @param state the rung
/// @param scale the scale at the display's pixels, above 0
/// @return NEAREST in the standard tier, at a whole-number scale and on the
///     NEAREST-chrome rung; otherwise PIXELART, sharp-bilinear or plain
///     LINEAR, as the card's magnification says
[[nodiscard]] ScaleFilter chrome_filter(const LadderState& state, double scale) noexcept;

/// How the window holds a screen drawn as one frame: the menus, the
/// loading screen and the other screens of 640x480, and a dialog over the
/// frame of its own size.
enum class FrameFit : uint8_t {
    /// As large as the window holds, centred, the rest of the window black.
    letterbox,
    /// The largest whole number of window pixels to each of the frame's
    /// that the window holds, centred, the rest of the window black.
    whole_steps,
};

/// Decides how the window holds a frame.
///
/// @param scaling the Menu scaling setting
/// @param window_width the window's width, in the pixels the frame is drawn in
/// @param window_height the window's height, in the same pixels
/// @param frame_width the frame's width, in its own pixels
/// @param frame_height the frame's height, in its own pixels
/// @return whole steps where the setting asks for them and the window holds
///     the frame at least once across and down; a smaller window, where one
///     whole step would lose the frame's edges, letterboxes it as Sharp does
[[nodiscard]] FrameFit frame_fit(
    MenuScaling scaling,
    int32_t window_width,
    int32_t window_height,
    int32_t frame_width,
    int32_t frame_height
) noexcept;

/// Returns how a frame the window holds (frame_fit) is scaled to it.
///
/// Unfiltered, and any whole-number scale, repeat each of the frame's
/// pixels (NEAREST). Otherwise, as Sharp, which Whole steps falls back to
/// in a window smaller than the frame: in the accelerated tier the
/// chrome's filter (chrome_filter); in the standard tier PIXELART where
/// the renderer's pixel-art scale mode works, else NEAREST.
///
/// @param scaling the Menu scaling setting
/// @param accelerated the accelerated tier's rung; null in the standard tier
/// @param pixelart the renderer's pixel-art scale mode works; read only in
///     the standard tier
/// @param scale window pixels per frame pixel, above 0
/// @return the filter
[[nodiscard]] ScaleFilter frame_filter(
    MenuScaling scaling, const LadderState* accelerated, bool pixelart, double scale
) noexcept;

/// Tells whether a frame's filter in the standard tier turns on the
/// renderer's pixel-art scale mode, so that whether it works is worth
/// finding out (frame_filter).
///
/// @param scaling the Menu scaling setting
/// @param scale window pixels per frame pixel, above 0
/// @return true unless the setting is Unfiltered or the scale a whole number
[[nodiscard]] bool frame_wants_pixelart(MenuScaling scaling, double scale) noexcept;

/// Returns how the card magnifies the battlefield's scene while the rung
/// magnifies it. The NEAREST-chrome rung leaves it as it is: only the
/// card's magnification lightens it.
///
/// @param state the rung
/// @param zoom screen pixels per map pixel, above 1
/// @return NEAREST in the standard tier and at a whole-number zoom;
///     otherwise PIXELART, sharp-bilinear or plain LINEAR, as the card's
///     magnification says
[[nodiscard]] ScaleFilter world_filter(const LadderState& state, double zoom) noexcept;

// ---------------------------------------------------------------------------
// The Full tier's anti-aliasing: the supersample factor and the budget S

/// The supersample budget S: the most pixels the Full tier's world target
/// may hold, its texture and the half the zoomed-out reduction reads
/// together: the largest target a renderer holds, 16384 a side
/// (card::largest_target_edge), and its half. Below it the renderer's
/// texture limit and the memory guard decide, factor by factor.
inline constexpr uint64_t supersample_budget_pixels =
    uint64_t{16384} * 16384 + uint64_t{8192} * 8192;
/// The largest supersample factor: the world target's texture holds this
/// many pixels a window pixel along each axis at most, and a render target
/// takes the powers of two up to this.
inline constexpr uint32_t largest_supersample_factor = 16;

/// Returns the supersample factor the Enhanced anti-aliasing row asks of
/// the Full tier, in whose frames the processor's anti-aliasing never
/// runs: the row's own number of samples across, 1 for off, 2, 4, 8 or 16.
///
/// @param level the row's level
/// @return the factor, a power of two from 1 to largest_supersample_factor
[[nodiscard]] uint32_t supersample_factor(oa::ui::engine_settings::AntiAliasing level) noexcept;

/// Returns the pixels the Full tier's world target holds at a size and a
/// factor: its texture, the size times the factor along each axis, and
/// its halves, each half the one before: one, which the zoomed-out
/// reduction reads, at a factor up to 4, and one fewer than the factor's
/// doublings above, which the resolve reduces through.
///
/// @param width window pixels across the battlefield
/// @param height window pixels down it
/// @param factor the supersample factor, a power of two up to largest_supersample_factor
/// @return the pixels of every texture
[[nodiscard]] uint64_t
supersample_target_pixels(uint32_t width, uint32_t height, uint32_t factor) noexcept;

/// Returns the supersample factor a world target of a size is made at: the
/// factor asked, halved until the target's texture keeps within the
/// renderer's texture limit along each axis and its pixels
/// (supersample_target_pixels) within the budget; 1 when no factor above
/// 1 does, at which the Full tier draws straight to the window and makes
/// no world target for anti-aliasing.
///
/// @param asked the factor the row asks for (supersample_factor)
/// @param width window pixels across the battlefield
/// @param height window pixels down it
/// @param budget the budget in pixels (supersample_budget_pixels)
/// @param texture_limit the largest texture edge the renderer makes; 0 for no limit
/// @return the factor, a power of two up to largest_supersample_factor,
///     never above `asked`
[[nodiscard]] uint32_t fit_supersample_factor(
    uint32_t asked, uint32_t width, uint32_t height, uint64_t budget, uint32_t texture_limit
) noexcept;

// ---------------------------------------------------------------------------
// Tiled textures

/// The largest tile, in texels, whatever the renderer allows.
inline constexpr uint32_t largest_tile_size = 2048;
/// The texels each tile carries from its neighbour on a side that has one,
/// so that LINEAR shows no seam.
inline constexpr uint32_t tile_gutter = 1;
/// The smallest tile a grid uses, in texels: room for content between two
/// gutters.
inline constexpr uint32_t smallest_tile_size = 3;

/// How a texture is split into tiles.
struct TileGrid {
    uint32_t width{};     ///< the texture's width in texels
    uint32_t height{};    ///< the texture's height in texels
    uint32_t tile_size{}; ///< the largest tile, gutters included; 0 when the texture is one tile
    uint32_t columns{};   ///< tiles across; 0 for an empty texture or a limit too small
    uint32_t rows{};      ///< tiles down
};

/// A rectangle in texels.
struct TexelRect {
    uint32_t x{};
    uint32_t y{};
    uint32_t width{};
    uint32_t height{};
};

/// One tile of a grid.
struct Tile {
    /// The texels the tile's texture holds, gutters included, in the whole
    /// texture's texels.
    TexelRect texture{};
    /// The texels it is drawn with, gutters left out, in the whole texture's
    /// texels.
    TexelRect content{};
    /// The content in the tile texture's own texels: its source rectangle.
    TexelRect source{};
};

/// Plans the tiles of a texture.
///
/// A texture within the renderer's limit, or with no limit, is one tile.
/// Beyond it, tiles are no larger than the lower of the limit and
/// largest_tile_size, gutters included, and carry a tile_gutter-texel
/// gutter on each side that has a neighbour.
///
/// @param width the texture's width in texels
/// @param height the texture's height in texels
/// @param limit the renderer's texture limit in texels (texture_limit);
///     0 for none
/// @return the grid; no tiles when the texture is empty or the limit is
///     under smallest_tile_size
[[nodiscard]] TileGrid plan_tiles(uint32_t width, uint32_t height, uint32_t limit) noexcept;

/// Returns one tile of a grid.
///
/// @param grid the grid
/// @param column the tile's column, under grid.columns
/// @param row the tile's row, under grid.rows
/// @return the tile
[[nodiscard]] Tile tile_at(const TileGrid& grid, uint32_t column, uint32_t row) noexcept;

} // namespace oa::app::render_policy

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The game's renderer and how it is made. Start-up walks SDL's render
// drivers one at a time, in SDL's own order, and keeps the first that
// starts, logging each refusal; with every driver working that is the
// driver SDL's own choice would make. Before SDL's software renderer, the
// last of the walk, it sets the framebuffer hint, so that the software
// renderer presents through the window's own framebuffer where there is
// one, and otherwise through the hardware drivers not recorded as failed.
// Under SDL_RENDER_DRIVER the start is SDL's own call, which tries only the
// drivers the variable names: no walk, no hint, and a failure ends the run.
// The order of the walk, the hint's value and when the walk starts over are
// the render policy's (render_policy.hpp); this is the part that acts. When a
// renderer fails while the game runs, it is made again the same way from
// the driver after the one that failed (RendererHost::rebuild).
//
// Once the renderer is made, the start decides the tier its first frame is
// drawn in from the command line, the setting, the environment, the
// machine's memory and what the probe found of the renderer, and logs it
// in the start-up line. Where the tier could be accelerated it first runs
// the start-up function test, which draws known patterns into render
// targets as the accelerated tier draws and reads them back
// (run_function_test). The facts the tier is decided from stay with the
// renderer for the runtime to keep up to date (RendererHost::tier_inputs).
//
// The host keeps the run's renderer records (renderer_state.hpp): read at
// the start, with what the last run left behind turned into strikes and
// records, the drivers recorded failed skipped by the walk, and the
// sentinel and the trial moved through the run's stages, from each
// driver's creation to `running` and each accelerated path's first frames.
// A failure while the game runs is struck against the driver that failed,
// and a clean exit erases the sentinel.
#pragma once

#include "oa/app/acceleration_status.hpp"
#include "oa/app/render_policy.hpp"
#include "oa/app/renderer_state.hpp"
#include "oa/platform/render_probe.hpp"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

struct SDL_Renderer;
struct SDL_Window;
union SDL_Event;

namespace oa::app {

/// What the error of a start that made no renderer begins with.
inline constexpr std::string_view renderer_creation_error = "SDL_CreateRenderer: ";
/// The reason given for a refusal that came with none.
inline constexpr std::string_view unexplained_refusal = "no reason given";
/// The reason given when there was no render driver to try.
inline constexpr std::string_view no_render_driver = "no render driver is available";

/// The reason a render driver gives when --render-fault create refuses it.
inline constexpr std::string_view refused_by_fault = "refused by --render-fault create";

/// What a test forces the start-up function test to draw wrongly, as a
/// graphics card that ignored a scale mode would; a player's run leaves
/// both false.
struct FunctionTestFaults {
    /// (b) draws its reduction by half NEAREST in place of LINEAR.
    bool half_nearest{};
    /// (d) draws its reduction at 0.75 NEAREST in place of LINEAR.
    bool pattern_nearest{};
};

/// What --check-renderer-ladder and the checks force of the renderer; a
/// player's run leaves every member empty.
struct RenderFaultHooks {
    void* context{};
    /// Says whether a render driver refuses to start, as if it had failed;
    /// null refuses none.
    bool (*refuse_driver)(void* context, std::string_view driver){};
    /// Answers in place of the renderer's device whether it can draw; null
    /// asks the device (oa::platform::render_probe::device_state).
    oa::platform::render_probe::DeviceState (*device_state)(void* context){};
    /// The texture limit in place of the renderer's, in texels; 0 keeps the
    /// renderer's.
    uint32_t texture_limit{};
    /// Answers in place of the window whether its pixels are 16-bit RGB565;
    /// null asks the window.
    bool (*rgb565_window)(void* context){};
    /// What the start-up function test draws wrongly (run_function_test).
    FunctionTestFaults function_test{};
    /// The physical memory taken in place of the machine's, in bytes; empty
    /// reads the machine's.
    std::optional<uint64_t> physical_memory{};
    /// How a left-over trial counts in place of the rule of the system the
    /// game runs on (renderer_state::crash_evidence); empty keeps that rule.
    std::optional<renderer_state::CrashEvidence> crash_evidence{};
    /// The name the records keep the renderer's driver under, in place of
    /// its own, so that SDL's software renderer, which is never recorded
    /// failed-driver, can stand for a driver that is; empty keeps its own.
    std::string record_driver{};
    /// The adapter the probe is taken to have read, in place of what it
    /// read, as a driver that describes the adapter in words of its own
    /// would; empty keeps what it read.
    std::string adapter{};
};

/// Where a start keeps the renderer records.
struct RecordsPlace {
    /// The folder of the player's own preferences file, which holds both
    /// files of the records; empty keeps the records and the sentinel in
    /// memory for the run, as with a named --preferences-file.
    std::filesystem::path folder{};
    /// The running engine's build: its version and the commit it was built
    /// from, which strikes, records and remembered rungs are written under,
    /// so that those of another build are dropped as the file is read.
    std::string engine_build{};
};

/// The clock the stages of the sentinel are timed by.
struct StageClock {
    void* context{};
    /// Returns the time in nanoseconds; null reads the steady clock.
    uint64_t (*now_ns)(void* context){};
};

/// A set of accelerated paths, one bit each (path_bit).
using PathSet = uint8_t;

/// Returns the bit of an accelerated path in a PathSet.
///
/// @param path the path
/// @return its bit
[[nodiscard]] constexpr PathSet path_bit(renderer_state::AcceleratedPath path) noexcept {
    return static_cast<PathSet>(1U << static_cast<unsigned>(path));
}

/// One attempt of a walk of the render drivers.
struct CreationAttempt {
    /// The render driver tried; empty for SDL's own choice, under SDL_RENDER_DRIVER.
    std::string driver{};
    bool created{};      ///< the driver made the renderer
    std::string error{}; ///< why it refused; empty when it made the renderer
};

/// What a walk of the render drivers calls to act.
struct CreationHooks {
    void* context{};
    /// Sets the framebuffer hint to a value; returns false, with the reason
    /// in error, when it was not taken, as when an environment variable
    /// takes priority. Null takes nothing.
    bool (*set_framebuffer_hint)(void* context, const std::string& value, std::string& error){};
    /// Creates the renderer of a render driver, or SDL's own choice for an
    /// empty name; returns false, with the reason in error, when it refused.
    /// Null refuses every driver.
    bool (*create)(void* context, const std::string& driver, std::string& error){};
    /// Logs one line, without its line break. Null logs nothing.
    void (*log)(void* context, const std::string& line){};
};

/// What a walk of the render drivers did.
struct CreationOutcome {
    std::vector<CreationAttempt> attempts{}; ///< every attempt, in the order made
    bool created{};                          ///< the last attempt made the renderer
    /// The walk passed over a driver recorded failed-driver, and did not
    /// start again with the records ignored.
    bool skipped_by_record{};
    /// For a start's walk that made the renderer: the drivers it passed over
    /// because the records hold them failed-driver, before the one it made,
    /// in SDL's order; empty when it passed over none.
    std::vector<std::string> skipped{};
    /// The records left nothing able to present, so the walk started again
    /// from the top with them ignored for the run.
    bool records_ignored{};
    /// When nothing was made: renderer_creation_error and the last refusal's
    /// reason, or no_render_driver when nothing was tried.
    std::string error{};
};

/// Returns the line logged when a render driver refuses to make the
/// renderer: "open-annihilation: graphics: renderer <driver> refused:
/// <reason>".
///
/// @param driver SDL's name for the render driver
/// @param reason why it refused
/// @return the line, without its line break
[[nodiscard]] std::string refusal_log_line(std::string_view driver, std::string_view reason);

/// Returns the line logged when the framebuffer hint was not taken.
///
/// @param value the hint's value
/// @param reason why it was not taken
/// @return the line, without its line break
[[nodiscard]] std::string
framebuffer_hint_log_line(std::string_view value, std::string_view reason);

/// Returns the line logged when no driver left could present and the walk
/// starts again from the top of SDL's order, with the drivers recorded as
/// failed tried too.
///
/// @return the line, without its line break
[[nodiscard]] std::string second_walk_log_line();

/// Walks the render drivers as the render policy plans it (start_creation
/// and next_attempt) until one makes the renderer: one driver at a time in
/// SDL's order, or SDL's own call once under SDL_RENDER_DRIVER. Each driver
/// that refuses is logged (refusal_log_line) and the walk goes on; SDL's
/// own call is not, since its failure ends the run with SDL's own words.
/// Before an attempt the policy marks, the framebuffer hint is set first;
/// a hint that is not taken is logged (framebuffer_hint_log_line), and the
/// walk goes on. The first attempt of a walk that starts again with the
/// records ignored is preceded by second_walk_log_line.
///
/// @param inputs the drivers in SDL's order, those recorded as failed, and
///     what the video driver and SDL's hint can take
/// @param hooks what sets the hint, makes the renderer and logs
/// @return every attempt, whether the last made the renderer, and the
///     error when none did
[[nodiscard]] CreationOutcome
walk_render_drivers(const render_policy::CreationInputs& inputs, const CreationHooks& hooks);

/// Walks the render drivers of a rebuild, as the render policy plans it
/// (start_rebuild and next_attempt), as walk_render_drivers walks those of
/// a start: the drivers after the one that failed, in SDL's order, or in
/// SDL_RENDER_DRIVER's list where it is set, then SDL's software renderer,
/// with the framebuffer hint set before it; and where nothing of that could
/// present, SDL's whole order again from the top.
///
/// @param inputs the drivers in SDL's order, SDL_RENDER_DRIVER's list, and
///     what the video driver and SDL's hint can take
/// @param failed_driver the driver that failed
/// @param hooks what sets the hint, makes the renderer and logs
/// @return every attempt, whether the last made the renderer, and the
///     error when none did
[[nodiscard]] CreationOutcome walk_rebuild_drivers(
    const render_policy::CreationInputs& inputs,
    std::string_view failed_driver,
    const CreationHooks& hooks
);

/// The name a failing call is struck under when its error names none.
inline constexpr std::string_view unnamed_call = "unnamed";

/// Returns the name the renderer records strike a failing call under: the
/// words of its error before the first colon, each run of characters other
/// than letters, digits, `_` and `-` made one `-`, none at either end, cut
/// to renderer_state::max_name_bytes, so that the same call failing again
/// is the same strike.
///
/// @param error the error, as "SDL_RenderClear: reason"
/// @return the name, or unnamed_call when nothing is left
[[nodiscard]] std::string failing_call_name(std::string_view error);

/// Returns the line logged when the start-up function test is skipped
/// because its trial record could not be written.
///
/// @return the line, without its line break
[[nodiscard]] std::string trial_unwritten_log_line();

/// Why the accelerated tier stops when a path's trial cannot be written.
inline constexpr std::string_view path_trial_unwritten_reason =
    "the trial of the graphics card's first use cannot be written";

/// Returns the line logged when an accelerated path is not used because
/// its trial record could not be written.
///
/// @param path the path
/// @return the line, without its line break
[[nodiscard]] std::string path_trial_unwritten_log_line(renderer_state::AcceleratedPath path);

/// Returns the line logged when a renderer fails while the game runs and
/// another is made: "open-annihilation: graphics: <driver> failed:
/// <reason>; making another renderer".
///
/// @param driver SDL's name for the render driver that failed
/// @param reason what failed
/// @return the line, without its line break
[[nodiscard]] std::string rebuild_log_line(std::string_view driver, std::string_view reason);

/// The most a channel of the start-up function test's LINEAR reduction by
/// half may differ from the average of the texels it covers: the graphics
/// card weighs neighbours to its own precision and rounds its own way.
inline constexpr int function_test_half_most_difference = 2;
/// The most a channel of the known pattern may differ from what the
/// processor computes for it after its two reductions. SDL's software
/// renderer weighs two texels in 128ths and truncates after each of its
/// passes, where the reference weighs exactly and rounds once, which reads
/// the pattern back up to 3 from it at a few texels.
inline constexpr int function_test_pattern_most_difference = 3;
/// The most the mean difference of the known pattern's channels may be.
inline constexpr double function_test_most_mean_difference = 0.5;

/// What the start-up function test found of a renderer.
struct FunctionTestResult {
    /// A render target read back the colour it was cleared to, a LINEAR
    /// reduction by half read back the average of the texels it covers,
    /// and the known pattern drawn as the accelerated tier draws read back
    /// as the processor computes it.
    bool passed{};
    bool pixelart{};       ///< the renderer's pixel-art scale mode works
    std::string failure{}; ///< what failed first; empty when it passed
};

/// Runs the start-up function test on a renderer, a few milliseconds of
/// work that only a tier that could be accelerated asks for:
/// (a) a 4x4 ARGB8888 render target is cleared and one pixel read back;
/// (b) a 4x4 texture is drawn LINEAR into a 2x2 target, which must read back
/// the average of each 2x2 block within function_test_half_most_difference;
/// (c) whether the pixel-art scale mode works (probe_pixelart);
/// (d) a seeded 32x32 ARGB8888 texture, drawn with no blending through a
/// source rectangle NEAREST at 2x into a 64x64 target, that target LINEAR
/// at 0.75 into a 48x48 target, and a 48x48 overlay, transparent but for an
/// opaque square, blended over it, must read back as the sharp-bilinear
/// reference at 1.5 and then the overlay rule
/// (oa::present::world_renderer::sharp_bilinear_rgb24, overlay_rgb24)
/// within function_test_pattern_most_difference and, on the mean,
/// function_test_most_mean_difference. A failure of (a), (b) or (d), or
/// of any SDL call they make, fails the test; (c) only chooses the filter.
/// Its textures and targets are destroyed and the render target set back
/// to the window when it ends.
///
/// @param renderer the renderer
/// @param faults what a test forces it to draw wrongly; empty in a
///     player's run
/// @return what it found
[[nodiscard]] FunctionTestResult
run_function_test(SDL_Renderer* renderer, const FunctionTestFaults& faults = {});

/// What the command line, the settings and the renderer records ask of the
/// window's pixel density as the game starts, before the window opens.
struct DensityRequest {
    /// The level a flag names (Options::hardware_acceleration); empty for
    /// no flag.
    std::optional<oa::ui::engine_settings::HardwareAcceleration> flag{};
    /// The platform the game is built for opens its windows at the
    /// display's own pixel density (the OA_NATIVE_DENSITY_WINDOWS build
    /// option, which main.cpp passes); false on the desktop.
    bool platform_native{};
    bool asked{};  ///< --native-density
    bool chosen{}; ///< the Native pixel density setting is on
    /// The Hardware acceleration setting read before the window opens.
    oa::ui::engine_settings::HardwareAcceleration setting{
        oa::ui::engine_settings::HardwareAcceleration::off
    };
    bool unattended{}; ///< a check, a benchmark or another scripted run
    bool capture{};    ///< the run captures video (--capture-video)
    /// The driver the native-density record names under the running
    /// engine's version (renderer_state::native_density_driver), read from
    /// the records the start opens before the window; empty when there is
    /// none, as with no records under SDL_RENDER_DRIVER.
    std::string record_driver{};
    /// The step-down rung remembered for that driver; none when none is,
    /// as at every start while the game writes no scale-level key.
    std::optional<render_policy::LadderState> remembered{};
};

/// Decides whether the game's window opens at the display's own pixel
/// density (render_policy::decide_native_density), from the request and
/// what the machine reports: its physical memory, the scene budget it
/// starts at with the record's driver, SDL_RENDER_DRIVER and the video
/// driver. No class of machine is measured at native density
/// (render_policy::native_density_measured), so only a platform whose
/// windows are at native density, --native-density and the Native pixel
/// density setting open the window at native density. A window that does
/// is logged, with the reason.
///
/// SDL's video must be started, and the window not yet made.
///
/// @param request what the command line, the settings and the records ask
/// @return whether the window opens at native density, and why
[[nodiscard]] render_policy::DensityDecision decide_window_density(const DensityRequest& request);

/// What the command line and the settings ask of the tier as the game
/// starts.
struct TierRequest {
    /// The level a flag names (Options::hardware_acceleration); empty for
    /// no flag.
    std::optional<oa::ui::engine_settings::HardwareAcceleration> flag{};
    bool force_capable{};       ///< --force-capable, which only a check passes
    bool players_own_profile{}; ///< no --preferences-file was named
    /// The Hardware acceleration setting the run starts with.
    oa::ui::engine_settings::HardwareAcceleration setting{
        oa::ui::engine_settings::HardwareAcceleration::off
    };
};

/// The game's renderer, made for its window and kept for the run, with what
/// the probe found of it and the attempts that made it, and the facts the
/// tier each frame is drawn in is decided from. HostDisplay owns one; the
/// runtime borrows it.
class RendererHost {
  public:

    RendererHost() = default;
    RendererHost(const RendererHost&) = delete;
    RendererHost& operator=(const RendererHost&) = delete;

    /// Destroys the renderer (destroy).
    ~RendererHost();

    /// Makes the renderer of a window and describes it
    /// (describe_game_renderer), on Windows before Vista with only direct3d
    /// capable (oa::platform::running_on_windows_before_vista);
    /// decide_start_tier logs it. With SDL_RENDER_DRIVER set, by SDL's own
    /// call, which tries only the drivers it names. Otherwise by walking
    /// SDL's render drivers (walk_render_drivers) in SDL's order, skipping
    /// those the records hold failed-driver, but walking again with the
    /// records ignored where they would leave nothing able to present, and
    /// stopping at the first that starts, with the sentinel `create
    /// <driver>` before each attempt. The framebuffer hint is set before
    /// SDL's software renderer only when an earlier driver refused or was
    /// skipped: "0" where the window has a framebuffer of its own
    /// (oa::platform::render_probe::native_window_framebuffer), otherwise
    /// the hardware drivers in SDL's order that no record skips, or the
    /// first of them alone when SDL's headers or library are older than
    /// 3.4. The sentinel then stands at `standard <driver>` while the
    /// probe reads the renderer, and, where the walk passed over no driver
    /// by record, the adapter it describes is noted in the records: each
    /// driver describes the adapter in words of its own, so only a start
    /// that makes the driver every such start makes compares like with
    /// like. Then the floating-point settings the game started with are
    /// put back, should the driver or the probe's reading of it have
    /// changed them.
    ///
    /// Throws std::runtime_error, beginning renderer_creation_error, when
    /// no renderer was made.
    ///
    /// @param window the game's window, which has no renderer yet
    /// @param faults what --check-renderer-ladder forces; empty in a
    ///     player's run. They are kept for the run (faults).
    /// @param records where the start keeps its records, read first
    ///     (open_records); empty keeps the records the host holds, which
    ///     until the first are none at all, as under SDL_RENDER_DRIVER
    void create(
        SDL_Window* window,
        const RenderFaultHooks& faults = {},
        const std::optional<RecordsPlace>& records = std::nullopt
    );

    /// Makes the renderer again after it failed while the game ran, and
    /// logs what was made (report_game_renderer), on the standard tier
    /// since the driver failed: destroys it, since a window holds one
    /// renderer, then walks the drivers after the one that failed
    /// (walk_rebuild_drivers), under SDL_RENDER_DRIVER those its list
    /// names, ending with SDL's software renderer. Then the floating-point
    /// settings the game started with are put back, should a driver have
    /// changed them. The tier's facts take the new renderer's capability,
    /// its function test is to run again, and acceleration is dropped
    /// for the run (render_policy::Drop::driver_failure) unless it was
    /// dropped already. Every texture made on the renderer must be
    /// destroyed first.
    ///
    /// The failure is struck against the driver that failed
    /// (note_running_failure), any accelerated path's stage under way is
    /// closed, and the trial the run wrote is erased; each attempt then
    /// stands under its `create` sentinel and the new renderer's first
    /// frames under `standard`, as at a start. The adapter the new driver
    /// describes is not noted, since its words for it are its own.
    ///
    /// Throws std::runtime_error, beginning renderer_creation_error, when
    /// no driver starts: the run ends, as a start does.
    ///
    /// @param reason what failed, for the log
    /// @param failure the failure as the records strike it: a present error,
    ///     a lost device or repeated resets; StrikeStage::none strikes
    ///     nothing
    void rebuild(std::string_view reason, const renderer_state::Strike& failure = {});

    /// Takes a render event that comes while no runtime handles events, as
    /// while the intro movies play: a lost device is noted for service; a
    /// reset needs nothing, since what draws then makes its own textures
    /// again.
    ///
    /// @param event any event
    /// @return true for the three render events, which nothing else needs
    bool take_event(const SDL_Event& event);

    /// Makes the renderer again (rebuild) when take_event noted a lost
    /// device, and does nothing otherwise.
    ///
    /// Throws std::runtime_error when no driver starts.
    void service();

    /// Destroys the renderer, if there is one; the window stays.
    void destroy() noexcept;

    /// Returns the renderer.
    ///
    /// @return the renderer; null before create, after destroy or in a
    ///     headless run
    [[nodiscard]] SDL_Renderer* renderer() const noexcept;

    /// Returns what the probe found of the renderer.
    ///
    /// @return the facts; empty while there is no renderer
    [[nodiscard]] const oa::platform::render_probe::AdapterFacts& facts() const noexcept;

    /// Returns the attempts of the walk that made the renderer, or of the
    /// last walk, which made none.
    ///
    /// @return the attempts, in the order made
    [[nodiscard]] std::span<const CreationAttempt> attempts() const noexcept;

    /// Says whether SDL_RENDER_DRIVER named the drivers at the start.
    ///
    /// @return true when it did
    [[nodiscard]] bool named() const noexcept;

    /// Returns the texture formats of the opaque layers on this renderer
    /// (render_policy::layer_formats), with its window's pixels as they were
    /// when it was made. The match's layers follow the window as it is now
    /// (opaque_format).
    ///
    /// @return the formats
    [[nodiscard]] const render_policy::LayerFormats& layer_formats() const noexcept;

    /// Returns the texture format of the match's opaque layers now. On SDL's
    /// software renderer it follows the window's pixels, which a display
    /// mode of another depth or a move to another display can change while
    /// the game runs, so the window is asked at each call; on every other
    /// renderer it is layer_formats' and fixed for the renderer.
    ///
    /// @return the format
    [[nodiscard]] render_policy::LayerFormat opaque_format() const;

    /// Returns the largest texture side the game makes: the faults' limit
    /// where it is set, otherwise the renderer's corrected one.
    ///
    /// @return texels; 0 for no limit
    [[nodiscard]] uint32_t texture_limit() const noexcept;

    /// Asks the renderer's device whether it can draw, or the faults where
    /// they answer.
    ///
    /// @return the device's state; unknown on a renderer whose device cannot
    ///     say
    [[nodiscard]] oa::platform::render_probe::DeviceState device_state() const;

    /// Returns what --check-renderer-ladder forces.
    ///
    /// @return the faults, which the check may change
    [[nodiscard]] RenderFaultHooks& faults() noexcept;

    /// Decides the tier the first frame is drawn in and logs the start-up
    /// line (graphics_log_line, with tier_description).
    ///
    /// Fills the tier's facts: the request, SDL_RENDER_DRIVER as the start
    /// saw it, whether the video driver draws no window, the machine's
    /// physical memory as the system reports it
    /// (oa::platform::sample_system_memory), and the capability probe items
    /// 1 to 3 found (render_policy::assess_renderer), with the adapter read
    /// under SDL_RENDER_DRIVER too when --hardware-acceleration or
    /// --force-capable asks for more than SDL's own start; and the facts the
    /// starting rung is sized from; and what the records say of the driver
    /// (an accelerated-unusable record, records that could not be read
    /// after an unclean start). Where the tier could be accelerated but for
    /// the function test, and so never under 2 GiB, runs it first
    /// (render_policy::step_tier, test_function).
    ///
    /// @param request what the command line and the settings ask
    void decide_start_tier(const TierRequest& request);

    /// Runs the start-up function test on the renderer (run_function_test,
    /// with the faults' function_test), puts back the floating-point
    /// settings the game started with, and keeps what it found: the
    /// function test passed or failed in the tier's facts, logging a
    /// failure, and whether the pixel-art scale mode works for the starting
    /// rung. The trial `probe <driver>` is written first and stands until
    /// the start-up stage passes, and the sentinel stands at `probe
    /// <driver>` through the test and at `standard <driver>` after it.
    /// Where the trial cannot be written the test does not run: its state
    /// becomes render_policy::FunctionTest::trial_unwritten, which keeps the
    /// standard tier until the player tries again, and the skip is logged.
    void test_function();

    /// Returns the hooks through which render_policy::step_tier runs the
    /// start-up function test on this renderer (test_function).
    ///
    /// @return the hooks, valid while the host lives
    [[nodiscard]] render_policy::FunctionTestHooks function_test_hooks() noexcept;

    /// Returns the facts the tier is decided from: those of the start, the
    /// function test's result, and what the runtime keeps up to date (the
    /// setting, a drop, a lost device, the director and a shared game).
    ///
    /// @return the facts
    [[nodiscard]] render_policy::TierInputs& tier_inputs() noexcept;

    /// Returns the facts the tier is decided from.
    ///
    /// @return the facts
    [[nodiscard]] const render_policy::TierInputs& tier_inputs() const noexcept;

    /// Returns the rung the accelerated tier starts at on this machine and
    /// renderer (render_policy::start_rung).
    ///
    /// @return the rung
    [[nodiscard]] render_policy::LadderState start_rung() const noexcept;

    /// Reads the records a start keeps and applies what the last run left
    /// behind (renderer_state::RendererState::resolve_leftovers), writing
    /// the strikes and records that made: under SDL_RENDER_DRIVER none at
    /// all; with no folder, records in memory for the run; otherwise the two
    /// files in the folder. How a left-over trial counts follows the
    /// system the game runs on, and on a machine under 2 GiB nothing of the
    /// accelerated tier is struck or recorded (renderer_state::RecordRules).
    /// The game's start calls it before the window opens, so that the
    /// native-density key reaches the window's density, and create keeps
    /// what it read; create calls it when it is given a place, and a check
    /// calls create again with one to start afresh on what the files hold,
    /// as a new start of the game would.
    ///
    /// @param place where the records live
    void open_records(const RecordsPlace& place);

    /// Returns the run's records.
    ///
    /// @return the records and their files
    [[nodiscard]] renderer_state::RendererState& records() noexcept;

    /// Returns the run's records.
    ///
    /// @return the records and their files
    [[nodiscard]] const renderer_state::RendererState& records() const noexcept;

    /// Returns where the records were last opened.
    ///
    /// @return the place
    [[nodiscard]] const RecordsPlace& records_place() const noexcept;

    /// Returns what the start made of what the last run left behind.
    ///
    /// @return the outcome of open_records
    [[nodiscard]] const renderer_state::LeftoverOutcome& leftovers() const noexcept;

    /// Returns the lines the records logged since they were opened.
    ///
    /// @return the lines, without their line breaks
    [[nodiscard]] std::span<const std::string> records_log() const noexcept;

    /// Returns the name the records keep the renderer's driver under: the
    /// faults' record_driver where it is set, otherwise the renderer's own.
    ///
    /// @return the name; empty while there is no renderer
    [[nodiscard]] std::string record_driver() const;

    /// Returns the drivers the start's walk passed over because the records
    /// hold them failed-driver, before the driver it made
    /// (CreationOutcome::skipped).
    ///
    /// @return the drivers, in SDL's order; empty when none was skipped or
    ///     the walk ignored the records
    [[nodiscard]] std::span<const std::string> skipped_drivers() const noexcept;

    /// Says whether the start's walk ignored the records, since they left
    /// nothing able to present; the rebuilds of the run ignore them too.
    ///
    /// @return true when it walked again with them ignored
    [[nodiscard]] bool records_ignored() const noexcept;

    /// Sets the clock the stages of the sentinel are timed by.
    ///
    /// @param clock the clock; an empty one reads the steady clock
    void set_stage_clock(const StageClock& clock) noexcept;

    /// Notes the first accelerated frame of a start or of a retry: the
    /// sentinel moves from `standard` to `accelerated`.
    void note_first_accelerated_frame();

    /// Notes a presented frame, of the menus, a loading screen or a match:
    /// after renderer_state::start_stage_frames frames and
    /// renderer_state::start_stage_ns the start-up stage has passed, so its
    /// strikes are cleared, the sentinel becomes `running` and its trial is
    /// erased; after renderer_state::path_stage_frames frames drawn with an
    /// accelerated path whose first frames stand under its own sentinel, so
    /// has the path's.
    ///
    /// @param drawn the accelerated paths the frame was drawn with
    void note_presented_frame(PathSet drawn);

    /// Says whether the start-up stage stands: the renderer's first frames,
    /// which note_presented_frame counts until it passes. A screen that
    /// waits for the player keeps presenting while it stands, so that a run
    /// the system ends there is not taken for the driver's failure.
    ///
    /// @return true while the sentinel stands at `standard` or `accelerated`
    [[nodiscard]] bool start_stage_open() const noexcept;

    /// Takes note that an accelerated path is about to be used. Its first
    /// use in the run, once the start-up stage has passed, writes the trial
    /// `path <name> <driver>` and the sentinel `path <name> <driver>`; while
    /// another stage stands that stage covers it, and nothing is written.
    ///
    /// @param path the path
    /// @return false when the path's trial could not be written (logged):
    ///     the path is not used, and a later call tries the write again
    [[nodiscard]] bool begin_path(renderer_state::AcceleratedPath path);

    /// Closes the stage of an accelerated path's first frames when the
    /// accelerated tier stops before it has passed: the sentinel goes back
    /// to `running` and the trial is erased, its strike left as it is.
    void end_path_stage();

    /// Strikes a failure seen while the game runs against the renderer's
    /// driver (renderer_state::note_running_failure) and writes the records
    /// when they changed. Nothing is struck or recorded under
    /// SDL_RENDER_DRIVER, against a driver whose device is lost in
    /// ordinary use, or against SDL's software renderer unless
    /// --force-capable runs the accelerated tier on it.
    ///
    /// @param failure the failure: a present error, an accelerated-only
    ///     call, a lost device or repeated resets
    /// @return what changed
    renderer_state::Change note_running_failure(const renderer_state::Strike& failure);

    /// Clears the strikes and the failure records in memory, as switching
    /// Hardware acceleration set to Off and back or Restore defaults does, and lets
    /// a start whose records could not be read try again. The clearing
    /// waits for OK (keep_cleared_records) or Cancel (restore_records):
    /// until then the file keeps what it took away.
    ///
    /// @return what changed
    renderer_state::Change clear_records();

    /// Writes the records clear_records cleared, as OK does
    /// (renderer_state::RendererState::confirm_clear), best effort.
    void keep_cleared_records() noexcept;

    /// Puts back the strikes, failure records and remembered rungs that
    /// clear_records took away, as Cancel does, with what was struck or
    /// recorded since (renderer_state::RendererState::restore_failures),
    /// and writes the records, best effort.
    void restore_records() noexcept;

    /// Tells the records whether a match runs, during which they are written
    /// only at its end; at the end it writes them.
    ///
    /// @param running a match runs
    void set_match_running(bool running);

    /// Fills the facts the settings dialog's status reads from the records:
    /// a driver skipped by a record at this start, whether the skipped
    /// drivers' records still stand, and what the records hold against the
    /// renderer's driver or, where it is not able, against the drivers
    /// skipped before it.
    ///
    /// @param[in,out] facts the facts
    void fill_record_facts(AccelerationFacts& facts) const;

    /// Ends the run's records cleanly, as every exit through main does: the
    /// strikes of failures the renderer's driver did not see again are
    /// cleared, the run's trial erased, the records written and the
    /// sentinel deleted. Call it before destroy.
    void finish_records() noexcept;

  private:

    /// Notes what was made: the facts, with the adapter read as
    /// adapter_asked_ says or the faults' in its place, the capability, the
    /// layers' formats and the floating-point settings put back.
    void take_renderer();

    /// Walks the drivers of a start or a rebuild with the records' failed
    /// drivers, keeps what the walk made, and stands the new renderer's
    /// sentinel at `standard` while the probe reads it.
    ///
    /// Throws std::runtime_error when no driver starts.
    ///
    /// @param failed_driver the rebuild's driver that failed; empty for a start
    /// @param rebuilding the walk is a rebuild's
    void make_renderer(const std::string& failed_driver, bool rebuilding);

    /// Moves the sentinel and the trial for an event of the run
    /// (renderer_state::sentinel_step) and writes what it asks.
    ///
    /// @param event the event
    /// @param path the path, for the path events
    /// @param driver the driver the sentinel names; empty for the renderer's
    ///     (record_driver)
    /// @return false when a trial it asked for could not be written, and the
    ///     stage it would cover is then skipped
    bool step_sentinel(
        renderer_state::LifeEvent event,
        renderer_state::AcceleratedPath path = renderer_state::AcceleratedPath::magnify,
        const std::string& driver = {}
    );

    /// Starts the timing of a stage of the sentinel.
    void begin_stage() noexcept;

    /// Returns the time the stages are timed by.
    ///
    /// @return nanoseconds
    [[nodiscard]] uint64_t stage_now_ns() const;

    /// Returns the machine's physical memory, or the faults' in its place.
    ///
    /// @return bytes; 0 when the system does not say
    [[nodiscard]] uint64_t machine_memory() const noexcept;

    /// Takes into the tier's facts whether the records hold the renderer's
    /// driver accelerated-unusable; where that cannot be told, as when
    /// memory runs short, the fact is left as it was.
    void sync_record_facts() noexcept;

    /// Says whether the window's pixels are 16-bit RGB565, or the faults'
    /// answer where they give one.
    ///
    /// @return true for RGB565
    [[nodiscard]] bool rgb565_window() const;

    SDL_Window* window_{};
    SDL_Renderer* renderer_{};
    oa::platform::render_probe::AdapterFacts facts_{};
    std::vector<CreationAttempt> attempts_{};
    RenderFaultHooks faults_{};
    render_policy::LayerFormats layer_formats_{};
    render_policy::TierInputs tier_{};     ///< what the tier is decided from
    render_policy::StartInputs machine_{}; ///< what the starting rung is sized from
    bool named_{};                         ///< SDL_RENDER_DRIVER named the drivers at the start
    /// A flag asks for the adapter under SDL_RENDER_DRIVER, which otherwise
    /// reads none.
    bool adapter_asked_{};
    bool lost_noted_{}; ///< take_event saw the device lost; service rebuilds
    /// The run's records: none at all until open_records.
    renderer_state::RendererState records_{renderer_state::RendererState::disabled()};
    RecordsPlace place_{};                        ///< where the records were opened
    renderer_state::LeftoverOutcome leftovers_{}; ///< what the start made of the last run's
    std::vector<std::string> records_log_{};      ///< the records' lines since they were opened
    renderer_state::SentinelLife life_{};         ///< where the sentinel and the trial stand
    /// For SDL's software renderer presenting through the framebuffer
    /// hint's list of drivers: that list, which its sentinels carry.
    std::vector<std::string> via_{};
    /// The driver the walk made the renderer of; empty for SDL's own choice.
    std::string driver_made_{};
    std::vector<std::string> skipped_drivers_{}; ///< the start's drivers passed over by record
    bool records_ignored_{};                     ///< the start's walk ignored the records
    StageClock clock_{};                         ///< what the stages are timed by
    uint32_t stage_frames_{};                    ///< frames presented in the start-up stage
    uint64_t stage_started_ns_{};                ///< when the start-up stage began
    uint32_t path_frames_{}; ///< frames drawn with the path whose first frames stand
    PathSet paths_begun_{};  ///< the accelerated paths used in the run
    /// The function test ran in the start-up stage that stands, so its
    /// passing clears a probe strike too.
    bool function_test_ran_{};
};

} // namespace oa::app

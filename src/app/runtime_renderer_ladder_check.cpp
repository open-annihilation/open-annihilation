// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-renderer-ladder: each renderer failure the game handles while it
// runs, forced on the renderer the start made, and the game presenting on
// through it. On the dummy video driver every hardware driver refuses, so
// the walk ends on SDL's software renderer, and every rebuild makes that
// renderer again. Then the renderer records across simulated restarts of
// the game, each reading the files a start in a scratch folder finds, as
// the player's own profile keeps them: strikes and records of crashes and
// of failures while running, the walk that skips a recorded driver and
// walks again when that leaves nothing able to present, the adapter as
// another driver words it, a trial that cannot be written, the 2 GiB rule,
// the main menu's notice and the settings dialog's retry; and the Full
// tier's own fallbacks, switched on as --hardware-acceleration=full and
// --force-capable would: a card call that fails, which drops Full to Basic
// with a strike, the same in the next run recorded full-unusable and told
// once, and a raise of the row clearing it; Full's left-over trial, struck
// once and recorded twice, or once where the first counts; its trial that
// cannot be written; and a shared game, in which a lower tier applies at
// once, a higher one from the next game, and every page is made as the
// loading screen begins. Four cases run only when --render-fault names
// them, since they switch the accelerated tier on where every other case
// draws in the standard tier: slow frames, which walk the step-down down
// its ladder to the standard tier, and the memory guard, which refuses the
// tier's buffers and then drops it; and the same for Full, whose rungs the
// step-down takes first and whose pages the guard drops first.
#include "oa/app/runtime.hpp"
#include "engine_settings_match_host.hpp"
#include "engine_settings_state.hpp"
#include "full_presentation.hpp"
#include "graphics_report.hpp"
#include "render_host.hpp"
#include "render_run.hpp"
#include "oa/app/frame_pacing.hpp"
#include "oa/app/renderer_state.hpp"
#include "oa/base/float_precision.hpp"
#include "oa/platform/preferences.hpp"
#include "oa/platform/render_probe.hpp"
#include "oa/ui/frontend_dialogs.hpp"
#include "oa/ui/frontend_state/app_modes.hpp"
#include <SDL3/SDL.h>
#include <algorithm>
#include <cfenv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace oa::app {
namespace {

namespace policy = render_policy;
namespace render_probe = oa::platform::render_probe;

/// Menu frames the walk's case presents.
constexpr uint32_t walk_menu_frames = 60;
/// The presented frame of its case a failure in a match comes at, unless
/// --render-fault gives one.
constexpr uint32_t match_case_frame = 10;
/// Frames a forced failure may take to come before the case fails.
constexpr uint32_t fault_frames_allowed = 1000;
/// Stalls the stall rule logs at.
constexpr uint32_t stalls_logged_at = 3;
/// The texture limit the tiles' case forces, texels.
constexpr uint32_t tiles_texture_limit = 2048;
/// The window the tiles' case draws, pixels: its battlefield is wider than
/// the limit.
constexpr int tiles_window_width = 2560;
constexpr int tiles_window_height = 1440;
/// How long before now the stall case says its match began, nanoseconds:
/// past a match's first 5 s, whose frames are not steady.
constexpr uint64_t match_begun_ns = 6'000'000'000;
/// Exit code of a check that skipped, which ctest reports as skipped.
constexpr int skipped_exit_code = 77;
/// Frames the memory case presents for the tier to settle on the rungs
/// below the buffers the memory guard refuses.
constexpr uint32_t refused_buffer_frames = 4;
/// The zooms the slow frames case presents at: one the area pass reduces,
/// zoom 1, and one the card magnifies.
constexpr float zoomed_out_zoom = 0.5F;
constexpr float zoom_one = 1.0F;
constexpr float zoomed_in_zoom = 2.0F;
/// A zoom that is not whole, at which the card magnifies the scene by its
/// own filter, not by NEAREST.
constexpr float part_zoom = 1.5F;
/// The window the memory case draws, pixels: its chrome scales by 1.6, so
/// the HUD is drawn through a prescale target.
constexpr int part_scale_width = 1024;
constexpr int part_scale_height = 768;
/// Frames the Full cases present for the card to draw a frame's terrain.
constexpr uint32_t full_frames = 3;

/// Counts the reports of a changed floating-point setting.
///
/// @param context the count
void count_float_change(
    void* context,
    const oa::base::float_precision::FloatControl&,
    const oa::base::float_precision::FloatControl&
) {
    ++*static_cast<uint32_t*>(context);
}

/// Answers the device's state from the case's own answer.
///
/// @param context the answer
/// @return the answer
render_probe::DeviceState answered_state(void* context) {
    return *static_cast<render_probe::DeviceState*>(context);
}

namespace rs = renderer_state;
namespace preferences = oa::platform::preferences;

/// The physical memory the records' cases take the machine to have, so
/// that they run alike on every machine: 8 GiB, or 1 GiB for the case of a
/// machine under 2 GiB.
constexpr uint64_t records_memory = uint64_t{8} << 30;
constexpr uint64_t small_memory = uint64_t{1} << 30;
/// The window the records' cases draw: the front end at a scale that is not
/// a whole number, which the graphics card scales through a prescale
/// target where the pixel-art scale mode is missing.
constexpr int records_window_width = 1000;
constexpr int records_window_height = 750;
/// Attempts at a scratch folder of a name no other run has.
constexpr int scratch_attempts = 64;
/// Device resets that make the renderer again.
constexpr int resets_that_rebuild = 3;
/// The zoom the path cases magnify the battlefield at: one the card scales
/// sharp-bilinear through the scene's prescale target.
constexpr float path_zoom = 1.37F;
/// Frames the notice case draws while it waits for the main menu, and the
/// most it waits for a screen to change.
constexpr uint32_t notice_wait_frames = 30;
/// The adapter the adapter case's driver describes, and how the driver a
/// record's skip or a rebuild lands on words the same adapter.
constexpr std::string_view case_adapter = "Example Graphics 3000";
constexpr std::string_view case_adapter_other_words = "Example Graphics 3000/PCIe/SSE2";
/// Another adapter altogether.
constexpr std::string_view other_adapter = "Example Graphics 4000";

/// Reads the check's clock, which the stages of the sentinel are timed by.
///
/// @param context the clock, in nanoseconds
/// @return its time
uint64_t read_check_clock(void* context) {
    return *static_cast<uint64_t*>(context);
}

/// Refuses SDL's software renderer as many times as the count left says,
/// as if it had failed.
///
/// @param context the refusals left
/// @param driver SDL's name for the render driver
/// @return true for software while refusals are left
bool refuse_software(void* context, std::string_view driver) {
    auto& left = *static_cast<uint32_t*>(context);
    if (driver != render_probe::software_renderer || left == 0)
        return false;
    --left;
    return true;
}

/// A folder of the check's own, of a name no other run has, under the
/// folder the check writes its reports in, removed with everything in it
/// when the check ends.
struct ScratchFolder {
    fs::path path{};

    explicit ScratchFolder(const fs::path& parent) {
        fs::create_directories(parent);
        // Making a folder fails where one of the name stands, so two runs at
        // once never share one.
        const auto stamp =
            static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        for (int attempt = 0; attempt < scratch_attempts && path.empty(); ++attempt) {
            const fs::path folder = parent / ("renderer-records-" + std::to_string(stamp) + "-" +
                                              std::to_string(attempt));
            std::error_code error;
            if (fs::create_directory(folder, error))
                path = folder;
        }
        if (path.empty())
            throw std::runtime_error("renderer ladder check: no scratch folder could be made");
    }

    ScratchFolder(const ScratchFolder&) = delete;
    ScratchFolder& operator=(const ScratchFolder&) = delete;

    ~ScratchFolder() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

/// Writes a file's text.
///
/// @param file the file
/// @param text its text
void write_text(const fs::path& file, std::string_view text) {
    std::ofstream output(file, std::ios::binary | std::ios::trunc);
    output << text;
    if (!output)
        throw std::runtime_error("renderer ladder check: cannot write " + file.string());
}

} // namespace

/// The cases, over one runtime and its renderer.
struct Runtime::RendererLadder {
    Runtime& runtime;
    fs::path report_directory{};
    std::vector<std::string> passed{};

    /// Returns the runtime's renderer state.
    RenderRun& run() { return *runtime.render_run_; }

    /// Throws when a case's expectation does not hold.
    ///
    /// @param holds the expectation
    /// @param where the case
    /// @param what what was expected
    static void expect(bool holds, std::string_view where, std::string_view what) {
        if (!holds)
            throw std::runtime_error(
                "renderer ladder check: " + std::string(where) + ": " + std::string(what)
            );
    }

    /// Forces a failure at a presented frame from now.
    ///
    /// @param point the failure
    /// @param frame the presented frame from now it comes at, from 1
    void arm(RenderFaultPoint point, uint32_t frame) {
        run().fault = point;
        run().fault_frame = run().presented + frame;
    }

    /// Presents frames until the forced failure has come.
    ///
    /// @param where the case
    void present_until_fault(std::string_view where) {
        for (uint32_t frame = 0; frame < fault_frames_allowed && run().fault; ++frame)
            runtime.render();
        expect(!run().fault, where, "the forced failure never came");
    }

    /// Posts a render event for the game's window and lets the event
    /// dispatch take it, as the game's loop does.
    ///
    /// @param type the event
    void post_render_event(uint32_t type) {
        SDL_Event event = render_event(type);
        expect(SDL_PushEvent(&event), "events", "SDL refused the event");
        bool running = true;
        SDL_Event taken{};
        while (SDL_PollEvent(&taken))
            runtime.dispatch_event(taken, running);
    }

    /// Returns a render event for the game's window.
    ///
    /// @param type the event
    /// @return the event
    SDL_Event render_event(uint32_t type) const {
        SDL_Event event{};
        event.type = type;
        event.render.windowID = SDL_GetWindowID(runtime.sdl_.window);
        return event;
    }

    /// Leaves any match for the main menu.
    void to_main_menu() {
        if (runtime.match_)
            runtime.leave_match();
        runtime.load(Screen::main_menu);
    }

    /// Starts the benchmark skirmish from the main menu, with the pointer in
    /// the blank corner right of the bottom bar.
    void start_match() {
        to_main_menu();
        runtime.start_benchmark_skirmish();
        runtime.update_pointer(
            static_cast<float>(runtime.match_layout_.width - 1),
            static_cast<float>(runtime.match_layout_.height - 1)
        );
    }

    /// Checks the next frame against the composed one.
    ///
    /// @param where the case
    void expect_composed(std::string_view where) {
        runtime.expect_presented_equals_composed(
            report_directory, "renderer-ladder-" + std::string(where)
        );
    }

    /// The walk of the render drivers ended on SDL's software renderer
    /// after every earlier driver of SDL's order refused, with the
    /// framebuffer hint set; then menu frames present.
    void walk(bool forced) {
        constexpr std::string_view where = "walk";
        const auto& host = *run().host;
        expect(!host.named(), where, "SDL_RENDER_DRIVER is set; the check walks SDL's drivers");
        const auto attempts = host.attempts();
        expect(!attempts.empty() && attempts.back().created, where, "no driver made the renderer");
        expect(
            host.facts().renderer == render_probe::software_renderer,
            where,
            "the walk ended on " + host.facts().renderer + ", not software"
        );
        for (std::size_t index = 0; index < attempts.size(); ++index) {
            const char* name = SDL_GetRenderDriver(static_cast<int>(index));
            expect(
                name != nullptr && attempts[index].driver == name,
                where,
                "attempt " + std::to_string(index) + " is not SDL's driver of that place"
            );
            if (index + 1 == attempts.size())
                continue;
            expect(
                !attempts[index].created,
                where,
                attempts[index].driver + " both refused and started"
            );
            expect(
                !attempts[index].error.empty(),
                where,
                attempts[index].driver + " refused with no reason"
            );
            if (forced)
                expect(
                    attempts[index].error == refused_by_fault,
                    where,
                    attempts[index].driver + " was not refused by --render-fault create"
                );
        }
        const char* hint = SDL_GetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION);
        if (attempts.size() > 1)
            expect(
                hint != nullptr && std::string_view(hint) == "0",
                where,
                "the framebuffer hint is not 0 after a driver refused"
            );
        const uint64_t before = run().presented;
        for (uint32_t frame = 0; frame < walk_menu_frames; ++frame)
            runtime.render();
        expect(
            run().presented == before + walk_menu_frames && run().rebuilds == 0,
            where,
            "the menu frames were not all presented"
        );
        passed.emplace_back(where);
    }

    /// A present that fails in a match makes the renderer again, and the
    /// next frame is the composed one.
    void present(uint32_t frame) {
        constexpr std::string_view where = "present";
        start_match();
        const uint32_t rebuilds = run().rebuilds;
        arm(RenderFaultPoint::present, frame);
        present_until_fault(where);
        expect(run().rebuilds == rebuilds + 1, where, "the renderer was not made again");
        expect(
            runtime.sdl_.renderer == run().host->renderer(),
            where,
            "the runtime kept the old renderer"
        );
        expect_composed(where);
        passed.emplace_back(where);
    }

    /// A present that fails on a loading frame, in the display sink, makes
    /// the renderer again at the loading pump's next frame.
    void loading_present() {
        constexpr std::string_view where = "loading-present";
        to_main_menu();
        const uint32_t rebuilds = run().rebuilds;
        // The next present is the load's first frame.
        arm(RenderFaultPoint::present, 1);
        runtime.start_benchmark_skirmish();
        expect(!run().fault, where, "the forced failure never came");
        expect(run().rebuilds == rebuilds + 1, where, "the renderer was not made again");
        expect(
            run().rebuilt_on == Screen::loading, where, "the loading pump did not make it again"
        );
        expect(run().pending_rebuild.empty(), where, "a rebuild still waits");
        runtime.update_pointer(
            static_cast<float>(runtime.match_layout_.width - 1),
            static_cast<float>(runtime.match_layout_.height - 1)
        );
        expect_composed(where);
        passed.emplace_back(where);
    }

    /// A texture SDL cannot make again leaves none behind: the old one is
    /// forgotten with its size, and the next frame makes it again and is
    /// the composed one.
    void texture() {
        constexpr std::string_view where = "texture";
        start_match();
        runtime.render();
        expect(runtime.match_hud_tex_ != nullptr, where, "the match drew no HUD texture");
        bool thrown = false;
        try {
            // SDL makes no texture of no size.
            runtime.ensure_streaming_texture(
                runtime.match_hud_tex_,
                runtime.opaque_layer_format(),
                0,
                0,
                runtime.match_hud_tex_w_,
                runtime.match_hud_tex_h_
            );
        } catch (const PresentError&) {
            thrown = true;
        }
        expect(thrown, where, "a texture of no size was made");
        expect(
            runtime.match_hud_tex_ == nullptr && runtime.match_hud_tex_w_ == 0 &&
                runtime.match_hud_tex_h_ == 0,
            where,
            "the destroyed texture was kept"
        );
        expect_composed(where);
        expect(runtime.match_hud_tex_ != nullptr, where, "the HUD texture was not made again");
        passed.emplace_back(where);
    }

    /// A device reset in a match forgets every texture; the next frame
    /// makes them again and is the composed one.
    void reset(uint32_t frame) {
        constexpr std::string_view where = "reset";
        start_match();
        for (uint32_t presented = 1; presented < frame; ++presented)
            runtime.render();
        const uint32_t resets = forget_resets();
        const uint32_t rebuilds = run().rebuilds;
        expect(runtime.match_world_tex_.tile_count() > 0, where, "the match drew no world texture");
        post_render_event(SDL_EVENT_RENDER_DEVICE_RESET);
        expect_reset_taken(where, resets, Screen::match);
        expect_composed(where);
        expect(
            runtime.match_world_tex_.tile_count() > 0, where, "the world texture was not made again"
        );
        expect(run().rebuilds == rebuilds, where, "a single reset made the renderer again");
        passed.emplace_back(where);
    }

    /// Forgets the device resets of the cases before, so that a case's own
    /// reset is never the third within a minute.
    ///
    /// @return the resets handled so far
    uint32_t forget_resets() {
        run().resets = {};
        return run().resets_handled;
    }

    /// Checks that one device reset more was taken, on a screen, and that
    /// it forgot the match's textures.
    ///
    /// @param where the case
    /// @param resets the resets handled before it
    /// @param screen the screen it was taken on
    void expect_reset_taken(std::string_view where, uint32_t resets, Screen screen) {
        expect(run().resets_handled == resets + 1, where, "the reset was not handled");
        expect(run().reset_on == screen, where, "the reset was taken on another screen");
        expect(
            runtime.match_world_tex_.tile_count() == 0 && runtime.match_hud_tex_ == nullptr,
            where,
            "the textures were not forgotten"
        );
    }

    /// A device reset in a match that a drain of input meets is taken, not
    /// dropped: the textures are forgotten and the next frame makes them
    /// again.
    void drain_reset() {
        constexpr std::string_view where = "drain-reset";
        start_match();
        runtime.render();
        const uint32_t resets = forget_resets();
        const uint32_t rebuilds = run().rebuilds;
        SDL_Event event = render_event(SDL_EVENT_RENDER_DEVICE_RESET);
        expect(SDL_PushEvent(&event), where, "SDL refused the event");
        runtime.drain_input();
        expect_reset_taken(where, resets, Screen::match);
        expect(
            !SDL_HasEvents(SDL_EVENT_RENDER_TARGETS_RESET, SDL_EVENT_RENDER_DEVICE_LOST),
            where,
            "a render event was left in the queue"
        );
        expect_composed(where);
        expect(run().rebuilds == rebuilds, where, "a single reset made the renderer again");
        passed.emplace_back(where);
    }

    /// A device reset the movie player hands on is taken as the game takes
    /// one; another event is not.
    void movie_reset() {
        constexpr std::string_view where = "movie-reset";
        start_match();
        runtime.render();
        const uint32_t resets = forget_resets();
        SDL_Event key{};
        key.type = SDL_EVENT_KEY_DOWN;
        Runtime::take_movie_event(&runtime, key);
        expect(run().resets_handled == resets, where, "a key was taken as a reset");
        Runtime::take_movie_event(&runtime, render_event(SDL_EVENT_RENDER_DEVICE_RESET));
        expect_reset_taken(where, resets, Screen::match);
        expect_composed(where);
        passed.emplace_back(where);
    }

    /// A device reset during a load is handled by the loading pump, before
    /// the match's first frame.
    void loading_reset() {
        constexpr std::string_view where = "loading-reset";
        to_main_menu();
        const uint32_t resets = forget_resets();
        SDL_Event event = render_event(SDL_EVENT_RENDER_DEVICE_RESET);
        expect(SDL_PushEvent(&event), where, "SDL refused the event");
        runtime.start_benchmark_skirmish();
        expect(run().resets_handled == resets + 1, where, "the load did not handle the reset");
        expect(run().reset_on == Screen::loading, where, "the loading pump did not take the reset");
        runtime.update_pointer(
            static_cast<float>(runtime.match_layout_.width - 1),
            static_cast<float>(runtime.match_layout_.height - 1)
        );
        expect_composed(where);
        passed.emplace_back(where);
    }

    /// A lost device makes the renderer again at the next frame.
    void lost(uint32_t frame) {
        constexpr std::string_view where = "lost";
        start_match();
        for (uint32_t presented = 1; presented < frame; ++presented)
            runtime.render();
        const uint32_t rebuilds = run().rebuilds;
        post_render_event(SDL_EVENT_RENDER_DEVICE_LOST);
        expect(!run().pending_rebuild.empty(), where, "the lost device asked for no rebuild");
        expect(
            run().rebuilds == rebuilds, where, "the event dispatch made the renderer again itself"
        );
        expect_composed(where);
        expect(run().rebuilds == rebuilds + 1, where, "the renderer was not made again");
        passed.emplace_back(where);
    }

    /// A device that answers it is lost, as one does on some renderers
    /// while another program holds the screen, waits for its reset: its failures make no rebuild and nothing is
    /// read back; after the reset a failure makes the renderer again.
    void device_lost(uint32_t frame) {
        constexpr std::string_view where = "lost-wait";
        start_match();
        auto& faults = run().host->faults();
        render_probe::DeviceState answer = render_probe::DeviceState::lost;
        faults.context = &answer;
        faults.device_state = answered_state;
        const uint32_t rebuilds = run().rebuilds;
        arm(RenderFaultPoint::present, frame);
        present_until_fault(where);
        expect(run().device_lost, where, "the failure did not wait for the device");
        expect(
            run().rebuilds == rebuilds, where, "a lost device's failure made the renderer again"
        );
        renderer::Surface read{};
        runtime.capture_frame_ = &read;
        runtime.render();
        runtime.capture_frame_ = nullptr;
        expect(read.rgb.empty(), where, "a lost device's frame was read back");
        arm(RenderFaultPoint::present, 1);
        present_until_fault(where);
        expect(run().rebuilds == rebuilds && run().device_lost, where, "the wait did not hold");
        post_render_event(SDL_EVENT_RENDER_TARGETS_RESET);
        expect(!run().device_lost, where, "the reset did not end the wait");
        answer = render_probe::DeviceState::ok;
        arm(RenderFaultPoint::present, 1);
        present_until_fault(where);
        expect(run().rebuilds == rebuilds + 1, where, "a failure after the reset made no rebuild");
        faults.device_state = nullptr;
        faults.context = nullptr;
        expect_composed(where);
        passed.emplace_back(where);
    }

    /// Presents a forced stall on the next frame.
    ///
    /// @param where the case
    void present_stall(std::string_view where) {
        arm(RenderFaultPoint::stall, 1);
        present_until_fault(where);
    }

    /// A stall on a frame that is not steady is not counted.
    ///
    /// @param where the case
    /// @param when what keeps the frame from being steady
    void expect_stall_passed_over(std::string_view where, std::string_view when) {
        const uint32_t counted = run().stalls.stalls;
        present_stall(where);
        expect(run().stalls.stalls == counted, where, "a stall " + std::string(when) + " counted");
    }

    /// Presents over 2 s count on steady frames alone: not in a match's
    /// first 5 s, within 2 s of a resize, nor at the idle rate. Three on
    /// steady frames log once, and make no rebuild; a fourth logs nothing
    /// more.
    void stall(uint32_t frame) {
        constexpr std::string_view where = "stall";
        start_match();
        for (uint32_t presented = 1; presented < frame; ++presented)
            runtime.render();
        const uint32_t logs = run().stall_logs;
        const uint32_t rebuilds = run().rebuilds;
        // Each frame that is not steady differs in one thing alone from the
        // steady frames that follow.
        run().unsteady_until_ns = 0;
        run().screen_since_ns = frame_pacing::steady_now_ns();
        expect_stall_passed_over(where, "in a match's first 5 s");
        run().screen_since_ns = frame_pacing::steady_now_ns() - match_begun_ns;
        SDL_Event resized{};
        resized.type = SDL_EVENT_WINDOW_RESIZED;
        resized.window.windowID = SDL_GetWindowID(runtime.sdl_.window);
        expect(!runtime.take_render_event(resized), where, "a resize was kept from the screens");
        expect_stall_passed_over(where, "just after a resize");
        run().unsteady_until_ns = 0;
        const auto wait = runtime.frame_wait_;
        runtime.frame_wait_ = frame_pacing::FrameWait::idle;
        expect_stall_passed_over(where, "at the idle rate");
        runtime.frame_wait_ = wait;
        expect(run().stall_logs == logs, where, "a stall that did not count was logged");
        for (uint32_t stall = 1; stall < stalls_logged_at; ++stall) {
            present_stall(where);
            expect(run().stall_logs == logs, where, "a stall was logged before the third");
        }
        present_stall(where);
        expect(run().stall_logs == logs + 1, where, "three stalls were not logged once");
        present_stall(where);
        expect(run().stall_logs == logs + 1, where, "a fourth stall was logged again");
        expect(run().rebuilds == rebuilds, where, "a stall made the renderer again");
        passed.emplace_back(where);
    }

    /// A floating-point setting changed before a present is put back after
    /// it, and reported once.
    void float_state(uint32_t frame) {
        constexpr std::string_view where = "float";
        start_match();
        auto& guard = oa::base::float_precision::program_float_control();
        const auto saved_hooks = guard.hooks;
        const bool saved_reported = guard.reported;
        uint32_t reports = 0;
        guard.hooks = {&reports, count_float_change};
        guard.reported = false;
        arm(RenderFaultPoint::float_state, frame);
        present_until_fault(where);
        const bool restored = std::fegetround() == FE_TONEAREST;
        const uint32_t first_reports = reports;
        arm(RenderFaultPoint::float_state, 1);
        present_until_fault(where);
        const bool restored_again = std::fegetround() == FE_TONEAREST;
        guard.hooks = saved_hooks;
        guard.reported = saved_reported;
        expect(restored && restored_again, where, "the rounding mode was not put back");
        expect(first_reports == 1 && reports == 1, where, "the change was not reported once");
        passed.emplace_back(where);
    }

    /// A window whose layers pass a texture limit of 2048: the world, the
    /// dialog layer and the OA settings layer go in tiles, no front-end
    /// texture is made, and each frame is the composed one.
    void tiles() {
        constexpr std::string_view where = "tiles";
        auto& faults = run().host->faults();
        faults.texture_limit = tiles_texture_limit;
        to_main_menu();
        expect(
            SDL_SetWindowSize(runtime.sdl_.window, tiles_window_width, tiles_window_height) &&
                SDL_SyncWindow(runtime.sdl_.window),
            where,
            std::string("SDL_SetWindowSize: ") + SDL_GetError()
        );
        start_match();
        expect_composed(where);
        expect(runtime.match_world_tex_.tile_count() > 1, where, "the world was not tiled");
        expect(
            runtime.output_texture_w_ == 0 && runtime.output_texture_h_ == 0 &&
                runtime.frontend_texture_.tile_count() == 0,
            where,
            "a front-end texture was made past the limit"
        );
        runtime.show_match_pause_menu();
        runtime.activate_pause_gadget("HELP");
        expect(
            oa::ui::frontend_dialogs::dialog_kind() == oa::ui::frontend_dialogs::DialogKind::help,
            where,
            "HELP did not open HELP.GUI"
        );
        expect_composed("tiles-help");
        expect(runtime.match_dialog_tex_.tile_count() > 1, where, "the dialog layer was not tiled");
        oa::ui::frontend_dialogs::close_dialog();
        runtime.open_engine_settings_in_match();
        expect(runtime.engine_settings_dialog() != nullptr, where, "the OA settings did not open");
        expect_composed("tiles-settings");
        expect(
            runtime.engine_settings_match_host().layer.tile_count() > 1,
            where,
            "the OA settings layer was not tiled"
        );
        faults.texture_limit = 0;
        passed.emplace_back(where);
    }

    /// Returns the accelerated tier's watch, which must exist.
    ///
    /// @param where the case
    /// @return the watch
    AcceleratedWatch& watch(std::string_view where) {
        expect(run().watch != nullptr, where, "the accelerated tier has no watch");
        return *run().watch;
    }

    /// Returns the rung the cases switch the accelerated tier on at: the
    /// full scene budget with the area pass, magnify on, the chrome filtered
    /// and the card's magnification by sharp-bilinear within the whole
    /// prescale budget, so that every rung below it is there to take.
    static policy::LadderState top_rung() {
        policy::LadderState rung;
        rung.method = policy::ZoomOutMethod::area;
        rung.budget = policy::SceneBudget::full;
        rung.blend_allowed = false;
        rung.magnify = true;
        rung.filtered_chrome = true;
        rung.card = policy::CardFilter::prescale_full;
        rung.standard = false;
        return rung;
    }

    /// Switches the accelerated tier on at a rung, as
    /// --hardware-acceleration=basic and --force-capable would, or back to
    /// what the run's own options say.
    ///
    /// @param where the case
    /// @param on true to switch it on
    void accelerate(std::string_view where, bool on) {
        if (on) {
            run().rung = top_rung();
            runtime.options_.hardware_acceleration =
                oa::ui::engine_settings::HardwareAcceleration::basic;
            runtime.options_.force_capable = true;
        } else {
            run().rung.reset();
            runtime.options_.hardware_acceleration.reset();
            runtime.options_.force_capable = false;
        }
        runtime.update_render_tier();
        if (on)
            expect(runtime.accelerated_presentation(), where, "the accelerated tier did not draw");
    }

    /// Makes the frames that follow steady: past a match's first 5 s and any
    /// window change, at the full rate, with the match clock at its rate.
    void steady_frames() {
        run().unsteady_until_ns = 0;
        run().screen_since_ns = frame_pacing::steady_now_ns() - match_begun_ns;
        runtime.frame_wait_ = frame_pacing::FrameWait::precise;
        runtime.match_timing_.actual_rate = runtime.match_timing_.requested_rate;
    }

    /// Sizes the window and lays the screen out for it.
    ///
    /// @param width the window's width in pixels
    /// @param height its height in pixels
    void resize(int width, int height) {
        expect(
            SDL_SetWindowSize(runtime.sdl_.window, width, height) &&
                SDL_SyncWindow(runtime.sdl_.window),
            "resize",
            std::string("SDL_SetWindowSize: ") + SDL_GetError()
        );
        runtime.apply_output_mode();
    }

    /// Sets the battlefield's zoom.
    ///
    /// @param zoom screen pixels per map pixel
    void at_zoom(float zoom) { runtime.match_zoom_ = runtime.match_zoom_target_ = zoom; }

    /// Presents one frame at part_zoom and tells whether the card drew the
    /// magnified scene through its prescale target.
    ///
    /// @param where the case
    /// @return true when the scene went through the prescale target
    bool magnify_once(std::string_view where) {
        at_zoom(part_zoom);
        const uint64_t draws = runtime.accelerated_.counts.prescale_draws;
        runtime.render();
        expect(runtime.accelerated_.magnified, where, "zoom 1.5 was not magnified");
        return runtime.accelerated_.counts.prescale_draws != draws;
    }

    /// Tells whether the scene a frame drew apart and the terrain at its
    /// size hold no memory.
    ///
    /// @return true when both are freed
    bool scene_buffers_freed() const {
        return runtime.match_scene_cpu_.rgb.capacity() == 0 &&
               runtime.match_terrain_cache_.rgb.capacity() == 0;
    }

    /// The memory guard, with the system's memory forced: free memory just
    /// over its threshold refuses every new buffer of the accelerated tier,
    /// which stays on the rungs below them, magnify off and plain LINEAR,
    /// and draws on, its status not saying frames were slow; committed
    /// memory past its threshold then drops the tier for the rest of the
    /// run, freeing what it made, the zoomed-out scene and its terrain
    /// among them, with the status saying so, and Off then On does not lift
    /// it.
    ///
    /// @param frame the presented frame of the match the case begins at
    /// @return 0, or skipped_exit_code under 2 GiB of memory
    int memory(uint32_t frame) {
        constexpr std::string_view where = "memory";
        if (run().host->tier_inputs().memory < policy::smallest_accelerated_memory) {
            std::cout << "renderer ladder check: memory: skipped: the machine reports less than "
                         "the 2 GiB threshold of memory\n";
            return skipped_exit_code;
        }
        to_main_menu();
        resize(part_scale_width, part_scale_height);
        start_match();
        for (uint32_t presented = 1; presented < frame; ++presented)
            runtime.render();
        accelerate(where, true);
        auto& watched = watch(where);
        const policy::MemoryGuardThresholds thresholds = watched.memory.thresholds;
        expect(thresholds.physical != 0, where, "the memory guard knows no physical memory");
        oa::platform::SystemMemorySample sample{};
        sample.physical = thresholds.physical;
        sample.available = thresholds.free_floor + 1;
        sample.available_known = true;
        sample.committed = 0;
        sample.committed_known = true;
        run().forced_memory = sample;
        at_zoom(zoomed_in_zoom);
        for (uint32_t presented = 0; presented < refused_buffer_frames; ++presented)
            runtime.render();
        const auto& state = runtime.accelerated_;
        expect(
            runtime.accelerated_presentation(), where, "free memory not yet low dropped the tier"
        );
        expect(
            !state.rung.magnify && state.rung.card == policy::CardFilter::linear && watched.moved &&
                watched.rung == state.rung,
            where,
            "the tier did not stay on the rungs below the buffers refused"
        );
        expect(
            !state.scene.made() && !state.overlay_texture.made() && !state.hud_prescale.made() &&
                !state.world_prescale.made(),
            where,
            "a buffer the memory guard refused was made"
        );
        expect(
            runtime.acceleration_report().status.state ==
                oa::ui::engine_settings::AccelerationState::in_use,
            where,
            "a buffer the memory guard refused was taken for slow frames"
        );
        // Zoomed out, the area pass holds a scene larger than the
        // battlefield, and the terrain at its size.
        at_zoom(zoomed_out_zoom);
        runtime.render();
        const std::size_t scene_capacity = runtime.match_terrain_cache_.rgb.capacity();
        expect(
            runtime.accelerated_.frame.method == SceneMethod::area &&
                !runtime.match_scene_cpu_.rgb.empty() &&
                scene_capacity > runtime.match_world_cpu_.rgb.size(),
            where,
            "zoom 0.5 did not draw a scene larger than the battlefield"
        );
        const uint32_t steps = watched.steps;
        // Committed memory past its threshold, at the next sample.
        sample.committed = thresholds.committed_limit + 1;
        run().forced_memory = sample;
        policy::resume_memory_guard(watched.memory);
        runtime.render();
        const auto& inputs = run().host->tier_inputs();
        expect(
            !runtime.accelerated_presentation() && inputs.drop == policy::Drop::memory &&
                watched.memory.tripped == policy::MemoryGuardCause::committed,
            where,
            "committed memory past its threshold did not drop the tier"
        );
        expect(
            state.base.empty() && state.overlay.empty() && !state.screen_prescale.made(),
            where,
            "a buffer of the accelerated tier outlived the drop"
        );
        // The standard tier's frame after the drop draws the terrain at the
        // battlefield's size, in a buffer of that size.
        expect(
            runtime.match_scene_cpu_.rgb.empty() && runtime.match_scene_cpu_.rgb.capacity() == 0 &&
                runtime.match_terrain_cache_.rgb.capacity() < scene_capacity,
            where,
            "the zoomed-out scene's buffers outlived the drop"
        );
        expect(
            runtime.acceleration_report().status.state ==
                oa::ui::engine_settings::AccelerationState::too_little_memory,
            where,
            "the status does not say there is too little memory"
        );
        runtime.forget_render_failures();
        runtime.update_render_tier();
        expect(
            !runtime.accelerated_presentation() && inputs.drop == policy::Drop::memory,
            where,
            "Off then On lifted the memory guard's drop"
        );
        expect(watched.steps == steps, where, "the drop was taken as a step");
        // The standard tier presents the composed frame, at the window the
        // other cases draw, where SDL's NEAREST lays the chrome out as the
        // composition does.
        resize(kDefaultWindowWidth, kDefaultWindowHeight);
        expect_composed(where);
        run().forced_memory.reset();
        accelerate(where, false);
        passed.emplace_back(where);
        return 0;
    }

    // -----------------------------------------------------------------------
    // The renderer records across simulated restarts

    /// Where the records' cases keep their folders.
    fs::path records_root{};
    /// The check's clock, which the stages of the sentinel are timed by.
    uint64_t clock_ns{};
    /// The first hardware driver of SDL's order, which the records' cases
    /// name the renderer by where a case needs a driver that can be
    /// recorded failed-driver; empty when SDL has none.
    std::string hardware_driver{};
    /// The refusals of SDL's software renderer the advice case has left.
    uint32_t software_refusals{};

    /// Returns the renderer's host.
    RendererHost& host() { return *run().host; }

    /// Returns a driver's strike and records.
    ///
    /// @param driver the driver
    /// @return its entry, or null when it has none
    const rs::DriverRecords* entry(std::string_view driver) {
        return rs::find_driver(host().records().records(), driver);
    }

    /// Makes a case's folder of records.
    ///
    /// @param name the case
    /// @return the folder
    fs::path records_folder(std::string_view name) {
        const fs::path folder = records_root / name;
        fs::create_directories(folder);
        return folder;
    }

    /// Reads the records file of a folder as a start would find it.
    ///
    /// @param folder the folder
    /// @return its keys and values
    static rs::Values records_file(const fs::path& folder) {
        return preferences::load(folder / rs::records_file_name);
    }

    /// Takes the overrides the records' cases run with: 8 GiB of memory,
    /// two left-over trials in a row before a record, the renderer named by
    /// its own name and no driver refused.
    void records_defaults() {
        auto& faults = host().faults();
        faults.physical_memory = records_memory;
        faults.crash_evidence = rs::CrashEvidence::two_in_a_row;
        faults.record_driver.clear();
        faults.adapter.clear();
        faults.refuse_driver = nullptr;
        faults.context = nullptr;
        run().rung.reset();
    }

    /// Starts the game again, as its next start would: the run before ends
    /// cleanly (RendererHost::finish_records) or as a crash leaves its files,
    /// then the start reads the records in a folder, walks the drivers and
    /// decides its first frame's tier with the setting at Basic on the
    /// player's own profile, under --force-capable.
    ///
    /// @param folder the folder of the records; empty keeps them in memory,
    ///     as with a named preferences file, with the setting at that file's
    ///     default
    /// @param clean_end the run before ended cleanly
    void start_on(const fs::path& folder, bool clean_end) {
        auto& renderer = host();
        if (clean_end)
            renderer.finish_records();
        to_main_menu();
        runtime.forget_render_textures();
        runtime.switch_accelerated_presentation(false, runtime.accelerated_.rung);
        runtime.sdl_.renderer = nullptr;
        RecordsPlace place;
        place.folder = folder;
        place.engine_build = renderer.records_place().engine_build;
        renderer.create(runtime.sdl_.window, renderer.faults(), place);
        runtime.sdl_.renderer = renderer.renderer();
        runtime.take_renderer_names(
            renderer.facts().renderer, stats_adapter_name(renderer.facts())
        );
        TierRequest request;
        request.force_capable = !folder.empty();
        request.players_own_profile = !folder.empty();
        request.setting = folder.empty() ? oa::ui::engine_settings::HardwareAcceleration::off
                                         : oa::ui::engine_settings::HardwareAcceleration::basic;
        renderer.decide_start_tier(request);
        auto& state = run();
        state.pending_rebuild.clear();
        state.pending_failure = {};
        state.device_lost = false;
        state.resets = {};
        state.notices_noted.clear();
        state.main_menu_frames = 0;
        state.presented_since_rebuild = 0;
        runtime.apply_output_mode();
    }

    /// Draws main menu frames as the game's loop does, the main menu's
    /// notice first.
    ///
    /// @param frames the frames
    void menu_frames(uint32_t frames) {
        for (uint32_t frame = 0; frame < frames; ++frame) {
            runtime.tell_renderer_records();
            runtime.render();
        }
    }

    /// Passes the start-up stage: 2 s on the check's clock and 60 frames.
    ///
    /// @param where the case
    void pass_start(std::string_view where) {
        clock_ns += rs::start_stage_ns;
        menu_frames(rs::start_stage_frames);
        const auto& sentinel = host().records().sentinel();
        expect(
            sentinel && sentinel->stage == rs::SentinelStage::running,
            where,
            "the start-up stage did not pass after 60 frames and 2 s"
        );
        expect(!host().records().records().trial, where, "the trial stood after the stage passed");
    }

    /// Leaves the files a crash in a start-up stage leaves: a sentinel, and
    /// no trial, as before the function test.
    ///
    /// @param folder the folder of the records
    /// @param sentinel the sentinel's value
    static void leave_over_sentinel(const fs::path& folder, std::string_view sentinel) {
        rs::Values values = records_file(folder);
        values.erase(std::string(rs::trial_key));
        preferences::save(folder / rs::records_file_name, values);
        preferences::save(
            folder / rs::sentinel_file_name,
            rs::Values{{std::string(rs::sentinel_key), std::string(sentinel)}}
        );
    }

    /// Returns the tier the last frame was drawn in.
    render_policy::TierDecision tier() { return run().tier; }

    /// Returns the status the settings dialog shows.
    oa::ui::engine_settings::AccelerationState status() {
        return runtime.acceleration_report().status.state;
    }

    /// Checks the notices the main menu noted in this start.
    ///
    /// @param where the case
    /// @param kinds the notices' kinds, in order
    void expect_notices(std::string_view where, std::initializer_list<rs::NoticeKind> kinds) {
        const auto& noted = run().notices_noted;
        bool same = noted.size() == kinds.size();
        std::size_t index = 0;
        for (const rs::NoticeKind kind : kinds)
            same = same && noted[index++].kind == kind;
        expect(same, where, "the main menu did not note one notice for each new record");
    }

    /// A left-over trial is a strike on the first restart, which runs as if
    /// nothing were recorded, also with the sentinel's file garbled or lost
    /// as a system crash leaves it; the second in a row records
    /// accelerated-unusable, which keeps the start standard and is told
    /// once. The start tests and accelerates by itself; the front end's
    /// prescale target, first used under the start-up sentinel, writes no
    /// sentinel or trial of its own.
    void left_over_trial() {
        constexpr std::string_view where = "records-trial";
        records_defaults();
        const fs::path folder = records_folder("trial");
        start_on(folder, true);
        menu_frames(1);
        expect(
            render_policy::card_tier(tier().tier),
            where,
            "the start did not run the function test and accelerate by itself"
        );
        const auto& trial = host().records().records().trial;
        expect(
            trial && trial->stage == rs::StrikeStage::probe &&
                trial->driver == render_probe::software_renderer,
            where,
            "no trial probe software stood through the first accelerated frames"
        );
        expect(
            records_file(folder).count(std::string(rs::trial_key)) == 1,
            where,
            "the trial was not written before the function test"
        );
        const auto& sentinel = host().records().sentinel();
        expect(
            sentinel && sentinel->stage == rs::SentinelStage::accelerated,
            where,
            "the first accelerated frame did not move the sentinel"
        );
        expect(
            runtime.accelerated_.screen_prescale.made(),
            where,
            "the front end was not scaled through a prescale target"
        );
        // A crash in the first accelerated frames, the sentinel's file
        // garbled.
        write_text(folder / rs::sentinel_file_name, "garbled");
        start_on(folder, false);
        const auto* struck = entry(render_probe::software_renderer);
        expect(
            host().leftovers().unclean_exit && struck != nullptr &&
                struck->strike.stage == rs::StrikeStage::probe &&
                struck->accelerated_unusable.failure == rs::RecordedFailure::none,
            where,
            "the first left-over trial was not a strike alone"
        );
        menu_frames(1);
        expect(
            render_policy::card_tier(tier().tier),
            where,
            "the start after a strike did not run as if nothing were recorded"
        );
        // Again, the sentinel's file lost with the power.
        fs::remove(folder / rs::sentinel_file_name);
        start_on(folder, false);
        const auto* recorded = entry(render_probe::software_renderer);
        expect(
            recorded != nullptr &&
                recorded->accelerated_unusable.failure == rs::RecordedFailure::stopped &&
                host().leftovers().change.new_record,
            where,
            "the second left-over trial in a row was not recorded accelerated-unusable"
        );
        menu_frames(3);
        expect(
            tier().reason == render_policy::TierReason::accelerated_unusable &&
                !host().records().records().trial,
            where,
            "the recorded driver did not stay standard with no trial"
        );
        expect(
            status() == oa::ui::engine_settings::AccelerationState::game_stopped,
            where,
            "the status did not say the game stopped while using it"
        );
        expect_notices(where, {rs::NoticeKind::accelerated_unusable});
        expect(
            entry(render_probe::software_renderer)->accelerated_unusable.told,
            where,
            "the notice did not mark the record told"
        );
        // The next start honours the record and tells nothing.
        start_on(folder, true);
        menu_frames(3);
        expect_notices(where, {});
        expect(
            tier().reason == render_policy::TierReason::accelerated_unusable,
            where,
            "the next start did not honour the record"
        );
        passed.emplace_back(where);
    }

    /// Where a fault in a trial's stage can stop the whole system, as on
    /// Windows before Vista and on Linux, the first left-over trial is
    /// already a record.
    void first_trial_counts() {
        constexpr std::string_view where = "records-first-trial";
        records_defaults();
        host().faults().crash_evidence = rs::CrashEvidence::first_counts;
        const fs::path folder = records_folder("first-trial");
        start_on(folder, true);
        menu_frames(1);
        start_on(folder, false);
        const auto* recorded = entry(render_probe::software_renderer);
        expect(
            recorded != nullptr &&
                recorded->accelerated_unusable.failure == rs::RecordedFailure::stopped,
            where,
            "the first left-over trial was not recorded"
        );
        menu_frames(1);
        expect(
            tier().reason == render_policy::TierReason::accelerated_unusable,
            where,
            "the start after it did not stay standard"
        );
        passed.emplace_back(where);
    }

    /// A start that passes its start-up stage clears the strike of a crash
    /// before it; a run that ends after its start-up stage, without a clean
    /// exit, is only logged at the next start.
    void clean_pass() {
        constexpr std::string_view where = "records-clean-pass";
        records_defaults();
        const fs::path folder = records_folder("clean-pass");
        start_on(folder, true);
        menu_frames(1);
        start_on(folder, false);
        const auto* struck = entry(render_probe::software_renderer);
        expect(
            struck != nullptr && struck->strike.stage == rs::StrikeStage::probe,
            where,
            "the left-over trial was not struck"
        );
        menu_frames(1);
        pass_start(where);
        struck = entry(render_probe::software_renderer);
        expect(
            struck == nullptr || struck->strike.stage == rs::StrikeStage::none,
            where,
            "passing the stage did not clear the strike"
        );
        expect(
            records_file(folder).count(std::string(rs::trial_key)) == 0,
            where,
            "the trial was not erased from the file"
        );
        // A crash once the start-up stage passed.
        start_on(folder, false);
        const auto& leftovers = host().leftovers();
        const auto log = host().records_log();
        expect(
            leftovers.unclean_exit && leftovers.driver.empty() && !leftovers.change.changed,
            where,
            "a left-over running marker was struck or recorded"
        );
        expect(
            std::count_if(
                log.begin(),
                log.end(),
                [](const std::string& line) {
                    return line.find("without a clean exit (running") != std::string::npos;
                }
            ) == 1,
            where,
            "the left-over running marker was not logged once"
        );
        passed.emplace_back(where);
    }

    /// A left-over create or standard sentinel strikes its driver, and the
    /// same at the next start records failed-driver, which the walk then
    /// skips and the main menu tells once; a clean pass between them clears
    /// the strike, and SDL's software renderer is never recorded so.
    void left_over_sentinel() {
        constexpr std::string_view where = "records-sentinel";
        records_defaults();
        const fs::path software_folder = records_folder("software-sentinel");
        start_on(software_folder, true);
        menu_frames(1);
        for (int start = 0; start < 2; ++start) {
            leave_over_sentinel(software_folder, "standard software");
            start_on(software_folder, false);
        }
        const auto* software = entry(render_probe::software_renderer);
        expect(
            software == nullptr || software->failed_driver.failure == rs::RecordedFailure::none,
            where,
            "software was recorded failed-driver"
        );
        host().faults().record_driver = hardware_driver;
        const fs::path folder = records_folder("sentinel");
        start_on(folder, true);
        menu_frames(1);
        leave_over_sentinel(folder, "standard " + hardware_driver);
        start_on(folder, false);
        expect(
            entry(hardware_driver) != nullptr &&
                entry(hardware_driver)->strike.stage == rs::StrikeStage::standard,
            where,
            "a left-over standard sentinel was not struck"
        );
        menu_frames(1);
        pass_start(where);
        expect(
            entry(hardware_driver)->strike.stage == rs::StrikeStage::none,
            where,
            "a clean pass did not clear the strike"
        );
        for (int start = 0; start < 2; ++start) {
            leave_over_sentinel(folder, "create " + hardware_driver);
            start_on(folder, false);
        }
        expect(
            entry(hardware_driver)->failed_driver.failure == rs::RecordedFailure::stopped,
            where,
            "the same create sentinel twice in a row was not recorded failed-driver"
        );
        const auto attempts = host().attempts();
        expect(
            std::none_of(
                attempts.begin(),
                attempts.end(),
                [&](const CreationAttempt& attempt) { return attempt.driver == hardware_driver; }
            ) && host().skipped_drivers().size() == 1 &&
                host().skipped_drivers().front() == hardware_driver,
            where,
            "the walk did not skip the recorded driver"
        );
        menu_frames(3);
        expect_notices(where, {rs::NoticeKind::failed_driver});
        expect(
            status() == oa::ui::engine_settings::AccelerationState::in_use_on_another_driver,
            where,
            "the status did not say another driver is in use"
        );
        passed.emplace_back(where);
    }

    /// Records that would leave nothing able to present are ignored for the
    /// run: the walk starts again from the top with them ignored.
    void advice() {
        constexpr std::string_view where = "records-advice";
        records_defaults();
        host().faults().record_driver = hardware_driver;
        const fs::path folder = records_folder("advice");
        start_on(folder, true);
        menu_frames(1);
        for (int start = 0; start < 2; ++start) {
            leave_over_sentinel(folder, "create " + hardware_driver);
            start_on(folder, false);
        }
        expect(
            rs::skips_driver(host().records().records(), hardware_driver),
            where,
            "the driver was not recorded failed-driver"
        );
        // SDL's software renderer refuses once: the walk that skips the
        // recorded driver ends with nothing able to present.
        software_refusals = 1;
        auto& faults = host().faults();
        faults.context = &software_refusals;
        faults.refuse_driver = refuse_software;
        start_on(folder, true);
        faults.refuse_driver = nullptr;
        faults.context = nullptr;
        const auto attempts = host().attempts();
        expect(
            host().records_ignored() && host().renderer() != nullptr &&
                std::any_of(
                    attempts.begin(),
                    attempts.end(),
                    [&](const CreationAttempt& attempt) {
                        return attempt.driver == hardware_driver;
                    }
                ),
            where,
            "the walk did not start again with the records ignored"
        );
        passed.emplace_back(where);
    }

    /// A trial that cannot be written skips the function test and keeps the
    /// start standard, with the status saying the game cannot save its
    /// files and nothing recorded; switching Off then On tries the write
    /// again.
    void unwritable_trial() {
        constexpr std::string_view where = "records-unwritable";
        records_defaults();
        // A file stands where the profile's folder should be.
        const fs::path blocker = records_folder("unwritable") / "blocker";
        write_text(blocker, "a file, not a folder");
        start_on(blocker / "profile", true);
        menu_frames(1);
        auto& inputs = host().tier_inputs();
        expect(
            inputs.function_test == render_policy::FunctionTest::trial_unwritten &&
                tier().reason == render_policy::TierReason::trial_unwritten,
            where,
            "the function test ran with no trial written"
        );
        expect(
            status() == oa::ui::engine_settings::AccelerationState::cannot_save &&
                !runtime.acceleration_report().acceleration_unavailable,
            where,
            "the status did not say the game cannot save its files, with the row free"
        );
        expect(
            !host().records().records().trial && host().records().records().drivers.empty(),
            where,
            "a trial or a record was kept"
        );
        const auto log = host().records_log();
        expect(
            std::any_of(
                log.begin(),
                log.end(),
                [](const std::string& line) {
                    return line.find("cannot write") != std::string::npos;
                }
            ),
            where,
            "the failed writes were not logged"
        );
        // Off then On tries the trial again, which fails again.
        auto& dialog = runtime.open_engine_settings_dialog();
        ++dialog.forget_renderer_failures;
        std::ignore =
            runtime.take_engine_settings_action(oa::ui::engine_settings::DialogAction::changed);
        expect(
            inputs.function_test == render_policy::FunctionTest::not_run,
            where,
            "Off then On did not let the test try again"
        );
        menu_frames(1);
        expect(
            inputs.function_test == render_policy::FunctionTest::trial_unwritten,
            where,
            "the retry did not try the trial again"
        );
        std::ignore =
            runtime.take_engine_settings_action(oa::ui::engine_settings::DialogAction::accepted);
        passed.emplace_back(where);
    }

    /// The first use of an accelerated path after the start-up stage writes
    /// its trial and sentinel, which its first frames pass; a crash in them
    /// is struck, and the second in a row records accelerated-unusable; a
    /// path whose trial cannot be written drops the tier and records
    /// nothing.
    void paths() {
        constexpr std::string_view where = "records-paths";
        records_defaults();
        const auto magnify = rs::AcceleratedPath::magnify;
        const fs::path folder = records_folder("paths");
        start_on(folder, true);
        menu_frames(1);
        pass_start(where);
        expect(host().begin_path(magnify), where, "the path's trial could not be written");
        const auto& sentinel = host().records().sentinel();
        const auto& trial = host().records().records().trial;
        expect(
            sentinel && sentinel->stage == rs::SentinelStage::path && sentinel->path == magnify &&
                trial && trial->stage == rs::StrikeStage::path && trial->path == magnify,
            where,
            "the path's first use did not write its trial and sentinel"
        );
        for (uint32_t frame = 0; frame < rs::path_stage_frames; ++frame)
            host().note_presented_frame(path_bit(magnify));
        expect(
            host().records().sentinel()->stage == rs::SentinelStage::running &&
                !host().records().records().trial,
            where,
            "the path's first frames did not pass"
        );
        expect(host().begin_path(magnify), where, "a later use of the path was refused");
        expect(
            host().records().sentinel()->stage == rs::SentinelStage::running,
            where,
            "a later use of the path wrote its sentinel again"
        );
        for (int start = 0; start < 2; ++start) {
            start_on(folder, start == 0);
            menu_frames(1);
            pass_start(where);
            expect(host().begin_path(magnify), where, "the path's trial could not be written");
        }
        start_on(folder, false);
        const auto* recorded = entry(render_probe::software_renderer);
        expect(
            recorded != nullptr &&
                recorded->accelerated_unusable.failure == rs::RecordedFailure::stopped,
            where,
            "two left-over path trials in a row were not recorded"
        );
        // A path whose trial cannot be written: a folder stands where the
        // records file should be.
        const fs::path unwritable = records_folder("path-unwritable");
        start_on(unwritable, true);
        menu_frames(1);
        pass_start(where);
        fs::remove(unwritable / rs::records_file_name);
        fs::create_directories(unwritable / rs::records_file_name / "blocked");
        bool dropped = false;
        try {
            runtime.begin_accelerated_path(magnify);
        } catch (const AccelerationError& error) {
            runtime.take_acceleration_error(error);
            dropped = true;
        }
        expect(
            dropped && host().tier_inputs().drop == render_policy::Drop::path_trial_unwritten &&
                host().records().records().drivers.empty(),
            where,
            "a path whose trial could not be written did not drop the tier with nothing struck"
        );
        menu_frames(1);
        expect(
            status() == oa::ui::engine_settings::AccelerationState::cannot_save,
            where,
            "the status did not say the game cannot save its files"
        );
        fs::remove_all(unwritable / rs::records_file_name);
        passed.emplace_back(where);
    }

    /// Returns a rung with magnify on and the card's sharp-bilinear
    /// magnification through prescale targets, as a machine of a tested
    /// class starts at.
    static render_policy::LadderState magnify_rung() {
        render_policy::LadderState rung{};
        rung.method = render_policy::ZoomOutMethod::area;
        rung.budget = render_policy::SceneBudget::full;
        rung.magnify = true;
        rung.filtered_chrome = true;
        rung.card = render_policy::CardFilter::prescale_full;
        rung.standard = false;
        return rung;
    }

    /// Checks that the sentinel stands at `running` with no trial.
    ///
    /// @param where the case
    /// @param what what was expected
    void expect_running(std::string_view where, std::string_view what) {
        const auto& sentinel = host().records().sentinel();
        expect(
            sentinel && sentinel->stage == rs::SentinelStage::running &&
                !host().records().records().trial,
            where,
            what
        );
    }

    /// Draws match frames at a zoom.
    ///
    /// @param zoom the battlefield's zoom
    /// @param frames the frames
    void match_frames(float zoom, uint32_t frames) {
        runtime.match_zoom_ = runtime.match_zoom_target_ = zoom;
        for (uint32_t frame = 0; frame < frames; ++frame)
            runtime.render();
    }

    /// The magnified world's path begins at the first frame drawn through
    /// it: a match on a magnify rung played at zoom 1, however long, leaves
    /// the sentinel at `running` with no trial and makes none of its
    /// textures; the first zoomed-in frame writes the path's trial and
    /// sentinel, which its first 60 magnified frames pass. Switching the
    /// tier off while a path's first frames stand closes their stage.
    void path_window() {
        constexpr std::string_view where = "records-path-window";
        records_defaults();
        run().rung = magnify_rung();
        const fs::path folder = records_folder("path-window");
        start_on(folder, true);
        menu_frames(1);
        pass_start(where);
        start_match();
        match_frames(1.0F, rs::path_stage_frames * 2);
        expect(
            render_policy::card_tier(tier().tier) && runtime.accelerated_.rung.magnify,
            where,
            "the match was not drawn in the accelerated tier on a magnify rung"
        );
        expect_running(where, "a match at zoom 1 left a path's sentinel or trial standing");
        expect(
            !runtime.accelerated_.scene.made() && !runtime.accelerated_.overlay_texture.made(),
            where,
            "the magnified world's textures were made at zoom 1"
        );
        match_frames(path_zoom, 1);
        const auto& sentinel = host().records().sentinel();
        const auto& trial = host().records().records().trial;
        expect(
            runtime.accelerated_.magnified && sentinel &&
                sentinel->stage == rs::SentinelStage::path &&
                sentinel->path == rs::AcceleratedPath::magnify && trial &&
                trial->stage == rs::StrikeStage::path,
            where,
            "the first zoomed-in frame did not write the magnified world's trial and sentinel"
        );
        expect(
            records_file(folder).count(std::string(rs::trial_key)) == 1,
            where,
            "the path's trial was not written before its first frame"
        );
        match_frames(path_zoom, rs::path_stage_frames - 1);
        expect_running(where, "the path's first 60 magnified frames did not pass its stage");
        // Switched off while the path's first frames stand.
        start_on(folder, true);
        menu_frames(1);
        pass_start(where);
        start_match();
        match_frames(path_zoom, 1);
        expect(
            host().records().sentinel()->stage == rs::SentinelStage::path,
            where,
            "the path's first frame wrote no sentinel"
        );
        auto& settings = runtime.engine_settings_state().current;
        settings.hardware_acceleration = oa::ui::engine_settings::HardwareAcceleration::off;
        match_frames(path_zoom, 1);
        settings.hardware_acceleration = oa::ui::engine_settings::HardwareAcceleration::basic;
        expect(!runtime.accelerated_.on, where, "the setting set to Off left the tier on");
        expect_running(where, "switching the tier off left the path's sentinel or trial standing");
        expect(
            records_file(folder).count(std::string(rs::trial_key)) == 0,
            where,
            "switching the tier off left the path's trial in the file"
        );
        to_main_menu();
        run().rung.reset();
        passed.emplace_back(where);
    }

    /// Each driver describes the adapter in words of its own, so only a
    /// start whose walk no record steered notes it: a start that skips the
    /// recorded driver, and a rebuild, keep the records whatever the driver
    /// they land on calls the adapter; a start that skips nothing and reads
    /// another adapter clears them.
    void adapter() {
        constexpr std::string_view where = "records-adapter";
        records_defaults();
        auto& faults = host().faults();
        faults.record_driver = hardware_driver;
        faults.adapter = case_adapter;
        const fs::path folder = records_folder("adapter");
        start_on(folder, true);
        menu_frames(1);
        expect(
            host().records().records().adapter == case_adapter,
            where,
            "the start did not note the adapter"
        );
        for (int start = 0; start < 2; ++start) {
            leave_over_sentinel(folder, "create " + hardware_driver);
            start_on(folder, false);
        }
        expect(
            rs::skips_driver(host().records().records(), hardware_driver),
            where,
            "the driver was not recorded failed-driver"
        );
        // The driver the skip lands on words the adapter its own way.
        faults.adapter = case_adapter_other_words;
        start_on(folder, true);
        const std::string record_key = std::string(rs::failed_driver_prefix) + hardware_driver;
        expect(
            host().skipped_drivers().size() == 1 &&
                rs::skips_driver(host().records().records(), hardware_driver) &&
                host().records().records().adapter == case_adapter &&
                records_file(folder).count(record_key) == 1,
            where,
            "a start that skipped a recorded driver took its driver's words for another adapter"
        );
        // A rebuild's driver too, after a lost device it strikes.
        menu_frames(2);
        post_render_event(SDL_EVENT_RENDER_DEVICE_LOST);
        menu_frames(1);
        const auto* kept = entry(hardware_driver);
        expect(
            kept != nullptr && kept->failed_driver.failure == rs::RecordedFailure::stopped &&
                kept->strike.stage == rs::StrikeStage::lost &&
                host().records().records().adapter == case_adapter,
            where,
            "a rebuild took its driver's words for another adapter and cleared the records"
        );
        // A start that skips nothing and reads another adapter clears them.
        const fs::path other = records_folder("adapter-other");
        faults.adapter = case_adapter;
        start_on(other, true);
        menu_frames(1);
        leave_over_sentinel(other, "standard " + hardware_driver);
        faults.adapter = other_adapter;
        start_on(other, false);
        const auto* cleared = entry(hardware_driver);
        expect(
            (cleared == nullptr || cleared->strike.stage == rs::StrikeStage::none) &&
                host().records().records().adapter == other_adapter,
            where,
            "another adapter did not clear the records"
        );
        faults.adapter.clear();
        passed.emplace_back(where);
    }

    /// After a start with -n the multiplayer signal waits on the main menu
    /// until the frontend's next pass, which runs at the player's first
    /// input: the notice of a new record waits through that and through the
    /// multiplayer screens, and is told once when the main menu shows again.
    void notice_waits() {
        constexpr std::string_view where = "records-notice-wait";
        records_defaults();
        const fs::path folder = records_folder("notice-wait");
        start_on(folder, true);
        menu_frames(1);
        start_on(folder, false);
        menu_frames(1);
        start_on(folder, false);
        expect(
            rs::has_untold_record(host().records().records()),
            where,
            "the second left-over trial left no record to tell"
        );
        // The main menu as -n leaves it: the multiplayer signal waits.
        frontend::set_frontend_signal(runtime.state_, runtime, frontend::signal_id::multiplayer);
        for (uint32_t frame = 0; frame < notice_wait_frames; ++frame)
            runtime.idle_tick();
        expect(
            runtime.screen_ == Screen::main_menu && run().notices_noted.empty(),
            where,
            "the notice was told while the multiplayer signal waited"
        );
        // The player's first input runs the frontend's pass.
        runtime.activate();
        for (uint32_t frame = 0; frame < notice_wait_frames && runtime.screen_ == Screen::main_menu;
             ++frame)
            runtime.idle_tick();
        expect(runtime.screen_ != Screen::main_menu, where, "the multiplayer screens never showed");
        for (uint32_t frame = 0; frame < notice_wait_frames; ++frame)
            runtime.idle_tick();
        expect(
            run().notices_noted.empty(), where, "the notice was told over the multiplayer screens"
        );
        // Back to the main menu, as the multiplayer screens' Cancel goes.
        frontend::set_frontend_signal(runtime.state_, runtime, frontend::signal_id::back);
        for (uint32_t frame = 0; frame < notice_wait_frames && runtime.screen_ != Screen::main_menu;
             ++frame) {
            runtime.frontend_pass_requested_ = true;
            runtime.idle_tick();
        }
        expect(runtime.screen_ == Screen::main_menu, where, "the main menu never showed again");
        for (uint32_t frame = 0; frame < notice_wait_frames; ++frame)
            runtime.idle_tick();
        expect_notices(where, {rs::NoticeKind::accelerated_unusable});
        expect(
            !rs::has_untold_record(host().records().records()),
            where,
            "the notice did not mark the record told"
        );
        passed.emplace_back(where);
    }

    /// A lost device records accelerated-unusable at once and strikes the
    /// driver; the next start makes the same driver on the standard tier,
    /// and a second lost device there, in a match, records failed-driver
    /// when the match ends, which the start after skips.
    void lost_device() {
        constexpr std::string_view where = "records-lost";
        records_defaults();
        host().faults().record_driver = hardware_driver;
        const fs::path folder = records_folder("lost");
        start_on(folder, true);
        menu_frames(2);
        post_render_event(SDL_EVENT_RENDER_DEVICE_LOST);
        menu_frames(1);
        const auto* recorded = entry(hardware_driver);
        expect(
            recorded != nullptr &&
                recorded->accelerated_unusable.failure == rs::RecordedFailure::lost &&
                recorded->strike.stage == rs::StrikeStage::lost &&
                recorded->failed_driver.failure == rs::RecordedFailure::none,
            where,
            "a lost device did not record accelerated-unusable with a strike"
        );
        menu_frames(2);
        expect_notices(where, {rs::NoticeKind::accelerated_unusable});
        start_on(folder, true);
        expect(host().skipped_drivers().empty(), where, "a single lost device skipped the driver");
        menu_frames(2);
        expect(
            tier().reason == render_policy::TierReason::accelerated_unusable,
            where,
            "the start after a lost device did not stay standard"
        );
        start_match();
        runtime.render();
        post_render_event(SDL_EVENT_RENDER_DEVICE_LOST);
        runtime.render();
        expect(
            entry(hardware_driver)->failed_driver.failure == rs::RecordedFailure::lost,
            where,
            "a second lost device in a row was not recorded failed-driver"
        );
        expect(
            records_file(folder).count(std::string(rs::failed_driver_prefix) + hardware_driver) ==
                0,
            where,
            "a record was written while the match ran"
        );
        to_main_menu();
        expect(
            records_file(folder).count(std::string(rs::failed_driver_prefix) + hardware_driver) ==
                1,
            where,
            "the record was not written when the match ended"
        );
        menu_frames(3);
        expect_notices(where, {rs::NoticeKind::failed_driver});
        start_on(folder, true);
        expect(
            host().skipped_drivers().size() == 1,
            where,
            "the start after two lost devices did not skip the driver"
        );
        passed.emplace_back(where);
    }

    /// Posts the device resets that make the renderer again, and draws the
    /// frame that makes it.
    void post_resets() {
        forget_resets();
        for (int reset = 0; reset < resets_that_rebuild; ++reset)
            post_render_event(SDL_EVENT_RENDER_DEVICE_RESET);
        menu_frames(1);
    }

    /// Three resets within 60 s record accelerated-unusable and strike the
    /// driver; a clean run between two such runs clears the strike, so the
    /// second records no failed-driver.
    void resets() {
        constexpr std::string_view where = "records-resets";
        records_defaults();
        host().faults().record_driver = hardware_driver;
        const fs::path folder = records_folder("resets");
        start_on(folder, true);
        menu_frames(2);
        post_resets();
        const auto* recorded = entry(hardware_driver);
        expect(
            recorded != nullptr &&
                recorded->accelerated_unusable.failure == rs::RecordedFailure::resets &&
                recorded->strike.stage == rs::StrikeStage::resets,
            where,
            "three resets did not record accelerated-unusable with a strike"
        );
        start_on(folder, true);
        menu_frames(2);
        start_on(folder, true);
        expect(
            entry(hardware_driver)->strike.stage == rs::StrikeStage::none,
            where,
            "a clean run did not clear the strike"
        );
        menu_frames(2);
        post_resets();
        expect(
            entry(hardware_driver)->failed_driver.failure == rs::RecordedFailure::none &&
                entry(hardware_driver)->strike.stage == rs::StrikeStage::resets,
            where,
            "resets after a clean run recorded failed-driver"
        );
        passed.emplace_back(where);
    }

    /// A present error is struck, and recorded failed-driver only when the
    /// same call fails in the next run on the driver.
    void present_repeats() {
        constexpr std::string_view where = "records-present";
        records_defaults();
        host().faults().record_driver = hardware_driver;
        const fs::path folder = records_folder("present");
        start_on(folder, true);
        for (int start = 0; start < 2; ++start) {
            if (start == 1)
                start_on(folder, true);
            menu_frames(2);
            arm(RenderFaultPoint::present, 1);
            present_until_fault(where);
        }
        const auto* recorded = entry(hardware_driver);
        expect(
            recorded != nullptr && recorded->failed_driver.failure == rs::RecordedFailure::present,
            where,
            "the same present error in two runs was not recorded failed-driver"
        );
        passed.emplace_back(where);
    }

    /// Under 2 GiB the start stays standard with no function test and no
    /// trial, and the row is locked; a lost device keeps only its strike,
    /// and the second in a row records failed-driver, never
    /// accelerated-unusable, which frees the row.
    void small_machine() {
        constexpr std::string_view where = "records-small";
        records_defaults();
        auto& faults = host().faults();
        faults.physical_memory = small_memory;
        faults.record_driver = hardware_driver;
        const fs::path folder = records_folder("small");
        start_on(folder, true);
        menu_frames(2);
        expect(
            tier().reason == render_policy::TierReason::memory &&
                host().tier_inputs().function_test == render_policy::FunctionTest::not_run &&
                !host().records().records().trial &&
                records_file(folder).count(std::string(rs::trial_key)) == 0,
            where,
            "a machine under 2 GiB ran the function test or wrote a trial"
        );
        expect(
            status() == oa::ui::engine_settings::AccelerationState::needs_memory &&
                runtime.acceleration_report().acceleration_unavailable,
            where,
            "the status did not say it needs the memory, with the row locked"
        );
        for (int start = 0; start < 2; ++start) {
            if (start == 1)
                start_on(folder, true);
            menu_frames(2);
            post_render_event(SDL_EVENT_RENDER_DEVICE_LOST);
            menu_frames(1);
            expect(
                entry(hardware_driver)->accelerated_unusable.failure == rs::RecordedFailure::none,
                where,
                "a lost device under 2 GiB recorded accelerated-unusable"
            );
        }
        expect(
            entry(hardware_driver)->failed_driver.failure == rs::RecordedFailure::lost,
            where,
            "two lost devices in a row under 2 GiB were not recorded failed-driver"
        );
        start_on(folder, true);
        menu_frames(1);
        expect(
            status() == oa::ui::engine_settings::AccelerationState::needs_memory_driver_skipped &&
                !runtime.acceleration_report().acceleration_unavailable,
            where,
            "a skipped driver under 2 GiB did not show, with the row free"
        );
        passed.emplace_back(where);
    }

    /// An error of the game's own drops the tier with nothing struck. An
    /// accelerated-only failure drops the tier with a strike, and the
    /// same in the next run records accelerated-unusable. Then the settings
    /// dialog's retry: switching Off then On clears the records in memory
    /// and tries the graphics card at once, the file keeping them, and a
    /// failure struck meanwhile beside them; Cancel puts them back, with
    /// that strike, and OK writes them cleared.
    void accelerated_failure_and_retry() {
        constexpr std::string_view where = "records-call";
        records_defaults();
        const fs::path folder = records_folder("call");
        const AccelerationError failure("SDL_CreateTexture of a scene tile: out of memory");
        // An error of the game's own drops the tier and strikes nothing.
        start_on(folder, true);
        menu_frames(1);
        runtime.take_acceleration_error(AccelerationError(
            "the prescale target is smaller than the source it is to hold",
            AccelerationFault::engine
        ));
        menu_frames(1);
        expect(
            !runtime.accelerated_.on &&
                host().tier_inputs().drop == render_policy::Drop::engine_fault &&
                entry(render_probe::software_renderer) == nullptr,
            where,
            "an error of the game's own did not drop the tier, or was struck against the driver"
        );
        expect(
            status() == oa::ui::engine_settings::AccelerationState::engine_error,
            where,
            "the status did not say an error stopped it"
        );
        start_on(folder, true);
        for (int start = 0; start < 2; ++start) {
            if (start == 1)
                start_on(folder, true);
            menu_frames(1);
            expect(
                render_policy::card_tier(tier().tier),
                where,
                "the tier was not accelerated before the failure"
            );
            runtime.take_acceleration_error(failure);
            expect(
                !runtime.accelerated_.on &&
                    host().tier_inputs().drop == render_policy::Drop::driver_failure,
                where,
                "the failure did not drop the tier"
            );
            const auto* struck = entry(render_probe::software_renderer);
            expect(
                struck != nullptr &&
                    (start == 0
                         ? struck->strike.stage == rs::StrikeStage::call &&
                               struck->strike.call == "SDL_CreateTexture-of-a-scene-tile"
                         : struck->accelerated_unusable.failure == rs::RecordedFailure::call),
                where,
                start == 0 ? "the failure was not struck"
                           : "the same failure in the next run was not recorded"
            );
        }
        start_on(folder, true);
        menu_frames(1);
        expect(
            tier().reason == render_policy::TierReason::accelerated_unusable,
            where,
            "the start after the record did not stay standard"
        );
        const std::string record_key = std::string(rs::accelerated_unusable_prefix) +
                                       std::string(render_probe::software_renderer);
        // Off then On, then Cancel.
        auto* dialog = &runtime.open_engine_settings_dialog();
        ++dialog->forget_renderer_failures;
        std::ignore =
            runtime.take_engine_settings_action(oa::ui::engine_settings::DialogAction::changed);
        expect(
            entry(render_probe::software_renderer) == nullptr &&
                records_file(folder).count(record_key) == 1,
            where,
            "Off then On did not clear the records in memory alone"
        );
        menu_frames(1);
        expect(
            render_policy::card_tier(tier().tier),
            where,
            "the retry did not try the graphics card at once"
        );
        std::ignore =
            runtime.take_engine_settings_action(oa::ui::engine_settings::DialogAction::cancelled);
        expect(
            entry(render_probe::software_renderer) != nullptr,
            where,
            "Cancel did not put the records back"
        );
        menu_frames(1);
        expect(
            tier().reason == render_policy::TierReason::accelerated_unusable,
            where,
            "the records put back did not keep the tier standard"
        );
        // A failure struck during a retry reaches the file at once, beside
        // the records the retry cleared, and Cancel puts both back.
        dialog = &runtime.open_engine_settings_dialog();
        ++dialog->forget_renderer_failures;
        std::ignore =
            runtime.take_engine_settings_action(oa::ui::engine_settings::DialogAction::changed);
        menu_frames(1);
        runtime.take_acceleration_error(AccelerationError("SDL_SetRenderTarget: device removed"));
        const std::string strike_key =
            std::string(rs::strike_prefix) + std::string(render_probe::software_renderer);
        const rs::Values during = records_file(folder);
        expect(
            during.count(record_key) == 1 && during.count(strike_key) == 1,
            where,
            "a strike during the retry did not reach the file beside the records it cleared"
        );
        std::ignore =
            runtime.take_engine_settings_action(oa::ui::engine_settings::DialogAction::cancelled);
        const auto* restored = entry(render_probe::software_renderer);
        expect(
            restored != nullptr &&
                restored->accelerated_unusable.failure == rs::RecordedFailure::call &&
                restored->strike.stage == rs::StrikeStage::call &&
                restored->strike.call == "SDL_SetRenderTarget",
            where,
            "Cancel did not put back the records with the strike found since"
        );
        // Off then On, then OK.
        dialog = &runtime.open_engine_settings_dialog();
        ++dialog->forget_renderer_failures;
        std::ignore =
            runtime.take_engine_settings_action(oa::ui::engine_settings::DialogAction::changed);
        std::ignore =
            runtime.take_engine_settings_action(oa::ui::engine_settings::DialogAction::accepted);
        expect(
            entry(render_probe::software_renderer) == nullptr &&
                records_file(folder).count(record_key) == 0,
            where,
            "OK did not write the records cleared"
        );
        passed.emplace_back(where);
    }

    // -----------------------------------------------------------------------
    // The Full tier: its drops, trial, strikes, records and rungs

    /// Returns the Full tier's presentation, which must exist.
    ///
    /// @param where the case
    /// @return the presentation
    FullPresentation& full(std::string_view where) {
        expect(runtime.full_ != nullptr, where, "the full tier has no presentation");
        return *runtime.full_;
    }

    /// Switches the Full tier on, as --hardware-acceleration=full and
    /// --force-capable would, at the top rung, or back to what the run's
    /// own options say.
    ///
    /// @param where the case
    /// @param on true to switch it on
    void full_tier(std::string_view where, bool on) {
        if (on) {
            run().rung = top_rung();
            runtime.options_.hardware_acceleration =
                oa::ui::engine_settings::HardwareAcceleration::full;
            runtime.options_.force_capable = true;
        } else {
            run().rung.reset();
            runtime.options_.hardware_acceleration.reset();
            runtime.options_.force_capable = false;
        }
        runtime.update_render_tier();
        if (on)
            expect(runtime.full_presentation(), where, "the full tier did not draw");
    }

    /// Presents match frames at zoom 1 until the card has drawn a frame's
    /// terrain.
    ///
    /// @param where the case
    void full_frame(std::string_view where) {
        at_zoom(zoom_one);
        for (uint32_t frame = 0; frame < full_frames && !full(where).drawn; ++frame)
            runtime.render();
        expect(full(where).drawn, where, "the card did not draw the terrain");
    }

    /// Returns the Full tier's facts of the run.
    render_policy::FullDrop full_drop() { return host().tier_inputs().full_drop; }

    /// A call of the card's own fails on a Full match frame (--render-fault
    /// card): Full drops to Basic for the run, which presents the frame the
    /// processor composed, with the failing call struck against the driver
    /// and the status saying Full stopped; Off then On tries Full again,
    /// and the same failure again in the run is struck no further.
    ///
    /// @param frame the presented frame of the match the failure comes at
    void card_failure(uint32_t frame) {
        constexpr std::string_view where = "card";
        start_match();
        for (uint32_t presented = 1; presented < frame; ++presented)
            runtime.render();
        // Whatever an earlier case dropped is lifted, as Off then On would.
        runtime.forget_render_failures();
        full_tier(where, true);
        full_frame(where);
        const auto& inputs = host().tier_inputs();
        expect(
            inputs.full_drop == render_policy::FullDrop::none &&
                tier().tier == render_policy::RenderTier::full,
            where,
            "the tier was not full before the failure"
        );
        arm(RenderFaultPoint::card, 1);
        present_until_fault(where);
        expect(
            !runtime.full_presentation() && runtime.accelerated_presentation() &&
                inputs.full_drop == render_policy::FullDrop::card_failure,
            where,
            "the card's failure did not drop full to basic"
        );
        const auto* struck = entry(render_probe::software_renderer);
        expect(
            struck != nullptr && struck->strike.stage == rs::StrikeStage::card &&
                struck->strike.call == "the-card-refused-the-frame" &&
                struck->full_unusable.failure == rs::RecordedFailure::none,
            where,
            "the failing call was not struck against the driver, or was recorded at once"
        );
        runtime.render();
        expect(
            tier().tier == render_policy::RenderTier::accelerated &&
                tier().full == render_policy::FullReason::dropped,
            where,
            "the next frame was not basic with full dropped"
        );
        expect(
            status() == oa::ui::engine_settings::AccelerationState::full_stopped,
            where,
            "the status did not say full stopped for this run"
        );
        expect_composed(where);
        // Off then On tries Full again, and a second failure in the same
        // run strikes no further.
        runtime.forget_render_failures();
        runtime.update_render_tier();
        expect(
            runtime.full_presentation() && inputs.full_drop == render_policy::FullDrop::none,
            where,
            "Off then On did not try full again"
        );
        full_frame(where);
        arm(RenderFaultPoint::card, 1);
        present_until_fault(where);
        struck = entry(render_probe::software_renderer);
        expect(
            !runtime.full_presentation() && struck != nullptr &&
                struck->strike.stage == rs::StrikeStage::card &&
                struck->full_unusable.failure == rs::RecordedFailure::none,
            where,
            "the same failure again in the run was recorded"
        );
        runtime.forget_render_failures();
        full_tier(where, false);
        passed.emplace_back(where);
    }

    /// In a shared game or a replay Full falls to Basic at once and rises
    /// again only when the match ends; a page or a target Full needs during
    /// the match is made then, as at any time, and a shared game's loading
    /// screen makes the terrain pages and the targets before the world is
    /// built.
    void shared_game() {
        constexpr std::string_view where = "full-shared";
        start_match();
        runtime.forget_render_failures();
        full_tier(where, true);
        full_frame(where);
        auto& gate = host().tier_inputs().match;
        const std::size_t struck_before = host().records().records().drivers.size();
        // The match, as a shared game from its loading screen.
        runtime.begin_render_tier_match(render_policy::MatchKind::shared_game);
        expect(gate.accelerated && gate.full, where, "the shared game did not begin in full");
        runtime.render();
        expect(runtime.full_presentation(), where, "full did not draw in the shared game");
        // Lower at once.
        runtime.options_.hardware_acceleration =
            oa::ui::engine_settings::HardwareAcceleration::basic;
        runtime.render();
        expect(
            !runtime.full_presentation() && runtime.accelerated_presentation() && !gate.full,
            where,
            "basic did not apply at once in the shared game"
        );
        // Higher from the next game.
        runtime.options_.hardware_acceleration =
            oa::ui::engine_settings::HardwareAcceleration::full;
        runtime.render();
        expect(
            !runtime.full_presentation() &&
                tier().full == render_policy::FullReason::waiting_for_match_end,
            where,
            "full did not wait for the shared game to end"
        );
        expect(
            status() == oa::ui::engine_settings::AccelerationState::full_waiting_for_game_end,
            where,
            "the status did not say full waits for the game's end"
        );
        runtime.end_render_tier_match();
        runtime.render();
        expect(runtime.full_presentation(), where, "full did not return after the match");
        // A page or a target Full needs during a replay or a shared game is
        // made then, as at any time: pages freed during a replay are made
        // again by the next Full frame, with nothing dropped or struck.
        runtime.begin_render_tier_match(render_policy::MatchKind::replay);
        runtime.free_full_presentation();
        expect(full(where).pages.empty(), where, "the pages were not freed");
        full_frame(where);
        expect(
            !full(where).pages.empty() && gate.full &&
                full_drop() == render_policy::FullDrop::none &&
                host().records().records().drivers.size() == struck_before,
            where,
            "the pages were not made again during the replay, or making them dropped or "
            "struck something"
        );
        runtime.end_render_tier_match();
        runtime.update_render_tier();
        expect(runtime.full_presentation(), where, "full did not draw on after the replay");
        // A shared game's loading screen makes the terrain pages and the
        // targets before the world is built, so that its first frames find
        // them and make no terrain page.
        runtime.begin_render_tier_match(render_policy::MatchKind::shared_game);
        runtime.free_full_presentation();
        runtime.preallocate_full_match_textures();
        auto& made = full(where);
        const std::size_t pages = made.pages.size();
        expect(
            pages != 0 && made.overlay_texture.made() &&
                (made.target != card::TargetHandle{} || made.target_refused) && gate.full,
            where,
            "the loading screen did not make full's pages and targets"
        );
        full_frame(where);
        expect(
            made.pages.size() == pages && gate.full && runtime.full_presentation(),
            where,
            "a frame of the shared game made a terrain page"
        );
        runtime.end_render_tier_match();
        full_tier(where, false);
        passed.emplace_back(where);
    }

    /// A left-over Full trial: the second in a row records full-unusable,
    /// which leaves Basic standing and is told once; where the first
    /// counts, at once. --hardware-acceleration=full ignores the record.
    void left_over_full_trial() {
        constexpr std::string_view where = "records-full-trial";
        records_defaults();
        const fs::path folder = records_folder("full-trial");
        runtime.options_.hardware_acceleration =
            oa::ui::engine_settings::HardwareAcceleration::full;
        start_on(folder, true);
        menu_frames(1);
        pass_start(where);
        // Full's first match frame writes its trial and sentinel; its first
        // frames pass after path_stage_frames of them.
        start_match();
        full_frame(where);
        const auto& sentinel = host().records().sentinel();
        const auto& trial = host().records().records().trial;
        expect(
            sentinel && sentinel->stage == rs::SentinelStage::path &&
                sentinel->path == rs::AcceleratedPath::full && trial &&
                trial->stage == rs::StrikeStage::path && trial->path == rs::AcceleratedPath::full,
            where,
            "full's first frame did not write its trial and sentinel"
        );
        expect(
            records_file(folder).at(std::string(rs::trial_key)) == "path full software",
            where,
            "full's trial did not reach the file"
        );
        match_frames(zoom_one, rs::path_stage_frames);
        expect_running(where, "full's first frames did not pass");
        // Killed in Full's first frames, twice in a row.
        for (int start = 0; start < 2; ++start) {
            rs::Values values = records_file(folder);
            values[std::string(rs::trial_key)] = "path full software";
            preferences::save(folder / rs::records_file_name, values);
            start_on(folder, false);
            const auto* struck = entry(render_probe::software_renderer);
            expect(
                struck != nullptr &&
                    (start == 0 ? struck->strike.stage == rs::StrikeStage::path &&
                                      struck->strike.path == rs::AcceleratedPath::full &&
                                      struck->full_unusable.failure == rs::RecordedFailure::none
                                : struck->full_unusable.failure == rs::RecordedFailure::stopped &&
                                      struck->strike.stage == rs::StrikeStage::none),
                where,
                start == 0 ? "the first left-over full trial was not a strike alone"
                           : "the second left-over full trial in a row was not recorded"
            );
            expect(
                struck->accelerated_unusable.failure == rs::RecordedFailure::none,
                where,
                "a left-over full trial counted against basic"
            );
        }
        menu_frames(3);
        expect(
            host().tier_inputs().full_unusable_record && runtime.full_presentation(),
            where,
            "the record was not read, or --hardware-acceleration=full did not ignore it"
        );
        expect_notices(where, {rs::NoticeKind::full_unusable});
        expect(
            entry(render_probe::software_renderer)->full_unusable.told,
            where,
            "the notice did not mark the record told"
        );
        // The setting alone honours it: Basic, as the policy says.
        runtime.options_.hardware_acceleration.reset();
        runtime.engine_settings_state().current.hardware_acceleration =
            oa::ui::engine_settings::HardwareAcceleration::full;
        menu_frames(1);
        expect(
            !runtime.full_presentation() && runtime.accelerated_presentation() &&
                tier().full != render_policy::FullReason::full,
            where,
            "the setting's full did not fall to basic"
        );
        expect(
            !rs::full_allowed(host().records().records(), render_probe::software_renderer, false),
            where,
            "the record does not keep full off the driver"
        );
        runtime.engine_settings_state().current.hardware_acceleration =
            oa::ui::engine_settings::HardwareAcceleration::basic;
        // Where the first counts, one left-over full trial is a record.
        host().faults().crash_evidence = rs::CrashEvidence::first_counts;
        const fs::path first = records_folder("full-first-trial");
        runtime.options_.hardware_acceleration =
            oa::ui::engine_settings::HardwareAcceleration::full;
        start_on(first, true);
        menu_frames(1);
        rs::Values values = records_file(first);
        values[std::string(rs::trial_key)] = "path full software";
        preferences::save(first / rs::records_file_name, values);
        start_on(first, false);
        const auto* recorded = entry(render_probe::software_renderer);
        expect(
            recorded != nullptr &&
                recorded->full_unusable.failure == rs::RecordedFailure::stopped &&
                recorded->accelerated_unusable.failure == rs::RecordedFailure::none &&
                host().leftovers().change.new_record,
            where,
            "the first left-over full trial was not recorded where the first counts"
        );
        host().faults().crash_evidence = rs::CrashEvidence::two_in_a_row;
        runtime.options_.hardware_acceleration.reset();
        passed.emplace_back(where);
    }

    /// Full's trial cannot be written: Full is not used, Basic draws, with
    /// nothing struck and the status saying the game cannot save its
    /// files; Off then On tries the write again.
    void unwritable_full_trial() {
        constexpr std::string_view where = "records-full-unwritable";
        records_defaults();
        const fs::path folder = records_folder("full-unwritable");
        runtime.options_.hardware_acceleration =
            oa::ui::engine_settings::HardwareAcceleration::full;
        start_on(folder, true);
        menu_frames(1);
        pass_start(where);
        fs::remove(folder / rs::records_file_name);
        fs::create_directories(folder / rs::records_file_name / "blocked");
        start_match();
        runtime.render();
        expect(
            !runtime.full_presentation() && runtime.accelerated_presentation() &&
                full_drop() == render_policy::FullDrop::trial_unwritten &&
                host().records().records().drivers.empty(),
            where,
            "full whose trial could not be written did not fall to basic with nothing struck"
        );
        expect_running(where, "full's unwritten trial left a sentinel or a trial");
        expect(
            status() == oa::ui::engine_settings::AccelerationState::full_cannot_save,
            where,
            "the status did not say the game cannot save its files"
        );
        fs::remove_all(folder / rs::records_file_name);
        runtime.forget_render_failures();
        runtime.update_render_tier();
        full_frame(where);
        expect(
            host().records().records().trial &&
                host().records().records().trial->path == rs::AcceleratedPath::full,
            where,
            "Off then On did not write full's trial"
        );
        runtime.options_.hardware_acceleration.reset();
        passed.emplace_back(where);
    }

    /// The same card call failing in two runs in a row records
    /// full-unusable, told once at the main menu; Off then On and a raise of
    /// the row from Basic to Full clear it, and a run that ends cleanly
    /// clears the strike.
    void card_failure_record() {
        constexpr std::string_view where = "records-card";
        records_defaults();
        const fs::path folder = records_folder("card");
        runtime.options_.hardware_acceleration =
            oa::ui::engine_settings::HardwareAcceleration::full;
        for (int start = 0; start < 2; ++start) {
            start_on(folder, true);
            menu_frames(1);
            start_match();
            full_frame(where);
            arm(RenderFaultPoint::card, 1);
            present_until_fault(where);
            const auto* struck = entry(render_probe::software_renderer);
            expect(
                !runtime.full_presentation() && struck != nullptr &&
                    (start == 0 ? struck->strike.stage == rs::StrikeStage::card &&
                                      struck->full_unusable.failure == rs::RecordedFailure::none
                                : struck->full_unusable.failure == rs::RecordedFailure::card &&
                                      struck->strike.stage == rs::StrikeStage::none),
                where,
                start == 0 ? "the card's failure was not struck"
                           : "the same failure in the next run was not recorded"
            );
            to_main_menu();
        }
        // The record reaches the file when the match ends, and the next
        // start tells of it once.
        expect(
            records_file(folder).count(
                std::string(rs::full_unusable_prefix) + std::string(render_probe::software_renderer)
            ) == 1,
            where,
            "the record did not reach the file"
        );
        start_on(folder, true);
        menu_frames(3);
        expect_notices(where, {rs::NoticeKind::full_unusable});
        // A raise of the row, Basic to Full, which the dialog counts as a
        // request to try the card afresh, clears it in memory, and OK
        // writes the clearing.
        runtime.options_.hardware_acceleration.reset();
        auto* dialog = &runtime.open_engine_settings_dialog();
        dialog->chosen.hardware_acceleration = oa::ui::engine_settings::HardwareAcceleration::full;
        ++dialog->forget_renderer_failures;
        std::ignore =
            runtime.take_engine_settings_action(oa::ui::engine_settings::DialogAction::changed);
        std::ignore =
            runtime.take_engine_settings_action(oa::ui::engine_settings::DialogAction::accepted);
        expect(
            entry(render_probe::software_renderer) == nullptr &&
                records_file(folder).count(
                    std::string(rs::full_unusable_prefix) +
                    std::string(render_probe::software_renderer)
                ) == 0,
            where,
            "the raise did not clear the record"
        );
        runtime.engine_settings_state().current.hardware_acceleration =
            oa::ui::engine_settings::HardwareAcceleration::basic;
        // A strike alone is cleared by a run that ends cleanly without it.
        runtime.options_.hardware_acceleration =
            oa::ui::engine_settings::HardwareAcceleration::full;
        start_on(folder, true);
        menu_frames(1);
        start_match();
        full_frame(where);
        arm(RenderFaultPoint::card, 1);
        present_until_fault(where);
        to_main_menu();
        start_on(folder, true);
        expect(
            entry(render_probe::software_renderer) != nullptr &&
                entry(render_probe::software_renderer)->strike.stage == rs::StrikeStage::card,
            where,
            "the strike did not stand at the next start"
        );
        start_on(folder, true);
        expect(
            entry(render_probe::software_renderer) == nullptr,
            where,
            "a clean run did not clear the strike"
        );
        runtime.options_.hardware_acceleration.reset();
        passed.emplace_back(where);
    }

    /// The memory guard with the system's memory forced: free memory just
    /// over its threshold refuses Full's pages, which drops Full to Basic,
    /// nothing struck and Off then On not lifting it; and committed memory
    /// past its threshold while Full draws drops Full first, freeing its
    /// pages, and Basic at the next sample while memory stays short.
    ///
    /// @param frame the presented frame of the match the case begins at
    /// @return 0, or skipped_exit_code under 2 GiB of memory
    int full_memory(uint32_t frame) {
        constexpr std::string_view where = "full-memory";
        if (run().host->tier_inputs().memory < policy::smallest_accelerated_memory) {
            std::cout << "renderer ladder check: full-memory: skipped: the machine reports less "
                         "than the 2 GiB threshold of memory\n";
            return skipped_exit_code;
        }
        start_match();
        for (uint32_t presented = 1; presented < frame; ++presented)
            runtime.render();
        accelerate(where, true);
        auto& watched = watch(where);
        const policy::MemoryGuardThresholds thresholds = watched.memory.thresholds;
        expect(thresholds.physical != 0, where, "the memory guard knows no physical memory");
        oa::platform::SystemMemorySample sample{};
        sample.physical = thresholds.physical;
        sample.available = thresholds.free_floor + 1;
        sample.available_known = true;
        sample.committed = 0;
        sample.committed_known = true;
        run().forced_memory = sample;
        full_tier(where, true);
        at_zoom(zoom_one);
        for (uint32_t presented = 0; presented < refused_buffer_frames; ++presented)
            runtime.render();
        auto& inputs = host().tier_inputs();
        expect(
            !runtime.full_presentation() && runtime.accelerated_presentation() &&
                inputs.full_drop == render_policy::FullDrop::memory &&
                inputs.drop == render_policy::Drop::none && full(where).pages.empty() &&
                host().records().records().drivers.empty(),
            where,
            "a page the memory guard refused did not drop full alone, with nothing struck"
        );
        expect(
            status() == oa::ui::engine_settings::AccelerationState::full_too_little_memory,
            where,
            "the status does not say there is too little memory for full"
        );
        runtime.forget_render_failures();
        runtime.update_render_tier();
        expect(
            !runtime.full_presentation() && inputs.full_drop == render_policy::FullDrop::memory,
            where,
            "Off then On lifted the memory guard's drop of full"
        );
        // Full drawing, and committed memory then past its threshold: Full
        // first, then Basic at the next sample.
        inputs.full_drop = render_policy::FullDrop::none;
        sample.available = thresholds.physical / 2;
        run().forced_memory = sample;
        full_tier(where, true);
        full_frame(where);
        expect(!full(where).pages.empty(), where, "full made no pages with memory to spare");
        sample.committed = thresholds.committed_limit + 1;
        run().forced_memory = sample;
        policy::resume_memory_guard(watched.memory);
        runtime.render();
        expect(
            !runtime.full_presentation() && runtime.accelerated_presentation() &&
                inputs.full_drop == render_policy::FullDrop::memory &&
                inputs.drop == render_policy::Drop::none && full(where).pages.empty() &&
                watched.memory.tripped == policy::MemoryGuardCause::none,
            where,
            "committed memory past its threshold did not drop full first, freeing its pages"
        );
        runtime.render();
        expect(
            !runtime.accelerated_presentation() && inputs.drop == render_policy::Drop::memory &&
                watched.memory.tripped == policy::MemoryGuardCause::committed,
            where,
            "memory still short did not drop basic at the next sample"
        );
        run().forced_memory.reset();
        full_tier(where, false);
        passed.emplace_back(where);
        return 0;
    }

    /// Runs the records' cases, then starts again on records in memory with
    /// the setting at the named file's default, as the check began.
    void records() {
        const ScratchFolder scratch(report_directory);
        records_root = scratch.path;
        for (int index = 0; index < SDL_GetNumRenderDrivers(); ++index) {
            const char* name = SDL_GetRenderDriver(index);
            if (name != nullptr && std::string_view(name) != render_probe::software_renderer) {
                hardware_driver = name;
                break;
            }
        }
        auto& renderer = host();
        renderer.set_stage_clock({&clock_ns, read_check_clock});
        const bool force_capable = runtime.options_.force_capable;
        auto& settings = runtime.engine_settings_state().current;
        const auto setting = settings.hardware_acceleration;
        runtime.options_.force_capable = true;
        settings.hardware_acceleration = oa::ui::engine_settings::HardwareAcceleration::basic;
        to_main_menu();
        expect(
            SDL_SetWindowSize(runtime.sdl_.window, records_window_width, records_window_height) &&
                SDL_SyncWindow(runtime.sdl_.window),
            "records",
            std::string("SDL_SetWindowSize: ") + SDL_GetError()
        );
        // Whatever happens, the check goes on, or ends, on records in
        // memory, with nothing left in the scratch folder's records.
        const auto acceleration = runtime.options_.hardware_acceleration;
        const auto back_in_memory = [&]() {
            runtime.options_.force_capable = force_capable;
            runtime.options_.hardware_acceleration = acceleration;
            settings.hardware_acceleration = setting;
            auto& faults = renderer.faults();
            faults.physical_memory.reset();
            faults.crash_evidence.reset();
            faults.record_driver.clear();
            faults.adapter.clear();
            run().rung.reset();
            faults.refuse_driver = nullptr;
            faults.context = nullptr;
            start_on({}, true);
            renderer.set_stage_clock({});
        };
        try {
            left_over_trial();
            first_trial_counts();
            clean_pass();
            unwritable_trial();
            paths();
            path_window();
            notice_waits();
            accelerated_failure_and_retry();
            left_over_full_trial();
            unwritable_full_trial();
            card_failure_record();
            if (hardware_driver.empty()) {
                std::cout << "renderer ladder check: SDL has no hardware render driver; the "
                             "cases that record one are left out\n";
            } else {
                left_over_sentinel();
                advice();
                adapter();
                lost_device();
                resets();
                present_repeats();
                small_machine();
            }
        } catch (...) {
            back_in_memory();
            throw;
        }
        back_in_memory();
        menu_frames(1);
    }
};

int Runtime::check_renderer_ladder() {
    if (!render_run_)
        throw std::runtime_error(
            "renderer ladder check: the game has no renderer of its host; run it with a window"
        );
    RendererLadder ladder{*this};
    ladder.report_directory = "local/reports";
    fs::create_directories(ladder.report_directory);
    const auto& fault = options_.render_fault;
    const auto runs = [&](RenderFaultPoint point) { return !fault || fault->point == point; };
    const uint32_t frame = fault && fault->frame ? *fault->frame : match_case_frame;
    // The memory guard's cases of the accelerated tier and of Full run only
    // when named.
    if (fault && (fault->point == RenderFaultPoint::memory ||
                  fault->point == RenderFaultPoint::full_memory)) {
        const int status = fault->point == RenderFaultPoint::memory ? ladder.memory(frame)
                                                                    : ladder.full_memory(frame);
        if (status == 0)
            std::cout << "renderer ladder check: " << ladder.passed.front() << " passed\n";
        return status;
    }
    if (runs(RenderFaultPoint::create))
        ladder.walk(fault.has_value());
    // The Full tier's cases come before the rebuilds, which drop
    // acceleration for the run; the shared game forces no failure.
    if (!fault)
        ladder.shared_game();
    if (runs(RenderFaultPoint::card))
        ladder.card_failure(frame);
    if (runs(RenderFaultPoint::present)) {
        ladder.present(frame);
        ladder.loading_present();
        ladder.texture();
    }
    if (runs(RenderFaultPoint::reset)) {
        ladder.reset(frame);
        ladder.loading_reset();
        ladder.drain_reset();
        ladder.movie_reset();
    }
    if (runs(RenderFaultPoint::lost)) {
        ladder.lost(frame);
        ladder.device_lost(frame);
    }
    if (runs(RenderFaultPoint::stall))
        ladder.stall(frame);
    if (runs(RenderFaultPoint::float_state))
        ladder.float_state(frame);
    // The records and the tiles force no failure: they run with every case.
    if (!fault) {
        ladder.records();
        ladder.tiles();
    }
    std::cout << "renderer ladder check:";
    for (const auto& name : ladder.passed)
        std::cout << ' ' << name;
    std::cout << " passed; " << render_run_->rebuilds << " renderer(s) made again, "
              << render_run_->resets_handled << " reset(s) handled, " << render_run_->stall_logs
              << " stall log(s)\n";
    return 0;
}

} // namespace oa::app

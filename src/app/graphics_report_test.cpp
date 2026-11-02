// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The texture limit the render policy corrects from what the probe found,
// for each kind of report; the line the game logs once its renderer is
// made, and the adapter the "+stats" overlay names: a hardware adapter
// named, one that could not be named, one not read, SDL's software
// renderer with no limit, and a limit the device's own cuts; whether
// SDL_RENDER_DRIVER's value, and the flags with it, let the start read the
// adapter; the tier the line names, with what it does or why the processor
// draws everything; what the render policy reads of a renderer, Windows
// before Vista's one driver among it; then SDL's software renderer
// on the dummy video driver, reported as the game reports it.
#include "graphics_report.hpp"

#include "oa/test/check.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace {

using oa::app::corrected_texture_limit;
using oa::app::graphics_log_line;
using oa::app::stats_adapter_name;
using oa::app::stats_device_adapter_name;
using oa::platform::render_probe::AdapterFacts;
using oa::platform::render_probe::AdapterState;

/// Returns facts as the probe fills them for a renderer.
///
/// @param renderer SDL's name for the render driver
/// @param video SDL's name for the video driver
/// @param state what is known of the adapter
/// @param adapter the adapter's name
/// @param reported the texture limit SDL reports, texels; 0 for none
/// @param device the device's own texture limit, texels; 0 where not read
/// @return the facts
AdapterFacts facts_of(
    const char* renderer,
    const char* video,
    AdapterState state,
    const char* adapter,
    int64_t reported,
    uint32_t device = 0
) {
    AdapterFacts facts{};
    facts.renderer = renderer;
    facts.video_driver = video;
    facts.adapter_state = state;
    facts.adapter = adapter;
    facts.reported_texture_limit = reported;
    facts.device_texture_limit = device;
    return facts;
}

/// Returns the corrected texture limit of a renderer whose adapter was read.
///
/// @param renderer SDL's name for the render driver
/// @param reported the texture limit SDL reports, texels; 0 or less for none
/// @param device the device's own texture limit, texels; 0 where not read
/// @return corrected_texture_limit's answer
uint32_t limit_of(const char* renderer, int64_t reported, uint32_t device) {
    return corrected_texture_limit(
        facts_of(renderer, "x11", AdapterState::read, "card", reported, device)
    );
}

/// SDL's software renderer reports no limit, and a report of 0 or less is
/// none; a driver that reports the device's own limit keeps its report,
/// held at 32 bits; vulkan and gpu, which report a fixed 16384, take the
/// device's own limit where it was read, never above the report, and
/// otherwise the report at most 8192, as the render policy decides.
void test_corrected_texture_limit() {
    constexpr int64_t kFixedReport = 16384;
    constexpr uint32_t kUnreadCap = 8192;
    OA_CHECK(limit_of("software", 0, 0) == 0);
    OA_CHECK(limit_of("opengl", 0, 0) == 0);
    OA_CHECK(limit_of("opengl", -1, 0) == 0);
    OA_CHECK(limit_of("direct3d", 2048, 0) == 2048);
    OA_CHECK(limit_of("metal", kFixedReport, 0) == kFixedReport);
    OA_CHECK(limit_of("direct3d11", kFixedReport, 4096) == kFixedReport);
    OA_CHECK(limit_of("opengl", int64_t{1} << 40, 0) == UINT32_MAX);
    OA_CHECK(limit_of("vulkan", kFixedReport, 4096) == 4096);
    OA_CHECK(limit_of("vulkan", kFixedReport, 32768) == kFixedReport);
    OA_CHECK(limit_of("vulkan", kFixedReport, 0) == kUnreadCap);
    OA_CHECK(limit_of("gpu", kFixedReport, 0) == kUnreadCap);
    OA_CHECK(limit_of("gpu", kFixedReport, 2048) == 2048);
    OA_CHECK(limit_of("gpu", 4096, 0) == 4096);
    OA_CHECK(limit_of("gpu", 0, 2048) == 2048);
    OA_CHECK(limit_of("gpu", 0, 0) == kUnreadCap);
    // The adapter not read, as under SDL_RENDER_DRIVER: no device limit.
    OA_CHECK(
        corrected_texture_limit(
            facts_of("vulkan", "x11", AdapterState::skipped, "", kFixedReport)
        ) == kUnreadCap
    );
}

/// The line names the driver, the video driver, the adapter as known, the
/// texture limit and the standard tier.
void test_log_line() {
    constexpr uint32_t kMetalLimit = 16384;
    constexpr uint32_t kOldCardLimit = 2048;
    OA_CHECK(
        graphics_log_line(
            facts_of("metal", "cocoa", AdapterState::read, "Apple M2", kMetalLimit)
        ) == "open-annihilation: graphics: metal on cocoa (Apple M2), textures up to 16384; "
             "standard tier: the processor draws everything"
    );
    OA_CHECK(
        graphics_log_line(facts_of(
            "opengl", "x11", AdapterState::read, "llvmpipe (LLVM 15.0.7, 256 bits)", kMetalLimit
        )) == "open-annihilation: graphics: opengl on x11 (llvmpipe (LLVM 15.0.7, 256 bits)), "
              "textures up "
              "to 16384; standard tier: the processor draws everything"
    );
    OA_CHECK(
        graphics_log_line(
            facts_of("direct3d12", "windows", AdapterState::unknown, "", kOldCardLimit)
        ) == "open-annihilation: graphics: direct3d12 on windows (unknown adapter), textures up to "
             "2048; "
             "standard tier: the processor draws everything"
    );
    OA_CHECK(
        graphics_log_line(
            facts_of("direct3d", "windows", AdapterState::skipped, "", kOldCardLimit)
        ) ==
        "open-annihilation: graphics: direct3d on windows, textures up to 2048; standard tier: the "
        "processor draws everything"
    );
    OA_CHECK(
        graphics_log_line(facts_of("software", "dummy", AdapterState::none, "", 0)) ==
        "open-annihilation: graphics: software on dummy, textures of any size; standard tier: the "
        "processor draws everything"
    );
    // The device's own limit cuts vulkan's fixed report.
    OA_CHECK(
        graphics_log_line(facts_of(
            "vulkan", "x11", AdapterState::read, "AMD Radeon RX 580", kMetalLimit, kOldCardLimit
        )) == "open-annihilation: graphics: vulkan on x11 (AMD Radeon RX 580), textures up to "
              "2048; standard tier: the processor draws everything"
    );
}

/// The "+stats" overlay names a read adapter, says an unnamed one is
/// unknown, and names none where none was read; and names the turned view's
/// graphics device's adapter, or says it is unknown.
void test_stats_adapter() {
    OA_CHECK(
        stats_adapter_name(facts_of("metal", "cocoa", AdapterState::read, "Apple M2", 0)) ==
        "Apple M2"
    );
    OA_CHECK(
        stats_adapter_name(facts_of("gpu", "x11", AdapterState::unknown, "", 0)) ==
        "unknown adapter"
    );
    OA_CHECK(stats_adapter_name(facts_of("opengl", "x11", AdapterState::skipped, "", 0)).empty());
    OA_CHECK(stats_adapter_name(facts_of("software", "x11", AdapterState::none, "", 0)).empty());
    // The turned view's graphics device: its adapter as SDL names it, or
    // unknown where SDL gives no name.
    OA_CHECK(
        stats_device_adapter_name("NVIDIA GeForce RTX 4070 Laptop GPU") ==
        "NVIDIA GeForce RTX 4070 Laptop GPU"
    );
    OA_CHECK(stats_device_adapter_name("") == "unknown adapter");
}

/// The adapter is read where SDL_RENDER_DRIVER names no driver, unset or
/// empty, and where it names one, known to SDL or not, only when a flag
/// asks for more than SDL's own start.
void test_adapter_read_for() {
    using oa::app::adapter_read_for;
    using oa::platform::render_probe::AdapterRead;
    OA_CHECK(adapter_read_for({}) == AdapterRead::read);
    OA_CHECK(adapter_read_for("") == AdapterRead::read);
    OA_CHECK(adapter_read_for("software") == AdapterRead::skip);
    OA_CHECK(adapter_read_for("opengl") == AdapterRead::skip);
    OA_CHECK(adapter_read_for("bogus") == AdapterRead::skip);
    // --hardware-acceleration or --force-capable reads it under a named
    // driver too, as every other start does.
    OA_CHECK(adapter_read_for("vulkan", true) == AdapterRead::read);
    OA_CHECK(adapter_read_for("opengl", true) == AdapterRead::read);
    OA_CHECK(adapter_read_for("", true) == AdapterRead::read);
    OA_CHECK(adapter_read_for("vulkan", false) == AdapterRead::skip);
}

/// SDL's software renderer on the dummy video driver, reported as the game
/// reports it with SDL_RENDER_DRIVER empty and naming it. Having no
/// adapter, it reads the same either way; test_adapter_read_for shows what
/// the hint decides.
void test_report_software_renderer() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        OA_CHECK(false);
        return;
    }
    constexpr int kWindowWidth = 64;
    constexpr int kWindowHeight = 48;
    SDL_Window* window = SDL_CreateWindow("graphics report test", kWindowWidth, kWindowHeight, 0);
    OA_CHECK(window != nullptr);
    SDL_Renderer* renderer = window != nullptr ? SDL_CreateRenderer(window, "software") : nullptr;
    OA_CHECK(renderer != nullptr);
    if (renderer != nullptr) {
        const std::string expected = std::string("open-annihilation: graphics: software on ") +
                                     SDL_GetCurrentVideoDriver() +
                                     ", textures of any size; standard tier: the processor draws "
                                     "everything";
        OA_CHECK(SDL_SetHint(SDL_HINT_RENDER_DRIVER, ""));
        const AdapterFacts walked = oa::app::report_game_renderer(renderer);
        OA_CHECK(walked.adapter_state == AdapterState::none);
        OA_CHECK(graphics_log_line(walked) == expected);
        OA_CHECK(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
        const AdapterFacts named = oa::app::report_game_renderer(renderer);
        OA_CHECK(named.adapter_state == AdapterState::none);
        OA_CHECK(graphics_log_line(named) == expected);
        SDL_DestroyRenderer(renderer);
    }
    if (window != nullptr)
        SDL_DestroyWindow(window);
    SDL_Quit();
}

} // namespace

/// The start-up line's tier: in use, what the graphics card does at each
/// reach, with why Basic draws where Full was asked for; otherwise the
/// standard tier with the status's reason.
void test_tier_description() {
    using oa::app::tier_description;
    using oa::ui::engine_settings::AccelerationReach;
    using oa::ui::engine_settings::AccelerationState;
    using oa::ui::engine_settings::AccelerationStatus;
    const auto described = [](AccelerationState state, AccelerationReach reach) {
        AccelerationStatus status{};
        status.state = state;
        status.reach = reach;
        return tier_description(status);
    };
    const auto standard = [&](AccelerationState state) {
        return described(state, AccelerationReach::zoomed_out);
    };
    OA_CHECK(
        described(AccelerationState::in_use, AccelerationReach::menus) ==
        "basic tier: the graphics card scales the interface"
    );
    OA_CHECK(
        described(AccelerationState::in_use, AccelerationReach::zoomed_in) ==
        "basic tier: the graphics card scales the interface and the zoomed-in view"
    );
    OA_CHECK(
        described(AccelerationState::in_use, AccelerationReach::zoomed_out) ==
        "basic tier: the graphics card scales the interface and the zoomed-in view, and "
        "the zoomed-out view is smoothed"
    );
    OA_CHECK(
        described(AccelerationState::in_use_no_smoothing, AccelerationReach::nearest_zoomed_out) ==
        "basic tier: the zoomed-out view is smoothed"
    );
    OA_CHECK(
        described(AccelerationState::in_use, AccelerationReach::nearest_none) ==
        "basic tier: the view is drawn as in the standard tier"
    );
    // Where Full stopped or waits, the note says why Basic draws; Full in
    // use is the full tier, as is the frame the flag forces.
    OA_CHECK(
        described(AccelerationState::full_stopped, AccelerationReach::menus) ==
        "basic tier (Full stopped for this run): the graphics card scales the interface"
    );
    OA_CHECK(
        described(AccelerationState::full_too_little_memory, AccelerationReach::menus) ==
        "basic tier (there is too little memory for Full): the graphics card scales the interface"
    );
    OA_CHECK(
        described(AccelerationState::full_cannot_save, AccelerationReach::menus) ==
        "basic tier (Full's trial cannot be written): the graphics card scales the interface"
    );
    OA_CHECK(
        described(AccelerationState::full_failed_before, AccelerationReach::menus) ==
        "basic tier (Full failed before on this driver): the graphics card scales the interface"
    );
    OA_CHECK(
        described(AccelerationState::full_lacks_feature, AccelerationReach::menus) ==
        "basic tier (the graphics card lacks a feature Full needs): the graphics card scales the "
        "interface"
    );
    OA_CHECK(
        described(AccelerationState::full_waiting_for_game_end, AccelerationReach::menus) ==
        "basic tier (Full waits for the game to end): the graphics card scales the interface"
    );
    OA_CHECK(
        described(AccelerationState::full_in_use, AccelerationReach::menus) ==
        "full tier: the graphics card draws the battlefield and scales the interface"
    );
    OA_CHECK(oa::app::full_shortfall_note(AccelerationState::in_use).empty());
    // Off by the setting or a flag gives that as the reason.
    for (const auto state :
         {AccelerationState::off_by_setting,
          AccelerationState::off_by_command_line,
          AccelerationState::off_driver_skipped})
        OA_CHECK(
            standard(state) ==
            "standard tier: the processor draws everything (hardware acceleration is off)"
        );
    OA_CHECK(
        standard(AccelerationState::needs_memory) ==
        "standard tier: the processor draws everything (it needs at least 2 GB of memory)"
    );
    OA_CHECK(
        standard(AccelerationState::environment_driver) ==
        "standard tier: the processor draws everything (the environment names a driver)"
    );
    OA_CHECK(
        standard(AccelerationState::no_usable_card) ==
        "standard tier: the processor draws everything (no usable graphics card was found)"
    );
    OA_CHECK(
        standard(AccelerationState::lacks_feature) ==
        "standard tier: the processor draws everything (the graphics card lacks a feature)"
    );
    OA_CHECK(
        standard(AccelerationState::driver_failed) ==
        "standard tier: the processor draws everything (the graphics driver failed)"
    );
    // The tier goes into the line after the texture limit.
    AccelerationStatus in_use{};
    in_use.state = AccelerationState::in_use;
    in_use.reach = AccelerationReach::zoomed_in;
    OA_CHECK(
        graphics_log_line(
            facts_of("metal", "cocoa", AdapterState::read, "Apple M2", 16384),
            tier_description(in_use)
        ) == "open-annihilation: graphics: metal on cocoa (Apple M2), textures up to 16384; "
             "basic tier: the graphics card scales the interface and the zoomed-in view"
    );
}

/// What the render policy reads of a renderer: the driver's traits, the
/// corrected limit and the adapter's classification.
void test_renderer_facts() {
    using oa::app::renderer_facts;
    using oa::app::render_policy::Capability;
    using oa::app::render_policy::assess_renderer;
    auto software = facts_of("software", "dummy", AdapterState::none, "", 0);
    software.software_rasteriser = true;
    const auto software_facts = renderer_facts(software);
    OA_CHECK(software_facts.driver.software);
    OA_CHECK(software_facts.software_rasteriser);
    OA_CHECK(assess_renderer(software_facts, false, false) == Capability::software_renderer);
    const auto metal =
        renderer_facts(facts_of("metal", "cocoa", AdapterState::read, "Apple M2", 16384));
    OA_CHECK(!metal.driver.software && !metal.driver.adapter_required);
    OA_CHECK(metal.adapter_known && metal.max_texture_size == 16384);
    OA_CHECK(assess_renderer(metal, false, false) == Capability::capable);
    // An unread adapter on vulkan cannot rule out a software rasteriser.
    const auto vulkan = renderer_facts(facts_of("vulkan", "x11", AdapterState::unknown, "", 16384));
    OA_CHECK(vulkan.driver.adapter_required && !vulkan.adapter_known);
    OA_CHECK(vulkan.max_texture_size == 8192);
    OA_CHECK(assess_renderer(vulkan, false, false) == Capability::unknown_adapter);
    // A small limit, a virtual adapter and Wine.
    OA_CHECK(
        assess_renderer(
            renderer_facts(facts_of("opengl", "x11", AdapterState::read, "card", 512)), false, false
        ) == Capability::small_texture_limit
    );
    auto virtual_adapter = facts_of("opengl", "x11", AdapterState::read, "VMware SVGA3D", 8192);
    virtual_adapter.virtual_adapter = true;
    OA_CHECK(
        assess_renderer(renderer_facts(virtual_adapter), false, false) ==
        Capability::virtual_adapter
    );
    auto wine = facts_of("direct3d11", "windows", AdapterState::read, "card", 16384);
    wine.wine = true;
    OA_CHECK(assess_renderer(renderer_facts(wine), false, false) == Capability::under_wine);
    // Before Vista only direct3d may be accelerated: opengl, which SDL
    // reaches when direct3d refuses, is not, nor is direct3d11.
    const auto direct3d =
        renderer_facts(facts_of("direct3d", "windows", AdapterState::read, "card", 4096));
    OA_CHECK(direct3d.driver.capable_before_vista);
    OA_CHECK(assess_renderer(direct3d, true, false) == Capability::capable);
    for (const char* renderer : {"opengl", "direct3d11", "opengles2"}) {
        const auto other =
            renderer_facts(facts_of(renderer, "windows", AdapterState::read, "card", 16384));
        OA_CHECK(!other.driver.capable_before_vista);
        OA_CHECK(assess_renderer(other, true, false) == Capability::before_vista_driver);
        OA_CHECK(assess_renderer(other, false, false) == Capability::capable);
    }
}

int main() {
    test_corrected_texture_limit();
    test_tier_description();
    test_renderer_facts();
    test_log_line();
    test_stats_adapter();
    test_adapter_read_for();
    test_report_software_renderer();
    return oa::test::check_exit_status();
}

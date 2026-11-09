// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The render policy, by table: the corrected texture limit and the
// renderer's capability; the tier over every combination of its inputs,
// checked against the conditions written out again, the standard tier
// under 2 GiB (a machine that reports 1.75 GiB has 2 GiB) whatever the
// flags, and the function test allowed only where the tier could be
// accelerated, so never under 2 GiB; a shared game or a replay starting
// nothing until it ends; the walk of the render drivers, skipping recorded
// ones but never software, with a framebuffer hint that is never empty, a
// second walk with the records ignored whenever they would leave nothing
// able to present, and SDL's own call under SDL_RENDER_DRIVER; present
// stalls; device resets; the layers' texture formats; the starting budget,
// the same at any memory, and the starting rung of every kind of machine,
// with the blend only above 4 GiB; the remembered rung and the step-down
// fed synthetic frames, the rate it holds frames to and the rules by which
// the game feeds it its presented frames; the rung the tier stays on when
// the memory guard refuses a buffer, and the words each step is logged
// with; the window's density over every input, never native under 2 GiB
// and, as the game fills it today, only on a platform at native density,
// for --native-density and for the Native pixel density setting; the
// chrome's filter, at the display's scale on a window at native density,
// and the magnified scene's; how each Menu scaling way holds a 640x480
// frame and the filter it draws it with; the prescale budget; the Full
// tier's supersample factor by the Enhanced anti-aliasing level, its
// budget S by the machine and the factor fitted to the budget and the
// texture limit, each world target's memory printed; the tiles of textures beyond the renderer's limit; and acting on the tier as
// the game does: the windowless video drivers, the flags, what the host
// does for each decision, what Off then On forgets, one frame's step with
// its function test and its note of a shared game, the function test's
// state at start, and a player's run through the setting, a shared game
// and a failed function test, beside the runs that stay on the standard
// tier. The driver names here are made
// up: the policy reads them only as names.
#include "oa/app/render_policy.hpp"

#include "oa/test/check.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace oa::app::render_policy;

// ---------------------------------------------------------------------------
// The renderer's facts and its capability

void test_texture_limit() {
    struct Case {
        TextureLimitSource source;
        uint32_t reported;
        uint32_t device;
        uint32_t expected;
    };

    const Case cases[] = {
        {TextureLimitSource::reported, 16384, 0, 16384},
        {TextureLimitSource::reported, 0, 0, 0},
        {TextureLimitSource::reported, 2048, 4096, 2048},
        // A fixed report: the device's own limit when known, never above
        // the report; otherwise the report, at most 8192.
        {TextureLimitSource::fixed_report, 16384, 8192, 8192},
        {TextureLimitSource::fixed_report, 16384, 32768, 16384},
        {TextureLimitSource::fixed_report, 16384, 16384, 16384},
        {TextureLimitSource::fixed_report, 16384, 2048, 2048},
        {TextureLimitSource::fixed_report, 16384, 0, 8192},
        {TextureLimitSource::fixed_report, 4096, 0, 4096},
        {TextureLimitSource::fixed_report, 0, 4096, 4096},
        {TextureLimitSource::fixed_report, 0, 0, 8192},
    };
    for (const Case& test : cases) {
        DriverTraits driver;
        driver.texture_limit_source = test.source;
        OA_CHECK(texture_limit(driver, test.reported, test.device) == test.expected);
    }
}

/// A renderer that items 1 to 3 accept.
RendererFacts capable_renderer() {
    RendererFacts renderer;
    renderer.driver.adapter_required = true;
    renderer.max_texture_size = 16384;
    renderer.adapter_known = true;
    return renderer;
}

void test_assess_renderer() {
    struct Case {
        const char* name;
        void (*change)(RendererFacts&);
        bool legacy_windows;
        bool accept_virtual;
        Capability expected;
    };

    const Case cases[] = {
        {"capable", [](RendererFacts&) {}, false, false, Capability::capable},
        {"software",
         [](RendererFacts& r) { r.driver.software = true; },
         false,
         false,
         Capability::software_renderer},
        {"software before everything",
         [](RendererFacts& r) {
             r.driver.software = true;
             r.under_wine = true;
             r.max_texture_size = 512;
         },
         true,
         true,
         Capability::software_renderer},
        {"before vista, other driver",
         [](RendererFacts&) {},
         true,
         false,
         Capability::before_vista_driver},
        {"before vista, allowed driver",
         [](RendererFacts& r) { r.driver.capable_before_vista = true; },
         true,
         false,
         Capability::capable},
        {"allowed driver after vista",
         [](RendererFacts& r) { r.driver.capable_before_vista = true; },
         false,
         false,
         Capability::capable},
        {"texture limit 512",
         [](RendererFacts& r) { r.max_texture_size = 512; },
         false,
         false,
         Capability::small_texture_limit},
        {"texture limit 1023",
         [](RendererFacts& r) { r.max_texture_size = 1023; },
         false,
         false,
         Capability::small_texture_limit},
        {"texture limit 1024",
         [](RendererFacts& r) { r.max_texture_size = 1024; },
         false,
         false,
         Capability::capable},
        {"no texture limit",
         [](RendererFacts& r) { r.max_texture_size = 0; },
         false,
         false,
         Capability::capable},
        {"wine",
         [](RendererFacts& r) { r.under_wine = true; },
         false,
         true,
         Capability::under_wine},
        {"wine before rasteriser",
         [](RendererFacts& r) {
             r.under_wine = true;
             r.software_rasteriser = true;
         },
         false,
         false,
         Capability::under_wine},
        {"software rasteriser",
         [](RendererFacts& r) { r.software_rasteriser = true; },
         false,
         false,
         Capability::software_rasteriser},
        {"software rasteriser, virtual accepted",
         [](RendererFacts& r) { r.software_rasteriser = true; },
         false,
         true,
         Capability::software_rasteriser},
        {"virtual adapter",
         [](RendererFacts& r) { r.virtual_adapter = true; },
         false,
         false,
         Capability::virtual_adapter},
        {"virtual adapter accepted",
         [](RendererFacts& r) { r.virtual_adapter = true; },
         false,
         true,
         Capability::capable},
        {"unknown adapter where needed",
         [](RendererFacts& r) { r.adapter_known = false; },
         false,
         false,
         Capability::unknown_adapter},
        {"unknown adapter elsewhere",
         [](RendererFacts& r) {
             r.adapter_known = false;
             r.driver.adapter_required = false;
         },
         false,
         false,
         Capability::capable},
    };
    for (const Case& test : cases) {
        RendererFacts renderer = capable_renderer();
        test.change(renderer);
        const Capability capability =
            assess_renderer(renderer, test.legacy_windows, test.accept_virtual);
        if (capability != test.expected)
            std::fprintf(stderr, "assess_renderer case: %s\n", test.name);
        OA_CHECK(capability == test.expected);
    }
}

// ---------------------------------------------------------------------------
// Choosing the tier

constexpr uint64_t mebibyte = uint64_t{1} << 20;

/// A run in which every condition for the accelerated tier holds.
TierInputs accelerated_run() {
    TierInputs inputs;
    inputs.renderer = true;
    inputs.memory = 8 * gibibyte;
    inputs.players_own_profile = true;
    inputs.setting = HardwareAcceleration::basic;
    inputs.capability = Capability::capable;
    inputs.function_test = FunctionTest::passed;
    return inputs;
}

void test_decide_by_table() {
    struct Case {
        const char* name;
        void (*change)(TierInputs&);
        RenderTier tier;
        TierReason reason;
    };

    const Case cases[] = {
        {"accelerated", [](TierInputs&) {}, RenderTier::accelerated, TierReason::accelerated},
        {"headless",
         [](TierInputs& i) { i.renderer = false; },
         RenderTier::standard,
         TierReason::no_renderer},
        {"director",
         [](TierInputs& i) { i.director_frame = true; },
         RenderTier::standard,
         TierReason::director_frame},
        // Under 2 GiB, and with memory not reported, the standard tier
        // whatever the flags; a machine that reports 1.75 GiB has 2 GiB.
        {"memory not reported",
         [](TierInputs& i) { i.memory = 0; },
         RenderTier::standard,
         TierReason::memory},
        {"256 MiB",
         [](TierInputs& i) { i.memory = 256 * mebibyte; },
         RenderTier::standard,
         TierReason::memory},
        {"1 GiB",
         [](TierInputs& i) { i.memory = gibibyte; },
         RenderTier::standard,
         TierReason::memory},
        {"just under 1.75 GiB",
         [](TierInputs& i) { i.memory = 1792 * mebibyte - 1; },
         RenderTier::standard,
         TierReason::memory},
        {"1.75 GiB counts as 2",
         [](TierInputs& i) { i.memory = 1792 * mebibyte; },
         RenderTier::accelerated,
         TierReason::accelerated},
        {"2 GiB",
         [](TierInputs& i) { i.memory = 2 * gibibyte; },
         RenderTier::accelerated,
         TierReason::accelerated},
        {"4 GiB",
         [](TierInputs& i) { i.memory = 4 * gibibyte; },
         RenderTier::accelerated,
         TierReason::accelerated},
        {"under 2 GiB, flag on",
         [](TierInputs& i) {
             i.memory = gibibyte;
             i.flag = AccelerationFlag::full;
         },
         RenderTier::standard,
         TierReason::memory},
        {"under 2 GiB, forced",
         [](TierInputs& i) {
             i.memory = gibibyte;
             i.force_capable = true;
         },
         RenderTier::standard,
         TierReason::memory},
        {"memory not reported, flag on and forced",
         [](TierInputs& i) {
             i.memory = 0;
             i.flag = AccelerationFlag::full;
             i.force_capable = true;
         },
         RenderTier::standard,
         TierReason::memory},
        {"under 2 GiB, flag off",
         [](TierInputs& i) {
             i.memory = gibibyte;
             i.flag = AccelerationFlag::off;
         },
         RenderTier::standard,
         TierReason::memory},
        {"under 2 GiB, headless",
         [](TierInputs& i) {
             i.memory = gibibyte;
             i.renderer = false;
         },
         RenderTier::standard,
         TierReason::no_renderer},
        {"under 2 GiB, director",
         [](TierInputs& i) {
             i.memory = 0;
             i.director_frame = true;
         },
         RenderTier::standard,
         TierReason::director_frame},
        {"flag off",
         [](TierInputs& i) { i.flag = AccelerationFlag::off; },
         RenderTier::standard,
         TierReason::flag_off},
        {"flag off beats force",
         [](TierInputs& i) {
             i.flag = AccelerationFlag::off;
             i.force_capable = true;
         },
         RenderTier::standard,
         TierReason::flag_off},
        {"environment",
         [](TierInputs& i) { i.render_driver_named = true; },
         RenderTier::standard,
         TierReason::environment},
        {"environment, flag on",
         [](TierInputs& i) {
             i.render_driver_named = true;
             i.flag = AccelerationFlag::full;
         },
         RenderTier::full,
         TierReason::accelerated},
        {"environment, forced",
         [](TierInputs& i) {
             i.render_driver_named = true;
             i.force_capable = true;
         },
         RenderTier::accelerated,
         TierReason::accelerated},
        {"dummy video",
         [](TierInputs& i) { i.virtual_video_driver = true; },
         RenderTier::standard,
         TierReason::environment},
        {"dummy video, flag on",
         [](TierInputs& i) {
             i.virtual_video_driver = true;
             i.flag = AccelerationFlag::full;
         },
         RenderTier::full,
         TierReason::accelerated},
        {"setting off",
         [](TierInputs& i) { i.setting = HardwareAcceleration::off; },
         RenderTier::standard,
         TierReason::setting_off},
        {"setting full",
         [](TierInputs& i) { i.setting = HardwareAcceleration::full; },
         RenderTier::full,
         TierReason::accelerated},
        {"setting off, flag on",
         [](TierInputs& i) {
             i.setting = HardwareAcceleration::off;
             i.flag = AccelerationFlag::full;
         },
         RenderTier::full,
         TierReason::accelerated},
        {"setting off, flag basic",
         [](TierInputs& i) {
             i.setting = HardwareAcceleration::off;
             i.flag = AccelerationFlag::basic;
         },
         RenderTier::accelerated,
         TierReason::accelerated},
        {"setting full, flag off",
         [](TierInputs& i) {
             i.setting = HardwareAcceleration::full;
             i.flag = AccelerationFlag::off;
         },
         RenderTier::standard,
         TierReason::flag_off},
        {"environment, flag basic",
         [](TierInputs& i) {
             i.render_driver_named = true;
             i.flag = AccelerationFlag::basic;
         },
         RenderTier::accelerated,
         TierReason::accelerated},
        {"setting off, forced",
         [](TierInputs& i) {
             i.setting = HardwareAcceleration::off;
             i.force_capable = true;
         },
         RenderTier::standard,
         TierReason::setting_off},
        {"not capable",
         [](TierInputs& i) { i.capability = Capability::software_renderer; },
         RenderTier::standard,
         TierReason::not_capable},
        {"not capable, flag on",
         [](TierInputs& i) {
             i.capability = Capability::software_rasteriser;
             i.flag = AccelerationFlag::full;
         },
         RenderTier::standard,
         TierReason::not_capable},
        {"not capable, forced",
         [](TierInputs& i) {
             i.capability = Capability::software_renderer;
             i.force_capable = true;
         },
         RenderTier::accelerated,
         TierReason::accelerated},
        {"test failed",
         [](TierInputs& i) { i.function_test = FunctionTest::failed; },
         RenderTier::standard,
         TierReason::function_test_failed},
        {"test failed, forced",
         [](TierInputs& i) {
             i.function_test = FunctionTest::failed;
             i.force_capable = true;
         },
         RenderTier::standard,
         TierReason::function_test_failed},
        {"test not run",
         [](TierInputs& i) { i.function_test = FunctionTest::not_run; },
         RenderTier::standard,
         TierReason::function_test_due},
        {"trial unwritten",
         [](TierInputs& i) { i.function_test = FunctionTest::trial_unwritten; },
         RenderTier::standard,
         TierReason::trial_unwritten},
        {"trial unwritten, named file",
         [](TierInputs& i) {
             i.function_test = FunctionTest::trial_unwritten;
             i.players_own_profile = false;
         },
         RenderTier::standard,
         TierReason::function_test_due},
        {"trial unwritten, environment",
         [](TierInputs& i) {
             i.function_test = FunctionTest::trial_unwritten;
             i.render_driver_named = true;
             i.flag = AccelerationFlag::full;
         },
         RenderTier::standard,
         TierReason::function_test_due},
        {"records unreadable",
         [](TierInputs& i) { i.records_unreadable_after_unclean_start = true; },
         RenderTier::standard,
         TierReason::records_unreadable},
        {"records unreadable, named file",
         [](TierInputs& i) {
             i.records_unreadable_after_unclean_start = true;
             i.players_own_profile = false;
         },
         RenderTier::accelerated,
         TierReason::accelerated},
        {"accelerated-unusable",
         [](TierInputs& i) { i.accelerated_unusable_record = true; },
         RenderTier::standard,
         TierReason::accelerated_unusable},
        {"accelerated-unusable, flag on",
         [](TierInputs& i) {
             i.accelerated_unusable_record = true;
             i.flag = AccelerationFlag::full;
         },
         RenderTier::full,
         TierReason::accelerated},
        {"accelerated-unusable, forced",
         [](TierInputs& i) {
             i.accelerated_unusable_record = true;
             i.force_capable = true;
         },
         RenderTier::standard,
         TierReason::accelerated_unusable},
        {"dropped: driver",
         [](TierInputs& i) { i.drop = Drop::driver_failure; },
         RenderTier::standard,
         TierReason::dropped},
        {"dropped: engine fault",
         [](TierInputs& i) { i.drop = Drop::engine_fault; },
         RenderTier::standard,
         TierReason::dropped},
        {"dropped: memory",
         [](TierInputs& i) { i.drop = Drop::memory; },
         RenderTier::standard,
         TierReason::dropped},
        {"dropped: stall",
         [](TierInputs& i) { i.drop = Drop::stall; },
         RenderTier::standard,
         TierReason::dropped},
        {"dropped: path trial",
         [](TierInputs& i) { i.drop = Drop::path_trial_unwritten; },
         RenderTier::standard,
         TierReason::dropped},
        {"dropped, flag on",
         [](TierInputs& i) {
             i.drop = Drop::memory;
             i.flag = AccelerationFlag::full;
         },
         RenderTier::standard,
         TierReason::dropped},
        {"device lost",
         [](TierInputs& i) { i.device_lost = true; },
         RenderTier::standard,
         TierReason::device_lost},
        {"shared game begun accelerated",
         [](TierInputs& i) { i.match = SharedMatchGate{MatchKind::shared_game, true}; },
         RenderTier::accelerated,
         TierReason::accelerated},
        {"shared game begun standard",
         [](TierInputs& i) { i.match = SharedMatchGate{MatchKind::shared_game, false}; },
         RenderTier::standard,
         TierReason::waiting_for_match_end},
        {"replay begun standard",
         [](TierInputs& i) { i.match = SharedMatchGate{MatchKind::replay, false}; },
         RenderTier::standard,
         TierReason::waiting_for_match_end},
        {"shared game, test not run",
         [](TierInputs& i) {
             i.match = SharedMatchGate{MatchKind::shared_game, false};
             i.function_test = FunctionTest::not_run;
         },
         RenderTier::standard,
         TierReason::waiting_for_match_end},
    };
    for (const Case& test : cases) {
        TierInputs inputs = accelerated_run();
        test.change(inputs);
        const TierDecision decision = decide_render_tier(inputs);
        if (decision.tier != test.tier || decision.reason != test.reason)
            std::fprintf(stderr, "decide_render_tier case: %s\n", test.name);
        OA_CHECK(decision.tier == test.tier);
        OA_CHECK(decision.reason == test.reason);
    }
}

/// Full: asked for by the flag or by the setting it is the Full tier;
/// dropped for the run it is Basic until Off and back lifts the drop; and
/// every condition of the accelerated tier still applies to it.
void test_decide_full() {
    struct Case {
        const char* name;
        void (*change)(TierInputs&);
        RenderTier tier;
        FullReason full;
    };

    const Case cases[] = {
        {"basic asked", [](TierInputs&) {}, RenderTier::accelerated, FullReason::not_asked},
        {"full by the setting",
         [](TierInputs& i) { i.setting = HardwareAcceleration::full; },
         RenderTier::full,
         FullReason::full},
        {"full by the flag",
         [](TierInputs& i) { i.flag = AccelerationFlag::full; },
         RenderTier::full,
         FullReason::full},
        {"full by the flag over a setting of basic",
         [](TierInputs& i) {
             i.flag = AccelerationFlag::full;
             i.setting = HardwareAcceleration::basic;
         },
         RenderTier::full,
         FullReason::full},
        {"basic by the flag over a setting of full",
         [](TierInputs& i) {
             i.flag = AccelerationFlag::basic;
             i.setting = HardwareAcceleration::full;
         },
         RenderTier::accelerated,
         FullReason::not_asked},
        {"full dropped for the run",
         [](TierInputs& i) {
             i.flag = AccelerationFlag::full;
             i.full_drop = FullDrop::card_failure;
         },
         RenderTier::accelerated,
         FullReason::dropped},
        {"full dropped by its function test",
         [](TierInputs& i) {
             i.flag = AccelerationFlag::full;
             i.full_drop = FullDrop::function_test;
         },
         RenderTier::accelerated,
         FullReason::dropped},
        {"full dropped by the memory guard",
         [](TierInputs& i) {
             i.flag = AccelerationFlag::full;
             i.full_drop = FullDrop::memory;
         },
         RenderTier::accelerated,
         FullReason::dropped},
        {"full's trial unwritten",
         [](TierInputs& i) {
             i.flag = AccelerationFlag::full;
             i.full_drop = FullDrop::trial_unwritten;
         },
         RenderTier::accelerated,
         FullReason::dropped},
        {"full-unusable record",
         [](TierInputs& i) {
             i.setting = HardwareAcceleration::full;
             i.full_unusable_record = true;
         },
         RenderTier::accelerated,
         FullReason::unusable_record},
        {"full-unusable record, flag full",
         [](TierInputs& i) {
             i.flag = AccelerationFlag::full;
             i.full_unusable_record = true;
         },
         RenderTier::full,
         FullReason::full},
        {"full-unusable record, flag basic",
         [](TierInputs& i) {
             i.flag = AccelerationFlag::basic;
             i.full_unusable_record = true;
         },
         RenderTier::accelerated,
         FullReason::not_asked},
        {"a shared game begun in basic",
         [](TierInputs& i) {
             i.flag = AccelerationFlag::full;
             i.match = SharedMatchGate{MatchKind::shared_game, true, false};
         },
         RenderTier::accelerated,
         FullReason::waiting_for_match_end},
        {"a replay begun in full",
         [](TierInputs& i) {
             i.flag = AccelerationFlag::full;
             i.match = SharedMatchGate{MatchKind::replay, true, true};
         },
         RenderTier::full,
         FullReason::full},
        {"a shared game begun in full, then a record ignored by the flag",
         [](TierInputs& i) {
             i.flag = AccelerationFlag::full;
             i.full_unusable_record = true;
             i.match = SharedMatchGate{MatchKind::shared_game, true, true};
         },
         RenderTier::full,
         FullReason::full},
        {"a record before a drop before the wait",
         [](TierInputs& i) {
             i.flag = AccelerationFlag::full;
             i.full_drop = FullDrop::card_failure;
             i.match = SharedMatchGate{MatchKind::shared_game, true, false};
         },
         RenderTier::accelerated,
         FullReason::dropped},
        {"full forced in a virtual video driver",
         [](TierInputs& i) {
             i.flag = AccelerationFlag::full;
             i.virtual_video_driver = true;
         },
         RenderTier::full,
         FullReason::full},
        {"full forced over an accelerated-unusable record",
         [](TierInputs& i) {
             i.flag = AccelerationFlag::full;
             i.accelerated_unusable_record = true;
         },
         RenderTier::full,
         FullReason::full},
    };
    for (const Case& test : cases) {
        TierInputs inputs = accelerated_run();
        test.change(inputs);
        const TierDecision decision = decide_render_tier(inputs);
        if (decision.tier != test.tier || decision.full != test.full)
            std::fprintf(stderr, "decide_render_tier full case: %s\n", test.name);
        OA_CHECK(decision.tier == test.tier);
        OA_CHECK(decision.full == test.full);
        OA_CHECK(decision.reason == TierReason::accelerated);
        OA_CHECK(card_tier(decision.tier));
    }
    // The standard tier's conditions stand before Full's: under 2 GiB, with
    // a drop, with the device lost, the flag forces nothing.
    for (void (*change)(TierInputs&) :
         {+[](TierInputs& i) { i.memory = gibibyte; },
          +[](TierInputs& i) { i.drop = Drop::memory; },
          +[](TierInputs& i) { i.device_lost = true; },
          +[](TierInputs& i) { i.function_test = FunctionTest::failed; },
          +[](TierInputs& i) { i.renderer = false; }}) {
        TierInputs inputs = accelerated_run();
        inputs.flag = AccelerationFlag::full;
        change(inputs);
        const TierDecision decision = decide_render_tier(inputs);
        OA_CHECK(decision.tier == RenderTier::standard);
        OA_CHECK(decision.full == FullReason::not_asked);
        OA_CHECK(!card_tier(decision.tier));
    }
    // Off and back lifts Full's drop with the others, but not the memory
    // guard's.
    for (const FullDrop drop :
         {FullDrop::card_failure, FullDrop::function_test, FullDrop::trial_unwritten}) {
        TierInputs dropped = accelerated_run();
        dropped.flag = AccelerationFlag::full;
        dropped.full_drop = drop;
        OA_CHECK(decide_render_tier(dropped).tier == RenderTier::accelerated);
        forget_failures(dropped);
        OA_CHECK(dropped.full_drop == FullDrop::none);
        OA_CHECK(decide_render_tier(dropped).tier == RenderTier::full);
    }
    TierInputs short_of_memory = accelerated_run();
    short_of_memory.flag = AccelerationFlag::full;
    short_of_memory.full_drop = FullDrop::memory;
    forget_failures(short_of_memory);
    OA_CHECK(short_of_memory.full_drop == FullDrop::memory);
    OA_CHECK(decide_render_tier(short_of_memory).full == FullReason::dropped);
    // The shared game's gate: begun in Full it stays Full until a frame is
    // drawn below Full, other than for a lost device, or something stops
    // the tier; begun in Basic, Full waits for the match to end; a frame
    // below Basic stops both.
    TierInputs shared = accelerated_run();
    shared.flag = AccelerationFlag::full;
    begin_match(shared.match, MatchKind::shared_game, true, true);
    OA_CHECK(shared.match.accelerated && shared.match.full);
    OA_CHECK(decide_render_tier(shared).tier == RenderTier::full);
    shared.device_lost = true;
    note_match_frame(shared.match, decide_render_tier(shared).tier, true);
    shared.device_lost = false;
    OA_CHECK(shared.match.full && decide_render_tier(shared).tier == RenderTier::full);
    shared.flag = AccelerationFlag::basic;
    note_match_frame(shared.match, decide_render_tier(shared).tier, false);
    OA_CHECK(shared.match.accelerated && !shared.match.full);
    shared.flag = AccelerationFlag::full;
    OA_CHECK(decide_render_tier(shared).tier == RenderTier::accelerated);
    OA_CHECK(decide_render_tier(shared).full == FullReason::waiting_for_match_end);
    shared.flag = AccelerationFlag::off;
    note_match_frame(shared.match, decide_render_tier(shared).tier, false);
    OA_CHECK(!shared.match.accelerated && !shared.match.full);
    end_match(shared.match);
    shared.flag = AccelerationFlag::full;
    OA_CHECK(decide_render_tier(shared).tier == RenderTier::full);
    begin_match(shared.match, MatchKind::replay, true, false);
    OA_CHECK(shared.match.accelerated && !shared.match.full);
    stop_until_match_end(shared.match);
    OA_CHECK(!shared.match.accelerated && !shared.match.full);
    end_match(shared.match);
    // A match played alone gates nothing; Full is never begun without Basic.
    begin_match(shared.match, MatchKind::none, true, true);
    OA_CHECK(!shared.match.accelerated && !shared.match.full);
    begin_match(shared.match, MatchKind::shared_game, false, true);
    OA_CHECK(!shared.match.accelerated && !shared.match.full);
    end_match(shared.match);
    // Full switches the presentation on as Basic does, and off again.
    const TierDecision full{RenderTier::full, TierReason::accelerated, FullReason::full};
    OA_CHECK(tier_action(full, false) == TierAction::switch_on);
    OA_CHECK(tier_action(full, true) == TierAction::none);
    OA_CHECK(card_tier(RenderTier::full) && card_tier(RenderTier::accelerated));
    OA_CHECK(!card_tier(RenderTier::standard));
}

/// The conditions for the accelerated tier, written out again apart from
/// the code under test. Basic and Full both ask for it: the game draws
/// Full as Basic until the battlefield is drawn on the graphics card.
///
/// @param in the inputs
/// @param function_test the function test's state to judge them with
/// @return true when every condition holds
bool accelerated_by_the_rules(const TierInputs& in, FunctionTest function_test) {
    const bool flag_on = in.flag == AccelerationFlag::basic || in.flag == AccelerationFlag::full;
    const bool on_disk = in.players_own_profile && !in.render_driver_named;
    return in.renderer && !in.director_frame && in.memory >= 1792 * mebibyte &&
           in.flag != AccelerationFlag::off &&
           (flag_on || in.force_capable || (!in.render_driver_named && !in.virtual_video_driver)) &&
           (in.setting != HardwareAcceleration::off || flag_on) &&
           (in.capability == Capability::capable || in.force_capable) &&
           function_test == FunctionTest::passed &&
           !(in.records_unreadable_after_unclean_start && on_disk) &&
           !(in.accelerated_unusable_record && !flag_on) && in.drop == Drop::none &&
           !in.device_lost && !(in.match.kind != MatchKind::none && !in.match.accelerated);
}

void test_decide_every_combination() {
    const AccelerationFlag flags[] = {
        AccelerationFlag::none,
        AccelerationFlag::off,
        AccelerationFlag::basic,
        AccelerationFlag::full
    };
    const HardwareAcceleration settings[] = {
        HardwareAcceleration::off, HardwareAcceleration::basic, HardwareAcceleration::full
    };
    const FunctionTest tests[] = {
        FunctionTest::not_run,
        FunctionTest::passed,
        FunctionTest::failed,
        FunctionTest::trial_unwritten
    };
    const SharedMatchGate gates[] = {
        SharedMatchGate{},
        SharedMatchGate{MatchKind::shared_game, true},
        SharedMatchGate{MatchKind::shared_game, true, true},
        SharedMatchGate{MatchKind::replay, false}
    };
    const uint64_t memories[] = {0, 1792 * mebibyte - 1, 1792 * mebibyte, 16 * gibibyte};
    uint32_t combinations = 0;
    uint32_t accelerated = 0;
    uint32_t tests_allowed_under_2_gib = 0;
    uint32_t mismatches = 0;
    for (uint32_t bits = 0; bits < (1u << 11); ++bits) {
        for (const AccelerationFlag flag : flags) {
            for (const HardwareAcceleration setting : settings) {
                for (const FunctionTest test : tests) {
                    for (const SharedMatchGate& gate : gates) {
                        for (const uint64_t memory : memories) {
                            TierInputs in;
                            in.memory = memory;
                            in.renderer = (bits & 1u) != 0;
                            in.director_frame = (bits & 2u) != 0;
                            in.force_capable = (bits & 4u) != 0;
                            in.render_driver_named = (bits & 8u) != 0;
                            in.virtual_video_driver = (bits & 16u) != 0;
                            in.players_own_profile = (bits & 32u) != 0;
                            in.setting = setting;
                            in.capability = (bits & 64u) != 0 ? Capability::capable
                                                              : Capability::software_renderer;
                            in.accelerated_unusable_record = (bits & 128u) != 0;
                            in.records_unreadable_after_unclean_start = (bits & 256u) != 0;
                            in.drop = (bits & 512u) != 0 ? Drop::memory : Drop::none;
                            in.device_lost = (bits & 1024u) != 0;
                            in.flag = flag;
                            in.function_test = test;
                            in.match = gate;
                            ++combinations;
                            const TierDecision decision = decide_render_tier(in);
                            const bool expected = accelerated_by_the_rules(in, test);
                            if (card_tier(decision.tier) != expected)
                                ++mismatches;
                            if ((decision.reason == TierReason::accelerated) !=
                                card_tier(decision.tier))
                                ++mismatches;
                            // Of the frames the card presents, Full is exactly
                            // those Full was asked for, by the flag or by the
                            // setting under no flag, outside a shared game or
                            // a replay the tier did not begin as Full.
                            const bool full_expected = expected &&
                                                       (flag == AccelerationFlag::full ||
                                                        (flag == AccelerationFlag::none &&
                                                         setting == HardwareAcceleration::full)) &&
                                                       (gate.kind == MatchKind::none || gate.full);
                            if ((decision.tier == RenderTier::full) != full_expected)
                                ++mismatches;
                            // The function test may run exactly where the tier would
                            // be accelerated once it passed, and only when it has not
                            // run, or skipped a trial that lives in memory and so
                            // cannot fail to be written.
                            const bool untested =
                                test == FunctionTest::not_run ||
                                (test == FunctionTest::trial_unwritten &&
                                 !records_on_disk(in.players_own_profile, in.render_driver_named));
                            if (function_test_may_run(in) !=
                                (untested && accelerated_by_the_rules(in, FunctionTest::passed)))
                                ++mismatches;
                            // Under 2 GiB, or with memory not reported, the tier is
                            // standard for the memory alone, and the function test
                            // never runs.
                            if (memory < 1792 * mebibyte && in.renderer && !in.director_frame &&
                                decide_render_tier(in).reason != TierReason::memory)
                                ++mismatches;
                            if (memory < 1792 * mebibyte && function_test_may_run(in))
                                ++tests_allowed_under_2_gib;
                            if (expected)
                                ++accelerated;
                        }
                    }
                }
            }
        }
    }
    OA_CHECK(combinations == (1u << 11) * 4 * 3 * 4 * 4 * 4);
    OA_CHECK(mismatches == 0);
    OA_CHECK(tests_allowed_under_2_gib == 0);
    OA_CHECK(accelerated > 0);
    OA_CHECK(smallest_accelerated_memory == 1792 * mebibyte);
}

void test_function_test_gate() {
    struct Case {
        const char* name;
        void (*change)(TierInputs&);
        bool may_run;
    };

    const Case cases[] = {
        {"capable, setting on", [](TierInputs&) {}, true},
        {"software renderer",
         [](TierInputs& i) { i.capability = Capability::software_renderer; },
         false},
        {"software renderer, forced",
         [](TierInputs& i) {
             i.capability = Capability::software_renderer;
             i.force_capable = true;
         },
         true},
        {"software rasteriser, flag on",
         [](TierInputs& i) {
             i.capability = Capability::software_rasteriser;
             i.flag = AccelerationFlag::full;
         },
         false},
        {"virtual adapter",
         [](TierInputs& i) { i.capability = Capability::virtual_adapter; },
         false},
        {"wine", [](TierInputs& i) { i.capability = Capability::under_wine; }, false},
        {"before vista",
         [](TierInputs& i) { i.capability = Capability::before_vista_driver; },
         false},
        {"small texture limit",
         [](TierInputs& i) { i.capability = Capability::small_texture_limit; },
         false},
        {"flag off", [](TierInputs& i) { i.flag = AccelerationFlag::off; }, false},
        {"environment", [](TierInputs& i) { i.render_driver_named = true; }, false},
        {"environment, flag on",
         [](TierInputs& i) {
             i.render_driver_named = true;
             i.flag = AccelerationFlag::full;
         },
         true},
        {"dummy video", [](TierInputs& i) { i.virtual_video_driver = true; }, false},
        {"setting off", [](TierInputs& i) { i.setting = HardwareAcceleration::off; }, false},
        {"setting full", [](TierInputs& i) { i.setting = HardwareAcceleration::full; }, true},
        {"setting off, named file",
         [](TierInputs& i) {
             i.setting = HardwareAcceleration::off;
             i.players_own_profile = false;
         },
         false},
        {"setting on, named file", [](TierInputs& i) { i.players_own_profile = false; }, true},
        {"accelerated-unusable",
         [](TierInputs& i) { i.accelerated_unusable_record = true; },
         false},
        {"accelerated-unusable, flag on",
         [](TierInputs& i) {
             i.accelerated_unusable_record = true;
             i.flag = AccelerationFlag::full;
         },
         true},
        {"shared game",
         [](TierInputs& i) { i.match = SharedMatchGate{MatchKind::shared_game, false}; },
         false},
        {"replay",
         [](TierInputs& i) { i.match = SharedMatchGate{MatchKind::replay, false}; },
         false},
        {"headless", [](TierInputs& i) { i.renderer = false; }, false},
        {"dropped", [](TierInputs& i) { i.drop = Drop::driver_failure; }, false},
        {"device lost", [](TierInputs& i) { i.device_lost = true; }, false},
        // A test that failed, or whose trial could not be written on disk,
        // does not run again until a retry sets it back to not run.
        {"test already failed",
         [](TierInputs& i) { i.function_test = FunctionTest::failed; },
         false},
        {"trial unwritten",
         [](TierInputs& i) { i.function_test = FunctionTest::trial_unwritten; },
         false},
        {"trial unwritten, named file",
         [](TierInputs& i) {
             i.function_test = FunctionTest::trial_unwritten;
             i.players_own_profile = false;
         },
         true},
        {"test passed", [](TierInputs& i) { i.function_test = FunctionTest::passed; }, false},
        // Never under 2 GiB, nor with memory not reported, whatever the
        // flags; a machine that reports 1.75 GiB has 2 GiB.
        {"under 2 GiB", [](TierInputs& i) { i.memory = gibibyte; }, false},
        {"memory not reported", [](TierInputs& i) { i.memory = 0; }, false},
        {"just under 1.75 GiB", [](TierInputs& i) { i.memory = 1792 * mebibyte - 1; }, false},
        {"1.75 GiB", [](TierInputs& i) { i.memory = 1792 * mebibyte; }, true},
        {"under 2 GiB, flag on",
         [](TierInputs& i) {
             i.memory = gibibyte;
             i.flag = AccelerationFlag::full;
         },
         false},
        {"under 2 GiB, forced",
         [](TierInputs& i) {
             i.memory = gibibyte;
             i.capability = Capability::software_renderer;
             i.force_capable = true;
         },
         false},
        {"memory not reported, flag on and forced",
         [](TierInputs& i) {
             i.memory = 0;
             i.render_driver_named = true;
             i.flag = AccelerationFlag::full;
             i.force_capable = true;
         },
         false},
    };
    for (const Case& test : cases) {
        TierInputs inputs = accelerated_run();
        inputs.function_test = FunctionTest::not_run;
        test.change(inputs);
        if (function_test_may_run(inputs) != test.may_run)
            std::fprintf(stderr, "function_test_may_run case: %s\n", test.name);
        OA_CHECK(function_test_may_run(inputs) == test.may_run);
    }
    OA_CHECK(records_on_disk(true, false));
    OA_CHECK(!records_on_disk(false, false));
    OA_CHECK(!records_on_disk(true, true));
}

void test_shared_match_gate() {
    // Accelerated as the loading screen begins: it stays so until something
    // leaves the tier, and then waits for the match to end.
    TierInputs inputs = accelerated_run();
    begin_match(
        inputs.match,
        MatchKind::shared_game,
        decide_render_tier(inputs).tier == RenderTier::accelerated
    );
    OA_CHECK(inputs.match.kind == MatchKind::shared_game);
    OA_CHECK(decide_render_tier(inputs).tier == RenderTier::accelerated);
    inputs.setting = HardwareAcceleration::off;
    TierDecision decision = decide_render_tier(inputs);
    OA_CHECK(decision.reason == TierReason::setting_off);
    note_match_frame(inputs.match, decision.tier, false);
    inputs.setting = HardwareAcceleration::basic;
    decision = decide_render_tier(inputs);
    OA_CHECK(decision.tier == RenderTier::standard);
    OA_CHECK(decision.reason == TierReason::waiting_for_match_end);
    end_match(inputs.match);
    OA_CHECK(decide_render_tier(inputs).tier == RenderTier::accelerated);

    // A lost device that a reset brings back does not stop it.
    begin_match(inputs.match, MatchKind::replay, true);
    inputs.device_lost = true;
    decision = decide_render_tier(inputs);
    OA_CHECK(decision.reason == TierReason::device_lost);
    note_match_frame(inputs.match, decision.tier, true);
    inputs.device_lost = false;
    OA_CHECK(decide_render_tier(inputs).tier == RenderTier::accelerated);
    // A drop, then lifting it, still waits.
    inputs.drop = Drop::driver_failure;
    note_match_frame(inputs.match, decide_render_tier(inputs).tier, false);
    inputs.drop = Drop::none;
    OA_CHECK(decide_render_tier(inputs).reason == TierReason::waiting_for_match_end);
    end_match(inputs.match);

    // A device reset that destroyed the tier's textures keeps it standard.
    begin_match(inputs.match, MatchKind::shared_game, true);
    stop_until_match_end(inputs.match);
    OA_CHECK(decide_render_tier(inputs).reason == TierReason::waiting_for_match_end);
    end_match(inputs.match);

    // Begun on the standard tier, with the test not yet run: neither the
    // setting nor the test starts anything before the match ends.
    inputs = accelerated_run();
    inputs.setting = HardwareAcceleration::off;
    inputs.function_test = FunctionTest::not_run;
    begin_match(
        inputs.match,
        MatchKind::shared_game,
        decide_render_tier(inputs).tier == RenderTier::accelerated
    );
    OA_CHECK(!inputs.match.accelerated);
    inputs.setting = HardwareAcceleration::basic;
    OA_CHECK(decide_render_tier(inputs).reason == TierReason::waiting_for_match_end);
    OA_CHECK(!function_test_may_run(inputs));
    end_match(inputs.match);
    OA_CHECK(function_test_may_run(inputs));

    // A match played alone is no shared match.
    begin_match(inputs.match, MatchKind::none, true);
    OA_CHECK(inputs.match.kind == MatchKind::none);
    OA_CHECK(!inputs.match.accelerated);
    note_match_frame(inputs.match, RenderTier::standard, false);
    inputs.function_test = FunctionTest::passed;
    OA_CHECK(decide_render_tier(inputs).tier == RenderTier::accelerated);
}

// ---------------------------------------------------------------------------
// Creating the renderer

/// SDL's order in these cases: three hardware drivers, then software.
constexpr std::array<std::string_view, 4> sdl_order = {"alpha", "beta", "gamma", "software"};

/// Walks the drivers, refusing those named, and writes down each attempt:
/// a driver's name, then for software the hint as [value] or {} when
/// unset; `*` for SDL's own call; `+` after an attempt made with the
/// records ignored; and `none` when nothing was left.
///
/// @param inputs the drivers and the records
/// @param refusing the drivers that refuse
/// @param rebuild_after the driver that failed, for a rebuild; empty for a start
/// @return the attempts, separated by spaces
std::string walk(
    const CreationInputs& inputs,
    std::vector<std::string_view> refusing,
    std::string_view rebuild_after = {}
) {
    CreationWalk state =
        rebuild_after.empty() ? start_creation(inputs) : start_rebuild(inputs, rebuild_after);
    std::string attempts;
    for (int guard = 0; guard < 64; ++guard) {
        const Attempt attempt = next_attempt(state, inputs);
        if (!attempts.empty())
            attempts += ' ';
        if (attempt.kind == AttemptKind::none) {
            attempts += "none";
            return attempts;
        }
        OA_CHECK(!attempt.set_framebuffer_hint || !attempt.framebuffer_hint.empty());
        std::string_view driver = attempt.driver;
        if (attempt.kind == AttemptKind::sdl_choice) {
            attempts += '*';
            driver = "*";
        } else {
            attempts += attempt.driver;
            std::string lower(attempt.driver);
            for (char& letter : lower)
                letter = static_cast<char>(std::tolower(static_cast<unsigned char>(letter)));
            if (lower == software_driver)
                attempts += attempt.set_framebuffer_hint ? "[" + attempt.framebuffer_hint + "]"
                                                         : std::string("{}");
            else
                OA_CHECK(!attempt.set_framebuffer_hint);
        }
        if (attempt.records_ignored)
            attempts += '+';
        bool refused = false;
        for (const std::string_view name : refusing)
            if (name == driver)
                refused = true;
        if (!refused)
            return attempts;
    }
    return attempts + " (no end)";
}

void test_creation_walk_by_table() {
    struct Case {
        const char* name;
        std::vector<std::string_view> recorded;
        std::vector<std::string_view> refusing;
        bool native;
        bool list;
        const char* expected;
    };

    const std::vector<Case> cases = {
        {"first driver starts", {}, {}, true, true, "alpha"},
        {"one refuses", {}, {"alpha"}, true, true, "alpha beta"},
        {"hardware refuses, window framebuffer",
         {},
         {"alpha", "beta", "gamma"},
         true,
         true,
         "alpha beta gamma software[0]"},
        {"hardware refuses, needs a driver",
         {},
         {"alpha", "beta", "gamma"},
         false,
         true,
         "alpha beta gamma software[alpha,beta,gamma]"},
        {"hardware refuses, single name",
         {},
         {"alpha", "beta", "gamma"},
         false,
         false,
         "alpha beta gamma software[alpha]"},
        {"recorded driver skipped", {"alpha"}, {}, true, true, "beta"},
        {"recorded driver left out of the hint",
         {"beta"},
         {"alpha", "gamma"},
         false,
         true,
         "alpha gamma software[alpha,gamma]"},
        {"recorded first left out of a single name",
         {"alpha"},
         {"beta", "gamma"},
         false,
         false,
         "beta gamma software[beta]"},
        {"all recorded, window framebuffer",
         {"alpha", "beta", "gamma"},
         {},
         true,
         true,
         "software[0]"},
        {"all recorded, needs a driver: walk again",
         {"alpha", "beta", "gamma"},
         {},
         false,
         true,
         "alpha+"},
        {"all recorded, needs a driver, all refuse",
         {"alpha", "beta", "gamma"},
         {"alpha", "beta", "gamma", "software"},
         false,
         true,
         "alpha+ beta+ gamma+ software[alpha,beta,gamma]+ none"},
        {"some recorded, software refuses: walk again",
         {"beta"},
         {"alpha", "gamma", "software"},
         false,
         true,
         "alpha gamma software[alpha,gamma] alpha+ beta+"},
        {"recorded, window framebuffer, software refuses",
         {"alpha"},
         {"beta", "gamma", "software"},
         true,
         true,
         "beta gamma software[0] alpha+"},
        {"software never skipped",
         {"software"},
         {"alpha", "beta", "gamma"},
         true,
         true,
         "alpha beta gamma software[0]"},
        {"nothing recorded, everything refuses",
         {},
         {"alpha", "beta", "gamma", "software"},
         true,
         true,
         "alpha beta gamma software[0] none"},
    };
    for (const Case& test : cases) {
        CreationInputs inputs;
        inputs.sdl_order = sdl_order;
        inputs.failed_drivers = test.recorded;
        inputs.native_window_framebuffer = test.native;
        inputs.hint_takes_list = test.list;
        const std::string attempts = walk(inputs, test.refusing);
        if (attempts != test.expected)
            std::fprintf(stderr, "walk case: %s: %s\n", test.name, attempts.c_str());
        OA_CHECK(attempts == test.expected);
    }

    // With software alone in SDL's order, it is tried with no hint.
    const std::array<std::string_view, 1> software_only = {"software"};
    CreationInputs alone;
    alone.sdl_order = software_only;
    OA_CHECK(walk(alone, {}) == "software{}");
    alone.native_window_framebuffer = true;
    OA_CHECK(walk(alone, {}) == "software{}");
}

void test_environment_and_rebuilds() {
    // SDL_RENDER_DRIVER: SDL's own call, once, with no hint and no records.
    const std::vector<std::string_view> recorded = {"alpha", "beta"};
    const std::array<std::string_view, 2> environment = {"beta", "gamma"};
    CreationInputs inputs;
    inputs.sdl_order = sdl_order;
    inputs.failed_drivers = recorded;
    inputs.environment_order = environment;
    inputs.render_driver_named = true;
    inputs.hint_takes_list = true;
    OA_CHECK(walk(inputs, {}) == "*");
    OA_CHECK(walk(inputs, {"*"}) == "* none");

    // A rebuild under SDL_RENDER_DRIVER: its later drivers, then software,
    // with a hint that leaves out only the failed driver; no second walk.
    OA_CHECK(walk(inputs, {"gamma", "software"}, "beta") == "gamma software[alpha,gamma] none");
    inputs.native_window_framebuffer = true;
    OA_CHECK(walk(inputs, {"gamma"}, "beta") == "gamma software[0]");
    OA_CHECK(walk(inputs, {"gamma", "software"}, "gamma") == "software[0] none");
    const std::array<std::string_view, 3> with_software = {"beta", "software", "gamma"};
    inputs.environment_order = with_software;
    OA_CHECK(walk(inputs, {"software", "gamma"}, "beta") == "software[0] gamma none");

    // A rebuild: the drivers after the failed one, skipping recorded ones,
    // then software with the failed one left out of the hint, then SDL's
    // full order again with the records ignored.
    CreationInputs rebuild;
    rebuild.sdl_order = sdl_order;
    rebuild.hint_takes_list = true;
    OA_CHECK(walk(rebuild, {}, "beta") == "gamma");
    OA_CHECK(walk(rebuild, {"gamma"}, "beta") == "gamma software[alpha,gamma]");
    OA_CHECK(
        walk(rebuild, {"gamma", "software", "alpha", "beta"}, "beta") ==
        "gamma software[alpha,gamma] alpha+ beta+ gamma+ software[alpha,gamma]+ none"
    );
    const std::vector<std::string_view> alpha_recorded = {"alpha"};
    rebuild.failed_drivers = alpha_recorded;
    OA_CHECK(walk(rebuild, {}, "gamma") == "software[beta]");
    const std::vector<std::string_view> two_recorded = {"alpha", "beta"};
    rebuild.failed_drivers = two_recorded;
    OA_CHECK(walk(rebuild, {}, "gamma") == "alpha+");
    OA_CHECK(
        walk(rebuild, {"alpha", "beta", "gamma"}, "gamma") ==
        "alpha+ beta+ gamma+ software[alpha,beta]+"
    );
    rebuild.native_window_framebuffer = true;
    OA_CHECK(walk(rebuild, {}, "gamma") == "software[0]");

    // SDL matches SDL_RENDER_DRIVER's names in any letter case, so a rebuild
    // finds the failed driver in the list however the tester spelled it,
    // and never tries it again.
    CreationInputs spelled;
    spelled.sdl_order = sdl_order;
    spelled.render_driver_named = true;
    spelled.hint_takes_list = true;
    const std::array<std::string_view, 2> beta_first = {"Beta", "gamma"};
    spelled.environment_order = beta_first;
    OA_CHECK(walk(spelled, {"gamma"}, "beta") == "gamma software[alpha,gamma]");
    OA_CHECK(walk(spelled, {"gamma", "software"}, "beta") == "gamma software[alpha,gamma] none");
    const std::array<std::string_view, 4> named_twice = {"beta", "GAMMA", "BETA", "Software"};
    spelled.environment_order = named_twice;
    OA_CHECK(walk(spelled, {"GAMMA"}, "beta") == "GAMMA Software[alpha,gamma]");
    OA_CHECK(walk(spelled, {"GAMMA", "Software"}, "beta") == "GAMMA Software[alpha,gamma] none");
    // A failed driver the list does not name: the whole list, never it.
    const std::array<std::string_view, 2> without_it = {"gamma", "alpha"};
    spelled.environment_order = without_it;
    OA_CHECK(walk(spelled, {"gamma", "alpha"}, "beta") == "gamma alpha software[alpha,gamma]");
}

void test_creation_walk_never_fails_for_records() {
    // Whatever is recorded and whatever refuses, a start fails only when
    // every driver refuses, never sets an empty hint, and always ends.
    const std::array<std::string_view, 4> names = sdl_order;
    uint32_t walks = 0;
    for (uint32_t recorded_bits = 0; recorded_bits < 16; ++recorded_bits) {
        for (uint32_t refusing_bits = 0; refusing_bits < 16; ++refusing_bits) {
            for (int variant = 0; variant < 4; ++variant) {
                std::vector<std::string_view> recorded;
                std::vector<std::string_view> refusing;
                for (uint32_t index = 0; index < names.size(); ++index) {
                    if ((recorded_bits >> index) & 1u)
                        recorded.push_back(names[index]);
                    if ((refusing_bits >> index) & 1u)
                        refusing.push_back(names[index]);
                }
                CreationInputs inputs;
                inputs.sdl_order = sdl_order;
                inputs.failed_drivers = recorded;
                inputs.native_window_framebuffer = (variant & 1) != 0;
                inputs.hint_takes_list = (variant & 2) != 0;
                const std::string attempts = walk(inputs, refusing);
                ++walks;
                const bool failed =
                    attempts.size() >= 4 && attempts.compare(attempts.size() - 4, 4, "none") == 0;
                OA_CHECK(attempts.find("(no end)") == std::string::npos);
                OA_CHECK(failed == (refusing_bits == 15));
                // With nothing recorded the walk makes SDL's own attempts in
                // SDL's order.
                if (recorded_bits == 0 && refusing_bits == 0)
                    OA_CHECK(attempts == "alpha");
            }
        }
    }
    OA_CHECK(walks == 16 * 16 * 4);
}

// ---------------------------------------------------------------------------
// Present stalls

void test_stalls() {
    constexpr uint64_t second = 1'000'000'000;
    // The standard tier logs once; the accelerated tier drops.
    StallWatch watch;
    OA_CHECK(
        note_present(watch, 1 * second, stall_present_ns, RenderTier::standard) == StallAction::none
    );
    OA_CHECK(
        note_present(watch, 2 * second, stall_present_ns + 1, RenderTier::standard) ==
        StallAction::none
    );
    OA_CHECK(
        note_present(watch, 5 * second, 3 * second, RenderTier::standard) == StallAction::none
    );
    OA_CHECK(note_present(watch, 8 * second, 3 * second, RenderTier::standard) == StallAction::log);
    for (uint64_t time = 9; time < 15; ++time)
        OA_CHECK(
            note_present(watch, time * second, 3 * second, RenderTier::standard) ==
            StallAction::none
        );
    StallWatch accelerated;
    OA_CHECK(
        note_present(accelerated, 1 * second, 3 * second, RenderTier::accelerated) ==
        StallAction::none
    );
    OA_CHECK(
        note_present(accelerated, 2 * second, 3 * second, RenderTier::accelerated) ==
        StallAction::none
    );
    OA_CHECK(
        note_present(accelerated, 3 * second, 3 * second, RenderTier::accelerated) ==
        StallAction::drop
    );
    // Three stalls spread over more than 10 s do nothing.
    StallWatch spread;
    OA_CHECK(note_present(spread, 0, 3 * second, RenderTier::accelerated) == StallAction::none);
    OA_CHECK(
        note_present(spread, 6 * second, 3 * second, RenderTier::accelerated) == StallAction::none
    );
    OA_CHECK(
        note_present(spread, 12 * second, 3 * second, RenderTier::accelerated) == StallAction::none
    );
    OA_CHECK(
        note_present(spread, 15 * second, 3 * second, RenderTier::accelerated) == StallAction::drop
    );
}

// ---------------------------------------------------------------------------
// The step-down ladder

void test_start_budget() {
    // The rules written out again: none with two processors or fewer, on
    // ARM other than Apple silicon and the Pis that have been run, before
    // Vista and in a class nobody has run; reduced with three processors and
    // in a class run but not measured; full otherwise. Memory never enters
    // it: the same at 1.75 GiB, 2 GiB, 4 GiB and above, and unreported.
    const ClassTesting classes[] = {
        ClassTesting::untested, ClassTesting::tested, ClassTesting::measured
    };
    const uint64_t memories[] = {
        0, 1792 * mebibyte, 2 * gibibyte, 4 * gibibyte, 4 * gibibyte + 1, 64 * gibibyte
    };
    uint32_t mismatches = 0;
    uint32_t full = 0;
    for (uint32_t processors = 1; processors <= 16; ++processors) {
        for (const ClassTesting run_class : classes) {
            for (uint32_t bits = 0; bits < 4; ++bits) {
                StartInputs machine;
                machine.processors = processors;
                machine.run_class = run_class;
                machine.other_arm = (bits & 1u) != 0;
                machine.legacy_windows = (bits & 2u) != 0;
                SceneBudget expected = SceneBudget::full;
                if (processors <= 2 || machine.other_arm || machine.legacy_windows ||
                    run_class == ClassTesting::untested)
                    expected = SceneBudget::none;
                else if (processors == 3 || run_class == ClassTesting::tested)
                    expected = SceneBudget::reduced;
                for (const uint64_t memory : memories) {
                    machine.memory = memory;
                    if (start_budget(machine) != expected || start_rung(machine).budget != expected)
                        ++mismatches;
                }
                if (expected == SceneBudget::full)
                    ++full;
            }
        }
    }
    OA_CHECK(mismatches == 0);
    OA_CHECK(full > 0);
}

void test_start_rung() {
    struct Case {
        const char* name;
        void (*change)(StartInputs&);
        SceneBudget budget;
        bool magnify;
        CardFilter card;
        bool blend_allowed;
    };

    const Case cases[] = {
        {"measured class",
         [](StartInputs&) {},
         SceneBudget::full,
         true,
         CardFilter::prescale_full,
         true},
        {"four processors",
         [](StartInputs& m) { m.processors = 4; },
         SceneBudget::full,
         true,
         CardFilter::prescale_full,
         true},
        {"three processors",
         [](StartInputs& m) { m.processors = 3; },
         SceneBudget::reduced,
         true,
         CardFilter::prescale_full,
         true},
        {"two processors",
         [](StartInputs& m) { m.processors = 2; },
         SceneBudget::none,
         false,
         CardFilter::prescale_full,
         true},
        {"light machine",
         [](StartInputs& m) {
             m.processors = 1;
             m.light_machine = true;
         },
         SceneBudget::none,
         false,
         CardFilter::prescale_quarter,
         true},
        {"tested class",
         [](StartInputs& m) {
             m.run_class = ClassTesting::tested;
             m.pixelart = true;
         },
         SceneBudget::reduced,
         true,
         CardFilter::pixelart,
         true},
        {"untested class",
         [](StartInputs& m) {
             m.run_class = ClassTesting::untested;
             m.pixelart = true;
         },
         SceneBudget::none,
         false,
         CardFilter::pixelart,
         true},
        {"other ARM, measured class",
         [](StartInputs& m) { m.other_arm = true; },
         SceneBudget::none,
         false,
         CardFilter::prescale_full,
         true},
        {"pi, 4 GiB",
         [](StartInputs& m) {
             m.processors = 4;
             m.memory = 4 * gibibyte;
             m.raspberry_pi = true;
             m.other_arm = true;
             m.run_class = ClassTesting::untested;
         },
         SceneBudget::none,
         false,
         CardFilter::prescale_quarter,
         false},
        {"pi, 8 GiB, pixelart",
         [](StartInputs& m) {
             m.processors = 4;
             m.memory = 8 * gibibyte;
             m.raspberry_pi = true;
             m.other_arm = true;
             m.pixelart = true;
         },
         SceneBudget::none,
         false,
         CardFilter::pixelart,
         true},
        {"before vista, 2 GiB",
         [](StartInputs& m) {
             m.processors = 2;
             m.memory = 2 * gibibyte;
             m.legacy_windows = true;
             m.driver.blend_excluded = true;
         },
         SceneBudget::none,
         false,
         CardFilter::prescale_full,
         false},
        {"before vista, run class",
         [](StartInputs& m) {
             m.legacy_windows = true;
             m.driver.blend_excluded = true;
         },
         SceneBudget::none,
         false,
         CardFilter::prescale_full,
         false},
        {"driver that excludes the blend",
         [](StartInputs& m) { m.driver.blend_excluded = true; },
         SceneBudget::full,
         true,
         CardFilter::prescale_full,
         false},
        {"blend not available",
         [](StartInputs& m) { m.blend_available = false; },
         SceneBudget::full,
         true,
         CardFilter::prescale_full,
         false},
        // Memory from 2 GiB sizes nothing but the blend, which needs more
        // than 4 GiB.
        {"1.75 GiB",
         [](StartInputs& m) { m.memory = 1792 * mebibyte; },
         SceneBudget::full,
         true,
         CardFilter::prescale_full,
         false},
        {"2 GiB",
         [](StartInputs& m) { m.memory = 2 * gibibyte; },
         SceneBudget::full,
         true,
         CardFilter::prescale_full,
         false},
        {"4 GiB",
         [](StartInputs& m) { m.memory = 4 * gibibyte; },
         SceneBudget::full,
         true,
         CardFilter::prescale_full,
         false},
        {"just over 4 GiB",
         [](StartInputs& m) { m.memory = 4 * gibibyte + 1; },
         SceneBudget::full,
         true,
         CardFilter::prescale_full,
         true},
        {"4 GiB, light machine",
         [](StartInputs& m) {
             m.memory = 4 * gibibyte;
             m.light_machine = true;
         },
         SceneBudget::full,
         true,
         CardFilter::prescale_quarter,
         false},
    };
    for (const Case& test : cases) {
        StartInputs machine;
        machine.processors = 8;
        machine.memory = 16 * gibibyte;
        machine.run_class = ClassTesting::measured;
        machine.blend_available = true;
        test.change(machine);
        const LadderState state = start_rung(machine);
        // No machine starts at the NEAREST-chrome rung by its size.
        const bool matches = state.budget == test.budget && state.magnify == test.magnify &&
                             state.filtered_chrome && state.card == test.card &&
                             state.blend_allowed == test.blend_allowed && !state.standard &&
                             state.method == ZoomOutMethod::area;
        if (!matches)
            std::fprintf(stderr, "start_rung case: %s\n", test.name);
        OA_CHECK(matches);
    }
    OA_CHECK(most_memory_without_blend == 4 * gibibyte);
    OA_CHECK(!magnify_measured_at_budget_none);
    OA_CHECK(!magnify_measured_before_vista);
}

/// A machine that starts at the top of the ladder.
StartInputs measured_desktop() {
    StartInputs machine;
    machine.processors = 8;
    machine.memory = 16 * gibibyte;
    machine.run_class = ClassTesting::measured;
    machine.blend_available = true;
    return machine;
}

void test_resume_and_step_up() {
    const StartInputs desktop = measured_desktop();
    const LadderState ceiling = start_ceiling(desktop);
    OA_CHECK(ceiling.budget == SceneBudget::full && ceiling.magnify && ceiling.filtered_chrome);

    // Step up undoes the lowest rung taken first.
    LadderState state = start_rung(desktop);
    state.method = ZoomOutMethod::blend;
    state.budget = SceneBudget::none;
    state.magnify = false;
    state.filtered_chrome = false;
    state.card = CardFilter::linear;
    state = step_up(state, ceiling);
    OA_CHECK(state.filtered_chrome && state.card == CardFilter::linear);
    state = step_up(state, ceiling);
    OA_CHECK(state.card == CardFilter::prescale_quarter);
    state = step_up(state, ceiling);
    OA_CHECK(state.card == CardFilter::prescale_full);
    state = step_up(state, ceiling);
    OA_CHECK(state.magnify);
    state = step_up(state, ceiling);
    OA_CHECK(state.budget == SceneBudget::reduced);
    state = step_up(state, ceiling);
    OA_CHECK(state.budget == SceneBudget::full);
    state = step_up(state, ceiling);
    OA_CHECK(state.method == ZoomOutMethod::area);
    const LadderState again = step_up(state, ceiling);
    OA_CHECK(
        again.method == state.method && again.budget == state.budget && again.card == state.card
    );

    // A remembered rung starts the run there, one higher only with headroom.
    LadderState remembered = start_rung(desktop);
    remembered.budget = SceneBudget::reduced;
    OA_CHECK(resume_rung(desktop, remembered, 70).budget == SceneBudget::reduced);
    OA_CHECK(resume_rung(desktop, remembered, headroom_percent).budget == SceneBudget::reduced);
    OA_CHECK(resume_rung(desktop, remembered, headroom_percent - 1).budget == SceneBudget::full);
    remembered.standard = true;
    OA_CHECK(!resume_rung(desktop, remembered, 90).standard);

    // A class not yet measured starts at reduced but may rise to full.
    StartInputs tested = desktop;
    tested.run_class = ClassTesting::tested;
    OA_CHECK(start_rung(tested).budget == SceneBudget::reduced);
    OA_CHECK(resume_rung(tested, start_rung(tested), 50).budget == SceneBudget::full);

    // Never above the starting rung's limit: at 4 GiB the blend never comes
    // back, though the budget may rise.
    StartInputs four = tested;
    four.memory = 4 * gibibyte;
    LadderState generous = start_rung(desktop);
    generous.method = ZoomOutMethod::blend;
    generous.budget = SceneBudget::none;
    const LadderState resumed = resume_rung(four, generous, 10);
    OA_CHECK(!resumed.blend_allowed && resumed.method == ZoomOutMethod::area);
    OA_CHECK(resumed.magnify && resumed.filtered_chrome);
    OA_CHECK(resumed.card == CardFilter::prescale_full && resumed.budget == SceneBudget::reduced);

    // A start at the magnify-off rung, before Vista or at budget none, rises
    // no higher by the remembered rung, whatever its headroom.
    StartInputs before_vista;
    before_vista.processors = 2;
    before_vista.memory = 2 * gibibyte;
    before_vista.legacy_windows = true;
    before_vista.light_machine = true;
    before_vista.run_class = ClassTesting::measured;
    before_vista.driver.blend_excluded = true;
    before_vista.blend_available = true;
    StartInputs untested = desktop;
    untested.run_class = ClassTesting::untested;
    StartInputs two_processors = desktop;
    two_processors.processors = 2;
    for (const StartInputs& machine : {before_vista, untested, two_processors}) {
        const LadderState limit = start_ceiling(machine);
        OA_CHECK(limit.budget == SceneBudget::none && !limit.magnify);
        for (uint32_t median = 0; median < 100; median += 10) {
            const LadderState start = resume_rung(machine, generous, median);
            OA_CHECK(start.budget == SceneBudget::none && !start.magnify);
            OA_CHECK(start.filtered_chrome && !start.standard);
        }
    }
    OA_CHECK(!resume_rung(before_vista, generous, 0).blend_allowed);
    OA_CHECK(resume_rung(before_vista, generous, 0).card == CardFilter::prescale_quarter);
}

void test_rung_without() {
    const LadderState top = start_rung(measured_desktop());
    // The scene: magnify off, and nothing else.
    LadderState lowered = rung_without(top, AcceleratedBuffer::scene);
    LadderState expected = top;
    expected.magnify = false;
    OA_CHECK(lowered == expected);
    OA_CHECK(rung_without(lowered, AcceleratedBuffer::scene) == lowered);
    // A prescale target: the card's magnification one rung lower, to plain
    // LINEAR, which makes none; PIXELART makes none either.
    lowered = rung_without(top, AcceleratedBuffer::prescale);
    expected = top;
    expected.card = CardFilter::prescale_quarter;
    OA_CHECK(lowered == expected);
    lowered = rung_without(lowered, AcceleratedBuffer::prescale);
    expected.card = CardFilter::linear;
    OA_CHECK(lowered == expected);
    OA_CHECK(rung_without(lowered, AcceleratedBuffer::prescale) == lowered);
    LadderState pixelart = top;
    pixelart.card = CardFilter::pixelart;
    OA_CHECK(rung_without(pixelart, AcceleratedBuffer::prescale) == pixelart);
    // Full's pages and targets: Basic, and nothing else; Basic's rungs make
    // neither.
    LadderState full = top;
    full.full = true;
    for (const AcceleratedBuffer buffer :
         {AcceleratedBuffer::card_pages, AcceleratedBuffer::card_targets}) {
        lowered = rung_without(full, buffer);
        expected = full;
        expected.full = false;
        OA_CHECK(lowered == expected);
        OA_CHECK(rung_without(top, buffer) == top);
    }

    // Each change is described, Full's before Basic's.
    OA_CHECK(describe_step(top, top) == "nothing changed");
    LadderState basic_again = full;
    basic_again.full = false;
    OA_CHECK(
        describe_step(full, basic_again) == "the graphics card no longer draws the battlefield"
    );
    LadderState blend = top;
    blend.method = ZoomOutMethod::blend;
    OA_CHECK(describe_step(top, blend) == "the graphics card blends the zoomed-out view");
    LadderState shed = top;
    shed.budget = SceneBudget::none;
    shed.magnify = false;
    OA_CHECK(
        describe_step(top, shed) ==
        "the zoomed-out view is no longer smoothed and the graphics card no longer magnifies "
        "the battlefield"
    );
    LadderState standard = top;
    standard.standard = true;
    OA_CHECK(
        describe_step(top, standard) == "the processor draws everything for the rest of the run"
    );
}

// ---------------------------------------------------------------------------
// Chrome filtering and the prescale budget

void test_prescale() {
    OA_CHECK(prescale_budget(CardFilter::prescale_full) == prescale_budget_pixels);
    OA_CHECK(prescale_budget(CardFilter::prescale_quarter) == prescale_budget_pixels / 4);
    OA_CHECK(prescale_budget(CardFilter::linear) == 0);
    OA_CHECK(prescale_budget(CardFilter::pixelart) == 0);

    PrescaleBudget budget{prescale_budget_pixels, 0};
    OA_CHECK(prescale_factor(budget, 640, 480, 1.5) == 2);
    OA_CHECK(prescale_factor(budget, 640, 480, 1.25) == 2);
    OA_CHECK(prescale_factor(budget, 640, 480, 2.0) == 2);
    OA_CHECK(prescale_factor(budget, 640, 480, 3.7) == 4);
    // n falls to the largest whole number that fits.
    OA_CHECK(prescale_factor(budget, 1920, 1080, 3.7) == 2);
    OA_CHECK(prescale_factor(budget, 3840, 2160, 1.5) == 1);
    OA_CHECK(prescale_factor(budget, 640, 480, 0.0) == 1);
    OA_CHECK(prescale_factor(budget, 0, 480, 1.5) == 1);
    OA_CHECK(prescale_factor(PrescaleBudget{}, 640, 480, 1.5) == 1);
    // A target that fills what is left exactly still fits.
    OA_CHECK(prescale_factor(PrescaleBudget{uint64_t{1280} * 960, 0}, 640, 480, 1.5) == 2);
    OA_CHECK(prescale_factor(PrescaleBudget{uint64_t{1280} * 960 - 1, 0}, 640, 480, 1.5) == 1);

    // The sum of all targets alive at once is charged; the front end's,
    // freed during a match, gives its room back.
    PrescaleBudget quarter{prescale_budget(CardFilter::prescale_quarter), 0};
    const uint64_t front_end = uint64_t{1280} * 960;
    OA_CHECK(prescale_factor(quarter, 640, 480, 1.5) == 2);
    OA_CHECK(charge_prescale(quarter, front_end));
    OA_CHECK(prescale_factor(quarter, 640, 480, 1.5) == 1);
    OA_CHECK(!charge_prescale(quarter, front_end));
    OA_CHECK(quarter.charged == front_end);
    release_prescale(quarter, front_end);
    OA_CHECK(quarter.charged == 0);
    OA_CHECK(prescale_factor(quarter, 640, 480, 1.5) == 2);
    release_prescale(quarter, front_end);
    OA_CHECK(quarter.charged == 0);
    OA_CHECK(charge_prescale(quarter, quarter.limit));
    OA_CHECK(!charge_prescale(quarter, 1));
}

/// The Full tier's anti-aliasing: the factor each level of the Enhanced
/// anti-aliasing row asks for, the budget S by the machine, the pixels a
/// world target holds, and the factor fitted to the budget and the texture
/// limit for battlefields about those of common windows, each target's
/// memory printed and held within the budget.
void test_supersampling() {
    using oa::ui::engine_settings::AntiAliasing;
    OA_CHECK(supersample_factor(AntiAliasing::off) == 1);
    OA_CHECK(supersample_factor(AntiAliasing::x2) == 2);
    OA_CHECK(supersample_factor(AntiAliasing::x4) == 4);
    OA_CHECK(supersample_factor(AntiAliasing::x8) == 8);
    OA_CHECK(supersample_factor(AntiAliasing::x16) == 16);

    // The game's budget holds the largest target a renderer can, 16384 a
    // side (card::largest_target_edge): its square and the half.
    constexpr uint32_t largest_target_edge = 16384;
    OA_CHECK(
        supersample_budget_pixels >=
        supersample_target_pixels(largest_target_edge, largest_target_edge, 1)
    );
    // Budgets the fitting is tried within below: 2^25 pixels, which the
    // first design set, and a quarter of it.
    constexpr uint64_t whole = uint64_t{1} << 25;
    constexpr uint64_t quarter = whole / 4;

    // The texture and its half.
    OA_CHECK(supersample_target_pixels(100, 50, 1) == 5000 + 1250);
    OA_CHECK(supersample_target_pixels(100, 50, 2) == 20000 + 5000);
    OA_CHECK(supersample_target_pixels(100, 50, 4) == 80000 + 20000);
    // Above 4 the halves the resolve reduces through, each a quarter of the
    // one before.
    OA_CHECK(supersample_target_pixels(100, 50, 8) == 320000 + 80000 + 20000);
    OA_CHECK(supersample_target_pixels(100, 50, 16) == 1280000 + 320000 + 80000 + 20000);

    // Battlefields about those of windows of 1920x1080, 2560x1440,
    // 3840x2160 and 5120x1440, the HUD strips taken out, with the factor
    // each budget allows and the memory the target then takes.
    struct Window {
        const char* name;
        uint32_t width;
        uint32_t height;
        uint32_t at_whole;   ///< the factor fitted within S when 4x is asked
        uint32_t at_quarter; ///< within S / 4
    };

    constexpr std::array<Window, 4> windows{{
        {"1920x1080", 1792, 864, 4, 2},
        {"2560x1440", 2560, 1296, 2, 1},
        {"3840x2160", 3584, 1728, 2, 1},
        {"5120x1440", 4864, 1152, 2, 1},
    }};
    for (const Window& window : windows)
        for (const uint64_t budget : {whole, quarter}) {
            const uint32_t fitted = fit_supersample_factor(
                4, window.width, window.height, budget, unlimited_texture_size
            );
            OA_CHECK(fitted == (budget == whole ? window.at_whole : window.at_quarter));
            const uint64_t pixels = supersample_target_pixels(window.width, window.height, fitted);
            if (fitted == 1)
                std::printf(
                    "supersampling: a window of %s, a battlefield of %ux%u, a budget of %llu "
                    "MiB: factor 1, no world target\n",
                    window.name,
                    window.width,
                    window.height,
                    static_cast<unsigned long long>(budget * 4 / (1024 * 1024))
                );
            else
                std::printf(
                    "supersampling: a window of %s, a battlefield of %ux%u, a budget of %llu "
                    "MiB: factor %u, the world target and its half %llu MiB\n",
                    window.name,
                    window.width,
                    window.height,
                    static_cast<unsigned long long>(budget * 4 / (1024 * 1024)),
                    fitted,
                    static_cast<unsigned long long>(pixels * 4 / (1024 * 1024))
                );
            OA_CHECK(fitted == 1 || pixels <= budget);
            // Asking for less never gives more.
            OA_CHECK(
                fit_supersample_factor(
                    2, window.width, window.height, budget, unlimited_texture_size
                ) == std::min(fitted, 2U)
            );
            OA_CHECK(
                fit_supersample_factor(
                    1, window.width, window.height, budget, unlimited_texture_size
                ) == 1
            );
        }
    // The texture limit bounds the factor as the budget does.
    OA_CHECK(fit_supersample_factor(4, 1792, 864, whole, 4096) == 2);
    OA_CHECK(fit_supersample_factor(4, 1792, 864, whole, 2048) == 1);
    OA_CHECK(fit_supersample_factor(4, 1792, 864, whole, 8192) == 4);
    OA_CHECK(fit_supersample_factor(4, 1792, 864, whole, 3584) == 2);
    // A factor between the ones a target takes is read as the one below.
    OA_CHECK(fit_supersample_factor(3, 640, 480, whole, unlimited_texture_size) == 2);
    OA_CHECK(fit_supersample_factor(8, 640, 480, whole, unlimited_texture_size) == 8);
    OA_CHECK(
        fit_supersample_factor(16, 640, 480, supersample_budget_pixels, unlimited_texture_size) ==
        16
    );
    OA_CHECK(
        fit_supersample_factor(32, 640, 480, supersample_budget_pixels, unlimited_texture_size) ==
        16
    );
    // The texture limit halves 16 to the largest that fits: 1792 by 16 is
    // beyond 16384, by 8 within it.
    OA_CHECK(fit_supersample_factor(16, 1792, 864, supersample_budget_pixels, 16384) == 8);
    // No budget, or no battlefield, is no target.
    OA_CHECK(fit_supersample_factor(4, 640, 480, 0, unlimited_texture_size) == 1);
    OA_CHECK(fit_supersample_factor(4, 0, 480, whole, unlimited_texture_size) == 1);
    // A target that fills the budget exactly still fits.
    const uint64_t exact = supersample_target_pixels(640, 480, 2);
    OA_CHECK(fit_supersample_factor(2, 640, 480, exact, unlimited_texture_size) == 2);
    OA_CHECK(fit_supersample_factor(2, 640, 480, exact - 1, unlimited_texture_size) == 1);
}

void test_chrome_filter() {
    LadderState state;
    state.filtered_chrome = true;
    state.card = CardFilter::pixelart;
    OA_CHECK(chrome_filter(state, 1.5) == ScaleFilter::pixelart);
    OA_CHECK(chrome_filter(state, 2.0) == ScaleFilter::nearest);
    OA_CHECK(chrome_filter(state, 1.0) == ScaleFilter::nearest);
    state.card = CardFilter::prescale_full;
    OA_CHECK(chrome_filter(state, 1.25) == ScaleFilter::sharp_bilinear);
    state.card = CardFilter::prescale_quarter;
    OA_CHECK(chrome_filter(state, 1.25) == ScaleFilter::sharp_bilinear);
    state.card = CardFilter::linear;
    OA_CHECK(chrome_filter(state, 1.25) == ScaleFilter::linear);
    // The NEAREST-chrome rung and the standard tier draw as today.
    state.card = CardFilter::pixelart;
    state.filtered_chrome = false;
    OA_CHECK(chrome_filter(state, 1.25) == ScaleFilter::nearest);
    state.filtered_chrome = true;
    state.standard = true;
    OA_CHECK(chrome_filter(state, 1.25) == ScaleFilter::nearest);
    // The magnified scene follows the card's magnification alone: the
    // NEAREST-chrome rung leaves it as it was, so the card's rungs below it
    // still lighten it. A whole-number zoom and the standard tier draw it
    // NEAREST.
    state.standard = false;
    for (const bool filtered : {true, false}) {
        state.filtered_chrome = filtered;
        state.card = CardFilter::pixelart;
        OA_CHECK(world_filter(state, 1.5) == ScaleFilter::pixelart);
        OA_CHECK(world_filter(state, 2.0) == ScaleFilter::nearest);
        state.card = CardFilter::prescale_full;
        OA_CHECK(world_filter(state, 1.5) == ScaleFilter::sharp_bilinear);
        state.card = CardFilter::prescale_quarter;
        OA_CHECK(world_filter(state, 2.5) == ScaleFilter::sharp_bilinear);
        state.card = CardFilter::linear;
        OA_CHECK(world_filter(state, 1.5) == ScaleFilter::linear);
    }
    OA_CHECK(chrome_filter(state, 1.5) == ScaleFilter::nearest);
    state.standard = true;
    OA_CHECK(world_filter(state, 1.5) == ScaleFilter::nearest);
    // The period floor, with 256 MiB, draws every frame in the standard
    // tier, so its chrome at 800x600's scale of 1.25 is NEAREST, as v0.6.1's.
    TierInputs floor = accelerated_run();
    floor.memory = 256 * mebibyte;
    OA_CHECK(decide_render_tier(floor).tier == RenderTier::standard);
    OA_CHECK(!function_test_may_run(floor));
}

// Menu scaling: how each way holds the 640x480 frame in windows of the
// common sizes, one smaller than the frame and one of an odd width, and the
// filter it draws with in each tier.
void test_frame_fit_and_filter() {
    struct Window {
        int32_t width;
        int32_t height;
        int32_t whole_scale; ///< the largest whole step that fits; 0 below 640x480
    };

    const Window windows[] = {
        {640, 480, 1},
        {800, 600, 1},
        {1280, 800, 1},
        {1366, 768, 1},
        {1920, 1080, 2},
        {2000, 1250, 2},
        {600, 400, 0},
        {1001, 625, 1},
    };
    LadderState accelerated;
    accelerated.filtered_chrome = true;
    accelerated.card = CardFilter::pixelart;
    LadderState prescaled = accelerated;
    prescaled.card = CardFilter::prescale_full;
    for (const Window& window : windows) {
        // The letterbox's scale: the frame as large as the window holds.
        const double letterbox = std::min(window.width / 640.0, window.height / 480.0);
        const bool letterbox_whole = std::floor(letterbox) == letterbox;
        for (const MenuScaling scaling :
             {MenuScaling::sharp, MenuScaling::whole_steps, MenuScaling::unfiltered}) {
            const FrameFit fit = frame_fit(scaling, window.width, window.height, 640, 480);
            const bool whole_steps = scaling == MenuScaling::whole_steps && window.whole_scale > 0;
            OA_CHECK(fit == (whole_steps ? FrameFit::whole_steps : FrameFit::letterbox));
            const double scale = whole_steps ? double(window.whole_scale) : letterbox;
            // Unfiltered, whole steps and any whole-number scale repeat
            // each pixel in every tier; otherwise PIXELART where it works.
            const bool repeated = scaling == MenuScaling::unfiltered || whole_steps ||
                                  (!whole_steps && letterbox_whole);
            OA_CHECK(frame_wants_pixelart(scaling, scale) == !repeated);
            for (const bool works : {false, true})
                OA_CHECK(
                    frame_filter(scaling, nullptr, works, scale) ==
                    (repeated || !works ? ScaleFilter::nearest : ScaleFilter::pixelart)
                );
            OA_CHECK(
                frame_filter(scaling, &accelerated, false, scale) ==
                (repeated ? ScaleFilter::nearest : ScaleFilter::pixelart)
            );
            OA_CHECK(
                frame_filter(scaling, &prescaled, true, scale) ==
                (repeated ? ScaleFilter::nearest : ScaleFilter::sharp_bilinear)
            );
        }
    }
    // A frame of any other size is held alike: the load and save dialogs
    // over a match of the window's own size take one step.
    OA_CHECK(frame_fit(MenuScaling::whole_steps, 1366, 768, 1366, 768) == FrameFit::whole_steps);
    OA_CHECK(frame_fit(MenuScaling::whole_steps, 1365, 768, 1366, 768) == FrameFit::letterbox);
    OA_CHECK(frame_fit(MenuScaling::whole_steps, 1366, 767, 1366, 768) == FrameFit::letterbox);
    OA_CHECK(frame_fit(MenuScaling::whole_steps, 1366, 768, 0, 0) == FrameFit::letterbox);
    // The accelerated tier's NEAREST-chrome rung keeps the chrome's filter.
    LadderState nearest_chrome = accelerated;
    nearest_chrome.filtered_chrome = false;
    OA_CHECK(frame_filter(MenuScaling::sharp, &nearest_chrome, true, 1.5) == ScaleFilter::nearest);
    OA_CHECK(!frame_wants_pixelart(MenuScaling::sharp, 0.0));
}

// On a window at native density every scale is the one at the display's
// pixels, the layout's scale times the density, and the whole-number test
// is made on that product.
void test_chrome_filter_at_the_display() {
    LadderState state;
    state.filtered_chrome = true;
    state.card = CardFilter::pixelart;

    struct Case {
        double layout_scale;
        double density;
        ScaleFilter expected;
    };

    const Case cases[] = {
        // A display of 1440x900 points: the chrome's 1.875 is 3.75 at density 2.
        {1.875, 2.0, ScaleFilter::pixelart},
        {1.875, 1.0, ScaleFilter::pixelart},
        // Not whole in layout pixels, whole at the display.
        {1.5, 2.0, ScaleFilter::nearest},
        {1.6, 1.25, ScaleFilter::nearest},
        {1.25, 2.0, ScaleFilter::pixelart},
        // Whole in layout pixels, not at the display.
        {1.0, 1.5, ScaleFilter::pixelart},
        {2.0, 1.25, ScaleFilter::pixelart},
        {2.0, 1.5, ScaleFilter::nearest},
        // The 1:1 layers are drawn at the density itself.
        {1.0, 1.0, ScaleFilter::nearest},
        {1.0, 2.0, ScaleFilter::nearest},
        {1.0, 3.0, ScaleFilter::nearest},
        {1.0, 1.75, ScaleFilter::pixelart},
    };
    for (const Case& test : cases)
        OA_CHECK(chrome_filter(state, test.layout_scale * test.density) == test.expected);
    // The NEAREST-chrome rung keeps NEAREST at any density.
    state.filtered_chrome = false;
    OA_CHECK(chrome_filter(state, 1.875 * 2.0) == ScaleFilter::nearest);
    OA_CHECK(chrome_filter(state, 1.0 * 1.5) == ScaleFilter::nearest);
}

// ---------------------------------------------------------------------------
// Native pixel density

/// A start that the rule opens at native density, every condition holding.
DensityInputs native_start() {
    DensityInputs in;
    in.memory = 16 * gibibyte;
    in.setting = HardwareAcceleration::basic;
    in.class_measured = true;
    in.budget = SceneBudget::reduced;
    in.record = true;
    return in;
}

/// A rung the step-down remembered: the reduced budget, with magnify and
/// the filtered chrome on or off, or the standard tier.
LadderState remembered_rung(bool magnify, bool filtered_chrome, bool standard) {
    LadderState rung;
    rung.budget = SceneBudget::reduced;
    rung.magnify = magnify;
    rung.filtered_chrome = filtered_chrome;
    rung.card = CardFilter::pixelart;
    rung.standard = standard;
    return rung;
}

void test_native_density_by_table() {
    struct Case {
        const char* name;
        void (*change)(DensityInputs&);
        bool native;
        DensityReason reason;
    };

    const Case cases[] = {
        {"every condition", [](DensityInputs&) {}, true, DensityReason::native},
        {"--hardware-acceleration with the setting Off",
         [](DensityInputs& in) {
             in.setting = HardwareAcceleration::off;
             in.flag = AccelerationFlag::full;
         },
         true,
         DensityReason::native},
        {"a remembered rung above magnify off",
         [](DensityInputs& in) { in.remembered = remembered_rung(true, true, false); },
         true,
         DensityReason::native},
        {"one byte under the 2 GiB threshold",
         [](DensityInputs& in) { in.memory = 1792 * mebibyte - 1; },
         false,
         DensityReason::memory},
        {"at the 2 GiB threshold",
         [](DensityInputs& in) { in.memory = 1792 * mebibyte; },
         true,
         DensityReason::native},
        {"memory not reported",
         [](DensityInputs& in) { in.memory = 0; },
         false,
         DensityReason::memory},
        {"--native-density under 2 GiB",
         [](DensityInputs& in) {
             in.asked = true;
             in.memory = 1 * gibibyte;
         },
         false,
         DensityReason::memory},
        {"--no-hardware-acceleration",
         [](DensityInputs& in) { in.flag = AccelerationFlag::off; },
         false,
         DensityReason::flag_off},
        {"--native-density with --no-hardware-acceleration",
         [](DensityInputs& in) {
             in.asked = true;
             in.flag = AccelerationFlag::off;
         },
         false,
         DensityReason::flag_off},
        {"--native-density on the dummy driver under SDL_RENDER_DRIVER, with nothing recorded",
         [](DensityInputs& in) {
             in = DensityInputs{};
             in.memory = 2 * gibibyte;
             in.asked = true;
             in.flag = AccelerationFlag::full;
             in.render_driver_named = true;
             in.virtual_video_driver = true;
             in.unattended = true;
         },
         true,
         DensityReason::asked},
        {"SDL_RENDER_DRIVER",
         [](DensityInputs& in) { in.render_driver_named = true; },
         false,
         DensityReason::environment},
        {"SDL_RENDER_DRIVER with --hardware-acceleration",
         [](DensityInputs& in) {
             in.render_driver_named = true;
             in.flag = AccelerationFlag::full;
         },
         false,
         DensityReason::environment},
        {"the dummy video driver",
         [](DensityInputs& in) { in.virtual_video_driver = true; },
         false,
         DensityReason::environment},
        {"an unattended run",
         [](DensityInputs& in) { in.unattended = true; },
         false,
         DensityReason::unattended},
        {"a video capture",
         [](DensityInputs& in) { in.capture = true; },
         false,
         DensityReason::capture},
        {"the setting Off",
         [](DensityInputs& in) { in.setting = HardwareAcceleration::off; },
         false,
         DensityReason::setting_off},
        {"the setting Full",
         [](DensityInputs& in) { in.setting = HardwareAcceleration::full; },
         true,
         DensityReason::native},
        {"--hardware-acceleration=basic with the setting Off",
         [](DensityInputs& in) {
             in.setting = HardwareAcceleration::off;
             in.flag = AccelerationFlag::basic;
         },
         true,
         DensityReason::native},
        {"a class not measured",
         [](DensityInputs& in) { in.class_measured = false; },
         false,
         DensityReason::class_unmeasured},
        {"budget none",
         [](DensityInputs& in) { in.budget = SceneBudget::none; },
         false,
         DensityReason::budget_none},
        {"budget full",
         [](DensityInputs& in) { in.budget = SceneBudget::full; },
         true,
         DensityReason::native},
        {"a remembered magnify-off rung",
         [](DensityInputs& in) { in.remembered = remembered_rung(false, true, false); },
         false,
         DensityReason::remembered_rung},
        {"a remembered NEAREST-chrome rung",
         [](DensityInputs& in) { in.remembered = remembered_rung(false, false, false); },
         false,
         DensityReason::remembered_rung},
        {"a remembered standard tier",
         [](DensityInputs& in) { in.remembered = remembered_rung(true, true, true); },
         false,
         DensityReason::remembered_rung},
        {"no record, as at a first start",
         [](DensityInputs& in) { in.record = false; },
         false,
         DensityReason::no_record},
        {"a platform at native density",
         [](DensityInputs& in) { in.platform_native = true; },
         true,
         DensityReason::platform},
        {"a platform at native density at a first start, on the dummy driver, unattended",
         [](DensityInputs& in) {
             in = DensityInputs{};
             in.memory = 2 * gibibyte;
             in.platform_native = true;
             in.render_driver_named = true;
             in.virtual_video_driver = true;
             in.unattended = true;
             in.capture = true;
         },
         true,
         DensityReason::platform},
        {"a platform at native density with the setting Off",
         [](DensityInputs& in) {
             in.platform_native = true;
             in.setting = HardwareAcceleration::off;
             in.class_measured = false;
             in.budget = SceneBudget::none;
             in.remembered = remembered_rung(true, true, true);
         },
         true,
         DensityReason::platform},
        {"a platform at native density under 2 GiB",
         [](DensityInputs& in) {
             in.platform_native = true;
             in.memory = 1792 * mebibyte - 1;
         },
         false,
         DensityReason::memory},
        {"a platform at native density with memory not reported",
         [](DensityInputs& in) {
             in.platform_native = true;
             in.memory = 0;
         },
         false,
         DensityReason::memory},
        {"a platform at native density with --no-hardware-acceleration",
         [](DensityInputs& in) {
             in.platform_native = true;
             in.flag = AccelerationFlag::off;
         },
         false,
         DensityReason::flag_off},
        {"a platform at native density with --native-density",
         [](DensityInputs& in) {
             in.platform_native = true;
             in.asked = true;
         },
         true,
         DensityReason::platform},
        {"the Native pixel density setting, nothing recorded or measured",
         [](DensityInputs& in) {
             in = DensityInputs{};
             in.memory = 2 * gibibyte;
             in.chosen = true;
         },
         true,
         DensityReason::chosen},
        {"the setting on the dummy driver in an unattended capture",
         [](DensityInputs& in) {
             in.chosen = true;
             in.virtual_video_driver = true;
             in.unattended = true;
             in.capture = true;
         },
         true,
         DensityReason::chosen},
        {"the setting under 2 GiB",
         [](DensityInputs& in) {
             in.chosen = true;
             in.memory = 1 * gibibyte;
         },
         false,
         DensityReason::memory},
        {"the setting with --no-hardware-acceleration",
         [](DensityInputs& in) {
             in.chosen = true;
             in.flag = AccelerationFlag::off;
         },
         false,
         DensityReason::flag_off},
        {"--native-density and the setting",
         [](DensityInputs& in) {
             in.asked = true;
             in.chosen = true;
         },
         true,
         DensityReason::asked},
        {"a platform at native density with --hardware-acceleration=basic",
         [](DensityInputs& in) {
             in.platform_native = true;
             in.flag = AccelerationFlag::basic;
         },
         true,
         DensityReason::platform},
    };
    for (const Case& test : cases) {
        DensityInputs in = native_start();
        test.change(in);
        const DensityDecision decision = decide_native_density(in);
        if (decision.native != test.native || decision.reason != test.reason) {
            std::fprintf(stderr, "native density case: %s\n", test.name);
            OA_CHECK(decision.native == test.native);
            OA_CHECK(decision.reason == test.reason);
        }
    }
}

// Tells whether the window opens at native density by the rule, written out again.
bool native_by_the_rules(const DensityInputs& in) {
    if (in.memory < 1792 * mebibyte || in.flag == AccelerationFlag::off)
        return false;
    if (in.platform_native || in.asked || in.chosen)
        return true;
    return !in.render_driver_named && !in.virtual_video_driver && !in.unattended && !in.capture &&
           (in.setting != HardwareAcceleration::off || in.flag == AccelerationFlag::basic ||
            in.flag == AccelerationFlag::full) &&
           in.class_measured && in.budget != SceneBudget::none &&
           (!in.remembered || (in.remembered->magnify && !in.remembered->standard)) && in.record;
}

void test_native_density_every_combination() {
    const AccelerationFlag flags[] = {
        AccelerationFlag::none,
        AccelerationFlag::off,
        AccelerationFlag::basic,
        AccelerationFlag::full
    };
    const HardwareAcceleration settings[] = {
        HardwareAcceleration::off, HardwareAcceleration::basic, HardwareAcceleration::full
    };
    const SceneBudget budgets[] = {SceneBudget::none, SceneBudget::reduced, SceneBudget::full};
    const uint64_t memories[] = {0, 1792 * mebibyte - 1, 1792 * mebibyte, 16 * gibibyte};
    LadderState above;
    above.magnify = true;
    LadderState magnify_off;
    const std::optional<LadderState> remembered[] = {std::nullopt, above, magnify_off};
    uint32_t combinations = 0;
    uint32_t mismatches = 0;
    uint32_t native = 0;
    uint32_t native_under_2_gib = 0;
    uint32_t native_by_the_shipped_rule = 0;
    uint32_t reasons_out_of_order = 0;
    for (uint32_t bits = 0; bits < (1u << 9); ++bits)
        for (const AccelerationFlag flag : flags)
            for (const HardwareAcceleration setting : settings)
                for (const SceneBudget budget : budgets)
                    for (const uint64_t memory : memories)
                        for (const auto& rung : remembered) {
                            DensityInputs in;
                            in.asked = (bits & 1u) != 0;
                            in.setting = setting;
                            in.render_driver_named = (bits & 2u) != 0;
                            in.virtual_video_driver = (bits & 4u) != 0;
                            in.unattended = (bits & 8u) != 0;
                            in.capture = (bits & 16u) != 0;
                            in.class_measured = (bits & 32u) != 0;
                            in.record = (bits & 64u) != 0;
                            in.platform_native = (bits & 128u) != 0;
                            in.chosen = (bits & 256u) != 0;
                            in.flag = flag;
                            in.budget = budget;
                            in.memory = memory;
                            in.remembered = rung;
                            ++combinations;
                            const DensityDecision decision = decide_native_density(in);
                            if (decision.native != native_by_the_rules(in))
                                ++mismatches;
                            if (decision.native != (decision.reason == DensityReason::native ||
                                                    decision.reason == DensityReason::platform ||
                                                    decision.reason == DensityReason::asked ||
                                                    decision.reason == DensityReason::chosen))
                                ++mismatches;
                            // The platform's density is decided after the
                            // memory and a flag that names Off, before
                            // everything else.
                            if (in.platform_native && memory >= 1792 * mebibyte &&
                                flag != AccelerationFlag::off &&
                                decision.reason != DensityReason::platform)
                                ++reasons_out_of_order;
                            if (decision.reason == DensityReason::asked && in.platform_native)
                                ++reasons_out_of_order;
                            // The setting gives way to the platform and to
                            // --native-density, and only to them.
                            if (decision.reason == DensityReason::chosen &&
                                (in.platform_native || in.asked))
                                ++reasons_out_of_order;
                            if (decision.native) {
                                ++native;
                                if (memory < 1792 * mebibyte)
                                    ++native_under_2_gib;
                            }
                            // As the game fills it today, the class is never
                            // measured: only a platform at native density,
                            // --native-density and the setting open a window
                            // at native density.
                            in.class_measured = native_density_measured;
                            if (decide_native_density(in).native && !in.asked && !in.chosen &&
                                !in.platform_native)
                                ++native_by_the_shipped_rule;
                        }
    OA_CHECK(combinations == (1u << 9) * 4 * 3 * 3 * 4 * 3);
    OA_CHECK(mismatches == 0);
    OA_CHECK(reasons_out_of_order == 0);
    OA_CHECK(native > 0);
    OA_CHECK(native_under_2_gib == 0);
    OA_CHECK(native_by_the_shipped_rule == 0);
    OA_CHECK(!native_density_measured);
}

// ---------------------------------------------------------------------------
// Tiled textures

void test_tiles_by_table() {
    // Within the limit, or with none, one tile.
    TileGrid grid = plan_tiles(8000, 6000, unlimited_texture_size);
    OA_CHECK(grid.columns == 1 && grid.rows == 1 && grid.tile_size == 0);
    Tile tile = tile_at(grid, 0, 0);
    OA_CHECK(tile.texture.width == 8000 && tile.texture.height == 6000);
    OA_CHECK(tile.content.width == 8000 && tile.source.x == 0 && tile.source.y == 0);
    grid = plan_tiles(1920, 1080, 16384);
    OA_CHECK(grid.columns == 1 && grid.rows == 1);
    grid = plan_tiles(2048, 2048, 2048);
    OA_CHECK(grid.columns == 1 && grid.rows == 1);

    // A 2560x1440 window's 2304-pixel-wide battlefield on a 2048 limit.
    grid = plan_tiles(2304, 1312, 2048);
    OA_CHECK(grid.columns == 2 && grid.rows == 1 && grid.tile_size == 2048);
    tile = tile_at(grid, 0, 0);
    OA_CHECK(tile.texture.x == 0 && tile.texture.width == 2048);
    OA_CHECK(tile.content.x == 0 && tile.content.width == 2047);
    OA_CHECK(tile.source.x == 0 && tile.source.width == 2047);
    OA_CHECK(tile.texture.height == 1312 && tile.content.height == 1312 && tile.source.y == 0);
    tile = tile_at(grid, 1, 0);
    OA_CHECK(tile.content.x == 2047 && tile.content.width == 257);
    OA_CHECK(tile.texture.x == 2046 && tile.texture.width == 258);
    OA_CHECK(tile.source.x == 1 && tile.source.width == 257);

    // A larger limit still tiles at 2048.
    grid = plan_tiles(5000, 3000, 4096);
    OA_CHECK(grid.tile_size == largest_tile_size && grid.columns == 3 && grid.rows == 2);

    // Nothing to tile, or a limit too small.
    OA_CHECK(plan_tiles(0, 100, 2048).columns == 0);
    OA_CHECK(plan_tiles(100, 0, 0).rows == 0);
    OA_CHECK(plan_tiles(100, 100, 2).columns == 0);
    OA_CHECK(tile_at(plan_tiles(100, 100, 2), 0, 0).texture.width == 0);
    OA_CHECK(tile_at(plan_tiles(2304, 1312, 2048), 2, 0).texture.width == 0);
}

void test_tiles_cover_every_texel() {
    const uint32_t limits[] = {3, 4, 64, 100, 1024, 2048, 4096};
    const uint32_t lengths[] = {
        1, 2, 3, 4, 63, 64, 65, 127, 128, 1000, 2046, 2047, 2048, 2049, 2304, 4093, 4094, 5000
    };
    uint32_t grids = 0;
    for (const uint32_t limit : limits) {
        for (const uint32_t width : lengths) {
            for (const uint32_t height : {uint32_t{1}, uint32_t{65}, uint32_t{2049}}) {
                const TileGrid grid = plan_tiles(width, height, limit);
                ++grids;
                OA_CHECK(grid.columns >= 1 && grid.rows >= 1);
                const bool tiled = width > limit || height > limit;
                OA_CHECK((grid.tile_size != 0) == tiled);
                if (tiled)
                    OA_CHECK(
                        grid.tile_size == (limit < largest_tile_size ? limit : largest_tile_size)
                    );
                // Across: the contents follow one another from 0 to the width,
                // each texture within the tile size and the limit, with a
                // one-texel gutter toward each neighbour and none at the edges.
                uint32_t next = 0;
                for (uint32_t column = 0; column < grid.columns; ++column) {
                    const Tile tile = tile_at(grid, column, 0);
                    OA_CHECK(tile.content.x == next);
                    OA_CHECK(tile.content.width > 0);
                    next = tile.content.x + tile.content.width;
                    const uint32_t before = column == 0 ? 0 : tile_gutter;
                    const uint32_t after = column + 1 == grid.columns ? 0 : tile_gutter;
                    OA_CHECK(tile.texture.x + before == tile.content.x);
                    OA_CHECK(tile.texture.width == tile.content.width + before + after);
                    OA_CHECK(tile.source.x == before && tile.source.width == tile.content.width);
                    if (tiled)
                        OA_CHECK(tile.texture.width <= grid.tile_size);
                }
                OA_CHECK(next == width);
                next = 0;
                for (uint32_t row = 0; row < grid.rows; ++row) {
                    const Tile tile = tile_at(grid, grid.columns - 1, row);
                    OA_CHECK(tile.content.y == next);
                    next = tile.content.y + tile.content.height;
                    const uint32_t before = row == 0 ? 0 : tile_gutter;
                    const uint32_t after = row + 1 == grid.rows ? 0 : tile_gutter;
                    OA_CHECK(tile.texture.y + before == tile.content.y);
                    OA_CHECK(tile.texture.height == tile.content.height + before + after);
                    OA_CHECK(tile.source.y == before);
                    if (tiled)
                        OA_CHECK(tile.texture.height <= grid.tile_size);
                }
                OA_CHECK(next == height);
            }
        }
    }
    OA_CHECK(grids == 7 * 18 * 3);
}

// ---------------------------------------------------------------------------
// Device resets and the layers' formats

void test_device_resets() {
    constexpr uint64_t second = 1000;
    // The third reset within 60 s rebuilds, and the count then starts again.
    ResetWatch watch;
    OA_CHECK(!note_device_reset(watch, 0));
    OA_CHECK(!note_device_reset(watch, 20 * second));
    OA_CHECK(note_device_reset(watch, 59 * second));
    OA_CHECK(!note_device_reset(watch, 60 * second));
    OA_CHECK(!note_device_reset(watch, 61 * second));
    OA_CHECK(note_device_reset(watch, 62 * second));
    // Three spread over 61 s do not; the first falls out of the window.
    ResetWatch spread;
    OA_CHECK(!note_device_reset(spread, 0));
    OA_CHECK(!note_device_reset(spread, 30 * second));
    OA_CHECK(!note_device_reset(spread, 61 * second));
    OA_CHECK(note_device_reset(spread, 62 * second));
    // A reset exactly 60 s after the first is outside its window.
    ResetWatch edge;
    OA_CHECK(!note_device_reset(edge, 0));
    OA_CHECK(!note_device_reset(edge, 1));
    OA_CHECK(!note_device_reset(edge, reset_window_ms));
    OA_CHECK(!note_device_reset(edge, reset_window_ms + 1));
    OA_CHECK(note_device_reset(edge, reset_window_ms + 2));
}

void test_layer_formats() {
    struct Case {
        bool software;
        bool rgb565_window;
        bool named;
        LayerFormat opaque;
        LayerFormat loading;
        LayerFormat front_end;
    };

    const Case cases[] = {
        // SDL's software renderer keeps today's formats, named or not.
        {true, false, false, LayerFormat::xrgb8888, LayerFormat::xrgb8888, LayerFormat::rgb24},
        {true, true, false, LayerFormat::rgb565, LayerFormat::xrgb8888, LayerFormat::rgb24},
        {true, false, true, LayerFormat::xrgb8888, LayerFormat::xrgb8888, LayerFormat::rgb24},
        {true, true, true, LayerFormat::rgb565, LayerFormat::xrgb8888, LayerFormat::rgb24},
        // Any driver SDL_RENDER_DRIVER names keeps them too, RGB565 aside.
        {false, false, true, LayerFormat::xrgb8888, LayerFormat::xrgb8888, LayerFormat::rgb24},
        {false, true, true, LayerFormat::xrgb8888, LayerFormat::xrgb8888, LayerFormat::rgb24},
        // A hardware driver of the walk gets ARGB8888 for every opaque layer.
        {false, false, false, LayerFormat::argb8888, LayerFormat::argb8888, LayerFormat::argb8888},
        {false, true, false, LayerFormat::argb8888, LayerFormat::argb8888, LayerFormat::argb8888},
    };
    for (const auto& c : cases) {
        const LayerFormats formats = layer_formats(c.software, c.rgb565_window, c.named);
        OA_CHECK(formats.opaque == c.opaque);
        OA_CHECK(formats.loading == c.loading);
        OA_CHECK(formats.front_end == c.front_end);
    }
}

} // namespace

// ---------------------------------------------------------------------------
// Acting on the tier

/// The video drivers that draw no window, in any letter case, and the
/// command line's flag.
void test_windowless_and_flag() {
    for (const std::string_view driver : {"dummy", "offscreen", "DUMMY", "Offscreen"})
        OA_CHECK(windowless_video_driver(driver));
    for (const std::string_view driver :
         {"cocoa", "windows", "x11", "wayland", "", "dumm", "dummy "})
        OA_CHECK(!windowless_video_driver(driver));
    OA_CHECK(acceleration_flag(std::nullopt) == AccelerationFlag::none);
    OA_CHECK(acceleration_flag(HardwareAcceleration::off) == AccelerationFlag::off);
    OA_CHECK(acceleration_flag(HardwareAcceleration::basic) == AccelerationFlag::basic);
    OA_CHECK(acceleration_flag(HardwareAcceleration::full) == AccelerationFlag::full);
    // A flag decides over the setting; without one the setting stands.
    for (const HardwareAcceleration setting :
         {HardwareAcceleration::off, HardwareAcceleration::basic, HardwareAcceleration::full}) {
        OA_CHECK(acceleration_asked(AccelerationFlag::none, setting) == setting);
        OA_CHECK(acceleration_asked(AccelerationFlag::off, setting) == HardwareAcceleration::off);
        OA_CHECK(
            acceleration_asked(AccelerationFlag::basic, setting) == HardwareAcceleration::basic
        );
        OA_CHECK(acceleration_asked(AccelerationFlag::full, setting) == HardwareAcceleration::full);
    }
    OA_CHECK(!flag_asks_for_card(AccelerationFlag::none));
    OA_CHECK(!flag_asks_for_card(AccelerationFlag::off));
    OA_CHECK(flag_asks_for_card(AccelerationFlag::basic));
    OA_CHECK(flag_asks_for_card(AccelerationFlag::full));
}

/// What the host does for each decision, with the presentation on and off.
void test_tier_action() {
    const TierDecision accelerated{RenderTier::accelerated, TierReason::accelerated};
    const TierDecision due{RenderTier::standard, TierReason::function_test_due};
    OA_CHECK(tier_action(accelerated, false) == TierAction::switch_on);
    OA_CHECK(tier_action(accelerated, true) == TierAction::none);
    OA_CHECK(tier_action(due, false) == TierAction::run_function_test);
    for (const TierReason reason :
         {TierReason::no_renderer,
          TierReason::director_frame,
          TierReason::memory,
          TierReason::flag_off,
          TierReason::environment,
          TierReason::setting_off,
          TierReason::not_capable,
          TierReason::function_test_failed,
          TierReason::records_unreadable,
          TierReason::accelerated_unusable,
          TierReason::dropped,
          TierReason::device_lost,
          TierReason::waiting_for_match_end,
          TierReason::trial_unwritten}) {
        const TierDecision standard{RenderTier::standard, reason};
        OA_CHECK(tier_action(standard, true) == TierAction::switch_off);
        OA_CHECK(tier_action(standard, false) == TierAction::none);
    }
}

/// Off then On, or Restore defaults, lets a failed or unwritten function
/// test run again and lifts every drop but the memory guard's; a passed
/// test stays passed.
void test_forget_failures() {
    TierInputs inputs;
    inputs.function_test = FunctionTest::failed;
    inputs.drop = Drop::driver_failure;
    forget_failures(inputs);
    OA_CHECK(inputs.function_test == FunctionTest::not_run);
    OA_CHECK(inputs.drop == Drop::none);
    inputs.function_test = FunctionTest::trial_unwritten;
    inputs.drop = Drop::memory;
    forget_failures(inputs);
    OA_CHECK(inputs.function_test == FunctionTest::not_run);
    OA_CHECK(inputs.drop == Drop::memory);
    inputs.function_test = FunctionTest::passed;
    for (const Drop drop : {Drop::engine_fault, Drop::stall, Drop::path_trial_unwritten}) {
        inputs.drop = drop;
        forget_failures(inputs);
        OA_CHECK(inputs.function_test == FunctionTest::passed);
        OA_CHECK(inputs.drop == Drop::none);
    }
    // Full's drops likewise, but for the memory guard's.
    inputs.full_drop = FullDrop::memory;
    forget_failures(inputs);
    OA_CHECK(inputs.full_drop == FullDrop::memory);
    inputs.full_drop = FullDrop::card_failure;
    forget_failures(inputs);
    OA_CHECK(inputs.full_drop == FullDrop::none);
}

/// A stand-in for the start-up function test: what it finds, and how often
/// it ran.
struct StandInTest {
    bool passes{true}; ///< it finds the renderer draws right
    int runs{};        ///< the times it ran
    /// Its trial record cannot be written, so it is skipped.
    bool trial_unwritable{};
    int skipped{}; ///< the times it was skipped for its trial
};

/// Runs the stand-in function test (FunctionTestHooks::run).
///
/// @param context the StandInTest
/// @return FunctionTest::trial_unwritten where its trial cannot be written,
///     else FunctionTest::passed or FunctionTest::failed, as it finds
FunctionTest run_stand_in_test(void* context) {
    auto& test = *static_cast<StandInTest*>(context);
    if (test.trial_unwritable) {
        ++test.skipped;
        return FunctionTest::trial_unwritten;
    }
    ++test.runs;
    return test.passes ? FunctionTest::passed : FunctionTest::failed;
}

/// Steps one frame as the game does (step_tier), and switches the
/// presentation as the step asks.
///
/// @param[in,out] inputs the run's facts
/// @param[in,out] presentation_on the accelerated presentation is on
/// @param[in,out] test the stand-in function test
/// @return the frame's decision
TierDecision drawn_frame(TierInputs& inputs, bool& presentation_on, StandInTest& test) {
    const TierStep step = step_tier(inputs, presentation_on, {&test, run_stand_in_test});
    OA_CHECK(step.action != TierAction::run_function_test);
    if (step.action == TierAction::switch_on)
        presentation_on = true;
    else if (step.action == TierAction::switch_off)
        presentation_on = false;
    return step.decision;
}

/// One frame's step: the function test runs only when only it is missing
/// and its result is kept; with nothing to run it the frame stays standard
/// and a presentation left on is switched off; and in a shared game each
/// frame's decision is noted, a lost device apart, while outside a match
/// the note changes nothing.
void test_step_tier() {
    TierInputs inputs;
    inputs.renderer = true;
    inputs.memory = 8 * gibibyte;
    inputs.setting = HardwareAcceleration::basic;
    StandInTest test;
    const FunctionTestHooks hooks{&test, run_stand_in_test};
    TierStep step = step_tier(inputs, true, {});
    OA_CHECK(step.decision.reason == TierReason::function_test_due);
    OA_CHECK(step.action == TierAction::switch_off);
    OA_CHECK(step_tier(inputs, false, {}).action == TierAction::none);
    OA_CHECK(inputs.function_test == FunctionTest::not_run);
    step = step_tier(inputs, false, hooks);
    OA_CHECK(step.decision.tier == RenderTier::accelerated && step.action == TierAction::switch_on);
    OA_CHECK(test.runs == 1 && inputs.function_test == FunctionTest::passed);
    step = step_tier(inputs, true, hooks);
    OA_CHECK(step.action == TierAction::none && test.runs == 1);
    // A failure is kept, and the test does not run again.
    TierInputs failing = inputs;
    failing.function_test = FunctionTest::not_run;
    StandInTest fails{false, 0};
    const FunctionTestHooks failing_hooks{&fails, run_stand_in_test};
    step = step_tier(failing, false, failing_hooks);
    OA_CHECK(step.decision.reason == TierReason::function_test_failed);
    OA_CHECK(step.action == TierAction::none);
    step = step_tier(failing, false, failing_hooks);
    OA_CHECK(fails.runs == 1 && failing.function_test == FunctionTest::failed);
    // A shared game that began accelerated: a lost device switches the
    // presentation off but keeps the gate; Off closes it until the end.
    begin_match(inputs.match, MatchKind::shared_game, true);
    inputs.device_lost = true;
    step = step_tier(inputs, true, hooks);
    OA_CHECK(step.decision.reason == TierReason::device_lost);
    OA_CHECK(step.action == TierAction::switch_off && inputs.match.accelerated);
    inputs.device_lost = false;
    OA_CHECK(step_tier(inputs, false, hooks).action == TierAction::switch_on);
    inputs.setting = HardwareAcceleration::off;
    OA_CHECK(step_tier(inputs, true, hooks).action == TierAction::switch_off);
    OA_CHECK(!inputs.match.accelerated);
    inputs.setting = HardwareAcceleration::basic;
    step = step_tier(inputs, false, hooks);
    OA_CHECK(step.decision.reason == TierReason::waiting_for_match_end);
    OA_CHECK(step.action == TierAction::none);
    end_match(inputs.match);
    OA_CHECK(step_tier(inputs, false, hooks).action == TierAction::switch_on);
    inputs.setting = HardwareAcceleration::off;
    OA_CHECK(step_tier(inputs, true, hooks).action == TierAction::switch_off);
    OA_CHECK(inputs.match.kind == MatchKind::none && !inputs.match.accelerated);
    OA_CHECK(test.runs == 1);
}

/// A player's run as the game drives it (step_tier): on the player's own
/// profile the start writes the trial and runs the function test at once,
/// and where the trial cannot be written keeps the standard tier until the
/// player sets the setting to Off and back; --hardware-acceleration and a
/// named preferences file that sets the setting to Basic or Full test at
/// start; then the setting set to Off and back, a
/// shared game and a replay, a failed function test retried, and the runs
/// that stay on the standard tier whatever happens: a named preferences
/// file at its default, the dummy video driver, under 2 GiB, headless and
/// the director.
void test_host_runs() {
    constexpr uint64_t eight_gibibytes = 8 * gibibyte;
    TierInputs player;
    player.renderer = true;
    player.memory = eight_gibibytes;
    player.players_own_profile = true;
    player.setting = HardwareAcceleration::full;
    player.capability = Capability::capable;
    TierInputs named_on = player;
    named_on.players_own_profile = false;

    // The player's own profile whose trial cannot be written: the test is
    // skipped and the start stays standard, trying no more; Off then Basic
    // writes the trial and runs the test once, and the frame is accelerated
    // from then.
    {
        TierInputs inputs = player;
        bool on = false;
        StandInTest test;
        test.trial_unwritable = true;
        OA_CHECK(drawn_frame(inputs, on, test).reason == TierReason::trial_unwritten);
        OA_CHECK(drawn_frame(inputs, on, test).reason == TierReason::trial_unwritten);
        OA_CHECK(!on && test.runs == 0 && test.skipped == 1);
        inputs.setting = HardwareAcceleration::off;
        OA_CHECK(drawn_frame(inputs, on, test).reason == TierReason::setting_off);
        forget_failures(inputs);
        inputs.setting = HardwareAcceleration::basic;
        test.trial_unwritable = false;
        OA_CHECK(drawn_frame(inputs, on, test).tier == RenderTier::accelerated);
        OA_CHECK(on && test.runs == 1);
        // Off applies at once, Basic again at once, with no second test.
        inputs.setting = HardwareAcceleration::off;
        OA_CHECK(drawn_frame(inputs, on, test).reason == TierReason::setting_off);
        OA_CHECK(!on);
        inputs.setting = HardwareAcceleration::basic;
        OA_CHECK(drawn_frame(inputs, on, test).tier == RenderTier::accelerated);
        OA_CHECK(on && test.runs == 1);
        // A shared game that begins accelerated stays so; Off in it applies
        // at once, and Basic waits for its end.
        begin_match(inputs.match, MatchKind::shared_game, on);
        OA_CHECK(drawn_frame(inputs, on, test).tier == RenderTier::accelerated);
        inputs.setting = HardwareAcceleration::off;
        OA_CHECK(drawn_frame(inputs, on, test).reason == TierReason::setting_off);
        OA_CHECK(!on);
        inputs.setting = HardwareAcceleration::basic;
        OA_CHECK(drawn_frame(inputs, on, test).reason == TierReason::waiting_for_match_end);
        OA_CHECK(!on);
        end_match(inputs.match);
        OA_CHECK(drawn_frame(inputs, on, test).tier == RenderTier::accelerated);
        OA_CHECK(on && test.runs == 1);
    }
    // The player's own profile, with --hardware-acceleration or without,
    // and a named preferences file that sets the setting to Full, test at
    // start.
    TierInputs flagged = player;
    flagged.flag = AccelerationFlag::full;
    for (TierInputs inputs : {player, flagged, named_on}) {
        bool on = false;
        StandInTest test;
        OA_CHECK(card_tier(drawn_frame(inputs, on, test).tier));
        OA_CHECK(on && test.runs == 1);
    }
    // The setting Off at start: no test until it is set to Basic, and none
    // in a replay until it ends.
    {
        TierInputs inputs = named_on;
        inputs.setting = HardwareAcceleration::off;
        bool on = false;
        StandInTest test;
        OA_CHECK(drawn_frame(inputs, on, test).reason == TierReason::setting_off);
        begin_match(inputs.match, MatchKind::replay, on);
        inputs.setting = HardwareAcceleration::basic;
        OA_CHECK(drawn_frame(inputs, on, test).reason == TierReason::waiting_for_match_end);
        OA_CHECK(!on && test.runs == 0);
        end_match(inputs.match);
        OA_CHECK(drawn_frame(inputs, on, test).tier == RenderTier::accelerated);
        OA_CHECK(on && test.runs == 1);
    }
    // A failed test keeps the standard tier and does not run again until
    // Off and back.
    {
        TierInputs inputs = named_on;
        bool on = false;
        StandInTest test{false, 0};
        OA_CHECK(drawn_frame(inputs, on, test).reason == TierReason::function_test_failed);
        OA_CHECK(drawn_frame(inputs, on, test).reason == TierReason::function_test_failed);
        OA_CHECK(!on && test.runs == 1);
        forget_failures(inputs);
        test.passes = true;
        OA_CHECK(card_tier(drawn_frame(inputs, on, test).tier));
        OA_CHECK(on && test.runs == 2);
        // A drop keeps it standard until forgotten.
        inputs.drop = Drop::driver_failure;
        OA_CHECK(drawn_frame(inputs, on, test).reason == TierReason::dropped);
        OA_CHECK(!on);
        forget_failures(inputs);
        OA_CHECK(card_tier(drawn_frame(inputs, on, test).tier));
        OA_CHECK(test.runs == 2);
    }

    // The runs that never test and never accelerate.
    struct Standard {
        const char* name;
        void (*change)(TierInputs&);
        TierReason reason;
    };

    const Standard standards[] = {
        {"a named preferences file at its default",
         [](TierInputs& i) {
             i.players_own_profile = false;
             i.setting = HardwareAcceleration::off;
         },
         TierReason::setting_off},
        {"the dummy video driver",
         [](TierInputs& i) { i.virtual_video_driver = true; },
         TierReason::environment},
        {"SDL_RENDER_DRIVER",
         [](TierInputs& i) { i.render_driver_named = true; },
         TierReason::environment},
        {"one byte under 2 GiB with both flags",
         [](TierInputs& i) {
             i.memory = smallest_accelerated_memory - 1;
             i.flag = AccelerationFlag::full;
             i.force_capable = true;
         },
         TierReason::memory},
        {"memory not reported", [](TierInputs& i) { i.memory = 0; }, TierReason::memory},
        {"--no-hardware-acceleration",
         [](TierInputs& i) { i.flag = AccelerationFlag::off; },
         TierReason::flag_off},
        {"headless", [](TierInputs& i) { i.renderer = false; }, TierReason::no_renderer},
        {"the director",
         [](TierInputs& i) { i.director_frame = true; },
         TierReason::director_frame},
        {"SDL's software renderer",
         [](TierInputs& i) { i.capability = Capability::software_renderer; },
         TierReason::not_capable},
    };
    for (const auto& run : standards) {
        TierInputs inputs = player;
        run.change(inputs);
        bool on = false;
        StandInTest test;
        const TierDecision decision = drawn_frame(inputs, on, test);
        if (decision.reason != run.reason || on || test.runs != 0)
            std::fprintf(stderr, "standard run: %s\n", run.name);
        OA_CHECK(decision.reason == run.reason);
        OA_CHECK(!on && test.runs == 0);
    }
    // The dummy video driver with --hardware-acceleration and
    // --force-capable, as native-render-tiers runs: SDL's software renderer
    // is tested and accelerated, but never at the threshold less a byte.
    {
        TierInputs inputs = player;
        inputs.virtual_video_driver = true;
        inputs.players_own_profile = false;
        inputs.setting = HardwareAcceleration::off;
        inputs.flag = AccelerationFlag::full;
        inputs.force_capable = true;
        inputs.capability = Capability::software_renderer;
        inputs.memory = smallest_accelerated_memory;
        bool on = false;
        StandInTest test;
        // The flag forces the Full tier, which the check draws its Full cases in.
        OA_CHECK(drawn_frame(inputs, on, test).tier == RenderTier::full);
        OA_CHECK(on && test.runs == 1);
    }
}

/// A replay whose loading screen is the first thing drawn takes the tier
/// decided then, including the function test where only that was missing.
/// A shared game begun with the presentation still off keeps the standard
/// tier until it ends. A replay opened after the decision, as from the
/// Library, keeps the tier that decision gave it.
void test_replay_before_first_decision() {
    constexpr uint64_t eight_gibibytes = 8 * gibibyte;

    // Full on the command line, the test not yet run, the presentation off.
    // The decision runs the test and the replay begins in Full. Basic
    // applies at once; Full again waits for the replay to end. A pending
    // replay after a content switch is this run: the new start has drawn
    // no frame.
    {
        TierInputs inputs;
        inputs.renderer = true;
        inputs.memory = eight_gibibytes;
        inputs.players_own_profile = false;
        inputs.setting = HardwareAcceleration::off;
        inputs.flag = AccelerationFlag::full;
        inputs.force_capable = true;
        inputs.capability = Capability::software_renderer;
        inputs.virtual_video_driver = true;
        inputs.function_test = FunctionTest::not_run;
        bool on = false;
        StandInTest test;
        const TierDecision decided = drawn_frame(inputs, on, test);
        OA_CHECK(decided.tier == RenderTier::full);
        OA_CHECK(decided.full == FullReason::full);
        OA_CHECK(on && test.runs == 1);
        begin_match(inputs.match, MatchKind::replay, on, decided.tier == RenderTier::full);
        OA_CHECK(inputs.match.kind == MatchKind::replay);
        OA_CHECK(inputs.match.accelerated && inputs.match.full);
        OA_CHECK(drawn_frame(inputs, on, test).tier == RenderTier::full);
        OA_CHECK(test.runs == 1);
        inputs.flag = AccelerationFlag::basic;
        OA_CHECK(drawn_frame(inputs, on, test).tier == RenderTier::accelerated);
        OA_CHECK(on && !inputs.match.full);
        inputs.flag = AccelerationFlag::full;
        const TierDecision waiting = drawn_frame(inputs, on, test);
        OA_CHECK(waiting.tier == RenderTier::accelerated);
        OA_CHECK(waiting.full == FullReason::waiting_for_match_end);
        OA_CHECK(test.runs == 1);
    }

    // The same facts for a shared game, closed on a presentation that has
    // not switched on. The test does not run, and Full waits for the end.
    {
        TierInputs inputs;
        inputs.renderer = true;
        inputs.memory = eight_gibibytes;
        inputs.players_own_profile = false;
        inputs.setting = HardwareAcceleration::off;
        inputs.flag = AccelerationFlag::full;
        inputs.force_capable = true;
        inputs.capability = Capability::software_renderer;
        inputs.virtual_video_driver = true;
        inputs.function_test = FunctionTest::not_run;
        begin_match(inputs.match, MatchKind::shared_game, false, false);
        bool on = false;
        StandInTest test;
        OA_CHECK(drawn_frame(inputs, on, test).reason == TierReason::waiting_for_match_end);
        OA_CHECK(!on && !inputs.match.accelerated && test.runs == 0);
        end_match(inputs.match);
        OA_CHECK(drawn_frame(inputs, on, test).tier == RenderTier::full);
        OA_CHECK(on && test.runs == 1);
    }

    // Opened after the menus, already in Full: it stays in Full, and the
    // test does not run again.
    {
        TierInputs inputs = accelerated_run();
        inputs.setting = HardwareAcceleration::full;
        bool on = true;
        StandInTest test;
        const TierDecision decided = drawn_frame(inputs, on, test);
        OA_CHECK(decided.tier == RenderTier::full && test.runs == 0);
        begin_match(inputs.match, MatchKind::replay, on, true);
        OA_CHECK(inputs.match.accelerated && inputs.match.full);
        OA_CHECK(drawn_frame(inputs, on, test).tier == RenderTier::full);
        OA_CHECK(on && test.runs == 0);
    }

    // Chosen Off, decided before the gate. The replay stays on the
    // processor, and Full during it waits until the replay ends.
    {
        TierInputs inputs = accelerated_run();
        inputs.function_test = FunctionTest::not_run;
        inputs.setting = HardwareAcceleration::off;
        bool on = false;
        StandInTest test;
        OA_CHECK(drawn_frame(inputs, on, test).reason == TierReason::setting_off);
        OA_CHECK(test.runs == 0);
        begin_match(inputs.match, MatchKind::replay, on, false);
        OA_CHECK(!inputs.match.accelerated && !inputs.match.full);
        inputs.setting = HardwareAcceleration::full;
        OA_CHECK(drawn_frame(inputs, on, test).reason == TierReason::waiting_for_match_end);
        OA_CHECK(!on && test.runs == 0);
        end_match(inputs.match);
        OA_CHECK(drawn_frame(inputs, on, test).tier == RenderTier::full);
        OA_CHECK(on && test.runs == 1);
    }

    // The function test fails before the gate. The replay stays standard.
    {
        TierInputs inputs = accelerated_run();
        inputs.function_test = FunctionTest::not_run;
        inputs.setting = HardwareAcceleration::full;
        bool on = false;
        StandInTest test{false, 0};
        OA_CHECK(drawn_frame(inputs, on, test).reason == TierReason::function_test_failed);
        begin_match(inputs.match, MatchKind::replay, on, false);
        OA_CHECK(!inputs.match.accelerated);
        OA_CHECK(drawn_frame(inputs, on, test).reason == TierReason::function_test_failed);
        OA_CHECK(!on && test.runs == 1);
    }

    // The dummy driver with no flag that asks for the card, as a check that
    // does not name Full. The replay stays on the processor.
    {
        TierInputs inputs = accelerated_run();
        inputs.players_own_profile = false;
        inputs.setting = HardwareAcceleration::full;
        inputs.virtual_video_driver = true;
        inputs.function_test = FunctionTest::not_run;
        bool on = false;
        StandInTest test;
        OA_CHECK(drawn_frame(inputs, on, test).reason == TierReason::environment);
        begin_match(inputs.match, MatchKind::replay, on, false);
        OA_CHECK(!inputs.match.accelerated && !inputs.match.full);
        OA_CHECK(drawn_frame(inputs, on, test).reason == TierReason::environment);
        OA_CHECK(!on && test.runs == 0);
    }
}

int main() {
    test_texture_limit();
    test_assess_renderer();
    test_decide_by_table();
    test_decide_full();
    test_decide_every_combination();
    test_function_test_gate();
    test_shared_match_gate();
    test_creation_walk_by_table();
    test_environment_and_rebuilds();
    test_creation_walk_never_fails_for_records();
    test_stalls();
    test_device_resets();
    test_layer_formats();
    test_start_budget();
    test_start_rung();
    test_resume_and_step_up();
    test_rung_without();
    test_prescale();
    test_supersampling();
    test_chrome_filter();
    test_chrome_filter_at_the_display();
    test_frame_fit_and_filter();
    test_native_density_by_table();
    test_native_density_every_combination();
    test_tiles_by_table();
    test_tiles_cover_every_texel();
    test_windowless_and_flag();
    test_tier_action();
    test_forget_failures();
    test_step_tier();
    test_host_runs();
    test_replay_before_first_decision();
    return oa::test::check_exit_status();
}

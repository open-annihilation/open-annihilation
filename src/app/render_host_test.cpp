// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The walk of the render drivers. First through stand-in hooks over made-up
// drivers: SDL's order walked one driver at a time, every refusal logged
// with its reason, the first driver that starts kept and nothing after it
// tried; drivers recorded as failed skipped, but never software; the
// framebuffer hint set just before software once a hardware driver
// refused, as "0" where the window has a framebuffer of its own and
// otherwise as the drivers still trusted, or the first alone where the
// hint takes no list; a hint not taken logged while the walk goes on; a
// second walk from the top, logged once, when the records would leave
// nothing able to present; and under SDL_RENDER_DRIVER SDL's own call
// alone. Then on SDL's dummy video driver: the walk makes the renderer
// SDL's own choice makes, after every earlier driver of SDL's order
// refused, with the hint "0" (read back only: SDL's dummy video driver
// never presents through a texture, so the hint changes nothing there); a
// hint SDL already holds at a higher priority is refused with no reason,
// and the line says so; a named driver that does not exist ends the start
// after one attempt, with the hint unset; software named is made with the
// hint unset. With --case named-missing, the missing driver is named by
// the environment variable itself. Rebuilds: through stand-in hooks, the
// drivers after the failed one, then software with the hint, SDL's whole
// order again after software, and under SDL_RENDER_DRIVER the list's later
// drivers then software; on the dummy video driver a rebuild makes
// software again, throws when nothing starts, a lost device noted before
// the runtime exists is mended by service, and a named list's rebuild
// tries software alone. The faults refuse drivers with their own reason
// and stand in for the texture limit and the device's state. The start-up
// function test passes on SDL's software renderer, which has no PIXELART,
// and puts back the render target and the draw colour, and fails the
// reduction by half or the known pattern drawn NEAREST; the start's tier on
// the dummy video driver, standard with the reason in the line, or with
// --hardware-acceleration and --force-capable accelerated after the
// function test passed, from 2 GiB, and standard where it failed; and a
// rebuild drops the tier for the run. The name a failing call is struck
// under. The records on the dummy video driver in scratch folders: the
// trial before the function test, the sentinel, a lost device recorded, a
// clean end, and a profile that cannot be written, which logs each file's
// failure once and keeps the start standard with the test skipped.
#include "render_host.hpp"

#include "oa/platform/preferences.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/engine_settings.hpp"
#include "oa/test/scratch_directory.hpp"

#include <SDL3/SDL.h>

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <tuple>
#include <vector>

namespace {

using oa::app::CreationAttempt;
using oa::app::CreationHooks;
using oa::app::CreationOutcome;
using oa::app::RendererHost;
using oa::app::render_policy::CreationInputs;

/// The window the dummy cases make a renderer for, pixels.
constexpr int window_width = 64;
constexpr int window_height = 48;

/// What the stand-in hooks did, in order.
struct Stand {
    std::vector<std::string_view> refusing{}; ///< drivers that refuse; others start
    bool refuse_choice{};                     ///< SDL's own choice refuses
    bool refuse_hint{};                       ///< the hint is not taken
    std::vector<std::string> calls{};         ///< "create <driver>", "create *" or "hint <value>"
    std::vector<std::string> log{};           ///< the lines logged
};

/// Takes or refuses the hint as the stand-in says.
///
/// @param context the Stand
/// @param value the hint's value
/// @param[out] error the reason, when refused
/// @return false when the stand-in refuses hints
bool stand_hint(void* context, const std::string& value, std::string& error) {
    auto& stand = *static_cast<Stand*>(context);
    stand.calls.push_back("hint " + value);
    if (!stand.refuse_hint)
        return true;
    error = "An environment variable is taking priority";
    return false;
}

/// Starts or refuses a driver as the stand-in says.
///
/// @param context the Stand
/// @param driver the driver; empty for SDL's own choice
/// @param[out] error the reason, when refused
/// @return true when the driver starts
bool stand_create(void* context, const std::string& driver, std::string& error) {
    auto& stand = *static_cast<Stand*>(context);
    if (driver.empty()) {
        stand.calls.emplace_back("create *");
        if (!stand.refuse_choice)
            return true;
        error = "missing not available";
        return false;
    }
    stand.calls.push_back("create " + driver);
    for (const std::string_view refusing : stand.refusing)
        if (refusing == driver) {
            error = driver + " would not start";
            return false;
        }
    return true;
}

/// Keeps a logged line.
///
/// @param context the Stand
/// @param line the line
void stand_log(void* context, const std::string& line) {
    static_cast<Stand*>(context)->log.push_back(line);
}

/// Walks the drivers through the stand-in.
///
/// @param inputs the walk's inputs
/// @param[in,out] stand the stand-in
/// @return the walk's outcome
CreationOutcome walk(const CreationInputs& inputs, Stand& stand) {
    CreationHooks hooks;
    hooks.context = &stand;
    hooks.set_framebuffer_hint = stand_hint;
    hooks.create = stand_create;
    hooks.log = stand_log;
    return oa::app::walk_render_drivers(inputs, hooks);
}

/// Returns a driver's refusal line as the stand-in refuses it.
///
/// @param driver the driver
/// @return the line
std::string refused(std::string_view driver) {
    return oa::app::refusal_log_line(driver, std::string(driver) + " would not start");
}

/// SDL's order in the stand-in cases: three hardware drivers, then software.
constexpr std::string_view driver_alpha = "alpha";
constexpr std::string_view driver_beta = "beta";
constexpr std::string_view driver_gamma = "gamma";
constexpr std::string_view software = oa::app::render_policy::software_driver;
const std::vector<std::string_view> sdl_order{driver_alpha, driver_beta, driver_gamma, software};

/// Returns the inputs of a start over the stand-in's order.
///
/// @param native whether the window has a framebuffer of its own
/// @param list whether the framebuffer hint takes a list
/// @return the inputs
CreationInputs start_inputs(bool native, bool list) {
    CreationInputs inputs;
    inputs.sdl_order = sdl_order;
    inputs.native_window_framebuffer = native;
    inputs.hint_takes_list = list;
    return inputs;
}

/// The log lines name the driver and the reason, after the game's prefix.
void test_log_lines() {
    OA_CHECK(
        oa::app::refusal_log_line("alpha", "no device") ==
        "open-annihilation: graphics: renderer alpha refused: no device"
    );
    OA_CHECK(
        oa::app::framebuffer_hint_log_line("0", "taken elsewhere") ==
        "open-annihilation: graphics: the framebuffer hint \"0\" was not taken: taken elsewhere"
    );
    OA_CHECK(oa::app::second_walk_log_line().starts_with("open-annihilation: graphics: "));
}

/// With every driver working the first of SDL's order is made, as SDL's
/// own choice makes it: one attempt, no hint, nothing logged.
void test_every_driver_works() {
    Stand stand;
    const CreationOutcome outcome = walk(start_inputs(true, true), stand);
    OA_CHECK(outcome.created);
    OA_CHECK(outcome.error.empty());
    OA_CHECK(outcome.attempts.size() == 1);
    OA_CHECK(outcome.attempts.size() == 1 && outcome.attempts[0].driver == driver_alpha);
    OA_CHECK(outcome.attempts.size() == 1 && outcome.attempts[0].created);
    OA_CHECK(stand.calls == std::vector<std::string>{"create alpha"});
    OA_CHECK(stand.log.empty());
}

/// The walk goes down SDL's order one driver at a time, logs each refusal
/// with its reason and stops at the first that starts, with no hint.
void test_walk_in_order() {
    Stand stand;
    stand.refusing = {driver_alpha, driver_beta};
    const CreationOutcome outcome = walk(start_inputs(true, true), stand);
    OA_CHECK(outcome.created);
    OA_CHECK(
        stand.calls == std::vector<std::string>({"create alpha", "create beta", "create gamma"})
    );
    OA_CHECK(stand.log == std::vector<std::string>({refused(driver_alpha), refused(driver_beta)}));
    OA_CHECK(outcome.attempts.size() == 3);
    if (outcome.attempts.size() == 3) {
        OA_CHECK(outcome.attempts[0].driver == driver_alpha && !outcome.attempts[0].created);
        OA_CHECK(outcome.attempts[0].error == "alpha would not start");
        OA_CHECK(outcome.attempts[1].driver == driver_beta && !outcome.attempts[1].created);
        OA_CHECK(outcome.attempts[2].driver == driver_gamma && outcome.attempts[2].created);
        OA_CHECK(outcome.attempts[2].error.empty());
    }
}

/// When every hardware driver refuses, the hint is set just before
/// software: "0" where the window has a framebuffer of its own, else the
/// drivers no record left out, as a list or the first alone.
void test_software_fall_back() {
    Stand native;
    native.refusing = {driver_alpha, driver_beta, driver_gamma};
    const CreationOutcome outcome = walk(start_inputs(true, true), native);
    OA_CHECK(outcome.created);
    OA_CHECK(
        native.calls ==
        std::vector<std::string>(
            {"create alpha", "create beta", "create gamma", "hint 0", "create software"}
        )
    );
    OA_CHECK(
        native.log == std::vector<std::string>(
                          {refused(driver_alpha), refused(driver_beta), refused(driver_gamma)}
                      )
    );
    OA_CHECK(outcome.attempts.size() == 4);
    OA_CHECK(!outcome.attempts.empty() && outcome.attempts.back().driver == software);

    Stand listed;
    listed.refusing = {driver_alpha, driver_beta, driver_gamma};
    OA_CHECK(walk(start_inputs(false, true), listed).created);
    OA_CHECK(listed.calls.size() == 5);
    OA_CHECK(listed.calls.size() == 5 && listed.calls[3] == "hint alpha,beta,gamma");

    Stand single;
    single.refusing = {driver_alpha, driver_beta, driver_gamma};
    OA_CHECK(walk(start_inputs(false, false), single).created);
    OA_CHECK(single.calls.size() == 5 && single.calls[3] == "hint alpha");
}

/// A hint that is not taken is logged and the walk goes on to software.
void test_hint_not_taken() {
    Stand stand;
    stand.refusing = {driver_alpha, driver_beta, driver_gamma};
    stand.refuse_hint = true;
    const CreationOutcome outcome = walk(start_inputs(true, true), stand);
    OA_CHECK(outcome.created);
    OA_CHECK(stand.calls.size() == 5 && stand.calls.back() == "create software");
    OA_CHECK(
        stand.log ==
        std::vector<std::string>(
            {refused(driver_alpha),
             refused(driver_beta),
             refused(driver_gamma),
             oa::app::framebuffer_hint_log_line("0", "An environment variable is taking priority")}
        )
    );
}

/// When software refuses too, nothing is made: every refusal is logged,
/// and the error is the game's usual one with the last reason.
void test_nothing_starts() {
    Stand stand;
    stand.refusing = {driver_alpha, driver_beta, driver_gamma, software};
    const CreationOutcome outcome = walk(start_inputs(true, true), stand);
    OA_CHECK(!outcome.created);
    OA_CHECK(outcome.attempts.size() == 4);
    OA_CHECK(stand.log.size() == 4 && stand.log.back() == refused(software));
    OA_CHECK(outcome.error == "SDL_CreateRenderer: software would not start");

    CreationInputs none;
    Stand nothing;
    const CreationOutcome empty = walk(none, nothing);
    OA_CHECK(!empty.created);
    OA_CHECK(empty.attempts.empty());
    OA_CHECK(empty.error == "SDL_CreateRenderer: no render driver is available");
}

/// A driver recorded as failed is never tried; software is never skipped.
void test_skipping() {
    const std::vector<std::string_view> recorded{driver_alpha};
    CreationInputs inputs = start_inputs(true, true);
    inputs.failed_drivers = recorded;
    Stand stand;
    const CreationOutcome skipped = walk(inputs, stand);
    OA_CHECK(skipped.created && skipped.skipped_by_record && !skipped.records_ignored);
    OA_CHECK(stand.calls == std::vector<std::string>{"create beta"});
    OA_CHECK(skipped.skipped == std::vector<std::string>{"alpha"});

    // A recorded driver after the one made was never passed over.
    const std::vector<std::string_view> around{driver_alpha, driver_gamma};
    inputs.failed_drivers = around;
    Stand before;
    const CreationOutcome passed = walk(inputs, before);
    OA_CHECK(passed.created && passed.attempts.back().driver == driver_beta);
    OA_CHECK(passed.skipped == std::vector<std::string>{"alpha"});
    // Nor is any where the first driver starts.
    const std::vector<std::string_view> later{driver_gamma};
    inputs.failed_drivers = later;
    Stand first;
    const CreationOutcome none = walk(inputs, first);
    OA_CHECK(none.created && none.attempts.back().driver == driver_alpha);
    OA_CHECK(!none.skipped_by_record && none.skipped.empty());

    // Skipping a driver counts as a miss: the hint is set before software.
    const std::vector<std::string_view> all_but_software{
        driver_alpha, driver_beta, driver_gamma, software
    };
    inputs.failed_drivers = all_but_software;
    Stand last;
    OA_CHECK(walk(inputs, last).created);
    OA_CHECK(last.calls == std::vector<std::string>({"hint 0", "create software"}));
    OA_CHECK(last.log.empty());
}

/// The records are advice: where they would leave nothing able to present,
/// the walk starts again from the top with them ignored, and logs that once.
void test_advice_rule() {
    // Every hardware driver recorded, and no framebuffer of the window's own:
    // software would have nothing to present through.
    const std::vector<std::string_view> every_hardware{driver_alpha, driver_beta, driver_gamma};
    CreationInputs inputs = start_inputs(false, true);
    inputs.failed_drivers = every_hardware;
    Stand stand;
    const CreationOutcome outcome = walk(inputs, stand);
    OA_CHECK(outcome.created && outcome.records_ignored && !outcome.skipped_by_record);
    OA_CHECK(outcome.skipped.empty());
    OA_CHECK(stand.calls == std::vector<std::string>{"create alpha"});
    OA_CHECK(stand.log == std::vector<std::string>{oa::app::second_walk_log_line()});

    // One driver recorded and software refused: the second walk tries the
    // recorded driver too, and the line is logged once.
    const std::vector<std::string_view> one{driver_alpha};
    inputs = start_inputs(true, true);
    inputs.failed_drivers = one;
    Stand again;
    again.refusing = {driver_beta, driver_gamma, software};
    const CreationOutcome second = walk(inputs, again);
    OA_CHECK(second.created);
    OA_CHECK(
        again.calls ==
        std::vector<std::string>(
            {"create beta", "create gamma", "hint 0", "create software", "create alpha"}
        )
    );
    OA_CHECK(
        again.log == std::vector<std::string>(
                         {refused(driver_beta),
                          refused(driver_gamma),
                          refused(software),
                          oa::app::second_walk_log_line()}
                     )
    );
    OA_CHECK(!second.attempts.empty() && second.attempts.back().driver == driver_alpha);
}

/// Under SDL_RENDER_DRIVER the start is SDL's own call, once, with no hint
/// and no line of its own; a refusal ends the start with SDL's reason.
void test_environment_start() {
    CreationInputs inputs = start_inputs(true, true);
    inputs.render_driver_named = true;
    Stand works;
    const CreationOutcome made = walk(inputs, works);
    OA_CHECK(made.created);
    OA_CHECK(works.calls == std::vector<std::string>{"create *"});
    OA_CHECK(made.attempts.size() == 1 && made.attempts[0].driver.empty());

    Stand refuses;
    refuses.refuse_choice = true;
    const CreationOutcome failed = walk(inputs, refuses);
    OA_CHECK(!failed.created);
    OA_CHECK(refuses.calls == std::vector<std::string>{"create *"});
    OA_CHECK(refuses.log.empty());
    OA_CHECK(failed.attempts.size() == 1);
    OA_CHECK(failed.error == "SDL_CreateRenderer: missing not available");
}

/// Walks the drivers of a rebuild through the stand-in.
///
/// @param inputs the walk's inputs
/// @param failed the driver that failed
/// @param[in,out] stand the stand-in
/// @return the walk's outcome
CreationOutcome rebuild_walk(const CreationInputs& inputs, std::string_view failed, Stand& stand) {
    CreationHooks hooks;
    hooks.context = &stand;
    hooks.set_framebuffer_hint = stand_hint;
    hooks.create = stand_create;
    hooks.log = stand_log;
    return oa::app::walk_rebuild_drivers(inputs, failed, hooks);
}

/// A rebuild tries the drivers after the one that failed, then software
/// with the hint; after software it walks SDL's whole order again from the
/// top. Under SDL_RENDER_DRIVER it tries the drivers the list names after
/// the failed one, then software, and never SDL's whole order.
void test_rebuild_walks() {
    Stand after_beta;
    after_beta.refusing = {driver_gamma};
    OA_CHECK(rebuild_walk(start_inputs(true, true), driver_beta, after_beta).created);
    OA_CHECK(
        after_beta.calls == std::vector<std::string>({"create gamma", "hint 0", "create software"})
    );
    OA_CHECK(
        oa::app::rebuild_log_line("beta", "it failed") ==
        "open-annihilation: graphics: beta failed: it failed; making another renderer"
    );

    Stand after_software;
    const CreationOutcome again = rebuild_walk(start_inputs(true, true), software, after_software);
    OA_CHECK(again.created);
    OA_CHECK(after_software.calls == std::vector<std::string>{"create alpha"});
    OA_CHECK(after_software.log == std::vector<std::string>{oa::app::second_walk_log_line()});

    const std::vector<std::string_view> named{"opengl", software};
    CreationInputs environment = start_inputs(true, true);
    environment.render_driver_named = true;
    environment.environment_order = named;
    Stand named_after;
    OA_CHECK(rebuild_walk(environment, "opengl", named_after).created);
    OA_CHECK(named_after.calls == std::vector<std::string>({"hint 0", "create software"}));

    Stand named_software;
    named_software.refusing = {software};
    const CreationOutcome fatal = rebuild_walk(environment, software, named_software);
    OA_CHECK(!fatal.created);
    OA_CHECK(named_software.calls == std::vector<std::string>{"create software"});
    OA_CHECK(fatal.error == "SDL_CreateRenderer: software would not start");
}

/// Returns the framebuffer hint SDL holds.
///
/// @return its value; empty when unset
std::string framebuffer_hint() {
    const char* value = SDL_GetHint(SDL_HINT_FRAMEBUFFER_ACCELERATION);
    return value != nullptr ? value : "";
}

/// Starts SDL's video and opens a window, for one case.
///
/// @return the window; null, with the failure counted, when SDL failed
SDL_Window* open_window() {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        OA_CHECK(false);
        return nullptr;
    }
    SDL_Window* window = SDL_CreateWindow("render host test", window_width, window_height, 0);
    OA_CHECK(window != nullptr);
    return window;
}

/// Closes a case's window and SDL, which forgets the hints the case set.
///
/// @param window the window; null is allowed
void close_window(SDL_Window* window) {
    if (window != nullptr)
        SDL_DestroyWindow(window);
    SDL_Quit();
}

/// On the dummy video driver, with no driver named, the walk tries SDL's
/// drivers in SDL's order and ends where SDL's own choice ends, with every
/// earlier driver refused and the hint "0" set before software.
void test_dummy_walk() {
    SDL_Window* window = open_window();
    if (window == nullptr) {
        close_window(window);
        return;
    }
    RendererHost host;
    try {
        host.create(window);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "create: %s\n", error.what());
    }
    OA_CHECK(host.renderer() != nullptr);
    const auto attempts = host.attempts();
    OA_CHECK(!attempts.empty());
    OA_CHECK(static_cast<int>(attempts.size()) <= SDL_GetNumRenderDrivers());
    for (std::size_t index = 0; index < attempts.size(); ++index) {
        const char* name = SDL_GetRenderDriver(static_cast<int>(index));
        OA_CHECK(name != nullptr && attempts[index].driver == name);
        const bool last = index + 1 == attempts.size();
        OA_CHECK(attempts[index].created == last);
        OA_CHECK(attempts[index].error.empty() == last);
    }
    OA_CHECK(host.facts().renderer == oa::app::render_policy::software_driver);
    OA_CHECK(framebuffer_hint() == (attempts.size() > 1 ? "0" : ""));
    if (host.renderer() != nullptr) {
        // SDL's own choice for another window of the same video driver.
        SDL_Window* other = SDL_CreateWindow("render host test", window_width, window_height, 0);
        SDL_Renderer* chosen = other != nullptr ? SDL_CreateRenderer(other, nullptr) : nullptr;
        OA_CHECK(chosen != nullptr);
        if (chosen != nullptr) {
            const char* name = SDL_GetRendererName(chosen);
            OA_CHECK(name != nullptr && host.facts().renderer == name);
            SDL_DestroyRenderer(chosen);
        }
        if (other != nullptr)
            SDL_DestroyWindow(other);
    }
    host.destroy();
    OA_CHECK(host.renderer() == nullptr);
    close_window(window);
}

/// On the dummy video driver, with the framebuffer hint already held at
/// SDL's override priority, SDL refuses the walk's hint without a reason:
/// the line logged says no reason was given rather than repeating the last
/// driver's refusal, and the walk still ends on software.
void test_dummy_hint_held() {
    SDL_Window* window = open_window();
    if (window == nullptr) {
        close_window(window);
        return;
    }
    OA_CHECK(SDL_SetHintWithPriority(SDL_HINT_FRAMEBUFFER_ACCELERATION, "1", SDL_HINT_OVERRIDE));
    RendererHost host;
    std::ostringstream logged;
    std::streambuf* const standard_output = std::cout.rdbuf(logged.rdbuf());
    try {
        host.create(window);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "create: %s\n", error.what());
    }
    std::cout.rdbuf(standard_output);
    OA_CHECK(host.renderer() != nullptr);
    OA_CHECK(host.facts().renderer == oa::app::render_policy::software_driver);
    OA_CHECK(framebuffer_hint() == "1");
    const auto attempts = host.attempts();
    std::vector<std::string> lines;
    std::istringstream reading(logged.str());
    for (std::string line; std::getline(reading, line);)
        lines.push_back(line);
    const std::string not_taken =
        oa::app::framebuffer_hint_log_line("0", oa::app::unexplained_refusal);
    std::size_t not_taken_count = 0;
    for (const std::string& line : lines)
        not_taken_count += line == not_taken ? 1U : 0U;
    OA_CHECK(not_taken_count == (attempts.size() > 1 ? 1U : 0U));
    host.destroy();
    close_window(window);
}

/// A driver that does not exist, named: SDL's own call fails once, the
/// start throws the game's usual error, and no hint is set.
///
/// @param by_hint name it through SDL's hint, as the variable would; false
///     when the environment variable itself names it
void test_named_missing(bool by_hint) {
    SDL_Window* window = open_window();
    if (window == nullptr) {
        close_window(window);
        return;
    }
    if (by_hint)
        OA_CHECK(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "missing"));
    RendererHost host;
    bool thrown = false;
    try {
        host.create(window);
    } catch (const std::runtime_error& error) {
        thrown = std::string_view(error.what()).starts_with(oa::app::renderer_creation_error);
    }
    OA_CHECK(thrown);
    OA_CHECK(host.renderer() == nullptr);
    OA_CHECK(host.attempts().size() == 1);
    OA_CHECK(host.attempts().size() == 1 && host.attempts()[0].driver.empty());
    OA_CHECK(host.attempts().size() == 1 && !host.attempts()[0].created);
    OA_CHECK(framebuffer_hint().empty());
    close_window(window);
}

/// Software named: SDL's own call makes it at once, with no hint.
void test_named_software() {
    SDL_Window* window = open_window();
    if (window == nullptr) {
        close_window(window);
        return;
    }
    OA_CHECK(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "software"));
    RendererHost host;
    try {
        host.create(window);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "create: %s\n", error.what());
    }
    OA_CHECK(host.renderer() != nullptr);
    OA_CHECK(host.facts().renderer == oa::app::render_policy::software_driver);
    OA_CHECK(host.attempts().size() == 1 && host.attempts()[0].created);
    OA_CHECK(framebuffer_hint().empty());
    host.destroy();
    close_window(window);
}

/// Refuses every render driver.
///
/// @return true
bool refuse_every_driver(void*, std::string_view) {
    return true;
}

/// Refuses every render driver but software, as --render-fault create does.
///
/// @param driver the driver
/// @return true for every driver but software
bool refuse_hardware(void*, std::string_view driver) {
    return driver != software;
}

/// Answers that the device is lost.
///
/// @return DeviceState::lost
oa::platform::render_probe::DeviceState answer_lost(void*) {
    return oa::platform::render_probe::DeviceState::lost;
}

/// Answers whether the window's pixels are RGB565 from the test's own answer.
///
/// @param context the answer
/// @return the answer
bool answer_rgb565(void* context) {
    return *static_cast<bool*>(context);
}

/// On the dummy video driver: the faults refuse the hardware drivers with
/// their own reason; a rebuild makes software again, through SDL's whole
/// order; with software refused too it throws and leaves no renderer. The
/// layers keep today's formats, the match's following the window's pixels
/// at each call, and the faults' texture limit and device state stand in
/// for the renderer's.
void test_dummy_rebuild() {
    SDL_Window* window = open_window();
    if (window == nullptr) {
        close_window(window);
        return;
    }
    RendererHost host;
    oa::app::RenderFaultHooks faults;
    faults.refuse_driver = refuse_hardware;
    try {
        host.create(window, faults);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "create: %s\n", error.what());
    }
    OA_CHECK(host.renderer() != nullptr);
    OA_CHECK(host.facts().renderer == software);
    const auto attempts = host.attempts();
    for (std::size_t index = 0; index + 1 < attempts.size(); ++index)
        OA_CHECK(attempts[index].error == oa::app::refused_by_fault);
    const auto& formats = host.layer_formats();
    OA_CHECK(formats.opaque == oa::app::render_policy::LayerFormat::xrgb8888);
    OA_CHECK(formats.loading == oa::app::render_policy::LayerFormat::xrgb8888);
    OA_CHECK(formats.front_end == oa::app::render_policy::LayerFormat::rgb24);
    // The window turns 16-bit and back with the renderer kept: the match's
    // layers follow it, the rest stay as the renderer was made.
    OA_CHECK(host.opaque_format() == oa::app::render_policy::LayerFormat::xrgb8888);
    bool rgb565 = true;
    host.faults().context = &rgb565;
    host.faults().rgb565_window = answer_rgb565;
    OA_CHECK(host.opaque_format() == oa::app::render_policy::LayerFormat::rgb565);
    OA_CHECK(host.layer_formats().opaque == oa::app::render_policy::LayerFormat::xrgb8888);
    OA_CHECK(host.layer_formats().loading == oa::app::render_policy::LayerFormat::xrgb8888);
    OA_CHECK(host.layer_formats().front_end == oa::app::render_policy::LayerFormat::rgb24);
    rgb565 = false;
    OA_CHECK(host.opaque_format() == oa::app::render_policy::LayerFormat::xrgb8888);
    host.faults().rgb565_window = nullptr;
    host.faults().context = nullptr;
    OA_CHECK(host.texture_limit() == 0);
    host.faults().texture_limit = 2048;
    OA_CHECK(host.texture_limit() == 2048);
    OA_CHECK(host.device_state() == oa::platform::render_probe::DeviceState::unknown);
    host.faults().device_state = answer_lost;
    OA_CHECK(host.device_state() == oa::platform::render_probe::DeviceState::lost);
    host.faults().device_state = nullptr;

    host.rebuild("a test");
    OA_CHECK(host.renderer() != nullptr);
    OA_CHECK(host.facts().renderer == software);
    OA_CHECK(!host.attempts().empty() && host.attempts().back().created);
    OA_CHECK(host.texture_limit() == 2048);

    host.faults().refuse_driver = refuse_every_driver;
    bool thrown = false;
    try {
        host.rebuild("a test");
    } catch (const std::runtime_error& error) {
        thrown = std::string_view(error.what()).starts_with(oa::app::renderer_creation_error);
    }
    OA_CHECK(thrown);
    OA_CHECK(host.renderer() == nullptr);
    close_window(window);
}

/// A lost device seen before the runtime exists is noted by take_event and
/// mended by service, which makes the renderer again; other events are not
/// taken.
void test_lost_before_runtime() {
    SDL_Window* window = open_window();
    if (window == nullptr) {
        close_window(window);
        return;
    }
    RendererHost host;
    try {
        host.create(window);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "create: %s\n", error.what());
    }
    SDL_Event key{};
    key.type = SDL_EVENT_KEY_DOWN;
    OA_CHECK(!host.take_event(key));
    SDL_Event reset{};
    reset.type = SDL_EVENT_RENDER_DEVICE_RESET;
    reset.render.windowID = SDL_GetWindowID(window);
    OA_CHECK(host.take_event(reset));
    SDL_Event lost{};
    lost.type = SDL_EVENT_RENDER_DEVICE_LOST;
    lost.render.windowID = SDL_GetWindowID(window);
    OA_CHECK(SDL_PushEvent(&lost));
    SDL_Event taken{};
    bool noted = false;
    while (SDL_PollEvent(&taken))
        noted = host.take_event(taken) || noted;
    OA_CHECK(noted);
    try {
        host.service();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "service: %s\n", error.what());
        OA_CHECK(false);
    }
    OA_CHECK(host.renderer() != nullptr);
    OA_CHECK(host.facts().renderer == software);
    // After software, the rebuild's walk went through SDL's order again
    // from the top, up to software.
    std::size_t software_place = 0;
    while (static_cast<int>(software_place) < SDL_GetNumRenderDrivers() &&
           std::string_view(SDL_GetRenderDriver(static_cast<int>(software_place))) != software)
        ++software_place;
    OA_CHECK(host.attempts().size() == software_place + 1);
    host.destroy();
    close_window(window);
}

/// Under SDL_RENDER_DRIVER standing in as "opengl,software" on the dummy
/// video driver the start makes software; a rebuild then tries software
/// alone, the only driver the list names after it, and the layers keep
/// today's formats.
void test_named_list_rebuild() {
    SDL_Window* window = open_window();
    if (window == nullptr) {
        close_window(window);
        return;
    }
    OA_CHECK(SDL_SetHint(SDL_HINT_RENDER_DRIVER, "opengl,software"));
    RendererHost host;
    try {
        host.create(window);
    } catch (const std::exception& error) {
        std::fprintf(stderr, "create: %s\n", error.what());
    }
    OA_CHECK(host.named());
    OA_CHECK(host.facts().renderer == software);
    OA_CHECK(host.layer_formats().opaque == oa::app::render_policy::LayerFormat::xrgb8888);
    OA_CHECK(host.layer_formats().front_end == oa::app::render_policy::LayerFormat::rgb24);
    try {
        host.rebuild("a test");
    } catch (const std::exception& error) {
        std::fprintf(stderr, "rebuild: %s\n", error.what());
    }
    OA_CHECK(host.renderer() != nullptr);
    OA_CHECK(host.attempts().size() == 1);
    OA_CHECK(
        host.attempts().size() == 1 && host.attempts()[0].driver == software &&
        host.attempts()[0].created
    );
    host.destroy();
    close_window(window);
}

} // namespace

/// Returns the lines a call logs on standard output.
///
/// @param call what logs
/// @return the lines, without their line breaks
template <typename Call>
std::vector<std::string> logged_lines(Call&& call) {
    std::ostringstream logged;
    std::streambuf* const standard_output = std::cout.rdbuf(logged.rdbuf());
    try {
        call();
    } catch (const std::exception& error) {
        std::fprintf(stderr, "logged call: %s\n", error.what());
        OA_CHECK(false);
    }
    std::cout.rdbuf(standard_output);
    std::vector<std::string> lines;
    std::istringstream reading(logged.str());
    for (std::string line; std::getline(reading, line);)
        lines.push_back(line);
    return lines;
}

/// The start-up function test on SDL's software renderer: its target, its
/// LINEAR reduction and the known pattern read back as they should, so it
/// passes; PIXELART there is nearest, so the probe finds it missing. The
/// window is the render target again and the draw colour as it was.
void test_function_test_on_software() {
    SDL_Window* window = open_window();
    SDL_Renderer* renderer =
        window != nullptr
            ? SDL_CreateRenderer(window, oa::app::render_policy::software_driver.data())
            : nullptr;
    OA_CHECK(renderer != nullptr);
    if (renderer != nullptr) {
        constexpr uint8_t draw_red = 12, draw_green = 34, draw_blue = 56, draw_alpha = 78;
        OA_CHECK(SDL_SetRenderDrawColor(renderer, draw_red, draw_green, draw_blue, draw_alpha));
        const auto result = oa::app::run_function_test(renderer);
        if (!result.passed)
            std::fprintf(stderr, "function test: %s\n", result.failure.c_str());
        OA_CHECK(result.passed);
        OA_CHECK(result.failure.empty());
        OA_CHECK(!result.pixelart);
        OA_CHECK(SDL_GetRenderTarget(renderer) == nullptr);
        uint8_t red = 0, green = 0, blue = 0, alpha = 0;
        OA_CHECK(SDL_GetRenderDrawColor(renderer, &red, &green, &blue, &alpha));
        OA_CHECK(
            red == draw_red && green == draw_green && blue == draw_blue && alpha == draw_alpha
        );
        SDL_DestroyRenderer(renderer);
    }
    // No renderer fails it.
    OA_CHECK(!oa::app::run_function_test(nullptr).passed);
    close_window(window);
}

/// The start-up function test fails a renderer that draws wrongly: on SDL's
/// software renderer, the reduction by half drawn NEAREST fails (b), and
/// the known pattern reduced NEAREST fails (d), each with its own words,
/// and neither leaves the render target set.
void test_function_test_fails_wrong_draws() {
    SDL_Window* window = open_window();
    SDL_Renderer* renderer =
        window != nullptr
            ? SDL_CreateRenderer(window, oa::app::render_policy::software_driver.data())
            : nullptr;
    OA_CHECK(renderer != nullptr);
    if (renderer != nullptr) {
        oa::app::FunctionTestFaults half{};
        half.half_nearest = true;
        const auto half_result = oa::app::run_function_test(renderer, half);
        OA_CHECK(!half_result.passed && !half_result.pixelart);
        OA_CHECK(
            half_result.failure ==
            "a LINEAR reduction by half did not read back the texels' average"
        );
        oa::app::FunctionTestFaults pattern{};
        pattern.pattern_nearest = true;
        const auto pattern_result = oa::app::run_function_test(renderer, pattern);
        OA_CHECK(!pattern_result.passed);
        OA_CHECK(
            pattern_result.failure ==
            "the known pattern did not read back as the processor computes it"
        );
        OA_CHECK(SDL_GetRenderTarget(renderer) == nullptr);
        // Drawn right again, it passes.
        OA_CHECK(oa::app::run_function_test(renderer).passed);
        SDL_DestroyRenderer(renderer);
    }
    close_window(window);
}

/// The start's tier on the dummy video driver, which draws no window: with
/// the setting at Full the processor draws everything, the function test does
/// not run, and the line says why; a named preferences file says it is off; with
/// --hardware-acceleration and --force-capable the function test runs and
/// passes on SDL's software renderer and the tier is accelerated, where the
/// machine has 2 GiB, and otherwise the line says it needs the memory; and
/// with the faults drawing the known pattern wrongly the test fails, which
/// --force-capable does not lift, and the line says no usable graphics card
/// was found.
void test_start_tier_on_dummy() {
    namespace policy = oa::app::render_policy;
    const std::string started =
        "open-annihilation: graphics: software on dummy, textures of any size; ";

    struct Case {
        oa::app::TierRequest request;
        bool tested;           ///< the function test runs, with 2 GiB
        std::string_view tier; ///< what the line ends with, with 2 GiB
        bool draws_wrongly{};  ///< the faults draw the known pattern wrongly
        /// The function test's state where it does not run.
        policy::FunctionTest untested{policy::FunctionTest::not_run};
    };

    using oa::ui::engine_settings::HardwareAcceleration;
    oa::app::TierRequest own{};
    own.players_own_profile = true;
    own.setting = HardwareAcceleration::full;
    oa::app::TierRequest named{};
    // --hardware-acceleration names Full, which the flag forces while Full is
    // not ready for players, and the line says so.
    oa::app::TierRequest flagged{};
    flagged.flag = HardwareAcceleration::full;
    flagged.force_capable = true;
    oa::app::TierRequest basic{};
    basic.flag = HardwareAcceleration::basic;
    basic.force_capable = true;
    oa::app::TierRequest refused{};
    refused.flag = HardwareAcceleration::off;
    refused.setting = HardwareAcceleration::basic;
    const Case cases[] = {
        {own,
         false,
         "standard tier: the processor draws everything (the environment names a driver)"},
        {named,
         false,
         "standard tier: the processor draws everything (hardware acceleration is off)"},
        {refused,
         false,
         "standard tier: the processor draws everything (hardware acceleration is off)"},
        {flagged,
         true,
         "full tier: the graphics card draws the battlefield and scales the interface"},
        {basic, true, "basic tier: the graphics card scales the interface"},
        {flagged,
         true,
         "standard tier: the processor draws everything (no usable graphics card was found)",
         true},
    };
    for (const auto& run : cases) {
        SDL_Window* window = open_window();
        if (window == nullptr) {
            close_window(window);
            continue;
        }
        RendererHost host;
        oa::app::RenderFaultHooks faults{};
        faults.function_test.pattern_nearest = run.draws_wrongly;
        const auto lines = logged_lines([&]() {
            host.create(window, faults);
            host.decide_start_tier(run.request);
        });
        const auto& inputs = host.tier_inputs();
        const bool memory = inputs.memory >= policy::smallest_accelerated_memory;
        const std::string_view tier =
            memory ? run.tier
                   : "standard tier: the processor draws everything (it needs at least 2 GB of "
                     "memory)";
        OA_CHECK(!lines.empty() && lines.back() == started + std::string(tier));
        OA_CHECK(inputs.renderer && inputs.virtual_video_driver);
        OA_CHECK(inputs.capability == policy::Capability::software_renderer);
        const policy::FunctionTest tested =
            run.draws_wrongly ? policy::FunctionTest::failed : policy::FunctionTest::passed;
        OA_CHECK(inputs.function_test == (run.tested && memory ? tested : run.untested));
        OA_CHECK(
            policy::card_tier(policy::decide_render_tier(inputs).tier) ==
            (run.tested && memory && !run.draws_wrongly)
        );
        // A failed test logs what failed before the start-up line.
        OA_CHECK(
            !(run.draws_wrongly && memory) ||
            (lines.size() >= 2 &&
             lines[lines.size() - 2] ==
                 "open-annihilation: graphics: the start-up test failed: the known pattern did "
                 "not read back as the processor computes it")
        );
        // The start never draws at the magnify rungs on a class nobody has run.
        OA_CHECK(!host.start_rung().magnify && host.start_rung().filtered_chrome);
        host.destroy();
        close_window(window);
    }
}

/// A rebuild drops the accelerated tier for the run and leaves the new
/// renderer's function test to run again, and the line it logs says the
/// driver failed.
void test_rebuild_drops_the_tier() {
    namespace policy = oa::app::render_policy;
    SDL_Window* window = open_window();
    if (window == nullptr) {
        close_window(window);
        return;
    }
    RendererHost host;
    oa::app::TierRequest flagged{};
    flagged.flag = oa::ui::engine_settings::HardwareAcceleration::basic;
    flagged.force_capable = true;
    const auto lines = logged_lines([&]() {
        host.create(window);
        host.decide_start_tier(flagged);
        host.rebuild("a test");
    });
    OA_CHECK(host.tier_inputs().drop == policy::Drop::driver_failure);
    OA_CHECK(host.tier_inputs().function_test == policy::FunctionTest::not_run);
    OA_CHECK(
        !lines.empty() && lines.back().ends_with(
                              "standard tier: the processor draws everything (the graphics "
                              "driver failed)"
                          )
    );
    host.destroy();
    close_window(window);
}

/// The name a failing call is struck under: the words of its error before
/// the colon, joined by single hyphens, cut to the records' limit.
void test_failing_call_name() {
    OA_CHECK(
        oa::app::failing_call_name("SDL_RenderClear: the device is gone") == "SDL_RenderClear"
    );
    OA_CHECK(
        oa::app::failing_call_name("SDL_CreateTexture of a scene tile: out of memory") ==
        "SDL_CreateTexture-of-a-scene-tile"
    );
    OA_CHECK(oa::app::failing_call_name("injected present error") == "injected-present-error");
    OA_CHECK(oa::app::failing_call_name("  (odd)  words!  ") == "odd-words");
    OA_CHECK(oa::app::failing_call_name("") == oa::app::unnamed_call);
    OA_CHECK(oa::app::failing_call_name(": nothing before the colon") == oa::app::unnamed_call);
    const std::string long_name = oa::app::failing_call_name(std::string(200, 'x') + ": reason");
    OA_CHECK(long_name.size() == oa::app::renderer_state::max_name_bytes);
    OA_CHECK(oa::app::renderer_state::valid_driver_name(long_name));
    OA_CHECK(
        oa::app::renderer_state::valid_driver_name(
            oa::app::failing_call_name("a " + std::string(70, 'y') + " b")
        )
    );
}

/// The host's records on the dummy video driver, in scratch folders as the
/// player's own profile keeps them: a start writes the trial before the
/// function test and leaves the sentinel at `standard software` after it,
/// a clean end erases the trial and deletes the sentinel, and a lost
/// device mended by a rebuild is recorded against the driver under
/// --force-capable. A profile that cannot be written logs each file's
/// failure once and the start goes on, with the function test skipped,
/// the tier standard and the line saying the game cannot save its files.
void test_records_on_dummy() {
    namespace policy = oa::app::render_policy;
    namespace rs = oa::app::renderer_state;
    const std::filesystem::path scratch = oa::test::make_scratch_directory("render-host-records");
    oa::app::RenderFaultHooks faults{};
    faults.physical_memory = uint64_t{8} << 30;
    faults.crash_evidence = rs::CrashEvidence::two_in_a_row;
    oa::app::TierRequest flagged{};
    flagged.flag = oa::ui::engine_settings::HardwareAcceleration::basic;
    flagged.force_capable = true;
    flagged.players_own_profile = true;
    {
        SDL_Window* window = open_window();
        if (window != nullptr) {
            const std::filesystem::path folder = scratch / "profile";
            RendererHost host;
            oa::app::RecordsPlace place;
            place.folder = folder;
            place.engine_build = "test";
            const auto lines = logged_lines([&]() {
                host.create(window, faults, place);
                host.decide_start_tier(flagged);
            });
            OA_CHECK(host.records().storage() == rs::Storage::disk);
            OA_CHECK(
                policy::decide_render_tier(host.tier_inputs()).tier ==
                policy::RenderTier::accelerated
            );
            const auto& trial = host.records().records().trial;
            OA_CHECK(
                trial && trial->stage == rs::StrikeStage::probe && trial->driver == "software"
            );
            const auto written = oa::platform::preferences::load(folder / rs::records_file_name);
            OA_CHECK(written.count(std::string(rs::trial_key)) == 1);
            const auto sentinel = oa::platform::preferences::load(folder / rs::sentinel_file_name);
            OA_CHECK(
                sentinel.count(std::string(rs::sentinel_key)) == 1 &&
                sentinel.at(std::string(rs::sentinel_key)) == "standard software"
            );
            // A lost device, mended by a rebuild, is recorded at once.
            SDL_Event lost{};
            lost.type = SDL_EVENT_RENDER_DEVICE_LOST;
            lost.render.windowID = SDL_GetWindowID(window);
            OA_CHECK(host.take_event(lost));
            std::ignore = logged_lines([&]() { host.service(); });
            const rs::DriverRecords* recorded =
                rs::find_driver(host.records().records(), "software");
            OA_CHECK(
                recorded != nullptr &&
                recorded->accelerated_unusable.failure == rs::RecordedFailure::lost &&
                recorded->strike.stage == rs::StrikeStage::lost
            );
            OA_CHECK(host.tier_inputs().accelerated_unusable_record);
            // A clean end erases the trial and deletes the sentinel.
            host.finish_records();
            OA_CHECK(!std::filesystem::exists(folder / rs::sentinel_file_name));
            const auto ended = oa::platform::preferences::load(folder / rs::records_file_name);
            OA_CHECK(
                ended.count(std::string(rs::trial_key)) == 0 &&
                ended.count(std::string(rs::accelerated_unusable_prefix) + "software") == 1
            );
            OA_CHECK(!lines.empty());
            host.destroy();
        }
        close_window(window);
    }
    {
        SDL_Window* window = open_window();
        if (window != nullptr) {
            // A file stands where the profile's folder should be.
            const std::filesystem::path blocker = scratch / "blocker";
            {
                std::ofstream(blocker) << "a file, not a folder";
            }
            RendererHost host;
            oa::app::RecordsPlace place;
            place.folder = blocker / "profile";
            place.engine_build = "test";
            const auto lines = logged_lines([&]() {
                host.create(window, faults, place);
                host.decide_start_tier(flagged);
            });
            const auto& inputs = host.tier_inputs();
            OA_CHECK(inputs.function_test == policy::FunctionTest::trial_unwritten);
            OA_CHECK(
                policy::decide_render_tier(inputs).reason == policy::TierReason::trial_unwritten
            );
            const auto counted = [&](std::string_view part) {
                std::size_t count = 0;
                for (const auto& line : lines)
                    if (line.find(part) != std::string::npos)
                        ++count;
                return count;
            };
            OA_CHECK(counted("cannot write renderer-sentinel.conf") == 1);
            OA_CHECK(counted("cannot write renderer-state.conf") == 1);
            OA_CHECK(counted(oa::app::trial_unwritten_log_line()) == 1);
            OA_CHECK(
                !lines.empty() &&
                lines.back().ends_with(
                    "standard tier: the processor draws everything (the game cannot save its "
                    "files)"
                )
            );
            OA_CHECK(!host.records().records().trial);
            host.destroy();
        }
        close_window(window);
    }
    std::error_code error;
    std::filesystem::remove_all(scratch, error);
}

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--case" &&
        std::string_view(argv[2]) == "named-missing") {
        test_named_missing(false);
        return oa::test::check_exit_status();
    }
    test_log_lines();
    test_every_driver_works();
    test_walk_in_order();
    test_software_fall_back();
    test_hint_not_taken();
    test_nothing_starts();
    test_skipping();
    test_advice_rule();
    test_environment_start();
    test_rebuild_walks();
    test_dummy_walk();
    test_dummy_hint_held();
    test_dummy_rebuild();
    test_lost_before_runtime();
    test_named_list_rebuild();
    test_named_missing(true);
    test_named_software();
    test_function_test_on_software();
    test_function_test_fails_wrong_draws();
    test_start_tier_on_dummy();
    test_rebuild_drops_the_tier();
    test_failing_call_name();
    test_records_on_dummy();
    return oa::test::check_exit_status();
}

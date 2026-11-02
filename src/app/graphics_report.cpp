// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "graphics_report.hpp"

#include "oa/app/render_policy.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <iostream>
#include <limits>

namespace oa::app {

namespace render_probe = oa::platform::render_probe;

namespace settings = oa::ui::engine_settings;

/// What the start-up line says the basic tier does, by its reach.
constexpr std::string_view kScalesInterface = "the graphics card scales the interface";
constexpr std::string_view kAndZoomedIn = " and the zoomed-in view";
constexpr std::string_view kAndSmoothed = ", and the zoomed-out view is smoothed";
constexpr std::string_view kSmoothedOnly = "the zoomed-out view is smoothed";
constexpr std::string_view kAsStandard = "the view is drawn as in the standard tier";

/// The reason the start-up line gives for the standard tier when the
/// setting or a flag set Hardware acceleration to Off.
constexpr std::string_view kTurnedOff = "hardware acceleration is off";

/// Returns the reason the start-up line gives for the standard tier: the
/// status's first line, with no "Not in use: " and no full stop.
///
/// @param state the status
/// @return the reason
std::string_view standard_reason(settings::AccelerationState state) noexcept {
    using settings::AccelerationState;
    switch (state) {
    case AccelerationState::off_driver_skipped:
    case AccelerationState::off_by_setting:
    case AccelerationState::off_by_command_line:
        return kTurnedOff;
    case AccelerationState::needs_memory_driver_skipped:
    case AccelerationState::needs_memory:
        return "it needs at least 2 GB of memory";
    case AccelerationState::environment_driver:
        return "the environment names a driver";
    case AccelerationState::too_little_memory:
        return "there is too little memory";
    case AccelerationState::waiting_for_game_end:
        return "it waits for the game to end";
    case AccelerationState::engine_error:
        return "an error stopped it for this run";
    case AccelerationState::driver_failed:
        return failed_driver_reason;
    case AccelerationState::game_stopped:
        return "the game stopped while using it";
    case AccelerationState::no_usable_card:
        return "no usable graphics card was found";
    case AccelerationState::lacks_feature:
        return "the graphics card lacks a feature";
    case AccelerationState::cannot_save:
        return "the game cannot save its files";
    case AccelerationState::next_start:
    case AccelerationState::full_cannot_save:
    case AccelerationState::full_too_little_memory:
    case AccelerationState::full_stopped:
    case AccelerationState::full_failed_before:
    case AccelerationState::full_lacks_feature:
    case AccelerationState::full_waiting_for_game_end:
    case AccelerationState::in_use_on_another_driver:
    case AccelerationState::in_use_no_smoothing:
    case AccelerationState::full_in_use:
    case AccelerationState::in_use:
        break;
    }
    return "it takes effect from the next start";
}

render_policy::DriverTraits driver_traits(std::string_view renderer) noexcept {
    render_policy::DriverTraits driver{};
    driver.software = renderer == render_probe::software_renderer;
    driver.capable_before_vista = render_probe::capable_before_vista(renderer);
    driver.adapter_required = render_probe::adapter_needed(renderer);
    driver.loses_device_in_normal_use = render_probe::loses_device_in_ordinary_use(renderer);
    driver.texture_limit_source = render_probe::reports_fixed_texture_limit(renderer)
                                      ? render_policy::TextureLimitSource::fixed_report
                                      : render_policy::TextureLimitSource::reported;
    return driver;
}

render_policy::RendererFacts renderer_facts(const render_probe::AdapterFacts& facts) noexcept {
    render_policy::RendererFacts renderer{};
    renderer.driver = driver_traits(facts.renderer);
    renderer.max_texture_size = corrected_texture_limit(facts);
    renderer.adapter_known = facts.adapter_state == render_probe::AdapterState::read;
    renderer.software_rasteriser = facts.software_rasteriser;
    renderer.virtual_adapter = facts.virtual_adapter;
    renderer.under_wine = facts.wine;
    return renderer;
}

std::string_view full_shortfall_note(settings::AccelerationState state) noexcept {
    using settings::AccelerationState;
    switch (state) {
    case AccelerationState::full_cannot_save:
        return " (Full's trial cannot be written)";
    case AccelerationState::full_too_little_memory:
        return " (there is too little memory for Full)";
    case AccelerationState::full_stopped:
        return " (Full stopped for this run)";
    case AccelerationState::full_failed_before:
        return " (Full failed before on this driver)";
    case AccelerationState::full_lacks_feature:
        return " (the graphics card lacks a feature Full needs)";
    case AccelerationState::full_waiting_for_game_end:
        return " (Full waits for the game to end)";
    default:
        return {};
    }
}

std::string tier_description(const settings::AccelerationStatus& status, bool full) {
    using settings::AccelerationReach;
    using settings::AccelerationState;
    if (full || status.state == AccelerationState::full_in_use)
        return std::string(full_tier_description);
    // Where Full was asked for and Basic draws, the basic tier is in use.
    const bool in_use = status.state == AccelerationState::full_cannot_save ||
                        status.state == AccelerationState::full_too_little_memory ||
                        status.state == AccelerationState::full_stopped ||
                        status.state == AccelerationState::full_failed_before ||
                        status.state == AccelerationState::full_lacks_feature ||
                        status.state == AccelerationState::full_waiting_for_game_end ||
                        status.state == AccelerationState::in_use_on_another_driver ||
                        status.state == AccelerationState::in_use_no_smoothing ||
                        status.state == AccelerationState::in_use;
    if (in_use) {
        std::string text(basic_tier_name);
        text += " tier";
        text += full_shortfall_note(status.state);
        text += ": ";
        switch (status.reach) {
        case AccelerationReach::menus:
            text += kScalesInterface;
            break;
        case AccelerationReach::zoomed_in:
            text += kScalesInterface;
            text += kAndZoomedIn;
            break;
        case AccelerationReach::zoomed_out:
            text += kScalesInterface;
            text += kAndZoomedIn;
            text += kAndSmoothed;
            break;
        case AccelerationReach::nearest_zoomed_out:
            text += kSmoothedOnly;
            break;
        case AccelerationReach::nearest_none:
            text += kAsStandard;
            break;
        }
        return text;
    }
    std::string text(standard_tier_description);
    text += " (";
    text += standard_reason(status.state);
    text += ')';
    return text;
}

uint32_t corrected_texture_limit(const render_probe::AdapterFacts& facts) noexcept {
    const render_policy::DriverTraits driver = driver_traits(facts.renderer);
    const uint32_t reported =
        facts.reported_texture_limit <= 0
            ? render_policy::unlimited_texture_size
            : static_cast<uint32_t>(std::min<int64_t>(
                  facts.reported_texture_limit, std::numeric_limits<uint32_t>::max()
              ));
    return render_policy::texture_limit(driver, reported, facts.device_texture_limit);
}

std::string graphics_log_line(const render_probe::AdapterFacts& facts, std::string_view tier) {
    std::string line(graphics_log_prefix);
    line += facts.renderer;
    if (!facts.video_driver.empty()) {
        line += " on ";
        line += facts.video_driver;
    }
    const std::string adapter = stats_adapter_name(facts);
    if (!adapter.empty()) {
        line += " (";
        line += adapter;
        line += ')';
    }
    const uint32_t limit = corrected_texture_limit(facts);
    if (limit == render_policy::unlimited_texture_size) {
        line += ", textures of any size";
    } else {
        line += ", textures up to ";
        line += std::to_string(limit);
    }
    line += "; ";
    line += tier;
    return line;
}

std::string stats_adapter_name(const render_probe::AdapterFacts& facts) {
    switch (facts.adapter_state) {
    case render_probe::AdapterState::read:
        return facts.adapter;
    case render_probe::AdapterState::unknown:
        return std::string(unknown_adapter_name);
    case render_probe::AdapterState::skipped:
    case render_probe::AdapterState::none:
        break;
    }
    return {};
}

std::string stats_device_adapter_name(std::string_view adapter) {
    return std::string(adapter.empty() ? unknown_adapter_name : adapter);
}

render_probe::AdapterRead adapter_read_for(std::string_view named_driver, bool flags_ask) noexcept {
    return named_driver.empty() || flags_ask ? render_probe::AdapterRead::read
                                             : render_probe::AdapterRead::skip;
}

render_probe::AdapterFacts describe_game_renderer(SDL_Renderer* renderer, bool flags_ask) {
    const char* named = SDL_GetHint(SDL_HINT_RENDER_DRIVER);
    return render_probe::describe(
        renderer,
        adapter_read_for(named != nullptr ? std::string_view(named) : std::string_view(), flags_ask)
    );
}

render_probe::AdapterFacts report_game_renderer(SDL_Renderer* renderer, std::string_view tier) {
    auto facts = describe_game_renderer(renderer);
    std::cout << graphics_log_line(facts, tier) << '\n';
    return facts;
}

} // namespace oa::app

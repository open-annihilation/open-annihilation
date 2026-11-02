// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What the game tells the player about the renderer it draws with: the line
// it logs at start, naming the render driver, the adapter, the texture
// limit and the tier frames are drawn in, with what the tier does or why the
// processor draws everything, and the names the "+stats" overlay's renderer
// row shows; and what the render policy needs to know of the renderer.
#pragma once

#include "oa/app/render_policy.hpp"
#include "oa/platform/render_probe.hpp"
#include "oa/ui/engine_settings/dialog.hpp"

#include <cstdint>
#include <string>
#include <string_view>

namespace oa::app {

/// What every line the game logs about its graphics begins with.
inline constexpr std::string_view graphics_log_prefix = "open-annihilation: graphics: ";
/// The name of the tier in which the processor draws and scales everything:
/// Hardware acceleration Off.
inline constexpr std::string_view standard_tier_name = "standard";
/// The name of the tier in which the graphics card scales and composes what
/// the processor draws: Hardware acceleration Basic.
inline constexpr std::string_view basic_tier_name = "basic";
/// The name of the tier in which the graphics card draws the battlefield
/// from texture pages: Hardware acceleration Full.
inline constexpr std::string_view full_tier_name = "full";
/// What the start-up line says the full tier does.
inline constexpr std::string_view full_tier_description =
    "full tier: the graphics card draws the battlefield and scales the interface";
/// What the start-up line says the standard tier does.
inline constexpr std::string_view standard_tier_text = "the processor draws everything";
/// What the start-up line says of the standard tier where it gives no
/// reason, as for a renderer the runtime made itself.
inline constexpr std::string_view standard_tier_description =
    "standard tier: the processor draws everything";
/// Returns what the start-up line puts after the basic tier's name where
/// Full was asked for and Basic draws: why Full stopped or waits, in
/// brackets; empty where Basic was asked for.
///
/// @param state the Hardware acceleration row's status
/// @return the note, with its leading space; empty for a state of Basic's
[[nodiscard]] std::string_view
full_shortfall_note(oa::ui::engine_settings::AccelerationState state) noexcept;
/// What the line logged after the renderer was made again gives as the
/// reason the processor draws everything.
inline constexpr std::string_view failed_driver_reason = "the graphics driver failed";
/// What the start-up line and the "+stats" overlay say of an adapter that
/// was read but could not be named.
inline constexpr std::string_view unknown_adapter_name = "unknown adapter";

/// Returns what the render policy needs to know of a render driver, from
/// its name: whether it is SDL's software renderer, whether it may be
/// accelerated on Windows before Vista
/// (oa::platform::render_probe::capable_before_vista), whether its adapter
/// must be read (oa::platform::render_probe::adapter_needed), whether it
/// reports a fixed texture limit
/// (oa::platform::render_probe::reports_fixed_texture_limit) and whether
/// its device is lost in ordinary use
/// (oa::platform::render_probe::loses_device_in_ordinary_use).
///
/// @param renderer SDL's name for the render driver
/// @return the traits
[[nodiscard]] render_policy::DriverTraits driver_traits(std::string_view renderer) noexcept;

/// Returns what probe items 1 to 3 found of a renderer, as the render
/// policy reads it (render_policy::assess_renderer): its driver's traits,
/// its corrected texture limit, whether its adapter was read, and whether
/// it rasterises on the processor, is a virtual machine's or runs under
/// Wine.
///
/// @param facts what the probe found
/// @return the facts
[[nodiscard]] render_policy::RendererFacts
renderer_facts(const oa::platform::render_probe::AdapterFacts& facts) noexcept;

/// Returns what the start-up line says of the status's tier: in the full
/// tier, by the flag or the status, full_tier_description; otherwise in use,
/// "basic tier: " and what the graphics card does at its reach, "the
/// graphics card scales the interface", "... and the zoomed-in view", "...,
/// and the zoomed-out view is smoothed", "the zoomed-out view is smoothed"
/// or "the view is drawn as in the standard tier", with the note of why
/// Basic draws after the name where Full was asked for
/// (full_shortfall_note); otherwise
/// standard_tier_description with the reason in brackets, "hardware
/// acceleration is off" when the setting or a flag turned it off, else the
/// status's first line without "Not in use: ", as in "(the environment
/// names a driver)".
///
/// @param status the Hardware acceleration row's status
/// @param full the frame is drawn in the full tier
/// @return the tier and what it does, without a full stop
[[nodiscard]] std::string
tier_description(const oa::ui::engine_settings::AccelerationStatus& status, bool full = false);

/// Returns the texture limit a renderer really has, as the render policy
/// corrects it (render_policy::texture_limit) from what the probe found: a
/// report of 0 or less means no limit, and one past 32 bits is held at the
/// largest 32-bit value; on a driver that reports a fixed limit
/// (render_probe::reports_fixed_texture_limit) the device's own limit
/// where the probe read it, otherwise the report at most 8192.
///
/// @param facts what the probe found
/// @return the limit in texels; 0 for none
[[nodiscard]] uint32_t
corrected_texture_limit(const oa::platform::render_probe::AdapterFacts& facts) noexcept;

/// Returns the line the game logs once its renderer is made: the render
/// driver on the video driver, the adapter in brackets where it was read,
/// the largest texture side (corrected_texture_limit), and the tier with
/// what it does, as in
/// "open-annihilation: graphics: metal on cocoa (Apple M2), textures up to
/// 16384; basic tier: the graphics card scales the interface". An
/// adapter that could not be named reads "(unknown adapter)"; one not read,
/// as under SDL_RENDER_DRIVER, and SDL's software renderer, which has none,
/// leave the brackets out. No limit reads "textures of any size".
///
/// @param facts what the probe found
/// @param tier the tier and what it does (tier_description)
/// @return the line, without its line break
[[nodiscard]] std::string graphics_log_line(
    const oa::platform::render_probe::AdapterFacts& facts,
    std::string_view tier = standard_tier_description
);

/// Returns the adapter the "+stats" overlay's renderer row names: its name
/// where it was read, unknown_adapter_name where it could not be named, and
/// nothing where it was not read or the renderer has none.
///
/// @param facts what the probe found
/// @return the name; empty for none
[[nodiscard]] std::string stats_adapter_name(const oa::platform::render_probe::AdapterFacts& facts);

/// Returns the adapter the "+stats" overlay names for the graphics device
/// the turned (3D) view draws on: the name SDL gave the device
/// (render_probe::gpu_device_adapter), or unknown_adapter_name where it gave
/// none, as before SDL 3.4.
///
/// @param adapter the device's adapter's name; empty for none
/// @return the name
[[nodiscard]] std::string stats_device_adapter_name(std::string_view adapter);

/// Says whether the start reads the adapter, from the render driver
/// SDL_RENDER_DRIVER names: it does where none is named, and not where one
/// is, which keeps the start the player asked for to SDL's own properties
/// alone, unless a flag that asks for the graphics card or --force-capable
/// asks for more than SDL's own start, which reads it as every other start
/// does.
///
/// @param named_driver the hint's value; empty where it is unset or empty
/// @param flags_ask a flag that asks for the card or --force-capable was given
/// @return AdapterRead::read for none named or when the flags ask, else
///     AdapterRead::skip
[[nodiscard]] oa::platform::render_probe::AdapterRead
adapter_read_for(std::string_view named_driver, bool flags_ask = false) noexcept;

/// Describes the renderer the game just made, reading the adapter as
/// adapter_read_for says for SDL_RENDER_DRIVER.
///
/// @param renderer the renderer
/// @param flags_ask a flag that asks for the card or --force-capable was given
/// @return what the probe found
[[nodiscard]] oa::platform::render_probe::AdapterFacts
describe_game_renderer(SDL_Renderer* renderer, bool flags_ask = false);

/// Describes the renderer the game just made (describe_game_renderer) and
/// logs graphics_log_line on standard output.
///
/// @param renderer the renderer
/// @param tier the tier and what it does (tier_description)
/// @return what the probe found
oa::platform::render_probe::AdapterFacts
report_game_renderer(SDL_Renderer* renderer, std::string_view tier = standard_tier_description);

} // namespace oa::app

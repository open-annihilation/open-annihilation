// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What the game can learn about the renderer SDL made and the graphics
// adapter behind it: the render driver's name, its texture limit, the
// adapter's name and identifiers where its graphics interface gives them,
// and whether the adapter draws on the processor or in a virtual machine.
// The code that reads each graphics interface is chosen per operating system
// at compile time; it reaches the interfaces only through the objects SDL
// has already made, or through functions the program looks up at run time,
// and links none of their libraries.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

struct SDL_GPUDevice;
struct SDL_Renderer;

namespace oa::platform::render_probe {

/// SDL's name for its software renderer, which draws on the processor.
inline constexpr std::string_view software_renderer = "software";
/// SDL's name for its Vulkan renderer.
inline constexpr std::string_view vulkan_renderer = "vulkan";
/// SDL's name for its renderer over SDL's own graphics device.
inline constexpr std::string_view gpu_renderer = "gpu";

/// The longest adapter name the probe keeps, bytes; a longer one is cut.
inline constexpr std::size_t adapter_name_limit = 255;

/// Whether describe reads the adapter as well as SDL's own properties.
enum class AdapterRead : uint8_t {
    read, ///< the name, the texture limit and the adapter
    skip, ///< the name and the texture limit alone, from SDL's properties
};

/// What is known of the adapter behind a renderer.
enum class AdapterState : uint8_t {
    skipped, ///< not read: the caller asked for SDL's properties alone
    none,    ///< SDL's software renderer, which draws on the processor with no adapter
    unknown, ///< read, but its graphics interface gave nothing usable, or no reader exists here
    read,    ///< named by its graphics interface
};

/// What the probe found out about a renderer and the adapter behind it.
struct AdapterFacts {
    std::string renderer{};     ///< SDL's name for the render driver ("metal", "direct3d11")
    std::string video_driver{}; ///< SDL's name for the video driver ("cocoa", "windows", "x11")
    int64_t
        reported_texture_limit{}; ///< the renderer's largest texture side as SDL reports it; 0 for none
    uint32_t
        device_texture_limit{};   ///< the device's own largest texture side where read; 0 where not
    AdapterState adapter_state{}; ///< whether the adapter was read
    std::string adapter{};        ///< the adapter's name; empty unless adapter_state is read
    std::string vendor{};         ///< the vendor's name, where the interface gives one apart
    uint32_t vendor_id{};         ///< the adapter's PCI vendor identifier; 0 where not given
    uint32_t device_id{};         ///< the adapter's PCI device identifier; 0 where not given
    bool software_flag{}; ///< the graphics interface marks the adapter as drawing on the processor
    bool wine{};          ///< the game runs on Windows' interfaces as provided by Wine
    bool software_rasteriser{}; ///< the renderer draws on the processor
    bool virtual_adapter{};     ///< the adapter is a virtual machine's
};

/// Reads the adapter behind a renderer for describe_reported.
struct AdapterReader {
    void* context{}; ///< handed back to read
    /// Fills the adapter's name, vendor, identifiers, software flag and
    /// device texture limit where it can; returns true when the adapter was
    /// read. Null reads nothing.
    bool (*read)(void* context, AdapterFacts& facts){};
};

/// Describes a renderer from what SDL reports of it, reading the adapter
/// through a reader only where that is asked for and there is an adapter:
/// SDL's software renderer has none (AdapterState::none), AdapterRead::skip
/// reads nothing (AdapterState::skipped), and otherwise the adapter is read
/// (AdapterState::read) or, where the reader fails or gives a name that
/// cleans to nothing, unknown. The names are cleaned (clean_name) and the
/// facts classified (classify); the texture limits are kept as reported and
/// read, for the render policy to correct. describe is this with SDL's own
/// properties and the readers of this system, and on Windows also asks
/// whether the game runs under Wine.
///
/// @param renderer SDL's name for the render driver
/// @param video_driver SDL's name for the video driver
/// @param reported_texture_limit the largest texture side SDL reports
/// @param read whether to read the adapter
/// @param reader what reads it
/// @return what was found
[[nodiscard]] AdapterFacts describe_reported(
    std::string_view renderer,
    std::string_view video_driver,
    int64_t reported_texture_limit,
    AdapterRead read,
    const AdapterReader& reader
);

/// Describes a renderer: its name and texture limit from SDL's properties,
/// and, when asked, the adapter behind it.
///
/// The adapter is read through the objects SDL made for the renderer, with
/// cheap queries only: a Direct3D 9 device's adapter identifier, a Direct3D
/// 11 or 12 device's DXGI adapter description, the OpenGL renderer and
/// vendor strings while SDL's context is current, a Vulkan physical device's
/// properties, the Metal device's name, and the name of SDL's graphics device
/// (SDL 3.4 and later). A renderer this build has no reader for, on this
/// system or this SDL, reads as unknown. On Windows it also asks whether the
/// game runs under Wine. Then it classifies the adapter by name and flags
/// (classify), as describe_reported does.
///
/// @param renderer the renderer SDL created; null gives facts with every field empty
/// @param read whether to read the adapter
/// @return what was found
[[nodiscard]] AdapterFacts describe(SDL_Renderer* renderer, AdapterRead read);

/// Returns the name of the adapter behind a graphics device SDL made
/// (SDL_CreateGPUDevice), as the gpu renderer's adapter is read: SDL's name
/// for the device, which it gives from SDL 3.4, cleaned (clean_name).
///
/// @param device the device; null names none
/// @return the name; empty where SDL gives none, as before SDL 3.4
[[nodiscard]] std::string gpu_device_adapter(SDL_GPUDevice* device);

/// Says whether an adapter's name, or its PCI identifiers, mark a software
/// or virtual rasteriser: a name holding, in any letter case, llvmpipe,
/// softpipe, lavapipe, swrast, software rasterizer, swiftshader, gdi
/// generic, apple software renderer, microsoft basic render driver,
/// virtualbox, vmware, svga3d or chromium, or vendor 0x1414 with device
/// 0x008C, the Microsoft Basic Render Driver.
///
/// @param name the adapter's name as its driver gives it
/// @param vendor its PCI vendor identifier; 0 for none
/// @param device its PCI device identifier; 0 for none
/// @return true for a software or virtual rasteriser
[[nodiscard]] bool
names_software_rasteriser(std::string_view name, uint32_t vendor, uint32_t device) noexcept;

/// Says whether an adapter's name marks a virtual machine's adapter, the
/// part of names_software_rasteriser's list that draws on the host's
/// graphics card: virtualbox, vmware, svga3d or chromium, in any letter case.
///
/// @param name the adapter's name as its driver gives it
/// @return true for a virtual machine's adapter
[[nodiscard]] bool names_virtual_adapter(std::string_view name) noexcept;

/// Says whether an OpenGL vendor string marks a virtual machine's adapter:
/// humper, in any letter case, VirtualBox's older OpenGL pass-through.
///
/// @param vendor the OpenGL vendor string
/// @return true for that pass-through
[[nodiscard]] bool vendor_names_virtual_adapter(std::string_view vendor) noexcept;

/// Says whether SDL reports a fixed texture limit for a render driver
/// rather than its device's own: true for vulkan and gpu, which report
/// 16384 whatever the device, so the render policy takes the device's own
/// limit (device_texture_limit) where it was read.
///
/// @param renderer SDL's name for the render driver
/// @return true for a driver whose reported limit is fixed
[[nodiscard]] bool reports_fixed_texture_limit(std::string_view renderer) noexcept;

/// Says whether a render driver's renderer can be accelerated only where
/// its adapter was read, since otherwise a software rasteriser cannot be
/// ruled out: true for direct3d12, vulkan and gpu.
///
/// @param renderer SDL's name for the render driver
/// @return true where an adapter that cannot be read leaves the renderer
///     not capable
[[nodiscard]] bool adapter_needed(std::string_view renderer) noexcept;

/// Says whether a render driver may draw through the graphics card on
/// Windows before Vista: only direct3d, SDL's Direct3D 9 renderer. Any other
/// there, opengl included, keeps the standard tier.
///
/// @param renderer SDL's name for the render driver
/// @return true for direct3d
[[nodiscard]] bool capable_before_vista(std::string_view renderer) noexcept;

/// Says whether a render driver's device is lost in ordinary use, when its
/// exclusive full screen loses the focus, the screen is locked or the
/// display mode changes: direct3d, SDL's Direct3D 9 renderer. What fails on
/// it while the game runs is never recorded against it.
///
/// @param renderer SDL's name for the render driver
/// @return true for direct3d
[[nodiscard]] bool loses_device_in_ordinary_use(std::string_view renderer) noexcept;

/// Says whether a render driver resets its device at each change of the
/// wait for the display, Vertical sync: direct3d, SDL's Direct3D 9
/// renderer. Such a reset may leave the device lost.
///
/// @param renderer SDL's name for the render driver
/// @return true for direct3d
[[nodiscard]] bool vertical_sync_resets_device(std::string_view renderer) noexcept;

/// Says whether the accelerated tier has been run with this build's system
/// and processor architecture on a render driver: metal on 64-bit ARM
/// macOS, and direct3d11 on x86 and x64 Windows. Every other class is one
/// nobody has run, which starts at the lowest budget.
///
/// @param renderer SDL's name for the render driver
/// @return true for a class that has been run
[[nodiscard]] bool accelerated_tier_run(std::string_view renderer) noexcept;

/// Says whether this build runs on an ARM processor that has not been run
/// on the accelerated tier: any ARM processor but Apple's, whose builds are
/// for macOS.
///
/// @return true on such a processor
[[nodiscard]] bool untried_arm_processor() noexcept;

/// Fills the classification of facts already read: software_rasteriser for
/// SDL's software renderer, a set software_flag, a name on the software part
/// of names_software_rasteriser's list or the Microsoft Basic Render
/// Driver's identifiers; virtual_adapter for a name names_virtual_adapter
/// accepts or a vendor vendor_names_virtual_adapter accepts.
///
/// @param[in,out] facts the facts; software_rasteriser and virtual_adapter are set
void classify(AdapterFacts& facts) noexcept;

/// Says whether facts describe a renderer that draws on the processor, in a
/// virtual machine, or under Wine.
///
/// @param facts the facts, classified
/// @return true for a software or virtual rasteriser
[[nodiscard]] bool software_or_virtual(const AdapterFacts& facts) noexcept;

/// Cleans a name a driver gives: control characters become spaces, the
/// spaces at either end go, and the rest is cut at adapter_name_limit bytes.
///
/// @param name the name as the driver gave it
/// @return the cleaned name
[[nodiscard]] std::string clean_name(std::string_view name);

/// Says whether a video driver gives each window a framebuffer of its own
/// that SDL's software renderer can present through with no graphics
/// driver: windows, x11, dummy and offscreen, in any letter case.
/// Elsewhere, as on cocoa, wayland and kmsdrm, SDL presents the software
/// renderer through a texture of one of its hardware render drivers, which
/// the framebuffer hint names.
///
/// @param video_driver SDL's name for the video driver
/// @return true where the window has a framebuffer of its own
[[nodiscard]] bool native_window_framebuffer(std::string_view video_driver) noexcept;

/// Whether a renderer's device can draw, as a Direct3D 9 device answers
/// TestCooperativeLevel. Such a device is lost when its exclusive full screen
/// loses the focus, and on Windows XP also when the screen is locked or the
/// display mode changes; SDL resets it once it can be reset.
enum class DeviceState : uint8_t {
    ok,        ///< the device draws
    lost,      ///< the device is lost and cannot be reset yet
    not_reset, ///< the device is lost and can be reset now
    unknown,   ///< no Direct3D 9 device, or it gave another answer
};

/// What a Direct3D 9 device answers while it is lost (D3DERR_DEVICELOST).
inline constexpr int32_t device_lost_result = static_cast<int32_t>(0x88760868);
/// What a Direct3D 9 device answers while it is lost and can be reset
/// (D3DERR_DEVICENOTRESET).
inline constexpr int32_t device_not_reset_result = static_cast<int32_t>(0x88760869);

/// Reads a Direct3D 9 device's answer to TestCooperativeLevel.
///
/// @param result the answer: 0 (D3D_OK), device_lost_result,
///     device_not_reset_result or another
/// @return ok, lost, not_reset, or unknown for any other answer
[[nodiscard]] DeviceState device_state_from_result(int32_t result) noexcept;

/// Asks a renderer's device whether it can draw: SDL's Direct3D 9 device,
/// through the device SDL made, with TestCooperativeLevel. Every other
/// renderer, and every system but Windows, answers unknown.
///
/// @param renderer the renderer; null answers unknown
/// @return the device's state
[[nodiscard]] DeviceState device_state(SDL_Renderer* renderer) noexcept;

} // namespace oa::platform::render_probe

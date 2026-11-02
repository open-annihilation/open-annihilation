# Render probe

What the game can learn about the renderer SDL made and the graphics adapter
behind it. Target `oa-platform-render-probe`, header
`oa/platform/render_probe.hpp`, namespace `oa::platform::render_probe`.
It is built only where SDL is, and links SDL privately; its public header
only declares `SDL_Renderer`.

## Entry points

- `describe(renderer, read)` returns `AdapterFacts`: SDL's name for the
  render driver and the video driver, the texture limit SDL reports, and,
  with `AdapterRead::read`, the adapter: its name, its
  vendor string and PCI identifiers where the interface gives them, its
  software flag, the device's own texture limit, and whether the game runs
  under Wine. With `AdapterRead::skip` only SDL's properties are read, as
  for a start whose `SDL_RENDER_DRIVER` names the driver. SDL's software
  renderer has no adapter (`AdapterState::none`); an adapter that its
  interface would not name, or a renderer this build has no reader for,
  is `AdapterState::unknown`. `describe_reported(renderer, video_driver,
  reported_texture_limit, read, reader)` is the same from SDL's names and
  report, with the adapter read through a hook (`AdapterReader`): the part
  of `describe` a test can drive without a graphics interface.
- `gpu_device_adapter(device)` names the adapter behind one of SDL's
  graphics devices: the name SDL gives the device from SDL 3.4, cleaned,
  or nothing where SDL gives none, as on SDL 3.2. The `gpu` renderer's
  adapter is read through it, and so is that of the device the 3D view
  draws on, which the game makes itself.
- `names_software_rasteriser(name, vendor, device)`: true for a name that
  holds, in any letter case, `llvmpipe`, `softpipe`, `lavapipe`, `swrast`,
  `software rasterizer`, `swiftshader`, `gdi generic`, `apple software
  renderer`, `microsoft basic render driver`, `virtualbox`, `vmware`,
  `svga3d` or `chromium`, and for vendor 0x1414 with device 0x008C (the
  Microsoft Basic Render Driver, which WARP reports). Mesa's drivers layered
  over a software rasteriser (zink, virgl, its Direct3D 12 driver over WARP)
  carry the rasteriser's name in their own. `names_virtual_adapter` is the
  part of the list that names a virtual machine's adapter, and
  `vendor_names_virtual_adapter` matches the OpenGL vendor string, for
  `humper` alone (VirtualBox's older OpenGL pass-through).
- `reports_fixed_texture_limit(renderer)`: true for `vulkan` and `gpu`,
  for which SDL reports a fixed 16384 rather than the device's own limit.
  The probe keeps both limits as reported and read; the app's render policy
  corrects them (`texture_limit` in
  [render_policy.hpp](../../app/include/oa/app/render_policy.hpp)): a
  report of 0 means no limit (SDL's software renderer sets none), and on
  these two drivers the device's own limit is taken where it was read, and
  otherwise the report at most 8192.
- `adapter_needed(renderer)`: true for `direct3d12`, `vulkan` and `gpu`,
  on which an adapter that cannot be read leaves the renderer not capable,
  since a software rasteriser cannot be ruled out there.
- `capable_before_vista(renderer)`: true only for `direct3d`, SDL's
  Direct3D 9 renderer, the one driver that may draw through the graphics
  card on Windows before Vista; any other there keeps the standard tier.
- `loses_device_in_ordinary_use(renderer)`: true only for `direct3d`, whose
  Direct3D 9 device is lost in ordinary use, when its exclusive full screen
  loses the focus, the screen is locked or the display mode changes; what
  fails on it while the game runs is never recorded against it.
- `vertical_sync_resets_device(renderer)`: true only for `direct3d`, which
  resets its device at each change of the wait for the display; such a
  reset may leave the device lost.
- `accelerated_tier_run(renderer)`: true for the one driver the
  accelerated tier has been run on with this build's system and processor
  architecture, `metal` on 64-bit ARM macOS and `direct3d11` on x86 and
  x64 Windows; every other class is one nobody has run, which the render
  policy starts at the lowest budget. `untried_arm_processor()` is true in
  an ARM build for any system but macOS.
- `classify(facts)` fills `software_rasteriser` (SDL's software renderer,
  the interface's software flag, a software rasteriser's name or WARP's
  identifiers) and `virtual_adapter` (a virtual machine's adapter by name or
  vendor string); `software_or_virtual(facts)` adds Wine, under which every
  renderer draws through Wine's own translation.
- `clean_name(name)` turns control characters into spaces, trims the ends
  and cuts a name at 255 bytes without splitting a character.
- `native_window_framebuffer(video_driver)`: true for the `windows`, `x11`,
  `dummy` and `offscreen` video drivers, in any letter case, whose windows
  have a framebuffer of their own that SDL's software renderer presents
  through with no graphics driver; elsewhere, as on `cocoa`, `wayland` and
  `kmsdrm`, SDL presents it through a texture of a hardware render driver,
  which the framebuffer hint names.
- `device_state(renderer)`: whether the renderer's device can draw, as
  SDL's `direct3d` renderer's Direct3D 9 device answers through the device
  SDL made: `ok`, `lost` (it cannot be reset yet), `not_reset` (it can be
  reset now), or `unknown` for every other renderer and every system but
  Windows. `device_state_from_result(result)` reads one such answer, which
  a test can drive.

## How each interface is read

Which readers exist is chosen per system at compile time; inside them
everything is found at run time. Nothing links a graphics library: each
reader reaches its interface through the object SDL made for the renderer
(its properties) or through a function looked up at run time, so one
Windows package still starts on every Windows release from XP, and Linux
needs no Vulkan library to start.

| Renderer | Systems | Read |
| --- | --- | --- |
| `direct3d` | Windows | The device's creation parameters, its `IDirect3D9`, and `GetAdapterIdentifier` of that adapter with flags 0, the cheap form |
| `direct3d11` | Windows | The device's `IDXGIDevice`, its adapter, and `IDXGIAdapter1::GetDesc1` (name, identifiers, `DXGI_ADAPTER_FLAG_SOFTWARE`), or `GetDesc` where the adapter is older |
| `direct3d12` | Windows, where the toolchain has `d3d12.h` and `dxgi1_4.h` | The swap chain's `IDXGIFactory4`, `EnumAdapterByLuid` with the device's adapter identifier, and `GetDesc1` |
| `opengl`, `opengles2` | every system | `glGetString` for the renderer and vendor strings, looked up with `SDL_GL_GetProcAddress`, and only while SDL's context is current |
| `vulkan` | Windows, Linux | `vkGetPhysicalDeviceProperties` through SDL's `vkGetInstanceProcAddr` and the renderer's instance: the name, identifiers, CPU device type and largest 2D image side, from a head of the structure declared here |
| `metal` | Apple | The name of the Metal layer's device, by Objective-C messages |
| `gpu` | SDL 3.4 and later | The name of SDL's graphics device; on SDL 3.2 the adapter is unknown |

The interface identifiers the Windows reader asks for are defined in its
source. Wine is found by `GetProcAddress` of `wine_get_version` in
`ntdll.dll`, which every Windows process has loaded.

## Who uses it

Start-up describes the renderer it made and logs one line, with the
texture limit the render policy corrects and the tier the first frame is
drawn in, and the +stats overlay names the driver and adapter of the
window's renderer and, while the 3D view's graphics device is open, that
device's adapter ([src/app](../../app/README.md)). The walk of the render drivers that makes
it asks `native_window_framebuffer` what to set the framebuffer hint to
before SDL's software renderer. The render policy reads what the probe
found (`adapter_needed`, `capable_before_vista`, the classification, the
corrected limit) to tell whether the renderer can be accelerated, and `accelerated_tier_run` and
`untried_arm_processor` for the rung it starts at. The settings lock
Vertical sync on a renderer `vertical_sync_resets_device` names, since the
game does not recover a device such a reset leaves lost.

## Tests

`platform-render-probe` checks the name tables over names real drivers
give, in any letter case, and real cards' names against them; WARP's
identifiers; the drivers that report a fixed texture limit or need their
adapter read, the one driver that may be accelerated on Windows before
Vista, loses its device in ordinary use and resets it at each change of
the wait for the display, and the class the accelerated tier has been run
on; the classification and the cleaning of names; what `describe_reported`
reads through a stand-in reader, for SDL's software renderer and for
others, with the adapter read or skipped, named, failing or blank; the
video drivers whose windows have a framebuffer of their own; SDL's
software renderer on the dummy video driver described; and that no
graphics device names no adapter, and a device the video driver makes is
named, which the dummy driver skips but a Mac's own driver, run by hand,
names. On Windows it reads
its own executable's import table, delay-loaded libraries included, and
finds none of the graphics libraries: Direct3D 9, 11 and 12, DXGI, OpenGL,
EGL with OpenGL ES 2, and Vulkan.

The readers of the graphics interfaces themselves have no automated test:
the checks run on the dummy video driver, where SDL makes only its software
renderer. They were checked by hand on a Mac, where the Metal, OpenGL and
`gpu` renderers each named the Apple GPU, the start-up line capped `gpu`'s
limit at 8192 and a skipped read left the adapter empty. On Windows 10,
built by Visual Studio 2022 as the Windows build CI makes is, the
`direct3d`, `direct3d11`, `direct3d12`, `opengl` and `vulkan` renderers
each named an NVIDIA GeForce RTX 3070 Ti in the start-up line, OpenGL with
the driver's own `/PCIe/SSE2` after it.

## Known limitations

- It does not report an OpenGL out-of-memory error.

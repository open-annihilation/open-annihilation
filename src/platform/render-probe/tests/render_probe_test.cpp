// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The render probe's pure tables: the software and virtual rasteriser names,
// matched in any letter case as parts of the names drivers give; the
// Microsoft Basic Render Driver's identifiers; the drivers that report a
// fixed texture limit; the classification and cleaning of what a driver
// reports; the drivers that need their adapter read, the one driver that
// may be accelerated on Windows before Vista, loses its device in ordinary
// use and resets it at each change of the wait for the display, and the
// class the accelerated tier has been run on; what describe reads, through
// a stand-in reader, for each renderer and each choice of reading; the
// video drivers whose windows have a framebuffer of their own; and a
// Direct3D 9 device's answers read as its states. Then SDL's software
// renderer on the dummy video driver, described as the game describes it,
// and its device state, unknown; and the adapter of a graphics device,
// where the video driver can make one. On Windows, the test's own
// executable, which links the probe and SDL, imports no graphics library.
#include "oa/platform/render_probe.hpp"

#include "oa/test/check.hpp"

#include <SDL3/SDL.h>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

#include <algorithm>
#include <array>
#include <cstdio>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace oa::platform::render_probe;

/// The Microsoft Basic Render Driver's PCI vendor and device identifiers.
constexpr uint32_t kBasicRenderVendor = 0x1414;
constexpr uint32_t kBasicRenderDevice = 0x008C;
/// A Direct3D 9 failure that is not a lost device (D3DERR_DRIVERINTERNALERROR).
constexpr int32_t kOtherDeviceFailure = static_cast<int32_t>(0x88760827);
/// A success code other than D3D_OK.
constexpr int32_t kOtherSuccess = 1;
/// Another Microsoft adapter's device identifier, which is no rasteriser.
constexpr uint32_t kOtherMicrosoftDevice = 0x008E;
/// PCI vendor identifiers of real graphics cards.
constexpr uint32_t kNvidiaVendor = 0x10DE;
constexpr uint32_t kIntelVendor = 0x8086;
/// A device identifier of a real graphics card.
constexpr uint32_t kCardDevice = 0x2503;

/// Names drivers give for rasterisers that draw on the processor, and for
/// drivers layered over one.
constexpr std::array<std::string_view, 14> kProcessorNames{
    "llvmpipe (LLVM 15.0.7, 256 bits)",
    "zink Vulkan 1.3(llvmpipe (LLVM 15.0.7, 256 bits) (MESA_LLVMPIPE))",
    "virgl (LLVMPIPE (LLVM 12.0.0, 256 bits))",
    "D3D12 (Microsoft Basic Render Driver)",
    "Microsoft Basic Render Driver",
    "softpipe",
    "lavapipe",
    "Mesa X11 swrast",
    "Software Rasterizer",
    "Google SwiftShader",
    "SwiftShader Device (Subzero)",
    "GDI Generic",
    "Apple Software Renderer",
    "LLVMPIPE",
};

/// Names drivers give for virtual machines' adapters.
constexpr std::array<std::string_view, 6> kVirtualNames{
    "VirtualBox Graphics Adapter (WDDM)",
    "VMware SVGA 3D",
    "SVGA3D; build: RELEASE;  LLVM;",
    "Chromium",
    "VMWARE",
    "virtualbox",
};

/// Names drivers give for real graphics cards, and for adapters that draw
/// on a real card although they run in a virtual machine.
constexpr std::array<std::string_view, 9> kCardNames{
    "NVIDIA GeForce RTX 3060",
    "AMD Radeon RX 6800 XT",
    "Intel(R) UHD Graphics 620",
    "Mesa Intel(R) UHD Graphics 620 (KBL GT2)",
    "Apple M2",
    "AMD Radeon Pro 5500M OpenGL Engine",
    "Parallels Display Adapter (WDDM)",
    "V3D 4.2",
    "",
};

/// Every name of the software and virtual rasteriser list, in any letter
/// case, and the Basic Render Driver's identifiers.
void test_rasteriser_names() {
    for (const auto name : kProcessorNames) {
        OA_CHECK(names_software_rasteriser(name, 0, 0));
        OA_CHECK(!names_virtual_adapter(name));
    }
    for (const auto name : kVirtualNames) {
        OA_CHECK(names_software_rasteriser(name, 0, 0));
        OA_CHECK(names_virtual_adapter(name));
    }
    for (const auto name : kCardNames) {
        OA_CHECK(!names_software_rasteriser(name, kNvidiaVendor, kCardDevice));
        OA_CHECK(!names_virtual_adapter(name));
    }
    // WARP, whatever name its description gives.
    OA_CHECK(names_software_rasteriser("", kBasicRenderVendor, kBasicRenderDevice));
    OA_CHECK(names_software_rasteriser("Some adapter", kBasicRenderVendor, kBasicRenderDevice));
    OA_CHECK(!names_software_rasteriser("Some adapter", kBasicRenderVendor, kOtherMicrosoftDevice));
    OA_CHECK(!names_software_rasteriser("Some adapter", kIntelVendor, kBasicRenderDevice));
    // VirtualBox's older pass-through by its OpenGL vendor string alone.
    OA_CHECK(vendor_names_virtual_adapter("Humper"));
    OA_CHECK(vendor_names_virtual_adapter("HUMPER"));
    OA_CHECK(!vendor_names_virtual_adapter("NVIDIA Corporation"));
    OA_CHECK(!vendor_names_virtual_adapter("Mesa"));
    // A vendor string is matched for humper only: Mesa's software
    // rasterisers name a virtual machine's maker as their vendor.
    OA_CHECK(!vendor_names_virtual_adapter("VMware, Inc."));
}

/// SDL reports a fixed texture limit for vulkan and gpu, and the device's
/// own for every other driver, its software renderer included; the names
/// are matched as SDL gives them.
void test_fixed_texture_limit() {
    OA_CHECK(reports_fixed_texture_limit("vulkan"));
    OA_CHECK(reports_fixed_texture_limit("gpu"));
    for (const std::string_view renderer :
         {"software", "opengl", "opengles2", "direct3d", "direct3d11", "direct3d12", "metal", ""})
        OA_CHECK(!reports_fixed_texture_limit(renderer));
    OA_CHECK(!reports_fixed_texture_limit("Vulkan"));
    OA_CHECK(!reports_fixed_texture_limit("gpu "));
}

/// direct3d12, vulkan and gpu need their adapter read, in SDL's own
/// spelling; no other driver does.
void test_adapter_needed() {
    for (const std::string_view renderer : {"direct3d12", "vulkan", "gpu"})
        OA_CHECK(adapter_needed(renderer));
    for (const std::string_view renderer :
         {"software", "opengl", "opengles2", "direct3d", "direct3d11", "metal", "", "Vulkan"})
        OA_CHECK(!adapter_needed(renderer));
}

/// The class the accelerated tier has been run on is this build's own
/// system and architecture with one driver; software never is, and an ARM
/// processor of Apple's is never untried.
void test_run_class() {
    OA_CHECK(!accelerated_tier_run("software"));
    OA_CHECK(!accelerated_tier_run(""));
    OA_CHECK(!accelerated_tier_run("opengl"));
#if defined(__APPLE__) && (defined(__aarch64__) || defined(__arm64__))
    OA_CHECK(accelerated_tier_run("metal"));
    OA_CHECK(!accelerated_tier_run("direct3d11"));
    OA_CHECK(!untried_arm_processor());
#elif defined(__APPLE__)
    OA_CHECK(!accelerated_tier_run("metal"));
    OA_CHECK(!untried_arm_processor());
#elif defined(_WIN32) &&                                                                           \
    (defined(_M_X64) || defined(__x86_64__) || defined(_M_IX86) || defined(__i386__)) &&           \
    !defined(_M_ARM64EC)
    // Both the x86 and the x64 Windows packages have been run on direct3d11.
    OA_CHECK(accelerated_tier_run("direct3d11"));
    OA_CHECK(!accelerated_tier_run("direct3d"));
    OA_CHECK(!accelerated_tier_run("direct3d12"));
    OA_CHECK(!accelerated_tier_run("metal"));
#endif
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
    OA_CHECK(!untried_arm_processor());
#endif
}

/// On Windows before Vista only direct3d, SDL's Direct3D 9 renderer, may be
/// accelerated, in SDL's own spelling.
void test_capable_before_vista() {
    OA_CHECK(capable_before_vista("direct3d"));
    for (const std::string_view renderer :
         {"opengl",
          "opengles2",
          "direct3d11",
          "direct3d12",
          "software",
          "vulkan",
          "gpu",
          "",
          "Direct3D",
          "direct3d "})
        OA_CHECK(!capable_before_vista(renderer));
}

/// Only direct3d, SDL's Direct3D 9 renderer, loses its device in ordinary
/// use, in SDL's own spelling.
void test_loses_device_in_ordinary_use() {
    OA_CHECK(loses_device_in_ordinary_use("direct3d"));
    for (const std::string_view renderer :
         {"opengl", "direct3d11", "direct3d12", "vulkan", "metal", "software", "", "Direct3D"})
        OA_CHECK(!loses_device_in_ordinary_use(renderer));
}

/// Only direct3d, SDL's Direct3D 9 renderer, resets its device at each
/// change of the wait for the display, in SDL's own spelling.
void test_vertical_sync_resets_device() {
    OA_CHECK(vertical_sync_resets_device("direct3d"));
    for (const std::string_view renderer :
         {"opengl", "direct3d11", "direct3d12", "vulkan", "metal", "software", "", "Direct3D"})
        OA_CHECK(!vertical_sync_resets_device(renderer));
}

/// What a stand-in reader gives describe_reported, and how often it was
/// asked.
struct StandInAdapter {
    bool answer{};           ///< what the reader returns
    std::string_view name{}; ///< the adapter's name it gives
    uint32_t device_limit{}; ///< the device's texture limit it gives; 0 for none
    bool software_flag{};    ///< the software flag it gives
    int reads{};             ///< how often it was asked
};

/// Reads a StandInAdapter into the facts, as a graphics interface's reader
/// would.
///
/// @param context the StandInAdapter
/// @param[in,out] facts the facts
/// @return the stand-in's answer
bool read_stand_in(void* context, AdapterFacts& facts) {
    auto* adapter = static_cast<StandInAdapter*>(context);
    ++adapter->reads;
    facts.adapter = adapter->name;
    facts.device_texture_limit = adapter->device_limit;
    facts.software_flag = adapter->software_flag;
    return adapter->answer;
}

/// What describe reads for a renderer other than SDL's software one: with
/// AdapterRead::skip, nothing, whatever the renderer, and SDL's report of
/// the texture limit alone; with AdapterRead::read, the adapter, named,
/// classified and with the device's limit, or unknown where the reader
/// fails or names nothing; and for SDL's software renderer never the
/// reader, which has no adapter to read. The limits are kept as reported
/// and read.
void test_describe_reported() {
    constexpr int64_t kFixedReport = 16384;
    constexpr uint32_t kDeviceLimit = 4096;
    StandInAdapter card{true, "NVIDIA GeForce RTX 3060", kDeviceLimit, false, 0};
    const AdapterReader reader{&card, read_stand_in};
    // Skipped: the reader is never asked, and the adapter and the device's
    // limit are empty.
    AdapterFacts facts =
        describe_reported("vulkan", "x11", kFixedReport, AdapterRead::skip, reader);
    OA_CHECK(card.reads == 0);
    OA_CHECK(facts.renderer == "vulkan");
    OA_CHECK(facts.video_driver == "x11");
    OA_CHECK(facts.adapter_state == AdapterState::skipped);
    OA_CHECK(facts.adapter.empty());
    OA_CHECK(facts.reported_texture_limit == kFixedReport);
    OA_CHECK(facts.device_texture_limit == 0);
    OA_CHECK(!software_or_virtual(facts));
    facts = describe_reported("gpu", "cocoa", kFixedReport, AdapterRead::skip, reader);
    OA_CHECK(card.reads == 0);
    OA_CHECK(facts.adapter_state == AdapterState::skipped);
    OA_CHECK(facts.reported_texture_limit == kFixedReport);
    OA_CHECK(facts.device_texture_limit == 0);
    facts = describe_reported("direct3d11", "windows", kFixedReport, AdapterRead::skip, reader);
    OA_CHECK(card.reads == 0);
    OA_CHECK(facts.adapter_state == AdapterState::skipped);
    OA_CHECK(facts.reported_texture_limit == kFixedReport);
    // Read: the card's name, cleaned, and its own texture limit.
    card.name = " NVIDIA GeForce RTX 3060\n";
    facts = describe_reported("vulkan", "x11", kFixedReport, AdapterRead::read, reader);
    OA_CHECK(card.reads == 1);
    OA_CHECK(facts.adapter_state == AdapterState::read);
    OA_CHECK(facts.adapter == "NVIDIA GeForce RTX 3060");
    OA_CHECK(facts.reported_texture_limit == kFixedReport);
    OA_CHECK(facts.device_texture_limit == kDeviceLimit);
    OA_CHECK(!software_or_virtual(facts));
    // A software rasteriser by name, and by the interface's flag.
    StandInAdapter lavapipe{true, "llvmpipe (LLVM 15.0.7, 256 bits)", 0, false, 0};
    facts = describe_reported(
        "vulkan", "x11", kFixedReport, AdapterRead::read, AdapterReader{&lavapipe, read_stand_in}
    );
    OA_CHECK(facts.adapter_state == AdapterState::read);
    OA_CHECK(facts.software_rasteriser);
    OA_CHECK(facts.device_texture_limit == 0);
    StandInAdapter flagged{true, "Some adapter", 0, true, 0};
    facts = describe_reported(
        "direct3d12",
        "windows",
        kFixedReport,
        AdapterRead::read,
        AdapterReader{&flagged, read_stand_in}
    );
    OA_CHECK(facts.software_rasteriser);
    // Unknown where the reader fails, where its name cleans to nothing, and
    // where there is no reader.
    StandInAdapter failing{false, "", 0, false, 0};
    facts = describe_reported(
        "gpu", "x11", kFixedReport, AdapterRead::read, AdapterReader{&failing, read_stand_in}
    );
    OA_CHECK(failing.reads == 1);
    OA_CHECK(facts.adapter_state == AdapterState::unknown);
    OA_CHECK(facts.reported_texture_limit == kFixedReport);
    OA_CHECK(facts.device_texture_limit == 0);
    StandInAdapter blank{true, " \t ", 0, false, 0};
    facts = describe_reported(
        "opengl", "x11", kFixedReport, AdapterRead::read, AdapterReader{&blank, read_stand_in}
    );
    OA_CHECK(facts.adapter_state == AdapterState::unknown);
    OA_CHECK(facts.adapter.empty());
    facts = describe_reported("metal", "cocoa", kFixedReport, AdapterRead::read, AdapterReader{});
    OA_CHECK(facts.adapter_state == AdapterState::unknown);
    // SDL's software renderer: no adapter, read or not, and no limit.
    card.reads = 0;
    for (const AdapterRead read : {AdapterRead::read, AdapterRead::skip}) {
        facts = describe_reported("software", "dummy", 0, read, reader);
        OA_CHECK(facts.adapter_state == AdapterState::none);
        OA_CHECK(facts.reported_texture_limit == 0);
        OA_CHECK(facts.device_texture_limit == 0);
        OA_CHECK(facts.software_rasteriser);
    }
    OA_CHECK(card.reads == 0);
}

/// The classification of facts as describe fills them.
void test_classify() {
    AdapterFacts card{};
    card.renderer = "direct3d11";
    card.adapter = "NVIDIA GeForce RTX 3060";
    card.vendor_id = kNvidiaVendor;
    card.device_id = kCardDevice;
    classify(card);
    OA_CHECK(!card.software_rasteriser);
    OA_CHECK(!card.virtual_adapter);
    OA_CHECK(!software_or_virtual(card));
    // DXGI's software flag, and a Vulkan device of CPU type, as described.
    AdapterFacts flagged = card;
    flagged.software_flag = true;
    classify(flagged);
    OA_CHECK(flagged.software_rasteriser);
    OA_CHECK(software_or_virtual(flagged));
    // WARP by its identifiers.
    AdapterFacts warp{};
    warp.renderer = "direct3d12";
    warp.adapter = "Some adapter";
    warp.vendor_id = kBasicRenderVendor;
    warp.device_id = kBasicRenderDevice;
    classify(warp);
    OA_CHECK(warp.software_rasteriser);
    // A virtual machine's adapter, by name and by vendor string.
    AdapterFacts virtual_card{};
    virtual_card.renderer = "direct3d";
    virtual_card.adapter = "VMware SVGA 3D";
    classify(virtual_card);
    OA_CHECK(virtual_card.virtual_adapter);
    OA_CHECK(!virtual_card.software_rasteriser);
    OA_CHECK(software_or_virtual(virtual_card));
    AdapterFacts humper{};
    humper.renderer = "opengl";
    humper.adapter = "Chromium";
    humper.vendor = "Humper";
    classify(humper);
    OA_CHECK(humper.virtual_adapter);
    AdapterFacts humper_vendor{};
    humper_vendor.renderer = "opengl";
    humper_vendor.adapter = "Some renderer";
    humper_vendor.vendor = "Humper";
    classify(humper_vendor);
    OA_CHECK(humper_vendor.virtual_adapter);
    // llvmpipe whose vendor string names a virtual machine's maker is a
    // software rasteriser, not a virtual adapter.
    AdapterFacts llvmpipe{};
    llvmpipe.renderer = "opengl";
    llvmpipe.adapter = "llvmpipe (LLVM 15.0.7, 256 bits)";
    llvmpipe.vendor = "VMware, Inc.";
    classify(llvmpipe);
    OA_CHECK(llvmpipe.software_rasteriser);
    OA_CHECK(!llvmpipe.virtual_adapter);
    // SDL's software renderer draws on the processor, whatever else is set.
    AdapterFacts software{};
    software.renderer = "software";
    classify(software);
    OA_CHECK(software.software_rasteriser);
    // Wine alone makes any adapter virtual.
    AdapterFacts wine = card;
    wine.wine = true;
    classify(wine);
    OA_CHECK(!wine.software_rasteriser);
    OA_CHECK(software_or_virtual(wine));
}

/// Control characters become spaces, the ends are trimmed, and a long name
/// is cut at the limit without splitting a character.
void test_clean_name() {
    OA_CHECK(clean_name("  Apple M2 \n") == "Apple M2");
    OA_CHECK(clean_name("Radeon\tRX\x01 6800") == "Radeon RX  6800");
    OA_CHECK(clean_name("") == "");
    OA_CHECK(clean_name(" \t\r\n") == "");
    const std::string long_name(adapter_name_limit + 20, 'x');
    OA_CHECK(clean_name(long_name).size() == adapter_name_limit);
    // A two-byte character across the limit is left out whole.
    std::string split(adapter_name_limit - 1, 'x');
    split += "\xc3\xa9";
    OA_CHECK(clean_name(split).size() == adapter_name_limit - 1);
}

/// The video drivers whose windows have a framebuffer of their own, in any
/// letter case, and those that present the software renderer through a
/// hardware render driver.
void test_native_window_framebuffer() {
    OA_CHECK(native_window_framebuffer("windows"));
    OA_CHECK(native_window_framebuffer("x11"));
    OA_CHECK(native_window_framebuffer("dummy"));
    OA_CHECK(native_window_framebuffer("offscreen"));
    OA_CHECK(native_window_framebuffer("X11"));
    OA_CHECK(native_window_framebuffer("Windows"));
    OA_CHECK(!native_window_framebuffer("cocoa"));
    OA_CHECK(!native_window_framebuffer("wayland"));
    OA_CHECK(!native_window_framebuffer("kmsdrm"));
    OA_CHECK(!native_window_framebuffer(""));
    OA_CHECK(!native_window_framebuffer("x1"));
    OA_CHECK(!native_window_framebuffer("dummy2"));
}

/// SDL's software renderer on the dummy video driver: its name, no adapter
/// to read, no texture limit, and a software rasteriser; asked to skip the
/// adapter, the same. A null renderer gives empty facts.
void test_software_renderer() {
    const AdapterFacts none = describe(nullptr, AdapterRead::read);
    OA_CHECK(none.renderer.empty());
    OA_CHECK(none.adapter_state == AdapterState::skipped);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        OA_CHECK(false);
        return;
    }
    constexpr int kWindowWidth = 64;
    constexpr int kWindowHeight = 48;
    SDL_Window* window = SDL_CreateWindow("render probe test", kWindowWidth, kWindowHeight, 0);
    OA_CHECK(window != nullptr);
    SDL_Renderer* renderer =
        window != nullptr ? SDL_CreateRenderer(window, software_renderer.data()) : nullptr;
    OA_CHECK(renderer != nullptr);
    if (renderer != nullptr) {
        const AdapterFacts facts = describe(renderer, AdapterRead::read);
        OA_CHECK(facts.renderer == software_renderer);
        OA_CHECK(facts.video_driver == std::string_view(SDL_GetCurrentVideoDriver()));
        OA_CHECK(facts.adapter_state == AdapterState::none);
        OA_CHECK(facts.adapter.empty());
        OA_CHECK(facts.reported_texture_limit == 0);
        OA_CHECK(facts.device_texture_limit == 0);
        OA_CHECK(facts.software_rasteriser);
        OA_CHECK(software_or_virtual(facts));
        const AdapterFacts skipped = describe(renderer, AdapterRead::skip);
        OA_CHECK(skipped.renderer == software_renderer);
        OA_CHECK(skipped.adapter_state == AdapterState::none);
        OA_CHECK(skipped.reported_texture_limit == 0);
        SDL_DestroyRenderer(renderer);
    }
    if (window != nullptr)
        SDL_DestroyWindow(window);
    SDL_Quit();
}

/// No device names no adapter. Where SDL's video driver can make a graphics
/// device, the device's adapter is the name SDL gives it from SDL 3.4,
/// cleaned. On the dummy video driver the test runs on, which makes none,
/// that part is skipped; run by hand on a Mac's own driver it names the
/// Metal device.
void test_gpu_device_adapter() {
    OA_CHECK(gpu_device_adapter(nullptr).empty());
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        OA_CHECK(false);
        return;
    }
    SDL_GPUDevice* device =
        SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL, false, nullptr);
    if (device == nullptr) {
        std::printf("no graphics device on this video driver: %s\n", SDL_GetError());
    } else {
        const std::string adapter = gpu_device_adapter(device);
        const char* driver = SDL_GetGPUDeviceDriver(device);
        std::printf("graphics device: %s (%s)\n", adapter.c_str(), driver != nullptr ? driver : "");
#if SDL_VERSION_ATLEAST(3, 4, 0)
        OA_CHECK(!adapter.empty());
        OA_CHECK(adapter == clean_name(adapter));
#else
        OA_CHECK(adapter.empty());
#endif
        SDL_DestroyGPUDevice(device);
    }
    SDL_Quit();
}

#if defined(_WIN32)
/// The libraries of the graphics interfaces the readers reach, which no
/// Windows build links: Direct3D 9, 11 and 12, DXGI, OpenGL, OpenGL ES 2
/// through EGL, and Vulkan.
constexpr std::array<std::string_view, 8> kGraphicsLibraries = {
    "d3d9.dll",
    "d3d11.dll",
    "d3d12.dll",
    "dxgi.dll",
    "opengl32.dll",
    "libegl.dll",
    "libglesv2.dll",
    "vulkan-1.dll",
};

/// Returns a library name in lower case.
///
/// @param name the name, as the import table spells it
/// @return the name with A to Z lowered
std::string lowered(std::string_view name) {
    std::string result(name);
    std::transform(result.begin(), result.end(), result.begin(), [](char letter) {
        return letter >= 'A' && letter <= 'Z' ? static_cast<char>(letter - 'A' + 'a') : letter;
    });
    return result;
}

/// Lists the libraries a loaded image imports, its delay-loaded ones too.
///
/// @param image the image, as the system loaded it
/// @return the libraries' names, in lower case
std::vector<std::string> imported_libraries(HMODULE image) {
    const auto* base = reinterpret_cast<const unsigned char*>(image);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* headers = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    const auto& directories = headers->OptionalHeader.DataDirectory;
    std::vector<std::string> names;
    const auto& imports = directories[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (imports.VirtualAddress != 0)
        for (auto* entry =
                 reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + imports.VirtualAddress);
             entry->Name != 0;
             ++entry)
            names.push_back(lowered(reinterpret_cast<const char*>(base + entry->Name)));
    const auto& delayed = directories[IMAGE_DIRECTORY_ENTRY_DELAY_IMPORT];
    if (delayed.VirtualAddress != 0)
        for (auto* entry =
                 reinterpret_cast<const IMAGE_DELAYLOAD_DESCRIPTOR*>(base + delayed.VirtualAddress);
             entry->DllNameRVA != 0;
             ++entry)
            names.push_back(lowered(reinterpret_cast<const char*>(base + entry->DllNameRVA)));
    return names;
}

/// The test's own executable, which links the probe and SDL, imports no
/// graphics library, so one Windows package starts where any is missing.
void test_no_graphics_library_imported() {
    const std::vector<std::string> names = imported_libraries(GetModuleHandleW(nullptr));
    // The executable imports at least the system's own kernel library, so
    // the table was found.
    OA_CHECK(std::find(names.begin(), names.end(), "kernel32.dll") != names.end());
    for (const std::string_view library : kGraphicsLibraries) {
        const bool imported = std::find(names.begin(), names.end(), library) != names.end();
        if (imported)
            std::fprintf(
                stderr, "imports %.*s\n", static_cast<int>(library.size()), library.data()
            );
        OA_CHECK(!imported);
    }
}
#endif

} // namespace

/// A Direct3D 9 device's answers read as its states, and every renderer
/// this test can make, which has no such device, answers unknown.
void test_device_state() {
    OA_CHECK(device_state_from_result(0) == DeviceState::ok);
    OA_CHECK(device_state_from_result(device_lost_result) == DeviceState::lost);
    OA_CHECK(device_state_from_result(device_not_reset_result) == DeviceState::not_reset);
    // Another failure, and another success code, are neither.
    OA_CHECK(device_state_from_result(kOtherDeviceFailure) == DeviceState::unknown);
    OA_CHECK(device_state_from_result(kOtherSuccess) == DeviceState::unknown);
    OA_CHECK(device_state(nullptr) == DeviceState::unknown);
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        std::fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        OA_CHECK(false);
        return;
    }
    constexpr int kWindowWidth = 64;
    constexpr int kWindowHeight = 48;
    SDL_Window* window = SDL_CreateWindow("render probe test", kWindowWidth, kWindowHeight, 0);
    OA_CHECK(window != nullptr);
    SDL_Renderer* renderer =
        window != nullptr ? SDL_CreateRenderer(window, software_renderer.data()) : nullptr;
    OA_CHECK(renderer != nullptr);
    if (renderer != nullptr) {
        OA_CHECK(device_state(renderer) == DeviceState::unknown);
        SDL_DestroyRenderer(renderer);
    }
    if (window != nullptr)
        SDL_DestroyWindow(window);
    SDL_Quit();
}

int main() {
    test_rasteriser_names();
    test_device_state();
    test_fixed_texture_limit();
    test_adapter_needed();
    test_run_class();
    test_capable_before_vista();
    test_loses_device_in_ordinary_use();
    test_vertical_sync_resets_device();
    test_describe_reported();
    test_classify();
    test_clean_name();
    test_native_window_framebuffer();
    test_software_renderer();
    test_gpu_device_adapter();
#if defined(_WIN32)
    test_no_graphics_library_imported();
#endif
    return oa::test::check_exit_status();
}

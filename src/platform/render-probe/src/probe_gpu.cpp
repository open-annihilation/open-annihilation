// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The adapter behind one of SDL's graphics devices: the name SDL gives the
// device, for the gpu renderer and for a device the game made itself. SDL
// can name a device only from SDL 3.4; on SDL 3.2 the adapter reads as
// unknown. SDL gives no texture limit for the device, so the device's own
// limit is left unread (0).
#include "readers.hpp"

#include <SDL3/SDL.h>

namespace oa::platform::render_probe {

std::string gpu_device_adapter([[maybe_unused]] SDL_GPUDevice* device) {
#if SDL_VERSION_ATLEAST(3, 4, 0)
    if (device == nullptr)
        return {};
    const char* name = SDL_GetStringProperty(
        SDL_GetGPUDeviceProperties(device), SDL_PROP_GPU_DEVICE_NAME_STRING, nullptr
    );
    return name != nullptr ? clean_name(name) : std::string();
#else
    return {};
#endif
}

namespace readers {

bool read_gpu_adapter(
    [[maybe_unused]] SDL_Renderer* renderer, [[maybe_unused]] AdapterFacts& facts
) {
#if SDL_VERSION_ATLEAST(3, 4, 0)
    facts.adapter = gpu_device_adapter(SDL_GetGPURendererDevice(renderer));
    return !facts.adapter.empty();
#else
    return false;
#endif
}

} // namespace readers
} // namespace oa::platform::render_probe

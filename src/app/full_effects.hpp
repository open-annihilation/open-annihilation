// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Optional world effects between Full's GPU draw and its interface overlays.
#pragma once

#include <SDL3/SDL.h>
#include <cstdint>
#include <functional>
#include <span>
#include <string>

namespace oa::app::full_effects {

/// Reusable upload of the processed battlefield, released before its renderer.
struct Upload {
    SDL_Texture* texture{}; ///< RGBA streaming texture
    int width{};            ///< allocated columns
    int height{};           ///< allocated rows
    /// Releases the reusable upload texture.
    ~Upload();
    Upload() = default;
    Upload(const Upload&) = delete;
    Upload& operator=(const Upload&) = delete;
    /// Releases the texture and its recorded dimensions.
    void reset() noexcept;
};

/// Mutable RGBA pixels and their mapping to battlefield layout coordinates.
struct Pixels {
    std::span<uint8_t> bytes;
    uint32_t width{};
    uint32_t height{};
    uint32_t pitch{};
    float origin_x{}; ///< battlefield coordinate of the first output pixel
    float origin_y{};
    float scale_x{1}; ///< output pixels per battlefield layout pixel
    float scale_y{1};
    int row_offset{}; ///< cropped rows relative to the complete battlefield readback
};

/// What a pass needs of the world the card drew.
enum class Pass : uint8_t {
    /// The world's own pixels, which heat distortion moves: they are read
    /// back, processed and drawn back in place. The readback waits for the
    /// card to finish drawing the world, supersampled or not.
    world,
    /// Light alone, which only adds to the world: `process` paints it over
    /// opaque black and the card adds the result to the world, which is
    /// never read back. Adding is exact: a channel the processor would have
    /// raised by whole steps, clamped at 255, is raised by the same steps.
    light,
};

/// Reads and processes only the battlefield, restoring window and target state.
///
/// A failure leaves the world as the card drew it: nothing is drawn over it
/// unless the processed pixels were uploaded whole.
///
/// @param renderer renderer holding the completed world
/// @param upload reusable output texture
/// @param battlefield window layout rectangle, excluding interface strips
/// @param moved_target zoomed-out world target, or null for the window
/// @param moved_factor target pixels per layout pixel; the target has a one-pixel border
/// @param process compositor called before the processed pixels are uploaded
/// @param effect_bounds optional affected rectangle in battlefield coordinates, including sampling margin
/// @param fits the memory guard, asked for the bytes the readback and its
///        conversion, or the light pass's layer, and a grown upload hold
///        together before any is made; empty allows every size
/// @param final_target the texture the window's frame is presented into, or null for the window
/// @param pass whether the world is read back and drawn back, or only added to
/// @return empty on success, otherwise the SDL failure or the guard's refusal
std::string compose(
    SDL_Renderer* renderer,
    Upload& upload,
    const SDL_Rect& battlefield,
    SDL_Texture* moved_target,
    uint32_t moved_factor,
    const std::function<void(const Pixels&)>& process,
    const SDL_FRect* effect_bounds = nullptr,
    const std::function<bool(uint64_t bytes)>& fits = {},
    SDL_Texture* final_target = nullptr,
    Pass pass = Pass::world
);

} // namespace oa::app::full_effects

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
/// @param fits the memory guard, asked for the bytes the readback, its
///        conversion and a grown upload hold together before any is made;
///        empty allows every size
/// @param final_target the texture the window's frame is presented into, or null for the window
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
    SDL_Texture* final_target = nullptr
);

} // namespace oa::app::full_effects

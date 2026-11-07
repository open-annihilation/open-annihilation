// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The zoom photo mode offers. It runs from the whole map to eight times
// normal size, and each tier keeps only the zooms that tier draws. Pure:
// no SDL, no Runtime, and no player's settings.
#pragma once

#include <cstdint>

namespace oa::app {

/// The closest photo mode zooms in, in screen pixels per map pixel: eight
/// times normal size, past the closest play zooms in.
inline constexpr float kPhotoClosestZoom = 8.0F;

/// Automatic's floor for the processor and the turned view, when a map has
/// no fit. The same number as kMinBattlefieldZoom. Named here so this header
/// does not include the game's options.
inline constexpr float kPhotoProcessorFloor = 0.5F;

/// The closest Full's classic view zooms out, and Automatic's floor for that
/// view when a map has no fit. The same number as kMinFullBattlefieldZoom.
inline constexpr float kPhotoFullFloor = kPhotoProcessorFloor / 3.0F;

/// Which picture is drawing the battlefield, and so which zooms it can draw.
enum class PhotoTier : uint8_t {
    /// The processor, including its far view.
    processor,
    /// Full, in the classic orientation. The processor's far view is not this
    /// picture.
    full_classic,
    /// The turned view. It draws its own frames, zoomed out as well.
    turned,
};

/// Why a tier's offer stops short of the whole map. none when it does not.
enum class PhotoZoomCut : uint8_t {
    /// The whole map through eight times is offered.
    none,
    /// The graphics card's classic view draws no farther out than its floor.
    card,
};

/// The zoom photo mode offers on one tier: farthest out, closest in, and
/// whether the tier stopped short of the whole map.
struct PhotoZoomRange {
    float least{};      ///< farthest out, screen pixels per map pixel
    float most{};       ///< closest in, screen pixels per map pixel
    PhotoZoomCut cut{}; ///< why least sits closer than the whole map
};

/// Returns the zoom photo mode offers on a tier for one map and battlefield.
///
/// Out to the whole map, as Maximum zoom out's Whole map does, and in to
/// kPhotoClosestZoom. The processor and the turned view offer that range.
/// Full's classic view stops at the closest it draws itself, which is its
/// own floor, when the whole map sits farther out than that. The player's
/// Maximum zoom settings are not an argument.
///
/// @param tier the picture in use
/// @param map_width map pixels across the shown map
/// @param map_height map pixels down it
/// @param battlefield_width screen pixels across the battlefield
/// @param battlefield_height screen pixels down it
/// @return the offer; least is at most most, most is kPhotoClosestZoom, and
///         least is never farther out than the whole map's fit
[[nodiscard]] PhotoZoomRange photo_zoom_range(
    PhotoTier tier,
    int32_t map_width,
    int32_t map_height,
    int32_t battlefield_width,
    int32_t battlefield_height
) noexcept;

} // namespace oa::app

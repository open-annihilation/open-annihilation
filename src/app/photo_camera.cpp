// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "photo_camera.hpp"

#include "oa/app/far_view.hpp"

#include <algorithm>

namespace oa::app {

PhotoZoomRange photo_zoom_range(
    PhotoTier tier,
    int32_t map_width,
    int32_t map_height,
    int32_t battlefield_width,
    int32_t battlefield_height
) noexcept {
    namespace settings = oa::ui::engine_settings;
    // Whole map ignores this floor whenever the map and the battlefield
    // have a fit. It is the answer only when they do not.
    const float automatic_floor =
        tier == PhotoTier::full_classic ? kPhotoFullFloor : kPhotoProcessorFloor;
    const float whole_map = least_battlefield_zoom(
        settings::ZoomOutLimit::whole_map,
        automatic_floor,
        map_width,
        map_height,
        battlefield_width,
        battlefield_height
    );
    PhotoZoomRange offer;
    offer.most = kPhotoClosestZoom;
    offer.least = whole_map;
    offer.cut = PhotoZoomCut::none;
    // Below Full's floor the processor's far view would draw the frame.
    // The classic card stops at the floor and says so.
    if (tier == PhotoTier::full_classic && offer.least < kPhotoFullFloor) {
        offer.least = kPhotoFullFloor;
        offer.cut = PhotoZoomCut::card;
    }
    if (offer.least > offer.most)
        offer.least = offer.most;
    return offer;
}

} // namespace oa::app

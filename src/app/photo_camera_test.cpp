// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Photo mode's zoom (photo_camera.hpp): the whole map through eight times
// on the processor and the turned view, Full's classic view no farther out
// than the zoom it draws itself, on a map that fits inside the battlefield,
// one the whole map sits between Full's floor and normal size, one farther
// than that floor, one past the farthest any view goes, and no map. The
// player's Maximum zoom settings are not read.
#include "photo_camera.hpp"

#include "oa/app/far_view.hpp"
#include "oa/test/check.hpp"
#include "oa/ui/engine_settings.hpp"

#include <cstdint>

namespace {

using oa::app::PhotoTier;
using oa::app::PhotoZoomCut;
using oa::app::PhotoZoomRange;
using oa::ui::engine_settings::ZoomInLimit;
using oa::ui::engine_settings::ZoomOutLimit;

/// A battlefield the checks measure maps against.
constexpr int32_t kBattlefieldWide = 640;
constexpr int32_t kBattlefieldHigh = 480;

/// @param left one offer
/// @param right the other
/// @return true when the two offers are the same
bool same_offer(const PhotoZoomRange& left, const PhotoZoomRange& right) {
    return left.least == right.least && left.most == right.most && left.cut == right.cut;
}

/// The whole map's fit, held to the farthest any view goes and never forced
/// in past normal size. The same rule least_battlefield_zoom uses.
float whole_map_floor(PhotoTier tier, int32_t map_wide, int32_t map_high) {
    const float automatic =
        tier == PhotoTier::full_classic ? oa::app::kPhotoFullFloor : oa::app::kPhotoProcessorFloor;
    return oa::app::least_battlefield_zoom(
        ZoomOutLimit::whole_map, automatic, map_wide, map_high, kBattlefieldWide, kBattlefieldHigh
    );
}

/// The offer on one tier, and the rules every tier keeps.
PhotoZoomRange offer_for(PhotoTier tier, int32_t map_wide, int32_t map_high) {
    const PhotoZoomRange offer =
        oa::app::photo_zoom_range(tier, map_wide, map_high, kBattlefieldWide, kBattlefieldHigh);
    const float whole = whole_map_floor(tier, map_wide, map_high);
    OA_CHECK(offer.most == oa::app::kPhotoClosestZoom);
    OA_CHECK(offer.least <= offer.most);
    OA_CHECK(offer.least + 1.0e-6F >= whole);
    OA_CHECK(offer.most <= oa::app::kPhotoClosestZoom);
    return offer;
}

/// A map the battlefield holds with room to spare stops at normal size, in
/// every tier. Eight times is still the close end.
void a_map_that_fits_stops_at_normal_size() {
    constexpr int32_t map_wide = 100;
    constexpr int32_t map_high = 80;
    const float whole = whole_map_floor(PhotoTier::processor, map_wide, map_high);
    OA_CHECK(whole == 1.0F);
    for (const PhotoTier tier :
         {PhotoTier::processor, PhotoTier::full_classic, PhotoTier::turned}) {
        const PhotoZoomRange offer = offer_for(tier, map_wide, map_high);
        OA_CHECK(offer.least == 1.0F);
        OA_CHECK(offer.cut == PhotoZoomCut::none);
    }
}

/// The whole map between Full's floor and normal size is offered on every
/// tier. Full draws that zoom itself.
void a_middle_map_is_offered_on_every_tier() {
    constexpr int32_t map_wide = 2000;
    constexpr int32_t map_high = 2000;
    const float whole = whole_map_floor(PhotoTier::processor, map_wide, map_high);
    OA_CHECK(whole > oa::app::kPhotoFullFloor);
    OA_CHECK(whole < 1.0F);
    const PhotoZoomRange processor = offer_for(PhotoTier::processor, map_wide, map_high);
    const PhotoZoomRange card = offer_for(PhotoTier::full_classic, map_wide, map_high);
    const PhotoZoomRange turned = offer_for(PhotoTier::turned, map_wide, map_high);
    OA_CHECK(processor.least == whole && processor.cut == PhotoZoomCut::none);
    OA_CHECK(same_offer(card, processor));
    OA_CHECK(same_offer(turned, processor));
}

/// Farther than Full's floor, the processor and the turned view still offer
/// the whole map. Full's classic view stops at its floor.
void a_large_map_stops_full_at_its_floor() {
    constexpr int32_t map_wide = 8192;
    constexpr int32_t map_high = 8192;
    const float whole = whole_map_floor(PhotoTier::processor, map_wide, map_high);
    OA_CHECK(whole < oa::app::kPhotoFullFloor);
    OA_CHECK(whole > oa::app::furthest_battlefield_zoom);
    const PhotoZoomRange processor = offer_for(PhotoTier::processor, map_wide, map_high);
    const PhotoZoomRange turned = offer_for(PhotoTier::turned, map_wide, map_high);
    const PhotoZoomRange card = offer_for(PhotoTier::full_classic, map_wide, map_high);
    OA_CHECK(processor.least == whole && processor.cut == PhotoZoomCut::none);
    OA_CHECK(same_offer(turned, processor));
    OA_CHECK(card.least == oa::app::kPhotoFullFloor);
    OA_CHECK(card.cut == PhotoZoomCut::card);
    OA_CHECK(card.most == oa::app::kPhotoClosestZoom);
}

/// Past the farthest any view goes, the offer stops there, and Full still
/// stops at its own floor when that is closer.
void a_huge_map_stops_at_the_farthest_zoom() {
    constexpr int32_t map_wide = 200000;
    constexpr int32_t map_high = 200000;
    const float whole = whole_map_floor(PhotoTier::processor, map_wide, map_high);
    OA_CHECK(whole == oa::app::furthest_battlefield_zoom);
    const PhotoZoomRange processor = offer_for(PhotoTier::processor, map_wide, map_high);
    const PhotoZoomRange turned = offer_for(PhotoTier::turned, map_wide, map_high);
    const PhotoZoomRange card = offer_for(PhotoTier::full_classic, map_wide, map_high);
    OA_CHECK(processor.least == oa::app::furthest_battlefield_zoom);
    OA_CHECK(processor.cut == PhotoZoomCut::none);
    OA_CHECK(same_offer(turned, processor));
    OA_CHECK(card.least == oa::app::kPhotoFullFloor);
    OA_CHECK(card.cut == PhotoZoomCut::card);
}

/// Without a map the offer keeps the tier's own floor, and does not claim
/// the card stopped short of a fit that is not there.
void no_map_keeps_the_tiers_floor() {
    const PhotoZoomRange processor = offer_for(PhotoTier::processor, 0, 0);
    const PhotoZoomRange turned = offer_for(PhotoTier::turned, 0, 0);
    const PhotoZoomRange card = offer_for(PhotoTier::full_classic, 0, 0);
    OA_CHECK(processor.least == oa::app::kPhotoProcessorFloor);
    OA_CHECK(processor.cut == PhotoZoomCut::none);
    OA_CHECK(same_offer(turned, processor));
    OA_CHECK(card.least == oa::app::kPhotoFullFloor);
    OA_CHECK(card.cut == PhotoZoomCut::none);
}

/// The range does not read Maximum zoom out or Maximum zoom in.
void the_players_zoom_settings_are_not_read() {
    oa::ui::engine_settings::EngineSettings chosen{};
    chosen.max_zoom_out = ZoomOutLimit::one_quarter;
    chosen.max_zoom_in = ZoomInLimit::twice;
    const oa::ui::engine_settings::EngineSettings kept = chosen;
    const PhotoZoomRange wide = oa::app::photo_zoom_range(
        PhotoTier::processor, 8192, 8192, kBattlefieldWide, kBattlefieldHigh
    );
    const PhotoZoomRange card = oa::app::photo_zoom_range(
        PhotoTier::full_classic, 8192, 8192, kBattlefieldWide, kBattlefieldHigh
    );
    OA_CHECK(chosen.max_zoom_out == kept.max_zoom_out);
    OA_CHECK(chosen.max_zoom_in == kept.max_zoom_in);
    OA_CHECK(wide.most == oa::app::kPhotoClosestZoom);
    OA_CHECK(wide.least < oa::app::kPhotoProcessorFloor);
    OA_CHECK(card.least == oa::app::kPhotoFullFloor);
    OA_CHECK(card.most > static_cast<float>(static_cast<int>(ZoomInLimit::four_times)));
}

} // namespace

int main() {
    a_map_that_fits_stops_at_normal_size();
    a_middle_map_is_offered_on_every_tier();
    a_large_map_stops_full_at_its_floor();
    a_huge_map_stops_at_the_farthest_zoom();
    no_map_keeps_the_tiers_floor();
    the_players_zoom_settings_are_not_read();
    return oa::test::check_exit_status();
}

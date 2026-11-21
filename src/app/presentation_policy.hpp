// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#pragma once

#include <cstdint>

namespace oa::app {

/// Which picture a frame is drawn into. Countdown labels are a processor
/// and Full choice; the card draws the world without them.
enum class PresentationTier : uint8_t { processor, full, card };

/// What a frame may draw from. Enhanced Mode is on or off. The revision
/// and the feature set are not inputs.
struct PresentationInputs {
    bool enhanced{};
    bool global_dots{};
    bool headless{};
    bool explicit_check{}; ///< the dedicated windowless live-renderer check
    bool photo{};
    bool director{};
    bool debug_lines{};
    bool gpu_ready{};
    bool retained{};
    PresentationTier tier{PresentationTier::processor};
    bool prototype_root_prefix{};
    /// Graphics → Lighter far units. Off leaves the original picture.
    bool lighter_far_units{};
};

/// One drawing choice. True means this frame may use it.
struct PresentationPolicy {
    bool root_pose_capture{};
    bool compact_markers{};
    bool body_detail{};
    bool shadow_templates{};
    bool deferred_geometry{};
    bool root_prefix{};
    bool full_display_cadence{};
    bool ground_holds{};
    bool geometry_shadow_metadata{};
    bool initial_ground_queries{};
    bool compact_picking_coordinates{};
    bool checked_integer_rotation{};
    bool countdown_overlays{};
};

/// Decides every drawing choice for one frame.
///
/// Choices that leave the picture unchanged turn on with Enhanced Mode.
/// Choices that change the picture also need Lighter far units, and they
/// stay off while Enhanced Mode is off. Root-prefix bodies also need the
/// prototype flag. Storage for deferred geometry ignores a photo frame:
/// the caller passes photo false for that allocation.
///
/// @param inputs Enhanced Mode, Graphics and this frame
/// @return the choices this frame may use
[[nodiscard]] constexpr PresentationPolicy
presentation_policy(const PresentationInputs& inputs) noexcept {
    PresentationPolicy policy;
    const bool lighter = inputs.enhanced && inputs.lighter_far_units;
    const bool marker_frame = inputs.global_dots && inputs.gpu_ready && !inputs.debug_lines &&
                              !inputs.director && !inputs.retained;
    // The frames after leaving Dots match full capture, so root capture
    // follows Enhanced Mode. The caller still requires a dots frame.
    policy.root_pose_capture = inputs.enhanced;
    policy.compact_markers =
        marker_frame && (inputs.explicit_check || (!inputs.headless && inputs.enhanced));
    policy.body_detail = lighter && !inputs.director && !inputs.photo;
    policy.shadow_templates = policy.body_detail;
    policy.deferred_geometry = lighter && !inputs.photo;
    policy.root_prefix = policy.body_detail && inputs.prototype_root_prefix;
    policy.full_display_cadence = inputs.enhanced;
    policy.ground_holds = inputs.enhanced;
    // Mirrored players stay excluded by local_geometry_shadow_frame.
    policy.geometry_shadow_metadata = inputs.enhanced && !inputs.director && !inputs.photo;
    policy.initial_ground_queries = inputs.enhanced && !inputs.photo;
    policy.compact_picking_coordinates = inputs.enhanced;
    policy.checked_integer_rotation = inputs.enhanced;
    policy.countdown_overlays =
        inputs.enhanced && inputs.global_dots && inputs.tier != PresentationTier::card;
    return policy;
}

} // namespace oa::app

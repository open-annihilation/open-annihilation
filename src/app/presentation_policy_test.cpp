// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Drawing choices follow Enhanced Mode, Graphics and the frame. The
// revision is not an input.
#include "presentation_policy.hpp"

#include <cstdio>

namespace {

namespace app = oa::app;

int failures = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #condition);     \
            ++failures;                                                                            \
        }                                                                                          \
    } while (false)

app::PresentationInputs plain() {
    app::PresentationInputs inputs;
    inputs.global_dots = true;
    inputs.gpu_ready = true;
    inputs.tier = app::PresentationTier::processor;
    return inputs;
}

/// Every choice that can be on in this ordinary frame.
bool mode_group(const app::PresentationPolicy& policy) {
    return policy.root_pose_capture && policy.compact_markers && policy.full_display_cadence &&
           policy.ground_holds && policy.geometry_shadow_metadata &&
           policy.initial_ground_queries && policy.compact_picking_coordinates &&
           policy.checked_integer_rotation && policy.countdown_overlays;
}

bool option_group(const app::PresentationPolicy& policy) {
    return policy.body_detail && policy.shadow_templates && policy.deferred_geometry;
}

bool any_choice(const app::PresentationPolicy& policy) {
    return option_group(policy) || policy.root_prefix || mode_group(policy);
}

void test_enhanced_off_draws_nothing_new() {
    auto inputs = plain();
    inputs.lighter_far_units = true;
    inputs.prototype_root_prefix = true;
    CHECK(!any_choice(app::presentation_policy(inputs)));
}

void test_mode_group_follows_enhanced_mode() {
    auto inputs = plain();
    const auto policy = app::presentation_policy(inputs);
    inputs.enhanced = true;
    const auto enhanced = app::presentation_policy(inputs);
    CHECK(!mode_group(policy));
    CHECK(mode_group(enhanced));
    CHECK(!option_group(enhanced));
    CHECK(!enhanced.root_prefix);
}

void test_option_group_needs_the_switch_and_enhanced_mode() {
    auto inputs = plain();
    inputs.enhanced = true;
    inputs.lighter_far_units = true;
    inputs.prototype_root_prefix = true;
    const auto on = app::presentation_policy(inputs);
    CHECK(option_group(on));
    CHECK(on.root_prefix);
    CHECK(on.shadow_templates == on.body_detail);
    inputs.prototype_root_prefix = false;
    CHECK(!app::presentation_policy(inputs).root_prefix);
    CHECK(app::presentation_policy(inputs).body_detail);
    inputs.lighter_far_units = false;
    CHECK(!option_group(app::presentation_policy(inputs)));
    CHECK(!app::presentation_policy(inputs).root_prefix);
}

void test_frame_exclusions() {
    auto inputs = plain();
    inputs.enhanced = true;
    inputs.lighter_far_units = true;
    inputs.prototype_root_prefix = true;
    inputs.director = true;
    auto directed = app::presentation_policy(inputs);
    CHECK(!directed.compact_markers);
    CHECK(!directed.body_detail);
    CHECK(!directed.shadow_templates);
    CHECK(!directed.root_prefix);
    CHECK(!directed.geometry_shadow_metadata);
    CHECK(directed.full_display_cadence);
    CHECK(directed.ground_holds);
    CHECK(directed.deferred_geometry);
    CHECK(directed.root_pose_capture);
    inputs.director = false;
    inputs.photo = true;
    auto photo = app::presentation_policy(inputs);
    CHECK(!photo.body_detail);
    CHECK(!photo.deferred_geometry);
    CHECK(!photo.root_prefix);
    CHECK(!photo.initial_ground_queries);
    CHECK(!photo.geometry_shadow_metadata);
    CHECK(photo.full_display_cadence);
    inputs.photo = false;
    inputs.debug_lines = true;
    CHECK(!app::presentation_policy(inputs).compact_markers);
    CHECK(app::presentation_policy(inputs).body_detail);
    inputs.debug_lines = false;
    inputs.headless = true;
    CHECK(!app::presentation_policy(inputs).compact_markers);
    inputs.explicit_check = true;
    CHECK(app::presentation_policy(inputs).compact_markers);
    inputs.enhanced = false;
    inputs.lighter_far_units = false;
    const auto dedicated = app::presentation_policy(inputs);
    CHECK(dedicated.compact_markers);
    CHECK(!dedicated.root_pose_capture);
    CHECK(!dedicated.body_detail);
    CHECK(!dedicated.deferred_geometry);
    CHECK(!dedicated.full_display_cadence);
    inputs.director = true;
    CHECK(!app::presentation_policy(inputs).compact_markers);
}

void test_countdown_and_card() {
    auto inputs = plain();
    inputs.enhanced = true;
    CHECK(app::presentation_policy(inputs).countdown_overlays);
    inputs.global_dots = false;
    CHECK(!app::presentation_policy(inputs).countdown_overlays);
    inputs.global_dots = true;
    inputs.tier = app::PresentationTier::full;
    CHECK(app::presentation_policy(inputs).countdown_overlays);
    inputs.tier = app::PresentationTier::card;
    CHECK(!app::presentation_policy(inputs).countdown_overlays);
    inputs.tier = app::PresentationTier::processor;
    inputs.retained = true;
    CHECK(!app::presentation_policy(inputs).compact_markers);
    inputs.retained = false;
    inputs.gpu_ready = false;
    CHECK(!app::presentation_policy(inputs).compact_markers);
}

} // namespace

int main() {
    test_enhanced_off_draws_nothing_new();
    test_mode_group_follows_enhanced_mode();
    test_option_group_needs_the_switch_and_enhanced_mode();
    test_frame_exclusions();
    test_countdown_and_card();
    return failures == 0 ? 0 : 1;
}

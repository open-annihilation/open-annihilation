// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Included after the real ARMPW has moved, animated and warmed on screen.
// This observes aggregate stock-frame LOD emission, not a particular unit's
// emitted limbs. The Scene component test separately checks indexed moving
// limb coordinates. Frozen comparisons retain the same physical LOD policy.
{
    namespace mode = oa::data::enhanced_mode;
    if (preplan_version != mode::cohort_target_version) {
        require(
            bench_timers::body_counts.body_lod_models == 0 &&
                bench_timers::body_counts.body_lod_vertices == 0,
            "a legacy rules frame emitted enhanced animated body geometry"
        );
    } else if (preplan_zoom < 1) {
        const auto prior_zoom = match_zoom_;
        const auto prior_zoom_target = match_zoom_target_;
        // Actual conservative posed bounds are just above the mesh band at
        // 0.35. Move the real camera farther out into the physical body band.
        constexpr float body_proof_zoom = 0.315F;
        match_zoom_ = match_zoom_target_ = body_proof_zoom;
        center_camera_on_unit(walker);
        render_match_surface();
        const auto require_bodies = [&] {
            const auto& counts = bench_timers::body_counts;
            require(
                live_renderer_active() && !live_renderer_failed() && walker_visible() &&
                    match_zoom() == body_proof_zoom &&
                    counts.full_mesh_models + counts.body_lod_models != 0 &&
                    counts.body_lod_models != 0 && counts.body_lod_vertices != 0 &&
                    counts.distance_markers == 0 && counts.budget_markers == 0,
                "the actual low-zoom rules-10 frame did not emit aggregate physical body geometry"
            );
        };
        require_bodies();
        const auto first_body_picture = match_world_cpu_;
        bool moving_stock_frame{};
        constexpr unsigned maximum_body_animation_ticks = 12;
        for (unsigned tick = 0; tick < maximum_body_animation_ticks && !moving_stock_frame;
             ++tick) {
            const auto before = match_->world().record.units[walker].position;
            step_match_simulation();
            set_presentation_alpha(0.5F);
            render_match_surface();
            require_bodies();
            require(!match_->fault(), "the real animated body tick raised a simulation fault");
            const auto after = match_->world().record.units[walker].position;
            const auto& motion = models.presentation.units[walker];
            moving_stock_frame = (before.x != after.x || before.z != after.z) &&
                                 motion.pieces_moved && motion.rig_valid && !motion.rig_primed &&
                                 match_world_cpu_.rgb != first_body_picture.rgb;
        }
        require(
            moving_stock_frame,
            "the low-zoom stock frame did not retain real walking and animated source poses"
        );
        const auto body_tick = uint32_t{match_->simulation().tick};
        const auto body_presentation = match_timing_.tick;
        const auto body_random = match_->random_state();
        const auto body_picture = match_world_cpu_;
        const auto body_picks = bounds_and_picks();
        const auto body_readback = match_draw_read_back();
        UnitPose body_pose_before, body_pose_after;
        capture_unit_pose(
            match_->world().record.units[walker],
            match_->runtime_state(walker).instance->model(),
            body_pose_before
        );
        render_match_surface();
        require_bodies();
        capture_unit_pose(
            match_->world().record.units[walker],
            match_->runtime_state(walker).instance->model(),
            body_pose_after
        );
        require(
            match_world_cpu_.rgb == body_picture.rgb && bounds_and_picks() == body_picks &&
                poses_equal(body_pose_before, body_pose_after) &&
                match_->simulation().tick == body_tick && match_timing_.tick == body_presentation &&
                match_->random_state() == body_random,
            "a frozen animated body redraw changed pixels, picks, raw pose, clocks or RNG"
        );
        if (const char* difference =
                draw_read_back_difference(body_readback, match_draw_read_back()))
            throw std::runtime_error(
                std::string("frozen physical body rendering changed derived readback: ") +
                difference
            );
        const auto& counts = bench_timers::body_counts;
        std::cout
            << "live renderer check: aggregate stock rules-10 physical bodies at zoom "
            << match_zoom() << ": " << counts.body_lod_models << "; logical body vertices " << counts.body_lod_vertices
            << "; zero distance/budget markers; real walking/source-pose animation; frozen pixels, "
               "picks, pose, clocks and RNG preserved\n";
        match_zoom_ = prior_zoom;
        match_zoom_target_ = prior_zoom_target;
        center_camera_on_unit(walker);
        render_match_surface();
        require(match_zoom() == prior_zoom && match_zoom_target_ == prior_zoom_target,
            "the physical-body proof did not restore the original camera zoom");
    }
}

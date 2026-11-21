// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Included after every released windowless renderer control. The opt-in
// witness uses stock simulation movement and real successful GPU frames.
if (root_prefix_requested) {
    const auto prefix_settings_kept = engine_settings();
    const auto prefix_limit_kept = frontend_game().max_units_setting;
    const auto prefix_map_kept = skirmish_settings_.map_name;
    const bool prefix_headless_kept = options_.headless_check;
    const auto prefix_rung_kept = accelerated_.rung;

    struct RestorePrefixObservation {
        bool enabled{plan_work::enabled};
        plan_work::PlanWorkCounts counts{plan_work::frame_counts};

        ~RestorePrefixObservation() {
            plan_work::enabled = enabled;
            plan_work::frame_counts = counts;
        }
    } restore_prefix_observation;

    constexpr int prefix_width = 1600;
    constexpr int prefix_height = 1000;
    require(
        sdl_.window == nullptr && sdl_.renderer == nullptr,
        "the root-prefix witness must create its own windowless Full renderer"
    );
    SDL_Surface* prefix_canvas =
        SDL_CreateSurface(prefix_width, prefix_height, SDL_PIXELFORMAT_ARGB8888);
    require(prefix_canvas != nullptr, "the root-prefix Full canvas allocation failed");
    sdl_.renderer = SDL_CreateSoftwareRenderer(prefix_canvas);
    if (sdl_.renderer == nullptr) {
        SDL_DestroySurface(prefix_canvas);
        require(false, "the root-prefix windowless Full renderer failed");
    }
    const auto finish_prefix_check = [&] {
        options_.prototype_root_prefix = false;
        set_full_presentation(false);
        free_accelerated_presentation();
        destroy_match_layer_textures();
        accelerated_.on = false;
        SDL_DestroyRenderer(sdl_.renderer);
        sdl_.renderer = nullptr;
        SDL_DestroySurface(prefix_canvas);
        options_.headless_check = prefix_headless_kept;
        apply_engine_settings(prefix_settings_kept);
        frontend_game().max_units_setting = prefix_limit_kept;
        skirmish_settings_.map_name = prefix_map_kept;
    };
    try {
        auto prefix_settings = prefix_settings_kept;
        constexpr uint16_t prefix_unit_limit = 1000;
        prefix_settings.unit_limit = std::max(prefix_settings.unit_limit, prefix_unit_limit);
        prefix_settings.hardware_acceleration = oa::ui::engine_settings::HardwareAcceleration::full;
        prefix_settings.zoomed_out_units = oa::ui::engine_settings::ZoomedOutUnits::rendered;
        prefix_settings.new_unit_shadows = true;
        prefix_settings.lighter_far_units = true;
        prefix_settings.allow_full_3d = true;
        apply_engine_settings(prefix_settings);
        options_.headless_check = false;
        options_.prototype_root_prefix = true;
        plan_work::enabled = true;
        constexpr std::size_t prefix_actors = 257;
        constexpr int32_t prefix_columns = 20;
        constexpr int32_t prefix_rows = 20;
        constexpr int32_t prefix_move_cells_x = 2;
        constexpr int32_t prefix_move_cells_z = 1;
        constexpr int32_t prefix_fixed = 65536;
        constexpr unsigned prefix_walk_ticks = 180;
        constexpr uint32_t prefix_commander_count = 2;
        constexpr int32_t prefix_view_margin_x = 256;
        constexpr int32_t prefix_view_margin_z = 768;
        constexpr auto prefix_visible_flag =
            static_cast<uint16_t>(sim::model_runtime::PieceFlag::visible);
        // Compare this same live SFX object's storage around a render, including
        // its separately allocated emitter ring; no save encoding omits a field.
        const auto effect_bytes = [&] {
            const auto& effects = match_->effects();
            const auto* bytes = reinterpret_cast<const uint8_t*>(&effects);
            std::vector<uint8_t> result(bytes, bytes + sizeof effects);
            const auto& block = effects.emitter_block;
            if (block.count != 0) {
                require(block.slots != nullptr, "the root-prefix SFX emitter block is missing");
                const auto* emitters = reinterpret_cast<const uint8_t*>(block.slots.get());
                result.insert(
                    result.end(), emitters, emitters + block.count * sizeof block.slots[0]
                );
            }
            return result;
        };
        for (const auto version : {mode::cohort_target_version, mode::cohort_retention_version}) {
            start_ground_match(version, "Core Prime Industrial Area");
            enter_console_check_line("+nowisee");
            clear_local_selection();
            switch_accelerated_presentation(true, prefix_rung_kept);
            set_full_presentation(true);
            set_live_models_on_card(true);
            require(full_presentation(), "the root-prefix planner did not enter Full");
            const std::array<uint16_t, 2> prefix_types{
                sim::unit_spawn::find_type_index(spawn_type_names_, "ARMPW"),
                sim::unit_spawn::find_type_index(spawn_type_names_, "CORAK")
            };
            require(
                prefix_types[0] != 0 && prefix_types[1] != 0,
                "a root-prefix stock actor type is unavailable"
            );
            const auto arm_commander =
                sim::unit_spawn::find_type_index(spawn_type_names_, "ARMCOM");
            const auto core_commander =
                sim::unit_spawn::find_type_index(spawn_type_names_, "CORCOM");
            uint32_t original_commanders{};
            for (const auto& slot : match_->world().slots) {
                if (!slot.unit || slot.record.type_index == 0)
                    continue;
                require(
                    slot.record.type_index == arm_commander ||
                        slot.record.type_index == core_commander,
                    "a non-commander preceded the root-prefix actors"
                );
                ++original_commanders;
            }
            require(
                original_commanders == prefix_commander_count,
                "the root-prefix world did not start with exactly two commanders"
            );
            int32_t footprint_x{}, footprint_z{};
            for (const auto type : prefix_types) {
                const auto& definition = spawn_types_[type];
                require(
                    definition.footprint_x > 0 && definition.footprint_x <= 2 &&
                        definition.footprint_z > 0 && definition.footprint_z <= 2,
                    "a root-prefix stock footprint exceeds the checked grid"
                );
                footprint_x = std::max(footprint_x, int32_t{definition.footprint_x});
                footprint_z = std::max(footprint_z, int32_t{definition.footprint_z});
            }
            const int32_t spacing_x = footprint_x + prefix_move_cells_x;
            const int32_t spacing_z = footprint_z + prefix_move_cells_x;
            const int32_t patch_width =
                (prefix_columns - 1) * spacing_x + footprint_x + prefix_move_cells_x;
            const int32_t patch_height =
                (prefix_rows - 1) * spacing_z + footprint_z + prefix_move_cells_z;
            const int32_t cells_x = match_->state().game.map_width;
            const int32_t cells_z = match_->state().game.map_height;
            std::vector<std::array<int32_t, 2>> sites;
            for (int32_t z = 4; sites.empty() && z + patch_height < cells_z - 4; z += 8) {
                for (int32_t x = 4; sites.empty() && x + patch_width < cells_x - 4; x += 8) {
                    const bool another_unit = std::any_of(
                        match_->world().slots.begin(),
                        match_->world().slots.end(),
                        [&](const auto& slot) {
                            if (!slot.unit || slot.record.type_index == 0)
                                return false;
                            const auto position = slot.record.position;
                            const int32_t px = position.x / prefix_fixed;
                            const int32_t pz = position.z / prefix_fixed;
                            return px >= x * 16 - prefix_view_margin_x &&
                                   px <= (x + patch_width) * 16 + prefix_view_margin_x &&
                                   pz >= z * 16 - prefix_view_margin_z &&
                                   pz <= (z + patch_height) * 16 + prefix_view_margin_z;
                        }
                    );
                    if (another_unit)
                        continue;
                    std::vector<std::array<int32_t, 2>> candidate;
                    for (int32_t row = 0; row < prefix_rows; ++row) {
                        for (int32_t column = 0; column < prefix_columns; ++column) {
                            const int32_t sx = x + column * spacing_x;
                            const int32_t sz = z + row * spacing_z;
                            if (std::all_of(
                                    prefix_types.begin(), prefix_types.end(), [&](const auto type) {
                                        return match_->site_clear_for(type, sx, sz, 0, 1) &&
                                               match_->site_clear_for(type, sx + 1, sz, 0, 1) &&
                                               match_->site_clear_for(type, sx + 1, sz + 1, 0, 1) &&
                                               match_->site_clear_for(
                                                   type,
                                                   sx + prefix_move_cells_x,
                                                   sz + prefix_move_cells_z,
                                                   0,
                                                   1
                                               );
                                    }
                                ))
                                candidate.push_back({sx, sz});
                        }
                    }
                    if (candidate.size() >= prefix_actors)
                        sites = std::move(candidate);
                }
            }
            require(sites.size() >= prefix_actors, "no checked root-prefix stock patch fits");
            sites.resize(prefix_actors);
            std::vector<uint16_t> actors;
            actors.reserve(prefix_actors);
            for (const auto& site : sites) {
                const auto type = prefix_types[actors.size() % prefix_types.size()];
                const auto& definition = spawn_types_[type];
                require(
                    match_->site_clear_for(type, site[0], site[1], 0, 1),
                    "a root-prefix stock spawn site became occupied"
                );
                const int32_t x = site[0] * 16 + definition.footprint_x * 8 - 8;
                const int32_t z = site[1] * 16 + definition.footprint_z * 8 - 8;
                const auto floor = match_->map_height(
                    static_cast<uint32_t>(x * prefix_fixed), static_cast<uint32_t>(z * prefix_fixed)
                );
                require(
                    floor >= match_->state().game.sea_level, "the root-prefix patch is submerged"
                );
                sim::unit_spawn::Request request;
                request.player = match_local_player_;
                request.type = type;
                request.finished = true;
                request.state = kGroundOccupancyState;
                request.position = {
                    static_cast<uint32_t>(x * prefix_fixed),
                    static_cast<uint32_t>(floor * prefix_fixed),
                    static_cast<uint32_t>(z * prefix_fixed)
                };
                const auto* placed = match_->create(request);
                require(
                    placed && placed->unit && placed->record.type_index == type &&
                        placed->record.id != 0 &&
                        std::find(actors.begin(), actors.end(), placed->unit_index) == actors.end(),
                    "a root-prefix stock actor did not spawn in a distinct slot"
                );
                actors.push_back(placed->unit_index);
            }
            int64_t min_x = std::numeric_limits<int64_t>::max();
            int64_t max_x = std::numeric_limits<int64_t>::min();
            int64_t min_z = min_x, max_z = max_x;
            for (const auto slot : actors) {
                const auto position = match_->world().record.units[slot].position;
                min_x = std::min(min_x, int64_t{position.x});
                max_x = std::max(max_x, int64_t{position.x});
                min_z = std::min(min_z, int64_t{position.z});
                max_z = std::max(max_z, int64_t{position.z});
                const int32_t x = position.x + prefix_move_cells_x * 16 * prefix_fixed;
                const int32_t z = position.z + prefix_move_cells_z * 16 * prefix_fixed;
                const auto floor =
                    match_->map_height(static_cast<uint32_t>(x), static_cast<uint32_t>(z));
                require(
                    !match_->order_refused(
                        match_->issue_ground_move(slot, {x, floor * prefix_fixed, z}, false)
                    ),
                    "a root-prefix stock movement order was refused"
                );
            }
            auto anchor = actors.front();
            uint64_t nearest = std::numeric_limits<uint64_t>::max();
            for (const auto slot : actors) {
                const auto position = match_->world().record.units[slot].position;
                const int64_t dx = (position.x - (min_x + max_x) / 2) / prefix_fixed;
                const int64_t dz = (position.z - (min_z + max_z) / 2) / prefix_fixed;
                const auto distance = static_cast<uint64_t>(dx * dx + dz * dz);
                if (distance < nearest) {
                    nearest = distance;
                    anchor = slot;
                }
            }
            auto& graphics = match_->state().game.graphics_flags;
            graphics = static_cast<uint16_t>(
                graphics | oa::present::model::graphics_shadows |
                oa::present::model::graphics_vehicle_shadows
            );
            match_layout_ = lay_out_match(prefix_width, prefix_height);
            match_zoom_ = match_zoom_target_ = 1;
            require(turn_live_camera(0, -35), "the root-prefix stock camera did not turn");
            center_camera_on_unit(anchor);
            std::vector<uint8_t> animated(actors.size());
            std::vector<uint8_t> moving_actors(actors.size());
            std::vector<FixedVec3> before_positions(actors.size());
            std::vector<UnitPose> frozen_poses(actors.size());
            std::vector<std::vector<uint16_t>> visible_flags(actors.size());
            uint32_t animated_count{};
            uint32_t witness_moving{};
            uint64_t witness_frame{}, witness_publishes{};
            RootPrefixActivationCounts witness_counts{};
            for (unsigned tick = 0;
                 tick < prefix_walk_ticks && (animated_count < actors.size() || witness_frame == 0);
                 ++tick) {
                for (std::size_t actor = 0; actor < actors.size(); ++actor)
                    before_positions[actor] = match_->world().record.units[actors[actor]].position;
                step_match_simulation();
                set_presentation_alpha(0.5F);
                const uint32_t frozen_tick = match_->simulation().tick;
                const auto frozen_presentation = match_timing_.tick;
                const auto frozen_random = match_->random_state();
                const auto frozen_world = match_world_digest();
                const auto frozen_effects = effect_bytes();
                for (std::size_t actor = 0; actor < actors.size(); ++actor) {
                    const auto slot = actors[actor];
                    auto* runtime = match_->runtime_state(slot).instance.get();
                    require(runtime != nullptr, "a root-prefix canonical rig is missing");
                    capture_unit_pose(
                        match_->world().record.units[slot], runtime->model(), frozen_poses[actor]
                    );
                    auto& flags = visible_flags[actor];
                    flags.clear();
                    for (const auto& piece : runtime->model().pieces())
                        flags.push_back(static_cast<uint16_t>(piece.flags & prefix_visible_flag));
                }
                render_match_surface();
                const auto captured = plan_work::frame_counts;
                const auto activation = live_root_prefix_activation_counts(captured.frame_id);
                require(
                    full_presentation() && !match_->fault() && live_renderer_active() &&
                        !live_renderer_failed() && !live_renderer_unavailable() &&
                        turned_view_device() != nullptr && sdl_.window == nullptr &&
                        captured.completed && captured.frame_id != 0,
                    "the root-prefix witness needs a complete successful windowless Full GPU frame"
                );
                require(
                    match_->simulation().tick == frozen_tick &&
                        match_timing_.tick == frozen_presentation &&
                        match_->random_state() == frozen_random &&
                        match_world_digest() == frozen_world && effect_bytes() == frozen_effects,
                    "root-prefix rendering changed the canonical world, VM, RNG, clocks or SFX"
                );
                const auto& presentation = match_models().presentation.units;
                uint32_t moving_now{};
                std::fill(moving_actors.begin(), moving_actors.end(), uint8_t{});
                for (std::size_t actor = 0; actor < actors.size(); ++actor) {
                    const auto slot = actors[actor];
                    const auto& canonical = match_->runtime_state(slot).instance->model();
                    UnitPose pose;
                    capture_unit_pose(match_->world().record.units[slot], canonical, pose);
                    require(
                        poses_equal(pose, frozen_poses[actor]),
                        "root-prefix rendering changed a canonical pose"
                    );
                    require(
                        canonical.pieces().size() == visible_flags[actor].size(),
                        "root-prefix rendering changed a canonical piece list"
                    );
                    for (std::size_t piece = 0; piece < canonical.pieces().size(); ++piece)
                        require(
                            (canonical.pieces()[piece].flags & prefix_visible_flag) ==
                                visible_flags[actor][piece],
                            "root-prefix rendering changed canonical piece visibility"
                        );
                    if (slot >= presentation.size())
                        continue;
                    const auto position = match_->world().record.units[slot].position;
                    const auto before = before_positions[actor];
                    const auto& motion = presentation[slot];
                    const bool moving = (position.x != before.x || position.z != before.z) &&
                                        motion.continued && motion.moved && motion.pieces_moved &&
                                        motion.rig_valid && !motion.rig_primed;
                    if (moving) {
                        moving_actors[actor] = 1;
                        ++moving_now;
                        if (!animated[actor]) {
                            animated[actor] = 1;
                            ++animated_count;
                        }
                    }
                }
                const auto publishes = captured.work[static_cast<std::size_t>(
                    plan_work::Work::root_prefix_recipe_publishes
                )];
                if (moving_now == 0 || publishes == 0 || activation.scene_instances == 0 ||
                    activation.gpu_instances == 0 || activation.gpu_surface_indices == 0)
                    continue;
                std::array<uint32_t, 2> actor_recipes{};
                for (const auto& draw : match_models().draws.models) {
                    if (!draw.root_prefix || !draw.plan.linked.drawn || draw.preplan_omitted ||
                        draw.stand_in >= 0 || !draw.model.unit ||
                        draw.root_prefix->rotation.sine == 0 ||
                        draw.root_prefix->rotation.cosine == 0)
                        continue;
                    const auto slot =
                        std::find_if(actors.begin(), actors.end(), [&](const auto actor) {
                            return draw.model.unit->id == match_->world().record.units[actor].id;
                        });
                    if (slot != actors.end()) {
                        const auto actor = static_cast<std::size_t>(slot - actors.begin());
                        if (moving_actors[actor] && live_unit_bounds(actors[actor]))
                            ++actor_recipes[actor % prefix_types.size()];
                    }
                }
                if (actor_recipes[0] == 0 || actor_recipes[1] == 0)
                    continue;
                witness_frame = captured.frame_id;
                witness_publishes = publishes;
                witness_counts = activation;
                witness_moving = moving_now;
            }
            require(
                animated_count == actors.size(),
                "not every root-prefix stock actor translated and animated within the tick bound"
            );
            require(
                witness_frame != 0 && witness_publishes > 0 && witness_counts.scene_instances > 0 &&
                    witness_counts.gpu_instances > 0 && witness_counts.gpu_surface_indices > 0 &&
                    witness_moving > 0,
                "the moving stock army never activated all four root-prefix production stages in "
                "one frame"
            );
            std::cout << "live renderer check: root-prefix activation rules=" << version
                      << " root_prefix_recipe_publishes=" << witness_publishes
                      << " root_prefix_scene_instances=" << witness_counts.scene_instances
                      << " root_prefix_gpu_instances=" << witness_counts.gpu_instances
                      << " root_prefix_gpu_surface_indices=" << witness_counts.gpu_surface_indices
                      << " source_frame=" << witness_frame << " actors=" << actors.size()
                      << " moving_animated=" << animated_count
                      << " same_frame_moving_animated=" << witness_moving << '\n';
        }
    } catch (...) {
        finish_prefix_check();
        throw;
    }
    finish_prefix_check();
}

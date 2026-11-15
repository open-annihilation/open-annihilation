// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Included in the actual windowless renderer check after its ground cases.
// Ordinary stock movement supplies the poses; successful backend frames alone
// supply the cache counters. No model definition or piece flags are changed.
{
    const auto stock_settings_kept = engine_settings();
    const auto stock_limit_kept = frontend_game().max_units_setting;
    const auto stock_map_kept = skirmish_settings_.map_name;
    auto stock_settings = stock_settings_kept;
    constexpr uint16_t stock_unit_limit = 1000;
    stock_settings.unit_limit = std::max(stock_settings.unit_limit, stock_unit_limit);
    stock_settings.zoomed_out_units = oa::ui::engine_settings::ZoomedOutUnits::rendered;
    stock_settings.new_unit_shadows = true;
    apply_engine_settings(stock_settings);
    try {
        constexpr std::size_t stock_casters = 257;
        constexpr int32_t stock_columns = 20;
        constexpr int32_t stock_rows = 20;
        constexpr int32_t stock_move_cells = 2;
        constexpr int32_t stock_fixed = 65536;
        constexpr unsigned stock_walk_ticks = 180;
        constexpr unsigned stock_warm_frames = 16;
        constexpr uint32_t stock_foreign_draw_limit = 2;
        constexpr uint32_t stock_casts_per_foreign_draw = 3;
        static_assert(
            stock_foreign_draw_limit * stock_casts_per_foreign_draw <
            renderer_live_gpu::shadow_template_minimum_casts
        );
        constexpr int32_t stock_view_margin_x = 256;
        constexpr int32_t stock_view_margin_z = 768;
        for (const auto version : {mode::cohort_target_version, mode::cohort_retention_version}) {
            for (const std::string_view stock_name : {"ARMPW", "CORAK"}) {
                start_ground_match(version, "Core Prime Industrial Area");
                enter_console_check_line("+nowisee");
                clear_local_selection();
                const auto type = sim::unit_spawn::find_type_index(spawn_type_names_, stock_name);
                require(type != 0, "the stock shadow-cache type is unavailable");
                const auto arm_commander =
                    sim::unit_spawn::find_type_index(spawn_type_names_, "ARMCOM");
                const auto core_commander =
                    sim::unit_spawn::find_type_index(spawn_type_names_, "CORCOM");
                std::vector<std::pair<uint16_t, uint16_t>> original_commanders;
                for (const auto& slot : match_->world().slots) {
                    if (!slot.unit || slot.record.type_index == 0)
                        continue;
                    const uint16_t commander_type = slot.record.type_index;
                    require(
                        commander_type == arm_commander || commander_type == core_commander,
                        "a non-commander unit preceded the stock shadow-cache actors"
                    );
                    const uint16_t commander_id = slot.record.id;
                    original_commanders.emplace_back(commander_id, commander_type);
                }
                require(
                    original_commanders.size() == stock_foreign_draw_limit,
                    "the stock shadow-cache world did not start with exactly two commanders"
                );
                const auto& definition = spawn_types_[type];
                require(
                    definition.footprint_x > 0 && definition.footprint_x <= 2 &&
                        definition.footprint_z > 0 && definition.footprint_z <= 2,
                    "the stock shadow-cache footprint exceeds the checked grid"
                );
                const int32_t spacing_x = definition.footprint_x + stock_move_cells;
                const int32_t spacing_z = definition.footprint_z + stock_move_cells;
                const int32_t patch_width =
                    (stock_columns - 1) * spacing_x + definition.footprint_x + stock_move_cells;
                const int32_t patch_height = (stock_rows - 1) * spacing_z + definition.footprint_z;
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
                                const int32_t px = position.x / stock_fixed;
                                const int32_t pz = position.z / stock_fixed;
                                return px >= x * 16 - stock_view_margin_x &&
                                       px <= (x + patch_width) * 16 + stock_view_margin_x &&
                                       pz >= z * 16 - stock_view_margin_z &&
                                       pz <= (z + patch_height) * 16 + stock_view_margin_z;
                            }
                        );
                        if (another_unit)
                            continue;
                        std::vector<std::array<int32_t, 2>> candidate;
                        for (int32_t row = 0; row < stock_rows; ++row) {
                            for (int32_t column = 0; column < stock_columns; ++column) {
                                const int32_t sx = x + column * spacing_x;
                                const int32_t sz = z + row * spacing_z;
                                if (match_->site_clear_for(type, sx, sz, 0, 1) &&
                                    match_->site_clear_for(type, sx + 1, sz, 0, 1) &&
                                    match_->site_clear_for(type, sx + stock_move_cells, sz, 0, 1))
                                    candidate.push_back({sx, sz});
                            }
                        }
                        if (candidate.size() >= stock_casters)
                            sites = std::move(candidate);
                    }
                }
                require(sites.size() >= stock_casters, "no checked stock shadow-cache patch fits");
                sites.resize(stock_casters);
                std::vector<uint16_t> actors;
                actors.reserve(stock_casters);
                for (const auto& site : sites) {
                    require(
                        match_->site_clear_for(type, site[0], site[1], 0, 1),
                        "a stock shadow-cache spawn site became occupied"
                    );
                    const int32_t x = site[0] * 16 + definition.footprint_x * 8 - 8;
                    const int32_t z = site[1] * 16 + definition.footprint_z * 8 - 8;
                    const auto floor = match_->map_height(
                        static_cast<uint32_t>(x * stock_fixed),
                        static_cast<uint32_t>(z * stock_fixed)
                    );
                    require(
                        floor >= match_->state().game.sea_level, "the stock patch is submerged"
                    );
                    sim::unit_spawn::Request request;
                    request.player = match_local_player_;
                    request.type = type;
                    request.finished = true;
                    request.state = kGroundOccupancyState;
                    request.position = {
                        static_cast<uint32_t>(x * stock_fixed),
                        static_cast<uint32_t>(floor * stock_fixed),
                        static_cast<uint32_t>(z * stock_fixed)
                    };
                    const auto* placed = match_->create(request);
                    require(placed && placed->unit, "a stock shadow-cache unit did not spawn");
                    actors.push_back(placed->unit_index);
                }
                auto anchor = actors.front();
                const auto low = match_->world().record.units[actors.front()].position;
                int64_t min_x = low.x, max_x = low.x, min_z = low.z, max_z = low.z;
                for (const auto slot : actors) {
                    const auto position = match_->world().record.units[slot].position;
                    min_x = std::min(min_x, int64_t{position.x});
                    max_x = std::max(max_x, int64_t{position.x});
                    min_z = std::min(min_z, int64_t{position.z});
                    max_z = std::max(max_z, int64_t{position.z});
                }
                uint64_t nearest = std::numeric_limits<uint64_t>::max();
                for (const auto slot : actors) {
                    const auto position = match_->world().record.units[slot].position;
                    const int64_t dx = (position.x - (min_x + max_x) / 2) / stock_fixed;
                    const int64_t dz = (position.z - (min_z + max_z) / 2) / stock_fixed;
                    const auto distance = static_cast<uint64_t>(dx * dx + dz * dz);
                    if (distance < nearest) {
                        nearest = distance;
                        anchor = slot;
                    }
                }
                for (const auto slot : actors) {
                    const auto here = match_->world().record.units[slot].position;
                    const int32_t x = here.x + stock_move_cells * 16 * stock_fixed;
                    const auto floor =
                        match_->map_height(static_cast<uint32_t>(x), static_cast<uint32_t>(here.z));
                    require(
                        !match_->order_refused(
                            match_->issue_ground_move(slot, {x, floor * stock_fixed, here.z}, false)
                        ),
                        "a stock shadow-cache movement order was refused"
                    );
                }
                auto& graphics = match_->state().game.graphics_flags;
                graphics = static_cast<uint16_t>(
                    graphics | oa::present::model::graphics_shadows |
                    oa::present::model::graphics_vehicle_shadows
                );
                match_layout_ = lay_out_match(1600, 1000);
                match_zoom_ = match_zoom_target_ = 1;
                require(turn_live_camera(0, -35), "the stock shadow-cache camera did not turn");
                center_camera_on_unit(anchor);
                std::vector<uint8_t> animated(actors.size());
                std::vector<FixedVec3> before_positions(actors.size());
                uint32_t animated_count{};
                for (unsigned tick = 0; tick < stock_walk_ticks && animated_count < actors.size();
                     ++tick) {
                    for (std::size_t actor = 0; actor < actors.size(); ++actor)
                        before_positions[actor] =
                            match_->world().record.units[actors[actor]].position;
                    step_match_simulation();
                    set_presentation_alpha(0.5F);
                    render_match_surface();
                    require(
                        !match_->fault() && live_renderer_active() && !live_renderer_failed(),
                        "the stock shadow-cache army failed to move or render"
                    );
                    const auto& presentation = match_models().presentation.units;
                    for (std::size_t actor = 0; actor < actors.size(); ++actor) {
                        const auto slot = actors[actor];
                        if (animated[actor] || slot >= presentation.size())
                            continue;
                        const auto position = match_->world().record.units[slot].position;
                        const auto before = before_positions[actor];
                        const bool translated = position.x != before.x || position.z != before.z;
                        const auto& motion = presentation[slot];
                        if (translated && motion.continued && motion.moved && motion.pieces_moved &&
                            motion.rig_valid && !motion.rig_primed) {
                            animated[actor] = 1;
                            ++animated_count;
                        }
                    }
                }
                require(
                    animated_count == actors.size(),
                    "not every real stock unit translated and animated within the tick bound"
                );
                const auto held_view = match_view_place();
                release_live_renderer();
                require(turn_live_camera(0, -35), "the cold stock shadow-cache camera failed");
                place_match_view(held_view[0], held_view[1]);
                oa::sim::messages::clear_messages(match_->state().game);
                const uint32_t frozen_tick = match_->simulation().tick;
                const auto frozen_presentation = match_timing_.tick;
                const auto frozen_random = match_->random_state();
                const auto frozen_world = match_world_digest();
                std::vector<UnitPose> frozen_poses(actors.size());
                for (std::size_t actor = 0; actor < actors.size(); ++actor) {
                    const auto slot = actors[actor];
                    auto* instance = match_->runtime_state(slot).instance.get();
                    require(instance != nullptr, "the stock shadow-cache rig is missing");
                    capture_unit_pose(
                        match_->world().record.units[slot], instance->model(), frozen_poses[actor]
                    );
                }
                ShadowIndexCounts cold{}, uploaded{}, warm{};
                std::vector<int32_t> frozen_picks;
                renderer::Surface cold_picture;
                bool saw_cold{}, saw_upload{}, saw_hit{};
                bool reported_foreign_non_caster{};
                uint32_t most_foreign_casts{};
                for (unsigned frame = 0; frame < stock_warm_frames && !saw_hit; ++frame) {
                    bench_timers::shadow_index_counts = {};
                    render_match_surface();
                    require(
                        !match_->fault() && live_renderer_active() && !live_renderer_failed() &&
                            !live_renderer_unavailable() && turned_view_device() != nullptr &&
                            sdl_.window == nullptr,
                        "the stock shadow-cache witness needs a successful offscreen GPU frame"
                    );
                    require(
                        match_->simulation().tick == frozen_tick &&
                            match_timing_.tick == frozen_presentation &&
                            match_->random_state() == frozen_random &&
                            match_world_digest() == frozen_world,
                        "stock shadow-cache warming changed the world, clocks or RNG"
                    );
                    for (std::size_t actor = 0; actor < actors.size(); ++actor) {
                        const auto slot = actors[actor];
                        UnitPose pose;
                        capture_unit_pose(
                            match_->world().record.units[slot],
                            match_->runtime_state(slot).instance->model(),
                            pose
                        );
                        require(
                            poses_equal(pose, frozen_poses[actor]),
                            "stock cache warming changed a canonical pose"
                        );
                    }
                    const auto counts = bench_timers::shadow_index_counts;
                    if (counts.logical_indices == 0)
                        continue;
                    require(
                        live_models_on_card() && live_renderer_marker_count() == 0 &&
                            live_renderer_card_model_count() >= stock_casters &&
                            counts.logical_indices ==
                                counts.dynamic_indices + counts.cached_indices &&
                            !match_world_cpu_.rgb.empty(),
                        "the stock shadow-cache frame has no complete GPU shadow/readback account"
                    );
                    const auto& draws = match_models().draws.models;
                    uint32_t foreign_draws{};
                    for (const auto& draw : draws) {
                        if (!draw.plan.linked.drawn || draw.preplan_omitted)
                            continue;
                        const auto* unit = draw.model.unit;
                        const auto& stand_ins = match_models().draws.stand_ins;
                        if (draw.stand_in >= 0 &&
                            static_cast<std::size_t>(draw.stand_in) < stand_ins.size())
                            unit = &stand_ins[static_cast<std::size_t>(draw.stand_in)];
                        if (unit != nullptr && draw.stand_in < 0 && unit->type_index == type)
                            continue;
                        const uint32_t flags = draw.model.def ? draw.model.def->flags : 0;
                        // shadow_of_draw returns before emitting any shadow for either
                        // condition. A linked scenery body is not itself a shadow cast.
                        // on_map and stand_in alone do not establish that exclusion.
                        const bool no_shadow = (flags & OA_UNIT_DEF_FLAG_NO_SHADOW) != 0;
                        const bool non_caster = !draw.plan.model.from_image || no_shadow;
                        if (!non_caster || !reported_foreign_non_caster) {
                            const auto foreign_type =
                                unit != nullptr ? uint32_t(unit->type_index) : 0;
                            const auto position = draw.shown_position.value_or(
                                unit != nullptr ? unit->position : oa::FixedVec3{}
                            );
                            std::cout << "stock shadow foreign draw: stock=" << stock_name
                                      << " rules=" << version << " type=" << foreign_type
                                      << " name="
                                      << (foreign_type < spawn_type_names_.size()
                                              ? std::string_view{spawn_type_names_[foreign_type]}
                                              : std::string_view{"<unknown>"})
                                      << " on_map=" << draw.on_map << " stand_in=" << draw.stand_in
                                      << " id=" << (unit != nullptr ? uint32_t(unit->id) : 0)
                                      << " position=" << position.x << ',' << position.y << ','
                                      << position.z << " omitted=" << draw.preplan_omitted
                                      << " linked=" << draw.plan.linked.drawn
                                      << " from_image=" << draw.plan.model.from_image
                                      << " no_shadow=" << no_shadow << " flags=" << flags
                                      << " proven_non_caster=" << non_caster << '\n';
                            reported_foreign_non_caster = non_caster;
                        }
                        if (non_caster)
                            continue;
                        require(
                            unit != nullptr && draw.stand_in < 0 &&
                                std::any_of(
                                    original_commanders.begin(),
                                    original_commanders.end(),
                                    [&](const auto& commander) {
                                        return unit->id == commander.first &&
                                               unit->type_index == commander.second;
                                    }
                                ),
                            "an unknown possible shadow caster entered the stock cache view"
                        );
                        require(
                            ++foreign_draws <= stock_foreign_draw_limit,
                            "too many possible foreign shadow draws entered the stock cache view"
                        );
                    }
                    // A building emits two card casts and another model emits one;
                    // count three for each possible foreign draw conservatively.
                    // Compact runs require at least sixteen casts of one type/mask
                    // pattern. Fewer foreign casts cannot form an admitted run, so
                    // the actual backend upload and hit below must cover stock actors.
                    const auto foreign_casts = foreign_draws * stock_casts_per_foreign_draw;
                    require(
                        foreign_casts < renderer_live_gpu::shadow_template_minimum_casts,
                        "foreign casts could supply the stock shadow-cache witness"
                    );
                    most_foreign_casts = std::max(most_foreign_casts, foreign_casts);
                    for (const auto slot : actors) {
                        const auto id = match_->world().record.units[slot].id;
                        const auto draw =
                            std::find_if(draws.begin(), draws.end(), [&](const auto& value) {
                                return value.model.unit && value.model.unit->id == id;
                            });
                        require(
                            draw != draws.end() && draw->plan.linked.drawn &&
                                draw->plan.model.from_image && draw->stand_in < 0 &&
                                !draw->preplan_omitted && live_unit_bounds(slot).has_value(),
                            "a stock caster did not reach the ordinary physical model path"
                        );
                    }
                    const auto picks_now = bounds_and_picks();
                    if (!saw_cold) {
                        require(
                            counts.cached_runs == 0 && counts.cached_indices == 0 &&
                                counts.cache_hits == 0 && counts.template_uploaded_indices == 0,
                            "the new stock renderer did not start with dynamic shadow indices"
                        );
                        cold = counts;
                        frozen_picks = picks_now;
                        cold_picture = match_world_cpu_;
                        saw_cold = true;
                    } else {
                        require(
                            counts.logical_indices == cold.logical_indices &&
                                picks_now == frozen_picks &&
                                match_world_cpu_.width == cold_picture.width &&
                                match_world_cpu_.height == cold_picture.height &&
                                match_world_cpu_.rgb == cold_picture.rgb,
                            "stock cache warming changed logical shadows, pixels, bounds or picks"
                        );
                    }
                    if (counts.cached_runs != 0 && counts.cached_indices != 0 &&
                        counts.template_uploaded_indices != 0 && counts.admitted_patterns != 0) {
                        uploaded = counts;
                        saw_upload = true;
                    }
                    if (saw_upload && counts.cached_runs != 0 && counts.cached_indices != 0 &&
                        counts.cache_hits != 0 && counts.admitted_patterns == 0 &&
                        counts.template_uploaded_indices == 0 &&
                        counts.dynamic_uploaded_indices < counts.logical_indices) {
                        warm = counts;
                        saw_hit = true;
                    }
                }
                require(
                    saw_cold && saw_upload && saw_hit,
                    "real homogeneous stock casters did not upload and reuse a GPU shadow template"
                );
                std::cout << "stock shadow cache: type=" << stock_name << " rules=" << version
                          << " actors=" << actors.size() << " moving_animated=" << animated_count
                          << " cold_logical=" << cold.logical_indices
                          << " upload_words=" << uploaded.template_uploaded_indices
                          << " admitted_patterns=" << uploaded.admitted_patterns
                          << " cached_runs=" << warm.cached_runs
                          << " cached_indices=" << warm.cached_indices
                          << " hits=" << warm.cache_hits
                          << " foreign_cast_bound=" << most_foreign_casts
                          << " dynamic_upload=" << warm.dynamic_uploaded_indices
                          << " logical=" << warm.logical_indices
                          << "; actual offscreen GPU; world, clocks, poses, pixels, bounds, picks "
                             "and RNG "
                             "preserved\n";
            }
        }
    } catch (...) {
        apply_engine_settings(stock_settings_kept);
        frontend_game().max_units_setting = stock_limit_kept;
        skirmish_settings_.map_name = stock_map_kept;
        throw;
    }
    apply_engine_settings(stock_settings_kept);
    frontend_game().max_units_setting = stock_limit_kept;
    skirmish_settings_.map_name = stock_map_kept;
}

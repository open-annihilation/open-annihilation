// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Live match camera and GPU battlefield composition.
#include "oa/app/runtime.hpp"
#include "oa/app/frame_coordinates.hpp"
#include "director_state.hpp"
#include "match_models.hpp"
#include "engine_settings_state.hpp"
#include "renderer_live_camera.hpp"
#include "renderer_live_gpu.hpp"
#include "renderer_live_scene.hpp"
#include "renderer_water_surface.hpp"
#include "oa/sim/unit_movement/terrain.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <iostream>
#include <limits>
#include <string_view>
#include <unordered_map>

namespace oa::app {
namespace {
/// Camera rotation per logical mouse pixel.
constexpr float orbit_degrees_per_pixel = 0.25F;
/// Painted height cell edge and the classic projection's height lift.
constexpr int terrain_cell_edge = 16;
constexpr float height_lift = 0.5F;
} // namespace

struct Runtime::LiveRenderer {
    renderer_live_camera::State camera{};
    renderer_live_camera::Matrix projection{};
    renderer_live_camera::View view{};
    bool pivot_held{};
    renderer_live_scene::Scene scene{};
    renderer_live_gpu::Context gpu{};
    std::vector<uint8_t> rgba{};
    std::vector<std::array<float, 3>> projected{};
    std::vector<uint16_t> triangle_units{};
    std::unordered_map<uint16_t, oa::Rect32> unit_bounds{};
    bool dragging{};
    uint8_t drag_button{};
    bool orbiting{};
    bool failed{};
    bool unavailable{};
    const void* match{};

    /// The camera of the last frame drawn, turned or classic: a frame that
    /// needs more room than the renderer has goes back to it, rather than to
    /// the classic view, so that a turn or a zoom stops where the frames
    /// could still be drawn.
    struct Drawn {
        bool held{};
        renderer_live_camera::State camera{};
        renderer_live_camera::View view{};
        std::array<double, 2> place{};
        float zoom{};
        Runtime::ViewHold view_hold{};
    };

    Drawn drawn{};
    /// The view has gone back for lack of room since a frame last drew
    /// where it was asked to: reported once until then.
    bool out_of_room{};
    /// The frame being drawn again is drawn from the camera last drawn.
    bool went_back{};
};

void Runtime::destroy_live_renderer(LiveRenderer* renderer) noexcept {
    delete renderer;
}

bool Runtime::live_renderer_active() const noexcept {
    // A director's turned frame on the graphics card asks for the turn
    // itself, whatever Allow Full 3D Mode says.
    return (!engine_settings_ || engine_settings_->current.allow_full_3d ||
            director_draws_on_card()) &&
           (full_presentation() || options_.headless_check) && live_renderer_ &&
           (live_renderer_->camera.yaw_degrees != 0 || live_renderer_->camera.pitch_degrees != 0);
}

bool Runtime::turn_live_camera(float yaw_degrees, float pitch_degrees) {
    if (!live_renderer_)
        live_renderer_.reset(new LiveRenderer{});
    auto& live = *live_renderer_;
    live.camera.yaw_degrees = yaw_degrees;
    live.camera.pitch_degrees = pitch_degrees;
    // Held within the unlocked range by the camera's own limits, so that
    // the projection takes the angles as they are.
    renderer_live_camera::set_unlocked(live.camera, true);
    live.dragging = false;
    live.drag_button = 0;
    live.pivot_held = false;
    live.failed = false;
    live.unavailable = false;
    return live.camera.yaw_degrees != 0 || live.camera.pitch_degrees != 0;
}

bool Runtime::live_renderer_unlocked() const noexcept {
    return live_renderer_ && live_renderer_->camera.unlocked;
}

bool Runtime::live_renderer_failed() const noexcept {
    return live_renderer_ && live_renderer_->failed;
}

bool Runtime::live_renderer_unavailable() const noexcept {
    return live_renderer_ && live_renderer_->unavailable;
}

std::size_t Runtime::live_renderer_vertex_count() const noexcept {
    return live_renderer_ ? live_renderer_->scene.vertices.size() : 0;
}

std::size_t Runtime::live_renderer_emissive_vertex_count() const noexcept {
    if (!live_renderer_)
        return 0;
    return static_cast<std::size_t>(std::count_if(
        live_renderer_->scene.vertices.begin(),
        live_renderer_->scene.vertices.end(),
        [](const auto& vertex) {
            return vertex.emission_r > 0 || vertex.emission_g > 0 || vertex.emission_b > 0;
        }
    ));
}

std::size_t Runtime::live_renderer_nano_particle_count() const noexcept {
    return live_renderer_ ? live_renderer_->scene.nano_particle_count : 0;
}

void Runtime::apply_live_camera_settings(bool allowed, bool unlocked) {
    if (!live_renderer_)
        return;
    if (!allowed) {
        reset_live_renderer();
        return;
    }
    renderer_live_camera::set_unlocked(live_renderer_->camera, unlocked);
}

void Runtime::reset_live_renderer() {
    if (live_renderer_) {
        renderer_live_camera::reset(live_renderer_->camera);
        live_renderer_->dragging = false;
        live_renderer_->pivot_held = false;
        live_renderer_->drag_button = 0;
        live_renderer_->failed = false;
        live_renderer_->unavailable = false;
    }
}

bool Runtime::answer_live_view_key(std::string_view action) {
    if (action != "camera.classic-view" && action != "camera.unlocked")
        return false;
    // Where no rotated view can be drawn, Alt and its keys stay the game's.
    if ((live_renderer_ && live_renderer_->gpu.device_unavailable) ||
        !engine_settings().allow_full_3d || screen_ != Screen::match || !match_ || !selected_tnt_)
        return false;
    if (!full_presentation() && !options_.headless_check)
        return false;
    // A marker's text takes the letters, and a panel or the question owns the
    // keys.
    if (match_question_open() || chat_composing_ || whiteboard_input_.editing || match_paused_)
        return false;
    if (!live_renderer_)
        live_renderer_.reset(new LiveRenderer{});
    if (action == "camera.classic-view") {
        reset_live_renderer();
        return true;
    }
    auto& live = *live_renderer_;
    live.failed = false;
    live.unavailable = false;
    const auto before = engine_settings();
    auto chosen = before;
    chosen.unlocked_camera = !chosen.unlocked_camera;
    apply_engine_settings(chosen);
    if (const auto error = save_engine_settings(before, chosen, false))
        status_ = *error;
    return true;
}

bool Runtime::live_renderer_take_event(SDL_Event& event) {
    // Where no rotated view can be drawn, Alt and its clicks and keys stay
    // the game's: below Full hardware acceleration, which never draws it,
    // and once SDL has made no device for it, for the rest of the run.
    if ((live_renderer_ && live_renderer_->gpu.device_unavailable) ||
        !engine_settings().allow_full_3d || screen_ != Screen::match || !match_ || !selected_tnt_)
        return false;
    if (!full_presentation() && !options_.headless_check) {
        // A camera drag begun on Full ends where Full does.
        if (live_renderer_) {
            live_renderer_->dragging = false;
            live_renderer_->drag_button = 0;
        }
        return false;
    }
    if (!live_renderer_)
        live_renderer_.reset(new LiveRenderer{});
    auto& live = *live_renderer_;
    if (event.type == SDL_EVENT_WINDOW_FOCUS_LOST) {
        live.dragging = false;
        live.drag_button = 0;
        return false;
    }
    if (match_question_open() || chat_composing_ || whiteboard_input_.editing || match_paused_) {
        // A panel or text entry ends the gesture, including when its button
        // is released while that panel owns the pointer. A marker's text
        // takes Alt+R and Alt+U as the letters they type.
        live.dragging = false;
        live.drag_button = 0;
        return false;
    }
    if (event.type == SDL_EVENT_KEY_DOWN) {
        const auto actions = match_action_events();
        for (const oa::ui::key_bindings::ActionEvent& action : actions) {
            if (action.phase != oa::ui::key_bindings::Phase::pressed)
                continue;
            if (action.action != "camera.classic-view" && action.action != "camera.unlocked")
                continue;
            return handle_match_action(action);
        }
    }
    SDL_Event converted = event;
    const bool pointer =
        event.type == SDL_EVENT_MOUSE_BUTTON_DOWN || event.type == SDL_EVENT_MOUSE_BUTTON_UP ||
        event.type == SDL_EVENT_MOUSE_MOTION || event.type == SDL_EVENT_MOUSE_WHEEL;
    if (pointer && sdl_.renderer && !convert_event_to_frame(sdl_.renderer, converted))
        return false;
    const float x = !pointer                                   ? 0.0F
                    : converted.type == SDL_EVENT_MOUSE_MOTION ? converted.motion.x
                    : converted.type == SDL_EVENT_MOUSE_WHEEL  ? converted.wheel.mouse_x
                                                               : converted.button.x;
    const float y = !pointer                                   ? 0.0F
                    : converted.type == SDL_EVENT_MOUSE_MOTION ? converted.motion.y
                    : converted.type == SDL_EVENT_MOUSE_WHEEL  ? converted.wheel.mouse_y
                                                               : converted.button.y;
    const bool battlefield = pointer && x >= match_layout_.left && y >= match_layout_.top &&
                             x < match_layout_.left + match_layout_.battlefield_width() &&
                             y < match_layout_.top + match_layout_.battlefield_height();
    if (event.type == SDL_EVENT_MOUSE_BUTTON_UP && event.button.button == live.drag_button &&
        live.drag_button != 0) {
        live.dragging = false;
        live.drag_button = 0;
        update_pointer(x, y);
        return true;
    }
    // While an order or a building is armed, Alt on the select button is the
    // click snap's override: that click places it unsnapped. The pan button
    // still orbits or pans.
    const bool order_armed = match_command_ != MatchCommand::none || pending_build_type_ != 0;
    ensure_key_bindings(false);
    if (event.type == SDL_EVENT_MOUSE_BUTTON_DOWN && battlefield) {
        const uint8_t button = event.button.button;
        const PointerCameraDrag drag = pointer_camera_drag(button, pointer_modifier_bits());
        const bool blocked = order_armed && button == match_select_button();
        if (drag != PointerCameraDrag::none && !blocked) {
            // An explicit camera gesture retries after a failed frame.
            live.failed = false;
            live.unavailable = false;
            renderer_live_camera::set_unlocked(live.camera, engine_settings().unlocked_camera);
            live.dragging = true;
            live.drag_button = button;
            live.orbiting = drag == PointerCameraDrag::orbit;
            match_drag_.reset();
            update_pointer(x, y);
            return true;
        }
    }
    if (event.type == SDL_EVENT_MOUSE_MOTION && live.dragging) {
        // Camera gestures consume the event before normal pointer handling.
        // Keep the custom cursor at the physical pointer throughout the drag.
        update_pointer(x, y);
        if (live.orbiting) {
            renderer_live_camera::orbit(
                live.camera,
                converted.motion.xrel * orbit_degrees_per_pixel,
                converted.motion.yrel * orbit_degrees_per_pixel
            );
        } else {
            float dx{}, dz{};
            if (renderer_live_camera::pan_delta(
                    live.projection, converted.motion.xrel, converted.motion.yrel, dx, dz
                )) {
                const auto place = match_view_place();
                place_match_view(place[0] - dx, place[1] - dz);
            }
        }
        return true;
    }
    if (!live_renderer_active())
        return false;
    if (event.type != SDL_EVENT_MOUSE_WHEEL || !battlefield)
        return false;
    // Build rotation and spacing keep their modified-wheel gestures.
    if (handle_build_tool_key(event))
        return true;
    if (!engine_settings().wheel_zoom || live.failed)
        return true;
    const float turned_raw =
        event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED ? -event.wheel.y : event.wheel.y;
    const float turned = pointer_zoom_amount(turned_raw);
    if (!std::isfinite(turned) || turned == 0)
        return true;
    const auto anchor = live_ground_hit(x, y);
    constexpr float fixed_scale = 65536;
    const float anchor_height =
        anchor ? static_cast<float>((*anchor)[1]) / fixed_scale : live.view.focus_y;
    auto view = live.view;
    const float zoom = wheel_zoom_target(turned);
    if (!renderer_live_camera::zoom_about(
            live.camera, view, zoom, x - match_layout_.left, y - match_layout_.top, anchor_height
        ))
        return true;
    stop_match_tracking();
    match_zoom_ = match_zoom_target_ = zoom;
    zoom_focus_ = {false, x, y};
    const float half_width = match_layout_.battlefield_width() / (2 * zoom);
    const float half_height = match_layout_.battlefield_height() / (2 * zoom);
    place_match_view(
        view.focus_x - half_width, view.focus_z - view.focus_y * height_lift - half_height
    );
    view_hold_ = {true, view.focus_x, view.focus_z - view.focus_y * height_lift};
    live.view = view;
    if (renderer_live_camera::projection(live.camera, live.view, live.projection))
        rebuild_live_picking();
    return true;
}

bool Runtime::render_live_match_surface() {
    if ((!engine_settings().allow_full_3d && !director_draws_on_card()) || !match_ ||
        !selected_tnt_)
        return true;
    if (!live_renderer_)
        live_renderer_.reset(new LiveRenderer{});
    auto& live = *live_renderer_;
    if (live.match != match_.get()) {
        live.scene = {};
        live.drawn = {};
        live.pivot_held = false;
        live.match = match_.get();
        live.failed = false;
        live.unavailable = false;
    }
    const auto& map = *selected_tnt_;
    const auto place = match_view_place();
    const float zoom = match_zoom();
    const float width = static_cast<float>(match_layout_.battlefield_width());
    const float height = static_cast<float>(match_layout_.battlefield_height());
    const float focus_x = static_cast<float>(place[0]) + width / (2 * zoom);
    const float focus_y = static_cast<float>(place[1]) + height / (2 * zoom);
    float elevation{};
    if (map.attribute_width && map.attribute_height && !map.attributes.empty()) {
        const auto column = std::clamp(
            static_cast<int>(focus_x) / terrain_cell_edge,
            0,
            static_cast<int>(map.attribute_width) - 1
        );
        const auto row = std::clamp(
            static_cast<int>(focus_y) / terrain_cell_edge,
            0,
            static_cast<int>(map.attribute_height) - 1
        );
        elevation =
            map.attributes[static_cast<std::size_t>(row) * map.attribute_width + column].height;
    }
    // Keep the orbit pivot's height while panning and zooming across terrain cells.
    // A sampled height change would otherwise shift the whole rotated view.
    if (live_renderer_active() && live.pivot_held)
        elevation = live.view.focus_y;
    live.pivot_held = live_renderer_active();
    live.view = {
        focus_x, elevation, focus_y + elevation * height_lift, width * 0.5F, height * 0.5F, zoom
    };
    if (!renderer_live_camera::projection(live.camera, live.view, live.projection))
        return true;
    // A classic frame is the last camera drawn until a turned one draws.
    if (!live_renderer_active())
        live.drawn = {true, live.camera, live.view, place, zoom, view_hold_};
    if (live_renderer_active() && !live.failed) {
        auto& models = match_models();
        renderer_live_scene::Inputs inputs{};
        inputs.map = &map;
        inputs.palette = &match_palette_;
        inputs.draws = &models.draws;
        inputs.graphics_flags = models.renderer.graphics_flags;
        inputs.moving_pieces_once_built = models.renderer.moving_pieces_once_built;
        inputs.shade_table = models.display.shade;
        inputs.light = {
            models.renderer.light[0], models.renderer.light[1], models.renderer.light[2]
        };
        inputs.light_scale = models.renderer.light_scale;
        inputs.sea_level = map.sea_level;
        inputs.detail_scale = zoom;
        // The sliders' strengths, which a lava sea leaves at none.
        const auto levels = water_levels();
        inputs.water = {
            levels.ripples,
            levels.coast,
            levels.shimmer,
            levels.reflections,
            float(map.sea_level),
            float(match_->simulation().tick % 108000U) / 30.0F,
            std::clamp((zoom - 0.25F) / 0.25F, 0.0F, 1.0F)
        };
        inputs.water.wind = water_wind();
        inputs.water.fine_detail = renderer_water_surface::fine_detail(zoom);
        inputs.water_surface = water_surface();
        inputs.water_disturbances = water_disturbances();
        inputs.nanolathe_lighting = engine_settings().nanolathe_lighting > 0;
        inputs.nanolathe_intensity = engine_settings().nanolathe_lighting / 100.0F;
        inputs.tick = match_->simulation().tick;
        inputs.translucent_units = match_->state().game.debug_overlay != 0;
        inputs.new_unit_shadows = engine_settings().new_unit_shadows;
        inputs.view_direction = {live.projection[8], live.projection[9], live.projection[10]};
        inputs.clip_projection = live.projection;
        inputs.clip_width = match_world_cpu_.width;
        inputs.clip_height = match_world_cpu_.height;
        inputs.camera_x = models.renderer.camera_x;
        inputs.camera_y = models.renderer.camera_y;
        std::copy_n(
            models.renderer.team_colors, full::team_colour_players, inputs.team_colors.begin()
        );
        const auto& sight = match_->sight();
        inputs.sight.coverage = match_->player_coverage(match_view_player());
        inputs.sight.player_bits = sight.player_bits;
        inputs.sight.width = sight.width;
        inputs.sight.height = sight.height;
        inputs.sight.shared_bits = allied_mapped_bits();
        inputs.sight.mapped_bits = static_cast<uint16_t>(
            (1U << (sight.viewpoint_player & 0x1fU)) | inputs.sight.shared_bits
        );
        inputs.sight.mapping = match_mapping_on();
        inputs.sight.line_of_sight = match_line_of_sight_on();
        inputs.sight.dithered =
            (match_->state().game.graphics_flags & init::preference_flags::dithered_fog) != 0;
        const int32_t map_width = static_cast<int32_t>(map.attribute_width) * terrain_cell_edge;
        const int32_t map_height = static_cast<int32_t>(map.attribute_height) * terrain_cell_edge;
        inputs.left = 0;
        inputs.top = 0;
        inputs.right = map_width;
        inputs.bottom = map_height;
        // Include the whole visible footprint at both terrain height limits.
        // Map edges bound the work even when an unlocked camera is nearly horizontal.
        constexpr float terrain_margin = 2 * terrain_cell_edge;
        renderer_live_camera::TerrainBounds visible{};
        if (renderer_live_camera::visible_terrain_bounds(
                live.projection, width, height, terrain_margin, visible
            )) {
            inputs.left =
                static_cast<int32_t>(std::clamp(std::floor(visible.min_x), 0.0F, float(map_width)));
            inputs.top = static_cast<int32_t>(
                std::clamp(std::floor(visible.min_y), 0.0F, float(map_height))
            );
            inputs.right =
                static_cast<int32_t>(std::clamp(std::ceil(visible.max_x), 0.0F, float(map_width)));
            inputs.bottom =
                static_cast<int32_t>(std::clamp(std::ceil(visible.max_y), 0.0F, float(map_height)));
        }
        std::string error;
        const bool built = renderer_live_scene::build(inputs, live.scene, error);
        // Picking reads only the frame's triangles, so a frame that draws
        // rebuilds it while the card draws them.
        const auto rebuild_picking = [this] { rebuild_live_picking(); };
        const renderer_live_gpu::KeptGround ground{
            live.scene.terrain_vertex_count, live.scene.ground_revision
        };
        const auto result = built ? renderer_live_gpu::render(
                                        live.gpu,
                                        live.scene.vertices,
                                        live.scene.texture_rgba,
                                        live.scene.texture_width,
                                        live.scene.texture_height,
                                        live.scene.texture_revision,
                                        match_world_cpu_.width,
                                        match_world_cpu_.height,
                                        live.projection,
                                        live.rgba,
                                        error,
                                        static_cast<uint32_t>(engine_settings().anti_aliasing),
                                        &live.scene.atlas_update,
                                        &inputs.water,
                                        &ground,
                                        rebuild_picking
                                    )
                                  : renderer_live_gpu::Result::failed;
        const auto& last = live.drawn;
        const bool at_last =
            live.went_back || (last.held && last.camera.yaw_degrees == live.camera.yaw_degrees &&
                               last.camera.pitch_degrees == live.camera.pitch_degrees &&
                               last.place == place && last.zoom == zoom);
        live.went_back = false;
        if (result == renderer_live_gpu::Result::success) {
            live.drawn = {true, live.camera, live.view, place, zoom, view_hold_};
            live.out_of_room = false;
            for (std::size_t pixel = 0; pixel < match_world_cpu_.rgb.size() / 3; ++pixel)
                std::copy_n(
                    live.rgba.data() + pixel * 4, 3, match_world_cpu_.rgb.data() + pixel * 3
                );
            compose_match_effects(match_world_cpu_, true);
        } else if (
            !built && live.scene.out_of_room && last.held && director_ == nullptr &&
            (!at_last || live.rgba.size() == match_world_cpu_.rgb.size() / 3 * 4)
        ) {
            // A frame that needs more room than the renderer has is no
            // failure of the card: the view goes back to the last camera
            // drawn, its angle, height, place and zoom, and the frame is
            // drawn again from there, so that a turn or a zoom stops at
            // that limit instead of the view going back to classic.
            if (!live.out_of_room)
                std::cerr << "3D view: " << error << "; the view stays where it was last drawn\n";
            live.out_of_room = true;
            if (at_last) {
                // The camera last drawn no longer fits either, as when more
                // comes into view: its last picture stays, with nothing to
                // pick in it.
                rebuild_live_picking();
                for (std::size_t pixel = 0; pixel < match_world_cpu_.rgb.size() / 3; ++pixel)
                    std::copy_n(
                        live.rgba.data() + pixel * 4, 3, match_world_cpu_.rgb.data() + pixel * 3
                    );
                compose_match_effects(match_world_cpu_, true);
            } else {
                const auto back = last;
                live.camera = back.camera;
                live.view = back.view;
                live.pivot_held = true;
                match_zoom_ = match_zoom_target_ = back.zoom;
                place_match_view(back.place[0], back.place[1]);
                view_hold_ = back.view_hold;
                live.went_back = true;
                // render_match_surface draws the frame again, from there.
                return false;
            }
        } else {
            live.failed = true;
            live.unavailable = result == renderer_live_gpu::Result::unavailable;
            // The fallback must also restore classic input, including wheel zoom.
            // A later camera gesture can retry from the new position and scale.
            renderer_live_camera::reset(live.camera);
            live.dragging = false;
            live.drag_button = 0;
            live.pivot_held = false;
            std::cerr << "3D view: " << error << '\n';
            // This frame left out the classic raster: render_match_surface
            // draws the frame again, once, classically.
            return false;
        }
    }
    if (!live_renderer_active())
        return true;
    // A director's frame without the interface is the world alone.
    const bool bare = director_ != nullptr && !director_->presentation.show_interface;
    if (!live.failed) {
        if (!bare) {
            draw_live_overlays();
            draw_live_selection();
            draw_live_unit_overlays();
        }
        rebuild_on_screen_units();
    }
    return true;
}

bool Runtime::scroll_live_match_view(double screen_dx, double screen_dy) {
    if (!live_renderer_active() || !live_renderer_ || live_renderer_->failed)
        return false;
    auto& live = *live_renderer_;
    if (live.dragging)
        return true;
    float dx{}, dz{};
    if (!renderer_live_camera::pan_delta(
            live.projection, static_cast<float>(screen_dx), static_cast<float>(screen_dy), dx, dz
        ))
        return false;
    stop_match_tracking();
    const auto place = match_view_place();
    place_match_view(place[0] + dx, place[1] + dz);
    const auto moved = match_view_place();
    remember_placed_view(moved[0], moved[1]);
    return true;
}

void Runtime::rebuild_live_picking() {
    auto& live = *live_renderer_;
    live.projected.clear();
    live.projected.reserve(live.scene.vertices.size());
    const auto& m = live.projection;
    for (const auto& vertex : live.scene.vertices)
        live.projected.push_back(
            {m[0] * vertex.x + m[1] * vertex.height + m[2] * vertex.z + m[3],
             m[4] * vertex.x + m[5] * vertex.height + m[6] * vertex.z + m[7],
             m[8] * vertex.x + m[9] * vertex.height + m[10] * vertex.z + m[11]}
        );
    live.triangle_units.assign(live.scene.vertices.size() / 3, 0);
    live.unit_bounds.clear();
    for (const auto& range : live.scene.unit_ranges) {
        if (range.unit_id == 0 || range.vertex_count == 0 ||
            range.first_vertex + range.vertex_count > live.projected.size() ||
            !match_->unit_shown(match_view_player(), range.unit_id))
            continue;
        auto [entry, inserted] = live.unit_bounds.try_emplace(
            range.unit_id,
            oa::Rect32{
                std::numeric_limits<int32_t>::max(),
                std::numeric_limits<int32_t>::max(),
                std::numeric_limits<int32_t>::min(),
                std::numeric_limits<int32_t>::min()
            }
        );
        auto& bounds = entry->second;
        for (std::size_t index = range.first_vertex;
             index < range.first_vertex + range.vertex_count;
             ++index) {
            const auto& point = live.projected[index];
            bounds.x1 = std::min(
                bounds.x1, static_cast<int32_t>(std::floor(point[0])) + match_layout_.left
            );
            bounds.y1 =
                std::min(bounds.y1, static_cast<int32_t>(std::floor(point[1])) + match_layout_.top);
            bounds.x2 =
                std::max(bounds.x2, static_cast<int32_t>(std::ceil(point[0])) + match_layout_.left);
            bounds.y2 =
                std::max(bounds.y2, static_cast<int32_t>(std::ceil(point[1])) + match_layout_.top);
            live.triangle_units[index / 3] = range.unit_id;
        }
    }
}

std::optional<oa::Rect32> Runtime::live_unit_bounds(uint16_t id) const {
    if (!live_renderer_ || live_renderer_->failed || !match_ || id == 0 ||
        id >= match_->world().slots.size())
        return std::nullopt;
    const auto found = live_renderer_->unit_bounds.find(id);
    if (found == live_renderer_->unit_bounds.end())
        return std::nullopt;
    // A previously drawn unit can leave its slot before the next frame. Empty
    // slots have no model bounds and must never enter the visibility query.
    const auto& slot = match_->world().slots[id];
    if (!slot.unit || slot.record.type_index == 0 || !match_->unit_shown(match_view_player(), id))
        return std::nullopt;
    return found->second;
}

oa::present::world_renderer::ScreenPoint
Runtime::live_project_world(float x, float height, float z) const {
    if (!live_renderer_)
        return {};
    const auto& m = live_renderer_->projection;
    return {
        match_layout_.left +
            static_cast<int32_t>(std::lround(m[0] * x + m[1] * height + m[2] * z + m[3])),
        match_layout_.top +
            static_cast<int32_t>(std::lround(m[4] * x + m[5] * height + m[6] * z + m[7]))
    };
}

std::optional<oa::sim::ground_orders::Point> Runtime::live_ground_point(float x, float y) const {
    const auto hit = live_ground_hit(x, y);
    if (!hit)
        return std::nullopt;
    // The map pixel the hit lies in, at the height the simulation gives that
    // pixel and never below the sea, as terrain_intersection takes it for a
    // classic pick; the mesh's height between cells and the fraction of the
    // pixel are the drawing's alone.
    const auto whole = [](int32_t fixed) {
        return std::bit_cast<int32_t>(static_cast<uint32_t>(fixed) & 0xffff0000U);
    };
    const int32_t map_x = whole((*hit)[0]);
    const int32_t map_z = whole((*hit)[2]);
    const oa::sim::unit_movement::Terrain terrain(*selected_tnt_);
    const int32_t height = std::max<int32_t>(terrain.sea_level(), terrain.height(map_x, map_z));
    return oa::sim::ground_orders::Point{
        map_x, std::bit_cast<int32_t>(static_cast<uint32_t>(height) << 16U), map_z
    };
}

std::optional<oa::sim::ground_orders::Point> Runtime::live_ground_hit(float x, float y) const {
    if (!live_renderer_ || live_renderer_->failed || !battlefield_contains(x, y))
        return std::nullopt;
    const auto& live = *live_renderer_;
    float best_depth = -std::numeric_limits<float>::infinity();
    std::optional<oa::sim::ground_orders::Point> result;
    constexpr float fixed_scale = 65536;
    for (std::size_t index = 0;
         index + 2 < live.scene.terrain_vertex_count && index + 2 < live.projected.size();
         index += 3) {
        std::array<float, 3> weights{};
        float depth{};
        if (!renderer_live_camera::triangle_hit(
                {live.projected[index], live.projected[index + 1], live.projected[index + 2]},
                x - match_layout_.left,
                y - match_layout_.top,
                weights,
                depth
            ) ||
            depth <= best_depth)
            continue;
        float world_x{}, height{}, world_z{};
        for (std::size_t corner = 0; corner < weights.size(); ++corner) {
            const auto& vertex = live.scene.vertices[index + corner];
            world_x += vertex.x * weights[corner];
            height += vertex.height * weights[corner];
            world_z += vertex.z * weights[corner];
        }
        // The drawn coastal relief may extend beyond the last simulation cell.
        if (world_x < 0 || world_z < 0 ||
            world_x >= selected_tnt_->attribute_width * terrain_cell_edge ||
            world_z >= selected_tnt_->attribute_height * terrain_cell_edge)
            continue;
        best_depth = depth;
        result = oa::sim::ground_orders::Point{
            static_cast<int32_t>(world_x * fixed_scale),
            static_cast<int32_t>(height * fixed_scale),
            static_cast<int32_t>(world_z * fixed_scale)
        };
    }
    return result;
}

uint16_t Runtime::live_unit_under(float x, float y) const {
    if (!live_renderer_ || live_renderer_->failed || !battlefield_contains(x, y))
        return 0;
    const auto& live = *live_renderer_;
    uint16_t picked{};
    float best_depth = -std::numeric_limits<float>::infinity();
    for (std::size_t index = 0; index + 2 < live.projected.size(); index += 3) {
        std::array<float, 3> weights{};
        float depth{};
        if (!renderer_live_camera::triangle_hit(
                {live.projected[index], live.projected[index + 1], live.projected[index + 2]},
                x - match_layout_.left,
                y - match_layout_.top,
                weights,
                depth
            ) ||
            depth <= best_depth)
            continue;
        float u{}, v{}, opacity{};
        for (std::size_t corner = 0; corner < weights.size(); ++corner) {
            const auto& vertex = live.scene.vertices[index + corner];
            u += vertex.u * weights[corner];
            v += vertex.v * weights[corner];
            opacity += vertex.opacity * weights[corner];
        }
        const auto column = std::clamp(
            static_cast<int32_t>(u * live.scene.texture_width),
            0,
            static_cast<int32_t>(live.scene.texture_width) - 1
        );
        const auto row = std::clamp(
            static_cast<int32_t>(v * live.scene.texture_height),
            0,
            static_cast<int32_t>(live.scene.texture_height) - 1
        );
        const auto alpha_index =
            (static_cast<std::size_t>(row) * live.scene.texture_width + column) * 4 + 3;
        // Smoke and translucent ground overlays do not block gameplay picks;
        // a unit is picked wherever its texels cover, a cloaked one too.
        const auto id = live.triangle_units[index / 3];
        if (alpha_index >= live.scene.texture_rgba.size() ||
            live.scene.texture_rgba[alpha_index] * (id != 0 ? 1.0F : opacity) < 128)
            continue;
        if (id != 0 && !live_unit_bounds(id))
            continue;
        best_depth = depth;
        picked = id;
    }
    return picked;
}

std::vector<uint8_t> Runtime::live_terrain_visibility_mask() const {
    std::vector<uint8_t> visibility;
    std::vector<float> depth;
    live_scene_visibility(visibility, depth, true);
    return visibility;
}

float Runtime::live_project_depth(float x, float height, float z) const {
    if (!live_renderer_ || live_renderer_->failed)
        return -std::numeric_limits<float>::infinity();
    const auto& m = live_renderer_->projection;
    return m[8] * x + m[9] * height + m[10] * z + m[11];
}

void Runtime::live_scene_visibility(
    std::vector<uint8_t>& visibility,
    std::vector<float>& depths,
    bool terrain_only,
    std::span<const uint32_t> row_left,
    std::span<const uint32_t> row_right,
    std::span<const uint8_t> requested_pixels
) const {
    // Preserve surface depth and alpha cutouts for lighting and ground overlays.
    visibility.clear();
    depths.clear();
    constexpr uint32_t maximum_dimension = 4096;
    const uint32_t width = match_world_cpu_.width;
    const uint32_t height = match_world_cpu_.height;
    if (!live_renderer_ || live_renderer_->failed || width == 0 || height == 0 ||
        width > maximum_dimension || height > maximum_dimension)
        return;
    if ((!row_left.empty() || !row_right.empty()) &&
        (row_left.size() != height || row_right.size() != height))
        return;
    const bool bounded = !row_left.empty();
    const auto& live = *live_renderer_;
    if (live.scene.texture_width == 0 || live.scene.texture_height == 0)
        return;
    const std::size_t count = static_cast<std::size_t>(width) * height;
    if (!requested_pixels.empty() && requested_pixels.size() != count)
        return;
    visibility.resize(count);
    depths.assign(count, -std::numeric_limits<float>::infinity());
    // Most scene triangles lie outside the small islands reached by lights.
    // Reject against short row bands before preparing barycentric coefficients.
    constexpr uint32_t rows_per_band = 16;
    const uint32_t band_count = (height + rows_per_band - 1) / rows_per_band;
    std::vector<uint32_t> band_left, band_right;
    uint32_t requested_left = width, requested_right = 0;
    uint32_t requested_top = height, requested_bottom = 0;
    if (bounded) {
        band_left.assign(band_count, width);
        band_right.assign(band_count, 0);
        for (uint32_t y = 0; y < height; ++y) {
            const uint32_t left = std::min(row_left[y], width);
            const uint32_t right = std::min(row_right[y], width);
            if (left >= right)
                continue;
            const auto band = y / rows_per_band;
            band_left[band] = std::min(band_left[band], left);
            band_right[band] = std::max(band_right[band], right);
            requested_left = std::min(requested_left, left);
            requested_right = std::max(requested_right, right);
            requested_top = std::min(requested_top, y);
            requested_bottom = y + 1;
        }
        if (requested_left >= requested_right)
            return;
    }

    // The triangles that can reach the requested pixels, in their order,
    // each with the pixels its bounds cover.
    struct Reach {
        std::size_t index{}; ///< the triangle's first vertex
        int x0{};
        int x1{};
        int y0{};
        int y1{};
    };

    std::vector<Reach> reaches;
    const auto vertex_count = std::min(live.projected.size(), live.scene.vertices.size());
    for (std::size_t index = 0; index + 2 < vertex_count; index += 3) {
        const std::array triangle{
            live.projected[index], live.projected[index + 1], live.projected[index + 2]
        };
        const float left = std::min({triangle[0][0], triangle[1][0], triangle[2][0]});
        const float right = std::max({triangle[0][0], triangle[1][0], triangle[2][0]});
        const float top = std::min({triangle[0][1], triangle[1][1], triangle[2][1]});
        const float bottom = std::max({triangle[0][1], triangle[1][1], triangle[2][1]});
        if (!std::isfinite(left) || !std::isfinite(right) || !std::isfinite(top) ||
            !std::isfinite(bottom))
            continue;
        if (bounded && (right <= requested_left || left >= requested_right ||
                        bottom <= requested_top || top >= requested_bottom))
            continue;
        const int x0 = static_cast<int>(std::clamp(std::floor(left), 0.0F, float(width)));
        const int x1 = static_cast<int>(std::clamp(std::ceil(right), 0.0F, float(width)));
        const int y0 = static_cast<int>(std::clamp(std::floor(top), 0.0F, float(height)));
        const int y1 = static_cast<int>(std::clamp(std::ceil(bottom), 0.0F, float(height)));
        if (x0 >= x1 || y0 >= y1)
            continue;
        if (bounded) {
            bool intersects = false;
            for (auto band = static_cast<uint32_t>(y0) / rows_per_band;
                 band <= static_cast<uint32_t>(y1 - 1) / rows_per_band;
                 ++band) {
                if (x1 > static_cast<int>(band_left[band]) &&
                    x0 < static_cast<int>(band_right[band])) {
                    intersects = true;
                    break;
                }
            }
            if (!intersects)
                continue;
        }
        reaches.push_back({index, x0, x1, y0, y1});
    }
    // Each band of rows takes the triangles that reach it in their order and
    // writes its own rows alone, so each pixel takes the triangles in the
    // same order whatever the bands and threads. One thread takes the rows
    // as one band, preparing each triangle once.
    const uint32_t raster_band_rows =
        draw_pool_ && draw_pool_->threads() > 1 ? rows_per_band : height;
    const uint32_t raster_bands = platform::job_pool::bands_of_rows(height, raster_band_rows);
    std::vector<uint32_t> band_firsts(raster_bands + 1, 0);
    for (const auto& reach : reaches)
        for (auto band = static_cast<uint32_t>(reach.y0) / raster_band_rows;
             band <= static_cast<uint32_t>(reach.y1 - 1) / raster_band_rows;
             ++band)
            ++band_firsts[band + 1];
    for (uint32_t band = 0; band < raster_bands; ++band)
        band_firsts[band + 1] += band_firsts[band];
    std::vector<uint32_t> band_reaches(band_firsts[raster_bands]);
    {
        std::vector<uint32_t> filled(band_firsts.begin(), band_firsts.end() - 1);
        for (uint32_t reach = 0; reach < reaches.size(); ++reach)
            for (auto band = static_cast<uint32_t>(reaches[reach].y0) / raster_band_rows;
                 band <= static_cast<uint32_t>(reaches[reach].y1 - 1) / raster_band_rows;
                 ++band)
                band_reaches[filled[band]++] = reach;
    }
    platform::job_pool::run_bands(draw_pool_.get(), raster_bands, [&](uint32_t band) {
        const int band_top = static_cast<int>(band * raster_band_rows);
        const int band_end = std::min(static_cast<int>(height), band_top + int(raster_band_rows));
        for (uint32_t slot = band_firsts[band]; slot < band_firsts[band + 1]; ++slot) {
            const auto& [index, x0, x1, y0, y1] = reaches[band_reaches[slot]];
            const std::array triangle{
                live.projected[index], live.projected[index + 1], live.projected[index + 2]
            };
            renderer_live_camera::PreparedTriangle prepared;
            if (!renderer_live_camera::prepare_triangle(triangle, prepared))
                continue;
            constexpr float pixel_centre = 0.5F;
            for (int y = std::max(y0, band_top); y < std::min(y1, band_end); ++y) {
                int left_limit =
                    bounded ? std::max(x0, static_cast<int>(std::min(row_left[y], width))) : x0;
                int right_limit =
                    bounded ? std::min(x1, static_cast<int>(std::min(row_right[y], width))) : x1;
                // Only the columns the triangle can cover on this row are tested.
                renderer_live_camera::narrow_row(
                    prepared, y + pixel_centre, pixel_centre, left_limit, right_limit
                );
                for (int x = left_limit; x < right_limit; ++x) {
                    const std::size_t pixel = static_cast<std::size_t>(y) * width + x;
                    if (!requested_pixels.empty() && requested_pixels[pixel] == 0)
                        continue;
                    std::array<float, 3> weights{};
                    float depth{};
                    if (!renderer_live_camera::triangle_hit_prepared(
                            prepared, x + pixel_centre, y + pixel_centre, weights, depth
                        ) ||
                        depth <= depths[pixel])
                        continue;
                    float u{}, v{}, opacity{}, brightness{};
                    for (std::size_t corner = 0; corner < weights.size(); ++corner) {
                        const auto& vertex = live.scene.vertices[index + corner];
                        u += vertex.u * weights[corner];
                        v += vertex.v * weights[corner];
                        opacity += vertex.opacity * weights[corner];
                        brightness += vertex.brightness * weights[corner];
                    }
                    constexpr float opaque_alpha = 128;
                    constexpr float full_alpha = 255;
                    if (!std::isfinite(u) || !std::isfinite(v) || !std::isfinite(opacity) ||
                        opacity * full_alpha < opaque_alpha)
                        continue;
                    const auto column = static_cast<uint32_t>(std::clamp(
                        u * live.scene.texture_width, 0.0F, float(live.scene.texture_width - 1)
                    ));
                    const auto row = static_cast<uint32_t>(std::clamp(
                        v * live.scene.texture_height, 0.0F, float(live.scene.texture_height - 1)
                    ));
                    const auto alpha =
                        (static_cast<std::size_t>(row) * live.scene.texture_width + column) * 4 + 3;
                    if (alpha >= live.scene.texture_rgba.size() ||
                        live.scene.texture_rgba[alpha] * opacity < opaque_alpha)
                        continue;
                    depths[pixel] = depth;
                    // Unmapped black terrain must not reveal its contours. Known
                    // terrain carries the same smooth fog attenuation as its draw.
                    visibility[pixel] =
                        (!terrain_only || index < live.scene.terrain_vertex_count) &&
                                std::isfinite(brightness)
                            ? static_cast<uint8_t>(std::clamp(brightness, 0.0F, 1.0F) * full_alpha)
                            : 0;
                }
            }
        }
    });
}

void Runtime::draw_live_selection() {
    // Draw directly into the GPU readback so feedback follows the shown geometry.
    const auto line = [&](int x0, int y0, int x1, int y1, const std::array<uint8_t, 3>& color) {
        const int steps = std::max(std::abs(x1 - x0), std::abs(y1 - y0));
        if (steps > 32768)
            return;
        for (int step = 0; step <= steps; ++step) {
            const float share = steps == 0 ? 0 : static_cast<float>(step) / steps;
            const int x =
                static_cast<int>(std::lround(x0 + (x1 - x0) * share)) - match_layout_.left;
            const int y = static_cast<int>(std::lround(y0 + (y1 - y0) * share)) - match_layout_.top;
            if (x < 0 || y < 0 || x >= static_cast<int>(match_world_cpu_.width) ||
                y >= static_cast<int>(match_world_cpu_.height))
                continue;
            const auto at = (static_cast<std::size_t>(y) * match_world_cpu_.width + x) * 3;
            std::copy(
                color.begin(),
                color.end(),
                match_world_cpu_.rgb.begin() + static_cast<std::ptrdiff_t>(at)
            );
        }
    };
    const auto box = [&](const oa::Rect32& bounds, const std::array<uint8_t, 3>& color) {
        line(bounds.x1, bounds.y1, bounds.x2, bounds.y1, color);
        line(bounds.x2, bounds.y1, bounds.x2, bounds.y2, color);
        line(bounds.x2, bounds.y2, bounds.x1, bounds.y2, color);
        line(bounds.x1, bounds.y2, bounds.x1, bounds.y1, color);
    };
    constexpr std::array<uint8_t, 3> selection_color{0, 255, 0};
    constexpr std::array<uint8_t, 3> hover_color{255, 255, 255};
    for (const auto id : selected_local_ids())
        if (const auto bounds = live_unit_bounds(id))
            box(*bounds, selection_color);
    if (const auto bounds = live_unit_bounds(hovered_match_unit_))
        box(*bounds, hover_color);
    const bool queue_drag = build_queue_drag_ && build_queue_drag_->moved;
    if (match_command_ == MatchCommand::build || queue_drag) {
        const auto ground = build_cursor_point(match_pointer_x_, match_pointer_y_);
        const auto site =
            !ground ? std::nullopt
            : queue_drag
                ? build_site_for(build_queue_drag_->type, build_queue_drag_->facing, *ground)
                : pending_build_site(*ground);
        if (site) {
            const float x0 = static_cast<float>(site->cell_x * terrain_cell_edge);
            const float z0 = static_cast<float>(site->cell_z * terrain_cell_edge);
            const float x1 = x0 + site->footprint_x * terrain_cell_edge;
            const float z1 = z0 + site->footprint_z * terrain_cell_edge;
            constexpr float fixed_scale = 65536;
            const float height = site->world[1] / fixed_scale;
            const std::array corners{
                live_project_world(x0, height, z0),
                live_project_world(x1, height, z0),
                live_project_world(x1, height, z1),
                live_project_world(x0, height, z1)
            };
            const auto color = site->legal ? selection_color : std::array<uint8_t, 3>{255, 0, 0};
            for (std::size_t edge = 0; edge < corners.size(); ++edge) {
                const auto next = (edge + 1) % corners.size();
                line(corners[edge].x, corners[edge].y, corners[next].x, corners[next].y, color);
            }
        }
    }
    if (match_drag_) {
        const auto corners = match_drag_corners(live_viewport(match_camera_x_, match_camera_z_));
        box({corners[0].x, corners[0].y, corners[1].x, corners[1].y}, selection_color);
    }
}

} // namespace oa::app

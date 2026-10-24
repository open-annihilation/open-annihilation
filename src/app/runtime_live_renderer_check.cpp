// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Windowless Skirmish coverage of the live camera, rendered world, command
// picking and orders, camera reset, render target resizing and advancing simulation.
#include "oa/app/runtime.hpp"
#include "engine_settings_state.hpp"
#include "match_models.hpp"
#include "device_state.hpp"
#include "renderer_effect_sources.hpp"
#include "renderer_live_scene.hpp"
#include "presentation_interpolation.hpp"
#include "oa/formats/png.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/sim/messages.hpp"
#include "oa/ui/hud/health_bar.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

namespace oa::app {
namespace {

/// Mouse travel that produces a visibly different constrained view.
[[maybe_unused]] constexpr float orbit_travel = 48.0F;
/// Target dimensions used to exercise render target replacement.
[[maybe_unused]] constexpr int resized_width = 960;
[[maybe_unused]] constexpr int resized_height = 640;
/// Exit code for a platform without the experimental Metal backend.
constexpr int unavailable_exit = 77;

/// Ends the native check with a specific explanation.
[[maybe_unused]] void require(bool condition, const char* message) {
    if (!condition)
        throw std::runtime_error(std::string("live renderer check: ") + message);
}

/// Save a world picture for inspection without creating a window.
[[maybe_unused]] void write_picture(const fs::path& path, const renderer::Surface& frame) {
    constexpr uint8_t channel_bits = 8;
    const formats::png::Header header{
        frame.width, frame.height, channel_bits, formats::png::ColorType::rgb, {}
    };
    std::vector<uint8_t> file;
    require(formats::png::write({header, {}, frame.rgb}, &file), "could not encode a capture");
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(
        reinterpret_cast<const char*>(file.data()), static_cast<std::streamsize>(file.size())
    );
    require(static_cast<bool>(output), "could not write a capture");
}

} // namespace

int Runtime::check_live_renderer() {
#if !defined(__APPLE__)
    std::cout << "live renderer check: skipped: the experimental backend requires Metal\n";
    return unavailable_exit;
#else
    // SDL's GPU backend needs the native video driver, but no window or
    // visible renderer is created by this check.
    require(SDL_InitSubSystem(SDL_INIT_VIDEO), "could not initialize windowless video");
    require(sdl_.window == nullptr, "the check must not own a visible window");
    options_.experimental_3d = false;
    start_benchmark_skirmish();
    require(match_ != nullptr, "Skirmish did not create a match");
    select_local_commander();
    require(selected_match_unit_ != 0, "Skirmish did not select a commander");
    const auto commander = selected_match_unit_;
    const auto tick = match_timing_.tick;
    const auto initial_layout = match_layout_;
    const auto initial_view = match_view_place();
    const auto initial_status = status_;
    const auto save_picture = [&](const char* name, const renderer::Surface& frame) {
        if (!options_.snapshot.empty()) {
            fs::create_directories(options_.snapshot);
            write_picture(options_.snapshot / name, frame);
        }
    };
    const auto snapshot = [&] {
        // Console echoes are transient interface messages, not battlefield
        // changes; keep them out of frozen-frame comparisons.
        oa::sim::messages::clear_messages(match_->state().game);
        render_match_surface();
        require(match_->fault() == nullptr, "rendering raised a simulation fault");
        require(!match_world_cpu_.rgb.empty(), "Skirmish produced an empty world picture");
        return match_world_cpu_;
    };
    const auto key = [&](SDL_Keycode code, SDL_Keymod modifiers = SDL_KMOD_NONE) {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.key = code;
        event.key.down = true;
        event.key.mod = modifiers;
        return live_renderer_take_event(event);
    };
    const auto orbit = [&](uint8_t button = SDL_BUTTON_MIDDLE) {
        const float x = static_cast<float>(match_layout_.left) +
                        static_cast<float>(match_layout_.battlefield_width()) * 0.5F;
        const float y = static_cast<float>(match_layout_.top) +
                        static_cast<float>(match_layout_.battlefield_height()) * 0.5F;
        const auto modifiers = SDL_GetModState();
        SDL_SetModState(SDL_KMOD_ALT);
        SDL_Event event{};
        event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        event.button.button = button;
        event.button.down = true;
        event.button.x = x;
        event.button.y = y;
        const bool began = live_renderer_take_event(event);
        SDL_SetModState(modifiers);
        require(began, "modified drag did not start camera orbit");
        event = {};
        event.type = SDL_EVENT_MOUSE_MOTION;
        event.motion.state = SDL_BUTTON_MASK(button);
        event.motion.x = x + orbit_travel;
        event.motion.y = y + orbit_travel;
        event.motion.xrel = orbit_travel;
        event.motion.yrel = orbit_travel;
        require(live_renderer_take_event(event), "orbit motion was not consumed");
        event = {};
        event.type = SDL_EVENT_MOUSE_BUTTON_UP;
        event.button.button = button;
        event.button.x = x + orbit_travel;
        event.button.y = y + orbit_travel;
        require(live_renderer_take_event(event), "orbit release was not consumed");
        require(live_renderer_active(), "orbit left the camera at classic orientation");
    };

    require(!live_renderer_active(), "Skirmish did not start at classic orientation");
    // A camera click with no motion still owns its release after Option is
    // released; ordinary classic-view clicks must remain gameplay input.
    for (const bool modified : {false, true}) {
        const auto saved_modifiers = SDL_GetModState();
        SDL_SetModState(modified ? SDL_KMOD_ALT : SDL_KMOD_NONE);
        SDL_Event click{};
        click.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        click.button.button = SDL_BUTTON_LEFT;
        click.button.down = true;
        click.button.x = static_cast<float>(match_layout_.left + 32);
        click.button.y = static_cast<float>(match_layout_.top + 32);
        require(
            live_renderer_take_event(click) == modified, "classic left press did not respect Option"
        );
        SDL_SetModState(SDL_KMOD_NONE);
        click.type = SDL_EVENT_MOUSE_BUTTON_UP;
        click.button.down = false;
        const bool consumed = live_renderer_take_event(click);
        SDL_SetModState(saved_modifiers);
        require(consumed == modified, "a stationary Option click lost ownership of its release");
        require(!live_renderer_active(), "a stationary camera click changed the orientation");
    }
    const auto classic = snapshot();
    save_picture("classic.png", classic);
    require(snapshot().rgb == classic.rgb, "a stationary classic frame changed without a tick");
    orbit(SDL_BUTTON_LEFT);
    const auto rotated = snapshot();
    if (live_renderer_unavailable()) {
        std::cout << "live renderer check: skipped: no supported GPU device is available\n";
        return unavailable_exit;
    }
    require(!live_renderer_failed(), "the live GPU frame failed");
    require(live_renderer_vertex_count() != 0, "the live frame contained no world geometry");
    save_picture("rotated.png", rotated);
    require(
        rotated.width == classic.width && rotated.height == classic.height,
        "orbit changed the battlefield dimensions"
    );
    require(classic.rgb != rotated.rgb, "orbit did not change the battlefield");

    // Held arrows and the edge-scroll directions move the view along the
    // screen axes after orbiting, rather than along the map's fixed axes.
    const auto before_scroll_view = match_view_place();
    const auto before_scroll_hold = view_hold_;
    const auto before_scroll_time = frame_time_ns_;
    const auto before_scroll_clock = scroll_clock_;
    const auto before_scroll_speed = match_->state().game.scroll_speed;
    constexpr uint64_t scroll_frame_ns = 33'333'333;
    constexpr int32_t check_scroll_speed = 16;
    constexpr double edge_travel = 16;
    const std::array<SDL_Scancode, 4> arrows{
        SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_UP, SDL_SCANCODE_DOWN
    };
    const std::array<std::array<int32_t, 2>, 4> directions{{{-1, 0}, {1, 0}, {0, -1}, {0, 1}}};
    match_->state().game.scroll_speed = check_scroll_speed;
    frame_time_ns_ = std::max<uint64_t>(frame_time_ns_, 1);
    for (const bool arrow : {true, false})
        for (std::size_t direction = 0; direction < directions.size(); ++direction) {
            const auto before = live_project_world(0, 0, 0);
            const auto way = directions[direction];
            if (arrow) {
                scroll_clock_ = frame_time_ns_;
                frame_time_ns_ += scroll_frame_ns;
                device_state::hold_key(arrows[direction], true);
                move_match_camera();
                device_state::hold_key(arrows[direction], false);
            } else {
                scroll_match_view(way[0], way[1], edge_travel);
            }
            snapshot();
            const auto after = live_project_world(0, 0, 0);
            require(
                way[0] != 0
                    ? (after.x - before.x) * way[0] < 0 && std::abs(after.y - before.y) <= 1
                    : (after.y - before.y) * way[1] < 0 && std::abs(after.x - before.x) <= 1,
                arrow ? "held arrow did not pan the rotated view in its screen direction"
                      : "edge scroll did not pan the rotated view in its screen direction"
            );
        }
    match_->state().game.scroll_speed = before_scroll_speed;
    frame_time_ns_ = before_scroll_time;
    scroll_clock_ = before_scroll_clock;
    view_hold_ = before_scroll_hold;
    place_match_view(before_scroll_view[0], before_scroll_view[1]);
    require(snapshot().rgb == rotated.rgb, "scrolling back changed the frozen rotated view");

    // Menu keys keep the gameplay dispatcher while the battlefield is rotated.
    // Exercise the full event route, including a selected unit and open panels.
    const auto press_menu_key = [&](SDL_Keycode code, SDL_Scancode scancode) {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.key = code;
        event.key.scancode = scancode;
        event.key.down = true;
        bool running = true;
        dispatch_event(event, running);
        require(running, "a rotated-view menu key ended the run");
        require(live_renderer_active(), "a menu key reset the rotated camera");
    };
    const auto menu_modifiers = SDL_GetModState();
    SDL_SetModState(SDL_KMOD_ALT);
    SDL_Event menu_drag{};
    menu_drag.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    menu_drag.button.button = SDL_BUTTON_LEFT;
    menu_drag.button.down = true;
    menu_drag.button.x = static_cast<float>(match_layout_.left + 32);
    menu_drag.button.y = static_cast<float>(match_layout_.top + 32);
    const bool menu_drag_began = live_renderer_take_event(menu_drag);
    SDL_SetModState(menu_modifiers);
    require(menu_drag_began, "camera drag before opening the menu did not begin");
    press_menu_key(SDLK_F2, SDL_SCANCODE_F2);
    require(match_paused_, "F2 did not open the menu over the rotated battlefield");
    require(
        match_hud_panel_ == oa::data::defs::gui_path("ARMOPT.GUI"),
        "F2 did not show the in-game menu"
    );
    menu_drag.type = SDL_EVENT_MOUSE_BUTTON_UP;
    menu_drag.button.down = false;
    require(!live_renderer_take_event(menu_drag), "camera captured a release over the menu");
    save_picture("rotated-menu.png", snapshot());
    press_menu_key(SDLK_O, SDL_SCANCODE_O);
    require(
        match_hud_panel_ == oa::data::defs::gui_path("PREFS.GUI"),
        "O did not open Options over the rotated battlefield"
    );
    press_menu_key(SDLK_C, SDL_SCANCODE_C);
    require(
        match_hud_panel_ == oa::data::defs::gui_path("ARMOPT.GUI"),
        "C did not cancel Options over the rotated battlefield"
    );
    press_menu_key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
    require(!match_paused_, "Escape did not resume the rotated match");
    SDL_Event menu_motion{};
    menu_motion.type = SDL_EVENT_MOUSE_MOTION;
    menu_motion.motion.x = menu_drag.button.x + orbit_travel;
    menu_motion.motion.y = menu_drag.button.y;
    menu_motion.motion.xrel = orbit_travel;
    require(!live_renderer_take_event(menu_motion), "camera drag stayed latched after menu close");
    require(selected_match_unit_ == commander, "menu navigation lost the selected commander");
    require(snapshot().rgb == rotated.rgb, "closing the menu changed the rotated battlefield");

    // Escape preserves its cancel-first behavior; the menu is the next
    // action only when that setting is enabled, including isolated profiles.
    require(engine_settings_ != nullptr, "the menu check has no engine settings");
    const bool saved_escape_menu = engine_settings_->current.escape_opens_menu;
    engine_settings_->current.escape_opens_menu = true;
    press_menu_key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
    require(
        !match_paused_ && !has_local_selection(),
        "Escape did not clear the rotated-view selection before opening a menu"
    );
    press_menu_key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
    require(match_paused_, "enabled Escape did not open the rotated-view menu");
    press_menu_key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
    require(!match_paused_, "Escape did not close the rotated-view menu");
    engine_settings_->current.escape_opens_menu = false;
    press_menu_key(SDLK_ESCAPE, SDL_SCANCODE_ESCAPE);
    require(!match_paused_, "disabled Escape unexpectedly opened the rotated-view menu");
    engine_settings_->current.escape_opens_menu = saved_escape_menu;
    select_local_commander();

    // Restrict effect visibility to row intervals without changing samples
    // within them. Unrequested pixels retain the empty-mask values.
    std::vector<uint8_t> full_visibility;
    std::vector<float> full_depth;
    live_scene_visibility(full_visibility, full_depth);
    std::vector<uint32_t> row_left(rotated.height);
    std::vector<uint32_t> row_right(rotated.height);
    for (uint32_t row = 0; row < rotated.height; ++row) {
        row_left[row] = row % 3 == 0 ? 0 : rotated.width / 4;
        row_right[row] = row % 3 == 0 ? 0 : rotated.width * 3 / 4;
    }
    std::vector<uint8_t> clipped_visibility;
    std::vector<float> clipped_depth;
    live_scene_visibility(clipped_visibility, clipped_depth, false, row_left, row_right);
    require(
        full_visibility.size() == static_cast<std::size_t>(rotated.width) * rotated.height &&
            full_depth.size() == full_visibility.size() &&
            full_visibility.size() == clipped_visibility.size() &&
            full_depth.size() == clipped_depth.size(),
        "clipped visibility changed the output dimensions"
    );
    for (uint32_t row = 0; row < rotated.height; ++row)
        for (uint32_t column = 0; column < rotated.width; ++column) {
            const auto pixel = static_cast<std::size_t>(row) * rotated.width + column;
            if (column >= row_left[row] && column < row_right[row]) {
                require(
                    clipped_visibility[pixel] == full_visibility[pixel] &&
                        clipped_depth[pixel] == full_depth[pixel],
                    "clipping changed visibility or depth inside a requested interval"
                );
            } else {
                require(
                    clipped_visibility[pixel] == 0 &&
                        clipped_depth[pixel] == -std::numeric_limits<float>::infinity(),
                    "clipped visibility painted outside the requested interval"
                );
            }
        }

    // Separated light rectangles leave gaps even within a row's enclosing
    // interval. Sparse masking must preserve exactly the requested samples.
    std::vector<uint8_t> requested_pixels(full_visibility.size(), 0);
    for (uint32_t row = 0; row < rotated.height; ++row)
        for (uint32_t column = row_left[row]; column < row_right[row]; ++column) {
            constexpr uint32_t stripe_width = 16;
            if ((column / stripe_width + row / stripe_width) % 2 == 0)
                requested_pixels[static_cast<std::size_t>(row) * rotated.width + column] = 1;
        }
    std::vector<uint8_t> sparse_visibility;
    std::vector<float> sparse_depth;
    live_scene_visibility(
        sparse_visibility, sparse_depth, false, row_left, row_right, requested_pixels
    );
    require(
        sparse_visibility.size() == full_visibility.size() &&
            sparse_depth.size() == full_depth.size(),
        "sparse visibility changed output dimensions"
    );
    for (std::size_t pixel = 0; pixel < requested_pixels.size(); ++pixel) {
        if (requested_pixels[pixel]) {
            require(
                sparse_visibility[pixel] == full_visibility[pixel] &&
                    sparse_depth[pixel] == full_depth[pixel],
                "sparse masking changed a requested visibility or depth sample"
            );
        } else {
            require(
                sparse_visibility[pixel] == 0 &&
                    sparse_depth[pixel] == -std::numeric_limits<float>::infinity(),
                "sparse visibility painted an unrequested pixel"
            );
        }
    }

    // The physical key below Escape toggles projected bars through the gameplay dispatcher.
    const auto saved_graphics = match_->state().game.graphics_flags;
    match_->state().game.graphics_flags &= ~oa::ui::hud::kGraphicsDamageBars;
    const auto without_bars = snapshot();
    SDL_Event bars_key{};
    bars_key.type = SDL_EVENT_KEY_DOWN;
    bars_key.key.key = SDLK_GRAVE;
    bars_key.key.scancode = SDL_SCANCODE_GRAVE;
    bool running = true;
    handle_sdl_event(bars_key, running);
    const auto with_bars = snapshot();
    require(!drawn_health_bars_.empty(), "backtick did not draw projected health bars");
    require(with_bars.rgb != without_bars.rgb, "projected health bars changed no pixels");
    save_picture("health-bars.png", with_bars);
    handle_sdl_event(bars_key, running);
    require(snapshot().rgb == without_bars.rgb, "backtick left health bars behind");
    match_->state().game.graphics_flags = saved_graphics;

    // Contours may affect known ground, but must not paint over objects or
    // reveal unexplored terrain. Exercise this before the reveal cheat.
    const auto terrain_mask = live_terrain_visibility_mask();
    require(
        terrain_mask.size() == static_cast<std::size_t>(rotated.width) * rotated.height,
        "the rotated terrain visibility mask has the wrong dimensions"
    );
    enter_console_check_line("+contour 3");
    const auto fog_contoured = snapshot();
    std::size_t protected_pixels{};
    std::size_t changed_ground_pixels{};
    for (std::size_t pixel = 0; pixel < terrain_mask.size(); ++pixel) {
        const auto at = pixel * 3;
        const bool changed = rotated.rgb[at] != fog_contoured.rgb[at] ||
                             rotated.rgb[at + 1] != fog_contoured.rgb[at + 1] ||
                             rotated.rgb[at + 2] != fog_contoured.rgb[at + 2];
        if (terrain_mask[pixel] == 0) {
            ++protected_pixels;
            require(!changed, "contour changed an object or unexplored terrain pixel");
        } else if (changed) {
            ++changed_ground_pixels;
        }
    }
    require(protected_pixels != 0, "the contour fog fixture has no protected pixels");
    require(changed_ground_pixels != 0, "contour did not mark any known rotated terrain");
    save_picture("contour-fog.png", fog_contoured);
    enter_console_check_line("+contour 0");
    const auto fog_contour_off = snapshot();
    save_picture("contour-fog-off.png", fog_contour_off);
    require(fog_contour_off.rgb == rotated.rgb, "disabling contour changed the fogged battlefield");

    const auto saved_pointer = std::array{pointer_x_, pointer_y_};
    const auto commander_bounds = live_unit_bounds(commander);
    require(commander_bounds.has_value(), "the rotated commander has no projected bounds");
    std::optional<std::array<float, 2>> commander_pixel;
    for (int y = commander_bounds->y1; y <= commander_bounds->y2 && !commander_pixel; ++y)
        for (int x = commander_bounds->x1; x <= commander_bounds->x2; ++x)
            if (live_unit_under(static_cast<float>(x), static_cast<float>(y)) == commander) {
                commander_pixel = std::array{static_cast<float>(x), static_cast<float>(y)};
                break;
            }
    require(commander_pixel.has_value(), "no visible commander triangle could be picked");
    clear_local_selection();
    select_match_unit((*commander_pixel)[0], (*commander_pixel)[1], 1);
    require(
        selected_match_unit_ == commander, "rotated triangle click did not select the commander"
    );
    clear_local_selection();
    box_select_units(
        commander_bounds->x1 - 4,
        commander_bounds->y1 - 4,
        commander_bounds->x2 + 4,
        commander_bounds->y2 + 4,
        false
    );
    require(selected_match_unit_ == commander, "rotated drag box did not select the commander");
    std::optional<oa::sim::ground_orders::Point> destination;
    std::array<float, 2> destination_pixel{};
    for (int offset = 80; offset <= 240 && !destination; offset += 40) {
        for (const int sign : {-1, 1}) {
            const float x = (*commander_pixel)[0] + sign * offset;
            const float y = (*commander_pixel)[1] + 48;
            if (live_unit_under(x, y) != 0)
                continue;
            if (const auto ground = live_ground_point(x, y)) {
                destination = ground;
                destination_pixel = {x, y};
                break;
            }
        }
    }
    require(destination.has_value(), "no terrain destination was visible in the rotated view");
    const auto projected_ground = live_project_world(
        (*destination)[0] / 65536.0F, (*destination)[1] / 65536.0F, (*destination)[2] / 65536.0F
    );
    require(
        std::abs(projected_ground.x - destination_pixel[0]) <= 1 &&
            std::abs(projected_ground.y - destination_pixel[1]) <= 1,
        "terrain ray intersection did not project back to the order click"
    );
    SDL_Event ordinary{};
    ordinary.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
    ordinary.button.button = SDL_BUTTON_LEFT;
    ordinary.button.x = destination_pixel[0];
    ordinary.button.y = destination_pixel[1];
    require(!live_renderer_take_event(ordinary), "ordinary rotated-view click was swallowed");
    match_command_ = MatchCommand::move;
    handle_match_left_click(destination_pixel[0], destination_pixel[1], 1);
    std::array<oa::sim::match_runtime::Match::OrderRecordView, 4> orders{};
    require(
        match_->queue_records(commander, false, orders.data(), orders.size()) > 0,
        "rotated move click did not issue an order"
    );
    require(
        orders[0].point[0] == (*destination)[0] && orders[0].point[2] == (*destination)[2],
        "rotated move order did not retain the terrain intersection"
    );
    const auto shift_before_orders = shift_held_by_check_;
    shift_held_by_check_ = false;
    const auto without_orders = snapshot();
    shift_held_by_check_ = true;
    const auto with_orders = snapshot();
    shift_held_by_check_ = shift_before_orders;
    require(with_orders.rgb != without_orders.rgb, "Shift did not draw the rotated order queue");
    save_picture("move-queue.png", with_orders);

    // A fractional trackpad wheel and its flipped counterpart must reverse
    // one another while keeping the terrain under the pointer anchored.
    const auto zoom_before = match_zoom_;
    SDL_Event wheel{};
    wheel.type = SDL_EVENT_MOUSE_WHEEL;
    wheel.wheel.y = 0.5F;
    wheel.wheel.mouse_x = destination_pixel[0];
    wheel.wheel.mouse_y = destination_pixel[1];
    require(live_renderer_take_event(wheel), "the rotated view did not consume a fractional wheel");
    snapshot();
    require(match_zoom_ > zoom_before, "wheel up did not zoom into the rotated view");
    const auto anchored = live_ground_point(destination_pixel[0], destination_pixel[1]);
    constexpr int32_t anchor_tolerance = 2 * 65536;
    require(
        anchored && std::abs((*anchored)[0] - (*destination)[0]) <= anchor_tolerance &&
            std::abs((*anchored)[2] - (*destination)[2]) <= anchor_tolerance,
        "wheel zoom moved the terrain point under the pointer"
    );
    save_picture("zoomed.png", match_world_cpu_);
    wheel.wheel.direction = SDL_MOUSEWHEEL_FLIPPED;
    require(live_renderer_take_event(wheel), "the rotated view did not consume a flipped wheel");
    snapshot();
    require(std::abs(match_zoom_ - zoom_before) < 0.001F, "flipped wheel did not reverse zoom");
    // Keep the exact classic-reset comparison independent of camera clamping.
    place_match_view(initial_view[0], initial_view[1]);
    snapshot();

    match_->stop_orders(commander);
    match_command_ = MatchCommand::none;
    hovered_match_unit_ = 0;
    match_->state().game.cursor_unit_id = 0;
    pointer_x_ = saved_pointer[0];
    pointer_y_ = saved_pointer[1];
    status_ = initial_status;
    const auto view_before_pan = match_view_place();
    const auto map_size = shown_map_size();
    const float pan_travel =
        view_before_pan[0] < static_cast<double>(map_size[0]) * 0.5 ? -orbit_travel : orbit_travel;
    const float pan_x = static_cast<float>(match_layout_.left) +
                        static_cast<float>(match_layout_.battlefield_width()) * 0.5F;
    const float pan_y = static_cast<float>(match_layout_.top) +
                        static_cast<float>(match_layout_.battlefield_height()) * 0.5F;
    for (const uint8_t button : std::array<uint8_t, 2>{SDL_BUTTON_MIDDLE, SDL_BUTTON_LEFT}) {
        const auto before_drag = match_view_place();
        const auto modifiers = SDL_GetModState();
        SDL_SetModState(
            button == SDL_BUTTON_LEFT ? static_cast<SDL_Keymod>(SDL_KMOD_ALT | SDL_KMOD_SHIFT)
                                      : SDL_KMOD_NONE
        );
        SDL_Event pan{};
        pan.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
        pan.button.button = button;
        pan.button.down = true;
        pan.button.x = pan_x;
        pan.button.y = pan_y;
        const bool pan_began = live_renderer_take_event(pan);
        SDL_SetModState(modifiers);
        require(pan_began, "middle-drag did not start a pan");
        pan = {};
        pan.type = SDL_EVENT_MOUSE_MOTION;
        pan.motion.state = SDL_BUTTON_MASK(button);
        pan.motion.x = pan_x + pan_travel;
        pan.motion.y = pan_y;
        pan.motion.xrel = pan_travel;
        require(live_renderer_take_event(pan), "pan motion was not consumed");
        pan = {};
        pan.type = SDL_EVENT_MOUSE_BUTTON_UP;
        pan.button.button = button;
        pan.button.x = pan_x + pan_travel;
        pan.button.y = pan_y;
        require(live_renderer_take_event(pan), "pan release was not consumed");
        require(match_view_place() != before_drag, "a camera pan binding did not move the view");
    }
    const auto panned_view = match_view_place();
    require(panned_view != view_before_pan, "middle-drag did not move the camera");
    const auto panned = snapshot();
    require(
        !live_renderer_failed() && panned.rgb != rotated.rgb,
        "middle-drag did not change the rendered world"
    );
    save_picture("panned.png", panned);
    require(
        key(SDLK_U, SDL_KMOD_ALT) && live_renderer_unlocked(), "Option+U did not unlock the camera"
    );
    require(
        key(SDLK_U, SDL_KMOD_ALT) && !live_renderer_unlocked(),
        "Option+U did not restrict the camera"
    );
    require(
        key(SDLK_R, SDL_KMOD_ALT) && !live_renderer_active(), "Option+R did not reset the camera"
    );
    require(match_view_place() == panned_view, "Option+R discarded the camera pan");
    const auto camera_settings = engine_settings();
    auto restricted_settings = camera_settings;
    restricted_settings.allow_full_3d = false;
    apply_engine_settings(restricted_settings);
    require(!live_renderer_active(), "disabling 3D left the camera active");
    require(!key(SDLK_U, SDL_KMOD_ALT), "disabled 3D consumed the unlock shortcut");
    restricted_settings.allow_full_3d = true;
    restricted_settings.unlocked_camera = true;
    apply_engine_settings(restricted_settings);
    require(live_renderer_unlocked(), "the camera ignored the menu unlock setting");
    require(!live_renderer_active(), "unlocking at rest changed the classic starting view");
    orbit();
    require(live_renderer_active(), "the default-enabled camera still needed the legacy flag");
    restricted_settings.allow_full_3d = false;
    apply_engine_settings(restricted_settings);
    require(!live_renderer_active(), "turning 3D off did not restore classic orientation");
    apply_engine_settings(camera_settings);

    orbit();
    require(!key(SDLK_F8) && live_renderer_active(), "F8 changed the camera");
    require(!key(SDLK_F9) && !live_renderer_unlocked(), "F9 changed the camera lock");
    require(!key(SDLK_F9, SDL_KMOD_CTRL), "the camera consumed the screenshot shortcut");
    require(
        key(SDLK_R, SDL_KMOD_ALT) && !live_renderer_active(), "Option+R did not reset the camera"
    );
    require(match_view_place() == panned_view, "reset discarded the camera pan");
    place_match_view(initial_view[0], initial_view[1]);
    const auto reset_picture = snapshot();
    save_picture("reset.png", reset_picture);
    require(
        reset_picture.rgb == classic.rgb, "reset did not restore the exact classic battlefield"
    );

    match_layout_ = lay_out_match(resized_width, resized_height);
    orbit();
    const auto resized = snapshot();
    require(!live_renderer_failed(), "resizing the live GPU target failed");
    require(
        resized.width != classic.width || resized.height != classic.height,
        "the live framebuffer did not follow the resized battlefield"
    );
    // A wide, zoomed-out viewport can see ground far from its orbit pivot,
    // including when the map itself is small. Pan the pivot past each side
    // while keeping the map center on screen, then pick its terrain.
    constexpr float distant_pivot = 2304.0F;
    constexpr float wide_zoom = 0.125F;
    constexpr float map_cell = 16.0F;
    constexpr float height_projection = 0.5F;
    constexpr float fixed_scale = 65536.0F;
    constexpr float ground_tolerance = 128.0F;
    constexpr int wide_width = 1280;
    constexpr int wide_height = 960;
    const auto before_wide_view = match_view_place();
    const float before_wide_zoom = match_zoom_;
    const float before_wide_target = match_zoom_target_;
    const auto column = selected_tnt_->attribute_width / 2;
    const auto row = selected_tnt_->attribute_height / 2;
    const float probe_x = (static_cast<float>(column) + 0.5F) * map_cell;
    const float probe_y = (static_cast<float>(row) + 0.5F) * map_cell;
    const float probe_height =
        selected_tnt_
            ->attributes[static_cast<std::size_t>(row) * selected_tnt_->attribute_width + column]
            .height;
    match_layout_ = lay_out_match(wide_width, wide_height);
    match_zoom_ = match_zoom_target_ = wide_zoom;
    for (const auto offset : std::array<std::array<float, 2>, 4>{
             {{distant_pivot, 0}, {-distant_pivot, 0}, {0, distant_pivot}, {0, -distant_pivot}}
         }) {
        orbit();
        place_match_view(
            probe_x + offset[0] - match_layout_.battlefield_width() / (2 * wide_zoom),
            probe_y + offset[1] - match_layout_.battlefield_height() / (2 * wide_zoom)
        );
        snapshot();
        require(!live_renderer_failed(), "zoomed-out panned terrain failed to render");
        const auto pixel =
            live_project_world(probe_x, probe_height, probe_y + probe_height * height_projection);
        require(
            battlefield_contains(static_cast<float>(pixel.x), static_cast<float>(pixel.y)),
            "the distant terrain probe was not inside the viewport"
        );
        const auto ground =
            live_ground_point(static_cast<float>(pixel.x), static_cast<float>(pixel.y));
        require(ground.has_value(), "zoomed-out terrain stopped before a visible ground point");
        const float picked_x = static_cast<float>((*ground)[0]) / fixed_scale;
        const float picked_y = (static_cast<float>((*ground)[2]) -
                                static_cast<float>((*ground)[1]) * height_projection) /
                               fixed_scale;
        require(
            std::abs(picked_x - probe_x) < ground_tolerance &&
                std::abs(picked_y - probe_y) < ground_tolerance,
            "zoomed-out terrain picking reached a different part of the map"
        );
    }
    match_zoom_ = before_wide_zoom;
    match_zoom_target_ = before_wide_target;
    place_match_view(before_wide_view[0], before_wide_view[1]);
    reset_live_renderer();
    match_layout_ = initial_layout;
    require(snapshot().rgb == classic.rgb, "reset after resizing changed the classic battlefield");
    orbit();
    require(
        !snapshot().rgb.empty() && !live_renderer_failed(),
        "the live GPU target could not be reused after reset"
    );
    // Backend rejection must release the camera into usable classic controls,
    // and a later gesture must be able to create a working live frame again.
    const auto recovery_layout = match_layout_;
    const auto recovery_view = match_view_place();
    const auto recovery_zoom = match_zoom_;
    const auto recovery_target = match_zoom_target_;
    constexpr int rejected_width = 4608;
    constexpr int rejected_height = 480;
    match_layout_ = lay_out_match(rejected_width, rejected_height);
    require(
        match_layout_.battlefield_width() > 4096,
        "failure fixture did not exceed the GPU dimension limit"
    );
    snapshot();
    require(
        live_renderer_failed() && !live_renderer_active(),
        "GPU rejection did not release the failed camera to classic controls"
    );
    match_layout_ = recovery_layout;
    SDL_Event recovery_wheel{};
    recovery_wheel.type = SDL_EVENT_MOUSE_WHEEL;
    recovery_wheel.wheel.y = 1.0F;
    recovery_wheel.wheel.mouse_x = static_cast<float>(match_layout_.left + 100);
    recovery_wheel.wheel.mouse_y = static_cast<float>(match_layout_.top + 100);
    const auto failed_zoom_target = match_zoom_target_;
    require(
        !live_renderer_take_event(recovery_wheel),
        "the failed renderer consumed the wheel before classic fallback"
    );
    // This fixture has no SDL renderer for window-to-canvas conversion;
    // hand its already-canvas coordinates to the ordinary classic handler.
    handle_match_zoom(
        recovery_wheel.wheel.y, recovery_wheel.wheel.mouse_x, recovery_wheel.wheel.mouse_y, true
    );
    require(
        match_zoom_target_ > failed_zoom_target,
        "the failed renderer trapped wheel zoom instead of allowing classic fallback"
    );
    orbit();
    snapshot();
    require(
        !live_renderer_failed() && live_renderer_active(),
        "a new camera gesture did not recover after GPU rejection"
    );
    for (int repeat = 0; repeat < 2; ++repeat)
        for (const float direction : {1.0F, -1.0F}) {
            reset_live_renderer();
            require(!live_renderer_failed(), "camera reset retained a sticky renderer failure");
            orbit();
            recovery_wheel.wheel.y = direction * 100.0F;
            require(
                live_renderer_take_event(recovery_wheel),
                "live camera did not accept the zoom-limit gesture"
            );
            const auto limit = direction > 0 ? most_match_zoom() : least_match_zoom();
            require(
                std::abs(match_zoom_ - limit) < 0.001F,
                "wheel gesture did not reach the configured zoom limit"
            );
            snapshot();
            require(!live_renderer_failed(), "a valid zoom limit rejected live scene geometry");
        }
    reset_live_renderer();
    match_zoom_ = recovery_zoom;
    match_zoom_target_ = recovery_target;
    place_match_view(recovery_view[0], recovery_view[1]);
    orbit();
    snapshot();

    // Existing enemy units remain unpickable under fog, then become attack targets
    // when the game's own reveal command makes them visible.
    uint16_t enemy{};
    for (const auto& slot : match_->world().slots)
        if (slot.unit && slot.unit_index != 0 && slot.owner_index != match_local_player_ &&
            slot.record.type_index != 0) {
            enemy = slot.unit_index;
            break;
        }
    require(enemy != 0, "Skirmish has no enemy for attack picking");
    center_camera_on_unit(enemy);
    snapshot();
    if (!match_->unit_shown(match_view_player(), enemy))
        require(!live_unit_bounds(enemy), "an enemy under fog entered live picking geometry");
    enter_console_check_line("+nowisee");
    require(
        match_->unit_shown(match_view_player(), enemy),
        "the reveal command did not expose the enemy"
    );
    snapshot();
    const auto enemy_bounds = live_unit_bounds(enemy);
    require(enemy_bounds.has_value(), "a visible enemy has no live bounds");
    std::optional<std::array<float, 2>> enemy_pixel;
    for (int y = enemy_bounds->y1; y <= enemy_bounds->y2 && !enemy_pixel; ++y)
        for (int x = enemy_bounds->x1; x <= enemy_bounds->x2; ++x)
            if (live_unit_under(static_cast<float>(x), static_cast<float>(y)) == enemy) {
                enemy_pixel = std::array{static_cast<float>(x), static_cast<float>(y)};
                break;
            }
    require(enemy_pixel.has_value(), "the visible enemy model could not be picked");
    clear_local_selection();
    adopt_selection(commander);
    match_command_ = MatchCommand::attack;
    handle_match_left_click((*enemy_pixel)[0], (*enemy_pixel)[1], 1);
    require(hovered_match_unit_ == enemy, "the rotated attack click picked a different unit");
    require(
        match_->queue_records(commander, false, orders.data(), orders.size()) > 0 &&
            orders[0].target == enemy,
        "the rotated attack click did not queue the enemy target"
    );
    save_picture("attack-target.png", snapshot());
    require(match_timing_.tick == tick, "camera checks advanced the simulation");
    // Empty slots and the no-hover sentinel have no visibility bounds. Rebuilding
    // gameplay picking between advancing ticks must leave simulation faults clear.
    require(!live_unit_bounds(0), "the no-unit sentinel has projected bounds");
    bool checked_empty_slot{};
    for (const auto& slot : match_->world().slots)
        if (slot.unit_index != 0 && slot.record.type_index == 0) {
            require(!live_unit_bounds(slot.unit_index), "an unused unit slot has projected bounds");
            checked_empty_slot = true;
            break;
        }
    require(checked_empty_slot, "Skirmish has no unused slot for the visibility check");
    // Console terrain overlays must survive the replacement of the flat
    // battlefield with the live GPU frame and disappear when disabled.
    center_camera_on_unit(commander);
    const auto no_contour = snapshot();
    enter_console_check_line("+contour 3");
    const auto contoured = snapshot();
    require(contoured.rgb != no_contour.rgb, "+contour did not change the rotated terrain");
    save_picture("contour.png", contoured);
    enter_console_check_line("+contour 0");
    require(snapshot().rgb == no_contour.rgb, "disabling contour left marks in the rotated view");

    constexpr int advancing_frames = 8;
    hovered_match_unit_ = 0;
    for (int frame = 0; frame < advancing_frames; ++frame) {
        rebuild_on_screen_units();
        require(match_->fault() == nullptr, "live picking raised a simulation fault");
        step_match_simulation();
        snapshot();
    }
    require(match_timing_.tick == tick + advancing_frames, "the live match did not advance");
    // A real moving commander must occupy intermediate screen positions
    // between ticks, including the bounds used for selection and picking.
    match_->stop_orders(commander);
    (void)match_->issue_ground_move(commander, *destination, false);
    center_camera_on_unit(commander);
    bool moving{};
    constexpr int movement_start_ticks = 150;
    for (int frame = 0; frame < movement_start_ticks && !moving; ++frame) {
        const auto before = match_->world().record.units[commander].position;
        step_match_simulation();
        observe_match_tick(match_models(), *match_);
        const auto after = match_->world().record.units[commander].position;
        moving = std::abs(after.x - before.x) >= 65536 || std::abs(after.z - before.z) >= 65536;
        require(match_->fault() == nullptr, "the moving commander raised a simulation fault");
    }
    require(moving, "the commander did not move for the interpolation check");
    snapshot();
    step_match_simulation();
    const auto sample_bounds = [&](float alpha, const char* name) {
        set_presentation_alpha(alpha);
        const auto picture = snapshot();
        const auto bounds = live_unit_bounds(commander);
        set_presentation_alpha(1.0F);
        require(bounds.has_value(), "the interpolated commander lost its picking bounds");
        save_picture(name, picture);
        return *bounds;
    };
    const auto from = sample_bounds(0.0F, "motion-start.png");
    const auto middle = sample_bounds(0.5F, "motion-half.png");
    const auto to = sample_bounds(1.0F, "motion-end.png");
    const int from_x = from.x1 + from.x2;
    const int from_y = from.y1 + from.y2;
    const int middle_x = middle.x1 + middle.x2;
    const int middle_y = middle.y1 + middle.y2;
    const int to_x = to.x1 + to.x2;
    const int to_y = to.y1 + to.y2;
    require(from_x != to_x || from_y != to_y, "the live commander snapped to one whole-tick pose");
    constexpr int midpoint_tolerance = 8;
    require(
        std::abs(2 * middle_x - from_x - to_x) <= midpoint_tolerance &&
            std::abs(2 * middle_y - from_y - to_y) <= midpoint_tolerance,
        "the live commander's half-tick bounds are not between its whole-tick bounds"
    );

    // Find an actual legal construction site and let the builder reach it.
    // This exercises construction and nanolathe rendering while simulation
    // and visibility advance, rather than injecting decorative particles.
    match_->stop_orders(commander);
    const auto solar = oa::sim::unit_spawn::find_type_index(spawn_type_names_, "ARMSOLAR");
    require(solar != 0 && solar < spawn_types_.size(), "the construction fixture lacks ARMSOLAR");
    const auto position = match_->world().record.units[commander].position;
    const int32_t cell_x = position.x >> 20;
    const int32_t cell_z = position.z >> 20;
    std::optional<PendingBuildSite> site;
    pending_build_type_ = solar;
    constexpr int nearest_site = 4;
    constexpr int farthest_site = 14;
    for (int ring = nearest_site; ring < farthest_site && !site; ++ring)
        for (int dz = -ring; dz <= ring && !site; dz += 2)
            for (int dx = -ring; dx <= ring && !site; dx += 2) {
                if (std::abs(dx) != ring && std::abs(dz) != ring)
                    continue;
                const auto& type = spawn_types_[solar];
                const oa::sim::ground_orders::Point point{
                    ((cell_x + dx) * 16 + type.footprint_x * 8) * 65536,
                    0,
                    ((cell_z + dz) * 16 + type.footprint_z * 8) * 65536
                };
                if (auto candidate = pending_build_site(point); candidate && candidate->legal)
                    site = candidate;
            }
    pending_build_type_ = 0;
    require(site.has_value(), "no legal construction site was available near the commander");
    (void)match_->issue_mobile_build(commander, solar, site->world, false);
    const auto shift_before_build = shift_held_by_check_;
    shift_held_by_check_ = true;
    save_picture("build-queue.png", snapshot());
    shift_held_by_check_ = shift_before_build;
    constexpr int construction_start_ticks = 300;
    bool building{};
    for (int frame = 0; frame < construction_start_ticks && !building; ++frame) {
        auto& player = match_->world().players[match_local_player_];
        player.metal = player.metal_cap;
        player.energy = player.energy_cap;
        step_match_simulation();
        require(match_->fault() == nullptr, "construction raised a simulation fault");
        building = match_->effects().nano.live != 0;
    }
    require(building, "the commander did not produce a nanolathe stream at its build site");
    center_camera_on_unit(commander);
    save_picture("construction-nanolathe.png", snapshot());
    require(
        live_renderer_nano_particle_count() != 0,
        "the live frame omitted active visible nanolathe particles"
    );
    require(
        live_renderer_emissive_vertex_count() != 0,
        "active construction and nanolathe particles produced no emissive geometry"
    );
    for (int frame = 0; frame < advancing_frames; ++frame) {
        rebuild_on_screen_units();
        step_match_simulation();
        snapshot();
    }
    require(!live_renderer_failed(), "the live frame failed during construction");
    // The same real sources must light both the classic battlefield and the
    // rotated view. Disabling their settings must leave composition inert.
    require(engine_settings_ != nullptr, "the effects check has no graphics settings");
    const auto saved_effect_settings = engine_settings_->current;
    const oa::ui::engine_settings::EngineSettings defaults{};
    require(
        defaults.weapon_lighting && defaults.nanolathe_lighting && defaults.wreck_glow &&
            defaults.heat_distortion,
        "the new battlefield effects are not enabled by default"
    );
    const auto effects_enabled = [&](bool enabled) {
        auto& settings = engine_settings_->current;
        settings.weapon_lighting = enabled ? 100 : 0;
        settings.nanolathe_lighting = enabled ? 100 : 0;
        settings.wreck_glow = enabled ? 100 : 0;
        settings.heat_distortion = enabled ? 100 : 0;
    };
    const auto inspect_effect_views = [&](const char* name, renderer_effect_sources::Kind kind) {
        for (const bool live : {true, false}) {
            if (live && !live_renderer_active())
                orbit();
            if (!live)
                reset_live_renderer();
            effects_enabled(false);
            const auto plain = snapshot();
            auto disabled = plain;
            compose_match_effects(disabled, live);
            require(disabled.rgb == plain.rgb, "disabled effects changed battlefield pixels");
            auto& settings = engine_settings_->current;
            settings.weapon_lighting = kind == renderer_effect_sources::Kind::weapon ? 100 : 0;
            settings.nanolathe_lighting =
                kind == renderer_effect_sources::Kind::nanolathe ? 100 : 0;
            settings.wreck_glow = kind == renderer_effect_sources::Kind::wreck ? 100 : 0;
            settings.heat_distortion = 0;
            const auto lit = snapshot();
            const std::string effect_context = std::string(name) + (live ? " 3D" : " classic");
            require(
                lit.rgb != plain.rgb, (effect_context + " did not light the battlefield").c_str()
            );
            if (live) {
                std::vector<uint8_t> terrain_visibility;
                std::vector<float> terrain_depth;
                live_scene_visibility(terrain_visibility, terrain_depth, true);
                std::size_t lit_ground_pixels = 0;
                for (std::size_t pixel = 0; pixel < terrain_visibility.size(); ++pixel)
                    if (terrain_visibility[pixel] != 0 && pixel * 3 + 2 < lit.rgb.size() &&
                        (lit.rgb[pixel * 3] != plain.rgb[pixel * 3] ||
                         lit.rgb[pixel * 3 + 1] != plain.rgb[pixel * 3 + 1] ||
                         lit.rgb[pixel * 3 + 2] != plain.rgb[pixel * 3 + 2]))
                        ++lit_ground_pixels;
                require(
                    lit_ground_pixels != 0,
                    (effect_context + " did not spill light onto visible terrain").c_str()
                );
            }
            require(snapshot().rgb == lit.rgb, "same-tick effects changed on a repeated frame");
            auto& intensity =
                kind == renderer_effect_sources::Kind::weapon      ? settings.weapon_lighting
                : kind == renderer_effect_sources::Kind::nanolathe ? settings.nanolathe_lighting
                                                                   : settings.wreck_glow;
            intensity = 50;
            const auto subtle = snapshot();
            intensity = 200;
            const auto strong = snapshot();
            require(
                subtle.rgb != lit.rgb && strong.rgb != lit.rgb && subtle.rgb != strong.rgb,
                (effect_context + " ignored the effect intensity").c_str()
            );
            intensity = 100;
            const std::string stem = std::string(name) + (live ? "-3d" : "-classic");
            save_picture((stem + "-off.png").c_str(), plain);
            save_picture((stem + "-on.png").c_str(), lit);
            if (kind != renderer_effect_sources::Kind::nanolathe) {
                settings.heat_distortion = 100;
                const auto shimmer = snapshot();
                require(
                    shimmer.rgb != lit.rgb,
                    (effect_context + " heat distortion did not change the battlefield").c_str()
                );
                require(
                    snapshot().rgb == shimmer.rgb, "paused heat distortion changed between frames"
                );
                save_picture((stem + "-heat.png").c_str(), shimmer);
            }
        }
    };
    const auto has_source = [&](renderer_effect_sources::Kind kind) {
        const auto sources = collect_match_effect_sources();
        return std::any_of(sources.begin(), sources.end(), [kind](const auto& source) {
            return source.kind == kind && (source.intensity > 0 || source.glow > 0);
        });
    };
    require(
        has_source(renderer_effect_sources::Kind::nanolathe), "construction has no light source"
    );
    inspect_effect_views("nanolathe-effects", renderer_effect_sources::Kind::nanolathe);

    // Capture the same real unit as construction replaces its open skeleton
    // with textured surfaces. Advance funded simulation rather than changing
    // the unit's canonical progress for the pictures.
    uint16_t construction_unit{};
    for (const auto& slot : match_->world().slots)
        if (slot.unit != nullptr && slot.record.type_index == solar &&
            slot.record.owner_index == match_local_player_ && slot.record.build_remaining > 0) {
            construction_unit = slot.unit_index;
            break;
        }
    require(construction_unit != 0, "the nanolathe fixture has no unfinished solar collector");
    if (!live_renderer_active())
        orbit();
    effects_enabled(true);
    save_picture("construction-early.png", snapshot());
    constexpr std::array progress_remaining{0.5F, 0.1F};
    constexpr std::array progress_names{"construction-middle.png", "construction-late.png"};
    constexpr int construction_finish_ticks = 3600;
    std::size_t stage{};
    for (int frame = 0; frame < construction_finish_ticks && stage < progress_remaining.size();
         ++frame) {
        auto& player = match_->world().players[match_local_player_];
        player.metal = player.metal_cap;
        player.energy = player.energy_cap;
        step_match_simulation();
        require(match_->fault() == nullptr, "staged construction raised a simulation fault");
        if (match_->world().slots[construction_unit].record.build_remaining <=
            progress_remaining[stage]) {
            save_picture(progress_names[stage], snapshot());
            ++stage;
        }
    }
    require(stage == progress_remaining.size(), "construction did not reach its detailed stage");

    // A factory's unfinished product is attached to its build piece. Its
    // geometry must reach the live scene independently of the factory image.
    const auto lab_type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, "ARMLAB");
    const auto kbot_type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, "ARMCK");
    require(lab_type != 0 && kbot_type != 0, "factory fixture lacks ARMLAB or ARMCK");
    auto* lab = place_finished_structure(lab_type, commander);
    require(lab != nullptr, "could not place the factory construction fixture");
    const uint16_t factory = lab->unit_index;
    match_->queue_factory_build(factory, kbot_type, 1);
    uint16_t factory_subject = 0;
    constexpr int factory_start_ticks = 1200;
    for (int frame = 0; frame < factory_start_ticks && !factory_subject; ++frame) {
        auto& player = match_->world().players[match_local_player_];
        player.metal = player.metal_cap;
        player.energy = player.energy_cap;
        step_match_simulation();
        require(match_->fault() == nullptr, "factory construction raised a simulation fault");
        for (const auto& slot : match_->world().slots)
            if (slot.unit && slot.record.type_index == kbot_type &&
                slot.record.attach_parent != 0 && slot.record.build_remaining > 0.1F &&
                slot.record.build_remaining < 0.95F)
                factory_subject = slot.unit_index;
    }
    require(factory_subject != 0, "factory did not begin its attached construction Kbot");
    center_camera_on_unit(factory);
    if (!live_renderer_active())
        orbit();
    save_picture("factory-nanoframe.png", snapshot());
    const auto& factory_draws = match_models().draws.models;
    const auto subject_draw =
        std::find_if(factory_draws.begin(), factory_draws.end(), [&](const ModelDraw& draw) {
            return draw.model.unit && draw.model.unit->id == factory_subject &&
                   draw.plan.linked.drawn;
        });
    require(
        subject_draw != factory_draws.end(),
        "live model plans omitted the factory's attached nanoframe"
    );
    require(
        subject_draw->shown_position.has_value() && !subject_draw->shown_vertices.empty(),
        "factory nanoframe omitted its prepared attachment pose"
    );
    require(
        subject_draw->model.unit->build_remaining > 0.0F &&
            subject_draw->model.unit->build_remaining < 1.0F,
        "factory nanoframe lost its actual construction progress"
    );
    require(
        live_unit_bounds(factory_subject).has_value(),
        "factory nanoframe produced no projected live geometry"
    );
    center_camera_on_unit(commander);

    // Fire the commander's real D-gun before spawning combat armies, so its
    // light-table flashes cannot be mistaken for a different weapon's blast.
    match_->stop_orders(commander);
    const auto gun_position = match_->world().record.units[commander].position;
    const int32_t toward_center =
        (gun_position.x >> 16) < static_cast<int32_t>(selected_tnt_->tile_width * 16U) ? 1 : -1;
    const oa::sim::ground_orders::Point gun_target{
        gun_position.x + toward_center * 120 * 65536, gun_position.y, gun_position.z
    };
    (void)match_->issue_attack_special(commander, gun_target, false);
    const auto gun_definition = match_->world().slots[commander].record.weapons[2].def;
    bool gun_fired{};
    bool gun_flash{};
    constexpr int gun_start_ticks = 300;
    for (int frame = 0; frame < gun_start_ticks && !gun_flash; ++frame) {
        auto& player = match_->world().players[match_local_player_];
        player.energy = player.energy_cap;
        step_match_simulation();
        require(match_->fault() == nullptr, "D-gun rendering raised a simulation fault");
        const auto& world = match_->state();
        for (int32_t shot = 0; shot < world.game.projectile_count; ++shot)
            gun_fired = gun_fired ||
                        (world.projectiles[shot].def == gun_definition &&
                         world.projectiles[shot].source == oa::oa_unit_ref_from_slot(commander));
        if (!gun_fired)
            continue;
        const auto picture = snapshot();
        const auto& models = match_models();
        WorldDrawList flashes;
        for (const auto& draw : models.draws.draws)
            if (draw.kind == WorldDrawKind::lit_sprite) {
                flashes.draws.push_back(
                    {WorldDrawKind::lit_sprite, static_cast<uint32_t>(flashes.sprites.size())}
                );
                flashes.sprites.push_back(models.draws.sprites[draw.index]);
            }
        if (flashes.sprites.empty())
            continue;
        renderer_live_scene::Inputs inputs;
        inputs.map = &*selected_tnt_;
        inputs.palette = &match_palette_;
        inputs.draws = &flashes;
        inputs.sight.mapping = false;
        inputs.sight.line_of_sight = false;
        inputs.camera_x = models.renderer.camera_x;
        inputs.camera_y = models.renderer.camera_y;
        inputs.left = inputs.top = std::numeric_limits<int32_t>::max();
        inputs.right = inputs.bottom = std::numeric_limits<int32_t>::min();
        for (const auto& sprite : flashes.sprites) {
            const auto& frame = *sprite.frame;
            const int32_t left = inputs.camera_x + sprite.place.x - frame.origin_x;
            const int32_t top = inputs.camera_y + sprite.place.y - frame.origin_y;
            inputs.left = std::min(inputs.left, left);
            inputs.top = std::min(inputs.top, top);
            inputs.right = std::max(inputs.right, left + static_cast<int32_t>(frame.width));
            inputs.bottom = std::max(inputs.bottom, top + static_cast<int32_t>(frame.height));
        }
        renderer_live_scene::Scene flash_scene;
        std::string error;
        require(renderer_live_scene::build(inputs, flash_scene, error), error.c_str());
        require(
            flash_scene.vertices.size() > flash_scene.terrain_vertex_count,
            "D-gun flash emitted no terrain geometry"
        );
        const auto height_limits = std::minmax_element(
            selected_tnt_->attributes.begin(),
            selected_tnt_->attributes.end(),
            [](const auto& a, const auto& b) { return a.height < b.height; }
        );
        for (std::size_t vertex = flash_scene.terrain_vertex_count;
             vertex < flash_scene.vertices.size();
             ++vertex) {
            const float height = flash_scene.vertices[vertex].height;
            require(
                height >= height_limits.first->height &&
                    height <= height_limits.second->height + 0.1F,
                "D-gun light mask rose above or clipped beneath the terrain"
            );
        }
        save_picture("dgun-ground-flash.png", picture);
        gun_flash = true;
    }
    require(gun_fired && gun_flash, "the commander's D-gun produced no captured ground flash");
    match_->stop_orders(commander);

    // The installed units fire real weapons; record their live light sources
    // while the match advances without rendering every intervening tick.
    spawn_combat_armies(8);
    constexpr int combat_start_ticks = 300;
    bool projectile_light{};
    bool muzzle_light{};
    bool impact_light{};
    for (int frame = 0;
         frame < combat_start_ticks && !(projectile_light && muzzle_light && impact_light);
         ++frame) {
        step_match_simulation();
        require(match_->fault() == nullptr, "weapon effects raised a simulation fault");
        const auto sources = collect_match_effect_sources();
        bool projectile_now{};
        bool impact_now{};
        for (const auto& source : sources) {
            projectile_now =
                projectile_now || source.origin == renderer_effect_sources::Origin::projectile;
            impact_now = impact_now || source.origin == renderer_effect_sources::Origin::impact;
            muzzle_light = muzzle_light || source.origin == renderer_effect_sources::Origin::muzzle;
        }
        if (projectile_now && !projectile_light) {
            projectile_light = true;
            center_camera_on_unit(commander);
            inspect_effect_views("weapon-effects", renderer_effect_sources::Kind::weapon);
        }
        if (impact_now && !impact_light) {
            impact_light = true;
            center_camera_on_unit(commander);
            inspect_effect_views("impact-effects", renderer_effect_sources::Kind::weapon);
        }
    }
    require(projectile_light, "the combat fixture produced no projectile lighting");
    require(muzzle_light, "the combat fixture produced no muzzle lighting");
    require(impact_light, "the combat fixture produced no impact lighting");

    // A light final hit leaves a real tank wreck. Its visual heat must be
    // born hot, then cool as simulation time advances.
    const auto tank_type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, "ARMSTUMP");
    require(tank_type != 0, "the wreck fixture lacks ARMSTUMP");
    const auto commander_position = match_->world().record.units[commander].position;
    oa::sim::unit_spawn::Request wreck_request;
    wreck_request.player = match_local_player_;
    wreck_request.type = tank_type;
    wreck_request.finished = true;
    wreck_request.state = kGroundOccupancyState;
    wreck_request.position = {
        static_cast<uint32_t>(commander_position.x + 96 * 65536),
        static_cast<uint32_t>(commander_position.y),
        static_cast<uint32_t>(commander_position.z)
    };
    auto* wreck_unit = match_->create(wreck_request);
    require(wreck_unit != nullptr && wreck_unit->unit != nullptr, "the wreck tank did not spawn");
    wreck_unit->unit->object_present = true;
    wreck_unit->unit->health = 1;
    wreck_unit->unit->previous_health_percent = 0;
    (void)collect_match_effect_sources();
    const auto wreck_count_before = match_->wrecks().size();
    const float wreck_x = static_cast<float>(wreck_request.position[0]) / 65536.0F;
    const float wreck_z = static_cast<float>(wreck_request.position[2]) / 65536.0F;
    const auto is_new_wreck = [&](const renderer_effect_sources::Source& source) {
        constexpr float wreck_position_tolerance = 32.0F;
        return source.kind == renderer_effect_sources::Kind::wreck &&
               std::abs(source.x - wreck_x) <= wreck_position_tolerance &&
               std::abs(source.z - wreck_z) <= wreck_position_tolerance;
    };
    match_->apply_damage_event(*wreck_unit, nullptr, 2, 1, 0);
    constexpr int wreck_start_ticks = 180;
    bool hot_wreck{};
    for (int frame = 0; frame < wreck_start_ticks && !hot_wreck; ++frame) {
        step_match_simulation();
        require(match_->fault() == nullptr, "wreck effects raised a simulation fault");
        const auto sources = collect_match_effect_sources();
        hot_wreck = match_->wrecks().size() > wreck_count_before &&
                    std::any_of(sources.begin(), sources.end(), is_new_wreck);
    }
    require(hot_wreck, "the destroyed tank produced no hot wreck source");
    center_camera_on_unit(commander);
    inspect_effect_views("wreck-effects", renderer_effect_sources::Kind::wreck);
    const auto sources_before_cooling = collect_match_effect_sources();
    const auto hot =
        std::find_if(sources_before_cooling.begin(), sources_before_cooling.end(), is_new_wreck);
    require(hot != sources_before_cooling.end(), "the hot wreck disappeared before cooling");
    const auto tracked_wreck = *hot;
    constexpr int cooling_ticks = 30;
    for (int frame = 0; frame < cooling_ticks; ++frame) {
        step_match_simulation();
        (void)collect_match_effect_sources();
    }
    const auto cooled_sources = collect_match_effect_sources();
    const auto cooled =
        std::find_if(cooled_sources.begin(), cooled_sources.end(), [&](const auto& source) {
            return source.kind == renderer_effect_sources::Kind::wreck &&
                   source.x == tracked_wreck.x && source.z == tracked_wreck.z;
        });
    require(
        cooled != cooled_sources.end() && cooled->glow < tracked_wreck.glow &&
            cooled->heat < tracked_wreck.heat,
        "the same wreck did not cool as simulation time advanced"
    );
    engine_settings_->current = saved_effect_settings;
    reset_live_renderer();
    return_to_skirmish_menu();
    std::cout
        << "live renderer check: actual Skirmish starts in classic view; mouse orbit changes "
           "the battlefield; middle-drag pans; rotated picking, selection and move orders "
           "work; fractional wheel zoom keeps the pointer anchored; Shift queues, contour "
           "overlays, movement interpolation and construction glow render; classic and 3D "
           "weapon, impact, nanolathe and cooling wreck effects obey settings; unlock, "
           "restrict, exact reset and "
           "target resize preserve the frozen frame; live picking and rendering remain fault-free "
           "as the simulation advances, without opening a window\n";
    return 0;
#endif
}

} // namespace oa::app

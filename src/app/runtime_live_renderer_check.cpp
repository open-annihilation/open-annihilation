// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Windowless Skirmish coverage of the live camera, rendered world, command
// picking and orders, camera reset, render target resizing and advancing simulation.
#include "oa/app/runtime.hpp"
#include "engine_settings_state.hpp"
#include "full_presentation.hpp"
#include "match_models.hpp"
#include "device_state.hpp"
#include "renderer_effect_sources.hpp"
#include "renderer_live_scene.hpp"
#include "renderer_live_camera.hpp"
#include "oa/sim/unit_movement/terrain.hpp"
#include "oa/present/model/shadow_fade.hpp"
#include "renderer_water.hpp"
#include "presentation_interpolation.hpp"
#include "oa/formats/png.hpp"
#include "oa/data/defs/layout.hpp"
#include "oa/core/weapon_def.h"
#include "oa/sim/messages.hpp"
#include "oa/ui/hud/health_bar.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
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
/// Exit code where SDL makes no graphics device the live renderer can draw with.
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
    // SDL's GPU backend needs the native video driver, but no window or
    // visible renderer is created by this check.
    require(SDL_InitSubSystem(SDL_INIT_VIDEO), "could not initialize windowless video");
    require(sdl_.window == nullptr, "the check must not own a visible window");
    options_.experimental_3d = false;
    // The rotated view draws on Full, where each of the four effects is on
    // at its own default; the check's named preferences file is Off, which
    // reads them as off.
    require(engine_settings_ != nullptr, "the check has no graphics settings");
    {
        using oa::ui::engine_settings::HardwareAcceleration;
        for (const auto& slider : oa::ui::engine_settings::effect_sliders)
            if (slider.follows_acceleration)
                engine_settings_->current.*slider.percent =
                    oa::ui::engine_settings::default_effect_intensity(
                        slider.percent, HardwareAcceleration::full
                    );
    }
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
    // A key goes in as the event loop takes it: with the place a keyboard
    // sends, fed to the action dispatcher first, whose actions the live
    // renderer answers, and then let go.
    const auto key = [&](SDL_Keycode code, SDL_Keymod modifiers = SDL_KMOD_NONE) {
        SDL_Event event{};
        event.type = SDL_EVENT_KEY_DOWN;
        event.key.key = code;
        event.key.scancode = SDL_GetScancodeFromKey(code, nullptr);
        event.key.down = true;
        event.key.mod = modifiers;
        std::ignore = feed_input_actions(event);
        const bool taken = live_renderer_take_event(event);
        SDL_Event release = event;
        release.type = SDL_EVENT_KEY_UP;
        release.key.down = false;
        std::ignore = feed_input_actions(release);
        return taken;
    };
    const auto orbit = [&](uint8_t button = SDL_BUTTON_MIDDLE, float travel = orbit_travel) {
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
        event.motion.x = x + travel;
        event.motion.y = y + travel;
        event.motion.xrel = travel;
        event.motion.yrel = travel;
        require(live_renderer_take_event(event), "orbit motion was not consumed");
        event = {};
        event.type = SDL_EVENT_MOUSE_BUTTON_UP;
        event.button.button = button;
        event.button.x = x + travel;
        event.button.y = y + travel;
        require(live_renderer_take_event(event), "orbit release was not consumed");
        require(live_renderer_active(), "orbit left the camera at classic orientation");
    };

    require(!live_renderer_active(), "Skirmish did not start at classic orientation");
    // Below Full no rotated view is drawn: Alt, its drags and its keys stay
    // the game's, with no status line in their place.
    {
        require(!full_presentation(), "the check's preferences do not keep the match below Full");
        const bool was_headless = options_.headless_check;
        options_.headless_check = false;
        const auto saved_modifiers = SDL_GetModState();
        SDL_SetModState(SDL_KMOD_ALT);
        bool taken = false;
        for (const uint8_t button : {uint8_t{SDL_BUTTON_LEFT}, uint8_t{SDL_BUTTON_MIDDLE}}) {
            SDL_Event press{};
            press.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
            press.button.button = button;
            press.button.down = true;
            press.button.x = static_cast<float>(match_layout_.left + 32);
            press.button.y = static_cast<float>(match_layout_.top + 32);
            taken = live_renderer_take_event(press) || taken;
            SDL_Event motion{};
            motion.type = SDL_EVENT_MOUSE_MOTION;
            motion.motion.state = SDL_BUTTON_MASK(button);
            motion.motion.x = press.button.x + orbit_travel;
            motion.motion.y = press.button.y + orbit_travel;
            motion.motion.xrel = orbit_travel;
            motion.motion.yrel = orbit_travel;
            taken = live_renderer_take_event(motion) || taken;
            SDL_Event release = press;
            release.type = SDL_EVENT_MOUSE_BUTTON_UP;
            release.button.down = false;
            taken = live_renderer_take_event(release) || taken;
        }
        SDL_SetModState(saved_modifiers);
        taken = key(SDLK_R, SDL_KMOD_ALT) || key(SDLK_U, SDL_KMOD_ALT) || taken;
        options_.headless_check = was_headless;
        require(!taken, "below Full the camera took Alt input from the game");
        require(status_ == initial_status, "below Full the camera answered with a status line");
        require(!live_renderer_active(), "below Full an Alt+drag turned the camera");
    }
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
    // A rotated box selection speaks and visits for 'n' as a classic one does.
    oa::sim::selection::clear_cycle_marks(match_->state());
    for (std::size_t pump = 0; pump < oa::audio::game_audio::AnnouncementQueue::capacity; ++pump)
        (void)offline_services_.pump_announcements();
    box_select_units(
        commander_bounds->x1 - 4,
        commander_bounds->y1 - 4,
        commander_bounds->x2 + 4,
        commander_bounds->y2 + 4,
        false
    );
    require(selected_match_unit_ == commander, "rotated drag box did not select the commander");
    {
        std::size_t select_lines = 0;
        for (std::size_t pump = 0; pump < oa::audio::game_audio::AnnouncementQueue::capacity;
             ++pump)
            for (const auto& event : offline_services_.pump_announcements())
                select_lines += event.unit_index == commander &&
                                        event.category ==
                                            oa::audio::game_audio::UnitAnnouncementCategory::select
                                    ? 1
                                    : 0;
        require(select_lines == 1, "the rotated drag box's commander did not speak its line once");
        require(
            (match_->world().slots[commander].record.flags & OA_UNIT_FLAG_CYCLE_VISITED) != 0,
            "the rotated drag box did not visit the commander for 'n'"
        );
        require(status_ == "1 selected", "the rotated drag box did not report its selection");
    }
    // A cloaked unit is blended at half its opacity, as the classic view
    // blends it, over the opaque scene, and can still be picked.
    {
        clear_local_selection();
        const auto solid = snapshot();
        auto& record = match_->world().record.units[commander];
        const uint8_t kept_state = record.state_flags;
        record.state_flags = static_cast<uint8_t>(record.state_flags | OA_UNIT_STATE_CLOAKED);
        const auto cloaked = snapshot();
        const auto cloaked_bounds = live_unit_bounds(commander);
        const auto cloaked_pick = live_unit_under((*commander_pixel)[0], (*commander_pixel)[1]);
        record.state_flags = kept_state;
        save_picture("cloaked-commander.png", cloaked);
        require(cloaked.rgb != solid.rgb, "a cloaked unit drew as it does uncloaked");
        require(cloaked_bounds.has_value(), "a cloaked unit lost its projected bounds");
        require(cloaked_pick == commander, "a cloaked unit could not be picked");
        // Only the unit's own pixels change.
        bool outside = false;
        for (uint32_t y = 0; y < cloaked.height && !outside; ++y)
            for (uint32_t x = 0; x < cloaked.width && !outside; ++x) {
                const std::size_t at = (static_cast<std::size_t>(y) * cloaked.width + x) * 3;
                if (std::equal(
                        cloaked.rgb.begin() + static_cast<std::ptrdiff_t>(at),
                        cloaked.rgb.begin() + static_cast<std::ptrdiff_t>(at + 3),
                        solid.rgb.begin() + static_cast<std::ptrdiff_t>(at)
                    ))
                    continue;
                const int32_t frame_x = static_cast<int32_t>(x) + match_layout_.left;
                const int32_t frame_y = static_cast<int32_t>(y) + match_layout_.top;
                outside = frame_x < cloaked_bounds->x1 - 1 || frame_x > cloaked_bounds->x2 + 1 ||
                          frame_y < cloaked_bounds->y1 - 1 || frame_y > cloaked_bounds->y2 + 1;
            }
        require(!outside, "cloaking a unit changed the picture away from it");
        select_match_unit((*commander_pixel)[0], (*commander_pixel)[1], 1);
        require(selected_match_unit_ == commander, "the commander could not be selected again");
    }
    std::optional<oa::sim::ground_orders::Point> destination;
    std::optional<oa::sim::ground_orders::Point> destination_hit;
    std::array<float, 2> destination_pixel{};
    for (int offset = 80; offset <= 240 && !destination; offset += 40) {
        for (const int sign : {-1, 1}) {
            const float x = (*commander_pixel)[0] + sign * offset;
            const float y = (*commander_pixel)[1] + 48;
            if (live_unit_under(x, y) != 0)
                continue;
            if (const auto ground = live_ground_point(x, y)) {
                destination = ground;
                destination_hit = live_ground_hit(x, y);
                destination_pixel = {x, y};
                break;
            }
        }
    }
    require(
        destination.has_value() && destination_hit.has_value(),
        "no terrain destination was visible in the rotated view"
    );
    const auto projected_ground = live_project_world(
        (*destination_hit)[0] / 65536.0F,
        (*destination_hit)[1] / 65536.0F,
        (*destination_hit)[2] / 65536.0F
    );
    require(
        std::abs(projected_ground.x - destination_pixel[0]) <= 1 &&
            std::abs(projected_ground.y - destination_pixel[1]) <= 1,
        "terrain ray intersection did not project back to the order click"
    );
    // The order takes the hit's whole map pixel at the simulation's terrain
    // height, as a classic pick does, never the mesh's height between cells.
    {
        const int32_t pixel_x = (*destination_hit)[0] >> 16;
        const int32_t pixel_z = (*destination_hit)[2] >> 16;
        const oa::sim::unit_movement::Terrain terrain(*selected_tnt_);
        const int32_t height = std::max<int32_t>(
            terrain.sea_level(), terrain.height(pixel_x * 65536, pixel_z * 65536)
        );
        require(
            (*destination)[0] == pixel_x * 65536 && (*destination)[2] == pixel_z * 65536 &&
                (*destination)[1] == height * 65536,
            "the rotated ground pick was not a whole map pixel at the terrain's height"
        );
    }
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
    // The failed rotated frame is drawn once, classically: the extension's
    // HUD is asked for once, and the frame is the classic frame that follows.
    int hud_draws = 0;
    const Extension saved_extension = extension_;
    extension_ = {};
    extension_.context = &hud_draws;
    extension_.draw_match_hud = [](void* context, Runtime&) { ++*static_cast<int*>(context); };
    const auto fallback = snapshot();
    extension_ = saved_extension;
    require(
        live_renderer_failed() && !live_renderer_active(),
        "GPU rejection did not release the failed camera to classic controls"
    );
    require(hud_draws == 1, "the failed rotated frame was drawn more than once");
    require(
        snapshot().rgb == fallback.rgb,
        "the failed rotated frame did not fall back to a classic one"
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
    // The camera unlocked and as low as it goes, zoomed out as far as the
    // wheel goes over the middle of the map: the frame is drawn from that
    // camera, its angle, place and zoom, and the view never goes back, to
    // classic or anywhere else.
    {
        constexpr float low_yaw = 30.0F;
        constexpr float low_pitch = -53.0F;
        match_layout_ = lay_out_match(wide_width, wide_height);
        reset_live_renderer();
        require(turn_live_camera(low_yaw, low_pitch), "the low unlocked camera did not turn");
        SDL_Event out{};
        out.type = SDL_EVENT_MOUSE_WHEEL;
        out.wheel.y = -100.0F;
        out.wheel.mouse_x =
            static_cast<float>(match_layout_.left + match_layout_.battlefield_width() / 2);
        out.wheel.mouse_y =
            static_cast<float>(match_layout_.top + match_layout_.battlefield_height() / 2);
        require(
            live_renderer_take_event(out),
            "the low unlocked camera did not take the zoom-out gesture"
        );
        const float low_zoom = match_zoom_;
        place_match_view(
            selected_tnt_->attribute_width * map_cell / 2 -
                match_layout_.battlefield_width() / (2 * low_zoom),
            selected_tnt_->attribute_height * map_cell / 2 -
                match_layout_.battlefield_height() / (2 * low_zoom)
        );
        const auto low_view = match_view_place();
        snapshot();
        require(
            live_renderer_active() && !live_renderer_failed(),
            "the low view of the whole map went back to the classic view"
        );
        require(
            std::abs(match_zoom_ - least_match_zoom()) < 0.001F && match_zoom_ == low_zoom &&
                match_view_place() == low_view,
            "the low view of the whole map did not draw where it was zoomed out to"
        );
        require(live_renderer_vertex_count() != 0, "the low view of the whole map drew nothing");
        match_layout_ = recovery_layout;
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
    for (const auto& slider : oa::ui::engine_settings::effect_sliders)
        require(
            oa::ui::engine_settings::default_effect_intensity(
                slider.percent, oa::ui::engine_settings::HardwareAcceleration::full
            ) != 0,
            "an effect is not on by default with Full acceleration"
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
            // Light alone only adds to the battlefield, which Full's effects
            // pass does without reading the world back.
            require(
                !match_effects_distort(),
                (effect_context + " counted light alone as heat distortion").c_str()
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
                require(
                    match_effects_distort(),
                    (effect_context + " did not count its heat distortion").c_str()
                );
                const auto shimmer = snapshot();
                require(
                    shimmer.rgb != lit.rgb,
                    (effect_context + " heat distortion did not change the battlefield").c_str()
                );
                require(
                    snapshot().rgb == shimmer.rgb, "paused heat distortion changed between frames"
                );
                save_picture((stem + "-heat.png").c_str(), shimmer);
            } else {
                // A nanolathe is never hot: its glow alone reaches the view
                // whatever Heat distortion is set to.
                settings.heat_distortion = 100;
                require(
                    !match_effects_distort(),
                    (effect_context + " counted the nanolathe glow as heat distortion").c_str()
                );
                settings.heat_distortion = 0;
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
            const auto& flash_frame = *sprite.frame;
            const int32_t left = inputs.camera_x + sprite.place.x - flash_frame.origin_x;
            const int32_t top = inputs.camera_y + sprite.place.y - flash_frame.origin_y;
            inputs.left = std::min(inputs.left, left);
            inputs.top = std::min(inputs.top, top);
            inputs.right = std::max(inputs.right, left + static_cast<int32_t>(flash_frame.width));
            inputs.bottom = std::max(inputs.bottom, top + static_cast<int32_t>(flash_frame.height));
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
    // Installed coast terrain exercises water boundaries and actual naval models.
    skirmish_settings_.map_name = "Coast to Coast";
    require(select_map("Coast to Coast"), "the water fixture map is missing");
    state_.player_count = 2;
    exercise_click(skirmish::resource_name(skirmish::Button::start));
    require(match_ && selected_tnt_, "the water Skirmish did not start");
    enter_console_check_line("+nowisee");
    clear_local_selection();
    const auto& water_map = *selected_tnt_;
    constexpr int cell_edge = 16;
    constexpr int minimum_depth = 32;
    constexpr int shore_distance_cells = 16;
    constexpr int submarine_offset_cells = 6;
    int water_x = -1;
    int water_z = -1;
    const auto sea = static_cast<int>(water_map.sea_level);
    for (int water_row = shore_distance_cells;
         water_x < 0 &&
         water_row + shore_distance_cells < static_cast<int>(water_map.attribute_height);
         ++water_row) {
        for (int water_column = shore_distance_cells;
             water_column + shore_distance_cells < static_cast<int>(water_map.attribute_width);
             ++water_column) {
            const auto height_at = [&](int x, int z) {
                return static_cast<int>(
                    water_map
                        .attributes[static_cast<std::size_t>(z) * water_map.attribute_width + x]
                        .height
                );
            };
            if (height_at(water_column, water_row) > sea - minimum_depth ||
                height_at(water_column + submarine_offset_cells, water_row) > sea - minimum_depth)
                continue;
            if (height_at(water_column + shore_distance_cells, water_row) < sea &&
                height_at(water_column - shore_distance_cells, water_row) < sea &&
                height_at(water_column, water_row + shore_distance_cells) < sea &&
                height_at(water_column, water_row - shore_distance_cells) < sea)
                continue;
            water_x = water_column * cell_edge;
            water_z = water_row * cell_edge;
            break;
        }
    }
    require(water_x >= 0, "the installed map has no deep water near a shore");
    const auto place_naval = [&](const char* name, int x_offset, int height, bool enemy = false) {
        oa::sim::unit_spawn::Request request;
        request.player = enemy ? (match_local_player_ == 0 ? 1 : 0) : match_local_player_;
        request.type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, name);
        require(request.type != 0, "the water fixture naval type is missing");
        request.finished = true;
        request.state = kGroundOccupancyState;
        request.position = {
            static_cast<uint32_t>((water_x + x_offset) * 65536),
            static_cast<uint32_t>(height * 65536),
            static_cast<uint32_t>(water_z * 65536)
        };
        auto* placed = match_->create(request);
        require(placed && placed->unit, "a naval fixture unit did not spawn");
        placed->unit->object_present = true;
        return placed->unit_index;
    };
    const auto battleship = place_naval("ARMBATS", 0, sea);
    const auto submarine =
        place_naval("ARMSUB", submarine_offset_cells * cell_edge, sea - minimum_depth / 2);
    require(
        match_->world().record.units[submarine].position.y < sea * 65536,
        "the submarine fixture is not submerged"
    );
    match_layout_ = lay_out_match(1280, 800);
    match_zoom_ = match_zoom_target_ = 1.0F;
    center_camera_on_unit(battleship);
    orbit();
    const auto water_settings_saved = engine_settings_->current;
    const auto water_level = [&](uint32_t level) {
        auto& settings = engine_settings_->current;
        settings.water_ripples = settings.coastal_waves = settings.underwater_shimmer =
            settings.naval_reflections = level;
    };
    water_level(0);
    const auto water_plain = snapshot();
    const auto* naval_draws = drawn_match_models();
    require(naval_draws != nullptr, "the water fixture has no world draws");
    const auto submerged_draw = std::find_if(
        naval_draws->draws.models.begin(), naval_draws->draws.models.end(), [&](const auto& draw) {
            return draw.model.unit && draw.model.unit->id == submarine;
        }
    );
    require(submerged_draw != naval_draws->draws.models.end(), "the submarine is not drawn");
    const auto sub_position =
        submerged_draw->shown_position.value_or(submerged_draw->model.unit->position);
    int32_t sub_bottom = std::numeric_limits<int32_t>::max();
    int32_t sub_top = std::numeric_limits<int32_t>::min();
    std::size_t sub_pieces{};
    for (const auto& piece : submerged_draw->shown_vertices) {
        if (!piece.empty())
            ++sub_pieces;
        for (const auto& point : piece) {
            sub_bottom = std::min(sub_bottom, sub_position.y + point.y);
            sub_top = std::max(sub_top, sub_position.y + point.y);
        }
    }
    const auto sub_floor = match_->map_height(sub_position.x, sub_position.z);
    std::cout << "water submarine: pieces=" << sub_pieces << " bottom=" << sub_bottom / 65536.0F
              << " top=" << sub_top / 65536.0F << " seabed=" << sub_floor << " sea=" << sea << '\n';
    require(
        sub_pieces > 1 && sub_top < sea * 65536 && sub_bottom >= sub_floor * 65536,
        "the multi-piece submarine is not wholly between seabed and waterline"
    );
    save_picture("water-disabled.png", water_plain);
    require(snapshot().rgb == water_plain.rgb, "disabled water changed a frozen frame");
    const std::array<const char*, 4> water_names{
        "water-ripples.png", "water-coast.png", "water-submerged.png", "water-reflections.png"
    };
    for (std::size_t effect = 0; effect < water_names.size(); ++effect) {
        water_level(0);
        auto& settings = engine_settings_->current;
        const std::array<uint32_t*, 4> levels{
            &settings.water_ripples,
            &settings.coastal_waves,
            &settings.underwater_shimmer,
            &settings.naval_reflections
        };
        *levels[effect] = 100;
        const auto picture = snapshot();
        require(!live_renderer_failed(), "the water GPU frame failed");
        require(picture.rgb != water_plain.rgb, water_names[effect]);
        require(snapshot().rgb == picture.rgb, "paused water effects changed between frames");
        save_picture(water_names[effect], picture);
    }
    // Timings include CPU preparation and synchronous GPU readback. They are
    // diagnostic costs, not the displayed game's frame rate.
    constexpr std::size_t timing_samples = 7;
    for (const float zoom : {1.0F, 0.25F}) {
        match_zoom_ = match_zoom_target_ = zoom;
        center_camera_on_unit(battleship);
        std::size_t disabled_water_vertices{};
        renderer::Surface disabled_water_picture;
        for (const uint32_t level : {0U, 100U}) {
            water_level(level);
            const auto water_picture = snapshot();
            if (level == 0) {
                disabled_water_vertices = live_renderer_vertex_count();
                disabled_water_picture = water_picture;
            } else if (zoom == 0.25F) {
                require(
                    live_renderer_vertex_count() == disabled_water_vertices,
                    "distant water effects added imperceptible geometry"
                );
                require(
                    water_picture.rgb == disabled_water_picture.rgb,
                    "distant water effects did not fade completely"
                );
            }
            std::array<double, timing_samples> timings{};
            for (auto& elapsed : timings) {
                const auto begin = std::chrono::steady_clock::now();
                snapshot();
                elapsed = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - begin
                )
                              .count();
            }
            std::sort(timings.begin(), timings.end());
            std::cout << "water diagnostic: zoom=" << zoom << " intensity=" << level
                      << " median render/readback ms=" << timings[timing_samples / 2]
                      << " vertices=" << live_renderer_vertex_count() << '\n';
        }
        save_picture(zoom == 1.0F ? "water-all.png" : "water-distant.png", snapshot());
    }
    if (!options_.snapshot.empty()) {
        // Canonical wind changes must not turn or restart stable map water flow.
        // The following motion sequence advances continuously without resetting it.
        const auto capture_flow = [&](const char* backend, const auto& picture) {
            auto& game = match_->state().game;
            const auto saved_vector = game.wind_vector;
            const auto saved_factor = game.wind_factor;
            water_level(100);
            const auto still = picture();
            for (int direction = 0; direction < 4; ++direction) {
                game.wind_vector =
                    direction == 0
                        ? oa::FixedVec3{}
                        : oa::FixedVec3{direction == 2 ? 0 : 10000, 0, direction == 2 ? -10000 : 0};
                game.wind_factor = direction == 0 ? 0 : (direction == 3 ? 1.0F : 0.5F);
                require(
                    picture().rgb == still.rgb,
                    "canonical wind changed stable map water flow at a frozen tick"
                );
            }
            game.wind_vector = saved_vector;
            game.wind_factor = saved_factor;
            save_picture((std::string(backend) + "-flow-start.png").c_str(), still);
            for (int frame = 0; frame < 60; ++frame) {
                for (int tick = 0; tick < 2; ++tick)
                    step_match_simulation();
                const auto name = std::string(backend) + "-flow-motion-" + (frame < 10 ? "0" : "") +
                                  std::to_string(frame) + ".png";
                save_picture(name.c_str(), picture());
            }
        };
        match_zoom_ = match_zoom_target_ = 1.0F;
        water_level(100);
        constexpr int wreck_offset = -96;
        const auto wreck_floor = match_->map_height(
            static_cast<uint32_t>((water_x + wreck_offset) * 65536),
            static_cast<uint32_t>(water_z * 65536)
        );
        const auto sunk_tank = place_naval("ARMSTUMP", wreck_offset, wreck_floor);
        auto& sunk = match_->world().slots[sunk_tank];
        sunk.unit->health = 1;
        sunk.unit->previous_health_percent = 0;
        const auto water_wreck_count = match_->wrecks().size();
        match_->apply_damage_event(sunk, nullptr, 2, 1, 0);
        constexpr int wreck_wait_ticks = 180;
        for (int elapsed = 0;
             elapsed < wreck_wait_ticks && match_->wrecks().size() == water_wreck_count;
             ++elapsed)
            step_match_simulation();
        require(
            match_->wrecks().size() > water_wreck_count,
            "the underwater capture did not produce a wreck"
        );
        const auto enemy_ship = place_naval("CORROY", 320, sea, true);
        constexpr int shot_offset = 240;
        constexpr int shot_z_offset = -128;
        const oa::sim::ground_orders::Point water_aim{
            (water_x + shot_offset) * 65536, sea * 65536, (water_z + shot_z_offset) * 65536
        };
        require(
            match_->issue_attack_ground(battleship, water_aim, false) != nullptr,
            "the battleship did not accept an attack-water order"
        );
        constexpr int aim_warmup_ticks = 90;
        for (int frame = 0; frame < aim_warmup_ticks; ++frame)
            step_match_simulation();
        bool saw_firing{};
        bool saw_water_hit{};
        bool saw_target_hit{};
        constexpr int animation_frames = 120;
        constexpr int ticks_per_animation_frame = 2;
        for (int frame = 0; frame < animation_frames; ++frame) {
            // Keep the target available for several real volleys in this capture.
            match_->world().slots[enemy_ship].unit->health = 30000;
            if (frame == 45)
                require(
                    match_->issue_attack(battleship, enemy_ship, false),
                    "the battleship did not accept an attack-ship order"
                );
            if (frame == animation_frames / 2)
                orbit();
            for (int step = 0; step < ticks_per_animation_frame; ++step)
                step_match_simulation();
            require(!match_->fault(), "the water animation raised a simulation fault");
            center_camera_on_unit(battleship);
            const auto name = std::string("water-animation-") + (frame < 10 ? "0" : "") +
                              std::to_string(frame) + ".png";
            save_picture(name.c_str(), snapshot());
            for (const auto& disturbance : water_disturbances()) {
                saw_firing |= disturbance.kind == renderer_water::Kind::firing;
                saw_water_hit |= disturbance.kind == renderer_water::Kind::water_impact;
                saw_target_hit |= disturbance.kind == renderer_water::Kind::hit;
            }
        }
        capture_flow("live-metal", snapshot);
        match_zoom_ = match_zoom_target_ = 2.0F;
        center_camera_on_unit(submarine);
        for (int angle = 0; angle < 2; ++angle) {
            const auto prefix = "water-sub-close-" + std::to_string(angle);
            water_level(0);
            save_picture((prefix + "-off.png").c_str(), snapshot());
            water_level(100);
            save_picture((prefix + "-on.png").c_str(), snapshot());
            orbit();
        }
        // Render actual planned models through the production GPU backend with
        // explicit camera matrices, including the exact default orientation.
        // Isolated draw lists make the hull and shadow masks independent of
        // the reflection/shadow algorithms being checked.
        const auto aircraft = place_naval("ARMBRAWL", 0, sea);
        const auto water_commander = place_naval("ARMCOM", 160, sea - 24);
        const auto shipyard = place_naval("ARMSY", 224, sea - 24);
        snapshot();
        const auto& planned = match_models();
        const auto model_for = [&](uint16_t unit) {
            const auto found = std::find_if(
                planned.draws.models.begin(), planned.draws.models.end(), [&](const auto& draw) {
                    return draw.model.unit && draw.model.unit->id == unit;
                }
            );
            require(found != planned.draws.models.end(), "the GPU fixture model is missing");
            return *found;
        };
        const auto boat_model = model_for(battleship);
        const auto aircraft_model = model_for(aircraft);
        auto commander_water_model = model_for(water_commander);
        auto shipyard_water_model = model_for(shipyard);
        const auto half_submerge = [&](auto& model) {
            int32_t low = std::numeric_limits<int32_t>::max();
            int32_t high = std::numeric_limits<int32_t>::min();
            for (const auto& piece : model.shown_vertices)
                for (const auto& vertex : piece) {
                    low = std::min(low, vertex.y);
                    high = std::max(high, vertex.y);
                }
            require(low < high, "the waterline fixture has no vertical model extent");
            auto position = model.shown_position.value_or(model.model.unit->position);
            position.y = sea * 65536 - (low + high) / 2;
            model.shown_position = position;
            match_->world().record.units[model.model.unit->id].position.y = position.y;
            std::cout << "controlled waterline model: unit=" << model.model.unit->id
                      << " below=" << (position.y + low) / 65536.0F
                      << " above=" << (position.y + high) / 65536.0F << " sea=" << sea << '\n';
            return position.y;
        };
        const auto commander_water_height = half_submerge(commander_water_model);
        const auto shipyard_water_height = half_submerge(shipyard_water_model);
        renderer_live_scene::Scene diagnostic_scene;
        renderer_live_gpu::Context diagnostic_gpu;
        constexpr uint32_t diagnostic_width = 800;
        constexpr uint32_t diagnostic_height = 600;
        constexpr float diagnostic_zoom = 2;
        const std::array<renderer_live_camera::State, 3> diagnostic_angles{
            {{}, {18, 12}, {38, -18}}
        };
        // New unit shadows for the diagnostic draws; the aircraft's proof
        // draws its shadow both ways.
        bool diagnostic_new_shadows = true;
        const auto render_diagnostic = [&](WorldDrawList& draws,
                                           const renderer_live_camera::State& camera,
                                           float x,
                                           float height,
                                           float z,
                                           bool reflections,
                                           float shimmer = 0.0F,
                                           bool dry_triangles_only = false) {
            renderer_live_camera::Matrix projection{};
            require(
                renderer_live_camera::projection(
                    camera,
                    {x,
                     height,
                     z,
                     diagnostic_width / 2.0F,
                     diagnostic_height / 2.0F,
                     diagnostic_zoom},
                    projection
                ),
                "the diagnostic camera is invalid"
            );
            renderer_live_scene::Inputs inputs{};
            inputs.map = &water_map;
            inputs.palette = &match_palette_;
            inputs.draws = &draws;
            inputs.graphics_flags = planned.renderer.graphics_flags;
            inputs.shade_table = planned.display.shade;
            inputs.light = {
                planned.renderer.light[0], planned.renderer.light[1], planned.renderer.light[2]
            };
            inputs.light_scale = planned.renderer.light_scale;
            inputs.sea_level = sea;
            inputs.detail_scale = diagnostic_zoom;
            inputs.clip_projection = projection;
            inputs.clip_width = diagnostic_width;
            inputs.clip_height = diagnostic_height;
            inputs.view_direction = {projection[8], projection[9], projection[10]};
            inputs.camera_x = planned.renderer.camera_x;
            inputs.camera_y = planned.renderer.camera_y;
            inputs.sight.mapping = false;
            inputs.sight.line_of_sight = false;
            std::copy_n(
                planned.renderer.team_colors, full::team_colour_players, inputs.team_colors.begin()
            );
            inputs.tick = match_->simulation().tick;
            inputs.water.sea_level = static_cast<float>(sea);
            inputs.water.seconds = inputs.tick / 30.0F;
            inputs.water.reflections = reflections ? 1.0F : 0.0F;
            inputs.water.shimmer = shimmer;
            inputs.new_unit_shadows = diagnostic_new_shadows;
            renderer_live_camera::TerrainBounds bounds{};
            require(
                renderer_live_camera::visible_terrain_bounds(
                    projection, diagnostic_width, diagnostic_height, 32, bounds
                ),
                "the diagnostic terrain bounds are invalid"
            );
            inputs.left = std::max(0, static_cast<int>(std::floor(bounds.min_x)));
            inputs.top = std::max(0, static_cast<int>(std::floor(bounds.min_y)));
            inputs.right = std::min(
                static_cast<int>(water_map.attribute_width) * cell_edge,
                static_cast<int>(std::ceil(bounds.max_x))
            );
            inputs.bottom = std::min(
                static_cast<int>(water_map.attribute_height) * cell_edge,
                static_cast<int>(std::ceil(bounds.max_y))
            );
            std::string error;
            require(renderer_live_scene::build(inputs, diagnostic_scene, error), error.c_str());
            if (dry_triangles_only) {
                std::vector<renderer_live_gpu::Vertex> dry;
                for (std::size_t i = 0; i + 2 < diagnostic_scene.vertices.size(); i += 3)
                    if (diagnostic_scene.vertices[i].height > sea + 1 &&
                        diagnostic_scene.vertices[i + 1].height > sea + 1 &&
                        diagnostic_scene.vertices[i + 2].height > sea + 1)
                        dry.insert(
                            dry.end(),
                            diagnostic_scene.vertices.begin() + i,
                            diagnostic_scene.vertices.begin() + i + 3
                        );
                // Ground at most a step above the sea may leave no triangle
                // dry: the picture is then the empty frame, which one
                // triangle of no area draws, as the card draws no empty list.
                if (dry.empty() && !diagnostic_scene.vertices.empty())
                    dry.assign(3, diagnostic_scene.vertices.front());
                diagnostic_scene.vertices = std::move(dry);
            }
            std::vector<uint8_t> rgba;
            require(
                renderer_live_gpu::render(
                    diagnostic_gpu,
                    diagnostic_scene.vertices,
                    diagnostic_scene.texture_rgba,
                    diagnostic_scene.texture_width,
                    diagnostic_scene.texture_height,
                    diagnostic_scene.texture_revision,
                    diagnostic_width,
                    diagnostic_height,
                    projection,
                    rgba,
                    error,
                    1,
                    &diagnostic_scene.atlas_update,
                    &inputs.water
                ) == renderer_live_gpu::Result::success,
                error.c_str()
            );
            renderer::Surface picture;
            picture.width = diagnostic_width;
            picture.height = diagnostic_height;
            picture.rgb.resize(static_cast<std::size_t>(diagnostic_width) * diagnostic_height * 3);
            for (std::size_t pixel = 0; pixel < picture.rgb.size() / 3; ++pixel)
                std::copy_n(rgba.data() + pixel * 4, 3, picture.rgb.data() + pixel * 3);
            return picture;
        };
        const auto differs = [](const renderer::Surface& a,
                                const renderer::Surface& b,
                                std::size_t pixel) {
            return !std::equal(
                a.rgb.begin() + pixel * 3, a.rgb.begin() + pixel * 3 + 3, b.rgb.begin() + pixel * 3
            );
        };
        const auto boat_position =
            boat_model.shown_position.value_or(boat_model.model.unit->position);
        for (std::size_t angle = 0; angle < diagnostic_angles.size(); ++angle) {
            WorldDrawList draws;
            draws.shadow_level = 0;
            const auto empty = render_diagnostic(
                draws,
                diagnostic_angles[angle],
                boat_position.x / 65536.0F,
                static_cast<float>(sea),
                boat_position.z / 65536.0F,
                false
            );
            draws.models.push_back(boat_model);
            const auto plain = render_diagnostic(
                draws,
                diagnostic_angles[angle],
                boat_position.x / 65536.0F,
                static_cast<float>(sea),
                boat_position.z / 65536.0F,
                false
            );
            const auto reflected = render_diagnostic(
                draws,
                diagnostic_angles[angle],
                boat_position.x / 65536.0F,
                static_cast<float>(sea),
                boat_position.z / 65536.0F,
                true
            );
            std::vector<bool> hull(diagnostic_width * diagnostic_height);
            for (std::size_t pixel = 0; pixel < hull.size(); ++pixel)
                if (differs(plain, empty, pixel)) {
                    const int x = static_cast<int>(pixel % diagnostic_width);
                    const int y = static_cast<int>(pixel / diagnostic_width);
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx)
                            if (x + dx >= 0 && y + dy >= 0 &&
                                x + dx < static_cast<int>(diagnostic_width) &&
                                y + dy < static_cast<int>(diagnostic_height))
                                hull[(y + dy) * diagnostic_width + x + dx] = true;
                }
            std::size_t reflection_outside_hull{};
            for (std::size_t pixel = 0; pixel < hull.size(); ++pixel)
                reflection_outside_hull += !hull[pixel] && differs(reflected, plain, pixel);
            std::cout << "naval reflection proof: angle=" << angle
                      << " changed pixels outside independent hull=" << reflection_outside_hull
                      << '\n';
            const auto prefix = "naval-reflection-" + std::to_string(angle);
            save_picture((prefix + "-off.png").c_str(), plain);
            save_picture((prefix + "-on.png").c_str(), reflected);
            require(
                reflection_outside_hull > 8, "the naval reflection stays hidden beneath its hull"
            );
        }
        int dry_x = water_x;
        int dry_z = water_z;
        bool found_dry{};
        for (int dry_row = 24;
             !found_dry && dry_row + 24 < static_cast<int>(water_map.attribute_height);
             ++dry_row)
            for (int dry_column = 24; dry_column + 24 < static_cast<int>(water_map.attribute_width);
                 ++dry_column)
                if (water_map
                        .attributes
                            [static_cast<std::size_t>(dry_row) * water_map.attribute_width +
                             dry_column]
                        .height > sea + 8) {
                    dry_x = dry_column * cell_edge;
                    dry_z = dry_row * cell_edge;
                    found_dry = true;
                    break;
                }
        require(found_dry, "the aircraft fixture lacks dry ground");
        // Frozen simulation and continuous camera movement straddle the former
        // reflection cutoff at abs(view_direction.y)==0.2.
        for (int frame = 0; frame < 24; ++frame) {
            const renderer_live_camera::State camera{-150, -49.0F - frame * 0.19F, true};
            WorldDrawList draws;
            draws.shadow_level = 0;
            draws.models.push_back(boat_model);
            const auto off = render_diagnostic(
                draws,
                camera,
                boat_position.x / 65536.0F,
                float(sea),
                boat_position.z / 65536.0F,
                false
            );
            const auto on = render_diagnostic(
                draws,
                camera,
                boat_position.x / 65536.0F,
                float(sea),
                boat_position.z / 65536.0F,
                true
            );
            uint64_t energy{};
            for (std::size_t i = 0; i < on.rgb.size(); ++i)
                energy += std::abs(int(on.rgb[i]) - int(off.rgb[i]));
            std::cout << "reflection sweep: frame=" << frame << " pitch=" << camera.pitch_degrees
                      << " energy=" << energy << '\n';
            const auto prefix =
                std::string("reflection-sweep-") + (frame < 10 ? "0" : "") + std::to_string(frame);
            save_picture((prefix + "-off.png").c_str(), off);
            save_picture((prefix + "-on.png").c_str(), on);
        }

        for (const bool yard : {false, true}) {
            const auto& model = yard ? shipyard_water_model : commander_water_model;
            const auto water_position = *model.shown_position;
            for (std::size_t angle = 0; angle < diagnostic_angles.size(); ++angle) {
                WorldDrawList draws;
                draws.shadow_level = 0;
                const auto empty = render_diagnostic(
                    draws,
                    diagnostic_angles[angle],
                    water_position.x / 65536.0F,
                    float(sea),
                    water_position.z / 65536.0F,
                    false
                );
                draws.models.push_back(model);
                const auto off = render_diagnostic(
                    draws,
                    diagnostic_angles[angle],
                    water_position.x / 65536.0F,
                    float(sea),
                    water_position.z / 65536.0F,
                    false
                );
                const auto on = render_diagnostic(
                    draws,
                    diagnostic_angles[angle],
                    water_position.x / 65536.0F,
                    float(sea),
                    water_position.z / 65536.0F,
                    false,
                    1
                );
                const auto dry_model = render_diagnostic(
                    draws,
                    diagnostic_angles[angle],
                    water_position.x / 65536.0F,
                    float(sea),
                    water_position.z / 65536.0F,
                    false,
                    0,
                    true
                );
                draws.models.clear();
                const auto dry_empty = render_diagnostic(
                    draws,
                    diagnostic_angles[angle],
                    water_position.x / 65536.0F,
                    float(sea),
                    water_position.z / 65536.0F,
                    false,
                    0,
                    true
                );
                std::size_t above{}, above_changed{}, changed{};
                for (std::size_t pixel = 0; pixel < off.rgb.size() / 3; ++pixel)
                    if (differs(empty, off, pixel)) {
                        changed += differs(off, on, pixel);
                        // Only full triangles physically above sea, still visible
                        // in the unfiltered image, form this independent dry mask.
                        if (differs(dry_empty, dry_model, pixel) &&
                            !differs(dry_model, off, pixel)) {
                            ++above;
                            above_changed += differs(off, on, pixel);
                        }
                    }
                std::cout << "waterline proof: " << (yard ? "shipyard" : "commander")
                          << " angle=" << angle << " changed hull=" << changed
                          << " upper hull=" << above << " upper changed=" << above_changed << '\n';
                const auto prefix = std::string("waterline-") +
                                    (yard ? "shipyard-" : "commander-") + std::to_string(angle);
                save_picture((prefix + "-off.png").c_str(), off);
                save_picture((prefix + "-on.png").c_str(), on);
                require(
                    changed > 0 && above > 0 && above_changed == 0,
                    "partial immersion must affect submerged hull and preserve upper hull"
                );
            }
        }

        constexpr std::array<int, 3> flight_heights{32, 112, 224};
        // How far the darkest shading of an aircraft's shadow may differ
        // between heights: its silhouette keeps the vehicles' darkness, and
        // only its edge widens.
        constexpr int darkness_tolerance = 2;
        for (const bool water_receiver : {false, true}) {
            const int x = water_receiver ? water_x : dry_x;
            const int z = water_receiver ? water_z : dry_z;
            const int receiver = std::max(sea, match_->map_height(x * 65536, z * 65536));
            for (std::size_t angle = 0; angle < diagnostic_angles.size(); ++angle) {
                std::size_t previous_area{};
                int first_darkness = -1;
                for (const int altitude : flight_heights) {
                    WorldDrawList draws;
                    draws.shadow_level = 0;
                    draws.models.push_back(aircraft_model);
                    draws.models.back().shown_position =
                        oa::FixedVec3{x * 65536, (receiver + altitude) * 65536, z * 65536};
                    const auto plain = render_diagnostic(
                        draws,
                        diagnostic_angles[angle],
                        static_cast<float>(x),
                        static_cast<float>(receiver),
                        static_cast<float>(z),
                        false
                    );
                    draws.shadow_level = oa::present::model::shadow_full_level;
                    const auto shadowed = render_diagnostic(
                        draws,
                        diagnostic_angles[angle],
                        static_cast<float>(x),
                        static_cast<float>(receiver),
                        static_cast<float>(z),
                        false
                    );
                    std::size_t area{};
                    int darkest{};
                    for (std::size_t pixel = 0; pixel < plain.rgb.size() / 3; ++pixel) {
                        if (!differs(plain, shadowed, pixel))
                            continue;
                        ++area;
                        for (std::size_t channel = 0; channel < 3; ++channel)
                            darkest = std::max(
                                darkest,
                                static_cast<int>(plain.rgb[pixel * 3 + channel]) -
                                    shadowed.rgb[pixel * 3 + channel]
                            );
                    }
                    std::cout << "aircraft shadow proof: water=" << water_receiver
                              << " angle=" << angle << " altitude=" << altitude
                              << " pixels=" << area << " maximum darkening=" << darkest << '\n';
                    const auto prefix = "aircraft-shadow-" +
                                        std::string(water_receiver ? "water-" : "land-") +
                                        std::to_string(angle) + "-" + std::to_string(altitude);
                    save_picture((prefix + "-off.png").c_str(), plain);
                    save_picture((prefix + "-on.png").c_str(), shadowed);
                    require(area > 0, "an aircraft has no visible terrain/water shadow");
                    if (first_darkness < 0)
                        first_darkness = darkest;
                    require(
                        area >= previous_area &&
                            std::abs(darkest - first_darkness) <= darkness_tolerance,
                        "aircraft shadows do not keep their darkness as their edge widens"
                    );
                    previous_area = area;
                    // With New unit shadows off the same aircraft casts the
                    // game's silhouette where the game lays it, on the
                    // ground or the sea bed under it, by the ground its plan
                    // holds, here the ground where it is shown: over land as
                    // dark, and with no soft edge smaller than the soft one;
                    // the lower the aircraft flies, the more of either it
                    // covers.
                    draws.models.back().plan.model.ground =
                        match_->map_height(x * 65536, z * 65536);
                    diagnostic_new_shadows = false;
                    const auto as_the_game = render_diagnostic(
                        draws,
                        diagnostic_angles[angle],
                        static_cast<float>(x),
                        static_cast<float>(receiver),
                        static_cast<float>(z),
                        false
                    );
                    diagnostic_new_shadows = true;
                    std::size_t game_area{};
                    int game_darkest{};
                    for (std::size_t pixel = 0; pixel < plain.rgb.size() / 3; ++pixel) {
                        if (!differs(plain, as_the_game, pixel))
                            continue;
                        ++game_area;
                        for (std::size_t channel = 0; channel < 3; ++channel)
                            game_darkest = std::max(
                                game_darkest,
                                static_cast<int>(plain.rgb[pixel * 3 + channel]) -
                                    as_the_game.rgb[pixel * 3 + channel]
                            );
                    }
                    std::cout << "aircraft shadow proof: water=" << water_receiver
                              << " angle=" << angle << " altitude=" << altitude
                              << " game shadow pixels=" << game_area
                              << " maximum darkening=" << game_darkest << '\n';
                    save_picture((prefix + "-game.png").c_str(), as_the_game);
                    if (!water_receiver)
                        require(
                            game_area > 0 && game_area < area &&
                                std::abs(game_darkest - first_darkness) <= darkness_tolerance,
                            "an aircraft's game shadow is not its plain silhouette at the "
                            "shadows' darkness"
                        );
                }
            }
        }
        // The Brawler belongs only to the isolated aircraft-shadow proof.
        // Relocate it away from the naval showcase before any classic capture.
        match_->world().record.units[aircraft].position.x = 16 * 65536;
        match_->world().record.units[aircraft].position.z = 16 * 65536;
        match_zoom_ = match_zoom_target_ = 1.0F;
        center_camera_on_unit(battleship);
        capture_flow("live-metal-naval", [&] {
            match_->world().record.units[water_commander].position.y = commander_water_height;
            match_->world().record.units[shipyard].position.y = shipyard_water_height;
            return snapshot();
        });
        // Exercise the actual classic Full presenter, including its water
        // hooks and AA resolve, on an SDL software surface with no window.
        require(
            sdl_.renderer == nullptr && sdl_.window == nullptr,
            "the classic Full capture must not borrow a user renderer"
        );
        const auto classic_settings = engine_settings_->current;
        const auto classic_rung = accelerated_.rung;
        const bool was_headless = options_.headless_check;
        auto classic_capture_settings = classic_settings;
        classic_capture_settings.anti_aliasing = oa::ui::engine_settings::AntiAliasing::x2;
        classic_capture_settings.weapon_lighting = 0;
        classic_capture_settings.nanolathe_lighting = 0;
        classic_capture_settings.wreck_glow = 0;
        classic_capture_settings.heat_distortion = 0;
        apply_engine_settings(classic_capture_settings);
        constexpr int classic_width = 1280;
        constexpr int classic_height = 800;
        SDL_Surface* classic_canvas =
            SDL_CreateSurface(classic_width, classic_height, SDL_PIXELFORMAT_XRGB8888);
        require(classic_canvas != nullptr, "the classic Full canvas allocation failed");
        sdl_.renderer = SDL_CreateSoftwareRenderer(classic_canvas);
        require(sdl_.renderer != nullptr, "the windowless classic Full renderer failed");
        const auto release_classic = [&] {
            set_full_presentation(false);
            free_accelerated_presentation();
            destroy_match_layer_textures();
            accelerated_.on = false;
            SDL_DestroyRenderer(sdl_.renderer);
            sdl_.renderer = nullptr;
            SDL_DestroySurface(classic_canvas);
            options_.headless_check = was_headless;
            apply_engine_settings(classic_settings);
        };
        try {
            options_.headless_check = false;
            switch_accelerated_presentation(true, classic_rung);
            set_full_presentation(true);
            reset_live_renderer();
            match_layout_ = lay_out_match(classic_width, classic_height);
            match_zoom_ = match_zoom_target_ = 1.0F;
            center_camera_on_unit(battleship);
            bool dry_colour_fixture = false;
            bool maintain_waterline_fixture = true;
            const auto classic_picture = [&] {
                if (maintain_waterline_fixture) {
                    match_->world().record.units[water_commander].position.y =
                        dry_colour_fixture ? (sea + 64) * 65536 : commander_water_height;
                    match_->world().record.units[shipyard].position.y =
                        dry_colour_fixture ? (sea + 64) * 65536 : shipyard_water_height;
                }
                oa::sim::messages::clear_messages(match_->state().game);
                render_match_surface();
                require(
                    full_presentation() && full_frame_drawn() && !live_renderer_active(),
                    "the actual classic Full draw path was not selected"
                );
                require(
                    present_full_match_layers(false) && full_->drawn,
                    "the actual classic Full card presenter failed or fell back"
                );
                require(
                    full_supersample() == 2, "the classic Full capture did not use requested 2x2 AA"
                );
                const SDL_Rect area{
                    match_layout_.left,
                    match_layout_.top,
                    match_layout_.battlefield_width(),
                    match_layout_.battlefield_height()
                };
                SDL_Surface* readback = SDL_RenderReadPixels(sdl_.renderer, &area);
                require(readback != nullptr, "the classic Full readback failed");
                SDL_Surface* rgb = SDL_ConvertSurface(readback, SDL_PIXELFORMAT_RGB24);
                SDL_DestroySurface(readback);
                require(rgb != nullptr, "the classic Full RGB conversion failed");
                renderer::Surface picture;
                picture.width = static_cast<uint32_t>(rgb->w);
                picture.height = static_cast<uint32_t>(rgb->h);
                picture.rgb.resize(static_cast<std::size_t>(rgb->w) * rgb->h * 3);
                for (int row = 0; row < rgb->h; ++row)
                    std::copy_n(
                        static_cast<const uint8_t*>(rgb->pixels) + row * rgb->pitch,
                        rgb->w * 3,
                        picture.rgb.data() + row * rgb->w * 3
                    );
                SDL_DestroySurface(rgb);
                return picture;
            };
            for (const float zoom : {1.0F, 1.31F}) {
                match_zoom_ = match_zoom_target_ = zoom;
                center_camera_on_unit(battleship);
                water_level(0);
                const auto off = classic_picture();
                const auto prefix = zoom == 1.0F ? "classic-full-zoom1" : "classic-full-zoom131";
                save_picture((std::string(prefix) + "-off.png").c_str(), off);
                for (const bool reflection : {false, true}) {
                    water_level(0);
                    if (reflection)
                        engine_settings_->current.naval_reflections = 100;
                    else
                        engine_settings_->current.water_ripples = 100;
                    const auto on = classic_picture();
                    std::size_t changed{};
                    unsigned maximum{};
                    uint64_t total{};
                    for (std::size_t pixel = 0; pixel < off.rgb.size() / 3; ++pixel) {
                        changed += differs(off, on, pixel);
                        for (std::size_t channel = 0; channel < 3; ++channel) {
                            const auto delta = static_cast<unsigned>(std::abs(
                                int(off.rgb[pixel * 3 + channel]) - int(on.rgb[pixel * 3 + channel])
                            ));
                            maximum = std::max(maximum, delta);
                            total += delta;
                        }
                    }
                    const auto name =
                        std::string(prefix) + (reflection ? "-reflection.png" : "-ripples.png");
                    save_picture(name.c_str(), on);
                    std::cout << "actual classic Full: zoom=" << zoom
                              << " AA=2x2 effect=" << (reflection ? "reflection" : "ripples")
                              << " changed=" << changed << " max=" << maximum << " total=" << total
                              << '\n';
                    require(
                        changed > 0, "the actual classic Full water effect has no visible pixels"
                    );
                    require(
                        classic_picture().rgb == on.rgb,
                        "the classic Full water effect changed at a frozen tick"
                    );
                }
            }
            match_zoom_ = match_zoom_target_ = 1.0F;
            center_camera_on_unit(battleship);
            for (const bool dry : {false, true}) {
                dry_colour_fixture = dry;
                for (const uint32_t effects : {0U, 100U}) {
                    water_level(effects);
                    const auto colour_classic = classic_picture();
                    set_full_presentation(false);
                    options_.headless_check = true;
                    orbit(SDL_BUTTON_MIDDLE, 0.001F);
                    const auto live = snapshot();
                    require(
                        live.width == colour_classic.width && live.height == colour_classic.height,
                        "near-default live/classic comparison dimensions differ"
                    );
                    const auto prefix = std::string("view-colour-") + (dry ? "dry-" : "half-") +
                                        std::to_string(effects);
                    save_picture((prefix + "-classic-full.png").c_str(), colour_classic);
                    save_picture((prefix + "-near-default-live.png").c_str(), live);
                    std::array<uint64_t, 3> classic_sum{}, live_sum{};
                    std::size_t count{};
                    // Stable dry coastal patch in this fixed showcase framing.
                    for (uint32_t y = 200; y < std::min(400U, colour_classic.height); ++y)
                        for (uint32_t x = 10; x < std::min(90U, colour_classic.width); ++x) {
                            const auto pixel = (y * colour_classic.width + x) * 3;
                            for (std::size_t channel = 0; channel < 3; ++channel) {
                                classic_sum[channel] += colour_classic.rgb[pixel + channel];
                                live_sum[channel] += live.rgb[pixel + channel];
                            }
                            ++count;
                        }
                    std::cout << "near-default colour: dry=" << dry << " effects=" << effects;
                    for (std::size_t channel = 0; channel < 3; ++channel)
                        std::cout << " channel" << channel
                                  << " classic=" << double(classic_sum[channel]) / count
                                  << " live=" << double(live_sum[channel]) / count;
                    std::cout << '\n';
                    reset_live_renderer();
                    options_.headless_check = false;
                    set_full_presentation(true);
                }
            }
            dry_colour_fixture = false;
            for (const auto unit : {water_commander, shipyard}) {
                match_zoom_ = match_zoom_target_ = 2.0F;
                center_camera_on_unit(unit);
                water_level(0);
                const auto off = classic_picture();
                engine_settings_->current.underwater_shimmer = 100;
                const auto on = classic_picture();
                const auto prefix = std::string("classic-waterline-") +
                                    (unit == water_commander ? "commander" : "shipyard");
                save_picture((prefix + "-off.png").c_str(), off);
                save_picture((prefix + "-on.png").c_str(), on);
                require(off.rgb != on.rgb, "classic partially submerged models show no shimmer");
            }
            match_zoom_ = match_zoom_target_ = 1.0F;
            center_camera_on_unit(battleship);
            std::vector<int16_t> previous_ripple_delta;
            bool ripple_moved{};
            constexpr int classic_frames = 30;
            for (int frame = 0; frame < classic_frames; ++frame) {
                step_match_simulation();
                step_match_simulation();
                water_level(0);
                const auto off = classic_picture();
                engine_settings_->current.water_ripples = 100;
                const auto on = classic_picture();
                std::vector<int16_t> delta(off.rgb.size());
                for (std::size_t index = 0; index < delta.size(); ++index)
                    delta[index] = int16_t(on.rgb[index]) - int16_t(off.rgb[index]);
                if (!previous_ripple_delta.empty())
                    ripple_moved |= delta != previous_ripple_delta;
                previous_ripple_delta = std::move(delta);
                const auto name = "classic-full-motion-" + std::string(frame < 10 ? "0" : "") +
                                  std::to_string(frame) + ".png";
                save_picture(name.c_str(), on);
            }
            require(ripple_moved, "actual classic Full ripples did not animate as ticks advanced");
            capture_flow("classic-full", classic_picture);
            // Fresh canonical combat separates underwater torpedoes from the
            // earlier battleship surface-shell fixture. No injected shots,
            // damage, altered weapon definitions or target health replenishment.
            maintain_waterline_fixture = false;
            reset_live_renderer();
            options_.headless_check = true;
            return_to_skirmish_menu();
            skirmish_settings_.map_name = "Coast to Coast";
            require(select_map("Coast to Coast"), "the torpedo fixture map is missing");
            state_.player_count = 2;
            exercise_click(skirmish::resource_name(skirmish::Button::start));
            require(match_ && selected_tnt_, "the torpedo Skirmish did not start");
            enter_console_check_line("+nowisee");
            clear_local_selection();
            const auto torpedo_sub = place_naval("ARMSUB", 96, sea - 16);
            const auto torpedo_target = place_naval("CORSUB", 352, sea - 16, true);
            require(
                match_->world().record.units[torpedo_sub].position.y < sea * 65536 &&
                    match_->world().record.units[torpedo_target].position.y < sea * 65536,
                "torpedo combat participants must actually be submerged"
            );
            require(
                match_->issue_attack(torpedo_sub, torpedo_target, false),
                "the actual submarine did not accept its attack order"
            );
            const auto initial_target_health = match_->world().record.units[torpedo_target].health;
            match_layout_ = lay_out_match(classic_width, classic_height);
            match_zoom_ = match_zoom_target_ = 1.5F;
            center_camera_on_unit(torpedo_sub);
            water_level(100);
            engine_settings_->current.weapon_lighting = 100;
            bool torpedo_seen{}, torpedo_hit{}, underwater_impact{}, torpedo_model{};
            bool hit_saved{};
            float torpedo_low = float(sea), torpedo_high = -10000;
            int torpedo_frames{};
            for (int torpedo_tick = 0; torpedo_tick < 480 && torpedo_frames < 90; ++torpedo_tick) {
                step_match_simulation();
                require(!match_->fault(), "actual torpedo combat raised a simulation fault");
                auto& world = match_->world().record;
                for (int32_t shot = 0; shot < world.game.projectile_count; ++shot) {
                    const auto& projectile = world.projectiles[shot];
                    if (projectile.source != oa::oa_unit_ref_from_slot(torpedo_sub))
                        continue;
                    const auto* definition = oa::world_weapon_def(&world, projectile.def);
                    require(
                        definition && (definition->flags & OA_WEAPON_FLAG_WATER_WEAPON) != 0,
                        "the installed submarine emitted a non-water weapon"
                    );
                    const float height = projectile.position.y / 65536.0F;
                    require(height < sea, "the real torpedo trajectory left the water");
                    torpedo_low = std::min(torpedo_low, height);
                    torpedo_high = std::max(torpedo_high, height);
                    torpedo_seen = true;
                }
                torpedo_hit |= world.units[torpedo_target].health < initial_target_health;
                for (const auto& source : collect_match_effect_sources())
                    underwater_impact |= source.origin == renderer_effect_sources::Origin::impact &&
                                         source.height < sea;
                require(
                    water_disturbances().empty(),
                    "actual underwater torpedoes incorrectly emitted surface rings"
                );
                if (!torpedo_seen || torpedo_tick % 2 != 0)
                    continue;
                reset_live_renderer();
                options_.headless_check = false;
                set_full_presentation(true);
                const auto torpedo_classic = classic_picture();
                if (const auto* draws = drawn_match_models())
                    for (const auto& shot : draws->draws.projectiles)
                        torpedo_model |= shot.object != nullptr && shot.prepared != nullptr;
                // Headless mode bypasses Full without destroying its cached
                // targets between paired captures of the same simulation tick.
                options_.headless_check = true;
                orbit();
                const auto live = snapshot();
                const auto number =
                    (torpedo_frames < 10 ? "0" : "") + std::to_string(torpedo_frames);
                save_picture(("torpedo-classic-" + number + ".png").c_str(), torpedo_classic);
                save_picture(("torpedo-live-" + number + ".png").c_str(), live);
                if (torpedo_hit && underwater_impact && !hit_saved) {
                    save_picture("torpedo-hit-classic.png", torpedo_classic);
                    save_picture("torpedo-hit-live.png", live);
                    hit_saved = true;
                }
                ++torpedo_frames;
            }
            std::cout << "actual torpedo proof: frames=" << torpedo_frames
                      << " projectile=" << torpedo_seen << " model=" << torpedo_model
                      << " height=" << torpedo_low << ".." << torpedo_high << " sea=" << sea
                      << " target-damaged=" << torpedo_hit
                      << " underwater-impact=" << underwater_impact
                      << " surface-rings=" << water_disturbances().size() << '\n';
            require(
                torpedo_seen && torpedo_model && torpedo_hit && underwater_impact && hit_saved,
                "the actual torpedo fixture lacks trajectory/model/underwater-hit evidence"
            );
            reset_live_renderer();
            options_.headless_check = false;
            require(sdl_.window == nullptr, "the classic Full fixture created a visible window");
        } catch (...) {
            release_classic();
            throw;
        }
        release_classic();
        std::cout << "water weapon events: firing=" << saw_firing
                  << " water-impact=" << saw_water_hit << " target-hit=" << saw_target_hit << '\n';
        require(
            saw_firing && saw_water_hit && saw_target_hit,
            "the naval capture lacks actual firing, water impact or target hit events"
        );
    }
    engine_settings_->current = water_settings_saved;
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
}

} // namespace oa::app

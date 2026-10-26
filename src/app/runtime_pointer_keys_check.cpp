// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The numpad drives the pointer on the main menu and in one skirmish.
// Both edges of a click are handed over before the frame that steps them, so
// the press and the release are one synthetic click.

#include "oa/app/runtime.hpp"

#include "pad_state.hpp"

#include "oa/ui/gui_input.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <optional>
#include <ostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <variant>

namespace oa::app {
namespace {

namespace settings = oa::ui::engine_settings;

[[noreturn]] void fail(std::string_view what) {
    throw std::runtime_error("pointer keys: " + std::string(what));
}

void require(bool ok, std::string_view what) {
    if (!ok)
        fail(what);
}

} // namespace

void Runtime::check_pointer_keys() {
    if (sdl_.renderer == nullptr || sdl_.window == nullptr)
        fail("needs the SDL renderer");
    options_.trace_input = true;
    const auto saved_preferences = preference_values_;
    const settings::EngineSettings saved_settings = engine_settings();

    struct Restore {
        Runtime& self;
        settings::EngineSettings settings;
        decltype(preference_values_) preferences;

        ~Restore() {
            self.apply_engine_settings(settings);
            self.preference_values_ = std::move(preferences);
            self.preferences_dirty_ = true;
            self.flush_preferences();
        }
    } restore{*this, saved_settings, saved_preferences};

    settings::EngineSettings chosen = saved_settings;
    chosen.pointer_keys = true;
    apply_engine_settings(chosen);

    bool running = true;
    const auto window_id = SDL_GetWindowID(sdl_.window);
    const auto key = [&](SDL_Scancode scancode, SDL_Keycode code, bool down) {
        SDL_Event event{};
        event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
        event.key.windowID = window_id;
        event.key.scancode = scancode;
        event.key.key = code;
        event.key.down = down;
        event.key.repeat = false;
        event.key.mod = SDL_KMOD_NONE;
        dispatch_event(event, running);
        require(running, "a key ended the run");
    };
    const auto type_text = [&](const char* typed) {
        SDL_Event event{};
        event.type = SDL_EVENT_TEXT_INPUT;
        event.text.text = typed;
        dispatch_event(event, running);
        require(running, "typed text ended the run");
    };
    const auto step_frames = [&](int count) {
        for (int frame = 0; frame < count; ++frame) {
            run_frame(running);
            require(running, "a frame ended the run");
        }
    };
    // One short press. The step waits until the clock moves, so the ramp
    // covers a few points and a tap can settle onto a button.
    const auto tap = [&](SDL_Scancode scancode, SDL_Keycode code) {
        key(scancode, code, true);
        const uint64_t began = SDL_GetTicks();
        do {
            SDL_Delay(5);
            run_frame(running);
            require(running, "a tap ended the run");
        } while (SDL_GetTicks() - began < 8U);
        key(scancode, code, false);
        run_frame(running);
    };
    const auto hold_until = [&](SDL_Scancode scancode, SDL_Keycode code, auto&& done) {
        key(scancode, code, true);
        bool met = false;
        for (int frame = 0; frame < 4000; ++frame) {
            SDL_Delay(5);
            run_frame(running);
            require(running, "a hold ended the run");
            if (done()) {
                met = true;
                break;
            }
        }
        key(scancode, code, false);
        run_frame(running);
        return met;
    };
    const auto hover_name = [&]() -> std::string {
        if (!hovered_ || *hovered_ >= resources_.layout.gadgets.size())
            return {};
        return resources_.layout.gadgets[*hovered_].common.name;
    };
    const auto where = [&]() {
        return std::to_string(pointer_x_) + "," + std::to_string(pointer_y_);
    };

    require(screen_ == Screen::main_menu, "did not start on the main menu");
    step_frames(1);

    const std::string single_name(menu::resource_name(menu::Button::single_player));
    const auto& gadgets = resources_.layout.gadgets;
    std::optional<oa::ui::gui_input::Rect> single_rect;
    for (std::size_t index = 1; index < gadgets.size(); ++index) {
        if (std::string(gadgets[index].common.name) != single_name)
            continue;
        const auto* button = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadgets[index].fields);
        if (button != nullptr && button->grayed_out)
            continue;
        single_rect = oa::ui::gui_input::gadget_geometry(gadgets, index);
        break;
    }
    require(single_rect.has_value(), "the main menu has no Single button");
    const auto origin = panel_origin();
    const float band_left = static_cast<float>(origin.x) + static_cast<float>(single_rect->left);
    const float band_right = static_cast<float>(origin.x) + static_cast<float>(single_rect->right);
    const float band_top = static_cast<float>(origin.y) + static_cast<float>(single_rect->top);
    const float band_bottom =
        static_cast<float>(origin.y) + static_cast<float>(single_rect->bottom);
    const auto in_band = [&]() { return pointer_y_ >= band_top && pointer_y_ <= band_bottom; };

    if (pointer_y_ < band_top) {
        require(
            hold_until(SDL_SCANCODE_KP_2, SDLK_KP_2, [&] { return pointer_y_ >= band_top; }),
            "Numpad2 did not reach Single, pointer at " + where()
        );
    }
    for (int nudge = 0; nudge < 400 && !in_band(); ++nudge) {
        if (pointer_y_ > band_bottom)
            tap(SDL_SCANCODE_KP_8, SDLK_KP_8);
        else
            tap(SDL_SCANCODE_KP_2, SDLK_KP_2);
    }
    require(in_band(), "the pointer missed Single's row, pointer at " + where());

    if (pointer_x_ > band_left - 6.0F) {
        require(
            hold_until(
                SDL_SCANCODE_KP_4, SDLK_KP_4, [&] { return pointer_x_ <= band_left - 6.0F; }
            ),
            "Numpad4 did not stop left of Single, pointer at " + where()
        );
    }
    for (int nudge = 0; nudge < 800 && hover_name() != single_name; ++nudge) {
        if (pointer_y_ > band_bottom)
            tap(SDL_SCANCODE_KP_8, SDLK_KP_8);
        else if (pointer_y_ < band_top)
            tap(SDL_SCANCODE_KP_2, SDLK_KP_2);
        else if (pointer_x_ > band_right)
            tap(SDL_SCANCODE_KP_4, SDLK_KP_4);
        else
            tap(SDL_SCANCODE_KP_6, SDLK_KP_6);
    }
    require(
        hover_name() == single_name,
        "Numpad6 did not land on Single, pointer at " + where() + " over " + hover_name()
    );
    std::cout << "pointer keys: menu Numpad6 moved right onto " << single_name << '\n';

    // Down and up before the frame, so the menu sees one click.
    key(SDL_SCANCODE_KP_5, SDLK_KP_5, true);
    key(SDL_SCANCODE_KP_5, SDLK_KP_5, false);
    run_frame(running);
    for (int frame = 0; frame < 30 && screen_ == Screen::main_menu; ++frame)
        run_frame(running);
    require(
        screen_ == Screen::single_player,
        "Numpad5 did not open single player, screen " + std::to_string(static_cast<int>(screen_))
    );
    std::cout << "pointer keys: menu Numpad5 pressed " << single_name
              << " and opened single player\n";

    load(Screen::main_menu);
    require(screen_ == Screen::main_menu, "could not return to the main menu");
    start_benchmark_skirmish();
    apply_output_mode();
    step_frames(4);
    require(screen_ == Screen::match && match_ != nullptr, "the skirmish did not start");

    const auto canvas_size = PadAccess::canvas(*this);
    const float scale_x = canvas_size.x / static_cast<float>(kCanvasWidth);
    const float scale_y = canvas_size.y / static_cast<float>(kCanvasHeight);
    require(scale_x > 0.0F && scale_y > 0.0F, "the canvas has no size");
    const float room_y = 300.0F * scale_y;
    if (pointer_y_ < room_y) {
        require(
            hold_until(SDL_SCANCODE_KP_2, SDLK_KP_2, [&] { return pointer_y_ >= room_y; }),
            "Numpad2 did not leave room to move up, pointer at " + where()
        );
    }
    const float from_x = pointer_x_;
    const float from_y = pointer_y_;
    key(SDL_SCANCODE_KP_8, SDLK_KP_8, true);
    const uint64_t held_from = SDL_GetTicks();
    while (SDL_GetTicks() - held_from < 1000U) {
        SDL_Delay(5);
        run_frame(running);
        require(running, "the one second hold ended the run");
    }
    const float moved_x = (pointer_x_ - from_x) / scale_x;
    const float moved_y = (pointer_y_ - from_y) / scale_y;
    const float points = std::hypot(moved_x, moved_y);
    key(SDL_SCANCODE_KP_8, SDLK_KP_8, false);
    run_frame(running);
    require(
        points >= 200.0F,
        "Numpad8 held 1 s moved " + std::to_string(points) + " points from " + where()
    );
    std::cout << "pointer keys: skirmish Numpad8 held 1 s moved " << std::lround(points)
              << " points\n";

    uint16_t commander = 0;
    for (const auto& slot : match_->world().slots) {
        if (slot.unit != nullptr && slot.record.type_index != 0 &&
            slot.record.owner_index == match_local_player_) {
            commander = slot.unit_index;
            break;
        }
    }
    require(commander != 0, "found no local commander");
    const auto spawn = [&](std::string_view name, int32_t dx, int32_t dz) -> uint16_t {
        const auto type = oa::sim::unit_spawn::find_type_index(spawn_type_names_, name);
        if (type == 0)
            return 0;
        const auto* home = match_->world().slots[commander].unit;
        if (home == nullptr)
            return 0;
        const auto x = static_cast<int32_t>(home->position[0] >> 16) + dx;
        const auto z = static_cast<int32_t>(home->position[2] >> 16) + dz;
        if (x < 32 || z < 32)
            return 0;
        oa::sim::unit_spawn::Request request;
        request.player = match_local_player_;
        request.type = type;
        request.finished = true;
        request.state = kGroundOccupancyState;
        request.position = {
            static_cast<uint32_t>(x) << 16,
            static_cast<uint32_t>(
                match_->map_height(static_cast<uint32_t>(x) << 16, static_cast<uint32_t>(z) << 16)
            ) << 16,
            static_cast<uint32_t>(z) << 16
        };
        auto* slot = match_->create(request);
        if (slot == nullptr || slot->unit == nullptr)
            return 0;
        slot->unit->object_present = true;
        return slot->unit_index;
    };
    const auto unit_selected = [&](uint16_t id) {
        if (id == 0 || id >= match_->world().slots.size())
            return false;
        const auto* unit = match_->world().slots[id].unit;
        return unit != nullptr && (unit->flags & OA_UNIT_FLAG_SELECTED) != 0;
    };
    uint16_t first = 0;
    uint16_t second = 0;
    for (const std::string_view name : {"ARMPW", "CORAK"}) {
        first = spawn(name, 192, 64);
        second = spawn(name, 240, 88);
        if (first != 0 && second != 0)
            break;
    }
    require(first != 0 && second != 0, "could not spawn two units");
    center_camera_on_unit(first);
    step_frames(3);
    clear_local_selection();

    const auto project = [&](uint16_t id) {
        const auto* unit = match_->world().slots[id].unit;
        require(unit != nullptr, "a spawned unit is gone");
        const auto viewport = live_viewport(match_camera_x_, match_camera_z_);
        return project_match_point(
            viewport,
            {static_cast<uint32_t>(unit->position[0]),
             static_cast<uint32_t>(unit->position[1]),
             static_cast<uint32_t>(unit->position[2])}
        );
    };
    const auto first_at = project(first);
    const auto second_at = project(second);
    const float cover_left = static_cast<float>(std::min(first_at.x, second_at.x)) - 20.0F;
    const float cover_top = static_cast<float>(std::min(first_at.y, second_at.y)) - 20.0F;
    const float cover_right = static_cast<float>(std::max(first_at.x, second_at.x)) + 20.0F;
    const float cover_bottom = static_cast<float>(std::max(first_at.y, second_at.y)) + 20.0F;
    const float side = std::max(cover_right - cover_left, cover_bottom - cover_top);
    const float field_left = static_cast<float>(match_layout_.left) + 8.0F;
    const float field_top = static_cast<float>(match_layout_.top) + 8.0F;
    const float field_right =
        static_cast<float>(match_layout_.left + match_layout_.battlefield_width()) - 9.0F;
    const float field_bottom =
        static_cast<float>(match_layout_.top + match_layout_.battlefield_height()) - 9.0F;
    float start_x = cover_left;
    float start_y = cover_top;
    if (start_x + side > field_right)
        start_x = field_right - side;
    if (start_y + side > field_bottom)
        start_y = field_bottom - side;
    if (start_x < field_left)
        start_x = field_left;
    if (start_y < field_top)
        start_y = field_top;
    const float end_x = start_x + side;
    const float end_y = start_y + side;
    require(
        start_x <= cover_left && start_y <= cover_top && end_x >= cover_right &&
            end_y >= cover_bottom && end_x <= field_right && end_y <= field_bottom,
        "the two units do not fit in a battlefield box"
    );

    const auto move_to = [&](float target_x, float target_y) {
        for (int leg = 0; leg < 12; ++leg) {
            const float error_x = target_x - pointer_x_;
            const float error_y = target_y - pointer_y_;
            if (std::fabs(error_x) <= 4.0F && std::fabs(error_y) <= 4.0F)
                return;
            SDL_Scancode scancode = SDL_SCANCODE_KP_6;
            SDL_Keycode code = SDLK_KP_6;
            bool across = true;
            if (std::fabs(error_x) >= std::fabs(error_y)) {
                scancode = error_x > 0.0F ? SDL_SCANCODE_KP_6 : SDL_SCANCODE_KP_4;
                code = error_x > 0.0F ? SDLK_KP_6 : SDLK_KP_4;
            } else {
                across = false;
                scancode = error_y > 0.0F ? SDL_SCANCODE_KP_2 : SDL_SCANCODE_KP_8;
                code = error_y > 0.0F ? SDLK_KP_2 : SDLK_KP_8;
            }
            const float sign =
                across ? (error_x > 0.0F ? 1.0F : -1.0F) : (error_y > 0.0F ? 1.0F : -1.0F);
            require(
                hold_until(
                    scancode,
                    code,
                    [&] {
                        const float left_x = target_x - pointer_x_;
                        const float left_y = target_y - pointer_y_;
                        if (std::fabs(left_x) <= 4.0F && std::fabs(left_y) <= 4.0F)
                            return true;
                        const float remaining = across ? left_x : left_y;
                        return remaining * sign <= 4.0F;
                    }
                ),
                "the pointer did not reach the box, pointer at " + where()
            );
        }
        require(
            std::fabs(target_x - pointer_x_) <= 4.0F && std::fabs(target_y - pointer_y_) <= 4.0F,
            "the pointer stopped at " + where()
        );
    };
    move_to(start_x, start_y);

    // The first press holds the button. Its key-up does not let go.
    key(SDL_SCANCODE_KP_PLUS, SDLK_KP_PLUS, true);
    run_frame(running);
    key(SDL_SCANCODE_KP_PLUS, SDLK_KP_PLUS, false);
    run_frame(running);
    require(
        hold_until(
            SDL_SCANCODE_KP_3,
            SDLK_KP_3,
            [&] { return pointer_x_ >= end_x - 2.0F && pointer_y_ >= end_y - 2.0F; }
        ),
        "Numpad3 did not draw the box, pointer at " + where()
    );
    key(SDL_SCANCODE_KP_PLUS, SDLK_KP_PLUS, true);
    run_frame(running);
    key(SDL_SCANCODE_KP_PLUS, SDLK_KP_PLUS, false);
    run_frame(running);
    require(
        unit_selected(first) && unit_selected(second) && !unit_selected(commander),
        "the box did not select the two units"
    );
    std::cout << "pointer keys: box selected 2 units\n";

    // Numpad 0 is the right button. In the right-click interface that press
    // gives the move. In the left-click interface it clears the selection,
    // and the side column goes back to Orders. The ground lookup also clamps
    // a point on the side column into the battlefield, so the pointer itself
    // has to be on the field, where the pad's events land.
    match_->state().game.interface_type = oa::sim::gameplay_input::interface_right_click;
    const auto on_field = [&] {
        return battlefield_contains(pointer_x_, pointer_y_) && pointer_x_ >= field_left &&
               pointer_x_ <= field_right && pointer_y_ >= field_top && pointer_y_ <= field_bottom &&
               !hovered_;
    };
    if (hovered_match_unit_ != 0 || !on_field() || !match_pointer_ground(pointer_x_, pointer_y_)) {
        require(
            hold_until(
                SDL_SCANCODE_KP_6,
                SDLK_KP_6,
                [&] {
                    return hovered_match_unit_ == 0 && on_field() &&
                           match_pointer_ground(pointer_x_, pointer_y_).has_value();
                }
            ),
            "Numpad6 did not reach open ground, pointer at " + where()
        );
    }
    require(
        hovered_match_unit_ == 0 && on_field() &&
            match_pointer_ground(pointer_x_, pointer_y_).has_value(),
        "the pointer is not on open ground"
    );
    key(SDL_SCANCODE_KP_0, SDLK_KP_0, true);
    key(SDL_SCANCODE_KP_0, SDLK_KP_0, false);
    run_frame(running);
    require(status_ == "Move", "Numpad0 gave \"" + status_ + "\" instead of a move");
    std::cout << "pointer keys: Numpad0 gave the selection a move\n";

    const int32_t speed_before = match_->state().game.current_speed;
    key(SDL_SCANCODE_KP_PLUS, SDLK_KP_PLUS, true);
    run_frame(running);
    key(SDL_SCANCODE_KP_PLUS, SDLK_KP_PLUS, false);
    run_frame(running);
    require(
        match_->state().game.current_speed == speed_before,
        "NumpadAdd changed the game speed while pointer keys were on"
    );
    std::cout << "pointer keys: NumpadAdd changed no game speed\n";
    key(SDL_SCANCODE_KP_PERIOD, SDLK_KP_PERIOD, true);
    run_frame(running);
    key(SDL_SCANCODE_KP_PERIOD, SDLK_KP_PERIOD, false);
    run_frame(running);

    chosen = engine_settings();
    chosen.pointer_keys = false;
    apply_engine_settings(chosen);
    key(SDL_SCANCODE_KP_PLUS, SDLK_KP_PLUS, true);
    run_frame(running);
    key(SDL_SCANCODE_KP_PLUS, SDLK_KP_PLUS, false);
    run_frame(running);
    require(
        match_->state().game.current_speed > speed_before, "NumpadAdd did not raise the game speed"
    );
    std::cout << "pointer keys: NumpadAdd raised the game speed\n";

    chosen.pointer_keys = true;
    apply_engine_settings(chosen);
    open_chat_line();
    require(chat_composing_, "the chat line did not open");
    const float chat_x = pointer_x_;
    const float chat_y = pointer_y_;
    key(SDL_SCANCODE_KP_8, SDLK_KP_8, true);
    type_text("8");
    run_frame(running);
    key(SDL_SCANCODE_KP_8, SDLK_KP_8, false);
    run_frame(running);
    require(chat_buffer_ == "8", "the chat line holds \"" + chat_buffer_ + "\"");
    require(
        std::fabs(pointer_x_ - chat_x) <= 0.5F && std::fabs(pointer_y_ - chat_y) <= 0.5F,
        "Numpad8 moved the pointer while the chat line was open"
    );
    std::cout << "pointer keys: chat Numpad8 typed 8\n";
}

} // namespace oa::app

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The skirmish Side button, clicked through the SDL presenter: left through
// every offered side and, with more than five sides, right back through them.
#include "oa/app/runtime.hpp"
#include "oa/core/side.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

namespace oa::app {

namespace {

constexpr auto kLeft = static_cast<uint8_t>(SDL_BUTTON_LEFT);
constexpr auto kRight = static_cast<uint8_t>(SDL_BUTTON_RIGHT);
constexpr int32_t kParkX = 0;
constexpr int32_t kParkY = 0;

[[noreturn]] void fail(std::string_view what) {
    throw std::runtime_error("skirmish sides check: " + std::string(what));
}

void require(bool condition, std::string_view what) {
    if (!condition)
        fail(what);
}

fs::path step_snapshot(const fs::path& snapshot, std::string_view step) {
    return snapshot.parent_path() / (snapshot.stem().string() + "-" + std::string(step) + ".ppm");
}

std::size_t changed_pixels(
    const renderer::Surface& before,
    const renderer::Surface& after,
    int32_t x,
    int32_t y,
    int32_t width,
    int32_t height
) {
    if (before.width != after.width || before.height != after.height)
        return static_cast<std::size_t>(std::max(width, 0)) *
               static_cast<std::size_t>(std::max(height, 0));
    std::size_t changed = 0;
    for (int32_t row = std::max(y, 0); row < std::min<int32_t>(y + height, before.height); ++row) {
        for (int32_t column = std::max(x, 0); column < std::min<int32_t>(x + width, before.width);
             ++column) {
            const auto offset =
                (static_cast<std::size_t>(row) * before.width + static_cast<std::size_t>(column)) *
                3U;
            if (!std::equal(
                    before.rgb.begin() + static_cast<std::ptrdiff_t>(offset),
                    before.rgb.begin() + static_cast<std::ptrdiff_t>(offset + 3U),
                    after.rgb.begin() + static_cast<std::ptrdiff_t>(offset)
                ))
                ++changed;
        }
    }
    return changed;
}

std::string side_name_of(const Side& side) {
    return std::string(side.name, ::strnlen(side.name, sizeof side.name));
}

} // namespace

void Runtime::check_skirmish_sides() {
    if (sdl_.renderer == nullptr || sdl_.window == nullptr)
        fail("needs the SDL renderer");
    bool running = true;
    const auto send = [&](SDL_EventType type, int32_t x, int32_t y, uint8_t button) {
        float window_x = 0.0F;
        float window_y = 0.0F;
        require(
            frame_to_window(
                sdl_.renderer, static_cast<float>(x), static_cast<float>(y), &window_x, &window_y
            ),
            SDL_GetError()
        );
        SDL_Event event{};
        event.type = type;
        if (type == SDL_EVENT_MOUSE_MOTION) {
            event.motion.windowID = SDL_GetWindowID(sdl_.window);
            event.motion.x = window_x;
            event.motion.y = window_y;
        } else {
            event.button.windowID = SDL_GetWindowID(sdl_.window);
            event.button.button = button;
            event.button.down = type == SDL_EVENT_MOUSE_BUTTON_DOWN;
            event.button.clicks = 1;
            event.button.x = window_x;
            event.button.y = window_y;
        }
        dispatch_event(event, running);
    };

    struct Box {
        int32_t x{};
        int32_t y{};
        int32_t width{};
        int32_t height{};
    };

    const auto box_of = [&](std::string_view name) {
        const auto* gadget = widget(name);
        if (gadget == nullptr)
            fail(std::string(name) + " is not on the skirmish");
        const auto origin = panel_origin();
        return Box{
            origin.x + gadget->common.x,
            origin.y + gadget->common.y,
            gadget->common.width,
            gadget->common.height
        };
    };
    const auto park_pointer = [&] {
        send(SDL_EVENT_MOUSE_MOTION, kParkX, kParkY, 0);
        require(!hovered_, "the parking point is over a gadget");
    };
    const auto frame = [&] {
        park_pointer();
        return frame_without_cursor();
    };
    const auto click = [&](uint8_t button) {
        const auto box = box_of("Side0");
        const auto x = box.x + box.width / 2;
        const auto y = box.y + box.height / 2;
        send(SDL_EVENT_MOUSE_MOTION, x, y, 0);
        send(SDL_EVENT_MOUSE_BUTTON_DOWN, x, y, button);
        send(SDL_EVENT_MOUSE_BUTTON_UP, x, y, button);
        idle_tick();
    };

    load(Screen::main_menu);
    exercise_click(menu::resource_name(menu::Button::single_player));
    exercise_click(entry::resource_name(entry::Button::skirmish));
    require(screen_ == Screen::skirmish, "Skirmish did not open SKIRMISH.GUI");
    require(!skirmish_settings_.slots.empty(), "the skirmish has no slots");

    const int32_t table_count = skirmish_ui_.side_count;
    require(table_count > 0, "SIDEDATA lists no sides");
    const auto hidden = [&](int32_t side) {
        return side >= 0 && side < 32 &&
               (skirmish_ui_.hidden_sides & (1U << static_cast<uint32_t>(side))) != 0;
    };
    int offered = 0;
    for (int32_t side = 0; side < table_count; ++side)
        if (!hidden(side))
            ++offered;
    if (offered == 0)
        offered = table_count;
    const auto stepped = [&](int32_t side, int32_t direction) {
        for (int32_t step = 1; step < table_count; ++step) {
            int32_t next = side + direction * step;
            next %= table_count;
            if (next < 0)
                next += table_count;
            if (!hidden(next))
                return next;
        }
        int32_t wrapped = side + direction;
        wrapped %= table_count;
        if (wrapped < 0)
            wrapped += table_count;
        return wrapped;
    };

    std::size_t side_frames = 0;
    if (const auto sprite = widget_sprites_.find("Side0"); sprite != widget_sprites_.end()) {
        if (const auto* sequence = gaf_sequence(resources_.sprites, sprite->second.sequence))
            side_frames = sequence->frames.size();
    }

    int by_art = 0;
    int by_name = 0;
    const auto shown_name = [&](int32_t side) {
        require(
            side >= 0 && static_cast<uint32_t>(side) < side_table_.count,
            "side " + std::to_string(side) + " is past SIDEDATA"
        );
        return side_name_of(side_table_.sides[side]);
    };
    const auto check_face = [&](int32_t side, bool tally) {
        const std::string key("Side0");
        const bool named = widget_caption_only_.find(key) != widget_caption_only_.end();
        const bool want_name = static_cast<std::size_t>(side) >= side_frames;
        const auto* gadget = widget(key);
        require(gadget != nullptr, "Side0 is not on the skirmish");
        const auto* fields = std::get_if<oa::ui::gui_layout::ButtonFields>(&gadget->fields);
        require(fields != nullptr, "Side0 is not a button");
        if (want_name) {
            require(named, "side " + std::to_string(side) + " still shows art");
            require(
                widget_gaf_frames_.find(key) == widget_gaf_frames_.end(),
                "side " + std::to_string(side) + " keeps a frame"
            );
            require(
                fields->text == shown_name(side),
                "side " + std::to_string(side) + " shows '" + fields->text + "' instead of '" +
                    shown_name(side) + "'"
            );
            if (tally)
                ++by_name;
        } else {
            require(!named, "side " + std::to_string(side) + " hides its art");
            require(
                fields->text.empty(),
                "side " + std::to_string(side) + " draws a caption over its art"
            );
            const auto frame_of = widget_gaf_frames_.find(key);
            require(
                frame_of != widget_gaf_frames_.end() &&
                    frame_of->second == static_cast<std::size_t>(side),
                "side " + std::to_string(side) + " shows the wrong frame"
            );
            if (tally)
                ++by_art;
        }
    };
    const auto step_button = [&](uint8_t button, int32_t direction, bool tally, bool picture) {
        const auto box = box_of("Side0");
        const int32_t before_side = skirmish_settings_.slots[0].side;
        const int32_t expected = stepped(before_side, direction);
        auto before = frame();
        click(button);
        require(
            skirmish_settings_.slots[0].side == expected,
            "Side0 went to side " + std::to_string(skirmish_settings_.slots[0].side) +
                " instead of " + std::to_string(expected)
        );
        check_face(expected, tally);
        auto after = frame();
        if (expected != before_side) {
            require(
                changed_pixels(before, after, box.x, box.y, box.width, box.height) != 0,
                "side " + std::to_string(expected) + " did not repaint Side0"
            );
        }
        if (picture && !options_.snapshot.empty())
            write_ppm(step_snapshot(options_.snapshot, "side-" + std::to_string(expected)), after);
    };

    const int32_t start = skirmish_settings_.slots[0].side;
    for (int step = 0; step < offered; ++step)
        step_button(kLeft, 1, true, true);
    require(
        skirmish_settings_.slots[0].side == start,
        "left clicks did not come back to side " + std::to_string(start)
    );
    require(by_art + by_name == offered, "the faces do not cover the offered sides");

    const bool back = table_count > OA_SIDE_COUNT;
    if (back) {
        for (int step = 0; step < offered; ++step)
            step_button(kRight, -1, false, false);
        require(
            skirmish_settings_.slots[0].side == start,
            "right clicks did not come back to side " + std::to_string(start)
        );
    } else {
        step_button(kRight, 1, false, false);
    }

    std::cout << "skirmish sides check: " << offered << " sides, " << by_art << " by art, "
              << by_name << " by name, back " << (back ? "yes" : "no") << '\n';
}

} // namespace oa::app

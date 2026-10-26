// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// ui.whiteboard in a running match: drawing, moving, writing and erasing
// marks while the draw action is held, the jump action to the newest
// received marker, drawing the marks, and the batches network play carries.

#include "oa/app/runtime.hpp"
#include "oa/formats/fnt.hpp"
#include "oa/present/game_text.hpp"
#include "oa/present/typed_text.hpp"
#include "oa/sim/messages.hpp"
#include "oa/ui/hud/shared_views.hpp"
#include "oa/ui/hud/whiteboard.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <tuple>
#include <vector>

namespace oa::app {
namespace {

namespace hud = oa::ui::hud;
namespace keys = oa::ui::key_bindings;

/// Map pixels past the view's edges that lines are still drawn within.
constexpr int32_t kLineMargin = 75;
/// Bytes a marker's game text holds.
constexpr std::size_t kMarkerTextLimit = 64;
/// The rows a marker's text line takes where the game's label font has not
/// loaded.
constexpr int32_t kMarkerTextFallbackRows = 12;

} // namespace

bool Runtime::whiteboard_on() const {
    return match_ && ui_rules().whiteboard.enabled && !megamap_open_ && !local_player_watches();
}

std::array<int32_t, 2> Runtime::battlefield_map_point(float x, float y) const {
    const auto width = std::max(1, match_layout_.battlefield_width());
    const auto height = std::max(1, match_layout_.battlefield_height());
    // A view drawn between map pixels shows each map pixel that far before
    // the camera's, and the card's shift moves the picture on (view_shift);
    // with neither, adding zero leaves the quotient as it was.
    const auto offset = view_offset();
    const auto shift = view_shift();
    const auto map_x =
        match_camera_x_ +
        static_cast<int32_t>(
            (x - static_cast<float>(match_layout_.left) - static_cast<float>(shift[0])) *
                static_cast<float>(visible_map_width()) / static_cast<float>(width) +
            static_cast<float>(offset.x)
        );
    const auto map_y =
        match_camera_z_ +
        static_cast<int32_t>(
            (y - static_cast<float>(match_layout_.top) - static_cast<float>(shift[1])) *
                static_cast<float>(visible_map_height()) / static_cast<float>(height) +
            static_cast<float>(offset.y)
        );
    return {map_x, map_y};
}

bool Runtime::whiteboard_pointer(const SDL_Event& event, float x, float y) {
    auto& input = whiteboard_input_;
    const bool busy = input.drawing || input.erasing || input.moving >= 0;
    if (!whiteboard_on() || (!busy && !action_held("whiteboard.draw")))
        return false;
    // A stroke starts in the overlays' area (the battlefield, or with the
    // touch controls on, the part of it they leave clear), not on a touch
    // control or a placed part of the HUD; one under way goes on anywhere.
    const auto area = overlay_area();
    const bool in_area = x >= static_cast<float>(area.x) && y >= static_cast<float>(area.y) &&
                         x < static_cast<float>(area.x + area.width) &&
                         y < static_cast<float>(area.y + area.height) && !placed_hud_covers(x, y);
    if (!busy && event.type != SDL_EVENT_MOUSE_MOTION && !in_area)
        return false;
    const auto point = battlefield_map_point(x, y);
    const auto color = hud::player_dot_color(match_->state(), match_local_player_);
    switch (event.type) {
    case SDL_EVENT_MOUSE_MOTION:
        if (input.drawing && (point[0] != input.last_x || point[1] != input.last_y)) {
            hud::whiteboard_draw_line(
                whiteboard_, input.last_x, input.last_y, point[0], point[1], color
            );
            input.last_x = point[0];
            input.last_y = point[1];
        } else if (input.erasing) {
            hud::whiteboard_area_erase(whiteboard_, point[0], point[1]);
        }
        return busy || action_held("whiteboard.draw");
    case SDL_EVENT_MOUSE_BUTTON_DOWN: {
        const uint8_t button = event.button.button;
        const bool doubled = event.button.clicks >= 2 && event.button.clicks % 2 == 0;
        if (doubled &&
            action_button_matches("whiteboard.text", button, keys::Gesture::double_press)) {
            // A double-click writes on the marker under it, or places one.
            input.drawing = false;
            const auto at = hud::whiteboard_marker_at(whiteboard_, point[0], point[1]);
            input.editing = true;
            input.editing_marker = at >= 0;
            input.edit_x = at >= 0 ? whiteboard_.markers[static_cast<std::size_t>(at)].x : point[0];
            input.edit_y = at >= 0 ? whiteboard_.markers[static_cast<std::size_t>(at)].y : point[1];
            // The marker's game text is edited as the characters it holds.
            input.edit_text = at >= 0 ? oa::present::decode_game_text(
                                            whiteboard_.markers[static_cast<std::size_t>(at)].text,
                                            game_text_utf8()
                                        )
                                      : "";
            // The marker's text box: from the dot's corner, a line of
            // the label font tall with the dot, across the rest of the
            // overlays' area.
            const oa::formats::fnt::Font* font = match_label_font();
            const int32_t text_rows =
                font != nullptr ? static_cast<int32_t>(oa::formats::fnt::line_height(*font))
                                : kMarkerTextFallbackRows;
            const int32_t half = hud::kWhiteboardDotSide / 2;
            const auto box_left = static_cast<int32_t>(x) - half;
            const auto box_top = static_cast<int32_t>(y) - half;
            start_text_input(
                oa::ui::display_layout::Rect{
                    box_left,
                    box_top,
                    std::max(area.x + area.width - box_left, hud::kWhiteboardDotSide),
                    text_rows + hud::kWhiteboardDotSide,
                }
            );
            return true;
        }
        if (doubled &&
            action_button_matches("whiteboard.erase", button, keys::Gesture::double_press)) {
            input.erasing = false;
            hud::whiteboard_spot_erase(whiteboard_, point[0], point[1]);
            return true;
        }
        if (action_button_matches("whiteboard.move", button, keys::Gesture::press) ||
            action_button_matches("whiteboard.line", button, keys::Gesture::press)) {
            const auto at = hud::whiteboard_marker_at(whiteboard_, point[0], point[1]);
            if (at >= 0 && action_button_matches("whiteboard.move", button, keys::Gesture::press)) {
                input.moving = at;
                input.from_x = whiteboard_.markers[static_cast<std::size_t>(at)].x;
                input.from_y = whiteboard_.markers[static_cast<std::size_t>(at)].y;
            } else if (action_button_matches("whiteboard.line", button, keys::Gesture::press)) {
                input.drawing = true;
                input.last_x = point[0];
                input.last_y = point[1];
            } else {
                return false;
            }
            return true;
        }
        if (action_button_matches("whiteboard.wipe", button, keys::Gesture::press)) {
            input.erasing = true;
            hud::whiteboard_area_erase(whiteboard_, point[0], point[1]);
            return true;
        }
        // The dot's press is taken so the battlefield does not see it. The
        // dot itself is placed when the button comes up.
        return action_button_matches("whiteboard.dot", button, keys::Gesture::press);
    }
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        const uint8_t button = event.button.button;
        if (action_button_matches("whiteboard.line", button, keys::Gesture::press) ||
            action_button_matches("whiteboard.move", button, keys::Gesture::press)) {
            if (input.moving >= 0)
                std::ignore = hud::whiteboard_move_marker(
                    whiteboard_, input.from_x, input.from_y, point[0], point[1]
                );
            input.moving = -1;
            input.drawing = false;
            return true;
        }
        if (action_button_matches("whiteboard.wipe", button, keys::Gesture::press)) {
            input.erasing = false;
            return true;
        }
        if (action_button_matches("whiteboard.dot", button, keys::Gesture::press)) {
            // The dot action's chord is a press. The mark is placed on release.
            hud::whiteboard_place_marker(whiteboard_, point[0], point[1], color, "");
            return true;
        }
        return false;
    }
    default:
        return false;
    }
}

bool Runtime::take_whiteboard_chord(const SDL_KeyboardEvent& key) {
    if (!match_ || !ui_rules().whiteboard.enabled)
        return false;
    auto& input = whiteboard_input_;
    if (input.editing) {
        if (key.key == SDLK_RETURN || key.key == SDLK_ESCAPE) {
            input.editing = false;
            stop_text_input();
            const auto color = hud::player_dot_color(match_->state(), match_local_player_);
            // The typed text is placed, and sent, as game text.
            const std::string text = typed_game_text(input.edit_text);
            if (input.editing_marker)
                std::ignore =
                    hud::whiteboard_edit_marker(whiteboard_, input.edit_x, input.edit_y, text);
            else
                hud::whiteboard_place_marker(whiteboard_, input.edit_x, input.edit_y, color, text);
            return true;
        }
        if (key.key == SDLK_BACKSPACE && !input.edit_text.empty())
            input.edit_text.erase(oa::present::last_character_start(input.edit_text));
        return true;
    }
    if (!whiteboard_on() || key.repeat)
        return false;
    // The jump chord goes to the newest marker another player placed.
    if (action_key_matches("whiteboard.jump", key)) {
        if (whiteboard_.received_marker)
            set_camera_position(
                whiteboard_.received_x - visible_map_width() / 2,
                whiteboard_.received_y - visible_map_height() / 2,
                0
            );
        return true;
    }
    return action_key_matches("whiteboard.draw", key);
}

bool Runtime::whiteboard_text(const SDL_Event& event) {
    auto& input = whiteboard_input_;
    if (event.type != SDL_EVENT_TEXT_INPUT || !input.editing)
        return false;
    // Characters that would take the marker's game text past its limit are
    // not taken, the first such one and the rest after it.
    const std::string taken =
        oa::present::typed_characters(event.text.text, oa::present::TypedCharacters::text);
    std::string typed = input.edit_text;
    for (std::size_t at = 0; at < taken.size();) {
        // One character more each time.
        std::string with = typed;
        const oa::present::TypedLimits limits{
            with.size() + taken.size(), oa::present::character_count(with) + 1
        };
        const auto added = oa::present::take_typed_text(
            with, std::string_view(taken).substr(at), oa::present::TypedCharacters::text, limits
        );
        if (added == 0 || typed_game_text(with).size() > kMarkerTextLimit)
            break;
        typed = std::move(with);
        at += added;
    }
    input.edit_text = std::move(typed);
    return true;
}

void Runtime::draw_whiteboard(const oa::present::world_renderer::BattlefieldViewport& viewport) {
    if (!match_ || !ui_rules().whiteboard.enabled || megamap_open_)
        return;
    // The marks go where the frame draws their map points: on the world
    // layer, from the battlefield's corner, and as far before it as a view
    // drawn between map pixels lies.
    const auto screen = [&](int32_t x, int32_t y) {
        return project_match_point(
            viewport, {static_cast<uint32_t>(x) << 16, 0, static_cast<uint32_t>(y) << 16}
        );
    };
    const int32_t left = match_camera_x_ - kLineMargin;
    const int32_t top = match_camera_z_ - kLineMargin;
    const int32_t right = match_camera_x_ + visible_map_width() + kLineMargin;
    const int32_t bottom = match_camera_z_ + visible_map_height() + kLineMargin;
    const auto inside = [&](int32_t x, int32_t y) {
        return x >= left && x < right && y >= top && y < bottom;
    };
    for (const auto& line : whiteboard_.lines) {
        if (!inside(line.x1, line.y1) && !inside(line.x2, line.y2))
            continue;
        const auto from = screen(line.x1, line.y1);
        const auto to = screen(line.x2, line.y2);
        // Each pixel of the line, from one end to the other.
        int32_t x0 = from.x, y0 = from.y;
        const int32_t dx = std::abs(to.x - x0), dy = -std::abs(to.y - y0);
        const int32_t sx = x0 < to.x ? 1 : -1, sy = y0 < to.y ? 1 : -1;
        int32_t error = dx + dy;
        for (;;) {
            fill_hud_rect(x0, y0, 1, 1, line.color);
            if (x0 == to.x && y0 == to.y)
                break;
            const int32_t twice = 2 * error;
            if (twice >= dy) {
                error += dy;
                x0 += sx;
            }
            if (twice <= dx) {
                error += dx;
                y0 += sy;
            }
        }
    }
    const auto& input = whiteboard_input_;
    for (const auto& marker : whiteboard_.markers) {
        if (!inside(marker.x, marker.y))
            continue;
        const auto at = screen(marker.x, marker.y);
        const int32_t half = hud::kWhiteboardDotSide / 2;
        fill_hud_rect(
            at.x - half, at.y - half, hud::kWhiteboardDotSide, hud::kWhiteboardDotSide, marker.color
        );
        const bool edited = input.editing && input.editing_marker && input.edit_x == marker.x &&
                            input.edit_y == marker.y;
        const auto text =
            edited ? typed_game_text(input.edit_text + text_composition_) : marker.text;
        if (!text.empty())
            draw_match_label(at.x + half + 2, at.y - half, text, marker.color);
    }
    if (input.editing && !input.editing_marker) {
        const auto at = screen(input.edit_x, input.edit_y);
        draw_match_label(
            at.x,
            at.y,
            typed_game_text(input.edit_text + text_composition_) + "_",
            hud::player_dot_color(match_->state(), match_local_player_)
        );
    }
}

hud::Whiteboard& Runtime::match_whiteboard() {
    return whiteboard_;
}

std::vector<uint8_t> Runtime::take_whiteboard_batch() {
    return hud::whiteboard_take_batch(whiteboard_);
}

void Runtime::receive_whiteboard_batch(std::span<const uint8_t> batch) {
    if (!match_ || !ui_rules().whiteboard.enabled)
        return;
    hud::WhiteboardEcho echo{};
    echo.user = this;
    echo.marker = [](void* user, uint8_t color, const char* text) {
        auto& self = *static_cast<Runtime*>(user);
        const auto line = hud::whiteboard_echo_line(self.match_->state(), color, text);
        if (!line.empty())
            self.post_match_message(line, oa::sim::messages::kind_player_chat);
    };
    std::ignore = hud::whiteboard_apply_batch(whiteboard_, batch, echo);
}

} // namespace oa::app

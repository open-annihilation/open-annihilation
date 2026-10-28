// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Panels drawn over the screen they are opened from, placed as their loaders
// place them with their bitmap as the panel's backdrop in the palette of the
// screen below: the load and save dialogs (LOADGAME.GUI), over that screen
// with its top panel darkened, and the in-game briefing over the paused match.
#include "oa/app/runtime.hpp"

#include "oa/ui/gui_input/gadget_panel.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace oa::app {

namespace {

namespace panel_flag = oa::ui::gui_input::panel_flag;

// Panel flags the load dialog opens with: centred, with a backdrop,
// darkening the panel below. Over a match the save dialog opens with them
// too, centred on the screen, a deliberate difference from 3.1c.
constexpr uint32_t kLoadDialogFlags =
    panel_flag::shade_below | panel_flag::centre | panel_flag::modal_backdrop;
// Over the end of a mission the save dialog opens at its authored position,
// as in 3.1c.
constexpr uint32_t kSaveDialogFlags = panel_flag::shade_below | panel_flag::modal_backdrop;

bool frame_valid(const renderer::Surface& frame) {
    return frame.width != 0 && frame.height != 0 &&
           frame.rgb.size() == static_cast<std::size_t>(frame.width) * frame.height * 3U;
}

} // namespace

void Runtime::capture_load_game_parent() {
    renderer::Surface frame;
    oa::ui::display_layout::Rect panel{};
    if (screen_ == Screen::match) {
        compose_shown_match(frame);
        load_game_palette_ = match_palette_;
        if (match_hud_ && !match_hud_->layout.gadgets.empty()) {
            const auto& root = match_hud_->layout.gadgets.front().common;
            panel = oa::ui::display_layout::source_rect_to_canvas(
                match_layout_, root.x, root.y, root.width, root.height
            );
        }
    } else {
        frame = frame_without_cursor();
        load_game_palette_ = resources_.background.palette.value_or(resources_.gui_palette);
        if (!resources_.layout.gadgets.empty()) {
            const auto& root = resources_.layout.gadgets.front().common;
            panel = {root.x, root.y, root.width, root.height};
        }
    }
    if (!frame_valid(frame)) {
        load_game_parent_ = {};
        return;
    }
    if (panel.width <= 0 || panel.height <= 0)
        panel = {0, 0, static_cast<int>(frame.width), static_cast<int>(frame.height)};
    renderer::shade_panel_below(
        frame,
        panel.x,
        panel.y,
        panel.width,
        panel.height,
        load_game_palette_,
        display_.context.shade_table
    );
    load_game_parent_ = std::move(frame);
}

void Runtime::enter_load_game() {
    const bool below = frame_valid(load_game_parent_);
    if (below)
        show_background_in(load_game_palette_);
    if (resources_.layout.gadgets.empty())
        return;
    auto& root = resources_.layout.gadgets.front().common;
    const bool authored = save_dialog_open() && options_parent_ != Screen::match;
    const auto flags = (authored ? kSaveDialogFlags : kLoadDialogFlags) | panel_flag::first_draw;
    oa::ui::gui_input::place_root(
        root.x,
        root.y,
        root.width,
        root.height,
        flags,
        below ? static_cast<int32_t>(load_game_parent_.width) : kCanvasWidth,
        below ? static_cast<int32_t>(load_game_parent_.height) : kCanvasHeight,
        oa::ui::gui_input::hud_strip_width
    );
}

renderer::Surface Runtime::frame_without_cursor() {
    struct Restore {
        bool& flag;

        ~Restore() { flag = false; }
    } restore{frame_without_cursor_};

    frame_without_cursor_ = true;
    rebuild_surface();
    return surface_;
}

void Runtime::leave_load_game() {
    load_game_parent_ = {};
}

void Runtime::show_background_in(const oa::PaletteBytes& palette) {
    auto& background = resources_.background;
    if (background.indices.size() !=
            static_cast<std::size_t>(background.width) * background.height ||
        background.rgb.size() < background.indices.size() * 3U)
        return;
    background.palette = palette;
    for (std::size_t pixel = 0; pixel < background.indices.size(); ++pixel)
        std::memcpy(
            &background.rgb[pixel * 3U],
            &palette[static_cast<std::size_t>(background.indices[pixel]) * oa::palette_entry_bytes],
            3
        );
}

bool Runtime::panel_over_screen() const {
    return screen_ == Screen::load_game || (screen_ == Screen::briefing && briefing_from_pause_);
}

const renderer::Surface* Runtime::panel_parent() const {
    if (!panel_over_screen())
        return nullptr;
    const auto& parent =
        screen_ == Screen::load_game ? load_game_parent_ : in_game_briefing_parent_;
    return frame_valid(parent) ? &parent : nullptr;
}

void Runtime::compose_panel_over_parent() {
    if (resources_.layout.gadgets.empty())
        return;
    const auto* parent = panel_parent();
    renderer::Surface frame;
    if (parent != nullptr)
        frame = *parent;
    else
        frame = {
            static_cast<uint32_t>(kCanvasWidth),
            static_cast<uint32_t>(kCanvasHeight),
            std::vector<uint8_t>(static_cast<std::size_t>(kCanvasWidth) * kCanvasHeight * 3U, 0)
        };
    const auto& root = resources_.layout.gadgets.front().common;
    const auto face_width = std::min<int32_t>(root.width, static_cast<int32_t>(surface_.width));
    const auto face_height = std::min<int32_t>(root.height, static_cast<int32_t>(surface_.height));
    const auto left = std::max<int32_t>(0, -root.x);
    const auto right = std::min<int32_t>(face_width, static_cast<int32_t>(frame.width) - root.x);
    for (int32_t y = std::max<int32_t>(0, -root.y); y < face_height; ++y) {
        const auto frame_y = root.y + y;
        if (frame_y >= static_cast<int32_t>(frame.height) || left >= right)
            break;
        std::memcpy(
            &frame.rgb
                 [(static_cast<std::size_t>(frame_y) * frame.width +
                   static_cast<std::size_t>(root.x + left)) *
                  3U],
            &surface_.rgb
                 [(static_cast<std::size_t>(y) * surface_.width + static_cast<std::size_t>(left)) *
                  3U],
            static_cast<std::size_t>(right - left) * 3U
        );
    }
    surface_ = std::move(frame);
}

const oa::PaletteBytes& Runtime::screen_palette() const {
    return resources_.background.palette ? *resources_.background.palette : resources_.gui_palette;
}

oa::ui::display_layout::Point Runtime::panel_origin() const {
    if (!panel_over_screen() || resources_.layout.gadgets.empty())
        return {};
    const auto& root = resources_.layout.gadgets.front().common;
    return {root.x, root.y};
}

} // namespace oa::app

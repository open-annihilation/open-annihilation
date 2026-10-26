// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/update_helper/window.hpp"

#include "oa/app/long_work.hpp"
#include "oa/platform/text_font.hpp"
#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/paint/painter.hpp"
#include "oa/update_helper/log.hpp"

#include <SDL3/SDL.h>

#include <chrono>
#include <memory>
#include <string>
#include <utility>

namespace oa::update_helper {
namespace {

namespace kit = oa::ui::kit;
namespace paint = oa::ui::paint;
namespace text_font = oa::platform::text_font;

int64_t now_ms() {
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::milliseconds>(now).count();
}

} // namespace

struct ProgressWindow::Shown {
    SDL_Window* window{};
    SDL_Renderer* renderer{};
    SDL_Texture* texture{};
    int width{};
    int height{};
    std::unique_ptr<text_font::FontStack> fonts;
    bool dismissed{};
};

PlacedUpdateWindow
place_update_window(std::string_view title, std::string_view line, int32_t progress) {
    kit::WorkDialog dialog;
    dialog.title = std::string(title);
    dialog.line = std::string(line);
    dialog.progress = progress;
    const kit::PlacedWorkDialog placed = kit::place_work_dialog(dialog, nullptr);
    PlacedUpdateWindow result;
    result.width = kit::notice_width;
    result.height = placed.height;
    result.line_x = placed.line.rect.x;
    result.line_y = placed.line.rect.y;
    result.line_width = placed.line.rect.width;
    result.line_height = placed.line.rect.height;
    result.line_text = placed.line.text;
    result.bar_width = placed.bar.width;
    result.bar_height = placed.bar.height;
    return result;
}

ProgressWindow::ProgressWindow() : started_ms_(now_ms()) {
}

ProgressWindow::~ProgressWindow() {
    close();
    delete shown_;
    shown_ = nullptr;
    if (display_open_)
        SDL_Quit();
}

void ProgressWindow::close() {
    if (shown_ == nullptr)
        return;
    if (shown_->texture != nullptr)
        SDL_DestroyTexture(shown_->texture);
    if (shown_->renderer != nullptr)
        SDL_DestroyRenderer(shown_->renderer);
    if (shown_->window != nullptr)
        SDL_DestroyWindow(shown_->window);
    shown_->texture = nullptr;
    shown_->renderer = nullptr;
    shown_->window = nullptr;
}

void ProgressWindow::show(
    std::string_view title, std::string_view line, int32_t progress, bool now
) {
    if (unavailable_ || (shown_ != nullptr && shown_->dismissed))
        return;
    if (!now && now_ms() - started_ms_ < static_cast<int64_t>(oa::app::long_work_show_after_ms))
        return;

    const PlacedUpdateWindow placed = place_update_window(title, line, progress);
    const int width = placed.width > 0 ? placed.width : 1;
    const int height = placed.height > 0 ? placed.height : 1;
    if (shown_ == nullptr)
        shown_ = new Shown;

    if (!display_open_) {
#if defined(__APPLE__)
        // A helper adds no Dock icon of its own. The game it starts does.
        SDL_SetHint(SDL_HINT_MAC_BACKGROUND_APP, "1");
#endif
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            if (log_ != nullptr)
                log_->line(std::string("the window could not be opened: ") + SDL_GetError());
            unavailable_ = true;
            return;
        }
        display_open_ = true;
        shown_->fonts = text_font::FontStack::open(text_font::bundled_font_directory());
    }
    if (shown_->window == nullptr) {
        const std::string caption =
            title.empty() ? std::string("Open Annihilation") : std::string(title);
        shown_->window = SDL_CreateWindow(caption.c_str(), width, height, 0);
        if (shown_->window == nullptr) {
            if (log_ != nullptr)
                log_->line(std::string("the window could not be opened: ") + SDL_GetError());
            unavailable_ = true;
            return;
        }
        shown_->renderer = SDL_CreateRenderer(shown_->window, nullptr);
        if (shown_->renderer == nullptr) {
            if (log_ != nullptr)
                log_->line(std::string("the window could not be opened: ") + SDL_GetError());
            close();
            unavailable_ = true;
            return;
        }
    } else if (shown_->width != width || shown_->height != height) {
        SDL_SetWindowSize(shown_->window, width, height);
    }

    if (shown_->texture == nullptr || shown_->width != width || shown_->height != height) {
        if (shown_->texture != nullptr)
            SDL_DestroyTexture(shown_->texture);
        shown_->texture = SDL_CreateTexture(
            shown_->renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, width, height
        );
        shown_->width = width;
        shown_->height = height;
        if (shown_->texture == nullptr) {
            if (log_ != nullptr)
                log_->line(std::string("the window could not be drawn: ") + SDL_GetError());
            return;
        }
    }

    paint::Canvas pixels = paint::make_canvas(width, height);
    paint::Painter painter(pixels);
    kit::Canvas canvas;
    canvas.smooth.painter = &painter;
    canvas.smooth.fonts = shown_->fonts.get();
    canvas.smooth.scale_percent = 100;
    kit::WorkDialog dialog;
    dialog.title = std::string(title);
    dialog.line = std::string(line);
    dialog.progress = progress;
    kit::draw_work_dialog(canvas, dialog);

    const int pitch = width * 4;
    if (pixels.rgba.size() == static_cast<std::size_t>(pitch) * static_cast<std::size_t>(height)) {
        static_cast<void>(SDL_UpdateTexture(shown_->texture, nullptr, pixels.rgba.data(), pitch));
        static_cast<void>(SDL_RenderClear(shown_->renderer));
        static_cast<void>(SDL_RenderTexture(shown_->renderer, shown_->texture, nullptr, nullptr));
        static_cast<void>(SDL_RenderPresent(shown_->renderer));
    }

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED)
            shown_->dismissed = true;
    }
    if (shown_->dismissed)
        close();
}

} // namespace oa::update_helper

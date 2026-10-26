// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The progress dialog while a piece of work runs off the thread that draws.
#include "oa/app/long_work.hpp"

#include "render_host.hpp"
#include "render_state.hpp"
#include "window_pixels.hpp"

#include "oa/app/full_screen.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/window_icon.hpp"
#include "oa/data/languages.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/platform/log_files.hpp"
#include "oa/platform/system.hpp"
#include "oa/platform/text_font.hpp"
#include "oa/ui/frontend_renderer.hpp"
#include "oa/ui/kit/canvas.hpp"
#include "oa/ui/kit/components.hpp"
#include "oa/ui/kit/components_more.hpp"
#include "oa/ui/kit/layout.hpp"
#include "oa/ui/kit/theme.hpp"
#include "oa/ui/paint/painter.hpp"

#include <SDL3/SDL.h>

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <utility>

namespace oa::app {
namespace {

namespace kit = oa::ui::kit;
namespace paint = oa::ui::paint;
namespace renderer = oa::ui::frontend_renderer;
namespace text_font = oa::platform::text_font;

std::atomic<bool>& progress_running_flag() noexcept {
    static std::atomic<bool> running{false};
    return running;
}

/// Set for the whole of a dialog's job, including the wait before it shows.
struct RunningFlag {
    RunningFlag() { progress_running_flag().store(true, std::memory_order_release); }

    ~RunningFlag() { progress_running_flag().store(false, std::memory_order_release); }

    RunningFlag(const RunningFlag&) = delete;
    RunningFlag& operator=(const RunningFlag&) = delete;
};

/// Joins a worker however the drawing thread leaves the dialog.
struct JoinWorker {
    std::thread& worker;

    explicit JoinWorker(std::thread& held) noexcept : worker(held) {}

    ~JoinWorker() {
        if (worker.joinable())
            worker.join();
    }

    JoinWorker(const JoinWorker&) = delete;
    JoinWorker& operator=(const JoinWorker&) = delete;
};

/// The bundled fonts the dialog's line is drawn in. One thread draws it.
text_font::FontStack& progress_fonts() {
    static std::unique_ptr<text_font::FontStack> stack;
    if (!stack) {
        stack = text_font::FontStack::open(text_font::bundled_font_directory());
        if (!stack)
            throw std::runtime_error("the progress dialog's fonts did not open");
    }
    return *stack;
}

/// Copies one RGB picture onto a canvas, stretched to the canvas when the
/// sizes differ. `under` has pixels.
void blit_behind(paint::Canvas& canvas, const renderer::Surface& under) {
    const int source_width = static_cast<int>(under.width);
    const int source_height = static_cast<int>(under.height);
    if (source_width <= 0 || source_height <= 0 || canvas.width <= 0 || canvas.height <= 0)
        return;
    const std::size_t source_pixels =
        static_cast<std::size_t>(under.width) * static_cast<std::size_t>(under.height);
    if (under.rgb.size() / 3 < source_pixels)
        return;
    const bool same = source_width == canvas.width && source_height == canvas.height;
    for (int row = 0; row < canvas.height; ++row) {
        const int source_row =
            same ? row : static_cast<int>((int64_t{row} * source_height) / canvas.height);
        for (int column = 0; column < canvas.width; ++column) {
            const int source_column =
                same ? column : static_cast<int>((int64_t{column} * source_width) / canvas.width);
            const std::size_t from =
                (static_cast<std::size_t>(source_row) * static_cast<std::size_t>(source_width) +
                 static_cast<std::size_t>(source_column)) *
                3;
            const std::size_t to =
                (static_cast<std::size_t>(row) * static_cast<std::size_t>(canvas.width) +
                 static_cast<std::size_t>(column)) *
                4;
            canvas.rgba[to] = under.rgb[from];
            canvas.rgba[to + 1] = under.rgb[from + 1];
            canvas.rgba[to + 2] = under.rgb[from + 2];
            canvas.rgba[to + 3] = 255;
        }
    }
}

/// The bar's fill, or -1 for the spinner. `done` and `whole` scale to a full bar.
int32_t bar_amount(const LongWork& work) noexcept {
    const int32_t done = work.done.load(std::memory_order_acquire);
    const int32_t whole = work.whole.load(std::memory_order_acquire);
    if (done < 0 || whole <= 0)
        return -1;
    int64_t scaled = int64_t{done} * kit::question_progress_whole / whole;
    if (scaled < 0)
        scaled = 0;
    if (scaled > kit::question_progress_whole)
        scaled = kit::question_progress_whole;
    return static_cast<int32_t>(scaled);
}

/// The frame the dialog is centred in, and a canvas of the window's pixels.
kit::Frame progress_room(SDL_Window* window, SDL_Renderer* renderer, paint::Canvas& pixels) {
    oa::ui::game_files::Viewport read;
    std::ignore = read_window_viewport(window, renderer, read);
    if (read.width <= 0 || read.height <= 0)
        return {};
    if (pixels.width != read.width || pixels.height != read.height)
        pixels = paint::make_canvas(read.width, read.height);
    kit::Viewport view;
    view.width = static_cast<int32_t>(read.width);
    view.height = static_cast<int32_t>(read.height);
    view.density = read.px_per_point;
    const long percent = std::lround(static_cast<double>(read.px_per_point) * 100.0);
    view.scale_percent = static_cast<int32_t>(percent < 100 ? long{100} : percent);
    view.safe.left = static_cast<int32_t>(read.safe.left);
    view.safe.top = static_cast<int32_t>(read.safe.top);
    view.safe.right = static_cast<int32_t>(read.safe.right);
    view.safe.bottom = static_cast<int32_t>(read.safe.bottom);
    return kit::frame_of(view);
}

/// The shipped app icon, cropped to what is visible, as the setup's header
/// draws it. Empty when the picture does not decode.
renderer::RgbaPicture progress_app_icon() {
    static WindowIcon kept;
    static bool decoded = false;
    if (!decoded) {
        decoded = true;
        WindowIcon full;
        std::string error;
        if (decode_window_icon(window_icon_png(), full, error))
            kept = visible_part(full);
    }
    if (kept.pixels.empty())
        return {};
    return {kept.width, kept.height, kept.pixels};
}

/// Dims the frame and draws the dialog in its middle. The canvas already holds
/// the picture behind it.
void paint_progress_dialog(
    paint::Canvas& canvas,
    const kit::Frame& room,
    std::string_view title,
    std::string_view line,
    int32_t progress,
    uint32_t turn
) {
    if (room.area.width <= 0 || room.area.height <= 0)
        return;
    paint::Painter painter(canvas);
    kit::Canvas drawn;
    drawn.smooth.painter = &painter;
    drawn.smooth.fonts = &progress_fonts();
    drawn.smooth.scale_percent = room.scale_percent;
    drawn.icon = progress_app_icon();
    kit::WorkDialog dialog;
    dialog.title = std::string(title);
    dialog.line = std::string(line);
    dialog.progress = progress;
    dialog.turn = turn;
    dialog.size_class = room.size_class;
    kit::blend_rect(drawn, room.area, kit::colour::backdrop, kit::menu_backdrop_opacity);
    const kit::PlacedWorkDialog placed = kit::place_work_dialog(dialog, nullptr);
    const int32_t box = kit::metrics_of(room.size_class).notice_width;
    const int32_t x = room.area.x + (room.area.width - box) / 2;
    const int32_t y = room.area.y + (room.area.height - placed.height) / 2;
    kit::draw_work_dialog(kit::placed_at(drawn, {x, y}), dialog);
}

} // namespace

void LongWork::use_line(std::string_view key) {
    std::lock_guard lock(line_mutex_);
    line_key_ = std::string(key);
}

std::string LongWork::line_key(std::string_view fallback) const {
    std::lock_guard lock(line_mutex_);
    if (line_key_.empty())
        return std::string(fallback);
    return line_key_;
}

bool long_work_running() noexcept {
    return progress_running_flag().load(std::memory_order_acquire);
}

void fill_progress_behind(paint::Canvas& canvas, const renderer::Surface* under) {
    if (canvas.width <= 0 || canvas.height <= 0)
        return;
    paint::Painter painter(canvas);
    const kit::Colour quiet = kit::colour::panel;
    painter.fill_rect(
        {0, 0, canvas.width, canvas.height}, paint::Rgba{quiet.r, quiet.g, quiet.b, 255}
    );
    if (under != nullptr && under->width > 0 && under->height > 0 && !under->rgb.empty())
        blit_behind(canvas, *under);
}

void paint_progress_frame(
    paint::Canvas& canvas,
    const kit::Frame& room,
    std::string_view title,
    std::string_view line,
    int32_t progress,
    uint32_t turn,
    const renderer::Surface* under
) {
    fill_progress_behind(canvas, under);
    paint_progress_dialog(canvas, room, title, line, progress, turn);
}

void show_work_frame(
    SDL_Window* window,
    SDL_Renderer* renderer,
    RendererHost* host,
    std::string_view title,
    std::string_view line
) {
    if (window == nullptr || renderer == nullptr)
        return;
    WindowFrame window_frame(renderer, "the progress dialog");
    paint::Canvas pixels;
    const kit::Frame dialog_frame = progress_room(window, renderer, pixels);
    if (dialog_frame.area.width <= 0 || pixels.width <= 0)
        return;
    fill_progress_behind(pixels, nullptr);
    paint_progress_dialog(pixels, dialog_frame, title, line, -1, 0);
    const RenderState kept(renderer);
    std::ignore = window_frame.show(pixels, kept, host);
}

LongWorkResult run_long_work(
    SDL_Window* window,
    SDL_Renderer* renderer,
    RendererHost* host,
    std::string_view title,
    std::string_view line,
    const std::function<void(paint::Canvas&)>& behind,
    const std::function<void(LongWork&)>& job,
    uint32_t show_after_ms,
    LongWorkClose close
) {
    if (window == nullptr || renderer == nullptr || long_work_running()) {
        LongWork inline_work;
        job(inline_work);
        return {};
    }

    // Copied before the job, which may change the language the catalogue shows.
    std::string title_shown(oa::data::languages::interface_text(title));
    std::string line_shown(oa::data::languages::interface_text(line));
    std::string key_shown(line);
    LongWorkResult result;
    std::exception_ptr failure;
    {
        RunningFlag running;
        LongWork work;
        std::atomic<bool> finished{false};
        std::thread worker([&] {
            try {
                job(work);
            } catch (...) {
                failure = std::current_exception();
            }
            finished.store(true, std::memory_order_release);
        });
        JoinWorker joined{worker};

        const uint32_t began = oa::platform::tick_ms();
        bool showing = false;
        uint32_t shown_at = 0;
        int last_eighth = -1;
        WindowFrame window_frame(renderer, "the progress dialog");
        paint::Canvas pixels;
        while (true) {
            // A close is put back for after the dialog. The window's own
            // events are dropped, though full screen's hold on the pointer
            // still follows them.
            if (drop_window_events(window) && close == LongWorkClose::ends_game) {
                work.stop.store(true, std::memory_order_release);
                std::cerr << "open-annihilation: progress dialog: the window was closed; the "
                             "game ends\n";
                // The read that is waiting cannot be interrupted. Leaving
                // through main would join that read. The renderer records are
                // closed first, so the next start does not count this one
                // as a failure, and the process then ends.
                if (host != nullptr)
                    host->finish_records();
                std::_Exit(0);
            }
            const bool done = finished.load(std::memory_order_acquire);
            const uint32_t now = oa::platform::tick_ms();
            if (std::string published = work.line_key(key_shown); published != key_shown) {
                key_shown = std::move(published);
                line_shown = std::string(oa::data::languages::interface_text(key_shown));
            }
            if (!showing) {
                if (done || now - began < show_after_ms) {
                    if (done)
                        break;
                    SDL_Delay(16);
                    continue;
                }
                showing = true;
                shown_at = now;
                result.shown = true;
                std::cerr << "open-annihilation: progress dialog: shown\n";
                oa::platform::log_files::note_timing("progress dialog shown");
            }
            if (done) {
                title_shown = std::string(oa::data::languages::interface_text(title));
                line_shown = std::string(oa::data::languages::interface_text(key_shown));
            }
            const int32_t amount = bar_amount(work);
            const uint32_t turn = kit::refresh_eighth(now - shown_at);
            if (amount < 0) {
                if (static_cast<int>(turn) != last_eighth) {
                    last_eighth = static_cast<int>(turn);
                    ++result.distinct_turns;
                    std::cerr << "open-annihilation: progress dialog: spinner\n";
                }
            } else {
                result.bar = true;
                if (result.least_bar < 0 || amount < result.least_bar)
                    result.least_bar = amount;
                if (amount > result.most_bar)
                    result.most_bar = amount;
            }
            const kit::Frame dialog_frame = progress_room(window, renderer, pixels);
            if (dialog_frame.area.width > 0 && pixels.width > 0) {
                behind(pixels);
                paint_progress_dialog(pixels, dialog_frame, title_shown, line_shown, amount, turn);
                const RenderState kept(renderer);
                std::ignore = window_frame.show(pixels, kept, host);
            }
            if (done && now - shown_at >= long_work_stay_ms)
                break;
            SDL_Delay(16);
        }
    }
    if (result.shown) {
        std::cerr << "open-annihilation: progress dialog: closed\n";
        oa::platform::log_files::note_timing("progress dialog closed");
    } else {
        std::cerr << "open-annihilation: progress dialog: not shown\n";
    }
    if (failure)
        std::rethrow_exception(failure);
    return result;
}

void require_icloud_download_lines() {
    // The catalogue is read with the archives, after this look. The check
    // reads the file the game ships so the four languages are required here.
    const std::filesystem::path file =
        std::filesystem::path(oa::platform::program_directory()) / "languages" / "interface.tdf";
    std::ifstream input(file, std::ios::binary);
    const std::string bytes(
        std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>{}
    );
    oa::data::languages::InterfaceText catalogue;
    std::string error;
    if (!input || !catalogue.add(bytes, &error))
        throw std::runtime_error(
            "icloud wait: the interface catalogue was not read" +
            (error.empty() ? std::string() : ": " + error)
        );
    const auto fits = [](std::string_view line) {
        kit::WorkDialog dialog;
        dialog.title = "Open Annihilation";
        dialog.line = std::string(line);
        dialog.progress = -1;
        dialog.size_class = kit::SizeClass::compact;
        // The same size the smooth dialog draws the small font at 640x480.
        const auto placed = kit::place_work_dialog(dialog, [](std::string_view text) {
            return paint::text_width(
                progress_fonts(), text, kit::smooth_text_points(kit::FontRole::small), false
            );
        });
        if (placed.line.text != line)
            throw std::runtime_error(
                "icloud wait: the line does not fit the progress dialog: " + std::string(line)
            );
    };
    const std::string_view english = icloud_download_line;
    fits(english);
    int translated_languages = 0;
    for (const oa::data::languages::Language* language : oa::data::languages::known_languages()) {
        const std::string_view tag = language->tag;
        if (tag != "de" && tag != "fr" && tag != "es" && tag != "it")
            continue;
        ++translated_languages;
        const std::string_view translated = catalogue.text(english, *language);
        if (translated == english)
            throw std::runtime_error(
                std::string("icloud wait: ") + std::string(tag) + " has no translation"
            );
        fits(translated);
        std::cout << "icloud wait: " << tag << ": " << translated << "\n";
    }
    if (translated_languages != 4)
        throw std::runtime_error("icloud wait: a built-in language is missing");
}

bool warm_progress_dialog(SDL_Window* window, SDL_Renderer* renderer) noexcept {
    if (window == nullptr || renderer == nullptr)
        return false;
    try {
        paint::Canvas pixels;
        const kit::Frame room = progress_room(window, renderer, pixels);
        if (room.area.width <= 0 || pixels.width <= 0)
            return false;
        fill_progress_behind(pixels, nullptr);
        for (uint32_t turn = 0; turn < 8; ++turn)
            paint_progress_dialog(
                pixels,
                room,
                oa::data::languages::interface_text("Open Annihilation"),
                oa::data::languages::interface_text("Loading the game's archives"),
                -1,
                turn
            );
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace oa::app

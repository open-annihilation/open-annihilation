// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A photo of the battlefield at the window's size, in the render mode in
// use. The picture is queued; the writer thread encodes it and writes it.
#include "oa/app/runtime.hpp"

#include "full_presentation.hpp"
#include "photo_state.hpp"
#include "photo_writer.hpp"
#include "render_run.hpp"
#include "render_state.hpp"

#include "oa/data/languages/interface_text.hpp"
#include "oa/platform/files.hpp"
#include "oa/present/photo/photo_file_name.hpp"
#include "oa/ui/console/console.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/frontend/savegame_dialogs.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace console = oa::ui::console;
namespace fs = std::filesystem;
namespace photo = oa::present::photo;

constexpr const char* kScreenshotFolder = "\\screenshots";
constexpr std::string_view kSaved = "Saved {name}";
constexpr std::string_view kNotSaved = "Photo not saved: {reason}";
constexpr std::string_view kNotInPhoto = "photo mode is not on";
constexpr std::string_view kQueueFull = "four photos are still being saved";
constexpr std::string_view kNotRead = "the picture could not be read";
constexpr std::string_view kDeviceLost = "the graphics device was lost";
constexpr std::string_view kNotWritten = "the file could not be written";
constexpr std::string_view kNotDrawn = "the picture could not be drawn";

/// Puts `value` in place of `token` in `pattern`.
///
/// @param pattern the sentence
/// @param token the placeholder, such as `{name}`
/// @param value the text that replaces it
/// @return the sentence
std::string fill_token(std::string_view pattern, std::string_view token, std::string_view value) {
    std::string text(pattern);
    const std::size_t at = text.find(token);
    if (at == std::string::npos)
        return text;
    text.replace(at, token.size(), value);
    return text;
}

/// The local date and time.
///
/// @return the moment a photo is named with
photo::LocalMoment local_moment() {
    const std::time_t now = std::time(nullptr);
    std::tm parts{};
#if defined(_WIN32)
    localtime_s(&parts, &now);
#else
    localtime_r(&now, &parts);
#endif
    photo::LocalMoment moment;
    moment.year = parts.tm_year + 1900;
    moment.month = parts.tm_mon + 1;
    moment.day = parts.tm_mday;
    moment.hour = parts.tm_hour;
    moment.minute = parts.tm_min;
    moment.second = parts.tm_sec;
    return moment;
}

/// Holds a flag for the rest of the scope, then clears it.
struct FlagHold {
    bool& flag;

    explicit FlagHold(bool& held) : flag(held) { flag = true; }

    FlagHold(const FlagHold&) = delete;
    FlagHold& operator=(const FlagHold&) = delete;

    ~FlagHold() { flag = false; }
};

} // namespace

void Runtime::destroy_photo_writer(PhotoWriter* writer) noexcept {
    delete writer;
}

PhotoWriter& Runtime::photo_files() {
    if (photo_writer_ == nullptr)
        photo_writer_.reset(new PhotoWriter());
    return *photo_writer_;
}

void Runtime::publish_photo_status() {
    if (photo_writer_ == nullptr)
        return;
    while (std::optional<PhotoWriter::Notice> notice = photo_writer_->poll()) {
        if (notice->saved)
            note_photo_saved(notice->file_name);
        else
            note_photo_refused(notice->reason);
    }
}

void Runtime::note_photo_saved(std::string file_name) {
    const std::string line =
        fill_token(oa::data::languages::interface_text(kSaved), "{name}", file_name);
    status_ = line;
    photo_notice_ = std::move(line);
}

void Runtime::note_photo_refused(std::string_view reason) {
    const std::string why(oa::data::languages::interface_text(reason));
    const std::string line =
        fill_token(oa::data::languages::interface_text(kNotSaved), "{reason}", why);
    status_ = line;
    photo_notice_ = line;
}

void Runtime::wait_for_photos() {
    if (photo_writer_ == nullptr)
        return;
    photo_writer_->wait_until_idle();
    publish_photo_status();
}

void Runtime::finish_photo_writes() noexcept {
    if (photo_writer_ == nullptr)
        return;
    try {
        photo_writer_->finish();
        publish_photo_status();
    } catch (...) {
    }
}

fs::path Runtime::photo_shots_folder() {
    if (match_ == nullptr)
        return {};
    const console::Console* con = match_console();
    const std::string output =
        console::output_directory(match_->state().game, con != nullptr ? con->host : nullptr);
    return game_file_path(output + kScreenshotFolder, ui::frontend::SavePathUse::write);
}

bool Runtime::prepare_photo_folder(fs::path& folder) {
    if (match_ == nullptr)
        return false;
    const console::Console* con = match_console();
    const std::string output =
        console::output_directory(match_->state().game, con != nullptr ? con->host : nullptr);
    const std::string shots = output + kScreenshotFolder;
    std::error_code mkdir_error;
    fs::create_directories(game_file_path(output, ui::frontend::SavePathUse::write), mkdir_error);
    mkdir_error.clear();
    fs::create_directories(game_file_path(shots, ui::frontend::SavePathUse::write), mkdir_error);
    folder = game_file_path(shots, ui::frontend::SavePathUse::write);
    std::error_code directory_error;
    return !folder.empty() && fs::is_directory(folder, directory_error);
}

bool Runtime::copy_world_with_player_gamma(
    std::vector<uint8_t>& rgb, uint32_t& width, uint32_t& height
) const {
    width = match_world_cpu_.width;
    height = match_world_cpu_.height;
    const std::size_t pixels = static_cast<std::size_t>(width) * height;
    if (width == 0 || height == 0 || match_world_cpu_.rgb.size() != pixels * 3U)
        return false;
    rgb = match_world_cpu_.rgb;
    apply_gamma_rgb(rgb.data(), pixels, 3);
    return true;
}

bool Runtime::copy_presented_battlefield(
    std::vector<uint8_t>& rgb, uint32_t& width, uint32_t& height
) {
    if (sdl_.renderer == nullptr)
        return false;
    width = match_world_cpu_.width;
    height = match_world_cpu_.height;
    if (width == 0 || height == 0)
        return false;

    SDL_Renderer* drawn_on = sdl_.renderer;
    int logical_width = 0;
    int logical_height = 0;
    SDL_RendererLogicalPresentation mode = SDL_LOGICAL_PRESENTATION_DISABLED;
    SDL_FRect shown{};
    float scale_x = 1.0F;
    float scale_y = 1.0F;
    if (!SDL_GetRenderLogicalPresentation(drawn_on, &logical_width, &logical_height, &mode) ||
        !SDL_GetRenderLogicalPresentationRect(drawn_on, &shown) ||
        !SDL_GetRenderScale(drawn_on, &scale_x, &scale_y))
        return false;
    if (mode != SDL_LOGICAL_PRESENTATION_DISABLED && logical_width > 0 && logical_height > 0) {
        scale_x *= shown.w / static_cast<float>(logical_width);
        scale_y *= shown.h / static_cast<float>(logical_height);
    } else {
        shown.x = 0.0F;
        shown.y = 0.0F;
    }
    const SDL_Rect area{
        static_cast<int>(std::lround(shown.x + static_cast<float>(match_layout_.left) * scale_x)),
        static_cast<int>(std::lround(shown.y + static_cast<float>(match_layout_.top) * scale_y)),
        std::max(1, static_cast<int>(std::lround(static_cast<float>(width) * scale_x))),
        std::max(1, static_cast<int>(std::lround(static_cast<float>(height) * scale_y)))
    };
    using SurfaceOwner = std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)>;
    SurfaceOwner converted(nullptr, SDL_DestroySurface);
    {
        const RenderState state(drawn_on);
        state.use_window_pixels();
        const SurfaceOwner read(SDL_RenderReadPixels(drawn_on, &area), SDL_DestroySurface);
        converted.reset(read ? SDL_ConvertSurface(read.get(), SDL_PIXELFORMAT_RGB24) : nullptr);
    }
    if (!converted || converted->w <= 0 || converted->h <= 0 || converted->pixels == nullptr)
        return false;

    // The size read, the window's pixels. A window at native density is finer
    // than the layout, and the photo keeps that picture rather than one pixel
    // of each block.
    const auto read_w = static_cast<uint32_t>(converted->w);
    const auto read_h = static_cast<uint32_t>(converted->h);
    const auto* pixels = static_cast<const uint8_t*>(converted->pixels);
    const auto pitch = static_cast<std::size_t>(converted->pitch);
    width = read_w;
    height = read_h;
    rgb.resize(static_cast<std::size_t>(read_w) * read_h * 3U);
    for (uint32_t row = 0; row < read_h; ++row)
        std::memcpy(
            rgb.data() + static_cast<std::size_t>(row) * read_w * 3U,
            pixels + static_cast<std::size_t>(row) * pitch,
            static_cast<std::size_t>(read_w) * 3U
        );
    return true;
}

bool Runtime::capture_photo_pixels(
    std::vector<uint8_t>& rgb, uint32_t& width, uint32_t& height, std::string& reason
) {
    reason = std::string(kNotDrawn);
    // A held picture is what photo mode shows while the shared match runs on
    // under it, so the photo is that copy, not a new frame of the live
    // battlefield. Every tier holds its copy before the display gamma, and
    // the screen uploads it through the gamma, so the player's gamma is
    // applied once here. At native density the copy is the layout's size,
    // the picture the window scales up while it is held.
    if (photo_picture_held()) {
        width = photo_->held_width;
        height = photo_->held_height;
        const std::size_t pixels = static_cast<std::size_t>(width) * height;
        if (width == 0 || height == 0 || photo_->held_rgb.size() != pixels * 3U)
            return false;
        rgb = photo_->held_rgb;
        apply_gamma_rgb(rgb.data(), pixels, 3);
        return true;
    }
    if (live_renderer_active()) {
        try {
            const FlagHold capture(photo_capture_live_);
            if (!render_live_match_surface() || live_renderer_failed())
                return false;
        } catch (const std::exception&) {
            return false;
        }
        if (!copy_world_with_player_gamma(rgb, width, height))
            return false;
        return true;
    }
    if (full_presentation()) {
        try {
            const FlagHold reading(accelerated_.reading_back);
            if (!present_full_match_layers(false) || full_ == nullptr || !full_->drawn)
                return false;
            if (!copy_presented_battlefield(rgb, width, height)) {
                reason = std::string(kNotRead);
                return false;
            }
        } catch (const std::exception&) {
            return false;
        }
        return true;
    }
    // Off and Basic save the battlefield the last frame left, which is what
    // photo mode shows, and apply the player's gamma once. Below zoom 1 that
    // picture is the area pass. Drawing the frame again would replace it.
    if (!copy_world_with_player_gamma(rgb, width, height))
        return false;
    return true;
}

void Runtime::take_photo() {
    publish_photo_status();
    if (!photo_mode() || match_ == nullptr) {
        note_photo_refused(kNotInPhoto);
        return;
    }
    if (photo_files().outstanding() >= 4) {
        note_photo_refused(kQueueFull);
        return;
    }
    const bool on_device = live_renderer_active() || full_presentation();
    if (on_device && render_run_ != nullptr && render_run_->device_lost) {
        note_photo_refused(kDeviceLost);
        return;
    }

    std::vector<uint8_t> rgb;
    uint32_t width = 0;
    uint32_t height = 0;
    std::string reason;
    if (!capture_photo_pixels(rgb, width, height, reason)) {
        note_photo_refused(reason);
        return;
    }

    fs::path folder;
    if (!prepare_photo_folder(folder)) {
        note_photo_refused(kNotWritten);
        return;
    }
    const photo::LocalMoment moment = local_moment();
    const std::size_t folder_characters = folder.native().size();
    const std::size_t path_budget = oa::platform::longest_path();
    std::string file_name;
    fs::path path;
    bool named = false;
    for (int copy = 1; copy < 10000; ++copy) {
        file_name = photo::photo_file_name(
            moment, selected_map_name_runtime_, copy, folder_characters, path_budget
        );
        path = folder / oa::platform::path_from_utf8(file_name);
        std::error_code exists_error;
        const bool exists = fs::exists(path, exists_error);
        if (!exists && !photo_files().holds(file_name)) {
            named = true;
            break;
        }
    }
    if (!named) {
        note_photo_refused(kNotWritten);
        return;
    }

    PhotoWriter::Picture picture;
    picture.rgb = std::move(rgb);
    picture.width = width;
    picture.height = height;
    picture.path = std::move(path);
    picture.file_name = file_name;
    if (!photo_files().enqueue(std::move(picture)))
        note_photo_refused(kQueueFull);
}

} // namespace oa::app

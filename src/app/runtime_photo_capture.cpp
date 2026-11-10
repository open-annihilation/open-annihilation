// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A photo of the battlefield at the window's size, in the render mode in
// use. The picture is queued; the writer thread encodes it and writes it.
#include "oa/app/runtime.hpp"

#include "oa/app/hook_call.hpp"

#include "full_presentation.hpp"
#include "photo_clipboard.hpp"
#include "photo_state.hpp"
#include "photo_writer.hpp"
#include "render_run.hpp"
#include "render_state.hpp"

#include "oa/base/sha256.hpp"
#include "oa/core/player.h"
#include "oa/core/player_setup.h"
#include "oa/core/world.h"
#include "oa/data/defs/layout.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/platform/files.hpp"
#include "oa/platform/machine.hpp"
#include "oa/present/photo/looks.hpp"
#include "oa/present/photo/photo_file_name.hpp"
#include "oa/present/photo/photo_texts.hpp"
#include "oa/ui/photo_panel/crop.hpp"
#include "oa/sim/simulation_state.hpp"
#include "oa/ui/console/console.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/frontend/savegame_dialogs.hpp"

#include <SDL3/SDL.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
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
/// Absent, or any text other than 0, names the players in a photo. 0 leaves
/// the names out. Written only when the player changes it.
constexpr std::string_view kPlayerNamesPreference = "open-annihilation.photo.player-names";

#ifndef OA_ENGINE_VERSION
#error OA_ENGINE_VERSION is set by the build
#endif

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

/// The UTC date and time a photo's Creation Time records.
///
/// @return the moment
photo::UtcMoment utc_moment() {
    const std::time_t now = std::time(nullptr);
    std::tm parts{};
#if defined(_WIN32)
    gmtime_s(&parts, &now);
#else
    gmtime_r(&now, &parts);
#endif
    photo::UtcMoment moment;
    moment.year = parts.tm_year + 1900;
    moment.month = parts.tm_mon + 1;
    moment.day = parts.tm_mday;
    moment.hour = parts.tm_hour;
    moment.minute = parts.tm_min;
    moment.second = parts.tm_sec;
    return moment;
}

/// The kind of battle the photo's Source line names.
///
/// @param bits the extension state
/// @param campaign true while a campaign mission is running
/// @return the kind
photo::GameKind photo_game_kind(uint32_t bits, bool campaign) noexcept {
    if ((bits & extension_state::replay) != 0)
        return photo::GameKind::replay;
    if ((bits & extension_state::shared_match) != 0)
        return photo::GameKind::multiplayer;
    if (campaign)
        return photo::GameKind::campaign;
    return photo::GameKind::skirmish;
}

/// The players a photo records, watchers left out. A name stops at the first
/// NUL and is never longer than the slot.
///
/// @param world the match
/// @return the players, in slot order
std::vector<photo::PlayerFact> photo_players(const oa::World& world) {
    std::vector<photo::PlayerFact> players;
    for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot) {
        const oa::Player& player = world.game.players[slot];
        if (!oa::sim::simulation_state::player_active(player))
            continue;
        const oa::PlayerSetupInfo* info = oa::world_player_info(&world, &player);
        if (info != nullptr && (info->options & OA_SETUP_OPTION_WATCHER) != 0)
            continue;
        photo::PlayerFact fact;
        const void* end = std::memchr(player.name, 0, sizeof player.name);
        const std::size_t length =
            end == nullptr ? sizeof player.name
                           : static_cast<std::size_t>(static_cast<const char*>(end) - player.name);
        fact.name.assign(player.name, length);
        if (info != nullptr) {
            fact.side = oa::data::defs::side_name(info->side);
            fact.colour = info->color;
        }
        // The game's team 0 is team 1. OA_PLAYER_NO_TEAM, and anything past
        // it, is no team.
        if (player.team < OA_PLAYER_NO_TEAM)
            fact.team = static_cast<int>(player.team) + 1;
        players.push_back(std::move(fact));
    }
    return players;
}

/// The event posted when a photo's notice is ready. 0 until it is registered.
uint32_t photo_notice_event = 0;

/// How long to wait while a photo is still being written.
constexpr int kPhotoNoticeWaitMs = 50;

/// Posts the notice event. The writer's thread calls this.
void push_photo_notice() noexcept {
    if (photo_notice_event == 0)
        return;
    SDL_Event wake{};
    wake.type = photo_notice_event;
    (void)SDL_PushEvent(&wake);
}

/// Holds a flag for the rest of the scope, then clears it.
struct FlagHold {
    bool& flag;

    explicit FlagHold(bool& held) : flag(held) { flag = true; }

    FlagHold(const FlagHold&) = delete;
    FlagHold& operator=(const FlagHold&) = delete;

    ~FlagHold() { flag = false; }
};

/// The copy note in the player's language. Empty when the copy was skipped.
///
/// @param report what the offer did
/// @return "Copied", "Photo not copied" and the reason, or empty
std::string copy_note(const PhotoCopyReport& report) {
    if (report.result == PhotoCopyResult::copied)
        return std::string(oa::data::languages::interface_text(kPhotoCopied));
    if (report.result != PhotoCopyResult::refused || report.reason.empty())
        return {};
    const std::string why(oa::data::languages::interface_text(report.reason));
    return fill_token(oa::data::languages::interface_text(kPhotoNotCopied), "{reason}", why);
}

} // namespace

void Runtime::destroy_photo_writer(PhotoWriter* writer) noexcept {
    delete writer;
}

PhotoWriter& Runtime::photo_files() {
    // Before the thread starts, so a finished photo can wake the wait.
    arm_photo_notice_wake();
    if (photo_writer_ == nullptr)
        photo_writer_.reset(new PhotoWriter());
    return *photo_writer_;
}

void Runtime::arm_photo_notice_wake() {
    if (photo_notice_event != 0)
        return;
    const uint32_t wake = SDL_RegisterEvents(1);
    if (wake == 0)
        return;
    photo_notice_event = wake;
    set_photo_writer_wake(push_photo_notice);
}

bool Runtime::is_photo_notice_event(uint32_t type) const noexcept {
    return photo_notice_event != 0 && type == photo_notice_event;
}

int Runtime::photo_idle_wait_ms() const {
    if (photo_writer_ == nullptr || photo_writer_->outstanding() == 0)
        return -1;
    return kPhotoNoticeWaitMs;
}

void Runtime::publish_photo_status() {
    if (photo_writer_ == nullptr)
        return;
    const bool image = photo_clipboard_has_image(this_photo_clipboard_system());
    const bool game_mode = oa::platform::running_in_steam_game_mode();
    while (std::optional<PhotoWriter::Notice> notice = photo_writer_->poll()) {
        if (!notice->saved) {
            note_photo_refused(notice->reason);
            continue;
        }
        PhotoClipboardImage prepared;
        prepared.bytes = std::move(notice->clipboard);
        prepared.mime = notice->clipboard_mime;
        prepared.refusal = notice->copy_refusal;
        const PhotoCopyReport report = offer_photo_clipboard(
            notice->file_name, std::move(prepared), photo_copy(), game_mode, image
        );
        note_photo_saved(std::move(notice->file_name), copy_note(report));
    }
}

bool Runtime::photo_copy() const {
    const auto found = preference_values_.find(std::string(kPhotoCopyPreference));
    if (found == preference_values_.end())
        return true;
    return photo_copy_on(found->second);
}

void Runtime::set_photo_copy(bool on) {
    const std::string key(kPhotoCopyPreference);
    const std::string value = on ? "1" : "0";
    const auto found = preference_values_.find(key);
    if (found != preference_values_.end() && found->second == value)
        return;
    preference_values_[key] = value;
    preferences_dirty_ = true;
    if (!preference_path_.empty())
        flush_preferences();
}

void Runtime::copy_last_photo() {
    if (!remembered_photo_exists())
        return;
    const std::string file_name(remembered_photo_name());
    // Copy to clipboard governs only the copy after each photo. An explicit
    // Copy copies whatever that switch says, and says so when it cannot.
    const bool game_mode = oa::platform::running_in_steam_game_mode();
    const bool image = photo_clipboard_has_image(this_photo_clipboard_system());
    if (game_mode || !image) {
        PhotoCopyReport refused;
        refused.result = PhotoCopyResult::refused;
        refused.reason = game_mode ? kPhotoCopyGameMode : kPhotoCopyTextOnly;
        note_photo_saved(file_name, copy_note(refused));
        return;
    }
    const PhotoCopyReport report = copy_remembered_photo(true, game_mode, image);
    if (report.result == PhotoCopyResult::skipped)
        return;
    note_photo_saved(file_name, copy_note(report));
}

void Runtime::note_photo_saved(std::string file_name, std::string copy_note_text) {
    std::string line = fill_token(oa::data::languages::interface_text(kSaved), "{name}", file_name);
    if (!copy_note_text.empty()) {
        line += "  ";
        line += copy_note_text;
    }
    status_ = line;
    photo_notice_ = std::move(line);
    const bool copied = copy_note_text == oa::data::languages::interface_text(kPhotoCopied);
    show_photo_toast(file_name, copied);
}

void Runtime::note_photo_refused(std::string_view reason) {
    const std::string why(oa::data::languages::interface_text(reason));
    const std::string line =
        fill_token(oa::data::languages::interface_text(kNotSaved), "{reason}", why);
    status_ = line;
    photo_notice_ = line;
    clear_photo_thumbnail();
    show_photo_toast(line, false);
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
    if (photo_ != nullptr) {
        photo_->sized_by_full = false;
        photo_->sized_by_live = false;
        photo_->sized_supersample = 1;
        photo_->poster_recorded = false;
    }
    // A poster is the view, a marked area or the whole map, at one map pixel
    // a picture pixel. It is drawn again. A held picture stays the copy.
    if (photo_ != nullptr && photo_->poster && !photo_picture_held())
        return capture_poster(rgb, width, height);
    // Screen with smooth edges off keeps the picture the window already
    // drew. A larger size, or smooth edges, is drawn again at that size.
    // A held picture stays the copy photo mode is showing.
    // A screen-sized photo is the picture the window already drew. A check
    // that compares that picture with tiles draws it again, the same way.
    if (photo_ != nullptr && !photo_picture_held() &&
        (photo_->size_factor != 1 || photo_->smooth_factor != 1 || photo_check_sized_capture_))
        return capture_sized_photo(rgb, width, height);
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

bool Runtime::photo_player_names() const {
    const auto found = preference_values_.find(std::string(kPlayerNamesPreference));
    if (found == preference_values_.end())
        return true;
    return found->second != "0";
}

void Runtime::set_photo_player_names(bool on) {
    if (photo_player_names() == on)
        return;
    if (on)
        preference_values_.erase(std::string(kPlayerNamesPreference));
    else
        preference_values_[std::string(kPlayerNamesPreference)] = "0";
    preferences_dirty_ = true;
    flush_preferences();
}

void Runtime::deliver_pending_photo() noexcept {
    if (photo_ == nullptr || !photo_->capture_waiting)
        return;
    if (!photo_->drawn_since_enter && !photo_->picture_held)
        return;
    try {
        take_photo();
    } catch (const std::exception&) {
        if (photo_ != nullptr)
            photo_->capture_waiting = false;
    }
}

void Runtime::take_photo() {
    publish_photo_status();
    if (!photo_mode() || match_ == nullptr) {
        note_photo_refused(kNotInPhoto);
        return;
    }
    // The play layout is what Full last presented, and what Off and Basic
    // last drew, until a photo-mode frame has been presented. Keep the
    // request and take it once that frame is up. A held picture is already
    // the frame photo mode shows, so it does not wait. Enter, Ctrl+F9 and
    // a later button all come through here.
    if (photo_ != nullptr && !photo_->drawn_since_enter && !photo_picture_held()) {
        photo_->capture_waiting = true;
        return;
    }
    if (photo_ != nullptr)
        photo_->capture_waiting = false;
    if (photo_ != nullptr)
        snap_photo_factors();
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
    int32_t crop_x = 0;
    int32_t crop_y = 0;
    // Before the crop. A Screen photo at a pixel density above 1 is the
    // window's pixels, wider than the battlefield. The look follows that
    // width, and a crop keeps the same factor.
    const uint32_t framed_width = width;
    if (photo_ != nullptr) {
        const auto guide = static_cast<oa::ui::photo_panel::Guide>(photo_->guide);
        const oa::ui::kit::Rect crop = oa::ui::photo_panel::crop_bounds(
            static_cast<int32_t>(width), static_cast<int32_t>(height), guide
        );
        if (crop.width > 0 && crop.height > 0) {
            crop_x = crop.x;
            crop_y = crop.y;
        }
        oa::ui::photo_panel::crop_centred(rgb, width, height, guide);
    }
    // After the crop, so every size that is saved through here carries the
    // look, and before the thumbnail, so the notice matches the file. A
    // photo larger than the battlefield draws its grain larger by its size
    // factor, so the grain covers the map as the preview's does.
    // A poster's grain and tilt-shift band grow by the poster's pixels over
    // the preview's for the same ground. A poster's pixel is a map pixel and
    // the preview shows the view's map pixels across the battlefield, so the
    // factor is the view's map width over the battlefield's (one over the
    // zoom), with photo_look_scale's rounding, whatever the poster's width,
    // and it is not capped at 4. Haze stays the size the panel shows,
    // painted from the poster's corner. Depth of field stays on the panel's
    // factor, which a poster keeps at 1.
    const bool poster_scale = photo_ != nullptr && photo_->poster && !photo_picture_held();
    const int size_factor = poster_scale ? 1
                                         : photo_look_scale(
                                               photo_ != nullptr && !photo_picture_held()
                                                   ? std::clamp(photo_->size_factor, 1, 4)
                                                   : 1,
                                               framed_width,
                                               match_layout_.battlefield_width()
                                           );
    const int grain_times =
        poster_scale
            ? photo_look_scale(
                  1, static_cast<uint32_t>(visible_map_width()), match_layout_.battlefield_width()
              )
            : size_factor;
    if (photo_look_active()) {
        // A poster's haze follows the poster's ground, from its corner at
        // zoom 1, wherever the window's view is.
        std::optional<std::array<int32_t, 2>> poster_corner;
        if (photo_ != nullptr && photo_->poster_recorded && !photo_picture_held())
            poster_corner =
                std::array<int32_t, 2>{photo_->poster_recorded_x, photo_->poster_recorded_z};
        apply_photo_look_rgb(
            rgb,
            width,
            height,
            size_factor,
            crop_x,
            crop_y,
            poster_corner,
            poster_scale ? grain_times : 0
        );
    }
    remember_photo_thumbnail(rgb, width, height);

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

    photo::PhotoFacts facts;
    facts.map = selected_map_name_runtime_;
    facts.engine = OA_ENGINE_VERSION;
    if (const oa::data::mod_profile::ModProfile* profile = mod_profile()) {
        facts.mod_id = profile->id;
        facts.mod_version = profile->version;
    }
    facts.kind = photo_game_kind(current_extension_state(), campaign_mission_);
    if (photo_picture_held()) {
        facts.tick = photo_->held_tick;
        facts.speed = photo_->held_speed;
    } else {
        facts.tick = match_->state().game.tick;
        facts.speed = match_->state().game.current_speed;
    }
    facts.players = photo_players(match_->state());
    const bool poster_picture =
        photo_ != nullptr && photo_->poster_recorded && !photo_picture_held();
    if (photo_ != nullptr && photo_->picture_held && photo_->camera_held) {
        facts.camera.x = photo_->held_camera_x;
        facts.camera.z = photo_->held_camera_z;
        facts.camera.zoom = photo_->held_zoom;
        facts.camera.yaw = photo_->held_live_yaw;
        facts.camera.pitch = photo_->held_live_pitch;
    } else if (poster_picture) {
        facts.camera.x = photo_->poster_recorded_x;
        facts.camera.z = photo_->poster_recorded_z;
        facts.camera.zoom = 1.0F;
    } else {
        facts.camera.x = match_camera_x_;
        facts.camera.z = match_camera_z_;
        facts.camera.zoom = match_zoom_;
        if (live_renderer_active()) {
            const std::array<float, 2> angles = live_camera_angles();
            facts.camera.yaw = angles[0];
            facts.camera.pitch = angles[1];
        }
    }
    facts.size.width = width;
    facts.size.height = height;
    const bool sized = photo_ != nullptr && !photo_picture_held() && !poster_picture &&
                       (photo_->size_factor != 1 || photo_->smooth_factor != 1);
    facts.size.smooth = sized ? photo_->smooth_factor : 1;
    facts.created = utc_moment();
    facts.player_names = photo_player_names();
    const bool card_dof = photo_ != nullptr && photo_->card_dof_pixels > 0;
    const bool card_haze = photo_ != nullptr && photo_->card_haze > 0;
    const bool classic_haze = photo_ != nullptr && !live_renderer_active() && photo_->look_haze > 0;
    if ((photo_look_active() || card_dof || card_haze) && photo_ != nullptr) {
        photo::LookFact fact;
        const int preset = std::clamp(static_cast<int>(photo_->look_preset), 0, 5);
        fact.preset = photo::preset_key(static_cast<photo::Preset>(preset));
        fact.vignette = std::clamp(photo_->look_vignette, 0, 100);
        fact.roundness = std::clamp(photo_->look_roundness, 0, 100);
        fact.grain = std::clamp(photo_->look_grain, 0, 100);
        fact.grain_size = std::clamp(photo_->look_grain_size, 1, 16);
        fact.grain_pixels = photo::photo_grain_size(fact.grain_size, grain_times);
        const int frame = std::clamp(static_cast<int>(photo_->look_frame), 0, 3);
        fact.frame = photo::frame_key(static_cast<photo::Frame>(frame));
        if (photo_->look_tilt_radius > 0) {
            photo::TiltShiftFact tilt;
            tilt.centre = std::clamp(photo_->look_tilt_centre, 0, 100);
            tilt.width = std::clamp(photo_->look_tilt_width, 0, 160);
            tilt.width_pixels = photo::photo_tilt_width(tilt.width, grain_times);
            tilt.radius = std::clamp(photo_->look_tilt_radius, 0, 32);
            tilt.radius_pixels = photo::photo_tilt_radius(tilt.radius, grain_times);
            fact.tilt_shift = tilt;
        }
        if (classic_haze || card_haze)
            fact.haze = std::clamp(photo_->look_haze, 0, 100);
        if (card_dof) {
            photo::DepthOfFieldFact field;
            field.radius = std::clamp(photo_->look_depth_of_field, 0, 32);
            // The saved picture's blur: the panel's times the photo's size.
            // The card drew it times smooth edges' step too, before the
            // reduction, and the frame after the photo is the preview's.
            field.radius_pixels = photo::photo_tilt_radius(field.radius, size_factor);
            fact.depth_of_field = field;
        }
        facts.look = std::move(fact);
    }
    // Passed so a photo that stored it would fail the capture check. The
    // text builder never writes a folder.
    facts.shots_folder = oa::platform::path_to_generic_utf8(folder);
    if (photo_time_is_replay() && !replay_recording_path_.empty()) {
        photo::ReplayFact fact;
        fact.file = oa::platform::path_to_utf8(replay_recording_path_.filename());
        const std::string path_key = oa::platform::path_to_utf8(replay_recording_path_);
        if (photo_ != nullptr && photo_->replay_digest_path == path_key &&
            !photo_->replay_digest_hex.empty()) {
            fact.sha256 = photo_->replay_digest_hex;
        } else {
            std::ifstream recording(replay_recording_path_, std::ios::binary);
            if (recording) {
                const std::vector<uint8_t> recording_bytes{
                    std::istreambuf_iterator<char>(recording), std::istreambuf_iterator<char>()
                };
                if (!recording_bytes.empty()) {
                    const oa::base::sha256::Digest digest =
                        oa::base::sha256::digest_of(recording_bytes);
                    const std::array<char, oa::base::sha256::hex_size> hex =
                        oa::base::sha256::to_hex(digest);
                    fact.sha256.assign(hex.data(), hex.size());
                    if (photo_ != nullptr) {
                        photo_->replay_digest_hex = fact.sha256;
                        photo_->replay_digest_path = path_key;
                    }
                }
            }
        }
        ReplayPlaybackView replay_now{};
        const bool filled = call_hook_or_report<&Extension::replay_state>(
            extension_, hook_error_report(), *this, &replay_now
        );
        const uint32_t replay_tick = filled ? replay_now.tick : match_->state().game.tick;
        fact.tick = static_cast<int64_t>(replay_tick);
        facts.replay = std::move(fact);
    }

    PhotoWriter::Picture picture;
    picture.rgb = std::move(rgb);
    picture.width = width;
    picture.height = height;
    picture.path = std::move(path);
    picture.file_name = file_name;
    picture.texts = photo::photo_texts(facts);
    if (!photo_files().enqueue(std::move(picture)))
        note_photo_refused(kQueueFull);
}

} // namespace oa::app

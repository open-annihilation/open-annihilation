// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Director mode: the running match presented for a director script
// (director_presentation.hpp). Its frames are drawn from the director's
// camera at the output size, the battlefield alone unless the interface is
// asked for; the match's sounds and every player's unit announcements go to
// the director's sound hooks. --check-director-view checks it against the
// same match replayed in director mode undrawn.
#include "oa/app/runtime.hpp"
#include "oa/core/map_plot.h"
#include "director_state.hpp"

#include "oa/audio/game_audio.hpp"
#include "oa/audio/mixer.hpp"
#include "oa/base/sha256.hpp"
#include "oa/media/director.hpp"
#include "oa/present/palette_tables.hpp"
#include "oa/present/world_renderer/world_camera.hpp"
#include "oa/ui/console/game_fields.hpp"
#include "oa/ui/display_layout.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace game_audio = oa::audio::game_audio;
using oa::media::director::EngineView;

/// Bytes of one pixel of a frame: red, green and blue.
constexpr size_t kPixelBytes = 3;
/// The widest and tallest frame director mode draws, in pixels.
constexpr uint32_t kMaxFrameSide = 16384;
/// The most map pixels a drawn frame may span across or down: more than any
/// map holds.
constexpr double kMaxDrawnMapPixels = 65536.0;
/// The display gamma of the Gamma setting a new player starts with (12,
/// shown as 12 / 24 + 0.5): every channel as drawn.
constexpr float kDefaultDisplayGamma = 1.0F;
/// The unit chat level a new player starts with: every announcement
/// priority is heard.
constexpr uint8_t kDefaultUnitChat = 10;
/// How director mode presents unit announcements: with sound at the default
/// unit chat level, the speech switch on, no caption and each unit's own
/// sounds.
constexpr game_audio::AnnouncementPresentationGates kDirectorAnnouncementGates{
    kDefaultUnitChat, 0, true, true, false, false, game_audio::novelty_voice_sounds
};

/// Copies part of a drawn frame into a director frame through a gamma table.
///
/// @param source the drawn frame, three bytes a pixel
/// @param x the part's left column in `source`
/// @param y the part's top row in `source`
/// @param width the part's width, the director frame's
/// @param height the part's height, the director frame's
/// @param table the gamma table; null leaves every channel as drawn
/// @param[out] rgb the director frame, width * height * 3 bytes
void copy_frame(
    const renderer::Surface& source,
    uint32_t x,
    uint32_t y,
    uint32_t width,
    uint32_t height,
    const std::array<uint8_t, 256>* table,
    std::span<uint8_t> rgb
) {
    if (static_cast<uint64_t>(x) + width > source.width ||
        static_cast<uint64_t>(y) + height > source.height ||
        source.rgb.size() != static_cast<size_t>(source.width) * source.height * kPixelBytes)
        throw std::logic_error("the drawn frame does not cover the director's frame");
    const size_t row_bytes = static_cast<size_t>(width) * kPixelBytes;
    for (uint32_t row = 0; row < height; ++row) {
        const uint8_t* from =
            source.rgb.data() + ((static_cast<size_t>(y) + row) * source.width + x) * kPixelBytes;
        uint8_t* to = rgb.data() + static_cast<size_t>(row) * row_bytes;
        if (table == nullptr) {
            std::memcpy(to, from, row_bytes);
            continue;
        }
        for (size_t byte = 0; byte < row_bytes; ++byte)
            to[byte] = (*table)[from[byte]];
    }
}

} // namespace

void Runtime::destroy_director_state(DirectorState* state) noexcept {
    delete state;
}

bool Runtime::director_mode() const noexcept {
    return director_ != nullptr;
}

void Runtime::enter_director_mode(
    const DirectorPresentation& presentation, const DirectorSoundHooks& sounds
) {
    if (!match_ || !selected_tnt_)
        throw std::logic_error("director mode needs a running match");
    if (director_ != nullptr)
        throw std::logic_error("the match is in director mode already");
    if (presentation.width == 0 || presentation.height == 0 || presentation.width > kMaxFrameSide ||
        presentation.height > kMaxFrameSide)
        throw std::invalid_argument(
            "director frames are 1 to " + std::to_string(kMaxFrameSide) + " pixels a side, not " +
            std::to_string(presentation.width) + "x" + std::to_string(presentation.height)
        );
    std::unique_ptr<DirectorState, void (*)(DirectorState*) noexcept> state{
        new DirectorState{}, destroy_director_state
    };
    state->presentation = presentation;
    state->sounds = sounds;
    state->layout = match_layout_;
    state->camera_x = match_camera_x_;
    state->camera_z = match_camera_z_;
    state->zoom = match_zoom_;
    state->zoom_target = match_zoom_target_;
    state->tracking = match_tracking_;
    state->tracked_unit = tracked_match_unit_;
    state->point_sound = match_->point_sound;
    state->named_sound = match_->named_sound;
    for (size_t channel = 0; channel < state->gamma_table.size(); ++channel) {
        state->gamma_table[channel] =
            oa::present::gamma_channel(static_cast<uint8_t>(channel), kDefaultDisplayGamma);
        state->gamma_identity = state->gamma_identity && state->gamma_table[channel] == channel;
    }
    director_ = std::move(state);

    namespace layout = oa::ui::display_layout;
    const auto width = static_cast<int>(presentation.width);
    const auto height = static_cast<int>(presentation.height);
    match_layout_ = presentation.show_interface ? lay_out_match(width, height)
                                                : layout::make_battlefield_layout(width, height);
    match_tracking_ = false;
    tracked_match_unit_ = 0;
    terrain_cache_cam_x_ = kUncachedTerrainCamera;
    // Every point sound is placed, as with 3D sound on, and heard through
    // the hooks (play_point_sound).
    match_->point_sound = {
        this,
        [](void*) { return true; },
        [](
            void* context, const char* name, const oa::sim::match_runtime::Match::PointSound& sound
        ) { static_cast<Runtime*>(context)->play_point_sound(name, sound); }
    };
    // Sound-table sounds the match plays unplaced, such as a mission's, go
    // to the hooks unplaced; their files are still looked up as before.
    match_->named_sound = {
        this,
        [](void* context, const char* name) {
            auto& self = *static_cast<Runtime*>(context);
            if (self.director_ == nullptr || !self.match_)
                return;
            const auto* sound = self.audio_registry_.get(self.audio_registry_.find(name));
            if (sound == nullptr)
                return;
            ++self.director_->tally.unplaced_sounds;
            self.director_->send(
                {sound->resource.c_str(),
                 oa::audio::volume_near,
                 false,
                 0,
                 0,
                 0,
                 0.0F,
                 0.0F,
                 0,
                 self.match_->simulation().tick}
            );
        },
        [](void* context, const char* name) -> const char* {
            const auto& self = *static_cast<Runtime*>(context);
            if (self.director_ == nullptr)
                return nullptr;
            const auto& kept = self.director_->named_sound;
            return kept.file != nullptr ? kept.file(kept.context, name) : nullptr;
        }
    };
    if (presentation.every_player_speaks)
        offline_services_.set_announcement_hooks(
            {this,
             [](void* context, uint8_t owner, const game_audio::AnnouncementRequest& request) {
                 auto& self = *static_cast<Runtime*>(context);
                 if (self.director_ == nullptr || owner >= OA_PLAYER_COUNT)
                     return;
                 auto& director = *self.director_;
                 ++director.tally.announcements_heard;
                 director.tally.speaking_players |= 1U << owner;
                 // Each player's queue takes its own units' announcements as
                 // the game's queue takes the viewing player's.
                 auto own = request;
                 own.local_owner = true;
                 auto& queue = director.announcements[owner];
                 const bool was_full = queue.size() == game_audio::AnnouncementQueue::capacity;
                 const auto result = queue.enqueue(own);
                 if (result != game_audio::AnnouncementEnqueueStatus::queued)
                     return;
                 ++director.tally.announcements_queued;
                 // The record a full queue evicts is presented without sound,
                 // and its caption is not shown.
                 if (was_full)
                     std::ignore = queue.present_evicted(
                         self.unit_sound_catalog_,
                         kDirectorAnnouncementGates,
                         director.next_random(),
                         request.tick
                     );
             }}
        );
    offline_services_.set_on_screen_test(&DirectorState::unit_in_view, &match_->state());
    // The graphics card's renderer last, so that a card that cannot be
    // opened leaves director mode as it found the match.
    if (presentation.full) {
        try {
            open_director_card();
        } catch (...) {
            leave_director_mode();
            throw;
        }
    }
}

void Runtime::leave_director_mode() noexcept {
    if (director_ == nullptr)
        return;
    close_director_card(*director_);
    const auto state = std::move(director_);
    match_layout_ = state->layout;
    match_camera_x_ = state->camera_x;
    match_camera_z_ = state->camera_z;
    match_zoom_ = state->zoom;
    match_zoom_target_ = state->zoom_target;
    exact_view_.held = false;
    match_tracking_ = state->tracking;
    tracked_match_unit_ = state->tracked_unit;
    terrain_cache_cam_x_ = kUncachedTerrainCamera;
    offline_services_.set_announcement_hooks({});
    if (!match_)
        return;
    match_->point_sound = state->point_sound;
    match_->named_sound = state->named_sound;
    // The Game block's camera and the on-screen list follow the player's
    // camera again, and so does the under-attack notice.
    try {
        refresh_on_screen_view();
    } catch (...) {
        offline_services_.set_on_screen_test(nullptr, nullptr);
    }
}

void Runtime::set_director_view(const EngineView& view) {
    if (director_ == nullptr)
        throw std::logic_error("the director's camera is set in director mode only");
    const auto& presentation = director_->presentation;
    const double drawn_width = static_cast<double>(presentation.width) + view.margin;
    const double drawn_height = static_cast<double>(presentation.height) + view.margin;
    if (!std::isfinite(view.zoom) || !(view.zoom > 0.0) || view.left < 0 || view.top < 0 ||
        view.visible_width <= 0 || view.visible_height <= 0 || view.margin == 0 ||
        view.offset_x > view.margin || view.offset_y > view.margin ||
        drawn_width / view.zoom > kMaxDrawnMapPixels ||
        drawn_height / view.zoom > kMaxDrawnMapPixels)
        throw std::invalid_argument("the director's camera does not draw a view on the map");
    director_->view = view;
    director_->view_set = true;
    match_camera_x_ = view.left;
    match_camera_z_ = view.top;
    match_zoom_ = static_cast<float>(view.zoom);
    match_zoom_target_ = match_zoom_;
    exact_view_.held = false;
    match_tracking_ = false;
    tracked_match_unit_ = 0;
}

void Runtime::bind_director_view() {
    if (director_ == nullptr || !director_->view_set || !match_)
        throw std::logic_error("the director's camera is bound once set, in director mode");
    namespace wr = oa::present::world_renderer;
    namespace layout = oa::ui::display_layout;
    const auto& view = director_->view;
    auto& game = match_->state().game;
    game.camera_x = static_cast<uint32_t>(view.left);
    game.camera_y = static_cast<uint32_t>(view.top);
    game.view_cells_width = view.visible_width / OA_MAP_CELL_PIXELS;
    game.view_cells_height = view.visible_height / OA_MAP_CELL_PIXELS;
    // As bind_match_view: the screen is the view with the side column and
    // bars around it, and the game view lies at (128, 32) on it.
    game.offscreen_width = static_cast<uint32_t>(layout::kSourceLeft + view.visible_width);
    game.offscreen_height =
        static_cast<uint32_t>(layout::kSourceTop + view.visible_height + layout::kSourceBottom);
    game.battlefield_rect = oa::Rect32{
        layout::kSourceLeft,
        layout::kSourceTop,
        layout::kSourceLeft + view.visible_width - 1,
        layout::kSourceTop + view.visible_height - 1
    };
    oa::Rect32 radar_view{};
    if (wr::radar_view_rect(game, radar_view) &&
        std::memcmp(&radar_view, &game.radar_view_rect, sizeof radar_view) != 0) {
        game.radar_view_rect = radar_view;
        game.radar_blink_flags =
            static_cast<uint16_t>(game.radar_blink_flags | wr::radar_flag_redraw);
    }
    offline_services_.set_on_screen_test(&DirectorState::unit_in_view, &match_->state());
}

void Runtime::present_director_announcements() {
    if (director_ == nullptr || !director_->presentation.every_player_speaks || !match_)
        return;
    namespace wr = oa::present::world_renderer;
    auto& director = *director_;
    const auto& world = match_->state();
    const auto& game = world.game;
    const uint32_t tick = match_->simulation().tick;
    for (size_t owner = 0; owner < director.announcements.size(); ++owner) {
        auto& queue = director.announcements[owner];
        if (queue.size() == 0)
            continue;
        const auto event = queue.pump(
            unit_sound_catalog_, kDirectorAnnouncementGates, director.next_random(), tick
        );
        if (!event || !event->sound_resource)
            continue;
        std::string resource = *event->sound_resource;
        std::replace(resource.begin(), resource.end(), '\\', '/');
        DirectorSound sound{
            resource.c_str(),
            oa::audio::volume_near,
            false,
            0,
            0,
            0,
            0.0F,
            0.0F,
            event->unit_index,
            tick
        };
        // Placed at the unit where it is now, as the match places a point
        // sound with 3D sound on; a unit that is gone speaks unplaced.
        if (const oa::Unit* unit = oa::world_unit_at(&world, event->unit_index);
            unit != nullptr && unit->type_index != 0) {
            const int32_t x = static_cast<int16_t>(unit->position.x >> 16);
            const int32_t y = static_cast<int16_t>(unit->position.y >> 16);
            const int32_t z = static_cast<int16_t>(unit->position.z >> 16);
            const auto camera_x = static_cast<int32_t>(game.camera_x);
            const auto camera_y = static_cast<int32_t>(game.camera_y);
            sound.placed = true;
            sound.x = x - camera_x - game.view_cells_width / 2 * OA_MAP_CELL_PIXELS;
            sound.z = (y >> 1) - z + game.view_cells_height / 2 * OA_MAP_CELL_PIXELS + camera_y;
            sound.min_distance = static_cast<float>(
                (game.view_cells_width + game.view_cells_height) / 2 * OA_MAP_CELL_PIXELS
            );
            sound.max_distance =
                static_cast<float>((game.map_width + game.map_height) * OA_MAP_CELL_PIXELS);
        }
        ++director.tally.announcements_played;
        director.send(sound);
    }
}

void Runtime::draw_director_frame(std::span<uint8_t> rgb) {
    if (director_ == nullptr || !director_->view_set)
        throw std::logic_error("a director frame is drawn once the director's camera is set");
    const auto& presentation = director_->presentation;
    if (rgb.size() != static_cast<size_t>(presentation.width) * presentation.height * kPixelBytes)
        throw std::invalid_argument("the director frame's buffer is not width * height * 3 bytes");
    const auto view = director_->view;
    const auto width = static_cast<int>(presentation.width);
    const auto height = static_cast<int>(presentation.height);
    if (director_draws_on_card()) {
        draw_director_card_frame(rgb);
        return;
    }
    const auto* gamma = director_->gamma_identity ? nullptr : &director_->gamma_table;
    namespace layout = oa::ui::display_layout;
    if (presentation.show_interface) {
        // The interface around a battlefield of the normal layout, drawn from
        // the whole map pixel at the view's corner.
        match_layout_ = lay_out_match(width, height);
        set_director_view(view);
        refresh_filtered_terrain();
        render_match_surface();
        renderer::Surface frame;
        compose_match_layers(frame);
        copy_frame(frame, 0, 0, presentation.width, presentation.height, gamma, rgb);
        return;
    }
    // The battlefield alone, drawn `margin` pixels larger from the whole map
    // pixel at the view's corner and cut at the view's offset.
    match_layout_ = layout::make_battlefield_layout(
        width + static_cast<int>(view.margin), height + static_cast<int>(view.margin)
    );
    set_director_view(view);
    refresh_filtered_terrain();
    render_match_surface();
    copy_frame(
        match_world_cpu_,
        view.offset_x,
        view.offset_y,
        presentation.width,
        presentation.height,
        gamma,
        rgb
    );
}

namespace {

/// Frames the director view check draws: a small 16:9 frame.
constexpr uint32_t kCheckWidth = 320;
constexpr uint32_t kCheckHeight = 180;
/// Ticks each pass of the check plays.
constexpr size_t kCheckTicks = 120;
/// Units a side in the check's fight, which fires the weapons whose sounds
/// the hooks must hear.
constexpr size_t kCheckArmy = 8;
/// The check's zoom, output pixels a map pixel: half a map pixel is one
/// output pixel, so the camera's steps of a quarter pixel cut the drawn
/// frame one pixel further in every other step.
constexpr double kCheckZoom = 2.0;
/// Quarters of a map pixel in one: the check's camera moves in quarters.
constexpr int64_t kQuarters = 4;
/// A close-up past the player's largest zoom (kMaxBattlefieldZoom).
constexpr double kExtendedZoom = 6.0;
/// The size of the stills written beside --snapshot.
constexpr uint32_t kStillWidth = 1280;
constexpr uint32_t kStillHeight = 720;

/// Returns the engine camera of a view whose top-left corner lies on a
/// quarter of a map pixel, as engine_view places one.
///
/// @param left_quarters the view's left edge, in quarters of a map pixel
/// @param top_quarters the view's top edge, in quarters of a map-image row
/// @param zoom output pixels a map pixel
/// @param width the output width, pixels
/// @param height the output height, pixels
/// @return the engine camera
[[nodiscard]] EngineView quarter_view(
    int64_t left_quarters, int64_t top_quarters, double zoom, uint32_t width, uint32_t height
) {
    EngineView view{};
    view.left = static_cast<int32_t>(left_quarters / kQuarters);
    view.top = static_cast<int32_t>(top_quarters / kQuarters);
    view.visible_width = static_cast<int32_t>(std::lround(static_cast<double>(width) / zoom));
    view.visible_height = static_cast<int32_t>(std::lround(static_cast<double>(height) / zoom));
    view.zoom = zoom;
    view.offset_x = static_cast<uint32_t>(
        std::floor(static_cast<double>(left_quarters % kQuarters) / kQuarters * zoom)
    );
    view.offset_y = static_cast<uint32_t>(
        std::floor(static_cast<double>(top_quarters % kQuarters) / kQuarters * zoom)
    );
    view.margin = static_cast<uint32_t>(std::ceil(zoom));
    return view;
}

/// Returns a frame's bytes as a surface write_ppm writes.
///
/// @param rgb the frame, three bytes a pixel
/// @param width its width, pixels
/// @param height its height, pixels
/// @return the surface
[[nodiscard]] renderer::Surface
frame_surface(std::span<const uint8_t> rgb, uint32_t width, uint32_t height) {
    return {width, height, std::vector<uint8_t>(rgb.begin(), rgb.end())};
}

/// A sound the check's hooks heard.
struct HeardSound {
    std::string resource{};
    int32_t volume{};
    bool placed{};
    uint16_t unit{};
    uint32_t tick{};
};

} // namespace

void Runtime::check_director_view() {
    const auto require = [](bool ok, const std::string& what) {
        if (!ok)
            throw std::runtime_error("director view check: " + what);
    };
    std::vector<HeardSound> heard;
    const DirectorSoundHooks hooks{
        &heard, [](void* context, const DirectorSound& sound) {
            static_cast<std::vector<HeardSound>*>(context)->push_back(
                {sound.resource, sound.volume, sound.placed, sound.unit, sound.tick}
            );
        }
    };
    // The world digest a save's state gives, the camera left out: the two
    // passes look from different places.
    const auto world_digest = [this] {
        const auto kept_x = match_camera_x_;
        const auto kept_z = match_camera_z_;
        match_camera_x_ = 0;
        match_camera_z_ = 0;
        const auto digest = match_world_digest();
        match_camera_x_ = kept_x;
        match_camera_z_ = kept_z;
        return digest;
    };
    // The skirmish a headless match starts, with the fight --combat sets up;
    // returns the map-image point the fight is centred on.
    const auto start_fight = [this] {
        start_benchmark_skirmish();
        match_layout_ = lay_out_match(options_.match_width, options_.match_height);
        match_zoom_ = std::clamp(options_.match_zoom, kMinBattlefieldZoom, kMaxBattlefieldZoom);
        match_zoom_target_ = match_zoom_;
        spawn_combat_armies(kCheckArmy);
        const int32_t centre_x = match_camera_x_ + visible_map_width() / 2;
        const int32_t centre_z = match_camera_z_ + visible_map_height() / 2;
        const int32_t ground = match_->map_height(
            static_cast<uint32_t>(centre_x) << 16, static_cast<uint32_t>(centre_z) << 16
        );
        return std::pair{centre_x, centre_z - ground / 2};
    };
    const auto step_tick = [this] {
        try {
            step_match_simulation();
        } catch (const std::exception& error) {
            report_match_tick_error(error.what());
        }
    };

    // The first pass: director mode, the camera panning a quarter of a map
    // pixel across and an eighth down each tick.
    const auto centre = start_fight();
    const int32_t centre_x = centre.first;
    const int32_t centre_row = centre.second;
    enter_director_mode({kCheckWidth, kCheckHeight, false, true}, hooks);
    const double visible_width = static_cast<double>(kCheckWidth) / kCheckZoom;
    const double visible_height = static_cast<double>(kCheckHeight) / kCheckZoom;
    const int64_t base_left =
        std::max<int64_t>(0, (centre_x - static_cast<int32_t>(visible_width)) * kQuarters);
    const int64_t base_top =
        std::max<int64_t>(0, (centre_row - static_cast<int32_t>(visible_height / 2)) * kQuarters);
    const auto view_at = [&](size_t tick) {
        return quarter_view(
            base_left + static_cast<int64_t>(tick),
            base_top + static_cast<int64_t>(tick / 2),
            kCheckZoom,
            kCheckWidth,
            kCheckHeight
        );
    };
    std::vector<uint8_t> frame(static_cast<size_t>(kCheckWidth) * kCheckHeight * kPixelBytes);
    oa::base::sha256::Hasher frames{};
    for (size_t tick = 1; tick <= kCheckTicks; ++tick) {
        set_director_view(view_at(tick));
        bind_director_view();
        step_tick();
        present_director_announcements();
        draw_director_frame(frame);
        oa::base::sha256::update(frames, frame);
    }
    const auto director_digest = world_digest();
    const auto tally = director_->tally;

    // No chrome: a view at a whole map pixel is the world layer's corner, at
    // the default gamma, and the layer is the battlefield alone.
    const auto whole = quarter_view(
        base_left / kQuarters * kQuarters,
        base_top / kQuarters * kQuarters,
        kCheckZoom,
        kCheckWidth,
        kCheckHeight
    );
    require(whole.offset_x == 0 && whole.offset_y == 0, "the whole-pixel view has an offset");
    std::vector<uint8_t> still(frame.size());
    set_director_view(whole);
    draw_director_frame(still);
    require(
        match_world_cpu_.width == kCheckWidth + whole.margin &&
            match_world_cpu_.height == kCheckHeight + whole.margin,
        "the world layer is not the frame and its margin"
    );
    require(match_hud_cpu_.rgb.empty(), "the interface was drawn");
    const auto world_layer = match_world_cpu_;
    std::vector<uint8_t> corner(frame.size());
    copy_frame(world_layer, 0, 0, kCheckWidth, kCheckHeight, nullptr, corner);
    require(still == corner, "the frame is not the corner of the world layer");
    require(
        std::any_of(still.begin(), still.end(), [&](uint8_t value) { return value != still[0]; }),
        "the frame is one colour"
    );

    // A view half a map pixel right and down draws the same layer and is cut
    // one output pixel further in.
    const auto shifted_view = quarter_view(
        base_left / kQuarters * kQuarters + kQuarters / 2,
        base_top / kQuarters * kQuarters + kQuarters / 2,
        kCheckZoom,
        kCheckWidth,
        kCheckHeight
    );
    require(
        shifted_view.left == whole.left && shifted_view.top == whole.top &&
            shifted_view.offset_x == 1 && shifted_view.offset_y == 1,
        "half a map pixel is not one output pixel at zoom 2"
    );
    std::vector<uint8_t> shifted(frame.size());
    set_director_view(shifted_view);
    draw_director_frame(shifted);
    require(match_world_cpu_.rgb == world_layer.rgb, "the same corner drew another world layer");
    std::vector<uint8_t> inner(frame.size());
    copy_frame(world_layer, 1, 1, kCheckWidth, kCheckHeight, nullptr, inner);
    require(shifted == inner && shifted != still, "half a map pixel did not shift the frame");

    // Zooms past the player's range draw at the frame's size.
    set_director_view(quarter_view(
        base_left / kQuarters * kQuarters,
        base_top / kQuarters * kQuarters,
        kExtendedZoom,
        kCheckWidth,
        kCheckHeight
    ));
    std::vector<uint8_t> close_up(frame.size());
    draw_director_frame(close_up);
    require(
        match_world_cpu_.width == kCheckWidth + static_cast<uint32_t>(kExtendedZoom),
        "a zoom past the player's range did not draw its margin"
    );

    // The sounds the pass played reached the hooks, placed, and a
    // sound-table sound goes unplaced.
    require(tally.point_sounds != 0, "no point sound reached the hooks");
    // With 3D sound on, the match places every point sound at the near
    // volume; a unit's announcement is placed too unless the unit is gone.
    for (const auto& sound : heard)
        require(
            (sound.placed || sound.unit != 0) && sound.volume == oa::audio::volume_near &&
                sound.resource.starts_with("sounds/") && sound.tick <= kCheckTicks,
            "a point sound was not placed: " + sound.resource
        );
    const size_t sounds_heard = heard.size();
    const game_audio::Sound* named = nullptr;
    for (game_audio::SoundIndex index = 0; named == nullptr; ++index) {
        const auto* entry = audio_registry_.get(index);
        require(entry != nullptr, "the sound table has no named sound");
        if (!entry->name.empty())
            named = entry;
    }
    match_->named_sound.play(match_->named_sound.context, named->name.c_str());
    require(
        heard.size() == sounds_heard + 1 && !heard.back().placed &&
            heard.back().resource == named->resource,
        "a sound-table sound did not reach the hooks unplaced"
    );

    // With the interface the frame shows it.
    leave_director_mode();
    enter_director_mode({kCheckWidth, kCheckHeight, true, true}, {});
    set_director_view(whole);
    std::vector<uint8_t> dressed(frame.size());
    draw_director_frame(dressed);
    require(dressed != still, "the interface did not show");
    leave_director_mode();

    // Stills for the eye beside --snapshot: a close shot, the widest view the
    // map holds and a close-up past the player's zoom, at 1280x720. The world
    // is compared already, so the stills show it with mapping and line of
    // sight off.
    fs::path stem = options_.snapshot;
    stem.replace_extension();
    if (!options_.snapshot.empty()) {
        namespace visibility_flag = oa::ui::console::visibility_flag;
        auto& visibility = match_->state().game.visibility_flags;
        visibility = static_cast<uint8_t>(
            visibility & ~(visibility_flag::mapping | visibility_flag::line_of_sight)
        );
        reset_sight_presentation();
        enter_director_mode({kStillWidth, kStillHeight, false, true}, {});
        std::vector<uint8_t> picture(static_cast<size_t>(kStillWidth) * kStillHeight * kPixelBytes);
        const auto& game = match_->state().game;
        const auto map_width = static_cast<double>(game.map_pixel_width);
        const auto map_height = static_cast<double>(game.map_pixel_height);
        const auto still_at = [&](double zoom, const char* name) {
            const double across = kStillWidth / zoom;
            const double down = kStillHeight / zoom;
            const double left = std::clamp(
                static_cast<double>(centre_x) - across / 2, 0.0, std::max(0.0, map_width - across)
            );
            const double top = std::clamp(
                static_cast<double>(centre_row) - down / 2, 0.0, std::max(0.0, map_height - down)
            );
            set_director_view(quarter_view(
                static_cast<int64_t>(std::max(0.0, left)) * kQuarters,
                static_cast<int64_t>(std::max(0.0, top)) * kQuarters,
                zoom,
                kStillWidth,
                kStillHeight
            ));
            draw_director_frame(picture);
            write_ppm(
                stem.string() + "-" + name + ".ppm",
                frame_surface(picture, kStillWidth, kStillHeight)
            );
        };
        still_at(kCheckZoom, "director-close");
        still_at(std::max(kStillWidth / map_width, kStillHeight / map_height), "director-wide");
        still_at(kExtendedZoom, "director-extended");
        leave_director_mode();
        write_ppm(
            stem.string() + "-director-check.ppm", frame_surface(still, kCheckWidth, kCheckHeight)
        );
    }

    // The second pass: the same skirmish in director mode with nothing
    // drawn, as the generator replays a recording. A director's draws put
    // back the piece transforms they rebuild, which the match reads, so the
    // drawn pass must reach the same world. (A headless match that draws
    // reaches another: the match reads the transforms its draws leave.)
    leave_match();
    load(Screen::main_menu);
    // The views come from view_at, so the fight's centre is not needed.
    start_fight();
    enter_director_mode({kCheckWidth, kCheckHeight, false, true}, {});
    for (size_t tick = 1; tick <= kCheckTicks; ++tick) {
        set_director_view(view_at(tick));
        bind_director_view();
        step_tick();
    }
    const auto undrawn_digest = world_digest();
    leave_director_mode();
    if (!options_.snapshot.empty()) {
        rebuild_surface();
        write_ppm(stem.string() + "-headless.ppm", surface_);
    }
    require(
        director_digest == undrawn_digest,
        "the director's drawn pass reached another world than its undrawn one"
    );
    const auto hex = oa::base::sha256::to_hex(oa::base::sha256::finish(frames));
    std::printf(
        "director view: %016llx %.*s\n",
        static_cast<unsigned long long>(director_digest),
        static_cast<int>(hex.size()),
        hex.data()
    );
    uint32_t speakers = 0;
    for (uint32_t bits = tally.speaking_players; bits != 0; bits &= bits - 1)
        ++speakers;
    std::printf(
        "director view: %llu point sounds; %llu unit announcements asked for by %u players, %llu "
        "queued, %llu played\n",
        static_cast<unsigned long long>(tally.point_sounds),
        static_cast<unsigned long long>(tally.announcements_heard),
        speakers,
        static_cast<unsigned long long>(tally.announcements_queued),
        static_cast<unsigned long long>(tally.announcements_played)
    );
    std::fflush(stdout);
}

} // namespace oa::app

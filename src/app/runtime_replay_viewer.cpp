// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The playback bar over a replay: the foot of the battlefield, the end card,
// and the keys that move through the recording. The bar reads the replay the
// app is already playing. It does not play a second copy.

#include "oa/app/runtime.hpp"

#include "library_host.hpp"
#include "map_packs.hpp"
#include "oa_layer.hpp"
#include "replay_index_pass.hpp"

#include "oa/app/hook_call.hpp"
#include "oa/app/replay_index.hpp"
#include "oa/app/replay_watch.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/base/threads.hpp"
#include "oa/core/player.h"
#include "oa/core/world.h"
#include "oa/data/input_actions/actions.hpp"
#include "oa/formats/oademo/oademo.hpp"
#include "oa/formats/oademo/timeline_index.hpp"
#include "oa/platform/files.hpp"
#include "oa/platform/system.hpp"
#include "oa/present/colour_vision/colour_vision.hpp"
#include "oa/ui/hud/resource_panel.hpp"
#include "oa/ui/hud/shared_views.hpp"
#include "oa/ui/library/export.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/library/screen.hpp"
#include "oa/ui/kit/text.hpp"
#include "oa/ui/pad_controls.hpp"
#include "oa/ui/replay_viewer/replay_viewer.hpp"

#include <SDL3/SDL_process.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

namespace oa::app {
namespace {

namespace rv = oa::ui::replay_viewer;
namespace kit = oa::ui::kit;
namespace library = oa::ui::library;
namespace oademo = oa::formats::oademo;
namespace fs = std::filesystem;

/// Ticks in ten seconds, and in a minute, at the bar's 30 ticks a second.
constexpr int32_t kStepTicks = 300;
constexpr int32_t kLongStepTicks = 1800;
/// How long the reached speed stays under the asked speed before the bar says so.
constexpr uint32_t kBehindMs = 1000;

/// One mark before pauses are paired. `flag` is a pause that began, or an alliance made.
struct RawMarker {
    rv::MarkerKind kind{rv::MarkerKind::chat};
    uint32_t tick{};
    int number{-1};
    bool flag{};
    std::string name{};
    std::string line{};
};

/// What the background read found. Colours are filled on the main thread.
struct BuiltPlayer {
    std::string name{};
    int number{-1};
    int32_t kills{};
    int32_t losses{};
    bool watcher{};
};

/// The index the background read stored. The main thread copies it out.
struct BuiltIndex {
    bool from_timeline{};
    bool damaged{};
    uint32_t damage_tick{};
    uint32_t length{};
    uint32_t heat_period{rv::ticks_per_second * 10};
    std::string winner{};
    bool no_result{};
    std::vector<BuiltPlayer> players{};
    std::vector<RawMarker> markers{};
    std::vector<uint32_t> heat{};
    std::vector<oademo::BattleEvent> battles{};

    /// Colours the recording names, applied on the main thread.
    struct MetaColour {
        std::string name{};
        int number{-1};
        int colour{-1};
    };

    std::vector<MetaColour> meta_colours{};
};

/// The game time of a tick, as "18:02". Minutes have no leading zero.
///
/// @param tick the game tick
/// @return the clock
std::string clock_of(uint32_t tick) {
    const uint32_t seconds_total = tick / rv::ticks_per_second;
    const uint32_t minutes = seconds_total / 60;
    const uint32_t seconds = seconds_total % 60;
    std::string text = std::to_string(minutes);
    text.push_back(':');
    if (seconds < 10)
        text.push_back('0');
    text += std::to_string(seconds);
    return text;
}

/// Returns a player's name up to the first zero. The slot's text is game text.
///
/// @param player the player
/// @return the name; empty when the slot has none
std::string copied_name(const oa::Player& player) {
    std::size_t length = 0;
    while (length < sizeof(player.name) && player.name[length] != '\0')
        ++length;
    return std::string(player.name, length);
}

/// Returns the name recorded for a player number. 0 is no player.
///
/// @param players the recording's players
/// @param number the player's number
/// @return the name; empty when none matches
std::string name_for(const std::vector<oademo::IndexPlayer>& players, int number) {
    if (number == 0)
        return {};
    for (const oademo::IndexPlayer& player : players)
        if (player.number == number)
            return player.name;
    return {};
}

/// Clamps a heat sample into the bar's width.
///
/// @param sample health points in one stretch
/// @return the sample, at most what a uint32_t holds
uint32_t heat_sample(uint64_t sample) noexcept {
    return sample > 0xffffffffu ? 0xffffffffu : static_cast<uint32_t>(sample);
}

/// Reads a whole file. An unreadable file is empty.
///
/// @param path the file
/// @return its bytes
std::vector<uint8_t> read_bytes(const fs::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        return {};
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

/// Copies colours the recording names onto a built index.
///
/// @param built the index
/// @param path the recording
void attach_meta(BuiltIndex& built, const fs::path& path) {
    const std::vector<uint8_t> bytes = read_bytes(path);
    const std::span<const uint8_t> file(bytes.data(), bytes.size());
    if (bytes.empty() || !oademo::recognised(file))
        return;
    const oademo::ReadResult read = oademo::read(file);
    if (!read.ok())
        return;
    for (const oademo::PlayerInfo& player : read.replay->meta.players) {
        BuiltIndex::MetaColour colour;
        colour.name = player.name;
        colour.number = player.number;
        colour.colour = player.colour;
        built.meta_colours.push_back(std::move(colour));
    }
}

/// The bar's kind for one index marker. A start marker is left out.
///
/// @param kind the index kind
/// @param[out] mapped the bar's kind
/// @return false for a start marker
bool map_kind(oademo::MarkerKind kind, rv::MarkerKind& mapped) noexcept {
    switch (kind) {
    case oademo::MarkerKind::commander_death:
        mapped = rv::MarkerKind::commander_death;
        return true;
    case oademo::MarkerKind::player_left:
        mapped = rv::MarkerKind::left;
        return true;
    case oademo::MarkerKind::player_dropped:
        mapped = rv::MarkerKind::dropped;
        return true;
    case oademo::MarkerKind::player_defeated:
        mapped = rv::MarkerKind::defeated;
        return true;
    case oademo::MarkerKind::alliance:
        mapped = rv::MarkerKind::alliance;
        return true;
    case oademo::MarkerKind::pause:
        mapped = rv::MarkerKind::pause;
        return true;
    case oademo::MarkerKind::chat:
        mapped = rv::MarkerKind::chat;
        return true;
    case oademo::MarkerKind::end:
        mapped = rv::MarkerKind::end;
        return true;
    case oademo::MarkerKind::start:
        return false;
    }
    return false;
}

/// Fills a built index from a timeline.json.
///
/// @param timeline the index
/// @return the bar's copy; damaged only when the timeline says the recording failed
BuiltIndex from_timeline(const oademo::TimelineIndex& timeline) {
    BuiltIndex built;
    built.from_timeline = true;
    built.damaged = !timeline.verdict.clean;
    built.damage_tick = timeline.verdict.first_error_tick;
    built.length = timeline.length_ticks;
    built.heat_period = timeline.period_ticks == 0 ? built.heat_period : timeline.period_ticks;
    for (int winner : timeline.result.winners) {
        const std::string name = name_for(timeline.players, winner);
        if (name.empty())
            continue;
        if (!built.winner.empty())
            built.winner += ", ";
        built.winner += name;
    }
    built.no_result = built.winner.empty();
    for (const oademo::IndexPlayer& player : timeline.players) {
        BuiltPlayer row;
        row.name = player.name;
        row.number = player.number;
        row.kills = static_cast<int32_t>(std::min(player.final.kills, uint32_t{0x7fffffff}));
        row.losses = static_cast<int32_t>(std::min(player.final.losses, uint32_t{0x7fffffff}));
        row.watcher = player.watcher;
        built.players.push_back(std::move(row));
    }
    for (const oademo::IndexMarker& marker : timeline.markers) {
        rv::MarkerKind kind = rv::MarkerKind::chat;
        if (!map_kind(marker.kind, kind))
            continue;
        RawMarker raw;
        raw.kind = kind;
        raw.tick = marker.tick;
        raw.number = marker.player == 0 ? -1 : marker.player;
        raw.flag = marker.flag;
        raw.name = name_for(timeline.players, marker.player);
        raw.line = marker.text;
        built.markers.push_back(std::move(raw));
    }
    built.heat.reserve(timeline.damage.size());
    for (uint64_t sample : timeline.damage)
        built.heat.push_back(heat_sample(sample));
    built.battles = timeline.events;
    return built;
}

/// Tells whether a timeline is one this build recorded the fighting into.
///
/// @param timeline the index
/// @return true for version 1 that names its battle events, including an empty list
bool timeline_current(const oademo::TimelineIndex& timeline) noexcept {
    return timeline.version == oademo::timeline_version && timeline.events_recorded;
}

/// Reads a timeline.json file into a built index.
///
/// @param path the file
/// @return the index; empty when the file is missing or is not a timeline
BuiltIndex read_timeline_file(const fs::path& path) {
    const std::vector<uint8_t> bytes = read_bytes(path);
    if (bytes.empty())
        return {};
    const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
    const std::optional<oademo::TimelineIndex> timeline = oademo::parse_timeline_index(text);
    if (!timeline || !timeline_current(*timeline))
        return {};
    return from_timeline(*timeline);
}

/// A timeline already stored for a recording, and whether the index pass is still wanted.
struct SavedTimeline {
    BuiltIndex index{}; ///< the timeline; empty when none is stored
    bool needs_pass{};  ///< no timeline was stored, so the pass should run
};

/// Reads a timeline kept in the recording, or the timeline.json beside it.
///
/// @param path the recording
/// @return the timeline, or a request for the index pass when neither is there
SavedTimeline read_saved_timeline(const fs::path& path) {
    const std::vector<uint8_t> bytes = read_bytes(path);
    const std::span<const uint8_t> file(bytes.data(), bytes.size());
    if (!bytes.empty() && oademo::recognised(file)) {
        const oademo::ReadResult read = oademo::read(file);
        if (read.ok() && read.replay->timeline) {
            if (const std::optional<oademo::TimelineIndex> timeline =
                    oademo::parse_timeline_index(*read.replay->timeline)) {
                if (timeline_current(*timeline))
                    return {from_timeline(*timeline), false};
            }
        }
    }
    fs::path beside = path;
    beside.replace_extension(".timeline.json");
    if (beside != path) {
        const BuiltIndex side = read_timeline_file(beside);
        if (side.from_timeline)
            return {side, false};
    }
    return {{}, true};
}

/// The path of a recording below Recordings, with slashes. Empty when it is not below.
///
/// @param user the player's folder
/// @param recording the recording
/// @return the relative path; empty when it leaves Recordings
std::string recording_below(const fs::path& user, const fs::path& recording) {
    if (user.empty() || recording.empty())
        return {};
    const fs::path root = (user / std::string(recordings_folder_name)).lexically_normal();
    const fs::path relative = recording.lexically_normal().lexically_relative(root);
    if (relative.empty() || relative.is_absolute())
        return {};
    const std::string text = oa::platform::path_to_generic_utf8(relative);
    if (text.empty() || text == "." || text.starts_with("../") || text.starts_with(".."))
        return {};
    return text;
}

} // namespace

/// The playback bar, a screen of the OA layer named replay-viewer.
struct Runtime::ReplayViewerScreen final : LayerScreen {
    /// Makes the bar and starts reading the recording.
    ///
    /// @param runtime the runtime
    explicit ReplayViewerScreen(Runtime& runtime) : runtime_(runtime) {
        runtime_.replay_viewer_ = this;
        runtime_.replay_stats_hidden_ = false;
        watched_path_ = runtime_.replay_recording_path_;
        handed_timeline_ = std::move(runtime_.replay_handed_timeline_);
        runtime_.replay_handed_timeline_.clear();
        std::error_code removed;
        fs::remove(
            runtime_.user_folder_ / std::string(recordings_folder_name) / ".replays" /
                "watch-index.timeline.json",
            removed
        );
        if (!oa::base::threads::start_thread(
                index_thread_, &ReplayViewerScreen::read_recording_entry, this
            ))
            read_and_store();
    }

    /// Joins the read and clears the bar.
    ~ReplayViewerScreen() override { finish(); }

    /// Returns "replay-viewer".
    ///
    /// @return the name
    [[nodiscard]] std::string_view name() const override { return "replay-viewer"; }

    /// The battlefield's size in the bar's own points, at the chrome's scale.
    ///
    /// @param seen what the screen is placed on
    /// @return the width and the height; zero off a match
    [[nodiscard]] static kit::Point battlefield_points(const LayerView& seen) {
        if (!seen.match || seen.match_layout == nullptr)
            return {};
        const oa::ui::display_layout::MatchLayout& layout = *seen.match_layout;
        const double scale = layout.chrome_scale > 0.0 ? layout.chrome_scale : layout.scale;
        if (scale <= 0.0 || layout.battlefield_width() <= 0 || layout.battlefield_height() <= 0)
            return {};
        const int32_t percent =
            std::max(static_cast<int32_t>(std::lround(scale * 100.0)), int32_t{100});
        const auto points_of = [percent](int pixels) {
            return static_cast<int32_t>(
                (static_cast<int64_t>(pixels) * 100 + percent / 2) / percent
            );
        };
        return {points_of(layout.battlefield_width()), points_of(layout.battlefield_height())};
    }

    /// Covers the battlefield, from the side column's right edge to the window's.
    ///
    /// @param seen what the screen is placed on
    /// @return the battlefield; empty off a match
    [[nodiscard]] LayerPlacement placement(const LayerView& seen) const override {
        const kit::Point points = battlefield_points(seen);
        if (points.x <= 0 || points.y <= 0 || seen.match_layout == nullptr)
            return {};
        const oa::ui::display_layout::MatchLayout& layout = *seen.match_layout;
        const double scale = layout.chrome_scale > 0.0 ? layout.chrome_scale : layout.scale;
        const int32_t percent =
            std::max(static_cast<int32_t>(std::lround(scale * 100.0)), int32_t{100});
        return {
            {layout.battlefield_x(),
             layout.battlefield_y(),
             layout.battlefield_width(),
             layout.battlefield_height()},
            points.x,
            points.y,
            percent
        };
    }

    /// Lays the bar out across the battlefield. The layer calls this before it
    /// draws or hands over a pointer, including after the window changes size.
    ///
    /// @param seen what the screen is placed on
    void lay_out(const LayerView& seen) override {
        const kit::Point points = battlefield_points(seen);
        if (points.x <= 0 || points.y <= 0)
            return;
        if (points.x == points_width_ && points.y == points_height_)
            return;
        points_width_ = points.x;
        points_height_ = points.y;
        refresh();
    }

    /// The match under the bar still takes input.
    ///
    /// @return false
    [[nodiscard]] bool modal() const override { return false; }

    /// The battlefield is not darkened.
    ///
    /// @return false
    [[nodiscard]] bool backdrop() const override { return false; }

    /// The bar is laid over the battlefield by its opacity.
    ///
    /// @return false
    [[nodiscard]] bool opaque() const override { return false; }

    /// The clock moves, so the drawing is not kept from the revision alone.
    ///
    /// @return false
    [[nodiscard]] bool revision_covers_drawing() const override { return false; }

    /// Draws the bar, and the export dialog while it is open.
    ///
    /// @param canvas the screen's picture
    void draw(const kit::Canvas& canvas) const override {
        // Photo mode puts the bar away, including one pinned with B.
        if (runtime_.photo_mode())
            return;
        rv::draw(canvas, layout_, model_);
        if (export_open_ && !library_list_.items.empty())
            library::draw_library(kit::placed_at(canvas, export_origin_), library_list_);
    }

    /// Takes the pointer. A move that only hovers goes on to the match. A press
    /// on the bar is kept. A drag on the timeline seeks.
    ///
    /// @param kind the pointer event
    /// @param button the button, 1 for the left
    /// @param at the pointer, in the screen's points
    /// @param finger_reach unused; a finger uses the same hit areas
    /// @return what the bar did
    LayerAnswer pointer(
        ScreenInputKind kind, uint8_t button, kit::Point at, int32_t /*finger_reach*/
    ) override {
        if (runtime_.photo_mode())
            return LayerAnswer::pass;
        note_pointer(at);
        const kit::Rect dialog{
            export_origin_.x + export_dialog_.x,
            export_origin_.y + export_dialog_.y,
            export_dialog_.width,
            export_dialog_.height
        };
        if (export_open_ && kit::contains(dialog, at))
            return take_export(kind, button, at);
        if (kind == ScreenInputKind::pointer_move) {
            if (dragging_) {
                seek_to(rv::tick_at(layout_.timeline, at.x, model_.length));
                return LayerAnswer::redraw;
            }
            ++revision_;
            return LayerAnswer::pass;
        }
        if (kind == ScreenInputKind::pointer_down) {
            if (button != 1)
                return LayerAnswer::pass;
            const int view_item = model_.view_open ? rv::view_menu_index(layout_, at) : -1;
            const int camera_item = model_.camera_open ? rv::camera_menu_index(layout_, at) : -1;
            if (view_item >= 0 || camera_item >= 0 || rv::over_watch_chrome(layout_, at)) {
                held_view_item_ = view_item;
                held_camera_item_ = camera_item;
                held_ = rv::Control::none;
                visibility_.show();
                ++revision_;
                return LayerAnswer::redraw;
            }
            const rv::Control hit = rv::hit_control(layout_, at);
            held_ = hit;
            if (hit == rv::Control::timeline) {
                dragging_ = true;
                seek_to(rv::tick_at(layout_.timeline, at.x, model_.length));
                visibility_.show();
                refresh();
                return LayerAnswer::redraw;
            }
            if (hit == rv::Control::none)
                return LayerAnswer::pass;
            visibility_.show();
            ++revision_;
            return LayerAnswer::redraw;
        }
        if (dragging_) {
            dragging_ = false;
            held_ = rv::Control::none;
            return LayerAnswer::none;
        }
        if (held_view_item_ >= 0 || held_camera_item_ >= 0) {
            const int view_now = model_.view_open ? rv::view_menu_index(layout_, at) : -1;
            const int camera_now = model_.camera_open ? rv::camera_menu_index(layout_, at) : -1;
            const int view_was = held_view_item_;
            const int camera_was = held_camera_item_;
            held_view_item_ = -1;
            held_camera_item_ = -1;
            if (view_was >= 0 && view_was == view_now)
                choose_view_row(view_was);
            else if (camera_was >= 0 && camera_was == camera_now)
                choose_camera_row(camera_was);
            refresh();
            ++revision_;
            return LayerAnswer::redraw;
        }
        const rv::Control hit = rv::hit_control(layout_, at);
        const rv::Control pressed = held_;
        held_ = rv::Control::none;
        if (pressed == rv::Control::none || pressed != hit)
            return hit == rv::Control::none ? LayerAnswer::pass : LayerAnswer::none;
        return activate(pressed);
    }

    /// Every key goes on to the match. Playback keys arrive as actions.
    ///
    /// @return pass
    LayerAnswer key(kit::Key /*pressed*/, uint32_t /*sdl_key*/) override {
        return LayerAnswer::pass;
    }

    /// Escape and the match's own keys go on.
    ///
    /// @return pass
    LayerAnswer shortcut(uint32_t /*sdl_key*/, uint16_t /*modifiers*/) override {
        return LayerAnswer::pass;
    }

    /// The wheel goes on to the match.
    ///
    /// @return pass
    LayerAnswer wheel(kit::Point /*at*/, float /*notches*/) override { return LayerAnswer::pass; }

    /// The pad's own path handles a replay. The screen does not take the action.
    ///
    /// @return pass
    LayerAnswer pad_action(oa::ui::pad_controls::Action /*action*/) override {
        return LayerAnswer::pass;
    }

    /// Returns the controls on show, for automation.
    ///
    /// @return the display list
    [[nodiscard]] kit::DisplayList display_list() const override {
        if (runtime_.photo_mode())
            return {};
        return shown_;
    }

    /// Returns the pointer's hover. The bar does not take the keyboard focus.
    ///
    /// @return the interaction
    [[nodiscard]] kit::Interaction interaction() const override { return interaction_; }

    /// Brings the bar up to date. Closes once the replay has ended.
    ///
    /// @return close once the replay bit is off; otherwise redraw
    LayerAnswer tick() override {
        const bool replaying = (runtime_.current_extension_state() & extension_state::replay) != 0;
        if (!replaying)
            return LayerAnswer::close;
        take_index();
        poll_index_pass();
        take_index();
        if (pass_wanted() && !pass_started_ && !index_taken_)
            start_index_pass();
        read_playback();
        // The top bar shows only with one player's sight.
        runtime_.keep_replay_top_bar();
        if (reset_after_seek_ && !seek_armed_) {
            reset_after_seek_ = false;
            reset_watch();
        }
        advance_visibility();
        refresh();
        // Follow their camera would move the framed view on every frame,
        // including across a seek. Photo mode keeps the camera where it is.
        if (!runtime_.photo_mode())
            apply_camera();
        pump_export();
        ++revision_;
        return LayerAnswer::redraw;
    }

    /// Returns a number that changes when the bar is pressed.
    ///
    /// @return the revision
    [[nodiscard]] uint64_t revision() const override { return revision_; }

    /// Joins the read and clears the bar. The layer calls this as the screen leaves.
    ///
    /// @param by_key unused
    void close(bool /*by_key*/) override { finish(); }

    /// Shows the bar. A playback key and a tap do this.
    void show_bar() {
        visibility_.show();
        refresh();
    }

    /// Carries out one playback action and shows the bar.
    ///
    /// @param id the action, `watch.replay-` and the rest
    void command(std::string_view id) {
        visibility_.show();
        if (id == "watch.replay-pause") {
            if (replay_paused_)
                play();
            else
                pause();
        } else if (id == "watch.replay-back") {
            seek_by(-kStepTicks);
        } else if (id == "watch.replay-forward") {
            seek_by(kStepTicks);
        } else if (id == "watch.replay-back-long") {
            seek_by(-kLongStepTicks);
        } else if (id == "watch.replay-forward-long") {
            seek_by(kLongStepTicks);
        } else if (id == "watch.replay-previous-marker") {
            seek_marker(false);
        } else if (id == "watch.replay-next-marker") {
            seek_marker(true);
        } else if (id == "watch.replay-start") {
            seek_to(0);
        } else if (id == "watch.replay-end") {
            seek_to(model_.length);
        } else if (id == "watch.replay-bar") {
            visibility_.toggle_pin();
        } else if (id == "watch.replay-keys") {
            model_.keys_open = !model_.keys_open;
        } else if (id == "watch.next-player-view") {
            step_player_view(1);
        } else if (id == "watch.previous-player-view") {
            step_player_view(-1);
        } else if (id == "watch.whole-view") {
            show_whole_view();
        } else if (id == "watch.camera-mode") {
            step_camera_mode();
        }
        refresh();
        ++revision_;
    }

    /// Seeks to a tick for photo mode. The bar stays as it is.
    ///
    /// @param tick the tick
    void seek_for_photo(uint32_t tick) { seek_to(tick); }

    /// Seeks by a number of ticks for photo mode. The bar stays as it is.
    ///
    /// @param delta the ticks to add; negative jumps back
    void seek_by_for_photo(int32_t delta) { seek_by(delta); }

    /// Tells whether a seek photo mode asked for has not landed.
    ///
    /// @return true while the landing rule is still holding the asked tick
    [[nodiscard]] bool photo_seek_pending() const { return seek_armed_; }

    /// Shows the next or previous row of the view menu.
    ///
    /// @param delta +1 for the next row, -1 for the previous
    void step_player_view(int delta) {
        if (model_.watch.empty())
            fill_watch();
        if (model_.watch.empty())
            return;
        int index = 0;
        for (std::size_t place = 0; place < model_.watch.size(); ++place) {
            if (model_.watch[place].slot == viewed_slot_)
                index = static_cast<int>(place);
        }
        const int count = static_cast<int>(model_.watch.size());
        int next = index + (delta < 0 ? -1 : 1);
        if (next < 0)
            next = count - 1;
        if (next >= count)
            next = 0;
        show_player(model_.watch[static_cast<std::size_t>(next)].slot);
    }

    /// Shows the whole map and every unit.
    void show_whole_view() { show_player(-1); }

    /// Moves to the next camera mode the recording can offer.
    ///
    /// Follow a unit is left for Free. Free then goes to the recorded camera
    /// when the recording shared one, and then to the camera that follows the
    /// fighting when the index holds a fight. Each step returns to Free.
    void step_camera_mode() {
        const rv::CameraMode from =
            camera_mode_ == rv::CameraMode::follow_unit ? rv::CameraMode::free : camera_mode_;
        rv::CameraMode next = rv::CameraMode::free;
        if (from == rv::CameraMode::free && runtime_.replay_shares_camera_)
            next = rv::CameraMode::follow_theirs;
        else if (from == rv::CameraMode::free && director_offered())
            next = rv::CameraMode::director;
        else if (from == rv::CameraMode::follow_theirs && director_offered())
            next = rv::CameraMode::director;
        use_camera(next);
    }

    /// Keeps one unit in the middle until the player pans.
    ///
    /// @param unit the unit's slot
    void follow_unit(uint16_t unit) {
        if (!runtime_.match_unit_present(unit))
            return;
        camera_mode_ = rv::CameraMode::follow_unit;
        model_.camera = camera_mode_;
        model_.camera_open = false;
        runtime_.begin_match_tracking(unit);
    }

    /// Leaves follow-a-unit. Any other mode stays.
    void release_unit_follow() {
        if (camera_mode_ != rv::CameraMode::follow_unit)
            return;
        runtime_.stop_match_tracking();
        camera_mode_ = rv::CameraMode::free;
        model_.camera = camera_mode_;
    }

    /// Shows every player and leaves the camera free. Tracking stops.
    ///
    /// The camera stays where it is. A playback that must match an earlier
    /// picture puts it back itself.
    void reset_watch() {
        runtime_.stop_match_tracking();
        camera_mode_ = rv::CameraMode::free;
        model_.camera = camera_mode_;
        model_.view_open = false;
        model_.camera_open = false;
        show_player(-1);
    }

    /// The bar's thread: it reads the recording and stores a BuiltIndex.
    ///
    /// @param argument the screen
    static void read_recording_entry(void* argument) {
        auto* screen = static_cast<ReplayViewerScreen*>(argument);
        if (screen != nullptr)
            screen->read_and_store();
    }

    /// Reads a timeline already stored for this recording. The background thread calls this.
    void read_and_store() {
        try {
            if (!handed_timeline_.empty()) {
                if (const std::optional<oademo::TimelineIndex> timeline =
                        oademo::parse_timeline_index(handed_timeline_)) {
                    if (timeline_current(*timeline)) {
                        BuiltIndex built = from_timeline(*timeline);
                        attach_meta(built, watched_path_);
                        store_index(std::move(built));
                        return;
                    }
                }
            }
            const SavedTimeline saved = read_saved_timeline(watched_path_);
            if (!saved.needs_pass) {
                BuiltIndex built = saved.index;
                attach_meta(built, watched_path_);
                store_index(std::move(built));
            } else {
                oa::base::threads::LockGuard lock(index_mutex_);
                needs_pass_ = true;
            }
        } catch (const std::exception&) {
            store_index({});
        }
    }

    /// Stops the index pass and forgets this recording when it is still the one on show.
    void release_watched_recording() {
        stop_index_pass();
        if (!watched_path_.empty() && runtime_.replay_recording_path_ == watched_path_)
            runtime_.replay_recording_path_.clear();
    }

    /// The first marker after a tick.
    ///
    /// @param tick the tick
    /// @return that marker's tick; none when there is none
    [[nodiscard]] std::optional<uint32_t> marker_after(uint32_t tick) const {
        std::optional<uint32_t> found;
        for (const rv::Marker& marker : model_.markers) {
            if (marker.tick <= tick)
                continue;
            if (!found || marker.tick < *found)
                found = marker.tick;
        }
        return found;
    }

    /// The first marker before a tick.
    ///
    /// @param tick the tick
    /// @return that marker's tick; none when there is none
    [[nodiscard]] std::optional<uint32_t> marker_before(uint32_t tick) const {
        std::optional<uint32_t> found;
        for (const rv::Marker& marker : model_.markers) {
            if (marker.tick >= tick)
                continue;
            if (!found || marker.tick > *found)
                found = marker.tick;
        }
        return found;
    }

    /// Tells whether a timeline has been taken.
    ///
    /// @return true once the index arrived from a timeline
    [[nodiscard]] bool index_arrived() const { return index_taken_ && from_timeline_; }

    /// The camera that follows the fighting is offered once the index holds
    /// a unit destroyed, a building lost or a large engagement. A commander
    /// being placed or lost does not offer it.
    ///
    /// @return true when the index holds a fight
    [[nodiscard]] bool director_offered() const {
        if (!index_arrived())
            return false;
        for (const oademo::BattleEvent& event : battles_)
            if (oademo::battle_fight(event.kind))
                return true;
        return false;
    }

    /// The index pass this bar started, while that process is running.
    ///
    /// @return the process id; 0 when none is running
    [[nodiscard]] int index_pid() const { return index_pid_; }

    /// The last progress the pass reported. It stays once the pass has ended.
    ///
    /// @return the percent, 0 to 99
    [[nodiscard]] int finding_percent() const { return finding_percent_; }

    /// The colour number the end card uses for a player.
    ///
    /// @param name the player's name
    /// @return the colour number, or -1 when the card has not named them
    [[nodiscard]] int named_colour(std::string_view name) const {
        for (std::size_t place = 0; place < model_.players.size(); ++place) {
            if (model_.players[place].name != name)
                continue;
            if (place < player_colour_used_.size())
                return player_colour_used_[place];
            return -1;
        }
        return -1;
    }

    /// The canvas row the bar starts on, shown or hidden.
    ///
    /// @param layout the match's layout the bar is placed on
    /// @return the row; the battlefield's last row past it before the bar is laid out
    [[nodiscard]] int32_t bar_top(const oa::ui::display_layout::MatchLayout& layout) const {
        const int32_t top = layout.battlefield_y();
        const int32_t height = layout.battlefield_height();
        if (points_height_ <= 0 || height <= 0)
            return top + height;
        return top +
               static_cast<int32_t>(static_cast<int64_t>(layout_.bar.y) * height / points_height_);
    }

    /// The players the stats panel lists: the view control's, in its order,
    /// with the colours it draws them in.
    ///
    /// @return the seats; empty before the first refresh
    [[nodiscard]] const std::vector<oa::ui::hud::ResourcePanelSeat>& panel_seats() const {
        return panel_seats_;
    }

    /// The colour the view control draws the player of one stats panel row in.
    ///
    /// @param row the panel's row, from 0
    /// @return the colour; none past the rows
    [[nodiscard]] std::optional<kit::Colour> panel_row_view_colour(int row) const {
        if (row < 0 || static_cast<std::size_t>(row) >= panel_rows_.size())
            return std::nullopt;
        const int watch = panel_rows_[static_cast<std::size_t>(row)];
        if (watch < 0 || static_cast<std::size_t>(watch) >= model_.watch.size())
            return std::nullopt;
        return model_.watch[static_cast<std::size_t>(watch)].colour;
    }

    /// Shows the view of the stats panel's row, as the view control's row
    /// for that player does.
    ///
    /// @param row the panel's row, from 0
    void choose_panel_row(int row) {
        if (row < 0 || static_cast<std::size_t>(row) >= panel_rows_.size())
            return;
        choose_view_row(panel_rows_[static_cast<std::size_t>(row)]);
        refresh();
        ++revision_;
    }

    /// Stores what the background read found. Called on that thread, or inline.
    ///
    /// @param built the index
    void store_index(BuiltIndex built) {
        oa::base::threads::LockGuard lock(index_mutex_);
        built_ = std::move(built);
        index_ready_ = true;
    }

  private:

    /// Joins the read, drops the bar's pointer and shows chat again.
    void finish() {
        seek_armed_ = false;
        if (camera_mode_ == rv::CameraMode::follow_unit)
            runtime_.stop_match_tracking();
        camera_mode_ = rv::CameraMode::free;
        stop_index_pass();
        oa::base::threads::join_thread(index_thread_);
        release_watched_recording();
        if (runtime_.replay_viewer_ == this)
            runtime_.replay_viewer_ = nullptr;
        runtime_.replay_chat_hidden_ = false;
    }

    /// Tells whether the background read found no stored timeline.
    ///
    /// @return true while the index pass is still wanted
    [[nodiscard]] bool pass_wanted() {
        oa::base::threads::LockGuard lock(index_mutex_);
        return needs_pass_;
    }

    /// Reads progress lines the pass has written. A line that is not ready does not wait.
    void drain_index_progress() {
        if (index_stdout_ == nullptr)
            return;
        char buffer[256];
        for (int spin = 0; spin < 32; ++spin) {
            const std::size_t got = SDL_ReadIO(index_stdout_, buffer, sizeof(buffer));
            if (got == 0)
                break;
            progress_text_.append(buffer, got);
            take_progress_lines();
        }
    }

    /// Takes whole `replay index: progress N` lines out of the buffer.
    void take_progress_lines() {
        for (;;) {
            const std::size_t end = progress_text_.find('\n');
            if (end == std::string::npos)
                return;
            std::string_view line(progress_text_.data(), end);
            if (!line.empty() && line.back() == '\r')
                line.remove_suffix(1);
            constexpr std::string_view mark = "replay index: progress ";
            if (line.starts_with(mark)) {
                const std::string_view number = line.substr(mark.size());
                int percent = 0;
                bool digits = !number.empty() && number.size() <= 2;
                if (digits) {
                    for (const char letter : number) {
                        if (letter < '0' || letter > '9') {
                            digits = false;
                            break;
                        }
                        percent = percent * 10 + (letter - '0');
                    }
                }
                if (digits)
                    finding_percent_ = std::clamp(percent, 0, 99);
            }
            progress_text_.erase(0, end + 1);
        }
    }

    /// Measures what the time and the speed captions need in the font on show.
    /// The result is kept until the length, the language or the font changes.
    ///
    /// @return the widths; all zero while no font is open
    [[nodiscard]] rv::TextWidths measure_widths() {
        const kit::Fonts* fonts = runtime_.oa_layer().screen_fonts();
        if (fonts == nullptr)
            return {};
        const std::string time = rv::time_text(model_.length, model_.length);
        const std::string view_caption = rv::view_button_caption(model_);
        const std::string camera_caption = rv::camera_button_caption(model_.camera);
        std::string key = time;
        key += '\n';
        key += rv::speed_caption(16000, 9000, true, false);
        key += '\n';
        key += rv::speed_caption(1000, 1000, false, true);
        key += '\n';
        key += view_caption;
        key += '\n';
        key += camera_caption;
        if (fonts == widths_fonts_ && key == widths_key_)
            return widths_;
        const auto width = [fonts](const std::string& text) {
            return kit::text_width(*fonts, kit::FontRole::small, text);
        };
        rv::TextWidths widths;
        // Every digit of the length in turn, so the widest digit counts.
        for (char digit = '0'; digit <= '9'; ++digit) {
            std::string sample = time;
            for (char& letter : sample) {
                if (letter >= '0' && letter <= '9')
                    letter = digit;
            }
            widths.time = std::max(widths.time, width(sample));
        }
        rv::Model sample;
        sample.reached_lower = true;
        for (const uint32_t reached : {600U, 9000U, 15000U}) {
            sample.asked_milli = 16000;
            sample.reached_milli = reached;
            widths.speed_full =
                std::max(widths.speed_full, width(rv::speed_caption(16000, reached, true, false)));
            widths.speed_brief = std::max(
                widths.speed_brief, width(rv::fitted_speed_caption(sample, rv::SpeedForm::brief))
            );
        }
        for (const uint32_t asked : rv::speed_milli) {
            sample.asked_milli = asked;
            widths.speed_asked = std::max(
                widths.speed_asked, width(rv::fitted_speed_caption(sample, rv::SpeedForm::asked))
            );
        }
        widths.skip = width(rv::speed_caption(1000, 1000, false, true));
        widths.view = width(view_caption) + rv::menu_mark_points;
        widths.camera = width(camera_caption) + rv::menu_mark_points;
        widths_fonts_ = fonts;
        widths_key_ = std::move(key);
        widths_ = widths;
        return widths;
    }

    /// Removes the pass's timeline file once it has been read, and the old scratch.
    void remove_index_output() {
        std::error_code removed;
        if (!index_output_.empty())
            fs::remove(index_output_, removed);
        fs::remove(
            runtime_.user_folder_ / std::string(recordings_folder_name) / ".replays" /
                "watch-index.timeline.json",
            removed
        );
        if (!index_errors_.empty())
            fs::remove(index_errors_, removed);
        index_output_.clear();
        index_errors_.clear();
    }

    /// Logs a reason the pass wrote, when it wrote one.
    ///
    /// @param status the pass's exit code
    void log_index_reason(int status) {
        const std::string reason = index_pass_reason(index_errors_);
        if (reason.empty() && status == 0)
            return;
        std::cerr << "open-annihilation: replay index: ";
        if (!reason.empty())
            std::cerr << reason;
        else
            std::cerr << "exit " << status;
        std::cerr << '\n';
    }

    /// Starts the index pass on the watched file, the same way the Library
    /// starts one, with the content this replay is playing with: the mod
    /// folder this run layers and the map pack kept for the replay.
    void start_index_pass() {
        if (pass_started_)
            return;
        pass_started_ = true;
        if (watched_path_.empty() || runtime_.user_folder_.empty()) {
            store_index({});
            return;
        }
        IndexPassRequest request;
        request.recording = watched_path_;
        request.user_folder = runtime_.user_folder_;
        request.game_dir = runtime_.options_.game_dir;
        request.scratch = runtime_.user_folder_ / ".watch-index";
        request.output_name = "timeline.json";
        request.pipe_stdout = true;
        // What Watch mounted, not what the Library would choose now: the
        // replay may have restarted the game with a mod, and a map pack
        // fetched for it lives in Replay releases, outside Maps.
        request.mod_folder = runtime_.options_.layered_mod_folder;
        request.map_pack = runtime_.map_packs().replay_folder();
        const IndexPassHandles started = launch_index_pass(request);
        if (!started.started || started.process == nullptr) {
            store_index({});
            return;
        }
        index_process_ = started.process;
        index_pid_ = started.pid;
        index_output_ = started.output;
        index_errors_ = started.errors;
        index_stdout_ = SDL_GetProcessOutput(index_process_);
        finding_ = true;
        finding_percent_ = 0;
    }

    /// Takes the index once the pass has exited, and stores it for the Library.
    void poll_index_pass() {
        if (index_process_ == nullptr)
            return;
        drain_index_progress();
        int status = 0;
        if (!SDL_WaitProcess(index_process_, false, &status))
            return;
        drain_index_progress();
        index_stdout_ = nullptr;
        SDL_DestroyProcess(index_process_);
        index_process_ = nullptr;
        index_pid_ = 0;
        finding_ = false;
        try {
            if (status != 0) {
                log_index_reason(status);
                store_index({});
                remove_index_output();
                return;
            }
            const std::vector<uint8_t> bytes = read_bytes(index_output_);
            const std::string text(reinterpret_cast<const char*>(bytes.data()), bytes.size());
            const std::optional<oademo::TimelineIndex> timeline =
                oademo::parse_timeline_index(text);
            if (!timeline || !timeline_current(*timeline)) {
                log_index_reason(status);
                store_index({});
                remove_index_output();
                return;
            }
            BuiltIndex built = from_timeline(*timeline);
            attach_meta(built, watched_path_);
            const std::string relative = recording_below(runtime_.user_folder_, watched_path_);
            if (!relative.empty())
                runtime_.library_host().remember_watched_timeline(runtime_, relative, text);
            store_index(std::move(built));
        } catch (const std::exception&) {
            store_index({});
        }
        remove_index_output();
    }

    /// Stops the index pass when one is running.
    void stop_index_pass() {
        if (index_process_ == nullptr)
            return;
        static_cast<void>(SDL_KillProcess(index_process_, true));
        int status = 0;
        static_cast<void>(SDL_WaitProcess(index_process_, true, &status));
        index_stdout_ = nullptr;
        SDL_DestroyProcess(index_process_);
        index_process_ = nullptr;
        index_pid_ = 0;
        finding_ = false;
        remove_index_output();
    }

    /// Copies a finished index onto the model. Colours wait for the match.
    void take_index() {
        std::optional<BuiltIndex> taken;
        {
            oa::base::threads::LockGuard lock(index_mutex_);
            if (!index_ready_ || index_taken_)
                return;
            taken = std::move(built_);
            index_taken_ = true;
        }
        if (!taken)
            return;
        from_timeline_ = taken->from_timeline;
        model_.damaged = taken->damaged;
        model_.damage_tick = taken->damage_tick;
        model_.winner = taken->winner;
        model_.no_result = taken->no_result;
        model_.heat = std::move(taken->heat);
        model_.heat_period = taken->heat_period;
        model_.players.clear();
        player_numbers_.clear();
        for (BuiltPlayer& player : taken->players) {
            rv::PlayerRow row;
            row.name = player.name;
            row.kills = player.kills;
            row.losses = player.losses;
            row.watcher = player.watcher;
            player_numbers_.push_back(player.number);
            model_.players.push_back(std::move(row));
        }
        model_.markers.clear();
        pause_open_.clear();
        std::optional<std::size_t> waiting;
        for (RawMarker& raw : taken->markers) {
            if (raw.kind == rv::MarkerKind::pause && !raw.flag) {
                if (waiting) {
                    model_.markers[*waiting].end_tick = raw.tick;
                    pause_open_[*waiting] = false;
                    waiting.reset();
                }
                continue;
            }
            if (raw.kind == rv::MarkerKind::pause && waiting) {
                model_.markers[*waiting].end_tick = raw.tick;
                pause_open_[*waiting] = false;
                waiting.reset();
            }
            rv::Marker marker;
            marker.kind = raw.kind;
            marker.tick = raw.tick;
            marker.end_tick = raw.tick;
            marker.number = raw.number;
            marker.allied = raw.kind == rv::MarkerKind::alliance && raw.flag;
            marker.name = std::move(raw.name);
            marker.line = std::move(raw.line);
            const bool open = raw.kind == rv::MarkerKind::pause;
            model_.markers.push_back(std::move(marker));
            pause_open_.push_back(open);
            if (open)
                waiting = model_.markers.size() - 1;
        }
        meta_colours_ = std::move(taken->meta_colours);
        battles_ = std::move(taken->battles);
        // Director reads the events near the moment on show, found by tick.
        std::stable_sort(
            battles_.begin(),
            battles_.end(),
            [](const oademo::BattleEvent& left, const oademo::BattleEvent& right) {
                return left.tick < right.tick;
            }
        );
        director_started_ = false;
        if (taken->length > 0) {
            index_length_ = taken->length;
            index_length_known_ = true;
            model_.length = index_length_;
        }
    }

    /// Reads the replay the app is playing into the model.
    void read_playback() {
        oa::app::ReplayPlaybackView playback{};
        const bool filled = call_hook_or_report<&Extension::replay_state>(
            runtime_.extension_, runtime_.hook_error_report(), runtime_, &playback
        );
        if (!filled)
            return;
        // The frame reads this before it runs a seek queued earlier in the
        // frame, so that sample is the tick the jump leaves. Hold the tick the
        // jump asked for until the playback has landed. From the next sample
        // on, a playback that is not catching up has run the jump: on the
        // tick asked for, a few ticks past it, or nowhere when it was refused.
        // The bar follows it from there, so the time never stays on a target
        // the playback passed between two samples.
        if (seek_armed_) {
            ++seek_samples_;
            if (!playback.skipping && (playback.tick == model_.tick || seek_samples_ > 1))
                seek_armed_ = false;
        }
        const bool caught_up = !playback.skipping && playback.catch_up_percent < 0;
        const bool playback_ended =
            caught_up && playback.length_known > 0 && playback.tick >= playback.length_known;
        const bool index_ended = caught_up && index_length_known_ && playback.tick >= index_length_;
        const bool ended = playback_ended || index_ended;
        // A sample from before a queued seek still looks ended. Snapping that
        // sample onto the index length would stick a jump back to the start.
        // A jump to the end is the index length, and the last loaded frame can
        // sit a few ticks short of it, so that jump does snap.
        const bool snap_end =
            ended && index_length_known_ && (!seek_armed_ || model_.tick >= index_length_);
        if (snap_end) {
            model_.tick = index_length_;
            seek_armed_ = false;
        } else if (!seek_armed_) {
            model_.tick = playback.tick;
        }
        if (index_length_known_)
            model_.length = index_length_;
        else
            model_.length = playback.length_known;
        replay_paused_ = playback.paused;
        model_.paused = replay_paused_ || runtime_.match_paused_;
        model_.skipping = playback.skipping;
        model_.catch_up_percent = playback.catch_up_percent;
        if (playback.asked_milli != 0)
            model_.asked_milli = playback.asked_milli;
        else if (model_.asked_milli == 0)
            model_.asked_milli = 1000;
        if (playback.reached_milli != 0) {
            smooth_reached_ = smooth_reached_ == 0
                                  ? playback.reached_milli
                                  : (smooth_reached_ * 3 + playback.reached_milli) / 4;
        }
        model_.reached_milli = smooth_reached_ == 0 ? model_.asked_milli : smooth_reached_;
        const bool standing = model_.length > 0 && model_.tick >= model_.length;
        // The in-game menu holds the replay without the bar's own pause. That
        // hold is paused too, or the caption would say the picture has stopped.
        const bool quiet = replay_paused_ || runtime_.match_paused_ || model_.skipping ||
                           standing || model_.asked_milli == 0;
        const bool slower = smooth_reached_ > 0 && smooth_reached_ < model_.asked_milli;
        const uint32_t now = oa::platform::tick_ms();
        if (!quiet && slower) {
            if (!behind_since_)
                behind_since_ = now;
            model_.reached_lower = now - *behind_since_ >= kBehindMs;
        } else {
            behind_since_.reset();
            model_.reached_lower = false;
        }
        model_.ready.clear();
        if (playback.ready != nullptr) {
            model_.ready.reserve(playback.ready_count);
            for (std::size_t place = 0; place < playback.ready_count; ++place)
                model_.ready.push_back({playback.ready[place].begin, playback.ready[place].end});
        }
        model_.chat_shown = !runtime_.replay_chat_hidden_;
        model_.end_card = ended;
        extend_pauses();
        ensure_end_marker();
        paint();
        if (!from_timeline_)
            take_live_totals();
    }

    /// Extends a pause that never resumed out to the length.
    void extend_pauses() {
        const uint32_t end = model_.length;
        for (std::size_t place = 0; place < model_.markers.size() && place < pause_open_.size();
             ++place) {
            if (pause_open_[place])
                model_.markers[place].end_tick = end > 0 ? end : model_.markers[place].tick;
        }
    }

    /// Adds the end flag once the length is known.
    void ensure_end_marker() {
        if (model_.length == 0)
            return;
        for (rv::Marker& marker : model_.markers) {
            if (marker.kind != rv::MarkerKind::end)
                continue;
            marker.tick = model_.length;
            marker.end_tick = model_.length;
            return;
        }
        rv::Marker marker;
        marker.kind = rv::MarkerKind::end;
        marker.tick = model_.length;
        marker.end_tick = model_.length;
        model_.markers.push_back(std::move(marker));
        pause_open_.push_back(false);
    }

    /// Colours each row and marker from the match, by name, else by number.
    void paint() {
        const oa::sim::match_runtime::Match* match = runtime_.running_match();
        player_colour_used_.assign(model_.players.size(), -1);
        for (std::size_t place = 0; place < model_.players.size(); ++place) {
            const int number = place < player_numbers_.size() ? player_numbers_[place] : -1;
            const int colour = colour_of(match, model_.players[place].name, number);
            player_colour_used_[place] = colour;
            model_.players[place].colour = ink_of(colour);
        }
        for (rv::Marker& marker : model_.markers)
            marker.colour = ink_of(colour_of(match, marker.name, marker.number));
    }

    /// The colour the HUD's marks use for a colour number, through the palette.
    ///
    /// @param colour the colour number; negative is the label
    /// @return the colour
    [[nodiscard]] kit::Colour ink_of(int colour) const {
        if (colour < 0)
            return kit::hud_colour::label;
        const int wrapped = colour % 10;
        namespace vision = oa::present::colour_vision;
        const uint8_t index = vision::active_mode() == vision::Mode::off
                                  ? oa::ui::hud::kPlayerDotColors[static_cast<std::size_t>(wrapped)]
                                  : vision::player_dot(wrapped);
        const std::size_t at = static_cast<std::size_t>(index) * 4U;
        if (at + 2 >= runtime_.match_palette_.size())
            return kit::hud_colour::label;
        return {
            runtime_.match_palette_[at],
            runtime_.match_palette_[at + 1],
            runtime_.match_palette_[at + 2],
            255
        };
    }

    /// Keeps a colour the match showed, the first time, under the player's number.
    ///
    /// @param number the player's number; negative is ignored
    /// @param colour the colour number; negative is ignored
    void remember_colour(int number, int colour) {
        if (number < 0 || colour < 0)
            return;
        const std::size_t slot = static_cast<std::size_t>(number);
        if (remembered_colours_.size() <= slot)
            remembered_colours_.resize(slot + 1, -1);
        if (remembered_colours_[slot] < 0)
            remembered_colours_[slot] = colour;
    }

    /// A colour the recording names for this player, by number, else by name.
    ///
    /// @param name the player's name
    /// @param number the player's number; negative skips the number
    /// @return the colour number, or -1
    [[nodiscard]] int recorded_colour(std::string_view name, int number) const {
        if (number >= 0) {
            for (const BuiltIndex::MetaColour& meta : meta_colours_) {
                if (meta.number == number && meta.colour >= 0)
                    return meta.colour;
            }
        }
        if (!name.empty()) {
            for (const BuiltIndex::MetaColour& meta : meta_colours_) {
                if (meta.name == name && meta.colour >= 0)
                    return meta.colour;
            }
        }
        return -1;
    }

    /// The colour of one live slot.
    ///
    /// @param match the match; null has no live colour
    /// @param slot the live slot; negative is none
    /// @return the colour number, or -1
    [[nodiscard]] int live_colour_at(const oa::sim::match_runtime::Match* match, int slot) const {
        if (match == nullptr || slot < 0 || slot >= OA_PLAYER_COUNT)
            return -1;
        const oa::World& world = match->state();
        const oa::Player& player = world.game.players[slot];
        if (player.in_use == 0 || player.index >= OA_PLAYER_COUNT)
            return -1;
        const oa::PlayerSetupInfo* info = oa::world_player_info(&world, &player);
        return info != nullptr ? static_cast<int>(info->color) : -1;
    }

    /// Returns a player's colour: the match, then a colour it showed before,
    /// then the recording, and only then the number.
    ///
    /// The live colour is the slot of the recorded number, never the first
    /// slot that shares the side's name.
    ///
    /// @param match the match; null has no live colour
    /// @param name the player's name
    /// @param number the player's number; negative is none
    /// @param slot_known a live slot already chosen; negative looks the number up
    /// @return the colour number, or -1
    int colour_of(
        const oa::sim::match_runtime::Match* match,
        std::string_view name,
        int number,
        int slot_known = -1
    ) {
        const int seat =
            slot_known >= 0 ? slot_known : rv::watch_slot(player_numbers_, number, OA_PLAYER_COUNT);
        const int live = live_colour_at(match, seat);
        if (live >= 0)
            remember_colour(number, live);
        int remembered = -1;
        if (number >= 0) {
            const std::size_t remembered_at = static_cast<std::size_t>(number);
            if (remembered_at < remembered_colours_.size())
                remembered = remembered_colours_[remembered_at];
        }
        return rv::player_colour(live, remembered, recorded_colour(name, number), number);
    }

    /// Copies kills and losses from the match when the index did not bring finals.
    void take_live_totals() {
        const oa::sim::match_runtime::Match* match = runtime_.running_match();
        if (match == nullptr)
            return;
        const oa::World& world = match->state();
        for (std::size_t place = 0; place < model_.players.size(); ++place) {
            rv::PlayerRow& row = model_.players[place];
            const int number = place < player_numbers_.size() ? player_numbers_[place] : -1;
            const int slot = rv::watch_slot(player_numbers_, number, OA_PLAYER_COUNT);
            if (slot < 0)
                continue;
            const oa::Player& player = world.game.players[slot];
            if (player.in_use == 0)
                continue;
            row.kills = player.kills;
            row.losses = player.losses;
        }
    }

    /// Moves the hide timer on from the wall clock.
    void advance_visibility() {
        const uint32_t now = oa::platform::tick_ms();
        const uint32_t step = last_ms_ == 0 ? 0 : now - last_ms_;
        last_ms_ = now;
        const int32_t pointer_y = pointer_over_ ? pointer_y_ : 0;
        visibility_.advance(step, pointer_y, points_height_, layout_.bar.y);
    }

    /// Lays the bar out and rebuilds the controls automation lists.
    void refresh() {
        points_width_ = std::max(points_width_, int32_t{1});
        points_height_ = std::max(points_height_, int32_t{1});
        fill_watch();
        model_.can_export = !recording_below(runtime_.user_folder_, watched_path_).empty();
        model_.finding = finding_;
        model_.finding_percent = finding_ ? finding_percent_ : 0;
        model_.widths = measure_widths();
        // F4 shows or hides the stats panel. The resource panel's action
        // takes that key when the hack is on, so the card lists whichever of
        // the two is bound, under the panel's name.
        std::vector<std::string> key_ids{
            "watch.replay-pause",
            "watch.replay-back",
            "watch.replay-forward",
            "watch.replay-back-long",
            "watch.replay-forward-long",
            "watch.replay-previous-marker",
            "watch.replay-next-marker",
            "watch.replay-start",
            "watch.replay-end",
            "watch.replay-bar",
            "watch.replay-keys",
            "watch.next-player-view",
            "watch.previous-player-view",
            "watch.whole-view",
            "watch.camera-mode",
        };
        const std::string resource_panel = runtime_.bound_chords_text("game.resource-panel");
        key_ids.push_back(resource_panel.empty() ? "game.kill-board" : "game.resource-panel");
        model_.key_chords.clear();
        model_.key_chords.reserve(key_ids.size());
        for (const std::string& id : key_ids) {
            rv::KeyChord row;
            const bool panel_key = id == "game.kill-board" || id == "game.resource-panel";
            const std::string named = panel_key ? "game.resource-panel" : id;
            if (const oa::data::input_actions::Action* action =
                    oa::data::input_actions::find(named))
                row.action = std::string(action->words.name);
            row.chord = runtime_.bound_chords_text(id);
            model_.key_chords.push_back(std::move(row));
        }
        layout_ = rv::lay_out(points_width_, points_height_, model_, visibility_.shown());
        model_.pointer_over = pointer_over_;
        model_.pointer_x = pointer_x_;
        model_.pointer_y = pointer_y_;
        rv::fill_controls(layout_, model_, shown_);
        export_open_ = runtime_.library_host().model.export_dialog.open;
        if (!export_open_)
            return;
        const kit::Fonts* fonts = runtime_.oa_layer().screen_fonts();
        if (fonts == nullptr)
            return;
        kit::Frame library_frame;
        library_frame.size_class = kit::SizeClass::compact;
        library_frame.scale_percent = 100;
        library_frame.area = {0, 0, points_width_, points_height_};
        export_size_ = library::window_size(library_frame);
        library_frame.area = {0, 0, export_size_.x, export_size_.y};
        export_origin_ = {
            std::max((points_width_ - export_size_.x) / 2, int32_t{0}),
            std::max((points_height_ - export_size_.y) / 2, int32_t{0})
        };
        library_list_ = library::library_layout(
            runtime_.library_host().model, runtime_.library_host().state, library_frame, *fonts
        );
        keep_export_sheet();
        std::vector<kit::Control> stacked;
        stacked.reserve(library_list_.controls.size() + shown_.controls.size());
        for (const kit::Control& control : library_list_.controls) {
            kit::Control shifted = control;
            shifted.rect.x += export_origin_.x;
            shifted.rect.y += export_origin_.y;
            if (shifted.clip.width != 0 || shifted.clip.height != 0) {
                shifted.clip.x += export_origin_.x;
                shifted.clip.y += export_origin_.y;
            }
            stacked.push_back(std::move(shifted));
        }
        for (kit::Control& control : shown_.controls)
            stacked.push_back(std::move(control));
        shown_.controls = std::move(stacked);
    }

    /// Keeps the export dialog's own rectangle: the shade and the Library behind it go.
    void keep_export_sheet() {
        std::size_t shade = library_list_.items.size();
        for (std::size_t index = 0; index < library_list_.items.size(); ++index) {
            if (library_list_.items[index].role == kit::Role::blend)
                shade = index;
        }
        if (shade < library_list_.items.size()) {
            library_list_.items.erase(
                library_list_.items.begin(),
                library_list_.items.begin() + static_cast<std::ptrdiff_t>(shade) + 1
            );
        }
        std::vector<kit::Control> kept;
        for (kit::Control& control : library_list_.controls) {
            if (control.name.find("export.") == std::string::npos)
                continue;
            if (control.name.find("export.block") != std::string::npos)
                continue;
            kept.push_back(std::move(control));
        }
        library_list_.controls = std::move(kept);
        kit::Rect bounds{};
        bool any = false;
        for (const kit::Item& item : library_list_.items) {
            if (!any) {
                bounds = item.rect;
                any = true;
                continue;
            }
            const int32_t right = std::max(bounds.x + bounds.width, item.rect.x + item.rect.width);
            const int32_t bottom =
                std::max(bounds.y + bounds.height, item.rect.y + item.rect.height);
            bounds.x = std::min(bounds.x, item.rect.x);
            bounds.y = std::min(bounds.y, item.rect.y);
            bounds.width = right - bounds.x;
            bounds.height = bottom - bounds.y;
        }
        export_dialog_ = any ? bounds : kit::Rect{};
    }

    /// Advances an open export. The dialog's worker writes between pumps.
    void pump_export() {
        if (!runtime_.library_host().model.export_dialog.open)
            return;
        static_cast<void>(runtime_.library_host().pump_replays(runtime_));
    }

    /// Remembers the pointer. A point on the lower edge shows the bar.
    ///
    /// @param at the pointer
    void note_pointer(kit::Point at) {
        if (points_width_ <= 0 || points_height_ <= 0) {
            points_width_ = std::max(at.x + 1, int32_t{640});
            points_height_ = std::max(at.y + 1, int32_t{480});
            refresh();
        }
        pointer_over_ = true;
        pointer_x_ = at.x;
        pointer_y_ = at.y;
        if (layout_.bar.height > 0 && at.y + rv::edge_band_points >= layout_.bar.y)
            visibility_.show();
        if (visibility_.shown() != layout_.shown)
            refresh();
        interaction_.hovered = kit::no_control;
    }

    /// Hands a point in the export window to the Library.
    ///
    /// @param kind the pointer event
    /// @param button the button
    /// @param at the pointer, in the screen's points
    /// @return redraw when the dialog changed
    LayerAnswer take_export(ScreenInputKind kind, uint8_t button, kit::Point at) {
        library::PointerKind mapped = library::PointerKind::move;
        if (kind == ScreenInputKind::pointer_down)
            mapped = library::PointerKind::down;
        else if (kind == ScreenInputKind::pointer_up)
            mapped = library::PointerKind::up;
        const kit::Point local{at.x - export_origin_.x, at.y - export_origin_.y};
        library::ScreenResult result = library::library_pointer(
            runtime_.library_host().model,
            runtime_.library_host().state,
            library_list_,
            mapped,
            button,
            local,
            0
        );
        const bool closed = runtime_.library_host().replay_action(runtime_, result);
        if (result.redraw || result.action.has_value() || closed) {
            refresh();
            ++revision_;
            return LayerAnswer::redraw;
        }
        return LayerAnswer::none;
    }

    /// Does what a control does on release.
    ///
    /// @param control the control
    /// @return redraw, or pass for a control that is not one
    LayerAnswer activate(rv::Control control) {
        const uint32_t milli = rv::milli_of(control);
        if (milli != 0) {
            set_speed(static_cast<int32_t>(milli));
            model_.speed_open = false;
            model_.asked_milli = milli;
            refresh();
            ++revision_;
            return LayerAnswer::redraw;
        }
        switch (control) {
        case rv::Control::back:
            seek_by(-kStepTicks);
            break;
        case rv::Control::play:
            if (replay_paused_)
                play();
            else
                pause();
            break;
        case rv::Control::forward:
            seek_by(kStepTicks);
            break;
        case rv::Control::speed:
            model_.speed_open = !model_.speed_open;
            if (model_.speed_open) {
                model_.view_open = false;
                model_.camera_open = false;
            }
            break;
        case rv::Control::view:
            model_.view_open = !model_.view_open;
            if (model_.view_open) {
                model_.camera_open = false;
                model_.speed_open = false;
            }
            break;
        case rv::Control::camera:
            model_.camera_open = !model_.camera_open;
            if (model_.camera_open) {
                model_.view_open = false;
                model_.speed_open = false;
            }
            break;
        case rv::Control::photo:
            static_cast<void>(runtime_.enter_photo_mode());
            break;
        case rv::Control::chat:
            runtime_.replay_chat_hidden_ = !runtime_.replay_chat_hidden_;
            model_.chat_shown = !runtime_.replay_chat_hidden_;
            break;
        case rv::Control::keys:
            model_.keys_open = !model_.keys_open;
            break;
        case rv::Control::leave:
            runtime_.show_match_pause_menu();
            break;
        case rv::Control::watch_again:
            seek_to(0);
            play();
            break;
        case rv::Control::library:
            back_to_library();
            break;
        case rv::Control::export_replay:
            open_export();
            break;
        case rv::Control::timeline:
        case rv::Control::keys_card:
        case rv::Control::end_card:
        case rv::Control::none:
        case rv::Control::speed_500:
        case rv::Control::speed_1000:
        case rv::Control::speed_2000:
        case rv::Control::speed_4000:
        case rv::Control::speed_8000:
        case rv::Control::speed_16000:
            break;
        }
        refresh();
        ++revision_;
        return LayerAnswer::redraw;
    }

    /// Plays the replay.
    void play() {
        call_hook_or_report<&Extension::replay_play>(
            runtime_.extension_, runtime_.hook_error_report(), runtime_
        );
        replay_paused_ = false;
        model_.paused = runtime_.match_paused_;
    }

    /// Pauses the replay.
    void pause() {
        call_hook_or_report<&Extension::replay_pause>(
            runtime_.extension_, runtime_.hook_error_report(), runtime_
        );
        replay_paused_ = true;
        model_.paused = true;
    }

    /// Sets the speed, in thousandths of 1×.
    ///
    /// @param milli the speed
    void set_speed(int32_t milli) {
        call_hook_or_report<&Extension::replay_set_speed>(
            runtime_.extension_, runtime_.hook_error_report(), runtime_, milli
        );
    }

    /// Seeks by a number of ticks, kept inside the recording.
    ///
    /// @param delta the ticks to add; negative jumps back
    void seek_by(int32_t delta) {
        const int64_t next = static_cast<int64_t>(model_.tick) + delta;
        uint32_t tick = next < 0 ? 0 : static_cast<uint32_t>(std::min(next, int64_t{0x7fffffff}));
        if (model_.length > 0 && tick > model_.length)
            tick = model_.length;
        seek_to(tick);
    }

    /// Seeks to a tick.
    ///
    /// @param tick the tick; past the length lands on the length
    void seek_to(uint32_t tick) {
        if (model_.length > 0 && tick > model_.length)
            tick = model_.length;
        model_.tick = tick;
        seek_armed_ = true;
        seek_samples_ = 0;
        call_hook_or_report<&Extension::replay_seek>(
            runtime_.extension_, runtime_.hook_error_report(), runtime_, tick
        );
    }

    /// Seeks to the nearest marker before or after the tick on show.
    ///
    /// @param forward the next marker; otherwise the previous
    void seek_marker(bool forward) {
        std::optional<uint32_t> found;
        for (const rv::Marker& marker : model_.markers) {
            const bool candidate = forward ? marker.tick > model_.tick : marker.tick < model_.tick;
            if (!candidate)
                continue;
            if (!found || (forward ? marker.tick < *found : marker.tick > *found))
                found = marker.tick;
        }
        if (found)
            seek_to(*found);
    }

    /// Leaves the match for the Library's replays.
    void back_to_library() {
        if (!runtime_.library_return_) {
            auto shown = std::make_shared<LibraryReturn>();
            shown->tab = library::Tab::replays;
            runtime_.library_return_ = std::move(shown);
            runtime_.library_return_since_.reset();
        }
        runtime_.replay_library_requested_ = true;
    }

    /// Opens Export for this recording.
    void open_export() {
        const std::string path =
            recording_below(runtime_.user_folder_, runtime_.replay_recording_path_);
        if (path.empty())
            return;
        runtime_.library_host().open_replay_export(runtime_, path);
        refresh();
    }

    /// Shows one live slot, every player when `slot` is -1, or nothing of a
    /// seat that is gone when `slot` is below -1.
    ///
    /// A gone seat keeps its row. The view is the watcher's own: the match
    /// no longer holds that player's sight.
    ///
    /// @param slot the live slot, -1 for the whole map, or below -1 for a gone seat
    void show_player(int slot) {
        model_.view_open = false;
        model_.camera_open = false;
        if (slot < -1) {
            const bool already = viewed_slot_ == slot;
            viewed_slot_ = slot;
            if (!already)
                runtime_.switch_watched_view(oa::ui::hud::ViewSwitch::own_view);
            return;
        }
        if (slot < 0 || slot >= OA_PLAYER_COUNT) {
            const bool already = viewed_slot_ < 0 && viewed_slot_ >= -1;
            viewed_slot_ = -1;
            if (!already)
                runtime_.switch_watched_view(oa::ui::hud::ViewSwitch::own_view);
            return;
        }
        if (viewed_slot_ == slot)
            return;
        runtime_.resource_panel_.viewed_slot = static_cast<uint8_t>(slot);
        runtime_.switch_watched_view(oa::ui::hud::ViewSwitch::view_player);
        viewed_slot_ = slot;
    }

    /// Follows one offered camera mode. Follow a unit is not offered here.
    ///
    /// @param mode the mode
    void use_camera(rv::CameraMode mode) {
        if (mode == rv::CameraMode::follow_unit)
            return;
        if (mode == rv::CameraMode::follow_theirs && !runtime_.replay_shares_camera_)
            return;
        if (mode == rv::CameraMode::director && !director_offered())
            return;
        if (camera_mode_ == rv::CameraMode::follow_unit)
            runtime_.stop_match_tracking();
        camera_mode_ = mode;
        model_.camera = mode;
        model_.view_open = false;
        model_.camera_open = false;
        if (mode == rv::CameraMode::director)
            director_started_ = false;
    }

    /// Shows the view menu's row.
    ///
    /// @param index the row
    void choose_view_row(int index) {
        if (index < 0 || static_cast<std::size_t>(index) >= model_.watch.size())
            return;
        show_player(model_.watch[static_cast<std::size_t>(index)].slot);
    }

    /// Follows the camera menu's mode.
    ///
    /// @param index the row
    void choose_camera_row(int index) {
        if (index < 0 || static_cast<std::size_t>(index) >= layout_.camera_modes.size())
            return;
        use_camera(layout_.camera_modes[static_cast<std::size_t>(index)]);
    }

    /// "Defeated 18:02" or "Left 21:40" for a player who has gone by this tick.
    ///
    /// The latest such marker at or before the tick on show wins. A dropped
    /// connection is shown as left. The recorded number picks the marker, so
    /// two players of one side do not share a departure.
    ///
    /// @param name the player's name, used when no number is known
    /// @param number the player's recorded number; negative matches the name
    /// @return the line; empty while the player is still in
    [[nodiscard]] std::string departure_of(std::string_view name, int number) const {
        const rv::Marker* latest = nullptr;
        for (const rv::Marker& marker : model_.markers) {
            const bool same = number >= 0 ? marker.number == number : marker.name == name;
            if (!same)
                continue;
            if (marker.kind != rv::MarkerKind::defeated && marker.kind != rv::MarkerKind::left &&
                marker.kind != rv::MarkerKind::dropped)
                continue;
            if (marker.tick > model_.tick)
                continue;
            if (latest == nullptr || marker.tick >= latest->tick)
                latest = &marker;
        }
        if (latest == nullptr)
            return {};
        return rv::departed_caption(
            latest->kind == rv::MarkerKind::defeated, clock_of(latest->tick)
        );
    }

    /// Adds one player to the stats panel's seats, as its view row shows them.
    ///
    /// @param slot the player's live slot
    /// @param colour the colour number the row is drawn in; negative is none
    /// @param gone the seat has left the match
    /// @param row the player's place in the view rows
    void add_panel_seat(int slot, int colour, bool gone, std::size_t row) {
        if (slot < 0 || slot >= OA_PLAYER_COUNT)
            return;
        const ReplayShownSlot& shown = runtime_.replay_shown_[static_cast<std::size_t>(slot)];
        oa::ui::hud::ResourcePanelSeat seat;
        seat.slot = static_cast<uint8_t>(slot);
        // The palette index ink_of draws the row's colour in.
        seat.colour = colour >= 0 ? oa::ui::hud::colour_dot_index(static_cast<uint8_t>(colour % 10))
                                  : oa::ui::hud::kResourcePanelNameColor;
        seat.economy = shown.economy;
        seat.income = shown.income;
        seat.metal_income = shown.metal;
        seat.energy_income = shown.energy;
        seat.gone = gone;
        panel_seats_.push_back(seat);
        panel_rows_.push_back(static_cast<int>(row));
    }

    /// Rebuilds the view menu's rows and the stats panel's seats. Once the
    /// index has arrived the rows are its players for the whole replay.
    /// Before that, only a live computer or a mirrored slot is listed. A
    /// watcher is never listed. The colour stays.
    void fill_watch() {
        model_.watch.clear();
        panel_seats_.clear();
        panel_rows_.clear();
        rv::WatchRow everyone;
        everyone.slot = -1;
        everyone.name = rv::view_button_caption(rv::Model{});
        everyone.colour = kit::hud_colour::label;
        model_.watch.push_back(std::move(everyone));
        const oa::sim::match_runtime::Match* match = runtime_.running_match();
        const bool from_index = index_arrived() && !model_.players.empty();
        if (from_index) {
            const oa::World* world = match != nullptr ? &match->state() : nullptr;
            for (std::size_t place = 0; place < model_.players.size(); ++place) {
                const rv::PlayerRow& card = model_.players[place];
                if (card.watcher || card.name.empty())
                    continue;
                const int number = place < player_numbers_.size() ? player_numbers_[place] : -1;
                const int found = world != nullptr
                                      ? rv::watch_slot(player_numbers_, number, OA_PLAYER_COUNT)
                                      : -1;
                const oa::Player* live = nullptr;
                if (found >= 0) {
                    const oa::Player& player = world->game.players[found];
                    if (player.in_use != 0 && player.index < OA_PLAYER_COUNT)
                        live = &player;
                }
                const bool seated = live != nullptr;
                rv::WatchRow row;
                row.slot = seated ? found : -2 - static_cast<int>(place);
                row.name = card.name;
                row.departed = departure_of(card.name, number);
                const int colour = colour_of(match, card.name, number, found);
                row.colour = ink_of(colour);
                if (found >= 0)
                    add_panel_seat(found, colour, !seated, model_.watch.size());
                model_.watch.push_back(std::move(row));
            }
        } else if (match != nullptr) {
            const oa::World& world = match->state();
            for (int slot = 0; slot < OA_PLAYER_COUNT; ++slot) {
                const oa::Player& player = world.game.players[slot];
                if (player.in_use == 0 || player.index >= OA_PLAYER_COUNT)
                    continue;
                if (player.status != OA_PLAYER_STATUS_COMPUTER &&
                    player.status != OA_PLAYER_STATUS_MIRRORED)
                    continue;
                const std::string name = copied_name(player);
                if (name.empty())
                    continue;
                const oa::PlayerSetupInfo* info = oa::world_player_info(&world, &player);
                if (info != nullptr && (info->options & OA_SETUP_OPTION_WATCHER) != 0)
                    continue;
                rv::WatchRow row;
                row.slot = slot;
                row.name = name;
                row.departed = departure_of(name, -1);
                const int colour = colour_of(match, name, -1, slot);
                row.colour = ink_of(colour);
                add_panel_seat(slot, colour, false, model_.watch.size());
                model_.watch.push_back(std::move(row));
            }
        }
        bool found = false;
        model_.viewed = 0;
        for (std::size_t place = 0; place < model_.watch.size(); ++place) {
            rv::WatchRow& row = model_.watch[place];
            row.eye = row.slot == viewed_slot_;
            if (!row.eye)
                continue;
            model_.viewed = static_cast<int>(place);
            found = true;
        }
        if (!found && !model_.watch.empty()) {
            viewed_slot_ = -1;
            model_.watch[0].eye = true;
            model_.viewed = 0;
            runtime_.switch_watched_view(oa::ui::hud::ViewSwitch::own_view);
        }
        model_.camera = camera_mode_;
        model_.offer_follow_camera = runtime_.replay_shares_camera_;
        model_.offer_director = director_offered();
    }

    /// Moves the camera for the mode on show. Free does not.
    ///
    /// A followed unit that has gone returns the camera to Free.
    void apply_camera() {
        if (camera_mode_ == rv::CameraMode::follow_unit && !runtime_.match_tracking()) {
            camera_mode_ = rv::CameraMode::free;
            model_.camera = camera_mode_;
            return;
        }
        if (camera_mode_ == rv::CameraMode::follow_theirs)
            apply_recorded_camera();
        else if (camera_mode_ == rv::CameraMode::director)
            apply_director_camera();
    }

    /// Centres the view on a recorded camera.
    ///
    /// The player on show is used when one is. While every player is shown,
    /// the first player who shared a camera is followed. A player whose
    /// camera was not shared does not move the view.
    void apply_recorded_camera() {
        int slot = viewed_slot_;
        if (slot < 0) {
            slot = -1;
            for (int candidate = 0; candidate < OA_PLAYER_COUNT; ++candidate) {
                if (runtime_.replay_shown_[static_cast<std::size_t>(candidate)].camera_shared) {
                    slot = candidate;
                    break;
                }
            }
        }
        if (slot < 0 || slot >= OA_PLAYER_COUNT)
            return;
        const ReplayShownSlot& shown = runtime_.replay_shown_[static_cast<std::size_t>(slot)];
        if (!shown.camera_shared)
            return;
        const int32_t x = static_cast<int32_t>(shown.camera_x) - runtime_.visible_map_width() / 2;
        const int32_t z = static_cast<int32_t>(shown.camera_y) - runtime_.visible_map_height() / 2;
        runtime_.set_camera_position(x, z, 0);
    }

    /// Puts the middle of the picture on a place.
    ///
    /// @param centre_x the place across the map
    /// @param centre_z the place down the map, on the row the picture draws
    void place_centre(int32_t centre_x, int32_t centre_z) {
        runtime_.set_camera_position(
            centre_x - runtime_.visible_map_width() / 2,
            centre_z - runtime_.visible_map_height() / 2,
            0
        );
    }

    /// The battle events within a minute either side of a tick: all that
    /// replay_index::director_view reads at that tick. The list is in tick
    /// order, so a step costs the same however many events the index keeps.
    ///
    /// @param tick the moment
    /// @return the events from battle_lull_ticks before the tick to as many after
    [[nodiscard]] std::span<const oademo::BattleEvent> battles_near(uint32_t tick) const {
        constexpr uint32_t reach = replay_index::battle_lull_ticks;
        constexpr uint32_t most = std::numeric_limits<uint32_t>::max();
        const uint32_t from = tick > reach ? tick - reach : 0U;
        const uint32_t to = tick > most - reach ? most : tick + reach;
        const auto first = std::partition_point(
            battles_.begin(), battles_.end(), [from](const oademo::BattleEvent& event) {
                return event.tick < from;
            }
        );
        const auto last =
            std::partition_point(first, battles_.end(), [to](const oademo::BattleEvent& event) {
                return event.tick <= to;
            });
        return {first, last};
    }

    /// Plays Director from the start of the battle up to a tick, as 1× would.
    ///
    /// A seek then shows the same place the steady playback would. The wall
    /// clock is set so the pan still under way continues in real time.
    ///
    /// @param tick the moment on show
    /// @param now the wall clock, in milliseconds
    /// @param view_width the picture's width, in map pixels
    /// @param view_height the picture's height, in map pixels
    void resimulate_director(uint32_t tick, uint32_t now, int32_t view_width, int32_t view_height) {
        director_view_ = {};
        for (uint32_t at = 0; at <= tick; ++at) {
            uint32_t since = 0;
            if (director_view_.has_subject && at >= director_view_.cut_tick)
                since = (at - director_view_.cut_tick) * 1000U / 30U;
            director_view_ = replay_index::director_view(
                battles_near(at), at, since, director_view_, view_width, view_height
            );
        }
        const uint32_t synthetic = director_view_.has_subject && tick >= director_view_.cut_tick
                                       ? (tick - director_view_.cut_tick) * 1000U / 30U
                                       : 0U;
        director_wall_ms_ = now - synthetic;
        director_tick_ = tick;
        director_started_ = true;
    }

    /// Shows the one fight Director is following.
    ///
    /// The picture eases toward a nearby fight and cuts to a far one. It does
    /// not play the filmed script. A jump backward, or a jump of more than
    /// ten seconds forward, is taken as a seek and matched to a steady 1×
    /// playback. While the playback runs, real time holds each view, so a
    /// fast speed skips fights rather than cutting faster. Before the first
    /// fight, the picture shows the first commander's start.
    void apply_director_camera() {
        const uint32_t tick = model_.tick;
        const uint32_t now = oa::platform::tick_ms();
        const int32_t view_width = runtime_.visible_map_width();
        const int32_t view_height = runtime_.visible_map_height();
        constexpr uint32_t seek_gap = replay_index::director_return_ticks;
        const bool jumped_back = director_started_ && tick < director_tick_;
        const bool jumped_forward = director_started_ && tick > director_tick_ + seek_gap;
        if (!director_started_ || jumped_back || jumped_forward) {
            resimulate_director(tick, now, view_width, view_height);
        } else if (tick > director_tick_) {
            for (uint32_t at = director_tick_ + 1; at <= tick; ++at) {
                const uint32_t since = now - director_wall_ms_;
                director_view_ = replay_index::director_view(
                    battles_near(at), at, since, director_view_, view_width, view_height
                );
                if (director_view_.changed)
                    director_wall_ms_ = now;
            }
            director_tick_ = tick;
        } else {
            const uint32_t since = now - director_wall_ms_;
            director_view_ = replay_index::director_view(
                battles_near(tick), tick, since, director_view_, view_width, view_height
            );
            if (director_view_.changed)
                director_wall_ms_ = now;
        }
        if (director_view_.has_subject) {
            place_centre(director_view_.x, director_view_.z);
            return;
        }
        // Before the first fight, a start base rather than wherever the
        // picture happened to be.
        const replay_index::BattleAim opening = replay_index::director_opening(battles_);
        if (opening.found)
            place_centre(opening.x, opening.z);
    }

    Runtime& runtime_;
    rv::Model model_{};
    rv::Layout layout_{};
    rv::BarVisibility visibility_{};
    kit::DisplayList shown_{};
    kit::DisplayList library_list_{};
    kit::Interaction interaction_{};
    kit::Point export_origin_{};
    kit::Point export_size_{};
    kit::Rect export_dialog_{};
    bool export_open_{};
    bool pointer_over_{};
    bool dragging_{};
    bool from_timeline_{};
    bool index_ready_{};
    bool index_taken_{};
    bool needs_pass_{};
    bool pass_started_{};
    bool finding_{};
    bool replay_paused_{true};
    bool seek_armed_{};
    bool reset_after_seek_{};
    rv::CameraMode camera_mode_{rv::CameraMode::free};
    bool director_started_{};
    uint32_t director_tick_{};
    uint32_t director_wall_ms_{};
    replay_index::DirectorView director_view_{};
    std::vector<oademo::BattleEvent> battles_{};
    int viewed_slot_{-1};
    int held_view_item_{-1};
    int held_camera_item_{-1};
    int seek_samples_{};
    bool index_length_known_{};
    int32_t pointer_x_{};
    int32_t pointer_y_{};
    int32_t points_width_{};
    int32_t points_height_{};
    uint32_t last_ms_{};
    uint32_t smooth_reached_{};
    uint32_t index_length_{};
    int finding_percent_{};
    int index_pid_{};
    uint64_t revision_{1};
    rv::Control held_{rv::Control::none};
    std::optional<uint32_t> behind_since_{};
    std::vector<int> player_numbers_{};
    std::vector<oa::ui::hud::ResourcePanelSeat> panel_seats_{};
    std::vector<int> panel_rows_{};
    std::vector<int> remembered_colours_{};
    std::vector<int> player_colour_used_{};
    std::vector<BuiltIndex::MetaColour> meta_colours_{};
    std::vector<bool> pause_open_{};
    std::string handed_timeline_{};
    std::string progress_text_{};
    const kit::Fonts* widths_fonts_{};
    std::string widths_key_{};
    rv::TextWidths widths_{};
    fs::path watched_path_{};
    fs::path index_output_{};
    fs::path index_errors_{};
    SDL_Process* index_process_{};
    SDL_IOStream* index_stdout_{};
    oa::base::threads::Thread index_thread_{};
    oa::base::threads::Mutex index_mutex_{};
    BuiltIndex built_{};
};

void Runtime::remember_replay_file(bool staged, const std::filesystem::path& recorded) {
    if (!staged || replay_recording_path_.empty())
        replay_recording_path_ = recorded;
    // Watch already handed the row's timeline. A file named on the command
    // line looks its row up in the Library's index the same way.
    if (!replay_handed_timeline_.empty())
        return;
    const std::string relative = recording_below(user_folder_, replay_recording_path_);
    if (relative.empty())
        return;
    replay_handed_timeline_ = library_host().timeline_of(*this, relative);
}

void Runtime::ensure_replay_viewer() {
    if ((current_extension_state() & extension_state::replay) == 0)
        return;
    if (oa_layer().find("replay-viewer") != nullptr)
        return;
    oa_layer().push(std::make_unique<ReplayViewerScreen>(*this));
}

bool Runtime::replay_viewer_command(std::string_view id) {
    if ((current_extension_state() & extension_state::replay) == 0)
        return false;
    ensure_replay_viewer();
    if (replay_viewer_ == nullptr)
        return false;
    replay_viewer_->command(id);
    return true;
}

bool Runtime::replay_photo_seek(uint32_t tick) {
    if ((current_extension_state() & extension_state::replay) == 0)
        return false;
    ensure_replay_viewer();
    if (replay_viewer_ == nullptr)
        return false;
    replay_viewer_->seek_for_photo(tick);
    return true;
}

bool Runtime::replay_photo_seek_by(int32_t delta) {
    if ((current_extension_state() & extension_state::replay) == 0)
        return false;
    ensure_replay_viewer();
    if (replay_viewer_ == nullptr)
        return false;
    replay_viewer_->seek_by_for_photo(delta);
    return true;
}

bool Runtime::replay_photo_seek_pending() const {
    return replay_viewer_ != nullptr && replay_viewer_->photo_seek_pending();
}

void Runtime::show_replay_bar() {
    if ((current_extension_state() & extension_state::replay) == 0)
        return;
    ensure_replay_viewer();
    if (replay_viewer_ != nullptr)
        replay_viewer_->show_bar();
}

bool Runtime::replay_hides_recorded_chat() const {
    return replay_chat_hidden_ && (current_extension_state() & extension_state::replay) != 0;
}

bool Runtime::replay_viewer_pad(oa::ui::pad_controls::PadButton button) {
    if ((current_extension_state() & extension_state::replay) == 0)
        return false;
    using oa::ui::pad_controls::PadButton;
    switch (button) {
    case PadButton::a:
        return replay_viewer_command("watch.replay-pause");
    case PadButton::l1:
        return replay_viewer_command("watch.replay-back");
    case PadButton::r1:
        return replay_viewer_command("watch.replay-forward");
    case PadButton::l2:
        call_hook_or_report<&Extension::replay_step_speed>(
            extension_, hook_error_report(), *this, -1
        );
        show_replay_bar();
        return true;
    case PadButton::r2:
        call_hook_or_report<&Extension::replay_step_speed>(
            extension_, hook_error_report(), *this, 1
        );
        show_replay_bar();
        return true;
    case PadButton::dpad_left:
        return replay_viewer_command("watch.replay-previous-marker");
    case PadButton::dpad_right:
        return replay_viewer_command("watch.replay-next-marker");
    case PadButton::b:
        return replay_viewer_command("watch.replay-bar");
    case PadButton::y:
        return replay_viewer_command("watch.next-player-view");
    case PadButton::x:
        return replay_viewer_command("watch.camera-mode");
    default:
        return false;
    }
}

void seat_replay_watcher(Runtime& runtime, uint8_t slot) {
    runtime.match_local_player_ = slot;
}

void note_replay_watch(
    Runtime& runtime, const std::array<ReplayShownSlot, OA_PLAYER_COUNT>& slots, bool shares_camera
) {
    runtime.replay_shown_ = slots;
    runtime.replay_shares_camera_ = shares_camera;
}

void Runtime::replay_step_player_view(int delta) {
    if (replay_viewer_ != nullptr)
        replay_viewer_->step_player_view(delta);
}

void Runtime::replay_show_whole_view() {
    if (replay_viewer_ != nullptr)
        replay_viewer_->show_whole_view();
}

void Runtime::replay_step_camera_mode() {
    if (replay_viewer_ != nullptr)
        replay_viewer_->step_camera_mode();
}

void Runtime::replay_follow_unit(uint16_t unit) {
    if ((current_extension_state() & extension_state::replay) == 0)
        return;
    ensure_replay_viewer();
    if (replay_viewer_ != nullptr)
        replay_viewer_->follow_unit(unit);
}

void Runtime::replay_release_unit_follow() {
    if (replay_viewer_ != nullptr)
        replay_viewer_->release_unit_follow();
}

int32_t Runtime::replay_bar_top() const {
    const int32_t bottom = match_layout_.battlefield_y() + match_layout_.battlefield_height();
    if (replay_viewer_ == nullptr)
        return bottom;
    return replay_viewer_->bar_top(match_layout_);
}

void Runtime::replay_reset_watch() {
    if (replay_viewer_ != nullptr)
        replay_viewer_->reset_watch();
}

std::optional<uint32_t> Runtime::replay_marker_after(uint32_t tick) const {
    if (replay_viewer_ == nullptr)
        return std::nullopt;
    return replay_viewer_->marker_after(tick);
}

std::optional<uint32_t> Runtime::replay_marker_before(uint32_t tick) const {
    if (replay_viewer_ == nullptr)
        return std::nullopt;
    return replay_viewer_->marker_before(tick);
}

bool Runtime::replay_index_arrived() const {
    return replay_viewer_ != nullptr && replay_viewer_->index_arrived();
}

int Runtime::replay_index_pid() const {
    return replay_viewer_ == nullptr ? 0 : replay_viewer_->index_pid();
}

int Runtime::replay_finding_percent() const {
    return replay_viewer_ == nullptr ? 0 : replay_viewer_->finding_percent();
}

int Runtime::replay_named_colour(std::string_view name) const {
    return replay_viewer_ == nullptr ? -1 : replay_viewer_->named_colour(name);
}

std::vector<oa::ui::hud::ResourcePanelSeat> Runtime::replay_panel_seats() const {
    if (replay_viewer_ == nullptr)
        return {};
    return replay_viewer_->panel_seats();
}

std::optional<std::array<uint8_t, 3>> Runtime::replay_panel_row_view_colour(int row) const {
    if (replay_viewer_ == nullptr)
        return std::nullopt;
    const std::optional<kit::Colour> colour = replay_viewer_->panel_row_view_colour(row);
    if (!colour)
        return std::nullopt;
    return std::array<uint8_t, 3>{colour->r, colour->g, colour->b};
}

void Runtime::replay_choose_panel_row(int row) {
    if (replay_viewer_ != nullptr)
        replay_viewer_->choose_panel_row(row);
}

void Runtime::release_replay_recording() {
    if (replay_viewer_ != nullptr)
        replay_viewer_->release_watched_recording();
}

} // namespace oa::app

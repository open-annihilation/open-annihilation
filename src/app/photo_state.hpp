// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Photo mode's state (Runtime::PhotoState): what it replaced and restores
// as it leaves, and the player's own view the under-attack notice keeps
// testing while the photo camera moves. runtime_photo_mode.cpp enters and
// leaves the mode. Leaving writes the player's follow back; nothing else
// here is written into the match.
#pragma once

#include "oa/app/runtime.hpp"
#include "oa/platform/memory_status.hpp"
#include "oa/sim/messages.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {

/// The match commands that still answer while photo mode is open.
///
/// The camera's pan is a held key, and the wheel zooms on its own. Home
/// centres and is listed here; it selects nothing while photo mode is open.
/// The dispatch takes every other match command and does nothing with it,
/// and the keys panel lists only these beside the photo and global keys.
///
/// @param id the action id
/// @return true when photo mode runs the command
[[nodiscard]] bool photo_match_command(std::string_view id) noexcept;

struct Runtime::PhotoState {
    /// What photo mode replaced, put back as it leaves.
    oa::ui::display_layout::MatchLayout layout{};
    int32_t camera_x{};
    int32_t camera_z{};
    float zoom{};
    float zoom_target{};
    ExactView exact{};
    ViewHold hold{};
    bool tracking{};
    uint16_t tracked_unit{};
    uint32_t follow_unit{}; ///< Game.follow_unit on entering
    bool live_kept{};       ///< the live renderer existed, so its turn is put back
    float live_yaw{};
    float live_pitch{};
    bool live_unlocked{};
    bool keys_overlay{};     ///< the keys overlay was open
    bool show_selection{};   ///< selection boxes over the bare battlefield
    bool show_health_bars{}; ///< health bars over the bare battlefield

    /// The player's camera and battlefield rectangle at entry. The on-screen
    /// test collects against these. The picture uses the photo camera, and
    /// the Game block keeps this one.
    uint32_t kept_camera_x{};
    uint32_t kept_camera_y{};
    oa::Rect32 kept_rect{};
    std::vector<uint16_t> kept_units{};
    uint32_t kept_count{};

    /// The message log's next write slot at entry, and the line that stood
    /// there, so a full ring can be told from a slot nothing has replaced.
    uint16_t chat_head{};
    oa::sim::messages::MessageLine chat_at_head{};

    /// How photo mode is playing a match on this machine. A shared match
    /// stays running: its commands are refused, and a held picture is separate.
    enum class Time : uint8_t {
        running, ///< the match's own clock
        frozen,  ///< the clock is held, as the game menu holds a local match
        slow,    ///< the clock is held and ticks are granted at a slow rate
    };
    Time time{Time::running};
    int32_t slow_speed{};        ///< 1..9 while slow; 0 otherwise
    uint64_t slow_interval_ns{}; ///< one granted tick, on frame_time_ns_
    uint64_t slow_origin_ns{};   ///< frame_time_ns_ when the slow pace started
    uint64_t slow_granted{};     ///< ticks granted since slow_origin_ns
    bool shutter{};              ///< an eighth between ticks replaces the fraction
    int32_t shutter_eighth{};    ///< 0 the tick before, 8 the frozen tick
    bool drawn_since_enter{};    ///< a photo-mode frame has been drawn this visit
    bool capture_waiting{};      ///< a photo was asked for before that frame
    bool dragging{};             ///< a photo pan drag is down
    bool drag_moved{};           ///< that drag has passed the click slop
    float drag_x{};              ///< where that drag last was
    float drag_y{};
    bool picture_held{};             ///< a shared match shows the copied battlefield
    uint32_t held_width{};           ///< the copy's width
    uint32_t held_height{};          ///< the copy's height
    std::vector<uint8_t> held_rgb{}; ///< the copy, row by row, three bytes a pixel
    /// The fog of war the copy was drawn with: 1 for each sight cell it
    /// showed never mapped (Runtime::photo_fog_cells). The haze keeps that
    /// black, whatever the match explores while the picture is held.
    std::vector<uint8_t> held_unmapped{};
    int32_t held_fog_width{};  ///< sight cells across; 0 when the copy showed no such fog
    int32_t held_fog_height{}; ///< sight cells down
    /// The camera when the picture was held. Letting go writes it back.
    /// Leaving photo mode still writes the camera from entering.
    bool camera_held{};
    int32_t held_camera_x{};
    /// The match's tick and speed when the picture was held. A photo of that
    /// picture records this moment.
    uint32_t held_tick{};
    uint16_t held_speed{};
    int32_t held_camera_z{};
    float held_zoom{};
    float held_zoom_target{};
    ExactView held_exact{};
    ViewHold held_view{};
    bool held_tracking{};
    uint16_t held_tracked_unit{};
    uint32_t held_follow_unit{};
    bool held_live{}; ///< the live renderer existed when the picture was held
    float held_live_yaw{};
    float held_live_pitch{};
    bool held_live_unlocked{};
    /// A replay's viewing speed on entering, in thousandths of 1×, for Run
    /// to put back. 0 is read as 1×.
    uint16_t entry_speed{};
    bool replay_visit{};           ///< this visit is a replay
    bool replay_paused_seen{};     ///< the playback was paused when it was last read
    int32_t replay_slow_seen{};    ///< 1 while a replay plays below 1×
    uint32_t replay_tick_seen{};   ///< the playback's tick when it was last read
    uint32_t replay_length_seen{}; ///< the recording's length when it was last read
    /// The framed camera, put back while a photo seek is in flight and on
    /// the frame it lands. A pan writes this again. It is dropped after that frame.
    bool guard_camera{};
    bool guard_awaiting{}; ///< a photo seek was asked and has not landed yet
    int32_t guard_camera_x{};
    int32_t guard_camera_z{};
    float guard_zoom{};
    float guard_zoom_target{};
    ExactView guard_exact{};
    ViewHold guard_hold{};
    bool guard_live{};
    float guard_live_yaw{};
    float guard_live_pitch{};
    bool guard_live_unlocked{};
    /// The recording's digest for this visit, so a second photo does not read the file again.
    std::string replay_digest_hex{};
    std::string replay_digest_path{};

    /// The panel beside the battlefield. Hiding it leaves the hint.
    bool panel_shown{true};
    bool section_camera{true};
    bool section_time{true};
    bool section_look{true};
    bool section_capture{true};
    /// The size chosen for the next photo: 1 is Screen, then 2, 3 and 4
    /// times the battlefield. Kept for this visit. Not a preference.
    int size_factor{1};
    /// Smooth edges for the next photo: 1 is off, then 2 and 4. The draw is
    /// this many times the picture, then box-reduced. Kept for this visit.
    int smooth_factor{1};
    /// The next photo is a poster: the view, a marked area, or the whole
    /// map, at one map pixel a picture pixel. The size factor is kept, and
    /// comes back when the poster is turned off. Not a preference.
    bool poster{false};
    /// Which area, as photo::PosterExtent: 0 the view, 1 marked, 2 the map.
    uint8_t poster_extent{0};
    /// The marked area, in map pixels. Empty is read as the view.
    int32_t poster_mark_x{0};
    int32_t poster_mark_z{0};
    int32_t poster_mark_w{0};
    int32_t poster_mark_h{0};
    /// A drag is marking that area, rather than panning the view.
    bool poster_marking{false};
    /// The pointer has moved past a click, so the drag replaces the mark.
    bool poster_mark_moved{false};
    float poster_press_x{0};
    float poster_press_y{0};
    /// The mark a click, with no drag, puts back.
    int32_t poster_saved_x{0};
    int32_t poster_saved_z{0};
    int32_t poster_saved_w{0};
    int32_t poster_saved_h{0};
    int32_t poster_anchor_x{0};
    int32_t poster_anchor_z{0};
    /// The poster just drawn, so its file records that origin at zoom 1.
    bool poster_recorded{false};
    int32_t poster_recorded_x{0};
    int32_t poster_recorded_z{0};
    /// The last sized photo was drawn by Full on the window's renderer.
    bool sized_by_full{false};
    /// The last sized photo was drawn by the turned view.
    bool sized_by_live{false};
    /// The anti-aliasing factor a Full photo was drawn at. 1 otherwise.
    uint32_t sized_supersample{1};
    /// The system's memory as the size plan last read it, and the steady
    /// clock then. The panel builds a plan on every tick and draw; the plan
    /// reads the memory again once a second, as the memory guard does.
    oa::platform::SystemMemorySample memory_read{};
    bool memory_read_ok{};     ///< that reading found the memory
    uint64_t memory_read_ns{}; ///< 0 until the first reading
    /// The guide over the battlefield, as photo_panel::Guide. Off is 0.
    uint8_t guide{};
    bool toast_on{}; ///< a photo's notice is showing
    /// The frame clock (frame_time_ns_) at which the notice hides. 0 until
    /// the first frame that shows it sets it. A press on the panel hides it
    /// sooner.
    uint64_t toast_until_ns{};
    bool toast_copied{};              ///< that photo was copied
    std::string toast_name{};         ///< the file name, or why it was not written
    std::vector<uint8_t> toast_rgb{}; ///< the thumbnail, packed RGB
    uint32_t toast_width{};
    uint32_t toast_height{};

    /// The look for this visit. None, a vignette of 0, a grain of 0 and no
    /// frame leave the picture as it was. Not a preference: leaving drops it.
    uint8_t look_preset{};      ///< photo::Preset, 0 is none
    int32_t look_vignette{};    ///< 0 to 100
    int32_t look_roundness{50}; ///< 0 is a box, 100 is a circle
    int32_t look_grain{};       ///< 0 to 100
    int32_t look_grain_size{2}; ///< pixels of one grain, at least 1
    uint8_t look_frame{};       ///< photo::Frame, 0 is none
    /// The depth page of Look. The grade shows while this is false.
    bool look_depth{false};
    /// Centre of the sharp band, 0 to 100 percent of the picture's height.
    int32_t look_tilt_centre{50};
    /// Width of the sharp band, in the battlefield's pixels.
    int32_t look_tilt_width{48};
    /// Blur at the top and the bottom, in the battlefield's pixels. 0 is off.
    int32_t look_tilt_radius{0};
    /// How far the picture fades, 0 to 100. 0 is off. The classic view fades
    /// lower ground. The turned view fades by distance from the camera, on
    /// the card, and only while the card keeps that depth.
    int32_t look_haze{0};
    /// Blur away from the focus, in the battlefield's pixels. 0 is off.
    /// The turned view draws it on the card. The classic view ignores it.
    int32_t look_depth_of_field{0};
    /// The blur the card drew on the last frame, in that picture's pixels.
    /// 0 when the depth pass did not run.
    int32_t card_dof_pixels{0};
    /// The haze the card drew on the last frame, 0 to 100. 0 when it did not.
    int32_t card_haze{0};

    /// Tells whether a unit is in the view photo mode kept: listed by the
    /// last collect against the player's camera, not the photo camera.
    ///
    /// @param context the PhotoState
    /// @param unit unit slot
    /// @return true when the unit is in the kept list
    [[nodiscard]] static bool unit_kept(const void* context, uint16_t unit) noexcept {
        const auto* state = static_cast<const PhotoState*>(context);
        if (state == nullptr)
            return false;
        const auto count = state->kept_count < state->kept_units.size()
                               ? state->kept_count
                               : static_cast<uint32_t>(state->kept_units.size());
        for (uint32_t index = 0; index < count; ++index)
            if (state->kept_units[index] == unit)
                return true;
        return false;
    }
};

} // namespace oa::app

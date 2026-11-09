// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The photo panel's model: the state a layout and a drawing read. It names
// no file, clock, preference or match.
#pragma once

#include "oa/ui/kit/layout.hpp"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace oa::ui::photo_panel {

/// Which guides lie over the battlefield. Crops from crop_16_9 on also crop
/// the photo, centred, when it is taken. off draws nothing.
enum class Guide : uint8_t {
    off = 0,
    thirds,
    centre,
    safe,
    crop_16_9,
    crop_3_2,
    crop_4_3,
    crop_1_1,
    crop_4_5,
    crop_9_16,
    crop_21_9,
    count,
};

/// Who the clock belongs to, which decides the chips and which controls lock.
enum class Visit : uint8_t {
    skirmish, ///< a skirmish or a campaign mission on this machine
    replay,   ///< a replay
    shared,   ///< a shared match, or a watcher
};

/// One control's number. Tab follows the order they are laid out.
inline constexpr kit::ControlId kHead = 1;
inline constexpr kit::ControlId kCamera = 2;
inline constexpr kit::ControlId kZoom = 3;
inline constexpr kit::ControlId kTurn = 4;
inline constexpr kit::ControlId kTilt = 5;
inline constexpr kit::ControlId kClassic = 6;
inline constexpr kit::ControlId kFollow = 7;
inline constexpr kit::ControlId kTime = 8;
inline constexpr kit::ControlId kRun = 9;
inline constexpr kit::ControlId kSlow = 10;
inline constexpr kit::ControlId kFreeze = 11;
inline constexpr kit::ControlId kSpeed = 12;
inline constexpr kit::ControlId kStep = 13;
inline constexpr kit::ControlId kBetween = 14;
inline constexpr kit::ControlId kClock = 15;
inline constexpr kit::ControlId kLook = 16;
inline constexpr kit::ControlId kSelection = 17;
inline constexpr kit::ControlId kHealth = 18;
inline constexpr kit::ControlId kGuides = 19;
inline constexpr kit::ControlId kCapture = 20;
inline constexpr kit::ControlId kSize = 21;
inline constexpr kit::ControlId kCopySwitch = 22;
inline constexpr kit::ControlId kNames = 23;
inline constexpr kit::ControlId kFile = 24;
inline constexpr kit::ControlId kTake = 25;
inline constexpr kit::ControlId kCopy = 26;
inline constexpr kit::ControlId kHint = 27;
/// A reason under a control, or the toast. None of these take Tab.
inline constexpr kit::ControlId kZoomNote = 28;
inline constexpr kit::ControlId kTurnReason = 29;
inline constexpr kit::ControlId kTiltReason = 30;
inline constexpr kit::ControlId kTimeReason = 31;
inline constexpr kit::ControlId kSpeedReason = 32;
inline constexpr kit::ControlId kStepReason = 33;
inline constexpr kit::ControlId kBetweenReason = 34;
inline constexpr kit::ControlId kSaved = 35;

/// A folding section's number, carried on a press of its header.
inline constexpr int kSectionCamera = 1;
inline constexpr int kSectionTime = 2;
inline constexpr int kSectionLook = 3;
inline constexpr int kSectionCapture = 4;

/// What a press on the panel asks the host to do.
enum class Command : uint8_t {
    none,             ///< the press changed nothing the match owns
    leave,            ///< leave photo mode
    show_panel,       ///< the hint was pressed
    toggle_section,   ///< fold or open the section in value
    set_zoom,         ///< zoom stop in value
    classic_view,     ///< the classic view
    follow_unit,      ///< follow a unit for this visit
    set_turn,         ///< turn stop in value
    set_tilt,         ///< tilt stop in value
    run,              ///< run at the match's speed
    slow,             ///< slow at the speed stop
    freeze,           ///< freeze, or hold the picture
    set_speed,        ///< speed stop in value
    step,             ///< one tick
    between,          ///< shutter eighth in value
    toggle_selection, ///< selection boxes
    toggle_health,    ///< health bars
    cycle_guides,     ///< the next guide
    toggle_copy,      ///< copy to the clipboard
    toggle_names,     ///< player names in the photo
    take_photo,       ///< write a photo
    copy_photo,       ///< copy the last photo
};

/// A press the panel reports.
struct Press {
    Command command{Command::none}; ///< what it asks
    int32_t value{0};               ///< a stop, a section, or an eighth
};

/// The panel as the player has it. Strings are already looked up.
struct Model {
    int32_t width{1280};                                ///< the window, in points
    int32_t height{720};                                ///< the window, in points
    kit::SizeClass size_class{kit::SizeClass::regular}; ///< the class to lay out at
    Visit visit{Visit::skirmish};                       ///< which chips and locks
    bool shown{true};                                   ///< the panel itself; false leaves the hint
    bool camera_open{true};
    bool time_open{true};
    bool look_open{true};
    bool capture_open{true};
    Guide guide{Guide::off}; ///< drawn over the battlefield, never in a photo
    bool selection{true};    ///< Show selection
    bool health{true};       ///< Show health bars
    bool copy_on{false};     ///< Copy to clipboard
    bool names_on{true};     ///< Player names
    /// The control under the pointer, or none.
    kit::ControlId hot{kit::no_control};

    int32_t zoom_stop{4};
    int32_t zoom_stops{9};
    std::string zoom_value{}; ///< the value and the scale's name, without a lock
    std::string zoom_lock{};  ///< empty when the slider moves
    /// The tier's reason, shown under the slider at the far limit. It does
    /// not lock the slider: the player can still zoom in.
    std::string zoom_note{};
    /// Values the zoom can show, one per scale name. The value's column is as
    /// wide as the widest of them, so the track does not change its width as
    /// the zoom moves. Empty measures the value alone.
    std::vector<std::string> zoom_reserve{};

    int32_t turn_stop{18};
    int32_t turn_stops{37};
    std::string turn_value{};
    std::string turn_lock{};

    int32_t tilt_stop{18};
    int32_t tilt_stops{37};
    std::string tilt_value{};
    std::string tilt_lock{};

    int32_t speed_stop{2}; ///< 0 is Slow 1
    int32_t speed_stops{9};
    std::string speed_value{};
    std::string speed_lock{};
    std::string slow_lock{}; ///< on the Slow button; empty when it runs
    std::string step_lock{};
    std::string between_lock{};
    int32_t between_stop{8};
    int32_t between_stops{9};
    std::string between_value{};

    std::string head{};           ///< "{key} or Esc leaves"
    std::string freeze_caption{}; ///< Freeze, or Hold picture
    /// Which of Run, Slow and Freeze is on: 0, 1 or 2. The strip lights it.
    int32_t time_mode{0};
    std::string clock{};
    std::string file_name{};
    std::string size_value{"Screen"}; ///< Screen, in this ticket
    std::string guide_value{"Off"};   ///< the guide's name, or Off
    std::string key_line{};           ///< the chords along the bottom
    /// Captions, already looked up. The defaults are the English keys.
    std::string camera_title{"Camera"};
    std::string time_title{"Time"};
    std::string look_title{"Look"};
    std::string capture_title{"Photo"};
    std::string zoom_label{"Zoom"};
    std::string turn_label{"Turn"};
    std::string tilt_label{"Tilt"};
    std::string classic_label{"Classic view"};
    std::string follow_label{"Follow unit"};
    std::string run_label{"Run"};
    std::string slow_label{"Slow"};
    std::string speed_label{"Speed"};
    std::string step_label{"Step"};
    std::string between_label{"Between ticks"};
    std::string clock_label{"Clock"};
    std::string selection_label{"Show selection"};
    std::string health_label{"Show health bars"};
    std::string guides_label{"Guides"};
    std::string size_label{"Size"};
    std::string copy_switch_label{"Copy to clipboard"};
    std::string names_label{"Player names"};
    std::string file_label{"File"};
    std::string take_label{"Take photo"};
    std::string copy_label{"Copy"};
    std::string hint_label{"{key} shows the panel"};
    std::string open_word{"Expanded"};
    std::string closed_word{"Collapsed"};
    std::string off_word{"Off"};
    std::string on_word{"On"};

    std::array<std::string, 3> chips{}; ///< up to three chips, empty ones skipped
    bool live_amber{false};             ///< the first chip is the live amber
    std::string alerts{};               ///< under the chips, in a shared match

    bool toast{false};
    std::string toast_name{};         ///< the file name
    std::string toast_note{};         ///< Copied, or empty
    std::vector<uint8_t> toast_rgb{}; ///< box-reduced RGB, or empty
    int32_t toast_columns{0};
    int32_t toast_rows{0};
};

} // namespace oa::ui::photo_panel

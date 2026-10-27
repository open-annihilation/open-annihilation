// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The words the preferences keep the settings' choices as, which the
// settings read and write (engine_settings.cpp) and by which the dialog's
// rows name a choice's items and a strip's levels to automation
// (settings_rows.cpp).
#pragma once

#include "oa/ui/engine_settings.hpp"
#include "oa/ui/pad_controls.hpp"

#include <array>
#include <cstddef>
#include <string_view>

namespace oa::ui::engine_settings {

/// A choice of a setting and the word the preferences keep it as.
template <typename Choice>
struct ChoiceWord {
    Choice choice{};       ///< the choice
    std::string_view word; ///< its word, in lower case
};

/// Explosion flash's words, in explosion_flash_choices' order.
inline constexpr std::array<ChoiceWord<ExplosionFlash>, 3> explosion_flash_words{{
    {ExplosionFlash::off, "off"},
    {ExplosionFlash::reduced, "reduced"},
    {ExplosionFlash::full, "full"},
}};
/// Fog of War's words, in fog_of_war_choices' order.
inline constexpr std::array<ChoiceWord<FogOfWar>, 2> fog_of_war_words{{
    {FogOfWar::classic, "classic"},
    {FogOfWar::smooth, "smooth"},
}};
/// Zoomed out units' words, for the ways a player may choose: Icons, which
/// the dialog shows but does not offer yet, has none.
inline constexpr std::array<ChoiceWord<ZoomedOutUnits>, 2> zoomed_out_units_words{{
    {ZoomedOutUnits::rendered, "rendered"},
    {ZoomedOutUnits::dots, "dots"},
}};
static_assert(zoomed_out_units_words.size() == offered_zoomed_out_units);
/// After zoom's words, in zoomed_out_afters' order.
inline constexpr std::array<ChoiceWord<ZoomedOutAfter>, 7> zoomed_out_after_words{{
    {ZoomedOutAfter::one_half, "1/2"},
    {ZoomedOutAfter::one_third, "1/3"},
    {ZoomedOutAfter::one_quarter, "1/4"},
    {ZoomedOutAfter::one_sixth, "1/6"},
    {ZoomedOutAfter::one_eighth, "1/8"},
    {ZoomedOutAfter::one_twelfth, "1/12"},
    {ZoomedOutAfter::one_sixteenth, "1/16"},
}};
/// Window edge's words, in window_edge_choices' order.
inline constexpr std::array<ChoiceWord<WindowEdge>, 3> window_edge_words{{
    {WindowEdge::capture, "capture"},
    {WindowEdge::sticky, "sticky"},
    {WindowEdge::free, "free"},
}};
/// Window frame's words, in window_frame_choices' order.
inline constexpr std::array<ChoiceWord<WindowFrame>, 2> window_frame_words{{
    {WindowFrame::hidden_in_play, "hidden-in-play"},
    {WindowFrame::always_shown, "always-shown"},
}};
/// Window size's words, in window_size_choices' order: the words the OA
/// layer reads the same key with.
inline constexpr std::array<ChoiceWord<WindowSize>, 4> window_size_words{{
    {WindowSize::automatic, "auto"},
    {WindowSize::compact, "compact"},
    {WindowSize::regular, "regular"},
    {WindowSize::large, "large"},
}};
/// Maximum zoom out's words, in zoom_out_limits' order.
inline constexpr std::array<ChoiceWord<ZoomOutLimit>, 7> zoom_out_words{{
    {ZoomOutLimit::automatic, "automatic"},
    {ZoomOutLimit::whole_map, "whole-map"},
    {ZoomOutLimit::one_thirty_second, "1/32"},
    {ZoomOutLimit::one_sixteenth, "1/16"},
    {ZoomOutLimit::one_eighth, "1/8"},
    {ZoomOutLimit::one_quarter, "1/4"},
    {ZoomOutLimit::one_half, "1/2"},
}};
/// Maximum zoom in's words, in zoom_in_limits' order.
inline constexpr std::array<ChoiceWord<ZoomInLimit>, 4> zoom_in_words{{
    {ZoomInLimit::none, "1"},
    {ZoomInLimit::twice, "2"},
    {ZoomInLimit::three_times, "3"},
    {ZoomInLimit::four_times, "4"},
}};
/// View past the map's edge's words, in view_past_map_edge_choices' order.
inline constexpr std::array<ChoiceWord<ViewPastMapEdge>, 3> view_past_map_edge_words{{
    {ViewPastMapEdge::off, "off"},
    {ViewPastMapEdge::one_quarter, "25"},
    {ViewPastMapEdge::one_half, "50"},
}};
/// Control size's words, in control_size_choices' order.
inline constexpr std::array<ChoiceWord<ControlSize>, 3> control_size_words{{
    {ControlSize::standard, "standard"},
    {ControlSize::large, "large"},
    {ControlSize::larger, "larger"},
}};
/// Scheme's words.
inline constexpr std::array<ChoiceWord<pad_controls::Scheme>, 2> scheme_words{{
    {pad_controls::Scheme::trackpads, "trackpads"},
    {pad_controls::Scheme::sticks, "sticks"},
}};
/// Right trackpad's words.
inline constexpr std::array<ChoiceWord<pad_controls::RightTrackpad>, 2> right_trackpad_words{{
    {pad_controls::RightTrackpad::relative, "relative"},
    {pad_controls::RightTrackpad::absolute, "absolute"},
}};
/// Pointer acceleration's words.
inline constexpr std::array<ChoiceWord<pad_controls::Acceleration>, 3> acceleration_words{{
    {pad_controls::Acceleration::off, "off"},
    {pad_controls::Acceleration::low, "low"},
    {pad_controls::Acceleration::high, "high"},
}};
/// Right stick's words.
inline constexpr std::array<ChoiceWord<pad_controls::RightStick>, 3> right_stick_words{{
    {pad_controls::RightStick::zoom_and_pages, "zoom"},
    {pad_controls::RightStick::pointer, "pointer"},
    {pad_controls::RightStick::nothing, "nothing"},
}};
/// Gyro pointer's words.
inline constexpr std::array<ChoiceWord<pad_controls::Gyro>, 4> gyro_words{{
    {pad_controls::Gyro::off, "off"},
    {pad_controls::Gyro::right_pad_touched, "right-pad"},
    {pad_controls::Gyro::right_stick_touched, "right-stick"},
    {pad_controls::Gyro::always, "always"},
}};
/// Haptics' words.
inline constexpr std::array<ChoiceWord<pad_controls::Haptics>, 3> haptics_words{{
    {pad_controls::Haptics::off, "off"},
    {pad_controls::Haptics::light, "light"},
    {pad_controls::Haptics::strong, "strong"},
}};
/// Button prompts' words.
inline constexpr std::array<ChoiceWord<pad_controls::Prompts>, 6> prompts_words{{
    {pad_controls::Prompts::automatic, "automatic"},
    {pad_controls::Prompts::steam_deck, "steam-deck"},
    {pad_controls::Prompts::xbox, "xbox"},
    {pad_controls::Prompts::playstation, "playstation"},
    {pad_controls::Prompts::nintendo, "nintendo"},
    {pad_controls::Prompts::off, "off"},
}};

/// Order wheel's words.
inline constexpr std::array<ChoiceWord<OrderWheel>, 3> order_wheel_words{{
    {OrderWheel::off, "off"},
    {OrderWheel::hold_right, "hold-right"},
    {OrderWheel::hold_middle, "hold-middle"},
}};

/// Returns the word the preferences keep a choice as.
///
/// @param words the setting's choices and words
/// @param choice the choice
/// @return its word; empty for a choice the table does not hold
template <typename Choice, std::size_t Count>
constexpr std::string_view
word_of(const std::array<ChoiceWord<Choice>, Count>& words, Choice choice) noexcept {
    for (const ChoiceWord<Choice>& entry : words)
        if (entry.choice == choice)
            return entry.word;
    return {};
}

} // namespace oa::ui::engine_settings

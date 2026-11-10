// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// English texts the photo panel passes to the interface catalogue.
#pragma once

#include <string_view>

namespace oa::ui::photo_panel::words {

inline constexpr std::string_view leave_head = "{key} or Esc leaves";
inline constexpr std::string_view shows_panel = "{key} shows the panel";
inline constexpr std::string_view show_panel = "Show panel";
inline constexpr std::string_view photo_not_taken = "Photo not taken";
inline constexpr std::string_view show_selection = "Show selection";
inline constexpr std::string_view show_health = "Show health bars";
inline constexpr std::string_view player_names = "Player names";
inline constexpr std::string_view shared_game = "Shared game";
inline constexpr std::string_view hold_picture = "Hold picture";
inline constexpr std::string_view thirds = "Thirds";
inline constexpr std::string_view safe_area = "Safe area";
inline constexpr std::string_view run = "Run";
inline constexpr std::string_view freeze = "Freeze";
inline constexpr std::string_view step = "Step";
inline constexpr std::string_view between_ticks = "Between ticks";
inline constexpr std::string_view follow_unit = "Follow unit";
inline constexpr std::string_view turn = "Turn";
inline constexpr std::string_view tilt = "Tilt";
inline constexpr std::string_view speed = "Speed";
inline constexpr std::string_view screen = "Screen";
inline constexpr std::string_view copy_to_clipboard = "Copy to clipboard";
inline constexpr std::string_view far = "Far";
inline constexpr std::string_view closest = "Closest";
inline constexpr std::string_view replay_paused = "Replay paused";
inline constexpr std::string_view live = "Live";
inline constexpr std::string_view time = "Time";
inline constexpr std::string_view hide_panel = "Hide panel";
inline constexpr std::string_view guides = "Guides";
inline constexpr std::string_view leave = "Leave";
inline constexpr std::string_view take_photo = "Take photo";
inline constexpr std::string_view copy = "Copy";
inline constexpr std::string_view camera = "Camera";
inline constexpr std::string_view look = "Look";
inline constexpr std::string_view photo = "Photo";
inline constexpr std::string_view classic_view = "Classic view";
inline constexpr std::string_view zoom = "Zoom";
inline constexpr std::string_view slow = "Slow";
inline constexpr std::string_view clock = "Clock";
inline constexpr std::string_view size = "Size";
inline constexpr std::string_view file = "File";
inline constexpr std::string_view expanded = "Expanded";
inline constexpr std::string_view collapsed = "Collapsed";
inline constexpr std::string_view off = "Off";
inline constexpr std::string_view on = "On";
inline constexpr std::string_view photo_mode = "Photo mode";
inline constexpr std::string_view frozen = "Frozen";
inline constexpr std::string_view whole_map = "Whole map";
inline constexpr std::string_view medium = "Medium";
inline constexpr std::string_view near = "Near";
inline constexpr std::string_view centre_lines = "Centre lines";
inline constexpr std::string_view copied = "Copied";
inline constexpr std::string_view smooth_edges = "Smooth edges";
inline constexpr std::string_view two = "2×";
inline constexpr std::string_view three = "3×";
inline constexpr std::string_view four = "4×";
inline constexpr std::string_view and_word = "and";
inline constexpr std::string_view card_edge =
    "This graphics card draws at most {limit} pixels a side";
inline constexpr std::string_view photo_edge = "A photo is at most {limit} pixels a side";
inline constexpr std::string_view turned_edge =
    "The 3D view draws at most {limit} pixels a side at this anti-aliasing";
inline constexpr std::string_view not_enough_memory = "Not enough memory";
inline constexpr std::string_view whole_pixels = "Off and Basic draw whole pixels";
inline constexpr std::string_view held_picture = "Held picture";
inline constexpr std::string_view year_1997 = "1997";
inline constexpr std::string_view arctic = "Arctic";
inline constexpr std::string_view bleach = "Bleach";
inline constexpr std::string_view caption_bar = "Caption bar";
inline constexpr std::string_view dusk = "Dusk";
inline constexpr std::string_view frame = "Frame";
inline constexpr std::string_view grain = "Grain";
inline constexpr std::string_view grain_size = "Grain size";
inline constexpr std::string_view noir = "Noir";
inline constexpr std::string_view roundness = "Roundness";
inline constexpr std::string_view preview_rate = "The preview lowers the frame rate.";
inline constexpr std::string_view thin_border = "Thin border";
inline constexpr std::string_view vignette = "Vignette";
inline constexpr std::string_view wide_mat = "Wide mat";
inline constexpr std::string_view depth = "Depth";
inline constexpr std::string_view band_centre = "Band centre";
inline constexpr std::string_view band_width = "Band width";
inline constexpr std::string_view blur = "Blur";
inline constexpr std::string_view tilt_note = "Blurs by screen row, not by depth.";
inline constexpr std::string_view haze = "Haze";
inline constexpr std::string_view haze_note = "Follows the ground, not the units.";
inline constexpr std::string_view depth_of_field = "Depth of field";
inline constexpr std::string_view needs_full_3d = "Needs Full 3D Mode";
inline constexpr std::string_view no_depth = "This graphics card keeps no depth.";

} // namespace oa::ui::photo_panel::words

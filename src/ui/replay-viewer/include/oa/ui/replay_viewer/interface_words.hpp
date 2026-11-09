// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// English texts the playback bar passes to the interface catalogue. oa-tool
// joins this list with the other modules' lists. A pattern's places are
// written {name}. A translation keeps them.

#pragma once

#include <string_view>

namespace oa::ui::replay_viewer::words {

/// The line over the timeline while a seek is catching up. `{percent}` is the
/// whole percent still to come.
inline constexpr std::string_view catching_up = "Catching up… {percent} %";
/// The end card's line when the recording failed part way. `{time}` is the
/// game time of the first failure, as "41:10".
inline constexpr std::string_view damaged = "The recording is damaged after {time}";
/// The end card's button that starts the replay again.
inline constexpr std::string_view watch_again = "Watch again";
/// The end card's button that leaves for the Library's replays.
inline constexpr std::string_view back_to_library = "Back to the Library";
/// The end card's title when the recording names no winner.
inline constexpr std::string_view undecided = "Undecided";
/// The end card's title when it names who won. `{name}` is that player.
inline constexpr std::string_view winner = "{name} won";
/// A commander's death on the timeline. `{time}` is the game time and `{name}`
/// the player whose commander died.
inline constexpr std::string_view commander_destroyed = "{time} · {name}'s commander destroyed";
/// A player whose connection dropped. `{time}` and `{name}` as above.
inline constexpr std::string_view dropped = "{time} · {name} dropped";
/// A player who left. `{time}` and `{name}` as above.
inline constexpr std::string_view left = "{time} · {name} left";
/// A player who was defeated. `{time}` and `{name}` as above.
inline constexpr std::string_view defeated = "{time} · {name} was defeated";
/// An alliance that was made. `{time}` and `{name}` as above.
inline constexpr std::string_view allied = "{time} · {name} allied";
/// An alliance that was broken. `{time}` and `{name}` as above.
inline constexpr std::string_view broke = "{time} · {name} broke the alliance";
/// A recorded chat line. `{time}` is the game time, `{name}` who said it and
/// `{line}` what was said.
inline constexpr std::string_view chat_line = "{time} · {name}: {line}";
/// A pause on the timeline. `{time}` is when it began.
inline constexpr std::string_view paused = "{time} · Paused";
/// The end of the recording. `{time}` is the last tick's game time.
inline constexpr std::string_view end_marker = "{time} · End";
/// The speed button while the picture is behind the speed that was asked for.
/// `{asked}` and `{reached}` are speed words such as "2×".
inline constexpr std::string_view speed_behind = "{asked} · {reached} now";
/// Half speed, as the speed button writes it.
inline constexpr std::string_view half_speed = "½×";
/// The end card's column of units destroyed.
inline constexpr std::string_view kills = "Kills";
/// The end card's column of units lost.
inline constexpr std::string_view losses = "Losses";
/// The button that shows or hides recorded chat.
inline constexpr std::string_view chat = "Chat";
/// The button that shows the shortcut card.
inline constexpr std::string_view keys = "Keys";
/// The button that opens the in-game menu.
inline constexpr std::string_view leave = "Leave";
/// The button that opens photo mode.
inline constexpr std::string_view photo = "Photo";
/// The timeline while the index pass is still finding markers. `{percent}` is
/// how far that pass has come.
inline constexpr std::string_view finding = "No markers yet · finding them, {percent} %";
/// Export on a recording that is not in the Library.
inline constexpr std::string_view export_outside = "Only a replay in the Library can be exported";
/// The end card's title when the index ended and nobody won.
inline constexpr std::string_view no_result = "No result";
/// The player list's title.
inline constexpr std::string_view watching = "Watching";
/// The view of the whole map and every unit.
inline constexpr std::string_view all_players = "All players";
/// The camera stays where the player leaves it.
inline constexpr std::string_view free_camera = "Free";
/// The camera follows the recorded player's own camera.
inline constexpr std::string_view follow_their_camera = "Follow their camera";
/// The camera keeps one unit in the middle.
inline constexpr std::string_view follow_a_unit = "Follow a unit";
/// A player who was defeated, on a list row. `{time}` is the game time.
inline constexpr std::string_view defeated_at = "Defeated {time}";
/// A player who left, on a list row. `{time}` is the game time.
inline constexpr std::string_view left_at = "Left {time}";
/// A player's team number. `{team}` is that number.
inline constexpr std::string_view team_label = "Team {team}";
/// Metal and energy produced per settlement. `{metal}` has one decimal and
/// `{energy}` is a whole number. Both are zero or more.
inline constexpr std::string_view income_rate = "+{metal} m / +{energy} e";
/// How many units a player has. `{units}` is that number, and not one.
inline constexpr std::string_view units_count = "{units} units";
/// How many units a player has when that number is one. `{units}` is "1".
inline constexpr std::string_view unit_count = "{units} unit";

} // namespace oa::ui::replay_viewer::words

namespace oa::ui::replay_viewer {

/// English texts this module passes to the interface catalogue
/// (oa/data/languages/interface_text.hpp). oa-tool sorts the modules' lists
/// together and keeps one of each text.
inline constexpr std::string_view interface_words[] = {
    words::catching_up,
    words::damaged,
    words::watch_again,
    words::back_to_library,
    words::undecided,
    words::winner,
    words::commander_destroyed,
    words::dropped,
    words::left,
    words::defeated,
    words::allied,
    words::broke,
    words::chat_line,
    words::paused,
    words::end_marker,
    words::speed_behind,
    words::half_speed,
    words::kills,
    words::losses,
    words::chat,
    words::keys,
    words::leave,
    words::photo,
    words::finding,
    words::export_outside,
    words::no_result,
    words::watching,
    words::all_players,
    words::free_camera,
    words::follow_their_camera,
    words::follow_a_unit,
    words::defeated_at,
    words::left_at,
    words::team_label,
    words::income_rate,
    words::units_count,
    words::unit_count,
};

} // namespace oa::ui::replay_viewer

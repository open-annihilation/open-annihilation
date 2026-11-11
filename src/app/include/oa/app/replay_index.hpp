// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The two parts of the index pass (--replay-index, docs/replays.md) that need
// no running match.
//
// A recording holds more than the replay shows: the chat lines the players
// typed, the pauses they called, the players who left, were dropped or were
// removed as defeated, the alliances they made, and the end-of-game records
// the machines exchanged for the score screen. read_stream_notes walks the
// recording's records once, without playing it, and notes each with the tick
// of the packet that carries it, as playback numbers the packets.
// build_index then combines those notes with the timeline the director's
// recorder made while the recording was replayed (the commander deaths, each
// player's totals, the damage dealt, and where units, buildings and large
// engagements were) into the index.
//
// Everything is a whole number or text taken from the recording, so the same
// recording always gives the same index.
#pragma once

#include "oa/formats/oademo/timeline_index.hpp"
#include "oa/media/director/timeline.hpp"

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace oa::app::replay_index {

/// What a note of the recording's stream is about.
enum class NoteKind : uint8_t {
    chat,         ///< a chat line: `player` said `text`
    pause,        ///< the game was paused (`value` 1) or resumed (`value` 0) by `player`
    rejected,     ///< a reject record named `other` with the reason in `value`
    disconnected, ///< a disconnect notice named `other`
    alliance,     ///< `player` set its alliance with `other` to `value`
    end_of_game,  ///< `player` sent its score for the end screen, asking for the others'
};

/// One record of the recording's stream that the index notes.
struct Note {
    /// The tick of the packet that holds the record, as playback numbers it.
    uint32_t tick{};
    /// What the record is.
    NoteKind kind{NoteKind::chat};
    /// The number of the player the packet is from.
    int player{};
    /// The number of the player the record names; 0 when it names none the recording holds.
    int other{};
    /// The record's value; see NoteKind.
    uint8_t value{};
    /// A chat line, as UTF-8.
    std::string text{};
};

/// A player of the recording.
struct StreamPlayer {
    /// The player's number in the recording.
    int number{};
    /// The player's name, as UTF-8.
    std::string name{};
    /// The player only watches.
    bool watcher{};
};

/// What a recording's own stream holds.
struct StreamNotes {
    /// The players, by player index: the order the replay seats them in.
    std::vector<StreamPlayer> players{};
    /// The notes, in the order the recording holds them.
    std::vector<Note> notes{};
};

/// Reads the notes of a recording.
///
/// A packet whose payload cannot be unpacked, or that holds no game record,
/// or that is from a player the recording does not list, is skipped, as
/// playback skips it. A private chat line is left out.
///
/// @param file a `.tad` or a `.oademo`
/// @param[out] error why the file was not read; untouched on success
/// @return the notes, or nothing when the file is neither a `.oademo` this
///         game reads nor a recording that parses
[[nodiscard]] std::optional<StreamNotes>
read_stream_notes(std::span<const uint8_t> file, std::string& error);

/// What the index is built from.
struct Source {
    /// The replayed game's timeline, finished.
    const oa::media::director::Timeline* timeline{};
    /// What the recording's stream holds.
    const StreamNotes* stream{};
    /// How every player stood on the last tick (director::read_player_stats).
    std::vector<oa::media::director::PlayerStats> final_stats{};
    /// The recording's length on the recorder's clock, in milliseconds; 0 when not known.
    uint64_t length_ms{};
    /// How `meta.json` says the game ended, when that word is `finished` or
    /// `left`. Nothing for a `.tad`, a description that was not read, and any
    /// other word. Used only when the replay reached the end of the recording
    /// and the commanders, a departure and the end screen did not decide it.
    std::optional<oa::formats::oademo::IndexEnd> recorded_end{};
    /// How a campaign mission ended for the player who recorded it, from
    /// that player's `result` in `meta.json`. A skirmish and a network game
    /// leave this unset, and their result stays with the commanders.
    enum class Mission : uint8_t { none, won, lost };
    Mission mission{Mission::none};
    /// That player's number in the recording. Unused when `mission` is `none`.
    int mission_player{0};
};

/// Builds a replay's index.
///
/// The totals are the timeline's player stats on every
/// formats::oademo::index_period_ticks-th tick from the first; a player whose
/// slot has been emptied keeps its last totals, with no units. The damage in
/// each stretch of that many ticks is the health points taken off units by
/// every health event but healing and paralysis. The recording ended `cut`
/// when the replay could not go on to the end of what was recorded. Otherwise
/// `finished` when the commanders decided it or its end-of-game records came,
/// `left` when players left or dropped with the game undecided, and, when
/// none of those, `finished` or `left` as `Source::recorded_end` says. It is
/// `cut` when that is unset. `ended.tick` is the last tick. A campaign
/// mission that reached its end names its result from `Source::mission`:
/// the recording player won, or every other player won so that player lost.
/// That replaces a result the commanders would have given. A mission that
/// was left, and any game that is not a campaign, keep the commanders' result.
///
/// @param source the timeline, the stream's notes, the last totals and the
///        recording's own end
/// @return the index. Its battle events are the units destroyed, the
///         buildings lost and the large engagements. The heaviest fight in
///         each square and stretch is kept, and what remains is spread across
///         the game when there are more than
///         formats::oademo::most_battle_events. A commander created or lost
///         is kept too, and does not by itself count as fighting. A commander
///         displaced from its slot is left out, its creation and its loss.
[[nodiscard]] oa::formats::oademo::TimelineIndex build_index(const Source& source);

/// How far ahead of the moment on show the camera looks for a fight, in
/// ticks: eight seconds.
inline constexpr uint32_t battle_look_ahead_ticks = 240;

/// How long a fight that has just passed still holds the camera, in ticks:
/// ten seconds.
inline constexpr uint32_t battle_trail_ticks = 300;

/// How far a quiet stretch still follows the battle, in ticks: one minute.
///
/// Inside it the nearest fight is shown, a fight ahead of the moment when
/// one ahead and one behind are equally close. Beyond it the camera has no
/// fight to follow.
inline constexpr uint32_t battle_lull_ticks = 1800;

/// Where the camera that follows the battle should look.
struct BattleAim {
    /// A fight was found.
    bool found{};
    /// Its place across the map, in map pixels.
    int32_t x{};
    /// Its place down the map, on the row the picture draws.
    int32_t z{};
    /// The tick of the fight it chose.
    uint32_t tick{};
};

/// Chooses the fight to show at a moment.
///
/// A fight in the next battle_look_ahead_ticks wins, the heaviest first. When
/// that stretch is quiet, the most recent fight in the battle_trail_ticks
/// just passed is shown. When both are quiet, the nearest fight inside
/// battle_lull_ticks is shown, the one ahead when the two are equally far.
/// A commander being placed or lost is not a fight.
///
/// @param events the index's battle events
/// @param tick the moment on show
/// @return the place; found is false when no fight is in those stretches
[[nodiscard]] BattleAim
battle_aim(std::span<const oa::formats::oademo::BattleEvent> events, uint32_t tick);

/// The place Director shows before it has a fight to follow: the start of
/// the first commander the index names, the lowest player number first on
/// the same tick. The opening then shows a base rather than wherever the
/// picture happened to be.
///
/// @param events the index's battle events
/// @return the commander's place; `found` is false when the index names none
[[nodiscard]] BattleAim director_opening(std::span<const oa::formats::oademo::BattleEvent> events);

/// The fewest ticks of the battle between two cuts: 4 seconds. The same
/// count the planner uses (`min_cut_ticks`).
inline constexpr uint32_t director_min_cut_ticks = 120;

/// How much a new fight must outweigh the one on show before the camera
/// leaves it: numerator over denominator, 1.5. The planner's switch ratio.
inline constexpr uint32_t director_switch_numerator = 3;
inline constexpr uint32_t director_switch_denominator = 2;

/// The farthest a new fight may be for a pan rather than a cut, in percent
/// of the picture's width. The planner's `pan_reach_percent`. A move farther
/// than this, or farther than about two screens, is a cut.
inline constexpr uint32_t director_pan_reach_percent = 75;

/// The least real time a view is held, in milliseconds: a second and a half.
/// A fast playback waits this long rather than cutting on every fight.
inline constexpr uint32_t director_hold_ms = 1500;

/// How long a place just left is not shown again, in ticks: 10 seconds.
inline constexpr uint32_t director_return_ticks = 300;

/// Two places this close, in map pixels, are the same subject. A move no
/// farther than this is not a cut.
inline constexpr int32_t director_same_place_pixels = 64;

/// Where Director is looking, and the fight it is following.
///
/// Passed back in as `previous`, so each step stays a pure function of its
/// arguments.
struct DirectorView {
    /// A fight is being followed.
    bool has_subject{};
    /// This step left the previous fight for another.
    bool changed{};
    /// The new fight was far enough to cut. False for a pan, and when the
    /// fight did not change.
    bool cut{};
    /// The place the picture shows, across the map, in map pixels.
    int32_t x{};
    /// The place the picture shows, down the map.
    int32_t z{};
    /// The fight being followed, across the map.
    int32_t subject_x{};
    /// The fight being followed, down the map.
    int32_t subject_z{};
    /// The tick of that fight.
    uint32_t subject_tick{};
    /// Its weight. At least 1 while a fight is followed.
    uint32_t subject_weight{};
    /// The tick of the last cut or pan.
    uint32_t cut_tick{};
    /// A place was left. The camera does not go back there inside
    /// director_return_ticks.
    bool left{};
    /// The place the picture showed when it left, across the map.
    int32_t left_x{};
    /// The place the picture showed when it left, down the map.
    int32_t left_z{};
    /// The tick it left.
    uint32_t left_tick{};
    /// Where the current pan started, across the map. A cut starts on the fight.
    int32_t pan_from_x{};
    /// Where the current pan started, down the map.
    int32_t pan_from_z{};
    /// The `since_cut_ms` at which this pan segment started. A change that
    /// restarts the caller's clock leaves this at 0. A fight that continues
    /// nearby starts a segment without restarting that clock.
    uint32_t pan_origin_ms{};
};

/// Follows one fight.
///
/// The fight on show is kept while it is ahead inside battle_look_ahead_ticks
/// or behind inside battle_trail_ticks. While it is, another fight takes the
/// picture only when it outweighs that one by
/// director_switch_numerator / director_switch_denominator. An older fight
/// gives way to the fight battle_aim names. Either way the picture changes
/// only once director_min_cut_ticks of the battle and director_hold_ms of
/// real time have passed since the last change. With nothing ahead, the most
/// recent fight behind is the one a new view may take. A fight within
/// director_same_place_pixels is the same subject: the picture eases across
/// and the hold is not started again. A fight within
/// director_pan_reach_percent of the picture's width, and within about two
/// screens, is eased to. Anything farther is a cut. A place just left is not
/// taken again inside director_return_ticks. The first fight is taken at once.
///
/// @param events the index's battle events
/// @param tick the moment on show
/// @param since_cut_ms real time since the last change, in milliseconds
/// @param previous the view the last call returned; a default view at the start
/// @param view_width the picture's width, in map pixels; 0 makes every move a cut
/// @param view_height the picture's height, in map pixels
/// @return the view to show. `x` and `z` are the picture's middle.
[[nodiscard]] DirectorView director_view(
    std::span<const oa::formats::oademo::BattleEvent> events,
    uint32_t tick,
    uint32_t since_cut_ms,
    const DirectorView& previous,
    int32_t view_width,
    int32_t view_height
);

} // namespace oa::app::replay_index

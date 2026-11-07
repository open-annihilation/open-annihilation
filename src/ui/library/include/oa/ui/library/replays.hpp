// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The Library's Replays section: the replays on this computer, as the app
// reads them (oa/data/replays/replays.hpp), with the section's filters, its
// kind menu, the search, the sort, the rows grouped by day and the details
// card. Like the rest of the model it reads no file or clock: the app gives
// each replay's local day and time and the day today.
#pragma once

#include "oa/ui/library/library.hpp"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::library {

/// Recomputes the visible replays from the filter, the kind, the search
/// and the sort, and keeps the selection and the marks among them.
///
/// A selection that still shows stays; otherwise the first row is selected.
/// Marks that no longer show are dropped.
///
/// @param[in,out] library the Library
void update_replays(Library& library);

/// Shows a filter, keeping the kind, the search and the sort.
///
/// @param[in,out] library the Library
/// @param filter the filter
void set_replay_filter(Library& library, ReplayFilter filter);

/// Shows one kind of game, or every kind.
///
/// @param[in,out] library the Library
/// @param kind the game.kind shown; empty for All kinds
void set_replay_kind(Library& library, std::string_view kind);

/// Orders the replays.
///
/// @param[in,out] library the Library
/// @param sort the order
void set_replay_sort(Library& library, ReplaySort sort);

/// Selects a replay, and marks it as a press with `pick` does.
///
/// A path that does not show changes nothing.
///
/// @param[in,out] library the Library
/// @param path the replay's path
/// @param pick how the marks change
void select_replay(Library& library, std::string_view path, ReplayPick pick = ReplayPick::only);

/// Moves the selection along the visible replays, marking that one alone.
///
/// @param[in,out] library the Library
/// @param delta rows to move; negative moves up
void move_replay_selection(Library& library, int delta);

/// Shows in the card why Watch could not start a replay, while that replay
/// is selected. The next press of Watch, or a new refresh of the rows, does
/// not clear it; selecting the replay again after another does.
///
/// @param[in,out] library the Library
/// @param path the replay's path
/// @param sentence what the game said, in the player's language
void refuse_replay(Library& library, std::string_view path, std::string sentence);

/// Returns the selected replay.
///
/// @param library the Library
/// @return its row; null when none is selected
[[nodiscard]] const ReplayRow* selected_replay(const Library& library);

/// Finds a replay by its path.
///
/// @param library the Library
/// @param path the replay's path
/// @return its row; null when no replay has that path
[[nodiscard]] const ReplayRow* find_replay(const Library& library, std::string_view path);

/// Returns where a replay shows in the list.
///
/// @param library the Library
/// @param path the replay's path
/// @return its place among the visible replays, from 0; none when it does not show
[[nodiscard]] std::optional<std::size_t>
replay_place(const Library& library, std::string_view path);

/// Counts what a filter keeps, before the kind and the search, so that no
/// count moves with them.
///
/// @param library the Library
/// @param filter the filter
/// @return how many replays it keeps
[[nodiscard]] std::size_t replay_filter_count(const Library& library, ReplayFilter filter);

/// Tells whether a filter keeps a replay.
///
/// This month keeps a replay recorded in the last month_days days, today
/// included, and a replay dated after today; Older keeps the rest. Kept keeps
/// the replays the player keeps, Imported those opened from outside. A
/// damaged replay is kept by This month or Older by its file's date.
///
/// @param library the Library
/// @param row the replay
/// @param filter the filter
/// @return true when it keeps it
[[nodiscard]] bool
replay_in_filter(const Library& library, const ReplayRow& row, ReplayFilter filter);

/// Tells whether every word of a search is in a replay's map, one of its
/// players, its mod or its planet, without ASCII case.
///
/// @param row the replay
/// @param query the search
/// @return true when it matches, and for an empty search
[[nodiscard]] bool replay_matches(const ReplayRow& row, std::string_view query);

/// One entry of the kind menu.
struct ReplayKindItem {
    std::string id{};    ///< the game.kind; empty for All kinds
    std::string title{}; ///< the words shown, looked up
};

/// Lists the kind menu: All kinds, Multiplayer, Skirmish, Campaign, then
/// each kind an extension registered, by its own title.
///
/// @param library the Library
/// @return the entries, top to bottom
[[nodiscard]] std::vector<ReplayKindItem> replay_kind_items(const Library& library);

/// Returns the words a kind of game is shown by.
///
/// The engine's three kinds have their words; a registered kind its title,
/// looked up; any other kind is shown as written.
///
/// @param library the Library
/// @param kind the game.kind
/// @return the words
[[nodiscard]] std::string replay_kind_title(const Library& library, std::string_view kind);

/// Returns the words a sort is shown by in the sort menu.
///
/// @param library the Library
/// @param sort the sort
/// @return such as "Newest first", looked up
[[nodiscard]] std::string replay_sort_title(const Library& library, ReplaySort sort);

/// Returns the words a filter is shown by in the section list.
///
/// @param library the Library
/// @param filter the filter
/// @return such as "This month", looked up
[[nodiscard]] std::string replay_filter_title(const Library& library, ReplayFilter filter);

/// One run of visible replays recorded on one day, under its heading.
struct ReplayDay {
    std::string heading{}; ///< "Today", "Yesterday", or the date: "Mon 2 Nov"
    std::size_t first{};   ///< its first replay's place among the visible ones
    std::size_t count{};   ///< how many replays it holds
};

/// Groups the visible replays by day, while the sort is Newest.
///
/// @param library the Library
/// @return the days, newest first; one run with no heading for another sort
[[nodiscard]] std::vector<ReplayDay> replay_days(const Library& library);

/// Writes a day as a heading: "Today", "Yesterday", the weekday, day and
/// month ("Mon 2 Nov"), and the year as well outside the year of today.
///
/// @param library the Library; today is its inputs'
/// @param day the day, as day_number counts
/// @return the words
[[nodiscard]] std::string replay_day_heading(const Library& library, int32_t day);

/// Writes a game's length as the list shows it: "23:52", "1:12:09".
///
/// @param length_ms the length, in milliseconds
/// @return the text; "–" when the length is not known
[[nodiscard]] std::string replay_length_text(uint64_t length_ms);

/// Writes a tick as the card writes a remembered place: "11:40", "0:00".
///
/// Thirty ticks are one second. Tick 0 is "0:00", not the dash a length of
/// zero uses.
///
/// @param tick the tick
/// @return the text
[[nodiscard]] std::string replay_tick_text(uint32_t tick);

/// Writes a local time of day: "21:14".
///
/// @param minute minutes into the day
/// @return the text
[[nodiscard]] std::string replay_time_text(int32_t minute);

/// Writes the players as a row's reading says them: the names joined by "vs"
/// up to three players, else "4 players".
///
/// @param library the Library
/// @param row the replay
/// @return the words; empty for a replay with no player
[[nodiscard]] std::string replay_players_text(const Library& library, const ReplayRow& row);

/// Returns the name a replay is shown by, on its row and as its card's title:
/// its map's, or, for a file that names none, the file's own name.
///
/// @param row the replay
/// @return the name
[[nodiscard]] std::string replay_title(const ReplayRow& row);

/// Writes the card's line of when and how long: "Today 15:12 · 1:07:49". A
/// length that is not known, and a damaged replay's, are left out.
///
/// @param library the Library
/// @param row the replay
/// @return the words
[[nodiscard]] std::string replay_when_line(const Library& library, const ReplayRow& row);

/// Writes why a replay cannot be watched: for a damaged one, its sentence
/// after "This replay cannot be watched."; for one the game refused, what the
/// game said; otherwise the sentences for content it still needs. A Delete
/// the trash refused for the selected replay is said first.
///
/// @param library the Library
/// @param row the replay
/// @return the sentences, as one text; empty when nothing stops it
[[nodiscard]] std::string replay_note(const Library& library, const ReplayRow& row);

/// Writes the amber chip for content a replay still needs.
///
/// A mod comes before a map, then a mutator, an expansion and the engine.
/// Offline and "not in the catalogue" leave the chip as the content. A
/// damaged replay, and a map named only by its title, have none.
///
/// @param library the Library
/// @param row the replay
/// @return the chip's words; empty when the row has no such chip
[[nodiscard]] std::string replay_need_chip(const Library& library, const ReplayRow& row);

/// The sentences that go in the card's amber box: a fetch in progress, or
/// the content the replay still needs. A damaged file and a refusal stay in
/// the note, and this is empty for those. A failed Delete stays in the note
/// too, and is not said here.
///
/// @param library the Library
/// @param row the replay
/// @return the sentences; empty when the box is not drawn
[[nodiscard]] std::string replay_caution(const Library& library, const ReplayRow& row);

/// The shorter Watch caption, used when the full one is wider than the bar.
///
/// @param library the Library
/// @return the caption; empty when no replay is selected
[[nodiscard]] std::string replay_watch_short(const Library& library);

/// One piece of a row's second line: a player's colour dot, or some text.
struct ReplayLinePiece {
    /// The player colour of a dot; unset for text.
    std::optional<int32_t> dot{};
    std::string text{}; ///< the text, for a piece that is not a dot
};

/// Lays out a row's second line as its pieces, left to right. Two or three
/// players are each a dot and the name, joined as the language joins them
/// ("● Varn vs ● Kestrel"); past three the dots stand together and "N players"
/// follows them. The game's length ends the line.
///
/// @param library the Library
/// @param row the replay
/// @return the pieces; none for a damaged replay with no player
[[nodiscard]] std::vector<ReplayLinePiece>
replay_line_pieces(const Library& library, const ReplayRow& row);

/// Writes what a row says under its map's name: its kind, and the planet
/// for a kind whose object names one.
///
/// @param library the Library
/// @param row the replay
/// @return such as "Multiplayer" or "Outpost · Corvane"
[[nodiscard]] std::string replay_kind_line(const Library& library, const ReplayRow& row);

/// Writes a replay's whole line, as automation and a screen reader read its row.
///
/// @param library the Library
/// @param row the replay
/// @return the map, the kind, the players, the length, the time, the size,
///         and Kept, After a crash and Damaged when they hold
[[nodiscard]] std::string replay_reading(const Library& library, const ReplayRow& row);

/// One line of the details card's facts.
struct ReplayFact {
    std::string key{};   ///< the fact's name for automation: "length", "kept-until", …
    std::string label{}; ///< "Length", "Recorded", "Mod", … in the player's language
    std::string value{}; ///< what it says
};

/// Lists the details card's facts for a replay: Length, Recorded, Mod,
/// Expansions, Mutators, File, and Kept until (or Kept).
///
/// @param library the Library
/// @param row the replay
/// @return the facts, in the order shown
[[nodiscard]] std::vector<ReplayFact> replay_facts(const Library& library, const ReplayRow& row);

/// Writes the details card's subtitle: the kind, and the planet and sector
/// for a kind whose object names them.
///
/// @param library the Library
/// @param row the replay
/// @return such as "Outpost · Corvane, Low Reach"
[[nodiscard]] std::string replay_card_kind(const Library& library, const ReplayRow& row);

/// One button of the details card.
struct ReplayButton {
    Action action{Action::watch_replay}; ///< what it asks for
    bool enabled{};                      ///< it takes a press
    std::string text{};                  ///< its words, looked up
    /// Drawn in the danger look: Delete replay, and the confirm beside it.
    bool danger{};
};

/// Lists the details card's buttons: Watch, then Watch from when a Watch was
/// left part way, then Export…, then Keep (Kept when every marked replay is
/// kept), then Delete replay, then the confirm beside it once Delete has been
/// pressed, then Open folder.
///
/// Watch is off for a damaged replay and during a game. When the replay
/// needs content, its words are Get … and watch, Turn on and watch, or
/// Check for updates, and it is left out when that content is not in the
/// catalogue or the registry could not be reached. Export… opens the
/// dialog for every marked replay. Keep acts on every marked replay, and is
/// left out when every marked replay was imported, which is kept already.
/// Delete asks, and the confirm beside it is what moves the marked files to
/// the trash. Choosing another replay takes the confirm away. Open folder
/// shows the folder the replay is in.
///
/// @param library the Library
/// @return the buttons, in the order shown; none while no replay is selected
[[nodiscard]] std::vector<ReplayButton> replay_buttons(const Library& library);

/// Tells whether every marked replay was imported, so that Keep has nothing
/// to do.
///
/// @param library the Library
/// @return true when at least one is marked and all of them are imported
[[nodiscard]] bool marked_all_imported(const Library& library);

/// Tells whether every marked replay is kept, as Keep's toggle shows.
///
/// @param library the Library
/// @return true when at least one is marked and all of them are kept
[[nodiscard]] bool marked_kept(const Library& library);

/// Writes the words an empty Replays list shows: the first line and, when
/// there is one, the line under it.
struct ReplayEmpty {
    std::string title{}; ///< "No replays yet.", or why the filter shows none
    bool reading{};      ///< the folder is being read and no row has come yet; the title says so
    std::string
        line{};     ///< "Games you play are kept here for 30 days.", "Recording is off.", or empty
    bool turn_on{}; ///< Turn on recording is offered
    bool open_file{}; ///< Open a replay file is offered
};

/// Returns what the Replays list shows when no row shows. While the folder is
/// being read and no row has come, it says so: it never says "No replays yet.".
///
/// @param library the Library
/// @return the words, and whether Turn on recording is offered
[[nodiscard]] ReplayEmpty replay_empty(const Library& library);

/// Writes the Replays section's status line for the footer: "Reading 12 of
/// 64 replays" while the folder is read ("Reading the replays…" until its
/// files are counted), else how many the list shows and their size.
///
/// @param library the Library
/// @return the line
[[nodiscard]] std::string replay_status(const Library& library);

} // namespace oa::ui::library

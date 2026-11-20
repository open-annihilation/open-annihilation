// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Which spoken lines a frame's notes, seats and unit reports become.
// The functions keep no game and no lobby: a test passes the records.
#pragma once

#include "oa/platform/screen_reader/announce.hpp"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app::spoken {

/// The low nibble of a match message is its kind. A unit report is 1.
inline constexpr uint8_t kKindMask = 0x0f;
/// A unit's speech caption (`messages::kind_unit_report`).
inline constexpr uint8_t kUnitReport = 1;
/// The team the battle room shows as "None".
inline constexpr uint8_t kNoTeam = 5;
/// The host's option bits a player changes in the room. The low nibble is
/// the count of seated players, 0x10 is START, 0x20 is the host's own ready
/// and 0x40 the host's own watching; none of them is an option.
inline constexpr uint16_t kRoomOptionBits = 0xff80;
/// The colours a seat can show. A larger number is no colour chosen yet.
inline constexpr uint8_t kColourCount = 10;
/// Member rows stay the room's list for this long after entering, in milliseconds.
inline constexpr uint64_t kListHoldMs = 2000;
/// A quiet stretch this long after the last member row settles the list, in
/// milliseconds. The list stays until the later of this and `kListHoldMs`.
inline constexpr uint64_t kListQuietMs = 1000;

/// One note from the spoken inbox.
struct Note {
    std::string_view kind; ///< the note's kind
    std::string_view text; ///< the note's text
};

/// A seat as it was remembered.
struct SeatWas {
    bool taken{};         ///< a player occupied it
    bool has_details{};   ///< a PlayerSetupInfo has been seen for this player
    uint32_t player_id{}; ///< the player
    std::string name;
    bool ready{};
    uint8_t side{};
    uint8_t color{};
    uint8_t team{};
    bool self{}; ///< the local player
};

/// A seat as this frame sees it.
struct SeatNow {
    bool taken{};
    bool has_details{}; ///< `slot_info` returned a record; zeros are not a baseline
    uint32_t player_id{};
    std::string name;
    bool ready{};
    uint8_t side{};
    uint8_t color{};
    uint8_t team{};
    bool self{};
    std::string side_name; ///< the side as the room names it, or empty for the number
};

/// The battle room as this frame sees it.
struct RoomNow {
    bool open{};         ///< the battle-room screen is showing a game
    bool creator_left{}; ///< the room closed because its creator left
    std::vector<SeatNow> seats;
    bool host_present{}; ///< `host_slot` names a seat
    std::size_t host_slot{};
    bool host_has_details{}; ///< the host's info was seen this frame
    std::string map;
    uint16_t option_bits{}; ///< options with the ready bit already off
    uint16_t metal{};
    uint16_t energy{};
    uint16_t units{};
    int16_t start_frame{};
};

/// One new row of the match message ring.
struct ReportRow {
    std::string text;
    uint8_t kind{};    ///< the line's kind, low nibble already applied by the caller or not
    bool local_unit{}; ///< the unit belongs to the local player
    bool commander{};  ///< the unit's definition is a commander
};

/// What has already been heard. The functions update it.
struct Memory {
    std::string chat_room{"Main"};
    std::string local_name;
    std::string session_said; ///< the last login sentence, so it is said once
    bool listing{};           ///< member rows are the room's list until the list has settled
    uint64_t entered_ms{};    ///< when the room was entered, on the caller's clock
    uint64_t last_row_ms{};   ///< when the latest member row arrived, on that clock
    bool room_open{};
    std::vector<SeatWas> seats;
    std::string map;
    uint16_t option_bits{};
    uint16_t metal{};
    uint16_t energy{};
    uint16_t units{};
    int16_t start_frame{};
    bool host_details{}; ///< the host's options and map have been seeded
    bool host_known{};
    uint32_t host_id{};
};

/// Which of the frame's lines may be spoken.
struct Hear {
    bool speaking{true}; ///< a reader is running, or the preference is on
    oa::platform::screen_reader::SpeakChat chat{oa::platform::screen_reader::SpeakChat::all};
    bool lobby{true};
    bool ticker{false};
};

/// The indices of a ring from `cursor` up to, but not including, `head`.
///
/// The walk is the one the battle-room chat uses. A wrapped head still
/// yields every new slot, including when the ring is full.
///
/// @param cursor the first index to read
/// @param head the index just past the last new line
/// @param capacity how many slots the ring holds; 0 yields nothing
/// @return the indices, in order
[[nodiscard]] std::vector<uint16_t> ring_indices(uint16_t cursor, uint16_t head, uint16_t capacity);

/// The lines a batch of notes becomes, and the room and mute memory they update.
///
/// An internal `line` note is queued as it is written. A raw page, session,
/// console, place, room or alert is not spoken again. A page (`G`) is
/// spoken under speak-chat `pages` and `all`, and a whisper (`W`) under
/// `all` only. Entering a room speaks nothing for member rows until the list
/// has settled: about two seconds after the entered line, or about one
/// second after the last row, whichever is later. A join after that is
/// spoken and may collapse. The local player's own join or leave is not
/// spoken. Two identical login lines are one sentence.
///
/// @param hear which lines may be spoken
/// @param[in,out] memory what was heard before
/// @param notes the batch, in order
/// @param now_ms the caller's clock, in milliseconds
/// @return the lines to queue, in order
[[nodiscard]] std::vector<oa::platform::screen_reader::SpokenOffer>
take_notes(const Hear& hear, Memory& memory, const std::vector<Note>& notes, uint64_t now_ms);

/// The lines a battle room's seats and host info become.
///
/// Until the host's info has been seen, the seats are stored and not
/// announced: a joiner sees itself a frame or two before the host's seat.
/// The map may be said once then. An option is not, and only the bits in
/// `kRoomOptionBits` are options. A seat's side, colour, team and ready are
/// seeded from its first info and are not announced as changes, and a
/// colour that is not one of the ten (none chosen yet) neither starts nor
/// ends a change. A join or a leave is said at once, so it comes before that
/// player's changes. The team is the number the room shows, from 1. The
/// local player's own changes are stored and not spoken. When the room
/// closes because its creator left, the line is "The host has left" and the
/// seats are not each announced as leaving.
///
/// @param hear which lines may be spoken
/// @param[in,out] memory what was heard before
/// @param room the room this frame
/// @return the lines to queue, in order
[[nodiscard]] std::vector<oa::platform::screen_reader::SpokenOffer>
take_room(const Hear& hear, Memory& memory, const RoomNow& room);

/// The commander-under-attack lines among new unit reports.
///
/// A row is spoken only when its kind is a unit report, the unit is the
/// local player's commander, and the text ends with `attack_ending` (the
/// translated ": Under Attack").
///
/// @param hear which lines may be spoken
/// @param rows the new rows, oldest first
/// @param attack_ending the caption ending, including the colon and space
/// @return the lines to queue
[[nodiscard]] std::vector<oa::platform::screen_reader::SpokenOffer>
take_reports(const Hear& hear, const std::vector<ReportRow>& rows, std::string_view attack_ending);

/// Battle-room and match chat lines, skipping the local player and the
/// engine banner.
///
/// A line whose body contains `[Engine:` is left out. The speaker's own
/// line is left out. A directed line is a whisper. The rest may collapse.
///
/// @param hear which lines may be spoken
/// @param room the room a burst names
/// @param lines the new lines, as the ring stores them
/// @param self_name the local player's name; empty skips no one
/// @return the lines to queue
[[nodiscard]] std::vector<oa::platform::screen_reader::SpokenOffer> take_chat_lines(
    const Hear& hear,
    const std::string& room,
    const std::vector<std::string>& lines,
    std::string_view self_name
);

} // namespace oa::app::spoken

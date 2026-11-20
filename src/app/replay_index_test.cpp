// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The index pass's two parts that need no match: the notes read from a
// made-up recording of every kind of record (also inside a .oademo, with the
// refusals), and the index built from a made-up timeline and notes: the
// totals, the damage, the markers and how they are ordered and capped, the
// commanders' result, how the replay ended, and where the fighting was.

#include "oa/app/replay_index.hpp"

#include "oa/core/player_setup.h"
#include "oa/formats/oademo/oademo.hpp"
#include "oa/formats/oademo/timeline_index.hpp"
#include "oa/formats/tad.hpp"
#include "oa/netgame/records.hpp"
#include "oa/netgame/unicode_chat.hpp"
#include "oa/session/demo/recording.hpp"
#include "oa/test/check.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace {

namespace director = oa::media::director;
namespace demo = oa::session::demo;
namespace netgame = oa::netgame;
namespace oademo = oa::formats::oademo;
namespace replay_index = oa::app::replay_index;
namespace tad = oa::formats::tad;

using replay_index::Note;
using replay_index::NoteKind;

/// Returns a record's wire bytes.
///
/// @param record the record
/// @return its bytes, the type byte first
template <class Record>
std::vector<uint8_t> wire(const Record& record) {
    std::vector<uint8_t> bytes(netgame::record_length_table[static_cast<uint8_t>(Record::type)]);
    std::size_t written = 0;
    OA_CHECK(
        netgame::encode_record(record, bytes.data(), bytes.size(), &written) ==
        netgame::WireError::ok
    );
    bytes.resize(written);
    return bytes;
}

/// Returns a chat record's wire bytes.
///
/// @param text the line, at most 63 bytes
/// @param private_line the line starts with a NUL byte, as a private one does
/// @return the record
std::vector<uint8_t> chat(const std::string& text, bool private_line = false) {
    netgame::ChatRecord record{};
    std::memcpy(record.text + (private_line ? 1 : 0), text.data(), text.size());
    return wire(record);
}

/// Returns a payload of plain records: the tick base and an empty tick when
/// the packet carries a tick, then the records.
///
/// @param tick the sender's tick, 0 for a packet with no unit state
/// @param records the game records
/// @return the payload, the marker byte first
std::vector<uint8_t> payload(uint32_t tick, std::initializer_list<std::vector<uint8_t>> records) {
    std::vector<uint8_t> bytes{tad::payload_marker};
    if (tick != 0) {
        bytes.push_back(static_cast<uint8_t>(tad::RecordType::tick_base));
        for (int shift = 0; shift < 32; shift += 8)
            bytes.push_back(static_cast<uint8_t>(tick >> shift));
        bytes.push_back(static_cast<uint8_t>(tad::RecordType::empty_tick));
    }
    for (const std::vector<uint8_t>& record : records)
        bytes.insert(bytes.end(), record.begin(), record.end());
    return bytes;
}

/// Adds a packet to a recording.
///
/// @param[in,out] recording the recording
/// @param sender the sender's number
/// @param bytes the payload
void add_packet(demo::DemoRecording& recording, uint8_t sender, const std::vector<uint8_t>& bytes) {
    constexpr uint8_t delay_ms = 10;
    std::vector<uint8_t> packet{delay_ms, 0, sender};
    packet.insert(packet.end(), bytes.begin(), bytes.end());
    recording.packets.push_back(std::move(packet));
}

/// The three players of the made-up recording: their transport ids.
constexpr uint32_t anna_id = 101;
constexpr uint32_t bob_id = 102;
constexpr uint32_t watcher_id = 103;

/// Makes a player of the recording.
///
/// @param id the player's transport id
/// @param name the player's name
/// @param watching the player only watches
/// @param unicode the player's chat is UTF-8
/// @return the player
demo::RecordingPlayer recording_player(uint32_t id, const char* name, bool watching, bool unicode) {
    demo::RecordingPlayer made;
    made.player_id = id;
    made.name = name;
    made.info.player_id = id;
    made.info.color = static_cast<uint8_t>(id % 10);
    if (watching)
        made.info.options = OA_SETUP_OPTION_WATCHER;
    if (unicode)
        netgame::mark_unicode_chat(reinterpret_cast<uint8_t*>(&made.info), true);
    return made;
}

/// Makes the recording the notes are read from: Anna (UTF-8 chat), Bob and a
/// watcher, with a chat line, a pause, an alliance, a recorder command, a
/// private line, departures, a speed change, an end-of-game record, and a
/// packet from a player the recording does not list.
std::vector<uint8_t> made_up_recording() {
    demo::RecordingSetup setup;
    setup.max_units = 10;
    setup.map_name = "Test Map";
    setup.players = {
        recording_player(anna_id, "Anna", false, true),
        recording_player(bob_id, "Bob", false, false),
        recording_player(watcher_id, "Cy", true, false),
    };
    demo::DemoRecording recording;
    demo::recording_begin(&recording, setup);

    netgame::PauseSpeedRecord pause{};
    pause.kind = netgame::pause_speed_kind_pause;
    pause.value = 1;
    add_packet(recording, 1, payload(30, {chat("<Anna> hello"), wire(pause)}));

    pause.value = 0;
    netgame::AllianceRecord alliance{};
    alliance.player_id_a = anna_id;
    alliance.player_id_b = bob_id;
    alliance.value = 1;
    alliance.both_sides = 1;
    // The same word as a team game's request that the other machine set its
    // own side: all ones.
    netgame::AllianceRecord request{};
    request.player_id_a = anna_id;
    request.player_id_b = watcher_id;
    request.value = 1;
    request.both_sides = 0xFFFFFFFFu;
    add_packet(
        recording,
        2,
        payload(31, {wire(pause), wire(alliance), wire(request), chat("<Bob> caf\xE9")})
    );

    // No unit state: the packet is due on the tick of the one before it.
    add_packet(
        recording, 1, payload(0, {chat("<Anna> +logo 1"), chat("<Anna> gg \xC3\xA9 \xE6\x97\xA5")})
    );

    netgame::PauseSpeedRecord speed{};
    speed.kind = 1;
    speed.value = 20;
    netgame::RejectRecord reject{};
    reject.player_id = bob_id;
    reject.reason = static_cast<uint8_t>(netgame::RejectReason::connection_lost);
    netgame::DisconnectNoticeRecord left{};
    left.player_id = bob_id;
    add_packet(
        recording, 3, payload(60, {chat("private", true), wire(reject), wire(left), wire(speed)})
    );

    // A packet from a number the recording does not list is skipped, and so
    // is one that holds no game record.
    add_packet(recording, 9, payload(75, {chat("<Nobody> hi")}));
    add_packet(recording, 1, {tad::payload_marker, 0x2e, '.', 'x'});

    // An economy record that asks for no reply is the machines' refresh, not
    // the end of the game.
    netgame::EconomyRecord refresh{};
    netgame::EconomyRecord economy{};
    economy.want_reply = 1;
    netgame::RejectRecord leaving{};
    leaving.player_id = watcher_id;
    leaving.reason = static_cast<uint8_t>(netgame::RejectReason::leaving);
    add_packet(recording, 1, payload(90, {wire(refresh), wire(economy), wire(leaving)}));

    const tad::WriteResult written = demo::recording_write(recording);
    OA_CHECK(written.ok());
    return written.bytes;
}

/// Returns a recording without its sectors of network addresses, as a
/// `.oademo` holds it.
///
/// @param bytes a recording
/// @return the same recording with no address sector
std::vector<uint8_t> without_addresses(const std::vector<uint8_t>& bytes) {
    const tad::ParseResult parsed = tad::parse(bytes);
    OA_CHECK(parsed.ok());
    if (!parsed.ok())
        return {};
    tad::Demo copy = *parsed.demo;
    copy.sectors.erase(
        std::remove_if(
            copy.sectors.begin(),
            copy.sectors.end(),
            [](const tad::Sector& sector) {
                return sector.type == static_cast<uint32_t>(tad::SectorType::player_address);
            }
        ),
        copy.sectors.end()
    );
    const tad::WriteResult written = tad::write(copy);
    OA_CHECK(written.ok());
    return written.bytes;
}

/// Wraps a recording in a `.oademo`.
///
/// @param recording a recording with no address sector
/// @return the replay's bytes
std::vector<uint8_t> wrapped(const std::vector<uint8_t>& recording) {
    oademo::Replay replay;
    replay.recording = recording;
    oademo::set_recording_identity(replay);
    replay.meta.recording.tad_version = tad::supported_version;
    replay.meta.recording.packets = 7;
    replay.meta.recording.addresses_removed = true;
    replay.meta.recorded.how = "multiplayer";
    replay.meta.game.kind = "multiplayer";
    replay.meta.map.name = "Test Map";
    replay.meta.content.base.id = "totala";
    replay.meta.content.base.version = "3.1c";
    replay.meta.units.count = 1;
    replay.meta.units.table_sha256 = std::string(64, '0');
    replay.meta.units.index_bits = 2;
    oademo::PlayerInfo anna;
    anna.number = 1;
    anna.name = "Anna";
    anna.side = "arm";
    anna.kind = "human";
    replay.meta.players.push_back(anna);
    replay.manifest.engine.recorded_with = "0.9.1";
    replay.manifest.engine.needs = ">=0.9.1";
    oademo::Need base;
    base.kind = "base";
    base.id = "totala";
    base.version = "3.1c";
    replay.manifest.needs.push_back(base);
    const oademo::WriteResult written = oademo::write(replay);
    OA_CHECK(written.ok());
    return written.bytes;
}

/// Tells whether a note is what a test expects of it.
///
/// @param note the note
/// @param tick its tick
/// @param kind its kind
/// @param player its player
/// @param other the player it names
/// @param value its value
/// @param text its text
/// @return true when they agree
bool is_note(
    const Note& note,
    uint32_t tick,
    NoteKind kind,
    int player,
    int other,
    uint8_t value,
    const std::string& text = {}
) {
    return note.tick == tick && note.kind == kind && note.player == player && note.other == other &&
           note.value == value && note.text == text;
}

// The notes of a recording: the players seated in the recording's order, a
// note for each record that is a chat line, a pause, a departure, an
// alliance or an end-of-game record, each on the tick of its packet, the
// private line, the speed change, the unlisted sender and the empty packet
// left out.
void notes_are_read_from_a_recording() {
    const std::vector<uint8_t> recording = made_up_recording();
    std::string error;
    const std::optional<replay_index::StreamNotes> read =
        replay_index::read_stream_notes(recording, error);
    OA_CHECK(read.has_value());
    if (!read.has_value())
        return;
    OA_CHECK(read->players.size() == 3);
    if (read->players.size() == 3) {
        OA_CHECK(read->players[0].number == 1 && read->players[0].name == "Anna");
        OA_CHECK(read->players[1].number == 2 && !read->players[1].watcher);
        OA_CHECK(read->players[2].number == 3 && read->players[2].watcher);
    }
    const std::vector<Note>& notes = read->notes;
    OA_CHECK(notes.size() == 12);
    if (notes.size() != 12)
        return;
    OA_CHECK(is_note(notes[0], 30, NoteKind::chat, 1, 0, 0, "<Anna> hello"));
    OA_CHECK(is_note(notes[1], 30, NoteKind::pause, 1, 0, 1));
    OA_CHECK(is_note(notes[2], 31, NoteKind::pause, 2, 0, 0));
    // An alliance record is one note: the first player's side of the pair,
    // whatever its last word is, 1 or all ones (that word is not read).
    OA_CHECK(is_note(notes[3], 31, NoteKind::alliance, 1, 2, 1));
    OA_CHECK(is_note(notes[4], 31, NoteKind::alliance, 1, 3, 1));
    // Bob's line is in the code page, and read as UTF-8.
    OA_CHECK(is_note(notes[5], 31, NoteKind::chat, 2, 0, 0, "<Bob> caf\xC3\xA9"));
    // The packet with no unit state is due on the tick before it; a command
    // is noted, the builder leaves it out; Anna's line is UTF-8.
    OA_CHECK(is_note(notes[6], 31, NoteKind::chat, 1, 0, 0, "<Anna> +logo 1"));
    OA_CHECK(is_note(notes[7], 31, NoteKind::chat, 1, 0, 0, "<Anna> gg \xC3\xA9 \xE6\x97\xA5"));
    OA_CHECK(is_note(notes[8], 60, NoteKind::rejected, 3, 2, 6));
    OA_CHECK(is_note(notes[9], 60, NoteKind::disconnected, 3, 2, 0));
    OA_CHECK(is_note(notes[10], 90, NoteKind::end_of_game, 1, 0, 1));
    OA_CHECK(is_note(notes[11], 90, NoteKind::rejected, 1, 3, 2));

    // The same notes come out of the recording wrapped in a replay.
    const std::vector<uint8_t> replay = wrapped(without_addresses(recording));
    const std::optional<replay_index::StreamNotes> inside =
        replay_index::read_stream_notes(replay, error);
    OA_CHECK(inside.has_value() && inside->notes.size() == notes.size());
    if (inside.has_value() && inside->notes.size() == notes.size())
        for (std::size_t at = 0; at < notes.size(); ++at)
            OA_CHECK(is_note(
                inside->notes[at],
                notes[at].tick,
                notes[at].kind,
                notes[at].player,
                notes[at].other,
                notes[at].value,
                notes[at].text
            ));
}

// What cannot be read is refused with a sentence: a file that is neither, a
// recording cut short, and a replay of a version this game does not read.
void unreadable_files_are_refused() {
    std::string error;
    const std::vector<uint8_t> nothing(32, 0x55);
    OA_CHECK(!replay_index::read_stream_notes(nothing, error).has_value());
    OA_CHECK(!error.empty());
    std::vector<uint8_t> cut = made_up_recording();
    cut.resize(cut.size() - 3);
    error.clear();
    OA_CHECK(!replay_index::read_stream_notes(cut, error).has_value());
    OA_CHECK(error.find("does not parse") != std::string::npos);
    error.clear();
    const std::vector<uint8_t> empty;
    OA_CHECK(!replay_index::read_stream_notes(empty, error).has_value());
    OA_CHECK(!error.empty());
    std::vector<uint8_t> replay = wrapped(without_addresses(made_up_recording()));
    // The entry's payload is `oademo 1`: a different version is not read.
    const std::string line = "oademo 1";
    const auto at = std::search(replay.begin(), replay.end(), line.begin(), line.end());
    OA_CHECK(at != replay.end());
    if (at != replay.end()) {
        *(at + 7) = '2';
        error.clear();
        OA_CHECK(!replay_index::read_stream_notes(replay, error).has_value());
        OA_CHECK(!error.empty());
    }
}

/// The made-up game: players 1 and 2 fight, 3 watches.
replay_index::StreamNotes three_players() {
    replay_index::StreamNotes stream;
    stream.players = {{1, "Anna", false}, {2, "Bob", false}, {3, "Cy", true}};
    return stream;
}

/// Makes an event.
///
/// @param tick its tick
/// @param kind its kind
/// @param owner the owner's player index
/// @param unit the unit's slot
/// @param commander the unit is a commander
/// @return the event
director::TimelineEvent
event(uint32_t tick, director::EventKind kind, uint8_t owner, uint16_t unit, bool commander) {
    director::TimelineEvent made;
    made.tick = tick;
    made.kind = kind;
    made.owner = owner;
    made.unit = unit;
    if (commander)
        made.flags = director::event_flag::commander;
    return made;
}

/// Makes a hit.
///
/// @param tick its tick
/// @param amount the health points taken
/// @param kind the health event's kind
/// @return the event
director::TimelineEvent hit(uint32_t tick, int32_t amount, uint8_t kind) {
    director::TimelineEvent made = event(tick, director::EventKind::damage, 1, 9, false);
    made.amount = amount;
    made.damage_kind = kind;
    return made;
}

/// Makes a player's stats.
///
/// @param tick the tick
/// @param player the player index
/// @param kills the player's kills
/// @return the stats
director::PlayerStats stats(uint32_t tick, uint8_t player, uint32_t kills) {
    director::PlayerStats made;
    made.tick = tick;
    made.player = player;
    made.kills = kills;
    made.losses = kills / 2;
    made.unit_count = static_cast<uint32_t>(5 + player);
    made.metal = static_cast<int32_t>(1000 + tick);
    made.energy = 2000;
    return made;
}

/// Makes the made-up game's timeline: two commanders (the first displaced
/// and replaced on tick 400, which is no loss) and the second killed on tick
/// 800 by the first, hits of every kind, and each player's stats every 30
/// ticks, the second player's stopping after tick 600 as if it had left.
director::Timeline made_up_timeline() {
    director::Timeline timeline;
    timeline.header.first_tick = 0;
    timeline.header.last_tick = 900;
    timeline.verdict.finished = true;
    timeline.verdict.clean = true;
    timeline.verdict.paced = true;
    timeline.events = {
        event(0, director::EventKind::created, 0, 5, true),
        event(0, director::EventKind::created, 1, 6, true),
        hit(10, 100, 1),
        hit(20, 50, 10),
        hit(25, 30, 2),
        hit(305, 70, 11),
        hit(306, 0, 1),
        event(400, director::EventKind::death, 0, 5, true),
        event(400, director::EventKind::created, 0, 5, true),
        event(800, director::EventKind::death, 1, 6, true),
        hit(5000, 9, 1),
    };
    timeline.events[7].damage_kind = 0;
    timeline.events[9].damage_kind = 1;
    timeline.events[9].other_owner = 0;
    for (uint32_t tick = 0; tick <= 900; tick += 30) {
        timeline.stats.push_back(stats(tick, 0, tick / 30));
        if (tick <= 600)
            timeline.stats.push_back(stats(tick, 1, tick / 60));
        timeline.stats.push_back(stats(tick, 2, 0));
    }
    return timeline;
}

/// Makes a note.
///
/// @param tick its tick
/// @param kind its kind
/// @param player the player it is from
/// @param other the player it names
/// @param value its value
/// @param text its text
/// @return the note
Note note(
    uint32_t tick,
    NoteKind kind,
    int player,
    int other = 0,
    uint8_t value = 0,
    std::string text = {}
) {
    Note made;
    made.tick = tick;
    made.kind = kind;
    made.player = player;
    made.other = other;
    made.value = value;
    made.text = std::move(text);
    return made;
}

/// Builds the index of a timeline and a stream.
///
/// @param timeline the timeline
/// @param stream the stream
/// @return the index
oademo::TimelineIndex
build(const director::Timeline& timeline, const replay_index::StreamNotes& stream) {
    replay_index::Source source;
    source.timeline = &timeline;
    source.stream = &stream;
    source.final_stats = {stats(900, 0, 30), stats(900, 2, 0)};
    source.length_ms = 30000;
    return replay_index::build_index(source);
}

/// Returns how many markers of a kind an index holds.
///
/// @param index the index
/// @param kind the kind
/// @return the count
std::size_t count_of(const oademo::TimelineIndex& index, oademo::MarkerKind kind) {
    return static_cast<std::size_t>(std::count_if(
        index.markers.begin(), index.markers.end(), [kind](const oademo::IndexMarker& marker) {
            return marker.kind == kind;
        }
    ));
}

// The index of the made-up game: its length and verdict, the totals every
// 300 ticks (a player who stopped is carried on with no units), the damage
// of each stretch (healing, paralysis and no damage left out, a hit past the
// end in the last stretch), the markers in tick order, the commanders'
// result and the end.
void an_index_is_built() {
    const director::Timeline timeline = made_up_timeline();
    replay_index::StreamNotes stream = three_players();
    stream.notes = {
        note(100, NoteKind::chat, 1, 0, 0, "<Anna> hi"),
        note(120, NoteKind::chat, 2, 0, 0, "<Bob> +logo 1"),
        note(50, NoteKind::pause, 1, 0, 1),
        note(200, NoteKind::pause, 1, 0, 1),
        note(230, NoteKind::pause, 2, 0, 0),
        note(240, NoteKind::pause, 2, 0, 0),
        note(150, NoteKind::alliance, 1, 3, 1),
        note(160, NoteKind::alliance, 3, 1, 1),
        note(700, NoteKind::alliance, 1, 3, 0),
        note(840, NoteKind::rejected, 1, 1, 1),
        note(850, NoteKind::rejected, 3, 2, 6),
        note(860, NoteKind::disconnected, 3, 2),
        note(880, NoteKind::rejected, 1, 3, 11),
        note(885, NoteKind::end_of_game, 1, 0, 1),
    };
    const oademo::TimelineIndex index = build(timeline, stream);
    OA_CHECK(index.length_ticks == 900 && index.ended_tick == 900 && index.first_tick == 0);
    OA_CHECK(index.length_ms == std::optional<uint64_t>{30000});
    OA_CHECK(index.verdict.finished && index.verdict.clean && index.verdict.paced);
    OA_CHECK(index.period_ticks == 300);

    OA_CHECK(index.players.size() == 3);
    if (index.players.size() == 3) {
        OA_CHECK(index.players[0].number == 1 && index.players[0].final.kills == 30);
        OA_CHECK(index.players[0].final.units == 5);
        // Bob's slot was emptied: what he had scored, and nothing in store.
        OA_CHECK(index.players[1].number == 2 && index.players[1].final.kills == 10);
        OA_CHECK(index.players[1].final.units == 0 && index.players[1].final.metal == 0);
        OA_CHECK(index.players[2].watcher);
    }
    OA_CHECK(index.totals.size() == 2);
    if (index.totals.size() == 2) {
        OA_CHECK(index.totals[0].player == 1 && index.totals[1].player == 2);
        OA_CHECK((index.totals[0].kills == std::vector<uint32_t>{0, 10, 20, 30}));
        OA_CHECK((index.totals[1].kills == std::vector<uint32_t>{0, 5, 10, 10}));
        OA_CHECK((index.totals[0].units == std::vector<uint32_t>{5, 5, 5, 5}));
        OA_CHECK((index.totals[1].units == std::vector<uint32_t>{6, 6, 6, 0}));
        OA_CHECK((index.totals[1].metal == std::vector<int32_t>{1000, 1300, 1600, 0}));
        OA_CHECK((index.totals[0].losses == std::vector<uint32_t>{0, 5, 10, 15}));
    }
    OA_CHECK((index.damage == std::vector<uint64_t>{100, 70, 0, 9}));

    // In tick order: the start first and the end last of their ticks; the
    // pause in the opening, the repeated pause, the command, the lobby's
    // reject, the repeated departure and the end-of-game record are no markers.
    using oademo::MarkerKind;
    const std::vector<std::pair<uint32_t, MarkerKind>> expected{
        {0, MarkerKind::start},
        {100, MarkerKind::chat},
        {160, MarkerKind::alliance},
        {200, MarkerKind::pause},
        {230, MarkerKind::pause},
        {700, MarkerKind::alliance},
        {800, MarkerKind::commander_death},
        {850, MarkerKind::player_dropped},
        {880, MarkerKind::player_defeated},
        {900, MarkerKind::end},
    };
    OA_CHECK(index.markers.size() == expected.size());
    if (index.markers.size() == expected.size()) {
        for (std::size_t at = 0; at < expected.size(); ++at)
            OA_CHECK(
                index.markers[at].tick == expected[at].first &&
                index.markers[at].kind == expected[at].second
            );
        OA_CHECK(index.markers[1].player == 1 && index.markers[1].text == "<Anna> hi");
        // An alliance marker names the pair and whether it is mutual now.
        OA_CHECK(index.markers[2].player == 3 && index.markers[2].other == 1);
        OA_CHECK(index.markers[2].flag && !index.markers[5].flag);
        OA_CHECK(index.markers[3].flag && !index.markers[4].flag);
        // The displaced commander is replaced, so no marker on tick 400.
        OA_CHECK(index.markers[6].player == 2 && index.markers[6].other == 1);
        OA_CHECK(index.markers[7].player == 2 && index.markers[8].player == 3);
    }
    OA_CHECK(index.markers_omitted == 0);
    // The commanders are placed, including the one that replaces the displaced
    // commander. That displacement is no loss, and the hits are too small to
    // be a fight. The file is version 1, and it names its events.
    OA_CHECK(index.version == oademo::timeline_version);
    OA_CHECK(index.events.size() == 4);
    if (index.events.size() == 4) {
        OA_CHECK(index.events[0].kind == oademo::BattleKind::commander);
        OA_CHECK(index.events[0].tick == 0 && index.events[0].player == 1);
        OA_CHECK(index.events[1].kind == oademo::BattleKind::commander);
        OA_CHECK(index.events[1].tick == 0 && index.events[1].player == 2);
        OA_CHECK(index.events[2].kind == oademo::BattleKind::commander);
        OA_CHECK(index.events[2].tick == 400 && index.events[2].player == 1);
        OA_CHECK(index.events[3].kind == oademo::BattleKind::commander_lost);
        OA_CHECK(index.events[3].tick == 800 && index.events[3].player == 2);
        for (const oademo::BattleEvent& battle : index.events)
            OA_CHECK(!oademo::battle_fight(battle.kind));
    }
    OA_CHECK(index.result.winners == std::vector<int>{1});
    OA_CHECK(index.result.by == oademo::ResultBy::commanders);
    OA_CHECK(index.ended == oademo::IndexEnd::finished);

    // The same inputs give the same text, and the text reads back.
    const std::string text = oademo::timeline_index_text(index);
    OA_CHECK(text == oademo::timeline_index_text(build(timeline, stream)));
    const std::optional<oademo::TimelineIndex> read = oademo::parse_timeline_index(text);
    OA_CHECK(read.has_value() && oademo::timeline_index_text(*read) == text);
}

// A chat marker is a line a player typed: it starts with the sender's name
// between `<` and `>` and a space. The recorders' notices and their replies
// to commands, which are chat records too, give no marker; a mistyped
// command without its `+` and an announcement that carries a name do.
void chat_markers_are_lines_a_player_typed() {
    const director::Timeline timeline = made_up_timeline();
    replay_index::StreamNotes stream = three_players();
    stream.notes = {
        note(100, NoteKind::chat, 1, 0, 0, "<Anna> hello"),
        note(110, NoteKind::chat, 2, 0, 0, "***Bob tried to unpause."),
        note(120, NoteKind::chat, 1, 0, 0, "Anna 12"),
        note(130, NoteKind::chat, 1, 0, 0, "All fixes turned on"),
        note(140, NoteKind::chat, 1, 0, 0, "<Anna> logo 6 4"),
        note(150, NoteKind::chat, 1, 0, 0, "<Anna> +logo 6 4"),
        note(160, NoteKind::chat, 2, 0, 0, "<Bob>  allied with Anna"),
        note(170, NoteKind::chat, 2, 0, 0, "<Bob without a close"),
        note(180, NoteKind::chat, 2, 0, 0, "<Bob>no space"),
        note(190, NoteKind::chat, 2, 0, 0, ""),
    };
    const oademo::TimelineIndex index = build(timeline, stream);
    std::vector<std::string> lines;
    for (const oademo::IndexMarker& marker : index.markers)
        if (marker.kind == oademo::MarkerKind::chat)
            lines.push_back(marker.text);
    const std::vector<std::string> expected{
        "<Anna> hello", "<Anna> logo 6 4", "<Bob>  allied with Anna"
    };
    OA_CHECK(lines == expected);
    OA_CHECK(index.markers_omitted == 0);
}

// How the replay ended: finished by the commanders, by the end-of-game
// records, left with the game undecided, cut when the replay stopped short
// or nothing ended it, and finished or left as the recording says when none
// of those decided a replay that reached its end; and what the commanders
// decide with teams and ties.
void how_a_replay_ended() {
    // Nobody's commander is lost, nobody leaves, no scores come: cut.
    director::Timeline quiet = made_up_timeline();
    quiet.events.erase(quiet.events.begin() + 9);
    replay_index::StreamNotes none = three_players();
    oademo::TimelineIndex index = build(quiet, none);
    OA_CHECK(index.ended == oademo::IndexEnd::cut);
    OA_CHECK(index.result.winners.empty() && index.result.by == oademo::ResultBy::none);
    OA_CHECK(count_of(index, oademo::MarkerKind::commander_death) == 0);

    // Players' scores come: finished, with no winner named.
    replay_index::StreamNotes scored = three_players();
    scored.notes = {note(880, NoteKind::end_of_game, 1, 0, 1)};
    index = build(quiet, scored);
    OA_CHECK(index.ended == oademo::IndexEnd::finished);
    OA_CHECK(index.result.winners.empty());

    // A player leaves with the game undecided: left, also when the scores follow.
    replay_index::StreamNotes departed = three_players();
    departed.notes = {
        note(700, NoteKind::disconnected, 1, 2), note(880, NoteKind::end_of_game, 1, 0, 1)
    };
    index = build(quiet, departed);
    OA_CHECK(index.ended == oademo::IndexEnd::left);
    OA_CHECK(count_of(index, oademo::MarkerKind::player_left) == 1);

    // A replay that stopped short is cut, at the tick it stopped on, whatever it held.
    director::Timeline stopped = made_up_timeline();
    stopped.verdict.finished = false;
    stopped.header.last_tick = 500;
    index = build(stopped, departed);
    OA_CHECK(index.ended == oademo::IndexEnd::cut && index.ended_tick == 500);
    OA_CHECK(index.length_ticks == 500 && index.totals[0].kills.size() == 2);

    // No commander, no departure and no scores, and the file says finished:
    // finished, on the last tick. A replay that could not go on stays cut.
    replay_index::Source recorded;
    recorded.timeline = &quiet;
    recorded.stream = &none;
    recorded.recorded_end = oademo::IndexEnd::finished;
    index = replay_index::build_index(recorded);
    OA_CHECK(index.ended == oademo::IndexEnd::finished && index.ended_tick == 900);
    director::Timeline unfinished = quiet;
    unfinished.verdict.finished = false;
    recorded.timeline = &unfinished;
    index = replay_index::build_index(recorded);
    OA_CHECK(index.ended == oademo::IndexEnd::cut && index.ended_tick == 900);
    recorded.timeline = &quiet;
    recorded.recorded_end = oademo::IndexEnd::left;
    index = replay_index::build_index(recorded);
    OA_CHECK(index.ended == oademo::IndexEnd::left && index.ended_tick == 900);

    // Both commanders die on one tick: the replay is decided, with no winner.
    director::Timeline tied = made_up_timeline();
    tied.events[9].tick = 400;
    tied.events[7].damage_kind = 1;
    tied.events.erase(tied.events.begin() + 8);
    index = build(tied, none);
    OA_CHECK(index.ended == oademo::IndexEnd::finished);
    OA_CHECK(index.result.winners.empty() && index.result.by == oademo::ResultBy::none);
    OA_CHECK(count_of(index, oademo::MarkerKind::commander_death) == 2);

    // Four players in two teams: a death that leaves two teams decides
    // nothing, the one that leaves one decides, and the team wins with the
    // player whose own commander died.
    replay_index::StreamNotes teams;
    teams.players = {{1, "A", false}, {2, "B", false}, {3, "C", false}, {4, "D", false}};
    director::Timeline four;
    four.header.last_tick = 900;
    four.verdict.finished = true;
    for (uint8_t slot = 0; slot < 4; ++slot)
        four.events.push_back(
            event(0, director::EventKind::created, slot, static_cast<uint16_t>(slot + 1), true)
        );
    four.events.push_back(event(300, director::EventKind::death, 0, 1, true));
    four.events.push_back(event(500, director::EventKind::death, 2, 3, true));
    four.events.push_back(event(600, director::EventKind::death, 3, 4, true));
    teams.notes = {
        note(10, NoteKind::alliance, 1, 2, 1),
        note(10, NoteKind::alliance, 2, 1, 1),
        note(10, NoteKind::alliance, 3, 4, 1),
        note(10, NoteKind::alliance, 4, 3, 1),
    };
    replay_index::Source source;
    source.timeline = &four;
    source.stream = &teams;
    index = replay_index::build_index(source);
    OA_CHECK(index.result.winners == (std::vector<int>{1, 2}));
    OA_CHECK(index.result.by == oademo::ResultBy::commanders);
    OA_CHECK(index.ended == oademo::IndexEnd::finished);
    OA_CHECK(count_of(index, oademo::MarkerKind::alliance) == 2);
    // Without the alliances every player is a team of its own, and the
    // third death does not leave one.
    teams.notes.clear();
    index = replay_index::build_index(source);
    OA_CHECK(index.result.winners == std::vector<int>{2});
    OA_CHECK(index.ended == oademo::IndexEnd::finished);
    OA_CHECK(count_of(index, oademo::MarkerKind::alliance) == 0);
}

// A campaign mission that reached its end takes the recording player's
// result from the mission: that player won, or the others did so that
// player lost. A mission that was left, and one the replay did not finish,
// name nobody. The commanders still decide a game that is not a mission,
// including one whose commanders would have named a winner the mission then
// overrides.
void a_campaign_mission_names_its_result() {
    director::Timeline quiet = made_up_timeline();
    quiet.events.erase(quiet.events.begin() + 9);
    replay_index::StreamNotes stream = three_players();
    replay_index::Source source;
    source.timeline = &quiet;
    source.stream = &stream;
    source.recorded_end = oademo::IndexEnd::finished;
    source.mission = replay_index::Source::Mission::won;
    source.mission_player = 1;
    oademo::TimelineIndex index = replay_index::build_index(source);
    OA_CHECK(index.ended == oademo::IndexEnd::finished);
    OA_CHECK(index.result.winners == std::vector<int>{1});
    OA_CHECK(index.result.by == oademo::ResultBy::mission);

    source.mission = replay_index::Source::Mission::lost;
    index = replay_index::build_index(source);
    OA_CHECK(index.result.by == oademo::ResultBy::mission);
    OA_CHECK(index.result.winners == std::vector<int>{2});

    // The recording player is the only one who plays: someone else won, so
    // the row can still say Lost.
    replay_index::StreamNotes alone;
    alone.players = {{1, "Anna", false}};
    source.stream = &alone;
    index = replay_index::build_index(source);
    OA_CHECK(index.result.winners == std::vector<int>{0});
    OA_CHECK(index.result.by == oademo::ResultBy::mission);

    // Left, and a replay that stopped short, show nothing of the mission.
    source.stream = &stream;
    source.mission = replay_index::Source::Mission::none;
    source.recorded_end = oademo::IndexEnd::left;
    index = replay_index::build_index(source);
    OA_CHECK(index.ended == oademo::IndexEnd::left);
    OA_CHECK(index.result.winners.empty() && index.result.by == oademo::ResultBy::none);

    source.mission = replay_index::Source::Mission::won;
    source.recorded_end = oademo::IndexEnd::finished;
    director::Timeline stopped = quiet;
    stopped.verdict.finished = false;
    source.timeline = &stopped;
    index = replay_index::build_index(source);
    OA_CHECK(index.ended == oademo::IndexEnd::cut);
    OA_CHECK(index.result.winners.empty() && index.result.by == oademo::ResultBy::none);

    // A commander death would name player 1. The mission's defeat replaces it.
    source.timeline = &quiet;
    director::Timeline decided = made_up_timeline();
    source.timeline = &decided;
    source.mission = replay_index::Source::Mission::lost;
    source.mission_player = 1;
    index = replay_index::build_index(source);
    OA_CHECK(index.result.by == oademo::ResultBy::mission);
    OA_CHECK(index.result.winners == std::vector<int>{2});

    // The same commanders, with no mission, still name player 1.
    source.mission = replay_index::Source::Mission::none;
    index = replay_index::build_index(source);
    OA_CHECK(index.result.winners == std::vector<int>{1});
    OA_CHECK(index.result.by == oademo::ResultBy::commanders);
}

// A kind of marker is kept up to a limit: later markers are left out and
// counted, the start and the end stay.
void markers_are_capped() {
    const director::Timeline timeline = made_up_timeline();
    replay_index::StreamNotes stream = three_players();
    for (uint32_t at = 0; at < 1500; ++at)
        stream.notes.push_back(note(100 + at / 10, NoteKind::chat, 1, 0, 0, "<Anna> line"));
    const oademo::TimelineIndex index = build(timeline, stream);
    OA_CHECK(count_of(index, oademo::MarkerKind::chat) == oademo::most_markers_of_a_kind);
    OA_CHECK(index.markers_omitted == 500);
    OA_CHECK(count_of(index, oademo::MarkerKind::start) == 1);
    OA_CHECK(count_of(index, oademo::MarkerKind::end) == 1);
    OA_CHECK(index.markers.front().kind == oademo::MarkerKind::start);
    OA_CHECK(index.markers.back().kind == oademo::MarkerKind::end);
    // The first ones are kept.
    const auto last_chat = std::find_if(
        index.markers.rbegin(), index.markers.rend(), [](const oademo::IndexMarker& marker) {
            return marker.kind == oademo::MarkerKind::chat;
        }
    );
    OA_CHECK(last_chat != index.markers.rend() && last_chat->tick == 100 + 99);
}

// A game of no ticks, and of a start that is not tick 0, still gives a
// whole index with one sample of each player's totals.
void a_short_game_gives_an_index() {
    director::Timeline timeline;
    timeline.header.first_tick = 100;
    timeline.header.last_tick = 100;
    timeline.verdict.finished = true;
    replay_index::StreamNotes stream = three_players();
    const oademo::TimelineIndex index = build(timeline, stream);
    OA_CHECK(index.length_ticks == 100 && index.first_tick == 100);
    OA_CHECK(index.damage == std::vector<uint64_t>{0});
    OA_CHECK(index.totals.size() == 2 && index.totals[0].kills.size() == 1);
    OA_CHECK(index.markers.size() == 2);
    OA_CHECK(index.ended == oademo::IndexEnd::cut);
    OA_CHECK(oademo::parse_timeline_index(oademo::timeline_index_text(index)).has_value());
}

/// Puts a timeline event on the map. The row the picture draws is z - y / 2.
///
/// @param made the event
/// @param x across the map
/// @param y height
/// @param z down the map
void place_at(director::TimelineEvent& made, int32_t x, int32_t y, int32_t z) {
    made.at.x = x;
    made.at.y = y;
    made.at.z = z;
}

/// Adds a unit type the index can name a loss from.
///
/// @param timeline the timeline
/// @param index the type's index
/// @param mobile a unit, rather than a building
/// @param metal its metal cost
/// @param health its full health
void add_type(
    director::Timeline& timeline, uint16_t index, bool mobile, uint32_t metal, uint32_t health
) {
    director::TimelineUnitType type;
    type.type_index = index;
    type.mobile = mobile;
    type.metal_cost = metal;
    type.max_damage = health;
    timeline.header.unit_types.push_back(type);
}

/// Counts battle events of one kind.
///
/// @param index the index
/// @param kind the kind
/// @return the count
std::size_t battles_of(const oademo::TimelineIndex& index, oademo::BattleKind kind) {
    return static_cast<std::size_t>(std::count_if(
        index.events.begin(), index.events.end(), [kind](const oademo::BattleEvent& battle) {
            return battle.kind == kind;
        }
    ));
}

// Units destroyed and buildings lost are kept, with their place on the row
// the picture draws. A capture, a reclaim, a dismissal, a cancelled build
// and a commander replaced on the same tick are not. Hits in one square over
// ten seconds become one engagement when they take 2,000 health or there
// are eight of them. A commander placed or lost is kept and is not a fight.
void the_battle_is_indexed() {
    constexpr uint16_t tank = 1;
    constexpr uint16_t solar = 2;
    constexpr uint8_t weapon = 1;
    constexpr uint8_t displaced = 0;
    constexpr uint8_t captured = 4;
    constexpr uint8_t reclaimed = 5;
    constexpr uint8_t dismissed = 7;
    constexpr uint8_t cancelled = 9;
    constexpr uint8_t self_destruct = 3;
    constexpr uint8_t healing = 10;
    constexpr uint8_t paralysis = 2;

    director::Timeline timeline;
    timeline.header.last_tick = 5000;
    timeline.verdict.finished = true;
    add_type(timeline, tank, true, 90, 600);
    add_type(timeline, solar, false, 145, 500);

    director::TimelineEvent red = event(0, director::EventKind::created, 0, 1, true);
    place_at(red, 1120, 0, 3696);
    director::TimelineEvent blue = event(0, director::EventKind::created, 1, 2, true);
    place_at(blue, 8880, 0, 13248);
    timeline.events.push_back(red);
    timeline.events.push_back(blue);

    director::TimelineEvent replaced = event(400, director::EventKind::death, 0, 1, true);
    replaced.damage_kind = displaced;
    place_at(replaced, 1120, 0, 3696);
    director::TimelineEvent replacement = event(400, director::EventKind::created, 0, 1, true);
    place_at(replacement, 1120, 0, 3696);
    timeline.events.push_back(replaced);
    timeline.events.push_back(replacement);

    const auto not_a_loss = [&](uint32_t tick, uint8_t kind) {
        director::TimelineEvent death = event(tick, director::EventKind::death, 0, 9, false);
        death.damage_kind = kind;
        death.unit_type = tank;
        place_at(death, 100, 0, 100);
        timeline.events.push_back(death);
    };
    not_a_loss(500, captured);
    not_a_loss(510, reclaimed);
    not_a_loss(520, dismissed);
    not_a_loss(530, cancelled);

    director::TimelineEvent tank_lost = event(2000, director::EventKind::death, 0, 9, false);
    tank_lost.damage_kind = weapon;
    tank_lost.unit_type = tank;
    place_at(tank_lost, 4000, 20, 3000);
    timeline.events.push_back(tank_lost);

    // Each loss is its own square and stretch, so the heaviest-per-square
    // rule keeps every one of them.
    director::TimelineEvent solar_lost = event(2400, director::EventKind::death, 1, 10, false);
    solar_lost.damage_kind = weapon;
    solar_lost.unit_type = solar;
    place_at(solar_lost, 5120, 0, 3600);
    timeline.events.push_back(solar_lost);

    director::TimelineEvent unknown = event(2700, director::EventKind::death, 0, 11, false);
    unknown.damage_kind = weapon;
    unknown.unit_type = 99;
    place_at(unknown, 6144, 0, 4200);
    timeline.events.push_back(unknown);

    director::TimelineEvent wrecked = event(3300, director::EventKind::death, 0, 12, false);
    wrecked.damage_kind = self_destruct;
    wrecked.unit_type = tank;
    place_at(wrecked, 7168, 0, 4800);
    timeline.events.push_back(wrecked);

    director::TimelineEvent heavy = hit(3000, 1500, weapon);
    place_at(heavy, 600, 0, 200);
    director::TimelineEvent light = hit(3010, 500, weapon);
    place_at(light, 700, 0, 400);
    timeline.events.push_back(heavy);
    timeline.events.push_back(light);

    for (uint32_t tick = 3600; tick < 3608; ++tick) {
        director::TimelineEvent small = hit(tick, 10, weapon);
        place_at(small, 100, 0, 100);
        timeline.events.push_back(small);
    }
    for (uint32_t tick = 4200; tick < 4207; ++tick) {
        director::TimelineEvent few = hit(tick, 100, weapon);
        place_at(few, 2000, 0, 2000);
        timeline.events.push_back(few);
    }
    director::TimelineEvent healed = hit(3011, 5000, healing);
    place_at(healed, 600, 0, 200);
    director::TimelineEvent held = hit(3012, 5000, paralysis);
    place_at(held, 600, 0, 200);
    timeline.events.push_back(healed);
    timeline.events.push_back(held);

    const oademo::TimelineIndex index = build(timeline, three_players());
    OA_CHECK(index.version == oademo::timeline_version);
    OA_CHECK(battles_of(index, oademo::BattleKind::commander) == 3);
    OA_CHECK(battles_of(index, oademo::BattleKind::commander_lost) == 0);
    OA_CHECK(battles_of(index, oademo::BattleKind::unit_lost) == 2);
    OA_CHECK(battles_of(index, oademo::BattleKind::building_lost) == 2);
    OA_CHECK(battles_of(index, oademo::BattleKind::engagement) == 2);

    const oademo::BattleEvent* tank_event = nullptr;
    const oademo::BattleEvent* solar_event = nullptr;
    const oademo::BattleEvent* unknown_event = nullptr;
    const oademo::BattleEvent* heavy_event = nullptr;
    const oademo::BattleEvent* small_event = nullptr;
    for (const oademo::BattleEvent& battle : index.events) {
        if (battle.tick == 400)
            OA_CHECK(battle.kind == oademo::BattleKind::commander);
        OA_CHECK(battle.tick != 500 && battle.tick != 510);
        OA_CHECK(battle.tick != 520 && battle.tick != 530 && battle.tick != 4200);
        if (battle.tick == 2000)
            tank_event = &battle;
        if (battle.tick == 2400)
            solar_event = &battle;
        if (battle.tick == 2700)
            unknown_event = &battle;
        if (battle.tick == 3000)
            heavy_event = &battle;
        if (battle.tick == 3600)
            small_event = &battle;
    }
    OA_CHECK(tank_event != nullptr && solar_event != nullptr && unknown_event != nullptr);
    OA_CHECK(heavy_event != nullptr && small_event != nullptr);
    if (tank_event != nullptr) {
        OA_CHECK(tank_event->kind == oademo::BattleKind::unit_lost);
        OA_CHECK(tank_event->x == 4000 && tank_event->z == 2990);
        OA_CHECK(tank_event->player == 1 && tank_event->metal == 90 && tank_event->health == 600);
    }
    if (solar_event != nullptr) {
        OA_CHECK(solar_event->kind == oademo::BattleKind::building_lost);
        OA_CHECK(solar_event->x == 5120 && solar_event->z == 3600);
        OA_CHECK(solar_event->player == 2 && solar_event->metal == 145);
        OA_CHECK(solar_event->health == 500);
    }
    if (unknown_event != nullptr) {
        OA_CHECK(unknown_event->kind == oademo::BattleKind::building_lost);
        OA_CHECK(unknown_event->x == 6144 && unknown_event->z == 4200);
        OA_CHECK(unknown_event->metal == 0 && unknown_event->health == 0);
    }
    if (heavy_event != nullptr) {
        OA_CHECK(heavy_event->kind == oademo::BattleKind::engagement);
        OA_CHECK(heavy_event->x == 625 && heavy_event->z == 250 && heavy_event->amount == 2000);
    }
    if (small_event != nullptr) {
        OA_CHECK(small_event->kind == oademo::BattleKind::engagement);
        OA_CHECK(small_event->x == 100 && small_event->z == 100 && small_event->amount == 80);
    }
}

// More fights than the file holds are spread across the game. Fights in one
// square and stretch keep only the heaviest. Commanders are kept, and when
// they alone fill the file the fights are left out.
void the_battle_is_spread() {
    constexpr uint16_t tank = 1;
    constexpr uint16_t heavy = 2;
    director::Timeline crowded;
    crowded.header.last_tick = 700000;
    crowded.verdict.finished = true;
    add_type(crowded, tank, true, 10, 10);
    crowded.events.push_back(event(0, director::EventKind::created, 0, 1, true));
    crowded.events.push_back(event(0, director::EventKind::created, 1, 2, true));
    constexpr uint32_t fights = 2000;
    for (uint32_t index = 0; index < fights; ++index) {
        director::TimelineEvent death = event(index * 300, director::EventKind::death, 0, 3, false);
        death.damage_kind = 1;
        death.unit_type = tank;
        place_at(death, static_cast<int32_t>(index) * 512, 0, 50);
        crowded.events.push_back(death);
    }
    const oademo::TimelineIndex spread = build(crowded, three_players());
    OA_CHECK(spread.events.size() == oademo::most_battle_events);
    OA_CHECK(battles_of(spread, oademo::BattleKind::commander) == 2);
    const std::size_t room = oademo::most_battle_events - 2;
    OA_CHECK(battles_of(spread, oademo::BattleKind::unit_lost) == room);
    bool early = false;
    uint32_t last_tick = 0;
    for (const oademo::BattleEvent& battle : spread.events) {
        if (battle.kind != oademo::BattleKind::unit_lost)
            continue;
        if (battle.tick == 0)
            early = true;
        last_tick = battle.tick;
    }
    const uint32_t expected_last = static_cast<uint32_t>((room - 1) * fights / room) * 300U;
    OA_CHECK(early && last_tick == expected_last);
    OA_CHECK(last_tick < (fights - 1) * 300U);

    director::Timeline piled;
    piled.header.last_tick = 1000;
    piled.verdict.finished = true;
    add_type(piled, tank, true, 10, 10);
    add_type(piled, heavy, true, 500, 10);
    piled.events.push_back(event(0, director::EventKind::created, 0, 1, true));
    for (uint32_t index = 0; index < 8; ++index) {
        director::TimelineEvent death = event(20 + index, director::EventKind::death, 0, 3, false);
        death.damage_kind = 1;
        death.unit_type = tank;
        place_at(death, 100, 0, 100);
        piled.events.push_back(death);
    }
    director::TimelineEvent costly = event(40, director::EventKind::death, 0, 4, false);
    costly.damage_kind = 1;
    costly.unit_type = heavy;
    place_at(costly, 120, 0, 110);
    piled.events.push_back(costly);
    const oademo::TimelineIndex one_cell = build(piled, three_players());
    OA_CHECK(battles_of(one_cell, oademo::BattleKind::unit_lost) == 1);
    bool kept_heavy = false;
    for (const oademo::BattleEvent& battle : one_cell.events) {
        if (battle.kind != oademo::BattleKind::unit_lost)
            continue;
        kept_heavy = battle.tick == 40 && battle.metal == 500;
    }
    OA_CHECK(kept_heavy);

    director::Timeline commanders;
    commanders.header.last_tick = 1000000;
    commanders.verdict.finished = true;
    add_type(commanders, tank, true, 10, 10);
    constexpr uint32_t many = 4100;
    for (uint32_t index = 0; index < many; ++index)
        commanders.events.push_back(
            event(index, director::EventKind::created, static_cast<uint8_t>(index % 2), 1, true)
        );
    director::TimelineEvent late = event(1000000, director::EventKind::death, 0, 3, false);
    late.damage_kind = 1;
    late.unit_type = tank;
    place_at(late, 1, 0, 1);
    commanders.events.push_back(late);
    const oademo::TimelineIndex only = build(commanders, three_players());
    OA_CHECK(only.events.size() == oademo::most_battle_events);
    OA_CHECK(battles_of(only, oademo::BattleKind::commander) == oademo::most_battle_events);
    OA_CHECK(battles_of(only, oademo::BattleKind::unit_lost) == 0);
    const uint32_t last_commander =
        static_cast<uint32_t>((oademo::most_battle_events - 1) * many / oademo::most_battle_events);
    OA_CHECK(only.events.front().tick == 0 && only.events.back().tick == last_commander);
    OA_CHECK(only.events.back().kind == oademo::BattleKind::commander);
}

// A commander displaced from its slot leaves out that creation as well as
// the loss. The replacement is kept. The same unit created again on that
// tick keeps the later creation.
void a_displaced_commander_creation_is_left_out() {
    constexpr uint8_t displaced = 0;
    director::Timeline timeline;
    timeline.header.last_tick = 10;
    timeline.verdict.finished = true;
    director::TimelineEvent first = event(1, director::EventKind::created, 0, 5, true);
    place_at(first, 100, 0, 200);
    director::TimelineEvent death = event(1, director::EventKind::death, 0, 5, true);
    death.damage_kind = displaced;
    place_at(death, 100, 0, 200);
    director::TimelineEvent replacement = event(1, director::EventKind::created, 0, 6, true);
    place_at(replacement, 100, 0, 200);
    timeline.events.push_back(first);
    timeline.events.push_back(death);
    timeline.events.push_back(replacement);
    const oademo::TimelineIndex other_unit = build(timeline, three_players());
    OA_CHECK(battles_of(other_unit, oademo::BattleKind::commander) == 1);
    OA_CHECK(battles_of(other_unit, oademo::BattleKind::commander_lost) == 0);
    if (!other_unit.events.empty())
        OA_CHECK(other_unit.events.front().kind == oademo::BattleKind::commander);

    director::Timeline same;
    same.header.last_tick = 10;
    same.verdict.finished = true;
    director::TimelineEvent earlier = event(1, director::EventKind::created, 0, 5, true);
    place_at(earlier, 100, 0, 200);
    director::TimelineEvent lost = event(1, director::EventKind::death, 0, 5, true);
    lost.damage_kind = displaced;
    place_at(lost, 100, 0, 200);
    director::TimelineEvent later = event(1, director::EventKind::created, 0, 5, true);
    place_at(later, 100, 0, 200);
    same.events.push_back(earlier);
    same.events.push_back(lost);
    same.events.push_back(later);
    const oademo::TimelineIndex reused = build(same, three_players());
    OA_CHECK(battles_of(reused, oademo::BattleKind::commander) == 1);
    OA_CHECK(battles_of(reused, oademo::BattleKind::commander_lost) == 0);
}

/// Aims the camera at a few events.
///
/// @param events the events
/// @param tick the moment on show
/// @return where it looks
replay_index::BattleAim aim_of(std::initializer_list<oademo::BattleEvent> events, uint32_t tick) {
    const std::vector<oademo::BattleEvent> held(events);
    return replay_index::battle_aim(held, tick);
}

/// One fight, for the camera test.
///
/// @param tick its tick
/// @param kind its kind
/// @param x across the map
/// @param z down the map
/// @return the event
oademo::BattleEvent fight_at(uint32_t tick, oademo::BattleKind kind, int32_t x, int32_t z) {
    oademo::BattleEvent made;
    made.tick = tick;
    made.kind = kind;
    made.x = x;
    made.z = z;
    return made;
}

// The camera looks at the heaviest fight in the next eight seconds, then at
// one in the ten seconds just passed. A commander placed or lost is not a
// fight, so two start bases do not hold it while the battle is elsewhere.
void the_camera_aims_at_the_fight() {
    using oademo::BattleKind;
    oademo::BattleEvent arm = fight_at(0, BattleKind::commander, 1120, 3696);
    oademo::BattleEvent core = fight_at(0, BattleKind::commander, 8880, 13248);
    oademo::BattleEvent loss = fight_at(23450, BattleKind::unit_lost, 4000, 8000);
    loss.metal = 200;
    oademo::BattleEvent building = fight_at(23480, BattleKind::building_lost, 4100, 8100);
    building.metal = 50;
    const std::vector<oademo::BattleEvent> battle{arm, core, loss, building};
    const replay_index::BattleAim aimed = replay_index::battle_aim(battle, 23400);
    OA_CHECK(aimed.found && aimed.tick == 23450 && aimed.x == 4000 && aimed.z == 8000);
    const replay_index::BattleAim bases = aim_of({arm, core}, 23400);
    OA_CHECK(!bases.found);

    oademo::BattleEvent cluster = fight_at(23410, BattleKind::engagement, 6000, 1000);
    cluster.amount = 3000;
    oademo::BattleEvent lighter = fight_at(23400, BattleKind::unit_lost, 100, 100);
    lighter.metal = 2500;
    const replay_index::BattleAim heavier = aim_of({lighter, cluster}, 23400);
    OA_CHECK(heavier.found && heavier.x == 6000 && heavier.z == 1000);

    oademo::BattleEvent early = fight_at(100, BattleKind::unit_lost, 20, 30);
    early.metal = 100;
    oademo::BattleEvent later = fight_at(150, BattleKind::unit_lost, 10, 10);
    later.metal = 100;
    const replay_index::BattleAim sooner = aim_of({later, early}, 0);
    OA_CHECK(sooner.found && sooner.tick == 100);

    oademo::BattleEvent right = fight_at(100, BattleKind::unit_lost, 40, 10);
    right.metal = 100;
    oademo::BattleEvent left = fight_at(100, BattleKind::unit_lost, 20, 80);
    left.metal = 100;
    const replay_index::BattleAim west = aim_of({right, left}, 0);
    OA_CHECK(west.found && west.x == 20);
    oademo::BattleEvent lower = fight_at(100, BattleKind::unit_lost, 20, 5);
    lower.metal = 100;
    oademo::BattleEvent upper = fight_at(100, BattleKind::unit_lost, 20, 90);
    upper.metal = 100;
    const replay_index::BattleAim north = aim_of({upper, lower}, 0);
    OA_CHECK(north.found && north.z == 5);

    oademo::BattleEvent passed = fight_at(23200, BattleKind::unit_lost, 3000, 4000);
    passed.metal = 80;
    const replay_index::BattleAim trail = aim_of({passed}, 23400);
    OA_CHECK(trail.found && trail.tick == 23200 && trail.x == 3000);
    // With nothing ahead, the most recent fight behind wins, not the heaviest.
    oademo::BattleEvent older_heavy = fight_at(23120, BattleKind::engagement, 1000, 1000);
    older_heavy.amount = 99999;
    oademo::BattleEvent newer_light = fight_at(23300, BattleKind::unit_lost, 5000, 5000);
    newer_light.metal = 1;
    const replay_index::BattleAim latest = aim_of({older_heavy, newer_light}, 23400);
    OA_CHECK(latest.found && latest.tick == 23300 && latest.x == 5000);

    oademo::BattleEvent edge = fight_at(1240, BattleKind::engagement, 8, 9);
    edge.amount = 2000;
    OA_CHECK(aim_of({edge}, 1000).found);
    // Just past the eight seconds, still inside the quiet minute.
    oademo::BattleEvent beyond = fight_at(1241, BattleKind::engagement, 8, 9);
    beyond.amount = 2000;
    OA_CHECK(aim_of({beyond}, 1000).found && aim_of({beyond}, 1000).tick == 1241);
    oademo::BattleEvent minute = fight_at(2800, BattleKind::engagement, 8, 9);
    minute.amount = 2000;
    OA_CHECK(aim_of({minute}, 1000).found);
    oademo::BattleEvent past_minute = fight_at(2801, BattleKind::engagement, 8, 9);
    past_minute.amount = 2000;
    OA_CHECK(!aim_of({past_minute}, 1000).found);

    oademo::BattleEvent just = fight_at(700, BattleKind::unit_lost, 1, 2);
    just.health = 10;
    OA_CHECK(aim_of({just}, 1000).found);
    // Just past the ten seconds, still inside the quiet minute.
    oademo::BattleEvent older = fight_at(699, BattleKind::unit_lost, 1, 2);
    older.health = 10;
    OA_CHECK(aim_of({older}, 1000).found && aim_of({older}, 1000).tick == 699);
    oademo::BattleEvent minute_back = fight_at(200, BattleKind::unit_lost, 1, 2);
    minute_back.health = 10;
    OA_CHECK(aim_of({minute_back}, 2000).found);
    oademo::BattleEvent older_minute = fight_at(199, BattleKind::unit_lost, 1, 2);
    older_minute.health = 10;
    OA_CHECK(!aim_of({older_minute}, 2000).found);

    // A quiet minute keeps the nearer fight. An equal gap keeps the one ahead.
    // Recording 884 at 13:00 is this: the battle is not at either start base.
    oademo::BattleEvent recent = fight_at(22569, BattleKind::unit_lost, 8081, 8829);
    recent.metal = 100;
    oademo::BattleEvent coming = fight_at(24691, BattleKind::engagement, 2104, 11726);
    coming.amount = 172;
    const replay_index::BattleAim lull = aim_of({arm, core, recent, coming}, 23400);
    OA_CHECK(lull.found && lull.tick == 22569 && lull.x == 8081 && lull.z == 8829);
    const replay_index::BattleAim only_coming = aim_of({arm, core, coming}, 23400);
    OA_CHECK(only_coming.found && only_coming.tick == 24691 && only_coming.x == 2104);
    oademo::BattleEvent soon = fight_at(1600, BattleKind::unit_lost, 3, 4);
    soon.metal = 1;
    oademo::BattleEvent as_far = fight_at(400, BattleKind::engagement, 7, 7);
    as_far.amount = 99999;
    const replay_index::BattleAim equal_gap = aim_of({as_far, soon}, 1000);
    OA_CHECK(equal_gap.found && equal_gap.tick == 1600);

    oademo::BattleEvent now = fight_at(1000, BattleKind::unit_lost, 4, 5);
    now.metal = 1;
    oademo::BattleEvent behind = fight_at(900, BattleKind::engagement, 9, 9);
    behind.amount = 99999;
    const replay_index::BattleAim ahead = aim_of({behind, now}, 1000);
    OA_CHECK(ahead.found && ahead.tick == 1000 && ahead.x == 4);
    OA_CHECK(!aim_of({}, 1000).found);
}

/// One step of Director.
///
/// @param events the fights
/// @param tick the moment
/// @param since real time since the last change, in milliseconds
/// @param previous the view so far
/// @return the next view
replay_index::DirectorView step_director(
    std::initializer_list<oademo::BattleEvent> events,
    uint32_t tick,
    uint32_t since,
    const replay_index::DirectorView& previous
) {
    const std::vector<oademo::BattleEvent> held(events);
    return replay_index::director_view(held, tick, since, previous, 1088, 624);
}

// Two fights a map apart that take turns being the heavier one do not pull
// the camera back and forth. A view is held for four seconds of the battle
// and for a second and a half of real time, and a place just left is not
// taken again inside ten seconds. A nearby fight is panned to, slowly; a
// far one is a cut.
void the_director_stays_with_one_fight() {
    using oademo::BattleKind;
    constexpr int32_t width = 1088;
    constexpr int32_t west = 0;
    constexpr int32_t east = 20000;
    std::vector<oademo::BattleEvent> events;
    for (uint32_t index = 0; index < 40; ++index) {
        oademo::BattleEvent made =
            fight_at(index * 60, BattleKind::unit_lost, index % 2 == 0 ? west : east, 0);
        made.metal = 50U * (index + 1);
        events.push_back(made);
    }
    replay_index::DirectorView view;
    int cuts = 0;
    int shorts = 0;
    int returns = 0;
    int32_t fastest = 0;
    int32_t previous_x = 0;
    int32_t previous_z = 0;
    bool have_previous = false;
    int32_t left_x = 0;
    uint32_t left_tick = 0;
    bool have_left = false;
    uint32_t last_cut = 0;
    bool had_cut = false;
    for (uint32_t tick = 0; tick <= 900; ++tick) {
        uint32_t since = 0;
        if (view.has_subject && tick >= view.cut_tick)
            since = (tick - view.cut_tick) * 1000U / 30U;
        const replay_index::DirectorView next =
            replay_index::director_view(events, tick, since, view, width, 624);
        if (have_previous) {
            const int64_t across = static_cast<int64_t>(next.x) - previous_x;
            const int64_t down = static_cast<int64_t>(next.z) - previous_z;
            const int64_t step = across * across + down * down;
            if (step > 64 * 64) {
                ++cuts;
                if (had_cut && tick - last_cut < replay_index::director_min_cut_ticks)
                    ++shorts;
                const int64_t back = static_cast<int64_t>(next.x) - left_x;
                if (have_left && tick - left_tick <= replay_index::director_return_ticks &&
                    back * back <= 64 * 64)
                    ++returns;
                left_x = previous_x;
                left_tick = tick;
                have_left = true;
                last_cut = tick;
                had_cut = true;
            } else if (step > fastest)
                fastest = static_cast<int32_t>(step);
        }
        previous_x = next.x;
        previous_z = next.z;
        have_previous = true;
        view = next;
    }
    OA_CHECK(shorts == 0 && returns == 0 && cuts == 0);
    // fastest holds a squared step. One screen a second is width/30 pixels.
    const int32_t screen_per_tick = width / 30;
    OA_CHECK(fastest <= screen_per_tick * screen_per_tick);

    oademo::BattleEvent home = fight_at(0, BattleKind::unit_lost, 0, 0);
    home.metal = 100;
    oademo::BattleEvent far = fight_at(200, BattleKind::unit_lost, east, 0);
    far.metal = 10000;
    const replay_index::DirectorView opened = step_director({home}, 0, 0, {});
    OA_CHECK(opened.has_subject && opened.x == 0 && opened.changed);
    const replay_index::DirectorView waiting = step_director({home, far}, 200, 0, opened);
    OA_CHECK(!waiting.changed && waiting.subject_x == 0);
    const replay_index::DirectorView jumped =
        step_director({home, far}, 200, replay_index::director_hold_ms, opened);
    OA_CHECK(jumped.changed && jumped.cut && jumped.x == east);

    oademo::BattleEvent near = fight_at(200, BattleKind::unit_lost, 400, 0);
    near.metal = 10000;
    const replay_index::DirectorView panned =
        step_director({home, near}, 200, replay_index::director_hold_ms, opened);
    OA_CHECK(panned.changed && !panned.cut && panned.x == 0 && panned.subject_x == 400);
    replay_index::DirectorView along = panned;
    int32_t largest = 0;
    int32_t shown = panned.x;
    for (uint32_t since = 33; since <= 4000; since += 33) {
        along = replay_index::director_view(
            std::vector<oademo::BattleEvent>{home, near}, 201, since, panned, width, 624
        );
        const int32_t moved = along.x - shown;
        if (moved > largest)
            largest = moved;
        shown = along.x;
        OA_CHECK(!along.changed);
    }
    OA_CHECK(along.x == 400);
    OA_CHECK(largest <= screen_per_tick + 1);

    // A later fight a short way off is the same subject. The picture eases
    // there instead of jumping the whole way in one tick.
    oademo::BattleEvent slide = fight_at(400, BattleKind::unit_lost, 50, 0);
    slide.metal = 100;
    const replay_index::DirectorView settled = step_director({home}, 400, 5000, opened);
    OA_CHECK(settled.x == 0 && !settled.changed);
    replay_index::DirectorView sliding = settled;
    int32_t slide_largest = 0;
    int32_t slide_x = settled.x;
    const std::vector<oademo::BattleEvent> slide_events{home, slide};
    for (uint32_t tick = 400; tick <= 430; ++tick) {
        const uint32_t since = 5000U + (tick - 400U) * 1000U / 30U;
        const replay_index::DirectorView next =
            replay_index::director_view(slide_events, tick, since, sliding, width, 624);
        const int32_t moved = next.x > slide_x ? next.x - slide_x : slide_x - next.x;
        if (moved > slide_largest)
            slide_largest = moved;
        slide_x = next.x;
        sliding = next;
        OA_CHECK(!next.changed && !next.cut);
    }
    OA_CHECK(sliding.x == 50);
    OA_CHECK(slide_largest <= screen_per_tick + 1);
}

// A heavy fight more than ten seconds old no longer holds the picture: a
// lighter fight elsewhere takes it once the holds have passed. Inside those
// ten seconds the heavy fight holds it.
void an_old_fight_gives_way() {
    using oademo::BattleKind;
    constexpr int32_t east = 20000;
    oademo::BattleEvent heavy = fight_at(0, BattleKind::engagement, 0, 0);
    heavy.amount = 20000;
    const replay_index::DirectorView opened = step_director({heavy}, 0, 0, {});
    OA_CHECK(opened.has_subject && opened.subject_x == 0);

    oademo::BattleEvent soon = fight_at(400, BattleKind::unit_lost, east, 0);
    soon.metal = 50;
    const replay_index::DirectorView held = step_director({heavy, soon}, 250, 5000, opened);
    OA_CHECK(!held.changed && held.subject_x == 0);

    oademo::BattleEvent later = fight_at(1100, BattleKind::unit_lost, east, 0);
    later.metal = 50;
    const replay_index::DirectorView moved = step_director({heavy, later}, 1000, 5000, opened);
    OA_CHECK(moved.changed && moved.cut && moved.x == east);
    const replay_index::DirectorView too_soon = step_director({heavy, later}, 1000, 100, opened);
    OA_CHECK(!too_soon.changed && too_soon.subject_x == 0);
}

// Before the first fight the picture shows a start base: the first
// commander, the lowest player number on the same tick. A commander lost is
// not one, and an index with no commander gives none.
void the_opening_shows_a_start() {
    using oademo::BattleKind;
    oademo::BattleEvent fourth = fight_at(1, BattleKind::commander, 1120, 3653);
    fourth.player = 4;
    oademo::BattleEvent third = fight_at(2, BattleKind::commander, 8880, 13229);
    third.player = 3;
    oademo::BattleEvent second = fight_at(1, BattleKind::commander, 500, 600);
    second.player = 2;
    oademo::BattleEvent lost = fight_at(0, BattleKind::commander_lost, 9, 9);
    lost.player = 1;
    const std::vector<oademo::BattleEvent> starts{fourth, third, second, lost};
    const replay_index::BattleAim opening = replay_index::director_opening(starts);
    OA_CHECK(opening.found && opening.x == 500 && opening.z == 600 && opening.tick == 1);
    const std::vector<oademo::BattleEvent> none{lost, fight_at(5, BattleKind::unit_lost, 1, 1)};
    OA_CHECK(!replay_index::director_opening(none).found);
}

} // namespace

int main() {
    notes_are_read_from_a_recording();
    unreadable_files_are_refused();
    an_index_is_built();
    chat_markers_are_lines_a_player_typed();
    how_a_replay_ended();
    a_campaign_mission_names_its_result();
    markers_are_capped();
    a_short_game_gives_an_index();
    the_battle_is_indexed();
    the_battle_is_spread();
    a_displaced_commander_creation_is_left_out();
    the_camera_aims_at_the_fight();
    the_director_stays_with_one_fight();
    an_old_fight_gives_way();
    the_opening_shows_a_start();
    return oa::test::check_exit_status();
}

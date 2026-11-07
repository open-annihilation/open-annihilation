// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The index pass's two parts that need no match: the notes read from a
// made-up recording of every kind of record (also inside a .oademo, with the
// refusals), and the index built from a made-up timeline and notes: the
// totals, the damage, the markers and how they are ordered and capped, the
// commanders' result and how the replay ended.

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
// or nothing ended it; and what the commanders decide with teams and ties.
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

} // namespace

int main() {
    notes_are_read_from_a_recording();
    unreadable_files_are_refused();
    an_index_is_built();
    chat_markers_are_lines_a_player_typed();
    how_a_replay_ended();
    markers_are_capped();
    a_short_game_gives_an_index();
    return oa::test::check_exit_status();
}

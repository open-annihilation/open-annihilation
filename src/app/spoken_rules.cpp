// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The spoken-update rules (spoken_rules.hpp).
#include "oa/app/spoken_rules.hpp"

#include "oa/data/languages/interface_text.hpp"

#include <initializer_list>
#include <string>
#include <utility>
#include <vector>

namespace oa::app::spoken {
namespace {

namespace bridge = oa::platform::screen_reader;

/// Replaces every `{token}` in a pattern.
///
/// @param pattern the looked-up pattern
/// @param token the place name, without braces
/// @param value the text that replaces it
/// @return the filled pattern
std::string fill_one(std::string pattern, std::string_view token, std::string_view value) {
    const std::string place = std::string("{") + std::string(token) + "}";
    std::string::size_type at = 0;
    while ((at = pattern.find(place, at)) != std::string::npos) {
        pattern.replace(at, place.size(), value);
        at += value.size();
    }
    return pattern;
}

/// The interface's words for an English sentence.
///
/// @param english the sentence as the source writes it
/// @return the sentence in the player's language, or the English
std::string say(const char* english) {
    return std::string(oa::data::languages::interface_text(english));
}

/// Looks up a pattern and fills its places.
///
/// @param english the pattern
/// @param places each place name and the text that replaces it
/// @return the filled sentence
std::string say_filled(
    const char* english, std::initializer_list<std::pair<std::string_view, std::string_view>> places
) {
    std::string pattern = say(english);
    for (const auto& place : places)
        pattern = fill_one(std::move(pattern), place.first, place.second);
    return pattern;
}

/// One line offered to the queue.
///
/// @param text the sentence
/// @param politeness how soon it is spoken
/// @param collapse true when a burst may replace it
/// @param room the room a collapsible line belongs to
/// @param speaker the player, empty when the line has none
/// @return the offer
bridge::SpokenOffer offer_of(
    std::string text,
    bridge::Politeness politeness,
    bool collapse,
    std::string room,
    std::string speaker
) {
    bridge::SpokenOffer offer;
    offer.text = std::move(text);
    offer.politeness = politeness;
    offer.collapsible = collapse;
    offer.room = std::move(room);
    offer.speaker = std::move(speaker);
    return offer;
}

/// The words for one of the ten player colours.
///
/// @param colour the colour number
/// @return the name, or empty when the number is not one of the ten
const char* colour_word(uint8_t colour) {
    static constexpr const char* kNames[] = {
        "Blue",
        "Red",
        "White",
        "Green",
        "Navy",
        "Purple",
        "Yellow",
        "Black",
        "Light blue",
        "Tan",
    };
    if (colour >= 10)
        return "";
    return kNames[colour];
}

/// The team as the room shows it. Team 0 is "1". No team is "none".
///
/// @param team the seat's team
/// @return the words
std::string team_words(uint8_t team) {
    if (team == kNoTeam)
        return say("none");
    return std::to_string(static_cast<unsigned>(team) + 1U);
}

/// Reads `[Entered 'Room']`.
///
/// @param text the line
/// @param[out] room the room name
/// @return true when the line is that sentence
bool entered_room(std::string_view text, std::string& room) {
    constexpr std::string_view kOpen = "[Entered '";
    constexpr std::string_view kClose = "']";
    if (!text.starts_with(kOpen) || !text.ends_with(kClose) ||
        text.size() < kOpen.size() + kClose.size())
        return false;
    room.assign(text.substr(kOpen.size(), text.size() - kOpen.size() - kClose.size()));
    return true;
}

/// Reads `<Page from Name> text`.
///
/// @param text the line
/// @return true when the line is a page
bool page_line(std::string_view text) {
    constexpr std::string_view kOpen = "<Page from ";
    if (!text.starts_with(kOpen))
        return false;
    const std::size_t end = text.find('>');
    return end != std::string_view::npos && end >= kOpen.size();
}

/// Whether this speaker is the local player.
///
/// @param speaker the line's speaker
/// @param local_name the name the session note carried
/// @return true when both are the same non-empty name
bool is_self(std::string_view speaker, std::string_view local_name) {
    return !speaker.empty() && speaker == local_name;
}

/// Reads `Name: text`, including a tagged name such as `Name [SysOp] : text`.
///
/// @param text the line
/// @param[out] name the speaker
/// @param[out] body the message
/// @return true when the line is ordinary chat
bool said_line(std::string_view text, std::string& name, std::string& body) {
    if (text.empty() || text.front() == '[' || text.front() == '<')
        return false;
    const std::size_t colon = text.find(": ");
    if (colon == std::string_view::npos || colon == 0)
        return false;
    name.assign(text.substr(0, colon));
    const std::size_t tag = name.find(" [");
    if (tag != std::string::npos)
        name.resize(tag);
    body.assign(text.substr(colon + 2));
    return !name.empty();
}

/// Reads `<Name> text` or `<Name->To> text`.
///
/// @param text the line
/// @param[out] name the speaker
/// @param[out] body the message
/// @param[out] directed true when the line names one recipient
/// @return true when the line is that chat
bool bracket_line(std::string_view text, std::string& name, std::string& body, bool& directed) {
    constexpr std::string_view kOpen = "<";
    constexpr std::string_view kClose = "> ";
    if (!text.starts_with(kOpen))
        return false;
    const std::size_t end = text.find(kClose);
    if (end == std::string_view::npos || end <= kOpen.size())
        return false;
    std::string_view from = text.substr(kOpen.size(), end - kOpen.size());
    directed = false;
    const std::size_t arrow = from.find("->");
    if (arrow != std::string_view::npos) {
        directed = true;
        from = from.substr(0, arrow);
    }
    name.assign(from);
    body.assign(text.substr(end + kClose.size()));
    return !name.empty();
}

/// Remembers the local name from a session note. The sentence itself is the
/// internal line, so this note is not spoken.
///
/// @param[in,out] memory what was heard before
/// @param text `level name`
void remember_session(Memory& memory, std::string_view text) {
    std::size_t at = 0;
    if (text.empty() || text.front() < '0' || text.front() > '9')
        return;
    for (; at < text.size() && text[at] >= '0' && text[at] <= '9'; ++at) {
    }
    if (at < text.size() && text[at] == ' ' && at + 1 < text.size())
        memory.local_name.assign(text.substr(at + 1));
}

/// Queues one internal line. `S` is a login and is said once. `G` is a page,
/// said at once unless speak-chat is `off`. `W` is a whisper, said only
/// under `all`. `C` may collapse. While the member list is still settling,
/// a `C` row is that list and is not spoken; it restarts the quiet clock.
/// The local player's own join or leave is not spoken.
///
/// @param[out] spoken the lines so far
/// @param[in,out] memory what was heard before
/// @param hear which lines may be spoken
/// @param text the note, with its prefix
/// @param now_ms the caller's clock, in milliseconds
void take_internal(
    std::vector<bridge::SpokenOffer>& spoken,
    Memory& memory,
    const Hear& hear,
    std::string_view text,
    uint64_t now_ms
) {
    if (text.size() < 2 || text[1] != '\t')
        return;
    const char prefix = text.front();
    const std::string_view rest = text.substr(2);
    if (prefix == 'S') {
        if (!hear.speaking || rest.empty() || rest == memory.session_said)
            return;
        memory.session_said.assign(rest);
        spoken.push_back(offer_of(std::string(rest), bridge::Politeness::polite, false, {}, {}));
        return;
    }
    if (prefix == 'P') {
        if (!hear.speaking || rest.empty())
            return;
        spoken.push_back(offer_of(std::string(rest), bridge::Politeness::polite, false, {}, {}));
        return;
    }
    if (prefix == 'A') {
        if (!hear.speaking || rest.empty())
            return;
        spoken.push_back(offer_of(std::string(rest), bridge::Politeness::assertive, false, {}, {}));
        return;
    }
    if (prefix == 'G') {
        if (!hear.speaking || rest.empty() || hear.chat == bridge::SpeakChat::off)
            return;
        spoken.push_back(offer_of(std::string(rest), bridge::Politeness::assertive, false, {}, {}));
        return;
    }
    if (prefix == 'W') {
        if (!hear.speaking || rest.empty() || hear.chat != bridge::SpeakChat::all)
            return;
        spoken.push_back(offer_of(std::string(rest), bridge::Politeness::polite, false, {}, {}));
        return;
    }
    if (prefix != 'C')
        return;
    const std::size_t room_end = rest.find('\t');
    if (room_end == std::string_view::npos)
        return;
    const std::size_t speaker_end = rest.find('\t', room_end + 1);
    if (speaker_end == std::string_view::npos)
        return;
    const std::string room(rest.substr(0, room_end));
    const std::string speaker(rest.substr(room_end + 1, speaker_end - room_end - 1));
    const std::string sentence(rest.substr(speaker_end + 1));
    if (memory.listing) {
        memory.last_row_ms = now_ms;
        return;
    }
    if (sentence.empty() || is_self(speaker, memory.local_name))
        return;
    if (!hear.speaking || hear.chat != bridge::SpeakChat::all)
        return;
    spoken.push_back(offer_of(sentence, bridge::Politeness::polite, true, room, speaker));
}

/// Stores a seat without announcing it.
///
/// @param[out] stored the remembered seat
/// @param now the seat this frame
void store_seat(SeatWas& stored, const SeatNow& now) {
    stored.taken = now.taken;
    stored.has_details = now.has_details;
    stored.player_id = now.player_id;
    stored.name = now.name;
    stored.ready = now.ready;
    stored.side = now.side;
    stored.color = now.color;
    stored.team = now.team;
    stored.self = now.self;
}

/// Announces the map once, and seeds or diffs the host's options.
///
/// @param[out] spoken the lines so far
/// @param[in,out] memory what was heard before
/// @param hear which lines may be spoken
/// @param room the room this frame
void take_host(
    std::vector<bridge::SpokenOffer>& spoken, Memory& memory, const Hear& hear, const RoomNow& room
) {
    if (!room.host_present || room.host_slot >= room.seats.size() ||
        !room.seats[room.host_slot].taken) {
        return;
    }
    memory.host_known = true;
    memory.host_id = room.seats[room.host_slot].player_id;
    if (!room.host_has_details)
        return;
    const bool lobby_speaks = hear.speaking && hear.lobby;
    // The seated count, START and the host's own ready and watching share the
    // word with the options; a change there is not an option.
    const auto options = static_cast<uint16_t>(room.option_bits & kRoomOptionBits);
    if (!memory.host_details) {
        memory.host_details = true;
        memory.map = room.map;
        memory.option_bits = options;
        memory.metal = room.metal;
        memory.energy = room.energy;
        memory.units = room.units;
        if (lobby_speaks && !room.map.empty()) {
            spoken.push_back(offer_of(
                say_filled("The map is {name}", {{"name", room.map}}),
                bridge::Politeness::polite,
                false,
                {},
                {}
            ));
        }
        return;
    }
    if (room.map != memory.map) {
        memory.map = room.map;
        if (lobby_speaks && !room.map.empty()) {
            spoken.push_back(offer_of(
                say_filled("The map is {name}", {{"name", room.map}}),
                bridge::Politeness::polite,
                false,
                {},
                {}
            ));
        }
    }
    if (options != memory.option_bits || room.metal != memory.metal ||
        room.energy != memory.energy || room.units != memory.units) {
        memory.option_bits = options;
        memory.metal = room.metal;
        memory.energy = room.energy;
        memory.units = room.units;
        if (lobby_speaks) {
            spoken.push_back(
                offer_of(say("An option changed"), bridge::Politeness::polite, false, {}, {})
            );
        }
    }
}

/// Announces one seat's join, leave and changes. The first info is stored
/// and is not a change. The local player is stored and not spoken. A join
/// and a leave are said at once, so they come before that player's changes.
///
/// @param[out] spoken the lines so far
/// @param[in,out] stored the remembered seat
/// @param now the seat this frame
/// @param hear which lines may be spoken
/// @param[in,out] memory what was heard before
void take_seat(
    std::vector<bridge::SpokenOffer>& spoken,
    SeatWas& stored,
    const SeatNow& now,
    const Hear& hear,
    Memory& memory
) {
    const bool lobby_speaks = hear.speaking && hear.lobby;
    if (stored.taken && (!now.taken || stored.player_id != now.player_id)) {
        const bool host_seat = memory.host_known && stored.player_id == memory.host_id;
        if (host_seat)
            memory.host_known = false;
        if (lobby_speaks && !stored.self) {
            if (host_seat) {
                spoken.push_back(
                    offer_of(say("The host has left"), bridge::Politeness::polite, false, {}, {})
                );
            }
            spoken.push_back(offer_of(
                say_filled("{name} left", {{"name", stored.name}}),
                bridge::Politeness::polite,
                false,
                {},
                stored.name
            ));
        }
        stored = {};
    }
    if (!now.taken) {
        stored = {};
        return;
    }
    if (!stored.taken) {
        store_seat(stored, now);
        if (lobby_speaks && !now.self) {
            spoken.push_back(offer_of(
                say_filled("{name} joined", {{"name", now.name}}),
                bridge::Politeness::polite,
                false,
                {},
                now.name
            ));
        }
        return;
    }
    stored.name = now.name;
    stored.self = now.self;
    stored.player_id = now.player_id;
    if (!stored.has_details && now.has_details) {
        stored.has_details = true;
        stored.ready = now.ready;
        stored.side = now.side;
        stored.color = now.color;
        stored.team = now.team;
        return;
    }
    if (!stored.has_details || !now.has_details)
        return;
    if (now.ready != stored.ready) {
        stored.ready = now.ready;
        if (lobby_speaks && !now.self) {
            spoken.push_back(offer_of(
                say_filled(
                    now.ready ? "{name} is ready" : "{name} is not ready", {{"name", now.name}}
                ),
                bridge::Politeness::polite,
                false,
                {},
                now.name
            ));
        }
    }
    if (now.side != stored.side) {
        stored.side = now.side;
        if (lobby_speaks && !now.self) {
            const std::string side_name = now.side_name.empty()
                                              ? std::to_string(static_cast<unsigned>(now.side))
                                              : now.side_name;
            spoken.push_back(offer_of(
                say_filled(
                    "{name} changed side to {side}", {{"name", now.name}, {"side", side_name}}
                ),
                bridge::Politeness::polite,
                false,
                {},
                now.name
            ));
        }
    }
    if (now.color != stored.color) {
        // A seat is given "no colour" before the host picks one for it.
        const bool chosen = stored.color < kColourCount && now.color < kColourCount;
        stored.color = now.color;
        if (lobby_speaks && !now.self && chosen) {
            const char* word = colour_word(now.color);
            const std::string colour_name =
                word[0] == '\0' ? std::to_string(static_cast<unsigned>(now.color)) : say(word);
            spoken.push_back(offer_of(
                say_filled(
                    "{name} changed colour to {colour}",
                    {{"name", now.name}, {"colour", colour_name}}
                ),
                bridge::Politeness::polite,
                false,
                {},
                now.name
            ));
        }
    }
    if (now.team != stored.team) {
        stored.team = now.team;
        if (lobby_speaks && !now.self) {
            const std::string team_name = team_words(now.team);
            spoken.push_back(offer_of(
                say_filled(
                    "{name} changed team to {team}", {{"name", now.name}, {"team", team_name}}
                ),
                bridge::Politeness::polite,
                false,
                {},
                now.name
            ));
        }
    }
}

/// Closes the member list once both the hold and the quiet stretch have passed.
///
/// @param[in,out] memory what was heard before
/// @param now_ms the caller's clock, in milliseconds
void settle_list(Memory& memory, uint64_t now_ms) {
    if (!memory.listing)
        return;
    const uint64_t hold_until = memory.entered_ms + kListHoldMs;
    const uint64_t quiet_until = memory.last_row_ms + kListQuietMs;
    const uint64_t until = hold_until > quiet_until ? hold_until : quiet_until;
    if (now_ms >= until)
        memory.listing = false;
}

} // namespace

std::vector<uint16_t> ring_indices(uint16_t cursor, uint16_t head, uint16_t capacity) {
    std::vector<uint16_t> indices;
    if (capacity == 0)
        return indices;
    uint16_t at = cursor;
    for (uint16_t steps = 0; at != head && steps < capacity; ++steps) {
        indices.push_back(at);
        at = static_cast<uint16_t>((static_cast<uint32_t>(at) + 1U) % capacity);
    }
    return indices;
}

std::vector<bridge::SpokenOffer>
take_notes(const Hear& hear, Memory& memory, const std::vector<Note>& notes, uint64_t now_ms) {
    std::vector<bridge::SpokenOffer> spoken;
    settle_list(memory, now_ms);
    for (const Note& note : notes) {
        if (note.kind == "announce" || note.kind == "muzzle" || note.kind == "hear")
            continue;
        if (note.kind == "session") {
            remember_session(memory, note.text);
            continue;
        }
        if (note.kind == "line") {
            take_internal(spoken, memory, hear, note.text, now_ms);
            continue;
        }
        if (note.kind == "page" || note.kind == "alert" || note.kind == "room" ||
            note.kind == "console" || note.kind == "place")
            continue;
        if (note.kind == "ticker") {
            if (hear.ticker && hear.speaking && !note.text.empty()) {
                spoken.push_back(
                    offer_of(std::string(note.text), bridge::Politeness::polite, false, {}, {})
                );
            }
            continue;
        }
        if (note.kind != "said")
            continue;
        std::string room;
        if (entered_room(note.text, room)) {
            if (!room.empty())
                memory.chat_room = room;
            memory.listing = true;
            memory.entered_ms = now_ms;
            memory.last_row_ms = now_ms;
            continue;
        }
        if (page_line(note.text))
            continue;
        std::string name;
        std::string body;
        if (!said_line(note.text, name, body))
            continue;
        if (body.find("[Engine:") != std::string::npos)
            continue;
        if (body.starts_with("<Whispered>"))
            continue;
        if (is_self(name, memory.local_name))
            continue;
        if (!hear.speaking || hear.chat != bridge::SpeakChat::all)
            continue;
        const std::string chat_room =
            memory.chat_room.empty() ? std::string("Main") : memory.chat_room;
        spoken.push_back(offer_of(
            std::string(note.text), bridge::Politeness::polite, true, chat_room, std::move(name)
        ));
    }
    return spoken;
}

std::vector<bridge::SpokenOffer> take_room(const Hear& hear, Memory& memory, const RoomNow& room) {
    std::vector<bridge::SpokenOffer> spoken;
    if (!room.open) {
        const bool was_open = memory.room_open;
        memory.room_open = false;
        memory.seats.clear();
        memory.host_known = false;
        memory.host_details = false;
        if (was_open && room.creator_left && hear.speaking && hear.lobby) {
            spoken.push_back(
                offer_of(say("The host has left"), bridge::Politeness::polite, false, {}, {})
            );
        }
        return spoken;
    }
    if (!memory.room_open) {
        memory.room_open = true;
        memory.seats.assign(room.seats.size(), SeatWas{});
        for (std::size_t index = 0; index < room.seats.size(); ++index)
            store_seat(memory.seats[index], room.seats[index]);
        take_host(spoken, memory, hear, room);
        memory.start_frame = room.start_frame;
        return spoken;
    }
    if (memory.seats.size() < room.seats.size())
        memory.seats.resize(room.seats.size());
    if (!memory.host_details) {
        // A joiner sees its own seat a frame or two before the host's, and the
        // host's info later still. Until that info arrives the room is still
        // filling in: every seat is stored as it is, and none is announced.
        for (std::size_t index = 0; index < room.seats.size(); ++index)
            store_seat(memory.seats[index], room.seats[index]);
        take_host(spoken, memory, hear, room);
        memory.start_frame = room.start_frame;
        return spoken;
    }
    for (std::size_t index = 0; index < room.seats.size(); ++index)
        take_seat(spoken, memory.seats[index], room.seats[index], hear, memory);
    take_host(spoken, memory, hear, room);
    if (memory.start_frame == 0 && room.start_frame > 0 && hear.speaking && hear.lobby) {
        spoken.push_back(
            offer_of(say("The countdown has started"), bridge::Politeness::assertive, false, {}, {})
        );
    }
    memory.start_frame = room.start_frame;
    return spoken;
}

std::vector<bridge::SpokenOffer>
take_reports(const Hear& hear, const std::vector<ReportRow>& rows, std::string_view attack_ending) {
    std::vector<bridge::SpokenOffer> spoken;
    if (!hear.speaking || attack_ending.empty())
        return spoken;
    for (const ReportRow& row : rows) {
        if ((row.kind & kKindMask) != kUnitReport || !row.local_unit || !row.commander)
            continue;
        if (row.text.size() < attack_ending.size() ||
            row.text.compare(
                row.text.size() - attack_ending.size(), attack_ending.size(), attack_ending
            ) != 0)
            continue;
        spoken.push_back(
            offer_of(say("Commander under attack"), bridge::Politeness::polite, false, {}, {})
        );
    }
    return spoken;
}

std::vector<bridge::SpokenOffer> take_chat_lines(
    const Hear& hear,
    const std::string& room,
    const std::vector<std::string>& lines,
    std::string_view self_name
) {
    std::vector<bridge::SpokenOffer> spoken;
    if (!hear.speaking || hear.chat != bridge::SpeakChat::all)
        return spoken;
    for (const std::string& line : lines) {
        if (line.find("[Engine:") != std::string::npos)
            continue;
        std::string name;
        std::string body;
        bool directed = false;
        if (!bracket_line(line, name, body, directed))
            continue;
        if (is_self(name, self_name))
            continue;
        if (directed) {
            spoken.push_back(offer_of(
                say_filled("Whisper from {name}: {text}", {{"name", name}, {"text", body}}),
                bridge::Politeness::polite,
                false,
                {},
                name
            ));
            continue;
        }
        // The text is made before the name moves into the speaker: the order
        // a compiler builds the arguments in is its own.
        std::string text = name + ": " + body;
        spoken.push_back(
            offer_of(std::move(text), bridge::Politeness::polite, true, room, std::move(name))
        );
    }
    return spoken;
}

} // namespace oa::app::spoken

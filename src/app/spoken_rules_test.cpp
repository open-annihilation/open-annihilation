// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The spoken-update rules: a page once, no flood on entering, the host
// leaving, a seat seeded from its first info, a join with no false lines on
// either side, speak-chat over pages and whispers, and a commander under
// attack.
#include "oa/app/spoken_rules.hpp"

#include "oa/test/check.hpp"

#include <string>
#include <vector>

namespace {

namespace spoken = oa::app::spoken;
namespace bridge = oa::platform::screen_reader;

spoken::Hear hearing() {
    spoken::Hear hear;
    hear.speaking = true;
    hear.chat = bridge::SpeakChat::all;
    hear.lobby = true;
    hear.ticker = false;
    return hear;
}

bool has_text(const std::vector<bridge::SpokenOffer>& lines, const std::string& fragment) {
    for (const bridge::SpokenOffer& line : lines) {
        if (line.text.find(fragment) != std::string::npos)
            return true;
    }
    return false;
}

int count_text(const std::vector<bridge::SpokenOffer>& lines, const std::string& fragment) {
    int count = 0;
    for (const bridge::SpokenOffer& line : lines) {
        if (line.text.find(fragment) != std::string::npos)
            ++count;
    }
    return count;
}

spoken::Note note_of(const char* kind, const char* text) {
    spoken::Note note;
    note.kind = kind;
    note.text = text;
    return note;
}

spoken::SeatNow seat_of(const char* name, uint32_t player_id, bool details) {
    spoken::SeatNow seat;
    seat.taken = true;
    seat.has_details = details;
    seat.player_id = player_id;
    seat.name = name;
    return seat;
}

/// A page note, its chat echo and a page-shaped alert are one line. A login
/// is said once.
void check_dedupe() {
    spoken::Memory memory;
    const spoken::Hear hear = hearing();
    const std::vector<spoken::Note> once{
        note_of("line", "G\tPage from Ghost: Meet me"),
        note_of("said", "<Page from Ghost> Meet me"),
        note_of("alert", "<Page from Ghost> Meet me"),
        note_of("page", "<Page from Ghost> Meet me"),
    };
    const std::vector<bridge::SpokenOffer> page = spoken::take_notes(hear, memory, once, 0);
    OA_CHECK(page.size() == 1);
    OA_CHECK(page[0].text == "Page from Ghost: Meet me");
    OA_CHECK(page[0].politeness == bridge::Politeness::assertive);
    OA_CHECK(!page[0].collapsible);

    const std::vector<spoken::Note> logins{
        note_of("session", "3 Ada"),
        note_of("line", "S\tLogged in as Ada"),
        note_of("line", "S\tLogged in as Ada"),
    };
    const std::vector<bridge::SpokenOffer> login = spoken::take_notes(hear, memory, logins, 0);
    OA_CHECK(login.size() == 1);
    OA_CHECK(login[0].text == "Logged in as Ada");
    OA_CHECK(memory.local_name == "Ada");
}

/// Member rows stay the room's list until it has settled. A row one frame,
/// four frames or 1.5 seconds after entering is not a join. A join after
/// the window is spoken and may collapse.
void check_flood() {
    spoken::Memory memory;
    const spoken::Hear hear = hearing();
    constexpr uint64_t kEntered = 10000;
    constexpr uint64_t kFrameMs = 16;
    const std::vector<spoken::Note> entering{
        note_of("session", "3 Ada"),
        note_of("said", "[Entered 'Calm']"),
        note_of("line", "P\tEntered Calm"),
        note_of("room", "joined Ada"),
        note_of("line", "C\tCalm\tAda\tAda joined Calm"),
        note_of("room", "joined Bea"),
        note_of("line", "C\tCalm\tBea\tBea joined Calm"),
        note_of("room", "joined Cy"),
        note_of("line", "C\tCalm\tCy\tCy joined Calm"),
    };
    const std::vector<bridge::SpokenOffer> first =
        spoken::take_notes(hear, memory, entering, kEntered);
    OA_CHECK(first.size() == 1);
    OA_CHECK(first[0].text == "Entered Calm");
    OA_CHECK(!has_text(first, "joined"));
    OA_CHECK(memory.listing);

    const std::vector<spoken::Note> frame1{note_of("line", "C\tCalm\tDee\tDee joined Calm")};
    const std::vector<bridge::SpokenOffer> at_frame =
        spoken::take_notes(hear, memory, frame1, kEntered + kFrameMs);
    OA_CHECK(at_frame.empty());
    OA_CHECK(memory.listing);

    const std::vector<spoken::Note> frame4{note_of("line", "C\tCalm\tEve\tEve joined Calm")};
    const std::vector<bridge::SpokenOffer> at_four =
        spoken::take_notes(hear, memory, frame4, kEntered + 4 * kFrameMs);
    OA_CHECK(at_four.empty());
    OA_CHECK(memory.listing);

    const std::vector<spoken::Note> late{note_of("line", "C\tCalm\tFay\tFay joined Calm")};
    const std::vector<bridge::SpokenOffer> at_late =
        spoken::take_notes(hear, memory, late, kEntered + 1500);
    OA_CHECK(at_late.empty());
    OA_CHECK(memory.listing);

    // The row at 1.5 s keeps the list open until 2.5 s (the later of the hold
    // and one second after that row).
    const std::vector<spoken::Note> later{
        note_of("line", "C\tCalm\tNewPlayer\tNewPlayer joined Calm"),
    };
    const uint64_t settled = kEntered + 1500 + spoken::kListQuietMs;
    const std::vector<bridge::SpokenOffer> join = spoken::take_notes(hear, memory, later, settled);
    OA_CHECK(join.size() == 1);
    OA_CHECK(join[0].collapsible);
    OA_CHECK(join[0].text == "NewPlayer joined Calm");
    OA_CHECK(join[0].room == "Calm");
    OA_CHECK(join[0].speaker == "NewPlayer");
    OA_CHECK(!memory.listing);
}

/// The room closing because its creator left is one sentence.
void check_host() {
    spoken::Memory memory;
    memory.room_open = true;
    memory.seats.resize(2);
    memory.seats[0].taken = true;
    memory.seats[0].name = "Host";
    memory.seats[1].taken = true;
    memory.seats[1].name = "Visitor";
    spoken::RoomNow closed;
    closed.open = false;
    closed.creator_left = true;
    const std::vector<bridge::SpokenOffer> line = spoken::take_room(hearing(), memory, closed);
    OA_CHECK(line.size() == 1);
    OA_CHECK(line[0].text == "The host has left");
    OA_CHECK(!memory.room_open);
    OA_CHECK(memory.seats.empty());

    spoken::Memory quiet;
    spoken::RoomNow never;
    never.open = false;
    never.creator_left = true;
    OA_CHECK(spoken::take_room(hearing(), quiet, never).empty());
}

/// The first info seeds a seat and the host. A later colour change is spoken.
/// Team 0 is the room's "1". The local player's ready change is silent.
void check_seed() {
    spoken::Memory memory;
    const spoken::Hear hear = hearing();
    spoken::RoomNow room;
    room.open = true;
    room.seats.resize(2);
    room.seats[0] = seat_of("Host", 1, false);
    room.seats[1] = seat_of("Visitor", 2, false);
    room.host_present = true;
    room.host_slot = 0;
    const std::vector<bridge::SpokenOffer> arrived = spoken::take_room(hear, memory, room);
    OA_CHECK(arrived.empty());
    OA_CHECK(memory.room_open);
    OA_CHECK(memory.seats[1].taken);
    OA_CHECK(!memory.seats[1].has_details);

    room.seats[0].has_details = true;
    room.seats[0].color = 1;
    room.seats[0].team = 0;
    room.seats[0].side = 1;
    room.seats[0].side_name = "Core";
    room.seats[1].has_details = true;
    room.seats[1].color = 0;
    room.seats[1].team = 0;
    room.host_has_details = true;
    room.map = "Comet Catcher";
    room.option_bits = 0x2000;
    room.metal = 10;
    const std::vector<bridge::SpokenOffer> seeded = spoken::take_room(hear, memory, room);
    OA_CHECK(count_text(seeded, "changed") == 0);
    OA_CHECK(count_text(seeded, "An option changed") == 0);
    OA_CHECK(count_text(seeded, "joined") == 0);
    OA_CHECK(has_text(seeded, "The map is Comet Catcher"));
    OA_CHECK(count_text(seeded, "The map is") == 1);
    OA_CHECK(memory.host_details);
    OA_CHECK(memory.seats[1].has_details);
    OA_CHECK(memory.seats[1].color == 0);
    OA_CHECK(memory.option_bits == 0x2000);

    room.seats[1].color = 3;
    room.seats[1].team = 0;
    const std::vector<bridge::SpokenOffer> colour = spoken::take_room(hear, memory, room);
    OA_CHECK(has_text(colour, "changed colour"));
    OA_CHECK(!has_text(colour, "An option changed"));

    room.seats[1].team = 0;
    memory.seats[1].team = 2;
    const std::vector<bridge::SpokenOffer> team = spoken::take_room(hear, memory, room);
    OA_CHECK(has_text(team, "changed team to 1"));

    room.seats[1].self = true;
    room.seats[1].ready = true;
    const std::vector<bridge::SpokenOffer> self_ready = spoken::take_room(hear, memory, room);
    OA_CHECK(self_ready.empty());
    OA_CHECK(memory.seats[1].ready);
}

/// A joiner sees its own seat first, the host's seat a frame later and the
/// host's info after that, and then the host's count of seated players
/// rises. None of that is a join or a change. A real option change is.
void check_joiner_first_frames() {
    spoken::Memory memory;
    const spoken::Hear hear = hearing();
    spoken::RoomNow room;
    room.open = true;
    room.seats.resize(10);
    room.seats[0] = seat_of("Visitor", 2, true);
    room.seats[0].self = true;
    std::vector<bridge::SpokenOffer> heard = spoken::take_room(hear, memory, room);

    room.seats[1] = seat_of("Hoster", 1, true);
    room.seats[1].color = 0;
    for (const bridge::SpokenOffer& line : spoken::take_room(hear, memory, room))
        heard.push_back(line);

    room.seats[1].color = 4;
    room.host_present = true;
    room.host_slot = 1;
    room.host_has_details = true;
    room.map = "Acid Foursome";
    room.option_bits = 0x0001;
    for (const bridge::SpokenOffer& line : spoken::take_room(hear, memory, room))
        heard.push_back(line);

    room.option_bits = 0x0002;
    for (const bridge::SpokenOffer& line : spoken::take_room(hear, memory, room))
        heard.push_back(line);
    OA_CHECK(count_text(heard, "joined") == 0);
    OA_CHECK(count_text(heard, "changed") == 0);
    OA_CHECK(count_text(heard, "An option changed") == 0);
    OA_CHECK(count_text(heard, "The map is Acid Foursome") == 1);
    OA_CHECK(heard.size() == 1);

    room.option_bits = 0x2002;
    const std::vector<bridge::SpokenOffer> cheats = spoken::take_room(hear, memory, room);
    OA_CHECK(cheats.size() == 1);
    OA_CHECK(has_text(cheats, "An option changed"));
}

/// On the host, a joiner's seat comes with "no colour" until the host picks
/// one, and the seated count rises. The join is said once, at once, and
/// before any change of that player's.
void check_host_sees_join() {
    spoken::Memory memory;
    const spoken::Hear hear = hearing();
    spoken::RoomNow room;
    room.open = true;
    room.seats.resize(10);
    room.seats[0] = seat_of("Hoster", 1, true);
    room.seats[0].self = true;
    room.host_present = true;
    room.host_slot = 0;
    room.host_has_details = true;
    room.map = "Acid Foursome";
    room.option_bits = 0x0001;
    OA_CHECK(count_text(spoken::take_room(hear, memory, room), "The map is") == 1);

    room.option_bits = 0x0002;
    room.seats[1] = seat_of("Visitor", 2, true);
    room.seats[1].color = 0;
    std::vector<bridge::SpokenOffer> heard = spoken::take_room(hear, memory, room);
    OA_CHECK(heard.size() == 1);
    OA_CHECK(heard.size() == 1 && heard[0].text == "Visitor joined");
    OA_CHECK(heard.size() == 1 && !heard[0].collapsible);

    room.seats[1].color = 0xff;
    for (const bridge::SpokenOffer& line : spoken::take_room(hear, memory, room))
        heard.push_back(line);
    room.seats[1].color = 1;
    for (const bridge::SpokenOffer& line : spoken::take_room(hear, memory, room))
        heard.push_back(line);
    OA_CHECK(count_text(heard, "changed") == 0);
    OA_CHECK(count_text(heard, "An option changed") == 0);

    room.seats[1].color = 2;
    const std::vector<bridge::SpokenOffer> white = spoken::take_room(hear, memory, room);
    OA_CHECK(white.size() == 1);
    OA_CHECK(has_text(white, "Visitor changed colour to White"));

    room.seats[1] = spoken::SeatNow{};
    room.option_bits = 0x0001;
    const std::vector<bridge::SpokenOffer> left = spoken::take_room(hear, memory, room);
    OA_CHECK(left.size() == 1);
    OA_CHECK(left.size() == 1 && left[0].text == "Visitor left");
}

/// speak-chat `off` silences pages and whispers, `pages` keeps the pages,
/// and a message box (`A`) is spoken under each.
void check_speak_chat() {
    const std::vector<spoken::Note> notes{
        note_of("line", "G\tPage from Ghost: Meet me"),
        note_of("line", "W\tWhisper from Ghost: psst"),
        note_of("line", "A\tThe server is busy"),
    };
    spoken::Hear hear = hearing();
    spoken::Memory all_memory;
    const std::vector<bridge::SpokenOffer> all = spoken::take_notes(hear, all_memory, notes, 0);
    OA_CHECK(all.size() == 3);
    OA_CHECK(has_text(all, "Whisper from Ghost: psst"));

    hear.chat = bridge::SpeakChat::pages;
    spoken::Memory pages_memory;
    const std::vector<bridge::SpokenOffer> pages = spoken::take_notes(hear, pages_memory, notes, 0);
    OA_CHECK(pages.size() == 2);
    OA_CHECK(has_text(pages, "Page from Ghost"));
    OA_CHECK(!has_text(pages, "Whisper"));
    OA_CHECK(has_text(pages, "The server is busy"));

    hear.chat = bridge::SpeakChat::off;
    spoken::Memory off_memory;
    const std::vector<bridge::SpokenOffer> off = spoken::take_notes(hear, off_memory, notes, 0);
    OA_CHECK(off.size() == 1);
    OA_CHECK(has_text(off, "The server is busy"));
}

/// Only the local commander's "Under Attack" caption is spoken. A wrapped
/// ring still yields every new slot.
void check_attack() {
    const std::vector<uint16_t> wrapped = spoken::ring_indices(28, 2, 30);
    OA_CHECK(wrapped.size() == 4);
    OA_CHECK(wrapped[0] == 28 && wrapped[1] == 29 && wrapped[2] == 0 && wrapped[3] == 1);
    OA_CHECK(spoken::ring_indices(4, 4, 30).empty());

    spoken::ReportRow solar;
    solar.text = "Solar Collector: Under Attack";
    solar.kind = spoken::kUnitReport;
    solar.local_unit = true;
    solar.commander = false;
    spoken::ReportRow other;
    other.text = "Commander: Under Attack";
    other.kind = spoken::kUnitReport;
    other.local_unit = false;
    other.commander = true;
    spoken::ReportRow chat;
    chat.text = "Commander: hello";
    chat.kind = spoken::kUnitReport;
    chat.local_unit = true;
    chat.commander = true;
    spoken::ReportRow hit;
    hit.text = "Commander: Wird angegriffen";
    hit.kind = spoken::kUnitReport;
    hit.local_unit = true;
    hit.commander = true;
    const std::vector<spoken::ReportRow> rows{solar, other, chat, hit};
    const std::vector<bridge::SpokenOffer> spoken =
        spoken::take_reports(hearing(), rows, ": Wird angegriffen");
    OA_CHECK(spoken.size() == 1);
    OA_CHECK(spoken[0].text == "Commander under attack");

    const std::vector<std::string> lines{
        "<Host> [Engine: OpenAnnihilation]",
        "<Ada> hello",
        "<Visitor> present",
    };
    const std::vector<bridge::SpokenOffer> chat_lines =
        spoken::take_chat_lines(hearing(), "battle room", lines, "Ada");
    OA_CHECK(chat_lines.size() == 1);
    OA_CHECK(chat_lines[0].text == "Visitor: present");
    OA_CHECK(chat_lines[0].room == "battle room");
}

} // namespace

int main() {
    check_dedupe();
    check_flood();
    check_host();
    check_seed();
    check_joiner_first_frames();
    check_host_sees_join();
    check_speak_chat();
    check_attack();
    return oa::test::check_exit_status();
}

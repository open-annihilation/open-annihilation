// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The battle room's Enhanced Mode: the revision the room plays, the lines a
// change posts, the seats the room reads, and START waiting for every
// machine's rules.
#include "oa/netgame/presence.hpp"
#include "oa/netgame/records.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"
#include "oa/ui/frontend_multiplayer/room_enhanced.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace mp = oa::ui::frontend_multiplayer;
namespace ng = oa::netgame;

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        ++failures;
    }
}

// --- The pure resolution ----------------------------------------------------

mp::EnhancedSeat known(int32_t slot, const char* name, uint32_t highest) {
    mp::EnhancedSeat seat;
    seat.slot = slot;
    seat.player = name;
    seat.announce = mp::EnhancedAnnounce::revision;
    seat.highest = highest;
    return seat;
}

mp::EnhancedSeat without(int32_t slot, const char* name) {
    mp::EnhancedSeat seat;
    seat.slot = slot;
    seat.player = name;
    seat.announce = mp::EnhancedAnnounce::none;
    return seat;
}

mp::EnhancedSeat unknown(int32_t slot, const char* name) {
    mp::EnhancedSeat seat;
    seat.slot = slot;
    seat.player = name;
    seat.announce = mp::EnhancedAnnounce::not_known;
    return seat;
}

mp::EnhancedRoomChange host_resolves(
    const std::vector<mp::EnhancedSeat>& seats,
    bool switch_on,
    const std::optional<mp::EnhancedRoom>& before,
    bool locked = false
) {
    mp::EnhancedRoomInputs inputs;
    inputs.hosting = true;
    inputs.switch_on = switch_on;
    inputs.locked = locked;
    inputs.seats = seats;
    return mp::resolve_enhanced_room(inputs, before);
}

mp::EnhancedRoomChange guest_resolves(
    const std::vector<mp::EnhancedSeat>& seats,
    std::optional<uint32_t> announced,
    const std::optional<mp::EnhancedRoom>& before
) {
    mp::EnhancedRoomInputs inputs;
    inputs.hosting = false;
    inputs.announced = announced;
    inputs.seats = seats;
    return mp::resolve_enhanced_room(inputs, before);
}

bool one_line(const mp::EnhancedRoomChange& change, const char* line) {
    return change.lines.size() == 1 && change.lines.front() == line;
}

void the_host_switch_decides() {
    const std::vector<mp::EnhancedSeat> alone{known(0, "Steve", 1)};
    auto off = host_resolves(alone, false, std::nullopt);
    expect(off.room.revision == 0, "a switch that is off plays Off");
    expect(off.room.reason == mp::EnhancedRoomReason::switch_off, "Off names the switch");
    expect(off.lines.empty(), "a room that opens Off posts nothing");
    auto on = host_resolves(alone, true, off.room);
    expect(on.room.revision == 1, "turning the switch on plays Revision 1");
    expect(on.room.reason == mp::EnhancedRoomReason::plays, "Revision 1 alone plays it");
    expect(one_line(on, "*** Enhanced Mode: Revision 1."), "turning it on posts the revision");
    auto again = host_resolves(alone, true, on.room);
    expect(again.lines.empty(), "the same room twice posts nothing");
    auto back = host_resolves(alone, false, again.room);
    expect(back.room.revision == 0, "turning it off plays Off");
    expect(one_line(back, "*** Enhanced Mode: Off."), "turning it off posts the off line");
    auto opened = host_resolves(alone, true, std::nullopt);
    expect(
        one_line(opened, "*** Enhanced Mode: Revision 1."),
        "a room that opens on posts its revision once"
    );
}

void the_room_plays_the_lowest_highest_revision() {
    const std::vector<mp::EnhancedSeat> same{known(0, "Steve", 1), known(1, "Visitor", 1)};
    auto both = host_resolves(same, true, std::nullopt);
    expect(both.room.revision == 1, "{1, 1} gives 1");
    expect(both.room.reason == mp::EnhancedRoomReason::plays, "{1, 1} names no one");
    expect(both.room.cause == -1 && both.room.cause_player.empty(), "{1, 1} has no cause");

    const std::vector<mp::EnhancedSeat> lower{known(0, "Steve", 2), known(1, "Visitor", 1)};
    auto lowered = host_resolves(lower, true, std::nullopt);
    expect(lowered.room.revision == 1, "{2, 1} gives 1");
    expect(lowered.room.reason == mp::EnhancedRoomReason::lower, "{2, 1} is lower");
    expect(lowered.room.cause == 1 && lowered.room.cause_player == "Visitor", "{2, 1} names 1");
    expect(
        one_line(lowered, "*** Enhanced Mode is Revision 1: Visitor's game has no later revision."),
        "{2, 1} posts the no-later-revision line"
    );
    expect(
        mp::enhanced_room_reason_text(lowered.room) == "Visitor's game has no later revision.",
        "the reason names the player with no later revision"
    );
    // Visitor leaves: the room plays 2, with one line.
    const std::vector<mp::EnhancedSeat> alone{known(0, "Steve", 2)};
    auto risen = host_resolves(alone, true, lowered.room);
    expect(risen.room.revision == 2, "the room rises once Visitor leaves");
    expect(one_line(risen, "*** Enhanced Mode: Revision 2."), "the rise posts one line");
    // A second lower machine changes neither the revision nor the lines.
    const std::vector<mp::EnhancedSeat> two{
        known(0, "Steve", 2), known(1, "Visitor", 1), known(2, "Ridge", 1)
    };
    auto first = host_resolves(two, true, std::nullopt);
    auto second = host_resolves(
        std::vector<mp::EnhancedSeat>{known(0, "Steve", 2), known(2, "Ridge", 1)}, true, first.room
    );
    expect(second.room.revision == 1 && second.room.cause == 2, "Ridge is named next");
    expect(second.lines.empty(), "a cause that changes at the same revision posts nothing");
}

void a_machine_without_a_revision_turns_the_room_off() {
    const std::vector<mp::EnhancedSeat> alone{known(0, "Steve", 1)};
    auto on = host_resolves(alone, true, std::nullopt);
    for (const char* name : {"Visitor", "Older"}) {
        // A 3.1c machine and an Open Annihilation record without 0x26 both
        // announce none.
        const std::vector<mp::EnhancedSeat> seated{known(0, "Steve", 1), without(1, name)};
        auto off = host_resolves(seated, true, on.room);
        expect(off.room.revision == 0, "a seat that announces none plays Off");
        expect(off.room.reason == mp::EnhancedRoomReason::lacks, "the room lacks it");
        expect(off.room.cause == 1 && off.room.cause_player == name, "the seat is named");
        const std::string line =
            std::string("*** Enhanced Mode is off: ") + name + "'s game doesn't have it.";
        expect(one_line(off, line.c_str()), "the lacks line names the seat");
        expect(
            mp::enhanced_room_reason_text(off.room) ==
                std::string(name) + "'s game doesn't have it.",
            "the reason names the seat"
        );
        auto still = host_resolves(seated, true, off.room);
        expect(still.lines.empty(), "the same Off room posts nothing again");
        auto left = host_resolves(alone, true, still.room);
        expect(left.room.revision == 1, "the room returns to Revision 1 when it leaves");
        expect(one_line(left, "*** Enhanced Mode: Revision 1."), "the return posts one line");
    }
    // The host turns the switch on with a 3.1c machine seated: the room stays
    // Off and says who. Turning it off again says so.
    const std::vector<mp::EnhancedSeat> seated{known(0, "Steve", 1), without(1, "Visitor")};
    auto opened = host_resolves(seated, false, std::nullopt);
    expect(opened.lines.empty(), "an Off room with a 3.1c seat posts nothing");
    auto turned = host_resolves(seated, true, opened.room);
    expect(turned.room.revision == 0, "the switch cannot play past a 3.1c seat");
    expect(
        one_line(turned, "*** Enhanced Mode is off: Visitor's game doesn't have it."),
        "turning the switch on with a 3.1c seat names it"
    );
    auto off_again = host_resolves(seated, false, turned.room);
    expect(one_line(off_again, "*** Enhanced Mode: Off."), "turning it off posts the off line");
}

void watchers_count_and_computers_are_their_machines() {
    // A watcher is a seat of its own; a computer player is not listed at all.
    const std::vector<mp::EnhancedSeat> watched{
        known(0, "Steve", 2), known(1, "Visitor", 2), known(4, "Watcher", 1)
    };
    auto room = host_resolves(watched, true, std::nullopt);
    expect(room.room.revision == 1 && room.room.cause == 4, "a watcher's revision counts");
}

void an_unknown_record_changes_nothing() {
    const std::vector<mp::EnhancedSeat> alone{known(0, "Steve", 1)};
    auto on = host_resolves(alone, true, std::nullopt);
    const std::vector<mp::EnhancedSeat> arriving{known(0, "Steve", 1), unknown(1, "Visitor")};
    auto waiting = host_resolves(arriving, true, on.room);
    expect(waiting.room.revision == 1, "a record that has not arrived keeps the revision");
    expect(waiting.room.waiting, "it is waited for");
    expect(waiting.lines.empty(), "it posts nothing");
    const std::vector<mp::EnhancedSeat> arrived{known(0, "Steve", 1), without(1, "Visitor")};
    auto off = host_resolves(arrived, true, waiting.room);
    expect(off.room.revision == 0 && !off.room.waiting, "the arrived record decides");
    expect(off.lines.size() == 1, "and posts once");
}

void boneyards_rooms_and_locked_launches() {
    expect(mp::enhanced_room_switch_start(true, false, false), "the switch starts from Settings");
    expect(!mp::enhanced_room_switch_start(false, false, false), "Settings Off starts it off");
    expect(!mp::enhanced_room_switch_start(true, true, false), "a Boneyards room starts Off");
    expect(!mp::enhanced_room_switch_start(true, false, true), "a locked launch starts Off");
    const std::vector<mp::EnhancedSeat> alone{known(0, "Steve", 1)};
    // A Boneyards room: Off at first, and the host may opt in.
    auto boneyards = host_resolves(alone, false, std::nullopt);
    expect(boneyards.room.revision == 0 && boneyards.lines.empty(), "Boneyards opens Off");
    auto opted = host_resolves(alone, true, boneyards.room);
    expect(opted.room.revision == 1, "the Boneyards host may turn it on");
    // A launch that locks the options keeps it Off, whatever the switch says.
    auto locked = host_resolves(alone, true, std::nullopt, true);
    expect(locked.room.revision == 0, "a locked launch plays Off");
    expect(locked.room.reason == mp::EnhancedRoomReason::locked, "a locked launch names the lock");
    expect(locked.lines.empty(), "a locked room opens without a line");
    expect(
        mp::enhanced_room_launch_lock_text() == "The launch keeps Enhanced Mode off.",
        "the launch's lock reason"
    );
    expect(
        mp::enhanced_room_guest_lock_text() == "Only the host can change Enhanced Mode.",
        "the guest's lock reason"
    );
}

void a_guest_follows_the_host() {
    const std::vector<mp::EnhancedSeat> seats{known(0, "Steve", 1), known(1, "Hoster", 1)};
    auto first = guest_resolves(seats, std::nullopt, std::nullopt);
    expect(first.room.revision == 0 && first.room.waiting, "a guest waits for the host");
    expect(first.lines.empty(), "and says nothing meanwhile");
    auto told = guest_resolves(seats, 1u, first.room);
    expect(told.room.revision == 1, "the guest plays the host's revision");
    expect(one_line(told, "*** Enhanced Mode: Revision 1."), "the guest posts it once");
    auto same = guest_resolves(seats, 1u, told.room);
    expect(same.lines.empty(), "the same revision posts nothing");
    auto silent = guest_resolves(seats, std::nullopt, same.room);
    expect(silent.room.revision == 1 && silent.lines.empty(), "a record in flight keeps it");
    // The host turns it off: no seat lacks it, so the line is the off line.
    auto off = guest_resolves(seats, 0u, silent.room);
    expect(one_line(off, "*** Enhanced Mode: Off."), "the guest reads the host's off");
    // A 3.1c machine joins a Revision 1 room: the guest names it.
    const std::vector<mp::EnhancedSeat> joined{
        known(0, "Steve", 1), known(1, "Hoster", 1), without(2, "Visitor")
    };
    auto lacked = guest_resolves(joined, 0u, told.room);
    expect(
        one_line(lacked, "*** Enhanced Mode is off: Visitor's game doesn't have it."),
        "the guest names the seat that turned the room off"
    );
    // That seat leaves while the host's switch is off: nothing changes.
    auto left = guest_resolves(seats, 0u, lacked.room);
    expect(left.lines.empty(), "an Off room stays quiet on a guest");
    // A guest that joins an Off room says nothing.
    auto joined_off = guest_resolves(seats, 0u, std::nullopt);
    expect(joined_off.lines.empty(), "joining an Off room posts nothing");
    // A host with a later revision: the guest names the seat that has none later.
    const std::vector<mp::EnhancedSeat> mixed{known(0, "Steve", 1), known(1, "Hoster", 2)};
    auto lower = guest_resolves(mixed, 1u, std::nullopt);
    expect(
        one_line(lower, "*** Enhanced Mode is Revision 1: Steve's game has no later revision."),
        "the guest names its own game when it has no later revision"
    );
}

void the_words() {
    expect(mp::enhanced_revision_word(1) == "Revision 1", "the revision's word");
    expect(mp::enhanced_revision_word(0) == "Off", "Off's word");
    expect(
        mp::enhanced_waiting_line("Visitor", 1) == "Waiting for Visitor: Revision 1",
        "the waiting line names the player and the revision"
    );
}

// --- The room's seats and START --------------------------------------------

struct Record {
    std::optional<uint32_t> highest{};
    std::optional<uint32_t> room{};
    std::optional<uint8_t> hash{uint8_t{7}};
};

std::vector<uint8_t> encoded(const Record& wanted) {
    ng::PresenceRecord value{};
    value.enhanced_highest_revision = wanted.highest;
    value.enhanced_room_revision = wanted.room;
    if (wanted.hash) {
        value.sim_hash = ng::PresenceDigest{};
        value.sim_hash->fill(*wanted.hash);
    }
    std::array<uint8_t, ng::presence_record_max_bytes> bytes{};
    std::size_t size = 0;
    expect(
        ng::encode_presence(value, bytes.data(), bytes.size(), &size) == ng::WireError::ok,
        "fixture presence encodes"
    );
    return {bytes.begin(), bytes.begin() + static_cast<std::ptrdiff_t>(size)};
}

struct Room {
    std::unique_ptr<oa::Game> game = std::make_unique<oa::Game>();
    mp::Lobby lobby{};

    explicit Room(bool hosting = true) {
        mp::lobby_reset(lobby, *game);
        game->players[0].player_id = hosting ? 100u : 200u;
        mp::lobby_seat_local(lobby, 0, hosting, hosting ? "Host" : "Guest");
        remote(1, hosting ? 200u : 100u, !hosting);
    }

    void remote(int32_t slot, uint32_t id, bool hosting = false, bool classic = false) {
        auto& player = mp::slot_player(lobby, slot);
        player.in_use = 1;
        player.player_id = id;
        player.index = static_cast<uint8_t>(slot);
        std::memset(player.name, 0, sizeof player.name);
        const char* name = slot == 1 ? "Visitor" : "Other";
        std::memcpy(player.name, name, std::strlen(name));
        mp::slot_set_status(lobby, player, mp::kSlotRemote);
        auto* info = mp::slot_info(lobby, slot);
        info->state = mp::kInfoStatePlaying;
        info->net_id = id;
        info->role = hosting ? mp::kRoleHost : 0;
        info->engine_signature[0] = classic ? 0 : ng::engine_signature_first;
        info->engine_signature[1] = classic ? 0 : ng::engine_signature_second;
        lobby.presence_records.block_player[slot] = id;
    }

    void presence(int32_t slot, std::span<const uint8_t> bytes) {
        auto& records = lobby.presence_records;
        if (slot == 0) {
            std::memcpy(records.local, bytes.data(), bytes.size());
            records.local_size = static_cast<uint16_t>(bytes.size());
        } else {
            std::memcpy(records.peer[slot], bytes.data(), bytes.size());
            records.peer_size[slot] = static_cast<uint16_t>(bytes.size());
            records.peer_id[slot] = mp::slot_player(lobby, slot).player_id;
        }
    }
};

void the_seats_the_room_reads() {
    Room room{};
    auto seats = mp::lobby_enhanced_seats(room.lobby, 1u);
    expect(seats.size() == 2, "the host and Visitor are seated");
    expect(
        seats[0].announce == mp::EnhancedAnnounce::revision && seats[0].highest == 1,
        "this machine announces its own revision"
    );
    expect(seats[1].announce == mp::EnhancedAnnounce::not_known, "no record is not known");
    room.presence(1, encoded({1u}));
    seats = mp::lobby_enhanced_seats(room.lobby, 1u);
    expect(
        seats[1].announce == mp::EnhancedAnnounce::revision && seats[1].highest == 1,
        "a record with 0x26 announces it"
    );
    expect(seats[1].player == "Visitor", "the seat carries its name");
    room.presence(1, encoded({std::nullopt}));
    seats = mp::lobby_enhanced_seats(room.lobby, 1u);
    expect(seats[1].announce == mp::EnhancedAnnounce::none, "a record without 0x26 has none");
    auto bytes = encoded({1u});
    bytes.pop_back();
    room.presence(1, bytes);
    seats = mp::lobby_enhanced_seats(room.lobby, 1u);
    expect(seats[1].announce == mp::EnhancedAnnounce::not_known, "a truncated record is unknown");
    room.remote(1, 200, false, true);
    seats = mp::lobby_enhanced_seats(room.lobby, 1u);
    expect(seats[1].announce == mp::EnhancedAnnounce::none, "a 3.1c block has none");
    room.remote(1, 200);
    room.lobby.presence_records.block_player[1] = 201;
    seats = mp::lobby_enhanced_seats(room.lobby, 1u);
    expect(
        seats[1].announce == mp::EnhancedAnnounce::not_known,
        "an earlier player's block is not this player's"
    );
    room.lobby.presence_records.block_player[1] = 200;
    seats = mp::lobby_enhanced_seats(room.lobby, std::nullopt);
    expect(seats[0].announce == mp::EnhancedAnnounce::none, "a local machine may announce none");

    // A watcher is a seat; a defeated player who does not watch and a
    // computer player are not.
    room.remote(2, 300);
    auto* info = mp::slot_info(room.lobby, 2);
    info->state = mp::kInfoStateDefeated;
    seats = mp::lobby_enhanced_seats(room.lobby, 1u);
    expect(seats.size() == 2, "a remote computer has no seat of its own");
    info->options = mp::option::watcher;
    seats = mp::lobby_enhanced_seats(room.lobby, 1u);
    expect(seats.size() == 3 && seats[2].slot == 2, "a watcher has a seat");
    auto& computer = mp::slot_player(room.lobby, 3);
    computer.in_use = 1;
    computer.index = 3;
    mp::slot_set_status(room.lobby, computer, mp::kSlotComputer);
    seats = mp::lobby_enhanced_seats(room.lobby, 1u);
    expect(seats.size() == 3, "a local computer is its machine's");
}

void a_guest_reads_the_hosts_announcement() {
    Room guest(false);
    expect(!mp::lobby_enhanced_announced(guest.lobby), "no host record is not known");
    guest.presence(1, encoded({1u, 1u}));
    expect(mp::lobby_enhanced_announced(guest.lobby) == 1u, "the host's 0x27 is read");
    expect(mp::lobby_enhanced_room_revision(guest.lobby) == 1, "the room's revision is read");
    guest.presence(1, encoded({1u}));
    expect(mp::lobby_enhanced_announced(guest.lobby) == 0u, "no 0x27 is Off");
    guest.remote(1, 100, true, true);
    expect(mp::lobby_enhanced_announced(guest.lobby) == 0u, "a 3.1c host is Off");
    guest.remote(1, 100, true);
    guest.lobby.presence_records.block_player[1] = 101;
    expect(!mp::lobby_enhanced_announced(guest.lobby), "no host block is not known");
}

void off_rooms_keep_their_start_rules() {
    Room room{};
    room.remote(1, 200, false, true);
    expect(mp::lobby_enhanced_allow_start(room.lobby), "an Off room with 3.1c is allowed");
    room.presence(0, encoded({1u, std::nullopt, 2}));
    room.presence(1, encoded({1u, std::nullopt, 3}));
    expect(mp::lobby_enhanced_allow_start(room.lobby), "an Off room adds no sim hash gate");
    expect(mp::lobby_enhanced_start_problem(room.lobby) == nullptr, "Off has no refusal line");
    mp::Lobby empty{};
    expect(mp::lobby_enhanced_allow_start(empty), "a lobby without a game adds no hold");
}

void start_waits_for_every_hash() {
    for (const bool hosting : {false, true}) {
        Room room(hosting);
        // The host's record names Revision 1.
        room.presence(hosting ? 0 : 1, encoded({1u, 1u, 7}));
        if (hosting) {
            auto block = mp::lobby_enhanced_block(room.lobby);
            expect(block.hold == mp::EnhancedHold::waiting, "START waits for a record");
            expect(block.slot == 1 && block.revision == 1, "it names the machine and revision");
            expect(!mp::lobby_enhanced_allow_start(room.lobby), "an absent record holds START");
        }
        room.presence(hosting ? 1 : 0, encoded({1u, std::nullopt, 8}));
        auto block = mp::lobby_enhanced_block(room.lobby);
        expect(block.hold == mp::EnhancedHold::rules_differ, "another hash holds START");
        expect(
            mp::lobby_enhanced_start_problem(room.lobby) != nullptr &&
                std::strstr(mp::lobby_enhanced_start_problem(room.lobby), "rules") != nullptr,
            "the refusal is the rules line"
        );
        room.presence(hosting ? 1 : 0, encoded({1u, std::nullopt, std::nullopt}));
        expect(!mp::lobby_enhanced_allow_start(room.lobby), "a missing hash holds START");
        room.presence(hosting ? 1 : 0, encoded({1u, std::nullopt, 7}));
        expect(mp::lobby_enhanced_allow_start(room.lobby), "the host's hash allows START");
        expect(mp::lobby_enhanced_start_problem(room.lobby) == nullptr, "and has no line");
    }
    // A missing host hash holds START.
    Room room{};
    room.presence(0, encoded({1u, 1u, std::nullopt}));
    room.presence(1, encoded({1u, std::nullopt, 7}));
    expect(!mp::lobby_enhanced_allow_start(room.lobby), "a missing host hash holds START");
}

void the_host_checks_every_machine_and_guests_check_the_host() {
    Room guest(false);
    guest.presence(0, encoded({1u, std::nullopt, 7}));
    guest.presence(1, encoded({1u, 1u, 7}));
    guest.remote(2, 300);
    expect(
        mp::lobby_enhanced_allow_start(guest.lobby),
        "a guest need not receive another guest's record before starting"
    );
    Room host{};
    host.presence(0, encoded({1u, 1u, 7}));
    host.presence(1, encoded({1u, std::nullopt, 7}));
    host.remote(2, 300);
    auto block = mp::lobby_enhanced_block(host.lobby);
    expect(
        block.hold == mp::EnhancedHold::waiting && block.slot == 2,
        "the host waits for every seated machine"
    );
    host.presence(2, encoded({1u, std::nullopt, 7}));
    expect(mp::lobby_enhanced_allow_start(host.lobby), "every machine agreeing allows START");
    // A 3.1c machine in a room the host's record still calls Enhanced holds
    // START until the host turns the room Off.
    host.remote(2, 300, false, true);
    expect(
        mp::lobby_enhanced_block(host.lobby).hold == mp::EnhancedHold::rules_differ,
        "a 3.1c seat cannot agree with Enhanced rules"
    );
    // A watcher must agree; a remote computer is its machine's.
    host.remote(2, 300);
    auto* info = mp::slot_info(host.lobby, 2);
    info->state = mp::kInfoStateDefeated;
    host.lobby.presence_records.peer_size[2] = 0;
    expect(mp::lobby_enhanced_allow_start(host.lobby), "a remote computer needs no record");
    info->options = mp::option::watcher;
    expect(!mp::lobby_enhanced_allow_start(host.lobby), "a watcher must agree");
}

void malformed_presence_cannot_agree() {
    Room room{};
    room.presence(0, encoded({1u, 1u, 7}));
    auto bytes = encoded({1u, std::nullopt, 7});
    bytes.pop_back();
    room.presence(1, bytes);
    expect(!mp::lobby_enhanced_allow_start(room.lobby), "a truncated record holds START");
}

} // namespace

int main() {
    the_host_switch_decides();
    the_room_plays_the_lowest_highest_revision();
    a_machine_without_a_revision_turns_the_room_off();
    watchers_count_and_computers_are_their_machines();
    an_unknown_record_changes_nothing();
    boneyards_rooms_and_locked_launches();
    a_guest_follows_the_host();
    the_words();
    the_seats_the_room_reads();
    a_guest_reads_the_hosts_announcement();
    off_rooms_keep_their_start_rules();
    start_waits_for_every_hash();
    the_host_checks_every_machine_and_guests_check_the_host();
    malformed_presence_cannot_agree();
    return failures == 0 ? 0 : 1;
}

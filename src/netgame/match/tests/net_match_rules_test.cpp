// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A network match under a mod profile's setup and team rules, over an
// in-memory transport: the host's start positions with team-mates
// together, every machine working out the host's fixed starts, a team
// record moving this machine's alliances, and an alliance request carried
// out. Each case also runs without the rules, where the match keeps 3.1c's
// behaviour.

#include "oa/netgame/match/net_match.hpp"
#include "oa/netgame/match/packet_layer.hpp"
#include "oa/netgame/network.hpp"
#include "oa/ui/frontend_multiplayer/team_rules.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <deque>
#include <memory>
#include <string>
#include <vector>

using namespace oa;
using namespace oa::netgame;
using namespace oa::netgame::match;
namespace tr = oa::ui::frontend_multiplayer::team_rules;

namespace {

int failures = 0;

#define CHECK(condition)                                                                           \
    do {                                                                                           \
        if (!(condition)) {                                                                        \
            ++failures;                                                                            \
            std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                   \
        }                                                                                          \
    } while (0)

struct Datagram {
    uint32_t from{};
    uint32_t to{};
    std::vector<uint8_t> bytes;
};

// Datagrams between two machines: each send lands in the peer's queue and
// is kept for the test to read back.
struct Link {
    std::deque<Datagram> queue;
    Link* peer{};
    std::vector<Datagram> sent;
};

uint32_t
link_send(void* context, uint32_t from, uint32_t to, uint32_t, const uint8_t* data, uint32_t size) {
    auto* link = static_cast<Link*>(context);
    Datagram datagram{from, to, std::vector<uint8_t>(data, data + size)};
    link->sent.push_back(datagram);
    if (link->peer != nullptr)
        link->peer->queue.push_back(std::move(datagram));
    return transport_result::ok;
}

uint32_t
link_receive(void* context, uint32_t* from, uint32_t* to, uint8_t* buffer, uint32_t* size) {
    auto* link = static_cast<Link*>(context);
    if (link->queue.empty())
        return transport_result::no_messages;
    const auto& d = link->queue.front();
    if (d.bytes.size() > *size) {
        *size = static_cast<uint32_t>(d.bytes.size());
        return transport_result::buffer_too_small;
    }
    std::memcpy(buffer, d.bytes.data(), d.bytes.size());
    *size = static_cast<uint32_t>(d.bytes.size());
    *from = d.from;
    *to = d.to;
    link->queue.pop_front();
    return transport_result::ok;
}

// One record a machine sent, with its datagram's addresses.
struct SentRecord {
    uint32_t from{};
    uint32_t to{};
    std::vector<uint8_t> bytes; // type byte first
};

// The records of one type a machine sent, unwrapped from their frames.
std::vector<SentRecord> records_sent(const Link& link, RecordType type) {
    std::vector<SentRecord> out;
    for (const auto& datagram : link.sent) {
        const auto frame = oa::netgame::network::decode_frame(datagram.bytes);
        for (std::size_t at = frame_header_bytes; at < frame.size();) {
            uint16_t length = 0;
            if (record_wire_length(frame.data() + at, frame.size() - at, &length) !=
                    WireError::ok ||
                length == 0)
                break;
            if (frame[at] == static_cast<uint8_t>(type))
                out.push_back(
                    {datagram.from,
                     datagram.to,
                     std::vector<uint8_t>(frame.begin() + at, frame.begin() + at + length)}
                );
            at += length;
        }
    }
    return out;
}

struct Seat {
    uint32_t id{};
    uint8_t status{};
    uint8_t team{tr::no_team};
    bool watcher{};
};

/// A machine seating up to six players over a Link.
struct Machine {
    World* world{};
    NetConnection connection{};
    Link link;
    std::unique_ptr<NetMatch> match = std::make_unique<NetMatch>();
    oa::data::match_rules::MatchRules rules{};

    Machine(const std::vector<Seat>& seats, uint8_t local_slot, bool loading) {
        world = world_create();
        const WorldCapacity capacity{4 * OA_PLAYER_COUNT + 1, 2, 0};
        CHECK(world != nullptr && world_alloc_tables(world, &capacity) != 0);
        for (uint8_t i = 0; i < OA_PLAYER_COUNT; ++i) {
            auto& p = world->game.players[i];
            p.index = i;
            p.player_id = no_player_id;
            p.info = oa_ref_from_index(i);
            p.team = tr::no_team;
            if (i < seats.size()) {
                p.in_use = 1;
                p.player_id = seats[i].id;
                p.status = seats[i].status;
                p.team = seats[i].team;
                p.machine_group = 1;
                auto& info = world->player_info[i];
                info.state = seats[i].status == OA_PLAYER_STATUS_COMPUTER ? tr::setup_computer
                                                                          : tr::setup_human;
                if (seats[i].watcher)
                    info.options |= OA_SETUP_OPTION_WATCHER;
            }
        }
        world->game.local_player_index = local_slot;
        world->game.session_flags = 1;
        world->player_info[local_slot].role = 1; // this machine hosts
        connection.packets = new PacketLayer();
        packet_layer_create(connection.packets);
        packet_layer_start(connection.packets, NetTransport{&link, link_send, link_receive});
        net_match_begin(match.get(), &connection, world, ReplicationSim{}, NetMatchHooks{});
        if (!loading)
            net_match_enter_game(match.get());
    }

    ~Machine() {
        delete connection.packets;
        world_destroy(world);
    }

    std::vector<SentRecord> sent(RecordType type) {
        packet_layer_flush(connection.packets, 0, true);
        return records_sent(link, type);
    }

    /// The start position the host assigned each slot: a local one from the
    /// match, a remote one from the record sent to it.
    std::vector<int32_t> positions() {
        std::vector<int32_t> out(OA_PLAYER_COUNT, -1);
        net_match_loader_waiting(match.get());
        (void)net_match_loading_frame(match.get());
        for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot) {
            const auto& p = world->game.players[slot];
            if (p.in_use == 0)
                continue;
            if (p.status != OA_PLAYER_STATUS_MIRRORED) {
                const auto start = net_match_start_position(match.get(), slot);
                out[slot] = start == no_start_position ? -1 : start;
                continue;
            }
            for (const auto& record : sent(RecordType::start_position))
                if (record.to == p.player_id)
                    out[slot] = record.bytes[1];
        }
        return out;
    }
};

/// The same players as another machine sees them: its local players are
/// remote here and the other way round.
///
/// @param seats a machine's seats
/// @return the seats seen from the other machine
std::vector<Seat> seen_from_the_other(std::vector<Seat> seats) {
    for (auto& seat : seats)
        seat.status = seat.status == OA_PLAYER_STATUS_MIRRORED ? OA_PLAYER_STATUS_LOCAL
                                                               : OA_PLAYER_STATUS_MIRRORED;
    return seats;
}

/// Sends a record from one machine's player and pumps it on another.
template <class R>
void deliver(Machine& from, Machine& to, uint32_t from_id, const R& record) {
    from.link.peer = &to.link;
    to.link.peer = &from.link;
    uint8_t bytes[80];
    std::size_t written = 0;
    CHECK(encode_record(record, bytes, sizeof bytes, &written) == WireError::ok);
    const auto to_id = to.world->game.players[to.world->game.local_player_index].player_id;
    CHECK(net_match_send(from.match.get(), from_id, to_id, bytes, written));
    packet_layer_flush(from.connection.packets, 0, true);
    from.link.sent.clear();
    (void)net_match_pump(to.match.get());
}

void start_positions_keep_team_mates_together() {
    const std::vector<Seat> seats{
        {0x10, OA_PLAYER_STATUS_LOCAL, 0},
        {0x11, OA_PLAYER_STATUS_MIRRORED, 0},
        {0x12, OA_PLAYER_STATUS_MIRRORED, 1},
        {0x13, OA_PLAYER_STATUS_MIRRORED, 1},
    };
    {
        Machine base(seats, 0, true);
        base.world->player_info[0].options |= OA_SETUP_OPTION_FIXED_LOCATIONS;
        const auto positions = base.positions();
        CHECK(positions[0] == 0 && positions[1] == 1 && positions[2] == 2 && positions[3] == 3);
    }
    {
        Machine baseline(seats, 0, true);
        baseline.world->player_info[0].options |= OA_SETUP_OPTION_FIXED_LOCATIONS;
        baseline.rules.setup.team_start_positions.enabled = true;
        net_match_bind_rules(baseline.match.get(), &baseline.rules, false);
        const auto positions = baseline.positions();
        CHECK(positions[0] == 0 && positions[1] == 1 && positions[2] == 2 && positions[3] == 3);
    }
    Machine teams(seats, 0, true);
    teams.world->player_info[0].options |= OA_SETUP_OPTION_FIXED_LOCATIONS;
    teams.rules.setup.team_start_positions.enabled = true;
    teams.rules.setup.team_start_positions.mode =
        oa::data::match_rules::SetupTeamStartPositionsMode::team_adjacent;
    net_match_bind_rules(teams.match.get(), &teams.rules, false);
    // Team 1 takes every second position from 0, team 2 from 1.
    const auto positions = teams.positions();
    CHECK(positions[0] == 0 && positions[1] == 2 && positions[2] == 1 && positions[3] == 3);
}

void start_positions_put_computers_last_on_neutral_maps() {
    const std::vector<Seat> seats{
        {0x10, OA_PLAYER_STATUS_COMPUTER},
        {0x11, OA_PLAYER_STATUS_LOCAL},
        {0x12, OA_PLAYER_STATUS_MIRRORED},
    };
    Machine host(seats, 1, true);
    host.world->player_info[1].options |= OA_SETUP_OPTION_FIXED_LOCATIONS;
    host.world->player_info[0].role = 0;
    host.rules.setup.team_start_positions.enabled = true;
    host.rules.setup.team_start_positions.mode =
        oa::data::match_rules::SetupTeamStartPositionsMode::team_adjacent;
    net_match_bind_rules(host.match.get(), &host.rules, true);
    const auto positions = host.positions();
    CHECK(positions[1] == 0 && positions[2] == 1 && positions[0] == 2);
}

/// The host's slot table: each of its slots' player id.
std::array<uint32_t, OA_PLAYER_COUNT> slot_table_of(const std::vector<Seat>& seats) {
    std::array<uint32_t, OA_PLAYER_COUNT> table{};
    for (std::size_t slot = 0; slot < seats.size(); ++slot)
        table[slot] = seats[slot].id;
    return table;
}

void every_machine_works_out_the_fixed_starts() {
    std::array<int32_t, OA_PLAYER_COUNT> worked{};
    // The host deals fixed starts in its slot order, past the watcher.
    const std::vector<Seat> seats{
        {0x10, OA_PLAYER_STATUS_LOCAL},
        {0x11, OA_PLAYER_STATUS_MIRRORED, tr::no_team, true},
        {0x12, OA_PLAYER_STATUS_COMPUTER},
        {0x13, OA_PLAYER_STATUS_MIRRORED},
    };
    const auto table = slot_table_of(seats);
    Machine host(seats, 0, true);
    host.world->player_info[0].options |= OA_SETUP_OPTION_FIXED_LOCATIONS;
    const auto dealt = host.positions();
    CHECK(dealt[0] == 0 && dealt[1] == -1 && dealt[2] == 1 && dealt[3] == 2);
    CHECK(net_match_fixed_start_positions(host.match.get(), table, worked));
    CHECK(std::equal(dealt.begin(), dealt.end(), worked.begin()));
    // The last player's machine seats it first and the others after it: it
    // reads the host's order from the host's slot table.
    Machine joiner(
        {{0x13, OA_PLAYER_STATUS_LOCAL},
         {0x10, OA_PLAYER_STATUS_MIRRORED},
         {0x11, OA_PLAYER_STATUS_MIRRORED, tr::no_team, true},
         {0x12, OA_PLAYER_STATUS_MIRRORED}},
        0,
        true
    );
    joiner.world->player_info[0].role = 0;
    joiner.world->player_info[1].role = 1;
    CHECK(net_match_fixed_start_positions(joiner.match.get(), table, worked));
    CHECK(worked[0] == 2 && worked[1] == 0 && worked[2] == -1 && worked[3] == 1);
    // A table that leaves out a player who plays gives no starts.
    auto stale = table;
    stale[3] = 0;
    CHECK(!net_match_fixed_start_positions(joiner.match.get(), stale, worked));
    CHECK(std::all_of(worked.begin(), worked.end(), [](int32_t start) { return start == -1; }));
}

void every_machine_puts_the_hosts_computers_last() {
    std::array<int32_t, OA_PLAYER_COUNT> worked{};
    // On a neutral map the team rules put the host's computer player after
    // the others. The joiner, in machine group 2, reads it as the host's by
    // its machine group.
    const std::vector<Seat> seats{
        {0x10, OA_PLAYER_STATUS_COMPUTER},
        {0x11, OA_PLAYER_STATUS_LOCAL},
        {0x12, OA_PLAYER_STATUS_MIRRORED},
    };
    const auto table = slot_table_of(seats);
    const auto team_adjacent = [](Machine& machine) {
        machine.rules.setup.team_start_positions.enabled = true;
        machine.rules.setup.team_start_positions.mode =
            oa::data::match_rules::SetupTeamStartPositionsMode::team_adjacent;
        net_match_bind_rules(machine.match.get(), &machine.rules, true);
    };
    Machine host(seats, 1, true);
    host.world->player_info[1].options |= OA_SETUP_OPTION_FIXED_LOCATIONS;
    host.world->game.players[2].machine_group = 2;
    team_adjacent(host);
    const auto dealt = host.positions();
    CHECK(dealt[1] == 0 && dealt[2] == 1 && dealt[0] == 2);
    CHECK(net_match_fixed_start_positions(host.match.get(), table, worked));
    CHECK(std::equal(dealt.begin(), dealt.end(), worked.begin()));
    Machine joiner(
        {{0x12, OA_PLAYER_STATUS_LOCAL},
         {0x10, OA_PLAYER_STATUS_MIRRORED},
         {0x11, OA_PLAYER_STATUS_MIRRORED}},
        0,
        true
    );
    joiner.world->player_info[0].role = 0;
    joiner.world->player_info[2].role = 1;
    joiner.world->player_info[1].state = tr::setup_computer;
    joiner.world->game.players[0].machine_group = 2;
    team_adjacent(joiner);
    CHECK(net_match_fixed_start_positions(joiner.match.get(), table, worked));
    CHECK(worked[0] == 1 && worked[1] == 2 && worked[2] == 0);
    // Without the host's machine group its computer players cannot be told
    // from another machine's, and the joiner gives no starts.
    joiner.world->game.players[2].machine_group = 0;
    CHECK(!net_match_fixed_start_positions(joiner.match.get(), table, worked));
    CHECK(std::all_of(worked.begin(), worked.end(), [](int32_t start) { return start == -1; }));
}

void team_records_move_local_alliances() {
    const std::vector<Seat> seats{
        {0x10, OA_PLAYER_STATUS_LOCAL, 1},
        {0x11, OA_PLAYER_STATUS_MIRRORED},
    };
    {
        Machine base(seats, 0, false);
        Machine guest(seen_from_the_other(seats), 1, false);
        PlayerTeamRecord team{};
        team.player_id = 0x11;
        team.value = 1;
        deliver(guest, base, 0x11, team);
        CHECK(base.world->game.players[1].team == 1);
        CHECK(base.world->game.players[0].alliance[1] == 0);
        CHECK(base.sent(RecordType::alliance).empty());
    }
    Machine m(seats, 0, false);
    Machine guest(seen_from_the_other(seats), 1, false);
    m.rules.teams.team_number_alliances.enabled = true;
    m.rules.teams.team_number_alliances.bit7_keeps_alliances = true;
    net_match_bind_rules(m.match.get(), &m.rules, false);
    PlayerTeamRecord team{};
    team.player_id = 0x11;
    team.value = 1;
    deliver(guest, m, 0x11, team);
    CHECK(m.world->game.players[1].team == 1);
    CHECK(m.world->game.players[0].alliance[1] == 1);
    const auto sent = m.sent(RecordType::alliance);
    CHECK(sent.size() == 2);
    if (sent.size() == 2) {
        AllianceRecord first{};
        AllianceRecord second{};
        CHECK(decode_record(sent[0].bytes.data(), sent[0].bytes.size(), &first) == WireError::ok);
        CHECK(decode_record(sent[1].bytes.data(), sent[1].bytes.size(), &second) == WireError::ok);
        CHECK(first.player_id_a == 0x10 && first.value == 1 && first.both_sides == 0);
        CHECK(sent[0].to == broadcast_destination_id);
        CHECK(
            second.player_id_a == 0x11 && second.player_id_b == 0x10 &&
            second.both_sides == tr::alliance_request && sent[1].to == 0x11
        );
    }
    team.value = 0x80 | 2;
    m.link.sent.clear();
    deliver(guest, m, 0x11, team);
    CHECK(m.sent(RecordType::alliance).empty());
    CHECK(m.world->game.players[1].team == 2);
    CHECK(m.world->game.players[0].alliance[1] == 1);
}

void alliance_requests_are_carried_out() {
    const std::vector<Seat> seats{
        {0x10, OA_PLAYER_STATUS_LOCAL},
        {0x11, OA_PLAYER_STATUS_MIRRORED},
    };
    for (const bool on : {false, true}) {
        Machine m(seats, 0, false);
        Machine guest(seen_from_the_other(seats), 1, false);
        m.rules.teams.team_number_alliances.enabled = on;
        net_match_bind_rules(m.match.get(), &m.rules, false);
        AllianceRecord request{};
        request.player_id_a = 0x10;
        request.player_id_b = 0x11;
        request.value = 1;
        request.both_sides = tr::alliance_request;
        deliver(guest, m, 0x11, request);
        CHECK(m.world->game.players[0].alliance[1] == 1);
        const auto sent = m.sent(RecordType::alliance);
        CHECK(sent.size() == (on ? 1U : 0U));
        if (on && sent.size() == 1)
            CHECK(sent[0].to == broadcast_destination_id && sent[0].from == 0x10);
    }
}

void the_host_deals_teams_by_position() {
    const std::vector<Seat> seats{
        {0x10, OA_PLAYER_STATUS_LOCAL},
        {0x11, OA_PLAYER_STATUS_MIRRORED},
        {0x12, OA_PLAYER_STATUS_COMPUTER},
    };
    char notice[96];
    Machine plain(seats, 0, true);
    net_match_deal_teams(plain.match.get(), 2, notice, sizeof notice);
    CHECK(std::string(notice) == "+autoteam is only available to the host of a multiplayer game");

    Machine host(seats, 0, true);
    host.world->player_info[0].options |= OA_SETUP_OPTION_FIXED_LOCATIONS;
    host.rules.setup.team_start_positions.enabled = true;
    host.rules.setup.team_start_positions.mode =
        oa::data::match_rules::SetupTeamStartPositionsMode::team_adjacent;
    host.rules.teams.team_number_alliances.enabled = true;
    net_match_bind_rules(host.match.get(), &host.rules, false);
    (void)host.positions();
    host.link.sent.clear();
    net_match_deal_teams(host.match.get(), 2, notice, sizeof notice);
    CHECK(std::string(notice) == "Alliances created with 2 teams");
    // Positions 0, 1, 2: slots 0 and 2 share a team.
    CHECK(host.world->game.players[0].alliance[2] == 1);
    CHECK(host.world->game.players[2].alliance[0] == 1);
    CHECK(host.world->game.players[0].alliance[1] == 0);
    const auto sent = host.sent(RecordType::alliance);
    bool asked = false;
    for (const auto& record : sent)
        asked = asked || (record.to == 0x11 && record.bytes[1] == 0x11);
    CHECK(asked);

    host.world->game.players[1].team = 0;
    net_match_deal_teams(host.match.get(), 2, notice, sizeof notice);
    CHECK(std::string(notice) == "+autoteam not available b/c players have team selections");
}

} // namespace

int main() {
    start_positions_keep_team_mates_together();
    start_positions_put_computers_last_on_neutral_maps();
    every_machine_works_out_the_fixed_starts();
    every_machine_puts_the_hosts_computers_last();
    team_records_move_local_alliances();
    alliance_requests_are_carried_out();
    the_host_deals_teams_by_position();
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("net match rules: all tests passed");
    return 0;
}

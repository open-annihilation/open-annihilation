// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/netgame/match/net_match.hpp"

#include "oa/netgame/player_slots.hpp"
#include "oa/netgame/unicode_chat.hpp"
#include "oa/base/text.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/sim/messages.hpp"
#include "oa/sim/speed.hpp"
#include "oa/ui/frontend_multiplayer/team_rules.hpp"

#include <algorithm>
#include <bit>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <utility>

namespace oa::netgame::match {
namespace {

constexpr uint8_t no_slot = OA_PLAYER_COUNT;
constexpr uint8_t reject_departed = 1;
constexpr uint8_t reject_creator_left = 10;
constexpr uint8_t reject_connection_lost = 6;
constexpr uint8_t reject_watching = 9;
constexpr uint32_t economy_send_every = 4;
constexpr uint32_t share_period_ticks = 0x3c;
constexpr uint32_t share_sight_period_ticks = 0x1c2;
constexpr float share_metal_fraction = 0.33333334F;
constexpr float share_energy_fraction = 0.5F;
constexpr uint8_t role_host = 0x01; // PlayerSetupInfo.role bits
constexpr uint8_t role_share_metal = 0x02;
constexpr uint8_t role_share_energy = 0x04;
constexpr uint8_t role_share_sight = 0x20;
constexpr uint32_t give_energy = 1;
constexpr uint32_t give_metal = 2;
constexpr uint32_t give_sight = 3;
constexpr uint8_t pause_speed_kind_speed = 1;
constexpr uint32_t host_machine_group = 1; // Player.machine_group of the host's own machine
constexpr uint32_t final_economy_wait_ms = 250;
constexpr uint8_t gui_flag_refresh = 0x01; // Game.gui_flags bit 0: redraw the lobby panel
// PlayerSetupInfo.options bits the published description holds the player count in.
constexpr uint16_t player_count_option_mask = 0x000f;

// Player bytes the match reads, named for their use here.
/// Returns the player's load progress, 0 to 100, as its last load-progress record gave it.
uint8_t& load_progress(Player& p) {
    return p.load_progress;
} // record 0x2a

/// Returns the player's machine flags; bit 0 is set when the player's machine answers a probe.
uint8_t& probe_flags(Player& p) {
    return p.machine_flags;
} // bit 0: 0x07 arrived

uint8_t& start_position(Player& p) {
    return p.start_position;
}

uint8_t* economy_requested(Player& p) {
    return p.economy_requested;
}

uint8_t* economy_processed(Player& p) {
    return p.economy_processed;
}

uint8_t* economy_answered(Player& p) {
    return p.economy_answered;
}

void bump_rx_count(Player& p) {
    uint32_t count = 0;
    std::memcpy(&count, p.update_count, sizeof count);
    ++count;
    std::memcpy(p.update_count, &count, sizeof count);
}

void clear_rx_count(Player& p) {
    std::memset(p.update_count, 0, sizeof p.update_count);
}

bool is_local(const Player& p) {
    return p.in_use != 0 &&
           (p.status == OA_PLAYER_STATUS_LOCAL || p.status == OA_PLAYER_STATUS_COMPUTER);
}

bool is_remote(const Player& p) {
    return p.in_use != 0 && p.status == OA_PLAYER_STATUS_MIRRORED;
}

// Remote slot whose info still reports it playing (info state 1).
bool remote_playing(World* world, Player& p) {
    const auto* info = world_player_info(world, &p);
    return is_remote(p) && info != nullptr && info->state == 1;
}

/// Tells whether a slot is in use by a local, computer or remote player that holds a player index.
///
/// @param p Player record.
/// @return True for an active slot.
bool slot_active(const Player& p) {
    return p.in_use != 0 &&
           (p.status == OA_PLAYER_STATUS_LOCAL || p.status == OA_PLAYER_STATUS_COMPUTER ||
            p.status == OA_PLAYER_STATUS_MIRRORED) &&
           p.index != no_slot;
}

/// Tells whether an active slot still takes part in the game: it holds units, or has never built any.
///
/// @param p Player record.
/// @return False for an inactive slot and for a player whose units are all gone.
bool participating(const Player& p) {
    return slot_active(p) && (p.unit_count != 0 || p.units_created == 0);
}

uint8_t slot_of(const World* world, uint32_t id) {
    return player_slot_of(world->game, id);
}

Player* player_of(World* world, uint32_t id) {
    return player_of_id(world->game, id);
}

/// Returns the id of the first player on this machine, human or computer.
///
/// @param world Match world.
/// @return That player's id, or no_player_id.
uint32_t primary_id(const World* world) {
    for (const auto& p : world->game.players)
        if (is_local(p))
            return p.player_id;
    return no_player_id;
}

/// Finds the in-use slot flagged as the game's host (info role bit 0).
///
/// @param world Match world.
/// @return The slot, or no_slot.
uint8_t host_slot(World* world) {
    for (uint8_t i = 0; i < OA_PLAYER_COUNT; ++i) {
        auto& p = world->game.players[i];
        const auto* info = world_player_info(world, &p);
        if (p.in_use != 0 && info != nullptr && (info->role & role_host) != 0)
            return i;
    }
    return no_slot;
}

/// Tells whether the host slot is simulated on this machine.
///
/// @param world Match world.
/// @return True when the host slot is local or computer.
bool host_is_local(World* world) {
    const auto slot = host_slot(world);
    return slot != no_slot && is_local(world->game.players[slot]);
}

uint32_t now_time(const NetMatch* m) {
    return net_connection_time(m->connection);
}

template <class R>
bool send_record(NetMatch* m, uint32_t from, uint32_t to, const R& record) {
    uint8_t bytes[record_length_table[static_cast<uint8_t>(R::type)]];
    std::size_t written = 0;
    if (encode_record(record, bytes, sizeof bytes, &written) != WireError::ok)
        return false;
    return net_match_send(m, from, to, bytes, written);
}

void flush_now(NetMatch* m) {
    packet_layer_flush(m->connection->packets, now_time(m), true);
}

namespace team_rules = ui::frontend_multiplayer::team_rules;

/// The setup rules of the match's profile; 3.1c's without one.
///
/// @param m Running match.
/// @return The rules.
const data::match_rules::SetupRules& setup_rules(const NetMatch* m) {
    static constexpr data::match_rules::SetupRules base{};
    return m->match_rules != nullptr ? m->match_rules->setup : base;
}

/// The team rules of the match's profile; 3.1c's without one.
///
/// @param m Running match.
/// @return The rules.
const data::match_rules::TeamsRules& teams_rules(const NetMatch* m) {
    static constexpr data::match_rules::TeamsRules base{};
    return m->match_rules != nullptr ? m->match_rules->teams : base;
}

/// Returns the match's slots as the team rules read them.
///
/// @param world Match world.
/// @return Each slot's seat, status, watching, setup state, team, alliances and name.
team_rules::TeamSlots team_slots(World* world) {
    team_rules::TeamSlots slots{};
    for (std::size_t i = 0; i < OA_PLAYER_COUNT; ++i) {
        auto& p = world->game.players[i];
        const auto* info = world_player_info(world, &p);
        auto& view = slots[i];
        view.in_use = p.in_use != 0;
        view.status = p.status;
        view.watcher = info != nullptr && (info->options & OA_SETUP_OPTION_WATCHER) != 0;
        view.setup_state = info != nullptr ? info->state : 0;
        view.team = static_cast<int8_t>(p.team);
        std::memcpy(view.alliance.data(), p.alliance, view.alliance.size());
        std::memcpy(view.name.data(), p.name, sizeof p.name);
    }
    return slots;
}

/// Sets a player's alliance with another and announces it, as the team rules do.
///
/// A player this machine runs takes the alliance and sends it to everyone
/// (both-sides word 0); a player on this machine at the other end takes it
/// as from the record. A player of another machine is asked by its
/// machine: the record goes to it from the local player with the
/// both-sides word team_rules::alliance_request.
///
/// @param m Running match.
/// @param a The player whose alliance changes.
/// @param b The other player.
/// @param value 1 allied, 0 not.
void announce_alliance(NetMatch* m, Player& a, Player& b, uint8_t value) {
    auto* world = m->world;
    if (a.index >= OA_PLAYER_COUNT || b.index >= OA_PLAYER_COUNT)
        return;
    AllianceRecord record{};
    record.player_id_a = a.player_id;
    record.player_id_b = b.player_id;
    record.value = value;
    if (is_local(a)) {
        a.alliance[b.index] = value;
        send_record(m, a.player_id, broadcast_destination_id, record);
        if (is_local(b))
            b.allied_by[a.index] = value;
        if (m->hooks.alliance_changed != nullptr)
            m->hooks.alliance_changed(m->hooks.context, world, a.index);
    } else {
        record.both_sides = team_rules::alliance_request;
        const auto& self = world->game.players[world->game.local_player_index];
        send_record(m, self.player_id, a.player_id, record);
    }
    flush_now(m);
}

/// Carries out alliance steps in order.
///
/// @param m Running match.
/// @param steps The steps; team steps are not sent in a match.
void apply_alliance_steps(NetMatch* m, const team_rules::TeamSteps& steps) {
    auto& players = m->world->game.players;
    for (uint16_t i = 0; i < steps.count; ++i) {
        const auto& step = steps.items[i];
        if (step.kind == team_rules::TeamStep::Kind::alliance)
            announce_alliance(m, players[step.from], players[step.to], step.value);
    }
}

/// Draws a value below a bound from the match's rand15 hook.
///
/// @param context The match.
/// @param bound Exclusive upper limit.
/// @return The draw; 0 without a hook.
uint32_t rand_below(void* context, uint32_t bound) {
    auto* m = static_cast<NetMatch*>(context);
    if (m->hooks.rand15 == nullptr || bound == 0)
        return 0;
    return static_cast<uint32_t>(m->hooks.rand15(m->hooks.context)) % bound;
}

/// Queues one record to every player of the battle room from one of its players, before a match owns the
/// connection.
///
/// While machines are shared (Game.shared_machines) it goes to one player of
/// each other machine instead, as net_match_send's broadcasts do.
///
/// @param[in,out] c The battle room's connection.
/// @param game The battle room's players.
/// @param from_id Transport id of the sending player.
/// @param record Record bytes, type byte first.
/// @param size Record length in bytes.
void queue_to_all(
    NetConnection* c, const Game& game, uint32_t from_id, const uint8_t* record, std::size_t size
) {
    if (game.shared_machines == 0) {
        net_connection_send_from(c, from_id, broadcast_destination_id, record, size, false);
        return;
    }
    uint32_t ids[OA_PLAYER_COUNT];
    const auto count = ui::frontend_multiplayer::machine_broadcast_targets(game, ids);
    for (int32_t i = 0; i < count; ++i)
        net_connection_send_from(c, from_id, ids[i], record, size, false);
}

/// Tells whether a battle-room player is simulated here and still in the game: a local or computer
/// player not rejected.
///
/// @param p Player record.
/// @return True for a player whose machine this is.
bool building_here(const Player& p) {
    return is_local(p) && p.reject_reason == 0;
}

/// Asks the host for the machine group of every active player still in group 0 (0x21), as the battle
/// room does.
///
/// A local or computer player asks for a group (a computer player for its
/// human's); a player simulated elsewhere asks for the group the host gave
/// it. On the host's own machine its players take group 1 and nothing is
/// sent; only a host simulated elsewhere is asked.
///
/// @param[in,out] m Running match.
void request_machine_groups(NetMatch* m) {
    auto* world = m->world;
    auto& game = world->game;
    const auto host = host_slot(world);
    const bool hosting = host != no_slot && is_local(game.players[host]);
    for (auto& p : game.players) {
        if (!slot_active(p) || p.machine_group != 0)
            continue;
        MachineGroupRequestRecord request{};
        request.player_id = p.player_id;
        request.same_machine_id = no_player_id;
        if (is_local(p)) {
            if (hosting) {
                p.machine_group = host_machine_group;
                continue;
            }
            request.assign = 1;
            if (p.status == OA_PLAYER_STATUS_COMPUTER)
                request.same_machine_id = game.players[game.local_player_index].player_id;
        }
        if (host == no_slot || !is_remote(game.players[host]))
            continue;
        send_record(m, primary_id(world), game.players[host].player_id, request);
    }
}

/// Retires a departed player's slot.
///
/// Local players stop allying with it and its units are destroyed on this
/// machine (NetMatchHooks::destroy_player_units); unless the game has started
/// and the slot is local, it becomes free (and so does its lobby block). The
/// player count drops, its disconnect status and alliances are cleared and
/// its packet channel is released. A departed host of a started game hands
/// the host role on.
///
/// @param[in,out] m Running match.
/// @param id Transport id of the departed player; inactive slots are ignored.
void slot_departure(NetMatch* m, uint32_t id) {
    auto* world = m->world;
    auto* p = player_of(world, id);
    if (p == nullptr || !slot_active(*p))
        return;
    const auto index = p->index;
    auto* info = world_player_info(world, p);
    const bool was_host = p->in_use != 0 && info != nullptr && (info->role & role_host) != 0;
    for (auto& other : world->game.players)
        if (is_local(other) && index < OA_PLAYER_COUNT) {
            other.allied_by[index] = 0;
            other.alliance[index] = 0;
            if (m->hooks.alliance_changed != nullptr && other.index < OA_PLAYER_COUNT)
                m->hooks.alliance_changed(m->hooks.context, world, other.index);
        }
    if (m->hooks.destroy_player_units != nullptr)
        m->hooks.destroy_player_units(m->hooks.context, world, slot_of(world, id));
    const bool started = (world->game.session_flags & kNetFlagGameStarted) != 0;
    if (!(started && is_local(*p))) {
        // The game mirrors the free status into the lobby block.
        p->status = OA_PLAYER_STATUS_FREE;
        if (info != nullptr)
            info->state = OA_PLAYER_STATUS_FREE;
        p->player_id = no_player_id;
        p->in_use = 0;
        p->machine_group = 0;
    }
    if (world->game.player_count > 0)
        --world->game.player_count;
    if (info != nullptr)
        info->status = static_cast<uint16_t>(info->status & ~OA_SETUP_STATUS_HAS_DISC);
    std::memset(p->alliance, 0, sizeof p->alliance);
    packet_layer_release_peer(m->connection->packets, id);
    if (started && was_host)
        net_match_designate_host(world);
}

/// Broadcasts {0x1b, id, reason} and retires the slot or slots.
///
/// Rejecting the local human player rejects every player of this machine.
/// Rejecting a human simulated elsewhere that is still playing (info state
/// 1) retires every slot of its machine group, giving each the reason;
/// rejecting any other player simulated elsewhere, a computer player among
/// them, retires that slot alone. A slot already rejected sends nothing.
///
/// @param[in,out] m Running match.
/// @param id Transport id of the rejected player.
/// @param reason RejectReason value, stored as the slot's reject reason.
/// @return True when a record was sent.
/// @quirk A human whose machine group is still 0 takes every slot of group 0
///        with it, as 3.1c does.
bool reject_player(NetMatch* m, uint32_t id, uint8_t reason) {
    auto* world = m->world;
    auto* p = player_of(world, id);
    if (p == nullptr)
        return false;
    bool sent = false;
    RejectRecord record{};
    record.reason = reason;
    if (is_local(*p) && p->reject_reason == 0) {
        if (p->status == OA_PLAYER_STATUS_LOCAL) {
            for (auto& other : world->game.players) {
                if (!is_local(other))
                    continue;
                record.player_id = other.player_id;
                send_record(m, primary_id(world), broadcast_destination_id, record);
                slot_departure(m, id);
                other.reject_reason = reason;
            }
        } else {
            record.player_id = p->player_id;
            send_record(m, primary_id(world), broadcast_destination_id, record);
            slot_departure(m, p->player_id);
        }
        sent = true;
    } else if (is_remote(*p) && p->reject_reason == 0) {
        record.player_id = id;
        sent = send_record(m, primary_id(world), broadcast_destination_id, record);
        if (remote_playing(world, *p)) {
            const auto group = p->machine_group;
            for (auto& other : world->game.players)
                if (other.machine_group == group) {
                    slot_departure(m, other.player_id);
                    other.reject_reason = reason;
                }
        } else {
            slot_departure(m, p->player_id);
        }
    }
    p->reject_reason = reason;
    return sent;
}

/// Retires a player another machine reported disconnected and passes the 0x1c notice on.
///
/// The notice goes out again from the local player to every player. The slot
/// is retired unless it is local and loading is under way. Only a received
/// notice gets here: a machine leaving the game sends none for its own
/// players, as closing the session tells the others. A notice naming this
/// machine's first local human player then ends the local game
/// (NetMatchHooks::end_local_game).
///
/// @param[in,out] m Running match.
/// @param id Transport id of the disconnected player; unknown ids are ignored.
void disconnect_notice(NetMatch* m, uint32_t id) {
    auto* world = m->world;
    const auto slot = slot_of(world, id);
    if (slot == no_slot)
        return;
    auto& p = world->game.players[slot];
    const auto load = world->game.load_flags;
    if (is_remote(p) || (load & load_flag_started) == 0 || (load & load_flag_loader_done) != 0)
        slot_departure(m, id);
    DisconnectNoticeRecord record{};
    record.player_id = id;
    const auto local = world->game.players[world->game.local_player_index].player_id;
    send_record(m, local, broadcast_destination_id, record);
    if (id == first_local_player_id(world->game) && m->hooks.end_local_game != nullptr)
        m->hooks.end_local_game(m->hooks.context);
}

void notice(NetMatch* m, const char* text) {
    if (m->hooks.notice != nullptr)
        m->hooks.notice(m->hooks.context, text);
}

/// Answers a 0x02 ping request, or records the round trip of a local player's answered ping.
///
/// A request is echoed with this machine's clock to its originator, from the
/// player it was addressed to (the local player for one sent to all), without
/// guaranteed delivery; an answer stores the elapsed milliseconds as the
/// sender's ping.
///
/// @param[in,out] m Running match.
/// @param[in,out] from Sender's player record.
/// @param to Addressee's player record.
/// @param data Record bytes.
/// @param size Record length in bytes.
void handle_ping(
    NetMatch* m, Player& from, const Player& to, const uint8_t* data, std::size_t size
) {
    PingRecord ping{};
    if (decode_record(data, size, &ping) != WireError::ok)
        return;
    const auto now_ms =
        m->connection->host != nullptr ? sock::host_now_ms(m->connection->host) : 0u;
    if (ping.echo_tick_count == 0) {
        ping.echo_tick_count = now_ms;
        auto* packets = m->connection->packets;
        flush_now(m);
        const bool guaranteed = packets->guaranteed;
        packets->guaranteed = false;
        send_record(m, to.player_id, ping.origin_player_id, ping);
        flush_now(m);
        packets->guaranteed = guaranteed;
        return;
    }
    if (const auto* origin = player_of(m->world, ping.origin_player_id);
        origin != nullptr && is_local(*origin))
        from.latency = static_cast<int32_t>(now_ms - ping.origin_tick_count);
}

/// Applies a 0x28 economy record and answers it when the sender wants a reply.
///
/// The sender's scores and resources are stored unless a local player has
/// already processed its final economy. A reply (0x29) goes back from each
/// local player, followed by that player's own economy when not yet sent,
/// unless the local player has already won (NetMatchHooks::local_player_won).
///
/// @param[in,out] m Running match.
/// @param[in,out] sender Sender's player record; ignored once rejected.
/// @param data Record bytes.
/// @param size Record length in bytes.
void apply_economy(NetMatch* m, Player& sender, const uint8_t* data, std::size_t size) {
    EconomyRecord record{};
    if (decode_record(data, size, &record) != WireError::ok || sender.reject_reason != 0)
        return;
    auto* world = m->world;
    const auto index = sender.index;
    if (index >= OA_PLAYER_COUNT)
        return;
    bool processed = false;
    for (auto& p : world->game.players)
        if (is_local(p) && economy_processed(p)[index] != 0)
            processed = true;
    if (!processed) {
        sender.kills = static_cast<int16_t>(record.kills);
        sender.losses = static_cast<int16_t>(record.losses);
        sender.commanders_killed = static_cast<int16_t>(record.commanders_killed);
        sender.commanders_lost = static_cast<int16_t>(record.commanders_lost);
        sender.metal = record.metal;
        sender.energy = record.energy;
        sender.metal_storage = record.metal_storage;
        sender.energy_storage = record.energy_storage;
        sender.energy_produced_total = record.energy_produced_total;
        sender.energy_requested_total = record.energy_requested_total;
        sender.energy_wasted_total = record.energy_wasted_total;
        sender.metal_produced_total = record.metal_produced_total;
        sender.metal_requested_total = record.metal_requested_total;
        sender.metal_wasted_total = record.metal_wasted_total;
    }
    if (record.want_reply == 0)
        return;
    for (auto& p : world->game.players) {
        if (!is_local(p) || p.reject_reason != 0)
            continue;
        economy_processed(p)[index] = 1;
        EconomyReplyRecord reply{};
        reply.mark_requested = 1;
        reply.mark_answered = economy_requested(p)[index] != 0 ? 1 : 0;
        send_record(m, p.player_id, sender.player_id, reply);
        const bool won =
            m->hooks.local_player_won != nullptr && m->hooks.local_player_won(m->hooks.context);
        if (!won && economy_requested(p)[index] == 0) {
            EconomyRecord own{};
            own.want_reply = 1;
            own.kills = p.kills;
            own.losses = p.losses;
            own.commanders_killed = p.commanders_killed;
            own.commanders_lost = p.commanders_lost;
            own.metal = p.metal;
            own.energy = p.energy;
            own.metal_storage = p.metal_storage;
            own.energy_storage = p.energy_storage;
            own.energy_produced_total = static_cast<float>(p.energy_produced_total);
            own.energy_requested_total = static_cast<float>(p.energy_requested_total);
            own.energy_wasted_total = static_cast<float>(p.energy_wasted_total);
            own.metal_produced_total = static_cast<float>(p.metal_produced_total);
            own.metal_requested_total = static_cast<float>(p.metal_requested_total);
            own.metal_wasted_total = static_cast<float>(p.metal_wasted_total);
            if (sender.reject_reason == 0)
                send_record(m, p.player_id, sender.player_id, own);
        }
    }
}

/// Sends a local player's economy snapshot (0x28) to one player, or to every human simulated elsewhere
/// that is still playing.
///
/// @param[in,out] m Running match.
/// @param from Local player whose economy is sent; rejected players send nothing.
/// @param to Receiving player, or null for every human still playing on another machine and not rejected.
/// @param want_reply Nonzero asks the receiver to answer.
void send_economy(NetMatch* m, Player& from, Player* to, uint8_t want_reply) {
    if (!is_local(from) || from.reject_reason != 0)
        return;
    EconomyRecord r{};
    r.want_reply = want_reply;
    r.kills = from.kills;
    r.losses = from.losses;
    r.commanders_killed = from.commanders_killed;
    r.commanders_lost = from.commanders_lost;
    r.metal = from.metal;
    r.energy = from.energy;
    r.metal_storage = from.metal_storage;
    r.energy_storage = from.energy_storage;
    r.energy_produced_total = static_cast<float>(from.energy_produced_total);
    r.energy_requested_total = static_cast<float>(from.energy_requested_total);
    r.energy_wasted_total = static_cast<float>(from.energy_wasted_total);
    r.metal_produced_total = static_cast<float>(from.metal_produced_total);
    r.metal_requested_total = static_cast<float>(from.metal_requested_total);
    r.metal_wasted_total = static_cast<float>(from.metal_wasted_total);
    if (to != nullptr) {
        if (to->reject_reason == 0)
            send_record(m, from.player_id, to->player_id, r);
        return;
    }
    for (auto& p : m->world->game.players)
        if (remote_playing(m->world, p) && p.reject_reason == 0)
            send_record(m, from.player_id, p.player_id, r);
}

/// Applies a 0x16 resource give: credits energy or metal, or shares sight.
///
/// @param[in,out] m Running match.
/// @param data Record bytes.
/// @param size Record length in bytes.
void apply_give(NetMatch* m, const uint8_t* data, std::size_t size) {
    ResourceGiveRecord r{};
    if (decode_record(data, size, &r) != WireError::ok)
        return;
    const auto from = slot_of(m->world, r.from_id);
    const auto to = slot_of(m->world, r.to_id);
    if (from == no_slot || to == no_slot)
        return;
    if ((r.subtype == give_energy || r.subtype == give_metal) && m->hooks.credit != nullptr)
        m->hooks.credit(m->hooks.context, m->world, to, r.subtype == give_metal, r.amount);
    else if (r.subtype == give_sight && m->hooks.share_sight != nullptr)
        m->hooks.share_sight(m->hooks.context, m->world, from, to);
}

/// Copies a player's name into a slot's name field, cut so that it keeps its terminator.
///
/// @param[out] field Name field of player_name_copy_bytes.
/// @param name The name.
void copy_slot_name(char* field, const char* name) {
    std::size_t n = 0;
    for (; name[n] != '\0' && n + 1 < player_name_copy_bytes; ++n)
        field[n] = name[n];
    std::memset(field + n, 0, player_name_copy_bytes - n);
}

/// Handles a player name system message: the player's slot takes the new
/// long name as its name and the new short name as its second name.
///
/// A name the message does not carry leaves its field as it was.
///
/// @param[in,out] m Running match.
/// @param packet The system message.
void apply_player_name(NetMatch* m, const Packet& packet) {
    dplay::PlayerNameView view{};
    if (!dplay::decode_player_name_image(packet.data, packet.size, &view))
        return;
    auto* p = player_of(m->world, view.id);
    if (p == nullptr)
        return;
    if (view.long_name != nullptr)
        copy_slot_name(p->name, view.long_name);
    if (view.short_name != nullptr)
        copy_slot_name(p->second_name, view.short_name);
}

/// Handles a player data system message: the data replaces the player's
/// setup block. When this machine's player is the game's host, its game does
/// not allow watching and the new block makes the player a watcher, the
/// player is rejected with reason 9.
///
/// Data shorter than the block replaces only its first bytes.
///
/// @param[in,out] m Running match.
/// @param packet The system message.
void apply_player_data(NetMatch* m, const Packet& packet) {
    dplay::PlayerDataView view{};
    if (!dplay::decode_player_data_image(packet.data, packet.size, &view))
        return;
    auto* world = m->world;
    auto* p = player_of(world, view.id);
    auto* info = p != nullptr ? world_player_info(world, p) : nullptr;
    if (info == nullptr)
        return;
    const std::size_t size =
        view.data_size < player_data_block_bytes ? view.data_size : player_data_block_bytes;
    if (size != 0)
        std::memcpy(static_cast<void*>(info), view.data, size);
    const auto local = world->game.local_player_index;
    if (host_slot(world) != local)
        return;
    const auto* host_info = world_player_info(world, &world->game.players[local]);
    if (host_info != nullptr && (host_info->options & OA_SETUP_OPTION_WATCHING_ALLOWED) == 0 &&
        (info->options & OA_SETUP_OPTION_WATCHER) != 0)
        reject_player(m, view.id, reject_watching);
}

/// Handles a system message addressed to this machine's player.
///
/// A destroyed player departs; when the game's creator leaves before the
/// game started, the local player is rejected too (creator left). A
/// player's name and data changes are applied to its slot. Other messages
/// are ignored, among them this machine becoming the session's name server.
///
/// @param[in,out] m Running match.
/// @param packet The system message.
void dispatch_system(NetMatch* m, const Packet& packet) {
    uint32_t message_type = 0;
    if (!dplay::read_system_message_type(packet.data, packet.size, &message_type))
        return;
    const auto type = static_cast<SystemMessageType>(message_type);
    if (type == SystemMessageType::player_name_changed) {
        apply_player_name(m, packet);
        return;
    }
    if (type == SystemMessageType::player_data_changed) {
        apply_player_data(m, packet);
        return;
    }
    dplay::PlayerDestroyedView destroyed{};
    if (!dplay::decode_player_destroyed_image(packet.data, packet.size, &destroyed) ||
        destroyed.player_type != dplay::system_message::player_type_player)
        return;
    auto* world = m->world;
    const auto id = destroyed.id;
    auto* p = player_of(world, id);
    if (p == nullptr || !slot_active(*p))
        return;
    const auto* info = world_player_info(world, p);
    const bool creator = info != nullptr && (info->role & 0x01) != 0;
    if ((world->game.session_flags & kNetFlagGameStarted) == 0 && creator && is_remote(*p)) {
        reject_player(m, id, reject_departed);
        auto& local = world->game.players[world->game.local_player_index];
        reject_player(m, local.player_id, reject_creator_left);
        local.status = OA_PLAYER_STATUS_FREE;
    } else {
        reject_player(m, id, reject_departed);
    }
    p->status = OA_PLAYER_STATUS_FREE;
    packet_layer_release_peer(m->connection->packets, id);
}

// ---------------------------------------------------------------------------
// The network rules of a mod profile (NetMatch::rules). With 3.1c's rules
// none of this sends or changes anything.

constexpr uint32_t time_ticks_per_second = 30;
bool watcher(World* world, Player& p);
// A chat line keeps a NUL in its last byte, so a receiver never reads past it.
constexpr std::size_t chat_line_chars = sizeof(ChatRecord::text) - 1;
constexpr std::size_t notice_bytes = 96;
// A watch asked for while seated starts this long after the command.
constexpr uint32_t fake_watch_delay_ticks = 5 * time_ticks_per_second;

// Bytes past a cut that show whether a UTF-8 character spans it.
constexpr std::size_t chat_lookahead = 4;

/// Gives how much of a line fits a limit without cutting a character.
///
/// @param line The line, zero-terminated.
/// @param limit The most bytes kept.
/// @return The bytes kept.
std::size_t chat_bytes_within(const char* line, std::size_t limit) {
    return oa::base::text::whole_characters(
        std::string_view(line, ::strnlen(line, limit + chat_lookahead)), limit
    );
}

// The most bytes of a line Unicode chat reads: more than four records hold.
constexpr std::size_t chat_line_read_bytes = 512;

/// Returns one of the game's own texts in the language shown (NetMatchHooks::translate_game_text).
///
/// @param m Running match.
/// @param english The text as the game's translate.tdf keys it.
/// @return The translation, valid until the next lookup; or the English.
const char* game_text(const NetMatch* m, const char* english) {
    const char* translated = m->hooks.translate_game_text != nullptr
                                 ? m->hooks.translate_game_text(m->hooks.context, english)
                                 : nullptr;
    return translated != nullptr ? translated : english;
}

/// Puts a player's name into a translated line where its "%s" stands.
///
/// The line is copied as written, except that its first "%s" becomes the
/// name and each "%%" a '%'; any other '%' sequence shows as written, so
/// the line is never read as a format.
///
/// @param text The line, as its translation writes it.
/// @param name The player's name.
/// @return The line with the name in it.
/// @quirk A line without "%s" leaves the name out, as 3.1c's German and
///        French disconnection lines do.
std::string with_name(const char* text, std::string_view name) {
    std::string line;
    bool named = false;
    for (const char* at = text; *at != '\0'; ++at) {
        if (at[0] == '%' && at[1] == '%') {
            line += '%';
            ++at;
        } else if (!named && at[0] == '%' && at[1] == 's') {
            line += name;
            named = true;
            ++at;
        } else {
            line += *at;
        }
    }
    return line;
}

/// Returns one of the engine's own notices in the language shown, from the
/// interface catalogue, with each "{field}" filled in.
///
/// The fields are filled in one pass over the notice, so a field's text
/// shows as it is even when it holds braces, as a player's name may. A
/// field the list does not name shows as written. The line keeps the most
/// bytes a notice holds, cut between whole characters.
///
/// @param english The notice in English, with its fields in braces.
/// @param fields Each field's name and text.
/// @return The notice.
std::string own_words(
    std::string_view english, std::initializer_list<std::pair<std::string_view, std::string>> fields
) {
    const std::string_view text = oa::data::languages::interface_text(english);
    std::string line;
    std::size_t at = 0;
    while (at < text.size()) {
        const std::size_t open = text.find('{', at);
        const std::size_t close =
            open == std::string_view::npos ? std::string_view::npos : text.find('}', open);
        if (close == std::string_view::npos) {
            line.append(text.substr(at));
            break;
        }
        line.append(text.substr(at, open - at));
        const std::string_view name = text.substr(open + 1, close - open - 1);
        const auto* field = std::find_if(fields.begin(), fields.end(), [&](const auto& entry) {
            return entry.first == name;
        });
        if (field != fields.end()) {
            line += field->second;
            at = close + 1;
        } else {
            line += '{';
            at = open + 1;
        }
    }
    line.resize(oa::base::text::whole_characters(line, notice_bytes - 1));
    return line;
}

/// Returns a player's name as a string.
///
/// @param p Player record.
/// @return The name, at most the field's size.
std::string name_of(const Player& p) {
    return std::string(p.name, ::strnlen(p.name, sizeof p.name));
}

/// Tells whether a player's machine reads chat as UTF-8: its setup block says so.
///
/// @param world Match world.
/// @param p The player.
/// @return True when it does.
bool reads_unicode_chat(const World* world, const Player& p) {
    return p.index < OA_PLAYER_COUNT &&
           announces_unicode_chat(reinterpret_cast<const uint8_t*>(&world->player_info[p.index]));
}

/// Sends one chat record to a destination in the form each machine there
/// reads; a broadcast reaching machines of both kinds goes as one copy per
/// machine. Every copy goes with NetMatch::sending_copies set.
///
/// @param[in,out] m Running match.
/// @param from Sending player id.
/// @param to Destination id, or broadcast_destination_id.
/// @param utf8 The record in UTF-8.
/// @param code_page The record in the code page.
void send_chat_forms(
    NetMatch* m, uint32_t from, uint32_t to, const ChatRecord& utf8, const ChatRecord& code_page
) {
    auto* world = m->world;
    const auto reads = [&](uint32_t id) {
        const auto* p = player_of(world, id);
        return p != nullptr && reads_unicode_chat(world, *p);
    };
    m->sending_copies = true;
    if (to != broadcast_destination_id) {
        send_record(m, from, to, reads(to) ? utf8 : code_page);
        m->sending_copies = false;
        return;
    }
    uint32_t ids[OA_PLAYER_COUNT];
    int32_t count = 0;
    if (world->game.shared_machines != 0) {
        count = ui::frontend_multiplayer::machine_broadcast_targets(world->game, ids);
    } else {
        for (const auto& p : world->game.players)
            if (is_remote(p) && p.reject_reason == 0)
                ids[count++] = p.player_id;
    }
    int32_t readers = 0;
    for (int32_t i = 0; i < count; ++i)
        readers += reads(ids[i]) ? 1 : 0;
    if (readers == 0 || readers == count) {
        send_record(m, from, broadcast_destination_id, readers == 0 ? code_page : utf8);
    } else {
        for (int32_t i = 0; i < count; ++i) {
            m->sending_copies = true;
            send_record(m, from, ids[i], reads(ids[i]) ? utf8 : code_page);
        }
    }
    m->sending_copies = false;
}

/// Sends a chat line while Unicode chat is on: read as UTF-8, cut into
/// records, each record in the form each machine reads. A recording keeps
/// one UTF-8 copy of each record.
///
/// @param[in,out] m Running match.
/// @param from Sending player id.
/// @param to Destination ids, broadcast_destination_id among them for everyone.
/// @param text The line, as this machine holds game text.
/// @param record_bytes The bytes one record keeps of the line.
/// @param split A line that is no command goes as up to chat_line_parts records.
/// @return The parts sent, in UTF-8.
std::vector<std::string> say_unicode(
    NetMatch* m,
    uint32_t from,
    std::span<const uint32_t> to,
    const char* text,
    std::size_t record_bytes,
    bool split
) {
    const auto line = chat_utf8(std::string_view(text, ::strnlen(text, chat_line_read_bytes)));
    std::vector<std::string> parts;
    if (split && !chat_command(line))
        parts = chat_parts(line, record_bytes, chat_line_parts);
    else
        parts.push_back(line.substr(0, oa::base::text::whole_characters(line, record_bytes)));
    const auto* self = player_of(m->world, from);
    const bool recorded = m->hooks.record_seen != nullptr && !to.empty() && self != nullptr &&
                          is_local(*self) && self->reject_reason == 0 &&
                          (m->world->game.session_flags & kNetFlagLive) != 0;
    for (const auto& part : parts) {
        ChatRecord utf8{};
        std::memcpy(utf8.text, part.data(), std::min(part.size(), sizeof utf8.text));
        const auto narrow = chat_code_page(part);
        ChatRecord code_page{};
        std::memcpy(code_page.text, narrow.data(), std::min(narrow.size(), sizeof code_page.text));
        if (std::memcmp(utf8.text, code_page.text, sizeof utf8.text) == 0) {
            for (const auto id : to)
                send_record(m, from, id, utf8);
            continue;
        }
        if (recorded) {
            uint8_t bytes[record_length_table[static_cast<uint8_t>(RecordType::chat)]];
            std::size_t written = 0;
            if (encode_record(utf8, bytes, sizeof bytes, &written) == WireError::ok)
                m->hooks.record_seen(m->hooks.context, from, bytes, written);
        }
        for (const auto id : to)
            send_chat_forms(m, from, id, utf8, code_page);
    }
    return parts;
}

/// Broadcasts a plain chat line from the first local player.
///
/// @param[in,out] m Running match.
/// @param line The line; cut at 63 bytes, between whole characters.
void say_to_all(NetMatch* m, const char* line) {
    const auto from = first_local_player_id(m->world->game);
    if (m->unicode_chat) {
        const uint32_t everyone = broadcast_destination_id;
        (void)say_unicode(m, from, {&everyone, 1}, line, chat_line_chars, false);
        return;
    }
    ChatRecord r{};
    std::memcpy(r.text, line, chat_bytes_within(line, chat_line_chars));
    send_record(m, from, broadcast_destination_id, r);
}

/// Sends a private message from the first local player.
///
/// @param[in,out] m Running match.
/// @param to Destination transport id; broadcast_destination_id for everyone.
/// @param message The message.
/// @return True when it was queued.
bool send_private(NetMatch* m, uint32_t to, const PrivateMessage& message) {
    uint8_t bytes[private_record_bytes];
    std::size_t written = 0;
    if (encode_private_message(message, bytes, sizeof bytes, &written) != WireError::ok)
        return false;
    return net_match_send(m, first_local_player_id(m->world->game), to, bytes, written);
}

/// Returns a player's name as a bounded view for printing.
///
/// @param p Player record.
/// @return The name's length, at most the field's size.
int name_length(const Player& p) {
    return static_cast<int>(::strnlen(p.name, sizeof p.name));
}

/// Returns the first local human player.
///
/// @param world Match world.
/// @return The player, or null.
Player* first_local_human(World* world) {
    for (auto& p : world->game.players)
        if (p.in_use != 0 && p.status == OA_PLAYER_STATUS_LOCAL)
            return &p;
    return nullptr;
}

// ---- integrity check

/// Answers a challenge: the program's and the game data's keyed digests, back to back, to the challenger.
///
/// @param[in,out] m Running match.
/// @param from The challenger.
/// @param message The challenge.
void integrity_reply(NetMatch* m, const Player& from, const PrivateMessage& message) {
    uint8_t nonce[integrity_nonce_bytes];
    std::memcpy(nonce, message.payload, sizeof nonce);
    uint8_t answer[integrity_answer_bytes];
    PrivateMessage reply{};
    reply.sub_id = private_sub_integrity;
    reply.op = integrity_op_module_reply;
    integrity_answer(nonce, m->integrity.identity.program, answer);
    std::memcpy(reply.payload, answer, sizeof answer);
    send_private(m, from.player_id, reply);
    reply = PrivateMessage{};
    reply.sub_id = private_sub_integrity;
    reply.op = integrity_op_data_reply;
    integrity_answer(nonce, m->integrity.identity.game_data, answer);
    std::memcpy(reply.payload, answer, sizeof answer);
    send_private(m, from.player_id, reply);
}

/// Handles an integrity check message from a remote player.
///
/// @param[in,out] m Running match.
/// @param from The sender.
/// @param message The message.
void handle_integrity(NetMatch* m, const Player& from, const PrivateMessage& message) {
    if (m->rules.integrity_check == IntegrityCheck::off)
        return;
    if (message.op == integrity_op_challenge) {
        integrity_reply(m, from, message);
        return;
    }
    if (message.op == integrity_op_module_reply || message.op == integrity_op_data_reply) {
        if (auto* peer = integrity_peer(m->integrity, from.player_id))
            integrity_note_answer(*peer, message.op, message.payload, m->integrity.identity);
        return;
    }
    if (message.op >= integrity_op_first_report && message.op <= integrity_op_last_report) {
        // A report request: every machine says what program it runs.
        const auto* self = first_local_human(m->world);
        if (self == nullptr || m->recorder.program[0] == '\0')
            return;
        char line[notice_bytes];
        std::snprintf(
            line,
            sizeof line,
            "*** %.*s uses %s",
            name_length(*self),
            self->name,
            m->recorder.program
        );
        say_to_all(m, line);
    }
}

/// Challenges every other human still playing, each with fresh random bytes, unless it already answered
/// in full before the retries end.
///
/// @param[in,out] m Running match.
void integrity_challenge(NetMatch* m) {
    auto* world = m->world;
    const auto tick = world->game.tick;
    for (auto& p : world->game.players) {
        if (!remote_playing(world, p) || p.reject_reason != 0 || watcher(world, p))
            continue;
        auto* peer = integrity_peer(m->integrity, p.player_id);
        if (peer == nullptr)
            continue;
        if (peer->challenged && peer->program_answered && peer->data_answered &&
            tick < integrity_retry_end_tick)
            continue;
        PrivateMessage challenge{};
        challenge.sub_id = private_sub_integrity;
        challenge.op = integrity_op_challenge;
        for (std::size_t i = 0; i < integrity_nonce_bytes; ++i) {
            const auto draw = m->hooks.rand15 != nullptr ? m->hooks.rand15(m->hooks.context) : 0;
            challenge.payload[i] = static_cast<uint8_t>(draw ^ (draw >> 8));
        }
        std::memcpy(peer->nonce, challenge.payload, integrity_nonce_bytes);
        peer->challenged = true;
        peer->program_answered = false;
        peer->data_answered = false;
        send_private(m, p.player_id, challenge);
    }
}

/// Reports, at tick 600, how many players failed the check: a notice here and a chat line to everyone.
///
/// @param[in,out] m Running match.
void integrity_report(NetMatch* m) {
    m->integrity.reported = true;
    const auto issues = integrity_issue_count(m->integrity);
    if (issues == 0)
        return;
    const auto* self = first_local_human(m->world);
    if (self == nullptr)
        return;
    // The notice is in the language shown; the chat line goes out in English.
    notice(
        m,
        own_words(
            "{player} reports VerCheck issues with {count} other players",
            {{"player", name_of(*self)}, {"count", std::to_string(issues)}}
        ).c_str()
    );
    char line[notice_bytes];
    std::snprintf(
        line,
        sizeof line,
        "%.*s reports VerCheck issues with %d other players",
        name_length(*self),
        self->name,
        issues
    );
    say_to_all(m, line);
}

// ---- votes to reject

/// Returns how long a vote stays open, in connection time.
///
/// @param m Running match.
/// @param flag vote_flag_manual or vote_flag_timeout
/// @return The window.
uint32_t vote_window(const NetMatch* m, uint8_t flag) {
    const uint32_t seconds =
        flag == vote_flag_timeout ? m->rules.timeout_vote_seconds : m->rules.vote_seconds;
    return seconds * time_ticks_per_second;
}

/// Counts who votes on a player: every slot with a player, and the target's allies both ways.
///
/// The target of a timeout vote, which has stopped answering, is not
/// counted.
///
/// @param world Match world.
/// @param vote The vote.
/// @param target The player the vote would reject.
/// @return The electorate.
VoteElectorate electorate_of(World* world, const Vote& vote, const Player& target) {
    VoteElectorate out{};
    for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot) {
        const auto& p = world->game.players[slot];
        if (p.in_use == 0 || p.player_id == 0 || p.player_id == no_player_id)
            continue;
        if (vote.flag == vote_flag_timeout && &p == &target)
            continue;
        ++out.count;
        if (&p != &target && target.index < OA_PLAYER_COUNT && p.alliance[target.index] != 0 &&
            target.alliance[slot] != 0)
            out.target_allies = static_cast<uint16_t>(out.target_allies | (1u << slot));
    }
    return out;
}

/// Shows a vote as it opens or changes.
///
/// @param[in,out] m Running match.
/// @param vote The vote.
void show_vote(NetMatch* m, const Vote& vote) {
    auto* target = player_of(m->world, vote.target_id);
    if (target == nullptr || target->index >= OA_PLAYER_COUNT)
        return;
    const auto electorate = electorate_of(m->world, vote, *target);
    const auto needed = vote_needed(vote, electorate.count);
    if (m->hooks.vote_shown != nullptr)
        m->hooks.vote_shown(m->hooks.context, vote, target->index, needed, electorate.count);
    notice(
        m,
        own_words(
            vote.flag == vote_flag_timeout
                ? "Timeout: reject {player} ({yes} yes/{no} no/{voters}, {seconds}s)"
                : "Vote: reject {player} ({yes} yes/{no} no/{voters}, {seconds}s)",
            {{"player", name_of(*target)},
             {"yes", std::to_string(std::popcount(vote.yes))},
             {"no", std::to_string(std::popcount(vote.no))},
             {"voters", std::to_string(static_cast<int>(electorate.count))},
             {"seconds", std::to_string(vote_seconds_left(vote, now_time(m)))}}
        ).c_str()
    );
}

/// Opens a vote on a player and asks every machine to vote.
///
/// @param[in,out] m Running match.
/// @param target The player the vote would reject.
/// @param flag vote_flag_manual or vote_flag_timeout
/// @return The vote, or null when none opened.
Vote* propose_vote(NetMatch* m, const Player& target, uint8_t flag) {
    auto* vote = vote_open(m->votes, target.player_id, flag, now_time(m), vote_window(m, flag));
    if (vote == nullptr)
        return nullptr;
    PrivateMessage message{};
    message.sub_id = private_sub_vote;
    message.op = vote_op_propose;
    store_u32(message.payload + vote_target_offset, target.player_id);
    message.payload[vote_flag_offset] = flag;
    send_private(m, broadcast_destination_id, message);
    return vote;
}

void tally_votes(NetMatch* m);

/// Handles a vote message from a remote player.
///
/// @param[in,out] m Running match.
/// @param from The sender.
/// @param message The message.
void handle_vote(NetMatch* m, const Player& from, const PrivateMessage& message) {
    if (!m->rules.vote_reject || from.index >= OA_PLAYER_COUNT)
        return;
    const auto target_id = load_u32(message.payload + vote_target_offset);
    const auto flag = message.payload[vote_flag_offset];
    if (message.op == vote_op_propose) {
        auto* target = player_of(m->world, target_id);
        if (target == nullptr || !slot_active(*target))
            return;
        // A machine never takes part in a timeout vote on its own player.
        if (flag == vote_flag_timeout && is_local(*target))
            return;
        auto* vote = vote_open(m->votes, target_id, flag, now_time(m), vote_window(m, flag));
        if (vote == nullptr)
            return;
        if (flag == vote_flag_manual)
            vote_cast(*vote, from.index, true);
        show_vote(m, *vote);
        tally_votes(m);
        return;
    }
    if (message.op == vote_op_yes || message.op == vote_op_no)
        if (auto* vote = vote_find(m->votes, target_id)) {
            vote_cast(*vote, from.index, message.op == vote_op_yes);
            show_vote(m, *vote);
            tally_votes(m);
        }
}

/// Counts every open vote; a vote that passed rejects its player here, as on every machine.
///
/// @param[in,out] m Running match.
void tally_votes(NetMatch* m) {
    const auto now = now_time(m);
    for (auto& vote : m->votes.votes) {
        if (vote.target_id == 0)
            continue;
        auto* target = player_of(m->world, vote.target_id);
        if (target == nullptr || !slot_active(*target) || target->reject_reason != 0) {
            vote = Vote{};
            continue;
        }
        const auto result = vote_tally(vote, electorate_of(m->world, vote, *target), now);
        if (result == VoteResult::open)
            continue;
        const auto target_id = vote.target_id;
        const auto reason = vote.flag;
        const std::string line = own_words(
            result == VoteResult::passed ? "{player} rejected" : "{player} stays in the game",
            {{"player", name_of(*target)}}
        );
        vote_close(m->votes, vote, result, now);
        notice(m, line.c_str());
        if (result == VoteResult::passed)
            reject_player(m, target_id, reason);
    }
}

/// Drops the timeout vote on a player heard from again.
///
/// @param[in,out] m Running match.
/// @param heard_id Transport id of the player heard from.
void cancel_timeout_vote(NetMatch* m, uint32_t heard_id) {
    auto* vote = vote_find(m->votes, heard_id);
    if (vote != nullptr && vote->flag == vote_flag_timeout)
        *vote = Vote{};
}

// ---- recorder

/// Tells whether a player runs the recorder, as its battle-room setup block said.
///
/// @param m Running match.
/// @param slot Player slot.
/// @return True for a recorder peer.
bool recorder_peer(const NetMatch* m, uint8_t slot) {
    return slot < OA_PLAYER_COUNT && m->recorder.peer_protocol[slot] != recorder_protocol_plain;
}

/// Sends one of the recorder's own records alone, to one player.
///
/// @param[in,out] m Running match.
/// @param to Destination transport id.
/// @param record The record.
/// @param size Its length.
void send_recorder_alone(NetMatch* m, uint32_t to, const uint8_t* record, std::size_t size) {
    flush_now(m);
    net_match_send(m, first_local_player_id(m->world->game), to, record, size);
    flush_now(m);
}

/// Tells whether a slot holds a computer player, here or on another machine.
///
/// @param world Match world.
/// @param p Player record.
/// @return True for a computer player.
bool computer_player(World* world, const Player& p) {
    if (p.status == OA_PLAYER_STATUS_COMPUTER)
        return true;
    const auto* info = world_player_info(world, &p);
    return is_remote(p) && info != nullptr && info->state == OA_PLAYER_STATUS_COMPUTER;
}

/// Tells whether every player in the game has finished its commander warp.
///
/// Computer players place no commander, so only the humans count.
///
/// @param m Running match.
/// @return True once each active human non-watcher has.
bool all_warps_done(NetMatch* m) {
    for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot) {
        auto& p = m->world->game.players[slot];
        if (!slot_active(p) || watcher(m->world, p) || p.reject_reason != 0 ||
            computer_player(m->world, p))
            continue;
        if (!m->recorder.warp_done[slot])
            return false;
    }
    return true;
}

/// Tells whether this machine shows a player's whiteboard marks: the
/// player allies this machine's player (Player.allied_by, from its last
/// alliance record or the battle room's alliances) or, under
/// sharing.recorder-take-give, typed .give naming it.
///
/// @param m Running match.
/// @param slot The marks' sender.
/// @return True when the marks are shown.
bool whiteboard_marks_shown(NetMatch* m, uint8_t slot) {
    const auto* self = first_local_human(m->world);
    if (self == nullptr || slot >= OA_PLAYER_COUNT)
        return false;
    return self->allied_by[slot] != 0 || m->take.granted[slot];
}

/// Handles a recorder record from a remote player.
///
/// @param[in,out] m Running match.
/// @param from The sender.
/// @param data The record.
/// @param size Its length.
void handle_recorder_record(
    NetMatch* m, const Player& from, const uint8_t* data, std::size_t size
) {
    ++m->recorder_records;
    const auto slot = from.index;
    if (slot >= OA_PLAYER_COUNT || size == 0)
        return;
    switch (static_cast<RecorderRecordType>(data[0])) {
    case RecorderRecordType::camera:
        if (size >= recorder_camera_bytes) {
            m->recorder.camera_x[slot] = load_u16(data + 1);
            m->recorder.camera_y[slot] = load_u16(data + 3);
            // Both halves off: the sender stopped sharing its camera.
            m->recorder.camera_shared[slot] = m->recorder.camera_x[slot] != recorder_camera_off ||
                                              m->recorder.camera_y[slot] != recorder_camera_off;
        }
        break;
    case RecorderRecordType::message: {
        RecorderMessage message{};
        if (decode_recorder_message(data, size, &message) != WireError::ok)
            break;
        if (message.kind == RecorderMessageKind::warp_done) {
            m->recorder.warp_done[slot] = true;
        } else if (message.kind == RecorderMessageKind::whiteboard) {
            if (message.size != 0 && message.size <= recorder_whiteboard_max_payload &&
                whiteboard_marks_shown(m, slot) && m->hooks.whiteboard_marks != nullptr) {
                ++m->whiteboard_batches;
                m->hooks.whiteboard_marks(m->hooks.context, slot, message.payload, message.size);
            }
        } else if (message.kind == RecorderMessageKind::cheat_mask && message.size >= 4) {
            const auto mask = load_u32(message.payload);
            if (mask != 0 && m->recorder.cheat_mask[slot] == 0 && m->rules.recorder_cheat_notices)
                notice(
                    m, own_words("{player} has cheats enabled", {{"player", name_of(from)}}).c_str()
                );
            m->recorder.cheat_mask[slot] = mask;
        }
        break;
    }
    default:
        break;
    }
}

/// Tells whether every player still playing has said it is ready.
///
/// @param m Running match.
/// @return True once each active non-watcher that holds units is ready.
bool all_ready(NetMatch* m) {
    for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot) {
        auto& p = m->world->game.players[slot];
        if (!slot_active(p) || watcher(m->world, p) || p.reject_reason != 0 || p.unit_count == 0)
            continue;
        if (!m->recorder.ready[slot])
            return false;
    }
    return true;
}

// ---- the recorder's take and prebuilt base

/// Silence, in clock units (30 a second), after which a player's units may be taken.
constexpr uint32_t take_silence = 30 * time_ticks_per_second;
/// The heading the hand-over records of a take carry.
constexpr uint32_t take_bank_heading = 0x7f640000u;
/// The heading the creation records of a prebuilt base carry.
constexpr uint32_t base_create_bank_heading = 0x74e90000u;
/// The heading the hand-over records of a prebuilt base carry.
constexpr uint32_t base_give_bank_heading = 0x7e640000u;
/// The state flags a prebuilt base's building is switched on with.
constexpr uint8_t base_state_mask = 0x01;
/// The take walks a block up to this many slots before its end.
constexpr uint32_t take_block_end_margin = 2;
/// A take starts after the commander, one slot into the block.
constexpr uint32_t take_first_slot = 2;
/// .takecmd, or a taker with no units, starts at the commander.
constexpr uint32_t take_commander_slot = 1;
/// The second unit a player creates marks the centre of its prebuilt base.
constexpr uint32_t base_centre_slot = 2;
/// Recorder protocols from which a taker's claim is held and released on a reject.
constexpr uint8_t claim_protocol_current = 5;
constexpr uint8_t claim_protocol_older = 3;
/// The damage, kind and direction of the records that kill a claimant's unused slots.
constexpr uint16_t claim_kill_damage = 30999;
constexpr uint8_t claim_kill_direction = 0x92;
constexpr uint8_t claim_kill_kind = 1;
constexpr uint8_t side_arm = 0;
constexpr uint8_t side_core = 1;
constexpr uint8_t side_unknown = 0xff; ///< a player with no setup block

/// Tells whether the recorder runs and the profile turns a rule on with its parameter available.
///
/// @param m Running match.
/// @param enabled the rule's hack is on
/// @param available its available parameter
/// @return True when both are set and the recorder runs.
bool recorder_rule(const NetMatch* m, bool enabled, bool available) {
    return m->rules.recorder_protocol != recorder_protocol_plain && enabled && available;
}

bool commander_warp_available(const NetMatch* m) {
    const auto* rules = m->match_rules;
    return rules != nullptr &&
           recorder_rule(
               m, rules->setup.commander_warp.enabled, rules->setup.commander_warp.available
           );
}

bool prebuilt_base_available(const NetMatch* m) {
    const auto* rules = m->match_rules;
    return rules != nullptr && recorder_rule(
                                   m,
                                   rules->setup.recorder_prebuilt_base.enabled,
                                   rules->setup.recorder_prebuilt_base.available
                               );
}

bool take_give_available(const NetMatch* m) {
    const auto* rules = m->match_rules;
    return rules != nullptr && recorder_rule(
                                   m,
                                   rules->sharing.recorder_take_give.enabled,
                                   rules->sharing.recorder_take_give.available
                               );
}

/// Says a line as the recorder does: plain chat to every player, shown here as well.
///
/// @param[in,out] m Running match.
/// @param line The line.
void recorder_say(NetMatch* m, const char* line) {
    say_to_all(m, line);
    const auto* self = first_local_human(m->world);
    if (m->hooks.chat != nullptr && self != nullptr)
        m->hooks.chat(m->hooks.context, self->index, line);
}

/// Shows a line of the recorder's on this machine only.
///
/// @param[in,out] m Running match.
/// @param line The line.
void recorder_show(NetMatch* m, const char* line) {
    const auto* self = first_local_human(m->world);
    if (m->hooks.chat != nullptr && self != nullptr)
        m->hooks.chat(m->hooks.context, self->index, line);
}

/// Returns the slot before the first unit of a player's block: the block's
/// units are base + 1 to base + units_per_player.
///
/// @param world Match world.
/// @param p Player record.
/// @param[out] base The slot.
/// @return False when the player has no block.
bool block_base(World* world, const Player& p, uint32_t* base) {
    uint32_t count = 0;
    Unit* first = world_player_units(world, &p, &count);
    if (first == nullptr)
        return false;
    *base = world_unit_slot(world, first) - 1;
    return true;
}

/// Notes a creation record that passes this machine, sent or received, as the recorder does.
///
/// The unit counts as created and not yet finished, and the second unit of
/// its owner's block marks the centre of the owner's prebuilt base.
///
/// @param[in,out] m Running match.
/// @param from The player whose machine sent it.
/// @param record The record.
void recorder_note_create(NetMatch* m, const Player& from, const UnitCreatedRecord& record) {
    const auto per_player = static_cast<uint32_t>(m->world->game.units_per_player);
    if (record.unit_index < m->recorder_units.size())
        m->recorder_units[record.unit_index].state = recorder_unit_state_created;
    if (per_player == 0 || from.index >= OA_PLAYER_COUNT ||
        record.unit_index % per_player != base_centre_slot)
        return;
    auto& centre = m->recorder.base.centre[from.index];
    centre[0] = static_cast<uint16_t>(static_cast<uint32_t>(record.position[0]) >> 16);
    centre[1] = static_cast<uint16_t>(static_cast<uint32_t>(record.position[1]) >> 16);
    centre[2] = static_cast<uint16_t>(static_cast<uint32_t>(record.position[2]) >> 16);
}

/// Notes an alliance record that passes this machine, sent or received: a
/// player that allies with one of this machine's players lets it take its
/// units, and one that breaks the alliance no longer does.
///
/// @param[in,out] m Running match.
/// @param record The record.
void recorder_note_alliance(NetMatch* m, const AllianceRecord& record) {
    if (!take_give_available(m))
        return;
    const auto* a = player_of(m->world, record.player_id_a);
    const auto* b = player_of(m->world, record.player_id_b);
    if (a == nullptr || b == nullptr || a->index >= OA_PLAYER_COUNT || !is_local(*b))
        return;
    m->take.granted[a->index] = record.value != 0;
    m->take.allied[a->index] = record.value != 0;
}

/// Carries out .give and .stopgive: when the line names this machine's
/// player, its sender lets that player take its units, or no longer does.
///
/// @param[in,out] m Running match.
/// @param from The sender.
/// @param line The command.
void recorder_grant(NetMatch* m, const Player& from, const RecorderCommandLine& line) {
    if (!take_give_available(m) || from.index >= OA_PLAYER_COUNT)
        return;
    const auto* self = first_local_human(m->world);
    if (self == nullptr || !recorder_arguments_name(line, self->name, sizeof self->name))
        return;
    const bool give = line.command == RecorderCommand::give;
    m->take.granted[from.index] = give;
    char text[notice_bytes];
    std::snprintf(
        text,
        sizeof text,
        give ? "%.*s ready to take units from %.*s" : "%.*s barred from taking units from %.*s",
        name_length(*self),
        self->name,
        name_length(from),
        from.name
    );
    recorder_say(m, text);
    if (give && m->take.allied[from.index])
        recorder_say(m, "You don't really need to type .give. Allying is enough.");
}

/// Carries out .take and .takecmd.
///
/// Typed here, it claims the first player, in slot order, that let this
/// machine's player take its units and has not been heard from for 30
/// seconds, while no claim is open; the take then hands its units over one
/// by one (take_step). Typed on another machine, it takes over any claim of
/// this machine's, which stops where it was and is not resumed, and notes
/// the claimant's in place of any earlier one; a claimant with no recorder
/// has the empty slots of its block killed at the next reject, and any other
/// claimant has nothing killed.
///
/// @param[in,out] m Running match.
/// @param from The sender.
/// @param commander True for .takecmd, which hands the commander over too.
void recorder_take(NetMatch* m, const Player& from, bool commander) {
    if (!take_give_available(m))
        return;
    auto* world = m->world;
    auto& take = m->take;
    const auto* self = first_local_human(world);
    if (self == nullptr)
        return;
    char text[notice_bytes];
    if (from.status == OA_PLAYER_STATUS_LOCAL) {
        const auto now = now_time(m);
        for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot) {
            auto& victim = world->game.players[slot];
            uint32_t base = 0;
            if (!take.granted[slot] || !is_remote(victim) || !block_base(world, victim, &base) ||
                base::game_loop::scaled_clock_elapsed(now, victim.last_update_time) <=
                    take_silence ||
                take.state != RecorderTakeState::idle)
                continue;
            take.state = RecorderTakeState::taking;
            take.granted[slot] = false;
            std::snprintf(
                take.claimant, sizeof take.claimant, "%.*s", name_length(victim), victim.name
            );
            ++take.takes;
            take.cursor[slot] =
                commander || from.unit_count == 0 ? take_commander_slot : take_first_slot;
            std::snprintf(
                text,
                sizeof text,
                "%.*s taking %.*ss units",
                name_length(*self),
                self->name,
                name_length(victim),
                victim.name
            );
            recorder_say(m, text);
        }
        return;
    }
    if (!is_remote(from) || from.index >= OA_PLAYER_COUNT)
        return;
    if (take.state == RecorderTakeState::taking) {
        take.state = RecorderTakeState::claimed;
        std::fill(std::begin(take.cursor), std::end(take.cursor), 0u);
        std::snprintf(
            text, sizeof text, "%.*s aborting take claim", name_length(*self), self->name
        );
        recorder_say(m, text);
    }
    std::snprintf(take.claimant, sizeof take.claimant, "%.*s", name_length(from), from.name);
    take.takes = 0;
    take.kill_records.clear();
    const auto protocol = m->recorder.peer_protocol[from.index];
    take.state = protocol >= claim_protocol_current ? RecorderTakeState::claimed
                 : protocol >= claim_protocol_older ? RecorderTakeState::claimed_older
                                                    : RecorderTakeState::claimed_plain;
    if (protocol != recorder_protocol_plain)
        return;
    // A claimant with no recorder: every empty slot of its block, and the
    // slot after it, is to be killed with the next reject.
    uint32_t base = 0;
    if (!block_base(world, from, &base))
        return;
    const auto per_player = static_cast<uint32_t>(world->game.units_per_player);
    for (uint32_t i = 0; i <= per_player; ++i) {
        const auto unit = base + i + 1;
        if (unit >= m->recorder_units.size() || unit > UINT16_MAX ||
            m->recorder_units[unit].state <= recorder_unit_state_created)
            continue;
        UnitDamageRecord kill{};
        kill.target_unit_index = static_cast<uint16_t>(unit);
        kill.amount = claim_kill_damage;
        kill.direction = claim_kill_direction;
        kill.kind = claim_kill_kind;
        uint8_t bytes[record_length_table[static_cast<uint8_t>(RecordType::unit_damage)]];
        std::size_t written = 0;
        if (encode_record(kill, bytes, sizeof bytes, &written) == WireError::ok)
            take.kill_records.insert(take.kill_records.end(), bytes, bytes + written);
    }
}

/// Takes the reject record a machine sent into the recorder's claim: a held
/// claim is released, and a claimant with no recorder has its unused slots
/// killed; with no claim held, the sender no longer lets this machine's
/// player take its units.
///
/// @param[in,out] m Running match.
/// @param from The sender.
void recorder_heard_reject(NetMatch* m, const Player& from) {
    if (!take_give_available(m))
        return;
    auto& take = m->take;
    char text[notice_bytes];
    switch (take.state) {
    case RecorderTakeState::idle:
    case RecorderTakeState::taking:
        if (from.index < OA_PLAYER_COUNT)
            take.granted[from.index] = false;
        return;
    case RecorderTakeState::claimed_older:
    case RecorderTakeState::claimed:
        std::snprintf(text, sizeof text, "%s take claim released", take.claimant);
        recorder_show(m, text);
        break;
    case RecorderTakeState::claimed_plain: {
        const auto* self = first_local_human(m->world);
        const auto from_id = self != nullptr ? self->player_id : no_player_id;
        const auto record_bytes =
            record_length_table[static_cast<uint8_t>(RecordType::unit_damage)];
        for (std::size_t at = 0; at + record_bytes <= take.kill_records.size();
             at += record_bytes) {
            const auto* record = take.kill_records.data() + at;
            (void)net_match_send(m, from_id, broadcast_destination_id, record, record_bytes);
            bool handled = false;
            if (from.index < OA_PLAYER_COUNT)
                (void)replication_apply_record(
                    m->world, &m->sim, from.index, record, record_bytes, &handled
                );
        }
        take.kill_records.clear();
        recorder_show(m, "Sending killing packets");
        break;
    }
    }
    take.takes = 0;
    take.state = RecorderTakeState::idle;
    take.claimant[0] = '\0';
}

/// Hands one unit of each player being taken over to this machine's player, as a take does once each time
/// the pump looks for a record.
///
/// Each step forges, as if the taken player's machine had sent it, a
/// hand-over (0x14) of the unit at the take's place in the block when the
/// recorder last saw that unit finished, with the health it then had, and
/// moves the place on. Past the block's end less two slots the take sends
/// every player a reject (0x1b) of the taken player, as its connection
/// lost. The claim ends with the last take.
///
/// @param[in,out] m Running match.
/// @return True when a hand-over was forged.
bool take_step(NetMatch* m) {
    auto& take = m->take;
    if (take.state != RecorderTakeState::taking)
        return false;
    auto* world = m->world;
    const auto per_player = static_cast<uint32_t>(world->game.units_per_player);
    const auto* self = first_local_human(world);
    const auto self_id = self != nullptr ? self->player_id : no_player_id;
    bool forged = false;
    for (uint8_t slot = 0; slot < OA_PLAYER_COUNT && take.state == RecorderTakeState::taking;
         ++slot) {
        auto& cursor = take.cursor[slot];
        if (cursor == 0)
            continue;
        auto& victim = world->game.players[slot];
        uint32_t base = 0;
        const bool has_block = block_base(world, victim, &base);
        const auto unit = base + cursor;
        if (has_block && unit < m->recorder_units.size() && unit <= UINT16_MAX &&
            m->recorder_units[unit].state == 0) {
            UnitTransferRecord give{};
            give.unit_index = static_cast<uint16_t>(unit);
            give.new_owner_id = self_id;
            give.health = static_cast<uint16_t>(m->recorder_units[unit].health);
            give.bank_heading = take_bank_heading;
            uint8_t bytes[record_length_table[static_cast<uint8_t>(RecordType::unit_transfer)]];
            std::size_t written = 0;
            bool handled = false;
            if (encode_record(give, bytes, sizeof bytes, &written) == WireError::ok &&
                replication_apply_record(world, &m->sim, slot, bytes, written, &handled) ==
                    WireError::ok) {
                ++m->taken_units;
                forged = true;
            }
        }
        if (cursor + take_block_end_margin >= per_player) {
            RejectRecord reject{};
            reject.player_id = victim.player_id;
            reject.reason = reject_connection_lost;
            send_record(m, self_id, broadcast_destination_id, reject);
            --take.takes;
            cursor = 0;
        } else {
            ++cursor;
        }
    }
    if (take.takes == 0 && take.state == RecorderTakeState::taking) {
        take.state = RecorderTakeState::idle;
        take.claimant[0] = '\0';
    }
    return forged;
}

/// Carries out the host's .baseoff: no base is offered for the rest of the session.
///
/// @param[in,out] m Running match.
/// @param from The host.
void turn_bases_off(NetMatch* m, const Player& from) {
    auto& base = m->recorder.base;
    base.enabled = false;
    std::fill(std::begin(base.available), std::end(base.available), false);
    if (is_local(from))
        recorder_say(m, "Quick base disabled");
}

/// Builds a player's prebuilt base, as the host's recorder does.
///
/// For each building of the player's side, the host's machine sends the
/// player's machine a creation record (0x09) of the building at the base's
/// centre plus the building's offset, in a spare slot, then switches it on
/// (0x11) and hands it to the player (0x14) with the table's health; the
/// player's machine then builds it in its own block. A building whose x or z
/// would not be past 0 is left out. For the host's own base the spare slot is
/// in the next player's block and the records are taken here as if received.
///
/// @param[in,out] m Running match.
/// @param player The player whose base it is.
/// @param spare_owner The player whose block holds the spare slot.
void build_base(NetMatch* m, const Player& player, const Player& spare_owner) {
    auto* world = m->world;
    const auto& base = m->recorder.base;
    const auto* info = world_player_info(world, &player);
    const auto side = info != nullptr ? info->side : side_unknown;
    if (side != side_arm && side != side_core) {
        recorder_say(m, "not arm/core");
        return;
    }
    char text[notice_bytes];
    std::snprintf(text, sizeof text, "%.*s just built a base", name_length(player), player.name);
    recorder_say(m, text);
    uint32_t spare_base = 0;
    if (!block_base(world, spare_owner, &spare_base))
        return;
    const auto per_player = static_cast<uint32_t>(world->game.units_per_player);
    const auto spare = static_cast<uint16_t>(spare_base + per_player - 1);
    const bool here = is_local(player);
    const auto* host = first_local_human(world);
    const auto host_id = host != nullptr ? host->player_id : no_player_id;
    const auto& centre = base.centre[player.index];
    const auto deliver = [&](const uint8_t* bytes, std::size_t size) {
        if (here) {
            bool handled = false;
            (void)replication_apply_record(
                world, &m->sim, spare_owner.index, bytes, size, &handled
            );
        } else {
            (void)net_match_send(m, host_id, player.player_id, bytes, size);
        }
    };
    for (int32_t i = 1; i <= base.per_side; ++i) {
        const auto entry =
            static_cast<std::size_t>(side) * static_cast<std::size_t>(base.per_side) +
            static_cast<std::size_t>(i);
        if (entry >= base.entries.size())
            break;
        const auto& building = base.entries[entry];
        const int32_t x = int32_t{centre[0]} + building.offset_x;
        const int32_t z = int32_t{centre[2]} + building.offset_z;
        if (x <= 0 || z <= 0)
            continue;
        UnitCreatedRecord create{};
        create.unit_def_index = building.unit_type;
        create.unit_index = spare;
        create.position[0] = static_cast<int32_t>(static_cast<uint32_t>(x) << 16);
        create.position[1] = static_cast<int32_t>(static_cast<uint32_t>(centre[1]) << 16);
        create.position[2] = static_cast<int32_t>(static_cast<uint32_t>(z) << 16);
        create.bank_heading = base_create_bank_heading;
        UnitStateFlagsRecord on{};
        on.unit_index = spare;
        on.state_mask = base_state_mask;
        UnitTransferRecord give{};
        give.unit_index = spare;
        give.new_owner_id = here ? host_id : player.player_id;
        give.health = building.health;
        give.bank_heading = base_give_bank_heading;
        uint8_t bytes[record_length_table[static_cast<uint8_t>(RecordType::unit_transfer)]];
        std::size_t written = 0;
        if (encode_record(create, bytes, sizeof bytes, &written) == WireError::ok)
            deliver(bytes, written);
        if (encode_record(on, bytes, sizeof bytes, &written) == WireError::ok)
            deliver(bytes, written);
        if (encode_record(give, bytes, sizeof bytes, &written) == WireError::ok)
            deliver(bytes, written);
        ++m->base_buildings;
    }
}

/// Carries out .dobase on the host's machine: the player's base is built
/// once .base offered it and the player created the unit that marks its
/// centre.
///
/// @param[in,out] m Running match.
/// @param player The player that asked.
void recorder_do_base(NetMatch* m, const Player& player) {
    auto* world = m->world;
    auto& base = m->recorder.base;
    if (!prebuilt_base_available(m) || !host_is_local(world) || !base.enabled ||
        player.index >= OA_PLAYER_COUNT || !base.available[player.index])
        return;
    if (base.centre[player.index][0] == 0) {
        recorder_say(m, "Please build a building to mark the centre of your base");
        return;
    }
    // The spare slot is the host's own, or for the host's base the next player's.
    const Player* spare_owner = first_local_human(world);
    if (is_local(player)) {
        spare_owner = nullptr;
        for (const auto& p : world->game.players)
            if (slot_active(p) && !is_local(p)) {
                spare_owner = &p;
                break;
            }
    }
    if (spare_owner == nullptr) {
        char text[notice_bytes];
        std::snprintf(
            text, sizeof text, "%.*s please wait for sync", name_length(player), player.name
        );
        recorder_say(m, text);
        return;
    }
    base.available[player.index] = false;
    build_base(m, player, *spare_owner);
}

/// Carries out the host's speed lock or unlock just applied to the recorder's session.
///
/// A game speed outside the range now allowed is set to its nearest end
/// and, unless this machine's player only watches, sent to every player, as
/// each recorder sends it; the speed_lock_changed hook then hears of the
/// range.
///
/// @param[in,out] m Running match.
void speed_lock_applied(NetMatch* m) {
    uint8_t slowest = 0;
    uint8_t fastest = 0;
    const bool locked = net_match_speed_range(m, &slowest, &fastest);
    auto& game = m->world->game;
    const int32_t speed = game.requested_speed;
    if (speed < slowest || speed > fastest) {
        auto& self = game.players[game.local_player_index];
        net_match_set_speed(m, speed < slowest ? slowest : fastest, !watcher(m->world, self));
    }
    if (m->hooks.speed_lock_changed != nullptr)
        m->hooks.speed_lock_changed(m->hooks.context, locked, slowest, fastest);
}

/// Turns this machine's camera sharing off, or back on (.sharemappos).
///
/// Turning it off sends the camera record with both halves off to every
/// player, in a frame of its own. Turning it back on sends the camera with
/// the next frame.
///
/// @param[in,out] m Running match.
void toggle_camera_sharing(NetMatch* m) {
    auto& recorder = m->recorder;
    recorder.local_camera_hidden = !recorder.local_camera_hidden;
    recorder.local_camera_sent = false;
    if (!recorder.local_camera_hidden)
        return;
    flush_now(m);
    net_match_send_camera(m, recorder_camera_off, recorder_camera_off);
    flush_now(m);
}

/// Sends this machine's camera when it has moved since it last went out, at
/// most once each send interval, while the camera is shared.
///
/// @param[in,out] m Running match.
void send_moved_camera(NetMatch* m) {
    auto& recorder = m->recorder;
    if (m->rules.recorder_protocol == recorder_protocol_plain || recorder.local_camera_hidden ||
        !recorder.local_camera_known)
        return;
    const auto now = now_time(m);
    if (recorder.local_camera_sent) {
        if (recorder.sent_camera_x == recorder.local_camera_x &&
            recorder.sent_camera_y == recorder.local_camera_y)
            return;
        if (now - recorder.sent_camera_time < m->connection->packets->ticks_between_sends)
            return;
    }
    net_match_send_camera(m, recorder.local_camera_x, recorder.local_camera_y);
    recorder.local_camera_sent = true;
    recorder.sent_camera_x = recorder.local_camera_x;
    recorder.sent_camera_y = recorder.local_camera_y;
    recorder.sent_camera_time = now;
}

/// Reads a chat line for the recorder's commands, as every recorder reads each line it sends or hears.
///
/// Host commands count only from the host. The answers that every recorder
/// gives (.report, .players) are sent by this machine for its own player.
///
/// @param[in,out] m Running match.
/// @param from The sender, local or remote.
/// @param text The line.
void recorder_chat_line(NetMatch* m, const Player& from, const char* text) {
    if (m->rules.recorder_protocol == recorder_protocol_plain)
        return;
    const auto line = parse_recorder_command(text);
    if (line.command == RecorderCommand::none)
        return;
    if (recorder_session_command(line.command) && !m->rules.recorder_session_commands)
        return;
    if (recorder_speed_command(line.command) && !m->rules.speed_lock)
        return;
    auto* world = m->world;
    if (recorder_command_host_only(line.command)) {
        if (host_slot(world) != from.index)
            return;
        switch (line.command) {
        case RecorderCommand::commander_warp:
            if (!commander_warp_available(m))
                return;
            break;
        case RecorderCommand::base_off:
            if (prebuilt_base_available(m))
                turn_bases_off(m, from);
            return;
        case RecorderCommand::base_file:
            // Bases are offered in the battle room.
            return;
        default:
            break;
        }
        (void)recorder_apply_host_command(m->recorder, line);
        if (recorder_speed_command(line.command))
            speed_lock_applied(m);
        return;
    }
    switch (line.command) {
    case RecorderCommand::give:
    case RecorderCommand::stop_give:
        recorder_grant(m, from, line);
        break;
    case RecorderCommand::take:
    case RecorderCommand::take_commander:
        recorder_take(m, from, line.command == RecorderCommand::take_commander);
        break;
    case RecorderCommand::do_base:
        recorder_do_base(m, from);
        break;
    case RecorderCommand::report:
    case RecorderCommand::report_mod: {
        const auto* self = first_local_human(world);
        if (self != nullptr && m->recorder.program[0] != '\0') {
            char answer[notice_bytes];
            std::snprintf(
                answer,
                sizeof answer,
                "*** %.*s uses %s",
                name_length(*self),
                self->name,
                m->recorder.program
            );
            say_to_all(m, answer);
        }
        break;
    }
    case RecorderCommand::ready:
    case RecorderCommand::vote_ready:
        if (from.index < OA_PLAYER_COUNT)
            m->recorder.ready[from.index] = true;
        // Once every player still playing is ready, every recorder unpauses.
        if (all_ready(m) && (world->game.sim_run_flags & run_flag_paused) != 0) {
            m->recorder.autopause_holding = false;
            flush_now(m);
            net_match_set_pause(m, false);
            flush_now(m);
        }
        break;
    case RecorderCommand::fake_watch:
        // The local player watches while seated, five seconds on.
        if (is_local(from)) {
            m->recorder.fake_watch = true;
            m->recorder.fake_watch_tick = world->game.tick + fake_watch_delay_ticks;
        }
        break;
    case RecorderCommand::share_camera:
        // The local player turns sharing its camera off, or back on.
        if (is_local(from))
            toggle_camera_sharing(m);
        break;
    case RecorderCommand::record:
        if (is_local(from) && line.argument[0] != '\0')
            oa::base::text::copy_terminated(
                m->recorder.record_name,
                std::string_view(
                    line.argument,
                    chat_bytes_within(line.argument, sizeof m->recorder.record_name - 1)
                )
            );
        break;
    default:
        break;
    }
}

/// Tells whether a received speed lies outside what the rules and the recorder's lock allow.
///
/// @param m Running match.
/// @param speed The speed.
/// @return True when a recorder removes it.
bool speed_locked_out(const NetMatch* m, int32_t speed) {
    uint8_t low = 0;
    uint8_t high = 0;
    if (!net_match_speed_range(m, &low, &high))
        return false;
    return speed < low || speed > high;
}

// ---- commander start sync

/// Sends one start sync record from a local player for each of its first units that moves on the ground.
///
/// Every record names unit index 0, so a receiver takes the last one for
/// the player's commander.
///
/// @param[in,out] m Running match.
/// @param player Local player.
void send_commander_sync(NetMatch* m, Player& player) {
    auto* world = m->world;
    if (watcher(world, player) || player.reject_reason != 0)
        return;
    uint32_t count = 0;
    Unit* first = world_player_units(world, &player, &count);
    constexpr uint32_t synced_slots = 90;
    const auto slots = std::min<uint32_t>(
        {count, synced_slots, static_cast<uint32_t>(world->game.units_per_player)}
    );
    const auto def_bits = static_cast<unsigned>(world->game.unit_def_id_bits);
    for (uint32_t i = 0; i < slots && first != nullptr; ++i) {
        Unit& unit = first[i];
        if ((unit.flags & OA_UNIT_FLAG_LIVE) == 0 || unit.type_index == 0 || unit.movement == 0 ||
            m->sim.movement == nullptr)
            continue;
        const auto* movement = m->sim.movement(m->sim.context, world, &unit);
        if (movement == nullptr || movement->driver != MovementClass::ground)
            continue;
        StartPosition position{};
        position.def_index = unit.type_index;
        position.x = static_cast<int16_t>(unit.position.x >> 16);
        position.z = static_cast<int16_t>(unit.position.z >> 16);
        position.target_x = position.x;
        position.target_z = position.z;
        if ((movement->ground.flags & ground_driver_path_set) != 0 &&
            movement->ground.path_count > 0) {
            position.target_x = movement->ground.path[0][0];
            position.target_z = movement->ground.path[0][1];
        }
        uint8_t storage[unit_state_writer_words * bit_stream_word_bytes];
        BitWriter writer;
        bit_writer_init(&writer, storage, unit_state_writer_words);
        uint16_t length = 0;
        if (unit_state_write_start_position(
                &writer, world->game.tick, position, def_bits, &length
            ) != WireError::ok) {
            ++m->record_errors;
            continue;
        }
        if (net_match_send(m, player.player_id, broadcast_destination_id, storage, length))
            ++m->commander_syncs_sent;
    }
}

/// Takes the commander position a remote player's 0x2c gives, while the game is younger than the unit
/// limit.
///
/// @param[in,out] m Running match.
/// @param from The sender.
/// @param data The record.
/// @param size Its length.
void apply_commander_sync(NetMatch* m, Player& from, const uint8_t* data, std::size_t size) {
    auto* world = m->world;
    if (world->game.tick >= world->game.units_per_player || !is_remote(from) ||
        watcher(world, from))
        return;
    int16_t x = 0;
    int16_t z = 0;
    if (!unit_state_start_position(
            data, size, static_cast<unsigned>(world->game.unit_def_id_bits), &x, &z
        ))
        return;
    uint32_t count = 0;
    Unit* first = world_player_units(world, &from, &count);
    if (first == nullptr || count == 0)
        return;
    // The whole part of each coordinate is set; the fraction stays.
    constexpr uint32_t fraction_mask = 0xffff;
    auto& position = first[0].position;
    position.x = static_cast<oa_fixed>(
        (static_cast<uint32_t>(static_cast<uint16_t>(x)) << 16) |
        (static_cast<uint32_t>(position.x) & fraction_mask)
    );
    position.z = static_cast<oa_fixed>(
        (static_cast<uint32_t>(static_cast<uint16_t>(z)) << 16) |
        (static_cast<uint32_t>(position.z) & fraction_mask)
    );
    ++m->commander_syncs_applied;
}

/// Handles a chat record whose text starts with NUL under a private channel.
///
/// @param[in,out] m Running match.
/// @param from The sender.
/// @param data The record.
/// @param size Its length.
void handle_private(NetMatch* m, const Player& from, const uint8_t* data, std::size_t size) {
    ++m->private_records;
    PrivateMessage message{};
    if (decode_private_message(data, size, &message) != WireError::ok ||
        !private_message_accepted(m->rules.private_channel, message))
        return;
    if (message.sub_id == private_sub_integrity)
        handle_integrity(m, from, message);
    else if (message.sub_id == private_sub_vote)
        handle_vote(m, from, message);
}

/// Dispatches one admitted record from a remote player.
///
/// @param[in,out] m Running match.
/// @param[in,out] from Sender's player record.
/// @param[in,out] to Addressee's player record (the local player for a broadcast).
/// @param packet The record.
/// @return False for a 0x08 start record, which ends the pump.
bool dispatch_record(NetMatch* m, Player& from, Player& to, const Packet& packet) {
    auto* world = m->world;
    const auto* data = packet.data;
    const auto size = static_cast<std::size_t>(packet.size);
    const auto type = static_cast<RecordType>(data[0]);
    switch (type) {
    case RecordType::ping:
        handle_ping(m, from, to, data, size);
        break;
    case RecordType::chat: {
        if (m->rules.private_channel != PrivateChannel::none && is_private_chat(data, size)) {
            handle_private(m, from, data, size);
            break;
        }
        const bool addressed_here = is_local(to) && size >= 1;
        char text[sizeof(ChatRecord::text) + 1]{};
        std::string shown;
        if (addressed_here) {
            std::memcpy(
                text,
                data + 1,
                size - 1 < sizeof(ChatRecord::text) ? size - 1 : sizeof(ChatRecord::text)
            );
            shown = net_match_chat_text(m, from, text);
        }
        if (is_local(to) && to.status == OA_PLAYER_STATUS_LOCAL && m->hooks.chat != nullptr &&
            size >= 1) {
            m->hooks.chat(m->hooks.context, from.index, shown.c_str());
            recorder_chat_line(m, from, text);
        }
        if (addressed_here && m->hooks.seat_chat != nullptr)
            m->hooks.seat_chat(
                m->hooks.context,
                from.index,
                to.index,
                packet.to_id == broadcast_destination_id,
                shown.c_str()
            );
        break;
    }
    case RecordType::probe: {
        const uint8_t reply = static_cast<uint8_t>(RecordType::probe_reply);
        net_match_send(m, primary_id(world), from.player_id, &reply, 1);
        break;
    }
    case RecordType::probe_reply:
        probe_flags(from) |= 1;
        break;
    case RecordType::game_start:
        world->game.session_flags =
            static_cast<uint8_t>(world->game.session_flags | kNetFlagGameStarted);
        return false;
    case RecordType::unit_created:
    case RecordType::unit_link:
    case RecordType::unit_damage:
    case RecordType::unit_killed:
    case RecordType::weapon_fire:
    case RecordType::projectile_intercepted:
    case RecordType::feature_event:
    case RecordType::cob_start:
    case RecordType::unit_state_flags:
    case RecordType::builder_link:
    case RecordType::sound:
    case RecordType::unit_transfer:
    case RecordType::unit_state: {
        if (m->rules.commander_sync_tick != 0 && type == RecordType::unit_state)
            apply_commander_sync(m, from, data, size);
        if (type == RecordType::unit_created &&
            m->rules.recorder_protocol != recorder_protocol_plain) {
            UnitCreatedRecord created{};
            if (decode_record(data, size, &created) == WireError::ok)
                recorder_note_create(m, from, created);
        }
        bool handled = false;
        const auto error =
            replication_apply_record(world, &m->sim, from.index, data, size, &handled);
        if (error != WireError::ok)
            ++m->record_errors;
        else if (handled)
            ++m->records_applied;
        break;
    }
    case RecordType::loaded:
        if ((world->game.load_flags & load_flag_barrier) != 0 && from.index < OA_PLAYER_COUNT)
            m->barrier.loaded[from.index] = 1;
        break;
    case RecordType::resource_give:
        apply_give(m, data, size);
        break;
    case RecordType::player_value_reply: {
        PlayerValueReplyRecord r{};
        if (decode_record(data, size, &r) == WireError::ok)
            if (auto* info = world_player_info(world, &to))
                info->color = r.value;
        break;
    }
    case RecordType::pause_speed: {
        PauseSpeedRecord r{};
        if (decode_record(data, size, &r) != WireError::ok)
            break;
        if (r.kind == pause_speed_kind_pause) {
            // Under autopause only the host's unpause starts the game; any
            // other is undone and announced, as every recorder does.
            if (m->recorder.autopause_holding && (r.value & run_flag_paused) == 0) {
                if (host_slot(world) != from.index) {
                    notice(
                        m,
                        own_words("{player} tried to unpause.", {{"player", name_of(from)}}).c_str()
                    );
                    break;
                }
                m->recorder.autopause_holding = false;
            }
            world->game.sim_run_flags = static_cast<uint16_t>(
                (world->game.sim_run_flags & ~run_flag_paused) | (r.value & run_flag_paused)
            );
            if ((r.value & run_flag_paused) == 0 && m->recorder.start_paused)
                m->recorder.warp_released = true;
        } else if (!speed_locked_out(m, r.value)) {
            net_match_set_speed(m, r.value, false);
        }
        break;
    }
    case RecordType::reject: {
        RejectRecord r{};
        if (decode_record(data, size, &r) != WireError::ok)
            break;
        recorder_heard_reject(m, from);
        if (auto* target = player_of(world, r.player_id))
            reject_player(m, target->player_id, r.reason);
        break;
    }
    case RecordType::disconnect_notice: {
        DisconnectNoticeRecord r{};
        if (decode_record(data, size, &r) != WireError::ok)
            break;
        if (const auto* p = player_of(world, r.player_id)) {
            // The whole line is the game's own text, translated, with the
            // name where its translation puts it, as 3.1c shows it.
            notice(m, with_name(game_text(m, "Player %s has disconnected"), name_of(*p)).c_str());
            disconnect_notice(m, r.player_id);
        }
        break;
    }
    case RecordType::start_position: {
        StartPositionRecord r{};
        if (decode_record(data, size, &r) != WireError::ok)
            break;
        start_position(to) = r.position;
        StartPositionAckRecord ack{};
        ack.player_id = to.player_id;
        send_record(m, to.player_id, from.player_id, ack);
        break;
    }
    case RecordType::start_position_ack: {
        StartPositionAckRecord r{};
        if (decode_record(data, size, &r) == WireError::ok) {
            const auto slot = slot_of(world, r.player_id);
            if (slot != no_slot)
                m->barrier.start_acked[slot] = 1;
        }
        break;
    }
    case RecordType::player_info: {
        PlayerInfoRecord r{};
        if (decode_record(data, size, &r) != WireError::ok)
            break;
        const auto slot = slot_of(world, r.player_id);
        if (slot != no_slot && is_remote(world->game.players[slot])) {
            auto* bytes = reinterpret_cast<uint8_t*>(&world->player_info[slot]);
            // The colour slot of a remote player stays as the battle room
            // left it under the rule; a recorder's in-game blocks say no
            // recorder, so the protocol learnt in the battle room stays.
            const auto colour = bytes[player_info_color_offset];
            const auto protocol = bytes[player_info_recorder_protocol_offset];
            std::memcpy(bytes, data + 1, player_info_block_bytes);
            if (m->rules.keep_remote_colour)
                bytes[player_info_color_offset] = colour;
            if (m->rules.recorder_protocol != recorder_protocol_plain)
                bytes[player_info_recorder_protocol_offset] = protocol;
            ui::frontend_multiplayer::note_shared_machines(world->game);
        }
        break;
    }
    case RecordType::slot_table: {
        SlotTableRecord r{};
        if (decode_record(data, size, &r) != WireError::ok)
            break;
        std::memcpy(world->game.slot_table, r.slot_ids, sizeof world->game.slot_table);
        world->game.gui_flags = static_cast<uint8_t>(world->game.gui_flags | gui_flag_refresh);
        break;
    }
    case RecordType::alliance: {
        AllianceRecord r{};
        if (decode_record(data, size, &r) != WireError::ok)
            break;
        auto* a = player_of(world, r.player_id_a);
        auto* b = player_of(world, r.player_id_b);
        if (a == nullptr || b == nullptr || a->index >= OA_PLAYER_COUNT ||
            b->index >= OA_PLAYER_COUNT)
            break;
        recorder_note_alliance(m, r);
        if (teams_rules(m).team_number_alliances.enabled &&
            team_rules::alliance_requested(team_slots(world)[a->index], r.both_sides)) {
            // A request for one of this machine's players: it sets the
            // alliance and announces it.
            announce_alliance(m, *a, *b, r.value != 0 ? 1 : 0);
            break;
        }
        if (is_local(*b))
            b->allied_by[a->index] = r.value;
        a->alliance[b->index] = r.value;
        if (m->hooks.alliance_changed != nullptr)
            m->hooks.alliance_changed(m->hooks.context, world, a->index);
        break;
    }
    case RecordType::player_team: {
        PlayerTeamRecord r{};
        if (decode_record(data, size, &r) != WireError::ok)
            break;
        auto* p = player_of(world, r.player_id);
        if (p == nullptr)
            break;
        const auto& teams = teams_rules(m).team_number_alliances;
        if (teams.enabled && p->index < OA_PLAYER_COUNT) {
            // teams.team-number-alliances: the team decides this machine's
            // players' alliances with the sender.
            const auto result = team_rules::receive_team_number(
                team_slots(world), p->index, r.value, teams.bit7_keeps_alliances
            );
            if (result.store)
                p->team = static_cast<uint8_t>(result.team);
            apply_alliance_steps(m, result.steps);
            break;
        }
        p->team = r.value;
        break;
    }
    case RecordType::integrity_notice: {
        IntegrityNoticeRecord r{};
        if (decode_record(data, size, &r) == WireError::ok)
            if (const auto* p = player_of(world, r.player_id)) {
                // The name, then the game's own line, with the two spaces
                // 3.1c shows inside it. 3.1c's table holds no translation
                // of it, so it shows in English unless a language pack
                // keys it.
                std::string line = name_of(*p);
                line += ' ';
                line += game_text(m, "has modified his executable.  Game integrity breached.");
                notice(m, line.c_str());
            }
        break;
    }
    case RecordType::economy:
        apply_economy(m, from, data, size);
        break;
    case RecordType::economy_reply: {
        EconomyReplyRecord r{};
        if (decode_record(data, size, &r) == WireError::ok && r.mark_requested != 0 &&
            from.index < OA_PLAYER_COUNT) {
            economy_requested(to)[from.index] = 1;
            if (r.mark_answered != 0)
                economy_answered(to)[from.index] = 1;
        }
        break;
    }
    case RecordType::load_progress: {
        LoadProgressRecord r{};
        if (decode_record(data, size, &r) == WireError::ok)
            load_progress(from) = r.percent;
        break;
    }
    default:
        ++m->records_refused;
        break;
    }
    return true;
}

/// Shuffles start positions with the match's rand15 hook.
///
/// Each element from the second on swaps with one before it, chosen as
/// rand() % its index. Does nothing without a rand hook.
///
/// @param m Running match providing the rand hook.
/// @param[in,out] first Positions to shuffle.
/// @param count Number of positions.
void shuffle(NetMatch* m, int32_t* first, int32_t count) {
    if (m->hooks.rand15 == nullptr)
        return;
    for (int32_t i = 1, span = 1; i < count; ++i, ++span) {
        const auto r = static_cast<uint32_t>(m->hooks.rand15(m->hooks.context)) & 0x7fffu;
        const auto j = static_cast<int32_t>(r % static_cast<uint32_t>(span));
        const auto held = first[i];
        first[i] = first[j];
        first[j] = held;
    }
}

bool watcher(World* world, Player& p) {
    const auto* info = world_player_info(world, &p);
    return p.in_use != 0 && info != nullptr && (info->options & OA_SETUP_OPTION_WATCHER) != 0;
}

/// Runs one step of the load barrier.
///
/// On the host's machine the start positions are assigned once (shuffled
/// unless fixed; with fewer than three players a coin flip decides whether
/// to shuffle) and sent to every remote player until acknowledged. Local
/// players then broadcast 0x15 "loaded".
///
/// @param[in,out] m Running match.
/// @return True when every remote player has loaded and, on the host, acknowledged its start position.
bool barrier_step(NetMatch* m) {
    auto* world = m->world;
    auto& game = world->game;
    auto& b = m->barrier;
    const bool assigning = host_is_local(world);
    if (assigning && !b.assigned) {
        const auto& local = game.players[game.local_player_index];
        const auto* local_info = world_player_info(world, &local);
        const bool fixed =
            local_info != nullptr && (local_info->options & OA_SETUP_OPTION_FIXED_LOCATIONS) != 0;
        int32_t next = 0;
        if (setup_rules(m).team_start_positions.mode ==
            data::match_rules::SetupTeamStartPositionsMode::team_adjacent) {
            // setup.team-start-positions: team-mates start together. With
            // random positions the order is shuffled and the coin flip of
            // 3.1c's shuffle is still drawn once, over no positions.
            const auto positions = team_rules::team_start_positions(
                team_slots(world), !fixed, m->map_neutral_units, {m, rand_below}
            );
            if (!fixed && m->hooks.rand15 != nullptr)
                (void)m->hooks.rand15(m->hooks.context);
            m->team_positions = positions;
            m->team_positions_assigned = true;
            for (int32_t i = 0; i < OA_PLAYER_COUNT; ++i) {
                auto& p = game.players[i];
                b.start_assignment[i] = slot_active(p) && !watcher(world, p)
                                            ? positions[static_cast<std::size_t>(i)]
                                            : -1;
            }
        } else if (!fixed) {
            int32_t order[OA_PLAYER_COUNT];
            int32_t count = 0;
            for (int32_t i = 0; i < OA_PLAYER_COUNT; ++i)
                order[i] = -1;
            for (int32_t i = 0; i < OA_PLAYER_COUNT; ++i) {
                auto& p = game.players[i];
                if (slot_active(p) && !watcher(world, p)) {
                    order[count] = count;
                    ++count;
                }
            }
            bool mix = true;
            if (count < 3 && m->hooks.rand15 != nullptr)
                mix = (m->hooks.rand15(m->hooks.context) * 2) / 0x8000 != 0;
            if (mix)
                shuffle(m, order, count);
            for (int32_t i = 0; i < OA_PLAYER_COUNT; ++i) {
                auto& p = game.players[i];
                b.start_assignment[i] = slot_active(p) && !watcher(world, p) ? order[next++] : -1;
            }
        } else {
            for (int32_t i = 0; i < OA_PLAYER_COUNT; ++i) {
                auto& p = game.players[i];
                b.start_assignment[i] = slot_active(p) && !watcher(world, p) ? next++ : -1;
            }
        }
        b.assigned = true;
    }
    bool ready = true;
    for (int32_t i = 0; i < OA_PLAYER_COUNT && ready; ++i) {
        if (!is_remote(game.players[i]))
            continue;
        ready = b.loaded[i] != 0 && (!assigning || b.start_acked[i] != 0);
    }
    if (assigning) {
        for (int32_t i = 0; i < OA_PLAYER_COUNT; ++i) {
            auto& p = game.players[i];
            if (b.start_acked[i] != 0 || p.in_use == 0)
                continue;
            const auto position = static_cast<uint8_t>(b.start_assignment[i]);
            if (is_remote(p)) {
                StartPositionRecord record{};
                record.position = position;
                send_record(m, first_local_player_id(game), p.player_id, record);
            } else if (is_local(p)) {
                start_position(p) = position;
                b.start_acked[i] = 1;
            }
        }
    }
    if (!assigning || ready)
        for (auto& p : game.players)
            if (is_local(p)) {
                const uint8_t loaded = static_cast<uint8_t>(RecordType::loaded);
                net_match_send(m, p.player_id, broadcast_destination_id, &loaded, 1);
            }
    return ready;
}

} // namespace

void net_match_begin(
    NetMatch* m,
    NetConnection* connection,
    World* world,
    const ReplicationSim& sim,
    const NetMatchHooks& hooks
) noexcept {
    *m = NetMatch{};
    m->connection = connection;
    m->world = world;
    m->sim = sim;
    m->hooks = hooks;
    m->rules = connection->rules;
    for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot)
        m->recorder.peer_protocol[slot] = reinterpret_cast<const uint8_t*>(
            &world->player_info[slot]
        )[player_info_recorder_protocol_offset];
    m->phase = NetPhase::loading;
    connection->packets->receiver.players = world->game.players;
    connection->packets->send_options = &world->game;
    auto& game = world->game;
    game.session_flags = static_cast<uint8_t>(game.session_flags | kNetFlagLive);
    if (game.player_timeout_seconds == 0)
        game.player_timeout_seconds = default_timeout_seconds;
    for (int32_t i = 0; i < OA_PLAYER_COUNT; ++i) {
        auto& p = game.players[i];
        m->barrier.loaded[i] = is_local(p) ? 1u : 0u;
        m->barrier.start_acked[i] = watcher(world, p) ? 1u : 0u;
        m->barrier.start_assignment[i] = -1;
    }
    game.load_flags = static_cast<uint16_t>(game.load_flags | load_flag_started);
    m->timeout_baseline = now_time(m);
    m->keepalive_time = now_time(m);
}

void net_match_bind_rules(
    NetMatch* m, const data::match_rules::MatchRules* rules, bool map_neutral_units
) noexcept {
    m->match_rules = rules;
    m->map_neutral_units = map_neutral_units;
    m->recorder_units.clear();
    if (take_give_available(m))
        m->recorder_units.assign(m->world->unit_slot_count, RecorderUnitView{});
}

void net_match_note_full_record(
    NetMatch* m, uint32_t unit_slot, const FullUnitRecord& record
) noexcept {
    if (unit_slot >= m->recorder_units.size())
        return;
    auto& view = m->recorder_units[unit_slot];
    if (record.unit_def_index == 0) {
        view.state = recorder_unit_state_empty;
        return;
    }
    view.health = record.health;
    view.state = record.build_byte;
}

void net_match_deal_teams(NetMatch* m, int32_t teams, char* notice, std::size_t capacity) noexcept {
    if (notice == nullptr || capacity == 0)
        return;
    if (!m->team_positions_assigned) {
        std::snprintf(
            notice, capacity, "+autoteam is only available to the host of a multiplayer game"
        );
        return;
    }
    int32_t placed = 0;
    for (const int32_t position : m->team_positions)
        placed += position >= 0 ? 1 : 0;
    if (placed < 2) {
        std::snprintf(notice, capacity, "+autoteam not available b/c too few players");
        return;
    }
    auto* world = m->world;
    for (auto& p : world->game.players)
        if (p.in_use != 0 && static_cast<int8_t>(p.team) < team_rules::no_team &&
            !watcher(world, p)) {
            std::snprintf(
                notice, capacity, "+autoteam not available b/c players have team selections"
            );
            return;
        }
    apply_alliance_steps(m, team_rules::alliances_by_position(m->team_positions, teams));
    std::snprintf(notice, capacity, "Alliances created with %d teams", teams);
}

void net_match_send_game_start(NetMatch* m) noexcept {
    auto* world = m->world;
    const auto& local = world->game.players[world->game.local_player_index];
    const auto* info = world_player_info(world, &local);
    if (info == nullptr || (info->role & role_host) == 0)
        return;
    const uint8_t start = static_cast<uint8_t>(RecordType::game_start);
    net_match_send(m, local.player_id, broadcast_destination_id, &start, 1);
}

void net_match_queue_game_start(NetConnection* c, const Game& game) noexcept {
    if (!c->in_session || game.local_player_index >= OA_PLAYER_COUNT)
        return;
    const uint8_t start = static_cast<uint8_t>(RecordType::game_start);
    queue_to_all(c, game, game.players[game.local_player_index].player_id, &start, 1);
}

bool loading_frame_due(LoadingPace* pace, uint32_t now) noexcept {
    if (pace->ran && now - pace->time < loading_frame_ticks)
        return false;
    pace->ran = true;
    pace->time = now;
    return true;
}

void net_match_building_frame(
    NetConnection* c, const Game& game, const uint8_t rows[load_progress_rows]
) noexcept {
    if (!c->in_session)
        return;
    if (c->host != nullptr)
        sock::host_pump(c->host, 0);
    const uint8_t probe = static_cast<uint8_t>(RecordType::probe);
    for (const auto& p : game.players)
        if (building_here(p)) {
            queue_to_all(c, game, p.player_id, &probe, 1);
            net_connection_flush(c);
        }
    int32_t sum = 0;
    for (std::size_t row = 0; row < load_progress_rows; ++row)
        sum += rows[row];
    LoadProgressRecord record{};
    record.percent = static_cast<uint8_t>(sum / static_cast<int32_t>(load_progress_rows));
    uint8_t bytes[record_length_table[static_cast<uint8_t>(RecordType::load_progress)]];
    std::size_t written = 0;
    if (encode_record(record, bytes, sizeof bytes, &written) == WireError::ok)
        for (const auto& p : game.players)
            if (building_here(p))
                queue_to_all(c, game, p.player_id, bytes, written);
    net_connection_flush(c);
}

void net_match_loader_waiting(NetMatch* m) noexcept {
    auto& game = m->world->game;
    game.load_flags = static_cast<uint16_t>(game.load_flags | load_flag_barrier);
}

bool net_match_loading_frame(NetMatch* m) noexcept {
    auto& game = m->world->game;
    if ((game.load_flags & load_flag_barrier_passed) != 0)
        return true;
    for (auto& p : game.players)
        if (is_local(p)) {
            const uint8_t probe = static_cast<uint8_t>(RecordType::probe);
            net_match_send(m, p.player_id, broadcast_destination_id, &probe, 1);
            flush_now(m);
        }
    (void)net_match_pump(m);
    if ((game.load_flags & load_flag_barrier) != 0 && barrier_step(m)) {
        game.load_flags = static_cast<uint16_t>(
            (game.load_flags & ~load_flag_barrier) | load_flag_barrier_passed
        );
        (void)session::session_set_guaranteed(&m->connection->session, false);
        m->connection->packets->guaranteed = false;
    }
    flush_now(m);
    return (game.load_flags & load_flag_barrier_passed) != 0;
}

bool net_match_paced_loading_frame(
    NetMatch* m, LoadingPace* pace, const uint8_t rows[load_progress_rows]
) noexcept {
    if ((m->world->game.load_flags & load_flag_barrier_passed) != 0) {
        if (m->frames_since_barrier < commander_wait_frames &&
            loading_frame_due(pace, now_time(m))) {
            if (m->connection->host != nullptr)
                sock::host_pump(m->connection->host, 0);
            ++m->frames_since_barrier;
        }
        return m->frames_since_barrier >= commander_wait_frames;
    }
    if (!loading_frame_due(pace, now_time(m)))
        return false;
    const bool ready = net_match_loading_frame(m);
    net_match_send_load_progress(m, rows);
    return ready && m->frames_since_barrier >= commander_wait_frames;
}

void net_match_end_loading(NetMatch* m) noexcept {
    auto* world = m->world;
    auto& local = world->game.players[world->game.local_player_index];
    if (auto* info = world_player_info(world, &local))
        info->options = static_cast<uint16_t>(info->options | OA_SETUP_OPTION_STARTED);
    net_match_send_player_status(m, true);
    net_match_republish(m);
    net_match_economy_period(m);
}

void net_match_republish(NetMatch* m) noexcept {
    auto* c = m->connection;
    if (c == nullptr || !c->hosting)
        return;
    auto* world = m->world;
    auto& game = world->game;
    auto* info = world_player_info(world, &game.players[game.local_player_index]);
    if (info == nullptr) {
        net_connection_republish(c, nullptr);
        return;
    }
    info->options = static_cast<uint16_t>(
        (info->options & ~player_count_option_mask) | (game.player_count & player_count_option_mask)
    );
    PlayerSetupInfo published = *info;
    published.status =
        static_cast<uint16_t>(published.status & ~ui::frontend_multiplayer::status::launch_only);
    uint8_t user[ui::frontend_multiplayer::kSessionUserBytes];
    std::memcpy(
        user,
        reinterpret_cast<const uint8_t*>(&published) +
            ui::frontend_multiplayer::kSessionUserInfoOffset,
        sizeof user
    );
    // The description says the game has started from then on.
    if ((published.options & OA_SETUP_OPTION_STARTED) != 0)
        c->session.desc.flags |= session_flag_join_disabled;
    net_connection_republish(c, user);
}

uint8_t net_match_start_position(const NetMatch* m, uint8_t slot) noexcept {
    if (slot >= OA_PLAYER_COUNT)
        return no_start_position;
    return m->world->game.players[slot].start_position;
}

void net_match_send_load_progress(NetMatch* m, const uint8_t rows[load_progress_rows]) noexcept {
    int32_t sum = 0;
    for (std::size_t row = 0; row < load_progress_rows; ++row)
        sum += rows[row];
    LoadProgressRecord record{};
    record.percent = static_cast<uint8_t>(sum / static_cast<int32_t>(load_progress_rows));
    for (int32_t slot = 0; slot < OA_PLAYER_COUNT; ++slot) {
        auto& p = m->world->game.players[slot];
        if (!is_local(p))
            continue;
        load_progress(p) = record.percent;
        send_record(m, p.player_id, broadcast_destination_id, record);
    }
}

void net_match_loading_screen_status(const NetMatch* m, LoadingScreenStatus* out) noexcept {
    *out = LoadingScreenStatus{};
    auto& game = m->world->game;
    if ((game.load_flags & load_flag_barrier_passed) != 0) {
        std::snprintf(out->text, sizeof out->text, "%s", game_text(m, "Synchronization complete"));
        return;
    }
    int32_t active = 0;
    for (int32_t slot = 0; slot < OA_PLAYER_COUNT; ++slot) {
        auto& p = game.players[slot];
        if (!slot_active(p))
            continue;
        ++active;
        if (load_progress(p) == load_progress_complete && m->barrier.loaded[slot] != 0)
            ++out->ready;
    }
    // The local player is always active, so the count is not zero here.
    const int32_t share = active != 0 ? loading_bars_width / active : 0;
    const int32_t width = share - loading_bar_gap;
    int32_t left = loading_bars_left;
    for (int32_t slot = 0; slot < OA_PLAYER_COUNT && active != 0; ++slot) {
        auto& p = game.players[slot];
        if (!slot_active(p))
            continue;
        auto& bar = out->bars[out->bar_count++];
        bar.player = &p;
        bar.left = left;
        bar.right = left + width;
        bar.filled = left + load_progress(p) * width / int32_t{load_progress_complete};
        left += share;
    }
    // A translation holds only until the next one: the first is copied.
    const std::string waiting = game_text(m, "Waiting for other players");
    std::snprintf(
        out->text,
        sizeof out->text,
        "%s.  %i %s",
        waiting.c_str(),
        out->ready,
        game_text(m, out->ready == 1 ? "player ready" : "players ready")
    );
}

void net_match_enter_game(NetMatch* m) noexcept {
    auto& game = m->world->game;
    game.load_flags = static_cast<uint16_t>(game.load_flags | load_flag_loader_done);
    m->phase = NetPhase::in_game;
    m->timeout_baseline = now_time(m);
}

uint32_t net_match_pump(NetMatch* m) noexcept {
    auto* world = m->world;
    if ((world->game.session_flags & kNetFlagLive) == 0)
        return 0;
    for (auto& p : world->game.players)
        clear_rx_count(p);
    uint32_t count = 0;
    Packet packet{};
    const auto phase_bit = static_cast<uint8_t>(m->phase);
    const auto local_id = world->game.players[world->game.local_player_index].player_id;
    for (;;) {
        // A take's hand-overs arrive ahead of each record the pump reads,
        // until a step finds no unit to hand over.
        if (m->phase == NetPhase::in_game && take_step(m))
            continue;
        if (!packet_layer_receive(
                m->connection->packets, static_cast<int32_t>(world->game.tick), &packet
            ))
            break;
        ++count;
        const auto to_id = packet.to_id != broadcast_destination_id ? packet.to_id : local_id;
        auto* to = player_of(world, to_id);
        if (packet.kind == PacketKind::system) {
            if (to != nullptr && to->status == OA_PLAYER_STATUS_LOCAL)
                dispatch_system(m, packet);
            continue;
        }
        if (packet.size != 0 && m->rules.recorder_protocol != recorder_protocol_plain &&
            is_recorder_record_type(packet.data[0])) {
            auto* sender = player_of(world, packet.from_id);
            if (sender != nullptr && is_remote(*sender)) {
                sender->last_update_time = now_time(m);
                if (m->hooks.record_seen != nullptr)
                    m->hooks.record_seen(
                        m->hooks.context, packet.from_id, packet.data, packet.size
                    );
                handle_recorder_record(m, *sender, packet.data, packet.size);
            }
            continue;
        }
        if (packet.size == 0 || !is_record_type(packet.data[0]) ||
            (record_phase_table[packet.data[0]] & phase_bit) == 0) {
            ++m->records_refused;
            continue;
        }
        auto* from = player_of(world, packet.from_id);
        if (from == nullptr) {
            ++m->records_refused;
            continue;
        }
        if (is_local(*from))
            continue;
        if (!is_remote(*from) || from->index == no_slot) {
            reject_player(m, packet.from_id, reject_connection_lost);
            continue;
        }
        if (to == nullptr || !slot_active(*to)) {
            ++m->records_refused;
            continue;
        }
        bump_rx_count(*from);
        from->last_update_time = now_time(m);
        if (m->hooks.record_seen != nullptr)
            m->hooks.record_seen(m->hooks.context, packet.from_id, packet.data, packet.size);
        if (m->rules.vote_reject)
            cancel_timeout_vote(m, from->player_id);
        if (!dispatch_record(m, *from, *to, packet))
            break;
    }
    ui::frontend_multiplayer::note_shared_machines(world->game);
    m->timeout_player = net_match_check_timeouts(m);
    // Under the vote rule every machine opens a timeout vote of its own on
    // a player that stopped answering; the vote decides, not the host.
    if (m->rules.vote_reject && m->phase == NetPhase::in_game &&
        m->timeout_player != no_player_id && vote_find(m->votes, m->timeout_player) == nullptr)
        if (auto* target = player_of(world, m->timeout_player))
            if (auto* vote = propose_vote(m, *target, vote_flag_timeout))
                show_vote(m, *vote);
    return count;
}

bool net_match_send(
    NetMatch* m, uint32_t from_id, uint32_t to_id, const uint8_t* record, std::size_t size
) noexcept {
    auto* world = m->world;
    if ((world->game.session_flags & kNetFlagLive) == 0 || record == nullptr || size == 0)
        return false;
    const auto* from = player_of(world, from_id);
    if (from == nullptr || !is_local(*from) || from->reject_reason != 0)
        return false;
    // A player watching while seated sends a load report in place of each
    // record but chat and resource shares, and empty unit states.
    uint8_t substitute[unit_state_writer_words * bit_stream_word_bytes];
    if (m->recorder.fake_watch && from->status == OA_PLAYER_STATUS_LOCAL &&
        static_cast<int32_t>(world->game.tick - m->recorder.fake_watch_tick) >= 0 &&
        record[0] != static_cast<uint8_t>(RecordType::chat) &&
        record[0] != static_cast<uint8_t>(RecordType::resource_give) && !m->sending_copies) {
        std::size_t length = 0;
        if (record[0] == static_cast<uint8_t>(RecordType::unit_state)) {
            BitWriter writer;
            bit_writer_init(&writer, substitute, unit_state_writer_words);
            unit_state_begin(&writer, world->game.tick);
            uint16_t written = 0;
            if (unit_state_finish(
                    &writer,
                    FullUnitRecord{},
                    static_cast<unsigned>(world->game.unit_def_id_bits),
                    &written
                ) == WireError::ok)
                length = written;
        } else {
            LoadProgressRecord progress{};
            progress.percent = load_progress_complete;
            (void)encode_record(progress, substitute, sizeof substitute, &length);
        }
        if (length != 0) {
            record = substitute;
            size = length;
        }
    }
    if (m->hooks.record_seen != nullptr && !m->sending_copies)
        m->hooks.record_seen(m->hooks.context, from_id, record, size);
    if (!m->sending_copies && m->rules.recorder_protocol != recorder_protocol_plain) {
        // The recorder reads what this machine sends as it reads what it hears.
        if (record[0] == static_cast<uint8_t>(RecordType::unit_created)) {
            UnitCreatedRecord created{};
            if (decode_record(record, size, &created) == WireError::ok)
                recorder_note_create(m, *from, created);
        } else if (record[0] == static_cast<uint8_t>(RecordType::alliance)) {
            AllianceRecord alliance{};
            if (decode_record(record, size, &alliance) == WireError::ok)
                recorder_note_alliance(m, alliance);
        }
    }
    if (to_id == broadcast_destination_id && world->game.shared_machines != 0) {
        // One record goes out as a copy per machine and is seen once.
        m->sending_copies = true;
        uint32_t ids[OA_PLAYER_COUNT];
        const auto count = ui::frontend_multiplayer::machine_broadcast_targets(world->game, ids);
        for (int32_t i = 0; i < count; ++i)
            (void)net_match_send(m, from_id, ids[i], record, size);
        m->sending_copies = false;
        return true;
    }
    if (to_id != broadcast_destination_id) {
        const auto* to = player_of(world, to_id);
        if (to == nullptr || !is_remote(*to) || to->reject_reason != 0)
            return false;
    }
    return packet_layer_send(m->connection->packets, from_id, to_id, record, size, now_time(m)) ==
           WireError::ok;
}

void net_match_send_player_state(NetMatch* m, Player* player) noexcept {
    auto* world = m->world;
    if (player == nullptr || !is_local(*player) || player->index >= OA_PLAYER_COUNT)
        return;
    if (m->rules.commander_sync_tick != 0 && world->game.tick == m->rules.commander_sync_tick)
        send_commander_sync(m, *player);
    uint8_t storage[unit_state_writer_words * bit_stream_word_bytes];
    BitWriter writer;
    bit_writer_init(&writer, storage, unit_state_writer_words);
    uint16_t length = 0;
    if (replication_pack_player(world, &m->sim, player->index, &writer, &length) != WireError::ok) {
        ++m->record_errors;
        return;
    }
    net_match_send(m, player->player_id, broadcast_destination_id, storage, length);
}

void net_match_send_player_status(NetMatch* m, bool machine_groups) noexcept {
    auto* world = m->world;
    if ((world->game.session_flags & kNetFlagLive) == 0)
        return;
    for (auto& p : world->game.players) {
        const auto* info = world_player_info(world, &p);
        if (!is_local(p) || info == nullptr)
            continue;
        PlayerInfoRecord status{};
        const auto* bytes = reinterpret_cast<const uint8_t*>(info);
        std::memcpy(status.info_head, bytes, sizeof status.info_head);
        status.player_id = p.player_id;
        std::memcpy(status.info_tail, bytes + player_info_tail_offset, sizeof status.info_tail);
        // A recorder stamps its protocol on battle-room blocks only.
        status.info_tail[player_info_recorder_protocol_offset - player_info_tail_offset] =
            recorder_protocol_plain;
        announce_unicode_chat(status, m->unicode_chat);
        // An in-game block says it is OA's as the battle room's do, so the
        // remote copies that replace those blocks keep the signature.
        stamp_engine_signature(status);
        send_record(m, p.player_id, broadcast_destination_id, status);
        PlayerTeamRecord team{};
        team.player_id = p.player_id;
        team.value = p.team;
        send_record(m, p.player_id, broadcast_destination_id, team);
        flush_now(m);
    }
    if (machine_groups)
        request_machine_groups(m);
    flush_now(m);
}

void net_match_send_alliance(NetMatch* m, uint8_t from, uint8_t to, uint8_t allied) noexcept {
    if (from >= OA_PLAYER_COUNT || to >= OA_PLAYER_COUNT)
        return;
    const auto& giver = m->world->game.players[from];
    const auto& other = m->world->game.players[to];
    AllianceRecord record{};
    record.player_id_a = giver.player_id;
    record.player_id_b = other.player_id;
    record.value = allied;
    if (send_record(m, giver.player_id, other.player_id, record))
        flush_now(m);
}

void net_match_remove_player(NetMatch* m, uint8_t slot, uint8_t reason) noexcept {
    if (slot >= OA_PLAYER_COUNT)
        return;
    // Under the vote rule a removal asks every machine instead.
    if (m->rules.vote_reject && reason == reject_departed) {
        (void)net_match_propose_reject(m, slot);
        return;
    }
    const auto& p = m->world->game.players[slot];
    if (p.in_use != 0)
        (void)reject_player(m, p.player_id, reason);
}

void net_match_send_unit_created(NetMatch* m, const Unit* unit) noexcept {
    auto* world = m->world;
    if (unit == nullptr)
        return;
    const auto* owner = world_player_ref(world, unit->owner);
    if (owner == nullptr || !is_local(*owner))
        return;
    UnitCreatedRecord r{};
    r.unit_def_index = unit->type_index;
    r.unit_index = unit->id;
    r.position[0] = unit->position.x;
    r.position[1] = unit->position.y;
    r.position[2] = unit->position.z;
    r.bank_heading =
        static_cast<uint16_t>(unit->bank) | (static_cast<uint32_t>(unit->heading) << 16);
    r.pitch = static_cast<uint16_t>(unit->pitch);
    send_record(m, owner->player_id, broadcast_destination_id, r);
}

void net_match_send_builder_link(NetMatch* m, const Unit* source, const Unit* subject) noexcept {
    auto* world = m->world;
    if (source == nullptr)
        return;
    const auto* owner = world_player_ref(world, source->owner);
    if (owner == nullptr || !is_local(*owner))
        return;
    BuilderLinkRecord r{};
    r.subject_unit_index = subject != nullptr ? subject->id : 0;
    r.source_unit_index = source->id;
    send_record(m, owner->player_id, broadcast_destination_id, r);
}

void net_match_send_damage(NetMatch* m, uint32_t route, const UnitDamageRecord& record) noexcept {
    send_record(m, route, broadcast_destination_id, record);
}

uint32_t net_match_local_route(const NetMatch* m) noexcept {
    return primary_id(m->world);
}

uint32_t net_match_host_id(const NetMatch* m) noexcept {
    const auto slot = host_slot(m->world);
    return slot != no_slot ? m->world->game.players[slot].player_id : no_player_id;
}

namespace {

/// Tells whether a slot may receive the local player's shared resources and sight.
///
/// @param world Match world.
/// @param self Local player sharing.
/// @param slot Slot of the candidate, 0..9.
/// @return True for a human simulated elsewhere that is still playing and
///         taking part, set in the sharing player's alliance row.
bool share_target(World* world, const Player& self, std::size_t slot) {
    auto& p = world->game.players[slot];
    return participating(p) && remote_playing(world, p) && self.alliance[slot] != 0;
}

/// Shares resources and sight with allies as the player's lobby options ask.
///
/// Every 60 ticks a third of the metal above Player.metal_share_threshold
/// and half of the energy above Player.energy_share_threshold go to a
/// share_target player, capped at its free storage; every 450 ticks sight
/// goes to each share_target player.
///
/// @param[in,out] m Running match.
/// @param self Local player sharing.
/// @quirk The resources go to the last share_target player in slot order
///        whose store is below the sharing player's, not to the poorest.
void share_resources(NetMatch* m, Player& self) {
    auto* world = m->world;
    const auto tick = world->game.tick;
    const auto* info = world_player_info(world, &self);
    if (info == nullptr || self.index >= OA_PLAYER_COUNT)
        return;
    if (tick % share_period_ticks == 0) {
        if ((info->role & role_share_metal) != 0 && self.metal > self.metal_share_threshold) {
            Player* target = &self;
            for (std::size_t slot = 0; slot < OA_PLAYER_COUNT; ++slot)
                if (share_target(world, self, slot) && world->game.players[slot].metal < self.metal)
                    target = &world->game.players[slot];
            if (target != &self) {
                auto amount = (self.metal - self.metal_share_threshold) * share_metal_fraction;
                if (target->metal_storage - target->metal <= amount)
                    amount = target->metal_storage - target->metal;
                net_match_give(m, self.index, target->index, true, amount);
            }
        }
        if ((info->role & role_share_energy) != 0 && self.energy > self.energy_share_threshold) {
            Player* target = &self;
            for (std::size_t slot = 0; slot < OA_PLAYER_COUNT; ++slot)
                if (share_target(world, self, slot) &&
                    world->game.players[slot].energy < self.energy)
                    target = &world->game.players[slot];
            if (target != &self) {
                auto amount = (self.energy - self.energy_share_threshold) * share_energy_fraction;
                if (target->energy_storage - target->energy <= amount)
                    amount = target->energy_storage - target->energy;
                net_match_give(m, self.index, target->index, false, amount);
            }
        }
    }
    if (tick % share_sight_period_ticks == 0 && (info->role & role_share_sight) != 0)
        for (std::size_t slot = 0; slot < OA_PLAYER_COUNT; ++slot)
            if (share_target(world, self, slot))
                net_match_share_sight(m, self.index, world->game.players[slot].index);
}

} // namespace

void net_match_after_tick(NetMatch* m) noexcept {
    auto& game = m->world->game;
    auto& local = game.players[game.local_player_index];
    if (is_local(local))
        share_resources(m, local);
    net_match_rules_tick(m);
    send_moved_camera(m);
    packet_layer_flush(m->connection->packets, now_time(m), false);
}

void net_match_rules_tick(NetMatch* m) noexcept {
    const auto tick = m->world->game.tick;
    if (m->rules.integrity_check != IntegrityCheck::off && m->phase == NetPhase::in_game) {
        if (integrity_challenge_due(m->rules.integrity_check, tick))
            integrity_challenge(m);
        if (tick == integrity_report_tick && !m->integrity.reported)
            integrity_report(m);
    }
    if (m->rules.vote_reject)
        tally_votes(m);
}

bool net_match_cast_vote(NetMatch* m, uint32_t target_id, bool yes) noexcept {
    if (!m->rules.vote_reject)
        return false;
    auto* vote = vote_find(m->votes, target_id);
    const auto* self = first_local_human(m->world);
    if (vote == nullptr || self == nullptr)
        return false;
    vote_cast(*vote, self->index, yes);
    PrivateMessage message{};
    message.sub_id = private_sub_vote;
    message.op = yes ? vote_op_yes : vote_op_no;
    store_u32(message.payload + vote_target_offset, target_id);
    message.payload[vote_flag_offset] = vote->flag;
    send_private(m, broadcast_destination_id, message);
    show_vote(m, *vote);
    tally_votes(m);
    return true;
}

bool net_match_propose_reject(NetMatch* m, uint8_t slot) noexcept {
    if (!m->rules.vote_reject || slot >= OA_PLAYER_COUNT)
        return false;
    auto& target = m->world->game.players[slot];
    const auto* self = first_local_human(m->world);
    if (!slot_active(target) || is_local(target) || self == nullptr)
        return false;
    auto* vote = propose_vote(m, target, vote_flag_manual);
    if (vote == nullptr)
        return false;
    vote_cast(*vote, self->index, true);
    show_vote(m, *vote);
    tally_votes(m);
    return true;
}

void net_match_warp_done(NetMatch* m) noexcept {
    if (m->rules.recorder_protocol == recorder_protocol_plain)
        return;
    auto& game = m->world->game;
    uint8_t record[recorder_message_header_bytes];
    std::size_t written = 0;
    if (encode_recorder_message(
            RecorderMessageKind::warp_done, nullptr, 0, record, sizeof record, &written
        ) != WireError::ok)
        return;
    for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot) {
        auto& p = game.players[slot];
        if (is_local(p))
            m->recorder.warp_done[slot] = true;
        else if (is_remote(p) && recorder_peer(m, slot) && p.reject_reason == 0)
            send_recorder_alone(m, p.player_id, record, written);
    }
}

void net_match_recorder_start(NetMatch* m) noexcept {
    if (m->rules.recorder_protocol == recorder_protocol_plain)
        return;
    const auto& options = m->recorder.options;
    if (options.autopause == 0 && options.commander_warp == 0)
        return;
    m->recorder.start_paused = true;
    m->recorder.autopause_holding = options.autopause != 0;
    flush_now(m);
    net_match_set_pause(m, true);
    flush_now(m);
}

uint32_t net_match_send_whiteboard(NetMatch* m, const uint8_t* batch, std::size_t size) noexcept {
    if (m->rules.recorder_protocol == recorder_protocol_plain || batch == nullptr || size == 0 ||
        size > recorder_whiteboard_max_payload)
        return 0;
    uint8_t record[recorder_message_header_bytes + recorder_whiteboard_max_payload];
    std::size_t written = 0;
    if (encode_recorder_message(
            RecorderMessageKind::whiteboard, batch, size, record, sizeof record, &written
        ) != WireError::ok)
        return 0;
    uint32_t sent = 0;
    for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot) {
        const auto& p = m->world->game.players[slot];
        if (!is_remote(p) || p.reject_reason != 0 ||
            m->recorder.peer_protocol[slot] < recorder_protocol_messages)
            continue;
        send_recorder_alone(m, p.player_id, record, written);
        ++sent;
    }
    return sent;
}

void net_match_send_camera(NetMatch* m, uint16_t x, uint16_t y) noexcept {
    if (m->rules.recorder_protocol == recorder_protocol_plain)
        return;
    uint8_t record[recorder_camera_bytes];
    encode_recorder_camera(x, y, record);
    net_match_send(
        m, first_local_player_id(m->world->game), broadcast_destination_id, record, sizeof record
    );
}

void net_match_note_camera(NetMatch* m, int32_t x, int32_t y) noexcept {
    // Off is reserved for turning sharing off.
    constexpr int32_t highest = recorder_camera_off - 1;
    auto& recorder = m->recorder;
    recorder.local_camera_known = true;
    recorder.local_camera_x = static_cast<uint16_t>(std::clamp(x, 0, highest));
    recorder.local_camera_y = static_cast<uint16_t>(std::clamp(y, 0, highest));
}

void net_match_paused_frame(NetMatch* m) noexcept {
    send_moved_camera(m);
    packet_layer_flush(m->connection->packets, now_time(m), false);
    (void)net_match_pump(m);
    // A game held for its commanders' warp starts once every warp is done:
    // each recorder sends its own unpause, alone.
    if (m->recorder.start_paused && m->recorder.options.commander_warp != 0 &&
        !m->recorder.warp_released && all_warps_done(m)) {
        m->recorder.warp_released = true;
        flush_now(m);
        net_match_set_pause(m, false);
        flush_now(m);
    }
    if (m->rules.vote_reject)
        tally_votes(m);
    const auto now = now_time(m);
    // The next probe is due once the clock passes keepalive_time, which is
    // never set more than keepalive_interval ahead of it: a reading further
    // from it has passed it, or turned over to 0 since it was set.
    if (m->keepalive_time - now > keepalive_interval) {
        m->keepalive_time = now + keepalive_interval;
        const uint8_t probe = static_cast<uint8_t>(RecordType::probe);
        net_match_send(m, primary_id(m->world), broadcast_destination_id, &probe, 1);
    }
}

uint32_t net_match_check_timeouts(NetMatch* m) noexcept {
    auto& game = m->world->game;
    // "Drop 0" skips the scan, the paused baseline and the dialog call alike.
    if ((game.console_flags & OA_CONSOLE_FLAG_NO_DROP) != 0)
        return m->timeout_player;
    const auto now = now_time(m);
    if ((game.sim_run_flags & run_flag_paused) != 0) {
        m->timeout_baseline = now;
        return no_player_id;
    }
    const auto limit = game.player_timeout_seconds * 30u;
    // Silent since the player was last heard from, or since the baseline
    // when that is later, counting a turn of the clock to 0 between.
    const auto stalled = [&](const Player& p) {
        const auto silent = std::min(
            base::game_loop::scaled_clock_elapsed(now, p.last_update_time),
            base::game_loop::scaled_clock_elapsed(now, m->timeout_baseline)
        );
        return is_remote(p) && silent > limit;
    };
    int64_t group = -1;
    bool several = false;
    for (const auto& p : game.players) {
        if (!stalled(p))
            continue;
        if (group < 0)
            group = p.machine_group;
        else if (group != p.machine_group)
            several = true;
    }
    if (several)
        return no_player_id;
    for (const auto& p : game.players)
        if (stalled(p))
            return p.player_id;
    return no_player_id;
}

void net_match_designate_host(World* world) noexcept {
    uint32_t highest = 0;
    for (const auto& p : world->game.players)
        if (p.in_use != 0 &&
            (p.status == OA_PLAYER_STATUS_MIRRORED || p.status == OA_PLAYER_STATUS_LOCAL) &&
            highest < p.player_id)
            highest = p.player_id;
    auto* successor = player_of(world, highest);
    if (successor == nullptr)
        return;
    if (auto* info = world_player_info(world, successor))
        info->role = static_cast<uint8_t>(info->role | role_host);
}

bool net_match_timeout_expired(NetMatch* m, uint32_t player_id) noexcept {
    auto* p = player_of(m->world, player_id);
    if (p == nullptr || !is_remote(*p) || m->rules.vote_reject)
        return false;
    const auto seconds =
        base::game_loop::scaled_clock_elapsed(now_time(m), p->last_update_time) / 30u;
    if (seconds < m->world->game.player_timeout_seconds + timeout_drop_extra_seconds)
        return false;
    reject_player(m, player_id, reject_connection_lost);
    return true;
}

void net_match_sync_timing(const World* world, base::game_loop::Timing* timing) noexcept {
    for (std::size_t i = 0; i < OA_PLAYER_COUNT && i < timing->players.size(); ++i) {
        const auto& p = world->game.players[i];
        auto& peer = timing->players[i];
        peer.present = p.in_use != 0;
        peer.status = p.status;
        peer.eligible = p.unit_count;
        peer.tick = static_cast<uint32_t>(p.last_sim_tick);
    }
}

void net_match_set_pause(NetMatch* m, bool paused) noexcept {
    auto& game = m->world->game;
    if (!paused && m->recorder.autopause_holding) {
        // Under autopause only the host starts the game.
        if (host_slot(m->world) != game.local_player_index) {
            game.sim_run_flags = static_cast<uint16_t>(game.sim_run_flags | run_flag_paused);
            notice(m, own_words("Autopause: only the host can unpause", {}).c_str());
            return;
        }
        m->recorder.autopause_holding = false;
    }
    game.sim_run_flags = static_cast<uint16_t>(
        (game.sim_run_flags & ~run_flag_paused) | (paused ? run_flag_paused : 0)
    );
    // Unpausing a game held for its commanders' warp ends the warp for everyone.
    if (!paused && m->recorder.start_paused)
        m->recorder.warp_released = true;
    PauseSpeedRecord r{};
    r.kind = pause_speed_kind_pause;
    r.value = paused ? 1 : 0;
    send_record(m, primary_id(m->world), broadcast_destination_id, r);
}

bool net_match_speed_range(const NetMatch* m, uint8_t* slowest, uint8_t* fastest) noexcept {
    const auto& rules = m->rules;
    const uint8_t fastest_allowed =
        rules.speed_max < rules.speed_min ? rules.speed_min : rules.speed_max;
    const bool locked = rules.speed_lock && rules.recorder_protocol != recorder_protocol_plain &&
                        m->recorder.options.speed_lock != 0;
    uint8_t low = rules.speed_min;
    uint8_t high = fastest_allowed;
    if (locked)
        recorder_speed_range(m->recorder, rules.speed_min, fastest_allowed, &low, &high);
    if (slowest != nullptr)
        *slowest = low;
    if (fastest != nullptr)
        *fastest = high;
    return locked;
}

void net_match_set_speed(NetMatch* m, int32_t speed, bool broadcast) noexcept {
    auto& game = m->world->game;
    // A local change outside the recorder's lock is not made at all.
    if (broadcast && speed_locked_out(m, speed))
        return;
    if (speed > m->rules.speed_max)
        speed = m->rules.speed_max;
    if (speed < m->rules.speed_min)
        speed = m->rules.speed_min;
    if (static_cast<uint16_t>(speed) != game.requested_speed) {
        // The line is the speed keys' own, in the language shown, for a
        // speed another machine set as for one set here, as in 3.1c.
        oa::sim::messages::Hooks words{};
        words.context = m;
        if (m->hooks.translate_game_text != nullptr)
            words.translate = [](void* context, const char* text) {
                return game_text(static_cast<const NetMatch*>(context), text);
            };
        char line[oa::sim::speed::message_bytes];
        oa::sim::speed::format_message(line, speed, words);
        notice(m, line);
    }
    game.requested_speed = static_cast<uint16_t>(speed);
    game.current_speed = static_cast<uint16_t>(speed);
    if (!broadcast)
        return;
    PauseSpeedRecord r{};
    r.kind = pause_speed_kind_speed;
    r.value = static_cast<uint8_t>(speed);
    send_record(m, primary_id(m->world), broadcast_destination_id, r);
}

/// The player a chat line is sent as.
///
/// The first human. +AI hands that human to the computer before the line is
/// posted, and the line is still theirs: a computer player seated beside
/// them is not the speaker.
///
/// @param game The match's players.
/// @return That player's id, or no_player_id when this machine has none.
static uint32_t chat_sender_id(const Game& game) noexcept {
    const auto human = first_local_player_id(game);
    if (human != no_player_id)
        return human;
    if (game.local_player_index >= OA_PLAYER_COUNT)
        return no_player_id;
    const Player& speaker = game.players[game.local_player_index];
    if (speaker.in_use != 0 && speaker.status == OA_PLAYER_STATUS_COMPUTER &&
        speaker.player_id != no_player_id)
        return speaker.player_id;
    return no_player_id;
}

namespace {

/// Where one chat line is sent.
struct ChatRoute {
    uint32_t to[OA_PLAYER_COUNT]{}; ///< destination ids
    std::size_t count{};            ///< how many `to` holds
};

/// Destinations of one chat line.
///
/// A command, or mode everyone, is one broadcast. Chosen players are the
/// slots whose target entry is set and whose player id is not 0. Allies and
/// enemies are the remote players by the speaker's alliance row. Any other
/// mode, this machine only included, sends nothing.
///
/// @param game the match's players
/// @param speaker the player whose alliance row routes allies and enemies
/// @param mode OA_CHAT_MODE_* or the local-only mode
/// @param targets chosen-player slots; null reaches nobody in chosen mode
/// @param command a line whose first byte is '+'
/// @return the destination ids
ChatRoute chat_route(
    const Game& game, const Player& speaker, uint8_t mode, const uint8_t* targets, bool command
) noexcept {
    ChatRoute route{};
    if (command || mode == OA_CHAT_MODE_EVERYONE) {
        route.to[route.count++] = broadcast_destination_id;
    } else if (mode == OA_CHAT_MODE_CHOSEN) {
        if (targets != nullptr)
            for (std::size_t slot = 0; slot < OA_PLAYER_COUNT; ++slot)
                if (targets[slot] != 0 && game.players[slot].player_id != 0)
                    route.to[route.count++] = game.players[slot].player_id;
    } else {
        for (std::size_t slot = 0; slot < OA_PLAYER_COUNT; ++slot) {
            const auto& remote = game.players[slot];
            if (!is_remote(remote))
                continue;
            const bool allied = speaker.alliance[slot] != 0;
            if ((mode == OA_CHAT_MODE_ALLIES && allied) ||
                (mode == OA_CHAT_MODE_ENEMIES && !allied))
                route.to[route.count++] = remote.player_id;
        }
    }
    return route;
}

} // namespace

void net_match_say(NetMatch* m, const char* text) noexcept {
    m->said_parts.clear();
    auto& game = m->world->game;
    const auto from = chat_sender_id(game);
    const uint8_t mode = game.chat_mode;
    const bool command = text != nullptr && text[0] == '+';
    const ChatRoute route =
        chat_route(game, game.players[game.local_player_index], mode, game.chat_targets, command);
    if (text == nullptr || !m->unicode_chat) {
        // The text's first 64 bytes, the rest zero: a line of 64 characters
        // or more fills the field with no terminator, as 3.1c's record
        // carries it. A UTF-8 character the 64th byte would split is left
        // out whole.
        ChatRecord r{};
        if (text != nullptr)
            std::memcpy(r.text, text, chat_bytes_within(text, sizeof r.text));
        for (std::size_t i = 0; i < route.count; ++i)
            send_record(m, from, route.to[i], r);
    } else {
        auto parts =
            say_unicode(m, from, {route.to, route.count}, text, sizeof(ChatRecord::text), true);
        if (parts.size() > 1)
            m->said_parts = std::move(parts);
    }
    // The recorder answers a command after the line that typed it.
    if (const auto* self = player_of(m->world, from); self != nullptr && text != nullptr)
        recorder_chat_line(m, *self, text);
}

void net_match_say_from(
    NetMatch* match, uint8_t seat, const char* line, uint8_t mode, const uint8_t* targets
) noexcept {
    if (match == nullptr || match->world == nullptr || line == nullptr || seat >= OA_PLAYER_COUNT)
        return;
    const Player& speaker = match->world->game.players[seat];
    if (speaker.in_use == 0 || speaker.status != OA_PLAYER_STATUS_COMPUTER)
        return;
    if (speaker.player_id == no_player_id || speaker.player_id == broadcast_destination_id)
        return;
    if (chat_command(line))
        return;
    const ChatRoute route = chat_route(match->world->game, speaker, mode, targets, false);
    if (!match->unicode_chat) {
        ChatRecord record{};
        std::memcpy(record.text, line, chat_bytes_within(line, sizeof record.text));
        for (std::size_t i = 0; i < route.count; ++i)
            send_record(match, speaker.player_id, route.to[i], record);
        return;
    }
    (void)say_unicode(
        match, speaker.player_id, {route.to, route.count}, line, sizeof(ChatRecord::text), true
    );
}

std::string net_match_chat_text(const NetMatch* m, const Player& from, std::string_view bytes) {
    const bool sender_utf8 =
        (m->unicode_chat || m->recorded_chat) && from.index < OA_PLAYER_COUNT &&
        announces_unicode_chat(
            reinterpret_cast<const uint8_t*>(&m->world->player_info[from.index])
        );
    std::string line(bytes);
    if (sender_utf8) {
        line = chat_strict_utf8(bytes);
        if (!m->unicode_chat)
            line = chat_code_page(line);
    }
    // The alliance lines arrive in English and show in the language shown.
    char shown[oa::sim::messages::most_chat_line_bytes];
    oa::sim::messages::format_shown_chat_line(
        shown, sizeof shown, line, m->hooks.translate_game_text, m->hooks.context
    );
    return shown;
}

void net_match_give(NetMatch* m, uint8_t from, uint8_t to, bool metal, float amount) noexcept {
    auto* world = m->world;
    if (from >= OA_PLAYER_COUNT || to >= OA_PLAYER_COUNT)
        return;
    const auto& source = world->game.players[from];
    const float stored = metal ? source.metal : source.energy;
    if (stored < amount)
        amount = stored;
    if (amount == 0.0F)
        return;
    if (m->hooks.debit != nullptr)
        (void)m->hooks.debit(m->hooks.context, world, from, metal, amount);
    if (m->hooks.credit != nullptr)
        m->hooks.credit(m->hooks.context, world, to, metal, amount);
    net_match_send_give(m, from, to, metal, amount);
}

void net_match_send_give(NetMatch* m, uint8_t from, uint8_t to, bool metal, float amount) noexcept {
    if (from >= OA_PLAYER_COUNT || to >= OA_PLAYER_COUNT)
        return;
    const auto& source = m->world->game.players[from];
    const auto& target = m->world->game.players[to];
    if (!participating(source) || !participating(target))
        return;
    ResourceGiveRecord r{};
    r.subtype = metal ? give_metal : give_energy;
    r.from_id = source.player_id;
    r.to_id = target.player_id;
    r.amount = amount;
    send_record(m, source.player_id, target.player_id, r);
}

void net_match_share_sight(NetMatch* m, uint8_t from, uint8_t to) noexcept {
    if (from >= OA_PLAYER_COUNT || to >= OA_PLAYER_COUNT)
        return;
    auto& source = m->world->game.players[from];
    auto& target = m->world->game.players[to];
    ResourceGiveRecord r{};
    r.subtype = give_sight;
    r.from_id = source.player_id;
    r.to_id = target.player_id;
    send_record(m, source.player_id, target.player_id, r);
}

void net_match_economy_period(NetMatch* m) noexcept {
    auto& game = m->world->game;
    const auto periods = ++m->connection->economy_periods;
    if ((periods & (economy_send_every - 1)) == 0 && game.viewpoint_player < OA_PLAYER_COUNT)
        send_economy(m, game.players[game.viewpoint_player], nullptr, 0);
}

bool net_match_final_economy(NetMatch* m) noexcept {
    auto& game = m->world->game;
    bool settled = true;
    for (auto& from : game.players) {
        if (!is_local(from) || from.units_created == 0 || from.reject_reason != 0)
            continue;
        for (auto& to : game.players) {
            const bool counted = is_remote(to) || to.units_created == 0 || to.reject_reason != 0;
            if (!counted || to.index > OA_PLAYER_COUNT)
                continue;
            const auto index = to.index;
            const bool unanswered = economy_requested(from)[index] == 0 ||
                                    economy_answered(from)[index] == 0 ||
                                    economy_processed(from)[index] == 0;
            if ((remote_playing(m->world, to) && unanswered) ||
                (is_remote(to) && economy_processed(from)[index] == 0)) {
                send_economy(m, from, &to, 1);
                settled = false;
            }
        }
    }
    if (m->connection != nullptr && m->connection->host != nullptr)
        sock::host_pump(m->connection->host, final_economy_wait_ms);
    return settled;
}

} // namespace oa::netgame::match

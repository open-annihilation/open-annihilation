// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/netgame/match/session_lobby.hpp"

#include "oa/netgame/match/net_match.hpp"
#include "oa/netgame/records.hpp"

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace oa::netgame::match {
namespace {

namespace mp = oa::ui::frontend_multiplayer;

constexpr uint32_t time_rate = 30; // game time base ticks per second
constexpr uint32_t lobby_max_players = 10;
constexpr uint16_t options_game_closed =
    0x8000; // PlayerSetupInfo.options (OA_SETUP_OPTION_GAME_CLOSED)
constexpr uint16_t status_password = 0x0001; // PlayerSetupInfo.status (OA_SETUP_STATUS_PASSWORD)
// Where PlayerSetupInfo.options and .status sit in the session's user bytes,
// which start at PlayerSetupInfo.memory_mb.
constexpr std::size_t user_options_offset = 2;
constexpr std::size_t user_status_offset = 4;
constexpr std::size_t max_address_bytes = 256; // a host name is at most 253 characters

NetConnection& self(void* context) {
    return *static_cast<NetConnection*>(context);
}

void backend_pump(void* context, uint32_t wait_ms) {
    auto& c = self(context);
    sock::host_pump(c.host, c.pump_other != nullptr ? wait_ms / 2 : wait_ms);
    if (c.pump_other != nullptr)
        c.pump_other(c.pump_context, wait_ms / 2);
}

uint32_t backend_now(void* context) {
    return sock::host_now_ms(self(context).host);
}

bool backend_listen(void* context) {
    return sock::host_listen_enumeration(self(context).host);
}

session::Backend backend_of(NetConnection* c) {
    session::Backend b = sock::host_session_backend(c->host);
    b.context = c;
    b.pump = backend_pump;
    b.now_ms = backend_now;
    b.listen_enumeration = backend_listen;
    return b;
}

bool parse_ipv4(const char* text, uint8_t out[4]) {
    if (text == nullptr)
        return false;
    const char* at = text;
    for (int part = 0; part < 4; ++part) {
        char* end = nullptr;
        const long value = std::strtol(at, &end, 10);
        if (end == at || value < 0 || value > 255)
            return false;
        out[part] = static_cast<uint8_t>(value);
        at = end;
        if (part < 3) {
            if (*at != '.')
                return false;
            ++at;
        }
    }
    return *at == '\0';
}

/// Copies an address without the white space around it.
///
/// @param text the address as typed; null reads as blank
/// @param[out] out the trimmed address, terminated
/// @param capacity bytes available in out, at least 1
/// @return false, leaving out blank, when the trimmed address does not fit
bool trim_address(const char* text, char* out, std::size_t capacity) {
    out[0] = '\0';
    if (text == nullptr)
        return true;
    while (std::isspace(static_cast<unsigned char>(*text)))
        ++text;
    std::size_t length = std::strlen(text);
    while (length > 0 && std::isspace(static_cast<unsigned char>(text[length - 1])))
        --length;
    if (length >= capacity)
        return false;
    std::memcpy(out, text, length);
    out[length] = '\0';
    return true;
}

// The game's queue outlives every session, so the configured rate
// survives the reset.
void reset_packet_queue(NetConnection& c) {
    packet_layer_create(c.packets);
    (void)packet_layer_set_rate(c.packets, c.sends_per_second);
}

/// Enters a session: starts the packet layer over the session's transport with guaranteed delivery.
///
/// @param[in,out] c Connection entering the session.
/// @param hosting True when this machine created the session.
/// @param local_id Transport id of the local player.
/// @param players The battle room's player table, naming the peers whose frame slots stay theirs.
void start_session(NetConnection* c, bool hosting, uint32_t local_id, const Player* players) {
    c->in_session = true;
    c->hosting = hosting;
    c->local_id = local_id;
    packet_layer_start(c->packets, session::session_transport(&c->session));
    c->packets->receiver.players = players;
    // An OA machine always reads presence records; only OA sends them,
    // each alone in its frame.
    c->packets->receiver.presence_records = true;
    // Battle-room traffic is guaranteed.
    (void)session::session_set_guaranteed(&c->session, true);
    c->packets->guaranteed = true;
}

int32_t providers(void*, mp::Provider* out, int32_t capacity) {
    if (out == nullptr || capacity < 1)
        return 0;
    mp::Provider provider{};
    std::snprintf(
        provider.name, sizeof provider.name, "%s", "Internet TCP/IP Connection For DirectPlay"
    );
    std::memcpy(provider.guid, mp::kProviderGuidTcpip, sizeof provider.guid);
    provider.kind = mp::ProviderKind::tcpip;
    out[0] = provider;
    return 1;
}

bool open(void* context, const mp::Provider* provider, const char* address) {
    auto& c = self(context);
    if (provider != nullptr && provider->kind != mp::ProviderKind::tcpip)
        return false;
    if (c.opened)
        return true;
    // Start from the configured target every time: opening keeps the
    // previous address in the host's own config.
    sock::HostConfig config = c.host_config;
    char name[max_address_bytes];
    const bool fits = trim_address(address, name, sizeof name);
    c.address_unresolved = false;
    if (!fits || name[0] != '\0') {
        uint8_t ip[4]{};
        if (fits && (parse_ipv4(name, ip) || sock::resolve_ipv4(name, ip)))
            std::memcpy(config.enum_target, ip, 4);
        else
            c.address_unresolved = true;
    }
    if (!sock::host_open(c.host, config))
        return false;
    session::session_init_multiplay(&c.session, backend_of(&c), c.application);
    session::session_init_defaults(&c.session);
    c.session.publish_without_password = c.rules.clear_session_password;
    c.opened = true;
    return true;
}

int32_t enumerate(void* context, mp::SessionEntry* out, int32_t capacity) {
    auto& c = self(context);
    if (!c.opened || out == nullptr || c.address_unresolved)
        return -1;
    session::GameEntry games[session::max_game_entries]{};
    // A request that could not be sent anywhere finds no games; only an
    // address that resolves to nothing fails.
    if (session::session_get_games(&c.session, games, session::max_game_entries) < 0)
        return 0;
    const auto& engine = c.host->engine;
    int32_t count = 0;
    for (uint32_t i = 0; i < engine.session_count && count < capacity; ++i) {
        const auto& found = engine.sessions[i];
        mp::SessionEntry entry{};
        std::memcpy(entry.instance_guid, found.instance.bytes, 16);
        // The name is cut to the entry's field; a failed format leaves it empty.
        if (std::snprintf(entry.name, sizeof entry.name, "%s", found.name) < 0)
            entry.name[0] = '\0';
        entry.max_players = found.desc.max_players;
        entry.current_players = found.desc.current_players;
        entry.flags = found.desc.flags;
        for (std::size_t u = 0; u < 4; ++u)
            store_u32(entry.user + 4 * u, found.desc.user[u]);
        out[count++] = entry;
    }
    return count;
}

/// Adds the local player to the session, presenting the version a host requires.
///
/// @param[in,out] c Connection whose session gains the player.
/// @param nickname Short and long name of the player.
/// @param tag Join tag (the session password), or null for none.
/// @param[out] local_id Receives the new player's id when not null.
/// @return ok, or failed when the session refused the player.
mp::LobbyResult
add_local_player(NetConnection& c, const char* nickname, const char* tag, uint32_t* local_id) {
    uint32_t id = 0;
    if (!session::session_add_player(
            &c.session,
            &id,
            nickname,
            nickname,
            tag != nullptr ? tag : "",
            create_player_version_low,
            create_player_version_high
        ))
        return mp::LobbyResult::failed;
    if (local_id != nullptr)
        *local_id = id;
    return mp::LobbyResult::ok;
}

mp::LobbyResult host(void* context, const mp::HostRequest* request, uint32_t* local_id) {
    auto& c = self(context);
    if (!c.opened || request == nullptr)
        return mp::LobbyResult::failed;
    c.session.max_players = request->max_players != 0 ? request->max_players : lobby_max_players;
    const char* password = request->password != nullptr ? request->password : "";
    // The session is created with its full name and the host's user bytes,
    // as the game creates it. It has no password (the game creates it with
    // an empty one): a joiner's password is checked against the join tag,
    // so 3.1c clients, which join without one, are not refused.
    const char* name =
        request->session_name != nullptr ? request->session_name : request->game_name;
    uint32_t user[4]{};
    if (request->user != nullptr)
        for (std::size_t u = 0; u < 4; ++u)
            user[u] = load_u32(request->user + 4 * u);
    if (!session::session_create_game(&c.session, name, "", user[0], user[1], user[2], user[3]))
        return mp::LobbyResult::failed;
    std::snprintf(c.join_tag, sizeof c.join_tag, "%.16s", password);
    c.host_options = request->user != nullptr ? load_u16(request->user + user_options_offset) : 0;
    uint32_t id = 0;
    const auto result = add_local_player(c, request->nickname, password, &id);
    if (result != mp::LobbyResult::ok) {
        (void)session::session_quit_game(&c.session);
        return result;
    }
    start_session(&c, true, id, request->players);
    if (local_id != nullptr)
        *local_id = id;
    return mp::LobbyResult::ok;
}

void list_roster_player(void* context, uint32_t id) {
    auto& c = self(context);
    if (c.roster_count < dplay::max_players)
        c.roster[c.roster_count++] = id;
}

// The name a seated player shows: the session's long name.
void event_name(const NetConnection& c, uint32_t id, mp::LobbyEvent* out) {
    char short_name[sizeof out->name];
    char long_name[sizeof out->name];
    if (session::session_player_names(&c.session, id, short_name, long_name, sizeof long_name))
        std::snprintf(out->name, sizeof out->name, "%s", long_name);
}

/// Joins the chosen session, creates the local player with the join block and starts the packet layer.
///
/// Every player already in the session is then enumerated; receive hands
/// each to the lobby like a new arrival.
///
/// @param context The NetConnection.
/// @param request Session, nickname, password and battle room player table.
/// @param[out] local_id Transport id of the local player, when not null.
/// @return ok, or failed when the connection is not open, the join fails or the player cannot be added.
mp::LobbyResult join(void* context, const mp::JoinRequest* request, uint32_t* local_id) {
    auto& c = self(context);
    if (!c.opened || request == nullptr || request->session == nullptr)
        return mp::LobbyResult::failed;
    dplay::Guid instance{};
    std::memcpy(instance.bytes, request->session->instance_guid, 16);
    if (!session::session_join_game(&c.session, instance))
        return mp::LobbyResult::failed;
    c.join_tag[0] = '\0';
    uint32_t id = 0;
    const auto result = add_local_player(c, request->nickname, request->password, &id);
    if (result != mp::LobbyResult::ok) {
        (void)session::session_quit_game(&c.session);
        return result;
    }
    start_session(&c, false, id, request->players);
    c.roster_count = 0;
    (void)session::session_enum_players(&c.session, list_roster_player, &c);
    if (local_id != nullptr)
        *local_id = id;
    return mp::LobbyResult::ok;
}

// A computer player is a session player of the machine that seats it.
mp::LobbyResult add_player(void* context, const char* name, const char* tag, uint32_t* id) {
    auto& c = self(context);
    if (!c.in_session)
        return mp::LobbyResult::failed;
    const auto result = add_local_player(c, name, tag, id);
    if (result == mp::LobbyResult::ok)
        list_roster_player(&c, *id);
    return result;
}

void remove_player(void* context, uint32_t id) {
    auto& c = self(context);
    if (c.in_session)
        (void)session::session_remove_player(&c.session, id);
}

void send(void* context, uint32_t to_id, const uint8_t* data, std::size_t size) {
    auto& c = self(context);
    net_connection_send_from(&c, c.local_id, to_id, data, size, false);
}

void send_from(
    void* context,
    uint32_t from_id,
    uint32_t to_id,
    const uint8_t* data,
    std::size_t size,
    bool unguaranteed
) {
    net_connection_send_from(&self(context), from_id, to_id, data, size, unguaranteed);
}

void flush(void* context) {
    net_connection_flush(&self(context));
}

/// Tells whether a transport id names a player this machine created: the local player or one of its
/// computer players.
///
/// @param c Connection whose session holds the players.
/// @param id Transport id.
/// @return True for an application player created here.
bool created_here(const NetConnection& c, uint32_t id) {
    const auto* player = dplay::engine_find_player(&c.host->engine, id);
    return player != nullptr && player->local &&
           (player->info.flags & dplay::player_flag::system_player) == 0;
}

/// Gives the host's verdict on a joining player's create message.
///
/// A refused joiner is not destroyed: the battle room tells it why, and it
/// leaves by itself, as with 3.1c. A host with a launch active does not
/// refuse a joiner because the game is closed.
///
/// @param c Connection; only a hosting one checks anything.
/// @param image The create-player system message.
/// @param size Message length in bytes.
/// @return The RejectReason the joiner is refused with, 0 when it may stay.
uint8_t join_verdict(const NetConnection& c, const uint8_t* image, uint32_t size) {
    if (!c.hosting)
        return 0;
    const bool launched = c.launch_active != nullptr && c.launch_active(c.launch_context);
    const bool closed = (c.host_options & options_game_closed) != 0 && !launched;
    const bool password = session::session_password_required(&c.session) || c.join_tag[0] != '\0';
    return session::session_join_reject_reason(image, size, closed, password, c.join_tag);
}

/// Answers the records the pump replies to by itself: a 0x06 probe and a 0x02 ping request.
///
/// A ping is echoed with this machine's clock from the player it was
/// addressed to (the local player for a broadcast), at once and without
/// guaranteed delivery. Any other record is left alone.
///
/// @param[in,out] c Connection to reply on.
/// @param packet Received record, at least one byte.
void answer_control(NetConnection& c, const Packet& packet) {
    const auto type = static_cast<RecordType>(packet.data[0]);
    if (type == RecordType::probe) {
        const uint8_t reply = static_cast<uint8_t>(RecordType::probe_reply);
        net_connection_send_now(&c, packet.from_id, &reply, 1);
        return;
    }
    if (type == RecordType::ping) {
        PingRecord ping{};
        if (decode_record(packet.data, packet.size, &ping) != WireError::ok ||
            ping.echo_tick_count != 0)
            return;
        ping.echo_tick_count = sock::host_now_ms(c.host);
        uint8_t bytes[16];
        std::size_t written = 0;
        if (encode_record(ping, bytes, sizeof bytes, &written) != WireError::ok)
            return;
        net_connection_send_from(&c, packet.to_id, ping.origin_player_id, bytes, written, true);
    }
}

bool receive(void* context, mp::LobbyEvent* out) {
    auto& c = self(context);
    if (!c.in_session || out == nullptr)
        return false;
    while (c.roster_count > 0) {
        const auto id = c.roster[--c.roster_count];
        if (dplay::engine_find_player(&c.host->engine, id) == nullptr)
            continue;
        *out = mp::LobbyEvent{};
        out->kind = mp::LobbyEventKind::player_joined;
        out->player_id = id;
        event_name(c, id, out);
        return true;
    }
    // Records queued outside the battle room's set points still go out
    // once the frame interval has passed.
    const auto now = net_connection_time(&c);
    packet_layer_flush(c.packets, now, false);
    Packet packet{};
    while (packet_layer_receive(c.packets, static_cast<int32_t>(now), &packet)) {
        *out = mp::LobbyEvent{};
        if (packet.kind == PacketKind::system) {
            uint32_t message_type = 0;
            if (!dplay::read_system_message_type(packet.data, packet.size, &message_type))
                continue;
            const auto type = static_cast<SystemMessageType>(message_type);
            if (type == SystemMessageType::player_created) {
                dplay::CreatePlayerView view{};
                if (!dplay::decode_create_player_image(packet.data, packet.size, &view) ||
                    view.id == c.local_id)
                    continue;
                out->kind = mp::LobbyEventKind::player_joined;
                out->player_id = view.id;
                out->refuse_reason = join_verdict(c, packet.data, packet.size);
                event_name(c, view.id, out);
                return true;
            }
            dplay::PlayerDestroyedView destroyed{};
            if (dplay::decode_player_destroyed_image(packet.data, packet.size, &destroyed)) {
                out->kind = mp::LobbyEventKind::player_left;
                out->player_id = destroyed.id;
                packet_layer_release_peer(c.packets, out->player_id);
                return true;
            }
            if (type == SystemMessageType::session_lost) {
                out->kind = mp::LobbyEventKind::session_lost;
                return true;
            }
            continue;
        }
        // A broadcast from one of this machine's players also reaches its
        // other players; the game does not hear from itself.
        if (created_here(c, packet.from_id))
            continue;
        if (packet.size == 0 || packet.size > sizeof out->data)
            continue;
        // A probe or a ping request is answered here and still handed on:
        // every record tells the battle room its sender was heard from.
        answer_control(c, packet);
        out->kind = mp::LobbyEventKind::record;
        out->player_id = packet.from_id;
        out->size = static_cast<uint16_t>(packet.size);
        std::memcpy(out->data, packet.data, packet.size);
        return true;
    }
    return false;
}

void describe(
    void* context, const char* name, const uint8_t user[16], uint32_t flags, uint32_t max_players
) {
    auto& c = self(context);
    if (!c.in_session || !c.hosting || user == nullptr)
        return;
    c.host_options = load_u16(user + user_options_offset);
    if ((load_u16(user + user_status_offset) & status_password) == 0)
        c.join_tag[0] = '\0';
    // Once the game has started, the published description says so for good.
    c.session.desc.flags = flags | (c.session.desc.flags & session_flag_join_disabled);
    c.session.desc.max_players = max_players;
    for (std::size_t u = 0; u < 4; ++u)
        c.session.desc.user[u] = load_u32(user + 4 * u);
    (void)session::session_update_game_info(&c.session, name);
}

void leave(void* context) {
    auto& c = self(context);
    if (!c.in_session)
        return;
    // What is queued goes out before the session is released.
    net_connection_flush(&c);
    (void)session::session_quit_game(&c.session);
    c.in_session = false;
    c.hosting = false;
    c.local_id = 0;
    c.roster_count = 0;
    reset_packet_queue(c);
}

void close(void* context) {
    auto& c = self(context);
    leave(context);
    if (c.opened) {
        session::session_uninit(&c.session);
        sock::host_close(c.host);
    }
    c.opened = false;
}

} // namespace

bool net_connection_create(NetConnection* c, const SessionLobbyConfig& config) noexcept {
    *c = NetConnection{};
    c->host = new (std::nothrow) sock::Host();
    c->packets = new (std::nothrow) PacketLayer();
    if (c->host == nullptr || c->packets == nullptr) {
        net_connection_destroy(c);
        return false;
    }
    c->host_config = config.host;
    c->host->config = config.host;
    if (config.application_named)
        c->application = config.application;
    else
        std::memcpy(c->application.bytes, application_guid, 16);
    packet_layer_create(c->packets);
    (void)net_connection_set_packet_rate(c, config.sends_per_second);
    return true;
}

void net_connection_destroy(NetConnection* c) noexcept {
    if (c->opened)
        close(c);
    delete c->host;
    delete c->packets;
    c->host = nullptr;
    c->packets = nullptr;
}

ui::frontend_multiplayer::LobbyNet session_lobby_net(NetConnection* c) noexcept {
    return {
        c,
        providers,
        open,
        enumerate,
        host,
        join,
        send,
        receive,
        describe,
        leave,
        close,
        add_player,
        remove_player,
        send_from,
        flush
    };
}

void net_connection_set_rules(NetConnection* c, const WireRules& rules) noexcept {
    c->rules = rules;
    c->session.publish_without_password = rules.clear_session_password;
    if (c->packets != nullptr) {
        c->packets->receiver.recorder_records = rules.recorder_protocol != recorder_protocol_plain;
        c->packets->receiver.wide_records = wide_weapon_records(rules);
    }
}

bool net_connection_set_packet_rate(NetConnection* c, int32_t sends_per_second) noexcept {
    if (!packet_layer_set_rate(c->packets, sends_per_second))
        return false;
    c->sends_per_second = sends_per_second;
    return true;
}

uint32_t net_connection_time(const NetConnection* c) noexcept {
    const auto ms = c->host != nullptr ? sock::host_now_ms(c->host) : 0u;
    return static_cast<uint32_t>(ms * time_rate) / 1000u;
}

void net_connection_send_now(
    NetConnection* c, uint32_t to_id, const uint8_t* record, std::size_t size
) noexcept {
    if (!c->in_session || record == nullptr || size == 0)
        return;
    const auto now = net_connection_time(c);
    if (packet_layer_send(c->packets, c->local_id, to_id, record, size, now) == WireError::ok)
        packet_layer_flush(c->packets, now, true);
}

void net_connection_finish(NetConnection* c, Game* game) noexcept {
    if ((game->session_flags & kNetFlagLive) == 0)
        return;
    if (c->packets != nullptr)
        packet_layer_flush(c->packets, net_connection_time(c), true);
    close(c);
    game->session_flags = static_cast<uint8_t>(game->session_flags & ~kNetFlagLive);
}

void net_connection_send_from(
    NetConnection* c,
    uint32_t from_id,
    uint32_t to_id,
    const uint8_t* record,
    std::size_t size,
    bool unguaranteed
) noexcept {
    if (!c->in_session || record == nullptr || size == 0)
        return;
    const auto from = created_here(*c, from_id) ? from_id : c->local_id;
    const auto now = net_connection_time(c);
    if (!unguaranteed || !c->packets->guaranteed) {
        (void)packet_layer_send(c->packets, from, to_id, record, size, now);
        return;
    }
    packet_layer_flush(c->packets, now, true);
    c->packets->guaranteed = false;
    if (packet_layer_send(c->packets, from, to_id, record, size, now) == WireError::ok)
        packet_layer_flush(c->packets, now, true);
    c->packets->guaranteed = true;
}

void net_connection_flush(NetConnection* c) noexcept {
    if (c->in_session)
        packet_layer_flush(c->packets, net_connection_time(c), true);
}

void net_connection_republish(NetConnection* c, const uint8_t* user) noexcept {
    if (!c->in_session || !c->hosting)
        return;
    if (user != nullptr) {
        c->host_options = load_u16(user + user_options_offset);
        for (std::size_t u = 0; u < 4; ++u)
            c->session.desc.user[u] = load_u32(user + 4 * u);
    }
    // The session keeps only the name's first 16 characters; the engine
    // holds the full name last published.
    char name[sizeof c->host->engine.session_name];
    std::memcpy(name, c->host->engine.session_name, sizeof name);
    name[sizeof name - 1] = '\0';
    (void)session::session_update_game_info(&c->session, name);
}

} // namespace oa::netgame::match

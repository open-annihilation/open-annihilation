// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/netgame/dplay/engine.hpp"

#include <cstdint>
#include <cstring>

namespace oa::netgame::dplay {
namespace {

constexpr uint32_t request_flags_system = player_flag::system_player | player_flag::local;
constexpr uint32_t request_flags_player = player_flag::local;
constexpr uint32_t dpname_bytes = 0x10;

uint32_t next_random(Engine* e) {
    uint32_t x = e->rng != 0 ? e->rng : 0x9e3779b9u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    e->rng = x;
    return x;
}

void copy_name(char* out, const char* in) {
    std::size_t n = 0;
    for (; in != nullptr && in[n] != '\0' && n < max_name_chars; ++n)
        out[n] = in[n];
    out[n] = '\0';
}

bool is_open(const Engine* e) {
    return e->state == EngineState::open_host || e->state == EngineState::open_client;
}

bool is_system(const PlayerInfo& p) {
    return (p.flags & player_flag::system_player) != 0;
}

EnginePlayer* find_player(Engine* e, uint32_t id) {
    if (id == 0)
        return nullptr;
    for (auto& p : e->players)
        if (p.used && p.info.id == id)
            return &p;
    return nullptr;
}

EnginePlayer* free_slot(Engine* e) {
    for (auto& p : e->players)
        if (!p.used && !p.reserved)
            return &p;
    return nullptr;
}

uint32_t slot_id(const Engine* e, std::size_t slot) {
    return ((static_cast<uint32_t>(e->uniqueness[slot]) << 16) | static_cast<uint32_t>(slot)) ^
           e->id_key;
}

// Low 16 bits of an id once the key is removed: its slot in the issuing
// name server's table.
constexpr uint32_t id_slot_mask = 0xffff;

/// Returns the lowest slot no player's id holds, as the name server issues ids.
///
/// A machine that took the name server's place holds ids another name
/// server issued, in places of its own table that need not match their
/// slots, so slots are read from the ids themselves.
///
/// @param e Engine holding the players; its key is the session's.
/// @return The slot, or max_players when every slot is taken.
std::size_t free_id_slot(const Engine* e) {
    for (std::size_t slot = 0; slot < max_players; ++slot) {
        bool taken = false;
        for (const auto& p : e->players)
            taken = taken ||
                    ((p.used || p.reserved) && ((p.info.id ^ e->id_key) & id_slot_mask) == slot);
        if (!taken)
            return slot;
    }
    return max_players;
}

// Remote endpoint: a zero ip means "the machine this came from".
Address resolve(const Address& advertised, const Address& source) {
    Address a = advertised;
    if (address_ip_is_zero(a))
        std::memcpy(a.ip, source.ip, 4);
    return a;
}

/// Returns where to answer the machine a message came from: the header's
/// port at the address the message arrived from, or the header's address
/// when the arrival address is unknown (0.0.0.0).
///
/// The header carries the sender's own view of its address, which behind
/// address translation is one this machine cannot reach; the address a
/// message arrived from always can be.
///
/// @param header Endpoint the message header names.
/// @param source Address the message arrived from.
/// @return The endpoint to answer.
Address sender_endpoint(const Address& header, const Address& source) {
    Address a = header;
    if (!address_ip_is_zero(source))
        std::memcpy(a.ip, source.ip, 4);
    return a;
}

Address own_stream(const Engine* e) {
    return e->config.stream;
}

// Stream endpoint of the machine owning player id.
bool player_route(const Engine* e, uint32_t id, const EnginePlayer** system) {
    const EnginePlayer* p = engine_find_player(e, id);
    if (p == nullptr)
        return false;
    if (!is_system(p->info)) {
        p = engine_find_player(e, p->info.system_id);
        if (p == nullptr)
            return false;
    }
    *system = p;
    return true;
}

// Byte offset of the IPv4 address inside the header's sockaddr.
constexpr std::size_t header_ip_offset = 8;

/// Writes this machine's address toward an endpoint into the header of scratch[0, size).
///
/// Only while the advertised address is 0.0.0.0 and the I/O finds one.
///
/// @param[in,out] e Engine whose scratch holds the message.
/// @param to Endpoint the message goes to.
/// @param size Message length in bytes.
void stamp_sender_address(Engine* e, const Address& to, std::size_t size) {
    if (size < header_ip_offset + 4 || !address_ip_is_zero(e->config.stream) ||
        e->io.local_address == nullptr)
        return;
    uint8_t ip[4]{};
    if (e->io.local_address(e->io.context, to, ip))
        std::memcpy(e->scratch + header_ip_offset, ip, 4);
}

bool send_stream(Engine* e, const Address& to, std::size_t size) {
    if (size == 0 || e->io.send_stream == nullptr)
        return false;
    stamp_sender_address(e, to, size);
    return e->io.send_stream(e->io.context, to, e->scratch, size);
}

// Send scratch[0, size) to every remote system player except `except`.
bool send_to_remote_systems(Engine* e, std::size_t size, uint32_t except) {
    bool ok = size != 0;
    for (const auto& p : e->players)
        if (p.used && !p.local && is_system(p.info) && p.info.id != except)
            ok = send_stream(e, p.info.stream, size) && ok;
    return ok;
}

bool queue_push(Engine* e, uint32_t from, uint32_t to, const uint8_t* data, std::size_t size) {
    if (e->queue_count == receive_queue_entries || size > receive_queue_bytes) {
        ++e->dropped_messages;
        return false;
    }
    if (receive_queue_bytes - e->bytes_end < size && e->bytes_start != 0) {
        const uint32_t shift = e->bytes_start;
        std::memmove(e->queue_bytes, e->queue_bytes + shift, e->bytes_end - shift);
        e->bytes_end -= shift;
        e->bytes_start = 0;
        for (uint32_t i = 0; i < e->queue_count; ++i)
            e->queue[(e->queue_head + i) % receive_queue_entries].offset -= shift;
    }
    if (receive_queue_bytes - e->bytes_end < size) {
        ++e->dropped_messages;
        return false;
    }
    auto& q = e->queue[(e->queue_head + e->queue_count) % receive_queue_entries];
    q = QueuedMessage{from, to, e->bytes_end, static_cast<uint32_t>(size)};
    if (size != 0)
        std::memcpy(e->queue_bytes + e->bytes_end, data, size);
    e->bytes_end += static_cast<uint32_t>(size);
    ++e->queue_count;
    return true;
}

// Deliver to every local non-system player except `except`, or once to 0
// when there is none (system messages before a player exists).
void deliver_local(
    Engine* e,
    uint32_t from,
    uint32_t except,
    const uint8_t* data,
    std::size_t size,
    bool fallback_to_zero
) {
    bool any = false;
    for (const auto& p : e->players)
        if (p.used && p.local && !is_system(p.info) && p.info.id != except) {
            (void)queue_push(e, from, p.info.id, data, size);
            any = true;
        }
    if (!any && fallback_to_zero)
        (void)queue_push(e, from, 0, data, size);
}

// Appends a NUL-terminated string to an image and returns its offset.
uint32_t image_string(uint8_t* image, std::size_t* at, const char* s) {
    const uint32_t offset = static_cast<uint32_t>(*at);
    const std::size_t n = std::strlen(s) + 1;
    std::memcpy(image + *at, s, n);
    *at += n;
    return offset;
}

void image_name(uint8_t* image, std::size_t name_at, std::size_t* at, const PlayerInfo& p) {
    store_u32(image + name_at, dpname_bytes);
    store_u32(
        image + name_at + system_message::name_short,
        p.has_short_name ? image_string(image, at, p.short_name) : 0
    );
    store_u32(
        image + name_at + system_message::name_long,
        p.has_long_name ? image_string(image, at, p.long_name) : 0
    );
}

void post_create_player(Engine* e, const PlayerInfo& p) {
    uint8_t
        image[system_message::create_bytes + max_player_data_bytes + 2 * (max_name_chars + 1)]{};
    std::size_t at = system_message::create_bytes;
    store_u32(image, static_cast<uint32_t>(SystemMessageType::player_created));
    store_u32(image + 4, system_message::player_type_player);
    store_u32(image + system_message::create_id, p.id);
    store_u32(image + system_message::create_current_players, e->desc.current_players);
    if (p.data_size != 0) {
        store_u32(image + system_message::create_data, static_cast<uint32_t>(at));
        std::memcpy(image + at, p.data, p.data_size);
        at += p.data_size;
    }
    store_u32(image + system_message::create_data_size, p.data_size);
    image_name(image, system_message::create_name, &at, p);
    store_u32(image + system_message::create_name + dpname_bytes, p.parent_id);
    deliver_local(e, system_message_sender_id, 0, image, at, true);
}

void post_destroy_player(Engine* e, const PlayerInfo& p) {
    uint8_t
        image[system_message::destroy_bytes + max_player_data_bytes + 2 * (max_name_chars + 1)]{};
    std::size_t at = system_message::destroy_bytes;
    store_u32(image, static_cast<uint32_t>(SystemMessageType::player_destroyed));
    store_u32(image + 4, system_message::player_type_player);
    store_u32(image + system_message::destroy_id, p.id);
    if (p.data_size != 0) {
        store_u32(image + system_message::destroy_remote_data, static_cast<uint32_t>(at));
        std::memcpy(image + at, p.data, p.data_size);
        at += p.data_size;
    }
    store_u32(image + system_message::destroy_remote_data_size, p.data_size);
    image_name(image, system_message::destroy_name, &at, p);
    store_u32(image + system_message::destroy_name + dpname_bytes, p.parent_id);
    deliver_local(e, system_message_sender_id, 0, image, at, true);
}

void post_session_desc(Engine* e) {
    uint8_t image[system_message::session_desc_image_bytes + 2 * (max_name_chars + 1)]{};
    std::size_t at = system_message::session_desc_image_bytes;
    SessionDesc d = e->desc;
    d.name_pointer = image_string(image, &at, e->session_name);
    d.password_pointer = image_string(image, &at, e->password);
    store_u32(image, static_cast<uint32_t>(SystemMessageType::session_desc_changed));
    (void)encode_session_desc(d, image + system_message::session_desc, session_desc_bytes);
    deliver_local(e, system_message_sender_id, 0, image, at, true);
}

void post_session_lost(Engine* e) {
    uint8_t image[4]{};
    store_u32(image, static_cast<uint32_t>(SystemMessageType::session_lost));
    deliver_local(e, system_message_sender_id, 0, image, sizeof image, true);
}

/// Tells this machine's players that it has become the session's name server.
///
/// @param[in,out] e Engine whose queue takes the message.
void post_host(Engine* e) {
    uint8_t image[system_message::host_bytes]{};
    store_u32(image, static_cast<uint32_t>(SystemMessageType::host_changed));
    deliver_local(e, system_message_sender_id, 0, image, sizeof image, true);
}

/// Tells this machine's players that a player's data changed, with the new data.
///
/// @param[in,out] e Engine whose queue takes the message.
/// @param p The player, holding its new data.
void post_player_data(Engine* e, const PlayerInfo& p) {
    uint8_t image[system_message::player_data_bytes + max_player_data_bytes]{};
    std::size_t at = system_message::player_data_bytes;
    store_u32(image, static_cast<uint32_t>(SystemMessageType::player_data_changed));
    store_u32(image + 4, system_message::player_type_player);
    store_u32(image + system_message::player_data_id, p.id);
    if (p.data_size != 0) {
        store_u32(image + system_message::player_data_data, static_cast<uint32_t>(at));
        std::memcpy(image + at, p.data, p.data_size);
        at += p.data_size;
    }
    store_u32(image + system_message::player_data_size, p.data_size);
    deliver_local(e, system_message_sender_id, 0, image, at, true);
}

/// Tells this machine's players that a player's name changed, with the new names.
///
/// @param[in,out] e Engine whose queue takes the message.
/// @param p The player, holding its new names.
void post_player_name(Engine* e, const PlayerInfo& p) {
    uint8_t image[system_message::player_name_bytes + 2 * (max_name_chars + 1)]{};
    std::size_t at = system_message::player_name_bytes;
    store_u32(image, static_cast<uint32_t>(SystemMessageType::player_name_changed));
    store_u32(image + 4, system_message::player_type_player);
    store_u32(image + system_message::player_name_id, p.id);
    image_name(image, system_message::player_name_name, &at, p);
    deliver_local(e, system_message_sender_id, 0, image, at, true);
}

SessionDesc wire_desc(const Engine* e) {
    SessionDesc d = e->desc;
    d.name_pointer = 0;
    d.password_pointer = 0;
    return d;
}

PlayerInfo wire_player(const EnginePlayer& p, bool roster) {
    PlayerInfo info = p.info;
    if (p.local)
        info.flags |= player_flag::local;
    if (roster)
        info.flags |= player_flag::in_group;
    return info;
}

/// Makes this machine the session's name server and tells the others.
///
/// The machine keeps the session's key, issues ids from the lowest slot no
/// player holds, answers enumeration requests and requests to join, sends
/// each other machine the announcement, and tells its own players with the
/// 0x0101 system message.
///
/// @param[in,out] e Open client engine.
void become_name_server(Engine* e) {
    e->state = EngineState::open_host;
    e->awaiting_name_server = false;
    e->name_server_id = e->system_id;
    e->id_key = e->desc.reserved[0];
    for (auto& u : e->uniqueness)
        u = 1;
    EnginePlayer* self = find_player(e, e->system_id);
    if (self == nullptr)
        return;
    self->info.flags |= player_flag::name_server;
    e->name_server = own_stream(e);
    if (e->io.listen_enumeration != nullptr)
        (void)e->io.listen_enumeration(e->io.context);
    const uint32_t flags =
        player_flag::system_player | player_flag::name_server | player_flag::in_group;
    for (const auto& p : e->players) {
        if (!p.used || p.local || !is_system(p.info))
            continue;
        const std::size_t n = encode_i_am_name_server(
            own_stream(e),
            p.info.id,
            e->system_id,
            flags,
            self->info.stream,
            self->info.datagram,
            e->scratch,
            sizeof e->scratch
        );
        (void)send_stream(e, p.info.stream, n);
    }
    post_host(e);
}

/// Follows the departure of the name server, or of any machine while a new name server is awaited.
///
/// Without the migrate-host flag the session is lost. With it, the machine
/// whose system player has the lowest id takes the name server's place:
/// this machine when that player is its own, else it awaits the other
/// machine's announcement.
///
/// @param[in,out] e Open client engine.
void name_server_left(Engine* e) {
    if ((e->desc.flags & session_flag::migrate_host) == 0) {
        if (!e->session_lost) {
            e->session_lost = true;
            post_session_lost(e);
        }
        return;
    }
    e->name_server_id = 0;
    const EnginePlayer* lowest = nullptr;
    for (const auto& p : e->players)
        if (p.used && is_system(p.info) && (lowest == nullptr || p.info.id < lowest->info.id))
            lowest = &p;
    if (lowest != nullptr && lowest->local)
        become_name_server(e);
    else
        e->awaiting_name_server = true;
}

// Remove a remote player; a system player takes its players with it.
void remove_player(Engine* e, EnginePlayer* p) {
    if (is_system(p->info)) {
        const uint32_t system = p->info.id;
        for (auto& child : e->players)
            if (child.used && !is_system(child.info) && child.info.system_id == system)
                remove_player(e, &child);
        const bool was_name_server = system == e->name_server_id;
        *p = EnginePlayer{};
        if (e->state == EngineState::open_client && !e->session_lost &&
            (was_name_server || e->awaiting_name_server))
            name_server_left(e);
        return;
    }
    const PlayerInfo info = p->info;
    *p = EnginePlayer{};
    if (e->desc.current_players != 0)
        --e->desc.current_players;
    post_destroy_player(e, info);
}

// announce: post the player-created system message (not for the roster a
// joiner receives, which the game reads by enumerating players).
EnginePlayer*
add_remote_player(Engine* e, const PlayerInfo& in, const Address& source, bool announce) {
    EnginePlayer* p = find_player(e, in.id);
    if (p != nullptr)
        return p;
    // The host may hold this id as its own reservation.
    for (auto& slot : e->players)
        if (slot.reserved && slot.info.id == in.id)
            p = &slot;
    if (p == nullptr)
        p = free_slot(e);
    if (p == nullptr)
        return nullptr;
    *p = EnginePlayer{};
    p->used = true;
    p->info = in;
    p->info.flags &= ~player_flag::local;
    if (is_system(in))
        p->info.system_id = in.id;
    if (in.has_addresses) {
        p->info.stream = resolve(in.stream, source);
        p->info.datagram = resolve(in.datagram, source);
    }
    if (!is_system(in)) {
        ++e->desc.current_players;
        if (announce)
            post_create_player(e, p->info);
    }
    return p;
}

EnginePlayer* add_local_player(
    Engine* e, EnginePlayer* slot, uint32_t id, uint32_t flags, const PlayerInfo& shape
) {
    *slot = EnginePlayer{};
    slot->used = true;
    slot->local = true;
    slot->info = shape;
    slot->info.id = id;
    slot->info.flags = flags;
    slot->info.version = protocol_version;
    slot->info.system_id = (flags & player_flag::system_player) != 0 ? id : e->system_id;
    slot->info.has_addresses = true;
    slot->info.stream = e->config.stream;
    slot->info.datagram = e->config.datagram;
    std::memset(slot->info.stream.ip, 0, 4);
    std::memset(slot->info.datagram.ip, 0, 4);
    return slot;
}

void announce_player(Engine* e, const EnginePlayer& p, uint32_t except) {
    const std::size_t n = encode_player_message(
        own_stream(e),
        command::create_player,
        0,
        wire_player(p, false),
        nullptr,
        0,
        e->scratch,
        sizeof e->scratch
    );
    (void)send_to_remote_systems(e, n, except);
}

/// Sends this machine's own players straight to a machine it has just met.
///
/// A player is announced only to the machines known when it is created, and
/// the list a new machine receives is the list as the host had it when that
/// machine was admitted. A player created in between is on neither, so each
/// of this machine's own players is sent straight to the new machine, as the
/// same announcement a machine sends when it creates a player. A machine
/// that already has the player keeps the one it has.
///
/// @param[in,out] e Engine sending.
/// @param to Stream endpoint carried by the new machine's system player.
/// @quirk The announcement is the one the game's own players already
///     exchange. A separate verification of a recent player belongs to a
///     later version of the session protocol, and a peer from the game
///     would not take it.
void send_local_application_players(Engine* e, const Address& to) {
    if (to.port == 0)
        return;
    for (const auto& player : e->players) {
        if (!player.used || !player.local || is_system(player.info))
            continue;
        const std::size_t n = encode_player_message(
            own_stream(e),
            command::create_player,
            0,
            wire_player(player, false),
            nullptr,
            0,
            e->scratch,
            sizeof e->scratch
        );
        (void)send_stream(e, to, n);
    }
}

uint32_t app_player_load(const Engine* e) {
    uint32_t n = 0;
    for (const auto& p : e->players)
        if ((p.used || p.reserved) && !is_system(p.info))
            ++n;
    return n;
}

void finish_request(Engine* e, uint32_t result, uint32_t id) {
    e->request_pending = false;
    e->request_result = result;
    e->request_id = id;
}

void fail_join(Engine* e, uint32_t result) {
    finish_request(e, result, 0);
    e->joined_roster_pending = false;
    for (auto& p : e->players)
        p = EnginePlayer{};
    e->state = EngineState::idle;
}

/// Records that a machine was heard from: its ping count restarts.
///
/// @param[in,out] system The machine's system player.
void heard_from(EnginePlayer* system) {
    if (system->chatter_count != UINT32_MAX)
        ++system->chatter_count;
    system->unanswered_pings = 0;
}

/// Records that the machine at a stream endpoint was heard from.
///
/// @param[in,out] e Engine holding the players.
/// @param endpoint The endpoint the message's sender answers at.
void heard_from_endpoint(Engine* e, const Address& endpoint) {
    for (auto& p : e->players)
        if (p.used && !p.local && is_system(p.info) && address_equal(p.info.stream, endpoint))
            heard_from(&p);
}

/// Records that the machine of a player was heard from.
///
/// @param[in,out] e Engine holding the players.
/// @param id Player id; an unknown or local player is ignored.
void heard_from_player(Engine* e, uint32_t id) {
    EnginePlayer* p = find_player(e, id);
    if (p != nullptr && !is_system(p->info))
        p = find_player(e, p->info.system_id);
    if (p != nullptr && !p->local)
        heard_from(p);
}

/// Leaves the session as the name server's rejection asks: every other machine is forgotten and this machine's
/// players are told the session was lost.
///
/// @param[in,out] e Open engine.
void lose_session(Engine* e) {
    for (auto& p : e->players)
        if (p.used && !p.local)
            p = EnginePlayer{};
    e->state = EngineState::open_client;
    e->name_server_id = 0;
    e->awaiting_name_server = false;
    e->session_lost = true;
    post_session_lost(e);
}

/// Tells the machine a message came from that it is not in the session.
///
/// @param[in,out] e Engine answering.
/// @param m The message.
/// @param source Address the message arrived from.
void send_you_are_dead(Engine* e, const Message& m, const Address& source) {
    const std::size_t n = encode_you_are_dead(own_stream(e), e->scratch, sizeof e->scratch);
    (void)send_stream(e, sender_endpoint(m.reply_to, source), n);
}

/// Runs the ping timer; see engine_poll.
///
/// @param[in,out] e Engine to update.
/// @param now Current time in milliseconds.
void run_ping_timer(Engine* e, uint32_t now) {
    const bool keep_alive = (e->desc.flags & session_flag::keep_alive) != 0;
    if (!is_open(e) || e->session_lost || (!keep_alive && !e->awaiting_name_server)) {
        e->ping_timer_running = false;
        return;
    }
    // The timer starting counts as it elapsing: what was heard before is
    // forgotten.
    if (!e->ping_timer_running) {
        e->ping_timer_running = true;
        e->ping_due_at = now + ping_period_ms;
        for (auto& p : e->players)
            p.chatter_count = 0;
        return;
    }
    if (static_cast<int32_t>(now - e->ping_due_at) < 0)
        return;
    e->ping_due_at = now + ping_period_ms;
    const bool watch_everyone = e->state == EngineState::open_host || e->awaiting_name_server;
    uint32_t silent[max_players]{};
    std::size_t silent_count = 0;
    for (auto& p : e->players) {
        if (!p.used || p.local || !is_system(p.info))
            continue;
        const bool heard = p.chatter_count != 0;
        p.chatter_count = 0;
        if (heard || (!watch_everyone && p.info.id != e->name_server_id))
            continue;
        if (p.unanswered_pings >= max_unanswered_pings) {
            silent[silent_count++] = p.info.id;
            continue;
        }
        ++p.unanswered_pings;
        const std::size_t n = encode_ping(
            own_stream(e), command::ping, e->system_id, now, e->scratch, sizeof e->scratch
        );
        (void)send_stream(e, p.info.stream, n);
    }
    // Dropping a machine can change the name server, and with it who is
    // watched, so the drops follow the scan.
    for (std::size_t i = 0; i < silent_count; ++i)
        if (EnginePlayer* p = find_player(e, silent[i]); p != nullptr)
            remove_player(e, p);
}

// ---- name-server handlers ----

void host_enum_request(Engine* e, const Message& m, const Address& source) {
    if (!guid_is_zero(m.application) &&
        std::memcmp(m.application.bytes, e->desc.application_guid, 16) != 0)
        return;
    if ((m.flags & enum_flag::all) == 0) {
        const bool closed =
            (e->desc.flags & (session_flag::new_players_disabled | session_flag::join_disabled)) !=
            0;
        const bool full =
            e->desc.max_players != 0 && e->desc.current_players >= e->desc.max_players;
        if (closed || full)
            return;
    }
    if (e->password[0] != '\0' && std::strcmp(m.password, e->password) != 0 &&
        (m.flags & enum_flag::password_required) == 0)
        return;
    const std::size_t n = encode_enum_sessions_reply(
        own_stream(e), wire_desc(e), e->session_name, e->scratch, sizeof e->scratch
    );
    (void)send_stream(e, sender_endpoint(m.reply_to, source), n);
}

void host_request_player_id(Engine* e, const Message& m, const Address& source, uint32_t now) {
    const bool system = (m.flags & player_flag::system_player) != 0;
    uint32_t result = result::ok;
    EnginePlayer* slot = free_slot(e);
    const std::size_t id_slot = free_id_slot(e);
    if (slot == nullptr || id_slot == max_players)
        result = result::cant_create_player;
    else if (
        system &&
        (e->desc.flags & (session_flag::new_players_disabled | session_flag::join_disabled)) != 0
    )
        result = result::cant_create_player;
    else if (!system && e->desc.max_players != 0 && app_player_load(e) >= e->desc.max_players)
        result = result::cant_create_player;
    uint32_t id = 0;
    if (result == result::ok) {
        id = slot_id(e, id_slot);
        *slot = EnginePlayer{};
        slot->reserved = true;
        slot->reserved_at = now;
        slot->info.id = id;
        slot->info.flags = m.flags & (player_flag::system_player);
    }
    const std::size_t n =
        encode_request_player_reply(own_stream(e), id, result, e->scratch, sizeof e->scratch);
    (void)send_stream(e, sender_endpoint(m.reply_to, source), n);
}

void host_add_forward_request(Engine* e, const Message& m, const Address& source) {
    EnginePlayer* slot = nullptr;
    for (auto& p : e->players)
        if (p.reserved && p.info.id == m.player.id && is_system(p.info))
            slot = &p;
    const Address reply = sender_endpoint(m.reply_to, source);
    if (slot == nullptr || !is_system(m.player)) {
        const std::size_t n = encode_add_forward_reply(
            own_stream(e), result::invalid_player, e->scratch, sizeof e->scratch
        );
        (void)send_stream(e, reply, n);
        return;
    }
    if (e->password[0] != '\0' && std::strcmp(m.password, e->password) != 0) {
        *slot = EnginePlayer{};
        const std::size_t n = encode_add_forward_reply(
            own_stream(e), result::invalid_password, e->scratch, sizeof e->scratch
        );
        (void)send_stream(e, reply, n);
        return;
    }
    EnginePlayer* p = add_remote_player(e, m.player, source, true);
    if (p == nullptr)
        return;
    p->info.flags |= player_flag::in_group;
    announce_player(e, *p, p->info.id);

    PlayerInfo roster[max_roster_players];
    std::size_t count = 0;
    for (const auto& q : e->players)
        if (q.used && count < max_roster_players)
            roster[count++] = wire_player(q, true);
    const std::size_t n = encode_super_enum_players_reply(
        own_stream(e),
        wire_desc(e),
        e->session_name,
        e->password,
        roster,
        count,
        e->scratch,
        sizeof e->scratch
    );
    (void)send_stream(e, p->info.stream, n);
}

// ---- peer handlers ----

void peer_request_reply(Engine* e, const Message& m) {
    if (!e->request_pending || (e->state == EngineState::joining && !e->request_is_join))
        return;
    if (e->request_is_join) {
        if (m.result != result::ok) {
            fail_join(e, m.result);
            return;
        }
        EnginePlayer* slot = free_slot(e);
        if (slot == nullptr) {
            fail_join(e, result::cant_create_player);
            return;
        }
        e->system_id = m.player_id;
        EnginePlayer* self =
            add_local_player(e, slot, m.player_id, player_flag::system_player, PlayerInfo{});
        e->request_is_join = false;
        e->joined_roster_pending = true;
        // The request ends with the session's first reserved dword, which
        // the enumeration reply gave.
        const std::size_t n = encode_player_message(
            own_stream(e),
            command::add_forward_request,
            0,
            wire_player(*self, false),
            e->password,
            e->desc.reserved[0],
            e->scratch,
            sizeof e->scratch
        );
        (void)send_stream(e, e->name_server, n);
        return;
    }
    if (m.result != result::ok) {
        finish_request(e, m.result, 0);
        return;
    }
    EnginePlayer* slot = free_slot(e);
    if (slot == nullptr) {
        finish_request(e, result::cant_create_player, 0);
        return;
    }
    EnginePlayer* p = add_local_player(e, slot, m.player_id, 0, e->request_player);
    ++e->desc.current_players;
    announce_player(e, *p, 0);
    finish_request(e, result::ok, p->info.id);
}

void peer_roster(Engine* e, const Message& m, const Address& source) {
    if (!e->joined_roster_pending)
        return;
    e->joined_roster_pending = false;
    e->desc = m.desc;
    copy_name(e->session_name, m.session_name);
    copy_name(e->password, m.password);
    e->desc.current_players = 0;
    for (uint32_t i = 0; i < m.roster_count; ++i) {
        const PlayerInfo& in = m.roster[i];
        if (in.id == e->system_id)
            continue;
        EnginePlayer* p = add_remote_player(e, in, source, false);
        if (p != nullptr && (in.flags & player_flag::name_server) != 0) {
            e->name_server_id = in.id;
            if (in.has_addresses)
                e->name_server = p->info.stream;
        }
    }
    e->state = EngineState::open_client;
    finish_request(e, result::ok, e->system_id);
}

void handle_control(Engine* e, const Message& m, const Address& source, uint32_t now) {
    const bool host = e->state == EngineState::open_host;
    if (is_open(e))
        heard_from_endpoint(e, sender_endpoint(m.reply_to, source));
    switch (m.command) {
    case command::enum_sessions:
        if (host)
            host_enum_request(e, m, source);
        return;
    case command::enum_sessions_reply: {
        if (!guid_is_zero(e->enum_application) &&
            std::memcmp(m.desc.application_guid, e->enum_application.bytes, 16) != 0)
            return;
        Guid instance{};
        std::memcpy(instance.bytes, m.desc.instance_guid, 16);
        SessionEntry* entry = nullptr;
        for (uint32_t i = 0; i < e->session_count; ++i)
            if (guid_equal(e->sessions[i].instance, instance))
                entry = &e->sessions[i];
        if (entry == nullptr && e->session_count < max_sessions)
            entry = &e->sessions[e->session_count++];
        if (entry == nullptr)
            return;
        entry->instance = instance;
        entry->desc = m.desc;
        copy_name(entry->name, m.session_name);
        entry->host = sender_endpoint(m.reply_to, source);
        return;
    }
    case command::request_player_id:
        if (host)
            host_request_player_id(e, m, source, now);
        return;
    case command::request_player_reply:
        peer_request_reply(e, m);
        return;
    case command::add_forward_request:
        if (host)
            host_add_forward_request(e, m, source);
        return;
    case command::add_forward_reply:
        if (e->state == EngineState::joining)
            fail_join(e, m.result != result::ok ? m.result : result::generic);
        return;
    case command::super_enum_players_reply:
        if (e->state == EngineState::joining)
            peer_roster(e, m, source);
        return;
    case command::create_player:
    case command::add_forward:
        if (is_open(e) || e->state == EngineState::joining) {
            const bool known = find_player(e, m.player.id) != nullptr;
            EnginePlayer* p = add_remote_player(e, m.player, source, true);
            if (p != nullptr && p->reserved)
                p->reserved = false;
            if (m.command == command::add_forward) {
                const std::size_t n = encode_add_forward_ack(
                    own_stream(e), m.player.id, e->scratch, sizeof e->scratch
                );
                (void)send_stream(e, sender_endpoint(m.reply_to, source), n);
            }
            // A machine just met, not one already listed. This machine's
            // players go straight to it. An application player's
            // announcement does not answer, so the two do not bounce it.
            if (!known && p != nullptr && is_system(p->info))
                send_local_application_players(e, p->info.stream);
        }
        return;
    case command::delete_player:
        if (EnginePlayer* p = find_player(e, m.player_id); p != nullptr && !p->local)
            remove_player(e, p);
        return;
    case command::session_desc_changed:
        if (e->state == EngineState::open_client) {
            e->desc = m.desc;
            copy_name(e->session_name, m.session_name);
            copy_name(e->password, m.password);
            post_session_desc(e);
        }
        return;
    case command::ping: {
        if (!is_open(e))
            return;
        // The reply echoes the ping's player id and tick. A pinging player
        // the session does not hold is told by the name server that it is
        // not in the session, and ignored by any other machine.
        const EnginePlayer* sys = nullptr;
        if (!player_route(e, m.player_id, &sys) || sys->local) {
            if (host)
                send_you_are_dead(e, m, source);
            return;
        }
        const std::size_t n = encode_ping(
            own_stream(e), command::ping_reply, m.player_id, m.tick, e->scratch, sizeof e->scratch
        );
        (void)send_stream(e, sys->info.stream, n);
        return;
    }
    case command::ping_reply:
        // The machine was heard from above; a reply naming a player the
        // session does not hold is answered as a ping would be.
        if (host && find_player(e, m.player_id) == nullptr)
            send_you_are_dead(e, m, source);
        return;
    case command::i_am_name_server: {
        if (!is_open(e))
            return;
        if (host) {
            send_you_are_dead(e, m, source);
            return;
        }
        EnginePlayer* server = find_player(e, m.player_id);
        if (server == nullptr || server->local || !is_system(server->info))
            return;
        for (auto& p : e->players)
            if (p.used && is_system(p.info))
                p.info.flags &= ~player_flag::name_server;
        server->info.flags |= player_flag::name_server;
        if (m.player.has_addresses && m.player.stream.port != 0) {
            server->info.stream = resolve(m.player.stream, source);
            server->info.datagram = resolve(m.player.datagram, source);
        }
        e->name_server_id = server->info.id;
        e->name_server = server->info.stream;
        e->awaiting_name_server = false;
        return;
    }
    case command::you_are_dead:
        if (is_open(e) && !e->session_lost)
            lose_session(e);
        return;
    case command::player_data_changed:
        if (EnginePlayer* p = find_player(e, m.player_id);
            is_open(e) && p != nullptr && !p->local) {
            p->info.data_size = m.player.data_size;
            std::memcpy(p->info.data, m.player.data, m.player.data_size);
            if (!is_system(p->info))
                post_player_data(e, p->info);
        }
        return;
    case command::player_name_changed:
        if (EnginePlayer* p = find_player(e, m.player_id);
            is_open(e) && p != nullptr && !p->local) {
            p->info.has_short_name = m.player.has_short_name;
            p->info.has_long_name = m.player.has_long_name;
            copy_name(p->info.short_name, m.player.short_name);
            copy_name(p->info.long_name, m.player.long_name);
            if (!is_system(p->info))
                post_player_name(e, p->info);
        }
        return;
    default:
        return;
    }
}

void deliver_data(Engine* e, uint32_t from, uint32_t to, const uint8_t* payload, std::size_t size) {
    if (!is_open(e))
        return;
    const EnginePlayer* sender = find_player(e, from);
    if (sender == nullptr || sender->local)
        return;
    heard_from_player(e, from);
    if (to == all_players_id) {
        deliver_local(e, from, from, payload, size, false);
        return;
    }
    const EnginePlayer* target = find_player(e, to);
    if (target != nullptr && target->local)
        (void)queue_push(e, from, to, payload, size);
}

bool known_remote(Engine* e, uint32_t id) {
    const EnginePlayer* p = find_player(e, id);
    return p != nullptr && !p->local;
}

} // namespace

namespace {

/// Reads a string an image points to.
///
/// @param image Message image.
/// @param size Image length in bytes.
/// @param field Offset of the field holding the string's offset.
/// @param[out] s The string, pointing into image; left as it is for an offset of 0.
/// @return False when the string starts outside the image or has no terminator within it.
bool image_string_at(const uint8_t* image, std::size_t size, std::size_t field, const char** s) {
    const uint32_t at = load_u32(image + field);
    if (at == 0)
        return true;
    if (at >= size || std::memchr(image + at, 0, size - at) == nullptr)
        return false;
    *s = reinterpret_cast<const char*>(image + at);
    return true;
}

/// Reads the data block an image points to.
///
/// @param image Message image.
/// @param size Image length in bytes.
/// @param data_field Offset of the field holding the data's offset.
/// @param size_field Offset of the field holding the data's size.
/// @param[out] data The data, pointing into image; null for none.
/// @param[out] data_size The data's size in bytes.
/// @return False when the data lies outside the image, or has a size but no offset.
bool image_data_at(
    const uint8_t* image,
    std::size_t size,
    std::size_t data_field,
    std::size_t size_field,
    const uint8_t** data,
    uint32_t* data_size
) {
    const uint32_t at = load_u32(image + data_field);
    *data_size = load_u32(image + size_field);
    if (at == 0)
        return *data_size == 0;
    if (at > size || size - at < *data_size)
        return false;
    *data = image + at;
    return true;
}

} // namespace

bool read_system_message_type(const uint8_t* image, std::size_t size, uint32_t* type) noexcept {
    if (image == nullptr || type == nullptr || size < sizeof(uint32_t))
        return false;
    *type = load_u32(image);
    return true;
}

bool decode_player_destroyed_image(
    const uint8_t* image, std::size_t size, PlayerDestroyedView* out
) noexcept {
    if (image == nullptr || out == nullptr || size < system_message::destroy_id_end ||
        load_u32(image) != static_cast<uint32_t>(SystemMessageType::player_destroyed))
        return false;
    out->player_type = load_u32(image + 4);
    out->id = load_u32(image + system_message::destroy_id);
    return true;
}

bool decode_create_player_image(
    const uint8_t* image, std::size_t size, CreatePlayerView* out
) noexcept {
    if (image == nullptr || out == nullptr || size < system_message::create_bytes ||
        load_u32(image) != static_cast<uint32_t>(SystemMessageType::player_created))
        return false;
    CreatePlayerView v{};
    v.id = load_u32(image + system_message::create_id);
    v.current_players = load_u32(image + system_message::create_current_players);
    if (!image_data_at(
            image,
            size,
            system_message::create_data,
            system_message::create_data_size,
            &v.data,
            &v.data_size
        ))
        return false;
    if (!image_string_at(
            image, size, system_message::create_name + system_message::name_short, &v.short_name
        ) ||
        !image_string_at(
            image, size, system_message::create_name + system_message::name_long, &v.long_name
        ))
        return false;
    *out = v;
    return true;
}

bool decode_player_data_image(
    const uint8_t* image, std::size_t size, PlayerDataView* out
) noexcept {
    if (image == nullptr || out == nullptr || size < system_message::player_data_bytes ||
        load_u32(image) != static_cast<uint32_t>(SystemMessageType::player_data_changed) ||
        load_u32(image + 4) != system_message::player_type_player)
        return false;
    PlayerDataView v{};
    v.id = load_u32(image + system_message::player_data_id);
    if (!image_data_at(
            image,
            size,
            system_message::player_data_data,
            system_message::player_data_size,
            &v.data,
            &v.data_size
        ))
        return false;
    *out = v;
    return true;
}

bool decode_player_name_image(
    const uint8_t* image, std::size_t size, PlayerNameView* out
) noexcept {
    if (image == nullptr || out == nullptr || size < system_message::player_name_bytes ||
        load_u32(image) != static_cast<uint32_t>(SystemMessageType::player_name_changed) ||
        load_u32(image + 4) != system_message::player_type_player)
        return false;
    PlayerNameView v{};
    v.id = load_u32(image + system_message::player_name_id);
    if (!image_string_at(
            image,
            size,
            system_message::player_name_name + system_message::name_short,
            &v.short_name
        ) ||
        !image_string_at(
            image, size, system_message::player_name_name + system_message::name_long, &v.long_name
        ))
        return false;
    *out = v;
    return true;
}

void engine_init(Engine* e, const EngineConfig& config, const EngineIo& io) noexcept {
    // Engine is trivially copyable and too large for a temporary.
    std::memset(static_cast<void*>(e), 0, sizeof *e);
    e->state = EngineState::idle;
    e->config = config;
    e->io = io;
    e->rng = config.seed;
}

uint32_t engine_host(
    Engine* e, const SessionDesc& desc, const char* name, const char* password, uint32_t now
) noexcept {
    (void)now;
    if (e->state != EngineState::idle)
        return result::already_initialized;
    for (auto& p : e->players)
        p = EnginePlayer{};
    e->desc = desc;
    e->desc.size = static_cast<uint32_t>(session_desc_bytes);
    for (std::size_t i = 0; i < 16; i += 4)
        store_u32(e->desc.instance_guid + i, next_random(e));
    e->id_key = next_random(e);
    e->desc.reserved[0] = e->id_key;
    e->desc.current_players = 0;
    copy_name(e->session_name, name);
    copy_name(e->password, password);
    if (e->password[0] != '\0')
        e->desc.flags |= session_flag::password_required;
    for (auto& u : e->uniqueness)
        u = 1;
    e->system_id = slot_id(e, 0);
    e->name_server_id = e->system_id;
    add_local_player(
        e,
        &e->players[0],
        e->system_id,
        player_flag::system_player | player_flag::name_server,
        PlayerInfo{}
    );
    e->session_lost = false;
    e->awaiting_name_server = false;
    e->ping_timer_running = false;
    e->state = EngineState::open_host;
    return result::ok;
}

uint32_t engine_enum_sessions(
    Engine* e, const Guid& application, const uint8_t target_ip[4], uint32_t now
) noexcept {
    (void)now;
    e->enum_application = application;
    e->session_count = 0;
    Address to{};
    std::memcpy(to.ip, target_ip, 4);
    to.port = e->config.enum_port;
    const std::size_t n = encode_enum_sessions(
        own_stream(e),
        application,
        nullptr,
        enum_flag::available | enum_flag::return_status,
        e->scratch,
        sizeof e->scratch
    );
    if (n == 0 || e->io.send_datagram == nullptr)
        return result::no_connection;
    stamp_sender_address(e, to, n);
    if (!e->io.send_datagram(e->io.context, to, e->scratch, n))
        return result::no_connection;
    return result::ok;
}

uint32_t engine_join(Engine* e, const Guid& instance, const char* password, uint32_t now) noexcept {
    if (e->state != EngineState::idle)
        return result::already_initialized;
    const SessionEntry* entry = nullptr;
    for (uint32_t i = 0; i < e->session_count; ++i)
        if (guid_equal(e->sessions[i].instance, instance))
            entry = &e->sessions[i];
    if (entry == nullptr)
        return result::no_sessions;
    for (auto& p : e->players)
        p = EnginePlayer{};
    e->desc = entry->desc;
    copy_name(e->session_name, entry->name);
    copy_name(e->password, password);
    e->name_server = entry->host;
    e->session_lost = false;
    e->awaiting_name_server = false;
    e->ping_timer_running = false;
    e->state = EngineState::joining;
    e->request_pending = true;
    e->request_is_join = true;
    e->request_started = now;
    e->request_result = result::connecting;
    const std::size_t n = encode_request_player_id(
        own_stream(e), request_flags_system, e->scratch, sizeof e->scratch
    );
    if (!send_stream(e, e->name_server, n)) {
        fail_join(e, result::no_connection);
        return result::no_connection;
    }
    return result::ok;
}

uint32_t engine_join_status(const Engine* e) noexcept {
    if (e->state == EngineState::open_client)
        return result::ok;
    if (e->state == EngineState::joining)
        return result::connecting;
    return e->request_result != result::ok ? e->request_result : result::generic;
}

uint32_t engine_create_player(
    Engine* e,
    const char* short_name,
    const char* long_name,
    const uint8_t* data,
    std::size_t data_size,
    uint32_t now,
    uint32_t* id
) noexcept {
    if (!is_open(e))
        return result::no_connection;
    if (data_size > max_player_data_bytes || (data == nullptr && data_size != 0))
        return result::invalid_params;
    if (e->request_pending)
        return result::connecting;
    PlayerInfo shape{};
    shape.has_short_name = short_name != nullptr;
    shape.has_long_name = long_name != nullptr;
    copy_name(shape.short_name, short_name);
    copy_name(shape.long_name, long_name);
    shape.data_size = static_cast<uint16_t>(data_size);
    if (data_size != 0)
        std::memcpy(shape.data, data, data_size);

    if (e->state == EngineState::open_host) {
        EnginePlayer* slot = free_slot(e);
        const std::size_t id_slot = free_id_slot(e);
        if (slot == nullptr || id_slot == max_players ||
            (e->desc.max_players != 0 && app_player_load(e) >= e->desc.max_players))
            return result::cant_create_player;
        const uint32_t new_id = slot_id(e, id_slot);
        EnginePlayer* p = add_local_player(e, slot, new_id, 0, shape);
        ++e->desc.current_players;
        announce_player(e, *p, 0);
        finish_request(e, result::ok, new_id);
        if (id != nullptr)
            *id = new_id;
        return result::ok;
    }
    e->request_player = shape;
    e->request_pending = true;
    e->request_is_join = false;
    e->request_started = now;
    e->request_result = result::connecting;
    const std::size_t n = encode_request_player_id(
        own_stream(e), request_flags_player, e->scratch, sizeof e->scratch
    );
    if (!send_stream(e, e->name_server, n)) {
        finish_request(e, result::no_connection, 0);
        return result::no_connection;
    }
    return result::connecting;
}

uint32_t engine_create_player_status(const Engine* e, uint32_t* id) noexcept {
    if (e->request_pending)
        return result::connecting;
    if (id != nullptr)
        *id = e->request_id;
    return e->request_result;
}

uint32_t engine_destroy_player(Engine* e, uint32_t id) noexcept {
    EnginePlayer* p = find_player(e, id);
    if (!is_open(e) || p == nullptr || !p->local || is_system(p->info))
        return result::invalid_player;
    const std::size_t n = encode_delete_player(own_stream(e), 0, id, e->scratch, sizeof e->scratch);
    (void)send_to_remote_systems(e, n, 0);
    *p = EnginePlayer{};
    if (e->desc.current_players != 0)
        --e->desc.current_players;
    return result::ok;
}

uint32_t engine_set_player_name(
    Engine* e, uint32_t id, const char* short_name, const char* long_name
) noexcept {
    if (!is_open(e))
        return result::no_connection;
    EnginePlayer* p = find_player(e, id);
    if (p == nullptr || !p->local || is_system(p->info))
        return result::invalid_player;
    p->info.has_short_name = short_name != nullptr;
    p->info.has_long_name = long_name != nullptr;
    copy_name(p->info.short_name, short_name);
    copy_name(p->info.long_name, long_name);
    const std::size_t n = encode_player_name_changed(
        own_stream(e),
        0,
        id,
        p->info.has_short_name ? p->info.short_name : nullptr,
        p->info.has_long_name ? p->info.long_name : nullptr,
        e->scratch,
        sizeof e->scratch
    );
    if (n == 0)
        return result::generic;
    (void)send_to_remote_systems(e, n, 0);
    return result::ok;
}

uint32_t
engine_set_player_data(Engine* e, uint32_t id, const uint8_t* data, std::size_t size) noexcept {
    if (!is_open(e))
        return result::no_connection;
    if (size > max_player_data_bytes || (data == nullptr && size != 0))
        return result::invalid_params;
    EnginePlayer* p = find_player(e, id);
    if (p == nullptr || !p->local || is_system(p->info))
        return result::invalid_player;
    p->info.data_size = static_cast<uint16_t>(size);
    if (size != 0)
        std::memcpy(p->info.data, data, size);
    if ((e->desc.flags & session_flag::no_data_messages) != 0)
        return result::ok;
    const std::size_t n = encode_player_data_changed(
        own_stream(e), 0, id, p->info.data, size, e->scratch, sizeof e->scratch
    );
    if (n == 0)
        return result::generic;
    (void)send_to_remote_systems(e, n, 0);
    return result::ok;
}

uint32_t engine_set_session_desc(
    Engine* e, const SessionDesc& desc, const char* name, const char* password
) noexcept {
    if (e->state != EngineState::open_host)
        return result::access_denied;
    SessionDesc d = desc;
    d.size = static_cast<uint32_t>(session_desc_bytes);
    std::memcpy(d.instance_guid, e->desc.instance_guid, 16);
    std::memcpy(d.application_guid, e->desc.application_guid, 16);
    d.reserved[0] = e->id_key;
    d.current_players = e->desc.current_players;
    e->desc = d;
    copy_name(e->session_name, name);
    copy_name(e->password, password);
    if (e->password[0] != '\0')
        e->desc.flags |= session_flag::password_required;
    // The description's string fields are never 0 in this message: they
    // hold the strings' offsets, which receivers read the strings by.
    SessionDesc wire = wire_desc(e);
    wire.name_pointer = session_desc_changed_name_offset;
    wire.password_pointer =
        e->publish_without_password ? 0 : session_desc_changed_password_offset(e->session_name);
    const std::size_t n = encode_session_desc_changed(
        own_stream(e), 0, wire, e->session_name, e->password, e->scratch, sizeof e->scratch
    );
    if (n == 0)
        return result::generic;
    (void)send_to_remote_systems(e, n, 0);
    return result::ok;
}

uint32_t engine_send(
    Engine* e,
    uint32_t from_id,
    uint32_t to_id,
    uint32_t flags,
    const uint8_t* data,
    std::size_t size
) noexcept {
    if (!is_open(e))
        return result::no_connection;
    const EnginePlayer* from = find_player(e, from_id);
    if (from == nullptr || !from->local)
        return result::invalid_player;
    if (data == nullptr && size != 0)
        return result::invalid_params;
    const bool guaranteed = (flags & send_guaranteed) != 0;
    if (!guaranteed && size + data_ids_bytes > max_datagram_bytes)
        return result::send_too_big;
    const std::size_t n =
        guaranteed
            ? encode_stream_data(
                  own_stream(e), from_id, to_id, data, size, e->scratch, sizeof e->scratch
              )
            : encode_datagram_data(from_id, to_id, data, size, e->scratch, sizeof e->scratch);
    if (n == 0)
        return result::send_too_big;

    const auto send_to = [&](const EnginePlayer& system) {
        if (guaranteed)
            return send_stream(e, system.info.stream, n);
        return e->io.send_datagram != nullptr &&
               e->io.send_datagram(e->io.context, system.info.datagram, e->scratch, n);
    };
    if (to_id == all_players_id) {
        bool ok = true;
        for (const auto& p : e->players)
            if (p.used && !p.local && is_system(p.info))
                ok = send_to(p) && ok;
        deliver_local(e, from_id, from_id, data, size, false);
        return ok ? result::ok : result::generic;
    }
    const EnginePlayer* target = find_player(e, to_id);
    if (target == nullptr)
        return result::invalid_player;
    if (target->local)
        return queue_push(e, from_id, to_id, data, size) ? result::ok : result::out_of_memory;
    const EnginePlayer* system = nullptr;
    if (!player_route(e, to_id, &system))
        return result::invalid_player;
    return send_to(*system) ? result::ok : result::generic;
}

uint32_t engine_receive(
    Engine* e, uint32_t* from_id, uint32_t* to_id, uint8_t* buffer, uint32_t* size
) noexcept {
    if (size == nullptr)
        return result::invalid_params;
    if (e->queue_count == 0)
        return result::no_messages;
    const QueuedMessage& q = e->queue[e->queue_head];
    if (*size < q.size || (buffer == nullptr && q.size != 0)) {
        *size = q.size;
        return result::buffer_too_small;
    }
    if (q.size != 0)
        std::memcpy(buffer, e->queue_bytes + q.offset, q.size);
    *size = q.size;
    if (from_id != nullptr)
        *from_id = q.from_id;
    if (to_id != nullptr)
        *to_id = q.to_id;
    e->bytes_start = q.offset + q.size;
    e->queue_head = (e->queue_head + 1) % receive_queue_entries;
    if (--e->queue_count == 0)
        e->bytes_start = e->bytes_end = 0;
    return result::ok;
}

void engine_close(Engine* e) noexcept {
    if (is_open(e)) {
        for (int pass = 0; pass < 2; ++pass)
            for (const auto& p : e->players) {
                if (!p.used || !p.local || is_system(p.info) != (pass == 1))
                    continue;
                const std::size_t n = encode_delete_player(
                    own_stream(e), 0, p.info.id, e->scratch, sizeof e->scratch
                );
                (void)send_to_remote_systems(e, n, 0);
            }
    }
    const EngineConfig config = e->config;
    const EngineIo io = e->io;
    const uint32_t rng = e->rng;
    for (auto& p : e->players)
        p = EnginePlayer{};
    e->state = EngineState::idle;
    e->request_pending = false;
    e->joined_roster_pending = false;
    e->queue_head = e->queue_count = e->bytes_start = e->bytes_end = 0;
    e->session_count = 0;
    e->system_id = e->name_server_id = 0;
    e->awaiting_name_server = false;
    e->ping_timer_running = false;
    e->config = config;
    e->io = io;
    e->rng = rng;
}

void engine_on_stream(
    Engine* e, const Address& source, const uint8_t* bytes, std::size_t size, uint32_t now
) noexcept {
    const ParseError r = decode_message(bytes, size, &e->message);
    if (r == ParseError::ok) {
        handle_control(e, e->message, source, now);
        return;
    }
    if (r == ParseError::not_control) {
        DataMessage d{};
        if (decode_stream_data(bytes, size, &d) == ParseError::ok)
            deliver_data(e, d.from_id, d.to_id, d.payload, d.payload_size);
    }
}

void engine_on_datagram(
    Engine* e, const Address& source, const uint8_t* bytes, std::size_t size, uint32_t now
) noexcept {
    if (bytes == nullptr || size < data_ids_bytes)
        return;
    if ((load_u32(bytes) >> 20) == dplay_envelope_token) {
        if (decode_message(bytes, size, &e->message) == ParseError::ok)
            handle_control(e, e->message, source, now);
        return;
    }
    std::size_t at = 0;
    if (!known_remote(e, load_u32(bytes))) {
        if (size < data_ids_bytes + 2 || !known_remote(e, load_u32(bytes + 2)))
            return;
        at = 2;
    }
    deliver_data(
        e,
        load_u32(bytes + at),
        load_u32(bytes + at + 4),
        bytes + at + data_ids_bytes,
        size - at - data_ids_bytes
    );
}

void engine_poll(Engine* e, uint32_t now) noexcept {
    if ((e->request_pending || e->joined_roster_pending) &&
        now - e->request_started > e->config.request_timeout_ms) {
        if (e->state == EngineState::joining)
            fail_join(e, result::timeout);
        else
            finish_request(e, result::timeout, 0);
    }
    if (e->state == EngineState::open_host)
        for (auto& p : e->players)
            if (p.reserved && !p.used && now - p.reserved_at > reservation_timeout_ms)
                p = EnginePlayer{};
    run_ping_timer(e, now);
}

const EnginePlayer* engine_find_player(const Engine* e, uint32_t id) noexcept {
    return find_player(const_cast<Engine*>(e), id);
}

uint32_t engine_player_count(const Engine* e) noexcept {
    uint32_t n = 0;
    for (const auto& p : e->players)
        if (p.used && !is_system(p.info))
            ++n;
    return n;
}

} // namespace oa::netgame::dplay

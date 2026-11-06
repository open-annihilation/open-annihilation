// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The lobby links and the link thread.

#include "link.h"

#include "oa/rover/cipher.h"
#include "protocol.h"
#include "sockets.h"

#include <string.h>

namespace {

// Stored for a failure that reports no Winsock code.
constexpr int32_t kUnspecifiedLinkError = 0x16;
// Stored when the peer closed an established link.
constexpr int32_t kPeerClosedError = -1;
// A yard master connect that neither finishes nor fails by then is closed, so LOGIN is not left
// waiting on a host that never answers.
constexpr uint32_t kYardMasterConnectTimeoutMs = 15000;
// Winsock's connection timed out.
constexpr int32_t kWsaTimedOut = 10060;

// Events a link registers with the select thread.
constexpr uint32_t kLinkNetEvents =
    rover_net_event_bit(RoverNetEvent::Read) | rover_net_event_bit(RoverNetEvent::Write) |
    rover_net_event_bit(RoverNetEvent::Connect) | rover_net_event_bit(RoverNetEvent::Close);

// What the link thread waits on. Each link has a connect request, its socket
// events and, once connected, its outbound queue.
enum class RoverWaitId : int32_t {
    ConnectBoneyard1 = 0,
    Boneyard1Socket = 1,
    ConnectBoneyard2 = 2,
    Boneyard2Socket = 3,
    ConnectYardMaster = 4,
    YardMasterSocket = 5,
    Boneyard1Queue = 6,
    Boneyard2Queue = 7,
    YardMasterQueue = 8,
    Stop = 9,
};
constexpr int32_t kWaitLimit = 10;

// The events the link thread waits on, in the order they were added; the
// lowest signalled one is served first.
struct RoverWaitSet {
    int32_t count{};
    RoverEvent* events[kWaitLimit]{};
    RoverWaitId ids[kWaitLimit]{};
};

/// Clears the wait set's count, events and ids.
///
/// @param[out] waits wait set to clear
void init_wait_set(RoverWaitSet& waits) {
    waits.count = 0;
    memset(waits.events, 0, sizeof waits.events);
    memset(waits.ids, 0, sizeof waits.ids);
}

// The record being gathered from received text: one for every link.
RoverRecord* g_receiving = nullptr;

RoverWaitId connect_wait(int32_t link) {
    return static_cast<RoverWaitId>(link * 2);
}

RoverWaitId socket_wait(int32_t link) {
    return static_cast<RoverWaitId>(link * 2 + 1);
}

RoverWaitId queue_wait(int32_t link) {
    return static_cast<RoverWaitId>(static_cast<int32_t>(RoverWaitId::Boneyard1Queue) + link);
}

void add_wait(RoverWaitSet& waits, RoverWaitId id, RoverEvent* event) {
    for (int32_t i = 0; i < waits.count; ++i) {
        if (waits.ids[i] == id)
            return;
    }
    waits.ids[waits.count] = id;
    waits.events[waits.count] = event;
    ++waits.count;
}

/// Drops an id from the wait set, keeping the order of the rest.
///
/// @param[in,out] waits wait set to change
/// @param id wait to drop; an absent id changes nothing
void remove_wait(RoverWaitSet& waits, RoverWaitId id) {
    int32_t at = 0;
    while (at < waits.count && waits.ids[at] != id)
        ++at;
    if (at == waits.count)
        return;
    --waits.count;
    for (; at < waits.count; ++at) {
        waits.ids[at] = waits.ids[at + 1];
        waits.events[at] = waits.events[at + 1];
    }
}

bool is_yard_master(const RoverState& state, const RoverConnection& link) {
    return &link == &state.links[static_cast<int32_t>(RoverLinkId::YardMaster)];
}

// A record holding one "Rover" entry with the given command, as every notice
// the link raises starts; null when out of memory.
RoverRecord* make_notice(const char* command, RoverEntry** out_entry) {
    RoverRecord* record = rover_record_create();
    if (!record)
        return nullptr;
    RoverEntry* entry = rover_record_add_entry(*record, false, kRoverClientService);
    rover_entry_set_string(*entry, kRoverKeyCommand, command);
    *out_entry = entry;
    return record;
}

/// Marks a link failed and tells the pump it closed.
///
/// The link keeps its first error; a zero error is stored as 0x16.
///
/// @param[in,out] state session block whose inbound queue receives notices
/// @param[in,out] link link that closed
/// @param error Winsock-numbered error, or 0 when none was reported
void report_closed(RoverState& state, RoverConnection& link, int32_t error) {
    const RoverPlatform* platform = rover_platform();
    platform->lock_enter(link.lock);
    link.state = RoverLinkState::Failed;
    if (link.last_error == 0)
        link.last_error = error ? error : kUnspecifiedLinkError;
    RoverEntry* entry = nullptr;
    RoverRecord* notice = make_notice(
        is_yard_master(state, link) ? kRoverCommandYardMasterClosed : kRoverCommandBoneyardClosed,
        &entry
    );
    if (notice)
        rover_queue_push(state.inbound, notice);
    platform->lock_leave(link.lock);
}

/// Records a socket error: an established link keeps its first error and has the select thread
/// raise its events; a connecting one is reported closed.
///
/// An established link whose peer closed stores -1 and the select table is left alone.
///
/// @param[in,out] state session block whose inbound queue receives notices
/// @param[in,out] link link the error happened on
/// @param error Winsock-numbered error, or 0 when none was reported
/// @param peer_closed true when the peer ended the stream rather than the socket failing
void report_socket_error(
    RoverState& state, RoverConnection& link, int32_t error, bool peer_closed
) {
    const RoverPlatform* platform = rover_platform();
    platform->lock_enter(link.lock);
    if (link.state == RoverLinkState::Connected) {
        if (link.last_error == 0) {
            if (peer_closed) {
                link.last_error = kPeerClosedError;
            } else {
                link.last_error = error ? error : kUnspecifiedLinkError;
                rover_link_set_socket_failed_dispatch(link.socket);
            }
        }
        link.state = RoverLinkState::Failed;
    } else if (link.state == RoverLinkState::Connecting) {
        report_closed(state, link, error);
    }
    platform->lock_leave(link.lock);
}

/// Creates the link's socket and registers it with the select thread, clearing the link's error and
/// send state.
///
/// A failure is reported to the pump as a close.
///
/// @param[in,out] state session block whose inbound queue receives notices
/// @param[in,out] link link to connect
/// @return true when the socket exists and is registered
bool begin_connection(RoverState& state, RoverConnection& link) {
    const RoverPlatform* platform = rover_platform();
    platform->lock_enter(link.lock);
    link.last_error = 0;
    link.send_left = 0;
    link.send_cursor = nullptr;
    link.state = RoverLinkState::Connecting;
    link.connecting = true;
    link.connect_started_ms = platform->clock_ms();
    bool started = false;
    link.socket = platform->tcp_create();
    if (link.socket == OA_ROVER_SOCKET_INVALID) {
        report_closed(state, link, platform->net_last_error());
    } else if (rover_sockets_register(link.socket, link.socket_event, kLinkNetEvents) == -1) {
        int32_t error = platform->net_last_error();
        platform->tcp_close(link.socket);
        link.socket = OA_ROVER_SOCKET_INVALID;
        report_closed(state, link, error);
    } else {
        started = true;
    }
    platform->lock_leave(link.lock);
    return started;
}

/// Starts connecting to the link's address and restarts both cipher counters at 1.
///
/// A failure is reported to the pump as a close.
///
/// @param[in,out] state session block whose inbound queue receives notices
/// @param[in,out] link link whose socket connects
/// @return true while the connect completed or is in progress
/// @quirk A text that is no numeric address connects to 255.255.255.255, as inet_addr's failure
///     value does.
bool open_socket(RoverState& state, RoverConnection& link) {
    const RoverPlatform* platform = rover_platform();
    platform->lock_enter(link.lock);
    uint32_t address = 0xffffffffu;
    platform->ipv4_parse(link.address, &address);
    bool opened = false;
    if (!platform->tcp_connect_start(link.socket, address, link.port)) {
        report_closed(state, link, platform->net_last_error());
    } else {
        link.send_counter = OA_ROVER_CIPHER_FIRST_COUNTER;
        link.receive_counter = OA_ROVER_CIPHER_FIRST_COUNTER;
        opened = true;
    }
    platform->lock_leave(link.lock);
    return opened;
}

/// Unregisters and closes the link's socket and marks the link closed.
///
/// @param[in,out] link link to close
void close_socket(RoverConnection& link) {
    const RoverPlatform* platform = rover_platform();
    platform->lock_enter(link.lock);
    if (link.socket != OA_ROVER_SOCKET_INVALID) {
        rover_sockets_register(link.socket, link.socket_event, 0);
        platform->tcp_close(link.socket);
        link.socket = OA_ROVER_SOCKET_INVALID;
    }
    link.send_waiting = false;
    link.state = RoverLinkState::Closed;
    platform->lock_leave(link.lock);
}

/// Marks the link connected once its connect finished.
///
/// @param[in,out] link link that connected
void mark_connected(RoverConnection& link) {
    const RoverPlatform* platform = rover_platform();
    platform->lock_enter(link.lock);
    link.connecting = false;
    link.state = RoverLinkState::Connected;
    platform->lock_leave(link.lock);
}

/// Notes whether a part-sent record waits for room on the socket.
///
/// @param[in,out] link link to mark
/// @param waiting true while the rest of a record waits
void set_send_waiting(RoverConnection& link, bool waiting) {
    const RoverPlatform* platform = rover_platform();
    platform->lock_enter(link.lock);
    link.send_waiting = waiting;
    platform->lock_leave(link.lock);
}

/// Formats a record into the send buffer, enciphers it and sends until done, the socket is full or
/// the link breaks.
///
/// A record that cannot be formatted is dropped. A broken link is reported and its socket closed.
///
/// @param[in,out] state session block whose inbound queue receives notices
/// @param[in,out] link link to send on
/// @param record record to send, destroyed here; null continues the record waiting in the buffer
/// @return Sent when everything went out, WouldBlock when the rest waits for room, Failed otherwise
/// @quirk A continued send enciphers the unsent rest once more, as 3.1c does.
RoverSendResult send_record(RoverState& state, RoverConnection& link, RoverRecord* record) {
    const RoverPlatform* platform = rover_platform();
    platform->lock_enter(link.lock);
    if (record) {
        int32_t length = 0;
        RoverRecordStatus status =
            rover_record_format(*record, link.send_buffer, kRoverRecordTextLimit, &length);
        rover_record_destroy(&record);
        if (status != RoverRecordStatus::Ok) {
            platform->lock_leave(link.lock);
            return RoverSendResult::Failed;
        }
        link.send_left = length;
        link.send_cursor = link.send_buffer;
        link.send_buffer[length] = '\0';
    }
    rover_link_encrypt_buffer(link, link.send_cursor, link.send_left);
    rover_link_begin_send_shim(link.socket);
    int32_t sent = 0;
    for (;;) {
        sent = platform->tcp_send(link.socket, link.send_cursor, link.send_left);
        if (sent < 0)
            break;
        rover_link_end_send_shim(link.socket);
        link.send_left -= sent;
        link.send_cursor += sent;
        if (link.send_left < 1) {
            platform->lock_leave(link.lock);
            return RoverSendResult::Sent;
        }
        rover_link_begin_send_shim(link.socket);
    }
    if (sent == OA_ROVER_IO_WOULD_BLOCK) {
        platform->lock_leave(link.lock);
        return RoverSendResult::WouldBlock;
    }
    int32_t error = platform->net_last_error();
    rover_link_end_send_shim(link.socket);
    report_socket_error(state, link, error, false);
    close_socket(link);
    platform->lock_leave(link.lock);
    return RoverSendResult::Failed;
}

/// Reads until the socket is empty, deciphering and splitting the text into records for the pump.
///
/// A record split across reads is kept and completed by the next one.
///
/// @param[in,out] state session block whose inbound queue receives the records
/// @param[in,out] link link to read from
/// @param[out] peer_closed set to true when the peer ended the stream, false when the socket ran
///     dry
/// @return false on a socket error or a record that does not parse, true otherwise
bool read_records(RoverState& state, RoverConnection& link, bool* peer_closed) {
    const RoverPlatform* platform = rover_platform();
    platform->lock_enter(link.lock);
    for (;;) {
        int32_t received =
            platform->tcp_recv(link.socket, link.receive_buffer, kRoverRecordTextLimit);
        if (received == OA_ROVER_IO_WOULD_BLOCK) {
            rover_sockets_clear_read(link.socket);
            *peer_closed = false;
            platform->lock_leave(link.lock);
            return true;
        }
        if (received < 0) {
            report_socket_error(state, link, platform->net_last_error(), false);
            platform->lock_leave(link.lock);
            return false;
        }
        if (received == 0) {
            *peer_closed = true;
            platform->lock_leave(link.lock);
            return true;
        }
        rover_link_decrypt_buffer(link, link.receive_buffer, received);
        const char* cursor = link.receive_buffer;
        while (received > 0) {
            int32_t consumed = 0;
            RoverRecord* record = nullptr;
            RoverRecordStatus status =
                rover_record_receive(&g_receiving, cursor, received, &consumed, &record);
            if (status == RoverRecordStatus::Incomplete) {
                *peer_closed = false;
                platform->lock_leave(link.lock);
                return true;
            }
            if (status != RoverRecordStatus::Ok) {
                rover_free_scratch_record(&g_receiving);
                g_receiving = nullptr;
                platform->lock_leave(link.lock);
                return false;
            }
            rover_queue_push(state.inbound, record);
            g_receiving = nullptr;
            cursor += consumed;
            received -= consumed;
        }
    }
}

// A connect request: open the link and tell the pump the connect started.
void serve_connect_request(RoverState& state, RoverWaitSet& waits, int32_t link_index) {
    const RoverPlatform* platform = rover_platform();
    RoverConnection& link = state.links[link_index];
    bool yard_master = link_index == static_cast<int32_t>(RoverLinkId::YardMaster);
    RoverEntry* entry = nullptr;
    RoverRecord* notice = make_notice(
        yard_master ? kRoverCommandConnectToYardMaster : kRoverCommandConnectToBoneyard, &entry
    );
    if (notice) {
        rover_entry_set_string(*entry, kRoverKeyState, kRoverLinkStateOpen);
        if (!yard_master)
            rover_entry_set_int(*entry, kRoverKeyYardNumber, link_index + 1);
    }
    platform->event_reset(state.connect_event[link_index]);
    int32_t result = kRoverLinkResultFailed;
    if (begin_connection(state, link)) {
        add_wait(waits, socket_wait(link_index), link.socket_event);
        if (open_socket(state, link)) {
            result = kRoverLinkResultOk;
        } else {
            remove_wait(waits, socket_wait(link_index));
            close_socket(link);
        }
    }
    if (notice) {
        rover_entry_set_int(*entry, kRoverKeyResult, result);
        rover_queue_push(state.inbound, notice);
    }
}

// A notice naming the link that connected or closed, with the yard number of a boneyard.
RoverRecord* make_link_notice(
    int32_t link_index,
    const char* yard_master_command,
    const char* boneyard_command,
    RoverEntry** out_entry
) {
    bool yard_master = link_index == static_cast<int32_t>(RoverLinkId::YardMaster);
    RoverRecord* notice =
        make_notice(yard_master ? yard_master_command : boneyard_command, out_entry);
    if (notice && !yard_master)
        rover_entry_set_int(*(*out_entry), kRoverKeyYardNumber, link_index + 1);
    return notice;
}

// Socket events of one link: connect finished, data, close, room to send.
void serve_socket_events(RoverState& state, RoverWaitSet& waits, int32_t link_index) {
    const RoverPlatform* platform = rover_platform();
    RoverConnection& link = state.links[link_index];
    RoverNetEvents events{};
    if (rover_sockets_collect(link.socket, &events) == -1) {
        report_socket_error(state, link, platform->net_last_error(), false);
        return;
    }
    if (events.signalled & rover_net_event_bit(RoverNetEvent::Connect)) {
        RoverEntry* entry = nullptr;
        RoverRecord* notice = make_link_notice(
            link_index, kRoverCommandConnectToYardMaster, kRoverCommandConnectToBoneyard, &entry
        );
        if (notice)
            rover_entry_set_string(*entry, kRoverKeyState, kRoverLinkStateConnect);
        int32_t error = events.errors[static_cast<int32_t>(RoverNetEvent::Connect)];
        int32_t result = kRoverLinkResultOk;
        if (error == 0) {
            mark_connected(link);
            add_wait(waits, queue_wait(link_index), state.outbound[link_index].ready);
        } else {
            result = kRoverLinkResultFailed;
            remove_wait(waits, socket_wait(link_index));
            report_closed(state, link, error);
            close_socket(link);
        }
        if (notice) {
            rover_entry_set_int(*entry, kRoverKeyResult, result);
            rover_queue_push(state.inbound, notice);
        }
    }
    if (events.signalled & rover_net_event_bit(RoverNetEvent::Read)) {
        bool peer_closed = false;
        if (!read_records(state, link, &peer_closed)) {
            report_socket_error(state, link, platform->net_last_error(), false);
        } else if (peer_closed) {
            rover_link_remove_socket_dispatch(link.socket);
            events.signalled |= rover_net_event_bit(RoverNetEvent::Close);
        }
    }
    if (events.signalled & rover_net_event_bit(RoverNetEvent::Close)) {
        RoverEntry* entry = nullptr;
        RoverRecord* notice = make_link_notice(
            link_index, kRoverCommandYardMasterClosed, kRoverCommandBoneyardClosed, &entry
        );
        remove_wait(waits, socket_wait(link_index));
        remove_wait(waits, queue_wait(link_index));
        report_socket_error(state, link, 0, true);
        close_socket(link);
        if (notice)
            rover_queue_push(state.inbound, notice);
    }
    if ((events.signalled & rover_net_event_bit(RoverNetEvent::Write)) && link.send_waiting) {
        RoverSendResult sent = send_record(state, link, nullptr);
        if (sent == RoverSendResult::Failed) {
            rover_link_stop(link);
        } else if (sent != RoverSendResult::WouldBlock) {
            add_wait(waits, queue_wait(link_index), state.outbound[link_index].ready);
            set_send_waiting(link, false);
        }
    }
}

// One queued record for a link: send it, or park the queue while the socket is full.
void serve_queue(RoverState& state, RoverWaitSet& waits, int32_t link_index) {
    RoverConnection& link = state.links[link_index];
    RoverRecord* record = rover_queue_pop(state.outbound[link_index]);
    if (!record)
        return;
    if (send_record(state, link, record) == RoverSendResult::WouldBlock) {
        remove_wait(waits, queue_wait(link_index));
        set_send_waiting(link, true);
    }
}

} // namespace

bool rover_link_init(RoverConnection& link) {
    link = RoverConnection{};
    link.lock = rover_platform()->lock_create();
    return link.lock != nullptr;
}

bool rover_link_create_event(RoverConnection& link) {
    const RoverPlatform* platform = rover_platform();
    platform->lock_enter(link.lock);
    link.socket_event = rover_link_new_socket_event();
    platform->lock_leave(link.lock);
    return link.socket_event != nullptr;
}

void rover_link_teardown(RoverConnection& link) {
    const RoverPlatform* platform = rover_platform();
    if (!link.lock)
        return;
    close_socket(link);
    platform->lock_destroy(link.lock);
    link.lock = nullptr;
    if (link.socket_event) {
        platform->event_destroy(link.socket_event);
        link.socket_event = nullptr;
    }
}

void rover_link_fail_pending(RoverState& state, RoverConnection& link, int32_t error) {
    const RoverPlatform* platform = rover_platform();
    platform->lock_enter(link.lock);
    link.connecting = true;
    platform->lock_leave(link.lock);
    report_closed(state, link, error);
}

void rover_link_stop(RoverConnection& link) {
    const RoverPlatform* platform = rover_platform();
    platform->lock_enter(link.lock);
    if (link.state == RoverLinkState::Connected || link.state == RoverLinkState::Connecting)
        platform->tcp_shutdown_send(link.socket);
    platform->lock_leave(link.lock);
}

void rover_link_encrypt_buffer(RoverConnection& link, char* bytes, int32_t length) {
    rover_cipher_apply(&link.send_counter, reinterpret_cast<uint8_t*>(bytes), length);
}

void rover_link_decrypt_buffer(RoverConnection& link, char* bytes, int32_t length) {
    rover_cipher_apply(&link.receive_counter, reinterpret_cast<uint8_t*>(bytes), length);
}

RoverEvent* rover_link_new_socket_event() {
    return rover_platform()->event_create(true, false);
}

void rover_link_remove_socket_dispatch(RoverSocket socket) {
    rover_sockets_remove(socket);
}

void rover_link_set_socket_failed_dispatch(RoverSocket socket) {
    rover_sockets_fail(socket);
}

void rover_link_end_send_shim(RoverSocket socket) {
    rover_sockets_end_send(socket);
}

void rover_link_begin_send_shim(RoverSocket socket) {
    rover_sockets_begin_send(socket);
}

// Closes a yard master connect that has waited out its timeout.
void fail_yard_master_if_timed_out(RoverState& state, RoverWaitSet& waits) {
    const RoverPlatform* platform = rover_platform();
    const int32_t index = static_cast<int32_t>(RoverLinkId::YardMaster);
    RoverConnection& link = state.links[index];
    if (link.state != RoverLinkState::Connecting)
        return;
    if (platform->clock_ms() - link.connect_started_ms < kYardMasterConnectTimeoutMs)
        return;
    remove_wait(waits, socket_wait(index));
    report_closed(state, link, kWsaTimedOut);
    close_socket(link);
}

// How long the link thread waits: the yard master's remaining connect time, or forever.
uint32_t link_thread_wait_ms(const RoverState& state) {
    const RoverPlatform* platform = rover_platform();
    const RoverConnection& link = state.links[static_cast<int32_t>(RoverLinkId::YardMaster)];
    if (link.state != RoverLinkState::Connecting)
        return OA_ROVER_WAIT_FOREVER;
    const uint32_t elapsed = platform->clock_ms() - link.connect_started_ms;
    if (elapsed >= kYardMasterConnectTimeoutMs)
        return 0;
    return kYardMasterConnectTimeoutMs - elapsed;
}

void rover_run_link_thread(void* /*context*/) {
    const RoverPlatform* platform = rover_platform();
    RoverState& state = *g_rover_state;
    RoverWaitSet waits;
    init_wait_set(waits);
    add_wait(waits, RoverWaitId::Stop, state.stop_event);
    for (int32_t link = 0; link < kRoverLinkCount; ++link)
        add_wait(waits, connect_wait(link), state.connect_event[link]);
    for (;;) {
        fail_yard_master_if_timed_out(state, waits);
        int32_t index =
            platform->event_wait_any(waits.events, waits.count, link_thread_wait_ms(state));
        if (index == OA_ROVER_WAIT_TIMEOUT) {
            fail_yard_master_if_timed_out(state, waits);
            continue;
        }
        if (index < 0)
            continue;
        RoverWaitId id = waits.ids[index];
        switch (id) {
        case RoverWaitId::ConnectBoneyard1:
        case RoverWaitId::ConnectBoneyard2:
        case RoverWaitId::ConnectYardMaster:
            serve_connect_request(state, waits, static_cast<int32_t>(id) / 2);
            break;
        case RoverWaitId::Boneyard1Socket:
        case RoverWaitId::Boneyard2Socket:
        case RoverWaitId::YardMasterSocket:
            serve_socket_events(state, waits, static_cast<int32_t>(id) / 2);
            break;
        case RoverWaitId::Boneyard1Queue:
        case RoverWaitId::Boneyard2Queue:
        case RoverWaitId::YardMasterQueue:
            serve_queue(
                state,
                waits,
                static_cast<int32_t>(id) - static_cast<int32_t>(RoverWaitId::Boneyard1Queue)
            );
            break;
        case RoverWaitId::Stop:
            for (RoverConnection& link : state.links) {
                if (link.socket != OA_ROVER_SOCKET_INVALID)
                    close_socket(link);
            }
            --state.worker_count;
            platform->event_reset(state.stop_event);
            return;
        }
    }
}

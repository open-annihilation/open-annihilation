// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Slots 1-8 and 43: attaching, the worker threads, the event pump, the login
// and the logout. Every slot but 1 does nothing (or fails) while detached.

#include "connect.h"
#include "link.h"
#include "protocol.h"
#include "requests.h"
#include "sector_fetch.h"
#include "servers.h"
#include "slots.h"
#include "sockets.h"
#include "state.h"

#include <new>
#include <string.h>

namespace {

// Bound of the server name compared with the server already resolved.
constexpr int32_t kResolvedServerCompareLimit = 0x17;
// Stage slot 7 reports when a different server's yard master must first close.
constexpr int32_t kStageServerChanged = 0;
constexpr int32_t kStageYardMasterConnected = 3;
// The fetch worker's log, truncated each time the worker starts.
constexpr char kHttpLogFile[] = "http.log";

// The two worker threads.
struct RoverWorkers {
    RoverThread* link{};
    RoverThread* fetch{};
};

RoverWorkers g_workers;

// The disconnect callback slot 6 sets aside while it restarts the workers.
RoverDisconnectFn g_saved_disconnect = nullptr;

/// Swallows one disconnect report and puts the game's callback back.
///
/// @param which link that was lost; ignored
/// @param connecting whether it was still connecting; ignored
/// @param socket_error its last socket error; ignored
void ROVER_CALL
restore_saved_disconnect(int32_t /*which*/, char /*connecting*/, int32_t /*socket_error*/) {
    g_rover_state->disconnect = g_saved_disconnect;
}

// The fetch worker's log while it runs; null once the worker has closed it,
// so a later start never closes it twice.
RoverFile* g_http_log = nullptr;

/// Runs the fetch worker: creates http.log afresh, keeps it open while the worker serves the fetch
/// queue, then closes it.
///
/// @param context thread context; unused
void run_fetch_thread(void* /*context*/) {
    const RoverPlatform* platform = rover_platform();
    g_http_log = platform->file_create(kHttpLogFile);
    rover_serve_fetches(*g_rover_state);
    if (g_http_log) {
        platform->file_close(g_http_log);
        g_http_log = nullptr;
    }
}

/// Frees everything the session block owns: its events, links, server list, fetch queue and event
/// queues.
///
/// @param[in,out] state session block to empty
void teardown_state(RoverState& state) {
    const RoverPlatform* platform = rover_platform();
    RoverEvent* events[] = {
        state.stop_event,
        state.connect_event[0],
        state.connect_event[1],
        state.connect_event[2],
        state.fetch_stop_event
    };
    for (RoverEvent* event : events) {
        if (event)
            platform->event_destroy(event);
    }
    for (RoverConnection& link : state.links)
        rover_link_teardown(link);
    rover_servers_clear(state.servers);
    rover_fetch_queue_teardown(state.fetches);
    for (RoverEventQueue& queue : state.outbound)
        rover_queue_teardown(queue);
    rover_queue_teardown(state.inbound);
}

/// Tears down and frees the session block, leaving it null.
void free_state() {
    rover_sector_fetch_cancel();
    if (g_rover_state) {
        teardown_state(*g_rover_state);
        delete g_rover_state;
    }
    g_rover_state = nullptr;
}

/// Allocates and builds the session block: queues, events, links and the empty file-server table.
///
/// A partly built block is freed again.
///
/// @return false when any part could not be created
bool create_state() {
    const RoverPlatform* platform = rover_platform();
    g_rover_state = new (std::nothrow) RoverState{};
    if (!g_rover_state)
        return false;
    RoverState& state = *g_rover_state;
    bool ready = rover_queue_init(state.inbound);
    for (RoverEventQueue& queue : state.outbound)
        ready = rover_queue_init(queue) && ready;
    for (RoverConnection& link : state.links)
        ready = rover_link_init(link) && ready;
    ready = rover_fetch_queue_init(state.fetches) && ready;
    state.stop_event = platform->event_create(true, false);
    state.fetch_stop_event = platform->event_create(true, false);
    for (RoverEvent*& event : state.connect_event)
        event = platform->event_create(true, false);
    ready = ready && state.stop_event && state.fetch_stop_event && state.connect_event[0] &&
            state.connect_event[1] && state.connect_event[2];
    for (RoverEventQueue& queue : state.outbound) {
        if (ready)
            ready = (queue.ready = platform->event_create(true, false)) != nullptr;
    }
    if (ready)
        ready = (state.inbound.ready = platform->event_create(true, false)) != nullptr;
    if (ready)
        ready = (state.fetches.ready = platform->event_create(true, false)) != nullptr;
    for (RoverConnection& link : state.links) {
        if (ready)
            ready = rover_link_create_event(link);
    }
    if (!ready) {
        free_state();
        return false;
    }
    for (RoverFileServer& server : state.file_servers)
        server.type_index = OA_ROVER_FILE_SERVER_INVALID;
    return true;
}

} // namespace

bool ROVER_CALL rover_attach(void) {
    const RoverPlatform* platform = rover_platform();
    if (!rover_sockets_init() || !platform->net_startup())
        return false;
    if (!create_state())
        return false;
    RoverServerList& servers = g_rover_state->servers;
    if (!rover_servers_read(servers, kRoverSavedServerFile) &&
        !rover_servers_read(servers, kRoverServerFile))
        rover_servers_add_default(servers);
    return true;
}

void ROVER_CALL rover_detach(void) {
    const RoverPlatform* platform = rover_platform();
    if (!g_rover_state)
        return;
    while (g_rover_state->worker_count != 0)
        platform->sleep_ms(0);
    free_state();
    platform->net_cleanup();
}

bool ROVER_CALL rover_activate(void) {
    const RoverPlatform* platform = rover_platform();
    if (!g_rover_state)
        return false;
    RoverState& state = *g_rover_state;
    if (state.worker_count == 0) {
        g_workers.link = platform->thread_start(rover_run_link_thread, nullptr);
        if (!g_workers.link)
            return false;
        ++state.worker_count;
        g_workers.fetch = platform->thread_start(run_fetch_thread, nullptr);
        if (!g_workers.fetch)
            return false;
        ++state.worker_count;
    }
    rover_handler_arm(state.handlers, RoverHandlerId::Latency);
    return true;
}

void ROVER_CALL rover_deactivate(void) {
    const RoverPlatform* platform = rover_platform();
    if (!g_rover_state)
        return;
    RoverState& state = *g_rover_state;
    if (state.worker_count != 0) {
        state.stopping = true;
        platform->event_set(state.stop_event);
        platform->event_set(state.fetch_stop_event);
        if (g_workers.link)
            platform->thread_join(g_workers.link);
        if (g_workers.fetch)
            platform->thread_join(g_workers.fetch);
        g_workers = RoverWorkers{};
        rover_sockets_shutdown();
    }
    state.worker_count = 0;
}

void ROVER_CALL rover_pump_events(void) {
    for (;;) {
        if (!g_rover_state)
            return;
        RoverRecord* record = rover_queue_pop(g_rover_state->inbound);
        if (!record)
            return;
        while (RoverEntry* entry = rover_record_end_entry(*record, false)) {
            rover_sector_fetch_offer(*record, *entry);
            if (rover_text_equals(entry->name, entry->name_length, kRoverClientService))
                rover_dispatch_message(g_rover_state->handlers, *entry);
            if (rover_record_entry_count(*record) == 1)
                break;
            rover_record_remove_first_entry(*record);
        }
        rover_record_destroy(&record);
    }
}

bool ROVER_CALL rover_reset_session(void) {
    if (!g_rover_state)
        return false;
    RoverState& state = *g_rover_state;
    rover_sector_fetch_cancel();
    state.login_started = false;
    state.resolved_server[0] = '\0';
    rover_handlers_disarm_all(state.handlers);
    g_saved_disconnect = state.disconnect;
    state.disconnect = restore_saved_disconnect;
    rover_link_stop(rover_link(state, RoverLinkId::YardMaster));
    rover_link_stop(rover_link(state, RoverLinkId::Boneyard1));
    rover_link_stop(rover_link(state, RoverLinkId::Boneyard2));
    rover_deactivate();
    return rover_activate();
}

bool ROVER_CALL rover_start_login(
    const char* server_name,
    const char* username,
    const char* password,
    RoverLoginProgressFn progress,
    RoverSystemAlertFn system_alert,
    RoverDisconnectFn disconnect
) {
    if (!g_rover_state)
        return false;
    RoverState& state = *g_rover_state;
    rover_servers_select(state.servers, server_name);
    if (state.login_started)
        return false;
    state.login_started = true;
    rover_copy_bounded(state.server_name, server_name, sizeof state.server_name);
    state.reserved_after_server_name = 1;
    rover_copy_bounded(state.username, username, sizeof state.username);
    if (password)
        rover_copy_bounded(state.password, password, sizeof state.password);
    else
        state.password[0] = '\0';
    state.progress = progress;
    state.system_alert = system_alert;
    state.disconnect = disconnect;
    state.login_stage = 0;
    bool resolved = state.resolved_server[0] != '\0';
    if (resolved && strncmp(server_name, state.resolved_server, kResolvedServerCompareLimit) != 0) {
        RoverHandlerSet& handlers = state.handlers;
        if (rover_handler_is_armed(handlers, RoverHandlerId::YardMasterClosed))
            rover_handler_disarm(handlers, RoverHandlerId::YardMasterClosed);
        handlers.yardmaster_closed = rover_connect_yard_master;
        rover_handler_arm(handlers, RoverHandlerId::YardMasterClosed);
        rover_link_stop(rover_link(state, RoverLinkId::YardMaster));
        state.progress(kStageServerChanged, 0, 0, 0, nullptr);
    } else if (!resolved) {
        rover_connect_yard_master();
    } else {
        rover_request_boneyard();
        state.progress(kStageYardMasterConnected, 0, 0, 0, nullptr);
        state.login_stage = kStageYardMasterConnected;
    }
    return true;
}

bool ROVER_CALL rover_send_logout(int32_t user_id, RoverLogoutFn logged_out) {
    if (!g_rover_state)
        return false;
    RoverState& state = *g_rover_state;
    RoverHandlerSet& handlers = state.handlers;
    if (rover_handler_is_armed(handlers, RoverHandlerId::LogoutConfirmed))
        rover_handler_disarm(handlers, RoverHandlerId::LogoutConfirmed);
    handlers.logout_confirmed = logged_out;
    rover_handler_arm(handlers, RoverHandlerId::LogoutConfirmed);
    RoverRecord* request = rover_request_logout(nullptr);
    if (request) {
        if (RoverEntry* entry = rover_record_end_entry(*request, true))
            rover_entry_set_uint(*entry, kRoverKeyUserId, static_cast<uint32_t>(user_id));
        rover_queue_push(
            state.outbound[static_cast<int32_t>(rover_active_boneyard(state))], request
        );
    }
    return true;
}

void ROVER_CALL rover_set_latency_reply_flags(uint32_t flags) {
    if (g_rover_state)
        g_rover_state->latency_reply_flags = flags;
}

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// HTTP downloads for slot 32. The game's fetches queue here; the fetch
// worker thread (started by slot 3) takes them in turn, saves each body to
// its file with a plain HTTP/1.0 GET, and posts an "httprequestcomplete"
// record that slot 5 hands to the game's completion callback.
#pragma once

#include "oa/rover/interface.h"
#include "oa/rover/platform.h"

#include <stdint.h>

// Size of a queued URL or file name, terminator included.
constexpr int32_t kRoverFetchTextLimit = 0x104;

// One queued fetch.
struct RoverFetchJob {
    char url[kRoverFetchTextLimit]{};
    char destination[kRoverFetchTextLimit]{};
    RoverHttpProgressFn progress{}; // called on the fetch thread
    RoverFetchJob* next{};
};

// The fetch queue of RoverState.fetches; its manual-reset event, created with the
// session's other queue events, is set while a fetch waits.
struct RoverFetchQueue {
    RoverEvent* ready{};
    RoverFetchJob* first{};
    RoverFetchJob* last{};
    int32_t count{};
    RoverLock* lock{};
};

/// Creates the fetch queue's lock; its event is created with the session's other queue events.
///
/// @param[out] queue queue to set up
/// @return false when the lock could not be created
bool rover_fetch_queue_init(RoverFetchQueue& queue);

/// Frees every queued fetch and the queue's lock and event.
///
/// @param[in,out] queue queue to tear down
void rover_fetch_queue_teardown(RoverFetchQueue& queue);

struct RoverState;

/// Serves the fetch queue until the fetch stop event is set; this is the fetch worker's loop.
///
/// A URL that does not parse is dropped without a completion; every other fetch posts one whether
/// or not it succeeded. Leaving on the stop event lowers the session's worker count.
///
/// @param[in,out] state session block holding the fetch queue and its events
void rover_serve_fetches(RoverState& state);

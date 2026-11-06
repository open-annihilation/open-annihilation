// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The session block and its queues.

#include "state.h"

#include "link.h"
#include "servers.h"

#include <new>
#include <string.h>

RoverState* g_rover_state = nullptr;

RoverConnection& rover_link(RoverState& state, RoverLinkId id) {
    return state.links[static_cast<int32_t>(id)];
}

RoverLinkId rover_active_boneyard(const RoverState& state) {
    return state.second_boneyard ? RoverLinkId::Boneyard2 : RoverLinkId::Boneyard1;
}

void rover_copy_bounded(char* destination, const char* source, int32_t capacity) {
    if (!source || capacity <= 0)
        return;
    int32_t length = 0;
    while (length < capacity - 1 && source[length]) {
        destination[length] = source[length];
        ++length;
    }
    destination[length] = '\0';
}

bool rover_queue_init(RoverEventQueue& queue) {
    queue.lock = rover_platform()->lock_create();
    return queue.lock != nullptr;
}

void rover_queue_teardown(RoverEventQueue& queue) {
    const RoverPlatform* platform = rover_platform();
    while (RoverRecord* record = queue.first) {
        queue.first = record->next_queued;
        rover_record_destroy(&record);
    }
    queue.last = nullptr;
    queue.count = 0;
    if (queue.lock)
        platform->lock_destroy(queue.lock);
    if (queue.ready)
        platform->event_destroy(queue.ready);
    queue.lock = nullptr;
    queue.ready = nullptr;
}

void rover_queue_push(RoverEventQueue& queue, RoverRecord* record) {
    const RoverPlatform* platform = rover_platform();
    platform->lock_enter(queue.lock);
    record->next_queued = nullptr;
    if (queue.last)
        queue.last->next_queued = record;
    else
        queue.first = record;
    queue.last = record;
    if (++queue.count == 1)
        platform->event_set(queue.ready);
    platform->lock_leave(queue.lock);
}

RoverRecord* rover_queue_pop(RoverEventQueue& queue) {
    const RoverPlatform* platform = rover_platform();
    platform->lock_enter(queue.lock);
    RoverRecord* record = nullptr;
    if (queue.count != 0) {
        record = queue.first;
        queue.first = record->next_queued;
        if (!queue.first)
            queue.last = nullptr;
        record->next_queued = nullptr;
        if (--queue.count == 0)
            platform->event_reset(queue.ready);
    }
    platform->lock_leave(queue.lock);
    return record;
}

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The get_sector exchange. It is not an interface slot: the client calls it
// around login, and the event pump offers every inbound entry here before
// the usual Rover dispatch. Map records and the WarManager replies are kept
// only while an exchange is open.
#pragma once

#include "record.h"

#include <stdint.h>

// How many planet and connector records one exchange will collect, and how
// many records it will collect in all. A sector past these is refused.
inline constexpr int32_t kSectorFetchPlanetLimit = 256;
inline constexpr int32_t kSectorFetchConnectorLimit = 512;
inline constexpr int32_t kSectorFetchRecordLimit = 512;

// Where an exchange stands. Idle ignores Map records. Open is waiting.
// Unchanged means the server kept the copy the client already has. Ready
// means the records arrived in order and the count matches. Failed means
// the exchange stopped and nothing should be written.
enum class SectorFetchPhase : int32_t {
    Idle = 0,
    Open = 1,
    Unchanged = 2,
    Ready = 3,
    Failed = 4,
};

// The reply of the exchange that is open, or the one that just finished.
// bytes and sha256 are null unless that phase keeps them, and they live
// until the next begin or cancel.
struct SectorFetchReply {
    SectorFetchPhase phase;
    const char* bytes;
    int32_t size;
    int32_t records;
    const char* sha256;
    int32_t planets;
    int32_t connectors;
};

/// Sends get_sector for area_id and opens an exchange.
///
/// have is the SHA-256 the client already has, or null or empty when it has
/// no copy. A previous exchange is cancelled first. False while Rover is
/// detached or the request cannot be queued, and then no exchange is open.
///
/// @param area_id the logged-in area, sent as given; it does not select the map
/// @param have_or_null SHA-256 text to send, or null or empty to omit it
/// @return true when the request was queued and the exchange is open
bool rover_sector_fetch_begin(uint32_t area_id, const char* have_or_null);

/// Offers one inbound entry to the open exchange.
///
/// Called from the event pump for every entry, before Rover dispatch. Outside
/// an open exchange it does nothing, so a Map record that arrives on its own
/// is ignored. It never takes the record and it does not throw.
///
/// @param record record the entry belongs to
/// @param entry entry to consider
void rover_sector_fetch_offer(const RoverRecord& record, const RoverEntry& entry);

/// Drops the exchange. A later Map record or sector_end is ignored.
void rover_sector_fetch_cancel();

/// Returns the phase of the exchange.
///
/// @return Idle when none is open
SectorFetchPhase rover_sector_fetch_phase();

/// Fills out with the exchange as it stands. Null is ignored.
///
/// @param[out] out receives the reply
void rover_sector_fetch_reply(SectorFetchReply* out);

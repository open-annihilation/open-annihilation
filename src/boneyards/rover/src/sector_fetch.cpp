// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Collects one get_sector reply. The server streams Map records, the sector
// record first, then its planets and connectors, and finishes with
// sector_end or sector_unchanged. Each kept Map record is stored with the
// CRLF the file uses. The hash and the sector reader are the client's; this
// only checks that the records arrived in order and that the count matches.

#include "sector_fetch.h"

#include "lobby.h"
#include "protocol.h"
#include "requests.h"
#include "state.h"

#include <cstring>
#include <exception>
#include <string>

namespace {

constexpr char kCommandGetSector[] = "get_sector";
constexpr char kCommandSector[] = "sector";
constexpr char kCommandPlanet[] = "planet";
constexpr char kCommandConnector[] = "connector";
constexpr char kCommandSectorEnd[] = "sector_end";
constexpr char kCommandUnchanged[] = "sector_unchanged";
constexpr char kCommandError[] = "error";
constexpr char kServiceMap[] = "Map";
constexpr char kServiceWarManager[] = "WarManager";
constexpr char kKeyHave[] = "have";
constexpr char kKeyRecords[] = "records";
constexpr char kKeySha256[] = "sha256";
constexpr char kKeyPlanets[] = "planets";
constexpr char kKeyConnectors[] = "connectors";
constexpr char kLineEnding[] = "\r\n";
// The assembled file, including every CRLF. Past this the exchange fails.
constexpr std::size_t kByteLimit = 1u << 20;

enum class Expect : int32_t {
    Sector = 0,
    Planet = 1,
    Connector = 2,
    End = 3,
};

struct Exchange {
    SectorFetchPhase phase = SectorFetchPhase::Idle;
    Expect expect = Expect::Sector;
    std::string bytes;
    std::string sha;
    int32_t lines = 0;
    int32_t records = 0;
    int32_t planets = 0;
    int32_t connectors = 0;
    int32_t seen_planets = 0;
    int32_t seen_connectors = 0;
};

Exchange g_exchange;

void fail_exchange() {
    g_exchange.bytes.clear();
    g_exchange.sha.clear();
    g_exchange.lines = 0;
    g_exchange.records = 0;
    g_exchange.planets = 0;
    g_exchange.connectors = 0;
    g_exchange.seen_planets = 0;
    g_exchange.seen_connectors = 0;
    g_exchange.phase = SectorFetchPhase::Failed;
}

bool command_is(const RoverEntry& entry, const char* name) {
    const char* command = nullptr;
    if (!rover_entry_get_string(entry, kRoverKeyCommand, &command, nullptr) || command == nullptr)
        return false;
    return std::strcmp(command, name) == 0;
}

bool append_record(const RoverRecord& record) {
    if (record.text_length <= 0)
        return false;
    const std::size_t incoming = static_cast<std::size_t>(record.text_length) + 2u;
    if (g_exchange.bytes.size() + incoming > kByteLimit)
        return false;
    if (g_exchange.lines >= kSectorFetchRecordLimit)
        return false;
    g_exchange.bytes.append(record.text, static_cast<std::size_t>(record.text_length));
    g_exchange.bytes.append(kLineEnding);
    ++g_exchange.lines;
    return true;
}

void after_body_record() {
    if (g_exchange.expect == Expect::Planet) {
        ++g_exchange.seen_planets;
        if (g_exchange.seen_planets < g_exchange.planets)
            return;
        g_exchange.expect = g_exchange.connectors > 0 ? Expect::Connector : Expect::End;
        return;
    }
    ++g_exchange.seen_connectors;
    if (g_exchange.seen_connectors >= g_exchange.connectors)
        g_exchange.expect = Expect::End;
}

void take_sector(const RoverEntry& entry) {
    int32_t planets = -1;
    int32_t connectors = -1;
    if (!rover_entry_get_int(entry, kKeyPlanets, &planets, -1) || planets < 0 ||
        planets > kSectorFetchPlanetLimit) {
        fail_exchange();
        return;
    }
    if (!rover_entry_get_int(entry, kKeyConnectors, &connectors, -1) || connectors < 0 ||
        connectors > kSectorFetchConnectorLimit) {
        fail_exchange();
        return;
    }
    if (1 + planets + connectors > kSectorFetchRecordLimit) {
        fail_exchange();
        return;
    }
    g_exchange.planets = planets;
    g_exchange.connectors = connectors;
    if (planets > 0)
        g_exchange.expect = Expect::Planet;
    else if (connectors > 0)
        g_exchange.expect = Expect::Connector;
    else
        g_exchange.expect = Expect::End;
}

void take_map(const RoverRecord& record, const RoverEntry& entry) {
    if (rover_record_entry_count(record) != 1) {
        fail_exchange();
        return;
    }
    if (g_exchange.expect == Expect::Sector) {
        if (!command_is(entry, kCommandSector) || !append_record(record)) {
            fail_exchange();
            return;
        }
        take_sector(entry);
        return;
    }
    if (g_exchange.expect == Expect::Planet) {
        if (!command_is(entry, kCommandPlanet) || !append_record(record)) {
            fail_exchange();
            return;
        }
        after_body_record();
        return;
    }
    if (g_exchange.expect == Expect::Connector) {
        if (!command_is(entry, kCommandConnector) || !append_record(record)) {
            fail_exchange();
            return;
        }
        after_body_record();
        return;
    }
    fail_exchange();
}

void take_war_manager(const RoverRecord& record, const RoverEntry& entry) {
    if (rover_record_entry_count(record) != 1) {
        fail_exchange();
        return;
    }
    if (command_is(entry, kCommandUnchanged)) {
        if (g_exchange.expect != Expect::Sector || g_exchange.lines != 0) {
            fail_exchange();
            return;
        }
        const char* sha = nullptr;
        if (!rover_entry_get_string(entry, kKeySha256, &sha, nullptr) || sha == nullptr ||
            sha[0] == '\0') {
            fail_exchange();
            return;
        }
        g_exchange.sha = sha;
        g_exchange.phase = SectorFetchPhase::Unchanged;
        return;
    }
    if (command_is(entry, kCommandSectorEnd)) {
        int32_t records = -1;
        const char* sha = nullptr;
        if (g_exchange.expect != Expect::End ||
            !rover_entry_get_int(entry, kKeyRecords, &records, -1) || records != g_exchange.lines ||
            !rover_entry_get_string(entry, kKeySha256, &sha, nullptr) || sha == nullptr ||
            sha[0] == '\0') {
            fail_exchange();
            return;
        }
        g_exchange.sha = sha;
        g_exchange.records = records;
        g_exchange.phase = SectorFetchPhase::Ready;
        return;
    }
    fail_exchange();
}

void offer_entry(const RoverRecord& record, const RoverEntry& entry) {
    if (g_exchange.phase != SectorFetchPhase::Open)
        return;
    if (rover_text_equals(entry.name, entry.name_length, kServiceMap)) {
        take_map(record, entry);
        return;
    }
    if (rover_text_equals(entry.name, entry.name_length, kServiceWarManager)) {
        take_war_manager(record, entry);
        return;
    }
    // A Rover error still has to reach the usual dispatch. Other Rover
    // traffic during the exchange is left alone.
    if (rover_text_equals(entry.name, entry.name_length, kRoverClientService) &&
        command_is(entry, kCommandError))
        fail_exchange();
}

} // namespace

bool rover_sector_fetch_begin(uint32_t area_id, const char* have_or_null) {
    try {
        if (!g_rover_state)
            return false;
        rover_sector_fetch_cancel();
        RoverRecord* record = nullptr;
        RoverEntry* entry = rover_add_request(&record, RoverService::WarManager, kCommandGetSector);
        if (entry == nullptr || !rover_entry_set_uint(*entry, kRoverKeyAreaId, area_id)) {
            rover_record_destroy(&record);
            return false;
        }
        if (have_or_null != nullptr && have_or_null[0] != '\0' &&
            !rover_entry_set_string(*entry, kKeyHave, have_or_null)) {
            rover_record_destroy(&record);
            return false;
        }
        rover_send_to_boneyard(record);
        g_exchange.phase = SectorFetchPhase::Open;
        g_exchange.expect = Expect::Sector;
        return true;
    } catch (const std::exception&) {
        rover_sector_fetch_cancel();
        return false;
    }
}

void rover_sector_fetch_offer(const RoverRecord& record, const RoverEntry& entry) {
    try {
        offer_entry(record, entry);
    } catch (const std::exception&) {
        fail_exchange();
    }
}

void rover_sector_fetch_cancel() {
    g_exchange = Exchange{};
}

SectorFetchPhase rover_sector_fetch_phase() {
    return g_exchange.phase;
}

void rover_sector_fetch_reply(SectorFetchReply* out) {
    if (out == nullptr)
        return;
    out->phase = g_exchange.phase;
    out->bytes = g_exchange.bytes.empty() ? nullptr : g_exchange.bytes.c_str();
    out->size = static_cast<int32_t>(g_exchange.bytes.size());
    out->records = g_exchange.records;
    out->sha256 = g_exchange.sha.empty() ? nullptr : g_exchange.sha.c_str();
    out->planets = g_exchange.planets;
    out->connectors = g_exchange.connectors;
}

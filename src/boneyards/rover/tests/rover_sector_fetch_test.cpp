// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// get_sector without a network: the request that is queued, and which
// inbound records move the exchange to ready, unchanged or failed. Map
// records outside an open exchange stay ignored.

#include "../src/record.h"
#include "../src/sector_fetch.h"
#include "../src/slots.h"
#include "../src/state.h"

#include <stdio.h>
#include <string.h>

#include <string>

namespace {

int failures = 0;

void check(bool condition, const char* what) {
    if (!condition) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

void drain() {
    if (!g_rover_state)
        return;
    while (RoverRecord* record = rover_queue_pop(g_rover_state->inbound))
        rover_record_destroy(&record);
    while (RoverRecord* record = rover_queue_pop(g_rover_state->outbound[0]))
        rover_record_destroy(&record);
    rover_sector_fetch_cancel();
}

void push_inbound(const char* text) {
    RoverRecord* pending = nullptr;
    RoverRecord* parsed = nullptr;
    int32_t consumed = 0;
    const int32_t length = static_cast<int32_t>(strlen(text));
    const RoverRecordStatus status =
        rover_record_receive(&pending, text, length, &consumed, &parsed);
    if (parsed != nullptr)
        pending = nullptr;
    rover_record_destroy(&pending);
    check(status == RoverRecordStatus::Ok && parsed != nullptr && consumed == length, text);
    if (status == RoverRecordStatus::Ok && parsed != nullptr && consumed == length)
        rover_queue_push(g_rover_state->inbound, parsed);
    else
        rover_record_destroy(&parsed);
}

std::string pop_sent() {
    RoverRecord* record = rover_queue_pop(g_rover_state->outbound[0]);
    if (record == nullptr)
        return {};
    char text[kRoverRecordTextLimit + 1] = {};
    int32_t length = 0;
    const RoverRecordStatus status =
        rover_record_format(*record, text, static_cast<int32_t>(sizeof text), &length);
    rover_record_destroy(&record);
    if (status != RoverRecordStatus::Ok)
        return {};
    return std::string(text, static_cast<size_t>(length));
}

void pump() {
    rover_pump_events();
}

const char* kSector = "{[Map<S:command=sector><I:planets=1><I:connectors=1>]}";
const char* kPlanet = "{[Map<S:command=planet><I:id=1>]}";
const char* kConnector = "{[Map<S:command=connector><I:planet1=1><I:planet2=2>]}";
const char* kSha = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";

std::string sector_end(int32_t records) {
    return "{[WarManager<S:command=sector_end><I:records=" + std::to_string(records) +
           "><S:sha256=" + kSha + ">]}";
}

void check_request() {
    drain();
    check(rover_sector_fetch_begin(1801, "abc"), "begin with a hash");
    check(
        pop_sent() == "{[WarManager<U:area_id=709><S:command=get_sector><S:have=abc>]}",
        "request carries the area and the hash"
    );
    check(rover_sector_fetch_begin(0, nullptr), "begin without a hash");
    check(
        pop_sent() == "{[WarManager<U:area_id=0><S:command=get_sector>]}",
        "a missing hash is omitted and area 0 is still sent"
    );
    rover_sector_fetch_cancel();
}

void check_ready() {
    drain();
    check(rover_sector_fetch_begin(7, nullptr), "begin for a stream");
    (void)pop_sent();
    push_inbound(kSector);
    push_inbound(kPlanet);
    push_inbound(kConnector);
    push_inbound(sector_end(3).c_str());
    pump();
    SectorFetchReply reply{};
    rover_sector_fetch_reply(&reply);
    const std::string expect =
        std::string(kSector) + "\r\n" + kPlanet + "\r\n" + kConnector + "\r\n";
    check(reply.phase == SectorFetchPhase::Ready, "a complete stream is ready");
    check(reply.records == 3 && reply.planets == 1 && reply.connectors == 1, "ready counts");
    check(reply.sha256 != nullptr && strcmp(reply.sha256, kSha) == 0, "ready hash");
    check(
        reply.bytes != nullptr && reply.size == static_cast<int32_t>(expect.size()) &&
            memcmp(reply.bytes, expect.data(), expect.size()) == 0,
        "ready bytes keep each record and a CRLF"
    );
    rover_sector_fetch_cancel();
    check(rover_sector_fetch_phase() == SectorFetchPhase::Idle, "cancel returns to idle");
}

void check_unchanged() {
    drain();
    check(rover_sector_fetch_begin(7, "abc"), "begin for unchanged");
    (void)pop_sent();
    push_inbound("{[WarManager<S:command=sector_unchanged><S:sha256=abc>]}");
    pump();
    SectorFetchReply reply{};
    rover_sector_fetch_reply(&reply);
    check(reply.phase == SectorFetchPhase::Unchanged, "sector_unchanged");
    check(reply.bytes == nullptr && reply.size == 0, "unchanged keeps no records");
    check(reply.sha256 != nullptr && strcmp(reply.sha256, "abc") == 0, "unchanged hash");
}

void check_failures() {
    drain();
    push_inbound(kPlanet);
    pump();
    check(
        rover_sector_fetch_phase() == SectorFetchPhase::Idle, "a map before the exchange is ignored"
    );

    check(rover_sector_fetch_begin(7, nullptr), "begin for a planet first");
    (void)pop_sent();
    push_inbound(kPlanet);
    pump();
    check(
        rover_sector_fetch_phase() == SectorFetchPhase::Failed, "a planet before the sector fails"
    );

    drain();
    check(rover_sector_fetch_begin(7, nullptr), "begin for a missing planet");
    (void)pop_sent();
    push_inbound(kSector);
    push_inbound(sector_end(1).c_str());
    pump();
    check(
        rover_sector_fetch_phase() == SectorFetchPhase::Failed,
        "sector_end before the planets fails"
    );

    drain();
    check(rover_sector_fetch_begin(7, nullptr), "begin for a bad count");
    (void)pop_sent();
    push_inbound(kSector);
    push_inbound(kPlanet);
    push_inbound(kConnector);
    push_inbound(sector_end(2).c_str());
    pump();
    check(
        rover_sector_fetch_phase() == SectorFetchPhase::Failed, "a mismatched record count fails"
    );

    drain();
    check(rover_sector_fetch_begin(7, nullptr), "begin for an error");
    (void)pop_sent();
    push_inbound("{[WarManager<S:command=error><S:message=unavailable>]}");
    pump();
    check(rover_sector_fetch_phase() == SectorFetchPhase::Failed, "an error fails the exchange");

    drain();
    push_inbound(kSector);
    push_inbound(sector_end(1).c_str());
    pump();
    check(rover_sector_fetch_phase() == SectorFetchPhase::Idle, "records after cancel are ignored");
}

} // namespace

int main() {
    if (!rover_attach()) {
        fprintf(stderr, "attach failed\n");
        return 1;
    }
    check_request();
    check_ready();
    check_unchanged();
    check_failures();
    drain();
    rover_detach();
    if (failures != 0) {
        fprintf(stderr, "%d check(s) failed\n", failures);
        return 1;
    }
    return 0;
}

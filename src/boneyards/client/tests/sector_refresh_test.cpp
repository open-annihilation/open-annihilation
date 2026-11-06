// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The sector fetched at login. A stream that checks out replaces the player's
// map and is what the galactic map then opens; a hash that does not match, a
// missing planet, a record out of place, a map the sector reader refuses, an
// error or a silence leaves the copy already there. Map records outside the
// exchange are ignored. Nothing here contacts a server.

#include "oa/boneyards/rover_session.hpp"

#include "record.h"
#include "sector_fetch.h"
#include "slots.h"
#include "state.h"

#include "oa/base/sha256.hpp"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr uint32_t kAreaId = 7;
constexpr const char* kArea = "alpha";
constexpr const char* kHeader =
    "{[Map<S:command=sector><I:id=45><S:name=New Hub><I:width=2000><I:height=1400>"
    "<I:planets=2><I:connectors=1>]}";
constexpr const char* kPlanetA =
    "{[Map<S:command=planet><I:id=1803><S:name=Fuel Dump><I:x=991><I:y=849><S:seqname=Metal_L>"
    "<S:side=A><I:capitol=0><S:desc=A metal world%8><S:ta_map=Over Crude Water>"
    "<S:ta_mapsource=TA><S:ta_maptype=Metal><S:pan_gaf=Metalbrief.gaf><I:planet_rot=5>"
    "<S:pan_seq=MetalPan><I:playerlimit=4><I:watching=2><I:los=3><I:location=1><I:deathopt=2>"
    "<I:maxunits=500><I:metal=10000><I:energy=0><I:mapping=1>]}";
constexpr const char* kPlanetB =
    "{[Map<S:command=planet><I:id=1804><S:name=Respite><I:x=20><I:y=1355><S:seqname=Green_m>"
    "<S:side=C><I:capitol=1><S:desc=Forest><S:ta_map=Sherwood><S:ta_mapsource=CC>"
    "<S:ta_maptype=Green><S:pan_gaf=Greenbrief.gaf><S:pan_seq=GreenPan>]}";
constexpr const char* kConnector = "{[Map<S:command=connector><I:planet1=1803><I:planet2=1804>]}";

std::string join_crlf(std::initializer_list<std::string_view> lines) {
    std::string out;
    for (std::string_view line : lines) {
        out.append(line);
        out.append("\r\n");
    }
    return out;
}

std::string sha256_hex(std::string_view bytes) {
    const auto digest = oa::base::sha256::digest_of(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size())
    );
    const auto hex = oa::base::sha256::to_hex(digest);
    return std::string(hex.data(), hex.size());
}

std::string replaced(std::string_view text, std::string_view from, std::string_view to) {
    std::string out(text);
    const std::size_t at = out.find(from);
    OA_CHECK(at != std::string::npos);
    if (at != std::string::npos)
        out.replace(at, from.size(), to);
    return out;
}

std::string read_bytes(const fs::path& path) {
    std::ifstream in(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

void write_bytes(const fs::path& path, std::string_view bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    OA_CHECK(static_cast<bool>(out));
}

void push_inbound(const std::string& text) {
    RoverRecord* pending = nullptr;
    RoverRecord* parsed = nullptr;
    int32_t consumed = 0;
    const auto length = static_cast<int32_t>(text.size());
    const RoverRecordStatus status =
        rover_record_receive(&pending, text.data(), length, &consumed, &parsed);
    if (parsed != nullptr)
        pending = nullptr;
    rover_record_destroy(&pending);
    OA_CHECK(status == RoverRecordStatus::Ok && parsed != nullptr && consumed == length);
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
    return std::string(text, static_cast<std::size_t>(length));
}

void drain() {
    if (g_rover_state == nullptr)
        return;
    while (RoverRecord* record = rover_queue_pop(g_rover_state->inbound))
        rover_record_destroy(&record);
    while (RoverRecord* record = rover_queue_pop(g_rover_state->outbound[0]))
        rover_record_destroy(&record);
    rover_sector_fetch_cancel();
}

std::string sector_end(int32_t records, std::string_view sha) {
    return "{[WarManager<S:command=sector_end><I:records=" + std::to_string(records) +
           "><S:sha256=" + std::string(sha) + ">]}";
}

std::string opened_name(const char* path) {
    int32_t sector_id = 0;
    int32_t width = 0;
    int32_t height = 0;
    int32_t planets = 0;
    int32_t connectors = 0;
    char name[51] = {};
    RoverSectorReader* reader =
        rover_open_sector_map(path, &sector_id, name, &width, &height, &planets, &connectors);
    if (reader == nullptr)
        return {};
    rover_close_sector_map(reader);
    return name;
}

const std::string kNewHub = join_crlf({kHeader, kPlanetA, kPlanetB, kConnector});
const std::string kOldYard =
    join_crlf({replaced(kHeader, "New Hub", "Old Yard"), kPlanetA, kPlanetB, kConnector});
const std::string kBadPlanet = replaced(kPlanetA, "<I:x=991>", "<I:x=1>");
const std::string kUnreadable = join_crlf({kHeader, kBadPlanet, kPlanetB, kConnector});

struct Fixture {
    fs::path scratch;
    fs::path install_map;
    fs::path player_map;
    fs::path player_part;

    bool attached = false;

    explicit Fixture(const fs::path& root)
        : scratch(root), install_map(root / "install" / "Boneyards" / "alpha_map.gwm"),
          player_map(root / "player" / "Boneyards" / "alpha_map.gwm"),
          player_part(root / "player" / "Boneyards" / "alpha_map.gwm.part") {
        write_bytes(install_map, kOldYard);
        oa::boneyards::rover_session_configure((root / "install").string().c_str(), "");
        oa::boneyards::rover_session_set_player_folder((root / "player").string().c_str());
        attached = rover_attach();
        OA_CHECK(attached);
    }

    ~Fixture() {
        drain();
        if (g_rover_state != nullptr)
            rover_detach();
        std::error_code error;
        fs::remove_all(scratch, error);
    }

    Fixture(const Fixture&) = delete;
    Fixture& operator=(const Fixture&) = delete;
};

void check_absent_extras(const Fixture& fixture) {
    OA_CHECK(!fs::exists(fixture.player_part));
    OA_CHECK(!fs::exists(fixture.player_map.parent_path() / "alpha_map.gwm.bak"));
}

void check_stream_replaces_the_map(Fixture& fixture) {
    const std::string install_before = read_bytes(fixture.install_map);
    push_inbound(kPlanetA);
    rover_pump_events();
    OA_CHECK(rover_sector_fetch_phase() == SectorFetchPhase::Idle);
    OA_CHECK(!fs::exists(fixture.player_map));

    OA_CHECK(oa::boneyards::rover_session_begin_sector_fetch(kAreaId, kArea));
    OA_CHECK(pop_sent() == "{[WarManager<U:area_id=7><S:command=get_sector>]}");
    const std::string sha = sha256_hex(kNewHub);
    push_inbound(kHeader);
    push_inbound(kPlanetA);
    push_inbound(kPlanetB);
    push_inbound(kConnector);
    push_inbound(sector_end(4, sha));
    rover_pump_events();
    oa::boneyards::rover_session_finish_sector_fetch(0);
    OA_CHECK(read_bytes(fixture.player_map) == kNewHub);
    OA_CHECK(read_bytes(fixture.install_map) == install_before);
    OA_CHECK(opened_name("Boneyards\\alpha_map.gwm") == "New Hub");
    check_absent_extras(fixture);

    const std::string kept = read_bytes(fixture.player_map);
    push_inbound(kHeader);
    rover_pump_events();
    OA_CHECK(rover_sector_fetch_phase() == SectorFetchPhase::Idle);
    OA_CHECK(read_bytes(fixture.player_map) == kept);
}

void check_unchanged_keeps_the_file(Fixture& fixture) {
    const std::string before = read_bytes(fixture.player_map);
    OA_CHECK(oa::boneyards::rover_session_begin_sector_fetch(kAreaId, kArea));
    const std::string sent = pop_sent();
    const std::string have = sha256_hex(before);
    OA_CHECK(sent == "{[WarManager<U:area_id=7><S:command=get_sector><S:have=" + have + ">]}");
    push_inbound("{[WarManager<S:command=sector_unchanged><S:sha256=" + have + ">]}");
    rover_pump_events();
    oa::boneyards::rover_session_finish_sector_fetch(0);
    OA_CHECK(read_bytes(fixture.player_map) == before);
    check_absent_extras(fixture);
}

void check_refused(Fixture& fixture, std::initializer_list<std::string> records, const char* what) {
    const std::string before = read_bytes(fixture.player_map);
    OA_CHECK(oa::boneyards::rover_session_begin_sector_fetch(kAreaId, kArea));
    OA_CHECK(!pop_sent().empty());
    for (const std::string& record : records)
        push_inbound(record);
    rover_pump_events();
    oa::boneyards::rover_session_finish_sector_fetch(0);
    OA_CHECK(read_bytes(fixture.player_map) == before);
    check_absent_extras(fixture);
    OA_CHECK(opened_name("Boneyards\\alpha_map.gwm") == "New Hub");
    (void)what;
}

void check_refusals(Fixture& fixture) {
    const std::string sha = sha256_hex(kNewHub);
    std::string wrong(oa::base::sha256::hex_size, 'a');
    check_refused(
        fixture, {kHeader, kPlanetA, kPlanetB, kConnector, sector_end(4, wrong)}, "bad hash"
    );
    check_refused(fixture, {kHeader, kPlanetA, sector_end(2, sha)}, "missing planet");
    check_refused(fixture, {kPlanetA, sector_end(1, sha)}, "planet before sector");
    const std::string bad_sha = sha256_hex(kUnreadable);
    check_refused(
        fixture,
        {kHeader, kBadPlanet, kPlanetB, kConnector, sector_end(4, bad_sha)},
        "reader rejects the planet"
    );
    check_refused(
        fixture, {"{[WarManager<S:command=error><S:message=unavailable>]}"}, "server error"
    );
}

void check_silence(Fixture& fixture) {
    const std::string before = read_bytes(fixture.player_map);
    OA_CHECK(oa::boneyards::rover_session_begin_sector_fetch(kAreaId, kArea));
    const std::string sent = pop_sent();
    OA_CHECK(sent.find("get_sector") != std::string::npos);
    oa::boneyards::rover_session_finish_sector_fetch(100);
    OA_CHECK(read_bytes(fixture.player_map) == before);
    check_absent_extras(fixture);
    OA_CHECK(rover_sector_fetch_phase() == SectorFetchPhase::Idle);
}

// A run that steps the frontend clock, and does not sleep, gives the request
// up after ninety steps. The player's map is left as it was.
void check_clock_gives_up(Fixture& fixture) {
    const std::string before = read_bytes(fixture.player_map);
    oa::boneyards::rover_session_note_clock(1000);
    OA_CHECK(oa::boneyards::rover_session_begin_sector_fetch(kAreaId, kArea));
    OA_CHECK(oa::boneyards::rover_session_sector_fetch_pending());
    OA_CHECK(!oa::boneyards::rover_session_settle_sector_fetch());
    oa::boneyards::rover_session_note_clock(1000 + oa::boneyards::kSectorFetchWaitTicks - 1);
    OA_CHECK(!oa::boneyards::rover_session_settle_sector_fetch());
    oa::boneyards::rover_session_note_clock(1000 + oa::boneyards::kSectorFetchWaitTicks);
    OA_CHECK(oa::boneyards::rover_session_settle_sector_fetch());
    OA_CHECK(!oa::boneyards::rover_session_sector_fetch_pending());
    OA_CHECK(read_bytes(fixture.player_map) == before);
    OA_CHECK(rover_sector_fetch_phase() == SectorFetchPhase::Idle);

    // The same turn the frontend clock uses (scaled_clock_turn).
    constexpr uint32_t turn = 4294967;
    oa::boneyards::rover_session_note_clock(turn - 10);
    OA_CHECK(oa::boneyards::rover_session_begin_sector_fetch(kAreaId, kArea));
    oa::boneyards::rover_session_note_clock(oa::boneyards::kSectorFetchWaitTicks - 10);
    OA_CHECK(oa::boneyards::rover_session_settle_sector_fetch());
    OA_CHECK(read_bytes(fixture.player_map) == before);
    OA_CHECK(rover_sector_fetch_phase() == SectorFetchPhase::Idle);
}

void check_unsafe_area() {
    drain();
    OA_CHECK(!oa::boneyards::rover_session_begin_sector_fetch(kAreaId, "a/b"));
    OA_CHECK(!oa::boneyards::rover_session_begin_sector_fetch(kAreaId, ""));
    OA_CHECK(!oa::boneyards::rover_session_begin_sector_fetch(kAreaId, nullptr));
    OA_CHECK(pop_sent().empty());
    OA_CHECK(oa::boneyards::rover_session_begin_sector_fetch(kAreaId, "BYPacific"));
    OA_CHECK(pop_sent().find("get_sector") != std::string::npos);
    oa::boneyards::rover_session_finish_sector_fetch(0);
}

void check_detached_begin_is_immediate() {
    if (g_rover_state != nullptr)
        rover_detach();
    OA_CHECK(!oa::boneyards::rover_session_begin_sector_fetch(kAreaId, kArea));
}

} // namespace

int main() {
    OA_CHECK(oa::boneyards::kSectorFetchWaitMs == 3000);
    OA_CHECK(oa::boneyards::kSectorFetchWaitTicks == 90);
    const fs::path scratch = oa::test::make_scratch_directory("E02-sector-refresh");
    {
        Fixture fixture(scratch);
        if (fixture.attached) {
            check_unsafe_area();
            check_stream_replaces_the_map(fixture);
            check_unchanged_keeps_the_file(fixture);
            check_refusals(fixture);
            check_silence(fixture);
            check_clock_gives_up(fixture);
        }
    }
    check_detached_begin_is_immediate();
    return oa::test::check_exit_status();
}

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// After login, ask for the current sector and, when the reply checks out,
// replace the player's copy of the war map. The dossier callback only begins
// the request. The session's idle tick gives it a few seconds, without
// sleeping or pumping again, and then says the dossier is loaded. A miss,
// an error or a failed check leaves the copy already on disk.

#include "oa/boneyards/rover_session.hpp"

#include "platform_select.h"
#include "record.h"
#include "sector_fetch.h"
#include "slots.h"
#include "state.h"

#include "oa/base/sha256.hpp"
#include "oa/platform/files.hpp"
#include "oa/rover/platform.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace oa::boneyards {
namespace {

namespace fs = std::filesystem;

constexpr int32_t kSleepSliceMs = 20;
constexpr uint32_t kFrontendClockHz = 30;
// The frontend clock turns over to 0 here, matching
// oa::base::game_loop::scaled_clock_turn. Kept as a number so this module
// does not link the game loop.
constexpr uint32_t kFrontendClockTurn = 4294967;
static_assert(
    kSectorFetchWaitTicks == static_cast<uint32_t>(kSectorFetchWaitMs) * kFrontendClockHz / 1000
);
constexpr int32_t kAreaNameLimit = 64;
constexpr std::size_t kHashReadLimit = 1u << 20;
constexpr char kMapDirectory[] = "Boneyards/";
constexpr char kMapSuffix[] = "_map.gwm";
constexpr char kPartSuffix[] = ".part";
constexpr char kBackupSuffix[] = ".bak";
constexpr char kServiceMap[] = "Map";
constexpr char kCommandSector[] = "sector";
constexpr char kCommandPlanet[] = "planet";
constexpr char kCommandConnector[] = "connector";
constexpr char kKeyPlanets[] = "planets";
constexpr char kKeyConnectors[] = "connectors";

std::string g_relative;
bool g_begun = false;
std::chrono::steady_clock::time_point g_deadline{};
uint32_t g_clock = 0;
bool g_clock_known = false;
uint32_t g_clock_start = 0;
bool g_clock_bound = false;

// Units from an earlier frontend-clock reading to a later one. The clock
// turns over to 0 at scaled_clock_turn, as the game's own clock does.
uint32_t clock_elapsed(uint32_t later, uint32_t earlier) {
    if (later >= earlier)
        return later - earlier;
    return later + (kFrontendClockTurn - earlier);
}

bool area_name_ok(const char* area) {
    if (area == nullptr || area[0] == '\0')
        return false;
    int32_t length = 0;
    for (const char* cursor = area; *cursor != '\0'; ++cursor) {
        const unsigned char letter = static_cast<unsigned char>(*cursor);
        const bool safe = (letter >= 'A' && letter <= 'Z') || (letter >= 'a' && letter <= 'z') ||
                          (letter >= '0' && letter <= '9') || letter == '_' || letter == '-';
        if (!safe)
            return false;
        ++length;
        if (length > kAreaNameLimit)
            return false;
    }
    return true;
}

char fold_ascii(char letter) {
    if (letter >= 'A' && letter <= 'Z')
        return static_cast<char>(letter - 'A' + 'a');
    return letter;
}

bool hex_digit(char letter) {
    return (letter >= '0' && letter <= '9') || (letter >= 'a' && letter <= 'f') ||
           (letter >= 'A' && letter <= 'F');
}

bool same_sha256(const char* given, std::string_view expect) {
    if (given == nullptr || expect.size() != oa::base::sha256::hex_size)
        return false;
    for (std::size_t index = 0; index < expect.size(); ++index) {
        if (!hex_digit(given[index]) || !hex_digit(expect[index]))
            return false;
        if (fold_ascii(given[index]) != fold_ascii(expect[index]))
            return false;
    }
    return given[oa::base::sha256::hex_size] == '\0';
}

// SHA-256 of the player's own copy, or empty when there is none, it is
// larger than the cap, or it cannot be read. The server then sends the map.
std::string hash_player_copy(const std::string& relative) {
    const std::string disk = rover_session_write_path(relative.c_str());
    if (disk.empty())
        return {};
    std::error_code error;
    if (!fs::is_regular_file(oa::platform::path_from_utf8(disk), error))
        return {};
    FILE* stream = oa::platform::open_file(disk.c_str(), "rb");
    if (stream == nullptr)
        return {};
    std::string bytes;
    bool readable = true;
    char chunk[4096];
    while (readable) {
        const std::size_t count = std::fread(chunk, 1, sizeof chunk, stream);
        if (count > 0)
            bytes.append(chunk, count);
        if (bytes.size() > kHashReadLimit) {
            readable = false;
            break;
        }
        if (count < sizeof chunk) {
            if (std::ferror(stream))
                readable = false;
            break;
        }
    }
    std::fclose(stream);
    if (!readable)
        return {};
    const auto digest = oa::base::sha256::digest_of(
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size())
    );
    const auto hex = oa::base::sha256::to_hex(digest);
    return std::string(hex.data(), hex.size());
}

bool next_line(
    const char* bytes, int32_t size, int32_t* cursor, const char** line, int32_t* length
) {
    const int32_t start = *cursor;
    int32_t index = start;
    while (index + 1 < size) {
        if (bytes[index] == '\r' && bytes[index + 1] == '\n') {
            *line = bytes + start;
            *length = index - start;
            *cursor = index + 2;
            return true;
        }
        if (bytes[index] == '\r' || bytes[index] == '\n')
            return false;
        ++index;
    }
    return false;
}

bool line_command(const RoverEntry& entry, const char* name) {
    const char* command = nullptr;
    if (!rover_entry_get_string(entry, "command", &command, nullptr) || command == nullptr)
        return false;
    return std::strcmp(command, name) == 0;
}

// Confirms the assembled bytes are the sector record, then its planets, then
// its connectors, each Map record one CRLF-ended line, and that the SHA-256
// of those exact bytes is the one sector_end named.
bool bytes_check(const SectorFetchReply& reply, int32_t* planets, int32_t* connectors) {
    if (reply.bytes == nullptr || reply.size <= 0 || reply.sha256 == nullptr)
        return false;
    if (reply.planets < 0 || reply.connectors < 0 || reply.planets > kSectorFetchPlanetLimit ||
        reply.connectors > kSectorFetchConnectorLimit || reply.records < 1 ||
        reply.records > kSectorFetchRecordLimit)
        return false;
    const auto digest = oa::base::sha256::digest_of(
        std::span<const uint8_t>(
            reinterpret_cast<const uint8_t*>(reply.bytes), static_cast<std::size_t>(reply.size)
        )
    );
    const auto hex = oa::base::sha256::to_hex(digest);
    if (!same_sha256(reply.sha256, std::string_view(hex.data(), hex.size())))
        return false;

    int32_t cursor = 0;
    int32_t lines = 0;
    int32_t header_planets = -1;
    int32_t header_connectors = -1;
    while (cursor < reply.size) {
        const char* line = nullptr;
        int32_t length = 0;
        if (!next_line(reply.bytes, reply.size, &cursor, &line, &length) || length <= 0)
            return false;
        RoverRecord* pending = nullptr;
        RoverRecord* parsed = nullptr;
        int32_t consumed = 0;
        const RoverRecordStatus status =
            rover_record_receive(&pending, line, length, &consumed, &parsed);
        if (parsed != nullptr)
            pending = nullptr;
        rover_record_destroy(&pending);
        const bool accepted = status == RoverRecordStatus::Ok && parsed != nullptr &&
                              consumed == length && rover_record_entry_count(*parsed) == 1;
        const RoverEntry* entry = accepted ? rover_record_end_entry(*parsed, false) : nullptr;
        bool in_order =
            entry != nullptr && rover_text_equals(entry->name, entry->name_length, kServiceMap);
        if (in_order && lines == 0) {
            int32_t read_planets = -1;
            int32_t read_connectors = -1;
            in_order = line_command(*entry, kCommandSector) &&
                       rover_entry_get_int(*entry, kKeyPlanets, &read_planets, -1) &&
                       rover_entry_get_int(*entry, kKeyConnectors, &read_connectors, -1) &&
                       read_planets == reply.planets && read_connectors == reply.connectors;
            header_planets = read_planets;
            header_connectors = read_connectors;
        } else if (in_order && lines <= reply.planets) {
            in_order = line_command(*entry, kCommandPlanet);
        } else if (in_order) {
            in_order = line_command(*entry, kCommandConnector);
        }
        rover_record_destroy(&parsed);
        if (!in_order)
            return false;
        ++lines;
    }
    if (lines != reply.records || lines != 1 + reply.planets + reply.connectors ||
        header_planets != reply.planets || header_connectors != reply.connectors)
        return false;
    *planets = header_planets;
    *connectors = header_connectors;
    return true;
}

bool write_all(const RoverPlatform* platform, RoverFile* file, const char* data, int32_t size) {
    int32_t done = 0;
    while (done < size) {
        const int32_t wrote = platform->file_write(file, data + done, size - done);
        if (wrote <= 0)
            return false;
        done += wrote;
    }
    return true;
}

struct PartFile {
    std::string disk;
    bool keep = false;

    ~PartFile() {
        if (keep || disk.empty())
            return;
        std::error_code error;
        fs::remove(oa::platform::path_from_utf8(disk), error);
    }
};

// Moves the part file over the player's map. The previous file is held aside
// and put back when the move fails.
bool replace_player_copy(const std::string& part_disk, const std::string& dest_disk) {
    const fs::path part = oa::platform::path_from_utf8(part_disk);
    const fs::path dest = oa::platform::path_from_utf8(dest_disk);
    fs::path backup = dest;
    backup += kBackupSuffix;
    std::error_code error;
    fs::remove(backup, error);
    error.clear();
    const bool had_dest = fs::is_regular_file(dest, error);
    if (had_dest) {
        fs::rename(dest, backup, error);
        if (error)
            return false;
    }
    error.clear();
    fs::rename(part, dest, error);
    if (error) {
        if (had_dest) {
            std::error_code restore;
            fs::rename(backup, dest, restore);
        }
        return false;
    }
    if (had_dest) {
        error.clear();
        fs::remove(backup, error);
    }
    return true;
}

bool reader_accepts(const std::string& part_relative, int32_t planets, int32_t connectors) {
    int32_t sector_id = 0;
    int32_t width = 0;
    int32_t height = 0;
    int32_t read_planets = -1;
    int32_t read_connectors = -1;
    char name[51] = {};
    RoverSectorReader* reader = rover_open_sector_map(
        part_relative.c_str(), &sector_id, name, &width, &height, &read_planets, &read_connectors
    );
    if (reader == nullptr)
        return false;
    const bool counts = read_planets == planets && read_connectors == connectors &&
                        read_planets >= 0 && read_connectors >= 0;
    bool read = false;
    if (counts) {
        std::vector<RoverSectorPlanet> planet_rows(static_cast<std::size_t>(read_planets));
        std::vector<RoverSectorConnector> connector_rows(static_cast<std::size_t>(read_connectors));
        read = rover_read_sector_planets(
            reader,
            width,
            height,
            read_planets > 0 ? planet_rows.data() : nullptr,
            read_planets,
            read_connectors > 0 ? connector_rows.data() : nullptr,
            read_connectors
        );
    }
    rover_close_sector_map(reader);
    return read;
}

void store_checked(const SectorFetchReply& reply) {
    int32_t planets = 0;
    int32_t connectors = 0;
    if (!bytes_check(reply, &planets, &connectors))
        return;
    const RoverPlatform* platform = rover_session_platform();
    if (platform == nullptr || platform->file_create == nullptr ||
        platform->file_write == nullptr || platform->file_close == nullptr)
        return;
    const std::string part_relative = g_relative + kPartSuffix;
    const std::string part_disk = rover_session_write_path(part_relative.c_str());
    const std::string dest_disk = rover_session_write_path(g_relative.c_str());
    if (part_disk.empty() || dest_disk.empty())
        return;
    RoverFile* file = platform->file_create(part_relative.c_str());
    if (file == nullptr)
        return;
    PartFile part{part_disk, false};
    const bool written = write_all(platform, file, reply.bytes, reply.size);
    platform->file_close(file);
    if (!written || !reader_accepts(part_relative, planets, connectors))
        return;
    if (replace_player_copy(part_disk, dest_disk))
        part.keep = true;
}

// Stores a Ready reply, then drops the exchange. The bytes are read before
// the drop. A phase other than Ready writes nothing.
void close_fetch() {
    if (!g_begun)
        return;
    g_begun = false;
    const bool ready =
        g_rover_state != nullptr && rover_sector_fetch_phase() == SectorFetchPhase::Ready;
    try {
        if (ready) {
            SectorFetchReply reply{};
            rover_sector_fetch_reply(&reply);
            store_checked(reply);
        }
    } catch (const std::exception&) {
    }
    try {
        rover_sector_fetch_cancel();
    } catch (const std::exception&) {
    }
}

} // namespace

void rover_session_note_clock(uint32_t tick) {
    g_clock = tick;
    g_clock_known = true;
}

bool rover_session_begin_sector_fetch(uint32_t area_id, const char* area) {
    g_begun = false;
    g_relative.clear();
    g_clock_bound = false;
    if (!area_name_ok(area))
        return false;
    const std::string relative = std::string(kMapDirectory) + area + kMapSuffix;
    const std::string have = hash_player_copy(relative);
    if (!rover_sector_fetch_begin(area_id, have.empty() ? nullptr : have.c_str()))
        return false;
    g_relative = relative;
    g_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kSectorFetchWaitMs);
    g_clock_start = g_clock;
    g_clock_bound = g_clock_known;
    g_begun = true;
    return true;
}

bool rover_session_sector_fetch_pending() {
    return g_begun;
}

bool rover_session_settle_sector_fetch() {
    if (!g_begun)
        return false;
    const bool open =
        g_rover_state != nullptr && rover_sector_fetch_phase() == SectorFetchPhase::Open;
    if (open) {
        const bool wall_open = std::chrono::steady_clock::now() < g_deadline;
        const bool clock_open =
            !g_clock_bound || clock_elapsed(g_clock, g_clock_start) < kSectorFetchWaitTicks;
        if (wall_open && clock_open)
            return false;
    }
    close_fetch();
    return true;
}

void rover_session_finish_sector_fetch(int32_t wait_ms) {
    if (!g_begun)
        return;
    const int32_t bounded = wait_ms < 0 ? 0 : wait_ms;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(bounded);
    // A direct caller (the sector test) may wait. It must not be a Rover
    // callback: this sleeps, and it does not pump.
    while (g_rover_state != nullptr && rover_sector_fetch_phase() == SectorFetchPhase::Open &&
           std::chrono::steady_clock::now() < deadline) {
        const RoverPlatform* platform = rover_platform();
        if (platform == nullptr || platform->sleep_ms == nullptr)
            break;
        platform->sleep_ms(static_cast<uint32_t>(kSleepSliceMs));
    }
    close_fetch();
}

} // namespace oa::boneyards

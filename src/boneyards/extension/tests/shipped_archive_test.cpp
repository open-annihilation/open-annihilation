// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// A made-up war map is read through the session when only the archive
// beside the program holds it. That read is the fallback the extension uses
// after the game's files miss. The player's copy is read ahead of it. A
// path the archive does not hold is still taken from the expansion folder.
// The test builds the archive itself and does not read a map from an install.

#include "shipped_archive.hpp"

#include "oa/boneyards/rover_session.hpp"
#include "oa/formats/hpi.hpp"
#include "oa/platform/files.hpp"
#include "oa/platform/system.hpp"
#include "oa/rover/interface.h"
#include "oa/rover/platform.h"
#include "oa/test/check.hpp"
#include "oa/test/scratch_directory.hpp"

#include <cstdint>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace {

namespace fs = std::filesystem;

constexpr const char* kMapPath = "Boneyards\\alpha_map.gwm";
constexpr const char* kExpansionOnlyPath = "Boneyards\\ridge_only.gwm";
constexpr const char* kArchiveName = "Ridge Reach";
constexpr const char* kPlayerName = "Kept Copy";
constexpr const char* kExpansionName = "Only Here";
constexpr int32_t kPlanetCount = 2;
constexpr int32_t kConnectorCount = 1;
constexpr int32_t kPlanetId = 11;

struct OpenedMap {
    RoverSectorReader* reader = nullptr;
    int32_t id = 0;
    char name[0x33]{};
    int32_t width = 0;
    int32_t height = 0;
    int32_t planets = 0;
    int32_t connectors = 0;
};

/// A sector of two made-up planets joined by one connector.
///
/// @param sector_name the sector's name
/// @return the map's bytes, with carriage returns kept
std::string sector_text(std::string_view sector_name) {
    std::string text;
    text += "{[Map<S:command=sector><I:id=7><S:name=";
    text += sector_name;
    text += "><I:width=800><I:height=600><I:planets=2><I:connectors=1>]}\r\n";
    text += "{[Map<S:command=planet><I:id=11><S:name=North Spire><I:x=100><I:y=120>"
            "<S:seqname=Spire_L><S:side=A><I:capitol=0><S:desc=A glass ridge>"
            "<S:ta_map=Glass Plain><S:ta_mapsource=OA><S:ta_maptype=Glass>"
            "<S:pan_gaf=spire.gaf><S:pan_seq=SpirePan>]}\r\n";
    text += "{[Map<S:command=planet><I:id=12><S:name=South Spire><I:x=400><I:y=300>"
            "<S:seqname=Shelf_M><S:side=C><I:capitol=1><S:desc=A red shelf>"
            "<S:ta_map=Red Shelf><S:ta_mapsource=OA><S:ta_maptype=Red>"
            "<S:pan_gaf=shelf.gaf><S:pan_seq=ShelfPan>]}\r\n";
    text += "{[Map<S:command=connector><I:planet1=11><I:planet2=12>]}\r\n";
    return text;
}

/// Writes `bytes` to `path`, creating its folder.
///
/// @param path the file
/// @param bytes its contents
void write_bytes(const fs::path& path, const std::vector<uint8_t>& bytes) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!bytes.empty()) {
        out.write(
            reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
        );
    }
    OA_CHECK(static_cast<bool>(out));
}

/// Puts the fixture archive where the fallback looks, and removes it after.
struct ProgramArchive {
    fs::path path;
    bool had_previous = false;
    std::vector<uint8_t> previous;

    ProgramArchive() {
        const std::string directory = oa::platform::program_directory();
        OA_CHECK(!directory.empty());
        if (directory.empty())
            return;
        path = oa::platform::path_from_utf8(directory) / "coreprime.ccx";
        OA_CHECK(path.string().find(".app/Contents/Resources") == std::string::npos);
        if (path.string().find(".app/Contents/Resources") != std::string::npos) {
            path.clear();
            return;
        }
        std::error_code error;
        if (fs::is_regular_file(path, error) && !error) {
            had_previous = true;
            std::ifstream in(path, std::ios::binary);
            previous.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
        }
        const std::string map = sector_text(kArchiveName);
        oa::HpiWriteFile entry{"boneyards/alpha_map.gwm", {map.begin(), map.end()}};
        write_bytes(path, oa::write_hpi(std::span<const oa::HpiWriteFile>(&entry, 1)));
    }

    ~ProgramArchive() {
        if (path.empty())
            return;
        std::error_code error;
        if (had_previous)
            write_bytes(path, previous);
        else
            fs::remove(path, error);
    }

    ProgramArchive(const ProgramArchive&) = delete;
    ProgramArchive& operator=(const ProgramArchive&) = delete;
};

/// The fallback the extension calls after the game's files miss.
///
/// @param context unused
/// @param path relative path
/// @param[out] out receives the bytes
/// @return true when the archive beside the program held the file
bool read_fallback(void* /*context*/, const char* path, std::vector<uint8_t>* out) {
    if (path == nullptr || out == nullptr)
        return false;
    return oa::boneyards::read_shipped_archive(path, *out);
}

/// Opens a map through the session's file layer.
///
/// @param rover Rover's table
/// @param path the map the session is asked for
/// @return the open map; its reader is null when the file could not be read
OpenedMap open_map(const RoverInterface* rover, const char* path) {
    OpenedMap opened;
    opened.reader = rover->open_sector_map(
        path,
        &opened.id,
        opened.name,
        &opened.width,
        &opened.height,
        &opened.planets,
        &opened.connectors
    );
    return opened;
}

/// Reads the fixture's two planets, then the player's copy, then an
/// expansion-only map.
///
/// @param scratch an empty directory removed by the caller
void check_shipped_map(const fs::path& scratch) {
    const fs::path player = scratch / "player";
    const fs::path expansion = scratch / "expansion";
    const fs::path install = scratch / "install";
    fs::create_directories(player);
    fs::create_directories(expansion);
    fs::create_directories(install);
    const std::string expansion_only = sector_text(kExpansionName);
    write_bytes(
        expansion / "boneyards" / "ridge_only.gwm", {expansion_only.begin(), expansion_only.end()}
    );

    const RoverInterface* rover = nullptr;
    rover_get_interface(&rover);
    OA_CHECK(rover != nullptr && rover->open_sector_map != nullptr);
    if (rover == nullptr || rover->open_sector_map == nullptr)
        return;

    oa::boneyards::rover_session_configure(install.string().c_str(), "");
    oa::boneyards::rover_session_set_player_folder(player.string().c_str());
    oa::boneyards::rover_session_set_expansion_folder(expansion.string().c_str());
    oa::boneyards::rover_session_set_virtual_files(read_fallback, nullptr);

    OpenedMap shipped = open_map(rover, kMapPath);
    OA_CHECK(shipped.reader != nullptr);
    OA_CHECK(std::strcmp(shipped.name, kArchiveName) == 0);
    OA_CHECK(shipped.planets == kPlanetCount && shipped.connectors == kConnectorCount);
    if (shipped.reader != nullptr && shipped.planets == kPlanetCount &&
        shipped.connectors == kConnectorCount) {
        static RoverSectorPlanet planets[kPlanetCount];
        static RoverSectorConnector connectors[kConnectorCount];
        std::memset(planets, 0, sizeof planets);
        OA_CHECK(rover->read_sector_planets(
            shipped.reader,
            shipped.width,
            shipped.height,
            planets,
            kPlanetCount,
            connectors,
            kConnectorCount
        ));
        OA_CHECK(planets[0].id == kPlanetId && std::strcmp(planets[0].name, "North Spire") == 0);
        OA_CHECK(planets[1].id == 12 && std::strcmp(planets[1].name, "South Spire") == 0);
    }
    rover->close_sector_map(shipped.reader);

    const std::string own_text = sector_text(kPlayerName);
    write_bytes(player / "Boneyards" / "alpha_map.gwm", {own_text.begin(), own_text.end()});
    OpenedMap own = open_map(rover, kMapPath);
    OA_CHECK(own.reader != nullptr && std::strcmp(own.name, kPlayerName) == 0);
    rover->close_sector_map(own.reader);

    OpenedMap from_expansion = open_map(rover, kExpansionOnlyPath);
    OA_CHECK(from_expansion.reader != nullptr);
    OA_CHECK(std::strcmp(from_expansion.name, kExpansionName) == 0);
    OA_CHECK(from_expansion.planets == kPlanetCount);
    rover->close_sector_map(from_expansion.reader);
}

} // namespace

int main() {
    const ProgramArchive archive;
    const fs::path scratch = oa::test::make_scratch_directory("oa-boneyards-shipped-map");
    try {
        if (!archive.path.empty())
            check_shipped_map(scratch);
    } catch (const std::exception& error) {
        std::cerr << "shipped map: " << error.what() << '\n';
        ++oa::test::failed_checks();
    }
    rover_use_platform(nullptr);
    std::error_code remove_error;
    fs::remove_all(scratch, remove_error);
    return oa::test::check_exit_status();
}

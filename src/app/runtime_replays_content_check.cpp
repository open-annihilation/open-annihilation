// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// --check-replays-content. The working folder holds replays-content.facts,
// written by tools/check_native_replays_content.py, and the recorded game
// replays-content-source.oademo.
//
// case=prepare writes the packages the fixture registry will list: an old
// map pack, a newer one, a mutator, and two releases of the made-up mod
// Ridge. Ridge 2.8 is a real mod folder with one added unit. It does not
// open the Library.
//
// case=play adds that registry, waits until its releases list is held, and
// writes the replays. One needs the old pack and the mutator. Get them and
// watch fetches both with one press, the card says so while they come, and
// the replay plays; the mutator is gone afterwards. One names Ridge 2.8.
// Get Ridge 2.8 and watch installs it, plays that replay with Ridge, and
// No Mod is chosen again afterwards, including after a start that ended
// during the replay. One names a digest the catalogue lists and the key
// answers 409; Watch then goes. One names a digest neither list holds. One
// needs a newer engine.
//
// case=offline uses a copy of the player's registry file and the catalogue
// cache from the play case, with the fixture stopped. After a failed refresh
// the replay reads "You're offline." before any press. A start that has the
// cache and has not refreshed yet offers Get them and watch; the press says
// "You're offline." within a few seconds and Watch does not come back.
//
// case=expansion runs with an expansion that is copied and turned off. Turn
// on and watch plays the replay and puts the player's set back. The next
// start plays nothing. The same holds after a start that ended during the
// replay and one that ended before the replay began. Leaving that replay
// part way says Watched to that time after the put-back, and Watch from
// starts there. Playing it to the length its description names, and leaving
// there with no end screen, keeps no place.
//
// case=beside starts with Ridge 2.9 installed and chosen. Get Ridge 2.8 and
// watch fetches 2.8 into Replay releases, beside Mods, and plays with it.
// Ridge 2.9 stays in Mods and is chosen again when the replay's end screen
// is left, and at the next start after one that was stopped during the
// replay. The copy stays. The registry is then stopped, and Watch plays
// that copy with no chip and no key request. That copy is then moved to
// Mods/ridge-2.8, as Install alongside leaves it, and Watch plays that
// folder. No snapshot is taken.
//
// case=update starts with Ridge 2.8 in Mods/ridge, its catalogue origin at
// release 1, and No Mod chosen. The row reads Watch. Update all, then
// Update, replaces that folder with 2.9. In the same run the row reads
// Get Ridge 2.8 and watch. Replay releases is not created.
//
// case=packs copies the older map pack into Maps by hand, with no origin.
// A replay of that copy plays and fetches nothing. The folder is then
// replaced with the newer catalogue release. A replay of the older release
// fetches it into Replay releases, plays that folder, and leaves the pack
// in Maps as it was.
//
// case=local writes three .tad files of the Ridge recording under
// Recordings/ridge, as 0.8 left them: one whose content note names Ridge by
// the name the player saw, one whose note names it by its id, and one with
// no note. With Ridge 2.8 in Mods and No Mod chosen, each row reads Watch
// and the background read plays each with Ridge. Watch restarts with Ridge,
// the replay plays, and the bar's own read, with the stored timeline taken
// away, plays it with Ridge too. No Mod comes back. With Ridge uninstalled,
// both noted rows say Needs Ridge and offer Get it and watch from the
// catalogue, and the one with no note reads Watch as before. One press
// fetches the catalogue's Ridge 2.9 and plays the .tad with it. Of two rows
// the background read refused, the one refused under an older engine build
// is read again and the one refused under this build is not.
//
// In case=packs, a downloaded replay in Recordings/Core Prime of the pack
// kept in Replay releases, with nothing in Maps, is watched, and the bar's
// own read plays it with that pack and stores its timeline.
//
// Get Ridge 2.8 and watch, Watch of the installed Ridge, and Turn on and
// watch, end through the replay's own end. The end screen stays until Main
// Menu. The restart that follows puts the player's choice back and opens
// the Library on Replays with that replay chosen. The start after it plays
// nothing.

#include "library_host.hpp"
#include "map_packs.hpp"
#include "oa_layer.hpp"
#include "oa_layer_check.hpp"

#include "oa/app/automation_host.hpp"
#include "oa/app/hook_call.hpp"
#include "oa/app/content/downloads.hpp"
#include "oa/app/content/service.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/mod_profile_loader.hpp"
#include "oa/app/package_install/oamap.hpp"
#include "oa/app/package_install/origin.hpp"
#include "oa/app/package_install/replay_release.hpp"
#include "oa/app/runtime.hpp"
#include "oa/netgame/stream_socket.hpp"
#include "oa/data/content_bundles/content_choice.hpp"
#include "oa/data/saves/content_record.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/base/sha256.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/map_pack/manifest.hpp"
#include "oa/data/mutators/loadout.hpp"
#include "oa/data/replays/replays.hpp"
#include "oa/formats/oademo/oademo.hpp"
#include "oa/formats/oamut/manifest.hpp"
#include "oa/formats/ota.hpp"
#include "oa/formats/png.hpp"
#include "oa/formats/tad.hpp"
#include "oa/formats/tnt.hpp"
#include "oa/formats/zip.hpp"
#include "oa/platform/files.hpp"
#include "oa/ui/display_layout.hpp"
#include "oa/ui/library/library.hpp"
#include "oa/ui/library/replays.hpp"

#include <SDL3/SDL.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <stdexcept>
#include <system_error>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app {

namespace {

namespace fs = std::filesystem;
namespace library = oa::ui::library;
namespace oademo = oa::formats::oademo;
namespace oamut = oa::formats::oamut;
namespace pack = oa::data::map_pack;
namespace ota_format = oa::formats::ota;
namespace png = oa::formats::png;
namespace sha = oa::base::sha256;
namespace tad = oa::formats::tad;
namespace tnt = oa::formats::tnt;
namespace zip = oa::formats::zip;

constexpr std::string_view kFactsName = "replays-content.facts";
constexpr std::string_view kSourceName = "replays-content-source.oademo";
constexpr std::string_view kRidgeSourceName = "replays-content-ridge.oademo";
constexpr std::string_view kRidgeFolderName = "ridge-mod";
constexpr std::string_view kPhaseName = "replays-content.phase";
constexpr std::string_view kExpansionName = "replays-content.expansion";
constexpr std::string_view kRidgePathName = "replays-content.ridge-folder";
constexpr std::string_view kKeptName = "kept.txt";
constexpr std::string_view kBesideOldName = "replays-content.beside-28";
constexpr std::string_view kBesideNewName = "replays-content.beside-29";
constexpr int kMostFrames = 600;
constexpr uint32_t kWatchedTicks = 30;
// One second in. Watched to is 0:01, and the recording (about three seconds)
// has not finished, so the card keeps the place. A few frames cannot reach it.
constexpr uint32_t kSwitchedLeftTick = 30;
constexpr std::string_view kFromTickName = "replays-content-from-tick.txt";
constexpr uint64_t kRegistryWaitMs = 45000;
constexpr uint64_t kDownloadWaitMs = 90000;
constexpr std::string_view kPackId = "ridge-reach";
constexpr std::string_view kMapStem = "ridge_reach";
constexpr std::string_view kMapTitle = "Ridge Reach";
constexpr std::string_view kMutatorId = "ridge-wind";
constexpr std::string_view kMutatorName = "Ridge Wind";
// The local .tad files case=local writes, as Recordings/ridge holds them.
constexpr std::string_view kTadNamed = "ridge/2026-10-20 0754 Named Ridge.tad";
constexpr std::string_view kTadById = "ridge/2026-10-20 0755 Ridge By Id.tad";
constexpr std::string_view kTadBare = "ridge/2026-10-20 0756 No Note.tad";
// A downloaded replay of the kept map pack, read by the bar's own pass.
constexpr std::string_view kKeptDownload = "Core Prime/Ridge Reach Download.oademo";
// How long the bar's read of a short recording may take to start and to end.
constexpr uint64_t kBarPassStartMs = 30000;
constexpr uint64_t kBarPassEndMs = 120000;

void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("replays content check: " + std::string(what));
}

/// Asks the loopback registry the facts name to stop answering.
///
/// The descriptor is `http://127.0.0.1:<port>/...`. One GET /stop is enough:
/// later requests are closed, so the next start finds the registry away.
///
/// @param descriptor the facts' registry descriptor
void ask_registry_stop(std::string_view descriptor) {
    constexpr std::string_view prefix = "http://127.0.0.1:";
    require(descriptor.starts_with(prefix), "the registry address is not the loopback");
    const std::size_t slash = descriptor.find('/', prefix.size());
    require(slash != std::string_view::npos, "the registry address has no path");
    int port = 0;
    for (std::size_t index = prefix.size(); index < slash; ++index) {
        const char digit = descriptor[index];
        require(digit >= '0' && digit <= '9', "the registry port is not a number");
        port = port * 10 + (digit - '0');
    }
    require(port > 0 && port <= 65535, "the registry port is out of range");
    oa::netgame::sock::Address address{};
    address.ip[0] = 127;
    address.ip[3] = 1;
    address.port = static_cast<uint16_t>(port);
    char connect_error[128] = {};
    intptr_t stream =
        oa::netgame::sock::stream_connect(address, connect_error, sizeof connect_error);
    require(stream != oa::netgame::sock::invalid_socket, "the registry could not be asked to stop");
    require(
        oa::netgame::sock::stream_wait_open(stream, 2000) == oa::netgame::sock::StreamOpen::open,
        "the registry did not accept the stop"
    );
    const std::string request =
        "GET /stop HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
    std::size_t sent = 0;
    const uint64_t began = SDL_GetTicks();
    while (sent < request.size() && SDL_GetTicks() - began < 2000) {
        const std::ptrdiff_t wrote = oa::netgame::sock::stream_write(
            stream, reinterpret_cast<const uint8_t*>(request.data()) + sent, request.size() - sent
        );
        if (wrote < 0)
            break;
        sent += static_cast<std::size_t>(wrote);
        if (wrote == 0)
            SDL_Delay(1);
    }
    oa::netgame::sock::stream_finish(stream);
    uint8_t got[64];
    const uint64_t read_began = SDL_GetTicks();
    while (SDL_GetTicks() - read_began < 2000) {
        const std::ptrdiff_t read = oa::netgame::sock::stream_read(stream, got, sizeof got);
        if (read != 0)
            break;
        SDL_Delay(1);
    }
    oa::netgame::sock::stream_close(&stream);
    require(sent == request.size(), "the stop was not sent");
}

fs::path step_snapshot(const fs::path& snapshot, std::string_view step) {
    fs::path file = snapshot.parent_path() / snapshot.stem();
    file += '-' + std::string(step) + ".ppm";
    return file;
}

std::vector<uint8_t> read_bytes(const fs::path& file) {
    std::ifstream in(file, std::ios::binary);
    if (!in)
        return {};
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

void write_bytes(const fs::path& file, std::span<const uint8_t> bytes) {
    std::error_code made;
    fs::create_directories(file.parent_path(), made);
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(
        reinterpret_cast<const char*>(bytes.data()), static_cast<std::streamsize>(bytes.size())
    );
    require(static_cast<bool>(out), "cannot write " + platform::path_to_utf8(file));
}

void write_text(const fs::path& file, std::string_view text) {
    write_bytes(file, {reinterpret_cast<const uint8_t*>(text.data()), text.size()});
}

/// The facts file. A line is `key=value`. A blank line is skipped.
std::map<std::string, std::string> read_facts() {
    const fs::path file = fs::current_path() / std::string(kFactsName);
    std::ifstream in(file);
    require(static_cast<bool>(in), std::string(kFactsName) + " is not in the working folder");
    std::map<std::string, std::string> facts;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || line[0] == '#')
            continue;
        const std::size_t split = line.find('=');
        require(split != std::string::npos, "a facts line has no equals");
        facts[line.substr(0, split)] = line.substr(split + 1);
    }
    return facts;
}

std::string fact(const std::map<std::string, std::string>& facts, std::string_view key) {
    const auto found = facts.find(std::string(key));
    require(
        found != facts.end() && !found->second.empty(), std::string(key) + " is not in the facts"
    );
    return found->second;
}

std::vector<uint8_t> preview_png(uint8_t colour) {
    png::Header header;
    header.width = 8;
    header.height = 8;
    header.bit_depth = 8;
    header.color_type = png::ColorType::rgb;
    header.interlace = png::Interlace::none;
    std::vector<uint8_t> rows(static_cast<std::size_t>(header.width * header.height * 3), colour);
    png::Image image;
    image.header = header;
    image.rows = std::span<const uint8_t>(rows);
    std::vector<uint8_t> bytes;
    require(png::write_stored(image, &bytes), "the preview was not written");
    return bytes;
}

/// The pack's manifest. The terrain is the map the recording played, so a
/// replay of the pack loads the same ground. `size` is that terrain's size.
pack::Manifest pack_manifest(std::string size) {
    pack::Manifest manifest;
    manifest.format = pack::format_version;
    manifest.id = std::string(kPackId);
    manifest.name = std::string(kMapTitle);
    manifest.version = "1.0";
    manifest.author.name = "Ridge";
    manifest.packaging = {1, "2026-11-01", "oa-tool"};
    manifest.requires_base = "ta-3.1c";
    pack::MapEntry map;
    map.stem = std::string(kMapStem);
    map.title = std::string(kMapTitle);
    map.players = 2;
    map.size = std::move(size);
    map.preview = "previews/ridge_reach.png";
    map.files = {"maps/ridge_reach.ota", "maps/ridge_reach.tnt"};
    manifest.maps.push_back(std::move(map));
    return manifest;
}

void write_pack(
    const fs::path& file,
    uint8_t colour,
    const std::vector<uint8_t>& ota,
    const std::vector<uint8_t>& tnt_bytes,
    const std::string& size
) {
    const std::string yaml = pack::write_manifest(pack_manifest(size));
    const std::vector<uint8_t> preview = preview_png(colour);
    const std::vector<uint8_t> yaml_bytes(yaml.begin(), yaml.end());
    const zip::NewEntry entries[] = {
        {"oamap.yaml", yaml_bytes},
        {"maps/ridge_reach.ota", ota},
        {"maps/ridge_reach.tnt", tnt_bytes},
        {"previews/ridge_reach.png", preview},
    };
    std::vector<uint8_t> archive;
    zip::ZipError error;
    require(zip::write_archive(entries, archive, error), "the map pack was not packed");
    write_bytes(file, archive);
}

void write_mutator(const fs::path& file) {
    oamut::Manifest manifest;
    manifest.format = oamut::format_version;
    manifest.id = std::string(kMutatorId);
    manifest.name = std::string(kMutatorName);
    manifest.version.major = 1;
    manifest.version.parts = 1;
    manifest.author.name = "Ridge";
    manifest.tags = {"economy"};
    manifest.works_with.push_back(oamut::Condition{"*", {}, {}});
    manifest.layers = {oamut::Layer::files};
    const std::string yaml = oamut::write_manifest(manifest);
    const std::string script = "function mutate(ctx)\n  return {}\nend\n";
    const std::vector<uint8_t> yaml_bytes(yaml.begin(), yaml.end());
    const std::vector<uint8_t> script_bytes(script.begin(), script.end());
    const zip::NewEntry entries[] = {
        {"oamut.yaml", yaml_bytes},
        {"mutator.lua", script_bytes},
    };
    std::vector<uint8_t> archive;
    zip::ZipError error;
    require(zip::write_archive(entries, archive, error), "the mutator was not packed");
    write_bytes(file, archive);
}

constexpr std::string_view kRidgeUnit = "[UNITINFO]\n"
                                        "{\n"
                                        "UnitName=RGNEW;\n"
                                        "MaxDamage=1000;\n"
                                        "}\n";

// The same empty model ridge-add-unit ships. The unit table keeps a unit
// whose FBI loaded with its model.
constexpr uint8_t kRidgeModel[] = {
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0xff,
    0xff, 0xff, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/// A profile the install accepts. It changes no rule. The added unit is a file.
std::string ridge_profile(std::string_view version) {
    return "oamod: 1\n"
           "id: ridge\n"
           "name: Ridge\n"
           "version: \"" +
           std::string(version) +
           "\"\n"
           "description: A made-up mod the replays check plays.\n"
           "requires: {base: ta-3.1c, catalogue: 1}\n"
           "author: {name: unknown}\n"
           "packaging: {revision: 1, date: 2026-11-01, packager: Open Annihilation}\n";
}

/// Packs Ridge and, for the release that is recorded, leaves the folder --mod-dir uses.
void write_mod(const fs::path& file, std::string_view version, const fs::path& folder) {
    const std::string profile = ridge_profile(version);
    const std::vector<uint8_t> model(std::begin(kRidgeModel), std::end(kRidgeModel));
    if (!folder.empty()) {
        write_text(folder / "oamod.yaml", profile);
        write_text(folder / "units" / "RGNEW.fbi", kRidgeUnit);
        write_bytes(folder / "objects3d" / "RGNEW.3DO", model);
    }
    const std::vector<uint8_t> yaml(profile.begin(), profile.end());
    const std::vector<uint8_t> unit(kRidgeUnit.begin(), kRidgeUnit.end());
    const zip::NewEntry entries[] = {
        {"oamod.yaml", yaml},
        {"units/RGNEW.fbi", unit},
        {"objects3d/RGNEW.3DO", model},
    };
    std::vector<uint8_t> archive;
    zip::ZipError error;
    require(zip::write_archive(entries, archive, error), "the mod was not packed");
    write_bytes(file, archive);
}

/// The pack size of an OTA size such as `16 x 16`. Empty when it is not
/// `<width>x<height>` with each side from 1 to the pack's limit.
std::string pack_size_text(std::string_view text) {
    std::string size;
    size.reserve(text.size());
    for (const char character : text) {
        if (character != ' ')
            size.push_back(character);
    }
    const std::size_t split = size.find('x');
    if (split == std::string::npos || size.find('x', split + 1) != std::string::npos)
        return {};
    const auto side = [](std::string_view digits) {
        if (digits.empty() || digits.size() > 3 || (digits.size() > 1 && digits.front() == '0'))
            return false;
        int value = 0;
        for (const char character : digits) {
            if (character < '0' || character > '9')
                return false;
            value = value * 10 + (character - '0');
        }
        return value >= 1 && value <= pack::most_map_side;
    };
    if (!side(std::string_view(size).substr(0, split)) ||
        !side(std::string_view(size).substr(split + 1)))
        return {};
    return size;
}

void write_packages(const std::vector<uint8_t>& ota, const std::vector<uint8_t>& tnt_bytes) {
    const tnt::ParseResult parsed = tnt::parse(tnt_bytes);
    require(parsed.ok(), "the recorded map's terrain does not parse");
    const std::string_view ota_text(reinterpret_cast<const char*>(ota.data()), ota.size());
    const ota_format::ParseResult settings = ota_format::parse(ota_text);
    require(settings.ok(), "the recorded map's settings do not parse");
    const std::string size = pack_size_text(settings.metadata->map_size);
    require(!size.empty(), "the recorded map does not fit a pack");
    const fs::path folder = fs::current_path();
    write_pack(folder / "ridge-reach.oamap", 40, ota, tnt_bytes, size);
    write_pack(folder / "ridge-reach-new.oamap", 200, ota, tnt_bytes, size);
    write_mutator(folder / "ridge-wind.oamut");
    write_mod(folder / "ridge-28.oamod", "2.8", folder / std::string(kRidgeFolderName));
    write_mod(folder / "ridge-29.oamod", "2.9", {});
    std::cout << "replays content: packages written\n";
}

/// The recording plus a mutators sector for ridge-wind at `sha_hex`.
std::vector<uint8_t>
recording_with_mutator(const std::vector<uint8_t>& recording, std::string_view sha_hex) {
    const tad::ParseResult parsed = tad::parse(recording);
    require(parsed.ok(), "the recorded game does not parse");
    tad::Demo demo = *parsed.demo;
    std::vector<tad::Sector> sectors;
    sectors.reserve(demo.sectors.size() + 1);
    for (const tad::Sector& sector : demo.sectors) {
        if (sector.type == static_cast<uint32_t>(tad::SectorType::mutators))
            continue;
        sectors.push_back(sector);
    }
    const std::optional<sha::Digest> digest = sha::parse_hex(sha_hex);
    require(digest.has_value(), "the mutator's digest is not hex");
    oa::data::mutators::Loadout loadout;
    oa::data::mutators::LoadoutEntry entry;
    entry.id = std::string(kMutatorId);
    entry.version = "1";
    entry.package = *digest;
    entry.clashes = oamut::ClashMode::replace;
    loadout.entries.push_back(std::move(entry));
    const std::string text = oa::data::mutators::canonical_loadout(loadout);
    const std::vector<uint8_t> sector_bytes(text.begin(), text.end());
    tad::Sector added;
    added.type = static_cast<uint32_t>(tad::SectorType::mutators);
    added.data = std::span<const uint8_t>(sector_bytes);
    sectors.push_back(added);
    demo.sectors = std::move(sectors);
    const tad::WriteResult written = tad::write(demo);
    require(written.ok(), "the recording with its mutator was not written");
    return written.bytes;
}

void store_replay(const fs::path& file, const oademo::Replay& replay) {
    oademo::Replay stored = replay;
    oademo::set_recording_identity(stored);
    const oademo::WriteResult written = oademo::write(stored);
    require(written.ok(), written.error.empty() ? "a replay was not written" : written.error);
    write_bytes(file, written.bytes);
}

oademo::Need
package_need(std::string kind, std::string id, std::string version, std::string sha_hex) {
    oademo::Need need;
    need.kind = std::move(kind);
    need.id = std::move(id);
    need.version = std::move(version);
    need.sha256 = std::move(sha_hex);
    return need;
}

std::string read_phase(const fs::path& user) {
    std::ifstream in(user / std::string(kPhaseName));
    std::string text;
    std::getline(in, text);
    if (!text.empty() && text.back() == '\r')
        text.pop_back();
    return text;
}

void write_phase(const fs::path& user, std::string_view phase) {
    write_text(user / std::string(kPhaseName), std::string(phase) + "\n");
}

/// The first line of a file the check wrote, without a trailing CR.
///
/// @param file the file
/// @return the line; empty when it cannot be read
std::string read_line(const fs::path& file) {
    std::ifstream in(file);
    std::string text;
    std::getline(in, text);
    if (!text.empty() && text.back() == '\r')
        text.pop_back();
    return text;
}

/// Reports whether an installed folder's profile names `version`.
///
/// @param folder the mod folder
/// @param version the version text
/// @return true when oamod.yaml names that version
bool folder_names_version(const fs::path& folder, std::string_view version) {
    std::ifstream in(folder / "oamod.yaml");
    const std::string text{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    return text.find("version: \"" + std::string(version) + "\"") != std::string::npos;
}

bool lists_id(const std::vector<std::string>& ids, std::string_view id) {
    for (const std::string& one : ids)
        if (one == id)
            return true;
    return false;
}

/// A path under `root` for a zip entry name.
fs::path under(const fs::path& root, std::string_view relative) {
    fs::path at = root;
    std::string part;
    const auto push = [&] {
        if (part.empty())
            return;
        at /= part;
        part.clear();
    };
    for (const char character : relative) {
        if (character == '/')
            push();
        else
            part.push_back(character);
    }
    push();
    return at;
}

/// Replaces `folder` with the files of a map pack archive.
void extract_pack(const fs::path& archive_file, const fs::path& folder) {
    const std::vector<uint8_t> bytes = read_bytes(archive_file);
    require(
        !bytes.empty(),
        platform::path_to_utf8(archive_file.filename()) + " is not in the working folder"
    );
    zip::CentralDirectory directory;
    zip::ZipError error;
    require(zip::read_directory(bytes, directory, error), "the map pack was not read");
    std::error_code made;
    fs::remove_all(folder, made);
    fs::create_directories(folder, made);
    require(!made, "the map pack folder was not made");
    for (const zip::Entry& entry : directory.entries) {
        if (entry.directory)
            continue;
        require(zip::name_is_safe(entry.name), "a map pack entry is not safe");
        std::vector<uint8_t> data;
        require(zip::read_entry(bytes, entry, data, error), "a map pack entry was not read");
        const fs::path dest = under(folder, entry.name);
        fs::create_directories(dest.parent_path(), made);
        require(!made, "a map pack folder was not made");
        write_bytes(dest, data);
    }
}

/// The recording with its map set to the pack map playback mounts.
std::vector<uint8_t> recording_on_pack(const std::vector<uint8_t>& recording) {
    const tad::ParseResult parsed = tad::parse(recording);
    require(parsed.ok(), "the recorded game does not parse");
    tad::Demo demo = *parsed.demo;
    demo.map_name = std::string(kMapStem) + "@" + std::string(kPackId);
    const tad::WriteResult written = tad::write(demo);
    require(written.ok(), "the recording was not written onto the pack map");
    return written.bytes;
}

/// One replay of the pack map, named by `sha`.
void store_pack_replay(const fs::path& file, const oademo::Replay& source, std::string sha) {
    oademo::Replay replay = source;
    replay.recording = recording_on_pack(replay.recording);
    oademo::Need need = package_need("oamap", std::string(kPackId), "1.0", std::move(sha));
    oademo::MapNeed map;
    map.name = std::string(kMapTitle);
    map.sha256 = std::string(64, 'c');
    need.maps.push_back(std::move(map));
    replay.manifest.needs = {std::move(need)};
    store_replay(file, replay);
}

/// SHA-256 of the files a replace would change, so the check can see they stayed.
std::string pack_fingerprint(const fs::path& folder) {
    std::string joined;
    for (const std::string_view name :
         {"oamap.yaml",
          ".oa-origin.yaml",
          "previews/ridge_reach.png",
          "maps/ridge_reach.ota",
          "maps/ridge_reach.tnt"}) {
        const std::vector<uint8_t> bytes = read_bytes(under(folder, name));
        joined.append(reinterpret_cast<const char*>(bytes.data()), bytes.size());
        joined.push_back('\n');
    }
    const std::vector<uint8_t> whole(joined.begin(), joined.end());
    const auto hex = sha::to_hex(sha::digest_of(whole));
    return std::string(hex.data(), hex.size());
}

/// Writes the replays under Recordings/default.
void write_replays(
    const fs::path& recordings,
    const oademo::Replay& source,
    const std::map<std::string, std::string>& facts
) {
    const fs::path folder = recordings / "default";
    const std::string pack_sha = fact(facts, "pack_sha");
    const std::string mutator_sha = fact(facts, "mutator_sha");
    const std::string ridge_sha = fact(facts, "ridge_sha");
    const std::string unlisted_sha = fact(facts, "unlisted_sha");
    const std::string gone_sha = fact(facts, "gone_sha");

    oademo::Replay both = source;
    both.recording = recording_with_mutator(both.recording, mutator_sha);
    oademo::Need map_need = package_need("oamap", std::string(kPackId), "1.0", pack_sha);
    oademo::MapNeed map;
    map.name = std::string(kMapTitle);
    map.sha256 = std::string(64, 'a');
    map_need.maps.push_back(std::move(map));
    both.manifest.needs = {
        std::move(map_need), package_need("oamut", std::string(kMutatorId), "1", mutator_sha)
    };
    store_replay(folder / "both.oademo", both);

    const std::vector<uint8_t> ridge_bytes =
        read_bytes(fs::current_path() / std::string(kRidgeSourceName));
    require(!ridge_bytes.empty(), std::string(kRidgeSourceName) + " is not in the working folder");
    const oademo::ReadResult ridge_read = oademo::read(ridge_bytes);
    require(ridge_read.ok(), std::string(kRidgeSourceName) + " does not read: " + ridge_read.error);
    oademo::Replay ridge = *ridge_read.replay;
    ridge.meta.content.mod = oademo::ModRef{};
    ridge.meta.content.mod->id = "ridge";
    ridge.meta.content.mod->name = "Ridge";
    ridge.meta.content.mod->version = "2.8";
    ridge.meta.content.mod->revision = 1;
    oademo::Need ridge_need = package_need("oamod", "ridge", "2.8", ridge_sha);
    ridge_need.revision = 1;
    ridge.manifest.needs = {std::move(ridge_need)};
    store_replay(folder / "ridge.oademo", ridge);

    oademo::Replay unlisted = ridge;
    unlisted.manifest.needs[0].sha256 = unlisted_sha;
    store_replay(folder / "unlisted.oademo", unlisted);

    oademo::Replay gone = source;
    oademo::Need gone_need = package_need("oamap", "ridge-gone", "1.0", gone_sha);
    oademo::MapNeed gone_map;
    gone_map.name = "Ridge Gone";
    gone_map.sha256 = std::string(64, 'b');
    gone_need.maps.push_back(std::move(gone_map));
    gone.manifest.needs = {std::move(gone_need)};
    store_replay(folder / "gone.oademo", gone);

    oademo::Replay engine = source;
    engine.manifest.needs.clear();
    engine.manifest.engine.needs = ">= 99.0.0";
    store_replay(folder / "engine.oademo", engine);
    std::cout << "replays content: replays written\n";
}

/// A replay of the pack map recorded with Ridge, named by both packages.
void store_pack_and_ridge(const fs::path& file, const oademo::Replay& ridge, std::string pack_sha) {
    oademo::Replay replay = ridge;
    replay.recording = recording_on_pack(replay.recording);
    oademo::Need map_need = package_need("oamap", std::string(kPackId), "1.0", std::move(pack_sha));
    oademo::MapNeed map;
    map.name = std::string(kMapTitle);
    map.sha256 = std::string(64, 'c');
    map_need.maps.push_back(std::move(map));
    replay.manifest.needs.push_back(std::move(map_need));
    store_replay(file, replay);
}

/// Writes the Ridge recording as a .tad under Recordings, with the content
/// note 0.8 wrote beside it: the base game, the expansions, and the mod as
/// `mod_line` names it. An empty `mod_line` writes no note.
void write_local_tad(
    const fs::path& recordings,
    std::string_view path,
    const std::vector<uint8_t>& tad_bytes,
    const oa::data::content_bundles::ContentChoice& choice,
    std::string_view mod_line
) {
    const fs::path file = under(recordings, path);
    write_bytes(file, tad_bytes);
    if (mod_line.empty())
        return;
    std::string note = "base-game: " + choice.base_game + "\nexpansions: ";
    for (std::size_t index = 0; index < choice.expansions.size(); ++index) {
        if (index != 0)
            note += ',';
        note += choice.expansions[index];
    }
    note += "\nmod: " + std::string(mod_line) + "\n";
    fs::path note_file = file;
    note_file += std::string(oa::data::saves::content_note_suffix);
    write_text(note_file, note);
}

/// Takes one replay's stored timeline out of the index on disk, so the bar
/// that watches it starts its own read.
void forget_timeline(const fs::path& recordings, std::string_view path) {
    oa::data::replays::Index index = oa::data::replays::load_index(recordings);
    bool found = false;
    for (oa::data::replays::Replay& replay : index.replays) {
        if (replay.path != path)
            continue;
        replay.timeline.reset();
        replay.read_stamp.reset();
        found = true;
    }
    require(found, std::string(path) + " is not in the index");
    require(oa::data::replays::save_index(recordings, index), "the index was not saved");
}

} // namespace

void Runtime::check_replays_content() {
    require(sdl_.window != nullptr, "the check needs the SDL presenter");
    require(!user_folder_.empty(), "the run has no folder of the player's own");
    const std::map<std::string, std::string> facts = read_facts();
    const std::string which = fact(facts, "case");
    if (which == "prepare") {
        const std::vector<uint8_t> recorded =
            read_bytes(fs::current_path() / std::string(kSourceName));
        require(!recorded.empty(), std::string(kSourceName) + " is not in the working folder");
        const oademo::ReadResult recorded_read = oademo::read(recorded);
        require(
            recorded_read.ok(), std::string(kSourceName) + " does not read: " + recorded_read.error
        );
        const tad::ParseResult played = tad::parse(recorded_read.replay->recording);
        require(played.ok(), "the recorded game does not parse");
        const std::optional<std::vector<uint8_t>> ota =
            read("maps/" + played.demo->map_name + ".ota");
        const std::optional<std::vector<uint8_t>> tnt_bytes =
            read("maps/" + played.demo->map_name + ".tnt");
        require(
            ota.has_value() && tnt_bytes.has_value() && !ota->empty() && !tnt_bytes->empty(),
            "the recorded map is not installed"
        );
        write_packages(*ota, *tnt_bytes);
        return;
    }
    require(
        which == "play" || which == "offline" || which == "expansion" || which == "beside" ||
            which == "update" || which == "packs" || which == "local",
        "the facts name no case this check runs"
    );

    const std::vector<uint8_t> source_bytes =
        read_bytes(fs::current_path() / std::string(kSourceName));
    require(!source_bytes.empty(), std::string(kSourceName) + " is not in the working folder");
    const oademo::ReadResult source_read = oademo::read(source_bytes);
    require(source_read.ok(), std::string(kSourceName) + " does not read: " + source_read.error);

    bool running = true;
    LibraryHost& host = library_host();
    const auto frames = [&](int count) {
        for (int frame = 0; frame < count; ++frame) {
            run_frame(running);
            require(running, "a frame ended the run");
        }
    };
    const auto until = [&](uint64_t budget_ms, const auto& done, std::string_view what) {
        const uint64_t began = SDL_GetTicks();
        while (SDL_GetTicks() - began < budget_ms) {
            run_frame(running);
            require(running, "a frame ended the run");
            if (done())
                return;
        }
        throw std::runtime_error("replays content check: " + std::string(what));
    };
    const auto named = [this](std::string_view name) -> std::optional<AutomationControl> {
        std::vector<AutomationControl> controls;
        oa_layer().automation_controls(controls);
        for (const AutomationControl& control : controls)
            if (control.name == name && control.visible && control.window_width > 0 &&
                control.window_height > 0)
                return control;
        return std::nullopt;
    };
    const auto click_named = [&](std::string_view name) {
        const std::optional<AutomationControl> control = named(name);
        require(control.has_value(), std::string(name) + " is not shown");
        require(control->enabled, std::string(name) + " is not enabled");
        const oa::ui::display_layout::Point pixel{
            control->window_x + control->window_width / 2,
            control->window_y + control->window_height / 2
        };
        for (const SDL_EventType type :
             {SDL_EVENT_MOUSE_MOTION, SDL_EVENT_MOUSE_BUTTON_DOWN, SDL_EVENT_MOUSE_BUTTON_UP}) {
            SDL_Event event = window_pointer_event(
                sdl_.window,
                sdl_.renderer,
                type,
                pixel,
                type == SDL_EVENT_MOUSE_MOTION ? 0 : SDL_BUTTON_LEFT
            );
            bool going = true;
            dispatch_event(event, going);
            require(going, "a click ended the run");
        }
        run_frame(running);
        require(running, "a frame ended the run");
    };
    const auto snapshot = [this](const std::string& step) {
        if (options_.snapshot.empty())
            return;
        oa_layer().read_whole_window(true);
        renderer::Surface presented;
        capture_frame_ = &presented;
        render();
        capture_frame_ = nullptr;
        oa_layer().read_whole_window(false);
        write_ppm(step_snapshot(options_.snapshot, step), presented);
    };
    const auto row_of = [&](std::string_view path) {
        const std::optional<std::size_t> place = library::replay_place(host.model, path);
        require(place.has_value(), std::string(path) + " is not listed");
        host.state.reveal_row = static_cast<int32_t>(*place);
        ++host.model_revision;
        frames(1);
        return "oa.library.row.replay." + std::to_string(*place + 1);
    };
    const auto show = [&](std::string_view path) {
        click_named(row_of(path));
        const library::ReplayRow* row = library::find_replay(host.model, path);
        require(row != nullptr, std::string(path) + " has no row");
        const std::string chip = library::replay_need_chip(host.model, *row);
        const std::string note = library::replay_note(host.model, *row);
        std::string button;
        if (const std::optional<AutomationControl> watch = named("oa.library.action.watch"))
            button = watch->text;
        std::cout << "replays content: " << path << " chip \"" << chip << "\" note \"" << note
                  << "\" button \"" << button << "\"\n";
        return std::pair{chip, note};
    };
    // A row Watch would have to fetch is left out of the pass. both.oademo
    // needs ridge-wind until Get has installed it.
    const auto both_not_read = [&] {
        const library::ReplayRow* row = library::find_replay(host.model, "default/both.oademo");
        require(row != nullptr, "both.oademo is not listed");
        require(!row->indexing, "both.oademo says Reading while ridge-wind is missing");
        require(
            !row->replay.timeline.has_value(), "both.oademo was read while ridge-wind was missing"
        );
        require(
            !row->replay.read_stamp.has_value(),
            "both.oademo was stored unread for good while ridge-wind was missing"
        );
        require(
            library::replay_reading(host.model, *row).find("Reading") == std::string::npos,
            "both.oademo's row says Reading while ridge-wind is missing"
        );
        require(!host.index_pass_running(), "the index pass ran while a mutator was missing");
    };
    const auto open_replays = [&] {
        load(Screen::main_menu);
        require(screen_ == Screen::main_menu, "the main menu did not open");
        require(
            SDL_SetWindowSize(sdl_.window, 1280, 720) && SDL_SyncWindow(sdl_.window),
            "the window did not take 1280x720"
        );
        apply_output_mode();
        frames(2);
        open_library(library::Tab::replays);
        for (int frame = 0; frame < kMostFrames && host.replays_reading(); ++frame) {
            run_frame(running);
            require(running, "a frame ended the run");
        }
        frames(2);
        require(!host.replays_reading(), "the Recordings folder was not read");
    };
    // The player's own start: start_scene borrows the pending replay for this
    // start and drops it. The replay then runs to its own end. The end screen
    // stays until Main Menu, and the frame after that puts the choice back.
    const auto play_through = [&] {
        require(
            call_hook_or_raise<&Extension::start_scene>(extension_, *this),
            "the replay did not start"
        );
        until(
            kDownloadWaitMs,
            [&] { return screen_ == Screen::match && match_ != nullptr; },
            "the replay did not start"
        );
        // The recording ends when its players leave, with no result. A
        // player's replay stops on that last frame. Granting ticks runs the
        // match on past it to the end screen, whose put-back this tests.
        constexpr int kReplayTickBudget = 8000;
        int replay_ticks = 0;
        while (screen_ != Screen::campaign_end && replay_ticks < kReplayTickBudget) {
            if (match_ != nullptr && !match_finished_) {
                const GrantedTicks grant = run_granted_ticks(30);
                replay_ticks += grant.ran > 0 ? static_cast<int>(grant.ran) : 1;
            } else {
                ++replay_ticks;
            }
            run_frame(running);
            require(running, "a frame ended the run");
        }
        require(screen_ == Screen::campaign_end, "the replay did not reach its end screen");
        require(!soft_restart_requested(), "the end screen did not stay until Main Menu");
        frames(4);
        require(
            screen_ == Screen::campaign_end && !soft_restart_requested(),
            "the end screen did not stay until Main Menu"
        );
        require(step_end_screen_to_panel().panel, "the end screen reached no panel");
        frames(2);
        require(
            screen_ == Screen::campaign_end && !soft_restart_requested(),
            "the end screen did not stay until Main Menu"
        );
        const auto* end_menu = widget("MainMenu");
        require(end_menu != nullptr && end_menu->common.active != 0, "Main Menu is not shown");
        click_end_panel(static_cast<std::size_t>(end_menu - resources_.layout.gadgets.data()));
        require(screen_ == Screen::main_menu, "Main Menu did not leave the end screen");
        std::cout << "replays content: end screen stayed until Main Menu\n";
    };
    // The restart after Main Menu opens the Library itself, on this replay.
    const auto library_on_replay = [&](std::string_view path) {
        until(
            kDownloadWaitMs,
            [&] {
                return oa_layer().find(library_screen_name) != nullptr &&
                       host.model.tab == library::Tab::replays && host.model.replays.selected &&
                       *host.model.replays.selected == path;
            },
            "the Library did not open on the replay"
        );
        require(match_ == nullptr && screen_ != Screen::match, "the Library opened over a match");
        std::cout << "replays content: library open on " << path << '\n';
    };
    const auto next_start_idle = [&] {
        require(
            !call_hook_or_raise<&Extension::start_scene>(extension_, *this),
            "the next start played the replay again"
        );
        require(match_ == nullptr, "the next start opened a match");
        require(screen_ != Screen::match, "the next start opened a match");
        std::cout << "replays content: the next start played nothing\n";
    };
    const auto state_key_gone = [&] {
        return preference_values_.find(std::string(replay_expansion_state_preference)) ==
               preference_values_.end();
    };

    if (which == "expansion") {
        namespace bundles = oa::data::content_bundles;
        const fs::path expansion_file = user_folder_ / std::string(kExpansionName);
        const std::string phase = read_phase(user_folder_);
        const auto read_id = [&] {
            std::ifstream in(expansion_file);
            std::string id;
            std::getline(in, id);
            if (!id.empty() && id.back() == '\r')
                id.pop_back();
            require(!id.empty(), "the expansion was not chosen");
            return id;
        };
        const auto turn = [&](std::string_view id, bool on) {
            const std::vector<std::string>& copied = kept_content_bundles();
            std::vector<std::string_view> present;
            present.reserve(copied.size());
            for (const std::string& one : copied)
                present.push_back(one);
            return bundles::turn_content_expansion(
                content_choice(), id, on, present, false, false, {}
            );
        };
        const auto expansion_off = [&](std::string_view id) {
            return !lists_id(content_choice().expansions, id);
        };
        const auto arm_stop = [&](std::string_view id,
                                  std::string_view state,
                                  std::string_view next) {
            const std::string player = bundles::expansions_text(content_choice().expansions);
            const bundles::ContentChangeResult on = turn(id, true);
            require(
                on.refusal == bundles::ContentRefusal::none && on.applied,
                "the stopped replay's expansion did not turn on"
            );
            remember_content_choice(preference_values_, on.choice);
            preference_values_[std::string(replay_expansion_preference)] = player;
            preference_values_[std::string(replay_expansion_state_preference)] = std::string(state);
            preference_values_.erase(std::string(oa::data::saves::pending_demo_key));
            preference_values_.erase(std::string(oa::data::saves::replay_map_pack_key));
            preference_values_.erase(std::string(replay_mod_marker_preference));
            preference_values_.erase(std::string(replay_mod_preference));
            preferences_dirty_ = true;
            flush_preferences();
            write_phase(user_folder_, next);
            request_soft_restart();
        };

        if (phase.empty()) {
            std::string chosen;
            for (const char* id : {"music", "cavedog-tilesets"}) {
                if (lists_id(kept_content_bundles(), id)) {
                    chosen = id;
                    break;
                }
            }
            require(
                !chosen.empty(), "no copied expansion can be turned off without changing units"
            );
            write_text(expansion_file, chosen + "\n");
            require(
                lists_id(content_choice().expansions, chosen), chosen + " is copied and not on"
            );
            const bundles::ContentChangeResult step = turn(chosen, false);
            require(
                step.refusal == bundles::ContentRefusal::none, "the expansion was not turned off"
            );
            require(step.applied, "the expansion was already off");
            require(store_content_change(preference_values_, step), "the choice was not stored");
            preferences_dirty_ = true;
            flush_preferences();
            write_phase(user_folder_, "off");
            request_soft_restart();
            std::cout << "replays content: " << chosen << " turned off\n";
            return;
        }

        const std::string expansion_id = read_id();
        const std::string expected_chip =
            "Needs " + oa::data::saves::bundle_english_name(expansion_id);
        if (phase == "off") {
            require(expansion_off(expansion_id), expansion_id + " stayed on");
            const fs::path recordings = user_folder_ / std::string(recordings_folder_name);
            {
                std::error_code removed;
                fs::remove_all(recordings, removed);
            }
            oademo::Replay replay = *source_read.replay;
            oademo::Need need;
            need.kind = "expansion";
            need.id = expansion_id;
            replay.manifest.needs = {std::move(need)};
            replay.meta.content.expansions = {expansion_id};
            store_replay(recordings / "default" / "expansion.oademo", replay);
            open_replays();
            const auto [got_chip, note] = show("default/expansion.oademo");
            require(got_chip == expected_chip, "the expansion chip is \"" + got_chip + "\"");
            static_cast<void>(note);
            const std::optional<AutomationControl> turn_on = named("oa.library.action.watch");
            require(
                turn_on && turn_on->text == "Turn on and watch",
                "the button is not Turn on and watch"
            );
            write_phase(user_folder_, "play");
            click_named("oa.library.action.watch");
            until(
                kDownloadWaitMs,
                [&] { return soft_restart_requested(); },
                "Turn on and watch did not restart"
            );
            return;
        }
        if (phase == "play") {
            require(
                lists_id(content_choice().expansions, expansion_id),
                "the expansion is not on for the replay"
            );
            write_phase(user_folder_, "back");
            play_through();
            std::cout << "replays content: expansion playing\n";
            until(
                kDownloadWaitMs,
                [&] { return soft_restart_requested(); },
                "the player's expansions were not put back"
            );
            return;
        }
        if (phase == "back") {
            next_start_idle();
            require(expansion_off(expansion_id), "the expansion stayed on after the replay");
            require(state_key_gone(), "the replay's expansion state was kept");
            std::cout << "replays content: expansions restored\n";
            library_on_replay("default/expansion.oademo");
            arm_stop(expansion_id, "play", "killed");
            return;
        }
        if (phase == "killed" || phase == "killed-wait") {
            require(state_key_gone(), "a stopped replay kept its expansion state");
            require(expansion_off(expansion_id), "a stopped replay left the expansion on");
            require(pending_demo_path().empty(), "a stopped replay still had a demo waiting");
            if (soft_restart_requested())
                return;
            next_start_idle();
            if (phase == "killed") {
                std::cout << "replays content: a stop during the replay put the set back and did "
                             "not play it\n";
                arm_stop(expansion_id, "wait", "killed-wait");
                return;
            }
            std::cout << "replays content: a start that ended before the replay put the set back "
                         "and did not play it\n";
            open_replays();
            const auto [part_chip, part_note] = show("default/expansion.oademo");
            require(part_chip == expected_chip, "the expansion chip is \"" + part_chip + "\"");
            static_cast<void>(part_note);
            const std::optional<AutomationControl> part_watch = named("oa.library.action.watch");
            require(
                part_watch && part_watch->text == "Turn on and watch",
                "the button is not Turn on and watch"
            );
            write_phase(user_folder_, "partway");
            click_named("oa.library.action.watch");
            until(
                kDownloadWaitMs,
                [&] { return soft_restart_requested(); },
                "Turn on and watch did not restart"
            );
            return;
        }
        if (phase == "partway") {
            require(
                lists_id(content_choice().expansions, expansion_id),
                "the expansion is not on for the replay left part way"
            );
            require(
                call_hook_or_raise<&Extension::start_scene>(extension_, *this),
                "the replay left part way did not start"
            );
            until(
                kDownloadWaitMs,
                [&] { return screen_ == Screen::match && match_ != nullptr; },
                "the replay left part way did not start"
            );
            const uint32_t first_tick = match_->state().game.tick;
            // The check's clock is fixed, so ticks are granted one at a time,
            // as the replays check grants them. The card stores that same
            // tick. The recording lasts about three seconds, so stopping
            // after one second leaves a place instead of the end.
            for (int frame = 0;
                 frame < kMostFrames && match_ != nullptr && screen_ != Screen::campaign_end &&
                 !match_finished_ && match_->state().game.tick < first_tick + kSwitchedLeftTick;
                 ++frame) {
                static_cast<void>(run_granted_ticks(1));
                run_frame(running);
                require(running, "a frame ended the run");
            }
            const uint32_t played =
                match_ != nullptr ? match_->state().game.tick : static_cast<uint32_t>(0);
            require(
                match_ != nullptr && screen_ != Screen::campaign_end && !match_finished_ &&
                    played >= first_tick + kSwitchedLeftTick,
                "the replay was not left after its ticks (at " + std::to_string(played) + ")"
            );
            uint32_t stored_tick = played;
            ReplayPlaybackView playback{};
            if (call_hook_or_report<&Extension::replay_state>(
                    extension_, hook_error_report(), *this, &playback
                ))
                stored_tick = playback.tick;
            require(
                stored_tick >= kSwitchedLeftTick, "the replay clock is before the replay was left"
            );
            write_text(
                user_folder_ / std::string(kFromTickName), std::to_string(stored_tick) + "\n"
            );
            write_phase(user_folder_, "partway-back");
            leave_match();
            load(Screen::main_menu);
            until(
                kDownloadWaitMs,
                [&] { return soft_restart_requested(); },
                "leaving the replay did not put the expansions back"
            );
            return;
        }
        if (phase == "partway-back") {
            next_start_idle();
            require(expansion_off(expansion_id), "the expansion stayed on after the replay");
            require(state_key_gone(), "the replay's expansion state was kept");
            library_on_replay("default/expansion.oademo");
            std::ifstream tick_file(user_folder_ / std::string(kFromTickName));
            std::string tick_line;
            std::getline(tick_file, tick_line);
            if (!tick_line.empty() && tick_line.back() == '\r')
                tick_line.pop_back();
            require(!tick_line.empty(), "the left tick was not stored");
            uint32_t expected = 0;
            for (const char digit : tick_line) {
                require(digit >= '0' && digit <= '9', "the left tick is not a number");
                expected = expected * 10u + static_cast<uint32_t>(digit - '0');
            }
            require(expected >= kSwitchedLeftTick, "the left tick is before the replay was left");
            std::string watched;
            uint32_t left_tick = 0;
            bool saw_row = false;
            const uint64_t began = SDL_GetTicks();
            while (SDL_GetTicks() - began < kDownloadWaitMs) {
                run_frame(running);
                require(running, "a frame ended the run");
                std::vector<AutomationControl> controls;
                oa_layer().automation_controls(controls);
                watched.clear();
                for (const AutomationControl& control : controls) {
                    if (!control.visible || control.window_width <= 0 || control.window_height <= 0)
                        continue;
                    if (control.text.starts_with("Watched to "))
                        watched = control.text;
                }
                const library::ReplayRow* row =
                    library::find_replay(host.model, "default/expansion.oademo");
                saw_row = row != nullptr && row->replay.watched_tick.has_value();
                if (saw_row)
                    left_tick = *row->replay.watched_tick;
                // Both ticks are replay_state's tick: the one written when
                // the replay was left, and the one the card stored.
                if (saw_row && left_tick == expected && watched.starts_with("Watched to "))
                    break;
                saw_row = false;
            }
            require(
                saw_row,
                "the card does not say where the replay was left (stored " +
                    std::to_string(left_tick) + ", match " + std::to_string(expected) +
                    ", label \"" + watched + "\")"
            );
            std::cout << "replays content: " << watched << '\n';
            const std::optional<AutomationControl> from_button =
                named("oa.library.action.watch-from");
            require(
                from_button && from_button->text.starts_with("Watch from "),
                "Watch from is not offered"
            );
            write_text(user_folder_ / std::string(kFromTickName), std::to_string(left_tick) + "\n");
            write_phase(user_folder_, "from");
            click_named("oa.library.action.watch-from");
            until(
                kDownloadWaitMs,
                [&] { return soft_restart_requested(); },
                "Watch from did not restart"
            );
            return;
        }
        if (phase == "from") {
            require(
                lists_id(content_choice().expansions, expansion_id),
                "the expansion is not on for Watch from"
            );
            std::ifstream tick_file(user_folder_ / std::string(kFromTickName));
            std::string tick_line;
            std::getline(tick_file, tick_line);
            if (!tick_line.empty() && tick_line.back() == '\r')
                tick_line.pop_back();
            require(!tick_line.empty(), "the left tick was not stored");
            uint32_t remembered = 0;
            for (const char digit : tick_line) {
                require(digit >= '0' && digit <= '9', "the left tick is not a number");
                remembered = remembered * 10u + static_cast<uint32_t>(digit - '0');
            }
            require(remembered >= kSwitchedLeftTick, "the left tick is before the replay was left");
            require(
                call_hook_or_raise<&Extension::start_scene>(extension_, *this),
                "Watch from did not start the replay"
            );
            until(
                kDownloadWaitMs,
                [&] { return screen_ == Screen::match && match_ != nullptr; },
                "Watch from did not start the replay"
            );
            // The jump is the replay's own. Granting ticks would let a start
            // at the beginning pass. Watch from seeks once the replay can
            // say where it is, and the card's tick is the one it seeks to.
            bool reached = false;
            uint32_t seen = match_->state().game.tick;
            for (int frame = 0;
                 frame < kMostFrames && match_ != nullptr && screen_ != Screen::campaign_end;
                 ++frame) {
                run_frame(running);
                require(running, "a frame ended the run");
                if (match_ == nullptr)
                    break;
                seen = match_->state().game.tick;
                ReplayPlaybackView playback{};
                const bool have = call_hook_or_report<&Extension::replay_state>(
                    extension_, hook_error_report(), *this, &playback
                );
                if (have && !playback.skipping && playback.tick == remembered &&
                    playback.tick >= kSwitchedLeftTick) {
                    reached = true;
                    seen = playback.tick;
                    break;
                }
            }
            require(reached, "Watch from did not reach the tick (at " + std::to_string(seen) + ")");
            std::cout << "replays content: watch from reached tick " << seen << '\n';
            write_phase(user_folder_, "from-back");
            leave_match();
            load(Screen::main_menu);
            until(
                kDownloadWaitMs,
                [&] { return soft_restart_requested(); },
                "the switched replay did not put the set back"
            );
            return;
        }
        if (phase == "from-back") {
            next_start_idle();
            require(expansion_off(expansion_id), "Watch from left the expansion on");
            require(state_key_gone(), "Watch from kept the expansion state");
            require(pending_demo_path().empty(), "Watch from left a demo waiting");
            std::cout << "replays content: the switched replay put the set back\n";
            open_replays();
            const auto [end_chip, end_note] = show("default/expansion.oademo");
            require(end_chip == expected_chip, "the expansion chip is \"" + end_chip + "\"");
            static_cast<void>(end_note);
            const std::optional<AutomationControl> end_watch = named("oa.library.action.watch");
            require(
                end_watch && end_watch->text == "Turn on and watch",
                "the button is not Turn on and watch"
            );
            write_phase(user_folder_, "ended");
            click_named("oa.library.action.watch");
            until(
                kDownloadWaitMs,
                [&] { return soft_restart_requested(); },
                "Turn on and watch did not restart"
            );
            return;
        }
        if (phase == "ended") {
            require(
                lists_id(content_choice().expansions, expansion_id),
                "the expansion is not on for the replay played to its last tick"
            );
            require(
                source_read.replay.has_value() &&
                    source_read.replay->meta.game.length_ticks.has_value(),
                "the recording does not name its length"
            );
            const uint64_t named_length = *source_read.replay->meta.game.length_ticks;
            require(
                named_length > 0 && named_length <= 0xffffffffULL,
                "the recording's length is not a tick"
            );
            const uint32_t recorded = static_cast<uint32_t>(named_length);
            require(
                call_hook_or_raise<&Extension::start_scene>(extension_, *this),
                "the replay played to its last tick did not start"
            );
            until(
                kDownloadWaitMs,
                [&] { return screen_ == Screen::match && match_ != nullptr; },
                "the replay played to its last tick did not start"
            );
            // One tick at a time near the end. Granting past the recording
            // runs the match on to an end screen. Leaving at the length the
            // recording names does not.
            bool reached = false;
            for (int frame = 0; frame < 8000 && match_ != nullptr &&
                                screen_ != Screen::campaign_end && !match_finished_;
                 ++frame) {
                const uint32_t tick = match_->state().game.tick;
                if (tick >= recorded) {
                    reached = true;
                    break;
                }
                const uint32_t room = recorded - tick;
                static_cast<void>(run_granted_ticks(room > 30u ? 30u : 1u));
                run_frame(running);
                require(running, "a frame ended the run");
            }
            require(
                reached && match_ != nullptr && screen_ != Screen::campaign_end && !match_finished_,
                "the replay did not stop on its last recorded tick"
            );
            write_phase(user_folder_, "ended-back");
            leave_match();
            load(Screen::main_menu);
            until(
                kDownloadWaitMs,
                [&] { return soft_restart_requested(); },
                "leaving the last tick did not put the expansions back"
            );
            return;
        }
        if (phase == "ended-back") {
            next_start_idle();
            require(expansion_off(expansion_id), "the expansion stayed on after the last tick");
            require(state_key_gone(), "the replay's expansion state was kept");
            library_on_replay("default/expansion.oademo");
            bool clear = false;
            std::string watched;
            const uint64_t began = SDL_GetTicks();
            while (SDL_GetTicks() - began < kDownloadWaitMs) {
                run_frame(running);
                require(running, "a frame ended the run");
                std::vector<AutomationControl> controls;
                oa_layer().automation_controls(controls);
                watched.clear();
                for (const AutomationControl& control : controls) {
                    if (!control.visible || control.window_width <= 0 || control.window_height <= 0)
                        continue;
                    if (control.text.starts_with("Watched to "))
                        watched = control.text;
                }
                const library::ReplayRow* row =
                    library::find_replay(host.model, "default/expansion.oademo");
                const bool no_tick = row != nullptr && !row->replay.watched_tick.has_value();
                if (no_tick && watched.empty() && !named("oa.library.action.watch-from")) {
                    clear = true;
                    break;
                }
            }
            require(clear, "a replay played to its last tick kept a place (\"" + watched + "\")");
            std::cout << "replays content: a replay played to its last tick keeps no place\n";
            return;
        }
        throw std::runtime_error("replays content check: the expansion phase is unknown");
    }

    if (which == "packs" && options_.restarts > 0) {
        const std::string phase = read_phase(user_folder_);
        const auto version_is = [&](std::string_view version) {
            return mod_profile() != nullptr && mod_profile()->version == version;
        };
        if (phase == "both-ready") {
            require(version_is("2.9"), "Ridge 2.9 is not chosen");
            const fs::path installed_pack = user_folder_ /
                                            std::string(package_install::oamap::maps_folder_name) /
                                            "ridge-reach";
            const std::vector<uint8_t> origin_bytes =
                read_bytes(installed_pack / std::string(package_install::origin_file_name));
            const std::string origin_text(origin_bytes.begin(), origin_bytes.end());
            require(
                origin_text.find(fact(facts, "pack_new_sha")) != std::string::npos,
                "Maps does not hold the newer pack"
            );
            const std::string registry_id = fact(facts, "registry");
            until(
                kRegistryWaitMs,
                [&] {
                    const std::shared_ptr<const content::Snapshot> snapshot_now =
                        content_service().snapshot();
                    if (!snapshot_now)
                        return false;
                    for (const content::RegistryView& registry_view : snapshot_now->registries) {
                        if (registry_view.registry.descriptor.id != registry_id)
                            continue;
                        return registry_view.releases != nullptr && !registry_view.refreshing;
                    }
                    return false;
                },
                "the releases list was not held"
            );
            open_replays();
            const auto [chip, note] = show("default/both-pack.oademo");
            require(
                note.find("another release of the map pack Ridge Reach") != std::string::npos,
                "the card does not name the other map pack release"
            );
            const std::optional<AutomationControl> watch = named("oa.library.action.watch");
            require(
                watch && watch->text.find("and watch") != std::string::npos,
                "the button does not offer to watch"
            );
            write_phase(user_folder_, "both-playing");
            click_named("oa.library.action.watch");
            until(
                kDownloadWaitMs,
                [&] { return soft_restart_requested(); },
                "the older pack and the older mod did not restart"
            );
            return;
        }
        if (phase == "both-playing") {
            write_phase(user_folder_, "both-back");
            require(version_is("2.8"), "Ridge 2.8 is not playing");
            require(
                call_hook_or_raise<&Extension::start_scene>(extension_, *this),
                "the replay did not start"
            );
            until(
                kDownloadWaitMs,
                [&] { return screen_ == Screen::match && match_ != nullptr; },
                "the replay did not start"
            );
            const PackMap* mounted = mounted_pack_map();
            require(mounted != nullptr, "the kept pack was not mounted after the restart");
            const std::string folder = platform::path_to_utf8(mounted->folder);
            const std::string old_sha = fact(facts, "pack_sha");
            require(
                folder.find("Replay releases") != std::string::npos &&
                    folder.find(old_sha) != std::string::npos,
                "the restarted replay is not using the kept pack"
            );
            const uint32_t began_tick = match_->state().game.tick;
            for (int frame = 0; frame < kMostFrames && match_ != nullptr &&
                                match_->state().game.tick < began_tick + kWatchedTicks;
                 ++frame) {
                static_cast<void>(run_granted_ticks(1));
                run_frame(running);
                require(running, "a frame ended the run");
            }
            require(
                match_ != nullptr && match_->state().game.tick >= began_tick + kWatchedTicks,
                "the restarted replay did not play its ticks"
            );
            std::cout << "replays content: older pack and older mod playing\n";
            leave_match();
            load(Screen::main_menu);
            until(
                kDownloadWaitMs,
                [&] { return soft_restart_requested(); },
                "Ridge 2.9 was not put back after the pack"
            );
            return;
        }
        if (phase == "both-back") {
            next_start_idle();
            require(version_is("2.9"), "Ridge 2.9 was not chosen after the pack");
            require(state_key_gone(), "the replay's state was kept");
            require(
                preference_values_.find(std::string(oa::data::saves::replay_map_pack_key)) ==
                    preference_values_.end(),
                "the kept pack folder was kept"
            );
            std::cout << "replays content: 2.9 chosen after the pack\n";
            return;
        }
        throw std::runtime_error("replays content check: the packs phase is unknown");
    }

    if (which == "play" && options_.restarts > 0) {
        const std::string phase = read_phase(user_folder_);
        if (phase == "ridge") {
            require(
                mod_profile() != nullptr && mod_profile()->id == "ridge", "Ridge is not playing"
            );
            require(!options_.layered_mod_folder.empty(), "Ridge's folder is not layered");
            write_text(
                user_folder_ / std::string(kRidgePathName),
                platform::path_to_utf8(options_.layered_mod_folder) + "\n"
            );
            write_phase(user_folder_, "ridge-back");
            play_through();
            std::cout << "replays content: ridge playing\n";
            until(
                kDownloadWaitMs, [&] { return soft_restart_requested(); }, "No Mod was not put back"
            );
            return;
        }
        if (phase == "ridge-back") {
            next_start_idle();
            require(mod_profile() == nullptr, "a mod stayed chosen after the replay");
            require(options_.layered_mod_folder.empty(), "a mod folder stayed layered");
            require(
                chosen_mod_directory({}, false, preference_values_).empty(),
                "the mod preference was not cleared"
            );
            std::cout << "replays content: no mod\n";
            library_on_replay("default/ridge.oademo");
            open_replays();
            const auto shown = show("default/ridge.oademo");
            static_cast<void>(shown);
            const std::optional<AutomationControl> watch = named("oa.library.action.watch");
            require(watch && watch->text == "Watch", "the button is not Watch");
            write_phase(user_folder_, "ridge-again");
            click_named("oa.library.action.watch");
            until(
                kDownloadWaitMs,
                [&] { return soft_restart_requested(); },
                "Watch did not play the installed Ridge again"
            );
            return;
        }
        if (phase == "ridge-again") {
            require(
                mod_profile() != nullptr && mod_profile()->id == "ridge",
                "Ridge is not playing the second time"
            );
            require(!options_.layered_mod_folder.empty(), "Ridge's folder is not layered");
            write_phase(user_folder_, "ridge-again-back");
            play_through();
            std::cout << "replays content: ridge again\n";
            until(
                kDownloadWaitMs,
                [&] { return soft_restart_requested(); },
                "No Mod was not put back after the second watch"
            );
            return;
        }
        if (phase == "ridge-again-back") {
            next_start_idle();
            require(mod_profile() == nullptr, "a mod stayed chosen after the second watch");
            require(options_.layered_mod_folder.empty(), "a mod folder stayed layered");
            require(
                chosen_mod_directory({}, false, preference_values_).empty(),
                "the mod preference was not cleared"
            );
            std::cout << "replays content: no mod\n";
            library_on_replay("default/ridge.oademo");
            std::ifstream saved(user_folder_ / std::string(kRidgePathName));
            std::string folder_text;
            std::getline(saved, folder_text);
            if (!folder_text.empty() && folder_text.back() == '\r')
                folder_text.pop_back();
            require(!folder_text.empty(), "Ridge's folder was not kept");
            remember_mod_directory(preference_values_, platform::path_from_utf8(folder_text));
            preference_values_[std::string(replay_mod_marker_preference)] = "1";
            preference_values_[std::string(replay_mod_preference)] = {};
            preference_values_[std::string(replay_expansion_state_preference)] = "play";
            preference_values_.erase(std::string(replay_expansion_preference));
            preference_values_.erase(std::string(oa::data::saves::pending_demo_key));
            preference_values_.erase(std::string(oa::data::saves::replay_map_pack_key));
            preferences_dirty_ = true;
            flush_preferences();
            write_phase(user_folder_, "mod-killed");
            request_soft_restart();
            return;
        }
        if (phase == "mod-killed") {
            require(state_key_gone(), "a stopped replay kept its state");
            require(pending_demo_path().empty(), "a stopped replay still had a demo waiting");
            require(
                chosen_mod_directory({}, false, preference_values_).empty(),
                "a stopped replay left Ridge chosen"
            );
            require(
                preference_values_.find(std::string(replay_mod_marker_preference)) ==
                    preference_values_.end(),
                "a stopped replay kept the mod marker"
            );
            if (soft_restart_requested())
                return;
            next_start_idle();
            require(
                mod_profile() == nullptr && options_.layered_mod_folder.empty(),
                "No Mod was not mounted"
            );
            std::cout << "replays content: a stop during the replay chose No Mod and did not play "
                         "it\n";
            return;
        }
        throw std::runtime_error("replays content check: the play phase is unknown");
    }

    if (which == "beside" && options_.restarts > 0) {
        const std::string phase = read_phase(user_folder_);
        const fs::path mods = user_folder_ / std::string(user_mods_folder_name);
        const fs::path installed_ridge = mods / "ridge";
        const auto version_is = [&](std::string_view version) {
            return mod_profile() != nullptr && mod_profile()->version == version;
        };
        if (phase == "installed") {
            require(version_is("2.9"), "Ridge 2.9 is not chosen");
            const std::string registry_id = fact(facts, "registry");
            until(
                kRegistryWaitMs,
                [&] {
                    const std::shared_ptr<const content::Snapshot> snapshot_now =
                        content_service().snapshot();
                    if (!snapshot_now)
                        return false;
                    for (const content::RegistryView& registry_view : snapshot_now->registries) {
                        if (registry_view.registry.descriptor.id != registry_id)
                            continue;
                        return registry_view.releases != nullptr && !registry_view.refreshing;
                    }
                    return false;
                },
                "the releases list was not held"
            );
            require(
                read_line(user_folder_ / std::string(kKeptName)) == "2.9", "the 2.9 marker is gone"
            );
            const std::vector<fs::path> listed = list_mods_in(mods);
            require(listed.size() == 1, "Mods lists something besides Ridge");
            for (const fs::directory_entry& entry : fs::directory_iterator(mods)) {
                require(
                    platform::path_to_utf8(entry.path().filename()) !=
                        package_install::replay_releases_folder_name,
                    "Replay releases is under Mods"
                );
            }
            open_replays();
            const auto [chip, note] = show("default/ridge.oademo");
            require(chip == "Needs Ridge 2.8", "Ridge's chip is \"" + chip + "\"");
            require(
                note.find("You have 2.9") != std::string::npos, "the card does not say you have 2.9"
            );
            const std::optional<AutomationControl> watch = named("oa.library.action.watch");
            require(
                watch && watch->text == "Get Ridge 2.8 and watch",
                "the button is not Get Ridge 2.8 and watch"
            );
            write_phase(user_folder_, "watching");
            click_named("oa.library.action.watch");
            until(
                kDownloadWaitMs,
                [&] { return soft_restart_requested(); },
                "Get Ridge 2.8 and watch did not restart"
            );
            return;
        }
        if (phase == "watching") {
            require(version_is("2.8"), "Ridge 2.8 is not playing");
            const std::string layered = platform::path_to_utf8(options_.layered_mod_folder);
            require(
                layered.find(package_install::replay_releases_folder_name) != std::string::npos,
                "Ridge 2.8 is not in Replay releases"
            );
            std::error_code same;
            require(
                !same && !fs::equivalent(options_.layered_mod_folder, installed_ridge, same),
                "the replay is using the installed Ridge"
            );
            require(
                read_line(user_folder_ / std::string(kKeptName)) == "2.9",
                "the 2.9 marker was changed"
            );
            require(
                folder_names_version(installed_ridge, "2.9"), "the installed Ridge is no longer 2.9"
            );
            write_text(
                user_folder_ / std::string(kBesideOldName),
                platform::path_to_utf8(options_.layered_mod_folder) + "\n"
            );
            write_text(
                user_folder_ / std::string(kBesideNewName),
                platform::path_to_utf8(installed_ridge) + "\n"
            );
            write_phase(user_folder_, "back");
            play_through();
            std::cout << "replays content: ridge 2.8 playing\n";
            until(
                kDownloadWaitMs,
                [&] { return soft_restart_requested(); },
                "Ridge 2.9 was not put back"
            );
            return;
        }
        if (phase == "back") {
            next_start_idle();
            require(version_is("2.9"), "Ridge 2.9 was not chosen after the replay");
            require(
                read_line(user_folder_ / std::string(kKeptName)) == "2.9",
                "the 2.9 marker was changed"
            );
            std::cout << "replays content: 2.9 chosen\n";
            library_on_replay("default/ridge.oademo");
            const std::string old_folder = read_line(user_folder_ / std::string(kBesideOldName));
            const std::string new_folder = read_line(user_folder_ / std::string(kBesideNewName));
            require(
                !old_folder.empty() && !new_folder.empty(), "the two Ridge folders were not kept"
            );
            remember_mod_directory(preference_values_, platform::path_from_utf8(old_folder));
            preference_values_[std::string(replay_mod_marker_preference)] = "1";
            preference_values_[std::string(replay_mod_preference)] = new_folder;
            preference_values_[std::string(replay_expansion_state_preference)] = "play";
            preference_values_.erase(std::string(replay_expansion_preference));
            preference_values_.erase(std::string(oa::data::saves::pending_demo_key));
            preference_values_.erase(std::string(oa::data::saves::replay_map_pack_key));
            preferences_dirty_ = true;
            flush_preferences();
            write_phase(user_folder_, "killed");
            request_soft_restart();
            return;
        }
        if (phase == "killed") {
            if (soft_restart_requested())
                return;
            next_start_idle();
            require(version_is("2.9"), "a stop during the replay did not choose Ridge 2.9");
            require(pending_demo_path().empty(), "a stopped replay still had a demo waiting");
            require(state_key_gone(), "a stopped replay kept its state");
            require(
                preference_values_.find(std::string(replay_mod_marker_preference)) ==
                    preference_values_.end(),
                "a stopped replay kept the mod marker"
            );
            require(
                read_line(user_folder_ / std::string(kKeptName)) == "2.9",
                "the 2.9 marker was changed"
            );
            std::cout << "replays content: a stop during the replay chose Ridge 2.9 and did not "
                         "play it\n";
            ask_registry_stop(fact(facts, "descriptor"));
            write_phase(user_folder_, "kept");
            request_soft_restart();
            return;
        }
        if (phase == "kept") {
            // A catalogue checked moments ago is not fetched again on start.
            // Ask once: the registry was stopped, so this refresh fails.
            const std::string kept_registry = fact(facts, "registry");
            content_service().refresh(content::RefreshReason::check_now, kept_registry);
            until(
                kRegistryWaitMs,
                [&] {
                    const std::shared_ptr<const content::Snapshot> snapshot_now =
                        content_service().snapshot();
                    if (!snapshot_now)
                        return false;
                    for (const content::RegistryView& registry_view : snapshot_now->registries) {
                        if (registry_view.registry.descriptor.id != kept_registry)
                            continue;
                        return !registry_view.refreshing &&
                               registry_view.status == content::RegistryStatus::failed;
                    }
                    return false;
                },
                "the stopped registry did not fail"
            );
            open_replays();
            const auto [kept_chip, kept_note] = show("default/ridge.oademo");
            require(kept_chip.empty(), "a kept release still has a chip \"" + kept_chip + "\"");
            require(kept_note.empty(), "a kept release still has a note \"" + kept_note + "\"");
            const std::optional<AutomationControl> kept_watch = named("oa.library.action.watch");
            require(kept_watch && kept_watch->text == "Watch", "the button is not Watch");
            const library::ReplayRow* kept_row =
                library::find_replay(host.model, "default/ridge.oademo");
            require(kept_row != nullptr, "the kept replay has no row");
            bool names_release = false;
            for (const library::ReplayFact& fact_line :
                 library::replay_facts(host.model, *kept_row)) {
                if (fact_line.key == "mod" && fact_line.value == "Ridge 2.8 (rev 1)")
                    names_release = true;
            }
            require(names_release, "the card does not name Ridge 2.8 (rev 1)");
            write_phase(user_folder_, "kept-playing");
            click_named("oa.library.action.watch");
            until(
                kDownloadWaitMs,
                [&] { return soft_restart_requested(); },
                "Watch of the kept release did not restart"
            );
            return;
        }
        if (phase == "kept-playing") {
            require(version_is("2.8"), "the kept Ridge 2.8 is not playing");
            const std::string layered = platform::path_to_utf8(options_.layered_mod_folder);
            require(
                layered.find(package_install::replay_releases_folder_name) != std::string::npos,
                "the kept Ridge 2.8 is not in Replay releases"
            );
            std::error_code same;
            require(
                !same && !fs::equivalent(options_.layered_mod_folder, installed_ridge, same),
                "the kept replay is using the installed Ridge"
            );
            write_phase(user_folder_, "kept-back");
            play_through();
            std::cout << "replays content: kept ridge 2.8 playing\n";
            until(
                kDownloadWaitMs,
                [&] { return soft_restart_requested(); },
                "Ridge 2.9 was not put back after the kept copy"
            );
            return;
        }
        if (phase == "kept-back") {
            next_start_idle();
            require(version_is("2.9"), "Ridge 2.9 was not chosen after the kept copy");
            library_on_replay("default/ridge.oademo");
            std::cout << "replays content: 2.9 chosen again\n";
            // Install alongside leaves the older release in Mods, with its origin.
            const std::optional<fs::path> kept_release =
                package_install::replay_release_folder(user_folder_, fact(facts, "ridge_sha"));
            require(kept_release.has_value(), "the kept Ridge 2.8 is not in Replay releases");
            const fs::path alongside = mods / "ridge-2.8";
            std::error_code moved;
            fs::rename(*kept_release, alongside, moved);
            require(!moved, "the kept release was not moved to Mods/ridge-2.8");
            write_phase(user_folder_, "alongside");
            request_soft_restart();
            return;
        }
        if (phase == "alongside") {
            require(version_is("2.9"), "Ridge 2.9 is not chosen before the alongside watch");
            open_replays();
            const auto [alongside_chip, alongside_note] = show("default/ridge.oademo");
            require(
                alongside_chip.empty(),
                "an alongside release still has a chip \"" + alongside_chip + "\""
            );
            require(
                alongside_note.empty(),
                "an alongside release still has a note \"" + alongside_note + "\""
            );
            const std::optional<AutomationControl> alongside_watch =
                named("oa.library.action.watch");
            require(
                alongside_watch && alongside_watch->text == "Watch",
                "the alongside button is not Watch"
            );
            write_phase(user_folder_, "alongside-playing");
            click_named("oa.library.action.watch");
            until(
                kDownloadWaitMs,
                [&] { return soft_restart_requested(); },
                "Watch of the alongside release did not restart"
            );
            return;
        }
        if (phase == "alongside-playing") {
            require(version_is("2.8"), "the alongside Ridge 2.8 is not playing");
            const fs::path alongside = mods / "ridge-2.8";
            std::error_code alongside_same;
            require(
                !alongside_same &&
                    fs::equivalent(options_.layered_mod_folder, alongside, alongside_same),
                "the replay is not using Mods/ridge-2.8"
            );
            write_phase(user_folder_, "alongside-back");
            play_through();
            std::cout << "replays content: alongside ridge 2.8 playing\n";
            until(
                kDownloadWaitMs,
                [&] { return soft_restart_requested(); },
                "Ridge 2.9 was not put back after the alongside copy"
            );
            return;
        }
        if (phase == "alongside-back") {
            next_start_idle();
            require(version_is("2.9"), "Ridge 2.9 was not chosen after the alongside copy");
            return;
        }
        throw std::runtime_error("replays content check: the beside phase is unknown");
    }

    const auto cached_found = facts.find("cached");
    const bool cached_press = cached_found != facts.end() && cached_found->second == "press";
    const std::string registry_id = fact(facts, "registry");
    if (which == "update" && options_.restarts > 0) {
        require(read_phase(user_folder_) == "ready", "the update phase is unknown");
        require(mod_profile() == nullptr, "a mod is chosen");
        const fs::path update_mods = user_folder_ / std::string(user_mods_folder_name);
        const fs::path update_ridge = update_mods / "ridge";
        require(folder_names_version(update_ridge, "2.8"), "Ridge 2.8 is not installed");
        const std::string update_registry = fact(facts, "registry");
        until(
            kRegistryWaitMs,
            [&] {
                const std::shared_ptr<const content::Snapshot> snapshot_now =
                    content_service().snapshot();
                if (!snapshot_now)
                    return false;
                for (const content::RegistryView& registry_view : snapshot_now->registries) {
                    if (registry_view.registry.descriptor.id != update_registry)
                        continue;
                    return registry_view.releases != nullptr && !registry_view.refreshing;
                }
                return false;
            },
            "the releases list was not held"
        );
        open_replays();
        const auto [update_chip, update_note] = show("default/ridge.oademo");
        static_cast<void>(update_chip);
        static_cast<void>(update_note);
        const std::optional<AutomationControl> before = named("oa.library.action.watch");
        require(before && before->text == "Watch", "Ridge 2.8 does not read Watch");
        click_named("oa.library.tab.updates");
        frames(4);
        // The button is drawn disabled until the installed Ridge is an update.
        const auto update_all_ready = [&] {
            const std::optional<AutomationControl> update_all =
                named("oa.library.action.update-all");
            return update_all && update_all->enabled;
        };
        if (!update_all_ready()) {
            click_named("oa.library.refresh");
            until(kRegistryWaitMs, update_all_ready, "Update all was not shown");
        }
        click_named("oa.library.action.update-all");
        until(
            kDownloadWaitMs,
            [&] { return named("oa.prompt.update").has_value(); },
            "Update all did not ask"
        );
        click_named("oa.prompt.update");
        until(
            kDownloadWaitMs,
            [&] { return folder_names_version(update_ridge, "2.9"); },
            "the update did not install Ridge 2.9"
        );
        click_named("oa.library.tab.replays");
        frames(2);
        click_named(row_of("default/ridge.oademo"));
        until(
            kDownloadWaitMs,
            [&] {
                const std::optional<AutomationControl> after = named("oa.library.action.watch");
                return after && after->text == "Get Ridge 2.8 and watch";
            },
            "the replay still reads Watch after the update"
        );
        const auto [replaced_chip, replaced_note] = show("default/ridge.oademo");
        require(replaced_chip == "Needs Ridge 2.8", "Ridge's chip is \"" + replaced_chip + "\"");
        require(
            replaced_note.find("You have 2.9.") != std::string::npos,
            "the card does not say you have 2.9."
        );
        std::error_code releases_missing;
        require(
            !fs::exists(
                user_folder_ / std::string(package_install::replay_releases_folder_name),
                releases_missing
            ),
            "Replay releases exists after the update"
        );
        std::cout << "replays content: update noticed\n";
        return;
    }

    if (which == "local" && options_.restarts > 0) {
        const std::string phase = read_phase(user_folder_);
        const fs::path recordings = user_folder_ / std::string(recordings_folder_name);
        const fs::path ridge = user_folder_ / std::string(user_mods_folder_name) / "ridge";
        const auto releases_held = [&] {
            until(
                kRegistryWaitMs,
                [&] {
                    const std::shared_ptr<const content::Snapshot> snapshot_now =
                        content_service().snapshot();
                    if (!snapshot_now)
                        return false;
                    for (const content::RegistryView& registry_view : snapshot_now->registries) {
                        if (registry_view.registry.descriptor.id != registry_id)
                            continue;
                        return registry_view.releases != nullptr && !registry_view.refreshing;
                    }
                    return false;
                },
                "the releases list was not held"
            );
        };
        const auto watch_text = [&] {
            const std::optional<AutomationControl> watch = named("oa.library.action.watch");
            return watch ? watch->text : std::string{};
        };
        if (phase == "local-ready") {
            require(mod_profile() == nullptr, "a mod is chosen");
            require(folder_names_version(ridge, "2.8"), "Ridge 2.8 is not installed");
            releases_held();
            open_replays();
            // Each note, and the folder of the .tad with none, names the
            // installed Ridge: nothing is missing, and Watch plays it.
            for (const std::string_view path : {kTadNamed, kTadById, kTadBare}) {
                const auto [chip, note] = show(path);
                require(chip.empty(), std::string(path) + " shows \"" + chip + "\"");
                require(note.empty(), std::string(path) + " says \"" + note + "\"");
                require(watch_text() == "Watch", std::string(path) + " is not Watch");
            }
            // The background read plays each with Ridge. Without it the
            // recorded unit table is not this one, and the read is refused.
            until(
                kDownloadWaitMs,
                [&] {
                    for (const std::string_view path : {kTadNamed, kTadById, kTadBare}) {
                        const library::ReplayRow* row = library::find_replay(host.model, path);
                        if (row == nullptr || !row->replay.timeline.has_value() ||
                            row->replay.timeline->empty())
                            return false;
                    }
                    return true;
                },
                "the index pass did not read the local .tad files with Ridge"
            );
            for (const std::string_view path : {kTadNamed, kTadById, kTadBare}) {
                const library::ReplayRow* row = library::find_replay(host.model, path);
                require(
                    row != nullptr && !row->replay.read_stamp.has_value(),
                    std::string(path) + " was refused"
                );
                std::cout << "replays content: index pass read " << path << " with Ridge\n";
            }
            show(kTadNamed);
            require(watch_text() == "Watch", "the named .tad is not Watch");
            write_phase(user_folder_, "local-playing");
            click_named("oa.library.action.watch");
            until(
                kDownloadWaitMs,
                [&] { return soft_restart_requested(); },
                "Watch did not restart with Ridge for the .tad"
            );
            return;
        }
        if (phase == "local-playing") {
            require(
                mod_profile() != nullptr && mod_profile()->id == "ridge",
                "Ridge is not playing the .tad"
            );
            require(!options_.layered_mod_folder.empty(), "Ridge's folder is not layered");
            std::cout << "replays content: local tad playing with Ridge\n";
            // The bar reads the replay itself when the Library holds no timeline.
            forget_timeline(recordings, kTadNamed);
            write_phase(user_folder_, "local-back");
            require(
                call_hook_or_raise<&Extension::start_scene>(extension_, *this),
                "the .tad did not start"
            );
            until(
                kDownloadWaitMs,
                [&] { return screen_ == Screen::match && match_ != nullptr; },
                "the .tad did not start"
            );
            // Paused, the replay cannot end and close the bar before its read does.
            until(
                kDownloadWaitMs,
                [&] { return replay_viewer_command("watch.replay-pause"); },
                "the replay did not pause"
            );
            until(
                kBarPassStartMs,
                [&] { return replay_index_pid() != 0; },
                "the bar's read did not start"
            );
            until(
                kBarPassEndMs, [&] { return replay_index_pid() == 0; }, "the bar's read did not end"
            );
            require(
                !host.timeline_of(*this, std::string(kTadNamed)).empty(),
                "the bar's read did not play the .tad with Ridge"
            );
            std::cout << "replays content: bar read " << kTadNamed << " with Ridge\n";
            leave_match();
            load(Screen::main_menu);
            until(
                kDownloadWaitMs,
                [&] { return soft_restart_requested(); },
                "No Mod was not put back after the .tad"
            );
            return;
        }
        if (phase == "local-back") {
            next_start_idle();
            require(mod_profile() == nullptr, "a mod stayed chosen after the .tad");
            require(options_.layered_mod_folder.empty(), "a mod folder stayed layered");
            std::cout << "replays content: no mod after the local tad\n";
            library_on_replay(kTadNamed);
            {
                std::error_code removed;
                fs::remove_all(ridge, removed);
                require(!removed, "Ridge was not uninstalled");
            }
            releases_held();
            open_replays();
            // A note's mod that is gone is named, and the catalogue's Ridge is offered.
            for (const std::string_view path : {kTadNamed, kTadById}) {
                const auto [chip, note] = show(path);
                require(chip == "Needs Ridge", std::string(path) + " shows \"" + chip + "\"");
                require(
                    note.find("This replay was recorded with Ridge.") != std::string::npos,
                    std::string(path) + " says \"" + note + "\""
                );
                require(
                    watch_text() == "Get it and watch",
                    std::string(path) + " offers \"" + watch_text() + "\""
                );
            }
            // A folder whose mod is gone plays as it always did.
            const auto [bare_chip, bare_note] = show(kTadBare);
            static_cast<void>(bare_note);
            require(bare_chip.empty(), "the .tad with no note shows \"" + bare_chip + "\"");
            require(watch_text() == "Watch", "the .tad with no note is not Watch");
            std::cout << "replays content: local tad needs Ridge once it is gone\n";
            // One press fetches the catalogue's Ridge and plays the .tad with it.
            show(kTadNamed);
            write_phase(user_folder_, "local-got");
            click_named("oa.library.action.watch");
            until(
                kDownloadWaitMs,
                [&] { return soft_restart_requested(); },
                "Get it and watch did not restart with Ridge"
            );
            return;
        }
        if (phase == "local-got") {
            require(folder_names_version(ridge, "2.9"), "the catalogue's Ridge 2.9 is not in Mods");
            require(
                mod_profile() != nullptr && mod_profile()->id == "ridge" &&
                    mod_profile()->version == "2.9",
                "the fetched Ridge is not playing the .tad"
            );
            write_phase(user_folder_, "local-done");
            require(
                call_hook_or_raise<&Extension::start_scene>(extension_, *this),
                "the .tad did not start with the fetched Ridge"
            );
            until(
                kDownloadWaitMs,
                [&] { return screen_ == Screen::match && match_ != nullptr; },
                "the .tad did not start with the fetched Ridge"
            );
            std::cout << "replays content: local tad playing with the fetched Ridge 2.9\n";
            leave_match();
            load(Screen::main_menu);
            until(
                kDownloadWaitMs,
                [&] { return soft_restart_requested(); },
                "No Mod was not put back after the fetched Ridge"
            );
            return;
        }
        if (phase == "local-done") {
            next_start_idle();
            require(mod_profile() == nullptr, "a mod stayed chosen after the fetched Ridge");
            std::cout << "replays content: no mod after the fetched Ridge\n";
            library_on_replay(kTadNamed);
            // The next start opens no Library before the index is written.
            write_phase(user_folder_, "local-refused");
            request_soft_restart();
            return;
        }
        if (phase == "local-refused") {
            // Two rows the background read refused: one under this build and
            // what is installed now, one under an older build. Only the older
            // one is read again.
            require(!options_.engine_build.empty(), "the run names no engine build");
            const std::string now = host.replay_read_stamp(*this);
            const std::string build_part = options_.engine_build + "|";
            require(now.starts_with(build_part), "the read stamp does not begin with the build");
            const std::string older = "0.9.0+000000000000|" + now.substr(build_part.size());
            oa::data::replays::Index index = oa::data::replays::load_index(recordings);
            for (oa::data::replays::Replay& replay : index.replays) {
                if (replay.path != kTadNamed && replay.path != kTadById)
                    continue;
                replay.timeline.reset();
                replay.read_stamp = replay.path == kTadNamed ? now : older;
            }
            require(oa::data::replays::save_index(recordings, index), "the index was not saved");
            open_replays();
            require(
                host.replay_read_stamp(*this) == now,
                "what is installed changed as the Library opened"
            );
            until(
                kDownloadWaitMs,
                [&] {
                    const library::ReplayRow* row = library::find_replay(host.model, kTadById);
                    return row != nullptr && row->replay.timeline.has_value() &&
                           !row->replay.timeline->empty();
                },
                "a read refused under an older build was not tried again"
            );
            std::cout << "replays content: a read refused under an older build was read\n";
            frames(30);
            require(!host.index_pass_running(), "the background read did not end");
            const library::ReplayRow* named_row = library::find_replay(host.model, kTadNamed);
            require(
                named_row != nullptr && !named_row->replay.timeline.has_value() &&
                    named_row->replay.read_stamp.has_value() &&
                    *named_row->replay.read_stamp == now,
                "a read refused under this build was tried again"
            );
            std::cout << "replays content: a read refused under this build stayed refused\n";
            return;
        }
        throw std::runtime_error("replays content check: the local phase is unknown");
    }

    if (which == "play" || which == "beside" || which == "update" || which == "packs" ||
        which == "local") {
        const std::string descriptor = fact(facts, "descriptor");
        const uint64_t request = content_service().check_registry_url(descriptor);
        std::optional<content::RegistryCheck> checked;
        until(
            kRegistryWaitMs,
            [&] {
                checked = content_service().registry_check(request);
                return checked.has_value();
            },
            "the registry was not checked"
        );
        require(
            checked->refusal == data::registry::Refusal::none,
            checked->detail.empty() ? "the registry was refused" : checked->detail
        );
        std::string why;
        const data::registry::Refusal added = content_service().add_checked_registry(request, &why);
        require(
            added == data::registry::Refusal::none, why.empty() ? "the registry was not added" : why
        );
        content_service().refresh(content::RefreshReason::check_now, registry_id);
        until(
            kRegistryWaitMs,
            [&] {
                const std::shared_ptr<const content::Snapshot> snapshot_now =
                    content_service().snapshot();
                if (!snapshot_now)
                    return false;
                for (const content::RegistryView& registry_view : snapshot_now->registries) {
                    if (registry_view.registry.descriptor.id != registry_id)
                        continue;
                    return registry_view.releases != nullptr && !registry_view.refreshing;
                }
                return false;
            },
            "the releases list was not fetched"
        );
        std::cout << "replays content: releases held\n";
    } else if (!cached_press) {
        content_service().refresh(content::RefreshReason::check_now, registry_id);
        until(
            kRegistryWaitMs,
            [&] {
                const std::shared_ptr<const content::Snapshot> snapshot_now =
                    content_service().snapshot();
                if (!snapshot_now)
                    return false;
                for (const content::RegistryView& registry_view : snapshot_now->registries) {
                    if (registry_view.registry.descriptor.id != registry_id)
                        continue;
                    return !registry_view.refreshing &&
                           registry_view.status == content::RegistryStatus::failed &&
                           registry_view.catalogue != nullptr;
                }
                return false;
            },
            "the registry did not fail offline"
        );
        std::cout << "replays content: registry offline\n";
    } else {
        std::cout << "replays content: cached catalogue, no refresh\n";
    }

    const fs::path recordings = user_folder_ / std::string(recordings_folder_name);
    {
        std::error_code removed;
        fs::remove_all(recordings, removed);
    }
    if (which == "local") {
        // Ridge 2.8 in Mods, No Mod chosen, and the recording of a Ridge
        // game as three .tad files the way 0.8 left them in Recordings/ridge.
        const fs::path ridge = user_folder_ / std::string(user_mods_folder_name) / "ridge";
        const std::vector<uint8_t> model(std::begin(kRidgeModel), std::end(kRidgeModel));
        write_text(ridge / "oamod.yaml", ridge_profile("2.8"));
        write_text(ridge / "units" / "RGNEW.fbi", kRidgeUnit);
        write_bytes(ridge / "objects3d" / "RGNEW.3DO", model);
        const std::vector<uint8_t> ridge_bytes =
            read_bytes(fs::current_path() / std::string(kRidgeSourceName));
        require(
            !ridge_bytes.empty(), std::string(kRidgeSourceName) + " is not in the working folder"
        );
        const oademo::ReadResult ridge_read = oademo::read(ridge_bytes);
        require(
            ridge_read.ok() && ridge_read.replay.has_value(),
            std::string(kRidgeSourceName) + " does not read: " + ridge_read.error
        );
        const std::vector<uint8_t>& tad_bytes = ridge_read.replay->recording;
        require(tad::parse(tad_bytes).ok(), "the Ridge recording is not a .tad");
        const oa::data::content_bundles::ContentChoice choice = content_choice();
        write_local_tad(recordings, kTadNamed, tad_bytes, choice, "Ridge");
        write_local_tad(recordings, kTadById, tad_bytes, choice, "ridge");
        write_local_tad(recordings, kTadBare, tad_bytes, choice, {});
        write_phase(user_folder_, "local-ready");
        request_soft_restart();
        std::cout << "replays content: local .tad files written\n";
        return;
    }
    write_replays(recordings, *source_read.replay, facts);
    if (which == "packs") {
        const fs::path maps_root =
            user_folder_ / std::string(package_install::oamap::maps_folder_name);
        const fs::path hand_folder = maps_root / "ridge-reach";
        extract_pack(fs::current_path() / "ridge-reach.oamap", hand_folder);
        std::string digest_error;
        const std::optional<sha::Digest> hand_digest =
            oa::data::mutators::folder_digest(hand_folder, &digest_error);
        require(hand_digest.has_value(), "the hand-copied pack has no digest: " + digest_error);
        const auto hand_hex = sha::to_hex(*hand_digest);
        store_pack_replay(
            recordings / "default" / "hand.oademo",
            *source_read.replay,
            std::string(hand_hex.data(), hand_hex.size())
        );
        open_replays();
        const auto [hand_chip, hand_note] = show("default/hand.oademo");
        static_cast<void>(hand_note);
        require(hand_chip.empty(), "the hand-copied pack shows \"" + hand_chip + "\"");
        const std::optional<AutomationControl> hand_button = named("oa.library.action.watch");
        require(hand_button && hand_button->text == "Watch", "the hand-copied pack is not Watch");
        click_named("oa.library.action.watch");
        until(
            kDownloadWaitMs,
            [&] { return screen_ == Screen::match && match_ != nullptr; },
            "the hand-copied pack did not play"
        );
        const PackMap* hand_mounted = mounted_pack_map();
        require(hand_mounted != nullptr, "the hand-copied pack was not mounted");
        const std::string hand_mounted_folder = platform::path_to_utf8(hand_mounted->folder);
        require(
            hand_mounted_folder.find("Replay releases") == std::string::npos,
            "the hand-copied pack was fetched"
        );
        require(
            hand_mounted_folder.find("Maps") != std::string::npos,
            "the hand-copied pack was not played from Maps"
        );
        const uint32_t hand_tick = match_->state().game.tick;
        for (int frame = 0; frame < kMostFrames && match_ != nullptr &&
                            match_->state().game.tick < hand_tick + kWatchedTicks;
             ++frame) {
            static_cast<void>(run_granted_ticks(1));
            run_frame(running);
            require(running, "a frame ended the run");
        }
        require(
            match_ != nullptr && match_->state().game.tick >= hand_tick + kWatchedTicks,
            "the hand-copied pack did not play its ticks"
        );
        std::cout << "replays content: hand played with no fetch\n";
        leave_match();
        load(Screen::main_menu);
        frames(5);

        extract_pack(fs::current_path() / "ridge-reach-new.oamap", hand_folder);
        const std::optional<sha::Digest> newer = sha::parse_hex(fact(facts, "pack_new_sha"));
        require(newer.has_value(), "the newer pack's digest is not hex");
        int64_t newer_release = 0;
        const std::string release_text = fact(facts, "pack_new_release");
        require(!release_text.empty(), "the newer pack's release is not named");
        for (const char digit : release_text) {
            require(digit >= '0' && digit <= '9', "the newer pack's release is not a number");
            newer_release = newer_release * 10 + (digit - '0');
        }
        require(newer_release > 0, "the newer pack's release is empty");
        package_install::Origin record;
        record.kind = package_install::OriginKind::catalogue;
        record.registry = fact(facts, "registry");
        record.catalogue_id = std::string(kPackId);
        record.release = newer_release;
        record.sha256 = *newer;
        record.installed = "2026-11-01";
        const std::string origin_record = package_install::origin_text(record);
        require(!origin_record.empty(), "the newer pack's origin was not written");
        write_text(hand_folder / std::string(package_install::origin_file_name), origin_record);
        const std::string installed_before = pack_fingerprint(hand_folder);
        store_pack_replay(
            recordings / "default" / "older.oademo", *source_read.replay, fact(facts, "pack_sha")
        );
        open_replays();
        until(
            kDownloadWaitMs,
            [&] {
                if (library::find_replay(host.model, "default/older.oademo") == nullptr)
                    return false;
                const auto [older_chip, older_note] = show("default/older.oademo");
                const std::optional<AutomationControl> watch = named("oa.library.action.watch");
                return older_chip == "Needs map" &&
                       older_note.find("another release of the map pack Ridge Reach") !=
                           std::string::npos &&
                       watch && watch->text == "Get Ridge Reach 1.0 and watch";
            },
            "the older pack did not offer Get Ridge Reach 1.0 and watch"
        );
        click_named("oa.library.action.watch");
        until(
            kDownloadWaitMs,
            [&] { return screen_ == Screen::match && match_ != nullptr; },
            "the older pack did not play"
        );
        const PackMap* older_mounted = mounted_pack_map();
        require(older_mounted != nullptr, "the older pack was not mounted");
        const std::string older_folder = platform::path_to_utf8(older_mounted->folder);
        const std::string old_sha = fact(facts, "pack_sha");
        require(
            older_folder.find("Replay releases") != std::string::npos &&
                older_folder.find(old_sha) != std::string::npos,
            "the older pack was not played from Replay releases"
        );
        const uint32_t older_tick = match_->state().game.tick;
        for (int frame = 0; frame < kMostFrames && match_ != nullptr &&
                            match_->state().game.tick < older_tick + kWatchedTicks;
             ++frame) {
            static_cast<void>(run_granted_ticks(1));
            run_frame(running);
            require(running, "a frame ended the run");
        }
        require(
            match_ != nullptr && match_->state().game.tick >= older_tick + kWatchedTicks,
            "the older pack did not play its ticks"
        );
        require(pack_fingerprint(hand_folder) == installed_before, "the installed pack changed");
        const std::vector<uint8_t> origin_bytes =
            read_bytes(hand_folder / std::string(package_install::origin_file_name));
        const std::string origin_after(origin_bytes.begin(), origin_bytes.end());
        require(
            origin_after.find(fact(facts, "pack_new_sha")) != std::string::npos,
            "the installed pack's record changed"
        );
        std::cout << "replays content: older pack playing from Replay releases\n";
        std::cout << "replays content: installed pack unchanged\n";
        leave_match();
        load(Screen::main_menu);
        frames(5);

        const std::string pack_map_name = std::string(kMapStem) + "@" + std::string(kPackId);
        const auto watch_kept = [&](std::string_view path, bool from_releases) {
            open_replays();
            const auto [chip, note] = show(path);
            static_cast<void>(chip);
            static_cast<void>(note);
            const std::optional<AutomationControl> button = named("oa.library.action.watch");
            require(button && button->text == "Watch", std::string(path) + " is not Watch");
            click_named("oa.library.action.watch");
            until(
                kDownloadWaitMs,
                [&] { return screen_ == Screen::match && match_ != nullptr; },
                "the replay did not play"
            );
            const PackMap* mounted = mounted_pack_map();
            require(mounted != nullptr, "the pack was not mounted");
            const std::string folder = platform::path_to_utf8(mounted->folder);
            if (from_releases) {
                require(
                    folder.find("Replay releases") != std::string::npos &&
                        folder.find(old_sha) != std::string::npos,
                    "the replay was not played from Replay releases"
                );
            } else {
                require(
                    folder.find("Maps") != std::string::npos &&
                        folder.find("Replay releases") == std::string::npos,
                    "the replay was not played from Maps"
                );
            }
            const uint32_t began_tick = match_->state().game.tick;
            for (int frame = 0; frame < kMostFrames && match_ != nullptr &&
                                match_->state().game.tick < began_tick + kWatchedTicks;
                 ++frame) {
                static_cast<void>(run_granted_ticks(1));
                run_frame(running);
                require(running, "a frame ended the run");
            }
            require(
                match_ != nullptr && match_->state().game.tick >= began_tick + kWatchedTicks,
                "the replay did not play its ticks"
            );
            leave_match();
            load(Screen::main_menu);
            frames(5);
        };

        write_bytes(hand_folder / "maps" / "ridge_reach.tnt", std::vector<uint8_t>{1, 2, 3, 4});
        map_packs().refresh();
        drop_pack_map_fits();
        refresh_listed_pack_maps();
        require(
            pack_map_blocked_reason(pack_map_name) != nullptr,
            "the damaged pack in Maps was not greyed"
        );
        watch_kept("default/older.oademo", true);
        std::cout << "replays content: greyed pack played from Replay releases\n";

        open_replays();
        library::select_replay(host.model, "default/older.oademo");
        ++host.model_revision;
        until(
            kDownloadWaitMs,
            [&] {
                const library::ReplayRow* indexed =
                    library::find_replay(host.model, "default/older.oademo");
                return indexed != nullptr && indexed->replay.timeline.has_value() &&
                       !indexed->replay.timeline->empty();
            },
            "the index pass did not read the kept pack"
        );
        std::cout << "replays content: index pass read default/older.oademo\n";

        {
            std::error_code removed;
            fs::remove_all(hand_folder, removed);
            require(!removed, "the pack in Maps was not uninstalled");
        }
        watch_kept("default/older.oademo", true);
        std::cout << "replays content: kept pack played with nothing in Maps\n";

        // A replay downloaded from Core Prime whose map is in the kept pack
        // only. The bar's own read mounts that pack, as Watch did.
        store_pack_replay(under(recordings, kKeptDownload), *source_read.replay, old_sha);
        open_replays();
        {
            const auto [download_chip, download_note] = show(kKeptDownload);
            static_cast<void>(download_note);
            require(download_chip.empty(), "the download shows \"" + download_chip + "\"");
            const library::ReplayRow* download_row =
                library::find_replay(host.model, kKeptDownload);
            require(
                download_row != nullptr && !download_row->replay.timeline.has_value(),
                "the download was read before Watch"
            );
            const std::optional<AutomationControl> download_watch =
                named("oa.library.action.watch");
            require(download_watch && download_watch->text == "Watch", "the download is not Watch");
        }
        click_named("oa.library.action.watch");
        until(
            kDownloadWaitMs,
            [&] { return screen_ == Screen::match && match_ != nullptr; },
            "the download did not play"
        );
        {
            const PackMap* download_mounted = mounted_pack_map();
            require(download_mounted != nullptr, "the download's pack was not mounted");
            const std::string download_folder = platform::path_to_utf8(download_mounted->folder);
            require(
                download_folder.find("Replay releases") != std::string::npos &&
                    download_folder.find(old_sha) != std::string::npos,
                "the download was not played from Replay releases"
            );
        }
        until(
            kDownloadWaitMs,
            [&] { return replay_viewer_command("watch.replay-pause"); },
            "the download did not pause"
        );
        until(
            kBarPassStartMs,
            [&] { return replay_index_pid() != 0; },
            "the bar's read of the download did not start"
        );
        until(
            kBarPassEndMs,
            [&] { return replay_index_pid() == 0; },
            "the bar's read of the download did not end"
        );
        require(
            !host.timeline_of(*this, std::string(kKeptDownload)).empty(),
            "the bar's read did not mount the kept pack"
        );
        std::cout << "replays content: bar read " << kKeptDownload << " with the kept pack\n";
        leave_match();
        load(Screen::main_menu);
        frames(5);

        {
            std::error_code removed;
            fs::remove_all(
                user_folder_ / std::string(package_install::replay_releases_folder_name), removed
            );
        }
        extract_pack(fs::current_path() / "ridge-reach.oamap", hand_folder);
        const std::optional<sha::Digest> file_sha = sha::parse_hex(old_sha);
        require(file_sha.has_value(), "the file pack's digest is not hex");
        package_install::Origin file_origin;
        file_origin.kind = package_install::OriginKind::file;
        file_origin.sha256 = *file_sha;
        file_origin.installed = "2026-11-01";
        const std::string file_record = package_install::origin_text(file_origin);
        require(!file_record.empty(), "the file origin was not written");
        write_text(hand_folder / std::string(package_install::origin_file_name), file_record);
        store_pack_replay(recordings / "default" / "file.oademo", *source_read.replay, old_sha);
        watch_kept("default/file.oademo", false);
        std::cout << "replays content: file origin watched with no fetch\n";

        extract_pack(fs::current_path() / "ridge-reach-new.oamap", hand_folder);
        package_install::Origin newer_record;
        newer_record.kind = package_install::OriginKind::catalogue;
        newer_record.registry = fact(facts, "registry");
        newer_record.catalogue_id = std::string(kPackId);
        newer_record.release = newer_release;
        newer_record.sha256 = *newer;
        newer_record.installed = "2026-11-01";
        const std::string newer_text = package_install::origin_text(newer_record);
        require(!newer_text.empty(), "the newer pack's origin was not written again");
        write_text(hand_folder / std::string(package_install::origin_file_name), newer_text);
        {
            std::error_code removed;
            fs::remove_all(
                user_folder_ / std::string(package_install::replay_releases_folder_name), removed
            );
        }
        const fs::path ridge = user_folder_ / std::string(user_mods_folder_name) / "ridge";
        const std::vector<uint8_t> model(std::begin(kRidgeModel), std::end(kRidgeModel));
        write_text(ridge / "oamod.yaml", ridge_profile("2.9"));
        write_text(ridge / "units" / "RGNEW.fbi", kRidgeUnit);
        write_bytes(ridge / "objects3d" / "RGNEW.3DO", model);
        const std::optional<sha::Digest> ridge_digest =
            sha::parse_hex(fact(facts, "ridge_new_sha"));
        require(ridge_digest.has_value(), "Ridge 2.9's digest is not hex");
        package_install::Origin ridge_origin;
        ridge_origin.kind = package_install::OriginKind::catalogue;
        ridge_origin.registry = fact(facts, "registry");
        ridge_origin.catalogue_id = "ridge";
        ridge_origin.release = 3;
        ridge_origin.sha256 = *ridge_digest;
        ridge_origin.installed = "2026-11-01";
        const std::string ridge_record = package_install::origin_text(ridge_origin);
        require(!ridge_record.empty(), "Ridge 2.9's origin was not written");
        write_text(ridge / std::string(package_install::origin_file_name), ridge_record);
        write_text(user_folder_ / std::string(kKeptName), "2.9\n");
        remember_mod_directory(preference_values_, ridge);
        preferences_dirty_ = true;
        flush_preferences();
        const std::vector<uint8_t> ridge_bytes =
            read_bytes(recordings / "default" / "ridge.oademo");
        const oademo::ReadResult ridge_read = oademo::read(ridge_bytes);
        require(ridge_read.ok(), "ridge.oademo does not read");
        store_pack_and_ridge(
            recordings / "default" / "both-pack.oademo", *ridge_read.replay, old_sha
        );
        write_phase(user_folder_, "both-ready");
        request_soft_restart();
        return;
    }
    if (which == "update") {
        // Release 1 is the catalogue's Ridge 2.8. No Mod stays chosen.
        const fs::path ridge = user_folder_ / std::string(user_mods_folder_name) / "ridge";
        const std::vector<uint8_t> model(std::begin(kRidgeModel), std::end(kRidgeModel));
        write_text(ridge / "oamod.yaml", ridge_profile("2.8"));
        write_text(ridge / "units" / "RGNEW.fbi", kRidgeUnit);
        write_bytes(ridge / "objects3d" / "RGNEW.3DO", model);
        const std::optional<sha::Digest> digest = sha::parse_hex(fact(facts, "ridge_sha"));
        require(digest.has_value(), "Ridge 2.8's digest is not hex");
        package_install::Origin record;
        record.kind = package_install::OriginKind::catalogue;
        record.registry = fact(facts, "registry");
        record.catalogue_id = "ridge";
        record.release = 1;
        record.sha256 = *digest;
        record.installed = "2026-11-01";
        const std::string origin_record = package_install::origin_text(record);
        require(!origin_record.empty(), "Ridge 2.8's origin was not written");
        write_text(ridge / std::string(package_install::origin_file_name), origin_record);
        write_phase(user_folder_, "ready");
        request_soft_restart();
        std::cout << "replays content: Ridge 2.8 installed\n";
        return;
    }
    if (which == "beside") {
        // Ridge 2.9 is already what the player uses. The replay's 2.8 is
        // fetched later, into Replay releases, and this folder is put back.
        const fs::path ridge = user_folder_ / std::string(user_mods_folder_name) / "ridge";
        const std::vector<uint8_t> model(std::begin(kRidgeModel), std::end(kRidgeModel));
        write_text(ridge / "oamod.yaml", ridge_profile("2.9"));
        write_text(ridge / "units" / "RGNEW.fbi", kRidgeUnit);
        write_bytes(ridge / "objects3d" / "RGNEW.3DO", model);
        const std::optional<sha::Digest> digest = sha::parse_hex(fact(facts, "ridge_new_sha"));
        require(digest.has_value(), "Ridge 2.9's digest is not hex");
        package_install::Origin record;
        record.kind = package_install::OriginKind::catalogue;
        record.registry = fact(facts, "registry");
        record.catalogue_id = "ridge";
        record.release = 3;
        record.sha256 = *digest;
        record.installed = "2026-11-01";
        const std::string origin_record = package_install::origin_text(record);
        require(!origin_record.empty(), "Ridge 2.9's origin was not written");
        write_text(ridge / std::string(package_install::origin_file_name), origin_record);
        write_text(user_folder_ / std::string(kKeptName), "2.9\n");
        remember_mod_directory(preference_values_, ridge);
        preferences_dirty_ = true;
        flush_preferences();
        write_phase(user_folder_, "installed");
        request_soft_restart();
        std::cout << "replays content: Ridge 2.9 installed\n";
        return;
    }
    open_replays();
    both_not_read();
    {
        const uint64_t began = SDL_GetTicks();
        while (SDL_GetTicks() - began < 2000) {
            run_frame(running);
            require(running, "a frame ended the run");
            both_not_read();
        }
    }
    std::cout << "replays content: both.oademo was not read while ridge-wind was missing\n";

    if (which == "offline" && cached_press) {
        const auto [chip, note] = show("default/both.oademo");
        static_cast<void>(note);
        require(chip == "Needs map", "the cached chip is \"" + chip + "\"");
        const std::optional<AutomationControl> get_cached = named("oa.library.action.watch");
        require(
            get_cached && get_cached->text == "Get them and watch",
            "the cached button is not Get them and watch"
        );
        const uint64_t began = SDL_GetTicks();
        click_named("oa.library.action.watch");
        until(
            5000,
            [&] {
                const library::ReplayRow* row =
                    library::find_replay(host.model, "default/both.oademo");
                if (row == nullptr)
                    return false;
                const std::string said = library::replay_note(host.model, *row);
                return said.find("You're offline.") != std::string::npos &&
                       !named("oa.library.action.watch");
            },
            "the press did not say you're offline"
        );
        require(SDL_GetTicks() - began < 5000, "offline took 5s or more");
        const auto [after_chip, after_note] = show("default/both.oademo");
        require(after_chip == "Needs map", "the chip was dropped while offline");
        require(
            after_note.find("You're offline.") != std::string::npos,
            "the card does not say you're offline"
        );
        require(
            after_note.find("Ridge Wind") != std::string::npos,
            "the offline card does not name Ridge Wind"
        );
        require(
            after_note.find("Ridge Reach") != std::string::npos,
            "the offline card does not name Ridge Reach"
        );
        require(!named("oa.library.action.watch"), "Watch shows after the registry was offline");
        std::cout << "replays content: offline after a press \"" << after_note << "\"\n";
        return;
    }

    if (which == "offline") {
        const auto [chip, note] = show("default/both.oademo");
        require(chip == "Needs map", "the offline chip is \"" + chip + "\"");
        require(
            note.find("You're offline.") != std::string::npos,
            "the card does not say you're offline"
        );
        require(
            note.find("Ridge Wind") != std::string::npos,
            "the offline card does not name Ridge Wind"
        );
        require(
            note.find("Ridge Reach") != std::string::npos,
            "the offline card does not name Ridge Reach"
        );
        require(!named("oa.library.action.watch"), "Watch shows while the registry is offline");
        snapshot("offline-1280x720");
        std::cout << "replays content: offline reads \"" << note << "\"\n";
        return;
    }

    const auto [map_chip, map_note] = show("default/both.oademo");
    require(map_chip == "Needs map", "the pack's chip is \"" + map_chip + "\"");
    require(map_note.find("Ridge Reach") != std::string::npos, "the card does not name the map");
    require(map_note.find("Ridge Wind") != std::string::npos, "the card does not name the mutator");
    const std::optional<AutomationControl> get_them = named("oa.library.action.watch");
    require(
        get_them && get_them->text == "Get them and watch", "the button is not Get them and watch"
    );
    snapshot("needs-map-1280x720");

    const auto [ridge_chip, ridge_note] = show("default/ridge.oademo");
    require(ridge_chip == "Needs Ridge 2.8", "Ridge's chip is \"" + ridge_chip + "\"");
    require(
        ridge_note.find("This replay was recorded with Ridge 2.8") != std::string::npos,
        "the card does not say the replay was recorded with Ridge 2.8"
    );
    snapshot("needs-ridge-1280x720");

    const auto [engine_chip, engine_note] = show("default/engine.oademo");
    require(engine_chip == "Needs 99.0.0", "the engine chip is \"" + engine_chip + "\"");
    require(
        engine_note.find("Needs Open Annihilation 99.0.0") != std::string::npos,
        "the card does not say it needs Open Annihilation 99.0.0"
    );
    const std::optional<AutomationControl> updates = named("oa.library.action.watch");
    require(updates && updates->text == "Check for updates", "the button is not Check for updates");
    snapshot("needs-engine-1280x720");

    const auto [unlisted_chip, unlisted_note] = show("default/unlisted.oademo");
    require(unlisted_chip == "Needs Ridge 2.8", "the unlisted chip is \"" + unlisted_chip + "\"");
    require(
        unlisted_note.find("not in Core Prime's catalogue") != std::string::npos,
        "the card does not say the release is not in the catalogue"
    );
    require(!named("oa.library.action.watch"), "Watch shows for a release that is not listed");
    snapshot("unlisted-1280x720");

    const auto [gone_chip, gone_note] = show("default/gone.oademo");
    require(gone_chip == "Needs map", "the 409 chip is \"" + gone_chip + "\"");
    static_cast<void>(gone_note);
    const std::optional<AutomationControl> get_gone = named("oa.library.action.watch");
    require(
        get_gone && get_gone->text == "Get it and watch", "the 409 button is not Get it and watch"
    );
    std::string gone_after;
    click_named("oa.library.action.watch");
    until(
        kDownloadWaitMs,
        [&] {
            const library::ReplayRow* row = library::find_replay(host.model, "default/gone.oademo");
            if (row == nullptr)
                return false;
            gone_after = library::replay_note(host.model, *row);
            return gone_after.find("not in Ridge Replays's catalogue") != std::string::npos &&
                   !named("oa.library.action.watch");
        },
        "a 409 left Get it and watch"
    );
    frames(3);
    require(!named("oa.library.action.watch"), "Watch came back after the 409");
    std::cout << "replays content: 409 left no Watch\n";
    std::cout << "replays content: 409 \"" << gone_after << "\"\n";

    click_named(row_of("default/both.oademo"));
    bool saw_fetch = false;
    bool named_reach = false;
    bool named_id = false;
    std::string last_getting;
    click_named("oa.library.action.watch");
    until(
        kDownloadWaitMs,
        [&] {
            if (const library::ReplayRow* row =
                    library::find_replay(host.model, "default/both.oademo")) {
                const std::string caution = library::replay_caution(host.model, *row);
                if (caution.find("Getting") != std::string::npos) {
                    saw_fetch = true;
                    if (caution.find("Getting Ridge Reach") != std::string::npos)
                        named_reach = true;
                    if (caution.find("Getting ridge-reach") != std::string::npos)
                        named_id = true;
                    if (caution != last_getting) {
                        last_getting = caution;
                        std::cout << "replays content: " << caution << '\n';
                    }
                }
            }
            if (const std::optional<AutomationControl> fetching =
                    named("oa.library.action.watch")) {
                if (fetching->text == "Cancel")
                    saw_fetch = true;
            }
            return screen_ == Screen::match && match_ != nullptr;
        },
        "Get them and watch did not start the replay"
    );
    require(saw_fetch, "the card did not say the packages were being fetched");
    require(named_reach, "the fetch did not name Ridge Reach");
    require(!named_id, "the fetch named the pack by its id");
    std::cout << "replays content: Getting\n";
    // The match can begin in the same frame as the Library's tick, which has
    // already run. The next tick closes the Library, as it does for Watch.
    until(
        kDownloadWaitMs,
        [&] { return oa_layer().find(library_screen_name) == nullptr; },
        "the Library stayed over the replay"
    );
    require(chosen_match_mutators().size() == 1, "the replay did not apply its mutator");
    std::cout << "replays content: watching\n";
    const uint32_t first_tick = match_->state().game.tick;
    for (int frame = 0; frame < kMostFrames && match_ != nullptr &&
                        match_->state().game.tick < first_tick + kWatchedTicks;
         ++frame) {
        static_cast<void>(run_granted_ticks(1));
        run_frame(running);
        require(running, "a frame ended the run");
    }
    require(
        match_ != nullptr && match_->state().game.tick >= first_tick + kWatchedTicks,
        "the replay did not play its ticks"
    );
    leave_match();
    load(Screen::main_menu);
    require(chosen_match_mutators().empty(), "the mutator stayed selected after the replay");
    std::cout << "replays content: mutators selected " << chosen_match_mutators().size() << '\n';

    open_replays();
    std::string both_said;
    until(
        180000,
        [&] {
            const library::ReplayRow* row = library::find_replay(host.model, "default/both.oademo");
            if (row == nullptr || !row->replay.timeline.has_value() ||
                row->replay.timeline->empty())
                return false;
            bool known = row->recorded != library::RecordedResult::none;
            for (const library::PlayerStanding& standing : row->standings)
                if (standing.known)
                    known = true;
            if (!known)
                return false;
            if (row->recorded == library::RecordedResult::won)
                both_said = "Won";
            else if (row->recorded == library::RecordedResult::lost)
                both_said = "Lost";
            else
                both_said = "no side";
            for (const library::PlayerStanding& standing : row->standings) {
                if (!standing.known)
                    continue;
                both_said += ", " + std::to_string(standing.kills) + " kills, " +
                             std::to_string(standing.losses) + " losses";
                break;
            }
            return true;
        },
        "both.oademo was not read after ridge-wind was installed"
    );
    std::cout << "replays content: both.oademo shows " << both_said
              << " after ridge-wind was installed\n";
    const auto [ridge_again_chip, ridge_again_note] = show("default/ridge.oademo");
    require(ridge_again_chip == "Needs Ridge 2.8", "Ridge's chip is \"" + ridge_again_chip + "\"");
    static_cast<void>(ridge_again_note);
    const std::optional<AutomationControl> get_ridge = named("oa.library.action.watch");
    require(
        get_ridge && get_ridge->text == "Get Ridge 2.8 and watch",
        "the button is not Get Ridge 2.8 and watch"
    );
    write_phase(user_folder_, "ridge");
    click_named("oa.library.action.watch");
    until(
        kDownloadWaitMs,
        [&] { return soft_restart_requested(); },
        "Get Ridge 2.8 and watch did not restart"
    );
}

} // namespace oa::app

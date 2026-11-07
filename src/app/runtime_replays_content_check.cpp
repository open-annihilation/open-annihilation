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
// replay and one that ended before the replay began.
//
// Get Ridge 2.8 and watch, Watch of the installed Ridge, and Turn on and
// watch, end through the replay's own end. The end screen stays until Main
// Menu. The restart that follows puts the player's choice back and opens
// the Library on Replays with that replay chosen. The start after it plays
// nothing.

#include "library_host.hpp"
#include "oa_layer.hpp"
#include "oa_layer_check.hpp"

#include "oa/app/automation_host.hpp"
#include "oa/app/hook_call.hpp"
#include "oa/app/content/downloads.hpp"
#include "oa/app/content/service.hpp"
#include "oa/app/game_directory.hpp"
#include "oa/app/runtime.hpp"
#include "oa/data/content_bundles/content_choice.hpp"
#include "oa/data/saves/content_record.hpp"
#include "oa/app/user_folder.hpp"
#include "oa/base/sha256.hpp"
#include "oa/data/languages/interface_text.hpp"
#include "oa/data/map_pack/manifest.hpp"
#include "oa/data/mutators/loadout.hpp"
#include "oa/formats/oademo/oademo.hpp"
#include "oa/formats/oamut/manifest.hpp"
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
constexpr int kMostFrames = 600;
constexpr uint32_t kWatchedTicks = 30;
constexpr uint64_t kRegistryWaitMs = 45000;
constexpr uint64_t kDownloadWaitMs = 90000;
constexpr std::string_view kPackId = "ridge-reach";
constexpr std::string_view kMapStem = "ridge_reach";
constexpr std::string_view kMapTitle = "Ridge Reach";
constexpr std::string_view kMutatorId = "ridge-wind";
constexpr std::string_view kMutatorName = "Ridge Wind";

void require(bool ok, std::string_view what) {
    if (!ok)
        throw std::runtime_error("replays content check: " + std::string(what));
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

/// A 4 by 4 map whose one feature is the pack's own ridgerock.
std::vector<uint8_t> tiny_tnt() {
    constexpr std::size_t tile_map_at = 64;
    constexpr std::size_t attributes_at = 72;
    constexpr std::size_t tiles_at = 136;
    const std::size_t features_at = tiles_at + 2 * tnt::layout::tile_bytes;
    std::vector<uint8_t> bytes(features_at + tnt::layout::feature_record_bytes);
    const auto put32 = [&bytes](std::size_t at, uint32_t value) {
        bytes[at] = static_cast<uint8_t>(value);
        bytes[at + 1] = static_cast<uint8_t>(value >> 8U);
        bytes[at + 2] = static_cast<uint8_t>(value >> 16U);
        bytes[at + 3] = static_cast<uint8_t>(value >> 24U);
    };
    const auto put16 = [&bytes](std::size_t at, uint16_t value) {
        bytes[at] = static_cast<uint8_t>(value);
        bytes[at + 1] = static_cast<uint8_t>(value >> 8U);
    };
    put32(0, static_cast<uint32_t>(tnt::Version::total_annihilation));
    put32(4, 4);
    put32(8, 4);
    put32(12, static_cast<uint32_t>(tile_map_at));
    put32(16, static_cast<uint32_t>(attributes_at));
    put32(20, static_cast<uint32_t>(tiles_at));
    put32(24, 2);
    put32(28, 1);
    put32(32, static_cast<uint32_t>(features_at));
    put32(36, 17);
    put16(tile_map_at, 0);
    put16(tile_map_at + 2, 1);
    put16(tile_map_at + 4, 1);
    put16(tile_map_at + 6, 0);
    constexpr std::string_view feature = "ridgerock";
    const std::size_t name_at = features_at + tnt::layout::feature_name_offset;
    for (std::size_t index = 0; index < feature.size(); ++index)
        bytes[name_at + index] = static_cast<uint8_t>(feature[index]);
    return bytes;
}

std::string tiny_ota() {
    return "[GlobalHeader]\n"
           "{\n"
           "[Schema 0]\n"
           "{\n"
           "Type=Network 1;\n"
           "[features]\n"
           "{\n"
           "[feature0]\n"
           "{\n"
           "Featurename=ridgerock;\n"
           "XPos=0;\n"
           "ZPos=0;\n"
           "}\n"
           "}\n"
           "}\n"
           "[Schema 1]\n"
           "{\n"
           "Type=Network 2;\n"
           "[units]\n"
           "{\n"
           "[unit0]\n"
           "{\n"
           "Unitname=ARMCOM;\n"
           "}\n"
           "}\n"
           "}\n"
           "}\n";
}

pack::Manifest pack_manifest() {
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
    map.size = "4x4";
    map.preview = "previews/ridge_reach.png";
    map.files = {"maps/ridge_reach.ota", "maps/ridge_reach.tnt", "features/ridge/rock.tdf"};
    manifest.maps.push_back(std::move(map));
    return manifest;
}

void write_pack(const fs::path& file, uint8_t colour) {
    const std::string yaml = pack::write_manifest(pack_manifest());
    const std::string ota = tiny_ota();
    const std::string feature = "[ridgerock]\n{\ndescription=Ridge rock;\n}\n";
    const std::vector<uint8_t> tnt_bytes = tiny_tnt();
    const std::vector<uint8_t> preview = preview_png(colour);
    const std::vector<uint8_t> yaml_bytes(yaml.begin(), yaml.end());
    const std::vector<uint8_t> ota_bytes(ota.begin(), ota.end());
    const std::vector<uint8_t> feature_bytes(feature.begin(), feature.end());
    const zip::NewEntry entries[] = {
        {"oamap.yaml", yaml_bytes},
        {"maps/ridge_reach.ota", ota_bytes},
        {"maps/ridge_reach.tnt", tnt_bytes},
        {"features/ridge/rock.tdf", feature_bytes},
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

void write_packages() {
    const fs::path folder = fs::current_path();
    write_pack(folder / "ridge-reach.oamap", 40);
    write_pack(folder / "ridge-reach-new.oamap", 200);
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

bool lists_id(const std::vector<std::string>& ids, std::string_view id) {
    for (const std::string& one : ids)
        if (one == id)
            return true;
    return false;
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

} // namespace

void Runtime::check_replays_content() {
    require(sdl_.window != nullptr, "the check needs the SDL presenter");
    require(!user_folder_.empty(), "the run has no folder of the player's own");
    const std::map<std::string, std::string> facts = read_facts();
    const std::string which = fact(facts, "case");
    if (which == "prepare") {
        write_packages();
        return;
    }
    require(
        which == "play" || which == "offline" || which == "expansion",
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
            return;
        }
        throw std::runtime_error("replays content check: the expansion phase is unknown");
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

    const auto cached_found = facts.find("cached");
    const bool cached_press = cached_found != facts.end() && cached_found->second == "press";
    const std::string registry_id = fact(facts, "registry");
    if (which == "play") {
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
    write_replays(recordings, *source_read.replay, facts);
    open_replays();

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
    click_named("oa.library.action.watch");
    until(
        kDownloadWaitMs,
        [&] {
            const library::ReplayRow* row = library::find_replay(host.model, "default/gone.oademo");
            if (row == nullptr)
                return false;
            const std::string note = library::replay_note(host.model, *row);
            return note.find("not in Core Prime's catalogue") != std::string::npos &&
                   !named("oa.library.action.watch");
        },
        "a 409 left Get it and watch"
    );
    frames(3);
    require(!named("oa.library.action.watch"), "Watch came back after the 409");
    std::cout << "replays content: 409 left no Watch\n";

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

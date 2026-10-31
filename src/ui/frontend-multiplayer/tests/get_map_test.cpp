// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// GET MAP in the battle room. Without arguments: every state and every line
// over hand-made records, packs and offers. With --data, the battle room's
// side over the installed game's GUI files: what a joiner reads of the
// host's map and its record, the host's pack as network play binds it
// (multiplayer_bind_map_source, then multiplayer_bind_map_pack), and a pack
// map the battle room finds once it is installed, its files read again.

#include "oa/formats/hpi.hpp"
#include "oa/netgame/presence.hpp"
#include "oa/netgame/records.hpp"
#include "oa/test/game_assets.hpp"
#include "oa/test/game_data.hpp"
#include "oa/ui/frontend_multiplayer/get_map.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"
#include "oa/ui/frontend_state/oa_button_place.hpp"
#include "oa/ui/frontend_renderer.hpp"

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

namespace mp = oa::ui::frontend_multiplayer;
namespace ng = oa::netgame;
using mp::GetMap;
using oa::app::ScreenId;

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        ++failures;
    }
}

/// The pack map the cases play: the stem isle_of_ashes of the pack archipelago.
constexpr const char* kPackMap = "isle_of_ashes@archipelago";
/// The host's release's SHA-256: 32 bytes of 0x22.
constexpr const char* kHostSha = "2222222222222222222222222222222222222222222222222222222222222222";
/// Another release's SHA-256: 32 bytes of 0x11.
constexpr const char* kOtherSha =
    "1111111111111111111111111111111111111111111111111111111111111111";
/// 41.0 MB in the Library's units.
constexpr uint64_t kPackBytes = 43011223;

ng::PresenceDigest filled(uint8_t byte) {
    ng::PresenceDigest digest{};
    digest.fill(byte);
    return digest;
}

/// The host's map pack as its record names it.
ng::PresenceMapPack record_pack() {
    return {
        "archipelago",
        3,
        filled(0x22),
        "example-maps",
        "http://127.0.0.1:1/example-maps.yaml",
        "Archipelago Pack",
        "1.2"
    };
}

/// A registry the player has, which lists the host's release.
mp::Offer offering() {
    mp::Offer offer;
    offer.registry_known = true;
    offer.registry_enabled = true;
    offer.downloads_on = true;
    offer.catalogue_loaded = true;
    offer.listed = true;
    offer.sha256 = kHostSha;
    offer.pack_name = "Archipelago Pack";
    offer.version = "1.2";
    offer.registry_name = "Example Maps";
    offer.release = 3;
    offer.bytes = kPackBytes;
    return offer;
}

/// Another release of the pack, installed from the same registry.
mp::LocalPack other_release() {
    mp::LocalPack local;
    local.installed = true;
    local.sha256 = kOtherSha;
    local.registry_id = "example-maps";
    local.release = 2;
    return local;
}

GetMap state_of(
    const std::optional<mp::HostPack>& host,
    const mp::LocalPack& local,
    const mp::Offer& offer,
    std::string_view map = kPackMap,
    bool downloading = false,
    bool failed = false
) {
    return mp::get_map_state(map, host, local, offer, downloading, failed);
}

void the_host_pack_from_its_record() {
    const mp::HostPack host = mp::host_pack_of(record_pack());
    expect(
        host.id == "archipelago" && host.sha256 == kHostSha && host.registry_id == "example-maps" &&
            host.registry_url == "http://127.0.0.1:1/example-maps.yaml" &&
            host.name == "Archipelago Pack" && host.version == "1.2" && host.release == 3,
        "the record's key, SHA-256 as hex, registry, address, name, version and release"
    );
}

void every_state() {
    const std::optional<mp::HostPack> host = mp::host_pack_of(record_pack());
    const mp::LocalPack missing{};
    const mp::Offer offer = offering();

    expect(state_of(host, missing, offer) == GetMap::get, "a pack not installed, offered: get");
    expect(
        state_of(host, missing, offer, "Coast To Coast") == GetMap::none, "a base map's name: none"
    );
    expect(state_of(std::nullopt, missing, offer) == GetMap::none, "no record from the host: none");
    std::optional<mp::HostPack> another = host;
    another->id = "islands";
    expect(state_of(another, missing, offer) == GetMap::none, "a record for another pack: none");
    mp::LocalPack same = other_release();
    same.sha256 = kHostSha;
    expect(state_of(host, same, offer) == GetMap::none, "the host's SHA-256 installed: none");
    expect(
        state_of(host, same, offer, kPackMap, true, true) == GetMap::none,
        "the host's SHA-256 installed: none, downloading or not"
    );

    expect(
        state_of(host, missing, offer, kPackMap, true) == GetMap::downloading,
        "a download under way: downloading"
    );
    expect(
        state_of(host, missing, offer, kPackMap, true, true) == GetMap::downloading,
        "downloading comes before failed"
    );
    expect(
        state_of(host, missing, offer, kPackMap, false, true) == GetMap::failed,
        "the last try failed: failed"
    );

    mp::Offer unknown;
    expect(
        state_of(host, missing, unknown) == GetMap::add_registry_first,
        "a registry the player has not added, with its address: add_registry_first"
    );
    std::optional<mp::HostPack> no_address = host;
    no_address->registry_url.clear();
    expect(
        state_of(no_address, missing, unknown) == GetMap::gone,
        "a registry the player has not added, with no address: gone"
    );

    mp::Offer off = offer;
    off.registry_enabled = false;
    expect(
        state_of(host, missing, off) == GetMap::downloads_off, "the registry is off: downloads_off"
    );
    off = offer;
    off.downloads_on = false;
    expect(
        state_of(host, missing, off) == GetMap::downloads_off,
        "the registry's downloads are off: downloads_off"
    );

    mp::Offer unread = offer;
    unread.catalogue_loaded = false;
    unread.listed = false;
    expect(
        state_of(host, missing, unread) == GetMap::downloading,
        "a registry just added, its catalogue not read yet: downloading"
    );

    mp::Offer unlisted = offer;
    unlisted.listed = false;
    expect(state_of(host, missing, unlisted) == GetMap::gone, "the catalogue lacks the pack: gone");
    mp::Offer newer = offer;
    newer.sha256 = kOtherSha;
    newer.release = 4;
    expect(
        state_of(host, missing, newer) == GetMap::gone,
        "a release no longer offered, another listed in its place: gone"
    );

    expect(
        state_of(host, other_release(), offer) == GetMap::other_release_get,
        "another release installed, the host's offered: other_release_get"
    );
    expect(
        state_of(host, other_release(), newer) == GetMap::other_release_gone,
        "another release installed, the host's no longer offered: other_release_gone"
    );
    expect(
        state_of(host, other_release(), unknown) == GetMap::other_release_gone,
        "another release installed, its registry unknown: other_release_gone"
    );
}

/// The catalogue's word on whether this game can play the host's map.
void the_catalogue_decides_what_can_be_fetched() {
    const std::optional<mp::HostPack> host = mp::host_pack_of(record_pack());
    const mp::LocalPack missing{};
    // It fits, or the catalogue does not say: fetched as before, and the
    // installer decides.
    const mp::Offer fits = offering();
    expect(
        state_of(host, missing, fits) == GetMap::get,
        "a release the catalogue lets this game play, or says nothing of: get"
    );
    expect(
        state_of(host, other_release(), fits) == GetMap::other_release_get,
        "another release installed, the host's playable here: other_release_get"
    );

    mp::Offer needs = offering();
    needs.unplayable = "Needs Core Contingency, which is off. Turn it on in the Library.";
    expect(
        state_of(host, missing, needs) == GetMap::unplayable,
        "a release this game cannot play: unplayable, nothing to fetch"
    );
    expect(
        state_of(host, other_release(), needs) == GetMap::unplayable,
        "another release installed, the host's not playable here: unplayable"
    );
    expect(
        state_of(host, missing, needs, kPackMap, true) == GetMap::downloading,
        "a download under way still shows its bar"
    );
    expect(
        state_of(host, missing, needs, kPackMap, false, true) == GetMap::failed,
        "a try that failed still says why"
    );
    mp::LocalPack same = other_release();
    same.sha256 = kHostSha;
    expect(state_of(host, same, needs) == GetMap::none, "the host's release installed: none");
    mp::Offer gone = needs;
    gone.sha256 = kOtherSha;
    expect(
        state_of(host, missing, gone) == GetMap::gone,
        "a release no longer offered is gone, whatever its catalogue says"
    );
    expect(
        state_of(host, other_release(), gone) == GetMap::other_release_gone,
        "another release installed, the host's no longer offered: other_release_gone"
    );
    mp::Offer off = needs;
    off.downloads_on = false;
    expect(
        state_of(host, missing, off) == GetMap::downloads_off,
        "downloads off come first: downloads_off"
    );

    expect(
        mp::get_map_text(GetMap::unplayable, *host, missing, needs, 0, {}) == needs.unplayable,
        "unplayable: the map list's own sentence"
    );
    expect(!mp::get_map_button_shown(GetMap::unplayable), "unplayable shows no button");
}

void every_line() {
    const mp::HostPack host = mp::host_pack_of(record_pack());
    const mp::LocalPack missing{};
    const mp::Offer offer = offering();
    const auto line =
        [&](GetMap state, const mp::Offer& with, uint64_t done = 0, std::string_view error = {}) {
            return mp::get_map_text(state, host, other_release(), with, done, error);
        };
    expect(line(GetMap::none, offer).empty(), "none says nothing");
    expect(
        mp::get_map_text(GetMap::get, host, missing, offer, 0, {}) ==
            "Archipelago Pack 1.2 · 41.0 MB from Example Maps",
        "get: the pack, its version, its size and the registry"
    );
    mp::Offer no_version = offer;
    no_version.version.clear();
    mp::HostPack unversioned = host;
    unversioned.version.clear();
    expect(
        mp::get_map_text(GetMap::get, unversioned, missing, no_version, 0, {}) ==
            "Archipelago Pack · 41.0 MB from Example Maps",
        "get without a version drops it and its space"
    );
    expect(
        mp::get_map_text(GetMap::add_registry_first, host, missing, mp::Offer{}, 0, {}) ==
            "Add example-maps to get Archipelago Pack",
        "add_registry_first: the registry by its id, the pack by the record's name"
    );
    expect(
        line(GetMap::other_release_get, offer) ==
            "You have release 2 of Archipelago Pack; the host plays release 3.",
        "other_release_get: both releases"
    );
    expect(
        line(GetMap::other_release_gone, offer) ==
            "You have release 2 of Archipelago Pack; the host plays release 3.",
        "other_release_gone: both releases"
    );
    expect(
        mp::get_map_text(
            GetMap::other_release_get, host, mp::LocalPack{true, kOtherSha, {}, 0}, offer, 0, {}
        ) == "You have release ? of Archipelago Pack; the host plays release 3.",
        "a release from no catalogue is not known"
    );
    expect(
        mp::get_map_text(GetMap::gone, host, missing, offer, 0, {}) ==
            "The host's release of Archipelago Pack is no longer offered by Example Maps.",
        "gone: the pack and the registry"
    );
    expect(
        mp::get_map_text(GetMap::downloads_off, host, missing, offer, 0, {}) ==
            "Downloads from Example Maps are off (Settings › Downloads).",
        "downloads_off: the registry and where to turn them on"
    );
    expect(
        mp::get_map_text(GetMap::downloading, host, missing, offer, 19084083, {}) ==
            "Downloading Archipelago Pack · 18.2 MB of 41.0 MB",
        "downloading: how much of the size has arrived"
    );
    expect(
        mp::get_map_text(GetMap::downloading, host, missing, offer, 0, {}) ==
            "Downloading Archipelago Pack · 0 of 41.0 MB",
        "downloading: nothing yet"
    );
    expect(
        mp::get_map_text(
            GetMap::failed, host, missing, offer, 0, "the server could not be reached"
        ) == "Couldn't get Archipelago Pack: the server could not be reached",
        "failed: the pack and why"
    );
    mp::HostPack bare = host;
    bare.name.clear();
    expect(
        mp::get_map_text(GetMap::add_registry_first, bare, missing, mp::Offer{}, 0, {}) ==
            "Add example-maps to get archipelago",
        "a pack the record does not name is named by its id"
    );
    expect(mp::get_map_caption() == "GET MAP", "the button's caption");
    expect(
        mp::get_map_button_shown(GetMap::get) &&
            mp::get_map_button_shown(GetMap::add_registry_first) &&
            mp::get_map_button_shown(GetMap::other_release_get) &&
            mp::get_map_button_shown(GetMap::failed) && !mp::get_map_button_shown(GetMap::none) &&
            !mp::get_map_button_shown(GetMap::other_release_gone) &&
            !mp::get_map_button_shown(GetMap::gone) &&
            !mp::get_map_button_shown(GetMap::unplayable) &&
            !mp::get_map_button_shown(GetMap::downloads_off) &&
            !mp::get_map_button_shown(GetMap::downloading),
        "the button shows for get, add_registry_first, other_release_get and failed only"
    );
}

// ---------------------------------------------------------------------------
// The battle room's side, over the installed game's GUI files

struct Host {
    std::optional<ScreenId> requested;
};

void request_screen(void* host, ScreenId id) {
    static_cast<Host*>(host)->requested = id;
}

void play_sound(void*, const char*) {
}

int read_number(void*, const char*, const char*, uint32_t*) {
    return 0;
}

void write_number(void*, const char*, const char*, uint32_t) {
}

int read_string(void*, const char*, const char*, char*, std::size_t) {
    return 0;
}

void write_string(void*, const char*, const char*, const char*) {
}

void set_status(void*, const char*) {
}

void set_frontend_signal(void*, uint8_t) {
}

void set_frontend_state(void*, uint8_t) {
}

void run_frontend(void*) {
}

constexpr oa::app::ScreenServices kServices{
    request_screen,
    play_sound,
    read_number,
    write_number,
    read_string,
    write_string,
    set_status,
    set_frontend_signal,
    set_frontend_state,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    nullptr,
    run_frontend
};

/// The multiplayer screens over the installed game, driven as a player would.
struct Driver {
    oa::app::ScreenRegistry registry{};
    Host host;
    oa::AssetStore assets;
    oa::ui::frontend_renderer::Surface surface;
    oa::app::ScreenContext ctx{};
    ScreenId current = 0;

    explicit Driver(const std::filesystem::path& data) : assets(oa::test::open_game_assets(data)) {
        oa::app::register_multiplayer_screens(&registry);
        ctx = {&host, &kServices, &assets, &surface, nullptr, nullptr, 0};
    }

    const oa::app::ScreenDesc* desc(ScreenId id) { return oa::app::screen_find(&registry, id); }

    void go(ScreenId id) {
        if (const auto* previous = desc(current); previous != nullptr && previous->leave != nullptr)
            previous->leave(&ctx, previous->state);
        current = id;
        ctx.screen = id;
        host.requested.reset();
        if (const auto* next = desc(id); next != nullptr && next->enter != nullptr)
            next->enter(&ctx, next->state);
    }

    bool follow(ScreenId expected) {
        const bool ok = host.requested.has_value() && *host.requested == expected;
        if (ok)
            go(expected);
        return ok;
    }

    void frame() {
        const auto* screen = desc(current);
        screen->tick(&ctx, screen->state);
        screen->draw(&ctx, screen->state);
    }

    bool click(const char* name) { return mp::multiplayer_click(&ctx, name); }

    /// Hosts a TCP/IP game and opens its battle room.
    bool host_battleroom(const char* game) {
        go(mp::kScreenProviders);
        auto* providers = mp::panel_control(mp::multiplayer_panel(), "DPLAY");
        if (providers == nullptr)
            return false;
        providers->list_selection = 1;
        if (!click("SELECT") || !follow(mp::kScreenTcp))
            return false;
        mp::panel_set_text(mp::multiplayer_panel(), "ADDRESS", "127.0.0.1");
        if (!click("OK") || !follow(mp::kScreenGameList))
            return false;
        if (!click("STARTNEW") || !follow(mp::kScreenNewGame))
            return false;
        mp::panel_set_text(mp::multiplayer_panel(), "GAMENAME", game);
        mp::panel_set_text(mp::multiplayer_panel(), "NICKNAME", "Host");
        return click("OK") && follow(mp::kScreenBattleroom);
    }

    /// Hosts a game, leaves it and joins the battle room as a joiner.
    bool join_battleroom() {
        if (!host_battleroom("Joined") || !click("PREVMENU") || !follow(mp::kScreenGameList))
            return false;
        mp::panel_set_text(mp::multiplayer_panel(), "NICKNAME", "Guest");
        return click("JOINGAME") && follow(mp::kScreenBattleroom);
    }
};

/// Writes one map's files, the pack map's, with terrain whose four tile bytes
/// tell one release from another.
///
/// @param folder the pack's folder, written afresh
/// @param tiles the terrain's tile bytes
/// @return true when the files are written
bool write_pack_folder(const std::filesystem::path& folder, uint8_t tiles) {
    std::error_code error;
    std::filesystem::remove_all(folder, error);
    if (!std::filesystem::create_directories(folder / "maps", error))
        return false;
    const char* ota = "[GlobalHeader]\n"
                      "{\n"
                      "missionname=Isle of Ashes;\n"
                      "missiondescription=A ring of cinders;\n"
                      "[Schema 0]\n"
                      "    {\n"
                      "    Type=Network 1;\n"
                      "    }\n"
                      "}\n";
    {
        std::ofstream out(folder / "maps" / "isle_of_ashes.ota", std::ios::binary);
        out << ota;
        if (!out)
            return false;
    }
    std::vector<uint8_t> terrain(0x44, 0);
    const auto put = [&](std::size_t offset, uint32_t value) {
        terrain[offset] = static_cast<uint8_t>(value);
        terrain[offset + 1] = static_cast<uint8_t>(value >> 8);
        terrain[offset + 2] = static_cast<uint8_t>(value >> 16);
        terrain[offset + 3] = static_cast<uint8_t>(value >> 24);
    };
    put(0, 0x2000);
    put(4, 1);
    put(8, 1);
    put(0x10, 0x40);
    std::fill(terrain.begin() + 0x40, terrain.end(), tiles);
    std::ofstream out(folder / "maps" / "isle_of_ashes.tnt", std::ios::binary);
    out.write(
        reinterpret_cast<const char*>(terrain.data()), static_cast<std::streamsize>(terrain.size())
    );
    return static_cast<bool>(out);
}

/// A source of one pack map, as network play binds pack_map_source: it keeps
/// the map it prepared, mounts its folder when it is given one, and answers
/// the map pack of the prepared map.
struct PackSource {
    oa::AssetStore* assets = nullptr;
    std::filesystem::path folder;
    bool listed = true;   ///< the map is installed
    bool held = false;    ///< its maps are held, as while GET MAP installs it
    std::string prepared; ///< the map prepared; empty when none is

    mp::LobbyMapSource source() {
        mp::LobbyMapSource bound{};
        bound.context = this;
        bound.count = [](void* context) {
            return static_cast<PackSource*>(context)->listed ? int32_t{1} : int32_t{0};
        };
        bound.at = [](void* context, int32_t index, mp::LobbyPackMap* out) {
            if (!static_cast<PackSource*>(context)->listed || index != 0 || out == nullptr)
                return false;
            *out = {kPackMap, "A ring of cinders", "16x16", 16};
            return true;
        };
        bound.prepare = [](void* context, const char* name, char* reason, std::size_t capacity) {
            auto* self = static_cast<PackSource*>(context);
            self->release_files();
            if (name == nullptr || std::strcmp(name, kPackMap) != 0 || !self->listed)
                return false;
            if (self->held) {
                if (reason != nullptr && capacity > 0)
                    reason[0] = '\0';
                return false;
            }
            if (self->assets != nullptr && !self->folder.empty()) {
                oa::PackLayerSpec spec;
                spec.kind = oa::PackLayerKind::folder;
                spec.location = self->folder;
                spec.label = "archipelago " + self->folder.filename().string();
                spec.files.push_back(
                    {"maps/isle_of_ashes@archipelago.ota", "maps/isle_of_ashes.ota"}
                );
                spec.files.push_back(
                    {"maps/isle_of_ashes@archipelago.tnt", "maps/isle_of_ashes.tnt"}
                );
                std::string error;
                if (!self->assets->mount_pack_layer(std::move(spec), &error))
                    return false;
            }
            self->prepared = name;
            return true;
        };
        bound.release = [](void* context) { static_cast<PackSource*>(context)->release_files(); };
        bound.pack = [](void* context, ng::PresenceMapPack* out) {
            const auto* self = static_cast<PackSource*>(context);
            if (out == nullptr || self->prepared != kPackMap)
                return false;
            *out = record_pack();
            return true;
        };
        return bound;
    }

    void release_files() {
        prepared.clear();
        if (assets != nullptr && assets->pack_layer_mounted())
            (void)assets->unmount_pack_layer();
    }
};

/// Binds a source as network play does, and unbinds it when the case ends.
struct Bound {
    explicit Bound(PackSource& source) {
        const mp::LobbyMapSource maps_source = source.source();
        mp::multiplayer_bind_map_source(maps_source);
        mp::multiplayer_bind_map_pack(maps_source.context, maps_source.pack);
    }

    ~Bound() {
        mp::multiplayer_bind_map_source({});
        mp::multiplayer_bind_map_pack(nullptr, nullptr);
    }

    Bound(const Bound&) = delete;
    Bound& operator=(const Bound&) = delete;
};

/// The host's pack reaches LobbyMaps::pack only through the binding.
void the_host_pack_through_the_binding(Driver& d) {
    PackSource source;
    {
        Bound bound(source);
        mp::multiplayer_reset();
        if (!d.host_battleroom("Pack"))
            return expect(false, "the host's battle room opens");
        auto& lobby = mp::multiplayer_lobby();
        expect(lobby.maps.pack != nullptr, "the bound pack is the lobby's");
        if (lobby.maps.pack == nullptr || lobby.maps.select == nullptr)
            return;
        expect(lobby.maps.select(lobby.maps.context, kPackMap), "the host selects the pack map");
        ng::PresenceMapPack out;
        expect(
            lobby.maps.pack(lobby.maps.pack_context, &out) && out == record_pack(),
            "after the pack map, LobbyMaps::pack answers its map pack"
        );
        std::string base;
        for (int32_t index = 0; index < lobby.maps.count(lobby.maps.context); ++index) {
            const char* name = lobby.maps.at(lobby.maps.context, index);
            if (name != nullptr && std::strcmp(name, kPackMap) != 0) {
                base = name;
                break;
            }
        }
        expect(
            !base.empty() && lobby.maps.select(lobby.maps.context, base.c_str()),
            "the host selects a base map"
        );
        expect(
            !lobby.maps.pack(lobby.maps.pack_context, &out),
            "after a base map, LobbyMaps::pack answers false"
        );
    }
    mp::multiplayer_reset();
    if (!d.host_battleroom("Unbound"))
        return expect(false, "the host's battle room opens again");
    expect(mp::multiplayer_lobby().maps.pack == nullptr, "with nothing bound it is null");
}

/// The host's setup block, as its 0x20 record carries it.
mp::LobbyEvent host_block(uint32_t player_id, const char* map, uint32_t map_hash) {
    mp::PlayerSetupInfo host{};
    host.role = mp::kRoleHost;
    host.version_major = 3;
    host.version_minor = 1;
    host.state = mp::kInfoStatePlaying;
    host.map_hash = map_hash;
    std::snprintf(host.map_name, sizeof host.map_name, "%s", map);
    ng::mark_engine_signature(reinterpret_cast<uint8_t*>(&host));
    ng::PlayerInfoRecord record{};
    std::memcpy(record.info_head, &host, sizeof record.info_head);
    record.player_id = player_id;
    std::memcpy(
        record.info_tail,
        reinterpret_cast<const uint8_t*>(&host) + ng::player_info_tail_offset,
        sizeof record.info_tail
    );
    mp::LobbyEvent info{};
    info.kind = mp::LobbyEventKind::record;
    info.player_id = player_id;
    std::size_t written = 0;
    (void)ng::encode_record(record, info.data, sizeof info.data, &written);
    info.size = static_cast<uint16_t>(written);
    return info;
}

/// The host's presence record, naming its map pack.
mp::LobbyEvent host_record(uint32_t player_id) {
    ng::PresenceRecord record;
    record.engine = ng::PresenceEngine{"0.8.0", "macOS", "arm64"};
    record.content = ng::PresenceContent{true};
    record.map_pack = record_pack();
    mp::LobbyEvent event{};
    event.kind = mp::LobbyEventKind::record;
    event.player_id = player_id;
    std::size_t written = 0;
    (void)ng::encode_presence(record, event.data, sizeof event.data, &written);
    event.size = static_cast<uint16_t>(written);
    return event;
}

/// Seats the host in a joiner's battle room, with its block naming a map.
bool seat_host(uint32_t player_id, const char* map, uint32_t map_hash) {
    mp::LobbyEvent joined{};
    joined.kind = mp::LobbyEventKind::player_joined;
    joined.player_id = player_id;
    std::snprintf(joined.name, sizeof joined.name, "%s", "Host");
    auto& loopback = mp::multiplayer_loopback();
    return mp::loopback_inject(loopback, joined) &&
           mp::loopback_inject(loopback, host_block(player_id, map, map_hash));
}

/// What a joiner reads of the host's map: the name, the record's pack, the label.
void the_joiner_reads_the_host_map(Driver& d) {
    constexpr uint32_t kHostId = 0x7701;
    PackSource source;
    source.listed = false;
    Bound bound(source);
    mp::multiplayer_reset();
    if (!d.join_battleroom())
        return expect(false, "the joiner's battle room opens");
    d.frame();
    expect(seat_host(kHostId, kPackMap, 0x01020304), "the host is seated");
    d.frame();
    mp::BattleRoomMap map;
    expect(mp::multiplayer_battle_room_map(&map), "the joiner reads the battle room's map");
    expect(map.host_map == kPackMap && map.joiner, "the host's map name, read by a joiner");
    expect(!map.host_pack, "with no stored record, no pack");
    expect(
        map.label_width > 0 && map.label_height > 0 && map.label_x >= 0 && map.label_y >= 0,
        "the MAPNAME label's rectangle"
    );

    expect(
        mp::loopback_inject(mp::multiplayer_loopback(), host_record(kHostId)), "the record comes"
    );
    d.frame();
    expect(mp::multiplayer_battle_room_map(&map), "the joiner reads the map again");
    expect(
        map.host_map == kPackMap && map.host_pack && map.host_pack->id == "archipelago" &&
            map.host_pack->sha256 == kHostSha && map.host_pack->registry_id == "example-maps" &&
            map.host_pack->release == 3,
        "the host slot's stored record gives the decoded pack"
    );
    expect(
        mp::get_map_state(map.host_map, map.host_pack, {}, offering(), false, false) == GetMap::get,
        "a joiner without the pack is offered GET MAP"
    );
    mp::BattleRoomMap none;
    expect(!mp::multiplayer_battle_room_map(nullptr), "no map is read into nothing");
    mp::multiplayer_reset();
    expect(!mp::multiplayer_battle_room_map(&none), "no map while no battle room shows");
}

/// A pack map installed while the battle room shows is found by its own
/// per-frame selection, and the release mounted afresh is read again.
void the_battle_room_finds_an_installed_map(Driver& d) {
    // A folder of this run's own: runs at once on one machine never share one.
    std::random_device entropy;
    const auto base = std::filesystem::temp_directory_path() /
                      ("oa-get-map-" + std::to_string(entropy()) + "-" + std::to_string(entropy()));
    const auto first = base / "release-2";
    const auto second = base / "release-3";

    struct Clean {
        std::filesystem::path folder;
        oa::AssetStore* assets;

        ~Clean() {
            if (assets->pack_layer_mounted())
                (void)assets->unmount_pack_layer();
            std::error_code error;
            std::filesystem::remove_all(folder, error);
        }
    } clean{base, &d.assets};

    expect(
        write_pack_folder(first, 0x11) && write_pack_folder(second, 0x33),
        "two releases of the pack map are written"
    );

    // The host's release: its map hash, as the host computes it.
    PackSource source;
    source.assets = &d.assets;
    source.folder = second;
    uint32_t host_hash = 0;
    {
        Bound bound(source);
        mp::multiplayer_reset();
        if (!d.host_battleroom("Hash"))
            return expect(false, "the host's battle room opens for the hash");
        auto& lobby = mp::multiplayer_lobby();
        expect(lobby.maps.select(lobby.maps.context, kPackMap), "the host's release is selected");
        host_hash = lobby.maps.content_hash(lobby.maps.context);
    }
    expect(host_hash != 0, "the host's release has a map hash");

    // A joiner without the pack.
    constexpr uint32_t kHostId = 0x7702;
    source.listed = false;
    source.folder = first;
    Bound bound(source);
    mp::multiplayer_reset();
    if (!d.join_battleroom())
        return expect(false, "the joiner's battle room opens");
    d.frame();
    expect(seat_host(kHostId, kPackMap, host_hash), "the host is seated with its pack map");
    d.frame();
    d.frame();
    auto& lobby = mp::multiplayer_lobby();
    expect(!mp::lobby_has_host_map(lobby), "without the pack the host's map is missing");

    // Another release is installed: the map is found, but it is not the host's.
    source.listed = true;
    d.frame();
    d.frame();
    expect(
        source.prepared == kPackMap && !mp::lobby_has_host_map(lobby),
        "another release is found and mounted, and differs from the host's"
    );

    // GET MAP holds the pack's maps while it installs the host's release.
    source.held = true;
    d.frame();
    expect(source.prepared.empty() && !d.assets.pack_layer_mounted(), "a held pack is not mounted");
    source.folder = second;
    d.frame();
    expect(!mp::lobby_has_host_map(lobby), "nothing is found while the pack is held");

    // The install ends: the battle room mounts the host's release and reads it again.
    source.held = false;
    d.frame();
    d.frame();
    expect(
        source.prepared == kPackMap && mp::lobby_has_host_map(lobby),
        "the host's release is found once installed, its files read again"
    );
    expect((mp::local_info(lobby).options & mp::option::ready) == 0, "nothing readies the joiner");
    mp::multiplayer_reset();
}

/// A point inside the map name, still within a finger's reach of GET MAP,
/// belongs to the map name. A point on the button, clear of that name, does not.
void a_point_inside_the_map_name_is_the_map_names() {
    namespace place = oa::ui::frontend_state;
    const place::PlaceRect map_name{110, 200, 191, 13};
    const place::PlaceRect button{221, 170, 80, 22};
    constexpr int32_t x = 250;
    constexpr int32_t y = 206;
    expect(place::point_distance_squared(x, y, map_name) == 0, "the point is inside the map name");
    expect(
        place::point_distance_squared(x, y, button) < static_cast<int64_t>(22) * 22,
        "the point is within a finger's reach of GET MAP"
    );
    const place::PlaceRect gadgets[] = {map_name};
    expect(
        place::nearer_gadget(x, y, button, gadgets),
        "a point inside the map name, within reach of GET MAP, is the map name's"
    );
    expect(
        !place::nearer_gadget(250, 180, button, gadgets),
        "a point on GET MAP, clear of the map name, is the button's"
    );
}

} // namespace

int main(int argc, char** argv) {
    if (!oa::test::game_data_requested(argc, argv)) {
        the_host_pack_from_its_record();
        every_state();
        the_catalogue_decides_what_can_be_fetched();
        every_line();
        a_point_inside_the_map_name_is_the_map_names();
    } else {
        const std::filesystem::path data =
            oa::test::require_game_directory("GET MAP in the battle room");
        mp::multiplayer_reset();
        Driver d(data);
        the_host_pack_through_the_binding(d);
        the_joiner_reads_the_host_map(d);
        the_battle_room_finds_an_installed_map(d);
        mp::multiplayer_reset();
    }
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("GET MAP tests passed");
    return 0;
}

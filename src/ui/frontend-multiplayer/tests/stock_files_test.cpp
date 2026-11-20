// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Game files and units that differ in the battle room.
//
// boneyards: a battle the Boneyards launches counts for its ladder, so START
// is refused while any player's presence record says their game files
// differ from Total Annihilation's, and the room names that player. A
// casual room keeps its rules.
//
// unit-sync: once the host's unit sync has finished, the units it found not
// alike are named by their names in the game and the machines they differ
// on, on the host and, through the host's presence record, on an OA guest.
// The host's first START says that the next one starts anyway. A room whose
// units all match writes nothing and starts at the first press.
#include "oa/data/languages/interface_text.hpp"
#include "oa/netgame/presence.hpp"
#include "oa/netgame/records.hpp"
#include "oa/ui/frontend_multiplayer/launch_block.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"
#include "oa/ui/frontend_multiplayer/lobby_net.hpp"
#include "oa/ui/frontend_multiplayer/panel.hpp"
#include "oa/ui/gui_layout.hpp"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace mp = oa::ui::frontend_multiplayer;
namespace ng = oa::netgame;
namespace gui = oa::ui::gui_layout;

namespace {

int g_failures = 0;

void expect(bool condition, const char* what) {
    if (condition)
        return;
    std::fprintf(stderr, "FAILED: %s\n", what);
    ++g_failures;
}

bool map_selected(void*) {
    return true;
}

const char* map_name(void*) {
    return "Ridge";
}

bool launch_on(void*) {
    return true;
}

/// A content checksum the host works out for its own units.
uint32_t own_checksum(void*, const mp::LobbyUnit& unit) {
    return unit.fbi_hash * 7U + 1U;
}

/// The battle room's START over a panel of its own.
gui::Layout room_layout() {
    const auto gadget = [](gui::GadgetType type,
                           const char* name,
                           int16_t x,
                           int16_t y,
                           int16_t width,
                           int16_t height,
                           int32_t attributes) {
        gui::Gadget made;
        made.common.type = type;
        made.common.name = name;
        made.common.x = x;
        made.common.y = y;
        made.common.width = width;
        made.common.height = height;
        made.common.attributes = attributes;
        made.common.active = 1;
        return made;
    };
    gui::Layout layout;
    layout.gadgets.push_back(gadget(gui::GadgetType::panel, "lounge2.GUI", 0, 0, 640, 480, 0));
    auto start = gadget(gui::GadgetType::button, "START", 522, 437, 96, 31, 0x10002);
    gui::ButtonFields fields;
    fields.text = "Start Game";
    start.fields = fields;
    layout.gadgets.push_back(start);
    return layout;
}

constexpr uint32_t kHost = 0x100;
constexpr uint32_t kGuest = 0x200;

/// A battle room: this machine in slot 0, the other machine's player in slot 1.
struct Room {
    std::unique_ptr<oa::Game> game = std::make_unique<oa::Game>();
    mp::Lobby lobby{};
    mp::LoopbackNet loopback{};
    mp::Panel panel{};
    mp::launch::LaunchBlock block{};

    /// @param hosting true seats this machine's player as the host
    explicit Room(bool hosting) {
        mp::loopback_reset(loopback);
        mp::lobby_reset(lobby, *game);
        lobby.net = mp::loopback_lobby_net(loopback);
        lobby.maps.selected = map_selected;
        lobby.maps.name = map_name;
        lobby.local_version_major = 3;
        game->session_flags |= 1;
        game->players[0].player_id = hosting ? kHost : kGuest;
        mp::lobby_seat_local(lobby, 0, hosting, hosting ? "Kestrel" : "Wren");
        mp::local_info(lobby).version_major = 3;
        mp::local_info(lobby).version_minor = 1;
        seat(hosting ? kGuest : kHost, hosting ? "Wren" : "Kestrel", !hosting);
        mp::panel_load(panel, "lounge2.GUI", room_layout());
    }

    /// Seats the other machine's player, on Open Annihilation, in slot 1.
    void seat(uint32_t id, const char* name, bool host_role) {
        auto& player = mp::slot_player(lobby, 1);
        player.in_use = 1;
        player.player_id = id;
        player.index = 1;
        std::snprintf(player.name, sizeof player.name, "%s", name);
        mp::slot_set_status(lobby, player, mp::kSlotRemote);
        auto* info = mp::slot_info(lobby, 1);
        info->state = mp::kInfoStatePlaying;
        info->net_id = id;
        info->version_major = 3;
        info->version_minor = 1;
        if (host_role)
            info->role = static_cast<uint8_t>(info->role | mp::kRoleHost);
        info->engine_signature[0] = ng::engine_signature_first;
        info->engine_signature[1] = ng::engine_signature_second;
        lobby.presence_records.block_player[1] = id;
    }

    /// Makes this battle one the Boneyards launched.
    void launch_for_ladder() {
        block.stock_files = 1;
        lobby.launch_link.block = &block;
        lobby.launch_link.launch_active = launch_on;
    }

    /// Stores the record this machine sends.
    void own_record(const std::vector<uint8_t>& bytes) {
        std::memcpy(lobby.presence_records.local, bytes.data(), bytes.size());
        lobby.presence_records.local_size = static_cast<uint16_t>(bytes.size());
    }

    /// Takes the other machine's record.
    bool their_record(const std::vector<uint8_t>& bytes) {
        mp::LobbyEvent event{};
        event.kind = mp::LobbyEventKind::record;
        event.player_id = mp::slot_player(lobby, 1).player_id;
        event.size = static_cast<uint16_t>(bytes.size());
        std::memcpy(event.data, bytes.data(), bytes.size());
        return mp::lobby_apply_event(lobby, event);
    }

    /// Presses START as the host.
    ///
    /// @return what the press asked for
    mp::LobbyAction press_start() {
        panel.selected = mp::panel_find(panel, "START");
        return mp::lobby_handle_event(lobby, panel);
    }

    /// The room's lines, oldest first.
    std::vector<std::string> lines() {
        std::vector<std::string> said;
        for (auto at = static_cast<uint32_t>(mp::lobby_chat_tail(*game));
             at != mp::lobby_chat_head(*game);
             at = (at + 1U) % static_cast<uint32_t>(mp::kChatLines))
            said.emplace_back(mp::lobby_chat_line(*game, at));
        return said;
    }

    /// Every line, joined: a long line is stored in several parts.
    std::string text() {
        std::string joined;
        for (const std::string& line : lines())
            joined += line + ' ';
        return joined;
    }
};

/// A presence record that names the base game and whether its files differ.
std::vector<uint8_t> bundles_record(bool differs) {
    ng::PresenceRecord record;
    record.bundles = ng::PresenceBundles{"totala", {"core-contingency"}, differs};
    std::vector<uint8_t> bytes(ng::presence_record_max_bytes);
    std::size_t written = 0;
    if (ng::encode_presence(record, bytes.data(), bytes.size(), &written) != ng::WireError::ok)
        return {};
    bytes.resize(written);
    return bytes;
}

/// A presence record that does not name the content: a game folder that is
/// not Open Annihilation's own copy, or an older Open Annihilation.
std::vector<uint8_t> plain_record() {
    ng::PresenceRecord record;
    record.language = std::string("English");
    std::vector<uint8_t> bytes(ng::presence_record_max_bytes);
    std::size_t written = 0;
    if (ng::encode_presence(record, bytes.data(), bytes.size(), &written) != ng::WireError::ok)
        return {};
    bytes.resize(written);
    return bytes;
}

bool has(const std::string& text, std::string_view part) {
    return text.find(part) != std::string::npos;
}

void a_ladder_battle_names_a_player_whose_files_differ() {
    Room room(true);
    room.launch_for_ladder();
    room.own_record(bundles_record(false));
    expect(room.their_record(bundles_record(true)), "the guest's record applies");
    expect(mp::lobby_stock_files_room(room.lobby), "a Boneyards launch counts for its ladder");
    expect(mp::lobby_stock_files_block(room.lobby) == 1, "the guest's files keep the battle back");
    expect(!mp::lobby_stock_files_allow_start(room.lobby), "START is refused");
    mp::lobby_note_stock_files(room.lobby);
    mp::lobby_note_stock_files(room.lobby);
    const std::string said = room.text();
    expect(
        has(said,
            "Wren's game files differ from Total Annihilation's; they must repair them "
            "before this battle can start."),
        "the room names the player whose files differ"
    );
    expect(room.lines().size() <= 2, "the line is written once");
    const std::size_t before = room.lines().size();
    expect(room.press_start() != mp::LobbyAction::start, "a press on START does not start");
    expect(room.lines().size() == before, "the refused press writes nothing more");

    // Their files are put right: the battle may start.
    expect(room.their_record(bundles_record(false)), "the repaired record applies");
    expect(mp::lobby_stock_files_allow_start(room.lobby), "START is allowed again");
    mp::lobby_note_stock_files(room.lobby);
    expect(room.lobby.stock_files_told[1] == 0, "the room forgets what it said");

    // The host's own files: the host reads what to do.
    Room own(true);
    own.launch_for_ladder();
    own.own_record(bundles_record(true));
    expect(own.their_record(bundles_record(false)), "the guest's record applies");
    expect(mp::lobby_stock_files_block(own.lobby) == 0, "the host's own files keep it back");
    mp::lobby_note_stock_files(own.lobby);
    expect(
        has(own.text(),
            "Your game files differ from Total Annihilation's, so this battle cannot "
            "start."),
        "the host reads its own line with the repair"
    );

    // The guest's own machine says the same to its own player.
    Room guest(false);
    guest.launch_for_ladder();
    guest.own_record(bundles_record(true));
    expect(guest.their_record(bundles_record(false)), "the host's record applies");
    mp::lobby_note_stock_files(guest.lobby);
    expect(
        has(guest.text(), "Your game files differ from Total Annihilation's"),
        "the guest whose files differ reads its own line"
    );
}

void a_casual_room_keeps_its_rules() {
    Room room(true);
    room.own_record(bundles_record(false));
    expect(room.their_record(bundles_record(true)), "the guest's record applies");
    expect(!mp::lobby_stock_files_room(room.lobby), "a room no launch made does not count");
    expect(mp::lobby_stock_files_allow_start(room.lobby), "the ladder's rule refuses nothing");
    mp::lobby_note_stock_files(room.lobby);
    expect(room.lines().empty(), "the casual room writes no ladder line");
    // The content hold is 0.8's, as before: the rules dot and a grey START.
    expect(!mp::lobby_content_allows_start(room.lobby), "the content hold is unchanged");

    Room clean(true);
    clean.launch_for_ladder();
    clean.own_record(bundles_record(false));
    expect(clean.their_record(bundles_record(false)), "the guest's record applies");
    expect(mp::lobby_stock_files_allow_start(clean.lobby), "known files start a ladder battle");
    mp::lobby_note_stock_files(clean.lobby);
    expect(clean.lines().empty(), "known files write nothing");
}

/// The host's units: the commander, the Metal Extractor, the Phalanx and a
/// unit that is off by default.
mp::LobbyUnit g_units[] = {
    {"", "", 0, 0, 0, 0, "", 0, 0},
    {"Commander", "ARM", 25000, 2500, mp::kUnitNoRestrict, 11, "ARMCOM", 0, 0},
    {"Metal Extractor", "ARM", 50, 50, 0, 22, "ARMMEX", 0, 0},
    {"Phalanx", "ARM", 900, 45, 0, 33, "ARMYORK", 0, 0},
    {"Nuke", "ARM", 90000, 1500, mp::kUnitDisabledDefault, 44, "ARMSILO", 0, 0},
};

/// Runs the host's unit sync with the guest's report, every verdict acknowledged.
///
/// @param room the host's room
/// @param reported the guest's keys and checksums
void sync_with(Room& room, const std::vector<std::pair<uint32_t, uint32_t>>& reported) {
    room.lobby.units = g_units;
    room.lobby.unit_count = 5;
    room.lobby.services.unit_checksum = own_checksum;
    mp::unit_sync_create(room.lobby, true);
    mp::unit_sync_tick(room.lobby);
    const auto handshake = [&](uint8_t subtype, uint32_t key, uint32_t value) {
        uint8_t record[14] = {0x1a, subtype};
        std::memcpy(record + 6, &key, 4);
        std::memcpy(record + 10, &value, 4);
        mp::unit_sync_receive(room.lobby, record, 1);
    };
    handshake(1, 0, static_cast<uint32_t>(reported.size()));
    for (const auto& [key, checksum] : reported)
        handshake(2, key, checksum);
    handshake(4, 0, 1000);
}

void a_unit_that_differs_is_named_and_start_waits() {
    Room room(true);
    // Both machines say their files are known: the line names both.
    room.own_record(bundles_record(false));
    expect(room.their_record(bundles_record(false)), "the guest's record applies");
    sync_with(room, {{11, 11 * 7 + 1}, {22, 0x5a5a}, {33, 33 * 7 + 1}, {44, 44 * 7 + 1}});
    expect(mp::unit_sync_complete(room.lobby), "the sync finished");
    mp::lobby_note_unit_sync(room.lobby);
    const ng::PresenceUnitSync& found = room.lobby.unit_report.found;
    expect(found.total == 1 && found.groups.size() == 1, "one unit differs");
    expect(
        !found.groups.empty() && found.groups[0].units.size() == 1 &&
            found.groups[0].units[0].name == "ARMMEX" &&
            found.groups[0].machines == std::vector<uint32_t>{kHost, kGuest},
        "the Metal Extractor differs on both machines when neither leads"
    );
    expect(
        has(room.text(),
            "*** Metal Extractor (ARMMEX) differs between Kestrel's and "
            "Wren's machines and can't be built in this battle."),
        "the room names the unit and both machines"
    );
    char diagnostic[mp::kChatLineBytes];
    expect(
        std::strcmp(
            mp::unit_sync_diagnostic(room.lobby, diagnostic, sizeof diagnostic), "1 unit differs"
        ) == 0,
        "+syncerr counts the unit instead of answering OK"
    );
    mp::UnitSyncRecord record{};
    expect(!mp::unit_sync_lookup(room.lobby, 22, &record), "the unit stays unavailable");
    expect(mp::unit_sync_lookup(room.lobby, 33, &record), "the others stay available");

    // START waits for the host to accept.
    const std::size_t before = room.lines().size();
    expect(room.press_start() != mp::LobbyAction::start, "the first START does not start");
    expect(
        has(room.text(), "Press START again to start anyway.") && room.lines().size() > before,
        "the first START says the next one starts anyway"
    );
    expect(room.press_start() == mp::LobbyAction::start, "the second START starts anyway");
    expect(!mp::unit_sync_lookup(room.lobby, 22, &record), "the unit is still unavailable");

    // The host's record carries the finding for OA guests.
    std::memcpy(
        room.lobby.presence_records.source,
        room.lobby.presence_records.local,
        room.lobby.presence_records.local_size
    );
    room.lobby.presence_records.source_size = room.lobby.presence_records.local_size;
    mp::presence_refresh(room.lobby);
    const auto carried = ng::presence_unit_sync(
        {room.lobby.presence_records.local, room.lobby.presence_records.local_size}
    );
    expect(carried && *carried == found, "the host's record carries the finding");
}

void the_machine_without_known_files_is_named() {
    // As in the 0.9.0 incident, the host's units are not Total Annihilation's
    // and the guest's are. Here the host plays a folder that is not Open
    // Annihilation's own copy, so its record does not say its files are
    // known, while the guest's does. (A copy whose files differ sets the
    // record's flag, and the content hold keeps START grey before the units
    // are looked at.) The host's Metal Extractor and Phalanx differ, the
    // guest has a unit the host does not, and the line names the host's
    // machine.
    Room room(true);
    room.own_record(plain_record());
    expect(room.their_record(bundles_record(false)), "the guest's record applies");
    sync_with(
        room, {{11, 11 * 7 + 1}, {22, 0x5a5a}, {33, 0x6b6b}, {44, 44 * 7 + 1}, {0xf694e82a, 9}}
    );
    mp::lobby_note_unit_sync(room.lobby);
    const ng::PresenceUnitSync& found = room.lobby.unit_report.found;
    expect(found.total == 3, "three units differ: two of the host's, one only the guest has");
    expect(
        found.groups.size() == 1 && found.groups[0].machines == std::vector<uint32_t>{kHost},
        "they differ on the host's machine"
    );
    expect(
        has(room.text(),
            "*** Metal Extractor (ARMMEX), Phalanx (ARMYORK) and 1 other unit "
            "differ on Kestrel's machine and can't be built in this battle."),
        "the room names the host's machine and groups the rest"
    );

    // A guest reads the host's finding and names the units itself.
    Room guest(false);
    guest.lobby.units = g_units;
    guest.lobby.unit_count = 5;
    std::memcpy(
        room.lobby.presence_records.source,
        room.lobby.presence_records.local,
        room.lobby.presence_records.local_size
    );
    room.lobby.presence_records.source_size = room.lobby.presence_records.local_size;
    mp::presence_refresh(room.lobby);
    const std::vector<uint8_t> host_record(
        room.lobby.presence_records.local,
        room.lobby.presence_records.local + room.lobby.presence_records.local_size
    );
    expect(guest.their_record(host_record), "the host's record reaches the guest");
    mp::lobby_note_unit_sync(guest.lobby);
    mp::lobby_note_unit_sync(guest.lobby);
    const std::string said = guest.text();
    expect(
        has(said,
            "Metal Extractor (ARMMEX), Phalanx (ARMYORK) and 1 other unit differ on "
            "Kestrel's machine"),
        "the guest names the units and the host's machine"
    );
    std::size_t lines = 0;
    for (const std::string& line : guest.lines())
        lines += line.rfind("*** Metal Extractor", 0) == 0 ? 1 : 0;
    expect(lines == 1, "the guest writes the finding once");
    expect(mp::lobby_unit_sync_take_start(guest.lobby), "the hold is the host's alone");
}

void a_room_whose_units_match_is_as_before() {
    Room room(true);
    room.own_record(bundles_record(false));
    expect(room.their_record(bundles_record(false)), "the guest's record applies");
    sync_with(room, {{11, 11 * 7 + 1}, {22, 22 * 7 + 1}, {33, 33 * 7 + 1}, {44, 44 * 7 + 1}});
    mp::lobby_note_unit_sync(room.lobby);
    expect(room.lobby.unit_report.found.groups.empty(), "nothing differs");
    expect(room.lines().empty(), "the room writes nothing");
    char diagnostic[mp::kChatLineBytes];
    expect(
        std::strcmp(mp::unit_sync_diagnostic(room.lobby, diagnostic, sizeof diagnostic), "OK") == 0,
        "+syncerr still answers OK"
    );
    expect(room.press_start() == mp::LobbyAction::start, "the first START starts");
    std::memcpy(
        room.lobby.presence_records.source,
        room.lobby.presence_records.local,
        room.lobby.presence_records.local_size
    );
    room.lobby.presence_records.source_size = room.lobby.presence_records.local_size;
    const std::vector<uint8_t> before(
        room.lobby.presence_records.local,
        room.lobby.presence_records.local + room.lobby.presence_records.local_size
    );
    mp::presence_refresh(room.lobby);
    const std::vector<uint8_t> after(
        room.lobby.presence_records.local,
        room.lobby.presence_records.local + room.lobby.presence_records.local_size
    );
    expect(before == after, "the host's record is unchanged");

    // A guest whose host sends no finding writes nothing.
    Room guest(false);
    guest.lobby.units = g_units;
    guest.lobby.unit_count = 5;
    expect(guest.their_record(after), "the host's record reaches the guest");
    mp::lobby_note_unit_sync(guest.lobby);
    expect(guest.lines().empty(), "the guest writes nothing");
}

} // namespace

int main(int argc, char** argv) {
    const std::string_view part = argc > 1 ? std::string_view(argv[1]) : std::string_view();
    if (part.empty() || part == "boneyards") {
        a_ladder_battle_names_a_player_whose_files_differ();
        a_casual_room_keeps_its_rules();
    }
    if (part.empty() || part == "unit-sync") {
        a_unit_that_differs_is_named_and_start_waits();
        the_machine_without_known_files_is_named();
        a_room_whose_units_match_is_as_before();
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("stock files and unit sync checks passed");
    return 0;
}

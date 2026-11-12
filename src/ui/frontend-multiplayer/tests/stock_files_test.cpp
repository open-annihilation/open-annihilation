// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Game files that differ in the battle room: a battle the Boneyards
// launches counts for its ladder, so START is refused while any player's
// presence record says their game files differ from Total Annihilation's,
// and the room names that player. A casual room keeps its rules.
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

} // namespace

int main(int argc, char** argv) {
    const std::string_view part = argc > 1 ? std::string_view(argv[1]) : std::string_view();
    if (part.empty() || part == "boneyards") {
        a_ladder_battle_names_a_player_whose_files_differ();
        a_casual_room_keeps_its_rules();
    }
    if (g_failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", g_failures);
        return 1;
    }
    std::puts("stock files checks passed");
    return 0;
}

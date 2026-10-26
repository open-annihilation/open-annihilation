// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Battleroom rules and map previews with the loopback net, and (with --data)
// against the installed game's LOUNGE2.GUI and SELGAME.GUI layouts.
#include "oa/data/defs/layout.hpp"
#include "oa/data/languages/unit_texts.hpp"
#include "oa/data/mod_profile.hpp"
#include "oa/data/mod_profile/overrides.hpp"
#include "oa/formats/gaf.hpp"
#include "oa/netgame/presence.hpp"
#include "oa/netgame/private_channel.hpp"
#include "oa/netgame/records.hpp"
#include "oa/netgame/unicode_chat.hpp"
#include "oa/ui/frontend_multiplayer/connect.hpp"
#include "oa/ui/frontend_multiplayer/dialogs.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"
#include "oa/ui/frontend_multiplayer/restrict.hpp"
#include "oa/test/game_assets.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <iterator>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mp = oa::ui::frontend_multiplayer;

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        ++failures;
    }
}

uint32_t clock_ticks = 1000;
// The lobby's clock reads 30 ticks a second from 0 through 4,294,967 and
// then turns over to 0, about every 39.8 hours.
constexpr uint32_t kLargestReading = 4'294'967;
// Ticks ahead of the clock a status block is set to keep it out of a test:
// about nine hours, past the test's last tick.
constexpr uint32_t kNoStatusTicks = 1'000'000;
/// The installed game's SLIDERS art, which the lounge's sliders are bound
/// with; none without --data.
oa::ui::gui_input::ScrollArtFrames slider_art;
std::string last_message;
std::vector<std::string> sounds;
std::string map_name = "Coast To Coast";

uint32_t tick(void*) {
    return clock_ticks;
}

void message(void*, const char* text) {
    last_message = text;
}

void sound(void*, const char* name) {
    sounds.emplace_back(name);
}

// The launch a test lobby reads, whether it is active, and the
// texts of the joins it asked for that failed.
oa::ui::frontend_multiplayer::launch::LaunchBlock launch_block{};
bool active_launch = false;
bool refuse_map = false;
// The id the loopback session gives the next player a Fixture's room adds as
// it opens; 0 keeps the session's own first id.
uint32_t fixture_next_player_id = 0;
// The unit types a Fixture's room knows as it opens; none when null.
mp::LobbyUnit* fixture_units = nullptr;
int32_t fixture_unit_count = 0;
std::vector<std::string> failed_joins;
std::string saved_address;
int32_t address_saves = 0;

bool test_launched(void*) {
    return active_launch;
}

void test_join_failed(void*, const char* text) {
    failed_joins.emplace_back(text);
}

bool read_saved_address(void*, char* out, std::size_t capacity) {
    std::snprintf(out, capacity, "%s", "192.168.0.9");
    return true;
}

void write_saved_address(void*, const char* address) {
    saved_address = address;
    ++address_saves;
}

/// Returns the launch link over the test's block and flags.
///
/// @return the link
mp::LaunchLink launch_link() {
    mp::LaunchLink link{};
    link.block = &launch_block;
    link.launch_active = test_launched;
    link.join_failed = test_join_failed;
    return link;
}

/// Starts a test with an empty launch block and no active launch.
void reset_launch() {
    launch_block = oa::ui::frontend_multiplayer::launch::LaunchBlock{};
    active_launch = false;
    refuse_map = false;
    failed_joins.clear();
    saved_address.clear();
    address_saves = 0;
}

bool selected(void*) {
    return !map_name.empty();
}

const char* name(void*) {
    return map_name.c_str();
}

bool select_map(void*, const char* value) {
    if (refuse_map || value == nullptr)
        return false;
    map_name = value;
    return true;
}

uint32_t hash(void*) {
    return 0x1234;
}

int32_t memory(void*) {
    return 32;
}

/// Returns the lobby services the tests give every lobby: the sound, message,
/// clock and disc stubs above, nothing else.
///
/// @return the services
mp::LobbyServices test_services() {
    mp::LobbyServices services{};
    services.play_sound = sound;
    services.message = message;
    services.tick = tick;
    return services;
}

/// Returns the map list the tests give a lobby that picks maps: one selected
/// map by name, with its content hash and memory.
///
/// @return the map list
mp::LobbyMaps test_maps() {
    mp::LobbyMaps maps{};
    maps.selected = selected;
    maps.name = name;
    maps.select = select_map;
    maps.content_hash = hash;
    maps.memory_mb = memory;
    return maps;
}

/// Reads a GUI layout file of the installed game.
///
/// @param assets the installed game's store
/// @param name file name under guis/
/// @return the bytes, or empty when nothing provides the file
std::vector<uint8_t> read_gui(const oa::AssetStore& assets, const char* name) {
    return oa::test::read_game_file(assets, std::string("guis/") + name);
}

struct Fixture {
    std::unique_ptr<oa::Game> game = std::make_unique<oa::Game>();
    mp::Lobby lobby{};
    mp::LoopbackNet loopback{};
    mp::Panel panel;
    oa::ui::gui_layout::Layout layout;

    explicit Fixture(
        const oa::ui::gui_layout::Layout& lounge,
        const mp::LaunchLink* online = nullptr,
        bool host = true,
        uint8_t lobby_buttons = 0,
        uint32_t sides = 2,
        std::vector<std::string> names = {}
    )
        : layout(lounge) {
        mp::loopback_reset(loopback);
        mp::lobby_reset(lobby, *game);
        if (online != nullptr) {
            lobby.launch_link = *online;
            lobby.option_4 = host && online->block != nullptr && online->block->tournament != 0;
        }
        lobby.net = mp::loopback_lobby_net(loopback);
        lobby.services = test_services();
        lobby.maps = test_maps();
        lobby.local_version_major = 3;
        game->session_flags |= 1;
        mp::lobby_max_units(*game) = 250;
        mp::lobby_max_units_default(*game) = 250;
        game->side_count = sides;
        lobby.side_names = std::move(names);
        loopback.opened = true;
        if (fixture_next_player_id != 0)
            loopback.next_player_id = fixture_next_player_id;
        if (fixture_units != nullptr) {
            lobby.units = fixture_units;
            lobby.unit_count = fixture_unit_count;
        }
        game->players[0].player_id = 0x100;
        mp::lobby_seat_local(lobby, 0, host, "Host");
        mp::panel_load(panel, "guis/lounge2.gui", layout);
        mp::panel_bind_sliders(panel, slider_art);
        lobby.lobby_buttons = lobby_buttons;
        mp::lobby_enter_battleroom(lobby, panel);
    }

    void join(uint32_t id, const char* player) {
        mp::LobbyEvent event{};
        event.kind = mp::LobbyEventKind::player_joined;
        event.player_id = id;
        std::snprintf(event.name, sizeof(event.name), "%s", player);
        expect(mp::lobby_apply_event(lobby, event), "join event applies");
        mp::unit_sync_tick(lobby); // the host greets the new peer on its next frame
        mp::lobby_update_status(lobby, panel);
    }

    void remote_info(uint32_t id, uint16_t options, uint8_t color) {
        const auto slot = mp::slot_for_player_id(lobby, id);
        mp::PlayerSetupInfo info{};
        info.options = options;
        info.color = color;
        info.state = mp::kInfoStatePlaying;
        info.status = mp::status::has_disc;
        info.version_major = 3;
        oa::netgame::PlayerInfoRecord record{};
        std::memcpy(record.info_head, &info, sizeof(record.info_head));
        record.player_id = id;
        std::memcpy(
            record.info_tail,
            reinterpret_cast<const uint8_t*>(&info) + oa::netgame::player_info_tail_offset,
            sizeof(record.info_tail)
        );
        mp::LobbyEvent event{};
        event.kind = mp::LobbyEventKind::record;
        event.player_id = id;
        std::size_t written = 0;
        (void)oa::netgame::encode_record(record, event.data, sizeof(event.data), &written);
        event.size = static_cast<uint16_t>(written);
        expect(mp::lobby_apply_event(lobby, event), "player info applies");
        expect(mp::slot_info(lobby, slot)->color == color, "player info colour copied");
        mp::lobby_update_status(lobby, panel);
    }

    void synced(uint32_t id) {
        const auto slot = static_cast<uint8_t>(mp::slot_for_player_id(lobby, id));
        for (const uint8_t subtype : {uint8_t{1}, uint8_t{2}}) {
            uint8_t record[14] = {0x1a, subtype};
            record[10] = 1;
            mp::unit_sync_receive(lobby, record, slot);
        }
        // The peer acknowledges everything the host sent it.
        for (int32_t index = 0; index < lobby.sync.peer_count; ++index)
            if (lobby.sync.peers[index].player_id == id) {
                uint8_t record[14] = {0x1a, 4};
                const auto sent = lobby.sync.peers[index].sent;
                std::memcpy(record + 10, &sent, sizeof sent);
                mp::unit_sync_receive(lobby, record, slot);
            }
    }

    bool press(const char* control, uint8_t button = 1) {
        if (!mp::panel_press(panel, control, button))
            return false;
        (void)mp::lobby_handle_event(lobby, panel);
        return true;
    }

    int32_t sent_of(oa::netgame::RecordType type) const {
        int32_t count = 0;
        for (int32_t index = 0; index < loopback.sent_count; ++index)
            if (loopback.sent[index][0] == static_cast<uint8_t>(type))
                ++count;
        return count;
    }
};

void test_rows_and_colors(const oa::ui::gui_layout::Layout& lounge) {
    Fixture f(lounge);
    expect(mp::panel_find(f.panel, "PLAYER9") != mp::kNoControl, "rows cloned for slot 9");
    expect(mp::panel_find(f.panel, "TEAMICONS9") != mp::kNoControl, "team icon row cloned");
    const auto* own = mp::panel_control(f.panel, "PLAYER0");
    expect(own != nullptr && own->type == mp::ControlType::label, "local player row is a label");
    expect(mp::panel_text(f.panel, "PLAYER0") == "Host", "local name shown");
    expect(mp::panel_text(f.panel, "PLAYER3") == "UNUSED", "open slot reads UNUSED");
    const auto* p1 = mp::panel_control(f.panel, "PLAYER1");
    const auto* p0y = mp::panel_control(f.panel, "READY0");
    const auto* p1y = mp::panel_control(f.panel, "READY1");
    expect(
        p1 != nullptr && p0y != nullptr && p1y != nullptr && p1y->y - p0y->y == mp::kRowPitch,
        "rows are 20 pixels apart"
    );
    expect(mp::local_info(f.lobby).color == 0, "host takes the first colour");
    f.join(0x200, "Guest");
    f.remote_info(0x200, 0, 1);
    expect(mp::lobby_next_free_color(f.lobby) == 2, "next free colour skips used ones");
    mp::lobby_update_status(f.lobby, f.panel);
    expect(mp::panel_text(f.panel, "PLAYER1") == "Guest", "remote row shows its name");
    expect(mp::panel_text(f.panel, "PING1") != "n/a", "playing remote shows a ping");
    expect(f.sent_of(oa::netgame::RecordType::player_info) > 0, "host broadcast its player info");
}

void test_side_and_watch(const oa::ui::gui_layout::Layout& lounge) {
    Fixture f(lounge);
    auto& info = mp::local_info(f.lobby);
    expect(info.side == 0, "side starts at ARM");
    expect(f.press("SIDE0"), "SIDE0 clickable");
    expect(info.side == 1, "SIDE cycles to CORE");
    info.options &= static_cast<uint16_t>(~mp::option::watching_allowed);
    expect(f.press("SIDE0"), "SIDE0 clickable again");
    expect(
        info.side == 0 && (info.options & mp::option::watcher) == 0,
        "wrap without watching resets side"
    );
    info.options |= mp::option::watching_allowed;
    (void)f.press("SIDE0");
    (void)f.press("SIDE0");
    expect((info.options & mp::option::watcher) != 0, "wrap with watching allowed makes a watcher");
    mp::lobby_update_side_button(f.lobby, f.panel, 0);
    expect(mp::panel_control(f.panel, "SIDE0")->stage == 2, "watcher shows side stage 2");
    (void)f.press("SIDE0");
    expect(
        (info.options & mp::option::watcher) == 0 && info.side == 0,
        "leaving watch returns to side 0"
    );
}

// Talon: four sides, the spare OTHER (side 3) kept out of the battle room by
// identity.battle-room-hidden-sides. The SIDE button steps TALON, ARM, CORE
// and back to TALON, never to OTHER; leaving watch lands on a side offered.
void test_hidden_side(const oa::ui::gui_layout::Layout& lounge) {
    const oa::data::defs::DataLayout base = oa::data::defs::data_layout();
    oa::data::defs::DataLayout talon = base;
    talon.battle_room_hidden_sides = 1U << 3;
    oa::data::defs::use_data_layout(talon);
    {
        Fixture f(lounge);
        f.game->side_count = 4;
        auto& info = mp::local_info(f.lobby);
        info.options &= static_cast<uint16_t>(~mp::option::watching_allowed);
        for (const uint8_t side : std::array<uint8_t, 4>{1, 2, 0, 1}) {
            (void)f.press("SIDE0");
            expect(info.side == side, "the SIDE button steps past the hidden side");
        }
        expect(mp::lobby_offered_side(*f.game, 3) == 4, "a hidden last side offers none after it");
        expect(mp::lobby_offered_side(*f.game, 2) == 2, "an offered side is itself");
        info.side = 2;
        info.options |= mp::option::watching_allowed;
        (void)f.press("SIDE0");
        expect((info.options & mp::option::watcher) != 0, "past the last offered side, a watcher");
        (void)f.press("SIDE0");
        expect(
            (info.options & mp::option::watcher) == 0 && info.side == 0,
            "leaving watch lands on the first side offered"
        );
    }
    oa::data::defs::use_data_layout(base);
}

// A mod's SIDEDATA with three sides: the SIDE button offers each of them
// before the watcher, as the skirmish screen's does.
void test_three_sides(const oa::ui::gui_layout::Layout& lounge) {
    Fixture f(lounge);
    f.game->side_count = 3;
    auto& info = mp::local_info(f.lobby);
    info.options |= mp::option::watching_allowed;
    expect(info.side == 0, "side starts at the first side");
    expect(f.press("SIDE0"), "SIDE0 clickable");
    expect(info.side == 1, "SIDE cycles to the second side");
    (void)f.press("SIDE0");
    expect(
        info.side == 2 && (info.options & mp::option::watcher) == 0, "SIDE offers the third side"
    );
    mp::lobby_update_side_button(f.lobby, f.panel, 0);
    expect(mp::panel_control(f.panel, "SIDE0")->stage == 2, "the third side shows its own stage");
    (void)f.press("SIDE0");
    expect((info.options & mp::option::watcher) != 0, "past the third side comes the watcher");
    (void)f.press("SIDE0");
    expect(
        (info.options & mp::option::watcher) == 0 && info.side == 0,
        "leaving watch returns to the first side"
    );
}

// Past 3.1c's five sides a right click walks the SIDE cycle backwards.
// Within five it still steps forward.
void test_side_steps_back(const oa::ui::gui_layout::Layout& lounge) {
    Fixture forward(lounge);
    forward.game->side_count = 7;
    auto& watched = mp::local_info(forward.lobby);
    watched.options |= mp::option::watching_allowed;
    expect(watched.side == 0, "seven sides start at the first");
    expect(forward.press("SIDE0", 2), "right click from the first side");
    expect(
        (watched.options & mp::option::watcher) != 0, "a right click from the first side watches"
    );
    expect(forward.press("SIDE0", 2), "right click from the watcher");
    expect(
        (watched.options & mp::option::watcher) == 0 && watched.side == 6,
        "a right click from the watcher lands on the last side"
    );
    expect(forward.press("SIDE0", 2), "right click steps back");
    expect(watched.side == 5, "a right click steps back one side");

    Fixture plain(lounge);
    plain.game->side_count = 7;
    auto& unwatched = mp::local_info(plain.lobby);
    unwatched.options &= static_cast<uint16_t>(~mp::option::watching_allowed);
    expect(plain.press("SIDE0", 2), "right click where watching is off");
    expect(
        (unwatched.options & mp::option::watcher) == 0 && unwatched.side == 6,
        "without watching a right click wraps to the last side"
    );
    expect(plain.press("SIDE0", 2), "right click back again");
    expect(unwatched.side == 5, "without watching a right click steps back");

    const oa::data::defs::DataLayout base = oa::data::defs::data_layout();
    oa::data::defs::DataLayout hidden = base;
    hidden.battle_room_hidden_sides = 1U << 5;
    oa::data::defs::use_data_layout(hidden);
    {
        Fixture skipped(lounge);
        skipped.game->side_count = 7;
        auto& info = mp::local_info(skipped.lobby);
        info.options &= static_cast<uint16_t>(~mp::option::watching_allowed);
        info.side = 4;
        expect(skipped.press("SIDE0"), "left click toward the hidden side");
        expect(info.side == 6, "a left click skips the hidden side");
        expect(skipped.press("SIDE0", 2), "right click toward the hidden side");
        expect(info.side == 4, "a right click skips the hidden side");
        expect(
            mp::lobby_offered_side_before(*skipped.game, 6) == 4,
            "the side before a hidden one is the one it skipped"
        );
    }
    oa::data::defs::use_data_layout(base);

    Fixture few(lounge);
    few.game->side_count = 4;
    auto& within = mp::local_info(few.lobby);
    within.options &= static_cast<uint16_t>(~mp::option::watching_allowed);
    expect(few.press("SIDE0", 2), "right click within five sides");
    expect(
        within.side == 1 && (within.options & mp::option::watcher) == 0,
        "within five sides a right click steps forward"
    );
}

void test_teams_and_alliances(const oa::ui::gui_layout::Layout& lounge) {
    Fixture f(lounge);
    f.join(0x200, "Guest");
    f.remote_info(0x200, 0, 1);
    auto& me = mp::local_player(f.lobby);
    expect(f.press("ALLY1"), "ALLY1 clickable");
    expect(me.alliance[1] == 1, "ally toggles on");
    expect(f.sent_of(oa::netgame::RecordType::alliance) == 1, "alliance record sent to the remote");
    expect(
        std::strstr(mp::lobby_chat_line(*f.game, 0), "allied with Guest") != nullptr,
        "alliance announced"
    );
    (void)f.press("ALLY1");
    expect(me.alliance[1] == 0, "ally toggles off");

    // In another language the line goes to the others in English, as
    // English 3.1c sends it, and shows here in the language shown; a line
    // from another machine shows so too, the names as they came.
    struct Phrases {
        const char* language;
        const char* allied;
        const char* broke;
        const char* missing;
    };

    static const Phrases german{
        "German", "Verb\xc3\xbcndet mit", "k\xc3\xbcndigt Allianz mit", "hat diese Karte nicht"
    };
    static const Phrases chinese{
        "Chinese",
        "\xe7\xbb\x93\xe7\x9b\x9f",
        "\xe8\xa7\xa3\xe9\x99\xa4\xe7\xbb\x93\xe7\x9b\x9f",
        "\xe6\xb2\xa1\xe6\x9c\x89\xe6\xad\xa4\xe5\x9c\xb0\xe5\x9b\xbe"
    };
    f.lobby.services.translate = [](void* context, const char* text) -> const char* {
        const auto& phrases = *static_cast<const Phrases*>(context);
        const std::string_view english(text);
        return english == "allied with"              ? phrases.allied
               : english == "broke alliance with"    ? phrases.broke
               : english == "does not have this map" ? phrases.missing
                                                     : nullptr;
    };
    const auto last_shown = [&f] {
        const auto head = mp::lobby_chat_head(*f.game);
        return std::string(
            mp::lobby_chat_line(*f.game, static_cast<std::size_t>(head + mp::kChatLines - 1))
        );
    };
    const auto last_sent = [&f] {
        std::string text;
        for (int32_t index = 0; index < f.loopback.sent_count; ++index) {
            const auto* record = f.loopback.sent[index];
            if (record[0] == static_cast<uint8_t>(oa::netgame::RecordType::chat))
                text.assign(
                    reinterpret_cast<const char*>(record + 1),
                    ::strnlen(reinterpret_cast<const char*>(record + 1), mp::kLobbyRecordBytes - 1)
                );
        }
        return text;
    };
    const auto hear = [&f](const char* line) {
        oa::netgame::ChatRecord record{};
        std::memcpy(record.text, line, std::min(std::strlen(line), sizeof record.text));
        mp::LobbyEvent event{};
        event.kind = mp::LobbyEventKind::record;
        event.player_id = 0x200;
        std::size_t written = 0;
        (void)oa::netgame::encode_record(record, event.data, sizeof(event.data), &written);
        event.size = static_cast<uint16_t>(written);
        expect(mp::lobby_apply_event(f.lobby, event), "a chat line applies");
    };
    for (const Phrases* phrases : {&german, &chinese}) {
        const std::string language = phrases->language;
        f.lobby.services.context = const_cast<Phrases*>(phrases);
        (void)f.press("ALLY1");
        expect(me.alliance[1] == 1, "ally toggles on again");
        expect(
            last_sent() == "<Host>  allied with Guest",
            (language + ": the alliance line goes in English").c_str()
        );
        expect(
            last_shown() == std::string("<Host>  ") + phrases->allied + " Guest",
            (language + ": the alliance line shows in the language shown").c_str()
        );
        (void)f.press("ALLY1");
        expect(me.alliance[1] == 0, "ally toggles off again");
        expect(
            last_sent() == "<Host>  broke alliance with Guest",
            (language + ": the broken alliance goes in English").c_str()
        );
        expect(
            last_shown() == std::string("<Host>  ") + phrases->broke + " Guest",
            (language + ": the broken alliance shows in the language shown").c_str()
        );
        hear("<Guest>  allied with Host");
        expect(
            last_shown() == std::string("<Guest>  ") + phrases->allied + " Host",
            (language + ": a received alliance line shows in the language shown").c_str()
        );
        hear("<Guest> does not have this map");
        expect(
            last_shown() == std::string("<Guest> ") + phrases->missing,
            (language + ": a received missing-map line shows in the language shown").c_str()
        );
        hear("<Guest> allied with them yesterday");
        expect(
            last_shown() == "<Guest> allied with them yesterday",
            (language + ": other chat shows as it came").c_str()
        );
    }
    // English has no translation and shows what it receives.
    f.lobby.services.translate = nullptr;
    f.lobby.services.context = nullptr;
    hear("<Guest>  broke alliance with Host");
    expect(last_shown() == "<Guest>  broke alliance with Host", "English shows the English");

    expect(f.press("TEAMICONS0"), "TEAMICONS0 clickable");
    expect(mp::lobby_player_team(me) == 0, "no team -> team 0 wraps from 5");
    mp::lobby_player_team(mp::slot_player(f.lobby, 1)) = 0;
    mp::lobby_update_ally_matrix(f.lobby);
    expect(
        me.alliance[1] == 1 && mp::lobby_player_allied_back(me)[1] == 1, "team mates ally both ways"
    );
    expect(mp::team_member_count(f.lobby, 0) == 2, "team count includes both");
    expect(mp::lobby_all_on_one_team(f.lobby), "all players on one team detected");
    mp::lobby_update_team_icons(f.lobby, f.panel);
    expect(mp::panel_control(f.panel, "TEAMICONS0")->stage == 0, "shared team icon stage team*2");
    (void)f.press("TEAMICONS0");
    expect(mp::lobby_player_team(me) == 1, "team advances");
    mp::lobby_update_team_icons(f.lobby, f.panel);
    expect(mp::panel_control(f.panel, "TEAMICONS0")->stage == 3, "lone team icon stage team*2+1");
    for (int i = 0; i < 4; ++i)
        (void)f.press("TEAMICONS0");
    expect(mp::lobby_player_team(me) == mp::kNoTeam, "team wraps to none after 4");
}

void test_options_and_sliders(const oa::ui::gui_layout::Layout& lounge) {
    Fixture f(lounge);
    auto& info = mp::local_info(f.lobby);
    const uint16_t before = info.options & mp::option::commander_mask;
    expect(before == 0, "commander continues by default");
    (void)f.press("COMMANDER");
    expect((info.options & mp::option::commander_mask) == 0x800, "commander -> game ends");
    (void)f.press("COMMANDER");
    expect((info.options & mp::option::commander_mask) == 0x1000, "commander -> deathmatch");
    (void)f.press("COMMANDER");
    expect((info.options & mp::option::commander_mask) == 0, "commander wraps");
    (void)f.press("LOSTYPE");
    expect((info.options & 0x600) == 0x600, "LOS permanent -> true");
    (void)f.press("LOSTYPE");
    expect((info.options & 0x600) == 0x200, "LOS true -> circular");
    (void)f.press("LOSTYPE");
    expect((info.options & 0x600) == 0, "LOS circular -> permanent");
    mp::lobby_update_option_buttons(f.lobby, f.panel);
    expect(mp::panel_control(f.panel, "LOSTYPE")->stage == 2, "permanent LOS stage 2");
    (void)f.press("GAMEOPEN");
    expect((info.options & mp::option::game_closed) != 0, "GAMEOPEN closes the game");
    (void)f.press("GAMEOPEN");
    expect((info.options & mp::option::game_closed) == 0, "GAMEOPEN reopens");
    (void)f.press("CHEATING");
    expect((info.options & mp::option::cheats_allowed) != 0, "cheats toggle");

    auto* metal = mp::panel_control(f.panel, "METAL");
    expect(metal != nullptr && metal->scroll.maximum == 0x2711, "METAL maximum 10001");
    expect(info.metal_hundreds == 10, "metal starts at 1000");
    // 2549 puts the knob at step 23 of 87, which stands for 2643.
    oa::ui::gui_input::scroll_set_value(metal->scroll, 2549);
    mp::lobby_on_metal(f.panel, &f.lobby);
    expect(metal->scroll.knob == 23, "2549 metal takes the knob to step 23");
    expect(info.metal_hundreds == 26, "metal rounds down to hundreds");
    expect(mp::panel_text(f.panel, "METALTEXT") == "2600", "METALTEXT shows rounded metal");
    oa::ui::gui_input::scroll_set_value(metal->scroll, 50000);
    mp::lobby_on_metal(f.panel, &f.lobby);
    expect(info.metal_hundreds == 100, "metal clamps to the slider maximum");
    auto* units = mp::panel_control(f.panel, "MAXUNITS");
    oa::ui::gui_input::scroll_set_value(units->scroll, 0);
    mp::lobby_on_max_units(f.panel, &f.lobby);
    expect(info.max_units == 20, "MAXUNITS floor 20");
    oa::ui::gui_input::scroll_set_value(units->scroll, 230);
    mp::lobby_on_max_units(f.panel, &f.lobby);
    expect(info.max_units == 250, "MAXUNITS top is the game limit");
    expect(mp::panel_text(f.panel, "MAXUNITSTEXT") == "250", "MAXUNITSTEXT follows");
}

/// Returns SLIDERS frame sizes laid out as COMMONGUI.GAF's: a vertical
/// bar's art from frame 0 and a horizontal bar's from frame 10, each a
/// 16-pixel track, a 10-pixel knob and two 9-pixel arrows with held faces.
///
/// @return the 20 frame sizes
std::vector<oa::ui::gui_input::ScrollFrame> shared_slider_frames() {
    std::vector<oa::ui::gui_input::ScrollFrame> frames(20);
    for (int base : {0, 10}) {
        const bool across = base == 10;
        for (int frame = 0; frame < 3; ++frame)
            frames[static_cast<std::size_t>(base + frame)] = {16, 16};
        for (int frame = 3; frame < 6; ++frame)
            frames[static_cast<std::size_t>(base + frame)] = {10, 10};
        for (int frame = 6; frame < 10; ++frame)
            frames[static_cast<std::size_t>(base + frame)] =
                across ? oa::ui::gui_input::ScrollFrame{9, 16}
                       : oa::ui::gui_input::ScrollFrame{16, 9};
    }
    return frames;
}

/// Builds a panel of sliders: METAL as LOUNGE2.GUI has it, a vertical bar, a
/// vertical bar that scrolls a list in its group, and an item after them.
///
/// @return the layout
oa::ui::gui_layout::Layout sliders_layout() {
    namespace gui = oa::ui::gui_layout;
    const auto gadget = [](gui::GadgetType type,
                           const char* name,
                           uint8_t group,
                           int16_t x,
                           int16_t y,
                           int16_t width,
                           int16_t height,
                           int32_t attributes) {
        gui::Gadget made;
        made.common.type = type;
        made.common.name = name;
        made.common.association = group;
        made.common.x = x;
        made.common.y = y;
        made.common.width = width;
        made.common.height = height;
        made.common.attributes = attributes;
        made.common.active = 1;
        if (type == gui::GadgetType::scroll_bar)
            made.fields = gui::ScrollBarFields{106, 22, 0, 10, "", ""};
        return made;
    };
    gui::Layout layout;
    layout.gadgets.push_back(gadget(gui::GadgetType::panel, "sliders.GUI", 0, 0, 0, 640, 480, 0));
    layout.gadgets.push_back(gadget(gui::GadgetType::scroll_bar, "METAL", 30, 510, 24, 120, 16, 1));
    layout.gadgets.push_back(
        gadget(gui::GadgetType::scroll_bar, "UPDOWN", 7, 100, 100, 20, 120, 2)
    );
    layout.gadgets.push_back(gadget(gui::GadgetType::list_box, "ROWS", 1, 200, 100, 100, 190, 1));
    layout.gadgets.push_back(
        gadget(gui::GadgetType::scroll_bar, "SLIDER", 1, 300, 100, 16, 190, 2)
    );
    layout.gadgets.push_back(gadget(gui::GadgetType::label, "", 0, 0, 0, 10, 10, 0));
    return layout;
}

/// Checks how panel_bind_sliders binds a panel's sliders to the engine's bars.
void test_slider_binding() {
    oa::ui::gui_input::ScrollArtFrames art;
    art.shared = shared_slider_frames();
    mp::Panel panel;
    mp::panel_load(panel, "sliders", sliders_layout());
    const auto loaded = panel.count;
    expect(
        mp::panel_control(panel, "METAL")->scroll.maximum == 22,
        "a slider's maximum is its GUI thickness"
    );
    mp::panel_bind_sliders(panel, art);
    expect(panel.count == loaded, "the arrows are the bars' own, not controls");

    const auto metal_index = mp::panel_find(panel, "METAL");
    const auto* metal = mp::panel_control(panel, "METAL");
    expect(
        metal->x == 519 && metal->y == 24 && metal->width == 102 && metal->height == 16 &&
            metal->scroll.rect.x == 519 && metal->scroll.rect.width == 102,
        "a horizontal slider lies between its arrows"
    );
    expect(
        metal->scroll.range == 88 && metal->scroll.knob_size == 10 &&
            metal->scroll.art == oa::ui::gui_input::ScrollArt::shared &&
            metal->scroll.art_base == 10,
        "a horizontal bar takes the shared art from frame 10 and 88 steps"
    );
    const auto& back_arrow = metal->scroll.back_arrow;
    const auto& forward_arrow = metal->scroll.forward_arrow;
    expect(
        back_arrow.x == 510 && back_arrow.y == 24 && back_arrow.width == 9 &&
            back_arrow.height == 16 && forward_arrow.x == 621 && forward_arrow.y == 24 &&
            forward_arrow.width == 9 && forward_arrow.height == 16,
        "the arrows sit at the bar's start and end"
    );
    expect(
        mp::panel_hit(panel, 512, 30) == metal_index, "an arrow takes the pointer for its slider"
    );
    panel.controls[static_cast<std::size_t>(metal_index)].active = 0;
    expect(mp::panel_hit(panel, 512, 30) == mp::kNoControl, "a hidden slider's arrows are hidden");
    panel.controls[static_cast<std::size_t>(metal_index)].active = 1;

    const auto* updown = mp::panel_control(panel, "UPDOWN");
    expect(
        updown->x == 100 && updown->y == 109 && updown->width == 16 && updown->height == 102 &&
            updown->scroll.range == 106 && updown->scroll.art_base == 0 &&
            updown->scroll.back_arrow.y == 100 && updown->scroll.forward_arrow.y == 211,
        "a vertical bar takes the art from frame 0, its width and the room between its arrows"
    );
    const auto* listed = mp::panel_control(panel, "SLIDER");
    expect(
        listed->scroll.art == oa::ui::gui_input::ScrollArt::shared && listed->y == 109 &&
            listed->height == 172 && listed->scroll.knob == 0 &&
            listed->scroll.back_arrow.y == 100 && listed->scroll.forward_arrow.y == 281,
        "a slider that scrolls a list is bound between its arrows as any vertical bar"
    );
    expect(
        listed->active == 0 && mp::panel_hit(panel, 305, 104) == mp::kNoControl &&
            mp::panel_hit(panel, 305, 150) == mp::kNoControl,
        "a list's scroll bar and its arrows are hidden until the list is filled"
    );
    expect(!listed->notifies_panel, "a list's scroll bar does not click its panel");

    mp::Panel bare;
    mp::panel_load(bare, "sliders", sliders_layout());
    mp::panel_bind_sliders(bare, {});
    const auto* bare_metal = mp::panel_control(bare, "METAL");
    expect(
        bare_metal->scroll.art == oa::ui::gui_input::ScrollArt::none &&
            bare_metal->scroll.range == 114 && bare_metal->x == 510,
        "without art a slider has no arrows and its longer side less 6 steps"
    );
}

/// Checks the pointer on a bound slider and its arrows: drags, steps toward
/// the pointer, and the arrows' steps and repeat.
void test_slider_input() {
    oa::ui::gui_input::ScrollArtFrames art;
    art.shared = shared_slider_frames();
    mp::Panel panel;
    mp::panel_load(panel, "sliders", sliders_layout());
    mp::panel_bind_sliders(panel, art);
    const auto index = mp::panel_find(panel, "METAL");
    auto& metal = panel.controls[static_cast<std::size_t>(index)];
    // A press on an arrow is a press on its slider where the arrow is.
    const auto back = index;
    const auto forward = index;
    metal.scroll.knob = 9;
    oa::ui::gui_input::ScrollHold hold;
    uint32_t tick = 500;

    // A press on the knob drags it pixel for pixel, once an update, however
    // often the pointer moves in between; the release lands on it.
    const int32_t knob_x = metal.x + metal.scroll.knob + 5;
    expect(
        mp::slider_press(panel, hold, index, knob_x, 32, tick) == mp::kNoControl,
        "a press moves nothing"
    );
    expect(hold.bar == index && hold.dragging, "a press on the knob drags it");
    for (const int32_t moved : {5, 12, 20})
        oa::ui::gui_input::scroll_move(hold, knob_x + moved, 5);
    expect(metal.scroll.knob == 9, "the pointer's moves alone leave the knob");
    expect(
        mp::slider_hold_tick(panel, hold, tick) == index && metal.scroll.knob == 29,
        "the next update moves the knob to the pointer, once"
    );
    expect(
        mp::slider_hold_tick(panel, hold, tick) == mp::kNoControl &&
            mp::slider_hold_tick(panel, hold, tick + 1) == mp::kNoControl &&
            metal.scroll.knob == 29,
        "a dragged knob does not creep"
    );
    expect(
        mp::slider_release(panel, hold, knob_x + 20, 5) == mp::kNoControl &&
            metal.scroll.knob == 29,
        "released on the knob, it stays"
    );
    expect(hold.bar == mp::kNoControl, "the release ends the hold");
    (void)mp::slider_press(panel, hold, index, metal.x + metal.scroll.knob + 3, 32, tick);
    oa::ui::gui_input::scroll_move(hold, 700, 32);
    expect(
        mp::slider_hold_tick(panel, hold, tick) == index && metal.scroll.knob == 87,
        "a drag stops at the last step"
    );
    oa::ui::gui_input::scroll_move(hold, 0, 32);
    expect(
        mp::slider_hold_tick(panel, hold, tick) == index && metal.scroll.knob == 0,
        "and at the first"
    );
    // Released before the knob, the release steps it toward the pointer.
    expect(
        mp::slider_release(panel, hold, 0, 32) == mp::kNoControl && metal.scroll.knob == 0,
        "the knob stays at the first step"
    );
    // A drag moved and released between two updates ends where it was last
    // updated, and the release steps it once toward the pointer.
    (void)mp::slider_press(panel, hold, index, metal.x + metal.scroll.knob + 3, 32, tick);
    oa::ui::gui_input::scroll_move(hold, metal.x + 30, 32);
    expect(
        mp::slider_release(panel, hold, metal.x + 30, 32) == index && metal.scroll.knob == 1,
        "the release does not drag"
    );

    // A press on the track holds it: one step toward the pointer each tick,
    // and one more on release.
    metal.scroll.knob = 40;
    expect(
        mp::slider_press(panel, hold, index, 610, 30, tick) == mp::kNoControl && !hold.dragging,
        "a press on the track does not drag"
    );
    expect(
        mp::slider_hold_tick(panel, hold, tick) == mp::kNoControl && metal.scroll.knob == 40,
        "no step in the tick of the press"
    );
    for (uint32_t step = 1; step <= 5; ++step) {
        expect(
            mp::slider_hold_tick(panel, hold, tick + step) == index, "a held track steps each tick"
        );
        expect(mp::slider_hold_tick(panel, hold, tick + step) == mp::kNoControl, "and once a tick");
    }
    expect(metal.scroll.knob == 45, "five ticks, five steps toward the pointer");
    expect(
        mp::slider_release(panel, hold, 610, 30) == index && metal.scroll.knob == 46,
        "the release steps once more"
    );
    (void)mp::slider_press(panel, hold, index, 520, 30, tick);
    expect(
        mp::slider_release(panel, hold, 520, 30) == index && metal.scroll.knob == 45,
        "a quick click before the knob steps back once"
    );

    // An arrow steps at once, then after 15 ticks once a tick; its handler runs even at the end.
    tick = 700;
    expect(
        mp::slider_press(panel, hold, forward, 625, 30, tick) == index && metal.scroll.knob == 46,
        "a forward arrow steps at once"
    );
    int32_t steps = 0;
    for (uint32_t step = 0; step < 15; ++step)
        if (mp::slider_hold_tick(panel, hold, tick + step) != mp::kNoControl)
            ++steps;
    expect(steps == 0 && metal.scroll.knob == 46, "a held arrow waits 15 ticks");
    expect(
        mp::slider_hold_tick(panel, hold, tick + 15) == index && metal.scroll.knob == 47,
        "then repeats"
    );
    expect(mp::slider_hold_tick(panel, hold, tick + 15) == mp::kNoControl, "once a tick");
    expect(
        mp::slider_hold_tick(panel, hold, tick + 16) == index && metal.scroll.knob == 48,
        "every tick"
    );
    expect(
        mp::slider_release(panel, hold, 625, 30) == mp::kNoControl && hold.bar == mp::kNoControl,
        "an arrow's release does nothing"
    );
    metal.scroll.knob = 0;
    expect(
        mp::slider_press(panel, hold, back, 512, 30, tick) == index && metal.scroll.knob == 0,
        "a back arrow at the start still runs the handler"
    );
    (void)mp::slider_release(panel, hold, 512, 30);
    metal.scroll.knob = 87;
    expect(
        mp::slider_press(panel, hold, forward, 625, 30, tick) == index && metal.scroll.knob == 87,
        "a forward arrow at the end still runs the handler"
    );

    // A slider grayed while held lets go.
    metal.grayed = true;
    expect(
        mp::slider_hold_tick(panel, hold, tick + 40) == mp::kNoControl &&
            hold.bar == mp::kNoControl,
        "a grayed slider ends the hold"
    );
    expect(
        mp::slider_press(panel, hold, forward, 625, 30, tick) == mp::kNoControl &&
            metal.scroll.knob == 87,
        "a grayed slider's arrow ignores a press"
    );
    metal.grayed = false;
    const auto listed = mp::panel_find(panel, "SLIDER");
    expect(
        mp::slider_press(panel, hold, listed, 305, 150, tick) == mp::kNoControl &&
            hold.bar == mp::kNoControl,
        "a hidden scroll bar is not held"
    );
}

/// Builds a panel of lists: ROWS and NAMES scrolled by SLIDER in group 1,
/// CHAT alone in group 2, and EMPTY with BAR3 in group 3. ROWS takes the focus.
///
/// @return the layout
oa::ui::gui_layout::Layout lists_layout() {
    namespace gui = oa::ui::gui_layout;
    const auto gadget = [](gui::GadgetType type,
                           const char* name,
                           uint8_t group,
                           int16_t x,
                           int16_t y,
                           int16_t width,
                           int16_t height,
                           int32_t attributes) {
        gui::Gadget made;
        made.common.type = type;
        made.common.name = name;
        made.common.association = group;
        made.common.x = x;
        made.common.y = y;
        made.common.width = width;
        made.common.height = height;
        made.common.attributes = attributes;
        made.common.active = 1;
        if (type == gui::GadgetType::scroll_bar)
            made.fields = gui::ScrollBarFields{106, 22, 0, 10, "", ""};
        else if (type == gui::GadgetType::list_box)
            made.fields = gui::ListBoxFields{};
        return made;
    };
    gui::Layout layout;
    auto root = gadget(gui::GadgetType::panel, "lists.GUI", 0, 0, 0, 640, 480, 0);
    gui::PanelFields fields;
    fields.default_focus = "ROWS";
    root.fields = fields;
    layout.gadgets.push_back(root);
    layout.gadgets.push_back(gadget(gui::GadgetType::list_box, "ROWS", 1, 200, 100, 100, 190, 1));
    layout.gadgets.push_back(gadget(gui::GadgetType::list_box, "NAMES", 1, 100, 100, 90, 190, 1));
    layout.gadgets.push_back(
        gadget(gui::GadgetType::scroll_bar, "SLIDER", 1, 300, 100, 16, 190, 2)
    );
    layout.gadgets.push_back(gadget(gui::GadgetType::list_box, "CHAT", 2, 0, 300, 100, 96, 1));
    layout.gadgets.push_back(gadget(gui::GadgetType::list_box, "EMPTY", 3, 400, 100, 90, 190, 1));
    layout.gadgets.push_back(gadget(gui::GadgetType::scroll_bar, "BAR3", 3, 500, 100, 16, 190, 2));
    return layout;
}

/// Returns count rows named by their number.
std::vector<std::string> numbered_rows(int32_t count) {
    std::vector<std::string> rows;
    for (int32_t row = 0; row < count; ++row)
        rows.push_back(std::to_string(row));
    return rows;
}

/// Checks the lists and the scroll bar that scrolls them: the first draw's
/// trim, filling, the knob's size and range, the two-way link between knob
/// and rows, a screen's pick, the keys, presses and the wheel's scroll.
void test_list_scrolling() {
    oa::ui::gui_input::ScrollArtFrames art;
    art.shared = shared_slider_frames();
    mp::Panel panel;
    mp::panel_load(panel, "lists", lists_layout());
    const auto rows_index = mp::panel_find(panel, "ROWS");
    const auto names_index = mp::panel_find(panel, "NAMES");
    const auto bar_index = mp::panel_find(panel, "SLIDER");
    const auto chat_index = mp::panel_find(panel, "CHAT");
    expect(panel.focus == rows_index, "the GUI's defaultfocus takes the focus");
    panel.line_height = 14;
    auto& rows = panel.controls[static_cast<std::size_t>(rows_index)];
    auto& names = panel.controls[static_cast<std::size_t>(names_index)];
    auto& bar = panel.controls[static_cast<std::size_t>(bar_index)];
    auto& chat = panel.controls[static_cast<std::size_t>(chat_index)];
    rows.list_first = 3;
    rows.list_selection = 4;
    mp::panel_bind_lists(panel);
    mp::panel_bind_sliders(panel, art);
    expect(
        rows.height == 176 && chat.height == 96 && rows.list_first == 0 && rows.list_selection == 0,
        "the first draw trims each list to whole 16-pixel rows and shows its first row"
    );
    expect(bar.y == 109 && bar.height == 172 && bar.active == 0, "the bar is bound and hidden");
    expect(
        mp::panel_group_member(panel, rows_index, mp::ControlType::slider) == bar_index &&
            mp::panel_group_member(panel, bar_index, mp::ControlType::list_box) == rows_index &&
            mp::panel_group_member(panel, chat_index, mp::ControlType::slider) == mp::kNoControl,
        "a list's scroll bar shares its group"
    );

    // Eleven 15-pixel rows fit in 176 pixels; twelve do not.
    mp::panel_fill_list(panel, "ROWS", numbered_rows(11));
    expect(
        rows.list_item_height == 15 && rows.list_last_first == 0 && bar.active == 0,
        "rows that fit leave the bar hidden"
    );
    bar.scroll.knob = 5;
    mp::panel_fill_list(panel, "ROWS", numbered_rows(12));
    mp::panel_fill_list(panel, "NAMES", numbered_rows(12));
    expect(
        bar.active != 0 && rows.list_last_first == 1 && bar.scroll.knob_size == 154 &&
            bar.scroll.range == 15,
        "rows that overflow show the bar with a knob of 11/12 of 169 pixels and 15 steps"
    );
    expect(bar.scroll.knob == 5, "filling a list leaves the knob where it was");
    expect(
        mp::panel_hit(panel, 305, 104) != mp::kNoControl, "a shown bar's arrows take the pointer"
    );
    bar.scroll.knob = 0;
    oa::ui::gui_input::ScrollHold hold;
    const auto forward = bar_index;
    expect(
        mp::slider_press(panel, hold, forward, 305, 285, 100) == bar_index && bar.scroll.knob == 1,
        "the down arrow steps the knob"
    );
    (void)mp::slider_release(panel, hold, 305, 285);
    mp::panel_sync_group(panel, bar_index);
    expect(rows.list_first == 0 && names.list_first == 0, "one step of 14 does not scroll a row");
    bar.scroll.knob = 14;
    mp::panel_sync_group(panel, bar_index);
    expect(
        rows.list_first == 1 && names.list_first == 1 && rows.list_selection == 0,
        "the last step shows the last page and keeps the selection"
    );

    // A hundred rows: 11 on a page, the last page from row 89, a knob of
    // 11/100 of 169 pixels and 151 steps.
    mp::panel_fill_list(panel, "ROWS", numbered_rows(100));
    mp::panel_fill_list(panel, "NAMES", numbered_rows(100));
    expect(
        rows.list_last_first == 89 && bar.scroll.knob_size == 18 && bar.scroll.range == 151,
        "a hundred rows size the knob"
    );
    bar.scroll.knob = 40;
    mp::panel_sync_group(panel, bar_index);
    expect(rows.list_first == 23 && names.list_first == 23, "the knob shows 89 * 40 / 150 rows on");
    mp::panel_select_list_row(panel, "ROWS", 30);
    expect(
        rows.list_selection == 30 && rows.list_first == 23 && bar.scroll.knob == 40,
        "a pick on the page does not scroll"
    );
    mp::panel_select_list_row(panel, "ROWS", 60);
    expect(
        rows.list_selection == 60 && rows.list_first == 60 && bar.scroll.knob == 101,
        "a pick off the page scrolls to it and moves the knob to 151 * 60 / 89"
    );
    mp::panel_select_list_row(panel, "ROWS", 95);
    expect(
        rows.list_first == 89 && bar.scroll.knob == 151,
        "a pick on the last page stops at it, the knob one past its last step"
    );

    // Up and Down move a row and scroll at the page's edges; the group follows.
    rows.list_first = 40;
    rows.list_selection = 50;
    mp::panel_step_list(panel, rows_index, true);
    expect(
        rows.list_selection == 51 && rows.list_first == 41 && names.list_first == 41 &&
            names.list_selection == 51 && bar.scroll.knob == 69,
        "Down past the page's last row scrolls a row and the knob follows"
    );
    mp::panel_step_list(panel, rows_index, false);
    expect(
        rows.list_selection == 50 && rows.list_first == 41, "Up on the page moves the selection"
    );
    rows.list_selection = 41;
    mp::panel_step_list(panel, rows_index, false);
    expect(rows.list_selection == 40 && rows.list_first == 40, "Up past the top scrolls back");
    rows.list_first = 89;
    rows.list_selection = 99;
    mp::panel_step_list(panel, rows_index, true);
    expect(rows.list_selection == 99 && rows.list_first == 89, "Down stops at the last row");
    rows.list_first = 0;
    rows.list_selection = 0;
    mp::panel_step_list(panel, rows_index, false);
    expect(rows.list_selection == 0 && rows.list_first == 0, "Up stops at the first row");
    rows.list_selection = 60;
    mp::panel_step_list(panel, rows_index, true);
    expect(
        rows.list_selection == 60 && rows.list_first == 60,
        "a selection off the page is brought into view first"
    );

    // A press selects the row under the pointer, focuses the list and gives the group its row.
    panel.focus = mp::kNoControl;
    expect(
        mp::panel_press_list(panel, rows_index, 148) == mp::ListPress::changed &&
            rows.list_selection == 63 && names.list_selection == 63 && panel.focus == rows_index,
        "a press selects its row for the whole group and focuses the list"
    );
    expect(
        mp::panel_press_list(panel, rows_index, 150) == mp::ListPress::same, "again, the same row"
    );
    expect(
        mp::panel_press_list(panel, rows_index, 272) == mp::ListPress::changed &&
            rows.list_selection == 70,
        "a press on the page's last pixels selects its last row"
    );
    expect(
        mp::panel_press_list(panel, rows_index, 273) == mp::ListPress::missed &&
            mp::panel_press_list(panel, rows_index, 101) == mp::ListPress::missed &&
            rows.list_selection == 70,
        "a press on the list's edge misses"
    );
    mp::panel_fill_list(panel, "ROWS", numbered_rows(3));
    expect(bar.active == 0, "three rows hide the bar again");
    expect(
        mp::panel_press_list(panel, rows_index, 190) == mp::ListPress::changed &&
            rows.list_selection == 2 && names.list_selection == 2,
        "a press below the last row selects the last row"
    );
    names.list_first = 0;
    expect(
        mp::panel_press_list(panel, names_index, 102 + 15 * 10) == mp::ListPress::changed &&
            names.list_selection == 10 && rows.list_selection == 2,
        "the group takes a row no further than its own last"
    );
    expect(
        mp::panel_press_list(panel, chat_index, 303) == mp::ListPress::missed &&
            panel.focus == names_index,
        "a press on an empty list misses and keeps the focus"
    );
    mp::panel_set_items(panel, "CHAT", {"hello"});
    expect(
        mp::panel_press_list(panel, chat_index, 303) == mp::ListPress::same &&
            panel.focus == chat_index,
        "a press on a list with no scroll bar focuses it too"
    );

    // The wheel's scroll stays within the pages, even for an empty list.
    mp::panel_fill_list(panel, "ROWS", numbered_rows(100));
    rows.list_first = 88;
    expect(
        mp::panel_scroll_list(panel, rows_index, 1) && rows.list_first == 89 &&
            bar.scroll.knob == 151 && names.list_first == 89,
        "the wheel scrolls a row and the knob and group follow"
    );
    expect(
        !mp::panel_scroll_list(panel, rows_index, 1) && rows.list_first == 89,
        "not past the last page"
    );
    rows.list_first = 0;
    expect(
        !mp::panel_scroll_list(panel, rows_index, -1) && rows.list_first == 0,
        "nor before the first"
    );
    mp::panel_fill_list(panel, "EMPTY", {});
    const auto empty_index = mp::panel_find(panel, "EMPTY");
    const auto& empty = panel.controls[static_cast<std::size_t>(empty_index)];
    expect(
        empty.list_last_first == -1 && mp::panel_control(panel, "BAR3")->active == 0 &&
            !mp::panel_scroll_list(panel, empty_index, 1) &&
            !mp::panel_scroll_list(panel, empty_index, -1) && empty.list_first == 0,
        "an empty list neither shows its bar nor scrolls"
    );
    expect(
        !mp::panel_scroll_list(panel, chat_index, 1) && chat.list_first == 0,
        "the wheel leaves a list with no pitch alone"
    );
}

/// Checks LOUNGE2's sliders once bound with the installed art: their
/// geometry, their starting values and an arrow step's player info.
void test_lounge_sliders(const oa::ui::gui_layout::Layout& lounge) {
    Fixture f(lounge);
    const auto index = mp::panel_find(f.panel, "METAL");
    const auto* metal = mp::panel_control(f.panel, "METAL");
    expect(
        metal != nullptr && metal->x == 519 && metal->y == 24 && metal->width == 102 &&
            metal->scroll.range == 88 && metal->scroll.knob_size == 10,
        "METAL lies between its arrows with 88 steps"
    );
    if (metal == nullptr)
        return;
    const auto& back_arrow = metal->scroll.back_arrow;
    const auto& forward_arrow = metal->scroll.forward_arrow;
    expect(
        back_arrow.x == 510 && back_arrow.y == 24 && back_arrow.width == 9 &&
            back_arrow.height == 16,
        "METAL's back arrow"
    );
    expect(
        forward_arrow.x == 621 && forward_arrow.y == 24 && forward_arrow.width == 9 &&
            forward_arrow.height == 16,
        "METAL's forward arrow"
    );
    const auto forward = index;
    const auto* energy = mp::panel_control(f.panel, "ENERGY");
    const auto* units = mp::panel_control(f.panel, "MAXUNITS");
    expect(
        metal->scroll.knob == 9 && mp::panel_text(f.panel, "METALTEXT") == "1000",
        "metal starts at 1000 with the knob at step 9"
    );
    expect(
        energy != nullptr && energy->scroll.knob == 9 &&
            mp::panel_text(f.panel, "ENERGYTEXT") == "1000",
        "energy starts at 1000 with the knob at step 9"
    );
    expect(
        units != nullptr && units->scroll.range == 88 && units->scroll.knob == 87 &&
            mp::panel_text(f.panel, "MAXUNITSTEXT") == "250",
        "max units starts at 250 with the knob at the last step"
    );

    auto& info = mp::local_info(f.lobby);
    oa::ui::gui_input::ScrollHold hold;
    const auto sent = f.sent_of(oa::netgame::RecordType::player_info);
    const auto stepped = mp::slider_press(f.panel, hold, forward, 625, 30, clock_ticks);
    expect(stepped == index, "the forward arrow steps METAL");
    if (stepped == index)
        metal->on_change(f.panel, &f.lobby);
    (void)mp::slider_release(f.panel, hold, 625, 30);
    expect(
        metal->scroll.knob == 10 && mp::panel_text(f.panel, "METALTEXT") == "1100" &&
            info.metal_hundreds == 11,
        "one step forward gives 1100 metal"
    );
    expect(f.sent_of(oa::netgame::RecordType::player_info) == sent + 1, "one player info is sent");

    // Every knob position stands for a value a 3.1c host can send.
    auto sample = *metal;
    for (int16_t knob = 0; knob < sample.scroll.range; ++knob) {
        sample.scroll.knob = knob;
        expect(
            oa::ui::gui_input::scroll_value(sample.scroll) / 100 != 7,
            "no knob position gives 700 metal"
        );
    }
}

void test_ready_and_start(const oa::ui::gui_layout::Layout& lounge) {
    Fixture f(lounge);
    expect(!mp::lobby_ready_to_start(f.lobby), "no remote players: not ready");
    f.join(0x200, "Guest");
    f.remote_info(0x200, 0, 1);
    mp::lobby_player_count(*f.game) = 2;
    expect(f.press("READY0"), "READY0 clickable");
    expect((mp::local_info(f.lobby).options & mp::option::ready) != 0, "local ready bit set");
    expect(!mp::lobby_ready_to_start(f.lobby), "remote not ready yet");
    f.remote_info(0x200, mp::option::ready, 1);
    expect(mp::lobby_ready_to_start(f.lobby), "all ready");
    f.game->gui_flags |= 1;
    (void)mp::lobby_tick(f.lobby, f.panel);
    expect(!mp::panel_control(f.panel, "START")->active, "START hidden until units are synced");
    f.synced(0x200);
    f.game->gui_flags |= 1;
    (void)mp::lobby_tick(f.lobby, f.panel);
    expect(!mp::panel_control(f.panel, "START")->grayed, "START enabled once everyone is ready");
    last_message.clear();
    mp::lobby_player_team(mp::local_player(f.lobby)) = 2;
    mp::lobby_player_team(mp::slot_player(f.lobby, 1)) = 2;
    expect(mp::panel_press(f.panel, "START"), "START pressable");
    expect(
        mp::lobby_handle_event(f.lobby, f.panel) == mp::LobbyAction::none, "same team blocks start"
    );
    expect(
        last_message == "Can not start game with all players on the same team.", "same team message"
    );
    mp::lobby_player_team(mp::slot_player(f.lobby, 1)) = mp::kNoTeam;
    (void)mp::panel_press(f.panel, "START");
    expect(mp::lobby_handle_event(f.lobby, f.panel) == mp::LobbyAction::start, "START accepted");
    expect((mp::local_info(f.lobby).options & mp::option::started) != 0, "started flag set");
    expect(f.game->frontend_pending_signal == 0x11, "start signal raised");

    // Eight players, none of them with a game disc: START still starts the game.
    Fixture g(lounge);
    for (int i = 0; i < 7; ++i)
        g.join(0x300 + static_cast<uint32_t>(i), "P");
    for (int i = 0; i < 7; ++i)
        g.remote_info(
            0x300 + static_cast<uint32_t>(i), mp::option::ready, static_cast<uint8_t>(i + 1)
        );
    for (int slot = 0; slot < 8; ++slot)
        mp::slot_info(g.lobby, slot)->status = 0;
    mp::lobby_player_team(mp::local_player(g.lobby)) = 1;
    mp::lobby_player_team(mp::slot_player(g.lobby, 1)) = 2;
    mp::lobby_player_count(*g.game) = 8;
    for (int i = 0; i < 7; ++i)
        g.synced(0x300 + static_cast<uint32_t>(i));
    (void)g.press("READY0");
    g.game->gui_flags |= 1;
    (void)mp::lobby_tick(g.lobby, g.panel);
    for (int slot = 0; slot < 8; ++slot)
        mp::slot_info(g.lobby, slot)->status = 0;
    last_message.clear();
    (void)mp::panel_press(g.panel, "START");
    expect(
        mp::lobby_handle_event(g.lobby, g.panel) == mp::LobbyAction::start,
        "eight players with no discs start"
    );
    expect(
        last_message != "There are not enough game CDs present to play",
        "START does not ask for game discs"
    );
    expect((mp::local_info(g.lobby).options & mp::option::started) != 0, "started with no discs");
}

/// Builds a battle room layout of its own: one row of READY, LOGO and CD
/// templates, a picture surface that takes no clicks, and START, SYNCHING and
/// the closed doors over them in LOUNGE2.GUI's rectangles and index order.
///
/// @return the layout
oa::ui::gui_layout::Layout doors_layout() {
    namespace gui = oa::ui::gui_layout;
    const auto gadget = [](gui::GadgetType type,
                           const char* name,
                           int16_t x,
                           int16_t y,
                           int16_t width,
                           int16_t height,
                           int32_t attributes,
                           bool active) {
        gui::Gadget made;
        made.common.type = type;
        made.common.name = name;
        made.common.x = x;
        made.common.y = y;
        made.common.width = width;
        made.common.height = height;
        made.common.attributes = attributes;
        made.common.active = active ? 1 : 0;
        return made;
    };
    const auto button = [&](const char* name,
                            int16_t x,
                            int16_t y,
                            int16_t width,
                            int16_t height,
                            int32_t attributes,
                            const char* text,
                            bool grayed) {
        auto made = gadget(gui::GadgetType::button, name, x, y, width, height, attributes, true);
        gui::ButtonFields fields;
        fields.text = text;
        fields.grayed_out = grayed;
        made.fields = fields;
        return made;
    };
    const auto surface = [&](const char* name,
                             int16_t x,
                             int16_t y,
                             int16_t width,
                             int16_t height,
                             bool active,
                             bool hot) {
        auto made = gadget(gui::GadgetType::hot_surface, name, x, y, width, height, 0, active);
        made.fields = gui::HotSurfaceFields{hot};
        return made;
    };
    gui::Layout layout;
    layout.gadgets.push_back(
        gadget(gui::GadgetType::panel, "lounge2.GUI", 0, 0, 640, 480, 0, true)
    );
    layout.gadgets.push_back(button("READYx", 479, 71, 16, 16, 0x8a, "", false));
    layout.gadgets.push_back(button("START", 522, 437, 96, 31, 0x10002, "Start Game", false));
    layout.gadgets.push_back(surface("LOGOx", 179, 71, 16, 16, true, true));
    layout.gadgets.push_back(surface("PICTURE", 300, 150, 100, 100, true, false));
    layout.gadgets.push_back(button("SYNCHING", 522, 437, 96, 31, 18, "Synching", true));
    layout.gadgets.push_back(surface("battlestart", 508, 437, 125, 30, false, false));
    layout.gadgets.push_back(gadget(gui::GadgetType::image, "CDx", 105, 71, 16, 16, 0, false));
    return layout;
}

/// Returns the control under a control's centre.
///
/// @param panel panel searched
/// @param name the control whose centre is taken
/// @return the index panel_hit finds there
int32_t hit_centre(const mp::Panel& panel, const char* name) {
    const auto* control = mp::panel_control(panel, name);
    if (control == nullptr)
        return mp::kNoControl;
    return mp::panel_hit(panel, control->x + control->width / 2, control->y + control->height / 2);
}

/// Checks that only hot surfaces take clicks.
void test_hot_surfaces() {
    mp::Panel panel;
    mp::panel_load(panel, "doors", doors_layout());
    expect(mp::panel_control(panel, "LOGOx")->hot, "a hot surface loads hot");
    expect(!mp::panel_control(panel, "PICTURE")->hot, "a surface with hotornot=0 loads not hot");
    expect(
        hit_centre(panel, "PICTURE") == mp::kNoControl, "a surface that is not hot is never hit"
    );
    expect(!mp::panel_press(panel, "PICTURE"), "a surface that is not hot cannot be pressed");
    expect(
        hit_centre(panel, "LOGOx") == mp::panel_find(panel, "LOGOx"), "a hot surface takes clicks"
    );
    expect(mp::panel_press(panel, "LOGOx"), "a hot surface can be pressed");
}

/// Checks START behind its doors.
///
/// The closed doors take START's clicks until everyone is first ready; then
/// SYNCHING stands in for START until the units are synced.
void test_start_doors(const oa::ui::gui_layout::Layout& lounge) {
    Fixture f(lounge);
    auto& panel = f.panel;
    const auto start = mp::panel_find(panel, "START");
    const auto synching = mp::panel_find(panel, "SYNCHING");
    const auto doors_index = mp::panel_find(panel, "battlestart");
    expect(
        start != mp::kNoControl && start < synching && synching < doors_index,
        "START, SYNCHING and the doors keep LOUNGE2's order"
    );
    auto* doors = mp::panel_control(panel, "battlestart");
    auto* start_button = mp::panel_control(panel, "START");
    const auto start_attributes = start_button->attributes;
    expect(
        doors->active != 0 && doors->hot && doors->stage == 0,
        "the battle room opens with its doors closed and taking clicks"
    );
    expect(hit_centre(panel, "START") == doors_index, "the closed doors take clicks over START");
    sounds.clear();
    expect(f.press("battlestart"), "the closed doors are pressed");
    expect(sounds.empty(), "a press on the closed doors does nothing");
    expect(
        hit_centre(panel, "LOGO0") == mp::panel_find(panel, "LOGO0") &&
            mp::panel_control(panel, "LOGO0")->hot,
        "the colour square takes clicks while the host is not ready"
    );

    f.join(0x200, "Guest");
    f.remote_info(0x200, mp::option::ready, 1);
    mp::lobby_player_count(*f.game) = 2;
    expect(f.press("READY0"), "READY0 clickable");
    mp::lobby_update_status(f.lobby, f.panel);
    const auto color = mp::local_info(f.lobby).color;
    expect(!f.press("LOGO0"), "the colour square ignores clicks while the host is ready");
    expect(
        hit_centre(panel, "LOGO0") != mp::panel_find(panel, "LOGO0"),
        "the colour square is not hit while the host is ready"
    );
    expect(mp::local_info(f.lobby).color == color, "a ready host keeps its colour");
    sounds.clear();
    f.game->gui_flags |= 1;
    (void)mp::lobby_tick(f.lobby, f.panel);
    expect(mp::lobby_ready_to_start(f.lobby), "everyone is ready");
    expect(!doors->hot && doors->stage == 1, "the doors stop taking clicks and start to open");
    expect(sounds.size() == 1 && sounds[0] == "Options", "the doors open with the Options sound");
    expect(
        start_button->active == 0 && mp::panel_control(panel, "SYNCHING")->active != 0,
        "SYNCHING stands in for START until the units are synced"
    );
    expect(hit_centre(panel, "START") == synching, "SYNCHING takes the click over START");
    expect(!mp::panel_press(panel, synching), "the grayed SYNCHING button ignores it");

    f.synced(0x200);
    f.game->gui_flags |= 1;
    (void)mp::lobby_tick(f.lobby, f.panel);
    expect(
        start_button->active != 0 && !start_button->grayed && hit_centre(panel, "START") == start,
        "START takes clicks once everyone is ready and the units are synced"
    );
    expect(start_button->attributes == start_attributes, "START keeps its attributes");
    expect(
        start_button->light_level == (clock_ticks & 0x1fU),
        "START is lit at the tick's low five bits"
    );

    sounds.clear();
    for (int step = 0; step < 12; ++step) {
        clock_ticks += 4;
        (void)mp::lobby_tick(f.lobby, f.panel);
    }
    expect(doors->stage == 8, "the doors open to their last frame");
    expect(sounds.size() == 1 && sounds[0] == "Panel", "the half-open doors play the Panel sound");
    (void)f.press("READY0");
    f.game->gui_flags |= 1;
    (void)mp::lobby_tick(f.lobby, f.panel);
    expect(start_button->grayed, "START grays again when a player is no longer ready");
    expect(
        !doors->hot && doors->stage == 8 && hit_centre(panel, "START") == start,
        "the doors stay open and out of the way"
    );
}

/// Checks each seated row's Go? light, colour square and CD icon.
void test_row_indicators(const oa::ui::gui_layout::Layout& lounge) {
    Fixture f(lounge);
    f.join(0x200, "Guest");
    f.remote_info(0x200, 0, 1);
    const auto* ready0 = mp::panel_control(f.panel, "READY0");
    const auto* ready1 = mp::panel_control(f.panel, "READY1");
    expect(
        ready0 != nullptr && ready0->active != 0 && ready0->value == 0 && !ready0->grayed,
        "the host's Go? light is dark and clickable"
    );
    expect(
        ready1 != nullptr && ready1->active != 0 && ready1->value == 0 && ready1->grayed,
        "a remote Go? light is grayed"
    );
    const auto* logo0 = mp::panel_control(f.panel, "LOGO0");
    const auto* logo1 = mp::panel_control(f.panel, "LOGO1");
    expect(
        logo0 != nullptr && logo0->active != 0 && logo0->stage == mp::local_info(f.lobby).color &&
            logo0->hot,
        "the host's colour square shows its colour"
    );
    expect(logo1 != nullptr && logo1->active != 0 && logo1->stage == 1, "a remote colour square");
    expect(mp::panel_control(f.panel, "CD0")->active == 0, "the host shows no CD icon");
    const auto host_badge = mp::lobby_row_badge(f.lobby, 0);
    expect(
        host_badge.badge == mp::RowBadge::open_annihilation && !host_badge.rules_differ,
        "the host's own row has the OA badge and no dot"
    );
    expect(
        mp::panel_control(f.panel, "CD1")->active != 0, "a remote 3.1c player with a CD shows it"
    );
    expect(
        mp::lobby_row_badge(f.lobby, 1).badge == mp::RowBadge::disc,
        "a remote 3.1c player with a CD has the CD icon for its badge"
    );
    expect(mp::panel_control(f.panel, "CD2")->active == 0, "an open slot shows no CD");
    expect(
        mp::lobby_row_badge(f.lobby, 2).badge == mp::RowBadge::none, "an open slot has no badge"
    );
    auto* remote_block = reinterpret_cast<uint8_t*>(mp::slot_info(f.lobby, 1));
    remote_block[oa::netgame::player_info_engine_signature_offset] =
        oa::netgame::engine_signature_first;
    remote_block[oa::netgame::player_info_engine_signature_offset + 1] =
        oa::netgame::engine_signature_second;
    mp::lobby_update_status(f.lobby, f.panel);
    expect(mp::panel_control(f.panel, "CD1")->active == 0, "a remote OA player shows no CD icon");
    expect(
        mp::lobby_row_badge(f.lobby, 1).badge == mp::RowBadge::open_annihilation,
        "a remote OA player has the OA badge"
    );
    remote_block[oa::netgame::player_info_engine_signature_offset] = 0;
    remote_block[oa::netgame::player_info_engine_signature_offset + 1] = 0;
    expect(
        (mp::local_info(f.lobby).status & mp::status::has_disc) != 0,
        "the local player reports a disc, as a 3.1c host counts them"
    );
    expect(
        oa::netgame::sent_by_open_annihilation(
            reinterpret_cast<const uint8_t*>(&mp::local_info(f.lobby))
        ),
        "the local player's own block carries the engine signature"
    );
    f.remote_info(0x200, mp::option::ready, 1);
    expect(ready1->value == 1 && ready1->grayed, "a ready remote lights its grayed Go? light");
    mp::slot_info(f.lobby, 1)->status = 0;
    mp::lobby_update_status(f.lobby, f.panel);
    expect(
        mp::panel_control(f.panel, "CD1")->active == 0, "a remote player without a CD shows none"
    );
    expect(
        (mp::local_info(f.lobby).status & mp::status::has_disc) != 0,
        "the status refresh keeps the local player's disc"
    );
    expect(f.press("READY0"), "READY0 clickable");
    mp::lobby_update_status(f.lobby, f.panel);
    expect(ready0->value == 1 && !ready0->grayed, "the host's Go? light is lit");
    expect(!logo0->hot && !logo1->hot, "no colour square takes clicks while the host is ready");
}

void test_slot_cycle(const oa::ui::gui_layout::Layout& lounge) {
    Fixture f(lounge);
    f.loopback.next_player_id = 0x101; // fixture manually seated the host at 0x100
    auto& slot = mp::slot_player(f.lobby, 2);
    expect(f.press("PLAYER2"), "PLAYER2 clickable");
    expect(slot.status == mp::kSlotBlocked, "host click blocks an open slot");
    mp::lobby_update_status(f.lobby, f.panel);
    expect(mp::panel_text(f.panel, "PLAYER2") == "[BLOCKED]", "blocked slot label");
    // In another language the labels are translated: BLOCKED inside its
    // brackets.
    f.lobby.services.translate = [](void*, const char* text) -> const char* {
        const std::string_view english(text);
        return english == "BLOCKED" ? "Gesperrt" : english == "UNUSED" ? "Unbenutzt" : nullptr;
    };
    mp::lobby_update_status(f.lobby, f.panel);
    expect(
        mp::panel_text(f.panel, "PLAYER2") == "[Gesperrt]" &&
            mp::panel_text(f.panel, "PLAYER3") == "Unbenutzt",
        "blocked and open slots' labels translated"
    );
    f.lobby.services.translate = nullptr;
    mp::lobby_update_status(f.lobby, f.panel);
    expect(f.press("PLAYER2"), "PLAYER2 clickable when blocked");
    // The computer stays pending until the session announces its arrival.
    expect(
        slot.status == mp::kSlotComputer && slot.in_use == 0,
        "computer waits for its session arrival"
    );
    const auto count_before = f.game->player_count;
    mp::LobbyEvent arrival{};
    expect(
        f.lobby.net.receive(f.lobby.net.context, &arrival) &&
            mp::lobby_apply_event(f.lobby, arrival),
        "computer session arrival"
    );
    expect(f.game->player_count == count_before + 1, "computer arrival counts once");
    expect(
        mp::slot_info(f.lobby, 2)->state == mp::kSlotComputer,
        "computer arrival keeps its controller"
    );
    expect(
        slot.status == mp::kSlotComputer && slot.in_use != 0,
        "blocked slot becomes a computer player"
    );
    expect(mp::slot_info(f.lobby, 2)->color == 1, "computer takes the next colour");
    expect(f.press("PLAYER3"), "PLAYER3 clickable");
    expect(f.press("PLAYER3"), "PLAYER3 clickable again");
    expect(
        mp::slot_player(f.lobby, 3).status == mp::kSlotComputer &&
            mp::slot_player(f.lobby, 3).in_use == 0,
        "a second computer player waits for its session arrival"
    );
    clock_ticks += 0x1f;
    expect(f.press("PLAYER2"), "PLAYER2 clickable as computer");
    expect(slot.status == mp::kSlotOpen && slot.in_use == 0, "old computer slot is rejected");
    expect(f.sent_of(oa::netgame::RecordType::reject) >= 1, "reject record sent");
    auto& info = mp::local_info(f.lobby);
    info.options |= mp::option::game_closed;
    last_message.clear();
    (void)f.press("PLAYER4");
    (void)f.press("PLAYER4");
    expect(
        last_message == "Can't add another player when game is closed.",
        "closed game refuses players"
    );
}

// The battle room buttons a mod's display rules add: the host's AUTOPAUSE
// and AUTOTEAM say their commands as typed lines; a client's are grayed;
// without the rules the gadgets do nothing.
void test_mod_lobby_buttons(const oa::ui::gui_layout::Layout& lounge) {
    auto layout = lounge;
    const auto base = std::find_if(layout.gadgets.begin(), layout.gadgets.end(), [](const auto& g) {
        return g.common.name == "PREVMENU";
    });
    expect(base != layout.gadgets.end(), "LOUNGE2.GUI has PREVMENU");
    if (base == layout.gadgets.end())
        return;
    const auto prototype = *base;
    int16_t x = 325;
    for (const char* name : {"AUTOTEAM", "AUTOPAUSE"}) {
        auto button = prototype;
        button.common.name = name;
        button.common.x = x;
        button.common.y = 450;
        layout.gadgets.push_back(button);
        x = static_cast<int16_t>(x + 81);
    }
    const uint8_t buttons = mp::lobby_button::autoteam | mp::lobby_button::autopause;
    {
        Fixture f(layout, nullptr, true, buttons);
        expect(f.press("AUTOPAUSE"), "the host's AUTOPAUSE takes a press");
        expect(
            std::strcmp(mp::lobby_chat_line(*f.game, 0), "<Host> .autopause") == 0,
            "AUTOPAUSE says .autopause"
        );
        expect(f.press("AUTOTEAM"), "the host's AUTOTEAM takes a press");
        expect(
            std::strcmp(mp::lobby_chat_line(*f.game, 1), "<Host> +autoteam") == 0,
            "AUTOTEAM says +autoteam"
        );
        expect(f.sent_of(oa::netgame::RecordType::chat) == 2, "both lines go to the others");
    }
    {
        Fixture f(layout, nullptr, false, buttons);
        const auto* pause = mp::panel_control(f.panel, "AUTOPAUSE");
        expect(pause != nullptr && pause->grayed, "a client's AUTOPAUSE is grayed");
        (void)f.press("AUTOPAUSE");
        expect(f.sent_of(oa::netgame::RecordType::chat) == 0, "a client's press says nothing");
    }
    {
        Fixture f(layout, nullptr, true, 0);
        (void)f.press("AUTOPAUSE");
        expect(f.sent_of(oa::netgame::RecordType::chat) == 0, "without the rules nothing is said");
    }
}

void test_chat_and_leave(const oa::ui::gui_layout::Layout& lounge) {
    Fixture f(lounge);
    f.panel.focus = mp::panel_find(f.panel, "MESSAGE");
    mp::panel_set_text(f.panel, "MESSAGE", "hello");
    (void)f.press("MESSAGE");
    expect(
        std::strcmp(mp::lobby_chat_line(*f.game, 0), "<Host> hello") == 0, "chat line formatted"
    );
    expect(mp::panel_text(f.panel, "MESSAGE").empty(), "message box cleared");
    expect(f.sent_of(oa::netgame::RecordType::chat) == 1, "chat record sent");
    mp::LobbyEvent chat{};
    chat.kind = mp::LobbyEventKind::record;
    oa::netgame::ChatRecord record{};
    std::snprintf(record.text, sizeof(record.text), "%s", "<Guest> hi");
    std::size_t written = 0;
    (void)oa::netgame::encode_record(record, chat.data, sizeof(chat.data), &written);
    chat.size = static_cast<uint16_t>(written);
    (void)mp::lobby_apply_event(f.lobby, chat);
    mp::lobby_update_status(f.lobby, f.panel);
    const auto* output = mp::panel_control(f.panel, "OUTPUT");
    expect(
        output != nullptr && output->items.size() == 2 && output->items[1] == "<Guest> hi",
        "OUTPUT lists chat"
    );
    (void)mp::panel_press(f.panel, "PREVMENU");
    expect(mp::lobby_handle_event(f.lobby, f.panel) == mp::LobbyAction::leave, "PREVMENU leaves");
}

/// Checks that a host browsing SELMAP's maps behind the battle room tells the others nothing.
///
/// A highlighted map is selected on the host's machine, so its map no
/// longer matches the one it announced; the battle room's status, which
/// would report the missing map, waits until the battle room is in front.
void test_browsing_behind_a_dialog(const oa::ui::gui_layout::Layout& lounge) {
    Fixture f(lounge);
    const auto committed = map_name;
    f.lobby.maps.content_hash = [](void*) {
        return map_name == "Coast To Coast" ? 0x1234U : 0x5678U;
    };
    f.join(0x200, "Guest");
    f.remote_info(0x200, 0, 1);
    f.game->gui_flags |= 1;
    (void)mp::lobby_tick(f.lobby, f.panel);
    auto& mine = mp::local_info(f.lobby);
    mine.options |= mp::option::ready;
    const std::string label(mp::panel_text(f.panel, "MAPNAME"));
    f.lobby.next_stats_tick = clock_ticks + 100;
    const auto sent = f.loopback.sent_count;
    const auto chat_head = mp::lobby_chat_head(*f.game);

    // SELMAP previews another map on the host's machine only.
    (void)select_map(nullptr, "Acid Pools");
    expect(!mp::lobby_has_host_map(f.lobby), "the previewed map is not the one announced");
    for (int frame = 0; frame < 3; ++frame)
        (void)mp::lobby_tick(f.lobby, f.panel, mp::LobbyFront::dialog);
    expect(f.loopback.sent_count == sent, "browsing behind a dialog sends nothing");
    expect(
        mp::lobby_chat_head(*f.game) == chat_head && (mine.options & mp::option::ready) != 0 &&
            mp::panel_text(f.panel, "MAPNAME") == label,
        "the host stays ready and the battle room still names the chosen map"
    );

    // PREVMENU puts the chosen map back before the battle room is in front again.
    (void)select_map(nullptr, committed.c_str());
    f.game->gui_flags |= 1;
    (void)mp::lobby_tick(f.lobby, f.panel);
    expect(
        f.loopback.sent_count == sent && (mine.options & mp::option::ready) != 0 &&
            mp::panel_text(f.panel, "MAPNAME") == label,
        "back in front, the battle room finds nothing to report"
    );

    // With the battle room in front, the same preview is reported as a missing map:
    // on a German machine in English, as English 3.1c sends it, and shown here in German.
    f.lobby.services.translate = [](void*, const char* text) -> const char* {
        return std::string_view(text) == "does not have this map" ? "hat diese Karte nicht"
                                                                  : nullptr;
    };
    (void)select_map(nullptr, "Acid Pools");
    (void)mp::lobby_tick(f.lobby, f.panel);
    expect(
        f.sent_of(oa::netgame::RecordType::chat) > 0 && (mine.options & mp::option::ready) == 0,
        "in front, the battle room reports a map the host lacks and clears its ready mark"
    );
    std::string said;
    for (int32_t index = 0; index < f.loopback.sent_count; ++index) {
        const auto* record = f.loopback.sent[index];
        if (record[0] == static_cast<uint8_t>(oa::netgame::RecordType::chat))
            said.assign(
                reinterpret_cast<const char*>(record + 1),
                ::strnlen(reinterpret_cast<const char*>(record + 1), mp::kLobbyRecordBytes - 1)
            );
    }
    expect(said == "<Host> does not have this map", "the missing map goes in English");
    const auto head = mp::lobby_chat_head(*f.game);
    expect(
        std::string_view(
            mp::lobby_chat_line(*f.game, static_cast<std::size_t>(head + mp::kChatLines - 1))
        ) == "<Host> hat diese Karte nicht",
        "the missing map shows in the language shown"
    );
    f.lobby.services.translate = nullptr;
    map_name = committed;
}

void test_client_rules(const oa::ui::gui_layout::Layout& lounge) {
    auto game = std::make_unique<oa::Game>();
    mp::Lobby lobby{};
    mp::LoopbackNet loopback{};
    mp::loopback_reset(loopback);
    mp::lobby_reset(lobby, *game);
    lobby.net = mp::loopback_lobby_net(loopback);
    lobby.services = test_services();
    lobby.maps = test_maps();
    game->players[0].player_id = 0x200;
    mp::lobby_seat_local(lobby, 0, false, "Guest");
    mp::LobbyEvent host{};
    host.kind = mp::LobbyEventKind::player_joined;
    host.player_id = 0x100;
    std::snprintf(host.name, sizeof(host.name), "%s", "Host");
    (void)mp::lobby_apply_event(lobby, host);
    auto* host_info = mp::slot_info(lobby, 1);
    host_info->role = mp::kRoleHost;
    host_info->max_units = 180;
    host_info->version_major = 3;
    host_info->version_minor = 1;
    host_info->map_hash = 0x9999;
    mp::Panel panel;
    mp::panel_load(panel, "guis/lounge2.gui", lounge);
    mp::panel_bind_sliders(panel, slider_art);
    mp::lobby_enter_battleroom(lobby, panel);
    expect(mp::panel_control(panel, "COMMANDER")->grayed, "client cannot change host options");
    expect(mp::panel_control(panel, "METAL")->grayed, "client cannot move sliders");
    // The locked sliders and their arrows ignore the pointer.
    for (const char* name : {"METAL", "ENERGY", "MAXUNITS"}) {
        const auto index = mp::panel_find(panel, name);
        expect(index != mp::kNoControl, "the lounge has its sliders");
        if (index == mp::kNoControl)
            continue;
        auto& slider = panel.controls[static_cast<std::size_t>(index)];
        const auto knob = slider.scroll.knob;
        const auto& forward = slider.scroll.forward_arrow;
        oa::ui::gui_input::ScrollHold hold;
        expect(
            forward.width != 0 && mp::panel_hit(panel, forward.x + 4, forward.y + 8) == index,
            "a locked slider's arrow is still shown"
        );
        expect(
            mp::slider_press(panel, hold, index, forward.x + 4, forward.y + 8, clock_ticks) ==
                    mp::kNoControl &&
                hold.bar == mp::kNoControl,
            "a locked slider's arrow ignores a press"
        );
        expect(
            mp::slider_press(panel, hold, index, slider.x + 90, slider.y + 8, clock_ticks) ==
                    mp::kNoControl &&
                hold.bar == mp::kNoControl &&
                mp::slider_release(panel, hold, slider.x + 90, slider.y + 8) == mp::kNoControl,
            "a locked slider ignores a press on its track"
        );
        expect(slider.scroll.knob == knob, "a locked slider's knob stays");
    }
    expect(mp::panel_text(panel, "MAP") == "View Map", "client map button views");
    expect(!mp::lobby_has_host_map(lobby), "map hash mismatch detected");
    mp::lobby_on_max_units(panel, &lobby);
    expect(mp::local_info(lobby).max_units == 180, "client adopts host max units");
    mp::lobby_update_status(lobby, panel);
    expect(mp::panel_control(panel, "MAP")->grayed, "client without the host map cannot view it");
    host_info->map_hash = 0x1234;
    mp::lobby_update_status(lobby, panel);
    (void)mp::panel_press(panel, "MAP");
    expect(
        mp::lobby_handle_event(lobby, panel) == mp::LobbyAction::view_map, "client MAP opens view"
    );
    // The client shows the host's metal after its own slider's round trip.
    std::snprintf(host_info->map_name, sizeof(host_info->map_name), "%s", map_name.c_str());
    host_info->metal_hundreds = 8;
    game->gui_flags |= 1;
    (void)mp::lobby_tick(lobby, panel);
    expect(mp::panel_text(panel, "METALTEXT") == "800", "a host's 800 metal shows as 800");
}

template <class Record>
mp::LobbyEvent record_event(uint32_t from, const Record& record) {
    mp::LobbyEvent event{};
    event.kind = mp::LobbyEventKind::record;
    event.player_id = from;
    std::size_t written = 0;
    (void)oa::netgame::encode_record(record, event.data, sizeof(event.data), &written);
    event.size = static_cast<uint16_t>(written);
    return event;
}

oa::netgame::MachineGroupRequestRecord
group_request(uint8_t assign, uint32_t player, uint32_t same_machine) {
    oa::netgame::MachineGroupRequestRecord request{};
    request.assign = assign;
    request.player_id = player;
    request.same_machine_id = same_machine;
    return request;
}

// The index of the last sent record of a type, or -1.
int32_t last_sent(const mp::LoopbackNet& loopback, oa::netgame::RecordType type) {
    for (int32_t index = loopback.sent_count - 1; index >= 0; --index)
        if (loopback.sent[index][0] == static_cast<uint8_t>(type))
            return index;
    return -1;
}

void join_remote(mp::Lobby& lobby, uint32_t id) {
    mp::LobbyEvent event{};
    event.kind = mp::LobbyEventKind::player_joined;
    event.player_id = id;
    (void)mp::lobby_apply_event(lobby, event);
}

/// A 4K monitor's sizes, as the game gives them to the battle room's RES
/// column: the narrower first.
int32_t four_k_modes(void*, mp::DisplayMode* out, int32_t capacity) {
    constexpr mp::DisplayMode modes[] = {
        {640, 480, 8},
        {1024, 768, 8},
        {1920, 1080, 8},
        {2560, 1440, 8},
        {3840, 2160, 8},
    };
    const int32_t count = std::min<int32_t>(capacity, static_cast<int32_t>(std::size(modes)));
    std::copy_n(modes, count, out);
    return count;
}

// The RES column steps through the sizes the game gives, past 1600x1200,
// wrapping at both ends, and each step goes to the other machines as the
// two 16-bit words of the player's setup block a 3.1c machine shows.
void test_resolution_cycle() {
    constexpr uint32_t kHost = 0x100;
    auto game = std::make_unique<oa::Game>();
    mp::Lobby lobby{};
    mp::LoopbackNet loopback{};
    mp::loopback_reset(loopback);
    mp::lobby_reset(lobby, *game);
    lobby.net = mp::loopback_lobby_net(loopback);
    lobby.services.display_modes = four_k_modes;
    game->session_flags |= 1;
    game->players[0].player_id = kHost;
    mp::lobby_seat_local(lobby, 0, true, "Host");
    auto& info = mp::local_info(lobby);
    info.screen_width = 1920;
    info.screen_height = 1080;
    // The size the setup block carries in the last player info sent.
    const auto sent_size = [&] {
        const auto index = last_sent(loopback, oa::netgame::RecordType::player_info);
        oa::netgame::PlayerInfoRecord record{};
        if (index < 0 ||
            oa::netgame::decode_record(loopback.sent[index], loopback.sent_size[index], &record) !=
                oa::netgame::WireError::ok)
            return std::pair<uint16_t, uint16_t>{};
        // The block's bytes as far as the record's head carries them.
        std::array<uint8_t, sizeof(mp::PlayerSetupInfo)> bytes{};
        std::copy(std::begin(record.info_head), std::end(record.info_head), bytes.begin());
        const auto block = std::bit_cast<mp::PlayerSetupInfo>(bytes);
        // Copied out of the packed block, whose words may lie unaligned.
        const uint16_t width = block.screen_width;
        const uint16_t height = block.screen_height;
        return std::pair<uint16_t, uint16_t>{width, height};
    };
    mp::lobby_cycle_resolution(lobby, false);
    expect(info.screen_width == 2560 && info.screen_height == 1440, "RES steps to 2560x1440");
    mp::lobby_cycle_resolution(lobby, false);
    expect(info.screen_width == 3840 && info.screen_height == 2160, "RES steps to 3840x2160");
    expect(
        static_cast<int32_t>(mp::lobby_screen_width(*game)) == 3840 &&
            static_cast<int32_t>(mp::lobby_screen_height(*game)) == 2160,
        "the battle room's game keeps 3840x2160"
    );
    expect(
        sent_size() == std::pair<uint16_t, uint16_t>{3840, 2160},
        "the other machines are sent 3840x2160 in the setup block's words"
    );
    mp::lobby_cycle_resolution(lobby, false);
    expect(info.screen_width == 640 && info.screen_height == 480, "RES wraps to the smallest");
    mp::lobby_cycle_resolution(lobby, true);
    expect(
        info.screen_width == 3840 && info.screen_height == 2160, "RES wraps back to the largest"
    );
    // A size the list does not hold stays, as in 3.1c.
    info.screen_width = 1366;
    info.screen_height = 768;
    mp::lobby_cycle_resolution(lobby, false);
    expect(info.screen_width == 1366 && info.screen_height == 768, "an unlisted size stays");
}

int alliance_notices = 0;

// Damaged lobby records from a seated player: each one is applied or
// refused whole, and a record cut short never reads past its size. The
// seeds are one record of each type the battle room reads.
void test_damaged_records() {
    constexpr uint32_t kHost = 0x100, kGuest = 0x200;
    auto game = std::make_unique<oa::Game>();
    mp::Lobby lobby{};
    mp::LoopbackNet loopback{};
    mp::loopback_reset(loopback);
    mp::lobby_reset(lobby, *game);
    lobby.net = mp::loopback_lobby_net(loopback);
    game->session_flags |= 1;
    game->players[0].player_id = kHost;
    mp::lobby_seat_local(lobby, 0, true, "Host");
    join_remote(lobby, kGuest);
    mp::unit_sync_create(lobby, true);
    std::vector<mp::LobbyEvent> seeds;
    oa::netgame::ChatRecord chat{};
    std::memcpy(chat.text, "<Guest> hi", 10);
    seeds.push_back(record_event(kGuest, chat));
    oa::netgame::PlayerInfoRecord info{};
    info.player_id = kGuest;
    seeds.push_back(record_event(kGuest, info));
    seeds.push_back(record_event(kGuest, group_request(1, kGuest, kHost)));
    oa::netgame::SlotTableRecord slots{};
    seeds.push_back(record_event(kGuest, slots));
    oa::netgame::MachineGroupReplyRecord group{};
    group.player_id = kGuest;
    seeds.push_back(record_event(kGuest, group));
    oa::netgame::PlayerTeamRecord team{};
    team.player_id = kGuest;
    seeds.push_back(record_event(kGuest, team));
    oa::netgame::AllianceRecord alliance{};
    alliance.player_id_a = kGuest;
    alliance.player_id_b = kHost;
    seeds.push_back(record_event(kGuest, alliance));
    oa::netgame::PingRecord ping{};
    ping.origin_player_id = kHost;
    ping.echo_tick_count = 5;
    seeds.push_back(record_event(kGuest, ping));
    oa::netgame::PlayerValueRequestRecord colour{};
    colour.value = 3;
    seeds.push_back(record_event(kGuest, colour));
    oa::netgame::PlayerValueReplyRecord reply{};
    seeds.push_back(record_event(kGuest, reply));
    oa::netgame::UnitDefHandshakeRecord handshake{};
    handshake.subtype = 2;
    seeds.push_back(record_event(kGuest, handshake));
    uint32_t state = 0x10bb1e5;
    const auto next = [&state] {
        state = state * 1664525u + 1013904223u;
        return state >> 8;
    };
    for (const auto& seed : seeds) {
        for (int i = 0; i < 200; ++i) {
            auto event = seed;
            const auto changes = 1 + next() % 3;
            for (uint32_t c = 0; c < changes && event.size != 0; ++c)
                event.data[next() % event.size] = static_cast<uint8_t>(next());
            if (next() % 3 == 0)
                event.size = static_cast<uint16_t>(next() % (event.size + 1u));
            // Bytes past the record's size must never be read.
            std::memset(event.data + event.size, 0xcd, sizeof(event.data) - event.size);
            (void)mp::lobby_apply_event(lobby, event);
            mp::loopback_reset(loopback);
        }
    }
    expect(mp::slot_for_player_id(lobby, kHost) == 0, "the host keeps its seat");
}

// Alliances over a computer, a remote and a received 0x23: a
// computer target mirrors both tables; a remote target is told, with the
// both-sides word set; a received record with that word set fills the
// local target's two entries, and the sender's own entry is kept as sent.
void test_alliance_relation() {
    using oa::netgame::RecordType;
    constexpr uint32_t kHost = 0x100, kGuest = 0x200;
    auto game = std::make_unique<oa::Game>();
    mp::Lobby lobby{};
    mp::LoopbackNet loopback{};
    mp::loopback_reset(loopback);
    mp::lobby_reset(lobby, *game);
    lobby.net = mp::loopback_lobby_net(loopback);
    lobby.services.notify = [](void*, int32_t event) {
        if (event == 4)
            ++alliance_notices;
    };
    game->session_flags |= 1;
    game->players[0].player_id = kHost;
    mp::lobby_seat_local(lobby, 0, true, "Host");
    join_remote(lobby, kGuest);
    auto& me = mp::local_player(lobby);
    const auto guest_slot = mp::slot_for_player_id(lobby, kGuest);
    expect(guest_slot > 0, "guest seated");
    if (guest_slot <= 0)
        return;
    auto& guest = mp::slot_player(lobby, guest_slot);
    auto& computer = game->players[5];
    computer.index = 5;
    computer.in_use = 1;
    computer.status = mp::kSlotComputer;

    alliance_notices = 0;
    mp::lobby_set_alliance(lobby, me, computer, 1, false);
    expect(
        me.alliance[5] == 1 && mp::lobby_player_allied_back(me)[5] == 1 &&
            mp::lobby_player_allied_back(computer)[0] == 1 && computer.alliance[0] == 1,
        "a computer target mirrors both tables"
    );
    expect(alliance_notices == 1, "each change is reported as event 4");

    mp::lobby_set_alliance(lobby, me, guest, 1, true);
    const auto index = last_sent(loopback, RecordType::alliance);
    oa::netgame::AllianceRecord sent{};
    expect(
        index >= 0 &&
            oa::netgame::decode_record(loopback.sent[index], loopback.sent_size[index], &sent) ==
                oa::netgame::WireError::ok &&
            sent.player_id_a == kHost && sent.player_id_b == kGuest && sent.value == 1 &&
            sent.both_sides == 1,
        "a remote target is told, both sides included"
    );
    expect(mp::lobby_player_allied_back(me)[guest_slot] == 1, "both sides set the mirror entry");

    me.alliance[guest_slot] = 0;
    mp::lobby_player_allied_back(me)[guest_slot] = 0;
    guest.alliance[0] = 0;
    oa::netgame::AllianceRecord received{};
    received.player_id_a = kGuest;
    received.player_id_b = kHost;
    received.value = 1;
    received.both_sides = 1;
    expect(mp::lobby_apply_event(lobby, record_event(kGuest, received)), "0x23 applies");
    expect(
        mp::lobby_player_allied_back(me)[guest_slot] == 1 && me.alliance[guest_slot] == 1 &&
            guest.alliance[0] == 1,
        "a received both-sides alliance fills both local entries and the sender's"
    );
}

// Machine groups (requests 0x21, replies 0x22 and the broadcast fan-out):
// the host settles every machine's group and
// answers each request with a broadcast 0x22; once a group seats two
// players every broadcast goes once to each other machine.
void test_machine_groups() {
    using oa::netgame::RecordType;
    constexpr uint32_t kHost = 0x100, kGuest = 0x200, kGuestComputer = 0x201, kThird = 0x300;
    constexpr uint32_t kNone = 0xffffffffU;
    {
        auto game = std::make_unique<oa::Game>();
        mp::Lobby lobby{};
        mp::LoopbackNet loopback{};
        mp::loopback_reset(loopback);
        mp::lobby_reset(lobby, *game);
        lobby.net = mp::loopback_lobby_net(loopback);
        game->session_flags |= 1;
        game->players[0].player_id = kHost;
        mp::lobby_seat_local(lobby, 0, true, "Host");
        for (const auto id : {kGuest, kGuestComputer, kThird})
            join_remote(lobby, id);
        mp::lobby_send_player_info(lobby);
        expect(game->players[0].machine_group == 1, "the host's own slot takes group 1");
        expect(last_sent(loopback, RecordType::machine_group_request) < 0, "the host asks nobody");

        const auto answered = [&](uint32_t player, uint8_t group) {
            const auto index = last_sent(loopback, RecordType::machine_group_reply);
            oa::netgame::MachineGroupReplyRecord reply{};
            return index >= 0 &&
                   oa::netgame::decode_record(
                       loopback.sent[index], loopback.sent_size[index], &reply
                   ) == oa::netgame::WireError::ok &&
                   reply.player_id == player && reply.machine_group == group;
        };
        expect(
            mp::lobby_apply_event(lobby, record_event(kGuest, group_request(1, kGuest, kNone))),
            "a new machine gets a group"
        );
        expect(answered(kGuest, 2), "the first free group after the host's is 2");
        expect(
            loopback.sent_count > 0 && loopback.sent_to[loopback.sent_count - 1] == 0,
            "the answer goes to all players"
        );
        expect(
            mp::lobby_apply_event(
                lobby, record_event(kGuest, group_request(1, kGuestComputer, kGuest))
            ),
            "a computer player joins its human's group"
        );
        expect(answered(kGuestComputer, 2), "the computer shares group 2");
        expect(
            mp::lobby_apply_event(lobby, record_event(kThird, group_request(1, kThird, kNone))),
            "third machine"
        );
        expect(answered(kThird, 3), "the third machine takes group 3");
        expect(mp::free_machine_group(*game) == 4, "group 4 is the next free one");
        expect(game->shared_machines == 0, "sharing is counted at the end of a pump");
        mp::note_shared_machines(*game);
        expect(game->shared_machines == 1, "group 2 seats two players");

        const auto sent_before = loopback.sent_count;
        expect(
            !mp::lobby_apply_event(lobby, record_event(kThird, group_request(1, kThird, kNone))),
            "a player with a group keeps it"
        );
        expect(loopback.sent_count == sent_before, "nothing answers a repeated request");
        expect(
            mp::lobby_apply_event(
                lobby, record_event(kThird, group_request(0, kGuestComputer, kNone))
            ),
            "a query is answered"
        );
        expect(
            loopback.sent_count == sent_before + 2 && loopback.sent_to[sent_before] == kGuest &&
                loopback.sent_to[sent_before + 1] == kThird && answered(kGuestComputer, 2),
            "a broadcast goes once to each other machine"
        );

        // The host's slot table: ids of seated slots, 0 open, -1 closed.
        mp::slot_set_status(lobby, mp::slot_player(lobby, 5), mp::kSlotBlocked);
        mp::lobby_send_slot_table(lobby);
        const uint32_t expected[mp::kSlotCount] = {
            kHost, kGuest, kGuestComputer, kThird, 0, kNone, 0, 0, 0, 0
        };
        expect(std::memcmp(game->slot_table, expected, sizeof(expected)) == 0, "table filled");
        const auto table = last_sent(loopback, RecordType::slot_table);
        oa::netgame::SlotTableRecord record{};
        expect(
            table >= 0 &&
                oa::netgame::decode_record(
                    loopback.sent[table], loopback.sent_size[table], &record
                ) == oa::netgame::WireError::ok &&
                std::memcmp(record.slot_ids, expected, sizeof(expected)) == 0,
            "slot table sent as 0x26"
        );
    }
    {
        auto game = std::make_unique<oa::Game>();
        mp::Lobby lobby{};
        mp::LoopbackNet loopback{};
        mp::loopback_reset(loopback);
        mp::lobby_reset(lobby, *game);
        lobby.net = mp::loopback_lobby_net(loopback);
        game->session_flags |= 1;
        game->players[0].player_id = kGuest;
        mp::lobby_seat_local(lobby, 0, false, "Guest");
        join_remote(lobby, kHost);
        const auto host_slot = mp::slot_for_player_id(lobby, kHost);
        mp::slot_info(lobby, host_slot)->role = mp::kRoleHost;
        auto& computer = mp::slot_player(lobby, 2);
        computer.in_use = 1;
        computer.player_id = kGuestComputer;
        computer.index = 2;
        mp::slot_set_status(lobby, computer, mp::kSlotComputer);
        loopback.sent_count = 0;
        mp::lobby_request_machine_groups(lobby);
        expect(loopback.sent_count == 3, "one request per slot without a group");
        oa::netgame::MachineGroupRequestRecord requests[3]{};
        for (int32_t index = 0; index < 3 && index < loopback.sent_count; ++index) {
            expect(loopback.sent_to[index] == kHost, "requests go to the host");
            (void)oa::netgame::decode_record(
                loopback.sent[index], loopback.sent_size[index], &requests[index]
            );
        }
        expect(
            requests[0].assign == 1 && requests[0].player_id == kGuest &&
                requests[0].same_machine_id == kNone,
            "the local player asks for a new group"
        );
        expect(
            requests[1].assign == 0 && requests[1].player_id == kHost,
            "the host's own group is only asked for"
        );
        expect(
            requests[2].assign == 1 && requests[2].player_id == kGuestComputer &&
                requests[2].same_machine_id == kGuest,
            "the computer asks for the local player's group"
        );
        for (const auto& [player, group] :
             {std::pair{kHost, 1}, std::pair{kGuest, 2}, std::pair{kGuestComputer, 2}}) {
            oa::netgame::MachineGroupReplyRecord reply{};
            reply.player_id = player;
            reply.machine_group = static_cast<uint8_t>(group);
            expect(mp::lobby_apply_event(lobby, record_event(kHost, reply)), "0x22 applies");
        }
        expect(
            computer.machine_group == 2 && mp::local_player(lobby).machine_group == 2,
            "groups stored"
        );
        mp::note_shared_machines(*game);
        const auto sent_before = loopback.sent_count;
        mp::lobby_say(lobby, mp::local_player(lobby), "hi");
        expect(
            loopback.sent_count == sent_before + 1 && loopback.sent_to[sent_before] == kHost,
            "chat goes to the host's machine alone"
        );
        // A line longer than the record's text fills it with no terminator.
        const std::string long_text(80, 'y');
        mp::lobby_say(lobby, mp::local_player(lobby), long_text.c_str());
        const auto said = loopback.sent_count - 1;
        const uint8_t* said_text = said >= 0 ? loopback.sent[said] + 1 : nullptr;
        expect(
            said_text != nullptr && loopback.sent_size[said] == 65 &&
                std::memchr(said_text, 0, 64) == nullptr && said_text[63] == 'y',
            "a long line fills the chat record's text"
        );
        oa::netgame::SlotTableRecord table{};
        table.slot_ids[0] = kHost;
        table.slot_ids[3] = kGuest;
        table.slot_ids[6] = kNone;
        expect(mp::lobby_apply_event(lobby, record_event(kHost, table)), "0x26 applies");
        expect(
            game->slot_table[3] == kGuest && game->slot_table[6] == kNone,
            "client keeps the host's table"
        );
        mp::lobby_send_slot_table(lobby);
        expect(loopback.sent_to[loopback.sent_count - 1] == kHost, "only the host sends its table");
        mp::LobbyEvent left{};
        left.kind = mp::LobbyEventKind::player_left;
        left.player_id = kHost;
        (void)mp::lobby_apply_event(lobby, left);
        expect(
            mp::slot_player(lobby, host_slot).machine_group == 0, "a departed slot drops its group"
        );
    }
}

void test_unit_sync_and_restrictions() {
    auto game = std::make_unique<oa::Game>();
    mp::Lobby lobby{};
    mp::LoopbackNet loopback{};
    mp::loopback_reset(loopback);
    mp::lobby_reset(lobby, *game);
    lobby.net = mp::loopback_lobby_net(loopback);
    game->session_flags |= 1;
    game->players[0].player_id = 0x100;
    mp::lobby_seat_local(lobby, 0, true, "Host");
    mp::LobbyUnit units[] = {
        {"", "", 0, 0, 0, 0, "", 0, 0},
        {"Commander", "ARM", 25000, 2500, mp::kUnitNoRestrict, 11, "ARMCOM", 0, 0},
        {"Peewee", "ARM", 1000, 50, 0, 22, "ARMPW", 0, 0},
        {"AK", "CORE", 900, 45, 0, 33, "CORAK", 0, 0},
        {"Nuke", "ARM", 90000, 1500, mp::kUnitDisabledDefault, 44, "ARMSILO", 0, 0},
    };
    lobby.units = units;
    lobby.unit_count = 5;
    mp::LobbyEvent join{};
    join.kind = mp::LobbyEventKind::player_joined;
    join.player_id = 0x200;
    (void)mp::lobby_apply_event(lobby, join);
    mp::unit_sync_create(lobby, true);
    expect(
        lobby.sync.record_count == 4 && lobby.sync.peer_count == 0, "sync table seeded per unit"
    );
    const auto greeted_at = loopback.sent_count;
    mp::unit_sync_tick(lobby);
    expect(
        lobby.sync.peer_count == 1 && loopback.sent[greeted_at][0] == 0x1a &&
            loopback.sent[greeted_at][1] == 0 && loopback.sent_to[greeted_at] == 0x200,
        "the host greets a playing peer"
    );
    expect(
        lobby.sync.peers[0].sent == 5 && loopback.sent_count == greeted_at + 5 &&
            loopback.sent[greeted_at + 1][1] == 3 && loopback.sent[greeted_at + 1][11] == 0,
        "the changed roster has every record relayed; the peer has reported none"
    );
    mp::unit_sync_tick(lobby);
    expect(
        lobby.sync.peer_count == 1 && loopback.sent_count == greeted_at + 5,
        "a peer is greeted once"
    );
    expect(!mp::unit_sync_complete(lobby), "peer handshake pending");
    char diagnostic[mp::kChatLineBytes];
    expect(
        std::strcmp(
            mp::unit_sync_diagnostic(lobby, diagnostic, sizeof diagnostic),
            "No units_expected sent from player"
        ) == 0,
        "+syncerr names a peer that sent no unit count"
    );
    expect(!mp::unit_sync_peer_complete(lobby, 0x200), "the peer itself is pending");
    expect(mp::unit_sync_peer_complete(lobby, 0x999), "an unseated id is not waited on");
    const auto handshake = [&](uint8_t subtype, uint32_t key, uint32_t value) {
        uint8_t record[14] = {0x1a, subtype};
        std::memcpy(record + 6, &key, 4);
        std::memcpy(record + 10, &value, 4);
        mp::unit_sync_receive(lobby, record, 1);
    };
    handshake(1, 0, 4);
    handshake(2, 22, 0);
    expect(
        std::strcmp(
            mp::unit_sync_diagnostic(lobby, diagnostic, sizeof diagnostic),
            "expected 4 units, got 1"
        ) == 0,
        "+syncerr reports the checksum shortfall"
    );
    handshake(2, 33, 0);
    handshake(2, 33, 0);
    expect(
        lobby.sync.peers[0].sent == 7 && lobby.sync.peers[0].received == 2 &&
            loopback.sent[loopback.sent_count - 1][11] == 1,
        "host answers each new key with a verdict; a repeated key is ignored"
    );
    handshake(2, 11, 0);
    handshake(2, 44, 0);
    expect(
        std::strcmp(
            mp::unit_sync_diagnostic(lobby, diagnostic, sizeof diagnostic), "packets sent=9  ackd=0"
        ) == 0,
        "+syncerr reports unacknowledged verdicts"
    );
    handshake(4, 0, 9);
    expect(mp::unit_sync_complete(lobby), "handshake complete once acknowledged");
    expect(mp::unit_sync_peer_complete(lobby, 0x200), "the peer is complete");
    expect(
        std::strcmp(mp::unit_sync_diagnostic(lobby, diagnostic, sizeof diagnostic), "OK") == 0,
        "+syncerr reports OK"
    );
    // The joiner seats a computer player. Until its info says it is one, the host takes it
    // for a playing peer, greets it and relays every record to it too, and the joiner's
    // machine acknowledges every record it handled, those included.
    mp::LobbyEvent computer{};
    computer.kind = mp::LobbyEventKind::player_joined;
    computer.player_id = 0x201;
    (void)mp::lobby_apply_event(lobby, computer);
    mp::unit_sync_tick(lobby);
    const auto computer_sent = lobby.sync.peer_count == 2 ? lobby.sync.peers[1].sent : 0;
    expect(
        computer_sent == 5, "a computer player taken for a playing peer is greeted and relayed to"
    );
    for (int32_t slot = 0; slot < mp::kSlotCount; ++slot)
        if (mp::slot_player(lobby, slot).player_id == 0x201)
            if (auto* info = mp::slot_info(lobby, slot))
                info->state = mp::kInfoStateDefeated; // its info: a computer player
    mp::unit_sync_tick(lobby);
    expect(lobby.sync.peer_count == 1, "its info drops it from the peers");
    const auto human_sent = lobby.sync.peers[0].sent;
    handshake(4, 0, human_sent + computer_sent);
    expect(
        lobby.sync.peers[0].acknowledged > human_sent && mp::unit_sync_complete(lobby) &&
            std::strcmp(mp::unit_sync_diagnostic(lobby, diagnostic, sizeof diagnostic), "OK") == 0,
        "a peer whose machine acknowledged its computer player's records too is complete"
    );
    lobby.sync.host = false;
    expect(
        mp::unit_sync_diagnostic(lobby, diagnostic, sizeof diagnostic) == nullptr,
        "a client has no diagnostic"
    );
    expect(!mp::unit_sync_peer_complete(lobby, 0x200), "a client reports no peer complete");
    lobby.sync.host = true;
    handshake(120, 0, 0);
    expect(lobby.sync.records_handled == 8, "subtypes >= 100 are dropped");

    mp::RestrictPanel restrict {};
    mp::Panel panel;
    oa::ui::gui_layout::Layout layout;
    const char* names[] = {
        "restrict2.GUI",
        "DESCLIST",
        "SLIDER0",
        "SLIDER1",
        "SLIDER2",
        "COUNT0",
        "COUNT1",
        "COUNT2",
        "OK",
        "Cancel",
        "Reset"
    };
    for (const char* control : names) {
        oa::ui::gui_layout::Gadget gadget;
        gadget.common.name = control;
        gadget.common.type = std::string_view(control).rfind("SLIDER", 0) == 0
                                 ? oa::ui::gui_layout::GadgetType::scroll_bar
                             : std::string_view(control).rfind("COUNT", 0) == 0
                                 ? oa::ui::gui_layout::GadgetType::label
                             : std::string_view(control) == "DESCLIST"
                                 ? oa::ui::gui_layout::GadgetType::list_box
                                 : oa::ui::gui_layout::GadgetType::button;
        gadget.common.active = 1;
        if (gadget.common.type == oa::ui::gui_layout::GadgetType::scroll_bar)
            gadget.fields = oa::ui::gui_layout::ScrollBarFields{102, 16, 0, 8, "", ""};
        layout.gadgets.push_back(gadget);
    }
    {
        oa::ui::gui_layout::Gadget piclist;
        piclist.common.name = "PICLIST";
        piclist.common.type = oa::ui::gui_layout::GadgetType::list_box;
        piclist.common.width = 96;
        layout.gadgets.push_back(piclist);
    }
    mp::panel_load(panel, "guis/restrict2.gui", layout);
    static std::vector<std::string> picture_paths;
    static std::vector<oa::oa_ref32> freed_pictures;
    picture_paths.clear();
    lobby.services.load_picture =
        [](void*, const char* path, int32_t* width, int32_t* height) -> oa::oa_ref32 {
        picture_paths.emplace_back(path);
        if (std::string_view(path).find("ARMPW") == std::string_view::npos)
            return 0;
        *width = 64;
        *height = 48;
        return 7;
    };
    lobby.services.free_picture = [](void*, oa::oa_ref32 picture) {
        freed_pictures.push_back(picture);
    };
    mp::restrict_open(lobby, restrict, panel);
    expect(restrict.count == 3, "norestrict units are left out");
    expect(
        mp::panel_control(panel, "SLIDER0")->notifies_panel &&
            mp::panel_control(panel, "SLIDER2")->notifies_panel,
        "the count sliders' moves reach the panel's handler"
    );
    // The panel loads one row's picture a call over the sorted rows while
    // the unit table lasts: AK, Nuke, Peewee, then the zeroed rows' type 0.
    for (int call = 0; call < 6; ++call)
        mp::restrict_load_next_picture(lobby, restrict, panel);
    expect(
        picture_paths.size() == 5 && picture_paths[0] == "unitpics/CORAK.PCX" &&
            picture_paths[2] == "unitpics/ARMPW.PCX" && picture_paths[3].empty(),
        "one picture a call, by the rows' unit names"
    );
    expect(
        restrict.picture_count == 5 && restrict.pictures[0].image ==
            0 && restrict.pictures[0].width == 96 && restrict.pictures[0].height ==
            mp::kRestrictMissingPictureHeight&& restrict.pictures[2].image ==
            7 && restrict.pictures[2].width == 64 && restrict.pictures[2].load_state ==
            mp::kRestrictPictureLoaded,
        "missing pictures take the list width and 32 rows"
    );
    expect(
        std::strncmp(restrict.entries[0].text, "AK\rCORE 45M  900E", 20) == 0, "rows sorted by text"
    );
    expect(restrict.entries[1].limit == 0, "disabled-by-default unit starts at 0");
    expect(mp::panel_text(panel, "COUNT0") == "No Limit", "unlimited row reads No Limit");
    expect(mp::panel_text(panel, "COUNT1") == "0", "disabled row reads 0");
    auto* slider = mp::panel_control(panel, "SLIDER2");
    oa::ui::gui_input::scroll_set_value(slider->scroll, 5);
    mp::restrict_on_count_slider(lobby, restrict, panel, 2);
    expect(
        restrict.entries[2].limit == 5 && mp::panel_text(panel, "COUNT2") == "5",
        "slider sets a limit"
    );
    mp::UnitSyncRecord record{};
    expect(
        mp::unit_sync_lookup(lobby, 44, &record) && record.limit == 0, "sync table keeps limits"
    );
    (void)mp::panel_press(panel, "Reset");
    (void)mp::restrict_handle_event(lobby, restrict, panel);
    expect(
        restrict.entries[2].limit == 100 && restrict.entries[1].limit == 0,
        "reset restores defaults"
    );
    (void)mp::panel_press(panel, "Cancel");
    expect(
        mp::restrict_handle_event(lobby, restrict, panel) == mp::RestrictAction::close,
        "cancel closes"
    );
    expect(
        mp::unit_sync_lookup(lobby, 22, &record) && record.limit == 0x65,
        "cancel restores saved limits"
    );
    // Closing as host enables every row whose limit is not 0 and disables
    // the rest; the relay queues the key for the panel.
    mp::restrict_close(lobby, restrict);
    expect(
        freed_pictures.size() == 1 && freed_pictures[0] == 7 && restrict.picture_count == 0,
        "closing frees the pictures"
    );
    expect(
        mp::unit_sync_lookup(lobby, 22, &record) && record.local == 1,
        "a limited unit stays enabled"
    );
    expect(
        !mp::unit_sync_lookup(lobby, 44, &record) && record.local == 0,
        "a zero-limit unit is disabled"
    );
    expect(mp::unit_sync_set_enabled(lobby, 44, true), "re-enabled unit both sides have");
    expect(!mp::unit_sync_set_enabled(lobby, 99, true), "an unknown unit is not enabled");
    // In a language whose unit files name a unit, its row shows that name and
    // sorts by it; a unit without one keeps its own.
    oa::data::languages::UnitTexts texts;
    texts.add("CORAK", "Italian", "Kbot AK", nullptr);
    const std::vector<std::string> italian{"Italian"};
    oa::data::languages::set_unit_texts(&texts, italian);
    mp::RestrictPanel localized{};
    mp::restrict_open(lobby, localized, panel);
    expect(
        localized.count == 3 && std::strncmp(localized.entries[0].text, "Kbot AK\rCORE", 12) == 0,
        "a row shows its unit's name in the language and sorts by it"
    );
    expect(
        std::strncmp(localized.entries[1].text, "Nuke\r", 5) == 0,
        "a row without a name in the language keeps its own"
    );
    mp::restrict_close(lobby, localized);
    oa::data::languages::set_unit_texts(nullptr, {});
}

// A client's table seeds its own types with the remote side clear and takes
// the host's verdicts over them; marking the units then keeps a type only on a
// record with both sides set and copies the record's limit.
void test_unit_sync_marks_units() {
    mp::Lobby lobby{};
    mp::LobbyUnit units[] = {
        {"", "", 0, 0, 0, 0, nullptr, 0, 0},
        {"Commander", "ARM", 0, 0, mp::kUnitNoRestrict, 11, nullptr, 0, 0},
        {"Nuke", "ARM", 0, 0, mp::kUnitDisabledDefault, 22, nullptr, 0, 0},
        {"AK", "CORE", 0, 0, 0, 33, nullptr, 0, 0},
        {"Peewee", "ARM", 0, 0, 0, 44, nullptr, 0, 0},
    };
    lobby.units = units;
    lobby.unit_count = 5;
    mp::unit_sync_create(lobby, false);
    const auto verdict = [&](uint32_t key, uint8_t local, uint8_t remote, int16_t limit) {
        uint8_t record[14] = {0x1a, 3};
        std::memcpy(record + 6, &key, 4);
        record[10] = local;
        record[11] = remote;
        std::memcpy(record + 12, &limit, 2);
        mp::unit_sync_receive(lobby, record, 0);
    };
    verdict(11, 1, 1, -1);
    verdict(33, 1, 0, 5);
    verdict(44, 1, 1, 7);
    verdict(55, 1, 1, -1);
    // A record cut short, one of another type and one past its length are dropped.
    {
        const auto handled = lobby.sync.records_handled;
        uint8_t record[15] = {0x1a, 3};
        record[6] = 77;
        mp::unit_sync_receive(lobby, std::span<const uint8_t>(record, 13), 0);
        mp::unit_sync_receive(lobby, std::span<const uint8_t>(record, 2), 0);
        mp::unit_sync_receive(lobby, std::span<const uint8_t>(record, 15), 0);
        mp::unit_sync_receive(lobby, std::span<const uint8_t>{}, 0);
        record[0] = 0x1b;
        mp::unit_sync_receive(lobby, std::span<const uint8_t>(record, 14), 0);
        expect(lobby.sync.records_handled == handled, "malformed unit-check records are dropped");
    }
    const uint32_t keys[] = {0, 11, 22, 33, 44, 66};
    std::vector<oa::UnitDef> records(std::size(keys));
    const auto count = static_cast<uint32_t>(records.size());
    for (std::size_t type = 0; type < records.size(); ++type) {
        records[type].fbi_hash = keys[type];
        records[type].flags = OA_UNIT_DEF_FLAG_AVAILABLE | OA_UNIT_DEF_FLAG_BUILDER;
        records[type].player_limit = -1;
    }
    mp::unit_sync_mark_units(lobby.sync, records.data(), count);
    const auto kept = [&](std::size_t type) {
        return (records[type].flags & OA_UNIT_DEF_FLAG_AVAILABLE) != 0;
    };
    expect(kept(0) && records[0].player_limit == -1, "slot 0 is not synced");
    expect(kept(1) && records[1].player_limit == -1, "a shared type stays unlimited");
    expect(!kept(2) && records[2].player_limit == 0, "own record without a verdict drops");
    expect(!kept(3) && records[3].player_limit == 5, "a verdict with the remote side clear drops");
    expect(kept(4) && records[4].player_limit == 7, "a shared type takes the agreed limit");
    expect(!kept(5) && records[5].player_limit == 0, "a type without a record drops with limit 0");
    expect((records[5].flags & OA_UNIT_DEF_FLAG_BUILDER) != 0, "only the catalog bit changes");

    auto* seeded = &lobby.sync.records[1];
    expect(seeded->key == 22, "record order follows the seeding");
    seeded->remote_high = 1;
    mp::unit_sync_mark_units(lobby.sync, records.data(), count);
    expect(kept(2), "the remote flag is read as a word");
    lobby.sync.finished = true;
    records[1].flags = 0;
    mp::unit_sync_mark_units(lobby.sync, records.data(), count);
    expect(!kept(1), "a finished sync leaves the table alone");
}

void test_game_list(const oa::AssetStore& assets) {
    auto game = std::make_unique<oa::Game>();
    mp::Lobby lobby{};
    mp::LoopbackNet loopback{};
    mp::loopback_reset(loopback);
    mp::lobby_reset(lobby, *game);
    lobby.net = mp::loopback_lobby_net(loopback);
    lobby.services = test_services();
    lobby.local_version_major = 3;
    mp::lobby_seat_local(lobby, 0, false, "");
    mp::ConnectState state{};
    const auto parsed = oa::ui::gui_layout::parse(read_gui(assets, "selgame.gui"));
    expect(parsed.ok(), "SELGAME.GUI parses");
    if (!parsed.ok())
        return;
    mp::Panel panel;
    mp::panel_load(panel, "guis/selgame.gui", *parsed.layout);
    panel.line_height = 14; // hattfont12's 'I' is 12 pixels high
    mp::panel_bind_lists(panel);
    mp::panel_bind_sliders(panel, slider_art);
    last_message.clear();
    expect(!mp::game_list_open(lobby, state, panel), "closed transport cannot list games");
    expect(last_message == "Invalid TCP/IP Address", "invalid address reported");
    loopback.opened = true;
    mp::SessionEntry open{};
    open.instance_guid[0] = 7;
    std::snprintf(open.name, sizeof(open.name), "%-16.16s%-15.15s", "Friday", "Seven Islands");
    open.max_players = 10;
    mp::PlayerSetupInfo host{};
    host.options = 3 | mp::option::watching_allowed | mp::option::commander_step;
    host.metal_hundreds = 10;
    host.energy_hundreds = 20;
    host.version_major = 3;
    std::memcpy(
        open.user, reinterpret_cast<const uint8_t*>(&host) + mp::kSessionUserInfoOffset, 16
    );
    (void)mp::loopback_add_session(loopback, open);
    auto closed = open;
    closed.instance_guid[0] = 8;
    host.options |= mp::option::game_closed;
    std::memcpy(
        closed.user, reinterpret_cast<const uint8_t*>(&host) + mp::kSessionUserInfoOffset, 16
    );
    (void)mp::loopback_add_session(loopback, closed);
    expect(mp::game_list_open(lobby, state, panel), "game list opens");
    const auto* names = mp::panel_control(panel, "GAMENAME");
    expect(
        names != nullptr && names->items.size() == 2 && names->items[0].rfind("Friday", 0) == 0,
        "game names listed"
    );
    expect(
        mp::panel_control(panel, "MAPNAME")->items[0] == "Seven Islands",
        "map name split from session name"
    );
    expect(
        mp::panel_control(panel, "PLAYERS")->items[0] == "3/10", "player count from option nibble"
    );
    expect(mp::panel_control(panel, "STATUS")->items[1] == "Lock", "closed game shows Lock");
    expect(mp::panel_control(panel, "METAL")->items[0] == "1000", "metal column in units");
    expect(mp::panel_control(panel, "COMMANDER")->items[0] == "Yes", "commander column");
    // In another language the list's values are translated, and a map's name
    // is lowered and looked up: one without a translation shows in lower case.
    static bool map_translated = false;
    oa::data::campaign::CampaignFiles german{};
    german.language = "German";
    lobby.services.files = &german;
    lobby.services.translate = [](void*, const char* text) -> const char* {
        const std::string_view english(text);
        if (english == "Lock")
            return "Gesperrt";
        if (english == "Yes")
            return "Ja";
        if (english == "seven islands" && map_translated)
            return "Sieben Inseln";
        return nullptr;
    };
    expect(mp::game_list_update(lobby, state, panel), "game list refreshes in German");
    expect(
        mp::panel_control(panel, "STATUS")->items[1] == "Gesperrt" &&
            mp::panel_control(panel, "COMMANDER")->items[0] == "Ja",
        "game list's values translated"
    );
    expect(
        mp::panel_control(panel, "MAPNAME")->items[0] == "seven islands",
        "untranslated map name lowered"
    );
    map_translated = true;
    expect(mp::game_list_update(lobby, state, panel), "game list refreshes again");
    expect(mp::panel_control(panel, "MAPNAME")->items[0] == "Sieben Inseln", "map name translated");
    lobby.services = test_services();
    expect(mp::game_list_update(lobby, state, panel), "game list refreshes in English");
    expect(!mp::panel_control(panel, "JOINGAME")->grayed, "open game joinable");
    expect(!mp::panel_control(panel, "WATCH")->grayed, "watching allowed");
    // Ten games fit the columns' 160 pixels; the bar shows from the eleventh.
    auto* bar = mp::panel_control(panel, "SLIDER");
    expect(
        names->height == 160 && mp::panel_control(panel, "STATUS")->height == 160 &&
            bar != nullptr && bar->y == 123 && bar->height == 170 && bar->active == 0,
        "two games leave SELGAME's bar hidden"
    );
    for (uint8_t extra = 0; extra < 9; ++extra) {
        auto more = open;
        more.instance_guid[0] = static_cast<uint8_t>(20 + extra);
        (void)mp::loopback_add_session(loopback, more);
    }
    mp::panel_control(panel, "GAMENAME")->list_selection = 1;
    (void)mp::panel_press(panel, "UPDATE");
    expect(
        mp::game_list_handle_event(lobby, state, panel) == mp::ConnectAction::none,
        "UPDATE lists the games again"
    );
    expect(
        names->items.size() == 11 && bar->active != 0 && bar->scroll.knob_size == 151 &&
            bar->scroll.range == 16 && names->list_last_first == 1,
        "eleven games show the bar with a knob of 10/11 of 167 pixels and 16 steps"
    );
    expect(names->list_selection == 0, "UPDATE selects the first game again");
    bar->scroll.knob = 15;
    mp::panel_sync_group(panel, mp::panel_find(panel, "SLIDER"));
    expect(
        names->list_first == 1 && mp::panel_control(panel, "PING")->list_first == 1,
        "the bar scrolls every column"
    );
    mp::panel_control(panel, "GAMENAME")->list_selection = 1;
    mp::game_list_refresh_join(lobby, state, panel);
    expect(mp::panel_control(panel, "JOINGAME")->grayed, "closed game not joinable");
    mp::panel_control(panel, "GAMENAME")->list_selection = 0;
    mp::game_list_refresh_join(lobby, state, panel);
    mp::panel_set_text(panel, "NICKNAME", "");
    (void)mp::panel_press(panel, "JOINGAME");
    last_message.clear();
    expect(
        mp::game_list_handle_event(lobby, state, panel) == mp::ConnectAction::none,
        "join needs a name"
    );
    expect(last_message == "You must enter your name", "missing name reported");
    mp::panel_set_text(panel, "NICKNAME", "Guest");
    (void)mp::panel_press(panel, "JOINGAME");
    expect(
        mp::game_list_handle_event(lobby, state, panel) == mp::ConnectAction::join, "join accepted"
    );
    expect(mp::connect_join(lobby, state, false), "loopback join succeeds");
    expect(
        std::strcmp(mp::local_player(lobby).name, "Guest") == 0, "local player seated with nickname"
    );
    const auto last = loopback.sent_count - 1;
    expect(
        mp::local_info(lobby).color == mp::kNoColor && last >= 2 &&
            loopback.sent[last - 2][0] ==
                static_cast<uint8_t>(oa::netgame::RecordType::player_info) &&
            loopback.sent[last][0] ==
                static_cast<uint8_t>(oa::netgame::RecordType::player_value_request) &&
            loopback.sent[last][1] == 0 && loopback.sent_to[last] == 0,
        "a joiner sends its info, then asks for colour 0 before it knows the host"
    );
    expect(
        mp::provider_kind(mp::kProviderGuidTcpip) == mp::ProviderKind::tcpip, "TCP/IP GUID kind"
    );
    expect(
        mp::provider_kind(mp::kProviderGuidSerial) == mp::ProviderKind::serial, "serial GUID kind"
    );
}

void test_open_service() {
    auto game = std::make_unique<oa::Game>();
    mp::Lobby lobby{};
    mp::LoopbackNet loopback{};
    mp::loopback_reset(loopback);
    mp::lobby_reset(lobby, *game);
    lobby.net = mp::loopback_lobby_net(loopback);
    lobby.services = test_services();
    mp::ConnectState state{};
    state.provider = -1;
    game->frontend_state = 0x15;
    game->frontend_signal = 1;
    game->frontend_pending_signal = 1;
    // An open session sets Game.session_flags bit 0 and returns 1.
    expect(mp::connect_open_service(lobby, state, "127.0.0.1"), "service opens");
    expect(
        (game->session_flags & mp::kNetFlagLive) != 0 && game->frontend_state == 0x15 &&
            game->frontend_signal == 1 && state.error_text[0] == '\0',
        "an open service marks the game live and keeps the frontend state"
    );
    game->session_flags = 0;
    lobby.net.open = +[](void*, const mp::Provider*, const char*) { return false; };
    expect(!mp::connect_open_service(lobby, state, "127.0.0.1"), "failed service returns 0");
    expect(
        std::strcmp(state.error_text, "An error occurred trying to use this service") == 0,
        "the service error text is left in the error buffer"
    );
    expect(
        game->frontend_state == 0x10 && game->frontend_signal == 0 &&
            game->frontend_pending_signal == 0 && (game->session_flags & mp::kNetFlagLive) == 0,
        "a failed service returns to state 0x10 with both signals 0"
    );
    // A non-empty error text is shown once and cleared.
    last_message.clear();
    mp::connect_show_error_text(lobby, state);
    expect(
        last_message == "An error occurred trying to use this service" &&
            state.error_text[0] == '\0',
        "the error text is shown and cleared"
    );
    last_message.clear();
    mp::connect_show_error_text(lobby, state);
    expect(last_message.empty(), "an empty error text shows nothing");
}

void test_map_memory() {
    using oa::ui::frontend_multiplayer::map_memory_mb;
    expect(map_memory_mb(0) == 16 && map_memory_mb(0x3e6665) == 16, "small maps need 16 MB");
    expect(map_memory_mb(0x3e6666) == 24 && map_memory_mb(0x5fffff) == 24, "24 MB band");
    expect(map_memory_mb(0x600000) == 32 && map_memory_mb(0x7fffff) == 32, "32 MB band");
    expect(map_memory_mb(0x800000) == 48 && map_memory_mb(0x9fffff) == 48, "48 MB band");
    expect(map_memory_mb(0xa00000) == 64 && map_memory_mb(0xbfffff) == 64, "64 MB band");
    expect(map_memory_mb(0xc00000) == 128, "large maps need 128 MB");
}

// ---------------------------------------------------------------------------
// Map previews

/// Fills rows top..bottom-1 and columns left..right-1 of a target, short of
/// its last row and column, with texels stepped in 16.16 from (u0, v0) by
/// the span's length toward (u1, v1): the stretch a map preview is drawn with.
///
/// @param[in,out] target the target's palette indices
/// @param target_width target width in pixels
/// @param target_height target height in rows
/// @param texels the texture's palette indices
/// @param texel_width texture width in pixels
/// @param left first column filled
/// @param top first row filled
/// @param right column after the last one filled
/// @param bottom row after the last one filled
/// @param u0 texture column at the left
/// @param v0 texture row at the top
/// @param u1 texture column the right edge steps toward
/// @param v1 texture row the bottom edge steps toward
void stretch_model(
    std::vector<uint8_t>& target,
    int32_t target_width,
    int32_t target_height,
    const std::vector<uint8_t>& texels,
    int32_t texel_width,
    int32_t left,
    int32_t top,
    int32_t right,
    int32_t bottom,
    int32_t u0,
    int32_t v0,
    int32_t u1,
    int32_t v1
) {
    const int32_t columns = right - left;
    const int32_t rows = bottom - top;
    if (columns <= 0 || rows <= 0)
        return;
    const int32_t du = ((u1 - u0) << 16) / columns;
    const int32_t dv = ((v1 - v0) << 16) / rows;
    for (int32_t row = 0; row < rows; ++row) {
        const int32_t y = top + row;
        if (y < 0 || y > target_height - 2)
            continue;
        const int32_t v = (v0 << 16) + row * dv;
        for (int32_t column = 0; column < columns; ++column) {
            const int32_t x = left + column;
            if (x < 0 || x > target_width - 2)
                continue;
            const int32_t u = (u0 << 16) + column * du;
            target[static_cast<std::size_t>(y * target_width + x)] =
                texels[static_cast<std::size_t>((v >> 16) * texel_width + (u >> 16))];
        }
    }
}

/// Returns a minimap fitted to a square box as 3.1c fits it: the map's
/// playable part (32 world pixels short on the right, 128 at the bottom)
/// keeps its shape, its longer side fills the box and it is centred, on
/// palette index 0.
///
/// @param minimap the minimap's palette indices
/// @param width minimap width in pixels
/// @param height minimap height in rows
/// @param box the box's side in pixels
/// @param world_width the map's width in world pixels
/// @param world_height the map's height in world pixels
/// @return box x box palette indices
std::vector<uint8_t> fit_model(
    const std::vector<uint8_t>& minimap,
    int32_t width,
    int32_t height,
    int32_t box,
    int32_t world_width,
    int32_t world_height
) {
    const int32_t playable_width = world_width - 32;
    const int32_t playable_height = world_height - 128;
    int32_t shown_width = box, shown_height = box, used_width = width, used_height = height;
    int32_t left = 0, top = 0;
    if (playable_width >= playable_height) {
        shown_height = playable_height * box / playable_width;
        used_height = height * playable_height / playable_width;
        top = (box - shown_height) / 2;
    } else {
        shown_width = playable_width * box / playable_height;
        used_width = width * playable_width / playable_height;
        left = (box - shown_width) / 2;
    }
    std::vector<uint8_t> fitted(static_cast<std::size_t>(box * box), 0);
    stretch_model(
        fitted,
        box,
        box,
        minimap,
        width,
        left,
        top,
        left + shown_width,
        top + shown_height,
        0,
        0,
        used_width - 1,
        used_height - 1
    );
    return fitted;
}

/// Returns a 252x252 minimap whose texels are never 0 and differ along both axes.
std::vector<uint8_t> test_minimap() {
    std::vector<uint8_t> minimap(252 * 252);
    for (int32_t y = 0; y < 252; ++y)
        for (int32_t x = 0; x < 252; ++x)
            minimap[static_cast<std::size_t>(y * 252 + x)] =
                static_cast<uint8_t>(1 + (x * 7 + y * 3) % 255);
    return minimap;
}

/// Tells whether a box's pixels inside a rectangle are all non-zero and those outside it all 0.
///
/// @param box box x box palette indices
/// @param side the box's side in pixels
/// @param left first column inside
/// @param top first row inside
/// @param right column after the last one inside
/// @param bottom row after the last one inside
/// @return true when the box shows exactly the rectangle
bool covers_only(
    const std::vector<uint8_t>& box,
    int32_t side,
    int32_t left,
    int32_t top,
    int32_t right,
    int32_t bottom
) {
    if (box.size() != static_cast<std::size_t>(side * side))
        return false;
    for (int32_t y = 0; y < side; ++y)
        for (int32_t x = 0; x < side; ++x) {
            const bool inside = x >= left && x < right && y >= top && y < bottom;
            if ((box[static_cast<std::size_t>(y * side + x)] != 0) != inside)
                return false;
        }
    return true;
}

/// Checks the fit of a minimap into a map preview's box.
void test_fit_map_picture() {
    const auto minimap = test_minimap();
    const auto fit = [&](int32_t world_width, int32_t world_height) {
        return mp::fit_map_picture(minimap, 252, 252, 125, 125, world_width, world_height);
    };
    // A map whose playable part is square fills the box but its last column and row.
    const auto square = fit(6176, 6272);
    expect(
        square == fit_model(minimap, 252, 252, 125, 6176, 6272),
        "a square map is stretched over the box"
    );
    expect(covers_only(square, 125, 0, 0, 124, 124), "it covers all but the last column and row");
    expect(square[0] == minimap[0], "the box's corner shows the minimap's");
    // A wide map (16352x11616 playable) is 88 rows tall, 18 rows down, and
    // shows the minimap's first 179 rows.
    const auto wide = fit(16384, 11744);
    expect(wide == fit_model(minimap, 252, 252, 125, 16384, 11744), "a wide map is letterboxed");
    expect(covers_only(wide, 125, 0, 18, 124, 106), "rows 18 to 105 show the map");
    expect(wide[18 * 125] == minimap[0], "the map's corner is on row 18");
    // A tall map (7168x19456 playable) is 46 columns wide, 39 columns in, and
    // shows the minimap's first 92 columns.
    const auto tall = fit(7200, 19584);
    expect(tall == fit_model(minimap, 252, 252, 125, 7200, 19584), "a tall map is pillarboxed");
    expect(covers_only(tall, 125, 39, 0, 85, 124), "columns 39 to 84 show the map");
    expect(tall[39] == minimap[0], "the map's corner is in column 39");
    expect(
        fit(32, 6272).empty() && fit(6176, 128).empty(),
        "a map with no playable part has no picture"
    );
    expect(
        mp::fit_map_picture(minimap, 252, 252, 0, 125, 6176, 6272).empty() &&
            mp::fit_map_picture(minimap, 252, 252, mp::kMapPictureMaxSide + 1, 125, 6176, 6272)
                .empty(),
        "an empty or oversized box has no picture"
    );
    expect(
        mp::fit_map_picture(std::span(minimap).first(252 * 251), 252, 252, 125, 125, 6176, 6272)
            .empty(),
        "a minimap shorter than its size says has no picture"
    );
}

/// Map services of the map preview tests: two maps and their files.
struct PreviewMaps {
    std::string selected = "Test Map";
    oa::data::campaign::CampaignFile test{};
    oa::data::campaign::CampaignFile other{};
    std::vector<uint8_t> test_file;
    std::vector<uint8_t> other_file;
    bool files_readable = true;
};

PreviewMaps* preview_maps = nullptr;

/// Returns a map file of a size in terrain cells whose minimap is given.
///
/// @param width map width in 16-pixel cells
/// @param height map height in 16-pixel cells
/// @param minimap the minimap's palette indices, 252x252
/// @return the file's bytes: the header, then the minimap
std::vector<uint8_t>
map_file(uint32_t width, uint32_t height, const std::vector<uint8_t>& minimap) {
    std::vector<uint8_t> bytes(64 + 8 + minimap.size());
    const auto put = [&](std::size_t at, uint32_t value) { std::memcpy(&bytes[at], &value, 4); };
    put(0x00, 0x2000); // version
    put(0x04, width);
    put(0x08, height);
    put(0x28, 64); // minimap offset
    put(0x2c, 1);  // minimap present
    put(64, 252);
    put(68, 252);
    std::memcpy(&bytes[72], minimap.data(), minimap.size());
    return bytes;
}

/// Fills a map context with a map's name, OTA memory and player counts, and map file path.
///
/// @param[out] map the context
/// @param name the map's name
/// @param memory the OTA's memory text
/// @param players the OTA's player counts
/// @param path the map file's path
void preview_context(
    oa::data::campaign::CampaignFile& map,
    const char* name,
    const char* memory,
    const char* players,
    const char* path
) {
    oa::data::campaign::campaign_file_init(&map);
    std::snprintf(map.localized_name, sizeof(map.localized_name), "%s", name);
    std::snprintf(map.memory, sizeof(map.memory), "%s", memory);
    std::snprintf(map.num_players, sizeof(map.num_players), "%s", players);
    std::snprintf(
        map.paths[static_cast<uint32_t>(oa::data::campaign::CampaignPath::mission)],
        sizeof(map.paths[0]),
        "%s",
        path
    );
}

/// Binds a lobby's map services to preview_maps.
///
/// @param[in,out] lobby the lobby
void bind_preview_maps(mp::Lobby& lobby) {
    lobby.maps.selected = [](void*) { return true; };
    lobby.maps.name = [](void*) { return preview_maps->selected.c_str(); };
    lobby.maps.select = [](void*, const char* value) {
        if (std::strcmp(value, "Test Map") != 0 && std::strcmp(value, "Other Map") != 0)
            return false;
        preview_maps->selected = value;
        return true;
    };
    lobby.maps.content_hash = [](void*) {
        return preview_maps->selected == "Test Map" ? 0x1111U : 0x2222U;
    };
    lobby.maps.description = [](void*) {
        return preview_maps->selected == "Test Map" ? "The test map" : "The other map";
    };
    lobby.maps.size_text = [](void*) { return "9 x 9"; };
    lobby.maps.map_context = [](void*) -> const oa::data::campaign::CampaignFile* {
        return preview_maps->selected == "Test Map" ? &preview_maps->test : &preview_maps->other;
    };
    lobby.maps.read_map = [](void*, const char* path, uint32_t offset, void* out, uint32_t size) {
        const auto& file = std::strcmp(path, "maps/test.tnt") == 0 ? preview_maps->test_file
                                                                   : preview_maps->other_file;
        if (!preview_maps->files_readable || offset > file.size() || size > file.size() - offset)
            return false;
        std::memcpy(out, file.data() + offset, size);
        return true;
    };
}

/// Builds VIEWMAP.GUI's gadgets: the map's name, its picture, description and size.
///
/// @return the layout
oa::ui::gui_layout::Layout view_map_layout() {
    namespace gui = oa::ui::gui_layout;
    const auto gadget = [](gui::GadgetType type,
                           const char* name,
                           int16_t x,
                           int16_t y,
                           int16_t width,
                           int16_t height) {
        gui::Gadget made;
        made.common.type = type;
        made.common.name = name;
        made.common.x = x;
        made.common.y = y;
        made.common.width = width;
        made.common.height = height;
        made.common.active = 1;
        if (type == gui::GadgetType::label)
            made.fields = gui::LabelFields{};
        else if (type == gui::GadgetType::hot_surface)
            made.fields = gui::HotSurfaceFields{false};
        return made;
    };
    gui::Layout layout;
    layout.gadgets.push_back(gadget(gui::GadgetType::panel, "HEADER", 141, 24, 330, 390));
    layout.gadgets.push_back(gadget(gui::GadgetType::hot_surface, "MAPPIC", 46, 114, 125, 125));
    layout.gadgets.push_back(gadget(gui::GadgetType::label, "DESCRIPTION", 49, 269, 235, 20));
    layout.gadgets.push_back(gadget(gui::GadgetType::label, "SIZE", 49, 288, 235, 20));
    layout.gadgets.push_back(gadget(gui::GadgetType::label, "MAPNAME", 50, 71, 242, 19));
    return layout;
}

/// Checks a map's summary: its name, memory and player counts, fitted minimap and description.
void test_map_summary() {
    PreviewMaps maps;
    preview_maps = &maps;
    const auto minimap = test_minimap();
    maps.test_file = map_file(386, 392, minimap);
    maps.other_file = map_file(1024, 734, minimap);
    preview_context(maps.test, "Test Map", "32 mb", "2, 4", "maps/test.tnt");
    preview_context(maps.other, "Other Map", "48 mb", "2, 3, 4, 5, 6", "maps/other.tnt");
    auto game = std::make_unique<oa::Game>();
    mp::Lobby lobby{};
    mp::lobby_reset(lobby, *game);
    bind_preview_maps(lobby);
    mp::Panel panel;
    mp::panel_load(panel, "guis/viewmap.gui", view_map_layout());
    mp::viewmap_open(lobby, panel);
    const auto* picture = mp::panel_control(panel, "MAPPIC");
    expect(mp::panel_text(panel, "MAPNAME") == "Test Map", "the map's name shows");
    expect(
        mp::panel_text(panel, "SIZE") == "32 mb  Players: 2, 4",
        "SIZE shows the map's memory and player counts"
    );
    expect(mp::panel_text(panel, "DESCRIPTION") == "The test map", "the map's description shows");
    expect(
        picture != nullptr && picture->picture_width == 125 && picture->picture_height == 125 &&
            picture->picture == fit_model(minimap, 252, 252, 125, 386 << 4, 392 << 4),
        "MAPPIC holds the map's minimap fitted to its box"
    );
    // "Players" goes through the game's translation, as translate.tdf gives
    // it for German; a text the translation does not give shows as it is.
    lobby.services.translate = [](void*, const char* text) -> const char* {
        return std::strcmp(text, "Players") == 0 ? "Spieler" : nullptr;
    };
    mp::map_summary_update(lobby, panel);
    expect(
        mp::panel_text(panel, "SIZE") == "32 mb  Spieler: 2, 4",
        "SIZE names the players in the game's language"
    );
    lobby.services.translate = [](void*, const char*) -> const char* { return nullptr; };
    mp::map_summary_update(lobby, panel);
    expect(
        mp::panel_text(panel, "SIZE") == "32 mb  Players: 2, 4",
        "a text the translation does not give shows as it is"
    );
    lobby.services.translate = nullptr;
    maps.selected = "Other Map";
    mp::map_summary_update(lobby, panel);
    expect(
        picture != nullptr &&
            picture->picture == fit_model(minimap, 252, 252, 125, 1024 << 4, 734 << 4) &&
            picture->picture[17 * 125] == 0 && picture->picture[18 * 125] != 0,
        "another map's minimap replaces it, letterboxed"
    );
    maps.files_readable = false;
    mp::map_summary_update(lobby, panel);
    expect(
        picture != nullptr && picture->picture.empty() && picture->picture_width == 0,
        "a map file that cannot be read leaves MAPPIC with no picture"
    );
    lobby.maps.map_context = nullptr;
    mp::map_summary_update(lobby, panel);
    expect(
        mp::panel_text(panel, "MAPNAME") == "Other Map" && mp::panel_text(panel, "SIZE") == "9 x 9",
        "without a map context the map's own name and size text show"
    );
    preview_maps = nullptr;
}

/// Checks that SELMAP hides MAPPIC while the highlighted map cannot be selected.
void test_map_select_preview() {
    namespace gui = oa::ui::gui_layout;
    PreviewMaps maps;
    preview_maps = &maps;
    const auto minimap = test_minimap();
    maps.test_file = map_file(386, 392, minimap);
    maps.other_file = map_file(1024, 734, minimap);
    preview_context(maps.test, "Test Map", "32 mb", "2, 4", "maps/test.tnt");
    preview_context(maps.other, "Other Map", "48 mb", "2, 3, 4, 5, 6", "maps/other.tnt");
    auto game = std::make_unique<oa::Game>();
    mp::Lobby lobby{};
    mp::lobby_reset(lobby, *game);
    bind_preview_maps(lobby);
    auto layout = view_map_layout();
    gui::Gadget names;
    names.common.type = gui::GadgetType::list_box;
    names.common.name = "MAPNAMES";
    names.common.x = 60;
    names.common.y = 86;
    names.common.width = 230;
    names.common.height = 193;
    names.common.active = 1;
    names.fields = gui::ListBoxFields{};
    layout.gadgets.push_back(names);
    mp::Panel panel;
    mp::panel_load(panel, "guis/selmap.gui", layout);
    mp::MapSelect select;
    select.names = {"Lost Map", "Other Map", "Test Map"};
    mp::panel_fill_list(panel, "MAPNAMES", select.names);
    auto* list = mp::panel_control(panel, "MAPNAMES");
    const auto* picture = mp::panel_control(panel, "MAPPIC");
    if (list == nullptr || picture == nullptr) {
        expect(false, "SELMAP has MAPNAMES and MAPPIC");
        preview_maps = nullptr;
        return;
    }
    list->list_selection = 2;
    mp::mapselect_preview(lobby, select, panel);
    expect(
        picture->active != 0 && picture->value == 0 &&
            picture->picture == fit_model(minimap, 252, 252, 125, 386 << 4, 392 << 4),
        "a map that can be selected shows its minimap in MAPPIC"
    );
    list->list_selection = 0;
    mp::mapselect_preview(lobby, select, panel);
    expect(
        picture->active == 0 && maps.selected == "Test Map" &&
            mp::panel_text(panel, "MAPNAME") == "Test Map",
        "a map that cannot be selected hides MAPPIC and keeps the summary"
    );
    list->list_selection = 1;
    mp::mapselect_preview(lobby, select, panel);
    expect(
        picture->active != 0 && maps.selected == "Other Map" &&
            picture->picture == fit_model(minimap, 252, 252, 125, 1024 << 4, 734 << 4),
        "the next map that can be selected shows MAPPIC again with its minimap"
    );
    preview_maps = nullptr;
}

/// Checks that a client's open VIEWMAP follows the host's map, and nothing else does while it is open.
void test_view_map_follows_host() {
    PreviewMaps maps;
    preview_maps = &maps;
    const auto minimap = test_minimap();
    maps.test_file = map_file(386, 392, minimap);
    maps.other_file = map_file(450, 1224, minimap);
    preview_context(maps.test, "Test Map", "32 mb", "2, 4", "maps/test.tnt");
    preview_context(maps.other, "Other Map", "64 mb", "2", "maps/other.tnt");
    auto game = std::make_unique<oa::Game>();
    mp::Lobby lobby{};
    mp::LoopbackNet loopback{};
    mp::loopback_reset(loopback);
    mp::lobby_reset(lobby, *game);
    lobby.net = mp::loopback_lobby_net(loopback);
    lobby.services.play_sound = sound;
    lobby.services.message = message;
    lobby.services.tick = tick;
    bind_preview_maps(lobby);
    lobby.local_version_major = 3;
    game->players[0].player_id = 0x200;
    mp::lobby_seat_local(lobby, 0, false, "Guest");
    mp::LobbyEvent host{};
    host.kind = mp::LobbyEventKind::player_joined;
    host.player_id = 0x100;
    std::snprintf(host.name, sizeof(host.name), "%s", "Host");
    (void)mp::lobby_apply_event(lobby, host);
    auto* host_info = mp::slot_info(lobby, 1);
    host_info->role = mp::kRoleHost;
    host_info->version_major = 3;
    host_info->version_minor = 1;
    host_info->map_hash = 0x1111;
    std::snprintf(host_info->map_name, sizeof(host_info->map_name), "%s", "Test Map");
    mp::Panel lounge;
    mp::panel_load(lounge, "guis/lounge2.gui", doors_layout());
    mp::panel_bind_sliders(lounge, slider_art);
    mp::lobby_enter_battleroom(lobby, lounge);
    mp::Panel view;
    mp::panel_load(view, "guis/viewmap.gui", view_map_layout());
    mp::viewmap_open(lobby, view);
    const auto* picture = mp::panel_control(view, "MAPPIC");
    expect(mp::panel_text(view, "MAPNAME") == "Test Map", "VIEWMAP opens on the host's map");

    // The host chooses another map: the open view follows it.
    std::snprintf(host_info->map_name, sizeof(host_info->map_name), "%s", "Other Map");
    host_info->map_hash = 0x2222;
    game->gui_flags |= 1;
    (void)mp::lobby_tick(lobby, lounge, mp::LobbyFront::view_map, &view);
    expect(maps.selected == "Other Map", "the client selects the host's new map");
    expect(
        mp::panel_text(view, "MAPNAME") == "Other Map" &&
            mp::panel_text(view, "SIZE") == "64 mb  Players: 2" &&
            mp::panel_text(view, "DESCRIPTION") == "The other map",
        "VIEWMAP shows the new map's summary"
    );
    expect(
        picture != nullptr &&
            picture->picture == fit_model(minimap, 252, 252, 125, 450 << 4, 1224 << 4) &&
            picture->picture[38] == 0 && picture->picture[39] != 0,
        "and its minimap, pillarboxed"
    );

    // Behind any other dialog the client keeps its map until the battle room is in front.
    std::snprintf(host_info->map_name, sizeof(host_info->map_name), "%s", "Test Map");
    host_info->map_hash = 0x1111;
    game->gui_flags |= 1;
    (void)mp::lobby_tick(lobby, lounge, mp::LobbyFront::dialog);
    expect(maps.selected == "Other Map", "behind another dialog the host's map waits");
    game->gui_flags |= 1;
    (void)mp::lobby_tick(lobby, lounge);
    expect(maps.selected == "Test Map", "with the battle room in front the client takes it");

    // A map this machine does not have shows its name and no picture.
    std::snprintf(host_info->map_name, sizeof(host_info->map_name), "%s", "Lost Map");
    game->gui_flags |= 1;
    (void)mp::lobby_tick(lobby, lounge, mp::LobbyFront::view_map, &view);
    expect(
        mp::panel_text(view, "MAPNAME") == "Lost Map" &&
            mp::panel_text(view, "DESCRIPTION").empty() && picture != nullptr &&
            picture->picture.empty(),
        "a map the client lacks shows its name, no description and no picture"
    );
    preview_maps = nullptr;
}

void test_client_unit_sync() {
    auto game = std::make_unique<oa::Game>();
    mp::Lobby lobby{};
    mp::LoopbackNet loopback{};
    mp::loopback_reset(loopback);
    mp::lobby_reset(lobby, *game);
    lobby.net = mp::loopback_lobby_net(loopback);
    game->session_flags |= 1;
    game->players[0].player_id = 0x200;
    mp::lobby_seat_local(lobby, 0, false, "Client");
    expect(mp::lobby_add_player(lobby, 0x100, "Host"), "the host is seated");
    mp::slot_info(lobby, 1)->role = mp::kRoleHost;
    std::vector<mp::LobbyUnit> units{{"", "", 0, 0, 0, 0, nullptr, 0, 0}};
    for (uint32_t key = 1; key <= 6; ++key)
        units.push_back({"U", "ARM", 0, 0, 0, key * 11, nullptr, 0, 0});
    // The client works each unit's checksum out as it reports it; the first
    // is already known.
    units[1].content_checksum = 0x1234;
    lobby.services.unit_checksum = [](void*, const mp::LobbyUnit& unit) {
        return unit.fbi_hash + 0x70000u;
    };
    lobby.units = units.data();
    lobby.unit_count = static_cast<int32_t>(units.size());
    mp::unit_sync_create(lobby, false);
    const auto start = loopback.sent_count;
    mp::unit_sync_tick(lobby);
    expect(loopback.sent_count == start, "a client waits for the host's greeting");
    const uint8_t greeting[14] = {0x1a, 0};
    mp::unit_sync_receive(lobby, greeting, 1);
    const auto read_u32 = [&](int32_t index, std::size_t offset) {
        uint32_t value = 0;
        std::memcpy(&value, loopback.sent[index] + offset, sizeof value);
        return value;
    };
    // The host lookup stops at the first host flag even when its slot is open.
    const auto saved_status = game->players[0].status;
    game->players[0].status = OA_PLAYER_STATUS_FREE;
    mp::slot_info(lobby, 0)->role |= mp::kRoleHost;
    mp::unit_sync_tick(lobby);
    expect(
        loopback.sent_count == start && lobby.sync.next_unit == 0,
        "an open first host slot prevents falling through to a later host"
    );
    game->players[0].status = saved_status;
    mp::slot_info(lobby, 0)->role &= static_cast<uint8_t>(~mp::kRoleHost);
    mp::unit_sync_tick(lobby);
    expect(
        loopback.sent_count == start + 1 && loopback.sent[start][1] == 1 &&
            read_u32(start, 10) == 6 && loopback.sent_to[start] == 0x100,
        "the unit count goes to the host first"
    );
    mp::unit_sync_tick(lobby);
    expect(
        loopback.sent_count == start + 5 && loopback.sent[start + 1][1] == 2 &&
            read_u32(start + 1, 6) == 11 && read_u32(start + 1, 10) == 0x1234 &&
            read_u32(start + 4, 6) == 44 && read_u32(start + 4, 10) == 44 + 0x70000u &&
            units[4].content_checksum == 44 + 0x70000u,
        "four unit keys a frame, each with its content checksum"
    );
    mp::unit_sync_tick(lobby);
    expect(loopback.sent_count == start + 7 && read_u32(start + 6, 6) == 66, "the remaining keys");
    mp::unit_sync_tick(lobby);
    expect(
        loopback.sent_count == start + 8 && loopback.sent[start + 7][1] == 4 &&
            read_u32(start + 7, 10) == 1,
        "then how many records it has handled"
    );
}

// ---- Who each battle-room record leaves from, and how records are batched ----

constexpr uint32_t kRoomHost = 0x100, kRoomGuest = 0x200, kRoomComputer = 0x201, kRoomThird = 0x300;
uint32_t milliseconds_now = 5000;

uint32_t milliseconds(void*) {
    return milliseconds_now;
}

/// Binds a lobby's clock, sounds, messages and disc to the test's.
///
/// @param[in,out] lobby Lobby whose services are set.
void bind_services(mp::Lobby& lobby) {
    lobby.services.play_sound = sound;
    lobby.services.message = message;
    lobby.services.tick = tick;
}

/// Binds a lobby's map services to the test's one map.
///
/// @param[in,out] lobby Lobby whose map services are set.
void bind_maps(mp::Lobby& lobby) {
    lobby.maps.selected = selected;
    lobby.maps.name = name;
    lobby.maps.select = select_map;
    lobby.maps.content_hash = hash;
    lobby.maps.memory_mb = memory;
}

/// A battle room with no layout: the local player in slot 0, the other
/// machine's player in slot 1 and a computer player of this machine in slot
/// 2, with the sent records cleared.
struct Room {
    std::unique_ptr<oa::Game> game = std::make_unique<oa::Game>();
    mp::Lobby lobby{};
    mp::LoopbackNet loopback{};
    mp::Panel panel;

    /// @param hosting true seats the local player as the host and the other
    ///        machine's player as a guest; false the other way round
    explicit Room(bool hosting) {
        mp::loopback_reset(loopback);
        mp::lobby_reset(lobby, *game);
        lobby.net = mp::loopback_lobby_net(loopback);
        bind_services(lobby);
        lobby.services.milliseconds = milliseconds;
        bind_maps(lobby);
        lobby.local_version_major = 3;
        game->session_flags |= 1;
        loopback.opened = true;
        game->players[0].player_id = hosting ? kRoomHost : kRoomGuest;
        mp::lobby_seat_local(lobby, 0, hosting, hosting ? "Host" : "Guest");
        join_remote(lobby, hosting ? kRoomGuest : kRoomHost);
        if (!hosting)
            mp::slot_info(lobby, 1)->role = mp::kRoleHost;
        auto& computer = mp::slot_player(lobby, 2);
        computer.in_use = 1;
        computer.player_id = kRoomComputer;
        computer.index = 2;
        mp::slot_set_status(lobby, computer, mp::kSlotComputer);
        mp::slot_info(lobby, 2)->color = 3;
        mp::local_info(lobby).color = 1;
        loopback.sent_count = 0;
    }

    /// Returns the indexes of the sent records of a type, in order.
    std::vector<int32_t> sent(oa::netgame::RecordType type) const {
        std::vector<int32_t> found;
        for (int32_t index = 0; index < loopback.sent_count; ++index)
            if (loopback.sent[index][0] == static_cast<uint8_t>(type))
                found.push_back(index);
        return found;
    }

    /// Decodes the sent 0x1b records in order.
    std::vector<oa::netgame::RejectRecord> rejects() const {
        std::vector<oa::netgame::RejectRecord> found;
        for (const auto index : sent(oa::netgame::RecordType::reject)) {
            oa::netgame::RejectRecord record{};
            (void)oa::netgame::decode_record(
                loopback.sent[index], loopback.sent_size[index], &record
            );
            found.push_back(record);
        }
        return found;
    }
};

// Each lobby block and team leaves from the player it describes, the two
// going out together; the machine-group requests leave from the primary
// player (the first seated one of this machine) for every slot still
// without a group, together, and are flushed.
void test_records_leave_from_their_players() {
    using oa::netgame::RecordType;
    Room room(false);
    const auto& sent = room.loopback;
    mp::lobby_send_player_info(room.lobby);
    const auto infos = room.sent(RecordType::player_info);
    const auto teams = room.sent(RecordType::player_team);
    auto groups = room.sent(RecordType::machine_group_request);
    expect(
        infos.size() == 2 && teams.size() == 2,
        "a lobby block and a team for each player of this machine"
    );
    if (infos.size() == 2 && teams.size() == 2) {
        expect(
            sent.sent_from[infos[0]] == kRoomGuest && sent.sent_from[infos[1]] == kRoomComputer,
            "each lobby block leaves from the player it describes"
        );
        expect(
            sent.sent_from[teams[0]] == kRoomGuest && sent.sent_from[teams[1]] == kRoomComputer,
            "each team leaves from its player"
        );
        expect(
            sent.sent_flush[infos[0]] == sent.sent_flush[teams[0]] &&
                sent.sent_flush[infos[1]] == sent.sent_flush[teams[1]] &&
                sent.sent_flush[infos[0]] < sent.sent_flush[infos[1]],
            "a player's block and team go out together, one player after the other"
        );
    }
    expect(groups.size() == 3, "a machine-group request for every slot without a group");
    bool from_primary = !groups.empty() && !teams.empty();
    for (const auto index : groups)
        from_primary = from_primary && sent.sent_from[index] == kRoomGuest &&
                       sent.sent_to[index] == kRoomHost &&
                       sent.sent_flush[index] == sent.sent_flush[groups[0]] &&
                       sent.sent_flush[index] > sent.sent_flush[teams.back()];
    expect(from_primary, "the requests leave together from the primary player to the host");
    expect(!groups.empty() && sent.flush_count > sent.sent_flush[groups.back()], "and are flushed");

    // Once groups are settled only a slot without one asks.
    mp::slot_player(room.lobby, 0).machine_group = 2;
    mp::slot_player(room.lobby, 1).machine_group = 1;
    room.loopback.sent_count = 0;
    mp::lobby_send_player_info(room.lobby);
    groups = room.sent(RecordType::machine_group_request);
    oa::netgame::MachineGroupRequestRecord request{};
    expect(
        groups.size() == 1 &&
            oa::netgame::decode_record(sent.sent[groups[0]], sent.sent_size[groups[0]], &request) ==
                oa::netgame::WireError::ok &&
            request.player_id == kRoomComputer && sent.sent_from[groups[0]] == kRoomGuest,
        "only the computer player, still without a group, is asked for"
    );

    // A seated sender was heard from; this machine's own players are not heard.
    clock_ticks += 10;
    oa::netgame::PlayerTeamRecord team{};
    team.player_id = kRoomHost;
    team.value = 2;
    expect(
        mp::lobby_apply_event(room.lobby, record_event(kRoomHost, team)), "the host's team applies"
    );
    expect(
        mp::slot_player(room.lobby, 1).last_update_time == clock_ticks,
        "the host was heard from now"
    );
    oa::netgame::ChatRecord echo{};
    std::snprintf(echo.text, sizeof(echo.text), "%s", "<Computer> echo");
    const auto head = mp::lobby_chat_head(*room.game);
    expect(
        !mp::lobby_apply_event(room.lobby, record_event(kRoomComputer, echo)) &&
            mp::lobby_chat_head(*room.game) == head,
        "a record from this machine's own computer player is not heard"
    );

    // The host answers a group request and sends its slot table from its own player.
    Room host(true);
    expect(
        mp::lobby_apply_event(
            host.lobby, record_event(kRoomGuest, group_request(1, kRoomGuest, 0xffffffffU))
        ),
        "the host answers a request"
    );
    const auto replies = host.sent(RecordType::machine_group_reply);
    expect(
        replies.size() == 1 && host.loopback.sent_from[replies[0]] == kRoomHost,
        "the group reply leaves from the host's player"
    );
    mp::lobby_send_slot_table(host.lobby);
    const auto tables = host.sent(RecordType::slot_table);
    expect(
        tables.size() == 1 && host.loopback.sent_from[tables[0]] == kRoomHost,
        "the slot table leaves from the host's player"
    );
}

// About every two seconds each player of this machine pings (unguaranteed,
// on its own) and probes; a player without a colour has the local player
// ask the host again; then the lobby blocks go out. An echo fills the
// echoing player's Ping column.
void test_periodic_block() {
    using oa::netgame::RecordType;
    Room room(false);
    const auto& sent = room.loopback;
    mp::local_info(room.lobby).color = mp::kNoColor;
    room.lobby.next_stats_tick = 0;
    milliseconds_now = 5000;
    (void)mp::lobby_tick(room.lobby, room.panel);
    const auto pings = room.sent(RecordType::ping);
    const auto probes = room.sent(RecordType::probe);
    const auto colours = room.sent(RecordType::player_value_request);
    const auto infos = room.sent(RecordType::player_info);
    expect(
        pings.size() == 2 && probes.size() == 2 && colours.size() == 1 && infos.size() == 2,
        "two pings, two probes, one colour request and the lobby blocks"
    );
    if (pings.size() == 2 && probes.size() == 2 && colours.size() == 1 && infos.size() == 2) {
        expect(
            sent.sent_from[pings[0]] == kRoomGuest && sent.sent_from[probes[0]] == kRoomGuest &&
                sent.sent_from[pings[1]] == kRoomComputer &&
                sent.sent_from[probes[1]] == kRoomComputer,
            "each player's ping and probe leave from it"
        );
        expect(
            pings[0] < probes[0] && probes[0] < colours[0] && colours[0] < pings[1] &&
                pings[1] < probes[1] && probes[1] < infos[0],
            "player by player: ping, probe, colour request; then the lobby blocks"
        );
        expect(
            sent.sent_unguaranteed[pings[0]] && sent.sent_unguaranteed[pings[1]] &&
                !sent.sent_unguaranteed[probes[0]] && !sent.sent_unguaranteed[infos[0]],
            "only the pings go without guaranteed delivery"
        );
        expect(
            sent.sent_flush[pings[0]] != sent.sent_flush[probes[0]] &&
                sent.sent_flush[pings[1]] != sent.sent_flush[probes[0]],
            "each ping goes out on its own"
        );
        oa::netgame::PingRecord ping{};
        expect(
            oa::netgame::decode_record(sent.sent[pings[1]], sent.sent_size[pings[1]], &ping) ==
                    oa::netgame::WireError::ok &&
                ping.origin_player_id == kRoomComputer && ping.origin_tick_count == 5000 &&
                ping.echo_tick_count == 0 && sent.sent_to[pings[1]] == 0,
            "a ping is broadcast with the sender's id and this machine's millisecond clock"
        );
        const auto* colour = sent.sent[colours[0]];
        expect(
            colour[1] == 0 && sent.sent_from[colours[0]] == kRoomGuest &&
                sent.sent_to[colours[0]] == kRoomHost,
            "the local player asks the host for colour 0"
        );
        expect(
            sent.flush_count > sent.sent_flush[sent.sent_count - 1],
            "everything queued goes out in the frame"
        );
    }
    expect(room.sent(RecordType::slot_table).empty(), "only the host sends the slot table");

    room.loopback.sent_count = 0;
    clock_ticks += 30;
    (void)mp::lobby_tick(room.lobby, room.panel);
    expect(room.sent(RecordType::ping).empty(), "nothing again within two seconds");
    clock_ticks += 31;
    (void)mp::lobby_tick(room.lobby, room.panel);
    expect(
        room.sent(RecordType::ping).size() == 2 &&
            room.sent(RecordType::player_value_request).size() == 1,
        "the block runs again after sixty ticks, still asking for a colour"
    );
    mp::local_info(room.lobby).color = 4;
    room.loopback.sent_count = 0;
    clock_ticks += 61;
    (void)mp::lobby_tick(room.lobby, room.panel);
    expect(
        room.sent(RecordType::ping).size() == 2 &&
            room.sent(RecordType::player_value_request).empty(),
        "with a colour no request goes out"
    );

    // Echoes.
    oa::netgame::PingRecord echo{};
    echo.origin_tick_count = 4990;
    echo.echo_tick_count = 1;
    echo.origin_player_id = kRoomGuest;
    milliseconds_now = 5030;
    auto& host_slot = mp::slot_player(room.lobby, 1);
    mp::lobby_player_ping(host_slot) = -1;
    expect(mp::lobby_apply_event(room.lobby, record_event(kRoomHost, echo)), "an echo applies");
    expect(mp::lobby_player_ping(host_slot) == 40, "the echoing player's ping is the round trip");
    echo.origin_player_id = kRoomThird;
    expect(
        !mp::lobby_apply_event(room.lobby, record_event(kRoomHost, echo)) &&
            mp::lobby_player_ping(host_slot) == 40,
        "an echo of a ping this machine did not send changes nothing"
    );
    echo.origin_player_id = kRoomGuest;
    echo.echo_tick_count = 0;
    expect(
        !mp::lobby_apply_event(room.lobby, record_event(kRoomHost, echo)),
        "a request is answered by the connection"
    );

    // The host's own player also sends the slot table, and needs no colour request.
    Room host(true);
    host.lobby.next_stats_tick = 0;
    (void)mp::lobby_tick(host.lobby, host.panel);
    const auto tables = host.sent(RecordType::slot_table);
    const auto host_probes = host.sent(RecordType::probe);
    expect(
        tables.size() == 1 && host.loopback.sent_from[tables[0]] == kRoomHost &&
            !host_probes.empty() && tables[0] > host_probes[0],
        "the host's player sends the slot table after its probe"
    );
    expect(host.sent(RecordType::player_value_request).empty(), "a host with a colour asks nobody");
}

// A client whose host's player is destroyed before the start rejects the
// host (1) and itself (10) and leaves with "The creator has left the game";
// any other departure is only passed on as a rejection.
void test_host_leaving() {
    using oa::netgame::RecordType;
    mp::LobbyEvent left{};
    left.kind = mp::LobbyEventKind::player_left;
    {
        Room room(false);
        left.player_id = kRoomHost;
        expect(mp::lobby_apply_event(room.lobby, left), "the host's departure applies");
        const auto rejects = room.rejects();
        expect(
            rejects.size() == 3 && rejects[0].player_id == kRoomHost && rejects[0].reason == 1 &&
                rejects[1].player_id == kRoomGuest && rejects[1].reason == 10 &&
                rejects[2].player_id == kRoomComputer && rejects[2].reason == 10,
            "the host is rejected with 1, then each player of this machine with 10"
        );
        const auto indexes = room.sent(RecordType::reject);
        expect(
            indexes.size() == 3 && room.loopback.sent_from[indexes[0]] == kRoomGuest &&
                room.loopback.sent_from[indexes[1]] == kRoomGuest &&
                room.loopback.sent_from[indexes[2]] == kRoomComputer &&
                room.loopback.sent_to[indexes[1]] == 0 && room.loopback.sent_to[indexes[2]] == 0,
            "each leaves to everyone from the first player of this machine still in the game"
        );
        expect(
            mp::local_player(room.lobby).reject_reason == 10 &&
                mp::slot_player(room.lobby, 2).reject_reason == 10,
            "every player of this machine leaves"
        );
        expect(
            std::strcmp(mp::reject_reason_text(10), "The creator has left the game") == 0,
            "the game list says the creator left"
        );
        expect(
            mp::slot_player(room.lobby, 1).status == mp::kSlotOpen && room.loopback.flush_count > 0,
            "the host's slot opens and the records go out"
        );
    }
    {
        Room room(false);
        join_remote(room.lobby, kRoomThird);
        room.loopback.sent_count = 0;
        left.player_id = kRoomThird;
        const auto slot = mp::slot_for_player_id(room.lobby, kRoomThird);
        // Each machine holds its own group, as the host's replies assign them.
        mp::slot_player(room.lobby, 1).machine_group = 1;
        if (slot > 0)
            mp::slot_player(room.lobby, slot).machine_group = 3;
        const uint16_t seated = mp::lobby_player_count(*room.game);
        expect(mp::lobby_apply_event(room.lobby, left), "another departure applies");
        expect(
            mp::lobby_player_count(*room.game) == seated - 1,
            "the departed player is counted out once"
        );
        const auto rejects = room.rejects();
        expect(
            rejects.size() == 1 && rejects[0].player_id == kRoomThird && rejects[0].reason == 1,
            "another departure is passed on once with reason 1"
        );
        expect(
            mp::local_player(room.lobby).reject_reason == 0 && slot > 0 &&
                mp::slot_player(room.lobby, slot).status == mp::kSlotOpen,
            "its slot opens and this machine stays"
        );
    }
    {
        Room room(false);
        room.game->session_flags |= mp::kNetFlagGameStarted;
        left.player_id = kRoomHost;
        (void)mp::lobby_apply_event(room.lobby, left);
        const auto rejects = room.rejects();
        expect(
            rejects.size() == 1 && rejects[0].player_id == kRoomHost &&
                mp::local_player(room.lobby).reject_reason == 0,
            "after the start the host's departure is only passed on"
        );
    }
}

// A received 0x1b naming a player not yet rejected is passed on once; one
// naming this machine's player rejects every player here with one record.
void test_reject_passed_on_once() {
    oa::netgame::RejectRecord record{};
    {
        Room room(true);
        record.player_id = kRoomGuest;
        record.reason = 1;
        const uint16_t seated = mp::lobby_player_count(*room.game);
        expect(
            mp::lobby_apply_event(room.lobby, record_event(kRoomGuest, record)),
            "a rejection applies"
        );
        expect(
            mp::lobby_player_count(*room.game) == seated - 1, "the rejected player is counted out"
        );
        mp::LobbyEvent left{};
        left.kind = mp::LobbyEventKind::player_left;
        left.player_id = kRoomGuest;
        expect(
            !mp::lobby_apply_event(room.lobby, left) &&
                mp::lobby_player_count(*room.game) == seated - 1,
            "its departure afterwards is not counted again"
        );
        auto rejects = room.rejects();
        const auto indexes = room.sent(oa::netgame::RecordType::reject);
        expect(
            rejects.size() == 1 && rejects[0].player_id == kRoomGuest && rejects[0].reason == 1 &&
                room.loopback.sent_from[indexes[0]] == kRoomHost &&
                room.loopback.sent_to[indexes[0]] == 0,
            "the first rejection is passed on from the primary player"
        );
        expect(
            mp::slot_player(room.lobby, 1).status == mp::kSlotOpen &&
                mp::slot_player(room.lobby, 1).in_use == 0,
            "the rejected slot opens"
        );
        expect(
            !mp::lobby_apply_event(room.lobby, record_event(kRoomThird, record)) &&
                room.rejects().size() == 1,
            "the same rejection again is not passed on"
        );
    }
    {
        Room room(false);
        record.player_id = kRoomGuest;
        record.reason = 1;
        expect(
            mp::lobby_apply_event(room.lobby, record_event(kRoomHost, record)),
            "a rejection of this machine applies"
        );
        auto rejects = room.rejects();
        const auto indexes = room.sent(oa::netgame::RecordType::reject);
        expect(
            rejects.size() == 2 && rejects[0].player_id == kRoomGuest && rejects[0].reason == 1 &&
                rejects[1].player_id == kRoomComputer && rejects[1].reason == 1 &&
                room.loopback.sent_from[indexes[0]] == kRoomGuest &&
                room.loopback.sent_from[indexes[1]] == kRoomComputer,
            "each player of this machine is named, the computer player by itself"
        );
        expect(
            mp::local_player(room.lobby).reject_reason == 1 &&
                mp::slot_player(room.lobby, 2).reject_reason == 1,
            "every player of this machine is rejected"
        );
        (void)mp::lobby_apply_event(room.lobby, record_event(kRoomHost, record));
        expect(room.rejects().size() == 2, "an already rejected player is not passed on again");
    }
}

// Rejecting this machine's player follows the slot order: before the start
// the local player leaves at the first record and the next seated player
// sends the rest, so a computer player seated ahead of it is the only one
// named; after the start only the first record goes out. A remote human
// still playing takes its machine's other players with it; a remote
// computer player leaves alone.
void test_rejection_order() {
    using oa::netgame::RecordType;
    {
        auto game = std::make_unique<oa::Game>();
        mp::Lobby lobby{};
        mp::LoopbackNet loopback{};
        mp::loopback_reset(loopback);
        mp::lobby_reset(lobby, *game);
        lobby.net = mp::loopback_lobby_net(loopback);
        bind_services(lobby);
        bind_maps(lobby);
        lobby.local_version_major = 3;
        game->session_flags |= 1;
        loopback.opened = true;
        auto& computer = mp::slot_player(lobby, 0);
        computer.in_use = 1;
        computer.player_id = kRoomComputer;
        computer.index = 0;
        mp::slot_set_status(lobby, computer, mp::kSlotComputer);
        game->players[1].player_id = kRoomGuest;
        mp::lobby_seat_local(lobby, 1, false, "Guest");
        loopback.sent_count = 0;
        mp::lobby_reject(lobby, kRoomGuest, 2);
        int32_t rejects = 0;
        for (int32_t index = 0; index < loopback.sent_count; ++index) {
            if (loopback.sent[index][0] != static_cast<uint8_t>(RecordType::reject))
                continue;
            oa::netgame::RejectRecord record{};
            (void)oa::netgame::decode_record(
                loopback.sent[index], loopback.sent_size[index], &record
            );
            expect(
                record.player_id == kRoomComputer && loopback.sent_from[index] == kRoomComputer,
                "the computer player seated ahead is named, by itself"
            );
            ++rejects;
        }
        expect(rejects == 1, "the local player is not named after it");
        expect(
            computer.reject_reason == 2 && mp::local_player(lobby).reject_reason == 2,
            "both players of this machine are rejected"
        );
    }
    {
        Room room(false);
        room.game->session_flags |= mp::kNetFlagGameStarted;
        mp::lobby_reject(room.lobby, kRoomGuest, 2);
        const auto rejects = room.rejects();
        expect(
            rejects.size() == 1 && rejects[0].player_id == kRoomGuest &&
                mp::slot_player(room.lobby, 2).reject_reason == 2,
            "after the start only the first record goes out"
        );
    }
    {
        Room room(true);
        join_remote(room.lobby, kRoomThird);
        const auto guest = mp::slot_for_player_id(room.lobby, kRoomGuest);
        const auto third = mp::slot_for_player_id(room.lobby, kRoomThird);
        mp::slot_player(room.lobby, guest).machine_group = 2;
        mp::slot_player(room.lobby, third).machine_group = 2;
        mp::slot_info(room.lobby, third)->state = 0;
        const uint16_t seated = mp::lobby_player_count(*room.game);
        mp::lobby_reject(room.lobby, kRoomThird, 1);
        expect(
            mp::slot_player(room.lobby, third).in_use == 0 &&
                mp::slot_player(room.lobby, guest).in_use != 0 &&
                mp::lobby_player_count(*room.game) == seated - 1,
            "a remote player that is not a human still playing leaves alone"
        );
        join_remote(room.lobby, kRoomThird);
        const auto again = mp::slot_for_player_id(room.lobby, kRoomThird);
        mp::slot_player(room.lobby, again).machine_group = 2;
        mp::slot_player(room.lobby, 2).machine_group = 2;
        room.loopback.sent_count = 0;
        mp::lobby_reject(room.lobby, kRoomGuest, 1);
        const auto rejects = room.rejects();
        expect(
            rejects.size() == 1 && rejects[0].player_id == kRoomGuest &&
                mp::slot_player(room.lobby, guest).in_use == 0 &&
                mp::slot_player(room.lobby, again).in_use == 0 &&
                mp::slot_player(room.lobby, again).reject_reason == 1 &&
                mp::lobby_player_count(*room.game) == seated - 2,
            "a remote human still playing takes its machine's other players with it"
        );
        expect(
            mp::slot_player(room.lobby, 2).in_use != 0 &&
                mp::slot_player(room.lobby, 2).reject_reason == 0,
            "this machine's own players stay, whatever their machine group"
        );
    }
}

// A joiner the host refuses is told with the host's lobby blocks, then the
// rejection; it is neither seated nor destroyed.
void test_refused_joiner() {
    using oa::netgame::RecordType;
    Room room(true);
    mp::LobbyEvent joined{};
    joined.kind = mp::LobbyEventKind::player_joined;
    joined.player_id = 0x400;
    joined.refuse_reason = 4;
    std::snprintf(joined.name, sizeof(joined.name), "%s", "Late");
    expect(!mp::lobby_apply_event(room.lobby, joined), "a refused joiner changes nothing here");
    expect(mp::slot_for_player_id(room.lobby, 0x400) < 0, "it is not seated");
    const auto infos = room.sent(RecordType::player_info);
    const auto rejects = room.rejects();
    const auto indexes = room.sent(RecordType::reject);
    expect(
        infos.size() == 2 && rejects.size() == 1 && rejects[0].player_id == 0x400 &&
            rejects[0].reason == 4 && indexes[0] > infos.back() &&
            room.loopback.sent_to[indexes[0]] == 0,
        "the lobby blocks go out, then 0x1b with the refusal"
    );
    expect(room.loopback.removed_player == 0, "its player is not destroyed");

    // Once machines are shared, records to everyone go to one player of each
    // machine; the joiner holds no slot and is told directly.
    Room shared(true);
    shared.game->shared_machines = 1;
    shared.loopback.sent_count = 0;
    expect(!mp::lobby_apply_event(shared.lobby, joined), "a refused joiner changes nothing here");
    const auto shared_rejects = shared.sent(RecordType::reject);
    bool told = false;
    bool machines_told = false;
    for (const auto index : shared_rejects) {
        told = told || shared.loopback.sent_to[index] == 0x400;
        machines_told = machines_told || shared.loopback.sent_to[index] == kRoomGuest;
    }
    bool blocks_told = false;
    for (const auto index : shared.sent(RecordType::player_info))
        blocks_told = blocks_told || shared.loopback.sent_to[index] == 0x400;
    expect(
        told && machines_told && blocks_told,
        "with shared machines the joiner still hears its lobby blocks and its rejection"
    );
    expect(shared.lobby.refused_joiner == 0, "later records go per machine alone");
}

// The session is created with its full name and user bytes, and published
// with flags 4 and a 0x20 that stays once set, never 0x400, and with the
// player limit the battle room holds.
void test_session_description() {
    auto game = std::make_unique<oa::Game>();
    mp::Lobby lobby{};
    mp::LoopbackNet loopback{};
    mp::ConnectState connect{};
    mp::loopback_reset(loopback);
    mp::lobby_reset(lobby, *game);
    lobby.net = mp::loopback_lobby_net(loopback);
    bind_services(lobby);
    bind_maps(lobby);
    lobby.local_version_major = 3;
    loopback.opened = true;
    std::snprintf(mp::lobby_game_name(*game), 17, "%s", "Big Battle");
    std::snprintf(mp::lobby_nickname(*game), 17, "%s", "Host");
    std::snprintf(mp::lobby_password(*game), 11, "%s", "secret");
    expect(mp::connect_host(lobby, connect), "the host creates the session");
    const auto& session = loopback.sessions[0];
    char expected[mp::kSessionNameBytes];
    std::snprintf(expected, sizeof expected, "%-16s%-15s", "Big Battle", name(nullptr));
    expect(
        std::memcmp(session.name, expected, sizeof expected) == 0,
        "the session starts with its full name"
    );
    const auto& info = mp::local_info(lobby);
    const auto* user =
        reinterpret_cast<const uint8_t*>(&info) + offsetof(mp::PlayerSetupInfo, memory_mb);
    expect(
        std::memcmp(session.user, user, sizeof session.user) == 0 &&
            (info.status & mp::status::password) != 0,
        "and with the host's user bytes, password bit included"
    );
    expect(
        mp::local_player(lobby).player_id == loopback.local_id && info.net_id == loopback.local_id,
        "the local player takes the id the session gave"
    );
    mp::lobby_publish_session(lobby);
    expect(
        session.flags == 0x4 && session.max_players == 10,
        "a password game publishes flags 4 and ten players"
    );
    mp::lobby_session_players(*game) = 9;
    mp::lobby_publish_session(lobby);
    expect(session.max_players == 9, "a closed slot lowers the published limit");
    mp::local_info(lobby).options |= mp::option::started;
    mp::lobby_publish_session(lobby);
    expect(session.flags == 0x24, "a started game adds 0x20");
    mp::local_info(lobby).options &= static_cast<uint16_t>(~mp::option::started);
    mp::lobby_publish_session(lobby);
    expect(session.flags == 0x24, "0x20 stays once published");
}

/// Returns network rules of a game versioned 10.2 with the recorder presented.
oa::netgame::WireRules versioned_recorder_rules() {
    oa::netgame::WireRules rules{};
    rules.version_major = 10;
    rules.version_minor = 2;
    rules.version_rule = oa::netgame::VersionRule::equal;
    rules.launch_version_bias = false;
    rules.private_channel = oa::netgame::PrivateChannel::sub_id_dispatch;
    rules.recorder_protocol = oa::netgame::recorder_protocol_current;
    rules.recorder_session_commands = true;
    rules.speed_lock = true;
    return rules;
}

/// Returns a raw record event from a player.
mp::LobbyEvent raw_event(uint32_t from, std::initializer_list<uint8_t> bytes) {
    mp::LobbyEvent event{};
    event.kind = mp::LobbyEventKind::record;
    event.player_id = from;
    std::size_t at = 0;
    for (const auto byte : bytes)
        event.data[at++] = byte;
    event.size = static_cast<uint16_t>(at);
    return event;
}

// A game versioned 10.2 compares majors for equality and has no launch
// bias: its session and blocks carry 10 and 2 as they are, and a 3.1 game
// is not joinable from it, nor it from a 3.1 game's rule of "at least".
void test_versioned_rules() {
    auto rules = versioned_recorder_rules();
    mp::PlayerSetupInfo host{};
    host.version_major = 10;
    expect(mp::info_version_compatible(host, 10, rules), "an equal major joins");
    host.version_major = 3;
    expect(
        !mp::info_version_compatible(host, 10, rules), "a 3.1 game does not join under equality"
    );
    host.version_major = 11;
    expect(!mp::info_version_compatible(host, 10, rules), "a newer major does not either");
    host.version_major = 110;
    host.status = mp::status::launch_only;
    expect(
        !mp::info_version_compatible(host, 10, rules),
        "without the launch bias a launch-only game's byte is read as it is"
    );
    host.version_major = 10;
    host.status = 0;
    expect(mp::info_version_compatible(host, 3), "3.1's rule still joins a newer major");

    reset_launch();
    auto game = std::make_unique<oa::Game>();
    mp::Lobby lobby{};
    mp::LoopbackNet loopback{};
    mp::ConnectState connect{};
    mp::loopback_reset(loopback);
    mp::lobby_reset(lobby, *game);
    lobby.net = mp::loopback_lobby_net(loopback);
    bind_services(lobby);
    bind_maps(lobby);
    lobby.wire_rules = rules;
    lobby.local_version_major = 10;
    lobby.local_version_minor = 2;
    lobby.launch_link = launch_link();
    active_launch = true;
    loopback.opened = true;
    std::snprintf(mp::lobby_game_name(*game), 17, "%s", "XGame");
    std::snprintf(mp::lobby_nickname(*game), 17, "%s", "Alpha");
    expect(mp::connect_host(lobby, connect), "the host creates the session");
    const auto& session = loopback.sessions[0];
    expect(
        session.user[14] == 10 && session.user[15] == 2,
        "the session's last user bytes are 10 and 2: no bias under an active launch"
    );
    expect(
        (mp::local_info(lobby).status & mp::status::launch_only) == 0,
        "nor is it marked launch-only"
    );
    active_launch = false;
    reset_launch();
}

// A recorder stamps its protocol on every battle-room block; the host sends
// its recorder options once to a joiner whose block says it runs one, and
// takes a joiner's recorder protocol from its block.
void test_recorder_in_the_battle_room() {
    using oa::netgame::RecordType;
    Room host(true);
    host.lobby.wire_rules = versioned_recorder_rules();
    mp::lobby_send_player_info(host.lobby);
    const auto infos = host.sent(RecordType::player_info);
    expect(infos.size() == 2, "a block for each player here");
    bool stamped = !infos.empty();
    for (const auto index : infos)
        stamped =
            stamped &&
            host.loopback.sent[index][1 + oa::netgame::player_info_recorder_protocol_offset] ==
                oa::netgame::recorder_protocol_current;
    expect(stamped, "every block carries the recorder's protocol");
    bool signed_by_oa = !infos.empty();
    for (const auto index : infos)
        signed_by_oa =
            signed_by_oa && oa::netgame::sent_by_open_annihilation(&host.loopback.sent[index][1]);
    expect(signed_by_oa, "every block says Open Annihilation sent it");

    // The joiner's first block from a recorder: the options go to it once.
    oa::netgame::PlayerInfoRecord block{};
    block.player_id = kRoomGuest;
    block.info_tail
        [oa::netgame::player_info_recorder_protocol_offset - oa::netgame::player_info_tail_offset] =
        oa::netgame::recorder_protocol_current;
    host.loopback.sent_count = 0;
    expect(mp::lobby_apply_event(host.lobby, record_event(kRoomGuest, block)), "the block applies");
    expect(
        host.lobby.recorder.peer_protocol[1] == oa::netgame::recorder_protocol_current,
        "the joiner's recorder protocol is noted"
    );
    auto options = host.sent(static_cast<RecordType>(0xfb));
    expect(options.size() == 1, "the host's options go out once");
    if (options.size() == 1) {
        const uint8_t expected[] = {0xfb, 0x06, 0x04, 0x00, 0x00, 0x00, 0x00, 0x01, 0x14};
        expect(
            host.loopback.sent_size[options[0]] == sizeof expected &&
                std::memcmp(host.loopback.sent[options[0]], expected, sizeof expected) == 0 &&
                host.loopback.sent_to[options[0]] == kRoomGuest,
            "as a recorder sends them before any lock, to the joiner alone"
        );
    }
    host.loopback.sent_count = 0;
    (void)mp::lobby_apply_event(host.lobby, record_event(kRoomGuest, block));
    expect(host.sent(static_cast<RecordType>(0xfb)).empty(), "and only once");

    // The joiner takes the host's options.
    Room guest(false);
    guest.lobby.wire_rules = versioned_recorder_rules();
    expect(
        mp::lobby_apply_event(
            guest.lobby,
            raw_event(kRoomHost, {0xfb, 0x06, 0x04, 0x01, 0x00, 0x01, 0x01, 0x05, 0x00})
        ),
        "the host's options apply"
    );
    expect(
        guest.lobby.recorder.options.autopause == 1 &&
            guest.lobby.recorder.options.commander_warp == 1 &&
            guest.lobby.recorder.options.speed_lock == 1,
        "autopause, commander warp and the lock are the host's"
    );
    // Under 3.1c's rules the record is not read.
    Room plain(false);
    expect(
        !mp::lobby_apply_event(plain.lobby, raw_event(kRoomHost, {0xfb, 0x00, 0x01})),
        "3.1c reads no recorder record"
    );
}

// Every block the battle room sends, a computer player's too, keeps the five
// bytes after the engine signature zero, as 0.7 sent them, and so does the
// block the local player keeps. The signature, the 3.1c version and the disc
// travel as before.
void test_no_presence_bytes_in_the_battle_room() {
    namespace ng = oa::netgame;
    Room host(true);
    auto& local = mp::local_info(host.lobby);
    const auto* computer = mp::slot_info(host.lobby, 2);
    const auto spare_zero = [](const uint8_t* block) {
        const auto* spare = block + offsetof(mp::PlayerSetupInfo, reserved_after_signature);
        const auto* end = spare + sizeof(mp::PlayerSetupInfo::reserved_after_signature);
        return std::all_of(spare, end, [](uint8_t at) { return at == 0; });
    };
    mp::lobby_send_player_info(host.lobby);
    const auto infos = host.sent(ng::RecordType::player_info);
    expect(infos.size() == 2, "a block for the host and the computer player");
    const mp::PlayerSetupInfo* sources[] = {&local, computer};
    bool as_before = infos.size() == 2;
    for (std::size_t index = 0; as_before && index < infos.size(); ++index) {
        const auto* block = host.loopback.sent[infos[index]] + 1;
        as_before = spare_zero(block) && ng::sent_by_open_annihilation(block) &&
                    block[ng::player_info_version_major_offset] == sources[index]->version_major &&
                    block[ng::player_info_version_minor_offset] == sources[index]->version_minor;
    }
    expect(as_before, "every block sent leaves the five bytes zero and the rest as before");

    mp::lobby_enter_battleroom(host.lobby, host.panel);
    const auto* kept = reinterpret_cast<const uint8_t*>(&mp::local_info(host.lobby));
    expect(
        spare_zero(kept) && ng::sent_by_open_annihilation(kept) &&
            (mp::local_info(host.lobby).status & mp::status::has_disc) != 0,
        "the block the battle room keeps leaves the five bytes zero"
    );
}

// ---- Presence records in the battle room ----

/// Encodes a presence record.
///
/// @param record the record's fields
/// @param capacity the bytes it may take
/// @return the record
std::vector<uint8_t> encoded_presence(
    const oa::netgame::PresenceRecord& record,
    std::size_t capacity = oa::netgame::presence_record_max_bytes
) {
    std::vector<uint8_t> bytes(oa::netgame::presence_record_max_bytes);
    std::size_t written = 0;
    expect(
        oa::netgame::encode_presence(record, bytes.data(), capacity, &written) ==
            oa::netgame::WireError::ok,
        "a test presence record encodes"
    );
    bytes.resize(written);
    return bytes;
}

/// Returns a small presence record: an engine line and a sim hash.
///
/// @param version the engine's version text
/// @return the record
std::vector<uint8_t> small_presence(const char* version) {
    oa::netgame::PresenceRecord record;
    record.engine = oa::netgame::PresenceEngine{version, "macOS", "arm64"};
    record.sim_hash = oa::netgame::PresenceDigest{};
    record.sim_hash->fill(0x5a);
    return encoded_presence(record);
}

/// Binds a record as the lobby's source, as the screens' binding copies it.
///
/// @param[in,out] lobby the lobby
/// @param record the record
void bind_presence_source(mp::Lobby& lobby, const std::vector<uint8_t>& record) {
    auto& records = lobby.presence_records;
    std::copy(record.begin(), record.end(), records.source);
    records.source_size = static_cast<uint16_t>(record.size());
}

/// Returns a presence record event from a player.
///
/// @param from the sender
/// @param record the record's bytes
/// @return the event
mp::LobbyEvent presence_event(uint32_t from, const std::vector<uint8_t>& record) {
    mp::LobbyEvent event{};
    event.kind = mp::LobbyEventKind::record;
    event.player_id = from;
    std::copy(record.begin(), record.end(), event.data);
    event.size = static_cast<uint16_t>(record.size());
    return event;
}

/// Returns a player's setup block as record 0x20 from it.
///
/// @param id the player
/// @param state the block's state: 1 for a human, 2 for a computer player
/// @param signature the bytes of the 'OA' signature it carries: 0, 1 ('O' alone) or 2
/// @return the record; the five bytes after the signature are zero, as every OA block's are
oa::netgame::PlayerInfoRecord setup_block(uint32_t id, uint8_t state, int signature) {
    namespace ng = oa::netgame;
    ng::PlayerInfoRecord record{};
    record.player_id = id;
    record.info_tail[offsetof(mp::PlayerSetupInfo, state) - ng::player_info_tail_offset] = state;
    auto* mark =
        record.info_tail + (ng::player_info_engine_signature_offset - ng::player_info_tail_offset);
    if (signature >= 1)
        mark[0] = ng::engine_signature_first;
    if (signature >= 2)
        mark[1] = ng::engine_signature_second;
    return record;
}

/// The map pack the test's binding names, and how often it was asked.
oa::netgame::PresenceMapPack test_pack;
int32_t pack_asks = 0;

/// Answers LobbyMaps::pack with test_pack, counting the asks.
///
/// @param[out] out the pack
/// @return true
bool count_pack(void*, oa::netgame::PresenceMapPack* out) {
    ++pack_asks;
    *out = test_pack;
    return true;
}

/// Returns the indexes of the sent presence records.
///
/// @param room the room
/// @return the indexes, in order
std::vector<int32_t> sent_presence(const Room& room) {
    return room.sent(static_cast<oa::netgame::RecordType>(oa::netgame::presence_record_type));
}

/// Tells whether a sent record went out alone: no other sent record shares its flush.
///
/// @param room the room
/// @param index the record's index
/// @return true when it went out alone
bool sent_alone(const Room& room, int32_t index) {
    for (int32_t other = 0; other < room.loopback.sent_count; ++other)
        if (other != index && room.loopback.sent_flush[other] == room.loopback.sent_flush[index])
            return false;
    return true;
}

/// Tells whether a sent record holds bytes.
///
/// @param room the room
/// @param index the record's index
/// @param bytes the bytes
/// @param size their length
/// @return true when the record is those bytes
bool sent_is(const Room& room, int32_t index, const uint8_t* bytes, std::size_t size) {
    return room.loopback.sent_size[index] == size &&
           std::memcmp(room.loopback.sent[index], bytes, size) == 0;
}

/// Tells whether every setup block a room sent keeps the five bytes after the signature zero.
///
/// @param room the room
/// @return true when they are all zero
bool blocks_without_presence(const Room& room) {
    for (const auto index : room.sent(oa::netgame::RecordType::player_info)) {
        const auto* spare =
            room.loopback.sent[index] + 1 + offsetof(mp::PlayerSetupInfo, reserved_after_signature);
        if (!std::all_of(spare, spare + 5, [](uint8_t at) { return at == 0; }))
            return false;
    }
    return true;
}

/// Seats the session player a click on a blocked slot asked for.
///
/// @param[in,out] room the battle room
/// @return the arrival's player id, or 0 when none came
uint32_t arrive_computer(Fixture& room) {
    mp::LobbyEvent arrival{};
    if (!room.lobby.net.receive(room.lobby.net.context, &arrival) ||
        !mp::lobby_apply_event(room.lobby, arrival))
        return 0;
    mp::lobby_update_status(room.lobby, room.panel);
    return arrival.player_id;
}

/// Battle-room rows for computer players: several per human, difficulty on
/// the row, a host change of another OA machine's, and a 3.1c machine's
/// left as a label the host can still remove.
void test_computer_rows(const oa::ui::gui_layout::Layout& lounge) {
    Fixture host(lounge);
    host.loopback.next_player_id = 0x101;
    expect(host.press("PLAYER1") && host.press("PLAYER1"), "a blocked slot seats a computer");
    const auto first = arrive_computer(host);
    auto& seated = mp::slot_player(host.lobby, 1);
    expect(
        first != 0 && seated.status == mp::kSlotComputer && seated.in_use != 0 &&
            mp::panel_text(host.panel, "PLAYER1") == "AI:Host",
        "the computer player's row names the human who runs it"
    );
    const auto* mem = mp::panel_control(host.panel, "MEM1");
    expect(
        mem != nullptr && mem->type == mp::ControlType::button && !mem->grayed &&
            mp::panel_text(host.panel, "MEM1") == "Hard" && host.lobby.computer_difficulty[1] == 2,
        "a computer player's row shows its difficulty"
    );
    expect(host.press("MEM1"), "the difficulty can be changed");
    mp::lobby_update_status(host.lobby, host.panel);
    expect(
        mp::panel_text(host.panel, "MEM1") == "Easy" && host.lobby.computer_difficulty[1] == 0 &&
            host.lobby.computer_difficulty_set[1],
        "the difficulty cycles to easy"
    );
    const auto side_before = mp::slot_info(host.lobby, 1)->side;
    const auto team_before = mp::lobby_player_team(seated);
    expect(host.press("SIDE1") && host.press("TEAMICONS1"), "side and team can be changed");
    expect(
        mp::slot_info(host.lobby, 1)->side != side_before &&
            mp::lobby_player_team(seated) != team_before,
        "the computer player's side and team changed"
    );
    expect(host.press("PLAYER2") && host.press("PLAYER2"), "a second computer player is added");
    expect(
        mp::slot_player(host.lobby, 2).status == mp::kSlotComputer,
        "the second computer player is seated"
    );
    clock_ticks += 0x1f;
    expect(host.press("PLAYER1"), "the first computer player can be removed");
    expect(
        seated.status == mp::kSlotOpen && seated.in_use == 0 &&
            mp::slot_player(host.lobby, 2).status == mp::kSlotComputer,
        "removing one computer player leaves the other"
    );

    Fixture mixed(lounge);
    mixed.loopback.next_player_id = 0x301;
    mixed.join(0x301, "Rival");
    mixed.remote_info(0x301, 0, 4);
    expect(
        !oa::netgame::sent_by_open_annihilation(
            reinterpret_cast<const uint8_t*>(mp::slot_info(mixed.lobby, 1))
        ),
        "the other player is not on OA"
    );
    expect(mixed.press("PLAYER2") && mixed.press("PLAYER2"), "a 3.1c peer does not block adding");
    expect(
        mp::slot_player(mixed.lobby, 2).status == mp::kSlotComputer,
        "a computer player is added while a 3.1c player is in the room"
    );

    Fixture remote(lounge);
    remote.join(0x401, "Friend");
    remote.remote_info(0x401, 0, 5);
    oa::netgame::mark_engine_signature(reinterpret_cast<uint8_t*>(mp::slot_info(remote.lobby, 1)));
    remote.join(0x402, "AI:Friend");
    const auto computer = mp::slot_for_player_id(remote.lobby, 0x402);
    expect(computer >= 0, "the remote computer player is seated");
    mp::slot_info(remote.lobby, computer)->state = mp::kInfoStateDefeated;
    mp::slot_player(remote.lobby, 1).machine_group = 4;
    mp::slot_player(remote.lobby, computer).machine_group = 4;
    mp::lobby_update_status(remote.lobby, remote.panel);
    char mem_name[16];
    std::snprintf(mem_name, sizeof mem_name, "MEM%d", static_cast<int>(computer));
    const auto* remote_mem = mp::panel_control(remote.panel, mem_name);
    expect(
        remote_mem != nullptr && remote_mem->type == mp::ControlType::button && !remote_mem->grayed,
        "the host can change another OA machine's computer player"
    );
    expect(remote.press(mem_name), "the host sets that computer player's difficulty");
    expect(
        remote.lobby.computer_command_kind == 4 && remote.lobby.computer_command_player == 0x402 &&
            remote.lobby.computer_command_value == 0,
        "the change waits for the machine that runs it"
    );
    const auto source = small_presence("0.8.0");
    bind_presence_source(remote.lobby, source);
    mp::presence_refresh(remote.lobby);
    std::span<const uint8_t> command;
    expect(
        oa::netgame::find_presence_field(
            remote.lobby.presence_records.local,
            remote.lobby.presence_records.local_size,
            oa::netgame::PresenceTag::computer_edit,
            &command
        ) && command.size() >= 6 &&
            command[4] == 4 && command[5] == 0,
        "the host's presence record carries the change"
    );

    Fixture owner(lounge, nullptr, false);
    owner.loopback.next_player_id = 0x210;
    owner.join(0x200, "Boss");
    mp::slot_info(owner.lobby, mp::slot_for_player_id(owner.lobby, 0x200))->role = mp::kRoleHost;
    expect(owner.press("PLAYER2"), "the joiner seats its own computer player");
    const auto owned = arrive_computer(owner);
    expect(owned != 0 && mp::slot_player(owner.lobby, 2).status == mp::kSlotComputer, "it arrived");
    uint8_t body[6] = {
        static_cast<uint8_t>(owned),
        static_cast<uint8_t>(owned >> 8),
        static_cast<uint8_t>(owned >> 16),
        static_cast<uint8_t>(owned >> 24),
        4,
        0
    };
    auto asked = small_presence("0.8.0");
    asked.resize(asked.size() + 16);
    std::size_t written = 0;
    expect(
        oa::netgame::append_presence_field(
            asked.data(),
            asked.size() - 16,
            asked.size(),
            oa::netgame::PresenceTag::computer_edit,
            std::span<const uint8_t>(body, sizeof body),
            &written
        ) == oa::netgame::WireError::ok,
        "a host command encodes"
    );
    asked.resize(written);
    const auto event = presence_event(0x200, asked);
    expect(mp::lobby_apply_event(owner.lobby, event), "the host's presence record is kept");
    expect(
        owner.lobby.computer_difficulty_set[2] && owner.lobby.computer_difficulty[2] == 0,
        "the machine that runs the computer player applies the difficulty"
    );

    Fixture plain(lounge);
    plain.join(0x501, "Old");
    plain.remote_info(0x501, 0, 6);
    plain.join(0x502, "AI:Old");
    const auto plain_computer = mp::slot_for_player_id(plain.lobby, 0x502);
    expect(plain_computer >= 0, "a 3.1c machine's computer player is seated");
    mp::slot_info(plain.lobby, plain_computer)->state = mp::kInfoStateDefeated;
    mp::slot_player(plain.lobby, 1).machine_group = 5;
    mp::slot_player(plain.lobby, plain_computer).machine_group = 5;
    mp::lobby_update_status(plain.lobby, plain.panel);
    char plain_mem[16];
    std::snprintf(plain_mem, sizeof plain_mem, "MEM%d", static_cast<int>(plain_computer));
    const auto* plain_control = mp::panel_control(plain.panel, plain_mem);
    expect(
        plain_control != nullptr && plain_control->type == mp::ControlType::label &&
            plain_control->grayed && !plain.press(plain_mem),
        "a 3.1c machine's computer player is not restyled"
    );
    char plain_row[16];
    std::snprintf(plain_row, sizeof plain_row, "PLAYER%d", static_cast<int>(plain_computer));
    expect(plain.press(plain_row), "the host can remove it");
    expect(
        plain.lobby.confirm_slot == static_cast<uint8_t>(plain_computer),
        "removing it asks, as for any remote player"
    );
}

/// Stores a little-endian player id.
///
/// @param[out] out four bytes
/// @param id the player id
void store_player_id(uint8_t* out, uint32_t id) {
    out[0] = static_cast<uint8_t>(id);
    out[1] = static_cast<uint8_t>(id >> 8);
    out[2] = static_cast<uint8_t>(id >> 16);
    out[3] = static_cast<uint8_t>(id >> 24);
}

/// One brain the battle room's tests seat.
mp::ComputerBrain steady_brain() {
    mp::ComputerBrain brain;
    brain.id = "steady-commander";
    brain.name = "Steady Commander";
    brain.version = "1.2";
    brain.level_id = "firm";
    brain.level = "Firm";
    return brain;
}

/// Appends a field to a presence record the test is building.
///
/// @param[in,out] record the record, grown to hold the field
/// @param tag the field
/// @param value the field's value
/// @return true when the field was added
bool append_field(
    std::vector<uint8_t>& record, oa::netgame::PresenceTag tag, std::span<const uint8_t> value
) {
    record.resize(record.size() + oa::netgame::presence_field_header_bytes + value.size());
    std::size_t written = 0;
    const auto error = oa::netgame::append_presence_field(
        record.data(),
        record.size() - oa::netgame::presence_field_header_bytes - value.size(),
        record.size(),
        tag,
        value,
        &written
    );
    if (error != oa::netgame::WireError::ok)
        return false;
    record.resize(written);
    return true;
}

/// A brain seat and a built-in seat share one record; a peer's brain greys
/// its row; a swap and a departure carry the brain; no brain adds no bytes.
void test_computer_brains(const oa::ui::gui_layout::Layout& lounge) {
    Fixture host(lounge);
    host.loopback.next_player_id = 0x111;
    expect(
        host.press("PLAYER1") && host.press("PLAYER1"), "the first computer player is asked for"
    );
    const auto first = arrive_computer(host);
    expect(
        host.press("PLAYER2") && host.press("PLAYER2"), "the second computer player is asked for"
    );
    const auto second = arrive_computer(host);
    expect(
        first != 0 && second != 0 && mp::slot_player(host.lobby, 1).status == mp::kSlotComputer &&
            mp::slot_player(host.lobby, 2).status == mp::kSlotComputer,
        "both computer players are seated"
    );
    const auto source = small_presence("0.8.0");
    bind_presence_source(host.lobby, source);
    mp::presence_refresh(host.lobby);
    uint8_t listed[10];
    store_player_id(listed, first);
    listed[4] = 2;
    store_player_id(listed + 5, second);
    listed[9] = 2;
    auto expected = source;
    expect(
        append_field(
            expected, oa::netgame::PresenceTag::computers, std::span<const uint8_t>(listed, 10)
        ),
        "the computer list the room used to send encodes"
    );
    const auto& plain = host.lobby.presence_records;
    std::span<const uint8_t> brains_field;
    expect(
        plain.local_size == expected.size() &&
            std::equal(expected.begin(), expected.end(), plain.local) &&
            !oa::netgame::find_presence_field(
                plain.local,
                plain.local_size,
                oa::netgame::PresenceTag::computer_brains,
                &brains_field
            ),
        "a room with no brain composes the same bytes as before"
    );

    mp::lobby_set_computer_brain(host.lobby, 1, steady_brain(), 2);
    mp::lobby_set_computer_brain(host.lobby, 0, steady_brain(), 0);
    const auto* seated = mp::lobby_computer_brain(host.lobby, 1);
    expect(
        seated != nullptr && seated->id == "steady-commander" && seated->level == "Firm" &&
            mp::lobby_computer_brain(host.lobby, 2) == nullptr &&
            mp::lobby_computer_brain(host.lobby, 0) == nullptr &&
            host.lobby.computer_difficulty[1] == 2,
        "this machine's computer player keeps the brain and its difficulty"
    );
    mp::lobby_update_status(host.lobby, host.panel);
    const auto* own_mem = mp::panel_control(host.panel, "MEM1");
    expect(
        own_mem != nullptr && own_mem->type == mp::ControlType::label && own_mem->grayed &&
            mp::panel_text(host.panel, "MEM1") == "Hard",
        "a brain seat's difficulty is a greyed label"
    );
    mp::presence_refresh(host.lobby);
    expect(
        oa::netgame::find_presence_field(
            host.lobby.presence_records.local,
            host.lobby.presence_records.local_size,
            oa::netgame::PresenceTag::computer_brains,
            &brains_field
        ),
        "the record carries the brain seat"
    );
    const auto decoded = oa::netgame::decode_presence_brains(brains_field);
    std::span<const uint8_t> computers_field;
    expect(
        decoded && decoded->size() == 1 && (*decoded)[0].player == first &&
            (*decoded)[0].id == "steady-commander" && (*decoded)[0].version == "1.2" &&
            (*decoded)[0].level_id == "firm" &&
            oa::netgame::find_presence_field(
                host.lobby.presence_records.local,
                host.lobby.presence_records.local_size,
                oa::netgame::PresenceTag::computers,
                &computers_field
            ) &&
            computers_field.size() == 10,
        "one brain seat and one built-in seat share the record"
    );

    mp::lobby_swap_slots(host.lobby, 1, 2);
    const auto* swapped = mp::lobby_computer_brain(host.lobby, 2);
    expect(
        swapped != nullptr && swapped->id == "steady-commander" &&
            mp::lobby_computer_brain(host.lobby, 1) == nullptr &&
            mp::slot_player(host.lobby, 2).player_id == first,
        "a slot swap carries the brain"
    );
    clock_ticks += 0x1f;
    expect(host.press("PLAYER2"), "the computer player that plays the brain can leave");
    expect(
        mp::slot_player(host.lobby, 2).status == mp::kSlotOpen &&
            mp::lobby_computer_brain(host.lobby, 2) == nullptr,
        "a computer player that leaves clears its brain"
    );

    Fixture remote(lounge);
    remote.join(0x401, "Friend");
    remote.remote_info(0x401, 0, 5);
    oa::netgame::mark_engine_signature(reinterpret_cast<uint8_t*>(mp::slot_info(remote.lobby, 1)));
    remote.join(0x402, "AI:Friend");
    const auto computer = mp::slot_for_player_id(remote.lobby, 0x402);
    expect(computer >= 0, "the remote computer player is seated");
    mp::slot_info(remote.lobby, computer)->state = mp::kInfoStateDefeated;
    mp::slot_player(remote.lobby, 1).machine_group = 4;
    mp::slot_player(remote.lobby, computer).machine_group = 4;
    oa::netgame::PresenceBrain announced;
    announced.player = 0x402;
    announced.id = "lantern-scout";
    announced.name = "Lantern Scout";
    announced.version = "1.0";
    announced.level_id = "wide";
    announced.level = "Wide";
    announced.full_awareness = true;
    uint8_t brain_value[256];
    std::size_t brain_written = 0;
    std::size_t brain_sent = 0;
    uint8_t remote_listed[5];
    store_player_id(remote_listed, 0x402);
    remote_listed[4] = 2;
    auto peer = small_presence("0.8.0");
    expect(
        oa::netgame::encode_presence_brains(
            std::span<const oa::netgame::PresenceBrain>(&announced, 1),
            brain_value,
            sizeof brain_value,
            &brain_written,
            &brain_sent
        ) == oa::netgame::WireError::ok &&
            brain_sent == 1 &&
            append_field(
                peer,
                oa::netgame::PresenceTag::computers,
                std::span<const uint8_t>(remote_listed, 5)
            ) &&
            append_field(
                peer,
                oa::netgame::PresenceTag::computer_brains,
                std::span<const uint8_t>(brain_value, brain_written)
            ),
        "the peer's brain seat encodes"
    );
    expect(
        mp::lobby_apply_event(remote.lobby, presence_event(0x401, peer)),
        "the peer's presence record is kept"
    );
    const auto* remote_brain = mp::lobby_computer_brain(remote.lobby, computer);
    expect(
        remote_brain != nullptr && remote_brain->id == "lantern-scout" &&
            remote_brain->full_awareness && remote.lobby.computer_difficulty[computer] == 2,
        "a peer's record sets that remote slot's brain"
    );
    mp::lobby_update_status(remote.lobby, remote.panel);
    char mem_name[16];
    std::snprintf(mem_name, sizeof mem_name, "MEM%d", static_cast<int>(computer));
    const auto* remote_mem = mp::panel_control(remote.panel, mem_name);
    expect(
        remote_mem != nullptr && remote_mem->type == mp::ControlType::label && remote_mem->grayed &&
            mp::panel_text(remote.panel, mem_name) == "Hard" && !remote.press(mem_name),
        "the host sees a greyed difficulty and cannot cycle it"
    );
    expect(
        mp::lobby_apply_event(remote.lobby, presence_event(0x401, small_presence("0.8.0"))),
        "a later record with no brain seat is kept"
    );
    expect(
        mp::lobby_computer_brain(remote.lobby, computer) == nullptr,
        "a peer that no longer lists the brain clears it"
    );
}

// The battle room sends this machine's presence record alone in its frame,
// to each human player whose block carries the 'OA' signature, once for each
// change and once to a player who arrives later; never to a block without
// the signature, a computer player or everyone at once. The host's record
// names its map's pack, asked once for each map; a joiner's names none. A
// record another OA player sent is kept as long as that player stays.
void test_presence_records_in_the_battle_room() {
    namespace ng = oa::netgame;
    using ng::RecordType;
    constexpr uint32_t kLater = 0x400;
    const auto saved_map = map_name;
    map_name = "Coast To Coast";
    auto room = std::make_unique<Room>(true);
    auto& lobby = room->lobby;
    auto& records = lobby.presence_records;
    const auto& sent = room->loopback;
    lobby.next_stats_tick = clock_ticks + kNoStatusTicks; // no periodic block
    const auto first = small_presence("0.8.0-dev");
    bind_presence_source(lobby, first);
    mp::presence_refresh(lobby);
    expect(
        records.local_size == first.size() &&
            std::equal(first.begin(), first.end(), records.local) && records.local_generation == 1,
        "the host's record is its source while no pack is bound"
    );
    const auto own = mp::lobby_presence_record(lobby, 0);
    const auto computer = mp::lobby_presence_record(lobby, 2);
    expect(
        own.size() == first.size() && own.data() == records.local &&
            computer.data() == records.local,
        "the host's slot and its computer player's answer with the record it sends"
    );

    // No record before a block says OA: none without the signature, none
    // with only its first byte, none to another machine's computer player.
    (void)mp::lobby_tick(lobby, room->panel);
    expect(
        mp::lobby_apply_event(lobby, record_event(kRoomGuest, setup_block(kRoomGuest, 1, 0))),
        "a block without the signature applies"
    );
    (void)mp::lobby_tick(lobby, room->panel);
    expect(
        mp::lobby_apply_event(lobby, record_event(kRoomGuest, setup_block(kRoomGuest, 1, 1))),
        "a block with only 'O' applies"
    );
    (void)mp::lobby_tick(lobby, room->panel);
    join_remote(lobby, kRoomThird);
    const auto third_slot = mp::slot_for_player_id(lobby, kRoomThird);
    // Each remote player plays on a machine of its own.
    mp::slot_player(lobby, 1).machine_group = 2;
    mp::slot_player(lobby, third_slot).machine_group = 3;
    expect(
        mp::lobby_apply_event(lobby, record_event(kRoomThird, setup_block(kRoomThird, 2, 2))),
        "another machine's computer player's block applies"
    );
    (void)mp::lobby_tick(lobby, room->panel);
    expect(sent_presence(*room).empty(), "no record to a block without 'OA' or a computer player");
    expect(blocks_without_presence(*room), "the blocks sent carry no presence");

    // The guest's block says OA: one record, alone, from the host's player.
    room->loopback.sent_count = 0;
    expect(
        mp::lobby_apply_event(lobby, record_event(kRoomGuest, setup_block(kRoomGuest, 1, 2))),
        "the OA guest's block applies"
    );
    auto presence = sent_presence(*room);
    expect(presence.size() == 1, "exactly one record goes out");
    if (presence.size() == 1) {
        const auto index = presence[0];
        expect(
            sent.sent_to[index] == kRoomGuest && sent.sent_from[index] == kRoomHost,
            "to the guest, from the host's player"
        );
        expect(sent_alone(*room, index), "alone in its frame");
        expect(sent_is(*room, index, records.local, records.local_size), "holding the record");
    }
    (void)mp::lobby_apply_event(lobby, record_event(kRoomGuest, setup_block(kRoomGuest, 1, 2)));
    for (int32_t frame = 0; frame < 3; ++frame)
        (void)mp::lobby_tick(lobby, room->panel);
    expect(sent_presence(*room).size() == 1, "the block again, and the frames after, send nothing");

    // A changed source goes once more to the OA guest.
    room->loopback.sent_count = 0;
    bind_presence_source(lobby, small_presence("0.8.1-dev"));
    (void)mp::lobby_tick(lobby, room->panel);
    (void)mp::lobby_tick(lobby, room->panel);
    presence = sent_presence(*room);
    expect(
        presence.size() == 1 && sent.sent_to[presence[0]] == kRoomGuest &&
            sent_alone(*room, presence[0]) &&
            sent_is(*room, presence[0], records.local, records.local_size) &&
            records.local_generation == 2,
        "a changed record goes once to the OA guest"
    );

    // The host's pack: asked once for a map, and once more for another.
    room->loopback.sent_count = 0;
    pack_asks = 0;
    test_pack = ng::PresenceMapPack{};
    test_pack.key = "archipelago";
    test_pack.release = 3;
    test_pack.sha256.fill(0xa5);
    test_pack.registry = "example";
    test_pack.descriptor_url = "http://registry.example/registry.yaml";
    test_pack.name = "Archipelago";
    test_pack.version = "1.2";
    lobby.maps.pack_context = nullptr;
    lobby.maps.pack = count_pack;
    for (int32_t frame = 0; frame < 10; ++frame)
        (void)mp::lobby_tick(lobby, room->panel);
    expect(pack_asks == 1, "ten frames on one map ask the pack once");
    const auto source_size = records.source_size;
    std::span<const uint8_t> pack_value;
    expect(
        records.local_size > source_size && records.local[source_size] == 0x40 &&
            ng::find_presence_field(
                records.local, records.local_size, ng::PresenceTag::map_pack, &pack_value
            ) &&
            source_size + ng::presence_field_header_bytes + pack_value.size() == records.local_size,
        "the host's record ends with the map-pack field"
    );
    presence = sent_presence(*room);
    expect(
        presence.size() == 1 && sent.sent_to[presence[0]] == kRoomGuest &&
            sent_is(*room, presence[0], records.local, records.local_size),
        "and goes again"
    );
    map_name = "Lava Run";
    (void)mp::lobby_tick(lobby, room->panel);
    (void)mp::lobby_tick(lobby, room->panel);
    expect(pack_asks == 2, "another map asks once more");

    // A URL of 300 bytes goes empty; the rest of the pack goes as it is.
    test_pack.descriptor_url = std::string(300, 'u');
    map_name = "Coast To Coast";
    (void)mp::lobby_tick(lobby, room->panel);
    ng::PresenceRecord decoded;
    expect(
        ng::decode_presence(records.local, records.local_size, &decoded) == ng::WireError::ok &&
            decoded.map_pack && decoded.map_pack->descriptor_url.empty() &&
            decoded.map_pack->key == "archipelago" && decoded.map_pack->release == 3,
        "a pack's URL of 300 bytes goes empty"
    );

    // A source written within the reserve, its lists filling it, still
    // takes a pack with every string at its longest.
    ng::PresenceRecord full;
    full.engine = ng::PresenceEngine{"0.8.0-dev", "macOS", "arm64"};
    full.game_hacks = ng::PresenceHacks{40, {}};
    full.game_hacks->ids.assign(40, std::string(40, 'g'));
    const auto reserved = encoded_presence(
        full, ng::presence_record_max_bytes - ng::presence_map_pack_field_max_bytes
    );
    expect(
        reserved.size() + 45 >
            ng::presence_record_max_bytes - ng::presence_map_pack_field_max_bytes,
        "the lists fill the reserve"
    );
    bind_presence_source(lobby, reserved);
    test_pack.key = std::string(64, 'k');
    test_pack.registry = std::string(32, 'r');
    test_pack.descriptor_url = std::string(255, 'u');
    test_pack.name = std::string(64, 'n');
    test_pack.version = std::string(32, 'v');
    map_name = "Lava Run";
    (void)mp::lobby_tick(lobby, room->panel);
    decoded = {};
    expect(
        records.local_size == reserved.size() + ng::presence_map_pack_field_max_bytes &&
            ng::decode_presence(records.local, records.local_size, &decoded) == ng::WireError::ok &&
            decoded.map_pack && decoded.map_pack->key == test_pack.key &&
            decoded.map_pack->descriptor_url == test_pack.descriptor_url &&
            decoded.map_pack->name == test_pack.name &&
            decoded.map_pack->version == test_pack.version,
        "a full source still takes the longest pack"
    );

    // Records the guest sends: kept for its slot; one from an unseated
    // sender, or whose length word is not its size, is not.
    const auto guest_record = small_presence("0.7.9");
    expect(
        mp::lobby_apply_event(lobby, presence_event(kRoomGuest, guest_record)),
        "the guest's record is kept"
    );
    const auto kept = mp::lobby_presence_record(lobby, 1);
    expect(
        std::equal(kept.begin(), kept.end(), guest_record.begin(), guest_record.end()),
        "and answers for its slot"
    );
    expect(
        !mp::lobby_apply_event(lobby, presence_event(0x999, guest_record)),
        "a record from an unseated sender is ignored"
    );
    auto long_word = small_presence("0.7.8");
    long_word.push_back(0);
    expect(
        !mp::lobby_apply_event(lobby, presence_event(kRoomGuest, long_word)),
        "a record whose length word is not its size is ignored"
    );
    const auto still = mp::lobby_presence_record(lobby, 1);
    expect(
        std::equal(still.begin(), still.end(), guest_record.begin(), guest_record.end()),
        "and leaves the kept record as it was"
    );

    // The guest leaves: its record goes, and a new OA player in its slot
    // gets this machine's record once its own block says OA.
    mp::LobbyEvent left{};
    left.kind = mp::LobbyEventKind::player_left;
    left.player_id = kRoomGuest;
    (void)mp::lobby_apply_event(lobby, left);
    expect(
        mp::lobby_presence_record(lobby, 1).empty() && records.peer_size[1] == 0,
        "a departed player's record is forgotten"
    );
    room->loopback.sent_count = 0;
    join_remote(lobby, kLater);
    const auto later_slot = mp::slot_for_player_id(lobby, kLater);
    expect(later_slot == 1, "the new player takes the slot");
    (void)mp::lobby_tick(lobby, room->panel);
    expect(
        sent_presence(*room).empty() && mp::lobby_presence_record(lobby, later_slot).empty(),
        "nothing goes to it, and nothing shows for it, before its own block"
    );
    expect(
        mp::lobby_apply_event(lobby, record_event(kLater, setup_block(kLater, 1, 2))),
        "its block applies"
    );
    presence = sent_presence(*room);
    expect(
        presence.size() == 1 && sent.sent_to[presence[0]] == kLater &&
            sent_alone(*room, presence[0]),
        "the new OA player gets the record"
    );
    expect(blocks_without_presence(*room), "the blocks sent still carry no presence");

    // With machines shared, each OA human still gets its own, by id.
    expect(
        mp::lobby_apply_event(lobby, record_event(kRoomThird, setup_block(kRoomThird, 1, 2))),
        "the third player's block now says a human"
    );
    mp::slot_player(lobby, 0).machine_group = 1;
    mp::slot_player(lobby, 2).machine_group = 1;
    mp::slot_player(lobby, later_slot).machine_group = 2;
    mp::slot_player(lobby, third_slot).machine_group = 3;
    room->game->shared_machines = 1;
    room->loopback.sent_count = 0;
    bind_presence_source(lobby, small_presence("0.8.2-dev"));
    (void)mp::lobby_tick(lobby, room->panel);
    presence = sent_presence(*room);
    bool by_id = presence.size() == 2 && room->game->shared_machines != 0;
    for (const auto index : presence)
        by_id = by_id && sent.sent_to[index] != 0 && sent_alone(*room, index);
    expect(
        by_id && presence.size() == 2 &&
            ((sent.sent_to[presence[0]] == kLater && sent.sent_to[presence[1]] == kRoomThird) ||
             (sent.sent_to[presence[0]] == kRoomThird && sent.sent_to[presence[1]] == kLater)),
        "with machines shared each OA human gets the record by id, never everyone"
    );
    mp::lobby_send_player_info(lobby);
    expect(blocks_without_presence(*room), "no block carries presence");

    // A joiner's record names no pack, and the joiner asks none.
    auto joiner = std::make_unique<Room>(false);
    joiner->lobby.next_stats_tick = clock_ticks + kNoStatusTicks;
    bind_presence_source(joiner->lobby, first);
    joiner->lobby.maps.pack = count_pack;
    pack_asks = 0;
    (void)mp::lobby_tick(joiner->lobby, joiner->panel);
    expect(
        mp::lobby_apply_event(joiner->lobby, record_event(kRoomHost, setup_block(kRoomHost, 1, 2))),
        "the host's block applies"
    );
    (void)mp::lobby_tick(joiner->lobby, joiner->panel);
    presence = sent_presence(*joiner);
    std::span<const uint8_t> no_pack;
    expect(
        presence.size() == 1 && joiner->loopback.sent_to[presence[0]] == kRoomHost &&
            joiner->loopback.sent_from[presence[0]] == kRoomGuest &&
            sent_is(*joiner, presence[0], first.data(), first.size()) &&
            !ng::find_presence_field(
                joiner->loopback.sent[presence[0]],
                joiner->loopback.sent_size[presence[0]],
                ng::PresenceTag::map_pack,
                &no_pack
            ) &&
            pack_asks == 0,
        "a joiner sends its record to the host with no map pack, and asks none"
    );
    expect(blocks_without_presence(*joiner), "the joiner's blocks carry no presence");
    map_name = saved_map;
}

/// Returns a presence record with an engine line and a sim hash.
///
/// @param hash the sim hash
/// @return the record
std::vector<uint8_t> presence_with_hash(const oa::netgame::PresenceDigest& hash) {
    oa::netgame::PresenceRecord record;
    record.engine = oa::netgame::PresenceEngine{"0.8.0", "macOS", "arm64"};
    record.sim_hash = hash;
    return encoded_presence(record);
}

/// Returns a presence record with an engine line and no sim hash.
///
/// @return the record
std::vector<uint8_t> presence_without_hash() {
    oa::netgame::PresenceRecord record;
    record.engine = oa::netgame::PresenceEngine{"0.8.0", "macOS", "arm64"};
    return encoded_presence(record);
}

/// Returns a sim hash of 32 equal bytes.
///
/// @param value each byte
/// @return the hash
oa::netgame::PresenceDigest filled_hash(uint8_t value) {
    oa::netgame::PresenceDigest hash{};
    hash.fill(value);
    return hash;
}

/// Tells whether a row's OA badge carries the dot, checking that the row's
/// badge and the rule agree.
///
/// @param lobby the lobby
/// @param slot the row's slot
/// @return true when the dot shows
bool shows_dot(mp::Lobby& lobby, int32_t slot) {
    const bool differ = mp::lobby_rules_differ_from_host(lobby, slot);
    expect(
        mp::lobby_row_badge(lobby, slot).rules_differ == differ,
        "the row's badge carries the dot the rule gives"
    );
    return differ;
}

// The dot on an OA badge says that the row's player plays by other rules
// than the host's, from the presence records alone: the two sim hashes when
// both sent one, the row's against the plain baseline's under a 3.1c host,
// and nothing when no record tells. The host's own row never carries it.
void test_rules_dot() {
    namespace profiles = oa::data::mod_profile;
    {
        auto room = std::make_unique<Room>(true);
        auto& lobby = room->lobby;
        expect(
            mp::lobby_apply_event(lobby, record_event(kRoomGuest, setup_block(kRoomGuest, 1, 2))),
            "the OA guest's block applies"
        );
        expect(
            mp::lobby_row_badge(lobby, 0).badge == mp::RowBadge::open_annihilation &&
                mp::lobby_row_badge(lobby, 1).badge == mp::RowBadge::open_annihilation,
            "the host and the OA guest have the OA badge"
        );
        expect(
            mp::lobby_row_badge(lobby, 2).badge == mp::RowBadge::none && !shows_dot(lobby, 2),
            "this machine's computer player has no badge and no dot"
        );
        expect(
            !shows_dot(lobby, 0) && !shows_dot(lobby, 1),
            "with no records on either side there is no dot"
        );
        bind_presence_source(lobby, presence_with_hash(filled_hash(0x5a)));
        mp::presence_refresh(lobby);
        expect(!shows_dot(lobby, 1), "the host's record alone tells nothing");
        expect(
            mp::lobby_apply_event(
                lobby, presence_event(kRoomGuest, presence_with_hash(filled_hash(0x5a)))
            ),
            "the guest's record is kept"
        );
        expect(!shows_dot(lobby, 0) && !shows_dot(lobby, 1), "equal sim hashes give no dot");
        expect(
            mp::lobby_apply_event(
                lobby, presence_event(kRoomGuest, presence_with_hash(filled_hash(0x77)))
            ),
            "the guest's other record is kept"
        );
        expect(shows_dot(lobby, 1), "different sim hashes put the dot on the guest's row");
        expect(!shows_dot(lobby, 0), "and never on the host's own row");
        expect(
            mp::lobby_apply_event(lobby, presence_event(kRoomGuest, presence_without_hash())),
            "the guest's record without a sim hash is kept"
        );
        expect(!shows_dot(lobby, 1), "a record without a sim hash tells nothing");
    }

    {
        const std::string text = profiles::base_game_profile_text();
        const auto resolved = profiles::resolve_profile(
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size()),
            profiles::base_game_id
        );
        expect(resolved.resolution.has_value(), "the plain baseline resolves");
        if (!resolved.resolution)
            return;
        const auto plain = resolved.resolution->profile.sim_hash;
        auto room = std::make_unique<Room>(false);
        auto& lobby = room->lobby;
        expect(
            lobby.presence_records.plain_sim_hash == plain,
            "the battle room holds the plain baseline's sim hash"
        );
        auto* host = mp::slot_info(lobby, 1);
        host->state = mp::kInfoStatePlaying;
        host->status = static_cast<uint16_t>(host->status | mp::status::has_disc);
        host->engine_signature[0] = 0;
        host->engine_signature[1] = 0;
        expect(mp::lobby_host_slot(lobby) == 1, "the other machine's player hosts");
        expect(
            mp::lobby_row_badge(lobby, 0).badge == mp::RowBadge::open_annihilation,
            "the local OA joiner has the OA badge"
        );
        expect(
            mp::lobby_row_badge(lobby, 1).badge == mp::RowBadge::disc,
            "the 3.1c host has the CD icon"
        );
        expect(!shows_dot(lobby, 0), "with no records on either side there is no dot");
        bind_presence_source(lobby, presence_with_hash(plain));
        mp::presence_refresh(lobby);
        expect(!shows_dot(lobby, 0), "a joiner on the plain baseline's rules has no dot");
        bind_presence_source(lobby, presence_with_hash(filled_hash(0x77)));
        mp::presence_refresh(lobby);
        expect(shows_dot(lobby, 0), "a joiner on other rules has the dot under a 3.1c host");
        expect(!shows_dot(lobby, 1), "the host's row never has it");
        bind_presence_source(lobby, presence_without_hash());
        mp::presence_refresh(lobby);
        expect(!shows_dot(lobby, 0), "a joiner's record without a sim hash tells nothing");
        bind_presence_source(lobby, presence_with_hash(filled_hash(0x77)));
        mp::presence_refresh(lobby);
        host->engine_signature[0] = oa::netgame::engine_signature_first;
        host->engine_signature[1] = oa::netgame::engine_signature_second;
        expect(
            mp::lobby_row_badge(lobby, 1).badge == mp::RowBadge::open_annihilation &&
                !shows_dot(lobby, 0),
            "an OA host that sent no record tells nothing"
        );
    }
}

// A guest that lacks one mutator of the host's list carries the rules dot
// through the mutator comparison, while the sim hashes stay equal. A guest
// that has every one does not, and the host's own row never does.
void test_mutator_rules_dot() {
    namespace ng = oa::netgame;
    ng::PresenceMutators ready;
    ready.built = true;
    ready.index = ng::presence_mutator_index_none;
    ready.total = 2;
    ready.held = 2;
    ready.digest.fill(0x11);
    ready.layer_digest.fill(0x22);
    const auto encode = [](const ng::PresenceMutators& field) {
        ng::PresenceRecord record;
        record.engine = ng::PresenceEngine{"0.8.0", "macOS", "arm64"};
        record.mutators = field;
        record.sim_hash = filled_hash(0x5a);
        return encoded_presence(record);
    };

    auto room = std::make_unique<Room>(true);
    auto& lobby = room->lobby;
    expect(
        mp::lobby_apply_event(lobby, record_event(kRoomGuest, setup_block(kRoomGuest, 1, 2))),
        "the OA guest's block applies"
    );
    bind_presence_source(lobby, encode(ready));
    mp::presence_refresh(lobby);

    ng::PresenceMutators missing = ready;
    ng::presence_mutator_set_state(missing.states, 0, 1);
    expect(
        mp::lobby_apply_event(lobby, presence_event(kRoomGuest, encode(missing))),
        "the guest's record that lacks one mutator is kept"
    );
    expect(!mp::lobby_sim_hash_differs_from_host(lobby, 1), "equal sim hashes do not differ");
    expect(
        mp::lobby_mutators_differ_from_host(lobby, 1),
        "a missing mutator differs through the mutator rule"
    );
    expect(shows_dot(lobby, 1), "a missing mutator puts the dot on the guest");
    expect(!shows_dot(lobby, 0), "the host's own row has no dot");
    expect(!mp::lobby_mutators_differ_from_host(lobby, 0), "the host does not differ from itself");

    expect(
        mp::lobby_apply_event(lobby, presence_event(kRoomGuest, encode(ready))),
        "the guest's record that has every mutator is kept"
    );
    expect(
        !mp::lobby_mutators_differ_from_host(lobby, 1), "a guest that has them all does not differ"
    );
    expect(!shows_dot(lobby, 1), "a guest that has them all has no dot");
    expect(!shows_dot(lobby, 0), "the host's row still has no dot");
}

/// A presence record that names a base game and expansions.
///
/// @param hash the sim hash
/// @param expansions the expansion ids
/// @param differs a chosen bundle differs from a known hash
/// @param base the base game
/// @return the record
oa::netgame::PresenceRecord content_presence(
    const oa::netgame::PresenceDigest& hash,
    std::vector<std::string> expansions,
    bool differs = false,
    const char* base = "totala"
) {
    oa::netgame::PresenceRecord record;
    record.engine = oa::netgame::PresenceEngine{"0.8.0", "macOS", "arm64"};
    record.sim_hash = hash;
    record.bundles = oa::netgame::PresenceBundles{base, std::move(expansions), differs};
    return record;
}

// Equal sim hashes, and the bundles field: a missing expansion dots the
// guest and refuses the start; music does not; a record that omits the
// field does not; a 3.1c guest is still outside the comparison.
void test_content_difference() {
    const auto hash = filled_hash(0x5a);
    auto room = std::make_unique<Room>(true);
    auto& lobby = room->lobby;
    expect(
        mp::lobby_apply_event(lobby, record_event(kRoomGuest, setup_block(kRoomGuest, 1, 2))),
        "the OA guest's block applies"
    );
    bind_presence_source(
        lobby, encoded_presence(content_presence(hash, {"campaign", "battle-tactics"}))
    );
    mp::presence_refresh(lobby);
    expect(
        mp::lobby_apply_event(
            lobby,
            presence_event(kRoomGuest, encoded_presence(content_presence(hash, {"campaign"})))
        ),
        "the guest's record is kept"
    );
    expect(shows_dot(lobby, 1), "a missing expansion puts the dot on the guest");
    expect(!shows_dot(lobby, 0), "the host's row has no dot");
    expect(!mp::lobby_sim_hash_differs_from_host(lobby, 1), "the sim hashes still match");
    expect(!mp::lobby_content_allows_start(lobby), "the host cannot start");

    expect(
        mp::lobby_apply_event(
            lobby,
            presence_event(
                kRoomGuest,
                encoded_presence(content_presence(hash, {"campaign", "battle-tactics", "music"}))
            )
        ),
        "music on the wire is kept"
    );
    expect(!shows_dot(lobby, 1), "music alone does not differ");
    expect(mp::lobby_content_allows_start(lobby), "music does not refuse the start");

    expect(
        mp::lobby_apply_event(
            lobby,
            presence_event(
                kRoomGuest,
                encoded_presence(content_presence(hash, {"campaign", "battle-tactics"}, true))
            )
        ),
        "a hash flag is kept"
    );
    expect(
        shows_dot(lobby, 1) && !mp::lobby_content_allows_start(lobby),
        "a file that does not match refuses the start"
    );

    expect(
        mp::lobby_apply_event(lobby, presence_event(kRoomGuest, presence_with_hash(hash))),
        "a record without bundles is kept"
    );
    expect(
        !shows_dot(lobby, 1) && mp::lobby_content_allows_start(lobby),
        "a record that does not name content does not refuse the start"
    );

    auto* info = mp::slot_info(lobby, 1);
    info->engine_signature[0] = 0;
    info->engine_signature[1] = 0;
    info->status = static_cast<uint16_t>(info->status | mp::status::has_disc);
    bind_presence_source(
        lobby, encoded_presence(content_presence(hash, {"campaign", "battle-tactics"}))
    );
    mp::presence_refresh(lobby);
    expect(mp::lobby_row_badge(lobby, 1).badge == mp::RowBadge::disc, "the guest is on 3.1c");
    expect(
        !shows_dot(lobby, 1) && mp::lobby_content_allows_start(lobby),
        "a 3.1c player is not compared by expansions"
    );

    Room joiner(false);
    expect(
        mp::lobby_content_allows_start(joiner.lobby),
        "a player who is not the host is not refused by this gate"
    );
}

// The battle room with Unicode chat on: the blocks it sends say so, and a
// line goes to the machine whose block says UTF-8 in UTF-8 and to the one
// whose block does not in the code page, '?' for each hanzi. A line from a
// machine that says UTF-8 is read strictly. With the setting off, the line
// goes once to everyone as typed and the blocks keep their chat bytes zero.
void test_unicode_chat_in_the_battle_room() {
    using oa::netgame::RecordType;
    // U+4F60 U+597D in UTF-8.
    const std::string hanzi = "\xe4\xbd\xa0\xe5\xa5\xbd";
    constexpr uint32_t kEveryone = 0; // a broadcast's destination id
    for (const bool on : {true, false}) {
        Room host(true);
        host.lobby.unicode_chat = on;
        join_remote(host.lobby, kRoomThird);
        const auto guest_slot = mp::slot_for_player_id(host.lobby, kRoomGuest);
        oa::netgame::mark_unicode_chat(
            reinterpret_cast<uint8_t*>(mp::slot_info(host.lobby, guest_slot)), true
        );
        mp::lobby_send_player_info(host.lobby);
        const auto infos = host.sent(RecordType::player_info);
        bool announced = !infos.empty();
        for (const auto index : infos) {
            const auto* chat =
                host.loopback.sent[index] + 1 + oa::netgame::player_info_chat_signature_offset;
            announced = announced && chat[0] == (on ? 'U' : 0) && chat[1] == (on ? '8' : 0) &&
                        chat[2] == (on ? oa::netgame::chat_flag_utf8 : 0);
        }
        expect(announced, "the blocks say Unicode chat only while it is on");

        host.loopback.sent_count = 0;
        mp::lobby_say(host.lobby, mp::local_player(host.lobby), hanzi.c_str());
        const auto chats = host.sent(RecordType::chat);
        const auto text = [&](int32_t index) {
            const auto* bytes = reinterpret_cast<const char*>(host.loopback.sent[index] + 1);
            return std::string(bytes, ::strnlen(bytes, 64));
        };
        if (on) {
            expect(
                chats.size() == 2 && host.loopback.sent_to[chats[0]] == kRoomGuest &&
                    text(chats[0]) == "<Host> " + hanzi &&
                    host.loopback.sent_to[chats[1]] == kRoomThird && text(chats[1]) == "<Host> ??",
                "each machine gets the line in the form it reads"
            );
        } else {
            expect(
                chats.size() == 1 && host.loopback.sent_to[chats[0]] == kEveryone &&
                    text(chats[0]) == "<Host> " + hanzi,
                "with the setting off the line goes once, as typed"
            );
        }

        oa::netgame::ChatRecord malformed{};
        std::memcpy(malformed.text, "<Guest> \xc0\xaf\xe4\xbd", 12);
        (void)mp::lobby_apply_event(host.lobby, record_event(kRoomGuest, malformed));
        auto& game = *host.game;
        const auto head = mp::lobby_chat_head(game);
        const std::string shown =
            mp::lobby_chat_line(game, static_cast<std::size_t>(head + mp::kChatLines - 1));
        expect(
            shown == (on ? std::string("<Guest> ????") : std::string(malformed.text, 12)),
            "a line from a machine that says UTF-8 is read strictly"
        );
    }
}

// The host's recorder commands typed as chat change the options on every
// recorder; a guest's do not, and private lines are never shown.
void test_recorder_commands_in_the_battle_room() {
    Room guest(false);
    guest.lobby.wire_rules = versioned_recorder_rules();
    oa::netgame::ChatRecord line{};
    std::snprintf(line.text, sizeof(line.text), "%s", "<Host> .syncon 0 5");
    expect(mp::lobby_apply_event(guest.lobby, record_event(kRoomHost, line)), "the line shows");
    expect(
        guest.lobby.recorder.options.speed_lock == 1 &&
            guest.lobby.recorder.options.speed_high == 5,
        "the host's .syncon locks the speed"
    );
    std::snprintf(line.text, sizeof(line.text), "%s", "<Host> .autopause");
    (void)mp::lobby_apply_event(guest.lobby, record_event(kRoomHost, line));
    expect(guest.lobby.recorder.options.autopause == 1, "the host's .autopause applies");

    // Without the rules' speed lock .syncon is only chat, and the host's
    // options bring no lock.
    Room unlocked(false);
    unlocked.lobby.wire_rules = versioned_recorder_rules();
    unlocked.lobby.wire_rules.speed_lock = false;
    std::snprintf(line.text, sizeof(line.text), "%s", "<Host> .syncon 0 5");
    expect(mp::lobby_apply_event(unlocked.lobby, record_event(kRoomHost, line)), "the line shows");
    expect(
        unlocked.lobby.recorder.options.speed_lock == 0,
        "no speed lock without the rules' speed lock"
    );
    (void)mp::lobby_apply_event(
        unlocked.lobby, raw_event(kRoomHost, {0xfb, 0x06, 0x04, 0x01, 0x00, 0x01, 0x01, 0x05, 0x00})
    );
    expect(
        unlocked.lobby.recorder.options.autopause == 1 &&
            unlocked.lobby.recorder.options.speed_lock == 0,
        "the host's other options apply, its lock does not"
    );

    Room host(true);
    host.lobby.wire_rules = versioned_recorder_rules();
    // Without setup.commander-warp the recorder offers no .cmdwarp: the line
    // is only chat.
    mp::lobby_say(host.lobby, mp::local_player(host.lobby), ".cmdwarp");
    expect(
        host.lobby.recorder.options.commander_warp == 0 &&
            host.sent(oa::netgame::RecordType::chat).size() == 1,
        "without the rule .cmdwarp is only chat"
    );
    oa::data::match_rules::MatchRules rules{};
    rules.setup.commander_warp.enabled = true;
    rules.setup.commander_warp.available = true;
    host.lobby.rules = &rules;
    std::snprintf(line.text, sizeof(line.text), "%s", "<Guest> .cmdwarp");
    (void)mp::lobby_apply_event(host.lobby, record_event(kRoomGuest, line));
    expect(host.lobby.recorder.options.commander_warp == 0, "a guest's .cmdwarp does nothing");
    // The host's own command is applied and announced; the next one turns
    // the warp off again.
    host.loopback.sent_count = 0;
    mp::lobby_say(host.lobby, mp::local_player(host.lobby), ".cmdwarp");
    expect(host.lobby.recorder.options.commander_warp == 1, "the host's own .cmdwarp applies");
    const auto lines = host.sent(oa::netgame::RecordType::chat);
    expect(
        lines.size() == 2 && std::strcmp(
                                 reinterpret_cast<const char*>(host.loopback.sent[lines[1]]) + 1,
                                 "Cmd warping enabled"
                             ) == 0,
        "the host's recorder says so"
    );
    host.loopback.sent_count = 0;
    mp::lobby_say(host.lobby, mp::local_player(host.lobby), ".cmdwarp");
    const auto off = host.sent(oa::netgame::RecordType::chat);
    expect(
        host.lobby.recorder.options.commander_warp == 0 && off.size() == 2 &&
            std::strcmp(
                reinterpret_cast<const char*>(host.loopback.sent[off[1]]) + 1,
                "Cmd warping disabled"
            ) == 0,
        "a second .cmdwarp turns the warp off"
    );
    host.lobby.rules = nullptr;
    // .report answers with the program line.
    std::snprintf(
        host.lobby.recorder.program, sizeof host.lobby.recorder.program, "%s", "Program 1"
    );
    host.loopback.sent_count = 0;
    std::snprintf(line.text, sizeof(line.text), "%s", "<Guest> .report");
    (void)mp::lobby_apply_event(host.lobby, record_event(kRoomGuest, line));
    const auto answers = host.sent(oa::netgame::RecordType::chat);
    expect(
        answers.size() == 1 &&
            std::strcmp(
                reinterpret_cast<const char*>(host.loopback.sent[answers[0]]) + 1,
                "*** Host uses Program 1"
            ) == 0,
        ".report is answered"
    );

    const auto head = mp::lobby_chat_head(*guest.game);
    const auto hidden = raw_event(kRoomHost, {0x05, 0x00, 0x2b, 0x41, 0x00, 0x01});
    auto event = hidden;
    event.size = static_cast<uint16_t>(oa::netgame::private_record_bytes);
    expect(
        !mp::lobby_apply_event(guest.lobby, event) && mp::lobby_chat_head(*guest.game) == head,
        "a private line is not shown"
    );
}

/// Returns the text of the last chat line a room sent.
///
/// @param room The room.
/// @return The text; empty when it sent none.
std::string last_chat_line(const Room& room) {
    const auto lines = room.sent(oa::netgame::RecordType::chat);
    if (lines.empty())
        return {};
    return reinterpret_cast<const char*>(room.loopback.sent[lines.back()]) + 1;
}

// The host's .base offers every seated player a prebuilt base, the standard
// one or one read from a file; a file in error, a missing file, .baseoff and
// a player joining each say so. Without setup.recorder-prebuilt-base the
// line is only chat.
void test_recorder_prebuilt_base_in_the_battle_room() {
    Room host(true);
    host.lobby.wire_rules = versioned_recorder_rules();
    auto& base = host.lobby.recorder.base;
    const auto say = [&](const char* text) {
        host.loopback.sent_count = 0;
        mp::lobby_say(host.lobby, mp::local_player(host.lobby), text);
        return last_chat_line(host);
    };
    expect(
        say(".base") == "<Host> .base" && base.per_side == 0, "without the rule .base is only chat"
    );
    oa::data::match_rules::MatchRules rules{};
    rules.setup.recorder_prebuilt_base.enabled = true;
    rules.setup.recorder_prebuilt_base.available = true;
    host.lobby.rules = &rules;
    expect(
        say(".base") == "Standard base initiated .baseoff to disable",
        "the standard base is announced"
    );
    expect(
        base.per_side == 15 && base.entries[1].unit_type == 132 &&
            base.entries[1].offset_x == -100 && base.entries[1].offset_z == 140 &&
            base.entries[1].health == 2500 && base.entries[16].unit_type == 274 &&
            base.entries[3].offset_z == 40 && base.entries[18].offset_z == 60,
        "the standard base holds both sides' buildings"
    );
    expect(
        base.available[0] && base.available[1] && base.available[2] && !base.available[3],
        "every seated player is offered a base"
    );
    host.lobby.services.read_file =
        [](void*, const char* name, std::size_t, std::string* contents) {
            if (std::strcmp(name, "base.txt") == 0)
                *contents = "; a base\r\n2\r\n1 99 10 -20 300;\r\n4 $10 0 0 5;\n";
            else if (std::strcmp(name, "bad.txt") == 0)
                *contents = "2\n1 99 10 -20 300\n";
            else
                return false;
            return true;
        };
    expect(
        say(".base base.txt") == "Fast base initiated from base.txt .baseoff to disable",
        "a base file is announced"
    );
    expect(
        base.per_side == 2 && base.entries[1].unit_type == 99 && base.entries[1].offset_x == 10 &&
            base.entries[1].offset_z == -20 && base.entries[1].health == 300 &&
            base.entries[4].unit_type == 16,
        "the file's buildings replace their entries"
    );
    expect(say(".base bad.txt") == "Erroneous base file5", "a line without its ';' is in error");
    expect(say(".base nothing.txt") == "Unable to open file nothing.txt", "a missing file is told");
    // A player joining takes the offer back.
    (void)say(".base");
    host.loopback.sent_count = 0;
    join_remote(host.lobby, kRoomThird);
    expect(
        last_chat_line(host) == "New player. Quick base toggled off" && !base.available[0] &&
            !base.available[1],
        "a joining player takes the offer back"
    );
    (void)say(".base");
    expect(
        say(".baseoff") == "Quick base disabled" && !base.enabled && !base.available[0],
        ".baseoff takes every base back for the session"
    );
}

/// Checks the battle room's commands, matched without case: they send no chat.
void test_chat_commands(const oa::ui::gui_layout::Layout& lounge) {
    Fixture f(lounge);
    for (const char* command :
         {"+SYNCERR", "+SyncErr", "+PAGE", "+Page Guest hi", "+P", "+P Guest hi"}) {
        f.panel.focus = mp::panel_find(f.panel, "MESSAGE");
        mp::panel_set_text(f.panel, "MESSAGE", command);
        (void)f.press("MESSAGE");
    }
    expect(
        f.sent_of(oa::netgame::RecordType::chat) == 0,
        "the commands send no chat whatever their case"
    );
    f.panel.focus = mp::panel_find(f.panel, "MESSAGE");
    mp::panel_set_text(f.panel, "MESSAGE", "+Pages of text");
    (void)f.press("MESSAGE");
    expect(
        f.sent_of(oa::netgame::RecordType::chat) == 1,
        "a line that only starts like a command is chat"
    );
}

/// Checks the Ping column and the host's published lowest latency from an echo.
void test_ping_column(const oa::ui::gui_layout::Layout& lounge) {
    Fixture f(lounge);
    f.lobby.services.milliseconds = milliseconds;
    f.join(0x200, "Guest");
    f.remote_info(0x200, 0, 1);
    oa::netgame::PingRecord echo{};
    echo.origin_tick_count = 4990;
    echo.echo_tick_count = 7;
    echo.origin_player_id = 0x100;
    milliseconds_now = 5030;
    mp::local_info(f.lobby).lowest_latency = 0xffff;
    expect(mp::lobby_apply_event(f.lobby, record_event(0x200u, echo)), "the guest's echo applies");
    mp::lobby_update_status(f.lobby, f.panel);
    const auto slot = mp::slot_for_player_id(f.lobby, 0x200);
    char control[16];
    std::snprintf(control, sizeof control, "PING%d", slot);
    expect(mp::panel_text(f.panel, control) == "40", "the Ping column shows the round trip");
    expect(mp::local_info(f.lobby).lowest_latency == 40, "the host keeps its lowest latency");
}

/// Checks that closing a slot publishes a lower player limit and reopening it a higher one.
void test_closing_slots_republishes(const oa::ui::gui_layout::Layout& lounge) {
    Fixture f(lounge);
    mp::SessionEntry hosted{};
    expect(mp::loopback_add_session(f.loopback, hosted), "the session is listed");
    f.loopback.hosted = 0;
    mp::lobby_session_players(*f.game) = 10;
    expect(f.press("PLAYER3"), "PLAYER3 clickable");
    expect(
        mp::slot_player(f.lobby, 3).status == mp::kSlotBlocked &&
            f.loopback.sessions[0].max_players == 9,
        "closing a slot publishes nine players"
    );
    expect(f.press("PLAYER3"), "PLAYER3 clickable again");
    expect(
        mp::slot_player(f.lobby, 3).status != mp::kSlotBlocked &&
            f.loopback.sessions[0].max_players == 10,
        "reopening it publishes ten again"
    );
}

void test_add_player_and_description() {
    auto game = std::make_unique<oa::Game>();
    mp::Lobby lobby{};
    mp::LoopbackNet loopback{};
    mp::loopback_reset(loopback);
    mp::lobby_reset(lobby, *game);
    lobby.net = mp::loopback_lobby_net(loopback);
    lobby.services = test_services();
    lobby.maps = test_maps();
    lobby.local_version_major = 3;
    game->session_flags |= 1;
    game->players[0].player_id = 0x100;
    mp::lobby_seat_local(lobby, 0, true, "Host");
    // Slot 1 is closed: the joiner takes the first slot neither in use nor closed.
    game->players[1].status = mp::kSlotBlocked;
    expect(mp::lobby_add_player(lobby, 0x300, "Joiner"), "a new id is seated");
    const auto& seated = game->players[2];
    expect(
        seated.in_use == 1 && seated.status == mp::kSlotRemote && seated.player_id == 0x300 &&
            seated.index == 2 && std::strcmp(seated.name, "Joiner") == 0 &&
            seated.last_update_time == clock_ticks && seated.reject_reason == 0,
        "the joiner is remote in the first open slot"
    );
    expect(mp::lobby_player_count(*game) == 2, "the joiner is counted");
    expect(
        loopback.sent_count > 0 && loopback.sent[loopback.sent_count - 1][0] ==
                                       static_cast<uint8_t>(oa::netgame::RecordType::player_team),
        "the local player's info goes out again"
    );
    expect(
        !mp::lobby_add_player(lobby, 0x300, "Joiner"), "an id already in use is not seated twice"
    );
    expect(!mp::lobby_add_player(lobby, 0x100, "Host"), "the local player's slot is in use");

    // Colours: the host takes a free one itself and hands the next free
    // one to a client whose request is taken.
    const auto sent_before = loopback.sent_count;
    mp::lobby_request_color(lobby, 4);
    expect(
        mp::local_info(lobby).color == 4 && loopback.sent_count == sent_before,
        "host takes a free colour"
    );
    expect(
        !mp::lobby_color_available(lobby, 0x300, 4) && mp::lobby_color_available(lobby, 0x100, 4) &&
            !mp::lobby_color_available(lobby, 0x300, 10) &&
            !mp::lobby_color_available(lobby, 0x300, 0xff),
        "a colour is taken by any other seated player only"
    );
    mp::LobbyEvent request{};
    request.kind = mp::LobbyEventKind::record;
    request.player_id = 0x300;
    request.size = 2;
    request.data[0] = static_cast<uint8_t>(oa::netgame::RecordType::player_value_request);
    request.data[1] = 4;
    expect(mp::lobby_apply_event(lobby, request), "the host answers a colour request");
    expect(loopback.sent_count > 0, "the colour reply is sent");
    if (loopback.sent_count == 0)
        return;
    const auto* reply = loopback.sent[loopback.sent_count - 1];
    expect(
        reply[0] == static_cast<uint8_t>(oa::netgame::RecordType::player_value_reply) &&
            reply[1] == 5 && loopback.sent_to[loopback.sent_count - 1] == 0x300 &&
            mp::slot_info(lobby, 2)->color == 5,
        "a taken colour is replaced by the next free one"
    );
    request.data[1] = 7;
    expect(mp::lobby_apply_event(lobby, request), "a free colour request is granted");
    reply = loopback.sent[loopback.sent_count - 1];
    expect(reply[1] == 7 && mp::slot_info(lobby, 2)->color == 5, "a granted colour is echoed");
    mp::LobbyEvent answer{};
    answer.kind = mp::LobbyEventKind::record;
    answer.player_id = 0x300;
    answer.size = 2;
    answer.data[0] = static_cast<uint8_t>(oa::netgame::RecordType::player_value_reply);
    answer.data[1] = 6;
    expect(
        mp::lobby_apply_event(lobby, answer) && mp::local_info(lobby).color == 6,
        "a reply sets the local colour"
    );

    std::snprintf(mp::lobby_game_name(*game), 17, "%s", "Big Battle");
    char session_name[mp::kSessionNameBytes];
    uint8_t user[mp::kSessionUserBytes];
    mp::lobby_session_description(lobby, session_name, user);
    char expected[mp::kSessionNameBytes];
    std::snprintf(expected, sizeof expected, "%-16s%-15s", "Big Battle", name(nullptr));
    expect(
        std::memcmp(session_name, expected, sizeof expected) == 0, "game and map names space padded"
    );
    const auto& info = mp::local_info(lobby);
    expect(
        (info.options & 0xf) == 2 && info.version_major == 3, "player count and version stamped"
    );
    const auto* user_bytes =
        reinterpret_cast<const uint8_t*>(&info) + offsetof(mp::PlayerSetupInfo, memory_mb);
    expect(
        std::memcmp(user, user_bytes, sizeof user) == 0,
        "user words are the info bytes from memory_mb on"
    );
}

// The host decides per unit whether every peer has it, from the keys and,
// from version 1.2 on, the checksums they report, and again whenever the
// roster changes. A reported checksum must also match the host's own for
// the unit, which it works out through its files the first time one is
// compared and keeps.
int host_checksums_worked = 0;

void test_unit_sync_reevaluation() {
    auto game = std::make_unique<oa::Game>();
    mp::Lobby lobby{};
    mp::LoopbackNet loopback{};
    mp::loopback_reset(loopback);
    mp::lobby_reset(lobby, *game);
    lobby.net = mp::loopback_lobby_net(loopback);
    game->session_flags |= 1;
    game->players[0].player_id = 0x100;
    mp::lobby_seat_local(lobby, 0, true, "Host");
    mp::LobbyUnit units[] = {
        {"", "", 0, 0, 0, 0, nullptr, 0, 0},
        {"Peewee", "ARM", 1000, 50, 0, 22, nullptr, 0, 0},
        {"AK", "CORE", 900, 45, 0, 33, nullptr, 0, 0},
        {"Flash", "ARM", 1100, 60, 0, 44, nullptr, 0, 0},
        {"Hammer", "ARM", 1300, 120, 0, 55, nullptr, 0, 0},
        {"Samson", "ARM", 1400, 130, 0, 66, nullptr, 0, 0},
    };
    lobby.units = units;
    lobby.unit_count = 6;
    for (const uint32_t id : {0x200U, 0x300U}) {
        mp::LobbyEvent join{};
        join.kind = mp::LobbyEventKind::player_joined;
        join.player_id = id;
        (void)mp::lobby_apply_event(lobby, join);
    }
    const auto first = mp::slot_for_player_id(lobby, 0x200);
    const auto second = mp::slot_for_player_id(lobby, 0x300);
    mp::slot_info(lobby, first)->version_major = 3;
    mp::slot_info(lobby, second)->version_major = 3;
    // Two machines, each its own group, as the host's replies assign them.
    mp::slot_player(lobby, first).machine_group = 2;
    mp::slot_player(lobby, second).machine_group = 3;
    lobby.services.unit_checksum = [](void*, const mp::LobbyUnit& unit) {
        ++host_checksums_worked;
        return unit.fbi_hash * 0x101u;
    };
    const auto own = [](uint32_t key) { return key * 0x101u; };
    mp::unit_sync_create(lobby, true);
    mp::unit_sync_tick(lobby);
    expect(lobby.sync.peer_count == 2, "both peers are greeted");
    const auto report = [&](int32_t slot, uint32_t key, uint32_t checksum) {
        uint8_t record[14] = {0x1a, 2};
        std::memcpy(record + 6, &key, 4);
        std::memcpy(record + 10, &checksum, 4);
        mp::unit_sync_receive(lobby, record, static_cast<uint8_t>(slot));
    };
    const auto sent_key = [&](int32_t index) {
        uint32_t key = 0;
        std::memcpy(&key, loopback.sent[index] + 6, sizeof key);
        return key;
    };
    mp::UnitSyncRecord record{};
    report(first, 22, 0);
    const auto last = loopback.sent_count - 1;
    expect(
        !mp::unit_sync_lookup(lobby, 22, &record) && sent_key(last - 1) == 22 &&
            sent_key(last) == 22 && loopback.sent[last][11] == 0 &&
            loopback.sent_to[last - 1] == 0x200 && loopback.sent_to[last] == 0x300,
        "a unit one peer has not reported is missing, and every peer is told"
    );
    report(second, 22, 0);
    expect(mp::unit_sync_lookup(lobby, 22, &record), "a unit every peer reported is available");
    expect(
        host_checksums_worked == 0, "a report without a checksum leaves the host's own unworked"
    );
    report(first, 33, own(33) + 1);
    report(second, 33, own(33));
    expect(
        !mp::unit_sync_lookup(lobby, 33, &record),
        "peers reporting different checksums lose the unit"
    );
    expect(
        host_checksums_worked == 1 && units[2].content_checksum == own(33) &&
            lobby.sync.records[1].checksum == own(33),
        "the host works its own checksum out once and keeps it"
    );
    mp::slot_info(lobby, first)->version_major = 1;
    mp::slot_info(lobby, first)->version_minor = 1;
    report(first, 44, 5);
    report(second, 44, own(44));
    expect(
        mp::unit_sync_lookup(lobby, 44, &record),
        "a peer before version 1.2 is compared by key only"
    );
    report(first, 55, own(55) + 2);
    report(second, 55, own(55) + 2);
    expect(
        !mp::unit_sync_lookup(lobby, 55, &record),
        "a unit whose files differ from the host's is missing"
    );
    expect(host_checksums_worked == 3, "each unit's checksum is worked out once");
    report(first, 66, 0);
    expect(!mp::unit_sync_lookup(lobby, 66, &record), "a unit the second peer lacks is missing");

    mp::LobbyEvent left{};
    left.kind = mp::LobbyEventKind::player_left;
    left.player_id = 0x300;
    (void)mp::lobby_apply_event(lobby, left);
    mp::unit_sync_tick(lobby);
    const auto relayed = loopback.sent_count - 1;
    expect(
        lobby.sync.peer_count == 1 && mp::unit_sync_lookup(lobby, 66, &record) &&
            mp::unit_sync_lookup(lobby, 33, &record) && sent_key(relayed) == 66 &&
            loopback.sent[relayed][11] == 1 && loopback.sent_to[relayed] == 0x200,
        "once the peer that lacked a unit leaves, the unit is available again"
    );
}

// The connection selection takes the launch's transport; the host's session
// takes its player limit; the session description marks an active launch.
void test_launch_connection() {
    reset_launch();
    auto game = std::make_unique<oa::Game>();
    mp::Lobby lobby{};
    mp::LoopbackNet loopback{};
    mp::loopback_reset(loopback);
    mp::lobby_reset(lobby, *game);
    lobby.net = mp::loopback_lobby_net(loopback);
    bind_services(lobby);
    bind_maps(lobby);
    lobby.local_version_major = 3;
    lobby.launch_link = launch_link();
    mp::ConnectState state{};
    state.provider = -1;
    state.provider_count = 2;
    std::memcpy(state.providers[0].guid, mp::kProviderGuidIpx, 16);
    std::memcpy(state.providers[1].guid, mp::kProviderGuidTcpip, 16);
    launch_block.connection_type = 1;
    std::snprintf(launch_block.password, sizeof launch_block.password, "%s", "YFCPDVPKETZYKXA1");
    expect(
        mp::providers_take_launch(lobby, state) && state.provider == 1 &&
            std::memcmp(mp::lobby_provider_guid(*game), mp::kProviderGuidTcpip, 16) == 0,
        "connection type 1 takes the TCP/IP provider"
    );
    expect(launch_block.connection_type == 0, "the connection type is taken once");
    expect(mp::lobby_password(*game)[0] == '\0', "without an active launch the password stays");
    launch_block.connection_type = 2;
    active_launch = true;
    expect(
        mp::providers_take_launch(lobby, state) && state.provider == 0,
        "connection type 2 takes the IPX provider"
    );
    expect(
        std::strcmp(mp::lobby_password(*game), "YFCPDVPKET") == 0,
        "an active launch's password becomes the game's, 10 characters"
    );
    launch_block.connection_type = 3;
    expect(
        !mp::providers_take_launch(lobby, state) && launch_block.connection_type == 0,
        "a transport not listed takes nothing and is still cleared"
    );
    expect(!mp::providers_take_launch(lobby, state), "no connection type takes nothing");

    loopback.opened = true;
    std::snprintf(mp::lobby_game_name(*game), 17, "%s", "Acid Pools");
    std::snprintf(mp::lobby_nickname(*game), 17, "%s", "Ghost");
    launch_block.player_limit = 4;
    expect(
        mp::connect_host(lobby, state) && mp::lobby_session_players(*game) == 4,
        "an active launch's player limit caps the session"
    );
    const auto& info = mp::local_info(lobby);
    expect(
        (info.status & mp::status::launch_only) != 0 && info.version_major == 103,
        "an active launch marks the session and biases its version"
    );
    expect(mp::info_version_compatible(info, 3), "the biased version still joins");
    launch_block.player_limit = 12;
    expect(
        mp::connect_host(lobby, state) && mp::lobby_session_players(*game) == 10,
        "a limit above 10 gives 10"
    );
    launch_block.player_limit = 1;
    expect(
        mp::connect_host(lobby, state) && mp::lobby_session_players(*game) == 10,
        "a limit of 1 leaves 10"
    );
    active_launch = false;
    launch_block.player_limit = 4;
    expect(
        mp::connect_host(lobby, state) && mp::lobby_session_players(*game) == 10,
        "without an active launch the session takes 10"
    );
    expect(
        (mp::local_info(lobby).status & mp::status::launch_only) == 0 &&
            mp::local_info(lobby).version_major == 3,
        "and its description is unmarked"
    );
    reset_launch();
}

/// Loads an installed GUI into a panel.
///
/// @param assets the installed game's store
/// @param file file name under guis/
/// @param[out] panel panel to load
/// @return false when the file does not parse
bool load_gui(const oa::AssetStore& assets, const char* file, mp::Panel& panel) {
    const auto parsed = oa::ui::gui_layout::parse(read_gui(assets, file));
    expect(parsed.ok(), file);
    if (!parsed.ok())
        return false;
    mp::panel_load(panel, (std::string("guis/") + file).c_str(), *parsed.layout);
    panel.line_height = 14;
    mp::panel_bind_lists(panel);
    mp::panel_bind_sliders(panel, slider_art);
    return true;
}

/// Adds a listed session, launch-only when asked.
///
/// @param[in,out] loopback loopback net
/// @param launch_only whether the host marks the game launch-only
void add_listed_session(mp::LoopbackNet& loopback, bool launch_only) {
    mp::SessionEntry entry{};
    entry.instance_guid[0] = 9;
    std::snprintf(entry.name, sizeof(entry.name), "%-16.16s%-15.15s", "Acid Pools", "Acid Pools");
    entry.max_players = 10;
    mp::PlayerSetupInfo host{};
    host.options = 1 | mp::option::watching_allowed;
    host.version_major = launch_only ? 103 : 3;
    host.status = launch_only ? mp::status::launch_only : 0;
    std::memcpy(
        entry.user, reinterpret_cast<const uint8_t*>(&host) + mp::kSessionUserInfoOffset, 16
    );
    (void)mp::loopback_add_session(loopback, entry);
}

// The connection screens after a launch: TCP.GUI takes the launch's
// address, the game list hosts at once or waits for the host, NEWMULTI goes
// on by itself, and a launch-only game is joinable only while a launch is
// active.
void test_launch_screens(const oa::AssetStore& assets) {
    reset_launch();
    auto game = std::make_unique<oa::Game>();
    mp::Lobby lobby{};
    mp::LoopbackNet loopback{};
    mp::loopback_reset(loopback);
    mp::lobby_reset(lobby, *game);
    lobby.net = mp::loopback_lobby_net(loopback);
    bind_services(lobby);
    bind_maps(lobby);
    lobby.local_version_major = 3;
    lobby.launch_link = launch_link();
    mp::lobby_seat_local(lobby, 0, false, "");
    mp::ConnectState state{};
    state.provider = -1;
    state.settings = {nullptr, read_saved_address, write_saved_address, nullptr};

    // TCP.GUI: the stored address without a launch address.
    mp::Panel tcp;
    if (!load_gui(assets, "tcp.gui", tcp))
        return;
    mp::tcp_open(lobby, state, tcp);
    expect(
        mp::panel_text(tcp, "ADDRESS") == "192.168.0.9" && !state.launch_address_used,
        "TCP.GUI shows the stored address"
    );
    // An active launch's address is taken and accepted at once, quietly.
    std::snprintf(launch_block.address, sizeof launch_block.address, "%s", "10.66.5.217");
    launch_block.hosting = 1;
    active_launch = true;
    mp::tcp_open(lobby, state, tcp);
    expect(
        mp::panel_text(tcp, "ADDRESS") == "10.66.5.217" && state.launch_address_used,
        "TCP.GUI takes the launch address"
    );
    sounds.clear();
    loopback.opened = false;
    expect(
        mp::tcp_accept_launch_address(lobby, state, tcp) == mp::ConnectAction::game_list,
        "the launch address opens the service"
    );
    expect(sounds.empty() && address_saves == 0, "no SmlButton and no saved address");
    expect(
        (game->connection_flags & (mp::kConnectAddressSet | mp::kConnectFromOk)) ==
            (mp::kConnectAddressSet | mp::kConnectFromOk),
        "the address is set and the OK bit hosts"
    );
    // Without an active launch the accept sounds and saves as OK does.
    active_launch = false;
    launch_block.hosting = 0;
    mp::tcp_open(lobby, state, tcp);
    game->connection_flags = 0;
    expect(
        mp::tcp_accept_launch_address(lobby, state, tcp) == mp::ConnectAction::game_list,
        "the '-n1' address opens the service"
    );
    expect(
        !sounds.empty() && sounds.back() == "SmlButton" && saved_address == "10.66.5.217",
        "SmlButton and the saved address"
    );
    expect(
        (game->connection_flags & (mp::kConnectAddressSet | mp::kConnectFromOk)) ==
            mp::kConnectAddressSet,
        "a joiner leaves the OK bit clear"
    );

    // SELGAME: a hosting launch goes on to NEWMULTI at once.
    mp::Panel list;
    if (!load_gui(assets, "selgame.gui", list))
        return;
    loopback.opened = true;
    active_launch = true;
    launch_block.hosting = 1;
    std::snprintf(mp::lobby_nickname(*game), 17, "%s", "Ghost");
    expect(mp::game_list_open(lobby, state, list), "the game list opens");
    expect(
        state.update_requested && !state.join_pending && launch_block.address[0] == '\0',
        "a hosting launch asks to host and the address is taken"
    );
    sounds.clear();
    expect(
        mp::game_list_tick(lobby, state, list) == mp::ConnectAction::new_game,
        "the game list goes on to NEWMULTI"
    );
    expect(sounds.empty(), "no BigButton while a launch is active");

    // A launched joiner waits for the host's game and joins it.
    std::snprintf(launch_block.address, sizeof launch_block.address, "%s", "10.66.5.217");
    launch_block.hosting = 0;
    expect(mp::game_list_open(lobby, state, list) && state.join_pending, "a joiner waits");
    last_message.clear();
    const auto start = clock_ticks;
    expect(
        mp::game_list_tick(lobby, state, list) == mp::ConnectAction::none && state.host_waiting,
        "no game listed yet"
    );
    expect(last_message == "Waiting for host... (0)", "the wait's first frame shows 0");
    clock_ticks += 1;
    (void)mp::game_list_tick(lobby, state, list);
    expect(last_message == "Waiting for host... (20)", "the next frame counts from the first");
    clock_ticks += 14; // 500 ms past the start: a poll, still nothing listed
    (void)mp::game_list_tick(lobby, state, list);
    clock_ticks += 1;
    (void)mp::game_list_tick(lobby, state, list);
    expect(last_message == "Waiting for host... (18)", "19.5 s left show as 18");
    add_listed_session(loopback, true);
    clock_ticks = start + 31; // past the next poll
    sounds.clear();
    expect(
        mp::game_list_tick(lobby, state, list) == mp::ConnectAction::join && !state.host_waiting,
        "the host's launch-only game is joined once listed"
    );
    expect(sounds.empty() && game->frontend_pending_signal == 0x12, "a quiet join");

    // Nobody hosts in time: the launch's join fails with host-not-found.
    loopback.session_count = 0;
    std::snprintf(launch_block.address, sizeof launch_block.address, "%s", "10.66.5.217");
    expect(mp::game_list_open(lobby, state, list) && state.join_pending, "a joiner waits again");
    auto action = mp::ConnectAction::none;
    for (int32_t frame = 0; frame < 700 && action == mp::ConnectAction::none; ++frame) {
        action = mp::game_list_tick(lobby, state, list);
        clock_ticks += 1;
    }
    expect(
        action == mp::ConnectAction::main_menu && game->frontend_state == 2,
        "after 20 s the game returns to the main menu"
    );
    expect(
        failed_joins.size() == 1 && failed_joins[0] == mp::kHostNotFoundText,
        "the launch's join fails once, with host-not-found"
    );
    expect(!state.host_waiting, "the wait ends");

    // A joiner whose launch is no longer active when its wait starts is
    // told "Host not found.  Exiting..." once the wait ends, and
    // 4 s later the game leaves; no launch's join fails.
    std::snprintf(launch_block.address, sizeof launch_block.address, "%s", "10.66.5.217");
    expect(
        mp::game_list_open(lobby, state, list) && state.join_pending, "a joiner waits once more"
    );
    active_launch = false;
    action = mp::ConnectAction::none;
    int32_t exiting_frame = -1;
    int32_t frame = 0;
    for (; frame < 900 && action == mp::ConnectAction::none; ++frame) {
        action = mp::game_list_tick(lobby, state, list);
        if (exiting_frame < 0 && state.host_not_found_exiting) {
            exiting_frame = frame;
            expect(
                last_message == mp::kHostNotFoundExitingText && !state.host_waiting,
                "the wait ends showing Host not found.  Exiting..."
            );
        }
        clock_ticks += 1;
    }
    expect(
        action == mp::ConnectAction::leave_game && exiting_frame >= 0 &&
            frame - 1 - exiting_frame == 120,
        "the game leaves 4 s after the text shows"
    );
    expect(
        failed_joins.size() == 1 && !state.host_not_found_exiting, "no launch's join fails with it"
    );

    // A launch-only game refuses a joiner without an active launch.
    active_launch = false;
    state.join_pending = false;
    add_listed_session(loopback, true);
    expect(mp::game_list_update(lobby, state, list), "the launch-only game is listed");
    mp::panel_control(list, "GAMENAME")->list_selection = 0;
    mp::panel_set_text(list, "NICKNAME", "Guest");
    (void)mp::panel_press(list, "JOINGAME");
    last_message.clear();
    expect(
        mp::game_list_handle_event(lobby, state, list) == mp::ConnectAction::none &&
            last_message == "You must join this game via the Boneyards.",
        "a launch-only game needs an active launch"
    );
    active_launch = true;
    (void)mp::panel_press(list, "JOINGAME");
    expect(
        mp::game_list_handle_event(lobby, state, list) == mp::ConnectAction::join,
        "with a launch active it is joinable"
    );
    // PREVMENU while a launch is active goes to the main menu.
    (void)mp::panel_press(list, "PREVMENU");
    game->frontend_state = 0x11;
    expect(
        mp::game_list_handle_event(lobby, state, list) == mp::ConnectAction::main_menu &&
            game->frontend_state == 2,
        "PREVMENU returns to the main menu"
    );

    // NEWMULTI: the launch's user name, and an active launch hosts at once.
    mp::Panel newmulti;
    if (!load_gui(assets, "newmulti.gui", newmulti))
        return;
    std::snprintf(launch_block.user_name, sizeof launch_block.user_name, "%s", "Ghost_Commander");
    std::snprintf(
        launch_block.session_name, sizeof launch_block.session_name, "%s", "Example game"
    );
    game->frontend_pending_signal = 0;
    mp::new_game_open(lobby, state, newmulti);
    expect(
        std::strcmp(mp::lobby_nickname(*game), "Ghost_Commander") == 0 &&
            mp::panel_text(newmulti, "NICKNAME") == "Ghost_Commander",
        "the launch's user name is the nickname"
    );
    expect(
        std::strcmp(mp::lobby_game_name(*game), "Example game") == 0 &&
            mp::panel_text(newmulti, "GAMENAME") == "Example game",
        "the launch's session name is the game name"
    );
    expect(
        state.host_at_once && game->frontend_pending_signal == 0x11,
        "an active launch hosts without OK"
    );
    active_launch = false;
    mp::new_game_open(lobby, state, newmulti);
    expect(!state.host_at_once, "without a launch NEWMULTI waits for OK");
    reset_launch();
}

// The battle room applies an active launch's options and locks.
void test_launch_battleroom(const oa::ui::gui_layout::Layout& lounge) {
    reset_launch();
    active_launch = true;
    launch_block.max_units = 200;
    launch_block.lock_options = 1;
    launch_block.death_option = 3;
    launch_block.energy = 3000;
    launch_block.metal = 2500;
    launch_block.faction = oa::ui::frontend_multiplayer::launch::faction::core;
    launch_block.team = 2;
    launch_block.los = 2;
    launch_block.cheats = 2;
    launch_block.location = 1;
    launch_block.mapping = 1;
    launch_block.watching = 2;
    launch_block.tournament = 1;
    std::snprintf(launch_block.mission, sizeof launch_block.mission, "%s", "Acid Pools");
    std::snprintf(launch_block.provider_label, sizeof launch_block.provider_label, "%s", "Ladder");
    const auto link = launch_link();
    {
        Fixture f(lounge, &link);
        const auto& info = mp::local_info(f.lobby);
        expect(info.max_units == 200 && mp::lobby_max_units(*f.game) == 200, "the unit limit");
        expect(
            mp::lobby_options_locked(f.lobby) && mp::lobby_launch_locked(f.lobby),
            "the lock and the tournament lock the options"
        );
        expect(
            (info.options & mp::option::commander_mask) == mp::option::commander_deathmatch &&
                f.game->session_rules == 2 && f.game->start_options[0] == 2,
            "commander rule 3 is deathmatch"
        );
        expect(
            info.side == 1 && mp::lobby_player_team(mp::local_player(f.lobby)) == 1,
            "Core side and team 2"
        );
        expect(
            (info.options & (mp::option::los_limited | mp::option::los_true)) ==
                mp::option::los_limited,
            "line of sight 2 is circular"
        );
        expect(
            (info.options & mp::option::cheats_allowed) != 0 &&
                (info.options & mp::option::fixed_locations) != 0 &&
                (info.options & mp::option::unmapped) != 0 &&
                (info.options & mp::option::watching_allowed) != 0,
            "cheats, fixed positions, unmapped and watching"
        );
        expect(
            info.metal_hundreds >= 24 && info.metal_hundreds <= 26 && info.energy_hundreds >= 29 &&
                info.energy_hundreds <= 31,
            "the sliders start at the launch's resources"
        );
        for (const char* control :
             {"MAXUNITS", "ENERGY", "METAL", "CHEATING", "LOSTYPE", "GAMEOPEN"})
            expect(mp::panel_control(f.panel, control)->grayed, control);
        expect(
            mp::panel_control(f.panel, "RESTRICTIONS")->grayed &&
                mp::panel_control(f.panel, "COMMANDER")->grayed,
            "RESTRICTIONS and COMMANDER locked"
        );
        expect(
            mp::panel_text(f.panel, "PREVMENU") == "Ladder", "PREVMENU shows the provider label"
        );
        expect(map_name == "Acid Pools", "the host plays the launch's map");
    }
    map_name = "Coast To Coast";
    refuse_map = true;
    last_message.clear();
    {
        Fixture missed(lounge, &link);
        expect(
            mp::local_info(missed.lobby).map_name[0] == '\0',
            "a launch map that is not installed is not another map"
        );
        expect(
            last_message.find("Could not find") != std::string::npos, "the missing map is reported"
        );
    }
    refuse_map = false;
    map_name = "Coast To Coast";
    std::snprintf(
        launch_block.provider_label, sizeof launch_block.provider_label, "%s", "TwelveLetter"
    );
    {
        Fixture f(lounge, &link, false);
        const auto& joined = mp::local_info(f.lobby);
        expect(map_name == "Coast To Coast", "a joiner leaves the map to the host");
        expect(
            joined.side == 1 && mp::lobby_player_team(mp::local_player(f.lobby)) == 1,
            "a joiner takes the launch's side and team"
        );
        expect(joined.metal_hundreds == 10, "a joiner leaves the metal");
        expect(
            (joined.options & mp::option::commander_mask) != mp::option::commander_deathmatch,
            "a joiner leaves the commander rule"
        );
        expect(
            mp::panel_text(f.panel, "PREVMENU") == "Previous Menu",
            "a long label leaves the caption"
        );
    }
    {
        reset_launch();
        active_launch = true;
        launch_block.player_limit = 4;
        auto game = std::make_unique<oa::Game>();
        mp::Lobby lobby{};
        mp::LoopbackNet loopback{};
        mp::loopback_reset(loopback);
        mp::lobby_reset(lobby, *game);
        lobby.net = mp::loopback_lobby_net(loopback);
        bind_services(lobby);
        bind_maps(lobby);
        lobby.local_version_major = 3;
        lobby.launch_link = launch_link();
        mp::ConnectState state{};
        loopback.opened = true;
        expect(
            mp::connect_host(lobby, state) && mp::lobby_session_players(*game) == 4,
            "the launch's player limit caps the session"
        );
    }
    // Without an active launch nothing applies.
    active_launch = false;
    {
        Fixture f(lounge, &link);
        expect(
            !mp::lobby_options_locked(f.lobby) && mp::local_info(f.lobby).max_units != 200,
            "an inactive launch applies nothing"
        );
        expect(!mp::panel_control(f.panel, "COMMANDER")->grayed, "COMMANDER stays open");
    }
    map_name = "Coast To Coast";
    reset_launch();
}

/// Counts chat lines that hold a phrase.
///
/// @param game the lobby's game
/// @param phrase the phrase
/// @return how many of the room's lines hold it
int lines_holding(oa::Game& game, const char* phrase) {
    int count = 0;
    for (std::size_t index = 0; index < mp::kChatLines; ++index)
        if (std::strstr(mp::lobby_chat_line(game, index), phrase) != nullptr)
            ++count;
    return count;
}

/// Writes a SIDEDATA name into a launch field.
///
/// @param field the name field
/// @param name the name
void write_side_name(char* field, const char* name) {
    std::snprintf(field, oa::ui::frontend_multiplayer::launch::kLaunchSideNameBytes, "%s", name);
}

/// A launch seats a numbered side, a SIDEDATA name and the sides arm and core,
/// and a side the room lacks stays the room's own side and is named once.
void test_launch_side_names(const oa::ui::gui_layout::Layout& lounge) {
    namespace nl = oa::ui::frontend_multiplayer::launch;
    const std::vector<std::string> names{
        "Arm", "Core", "Ridge2", "Ridge3", "Ridge4", "Ridge5", "Ridge6"
    };
    reset_launch();
    active_launch = true;
    const auto link = launch_link();
    uint8_t room_side = 0;
    uint16_t room_options = 0;
    {
        Fixture plain(lounge, &link, true, 0, 7, names);
        room_side = mp::local_info(plain.lobby).side;
        room_options = mp::local_info(plain.lobby).options;
        expect(
            lines_holding(*plain.game, "The launch names side") == 0,
            "an empty launch names no side"
        );
    }
    launch_block.faction = 7;
    {
        Fixture seated(lounge, &link, true, 0, 7, names);
        expect(mp::local_info(seated.lobby).side == 6, "faction 7 seats side 6 of seven");
        expect(
            lines_holding(*seated.game, "The launch names side") == 0,
            "a side the room has is not named in the chat"
        );
    }
    launch_block.faction = nl::faction::none;
    write_side_name(launch_block.faction_name, "ridge6");
    {
        Fixture named(lounge, &link, true, 0, 7, names);
        expect(mp::local_info(named.lobby).side == 6, "the name ridge6 seats side 6");
        expect(
            lines_holding(*named.game, "The launch names side") == 0, "a known name is not posted"
        );
    }
    launch_block.faction = nl::faction::arm;
    launch_block.faction_name[0] = '\0';
    {
        Fixture arm(lounge, &link, true, 0, 7, names);
        expect(mp::local_info(arm.lobby).side == 0, "arm seats side 0");
    }
    launch_block.faction = nl::faction::core;
    {
        Fixture core(lounge, &link, true, 0, 7, names);
        expect(mp::local_info(core.lobby).side == 1, "core seats side 1");
    }
    launch_block.faction = 9;
    {
        Fixture missing(lounge, &link, true, 0, 7, names);
        const auto& info = mp::local_info(missing.lobby);
        expect(info.side == room_side, "faction 9 of seven leaves the room's side");
        expect(
            (info.options & mp::option::watcher) == (room_options & mp::option::watcher),
            "a missing number leaves the room's watcher"
        );
        expect(
            lines_holding(*missing.game, "The launch names side 8,") == 1,
            "faction 9 of seven posts side 8 once"
        );
    }
    launch_block.faction = nl::faction::none;
    write_side_name(launch_block.faction_name, "NoSuch");
    {
        Fixture unknown(lounge, &link, true, 0, 7, names);
        expect(
            mp::local_info(unknown.lobby).side == room_side,
            "an unknown name leaves the room's side"
        );
        expect(
            lines_holding(*unknown.game, "The launch names side NoSuch,") == 1,
            "an unknown name is posted once"
        );
    }
    launch_block.faction_name[0] = '\0';
    launch_block.computer_count = 1;
    launch_block.computers[0].faction = 7;
    fixture_next_player_id = 0x101;
    {
        Fixture host(lounge, &link, true, 0, 7, names);
        const auto* info = mp::slot_info(host.lobby, 1);
        expect(
            mp::slot_player(host.lobby, 1).status == mp::kSlotComputer && info != nullptr &&
                info->side == 6,
            "a computer's faction 7 seats side 6"
        );
        expect(
            lines_holding(*host.game, "The launch names side") == 0,
            "a computer side the room has is not posted"
        );
    }
    launch_block.computers[0].faction = nl::faction::none;
    write_side_name(launch_block.computers[0].faction_name, "Missing");
    {
        Fixture host(lounge, &link, true, 0, 7, names);
        const auto* info = mp::slot_info(host.lobby, 1);
        expect(
            info != nullptr && info->side == 0,
            "an unknown computer name leaves the computer's side"
        );
        expect(
            lines_holding(*host.game, "The launch names side Missing,") == 1,
            "an unknown computer name is posted once"
        );
    }
    fixture_next_player_id = 0;
    reset_launch();
}

/// A launch seats the player and this machine's computer players: the
/// player's colour and watching, and each computer player's side, team,
/// colour and difficulty, for a host and for a player who joins. A
/// deathmatch seats no computer players, and a second entry seats none again.
void test_launch_seats(const oa::ui::gui_layout::Layout& lounge) {
    namespace nl = oa::ui::frontend_multiplayer::launch;
    reset_launch();
    active_launch = true;
    launch_block.faction = nl::faction::core;
    launch_block.team = 3;
    launch_block.color = 5;
    launch_block.watching = 2;
    launch_block.computer_count = 2;
    launch_block.computers[0] = nl::LaunchComputer{nl::faction::core, 2, 7, nl::difficulty::medium};
    launch_block.computers[1] = nl::LaunchComputer{nl::faction::arm, 0, 0, nl::difficulty::none};
    const auto link = launch_link();
    // The host is 0x100; the computer players the launch seats come after.
    fixture_next_player_id = 0x101;
    {
        Fixture host(lounge, &link);
        const auto& info = mp::local_info(host.lobby);
        expect(
            info.side == 1 && mp::lobby_player_team(mp::local_player(host.lobby)) == 2 &&
                info.color == 4,
            "the host takes Core, team 3 and colour 5"
        );
        expect(
            mp::slot_player(host.lobby, 1).status == mp::kSlotComputer &&
                mp::slot_player(host.lobby, 2).status == mp::kSlotComputer &&
                mp::slot_player(host.lobby, 3).status == mp::kSlotOpen,
            "the launch's two computer players take the next open slots"
        );
        const auto first = arrive_computer(host);
        const auto second = arrive_computer(host);
        expect(first != 0 && second != 0, "both computer players arrive");
        const auto* one = mp::slot_info(host.lobby, 1);
        const auto* two = mp::slot_info(host.lobby, 2);
        expect(
            one != nullptr && one->side == 1 && one->color == 6 &&
                mp::lobby_player_team(mp::slot_player(host.lobby, 1)) == 1 &&
                host.lobby.computer_difficulty[1] == 1 && host.lobby.computer_difficulty_set[1],
            "the first computer player is Core, team 2, colour 7, medium"
        );
        expect(
            two != nullptr && two->side == 0 && two->color != 4 && two->color != 6 &&
                mp::lobby_player_team(mp::slot_player(host.lobby, 2)) == mp::kNoTeam &&
                host.lobby.computer_difficulty[2] == 2,
            "the second computer player is Arm on no team, in a free colour, hard"
        );
        expect(
            mp::panel_text(host.panel, "MEM1") == "Medium" &&
                mp::panel_text(host.panel, "MEM2") == "Hard",
            "the rows show the computer players' difficulty"
        );
        expect(
            host.sent_of(oa::netgame::RecordType::player_team) >= 2,
            "the computer players' teams are sent"
        );
        mp::lobby_enter_battleroom(host.lobby, host.panel);
        expect(
            mp::slot_player(host.lobby, 3).status == mp::kSlotOpen,
            "entering the room again seats no more computer players"
        );
    }
    launch_block.watcher = 1;
    {
        Fixture watching(lounge, &link);
        expect(
            (mp::local_info(watching.lobby).options & mp::option::watcher) != 0,
            "a launch's watcher watches"
        );
    }
    launch_block.watcher = 0;
    // The launch's unit restrictions, by FBI UnitName in any case, and a game
    // closed to more players.
    mp::LobbyUnit units[] = {
        {"", "", 0, 0, 0, 0, "", 0, 0},
        {"Peewee", "ARM", 1000, 50, 0, 22, "ARMPW", 0, 0},
        {"AK", "CORE", 900, 45, 0, 33, "CORAK", 0, 0},
        {"Nuke", "ARM", 90000, 1500, mp::kUnitDisabledDefault, 44, "ARMSILO", 0, 0},
    };
    fixture_units = units;
    fixture_unit_count = 4;
    launch_block.computer_count = 0;
    launch_block.closed = 1;
    launch_block.restriction_count = 4;
    launch_block.restrictions[0] = nl::LaunchRestriction{"CORAK", 0};
    launch_block.restrictions[1] = nl::LaunchRestriction{"armpw", 7};
    launch_block.restrictions[2] = nl::LaunchRestriction{"ARMSILO", nl::kLaunchNoLimit};
    launch_block.restrictions[3] = nl::LaunchRestriction{"NOSUCH", 3};
    {
        Fixture restricted(lounge, &link);
        mp::UnitSyncRecord ak{};
        mp::UnitSyncRecord peewee{};
        mp::UnitSyncRecord nuke{};
        (void)mp::unit_sync_lookup(restricted.lobby, 33, &ak);
        (void)mp::unit_sync_lookup(restricted.lobby, 22, &peewee);
        (void)mp::unit_sync_lookup(restricted.lobby, 44, &nuke);
        expect(ak.limit == 0 && ak.local == 0, "a restriction of 0 disables the unit");
        expect(peewee.limit == 7 && peewee.local != 0, "a restriction names the unit in any case");
        expect(nuke.limit == nl::kLaunchNoLimit && nuke.local != 0, "no-limit enables a unit");
        expect(restricted.lobby.sync.record_count == 3, "a unit the content lacks is left out");
        expect(
            (mp::local_info(restricted.lobby).options & mp::option::game_closed) != 0,
            "a launch closes the game"
        );
    }
    {
        Fixture joined(lounge, &link, false);
        mp::UnitSyncRecord ak{};
        (void)mp::unit_sync_lookup(joined.lobby, 33, &ak);
        expect(
            ak.limit != 0 && (mp::local_info(joined.lobby).options & mp::option::game_closed) == 0,
            "a joiner leaves the restrictions and the game's openness to the host"
        );
    }
    fixture_units = nullptr;
    fixture_unit_count = 0;
    launch_block.closed = 0;
    launch_block.restriction_count = 0;
    launch_block.computer_count = 2;
    launch_block.death_option = 3;
    {
        Fixture deathmatch(lounge, &link);
        expect(
            mp::slot_player(deathmatch.lobby, 1).status == mp::kSlotOpen,
            "a deathmatch seats no computer players"
        );
    }
    launch_block.death_option = 0;
    launch_block.computer_count = 1;
    {
        Fixture joined(lounge, &link, false);
        const auto& info = mp::local_info(joined.lobby);
        expect(
            info.side == 1 && mp::lobby_player_team(mp::local_player(joined.lobby)) == 2,
            "a joiner takes the launch's side and team"
        );
        expect(
            joined.sent_of(oa::netgame::RecordType::player_value_request) >= 1,
            "a joiner asks the host for the launch's colour"
        );
        expect(
            mp::slot_player(joined.lobby, 1).status == mp::kSlotComputer,
            "a joiner seats its own computer player"
        );
    }
    fixture_next_player_id = 0;
    reset_launch();
}

/// Computer players take only the sides identity.computer-player-sides allows.
///
/// A new computer seat, its SIDE button and a launch computer on arm move to
/// side 1 when that is the only side they may take. A human still cycles
/// every side and the watcher. With the key absent, each of those stays as it
/// is today.
void test_computer_player_sides(const oa::ui::gui_layout::Layout& lounge) {
    namespace nl = oa::ui::frontend_multiplayer::launch;

    struct Restore {
        oa::data::defs::DataLayout base;

        ~Restore() {
            oa::data::defs::use_data_layout(base);
            fixture_next_player_id = 0;
            reset_launch();
        }
    } restore{oa::data::defs::data_layout()};

    oa::data::defs::DataLayout limited = restore.base;
    limited.computer_player_sides = 1U << 1;
    oa::data::defs::use_data_layout(limited);
    fixture_next_player_id = 0x101;
    {
        Fixture room(lounge, nullptr, true, 0, 7);
        expect(room.press("PLAYER1") && room.press("PLAYER1"), "an open slot seats a computer");
        const auto arrived = arrive_computer(room);
        const auto* computer = mp::slot_info(room.lobby, 1);
        expect(
            arrived != 0 && computer != nullptr && computer->side == 1,
            "a new computer seat takes the side computer players may take"
        );
        expect(room.press("SIDE1"), "the computer's SIDE button clicks");
        expect(mp::slot_info(room.lobby, 1)->side == 1, "a forward click stays on that side");
        expect(room.press("SIDE1", 2), "the computer's SIDE button takes a right click");
        expect(
            mp::slot_info(room.lobby, 1)->side == 1 &&
                (mp::slot_info(room.lobby, 1)->options & mp::option::watcher) == 0,
            "a back click stays on that side and off the watcher"
        );
        auto& human = mp::local_info(room.lobby);
        human.options |= mp::option::watching_allowed;
        const uint8_t stepped[] = {1, 2, 3, 4, 5, 6};
        for (const uint8_t side : stepped) {
            expect(room.press("SIDE0"), "the human's SIDE button clicks");
            expect(human.side == side, "a human still steps through every side");
        }
        expect(room.press("SIDE0"), "the human's SIDE button reaches the watcher");
        expect((human.options & mp::option::watcher) != 0, "a human can still watch");
        expect(room.press("SIDE0"), "the human's SIDE button leaves the watcher");
        expect(
            (human.options & mp::option::watcher) == 0 && human.side == 0,
            "leaving watch returns a human to side 0"
        );
    }
    {
        // Sides 0 and 5 are allowed, and 5 is kept out of the room, so 0 is
        // the only side offered. A right click stays there.
        oa::data::defs::DataLayout one_left = limited;
        one_left.computer_player_sides = (1U << 0) | (1U << 5);
        one_left.battle_room_hidden_sides = 1U << 5;
        oa::data::defs::use_data_layout(one_left);
        Fixture room(lounge, nullptr, true, 0, 7);
        expect(room.press("PLAYER1") && room.press("PLAYER1"), "an open slot seats a computer");
        const auto arrived = arrive_computer(room);
        const auto* computer = mp::slot_info(room.lobby, 1);
        expect(
            arrived != 0 && computer != nullptr && computer->side == 0,
            "a computer opens on the one side the room offers"
        );
        expect(room.press("SIDE1"), "that computer's SIDE button clicks");
        expect(mp::slot_info(room.lobby, 1)->side == 0, "a forward click stays on that one side");
        expect(room.press("SIDE1", 2), "that computer's SIDE button takes a right click");
        expect(
            mp::slot_info(room.lobby, 1)->side == 0 &&
                (mp::slot_info(room.lobby, 1)->options & mp::option::watcher) == 0,
            "a back click stays on the one side the room offers"
        );
        oa::data::defs::use_data_layout(limited);
    }
    reset_launch();
    active_launch = true;
    launch_block.computer_count = 1;
    launch_block.computers[0].faction = nl::faction::arm;
    const auto link = launch_link();
    {
        Fixture host(lounge, &link, true, 0, 5);
        const auto* info = mp::slot_info(host.lobby, 1);
        expect(
            mp::slot_player(host.lobby, 1).status == mp::kSlotComputer && info != nullptr &&
                info->side == 1,
            "a launch computer on arm moves to the side computer players may take"
        );
        // The room keeps 71 characters of a chat line, so this matches the
        // start of the word, which is what the room shows.
        expect(
            lines_holding(*host.game, "gives a computer player side") == 1,
            "the launch posts that line once"
        );
        expect(lines_holding(*host.game, "side 0,") == 1, "the line names the side as written");
    }
    oa::data::defs::use_data_layout(restore.base);
    {
        Fixture host(lounge, &link, true, 0, 5);
        const auto* info = mp::slot_info(host.lobby, 1);
        expect(
            info != nullptr && info->side == 0,
            "with no key a launch computer on arm stays on side 0"
        );
        expect(
            lines_holding(*host.game, "gives a computer player side") == 0,
            "with no key the launch posts no such line"
        );
    }
    active_launch = false;
    {
        Fixture room(lounge, nullptr, true, 0, 7);
        expect(room.press("PLAYER1") && room.press("PLAYER1"), "an open slot seats a computer");
        const auto arrived = arrive_computer(room);
        const auto* computer = mp::slot_info(room.lobby, 1);
        expect(
            arrived != 0 && computer != nullptr && computer->side == 0,
            "with no key a new computer stays on side 0"
        );
        expect(room.press("SIDE1"), "its SIDE button clicks");
        expect(mp::slot_info(room.lobby, 1)->side == 1, "with no key its SIDE button steps");
    }
    fixture_next_player_id = 0;
    reset_launch();
}

} // namespace

// Without arguments the self-contained cases; with --data the lounge and game
// list over the installed game's GUI files.
/// Queues a one-byte probe (0x06) from a player for the battle room's next pump.
///
/// @param[in,out] loopback the room's loopback net
/// @param from the probing player
void inject_probe(mp::LoopbackNet& loopback, uint32_t from) {
    mp::LobbyEvent event{};
    event.kind = mp::LobbyEventKind::record;
    event.player_id = from;
    event.size = 1;
    event.data[0] = static_cast<uint8_t>(oa::netgame::RecordType::probe);
    expect(mp::loopback_inject(loopback, event), "the probe is queued");
}

// The battle room's stall scan, which runs as each pump ends. A remote
// player silent for more than the player timeout (30 s unless the game
// names another) is named for TIMEOUT.GUI, and any record from it, a probe
// as well, clears the name. This machine's own players, its computer player
// among them, are never named, and a player is silent only from its
// arrival. Silent players of two machines name no one, and of one machine
// the first in slot order. "Drop 0" and a pause leave the name as it was,
// and after a pause silence counts from the pause. TIMEOUT.GUI's countdown
// rejects the player once, with reason 6, at the timeout plus 120 s.
void test_player_timeout() {
    constexpr uint16_t kPaused = 0x01; // Game.sim_run_flags: the game is paused
    Room room(true);
    auto& lobby = room.lobby;
    auto& game = *room.game;
    lobby.next_stats_tick = clock_ticks + kNoStatusTicks; // no periodic block
    expect(
        game.player_timeout_seconds == mp::kDefaultPlayerTimeoutSeconds &&
            mp::kDefaultPlayerTimeoutSeconds == 30,
        "the battle room's player timeout is 30 s by default"
    );
    const uint32_t limit = 30U * 30U;
    auto& guest = mp::slot_player(lobby, mp::slot_for_player_id(lobby, kRoomGuest));
    auto& computer = mp::slot_player(lobby, 2);
    expect(computer.last_update_time == 0, "this machine's computer player was never heard from");

    uint32_t heard = guest.last_update_time;
    clock_ticks = heard + limit;
    (void)mp::lobby_tick(lobby, room.panel);
    expect(lobby.stalled_player == 0, "silence for the timeout itself is not yet a stall");
    clock_ticks = heard + limit + 1;
    (void)mp::lobby_tick(lobby, room.panel);
    expect(lobby.stalled_player == kRoomGuest, "a remote player silent past the timeout is named");
    inject_probe(room.loopback, kRoomGuest);
    (void)mp::lobby_tick(lobby, room.panel);
    expect(
        guest.last_update_time == clock_ticks && lobby.stalled_player == 0,
        "a probe from it is heard, and no one is named any more"
    );

    clock_ticks += 100 * limit;
    guest.last_update_time = clock_ticks;
    (void)mp::lobby_tick(lobby, room.panel);
    expect(
        lobby.stalled_player == 0 && computer.in_use != 0 && computer.status == mp::kSlotComputer,
        "this machine's computer player, never heard from, is never named"
    );
    join_remote(lobby, kRoomThird);
    const auto third_slot = mp::slot_for_player_id(lobby, kRoomThird);
    expect(third_slot == 3, "a third player arrives");
    auto& third = mp::slot_player(lobby, third_slot);
    (void)mp::lobby_tick(lobby, room.panel);
    expect(
        third.last_update_time == clock_ticks && lobby.stalled_player == 0,
        "an arriving player is silent only from its arrival"
    );

    // Two machines, then one.
    guest.machine_group = 2;
    third.machine_group = 3;
    heard = clock_ticks;
    clock_ticks = heard + limit + 1;
    (void)mp::lobby_tick(lobby, room.panel);
    expect(lobby.stalled_player == 0, "silent players of two machines name no one");
    third.machine_group = 2;
    (void)mp::lobby_tick(lobby, room.panel);
    expect(
        lobby.stalled_player == kRoomGuest,
        "silent players of one machine name the first in slot order"
    );

    // "Drop 0" leaves the name as it was.
    game.console_flags |= OA_CONSOLE_FLAG_NO_DROP;
    guest.last_update_time = clock_ticks;
    third.last_update_time = clock_ticks;
    (void)mp::lobby_tick(lobby, room.panel);
    expect(lobby.stalled_player == kRoomGuest, "with Drop 0 the scan does not run");
    game.console_flags &= static_cast<uint16_t>(~OA_CONSOLE_FLAG_NO_DROP);
    (void)mp::lobby_tick(lobby, room.panel);
    expect(lobby.stalled_player == 0, "without it the players are heard again");

    // A pause leaves the name as it was, and silence counts from its end.
    heard = clock_ticks;
    clock_ticks = heard + limit + 1;
    (void)mp::lobby_tick(lobby, room.panel);
    expect(lobby.stalled_player == kRoomGuest, "the guest's machine is silent again");
    game.sim_run_flags |= kPaused;
    clock_ticks += 5 * limit;
    (void)mp::lobby_tick(lobby, room.panel);
    expect(
        lobby.stalled_player == kRoomGuest && lobby.timeout_baseline == clock_ticks,
        "while paused the scan only follows the clock"
    );
    game.sim_run_flags &= static_cast<uint16_t>(~kPaused);
    (void)mp::lobby_tick(lobby, room.panel);
    expect(lobby.stalled_player == 0, "once the pause ends, the silence before it is forgotten");
    const uint32_t resumed = clock_ticks;
    clock_ticks = resumed + limit;
    (void)mp::lobby_tick(lobby, room.panel);
    expect(lobby.stalled_player == 0, "silence counts from the pause");
    clock_ticks = resumed + limit + 1;
    (void)mp::lobby_tick(lobby, room.panel);
    expect(lobby.stalled_player == kRoomGuest, "and names the player once past the timeout");

    // TIMEOUT.GUI's countdown, without its layout.
    mp::Panel dialog;
    expect(!mp::timeout_open(lobby, dialog, 0x999), "TIMEOUT opens for no id without a slot");
    expect(mp::timeout_open(lobby, dialog, kRoomGuest), "TIMEOUT opens for the silent player");
    const uint32_t last = clock_ticks;
    guest.last_update_time = last;
    const uint32_t drop = (30U + 120U) * 30U;
    room.loopback.sent_count = 0;
    clock_ticks = last + drop - 1;
    expect(
        !mp::timeout_tick(lobby, dialog) && room.rejects().empty(),
        "short of the timeout plus 120 s, no one is rejected"
    );
    clock_ticks = last + drop;
    expect(mp::timeout_tick(lobby, dialog), "at the timeout plus 120 s the dialog closes");
    const auto rejects = room.rejects();
    const auto sent = room.sent(oa::netgame::RecordType::reject);
    expect(
        rejects.size() == 1 && sent.size() == 1 && rejects[0].player_id == kRoomGuest &&
            rejects[0].reason == 6 && room.loopback.sent_from[sent[0]] == kRoomHost,
        "one 0x1b {id, 6} goes out from the host's player"
    );
    expect(
        mp::slot_for_player_id(lobby, kRoomGuest) < 0 && guest.status == mp::kSlotOpen &&
            guest.reject_reason == 6 && mp::slot_for_player_id(lobby, kRoomThird) < 0,
        "the silent player's slot opens, with its machine's other player"
    );
    expect(
        mp::timeout_tick(lobby, dialog) && room.rejects().size() == 1,
        "a closed countdown sends nothing more"
    );
    (void)mp::lobby_tick(lobby, room.panel);
    const auto computer_slot = mp::slot_for_player_id(lobby, kRoomComputer);
    expect(
        lobby.stalled_player == 0 && room.rejects().size() == 1 && computer_slot >= 0 &&
            mp::slot_player(lobby, computer_slot).status == mp::kSlotComputer,
        "no one is left to name, and the computer player stays"
    );

    // A game that names its own timeout keeps it.
    auto named = std::make_unique<oa::Game>();
    named->player_timeout_seconds = 120;
    mp::Lobby fresh{};
    mp::lobby_reset(fresh, *named);
    expect(named->player_timeout_seconds == 120, "a timeout already set is kept");
    Room slow(true);
    slow.lobby.next_stats_tick = clock_ticks + kNoStatusTicks;
    slow.game->player_timeout_seconds = 120;
    heard = mp::slot_player(slow.lobby, 1).last_update_time;
    clock_ticks = heard + 120U * 30U;
    (void)mp::lobby_tick(slow.lobby, slow.panel);
    expect(slow.lobby.stalled_player == 0, "120 s of silence is within a 120 s timeout");
    clock_ticks = heard + 120U * 30U + 1;
    (void)mp::lobby_tick(slow.lobby, slow.panel);
    expect(slow.lobby.stalled_player == kRoomGuest, "past it the player is named");
}

/// Steps the test's clock one tick, over to 0 past its largest reading.
void step_clock() {
    clock_ticks = clock_ticks == kLargestReading ? 0 : clock_ticks + 1;
}

// Across the turn of the lobby's clock the status block goes on: one due
// past the turn runs at the first reading after it, and the next sixty
// ticks after that.
void test_status_across_the_clock_turn() {
    using oa::netgame::RecordType;
    const auto saved_clock = clock_ticks;
    clock_ticks = kLargestReading - 30;
    Room room(false);
    room.lobby.next_stats_tick = 0;
    (void)mp::lobby_tick(room.lobby, room.panel);
    expect(room.sent(RecordType::ping).size() == 2, "the block runs 30 ticks before the turn");
    room.loopback.sent_count = 0;
    clock_ticks = kLargestReading;
    (void)mp::lobby_tick(room.lobby, room.panel);
    expect(room.sent(RecordType::ping).empty(), "and not again before the turn");
    clock_ticks = 5;
    (void)mp::lobby_tick(room.lobby, room.panel);
    expect(
        room.sent(RecordType::ping).size() == 2 && room.sent(RecordType::player_info).size() == 2,
        "the block runs again after the turn"
    );
    room.loopback.sent_count = 0;
    clock_ticks = 65;
    (void)mp::lobby_tick(room.lobby, room.panel);
    expect(room.sent(RecordType::ping).empty(), "not within sixty ticks of that");
    clock_ticks = 66;
    (void)mp::lobby_tick(room.lobby, room.panel);
    expect(room.sent(RecordType::ping).size() == 2, "and again sixty ticks on");
    clock_ticks = saved_clock;
}

// A player heard from just before the clock turns over is silent only from
// then: the stall scan names it once the timeout has passed across the turn.
void test_stall_scan_across_the_clock_turn() {
    constexpr uint16_t kPaused = 0x01; // Game.sim_run_flags: the game is paused
    const auto saved_clock = clock_ticks;
    clock_ticks = kLargestReading - 100;
    Room room(true);
    auto& lobby = room.lobby;
    auto& game = *room.game;
    auto& guest = mp::slot_player(lobby, mp::slot_for_player_id(lobby, kRoomGuest));
    game.sim_run_flags |= kPaused;
    (void)mp::lobby_tick(lobby, room.panel);
    game.sim_run_flags &= static_cast<uint16_t>(~kPaused);
    expect(lobby.timeout_baseline == clock_ticks, "the game pauses 100 ticks before the turn");
    clock_ticks = kLargestReading - 10;
    inject_probe(room.loopback, kRoomGuest);
    (void)mp::lobby_tick(lobby, room.panel);
    expect(
        guest.last_update_time == clock_ticks && lobby.stalled_player == 0,
        "the guest is heard 10 ticks before the turn"
    );
    clock_ticks = 20;
    (void)mp::lobby_tick(lobby, room.panel);
    expect(lobby.stalled_player == 0, "30 ticks later, across the turn, it is not silent");
    clock_ticks = 890;
    (void)mp::lobby_tick(lobby, room.panel);
    expect(lobby.stalled_player == 0, "silence for the timeout across the turn is not yet a stall");
    clock_ticks = 891;
    (void)mp::lobby_tick(lobby, room.panel);
    expect(lobby.stalled_player == kRoomGuest, "past it the player is named");
    clock_ticks = saved_clock;
}

// The doors over START open a frame every four ticks across the turn of the
// clock, as before it, to their last frame.
void test_start_doors_across_the_clock_turn() {
    const auto saved_clock = clock_ticks;
    clock_ticks = kLargestReading - 40;
    Fixture f(doors_layout());
    auto* doors = mp::panel_control(f.panel, "battlestart");
    f.join(0x200, "Guest");
    f.remote_info(0x200, mp::option::ready, 1);
    mp::lobby_player_count(*f.game) = 2;
    expect(f.press("READY0"), "READY0 clickable");
    clock_ticks = kLargestReading - 6;
    f.game->gui_flags |= 1;
    (void)mp::lobby_tick(f.lobby, f.panel);
    expect(doors->stage == 1, "the doors start to open 6 ticks before the turn");
    std::vector<uint32_t> steps; // the readings the doors stepped at
    for (int frame = 0; frame < 30; ++frame) {
        const auto stage = doors->stage;
        step_clock();
        (void)mp::lobby_tick(f.lobby, f.panel);
        if (doors->stage != stage)
            steps.push_back(clock_ticks);
    }
    const std::vector<uint32_t> expected{kLargestReading - 5, kLargestReading - 1, 0, 5, 9, 13, 17};
    expect(steps == expected, "a step every four ticks, and at the first reading after the turn");
    expect(doors->stage == 8, "the doors open to their last frame across the turn");
    clock_ticks = saved_clock;
}

// A computer player seated just before the clock turns over still takes no
// click for 31 ticks, counted across the turn.
void test_computer_guard_across_the_clock_turn() {
    namespace gui = oa::ui::gui_layout;
    const auto saved_clock = clock_ticks;
    clock_ticks = kLargestReading - 40;
    auto layout = doors_layout();
    gui::Gadget player;
    player.common.type = gui::GadgetType::button;
    player.common.name = "PLAYERx";
    player.common.x = 20;
    player.common.y = 71;
    player.common.width = 150;
    player.common.height = 16;
    player.common.active = 1;
    player.fields = gui::ButtonFields{};
    layout.gadgets.push_back(player);
    Fixture f(layout);
    f.loopback.next_player_id = 0x101; // fixture manually seated the host at 0x100
    auto& slot = mp::slot_player(f.lobby, 2);
    clock_ticks = kLargestReading - 5;
    expect(f.press("PLAYER2") && f.press("PLAYER2"), "PLAYER2 blocks, then adds a computer");
    mp::LobbyEvent arrival{};
    expect(
        f.lobby.net.receive(f.lobby.net.context, &arrival) &&
            mp::lobby_apply_event(f.lobby, arrival),
        "computer session arrival"
    );
    expect(
        slot.status == mp::kSlotComputer && slot.in_use != 0,
        "a computer player is seated 5 ticks before the turn"
    );
    clock_ticks = 25;
    expect(f.press("PLAYER2"), "PLAYER2 clickable as computer");
    expect(
        slot.status == mp::kSlotComputer && slot.in_use != 0,
        "30 ticks later, across the turn, a click keeps it"
    );
    clock_ticks = 26;
    expect(f.press("PLAYER2"), "PLAYER2 clickable as computer again");
    expect(slot.status == mp::kSlotOpen && slot.in_use == 0, "31 ticks later the click rejects it");
    clock_ticks = saved_clock;
}

// TIMEOUT.GUI's countdown runs across the turn of the clock: it is redrawn
// every two ticks, and rejects the player at the timeout plus 120 s counted
// from its last word before the turn.
void test_timeout_dialog_across_the_clock_turn() {
    const auto saved_clock = clock_ticks;
    clock_ticks = kLargestReading - 10;
    Room room(true);
    auto& lobby = room.lobby;
    auto& guest = mp::slot_player(lobby, mp::slot_for_player_id(lobby, kRoomGuest));
    guest.last_update_time = clock_ticks;
    mp::Panel dialog;
    expect(mp::timeout_open(lobby, dialog, kRoomGuest), "TIMEOUT opens for the silent player");
    clock_ticks = kLargestReading - 1;
    (void)mp::timeout_tick(lobby, dialog);
    dialog.dirty = false;
    clock_ticks = kLargestReading;
    expect(
        !mp::timeout_tick(lobby, dialog) && !dialog.dirty,
        "the countdown is not redrawn within two ticks"
    );
    clock_ticks = 20;
    expect(
        !mp::timeout_tick(lobby, dialog) && room.rejects().empty(),
        "30 ticks after the guest was heard, across the turn, no one is rejected"
    );
    expect(dialog.dirty, "and the countdown is redrawn after the turn");
    const uint32_t drop = (30U + 120U) * 30U;
    clock_ticks = drop - 11;
    expect(
        !mp::timeout_tick(lobby, dialog) && room.rejects().empty(),
        "short of the timeout plus 120 s across the turn, no one is rejected"
    );
    clock_ticks = drop - 10;
    const auto rejects_at_drop = mp::timeout_tick(lobby, dialog);
    const auto rejects = room.rejects();
    expect(
        rejects_at_drop && rejects.size() == 1 && rejects[0].player_id == kRoomGuest &&
            rejects[0].reason == 6,
        "at the timeout plus 120 s across the turn the player is rejected"
    );
    clock_ticks = saved_clock;
}

// RESTRICT2.GUI loads a row's picture every two ticks across the turn of the clock.
void test_restrict_pictures_across_the_clock_turn() {
    const auto saved_clock = clock_ticks;
    clock_ticks = kLargestReading - 1;
    Room room(true);
    auto restrict = std::make_unique<mp::RestrictPanel>();
    mp::Panel panel;
    mp::restrict_tick(room.lobby, * restrict, panel);
    expect(restrict->picture_rows == 1, "a row's picture loads a tick before the turn");
    clock_ticks = kLargestReading;
    mp::restrict_tick(room.lobby, * restrict, panel);
    expect(restrict->picture_rows == 1, "none the next tick");
    clock_ticks = 0;
    mp::restrict_tick(room.lobby, * restrict, panel);
    expect(restrict->picture_rows == 2, "the next at the first reading after the turn");
    clock_ticks = 2;
    mp::restrict_tick(room.lobby, * restrict, panel);
    expect(restrict->picture_rows == 2, "none within two ticks of that");
    clock_ticks = 3;
    mp::restrict_tick(room.lobby, * restrict, panel);
    expect(restrict->picture_rows == 3, "and the next after two");
    clock_ticks = saved_clock;
}

int main(int argc, char** argv) {
    if (!oa::test::game_data_requested(argc, argv)) {
        test_unit_sync_and_restrictions();
        test_unit_sync_marks_units();
        test_map_memory();
        test_fit_map_picture();
        test_map_summary();
        test_map_select_preview();
        test_view_map_follows_host();
        test_open_service();
        test_launch_connection();
        test_add_player_and_description();
        test_client_unit_sync();
        test_unit_sync_reevaluation();
        test_machine_groups();
        test_alliance_relation();
        test_damaged_records();
        test_resolution_cycle();
        test_records_leave_from_their_players();
        test_periodic_block();
        test_host_leaving();
        test_reject_passed_on_once();
        test_rejection_order();
        test_refused_joiner();
        test_player_timeout();
        test_status_across_the_clock_turn();
        test_stall_scan_across_the_clock_turn();
        test_start_doors_across_the_clock_turn();
        test_computer_guard_across_the_clock_turn();
        test_timeout_dialog_across_the_clock_turn();
        test_restrict_pictures_across_the_clock_turn();
        test_session_description();
        test_versioned_rules();
        test_recorder_in_the_battle_room();
        test_no_presence_bytes_in_the_battle_room();
        test_presence_records_in_the_battle_room();
        test_rules_dot();
        test_mutator_rules_dot();
        test_content_difference();
        test_unicode_chat_in_the_battle_room();
        test_recorder_commands_in_the_battle_room();
        test_recorder_prebuilt_base_in_the_battle_room();
        test_hot_surfaces();
        test_slider_binding();
        test_slider_input();
        test_list_scrolling();
        test_start_doors(doors_layout());
    } else {
        const auto assets = oa::test::require_game_assets("the lobby over the installed GUI files");
        const auto lounge = oa::ui::gui_layout::parse(read_gui(assets, "lounge2.gui"));
        expect(lounge.ok(), "the install's LOUNGE2.GUI parses");
        const auto common =
            oa::formats::gaf::parse(oa::test::read_game_file(assets, "anims/commongui.gaf"));
        expect(common.ok(), "the install's COMMONGUI.GAF parses");
        if (common.ok())
            for (const auto& sequence : common.archive->sequences)
                if (sequence.name == "SLIDERS")
                    for (const auto& frame : sequence.frames)
                        slider_art.shared.push_back(
                            {static_cast<int16_t>(frame.width), static_cast<int16_t>(frame.height)}
                        );
        expect(!slider_art.shared.empty(), "COMMONGUI.GAF has the SLIDERS art");
        if (lounge.ok()) {
            test_rows_and_colors(*lounge.layout);
            test_side_and_watch(*lounge.layout);
            test_three_sides(*lounge.layout);
            test_side_steps_back(*lounge.layout);
            test_hidden_side(*lounge.layout);
            test_computer_player_sides(*lounge.layout);
            test_teams_and_alliances(*lounge.layout);
            test_options_and_sliders(*lounge.layout);
            test_lounge_sliders(*lounge.layout);
            test_ready_and_start(*lounge.layout);
            test_start_doors(*lounge.layout);
            test_row_indicators(*lounge.layout);
            test_slot_cycle(*lounge.layout);
            test_computer_rows(*lounge.layout);
            test_computer_brains(*lounge.layout);
            test_chat_and_leave(*lounge.layout);
            test_mod_lobby_buttons(*lounge.layout);
            test_client_rules(*lounge.layout);
            test_browsing_behind_a_dialog(*lounge.layout);
            test_chat_commands(*lounge.layout);
            test_ping_column(*lounge.layout);
            test_closing_slots_republishes(*lounge.layout);
            test_launch_battleroom(*lounge.layout);
            test_launch_seats(*lounge.layout);
            test_launch_side_names(*lounge.layout);
        }
        test_game_list(assets);
        test_launch_screens(assets);
    }
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("multiplayer lobby tests passed");
    return 0;
}

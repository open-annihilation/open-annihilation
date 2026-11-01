// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The battle room under a mod profile's setup and team rules, with the
// loopback net: the start gate, several computer players, their names,
// Allied Victory, team records, alliance requests, resent alliances, the
// chat commands and, with --data over the installed LOUNGE2.GUI, the
// neutral computer player START seats. Every case of a rule also runs
// without it, where the battle room keeps 3.1c's behaviour; the start gate
// runs with the retired hack on and off, which changes nothing.
#include "oa/netgame/records.hpp"
#include "oa/ui/frontend_multiplayer/lobby.hpp"
#include "oa/ui/frontend_multiplayer/team_rules.hpp"
#include "oa/test/game_assets.hpp"

#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

namespace mp = oa::ui::frontend_multiplayer;
namespace rules = oa::data::match_rules;
namespace tr = oa::ui::frontend_multiplayer::team_rules;

namespace {

int failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        std::fprintf(stderr, "FAILED: %s\n", what);
        ++failures;
    }
}

uint32_t clock_ticks = 1000;
std::string last_message;

uint32_t tick(void*) {
    return clock_ticks;
}

void message(void*, const char* text) {
    last_message = text;
}

uint32_t first_draw(void*, uint32_t) {
    return 0;
}

/// The selected map of a test that seats a neutral computer player.
oa::data::campaign::CampaignFile neutral_map{};
oa::data::campaign::MissionUnit neutral_units[2]{};

const oa::data::campaign::CampaignFile* map_context(void*) {
    return &neutral_map;
}

struct Fixture {
    std::unique_ptr<oa::Game> game = std::make_unique<oa::Game>();
    mp::Lobby lobby{};
    mp::LoopbackNet loopback{};
    rules::MatchRules match_rules{};

    explicit Fixture(bool with_rules, bool host = true) {
        mp::loopback_reset(loopback);
        mp::lobby_reset(lobby, *game);
        lobby.net = mp::loopback_lobby_net(loopback);
        lobby.services.message = message;
        lobby.services.tick = tick;
        lobby.services.random_below = first_draw;
        lobby.local_version_major = 3;
        if (with_rules)
            lobby.rules = &match_rules;
        game->session_flags |= 1;
        mp::lobby_max_units(*game) = 250;
        loopback.opened = true;
        game->players[0].player_id = 0x100;
        mp::lobby_seat_local(lobby, 0, host, "Host");
        loopback.next_player_id = 0x500;
    }

    void join(uint32_t id, const char* player) {
        mp::LobbyEvent event{};
        event.kind = mp::LobbyEventKind::player_joined;
        event.player_id = id;
        std::snprintf(event.name, sizeof(event.name), "%s", player);
        expect(mp::lobby_apply_event(lobby, event), "join event applies");
    }

    template <class Record>
    bool receive(uint32_t from, const Record& record) {
        mp::LobbyEvent event{};
        event.kind = mp::LobbyEventKind::record;
        event.player_id = from;
        std::size_t written = 0;
        (void)oa::netgame::encode_record(record, event.data, sizeof(event.data), &written);
        event.size = static_cast<uint16_t>(written);
        return mp::lobby_apply_event(lobby, event);
    }

    /// The alliance records sent from the start, in order.
    std::vector<oa::netgame::AllianceRecord> alliances() const {
        std::vector<oa::netgame::AllianceRecord> found;
        for (int32_t i = 0; i < loopback.sent_count; ++i)
            if (loopback.sent[i][0] == static_cast<uint8_t>(oa::netgame::RecordType::alliance)) {
                oa::netgame::AllianceRecord record{};
                if (oa::netgame::decode_record(loopback.sent[i], loopback.sent_size[i], &record) ==
                    oa::netgame::WireError::ok)
                    found.push_back(record);
            }
        return found;
    }

    /// The team records sent from the start, in order.
    std::vector<oa::netgame::PlayerTeamRecord> teams() const {
        std::vector<oa::netgame::PlayerTeamRecord> found;
        for (int32_t i = 0; i < loopback.sent_count; ++i)
            if (loopback.sent[i][0] == static_cast<uint8_t>(oa::netgame::RecordType::player_team)) {
                oa::netgame::PlayerTeamRecord record{};
                if (oa::netgame::decode_record(loopback.sent[i], loopback.sent_size[i], &record) ==
                    oa::netgame::WireError::ok)
                    found.push_back(record);
            }
        return found;
    }

    /// The last chat line posted here.
    std::string last_chat() {
        const auto head = mp::lobby_chat_head(*game);
        return mp::lobby_chat_line(*game, static_cast<std::size_t>(head + mp::kChatLines - 1));
    }
};

/// The name the loopback session was asked to seat last.
std::string last_seated_name(const mp::LoopbackNet& loopback) {
    for (int32_t i = loopback.queue_count; i > 0; --i) {
        const auto& event = loopback.queue[(loopback.queue_head + i - 1) % 32];
        if (event.kind == mp::LobbyEventKind::player_joined)
            return event.name;
    }
    return {};
}

void test_start_with_computers() {
    for (const bool on : {false, true}) {
        // The retired setup.allow-start-with-ai, on or off, changes nothing.
        Fixture f(true);
        f.match_rules.setup.allow_start_with_ai.enabled = on;
        mp::local_info(f.lobby).options |= mp::option::ready;
        mp::lobby_player_count(*f.game) = 2;
        expect(!mp::lobby_ready_to_start(f.lobby), "one human alone does not start");
        mp::lobby_add_computer(f.lobby, 1);
        auto& computer = mp::slot_player(f.lobby, 1);
        computer.in_use = 1;
        mp::lobby_player_count(*f.game) = 3;
        expect(!mp::lobby_ready_to_start(f.lobby), "the computer player is not ready yet");
        mp::slot_info(f.lobby, 1)->options |= mp::option::ready;
        expect(mp::lobby_ready_to_start(f.lobby), "one human and a computer player may start");
        // Options a launch locked keep 3.1c's wait for a remote player.
        mp::lobby_option_word(*f.game) = static_cast<uint16_t>(mp::lobby_option_word(*f.game) | 1U);
        expect(mp::lobby_options_locked(f.lobby), "the options are locked");
        expect(!mp::lobby_ready_to_start(f.lobby), "locked options wait for a remote player");
        f.join(0x200, "Guest");
        mp::PlayerSetupInfo* guest = nullptr;
        for (int32_t slot = 0; slot < mp::kSlotCount; ++slot)
            if (mp::slot_player(f.lobby, slot).player_id == 0x200)
                guest = mp::slot_info(f.lobby, slot);
        expect(guest != nullptr, "the guest is seated");
        if (guest != nullptr)
            guest->options |= mp::option::ready;
        mp::lobby_player_count(*f.game) = 4;
        expect(mp::lobby_ready_to_start(f.lobby), "a ready remote player starts a locked room");
    }
    Fixture base(false);
    mp::local_info(base.lobby).options |= mp::option::ready;
    mp::lobby_add_computer(base.lobby, 1);
    mp::slot_player(base.lobby, 1).in_use = 1;
    mp::slot_info(base.lobby, 1)->options |= mp::option::ready;
    mp::lobby_player_count(*base.game) = 3;
    expect(mp::lobby_ready_to_start(base.lobby), "without a profile one human may start too");
}

void test_computer_names() {
    Fixture base(false);
    mp::lobby_add_computer(base.lobby, 3);
    expect(last_seated_name(base.loopback) == "AI:Host", "3.1c names the computer AI:<name>");

    Fixture f(true);
    f.match_rules.setup.ai_player_name_format.enabled = true;
    f.match_rules.setup.ai_player_name_format.format.assign("AI:%s %d");
    mp::lobby_add_computer(f.lobby, 3);
    expect(last_seated_name(f.loopback) == "AI:Host 3", "the profile's format with the slot");
    f.match_rules.setup.ai_player_name_format.format.assign("AI:%s");
    mp::lobby_add_computer(f.lobby, 4);
    expect(last_seated_name(f.loopback) == "AI:Host", "the baseline format names as 3.1c does");
    std::snprintf(
        mp::local_player(f.lobby).name,
        sizeof mp::local_player(f.lobby).name,
        "%s",
        "AVeryLongPlayerName"
    );
    f.match_rules.setup.ai_player_name_format.format.assign("AI:%s %d");
    mp::lobby_add_computer(f.lobby, 5);
    expect(last_seated_name(f.loopback) == "AI:AVeryLongPlay", "cut to 16 characters");
}

void test_allied_victory() {
    for (const bool kept : {false, true}) {
        Fixture f(true);
        f.match_rules.teams.allied_victory_kept.enabled = kept;
        mp::lobby_player_team(mp::local_player(f.lobby)) = 1;
        mp::local_info(f.lobby).status |= mp::status::allied_victory;
        mp::lobby_update_ally_matrix(f.lobby);
        const bool set = (mp::local_info(f.lobby).status & mp::status::allied_victory) != 0;
        expect(set == kept, kept ? "a lone team keeps Allied Victory" : "3.1c clears it");
    }
}

void test_team_records() {
    // 3.1c stores the team and changes no alliance.
    Fixture base(false);
    base.join(0x200, "Guest");
    mp::lobby_player_team(mp::local_player(base.lobby)) = 1;
    oa::netgame::PlayerTeamRecord team{};
    team.player_id = 0x200;
    team.value = 1;
    const auto before = base.loopback.sent_count;
    expect(base.receive(0x200, team), "team record applies");
    expect(mp::lobby_player_team(mp::slot_player(base.lobby, 1)) == 1, "3.1c stores the team");
    expect(base.loopback.sent_count == before, "3.1c sends nothing for it");

    Fixture f(true);
    f.match_rules.teams.team_number_alliances.enabled = true;
    f.match_rules.teams.team_number_alliances.bit7_keeps_alliances = true;
    f.join(0x200, "Guest");
    mp::lobby_player_team(mp::local_player(f.lobby)) = 1;
    // The join's blocks already went out with their alliances.
    f.loopback.sent_count = 0;
    expect(f.receive(0x200, team), "team record applies");
    expect(mp::lobby_player_team(mp::slot_player(f.lobby, 1)) == 1, "the team is stored");
    expect(mp::local_player(f.lobby).alliance[1] == 1, "the local player allies its team-mate");
    const auto sent = f.alliances();
    expect(sent.size() == 2, "the alliance and the request go out");
    expect(
        sent.size() == 2 && sent[0].player_id_a == 0x100 && sent[0].player_id_b == 0x200 &&
            sent[0].value == 1 && sent[0].both_sides == 0,
        "the local player's alliance is announced"
    );
    expect(
        sent.size() == 2 && sent[1].player_id_a == 0x200 && sent[1].player_id_b == 0x100 &&
            sent[1].value == 1 && sent[1].both_sides == tr::alliance_request,
        "the guest's machine is asked to ally back"
    );

    // Bit 7 stores the team and changes nothing else.
    const auto count = f.loopback.sent_count;
    team.value = 0x82;
    expect(f.receive(0x200, team), "team record with bit 7 applies");
    expect(mp::lobby_player_team(mp::slot_player(f.lobby, 1)) == 2, "bit 7: team 2 stored");
    expect(f.loopback.sent_count == count, "bit 7: no alliance moves");
}

void test_alliance_requests() {
    for (const bool on : {false, true}) {
        Fixture f(true);
        f.match_rules.teams.team_number_alliances.enabled = on;
        f.join(0x200, "Guest");
        oa::netgame::AllianceRecord request{};
        request.player_id_a = 0x100;
        request.player_id_b = 0x200;
        request.value = 1;
        request.both_sides = tr::alliance_request;
        f.loopback.sent_count = 0;
        const auto before = f.loopback.sent_count;
        expect(f.receive(0x200, request), "request applies");
        const auto sent = f.alliances();
        if (on) {
            expect(mp::local_player(f.lobby).alliance[1] == 1, "the request is carried out");
            expect(
                sent.size() == 1 && sent[0].player_id_a == 0x100 && sent[0].both_sides == 0,
                "and announced to everyone"
            );
        } else {
            expect(f.loopback.sent_count == before, "3.1c announces nothing");
        }
    }
}

void test_resent_alliances() {
    for (const auto mode :
         {rules::TeamsTeamNumberAlliancesLobbyRebroadcast::stored_matrix,
          rules::TeamsTeamNumberAlliancesLobbyRebroadcast::recompute_from_teams}) {
        Fixture f(true);
        f.match_rules.teams.team_number_alliances.enabled = true;
        f.match_rules.teams.team_number_alliances.lobby_rebroadcast = mode;
        f.join(0x200, "Guest");
        mp::lobby_player_team(mp::local_player(f.lobby)) = 0;
        mp::lobby_player_team(mp::slot_player(f.lobby, 1)) = 0;
        mp::local_player(f.lobby).alliance[1] = 0;
        f.loopback.sent_count = 0;
        mp::lobby_send_player_info(f.lobby);
        const auto sent = f.alliances();
        const uint8_t wanted =
            mode == rules::TeamsTeamNumberAlliancesLobbyRebroadcast::stored_matrix ? 0 : 1;
        expect(
            sent.size() == 1 && sent[0].player_id_b == 0x200 && sent[0].value == wanted,
            "the block is followed by the player's alliance as the mode decides"
        );
    }
    Fixture base(false);
    base.join(0x200, "Guest");
    base.loopback.sent_count = 0;
    mp::lobby_send_player_info(base.lobby);
    expect(base.alliances().empty(), "3.1c sends no alliance with the block");
}

void test_commands() {
    Fixture base(false);
    expect(!mp::lobby_run_setup_command(base.lobby, "+autoteam"), "3.1c has no +autoteam");

    Fixture f(true);
    f.match_rules.teams.team_number_alliances.enabled = true;
    f.match_rules.setup.map_scripted_units.enabled = true;
    f.join(0x200, "Guest");
    f.join(0x201, "Other");
    f.join(0x202, "Third");
    mp::local_player(f.lobby).machine_group = 1;
    mp::local_player(f.lobby).alliance[1] = 1;
    f.loopback.sent_count = 0;
    expect(mp::lobby_run_setup_command(f.lobby, "+AutoTeam 2"), "+autoteam runs, any case");
    expect(f.last_chat() != "", "a notice is posted");
    const auto teams = f.teams();
    expect(teams.size() == 4, "a team record per counted player");
    bool flagged = !teams.empty();
    for (const auto& record : teams)
        flagged = flagged && (record.value & tr::team_keeps_alliances) != 0;
    expect(flagged, "dealt teams go out with the keep-alliances bit");
    expect(
        mp::local_player(f.lobby).alliance[1] == 0 || mp::local_player(f.lobby).alliance[1] == 1,
        "alliances are dealt"
    );

    Fixture guest(true, false);
    guest.match_rules.teams.team_number_alliances.enabled = true;
    guest.lobby.game->players[0].machine_group = 2;
    expect(mp::lobby_run_setup_command(guest.lobby, "+randomteam"), "+randomteam is a command");
    expect(guest.last_chat() == "+randomteam can only be used by host", "only the host deals");

    expect(mp::lobby_run_setup_command(f.lobby, "+spawnoff"), "+spawnoff runs");
    expect(!f.lobby.map_units_on, "+spawnoff turns map units off");
    expect(f.last_chat() == "Unit spawn is disabled ...", "+spawnoff notice");
    expect(mp::lobby_run_setup_command(f.lobby, "+spawnon"), "+spawnon runs");
    expect(f.lobby.map_units_on, "+spawnon turns them on");
    expect(!mp::lobby_run_setup_command(f.lobby, "+autoteamx"), "a longer word is chat");
}

/// The lounge of the installed game, for the panel cases.
struct PanelFixture : Fixture {
    mp::Panel panel;
    oa::ui::gui_layout::Layout layout;

    PanelFixture(const oa::ui::gui_layout::Layout& lounge, bool with_rules)
        : Fixture(with_rules), layout(lounge) {
        mp::panel_load(panel, "guis/lounge2.gui", layout);
        mp::lobby_enter_battleroom(lobby, panel);
    }

    /// Clicks a control, then lets the battle room take what the session sent.
    void press(const char* control) {
        if (auto* pressed = mp::panel_control(panel, control)) {
            pressed->active = 1;
            pressed->grayed = false;
        }
        if (mp::panel_press(panel, control, 1))
            (void)mp::lobby_handle_event(lobby, panel);
        (void)mp::lobby_tick(lobby, panel);
    }

    int32_t computers() {
        int32_t count = 0;
        for (int32_t slot = 0; slot < mp::kSlotCount; ++slot)
            count += mp::slot_player(lobby, slot).status == mp::kSlotComputer ? 1 : 0;
        return count;
    }
};

void test_several_computers(const oa::ui::gui_layout::Layout& lounge) {
    for (const bool on : {false, true}) {
        PanelFixture f(lounge, true);
        f.match_rules.setup.multiple_local_ai.enabled = on;
        // A blocked slot clicked again seats a computer player.
        for (const char* slot : {"PLAYER1", "PLAYER1", "PLAYER2", "PLAYER2"})
            f.press(slot);
        expect(
            f.computers() == 2,
            on ? "the hack still seats two computer players"
               : "each human seats more than one computer player"
        );
    }
}

void test_neutral_computer(const oa::ui::gui_layout::Layout& lounge) {
    neutral_units[0].unit_name = "ARMSOLAR";
    neutral_units[0].player = 1;
    neutral_units[1].unit_name = "CORGATOR";
    neutral_units[1].player = 11;
    neutral_map.units = neutral_units;
    neutral_map.unit_count = 2;
    for (const bool on : {false, true}) {
        PanelFixture f(lounge, true);
        f.match_rules.setup.map_scripted_units.enabled = on;
        f.match_rules.setup.map_scripted_units.auto_neutral_ai = on;
        f.lobby.maps.map_context = map_context;
        f.press("START");
        expect(
            f.computers() == (on ? 1 : 0),
            on ? "START seats a computer player for neutral units" : "3.1c seats none"
        );
        if (on) {
            expect(
                f.last_chat() == "Use +spawnoff to disable extra unit spawn in general",
                "the notices are posted"
            );
            f.press("START");
            expect(f.computers() == 1, "only once per battle room");
        }
    }
    PanelFixture off(lounge, true);
    off.match_rules.setup.map_scripted_units.enabled = true;
    off.match_rules.setup.map_scripted_units.auto_neutral_ai = true;
    off.lobby.maps.map_context = map_context;
    (void)mp::lobby_run_setup_command(off.lobby, "+spawnoff");
    off.press("START");
    expect(off.computers() == 0, "+spawnoff seats none");
}

} // namespace

int main(int argc, char** argv) {
    if (!oa::test::game_data_requested(argc, argv)) {
        test_start_with_computers();
        test_computer_names();
        test_allied_victory();
        test_team_records();
        test_alliance_requests();
        test_resent_alliances();
        test_commands();
    } else {
        const auto assets = oa::test::require_game_assets("the battle room's setup rules");
        const auto lounge =
            oa::ui::gui_layout::parse(oa::test::read_game_file(assets, "guis/lounge2.gui"));
        expect(lounge.ok(), "the install's LOUNGE2.GUI parses");
        if (lounge.ok()) {
            test_several_computers(*lounge.layout);
            test_neutral_computer(*lounge.layout);
        }
    }
    if (failures != 0) {
        std::fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    std::puts("battle room setup rules tests passed");
    return 0;
}

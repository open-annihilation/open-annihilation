// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The console's network commands against the running app. The share toggles
// re-send the local players' lobby blocks through the network match, "Page"
// pages through the extension built on network play while a launch is
// active; "Compression" and "Senderror" are read by the packet layer's sender
// and "Drop" by the stall check.
#include "oa/app/runtime.hpp"
#include "oa/base/threads.hpp"
#include "network_play.hpp"
#include "launch_binding.hpp"
#include "net_state.hpp"

#include "oa/sim/match_runtime.hpp"
#include "oa/netgame/console/console_commands.hpp"
#include "oa/netgame/match/net_match.hpp"
#include "oa/netgame/match/session_lobby.hpp"
#include "oa/ui/console/console.hpp"
#include "oa/ui/console/game_fields.hpp"

#include <SDL3/SDL.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace oa::app {

namespace console = oa::ui::console;
namespace nm = oa::netgame::match;

namespace {

constexpr uint16_t kAllShareFlags = console::share_flag::metal | console::share_flag::energy |
                                    console::share_flag::mapping | console::share_flag::radar;
constexpr int kSessionWaitSteps = 200;
constexpr int kLossSteps = 10;
constexpr int kCompressionSteps = 3;
// Longer than two transport time ticks (1/30 s each).
constexpr uint32_t kSilence = 100;

NetworkPlay* play_of(void* context) noexcept {
    return static_cast<NetworkPlay*>(context);
}

bool logged(const std::vector<std::string>& lines, std::string_view text) {
    return std::find(lines.begin(), lines.end(), text) != lines.end();
}

bool logged_part(const std::vector<std::string>& lines, std::string_view part) {
    return std::any_of(lines.begin(), lines.end(), [part](const std::string& line) {
        return line.find(part) != std::string::npos;
    });
}

// The line a command posted: the one before its echo.
bool posted(const std::vector<std::string>& lines, std::string_view text) {
    return lines.size() >= 2 && lines[lines.size() - 2] == text;
}

std::string field_text(const char* field, std::size_t capacity) {
    return std::string(field, strnlen(field, capacity));
}

PlayerSetupInfo* local_lobby_info(oa::World& world) {
    auto& game = world.game;
    return game.local_player_index < OA_PLAYER_COUNT
               ? oa::world_player_info(&world, &game.players[game.local_player_index])
               : nullptr;
}

const Player* player_with_id(const oa::World& world, uint32_t net_id) {
    for (const auto& player : world.game.players)
        if (player.in_use != 0 && player.player_id == net_id)
            return &player;
    return nullptr;
}

} // namespace

void NetworkPlay::bind_console_network_hooks(console::ConsoleHost& host) {
    // The host's context is the runtime; the commands' is this network play.
    host.player_info_changed = [](void* context) {
        NetworkPlay::of(*static_cast<Runtime*>(context)).net_send_player_status();
    };
    oa::netgame::console::CommandHost& commands = net_->console_commands;
    commands.context = this;
    commands.reset_traffic_stats = [](void* context) {
        play_of(context)->net_reset_traffic_stats();
    };
    // Page sends while a launch is active and an extension pages
    // (oa::app::netgame::extension_api::Hooks::page); otherwise the console refuses it.
    commands.launch_active = [](void*) {
        return launch_active(nullptr) && extension_hooks().page != nullptr;
    };
    commands.send_page = [](void*, const char* user, const char* text) {
        const auto& hooks = extension_hooks();
        if (hooks.page != nullptr)
            hooks.page(hooks.context, user, text);
    };
    // teams.team-number-alliances: the host deals teams by start position.
    const auto* profile = runtime_.mod_profile();
    commands.deal_teams = nullptr;
    if (profile != nullptr && profile->rules.teams.team_number_alliances.enabled)
        commands.deal_teams =
            [](void* context, const char* argument, char* notice, std::size_t capacity) {
                auto& state = *play_of(context)->net_;
                if (!state.active || !state.net) {
                    std::snprintf(
                        notice,
                        capacity,
                        "+autoteam is only available to the host of a multiplayer game"
                    );
                    return;
                }
                nm::net_match_deal_teams(
                    state.net.get(),
                    oa::ui::frontend_multiplayer::team_rules::dealt_team_count(argument),
                    notice,
                    capacity
                );
            };
    commands.cast_vote = [](void* context, bool yes) {
        auto* play = play_of(context);
        const auto* state = play->session_net_match();
        return state != nullptr &&
               nm::net_match_cast_vote(play->net_->net.get(), state->vote_target, yes);
    };
    host.extension_context = &commands;
    host.extend = oa::netgame::console::register_console_commands;
    net_->console_host = &host;
}

// Outside a live network game the share toggles and "Compression" do
// nothing. "Drop" sets its bit for 0 (or no value) and clears it for
// anything else, "Page" says what it needs, and the developer command
// "Senderror" takes 0..100 from a single value, any other value as 0.
void NetworkPlay::check_console_network_commands(
    const std::function<void(const char*)>& enter_line
) {
    oa::World& world = runtime_.match_->state();
    oa::Game& game = world.game;
    const auto require = [](bool ok, const char* what) {
        if (!ok)
            throw std::runtime_error(std::string("console network check: ") + what);
    };
    require((game.session_flags & nm::kNetFlagLive) == 0, "the match is a network game");
    const auto* info = local_lobby_info(world);
    require(info != nullptr, "no local lobby block");
    const auto shares = console::share_flags(*info);
    const auto uncompressed = game.compression_off;
    for (const char* line :
         {"+sharemetal", "+shareenergy", "+sharemapping", "+shareradar", "+shareall"})
        enter_line(line);
    enter_line("+compression");
    require(console::share_flags(*info) == shares, "a share toggle changed the lobby block");
    require(game.compression_off == uncompressed, "+compression changed the send option");
    const auto lines = runtime_.match_message_lines();
    require(
        !logged_part(lines, "Toggled Share") && !logged_part(lines, "packet compression"),
        "a network toggle posted a notice"
    );

    const auto no_drop = [&] {
        return (console::console_flags(game) & console::console_flag::no_drop) != 0;
    };
    enter_line("+drop 1");
    require(!no_drop(), "+drop 1 left the no-drop bit set");
    enter_line("+drop 0");
    require(no_drop(), "+drop 0 did not set the no-drop bit");
    enter_line("+drop 7");
    require(!no_drop(), "+drop 7 did not clear the no-drop bit");
    enter_line("+drop");
    require(no_drop(), "+drop without a value did not set the no-drop bit");
    enter_line("+drop 1");

    enter_line("+page");
    require(
        posted(runtime_.match_message_lines(), "Syntax: page <user> <text>"),
        "+page posted no syntax line"
    );
    enter_line("+page someone hello there");
    require(
        posted(
            runtime_.match_message_lines(), "Page command requires game launch from the Boneyards."
        ),
        "+page did not ask for an active launch"
    );
    enter_line("+p someone hello");
    require(
        posted(
            runtime_.match_message_lines(), "Page command requires game launch from the Boneyards."
        ),
        "+p is not the page command"
    );

    enter_line("+Now Film Chris Include Reload Assert");
    enter_line("+senderror 30");
    require(game.send_error_percent == 30, "+senderror 30 did not set 30%");
    enter_line("+senderror 40 50");
    require(game.send_error_percent == 30, "+senderror with two values changed the loss");
    enter_line("+senderror 101");
    require(game.send_error_percent == 0, "+senderror 101 did not become 0");
    enter_line("+senderror 100");
    require(game.send_error_percent == 100, "+senderror 100 did not set 100%");
    enter_line("+senderror -1");
    require(game.send_error_percent == 0, "+senderror -1 did not become 0");
    std::cout << "console network check: share toggles and +compression idle offline, +drop "
                 "sets and clears its bit, +page and +p need an active launch, +senderror "
                 "keeps 0..100\n";
}

// The share toggles reach the peer's copy of this machine's lobby block,
// "Compression" sends every frame stored, "Senderror 100" drops every frame
// before the transport (still counted as sent) and "Drop 0" keeps the stall
// check from naming a silent peer.
void NetworkPlay::check_console_network_session(
    Runtime& peer, const std::function<void(bool)>& step
) {
    const auto require = [](bool ok, const std::string& what) {
        if (!ok)
            throw std::runtime_error("console network session check: " + what);
    };
    const nm::NetMatch* match =
        session_net_match() != nullptr ? session_net_match()->net.get() : nullptr;
    require(match != nullptr && runtime_.match_ && peer.match_, "no network match");
    oa::World& world = runtime_.match_->state();
    oa::Game& game = world.game;
    const oa::World& peer_world = peer.match_->state();
    require((game.session_flags & nm::kNetFlagLive) != 0, "the game is not live");
    const Player& local = game.players[game.local_player_index];
    const auto local_id = local.player_id;
    const auto* peer_match = NetworkPlay::of(peer).session_net_match() != nullptr
                                 ? NetworkPlay::of(peer).session_net_match()->net.get()
                                 : nullptr;
    require(peer_match != nullptr, "the peer has no network match");
    const auto peer_id = peer_world.game.players[peer_world.game.local_player_index].player_id;
    PlayerSetupInfo* info = local_lobby_info(world);
    require(info != nullptr, "no local lobby block");
    const auto copy_on_peer = [&]() -> const PlayerSetupInfo* {
        const Player* player = player_with_id(peer_world, local_id);
        return player != nullptr ? oa::world_player_info(&peer_world, player) : nullptr;
    };
    const auto wait = [&](const auto& done, const char* what) {
        for (int i = 0; i < kSessionWaitSteps && !done(); ++i)
            step(true);
        require(done(), what);
    };
    const auto shares = [](const PlayerSetupInfo* block) {
        return block != nullptr
                   ? static_cast<uint16_t>(console::share_flags(*block) & kAllShareFlags)
                   : uint16_t{0xffff};
    };

    const uint16_t original = shares(info);

    struct Toggle {
        const char* line;
        const char* name;
        uint16_t bit;
    };

    const Toggle toggles[] = {
        {"+sharemetal", "ShareMetal", console::share_flag::metal},
        {"+shareenergy", "ShareEnergy", console::share_flag::energy},
        {"+sharemapping", "ShareMapping", console::share_flag::mapping},
        {"+shareradar", "ShareRadar", console::share_flag::radar},
    };
    uint16_t expected = original;
    for (const auto& toggle : toggles) {
        runtime_.enter_console_check_line(toggle.line);
        expected = static_cast<uint16_t>(expected ^ toggle.bit);
        require(shares(info) == expected, std::string(toggle.line) + " did not toggle its bit");
        const std::string notice = std::string("Toggled ") + toggle.name +
                                   " to: " + ((expected & toggle.bit) != 0 ? "ON" : "OFF");
        require(
            logged(runtime_.match_message_lines(), notice),
            std::string(toggle.line) + " posted no notice"
        );
        wait(
            [&] { return shares(copy_on_peer()) == expected; },
            "a share toggle did not reach the peer"
        );
    }
    runtime_.enter_console_check_line("+shareall");
    require(shares(info) == original, "+shareall did not toggle all four bits");
    wait([&] { return shares(copy_on_peer()) == original; }, "+shareall did not reach the peer");

    const auto& traffic = match->connection->packets->traffic;
    const std::string compressible(60, 'a');
    const auto send_chat = [&] {
        const auto raw = traffic.sent_bytes;
        const auto wire = traffic.condensed_bytes;
        net_say(compressible);
        for (int i = 0; i < kCompressionSteps; ++i)
            step(true);
        return std::pair{traffic.sent_bytes - raw, traffic.condensed_bytes - wire};
    };
    runtime_.enter_console_check_line("+compression");
    require(game.compression_off != 0, "+compression did not turn compression off");
    require(
        posted(runtime_.match_message_lines(), "Ok.  Outgoing packet compression turned OFF"),
        "+compression posted no OFF notice"
    );
    const auto [stored_raw, stored_wire] = send_chat();
    require(
        stored_raw != 0 && stored_wire == stored_raw, "a frame was compressed with compression off"
    );
    runtime_.enter_console_check_line("+compression");
    require(game.compression_off == 0, "+compression did not turn compression on");
    require(
        posted(runtime_.match_message_lines(), "Ok.  Outgoing packet compression turned ON"),
        "+compression posted no ON notice"
    );
    const auto [packed_raw, packed_wire] = send_chat();
    require(
        packed_raw != 0 && packed_wire < packed_raw, "the repeated chat line was not compressed"
    );

    const auto no_drop = [&] {
        return (console::console_flags(game) & console::console_flag::no_drop) != 0;
    };
    const auto seconds = game.player_timeout_seconds;
    const auto silent_peer = [&] {
        step(false);
        base::threads::sleep_ms(kSilence);
        step(false);
        return match->timeout_player;
    };
    runtime_.enter_console_check_line("+drop 0");
    require(no_drop(), "+drop 0 did not set the no-drop bit");
    game.player_timeout_seconds = 0;
    require(silent_peer() == nm::no_player_id, "the stall check ran with Drop 0");
    runtime_.enter_console_check_line("+drop 1");
    require(!no_drop(), "+drop 1 did not clear the no-drop bit");
    require(silent_peer() == peer_id, "the stall check did not name the silent peer");
    game.player_timeout_seconds = seconds;
    wait([&] { return match->timeout_player == nm::no_player_id; }, "the stall did not clear");

    runtime_.enter_console_check_line("+page someone hello");
    require(
        posted(
            runtime_.match_message_lines(), "Page command requires game launch from the Boneyards."
        ),
        "+page did not ask for an active launch"
    );

    const Player* local_on_peer = player_with_id(peer_world, local_id);
    require(local_on_peer != nullptr, "the peer has no slot for this machine");
    std::string speaker = "<";
    speaker += field_text(local.name, sizeof local.name);
    speaker += "> ";
    runtime_.enter_console_check_line("+Now Film Chris Include Reload Assert");
    runtime_.enter_console_check_line("+senderror 100");
    require(game.send_error_percent == 100, "+senderror 100 did not set 100%");
    // Frames already on their way arrive first.
    for (int i = 0; i < kLossSteps; ++i)
        step(true);
    const auto sent_frames = traffic.sent_datagrams;
    const auto heard_at = local_on_peer->last_update_time;
    net_say("lost in transit");
    for (int i = 0; i < kLossSteps; ++i)
        step(true);
    require(traffic.sent_datagrams > sent_frames, "the dropped frames were not counted as sent");
    require(local_on_peer->last_update_time == heard_at, "a frame reached the peer at 100% loss");
    runtime_.enter_console_check_line("+senderror 0");
    require(game.send_error_percent == 0, "+senderror 0 did not clear the loss");
    net_say("through again");
    wait(
        [&] { return logged(peer.match_message_lines(), speaker + "through again"); },
        "chat did not reach the peer after the loss ended"
    );
    require(
        !logged(peer.match_message_lines(), speaker + "lost in transit"),
        "a dropped chat line reached the peer"
    );
    runtime_.enter_console_check_line("+Now");
    std::cout << "console network session check: share toggles reached the peer, +compression "
                 "sent stored frames, +drop 0 held the stall check, +senderror 100 dropped every "
                 "frame\n";
}

} // namespace oa::app

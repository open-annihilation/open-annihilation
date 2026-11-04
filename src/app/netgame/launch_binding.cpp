// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The launch block as the running game binds it (launch_binding.hpp).
#include "launch_binding.hpp"

#include "oa/app/netgame/launch_file.hpp"
#include "oa/netgame/overlay_route.hpp"
#include "oa/ui/frontend_multiplayer/screens.hpp"

#include <cstddef>
#include <cstdio>

namespace oa::app {

namespace {

namespace mp = oa::ui::frontend_multiplayer;
namespace nl = oa::app::netgame::launch;

// Characters of the player's name the nickname keeps.
constexpr std::size_t kNicknameLimit = oa::app::netgame::kPlayerNameLimit;

LaunchBinding g_launch{};
// A launch is active: applied, or marked active, and not ended since.
bool g_launch_active = false;
oa::app::netgame::extension_api::Hooks g_extension_hooks{};

/// Hands a join the launch asked for that failed to the extension's hook (LaunchLink::join_failed).
///
/// @param context unused
/// @param text the text to show
void join_failed(void*, const char* text) noexcept {
    if (g_extension_hooks.join_failed != nullptr)
        g_extension_hooks.join_failed(g_extension_hooks.context, text);
}

} // namespace

void bind_launch(const LaunchBinding& binding) noexcept {
    g_launch = binding;
}

oa::ui::frontend_multiplayer::launch::LaunchBlock* launch_block() noexcept {
    return g_launch.block;
}

bool launch_active(void*) noexcept {
    if (g_launch_active && g_launch.launch_ended != nullptr && g_launch.launch_ended())
        g_launch_active = false;
    return g_launch_active;
}

void set_launch_active(bool active) noexcept {
    g_launch_active = active;
}

void apply_launch() noexcept {
    const char* player_name = g_launch.block != nullptr ? g_launch.block->user_name : "";
    std::snprintf(
        mp::lobby_nickname(mp::multiplayer_game()),
        kNicknameLimit + 1,
        "%.*s",
        static_cast<int>(kNicknameLimit),
        player_name
    );
    if (g_launch.launch_pending != nullptr)
        *g_launch.launch_pending = 1;
    if (g_launch.close_handlers != nullptr)
        g_launch.close_handlers->installed = CloseHandler::exit_confirm;
    g_launch_active = true;
}

void end_launch() noexcept {
    g_launch_active = false;
}

void request_setup() noexcept {
    if (g_launch.switches != nullptr)
        nl::enable_netsetup(*g_launch.switches);
}

void request_app_mode(void*, int32_t mode) noexcept {
    if (g_launch.app_mode_pending != nullptr)
        *g_launch.app_mode_pending = mode;
}

void leave_launched_game(void*, bool with_reason) noexcept {
    if (g_launch.leave_game != nullptr)
        g_launch.leave_game(g_launch.leave_context, with_reason);
}

oa::ui::frontend_multiplayer::LaunchLink launch_link() noexcept {
    mp::LaunchLink link{};
    link.block = g_launch.block;
    link.launch_active = launch_active;
    link.join_failed = join_failed;
    link.request_app_mode = request_app_mode;
    link.leave_game = leave_launched_game;
    return link;
}

const oa::app::netgame::extension_api::Hooks& extension_hooks() noexcept {
    return g_extension_hooks;
}

bool launch_tournament() noexcept {
    // The field stays as the last launch wrote it, after the return from
    // its battle too, as 3.1c's does.
    return g_launch.block != nullptr && g_launch.block->tournament != 0;
}

int32_t launch_connection_type(void*) noexcept {
    return g_launch.block != nullptr ? g_launch.block->connection_type : 0;
}

} // namespace oa::app

namespace oa::app::netgame::extension_api {

namespace {

// The block launch_block returns while none is bound: an empty one, which
// no game reads.
oa::ui::frontend_multiplayer::launch::LaunchBlock g_unbound_block{};

} // namespace

oa::ui::frontend_multiplayer::launch::LaunchBlock& launch_block() noexcept {
    oa::ui::frontend_multiplayer::launch::LaunchBlock* block = oa::app::launch_block();
    return block != nullptr ? *block : g_unbound_block;
}

void request_setup() noexcept {
    oa::app::request_setup();
}

void apply_launch() noexcept {
    oa::app::apply_launch();
}

void end_launch() noexcept {
    oa::app::end_launch();
}

bool launch_active() noexcept {
    return oa::app::launch_active(nullptr);
}

void request_app_mode(int32_t mode) noexcept {
    oa::app::request_app_mode(nullptr, mode);
}

void leave_game(bool with_reason) noexcept {
    oa::app::leave_launched_game(nullptr, with_reason);
}

int32_t connection_type() noexcept {
    return oa::app::launch_connection_type(nullptr);
}

void set_hooks(const Hooks& hooks) noexcept {
    oa::app::g_extension_hooks = hooks;
}

void set_overlay_route(const oa::netgame::sock::OverlayRoute* route) noexcept {
    oa::netgame::sock::set_overlay_route(route);
}

uint64_t overlay_direct_packets() noexcept {
    return oa::netgame::sock::overlay_direct_packets();
}

} // namespace oa::app::netgame::extension_api

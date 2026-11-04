// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The tools a new MCP session offers (tools.cpp, ui.cpp): game.info,
// game.status, game.speed, game.pause, the camera tools and game.quit, the
// resources oa://game/info and oa://game/status, and the tools that read
// and drive the screen.
#pragma once

#include "oa/app/mcp/actions.hpp"
#include "oa/app/mcp/session.hpp"
#include "oa/app/presence_facts.hpp"

#include "ui.hpp"

#include <cstdint>
#include <string>

namespace oa {
struct World;
}

namespace oa::app {
class Runtime;
}

namespace oa::app::mcp {

/// Which part of the frame the tools are being served from.
enum class ServeStage : uint8_t {
    idle,      ///< outside a frame
    pump,      ///< the frame's pump, before the game has drawn
    presented, ///< the frame has been presented
};

/// Facts copied off the runtime for game.info, plus the stand-in a test reads.
///
/// The frame hook fills the profile and the awareness callback before it
/// pumps a client. A test sets world and leaves runtime null. game_type
/// stays empty: a running match does not carry a launcher mode name.
struct GameReading {
    std::string engine_version;                      ///< the version initialize reports
    MatchProfileReading profile{};                   ///< the mod, the hacks and the mutators
    bool has_game_type{};                            ///< a launcher mode name was copied
    std::string game_type;                           ///< that name, when has_game_type is set
    bool (*full_awareness_allowed)(void* context){}; ///< the match view's rule
    void* awareness_context{};                       ///< passed to full_awareness_allowed
    const oa::World* world{};                        ///< a test's match; null reads the runtime
};

/// The running game a tool reads. The frame hook sets runtime before it pumps.
struct Serve {
    Runtime* runtime{};                 ///< the runtime of this frame; null outside one
    bool quit{};                        ///< game.quit has been called
    ServeStage stage{ServeStage::idle}; ///< the stage being served
    uint64_t frame{};                   ///< the frames served; raised at each pump
    UiState ui;                         ///< the screen tools' drive, wait and picture
    GameReading reading;                ///< the facts game.info and game.status read
};

/// Adds game.info, game.status, game.speed, game.pause, the camera tools,
/// game.quit, oa://game/info and oa://game/status.
///
/// Points play at the running game, then adds the act tools. play outlives
/// the session's calls, and is not moved after this: the tools keep its address.
///
/// @param[in,out] session the session
/// @param[in,out] serve the game the tools read; it outlives the session's calls
/// @param[in,out] play the queue and the host; it outlives the session's calls
void add_game_tools(Session& session, Serve& serve, PlayBridge& play);

} // namespace oa::app::mcp

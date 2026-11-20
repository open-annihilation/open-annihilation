// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The events the automation endpoint sends unasked to a client that
// subscribed to them: what changed in the screen shown, the battle room and
// the running match. The endpoint learns them by comparing what the game
// holds at each frame's pump stage with what it held at the last, and, while
// a match loads and no frame runs, from the loading's progress; it adds
// nothing to the simulation to learn them and changes nothing of the game.
//
// Each event carries its kind, its number among the client's events (seq),
// and the frame and tick it was seen in, then its own members:
//
//   screen     name, previous (null for the screen shown as the client
//              subscribed to screen), dialogs (those over the screen)
//   player     change ("joined" or "left"), slot, name: a battle-room seat
//   ready      slot, name, ready
//   chat       where ("room" or "match"), from, to (a chat line sent to
//              one player, else absent), text
//   loaded     player and slot (null while this machine's own load has no
//              players yet), progress (0 to 100), local
//   match      change ("started" or "ended"); ended also carries results,
//              winner and tick, taken while the match was still there
//   pause      paused
//   speed      speed (1 to 20; 10 is normal)
//   alliance   from, from_slot, to, to_slot, allied
//   game_over  outcome ("victory" or "defeat"), winner (a name, or null,
//              this machine's own), then results and tick, the same on
//              every machine
//   focus      name (null when no control is focused), label, role. One
//              event when the focused control changes. None when it stays,
//              and none the first time the watch sees the game.
//   transfer   accepted by subscribe, never sent yet: no part of the game
//              that the endpoint reads reports units or resources given
#pragma once

#include "oa/formats/json.hpp"
#include "oa/core/player.h"
#include "reports.hpp"

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa {
struct World;
}

namespace oa::ui::frontend_multiplayer {
struct Lobby;
}

namespace oa::app::automation {

// A source can include more than one of these headers, so each name is declared once.
#ifndef OA_APP_AUTOMATION_USING_JSON_WRITER
#define OA_APP_AUTOMATION_USING_JSON_WRITER
using oa::formats::json::JsonWriter;
#endif

/// The kinds of event, as bits of a subscription (Endpoint::subscribe).
namespace event_kind {
inline constexpr uint32_t screen = 0x001;
inline constexpr uint32_t player = 0x002;
inline constexpr uint32_t ready = 0x004;
inline constexpr uint32_t chat = 0x008;
inline constexpr uint32_t loaded = 0x010;
inline constexpr uint32_t match = 0x020;
inline constexpr uint32_t pause = 0x040;
inline constexpr uint32_t speed = 0x080;
inline constexpr uint32_t alliance = 0x100;
inline constexpr uint32_t transfer = 0x200;
inline constexpr uint32_t game_over = 0x400;
inline constexpr uint32_t focus = 0x800;
/// Every kind.
inline constexpr uint32_t all = 0xfff;
} // namespace event_kind

/// A kind of event and the name the protocol gives it.
struct EventKindName {
    uint32_t kind{};       ///< its bit (event_kind)
    std::string_view name; ///< as an event's "event" and subscribe's events name it
};

/// Returns every kind of event, in the order subscribe's answer lists them.
///
/// @return the kinds
[[nodiscard]] std::span<const EventKindName> event_kind_names() noexcept;

/// Finds a kind of event by its name.
///
/// @param name the name
/// @return its bit, or 0 when no kind has that name
[[nodiscard]] uint32_t event_kind_named(std::string_view name) noexcept;

/// Where the event watch sends what it sees.
struct EventHooks {
    void* context{};
    /// Tells whether events of a kind are wanted; null wants none.
    ///
    /// @param context EventHooks::context
    /// @param kind the kind's bit
    /// @return true to have them built and sent
    bool (*wanted)(void* context, uint32_t kind){};
    /// Starts an event: writes its kind, seq, frame and tick into an open object.
    ///
    /// @param context EventHooks::context
    /// @param kind the event's kind, as the protocol names it
    /// @return the event, its object open for the event's members
    JsonWriter (*begin)(void* context, std::string_view kind){};
    /// Sends an event begun with begin, its object still open.
    ///
    /// @param context EventHooks::context
    /// @param[in,out] event the event; its object is closed
    void (*send)(void* context, JsonWriter& event){};
};

/// What the event watch reads of the game at one moment.
struct Observed {
    std::string screen; ///< the screen shown, by the endpoint's name for it
    /// The dialogs over the screen whose controls take the pointer, by
    /// their GUI file's name, as the screen answer gives them; read only
    /// for a screen event.
    std::vector<std::string> dialogs;
    const oa::World* match{}; ///< the running match; null while none runs
    /// The multiplayer screens' lobby while the battle room shows; null
    /// otherwise. Only read.
    oa::ui::frontend_multiplayer::Lobby* room{};
    /// The control that holds the keyboard focus, by its name. Empty when none does.
    std::string focus_name;
    std::string focus_label; ///< that control's label
    std::string focus_role;  ///< that control's role, as the protocol names it
    /// This moment read which control is focused. A moment that did not
    /// leaves the watch unread, so the next one that does stores the name
    /// and sends nothing.
    bool focus_read{true};
};

/// What the event watch last saw, to which it compares what it sees next.
struct EventWatch {
    /// One seat of the battle room.
    struct Seat {
        bool taken{};         ///< a player sits in it
        uint32_t player_id{}; ///< the player's id
        std::string name;     ///< the player's name
        bool ready{};         ///< the player is ready
    };

    bool seen{};        ///< the watch has seen the game once
    std::string screen; ///< the screen shown
    /// The next screen event names no previous screen: the client has just
    /// subscribed to screen and is told the one shown.
    bool announce_screen{};

    bool in_room{};                            ///< the battle room shows
    std::array<Seat, OA_PLAYER_COUNT> seats{}; ///< the battle room's seats
    uint16_t room_chat_next{};                 ///< the room's chat line read next

    const oa::World* match{}; ///< the match being watched; null for none
    bool match_started{};     ///< its start was reported
    bool paused{};            ///< its pause
    uint16_t speed{};         ///< its game speed
    bool decided{};           ///< its outcome was reported
    /// Its results, taken while the world was still there, so the end can
    /// name them after the world has gone.
    std::optional<MatchResults> results;
    uint16_t match_chat_next{}; ///< its message log's line read next
    /// Its players' load progress, 0 to 100.
    std::array<uint8_t, OA_PLAYER_COUNT> load_progress{};
    /// Its players' alliances: alliances[from][to] is nonzero while from allies with to.
    std::array<std::array<uint8_t, OA_PLAYER_COUNT>, OA_PLAYER_COUNT> alliances{};

    /// The progress this machine's loading last reported, 0 to 100; -1 for none.
    int32_t local_load{-1};

    /// The watch has seen which control is focused once. The first sight
    /// stores it and sends nothing.
    bool focus_seen{};
    std::string focus_name; ///< the focused control's name last seen
};

/// Compares what the game holds now with what the watch saw last, sends
/// an event for each change the hooks want, and keeps what it saw.
///
/// The first time it sees the game it only keeps it; a new battle room
/// and a new match are first seen without seats, ready players, load
/// progress or chat lines, so that those already there are reported as
/// they are found, and with the match's pause, speed and alliances as they
/// are.
///
/// @param[in,out] watch what the watch saw last
/// @param now what the game holds now
/// @param hooks where events go
void watch_events(EventWatch& watch, const Observed& now, const EventHooks& hooks);

/// Notes that the running match is about to go, reporting its end when its
/// start was reported.
///
/// @param[in,out] watch what the watch saw last
/// @param hooks where events go
void watch_match_gone(EventWatch& watch, const EventHooks& hooks);

/// Reports this machine's loading of a match: its mean progress over the
/// loading screen's rows, when it changed.
///
/// @param[in,out] watch what the watch saw last
/// @param match the match being built, whose local player is named; null
///        before it exists
/// @param rows the loading screen's rows, each 0 to 100 percent
/// @param hooks where events go
void watch_loading(
    EventWatch& watch,
    const oa::World* match,
    std::span<const uint8_t> rows,
    const EventHooks& hooks
);

/// Has the next screen event tell the client the screen shown, as it
/// starts to take screen events.
///
/// @param[in,out] watch what the watch saw last
void announce_screen(EventWatch& watch) noexcept;

/// Tells whether watch_events, given this screen, sends a screen event, so
/// that the dialogs over it need be read only then.
///
/// @param watch what the watch saw last
/// @param screen the screen shown, by the endpoint's name for it
/// @return true when the screen is announced or differs from the last seen
[[nodiscard]] bool screen_event_due(const EventWatch& watch, std::string_view screen) noexcept;

} // namespace oa::app::automation

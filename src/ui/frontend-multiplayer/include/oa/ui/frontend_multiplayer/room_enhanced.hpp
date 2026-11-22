// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The battle room's Enhanced Mode: the revision the room plays, the lines it
// posts when that changes, and whether START waits for every machine's rules.
#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace oa::ui::frontend_multiplayer {

struct Lobby;

/// What one seated machine announces about Enhanced Mode.
enum class EnhancedAnnounce : uint8_t {
    revision,  ///< the highest revision it plays (presence field 0x26)
    none,      ///< no revision: a 3.1c machine, or an Open Annihilation record without 0x26
    not_known, ///< its setup block or its presence record has not arrived yet
};

/// One seated machine: a human or a watcher. A computer player counts as
/// the machine that runs it, so it has no seat of its own here.
struct EnhancedSeat {
    int32_t slot{-1};                                       ///< the seat's slot, 0 to 9
    std::string player{};                                   ///< the seat's name, UTF-8
    EnhancedAnnounce announce{EnhancedAnnounce::not_known}; ///< what the machine announces
    uint32_t highest{}; ///< with `revision`: the highest revision it plays, at least 1

    /// Compares two seats.
    friend bool operator==(const EnhancedSeat&, const EnhancedSeat&) = default;
};

/// Why the room plays the revision it plays.
enum class EnhancedRoomReason : uint8_t {
    switch_off, ///< the host's switch is off (on a guest: the host keeps it off)
    locked,     ///< a launch locks the room's options, and the switch with them
    plays,      ///< every machine plays the room's revision and none a later one
    lacks,      ///< a machine has no Enhanced Mode, so the room is Off
    lower,      ///< a machine has no later revision, so the room plays its highest
};

/// The room's Enhanced Mode, as one machine resolves it.
struct EnhancedRoom {
    uint32_t revision{}; ///< the revision the room plays; 0 is Off
    EnhancedRoomReason reason{EnhancedRoomReason::switch_off}; ///< why
    int32_t cause{-1};          ///< the seat `lacks` and `lower` name; -1 for none
    std::string cause_player{}; ///< that seat's name, UTF-8; empty for none
    /// A seated machine's announce has not arrived. It changes nothing until
    /// it does, and holds START meanwhile.
    bool waiting{};
    /// On a guest, while the room's Off line waits for a seat to name: the
    /// time (EnhancedRoomInputs::now_ms) the host's record turned the room
    /// Off with no seat this machine sees announcing none. Nothing once the
    /// line has posted, and always nothing on the host.
    std::optional<uint64_t> off_line_since{};

    /// Compares two resolutions.
    friend bool operator==(const EnhancedRoom&, const EnhancedRoom&) = default;
};

/// How long a guest's Off line waits for the seat that turned the room Off,
/// in milliseconds: three seconds.
///
/// A joiner sends its record to a guest only once the guest's own block has
/// reached it, and the battle room sends its block about every two seconds,
/// so the joiner's record can reach a guest up to two seconds after the
/// host's, plus the frames and the trip between. A second more covers them.
/// After it the guest posts "*** Enhanced Mode: Off.", which is also the
/// line for a host that turned its switch off, or a seat the guest never
/// learns of.
inline constexpr uint64_t enhanced_off_line_wait_ms = 3000;

/// What a machine knows when it resolves the room.
struct EnhancedRoomInputs {
    bool hosting{};   ///< this machine hosts: its switch decides
    bool switch_on{}; ///< the host's switch; read on the host only
    bool locked{};    ///< a launch locks the room's options; read on the host only
    /// On a guest, the room's revision the host announces (presence field
    /// 0x27): 0 when its record names none or the host plays 3.1c; nothing
    /// until the host's setup block or record has arrived. Not read on the host.
    std::optional<uint32_t> announced{};
    /// Every seated human and watcher, this machine's own included, in slot order.
    std::span<const EnhancedSeat> seats{};
    /// The time now, in milliseconds of a clock that never goes back. Read
    /// on a guest only, to end the wait of an Off line
    /// (enhanced_off_line_wait_ms).
    uint64_t now_ms{};
};

/// A resolution and the room lines it posts.
struct EnhancedRoomChange {
    EnhancedRoom room{}; ///< the room as resolved now
    /// The lines to post, in the language shown, each one whole sentence
    /// with its names filled; the caller fits them to the chat list.
    std::vector<std::string> lines{};
};

/// Resolves the room's Enhanced Mode revision and the lines a change posts.
///
/// On the host: Off while a launch locks the room's options or the switch is
/// off. Otherwise the lowest highest-revision across every seated human and
/// watcher; Off when any of them announces none, naming the first such seat;
/// and `lower`, naming the first seat at that revision, when another plays a
/// later one. A seat whose announce is not known changes nothing. On a
/// guest the revision is the host's announcement, and the reason is the one
/// the seats it sees give that revision: `lacks` names a seat that announces
/// none, `lower` a seat at that revision; else the host keeps it off, or the
/// room plays it. Until the host's announcement arrives a guest keeps
/// `before` (Off, waiting, the first time).
///
/// Lines post once per change. On the host a change is one of the revision
/// or of whether the switch is in effect (on and not locked). On a guest it
/// is one of the revision. The first resolution posts only a revision of 1
/// or more. A revision of 1 or more posts "*** Enhanced Mode: Revision {n}.",
/// or, for `lower`, "*** Enhanced Mode is Revision {n}: {player}'s game has no
/// later revision." Off posts "*** Enhanced Mode is off: {player}'s game
/// doesn't have it." for `lacks`, and "*** Enhanced Mode: Off." otherwise.
///
/// The host's record and the record of the seat that turned the room Off
/// reach a guest in either order. When the host's comes first, the guest's
/// room is Off at once and its line waits (EnhancedRoom::off_line_since):
/// it names the first seat that announces none as soon as one does, or,
/// once enhanced_off_line_wait_ms has passed, posts "*** Enhanced Mode:
/// Off.". Either way the change posts one line. A revision the host
/// announces before then posts the waiting Off line first, then its own.
///
/// @param inputs the switch, the lock, the host's announcement, the seats and the time
/// @param before the room as last resolved in this battle room; nothing on entering it
/// @return the room now and the lines to post
[[nodiscard]] EnhancedRoomChange
resolve_enhanced_room(const EnhancedRoomInputs& inputs, const std::optional<EnhancedRoom>& before);

/// Tells where the host's switch starts.
///
/// From the host's own Enhanced Mode setting; Off in a Boneyards room (a
/// launch that sets stock_files) and while a launch locks the room's
/// options. The host may turn it on afterwards unless the options are locked.
///
/// @param settings_on the host's Settings › Engine Limits › Enhanced Mode
/// @param stock_files the launch counts for a ladder: a Boneyards room
/// @param locked a launch locks the room's options
/// @return true when the switch starts on
[[nodiscard]] bool
enhanced_room_switch_start(bool settings_on, bool stock_files, bool locked) noexcept;

/// The word for a revision, in the language shown: "Revision {n}", or "Off" for 0.
///
/// @param revision the revision; 0 is Off
/// @return the word
[[nodiscard]] std::string enhanced_revision_word(uint32_t revision);

/// The reason the switch shows under its value, in the language shown.
///
/// "{player}'s game doesn't have it." for `lacks` and "{player}'s game has no
/// later revision." for `lower`; empty for every other reason.
///
/// @param room the room
/// @return the reason; empty for none
[[nodiscard]] std::string enhanced_room_reason_text(const EnhancedRoom& room);

/// The lock reason a launch gives: "The launch keeps Enhanced Mode off.", in the language shown.
///
/// @return the words
[[nodiscard]] std::string enhanced_room_launch_lock_text();

/// The lock reason a guest reads: "Only the host can change Enhanced Mode.", in the language shown.
///
/// @return the words
[[nodiscard]] std::string enhanced_room_guest_lock_text();

/// The line beside START while it waits for a machine's rules:
/// "Waiting for {player}: Revision {n}", in the language shown.
///
/// @param player the machine's player, UTF-8
/// @param revision the room's revision
/// @return the line
[[nodiscard]] std::string enhanced_waiting_line(std::string_view player, uint32_t revision);

/// The battle room's seated humans and watchers, as the room resolves them.
///
/// A computer player and a seat that plays no longer and does not watch are
/// left out. This machine's own player announces `local`. Another machine's
/// seat is `not_known` until its own setup block and then, on Open
/// Annihilation, a well-framed presence record have arrived; `none` for a
/// 3.1c block or a record without a valid 0x26; else that revision.
///
/// @param lobby the battle room's lobby
/// @param local the highest revision this machine announces; nothing for none
/// @return the seats, in slot order
[[nodiscard]] std::vector<EnhancedSeat>
lobby_enhanced_seats(Lobby& lobby, std::optional<uint32_t> local);

/// The room's revision the host announces, as a guest reads it.
///
/// @param lobby the battle room's lobby
/// @return the host record's 0x27, 0 when that record names none or the host
///     plays 3.1c; nothing while the host's setup block or record has not arrived
[[nodiscard]] std::optional<uint32_t> lobby_enhanced_announced(Lobby& lobby) noexcept;

/// The room's revision as the host's record names it now, 0 for Off.
///
/// On the host this is its own record. Allocates nothing.
///
/// @param lobby the battle room's lobby
/// @return the revision; 0 while the host's record names none or has not arrived
[[nodiscard]] uint32_t lobby_enhanced_room_revision(Lobby& lobby) noexcept;

/// Why START waits on Enhanced Mode.
enum class EnhancedHold : uint8_t {
    none,         ///< nothing about Enhanced Mode holds START
    waiting,      ///< a seated machine's presence record has not arrived
    rules_differ, ///< a seated machine's sim hash is not the host's
};

/// What holds START, and the machine it waits for.
struct EnhancedBlock {
    EnhancedHold hold{}; ///< none when START is not waiting on Enhanced Mode
    int32_t slot{-1};    ///< the machine it waits for; -1 for none
    uint32_t revision{}; ///< the room's revision
};

/// Returns why START waits on Enhanced Mode.
///
/// While the host's record names no revision nothing holds START: an Off
/// room keeps 3.1c's rules. Otherwise the host requires a record whose sim
/// hash is its own from every seated human and watcher; a guest checks its
/// own against the host's. Allocates nothing.
///
/// @param lobby Lobby state.
/// @return the hold, and the first machine it waits for
[[nodiscard]] EnhancedBlock lobby_enhanced_block(Lobby& lobby) noexcept;

/// The first reason Enhanced Mode holds START, or none.
///
/// @param lobby Lobby state.
/// @return the English rules line, "*** Enhanced Mode rules differ. Every
///     player must use the same mod and match rules.", while anything holds
///     START; null otherwise. Allocates nothing.
[[nodiscard]] const char* lobby_enhanced_start_problem(Lobby& lobby) noexcept;

/// Tells whether the room agrees on Enhanced Mode as far as START is concerned.
///
/// @param lobby Lobby state.
/// @return True when nothing about Enhanced Mode holds START. Allocates nothing.
[[nodiscard]] bool lobby_enhanced_allow_start(Lobby& lobby) noexcept;

/// Posts the rules line on this machine, translated, and writes it to the log
/// whole, when a seated machine's rules differ from the host's.
///
/// The line is not sent. A room that only waits for a record posts nothing:
/// the waiting line beside START names that player.
///
/// @param lobby Lobby state.
void lobby_note_enhanced_refused_start(Lobby& lobby) noexcept;

/// Posts one Enhanced Mode room line on this machine and writes it to the log whole.
///
/// The line is fitted to the chat list as lobby_post_room_line fits it. It
/// is not sent.
///
/// @param[in,out] lobby Lobby state.
/// @param line the line, in the language shown, every name filled
void lobby_post_enhanced_line(Lobby& lobby, const std::string& line) noexcept;

} // namespace oa::ui::frontend_multiplayer

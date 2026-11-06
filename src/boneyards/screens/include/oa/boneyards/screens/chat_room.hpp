// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// What the Chat Frequencies console and its BYChatRoomListItem rows keep of
// a frequency beyond its id: the type Rover reports and the sizes the name,
// description and tag are cut to.
#pragma once

#include "oa/boneyards/utf8.hpp"

#include <cstddef>
#include <cstdint>

namespace oa::boneyards::screens {

// A frequency name is at most 23 characters and its description 127. The
// fields hold that many of the widest characters.
inline constexpr int32_t kRoomNameCharacters = 0x17;
inline constexpr int32_t kRoomDescriptionCharacters = 0x7f;
inline constexpr size_t kRoomNameCapacity =
    oa::boneyards::utf8::bytes_for(static_cast<size_t>(kRoomNameCharacters));
inline constexpr size_t kRoomDescriptionCapacity =
    oa::boneyards::utf8::bytes_for(static_cast<size_t>(kRoomDescriptionCharacters));
inline constexpr size_t kRoomTagCapacity = 0x40;

// Frequency types. The frequency info pane labels 1 "Planetary Default" and
// 2 "Battle Debriefing", each beside "System", and 3 "User Created" beside
// the frequency's tag; other types are "unknown". The list puts planetary
// defaults first by name, user-created frequencies before the first battle
// debriefing, and debriefings and any other type last in arrival order.
namespace room_type {
inline constexpr int32_t unlisted = 0; // never gets a row
inline constexpr int32_t planetary_default = 1;
inline constexpr int32_t battle_debriefing = 2;
inline constexpr int32_t user_created = 3;
} // namespace room_type

} // namespace oa::boneyards::screens

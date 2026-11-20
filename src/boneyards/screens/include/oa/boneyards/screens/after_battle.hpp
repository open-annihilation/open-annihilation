// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The notice or prompt a player sees once a battle reached the game and they
// are back. A ladder battle is published by itself. A For Fun battle can be
// shared. Any other game type says nothing. A launch that never reached the
// game shows nothing.
#pragma once

#include <cstdint>
#include <string_view>

namespace oa::app {
struct ScreenServices;
}

namespace oa::boneyards::screens {

/// What a player reads after a ladder battle.
inline constexpr std::string_view kAfterBattleRanked =
    "This battle goes on Core Prime by itself, a few minutes after it ends.";

/// What a player reads after a For Fun battle. The buttons answer it.
inline constexpr std::string_view kAfterBattleShare = "Share this battle on Core Prime?";

/// What a player reads after a For Fun battle that was not recorded.
/// It is a statement: there is nothing to share.
inline constexpr std::string_view kAfterBattleUnrecorded =
    "This battle was not recorded, so it is not shared.";

/// The button that opens the share.
inline constexpr std::string_view kAfterBattleShareButton = "Share…";

/// The button that leaves the offer.
inline constexpr std::string_view kAfterBattleLater = "Not now";

/// The notice's title, and the prompt's.
inline constexpr std::string_view kAfterBattleTitle = "Core Prime";

/// Remembers the wire game type of a launch that is about to start.
///
/// A later arm replaces this one. The type is used only once the launch
/// starts (begin_after_battle). A search that has not launched arms nothing
/// of its own: the caller arms only as it sends the launch.
///
/// @param wire_game_type the battle's game type on the wire
void arm_after_battle(int32_t wire_game_type);

/// The launch started. The armed type becomes this battle's type.
///
/// The battle has not yet reached the game. An arm that was never set
/// leaves no type, so a launch this module did not send shows nothing.
void begin_after_battle();

/// The launch ended without a match, or was refused. Nothing is shown.
void forget_after_battle();

/// The battle reached the game. Noted only when a launch has started.
void mark_after_battle_played();

/// Whether this battle has a recording. A launch starts as recorded.
///
/// Sampled when the match finishes or is left, not when a later battle's
/// setup tears the previous match down.
///
/// @param kept true when this battle wrote a recording
void mark_after_battle_recorded(bool kept);

/// How many notices or prompts were actually queued.
///
/// @return the count since the process started
[[nodiscard]] int after_battle_shown();

/// The snapshot name of the notice or prompt last queued.
///
/// A For Fun prompt is "notice-share". A ladder notice, and a run that has
/// shown nothing, is "notice-ranked".
///
/// @return the stage name, never null
[[nodiscard]] const char* after_battle_picture_name();

/// Takes the share the player chose on the For Fun prompt, once.
///
/// @return true the first time it is asked after Share…, then false
[[nodiscard]] bool take_after_battle_share();

/// The sentence a player sees after this wire game type.
///
/// For Fun (0) offers a share. Duel, Winner Takes All, Galactic War and War
/// Training (1, 2, 4 and 7) say the battle is published by itself. Anything
/// else, including a type that was not known, says nothing.
///
/// @param wire_game_type the battle's game type on the wire
/// @return the English sentence, or empty when nothing is shown
[[nodiscard]] std::string_view after_battle_sentence(int32_t wire_game_type);

/// Shows the notice or prompt when the battle reached the game, then forgets it.
///
/// A ladder battle is a kit notice. A recorded For Fun battle is a kit prompt
/// whose Share… is taken later (take_after_battle_share) and whose Not now
/// leaves it. A For Fun battle that was not recorded is a statement, not a
/// question. A launch that did not reach the game, a null service, or a
/// service that cannot show the question, shows nothing and still forgets
/// the battle.
///
/// @param services the screen services; null shows nothing
/// @param host handed to the notice or prompt, as the services expect
void show_after_battle_notice(const oa::app::ScreenServices* services, void* host);

} // namespace oa::boneyards::screens

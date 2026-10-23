// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/app/seat_chat.hpp"

#include <cstring>

namespace oa::app::seat_chat {
namespace {

namespace messages = oa::sim::messages;

/// Tells whether a slot is a computer seat on this machine.
///
/// @param player the player record
/// @return true when the slot is in use and a computer player
bool computer_seat(const Player& player) noexcept {
    return player.in_use != 0 && player.status == OA_PLAYER_STATUS_COMPUTER;
}

/// Tells whether a cleaned line has nothing but spaces.
///
/// @param text the cleaned words
/// @return true when every byte is a space, an empty line included
bool only_spaces(std::string_view text) noexcept {
    for (const char byte : text)
        if (byte != ' ')
            return false;
    return true;
}

/// Replaces bytes below 0x20 with spaces.
///
/// @param text the words as said
/// @return the words a seat may queue
std::string clean_words(std::string_view text) {
    std::string cleaned(text.size(), ' ');
    for (std::size_t index = 0; index < text.size(); ++index) {
        const auto byte = static_cast<unsigned char>(text[index]);
        if (byte >= 0x20)
            cleaned[index] = static_cast<char>(byte);
    }
    return cleaned;
}

} // namespace

void SeatChat::reset() noexcept {
    pending_.clear();
    std::memset(said_at_, 0, sizeof said_at_);
    std::memset(said_, 0, sizeof said_);
}

void SeatChat::set_listener(const Listener& listener) noexcept {
    listener_ = listener;
}

Said SeatChat::say(
    const World& world, uint8_t seat, std::string_view text, uint8_t mode, uint8_t target
) {
    if (seat >= OA_PLAYER_COUNT || !computer_seat(world.game.players[seat]))
        return Said::not_a_seat;
    const std::string cleaned = clean_words(text);
    if (only_spaces(cleaned))
        return Said::empty;
    if (messages::chat_words_command(cleaned))
        return Said::command;
    const bool audience = mode == OA_CHAT_MODE_EVERYONE || mode == OA_CHAT_MODE_ALLIES ||
                          mode == OA_CHAT_MODE_ENEMIES || mode == OA_CHAT_MODE_CHOSEN;
    if (!audience)
        return Said::no_target;
    uint8_t targets[OA_PLAYER_COUNT]{};
    if (mode == OA_CHAT_MODE_CHOSEN) {
        if (target >= OA_PLAYER_COUNT || target == seat || world.game.players[target].in_use == 0)
            return Said::no_target;
        targets[target] = 1;
    }
    if (said_[seat] && world.game.tick - said_at_[seat] < messages::seat_chat_interval_ticks)
        return Said::too_soon;
    said_[seat] = true;
    said_at_[seat] = world.game.tick;
    Pending queued{};
    queued.seat = seat;
    queued.mode = mode;
    std::memcpy(queued.targets, targets, sizeof queued.targets);
    queued.words = cleaned;
    pending_.push_back(std::move(queued));
    return Said::queued;
}

void SeatChat::flush(World& world, const messages::Hooks& hooks) {
    std::vector<Pending> lines;
    lines.swap(pending_);
    for (const Pending& pending : lines) {
        if (pending.seat >= OA_PLAYER_COUNT)
            continue;
        messages::post_seat_chat(
            world,
            world.game.players[pending.seat],
            pending.words,
            pending.mode,
            pending.targets,
            hooks
        );
        if (listener_.heard == nullptr)
            continue;
        for (uint8_t hearer = 0; hearer < OA_PLAYER_COUNT; ++hearer) {
            if (!computer_seat(world.game.players[hearer]))
                continue;
            if (!messages::seat_hears(
                    world.game, pending.seat, hearer, pending.mode, pending.targets
                ))
                continue;
            listener_.heard(listener_.context, hearer, pending.seat, pending.mode, pending.words);
        }
    }
}

void SeatChat::hear_local(
    const World& world,
    uint8_t speaker,
    std::string_view words,
    uint8_t mode,
    const uint8_t* targets
) const {
    if (listener_.heard == nullptr || messages::chat_words_command(words))
        return;
    for (uint8_t hearer = 0; hearer < OA_PLAYER_COUNT; ++hearer) {
        if (!computer_seat(world.game.players[hearer]))
            continue;
        if (!messages::seat_hears(world.game, speaker, hearer, mode, targets))
            continue;
        listener_.heard(listener_.context, hearer, speaker, mode, words);
    }
}

void SeatChat::hear_remote(
    const World& world, uint8_t from, uint8_t addressee, bool broadcast, std::string_view line
) const {
    if (listener_.heard == nullptr)
        return;
    const messages::ChatLineParts parts = messages::split_chat_line(line);
    if (messages::chat_words_command(parts.words))
        return;
    if (broadcast) {
        for (uint8_t hearer = 0; hearer < OA_PLAYER_COUNT; ++hearer) {
            if (!computer_seat(world.game.players[hearer]))
                continue;
            listener_.heard(
                listener_.context,
                hearer,
                from,
                static_cast<uint8_t>(OA_CHAT_MODE_EVERYONE),
                parts.words
            );
        }
        return;
    }
    if (addressee >= OA_PLAYER_COUNT || !computer_seat(world.game.players[addressee]))
        return;
    listener_.heard(
        listener_.context, addressee, from, messages::chat_mode_of_target(parts.target), parts.words
    );
}

std::size_t SeatChat::pending() const noexcept {
    return pending_.size();
}

} // namespace oa::app::seat_chat

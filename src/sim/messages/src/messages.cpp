// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/sim/messages.hpp"
#include "oa/base/text.hpp"
#include "oa/base/text/line_break.hpp"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <initializer_list>
#include <span>
#include <string_view>

namespace oa::sim::messages {
namespace {

inline constexpr size_t player_chat_name_bytes = sizeof(Player::second_name);
inline constexpr size_t formatted_bytes = 200;
// Bytes past a cut that show whether a UTF-8 character spans it.
inline constexpr size_t chat_lookahead_bytes = 4;

// Bounded copy of Player.second_name, the name chat lines carry.
void chat_name(const Player& player, char out[player_chat_name_bytes + 1]) {
    std::memcpy(out, player.second_name, player_chat_name_bytes);
    out[player_chat_name_bytes] = '\0';
}

// The bytes a formatted chat line holds, its end included.
size_t chat_line_limit(const Hooks& hooks) noexcept {
    size_t line_bytes = formatted_bytes;
    if (hooks.shared_chat_line_bytes != nullptr)
        if (const size_t asked = hooks.shared_chat_line_bytes(hooks.context); asked != 0)
            line_bytes = asked < most_chat_line_bytes ? asked : most_chat_line_bytes;
    return line_bytes;
}

// Writes "<name->target> text", cut between whole UTF-8 characters.
// line_bytes is the bytes of `line`, its end included, at least 1.
void write_chat_line(
    char* line, size_t line_bytes, const char* name, const char* target, const char* text
) noexcept {
    // line_bytes is at most most_chat_line_bytes, so the look-ahead fits.
    if (line_bytes > most_chat_line_bytes)
        line_bytes = most_chat_line_bytes;
    char whole[most_chat_line_bytes + chat_lookahead_bytes];
    std::snprintf(
        whole,
        line_bytes + chat_lookahead_bytes,
        "<%s%s%s> %s",
        name,
        target != nullptr ? "->" : "",
        target != nullptr ? target : "",
        text != nullptr ? text : ""
    );
    const std::string_view formatted(whole);
    oa::base::text::copy_terminated(
        std::span<char>(line, line_bytes),
        formatted.substr(0, oa::base::text::whole_characters(formatted, line_bytes - 1))
    );
}

// ASCII case-insensitive equality of two words of the same length.
bool same_ascii_word(std::string_view word, std::string_view known) noexcept {
    if (word.size() != known.size())
        return false;
    for (size_t i = 0; i < word.size(); ++i) {
        const auto lower = [](unsigned char c) {
            return c >= 'A' && c <= 'Z' ? static_cast<unsigned char>(c - 'A' + 'a') : c;
        };
        if (lower(static_cast<unsigned char>(word[i])) !=
            lower(static_cast<unsigned char>(known[i])))
            return false;
    }
    return true;
}

/// Tells whether a chat line's text is an alliance line: a space, the
/// phrase, a space and the other player's name.
///
/// @param text the line's text, after its head
/// @param phrase the alliance phrase
/// @return true when the text says the phrase so
bool says_alliance(std::string_view text, std::string_view phrase) noexcept {
    return text.size() > phrase.size() + 1 && text[0] == ' ' &&
           text.substr(1, phrase.size()) == phrase && text[phrase.size() + 1] == ' ';
}

} // namespace

void format_shown_chat_line(
    char* out,
    size_t size,
    std::string_view line,
    const char* (*translate)(void* context, const char* text),
    void* context
) noexcept {
    if (size == 0)
        return;
    const size_t close = !line.empty() && line[0] == '<' ? line.find("> ") : line.npos;
    std::string_view head = line;
    std::string_view rest{};
    const char* phrase = nullptr;
    bool alliance = false;
    if (close != line.npos) {
        const std::string_view text = line.substr(close + 2);
        for (const char* said : {phrase_allied_with, phrase_broke_alliance_with})
            if (says_alliance(text, said)) {
                phrase = said;
                rest = text.substr(1 + std::strlen(said));
                alliance = true;
            }
        if (text == phrase_missing_map)
            phrase = phrase_missing_map;
    }
    const char* translated =
        phrase != nullptr && translate != nullptr ? translate(context, phrase) : nullptr;
    if (translated != nullptr)
        head = line.substr(0, close + 2);
    // Each part keeps what still fits, less a UTF-8 character the cut would
    // split, and nothing follows a part that was cut.
    size_t used = 0;
    bool cut = false;
    const auto put = [&](std::string_view part) {
        if (cut)
            return;
        const size_t kept = oa::base::text::whole_characters(part, size - 1 - used);
        std::copy_n(part.data(), kept, out + used);
        used += kept;
        cut = kept < part.size();
    };
    put(head);
    if (translated != nullptr) {
        if (alliance)
            put(" ");
        put(translated);
        put(rest);
    }
    out[used] = '\0';
}

int32_t line_capacity(const Game& game) noexcept {
    return game.text_lines;
}

int32_t text_scroll(const Game& game) noexcept {
    return game.text_scroll;
}

void set_log_options(
    Game& game, int32_t lines, int32_t scroll, int32_t filter, uint32_t screen_chat
) noexcept {
    game.text_lines = lines;
    game.text_scroll = scroll;
    game.message_filter = filter;
    game.screen_chat = screen_chat;
}

MessageLine* message_line(Game& game, uint32_t index) noexcept {
    return index < OA_CHAT_LINE_COUNT ? reinterpret_cast<MessageLine*>(game.chat_lines[index])
                                      : nullptr;
}

const char* translate(const Hooks& hooks, const char* text) noexcept {
    const char* translated =
        hooks.translate != nullptr ? hooks.translate(hooks.context, text) : nullptr;
    return translated != nullptr ? translated : text;
}

void clear_messages(Game& game) noexcept {
    game.chat_head = 0;
    game.chat_tail = 0;
}

bool expire_oldest_message(Game& game) noexcept {
    if (game.chat_head == game.chat_tail)
        return false;
    const MessageLine* oldest = message_line(game, game.chat_tail);
    if (oldest == nullptr)
        return false;
    const uint32_t expires =
        oldest->tick + static_cast<uint32_t>(text_scroll(game) + 1) * ticks_per_second;
    if (expires >= game.tick)
        return false;
    ++game.chat_tail;
    if (game.chat_tail == OA_CHAT_LINE_COUNT)
        game.chat_tail = 0;
    return true;
}

bool track_next_reported_unit(World& world, const Hooks& hooks) {
    Game& game = world.game;
    if (game.chat_head >= OA_CHAT_LINE_COUNT)
        return false;
    for (uint32_t index = game.chat_tail; index != game.chat_head;) {
        MessageLine* line = message_line(game, index);
        if (line == nullptr)
            return false;
        const Unit* unit = line->value != 0 && (line->kind & line_flag_visited) == 0
                               ? world_unit_at(&world, line->value)
                               : nullptr;
        if (unit != nullptr && (unit->flags & OA_UNIT_FLAG_LIVE) != 0) {
            line->kind = static_cast<uint8_t>(line->kind | line_flag_visited | line_flag_highlight);
            if (hooks.center_camera != nullptr)
                hooks.center_camera(
                    hooks.context,
                    static_cast<int16_t>(static_cast<uint32_t>(unit->position.x) >> 16),
                    static_cast<int16_t>(static_cast<uint32_t>(unit->position.z) >> 16),
                    reported_unit_camera_glide
                );
            return true;
        }
        if (++index == OA_CHAT_LINE_COUNT)
            index = 0;
    }
    return false;
}

void cycle_reported_units(World& world, const Hooks& hooks) {
    for (uint32_t index = 0; index < OA_CHAT_LINE_COUNT; ++index)
        if (MessageLine* line = message_line(world.game, index))
            line->kind = static_cast<uint8_t>(line->kind & ~line_flag_highlight);
    if (track_next_reported_unit(world, hooks))
        return;
    for (uint32_t index = 0; index < OA_CHAT_LINE_COUNT; ++index)
        if (MessageLine* line = message_line(world.game, index))
            line->kind = static_cast<uint8_t>(line->kind & ~line_flag_visited);
    (void)track_next_reported_unit(world, hooks);
}

void post_message(
    World& world, const char* text, uint8_t kind, uint16_t value, uint8_t sender, const Hooks& hooks
) {
    Game& game = world.game;
    if (text[0] == '\0')
        return;
    const int32_t capacity = line_capacity(game);
    if (capacity == 0)
        return;
    if ((static_cast<int32_t>(game.chat_head) + 1) % capacity ==
        static_cast<int32_t>(game.chat_tail)) {
        ++game.chat_tail;
        if (game.chat_tail == OA_CHAT_LINE_COUNT)
            game.chat_tail = 0;
    }
    MessageLine* line = message_line(game, game.chat_head);
    if (line == nullptr)
        return;
    // A line keeps its first text_bytes - 1 bytes, less the start of a
    // UTF-8 character the cut would split.
    constexpr std::size_t lookahead = 4;
    const std::string_view whole(text, strnlen(text, text_bytes + lookahead));
    const std::size_t kept = oa::base::text::whole_characters(whole, text_bytes - 1);
    std::memcpy(line->text, text, kept);
    std::memset(line->text + kept, 0, text_bytes - kept);
    line->tick = game.tick;
    line->kind = static_cast<uint8_t>((line->kind & ~kind_mask) | (kind & kind_mask));
    line->value = value;
    line->sender = sender;
    ++game.chat_head;
    if (game.chat_head == OA_CHAT_LINE_COUNT)
        game.chat_head = 0;
    if (sender != sender_none && hooks.play_sound != nullptr)
        hooks.play_sound(hooks.context, sound_message_arrived);
    if (hooks.refresh_panel != nullptr)
        hooks.refresh_panel(hooks.context);
}

void post_notice(World& world, const char* text, const Hooks& hooks) {
    while (*text == ' ')
        ++text;
    char chunk[text_bytes];
    // A notice with a character of several bytes breaks where the line
    // breaker lets it (oa::base::text::first_row), into lines of whole
    // characters; the game's one-byte text breaks as below.
    if (const std::string_view whole(text); oa::base::text::has_multibyte_character(whole)) {
        std::string_view rest = whole;
        const auto fits = [](std::string_view start) {
            return start.size() <= static_cast<std::size_t>(wrap_width);
        };
        while (!rest.empty()) {
            const auto row = oa::base::text::first_row(rest, fits);
            std::memcpy(chunk, rest.data(), row.bytes);
            chunk[row.bytes] = '\0';
            post_message(world, chunk, kind_notice, 0, sender_none, hooks);
            rest.remove_prefix(row.next);
        }
        return;
    }
    auto remaining = static_cast<int32_t>(std::strlen(text));
    for (;;) {
        if (remaining < static_cast<int32_t>(text_bytes)) {
            if (remaining > 0)
                post_message(world, text, kind_notice, 0, sender_none, hooks);
            return;
        }
        int32_t length = wrap_width;
        if (*text != ' ') {
            for (int32_t step = 0; step < wrap_lookback; ++step) {
                if (text[wrap_width - 1 - step] == ' ')
                    break;
                --length;
            }
        }
        std::memcpy(chunk, text, static_cast<size_t>(length));
        chunk[length] = '\0';
        remaining -= length;
        post_message(world, chunk, kind_notice, 0, sender_none, hooks);
        for (text += length; remaining > 0 && *text == ' '; ++text)
            --remaining;
    }
}

void post_elimination(World& world, const Player& player, const Hooks& hooks) {
    const PlayerSetupInfo* info = world_player_info(&world, &player);
    const uint8_t slot = info != nullptr ? info->side : 1;
    const char* side = hooks.side_name != nullptr ? hooks.side_name(hooks.context, slot)
                       : slot == 0                ? side_name_arm
                                                  : side_name_core;
    const uint32_t roll = hooks.random != nullptr ? hooks.random(hooks.context) : 0;
    const uint32_t ending = roll % elimination_message_count;
    const char* replaced = hooks.elimination_ending != nullptr
                               ? hooks.elimination_ending(hooks.context, ending)
                               : nullptr;
    const char* taunt =
        replaced != nullptr ? replaced : translate(hooks, elimination_messages[ending]);
    char line[formatted_bytes];
    std::snprintf(line, sizeof line, "%s %s", side, taunt);
    post_message(world, line, kind_elimination, 0, player.index, hooks);
}

void format_kill_lead(
    char* out, size_t size, const char* text, std::string_view name, int32_t score
) noexcept {
    if (out == nullptr || size == 0)
        return;
    size_t written = 0;
    const auto append = [&](std::string_view part) {
        const size_t room = size - 1 - written;
        const size_t count = part.size() < room ? part.size() : room;
        std::memcpy(out + written, part.data(), count);
        written += count;
    };
    char digits[12];
    const int digit_count = std::snprintf(digits, sizeof digits, "%d", static_cast<int>(score));
    const std::string_view number(digits, digit_count > 0 ? static_cast<size_t>(digit_count) : 0);
    bool name_placed = false;
    bool score_placed = false;
    for (const char* at = text; at != nullptr && *at != '\0'; ++at) {
        if (*at == '%' && at[1] == 's' && !name_placed) {
            append(name);
            name_placed = true;
            ++at;
        } else if (*at == '%' && at[1] == 'd' && !score_placed) {
            append(number);
            score_placed = true;
            ++at;
        } else if (*at == '%' && at[1] == '%') {
            append("%");
            ++at;
        } else {
            append({at, 1});
        }
    }
    out[written] = '\0';
}

void post_kill_lead(World& world, const Player& leader, int16_t score, const Hooks& hooks) {
    const char* replaced =
        hooks.kill_lead_text != nullptr ? hooks.kill_lead_text(hooks.context) : nullptr;
    const char* text = replaced != nullptr ? replaced : translate(hooks, kill_lead_message);
    const auto* name_end =
        static_cast<const char*>(std::memchr(leader.name, '\0', sizeof leader.name));
    const std::string_view name(
        leader.name,
        name_end != nullptr ? static_cast<size_t>(name_end - leader.name) : sizeof leader.name
    );
    char line[formatted_bytes];
    format_kill_lead(line, sizeof line, text, name, score);
    post_message(world, line, kind_status, 0, sender_none, hooks);
}

void post_chat(
    World& world,
    const Player& speaker,
    const char* text,
    uint8_t kind,
    const char* target,
    const Hooks& hooks
) {
    char name[player_chat_name_bytes + 1];
    chat_name(speaker, name);
    // The line keeps what fits, less a UTF-8 character the cut would split:
    // the bytes past the cut show whether one spans it. A line that may go
    // out as several records holds what they carry.
    const size_t line_bytes = chat_line_limit(hooks);
    char line[most_chat_line_bytes];
    write_chat_line(line, line_bytes, name, target, text);
    const bool shared = world.game.chat_mode != chat_mode_local_only && hooks.share_chat != nullptr;
    if (shared)
        hooks.share_chat(hooks.context, line);
    const uint8_t mode = world.game.chat_mode;
    if (hooks.game_kind != nullptr && hooks.game_kind(hooks.context) == game_kind_multiplayer &&
        mode != OA_CHAT_MODE_CHOSEN && mode != OA_CHAT_MODE_ALLIES &&
        mode != chat_mode_local_only && hooks.record_chat != nullptr)
        hooks.record_chat(hooks.context, line);
    // The speaker's log shows the line as the other players' logs do.
    char shown[most_chat_line_bytes];
    std::size_t parts = 0;
    if (shared && hooks.shared_chat_line != nullptr)
        for (const char* part = nullptr;
             (part = hooks.shared_chat_line(hooks.context, line, parts)) != nullptr;
             ++parts) {
            format_shown_chat_line(shown, sizeof shown, part, hooks.translate, hooks.context);
            post_message(world, shown, kind, 0, sender_none, hooks);
        }
    if (parts == 0) {
        format_shown_chat_line(shown, sizeof shown, line, hooks.translate, hooks.context);
        post_message(world, shown, kind, 0, sender_none, hooks);
    }
}

ChatLineParts split_chat_line(std::string_view line) noexcept {
    ChatLineParts parts{};
    if (line.empty() || line.front() != '<') {
        parts.words = line;
        return parts;
    }
    const size_t close = line.find("> ");
    if (close == line.npos) {
        parts.words = line;
        return parts;
    }
    const std::string_view head = line.substr(1, close - 1);
    parts.words = line.substr(close + 2);
    const size_t arrow = head.find("->");
    if (arrow == head.npos) {
        parts.speaker = head;
        return parts;
    }
    parts.speaker = head.substr(0, arrow);
    parts.target = head.substr(arrow + 2);
    return parts;
}

uint8_t chat_mode_of_target(std::string_view target) noexcept {
    if (target.empty())
        return static_cast<uint8_t>(OA_CHAT_MODE_EVERYONE);
    if (same_ascii_word(target, "Allies"))
        return static_cast<uint8_t>(OA_CHAT_MODE_ALLIES);
    if (same_ascii_word(target, "Enemies"))
        return static_cast<uint8_t>(OA_CHAT_MODE_ENEMIES);
    return static_cast<uint8_t>(OA_CHAT_MODE_CHOSEN);
}

bool chat_words_command(std::string_view words) noexcept {
    size_t index = 0;
    while (index < words.size() && words[index] == ' ')
        ++index;
    return index < words.size() && (words[index] == '+' || words[index] == '.');
}

bool seat_hears(
    const Game& game, uint8_t speaker, uint8_t seat, uint8_t mode, const uint8_t* targets
) noexcept {
    if (speaker >= OA_PLAYER_COUNT || seat >= OA_PLAYER_COUNT || speaker == seat)
        return false;
    if (mode == OA_CHAT_MODE_EVERYONE)
        return true;
    if (mode == OA_CHAT_MODE_ALLIES)
        return game.players[speaker].alliance[seat] != 0;
    if (mode == OA_CHAT_MODE_ENEMIES)
        return game.players[speaker].alliance[seat] == 0;
    if (mode == OA_CHAT_MODE_CHOSEN)
        return targets != nullptr && targets[seat] != 0;
    return false;
}

void post_seat_chat(
    World& world,
    const Player& seat,
    std::string_view words,
    uint8_t mode,
    const uint8_t* targets,
    const Hooks& hooks
) {
    if (chat_words_command(words))
        return;
    // The words may not be terminated. Enough of them is copied that the
    // formatted line's own cut, not this copy, is what shortens it.
    char text[most_chat_line_bytes + chat_lookahead_bytes];
    text[0] = '\0';
    if (!words.empty()) {
        const size_t count = words.size() < sizeof text - 1 ? words.size() : sizeof text - 1;
        std::memcpy(text, words.data(), count);
        text[count] = '\0';
    }
    char name[player_chat_name_bytes + 1];
    chat_name(seat, name);
    const char* target = nullptr;
    char chosen_name[player_chat_name_bytes + 1]{};
    if (mode == OA_CHAT_MODE_ALLIES) {
        target = "Allies";
    } else if (mode == OA_CHAT_MODE_ENEMIES) {
        target = "Enemies";
    } else if (mode == OA_CHAT_MODE_CHOSEN && targets != nullptr) {
        for (uint8_t slot = 0; slot < OA_PLAYER_COUNT; ++slot) {
            if (targets[slot] == 0)
                continue;
            chat_name(world.game.players[slot], chosen_name);
            target = chosen_name;
            break;
        }
    }
    const size_t line_bytes = chat_line_limit(hooks);
    char line[most_chat_line_bytes];
    write_chat_line(line, line_bytes, name, target, text);
    if (mode != chat_mode_local_only && hooks.share_seat_chat != nullptr)
        hooks.share_seat_chat(hooks.context, seat.index, line, mode, targets);
    if (hooks.game_kind != nullptr && hooks.game_kind(hooks.context) == game_kind_multiplayer &&
        mode == OA_CHAT_MODE_EVERYONE && hooks.record_chat != nullptr)
        hooks.record_chat(hooks.context, line);
    if (!seat_hears(world.game, seat.index, world.game.local_player_index, mode, targets))
        return;
    char shown[most_chat_line_bytes];
    format_shown_chat_line(shown, sizeof shown, line, hooks.translate, hooks.context);
    post_message(world, shown, kind_player_chat, 0, seat.index, hooks);
}

} // namespace oa::sim::messages

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

#include "oa/app/command_line.hpp"

#include <cstring>
#include <span>

namespace oa::app::command_line {
namespace {

// Developer switches, matched by prefix before the switch letter is read and
// skipped whole.
constexpr const char* kDebugHeapSwitches[] = {
    "-memfussy",
    "-memnofussy",
    "-memfrontalign",
    "-gonzo",
    "-memset",
    "-memnoset",
    "-fpufussy",
    "-fpunofussy",
    "-dprinton",
    "-dprintoff",
    "-dprintfile",
    "-memorystatus",
    "-performancestatus",
    "-disableimagehlp",
    "-enableimagehlp",
    "-disableimagehlplines",
    "-enableimagehlplines",
    "-debughelper",
    "-debughelperold",
    "-saveresources",
    "-assertmethod",
};

constexpr char kLockWord[] = "lock";

/// Tests for a token delimiter: blank or tab.
///
/// @param c Character.
/// @return Whether `c` separates tokens.
bool is_delimiter(char c) noexcept {
    return c == ' ' || c == '\t';
}

/// Tests for the C locale's space class: blank, tab, newline, vertical tab, form feed and return.
///
/// @param c Character.
/// @return Whether `c` is a space.
bool is_space(char c) noexcept {
    return c == ' ' || (c >= '\t' && c <= '\r');
}

/// Lowers an ASCII capital letter; other characters are returned unchanged.
///
/// @param c Character.
/// @return The lower-case character.
char fold(char c) noexcept {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c + ('a' - 'A')) : c;
}

/// Compares two strings for equality, ignoring ASCII case.
///
/// @param a First string.
/// @param b Second string.
/// @return Whether the strings are equal.
bool equal_nocase(const char* a, const char* b) noexcept {
    for (; *a != '\0' && fold(*a) == fold(*b); ++a, ++b) {
    }
    return fold(*a) == fold(*b);
}

/// Tests whether a text starts with a prefix, ignoring ASCII case.
///
/// @param text Text to test.
/// @param prefix Prefix.
/// @return Whether `text` starts with `prefix`.
bool starts_with_nocase(const char* text, const char* prefix) noexcept {
    for (; *prefix != '\0'; ++text, ++prefix)
        if (fold(*text) != fold(*prefix))
            return false;
    return true;
}

/// Tests whether a token is one of the debug-heap switches, which are matched by prefix.
///
/// @param token Token, switch character included.
/// @return Whether the token starts with an entry of kDebugHeapSwitches.
bool debug_heap_switch(const char* token) noexcept {
    for (const char* prefix : kDebugHeapSwitches)
        if (starts_with_nocase(token, prefix))
            return true;
    return false;
}

/// Copies text into a text field, truncated to kTextBytes - 1 characters.
///
/// @param[out] destination Field receiving the text.
/// @param source Text to copy.
void copy_text(char (&destination)[kTextBytes], const char* source) noexcept {
    std::size_t length = 0;
    while (length + 1 < kTextBytes && source[length] != '\0')
        ++length;
    std::memcpy(destination, source, length);
    destination[length] = '\0';
}

// The tokens of the private copy: strtok with blank and tab as the only
// delimiters, or, for whole words, each word ended by a NUL.
struct Tokens {
    char* next{};
    char* end{};        // past the last word's NUL, for whole words
    bool whole_words{}; // the copy holds words ended by NULs, not one line
    // A copy of the token peek_next_token last looked at. A token of a line
    // still being split is not yet its own string, and a whole word may
    // still have blanks the take trims, so the peek cannot hand out either
    // in place.
    char peek_storage[kMaximumLineBytes]{};

    /// Returns the next token, terminating it in place.
    ///
    /// @param start Text to start splitting, or null to continue after the previous token.
    /// @return The token, or null past the last one.
    char* take(char* start) noexcept {
        char* at = start != nullptr ? start : next;
        if (at == nullptr)
            return nullptr;
        if (whole_words)
            return take_word(at);
        while (*at != '\0' && is_delimiter(*at))
            ++at;
        if (*at == '\0') {
            next = nullptr;
            return nullptr;
        }
        char* token = at;
        while (*at != '\0' && !is_delimiter(*at))
            ++at;
        if (*at != '\0')
            *at++ = '\0';
        next = at;
        return token;
    }

    /// Returns the next whole word from `at` on, its blanks and tabs at either end dropped.
    ///
    /// @param at Start of a word of the copy.
    /// @return The word, or null past the last one; a word of blanks and tabs alone is skipped.
    char* take_word(char* at) noexcept {
        while (at < end) {
            char* word_end = at + std::strlen(at);
            char* const following = word_end + 1;
            while (at < word_end && is_delimiter(*at))
                ++at;
            while (word_end > at && is_delimiter(word_end[-1]))
                *--word_end = '\0';
            if (at < word_end) {
                next = following;
                return at;
            }
            at = following;
        }
        next = nullptr;
        return nullptr;
    }
};

/// Takes the next token for a switch handler (SwitchArguments::next_token).
///
/// @param tokens The parse's Tokens.
/// @return The token, or null past the last one.
const char* take_next_token(void* tokens) noexcept {
    return static_cast<Tokens*>(tokens)->take(nullptr);
}

/// Copies `length` characters of `text` into the peek storage.
///
/// @param parsed The parse's tokens, whose peek storage receives the copy.
/// @param text The characters to copy.
/// @param length How many characters to copy, cut to the storage.
/// @return The copy, NUL-terminated.
const char* store_peek(Tokens& parsed, const char* text, std::size_t length) noexcept {
    if (length >= kMaximumLineBytes)
        length = kMaximumLineBytes - 1;
    std::memcpy(parsed.peek_storage, text, length);
    parsed.peek_storage[length] = '\0';
    return parsed.peek_storage;
}

/// Looks at the next token without taking it (SwitchArguments::peek_token).
///
/// The text matches what take would return: blanks and tabs at either end
/// of a whole word are left out, and a word of only those is skipped. It
/// is a copy, valid until the next peek on these tokens or until the parse
/// returns.
///
/// @param tokens The parse's Tokens.
/// @return The next token, or null when none follows.
const char* peek_next_token(void* tokens) noexcept {
    Tokens& parsed = *static_cast<Tokens*>(tokens);
    if (parsed.whole_words) {
        char* at = parsed.next;
        if (at == nullptr)
            return nullptr;
        while (at < parsed.end) {
            char* word_end = at + std::strlen(at);
            char* const following = word_end + 1;
            while (at < word_end && is_delimiter(*at))
                ++at;
            while (word_end > at && is_delimiter(word_end[-1]))
                --word_end;
            if (at < word_end)
                return store_peek(parsed, at, static_cast<std::size_t>(word_end - at));
            at = following;
        }
        return nullptr;
    }
    const char* at = parsed.next;
    if (at == nullptr)
        return nullptr;
    while (*at != '\0' && is_delimiter(*at))
        ++at;
    if (*at == '\0')
        return nullptr;
    std::size_t length = 0;
    while (at[length] != '\0' && !is_delimiter(at[length]) && length + 1 < kMaximumLineBytes)
        ++length;
    return store_peek(parsed, at, length);
}

/// Offers a switch to the handler, and applies the effects it asks for when it takes it.
///
/// @param handler The handler; null takes nothing.
/// @param letter Lower-case switch letter.
/// @param token The switch's token, switch character included.
/// @param tokens The parse's Tokens, from which the handler may take the next token.
/// @param[in,out] switches Gains skip_intro when the handler asks for it.
/// @return Whether the handler took the switch.
bool offer(
    const SwitchHandler* handler, char letter, char* token, Tokens& tokens, Switches& switches
) noexcept {
    if (handler == nullptr || handler->take == nullptr)
        return false;
    const SwitchArguments arguments{token + 2, take_next_token, &tokens, peek_next_token};
    uint32_t effects = 0;
    if (handler->take(handler->context, letter, &arguments, &effects) == 0)
        return false;
    if ((effects & switch_effect_skip_intro) != 0)
        switches.skip_intro = 1;
    return true;
}

/// Tests whether a lower-case switch letter is one of kReservedSwitches.
///
/// @param letter Switch letter.
/// @return Whether it is reserved; false for '\0'.
bool reserved(char letter) noexcept {
    return letter != '\0' && std::strchr(kReservedSwitches, letter) != nullptr;
}

/// Applies each switch of the copy's tokens in order (see parse).
///
/// @param line The whole line, at the copy's offsets; "-r" takes the rest of it.
/// @param copy The private copy the tokens are ended in.
/// @param tokens Reads the copy's tokens.
/// @param[in,out] switches Receives the switch values.
/// @param handler Takes the letters the engine does not handle; null offers none.
/// @return As parse returns, once the line fits.
Status scan(
    const char* line, char* copy, Tokens& tokens, Switches& switches, const SwitchHandler* handler
) noexcept {
    switches.skip_intro = 0;
    if (handler != nullptr && handler->reset != nullptr)
        handler->reset(handler->context);
    for (char* token = tokens.take(copy); token != nullptr; token = tokens.take(nullptr)) {
        if (token[0] != '-' && token[0] != '/') {
            // A longer word than the field holds keeps its first 63 characters.
            copy_text(switches.language, token);
            continue;
        }
        if (debug_heap_switch(token)) {
            if (switches.developer_switches == 0)
                copy_text(switches.developer_switch, token);
            ++switches.developer_switches;
            continue;
        }
        switch (fold(token[1])) {
        case 'b':
            // A handler may give the word after -b its meaning. Without one
            // the whole token, switch characters included, is compared; of
            // the words compared, only "lock" has an effect.
            if (offer(handler, 'b', token, tokens, switches))
                break;
            if (equal_nocase(token, kLockWord))
                switches.game_options |= kGameLocked;
            break;
        case 'd':
            switches.display_option =
                fold(token[2]) == 'f' ? display_option::f_suffix : display_option::other;
            break;
        case 'f':
            set_forced_disc_drive(switches, 1);
            break;
        case 'l':
            set_l_switch_flag(switches, 0);
            break;
        case 'r': {
            // The rest of the unsplit line, so later tokens belong to it.
            const char* rest = line + (token - copy) + 2;
            while (is_space(*rest))
                ++rest;
            std::memcpy(switches.registration, rest, std::strlen(rest) + 1);
            return Status::register_application;
        }
        case 's':
            switches.playback_suppressed = 1;
            break;
        case 'w':
            // Sounds through the system player also silence the mixer.
            switches.system_sound = 1;
            switches.playback_suppressed = 1;
            break;
        default: {
            const char letter = fold(token[1]);
            if (letter == '\0')
                break;
            if (offer(handler, letter, token, tokens, switches))
                break;
            if (reserved(letter)) {
                switches.unavailable_switch = token[1];
                return Status::unavailable_switch;
            }
            break;
        }
        }
    }
    return Status::run;
}

} // namespace

Status parse(const char* line, Switches& switches, const SwitchHandler* handler) noexcept {
    const size_t length = std::strlen(line);
    if (length >= kMaximumLineBytes)
        return Status::line_too_long;
    char copy[kMaximumLineBytes];
    std::memcpy(copy, line, length + 1);
    Tokens tokens;
    return scan(line, copy, tokens, switches, handler);
}

Status parse(
    std::span<const char* const> words, Switches& switches, const SwitchHandler* handler
) noexcept {
    // The line the words make, with one blank between each word and the next.
    size_t length = words.empty() ? 0 : words.size() - 1;
    for (const char* word : words) {
        length += std::strlen(word);
        if (length >= kMaximumLineBytes)
            return Status::line_too_long;
    }
    // That line, for "-r", and a copy at the same offsets with a NUL after each word.
    char line[kMaximumLineBytes];
    char copy[kMaximumLineBytes];
    size_t at = 0;
    for (size_t index = 0; index < words.size(); ++index) {
        if (index != 0) {
            line[at] = ' ';
            copy[at] = '\0';
            ++at;
        }
        const size_t word_length = std::strlen(words[index]);
        std::memcpy(line + at, words[index], word_length);
        std::memcpy(copy + at, words[index], word_length);
        at += word_length;
    }
    line[at] = '\0';
    copy[at] = '\0';
    Tokens tokens;
    tokens.end = copy + at + 1;
    tokens.whole_words = true;
    return scan(line, copy, tokens, switches, handler);
}

void set_forced_disc_drive(Switches& switches, uint32_t value) noexcept {
    switches.forced_disc_drive = value;
}

void set_l_switch_flag(Switches& switches, uint32_t value) noexcept {
    switches.l_switch_flag = value;
}

const char* launch_language(const Switches& switches) noexcept {
    return switches.language[0] != '\0' ? switches.language : nullptr;
}

} // namespace oa::app::command_line

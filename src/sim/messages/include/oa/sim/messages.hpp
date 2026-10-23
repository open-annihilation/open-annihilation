// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The in-game message log: the 30-line ring in Game.chat_lines that the
// match screen shows, chat lines, wrapped notices, the "player destroyed"
// announcement and the kills board's new leader.
#pragma once

#include "oa/core/world.h"

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace oa::sim::messages {

inline constexpr uint32_t text_bytes = 0x40;
/// The most bytes a formatted chat line ("<Name> text") holds, its end
/// included, where the hooks let it grow past the 200 of 3.1c's
/// (Hooks::shared_chat_line_bytes).
inline constexpr size_t most_chat_line_bytes = 0x100;
// Sender value for lines that come from no player (no arrival sound).
inline constexpr uint8_t sender_none = 10;
// Line kinds (low nibble of the last byte of a line).
inline constexpr uint8_t kind_unit_report = 1; // a unit's speech caption
inline constexpr uint8_t kind_status = 2;      // game speed changes, console notices
inline constexpr uint8_t kind_elimination = 4;
// A chat line another player sent, stored with the sender's Player.index:
// shown under filter 3 even while ScreenChat is off, hidden under filter 2.
inline constexpr uint8_t kind_player_chat = 8;
inline constexpr uint8_t kind_notice = 0x10; // masks to 0 in the stored line
// The bits of MessageLine.kind that hold the kind.
inline constexpr uint8_t kind_mask = 0x0f;
// Bits above the kind in a stored line: its unit was already gone to by the
// reported-unit key, and the line is drawn highlighted (the last one gone to).
inline constexpr uint8_t line_flag_visited = 0x10;
inline constexpr uint8_t line_flag_highlight = 0x20;
// The reported-unit key glides the camera rather than jumping it.
inline constexpr int32_t reported_unit_camera_glide = 1;
// Width a notice is wrapped to, and how far back a break looks for a space.
inline constexpr int32_t wrap_width = 0x3f;
inline constexpr int32_t wrap_lookback = 0xc;
inline constexpr uint32_t elimination_message_count = 3;

inline constexpr const char* sound_message_arrived = "MessageArrived";
inline constexpr const char* elimination_messages[elimination_message_count] = {
    "forces have been obliterated",
    "vermin have been exterminated",
    "forces have gone to a better place",
};
// The line announcing the kills board's new leader: %s takes the player's
// name and %d its board score.
inline constexpr const char* kill_lead_message = "%s has taken the lead with %d kills";
inline constexpr const char* side_name_arm = "Arm";
inline constexpr const char* side_name_core = "Core";

/// The phrase of the line a player says on allying with another,
/// " allied with <name>".
inline constexpr const char* phrase_allied_with = "allied with";
/// The phrase of the line a player says on breaking an alliance,
/// " broke alliance with <name>".
inline constexpr const char* phrase_broke_alliance_with = "broke alliance with";
/// The whole text of the line the battle room says for a player who lacks
/// the host's map.
inline constexpr const char* phrase_missing_map = "does not have this map";

// Game.message_filter every session starts with: kinds 1, 4 and 8, and every
// kind while ScreenChat is on.
inline constexpr int32_t filter_session_start = 3;
inline constexpr uint32_t ticks_per_second = 30;

// Game kind (CampaignFile.kind) whose chat is also recorded.
inline constexpr int32_t game_kind_multiplayer = 3;
// Chat modes that are neither shared nor recorded (see OA_CHAT_MODE_*).
inline constexpr uint8_t chat_mode_local_only = 4;

#pragma pack(push, 1)

// One 0x48-byte line of Game.chat_lines.
struct MessageLine {
    char text[text_bytes]{}; // always terminated at text[0x3f]
    uint32_t tick{};
    uint16_t value{};
    uint8_t sender{}; // Player.index, or sender_none
    uint8_t kind{};   // low nibble
};

#pragma pack(pop)
static_assert(sizeof(MessageLine) == OA_CHAT_LINE_BYTES);

struct Hooks {
    void* context{};
    void (*play_sound)(void* context, const char* name){};
    // Redraw TIMEOUT.GUI when it is on screen.
    void (*refresh_panel)(void* context){};
    const char* (*translate)(void* context, const char* text){};
    // Random numbers; picks the elimination taunt. The application draws them
    // from the message log's own stream, never the match's.
    uint32_t (*random)(void* context){};
    // Hands a chat line to the other players.
    void (*share_chat)(void* context, const char* line){};
    // Records a chat line of a multiplayer game.
    void (*record_chat)(void* context, const char* line){};
    // CampaignFile.kind.
    int32_t (*game_kind)(void* context){};
    // Centre the camera on a map pixel.
    void (*center_camera)(void* context, int32_t x, int32_t y, int32_t glide){};
    /// Names the eliminated player's side (PlayerSetupInfo.side) in an
    /// elimination message; null names side 0 side_name_arm and every other
    /// side side_name_core.
    const char* (*side_name)(void* context, uint8_t side){};
    /// Gives the ending of an elimination message in place of
    /// elimination_messages[index] (index 0 to 2), shown as written whatever
    /// the language; null, or a null return, shows that ending through
    /// `translate`.
    const char* (*elimination_ending)(void* context, uint32_t index){};
    /// Gives the text of the kills board's new-leader line in place of
    /// kill_lead_message, shown whatever the language; null, or a null
    /// return, takes kill_lead_message through `translate`.
    const char* (*kill_lead_text)(void* context){};
    /// Gives the lines a chat line share_chat just shared went out as, when
    /// it went out as more than one (Unicode multiplayer chat cuts a long
    /// line into "<Name> part" records): the line at `index`, or null past
    /// the last. Null, or null at index 0, shows the line as one.
    const char* (*shared_chat_line)(void* context, const char* line, std::size_t index){};
    /// Gives the bytes a formatted chat line holds, its end included, where
    /// it may go out as more than one record (Unicode multiplayer chat cuts
    /// a long line into up to four "<Name> part" records): at most
    /// most_chat_line_bytes. Null, or 0, keeps 3.1c's 200.
    std::size_t (*shared_chat_line_bytes)(void* context){};
    /// Hands a line a computer seat of this machine said to the other
    /// players, from that seat, routed by the mode and targets. Null shares
    /// nothing.
    void (*share_seat_chat)(
        void* context, uint8_t seat, const char* line, uint8_t mode, const uint8_t* targets
    ){};
};

/// Returns the number of lines the ring keeps (Game.text_lines).
///
/// @param game game record
/// @return the TextLines setting; 0 disables the log
[[nodiscard]] int32_t line_capacity(const Game& game) noexcept;
/// Returns one line of the ring.
///
/// @param game game record
/// @param index ring index
/// @return the line, or null past the ring's storage
[[nodiscard]] MessageLine* message_line(Game& game, uint32_t index) noexcept;

/// Returns the seconds a line stays on screen, less one (Game.text_scroll).
///
/// @param game game record
/// @return the TextScroll setting
[[nodiscard]] int32_t text_scroll(const Game& game) noexcept;

/// Stores the log options the settings keep in Game.
///
/// @param[in,out] game game record
/// @param lines TextLines, stored in Game.text_lines
/// @param scroll TextScroll, stored in Game.text_scroll
/// @param filter line filter, stored in Game.message_filter
/// @param screen_chat ScreenChat, stored in Game.screen_chat
void set_log_options(
    Game& game, int32_t lines, int32_t scroll, int32_t filter, uint32_t screen_chat
) noexcept;

/// Translates a text through the hooks.
///
/// @param hooks translation hook
/// @param text text to translate
/// @return hooks.translate's text, or `text` when there is none
[[nodiscard]] const char* translate(const Hooks& hooks, const char* text) noexcept;

/// Empties the log (F12).
///
/// @param[in,out] game game record
void clear_messages(Game& game) noexcept;

/// Drops the oldest line once it has been up for TextScroll + 1 seconds.
///
/// The frame runs it once after its simulated ticks.
///
/// @param[in,out] game game record
/// @return true when a line went
bool expire_oldest_message(Game& game) noexcept;

/// Goes to the unit of the oldest unvisited line that names a live unit.
///
/// Marks the line visited and highlighted and centres the camera on the unit.
///
/// @param[in,out] world message log and units
/// @param hooks camera service
/// @return false when no such line is left
bool track_next_reported_unit(World& world, const Hooks& hooks);

/// Handles the reported-unit key (F3).
///
/// Drops every line's highlight and goes to the next reported unit; once all have been
/// visited, forgets the visits and starts again from the oldest line.
///
/// @param[in,out] world message log and units
/// @param hooks camera service
void cycle_reported_units(World& world, const Hooks& hooks);

/// Appends a line to the log, dropping the oldest when the ring is full.
///
/// @param[in,out] world message log
/// @param text line text; truncated to 63 characters
/// @param kind line kind (kind_*)
/// @param value value stored with the line (the reported unit for unit reports)
/// @param sender Player.index, or sender_none for no arrival sound
/// @param hooks sound and panel services
void post_message(
    World& world, const char* text, uint8_t kind, uint16_t value, uint8_t sender, const Hooks& hooks
);

/// Posts a notice, word-wrapped into 63-character lines.
///
/// One-byte text breaks at a space within the last 12 characters of a line,
/// else at 63. Text with a character of several bytes breaks where the line
/// breaker lets it (oa::base::text::first_row), into lines of at most 63
/// bytes of whole characters.
///
/// @param[in,out] world message log
/// @param text notice text
/// @param hooks sound and panel services
void post_notice(World& world, const char* text, const Hooks& hooks);

/// Announces that a player was destroyed with one of three random taunts.
///
/// The line is the side's name, a space and the ending the random stream
/// picks: Hooks::elimination_ending's, or elimination_messages' through the
/// translation hook.
///
/// @param[in,out] world message log
/// @param player destroyed player
/// @param hooks random stream, endings, translation, sound and panel services
void post_elimination(World& world, const Player& player, const Hooks& hooks);

/// Writes the kills board's new-leader line from its text.
///
/// The text is copied as written, except that its first %s gives way to the
/// player's name, its first %d to the score and each %% to one %. A text
/// without %s or %d shows what it has; a further %s or %d, and any other
/// sequence starting with %, is shown as written. The text is never read as a
/// format.
///
/// @param[out] out the line, always terminated; cut to `size` - 1 characters
/// @param size bytes of `out`; 0 writes nothing
/// @param text the line's text; null writes an empty line
/// @param name the player's name
/// @param score the player's board score
void format_kill_lead(
    char* out, size_t size, const char* text, std::string_view name, int32_t score
) noexcept;

/// Announces that a player took the top row of the kills board.
///
/// Posts a status line from no player: Hooks::kill_lead_text, or
/// kill_lead_message through the translation hook, with the player's name
/// (Player.name) and its board score put in by format_kill_lead.
///
/// @param[in,out] world message log
/// @param leader the player now at the top of the board
/// @param score its board score
/// @param hooks text, translation, sound and panel services
void post_kill_lead(World& world, const Player& leader, int16_t score, const Hooks& hooks);

/// Writes a chat line as this machine shows it.
///
/// The alliance lines and the battle room's missing-map line go between
/// machines in English, as English 3.1c sends them, and each machine shows
/// their phrase in its own language: in a line "<head> text", where the
/// head ends at the line's first "> ", a text that is a space,
/// phrase_allied_with or phrase_broke_alliance_with, a space and a name, or
/// that is phrase_missing_map and nothing more, shows the phrase through
/// `translate`, and the head and the name as they came. A typed line in the
/// same words shows so too. Any other line, and one whose phrase `translate`
/// gives no translation for, is copied as it came. This is a deliberate
/// difference from 3.1c, which sends the phrase in the sender's language.
///
/// @param[out] out the line to show, always terminated; cut between whole
///        UTF-8 characters to `size` - 1 bytes
/// @param size bytes of `out`; 0 writes nothing
/// @param line the chat line, as said or received
/// @param translate gives a phrase in the language shown, or null for none;
///        null shows every line as it came
/// @param context passed to `translate`
void format_shown_chat_line(
    char* out,
    size_t size,
    std::string_view line,
    const char* (*translate)(void* context, const char* text),
    void* context
) noexcept;

/// Formats a chat line, hands it to the other players and adds it to the log.
///
/// The line is "<name->target> text", cut between whole UTF-8 characters to the 199
/// bytes 3.1c's line holds, or to the bytes Hooks::shared_chat_line_bytes gives. It is
/// not shared in the local-only chat mode, and is recorded in a multiplayer game unless
/// sent to chosen players or allies. The log shows the lines a shared line went out as
/// (Hooks::shared_chat_line), else the line, each as format_shown_chat_line shows it
/// through Hooks::translate.
///
/// @param[in,out] world message log and chat mode
/// @param speaker player speaking
/// @param text chat text
/// @param kind line kind
/// @param target recipient name, or null for everyone
/// @param hooks sharing, recording, translation, sound and panel services
void post_chat(
    World& world,
    const Player& speaker,
    const char* text,
    uint8_t kind,
    const char* target,
    const Hooks& hooks
);

/// The speaker, the target and the words of one chat line.
struct ChatLineParts {
    std::string_view speaker{}; ///< name in the head; empty when the line has none
    std::string_view target{};  ///< name after "->"; empty when the head names none
    std::string_view words{};   ///< the line after the head, or the whole line
};

/// Splits a chat line into its speaker, its target and its words.
///
/// A line "<Speaker->Target> words" splits as named. "<Speaker> words" has
/// an empty target. The head ends at the line's first "> ", as
/// format_shown_chat_line reads it. A line without that head is all words,
/// with an empty speaker and target.
///
/// @param line the chat line, as said or received
/// @return the three parts, each a view of `line`
[[nodiscard]] ChatLineParts split_chat_line(std::string_view line) noexcept;

/// Gives the chat mode a head's target names.
///
/// An empty target is everyone. "Allies" and "Enemies" are those modes,
/// ASCII case ignored. Any other target is one chosen player.
///
/// @param target the target from split_chat_line
/// @return OA_CHAT_MODE_EVERYONE, ALLIES, ENEMIES or CHOSEN
[[nodiscard]] uint8_t chat_mode_of_target(std::string_view target) noexcept;

/// Tells whether words are a command: '+' or '.' first, after leading spaces.
///
/// @param words the words, without a "<Name>" head
/// @return true for a command
[[nodiscard]] bool chat_words_command(std::string_view words) noexcept;

/// Tells whether a seat hears a line a speaker said.
///
/// The speaker never hears its own line. Everyone hears mode 0. Allies and
/// enemies go by the speaker's alliance row. Chosen players go by
/// `targets`. The local-only mode reaches nobody. A seat or a speaker
/// outside 0..9 hears nothing.
///
/// @param game the match's players
/// @param speaker slot of the player who said the line
/// @param seat slot asked about
/// @param mode OA_CHAT_MODE_* or chat_mode_local_only
/// @param targets chosen-player slots; null reaches nobody in chosen mode
/// @return true when `seat` hears the line
[[nodiscard]] bool seat_hears(
    const Game& game, uint8_t speaker, uint8_t seat, uint8_t mode, const uint8_t* targets
) noexcept;

/// Ticks of Game.tick a computer seat waits between lines it says.
inline constexpr uint32_t seat_chat_interval_ticks = 30;

/// Formats, shares, records and shows a line a computer seat said.
///
/// The line is "<name->target> text", as post_chat writes it: no target for
/// everyone, "Allies", "Enemies", or the first chosen player's chat name.
/// It is cut as post_chat cuts. It is shared through share_seat_chat unless
/// the mode is local-only, and recorded through record_chat only for a
/// multiplayer line to everyone. The log shows it, through
/// format_shown_chat_line, as a player chat line from the seat, only when
/// the local player hears it. A command is neither shared nor shown.
///
/// @param[in,out] world message log
/// @param seat the computer seat speaking
/// @param words the words, without a "<Name>" head
/// @param mode OA_CHAT_MODE_* or chat_mode_local_only
/// @param targets chosen-player slots; null when the mode names none
/// @param hooks sharing, recording, translation, sound and panel services
void post_seat_chat(
    World& world,
    const Player& seat,
    std::string_view words,
    uint8_t mode,
    const uint8_t* targets,
    const Hooks& hooks
);

} // namespace oa::sim::messages

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// In-game chat: the TALK/TALK2 panel, its target controls and the message
// log drawn over the battlefield.
#pragma once

#include "oa/ui/hud/boundary.hpp"
#include "oa/ui/hud/order_panel.hpp"

#include "oa/core/world.h"
#include "oa/data/campaign/campaign_file.hpp"

#include <cstddef>
#include <cstdint>

namespace oa::ui::hud {

/// frame_flags bit held while the chat panel is open.
inline constexpr uint16_t kFrameChatOpen = 0x0004u;
/// frame_flags bit held while the unit info panel is open.
inline constexpr uint16_t kFrameUnitInfoOpen = 0x0800u;
/// Chat mode that neither broadcasts nor reports a line.
inline constexpr uint8_t kChatModeLocalOnly = 4;
/// Message kind of a chat line.
inline constexpr uint8_t kMessageKindChat = 4;
inline constexpr size_t kChatDraftBytes = 0x81;

/// Text typed into the chat line, kept while the panel switches layout.
struct ChatDraft {
    char text[kChatDraftBytes]{};
    bool initialized{};
};

/// Command and broadcast services behind the chat line.
struct ChatHost {
    void* user{};
    /// Runs a '+' command line (after leading spaces) and returns the chat
    /// mode its echo uses; returns `mode` for plain text.
    uint8_t (*submit_command)(void* user, const char* line, uint8_t mode){};
    /// Formats, broadcasts and logs `text` from `speaker` ("<name->target>").
    void (*post_chat)(
        void* user, const Player& speaker, const char* text, uint8_t kind, const char* target
    ){};
    /// Relists the players in the team chat panel.
    void (*refresh_player_slots)(void* user){};
};

/// Opens TALK.GUI (TALK2.GUI when sending to the team in a multiplayer game) with the draft text and target controls.
///
/// Watchers get no chat, and nothing opens while the unit info panel is up.
/// On success kFrameChatOpen is set, TALK takes the draft, SENDTO shows the
/// team choice; a multiplayer team panel also shows SENDTYPE, relists the
/// players and refreshes the target buttons. TALK gets the keyboard focus.
///
/// @param[in,out] world World whose Game.frame_flags gain kFrameChatOpen.
/// @param session_kind CampaignFile.kind of the session .
/// @param[in,out] draft Text kept between openings; cleared on first use.
/// @param loader Loads the panel layout.
/// @param controls Named controls of the loaded panel.
/// @param host Relists the players for the team panel.
/// @return Whether the panel opened.
bool open_chat_panel(
    World& world,
    oa::data::campaign::SessionKind session_kind,
    ChatDraft& draft,
    const PanelLoader& loader,
    const PanelControls& controls,
    const ChatHost& host
);

/// What the host does after a chat panel click.
enum class ChatClick : uint8_t {
    none,
    closed,          // panel dismissed
    reopened,        // SENDTO switched layout; the panel was reloaded
    clear_selection, // clear the clicked control's selection, then focus TALK
    focus_text,      // focus TALK
};

/// The bytes of typed text send_chat_line reads, its terminating zero among
/// them.
inline constexpr std::size_t typed_text_bytes = 0x100;

/// Sends typed text as TALK's Enter does.
///
/// After leading spaces a '+' line first runs as a command, whose returned
/// chat mode the echo uses; empty text sends nothing. "a:", "e:" and
/// "<digit>:" prefixes (',' and ';' also separate) pick allies, enemies or
/// one player for that one line; a digit naming a player whose Player.player_id
/// is 0 drops the line. The line goes to post_chat as a chat message from the local
/// player, and the chat mode and targets are put back afterwards.
///
/// @param[in,out] world World whose Game.chat_mode and chat_targets change for
///                      the send and are restored after.
/// @param typed Typed text (up to 255 characters are used); null is empty.
/// @param host Command and broadcast services.
void send_chat_line(World& world, const char* typed, const ChatHost& host);

/// Reacts to a click on the chat panel's target controls and sends typed lines.
///
/// A null name closes the panel (clears kFrameChatOpen). LIVEPLYR<n> switches
/// to chosen targets and sets player n's target flag from the control's value;
/// SENDTO stores the team choice, keeps the typed text in `draft` and reloads
/// the panel; SENDTYPE sets the chat mode (values past 4 mean everyone) and
/// refreshes the targets. Enter in TALK sends the line through send_chat_line,
/// clears the draft and turns team chat off. Target clicks play "SmallButton".
///
/// @param[in,out] world World whose chat state and frame_flags change.
/// @param session_kind CampaignFile.kind of the session.
/// @param name Clicked control name, compared case-insensitively; null when the panel closes.
/// @param control Index of the clicked control.
/// @param[in,out] draft Text kept between openings.
/// @param loader Reloads the panel after SENDTO.
/// @param controls Named controls of the loaded panel.
/// @param host Command and broadcast services.
/// @param events Receives the button sound.
/// @return What the host does next.
ChatClick chat_panel_click(
    World& world,
    oa::data::campaign::SessionKind session_kind,
    const char* name,
    int32_t control,
    ChatDraft& draft,
    const PanelLoader& loader,
    const PanelControls& controls,
    const ChatHost& host,
    const HudEvents& events
);

/// Drawing services for the message log.
struct MessageLogSink {
    void* user{};
    /// The step between lines: COMIX's header height (14), or that height
    /// at the size the log's text is drawn at.
    int32_t (*font_height)(void* user){};
    /// The label colour of the next line's text. It shows only when no GUI
    /// font is loaded: the GUI font's glyphs carry their own colours.
    void (*set_color)(void* user, uint8_t color){};
    /// Player logo scaled into the square [x0, x1) x [y0, y1).
    void (*logo)(
        void* user, const Player& player, int32_t x0, int32_t y0, int32_t x1, int32_t y1
    ){};
    /// A line's text with its pen at (x, y), written as the GUI writes gadget
    /// text: in the GUI's font (hattfont12.gaf), or as a COMIX label in the
    /// set colour when the GUI has no font; a line of several rows writes
    /// them a font height apart.
    void (*text)(void* user, const char* text, int32_t x, int32_t y){};
    /// The rows a line's text takes when it is written with its pen at
    /// column x, 1 or more; null writes every line in one row.
    int32_t (*rows)(void* user, const char* text, int32_t x){};
    /// The most rows the lines shown may take: while they take more, the
    /// oldest of them is left out, though the newest always shows. 0 for no
    /// limit.
    int32_t most_rows{};
    /// A line a player typed, beginning `<name> `, is left out. System lines
    /// and the rest of the log still show. Off leaves every line as it was.
    bool hide_recorded_chat{};
};

/// Left edge of the message log.
inline constexpr int32_t kMessageLogLeft = 0x8a;
inline constexpr int32_t kMessageLogTop = 0x34;
/// Message line flag that draws it in the highlight colour.
inline constexpr uint8_t kMessageHighlight = 0x20;

/// Draws the newest message lines that fit the configured line count and pass the display filter.
///
/// Lines run down from kMessageLogTop, one font height apart, oldest first;
/// a line the sink writes in several rows (MessageLogSink::rows) takes a
/// font height for each, and while the lines take more rows than
/// MessageLogSink::most_rows the oldest are left out.
/// Filter 2 hides kind 8; filter 3 shows kinds 1, 4 and 8, or every kind when
/// the show-all option is on. Highlighted lines set UI colour 10, the rest
/// UI colour 15, which the text shows only without a GUI font. A line with a sender starts with that player's logo, 0.8 of a
/// line high, and its text 1.5 logo widths past kMessageLogLeft. Under
/// filter 1 the lines before the first line whose kind is not 2 always show;
/// in 3.1c they may be hidden as well.
///
/// @param world World holding the message ring and the message options.
/// @param sink Drawing callbacks.
/// @quirk Filter 1 hides every line from the first one whose kind is not 2
///        on.
void draw_message_log(const World& world, const MessageLogSink& sink);

/// Destination and source corners of a player logo blit: the logo sequence
/// frame of the player's colour stretched into `rect` shifted down by `lift`.
struct LogoBlit {
    int32_t dest[8]{};   // x0,y0 x1,y0 x1,y1 x0,y1
    int32_t source[8]{}; // same order over the logo frame
};

/// Computes the corners of a player logo blit.
///
/// @param rect Destination square as x0, y0, x1, y1 in screen pixels.
/// @param frame_width Width of the logo frame in pixels.
/// @param frame_height Height of the logo frame in pixels.
/// @param lift Pixels added to both destination y coordinates.
/// @return Destination corners of `rect` shifted by `lift` and the whole logo frame as source.
[[nodiscard]] LogoBlit player_logo_blit(
    const int32_t rect[4], int32_t frame_width, int32_t frame_height, int32_t lift
) noexcept;

} // namespace oa::ui::hud

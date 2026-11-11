// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// Spoken updates. A polite chat line waits a short while, a burst in one
// room collapses, and a muted player is left out. Each announcement is its
// own live node, so a repeated line is raised too. This header does not
// name AccessKit, so a build that leaves the bridge out can include it.
#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace oa::platform::screen_reader {

/// How soon a line is spoken.
enum class Politeness : uint8_t {
    polite,    ///< waits for a pause
    assertive, ///< spoken at once
};

/// Which chat lines are spoken.
enum class SpeakChat : uint8_t {
    off,   ///< no chat, whispers or pages
    pages, ///< pages only
    all,   ///< chat, whispers and pages
};

/// One line ready to speak, after the queue has collapsed a burst.
struct Announcement {
    std::string text; ///< the sentence, in English for a burst
    Politeness politeness{Politeness::polite};
    bool burst{false}; ///< several chat lines became one sentence
    int count{0};      ///< how many lines the burst replaced
    std::string room;  ///< the room a burst names
};

/// One line offered to the queue, before a burst is collapsed.
struct SpokenOffer {
    std::string text; ///< the sentence for a single line
    Politeness politeness{Politeness::polite};
    bool collapsible{false}; ///< a chat line a burst may replace
    std::string room;        ///< the room a collapsible line belongs to
    std::string speaker;     ///< the player, empty when the line has none
};

/// The id of the live region the bridge adds to the tree.
inline constexpr uint64_t live_region_node = 0xA11A11A11ULL;

/// `open-annihilation.access.speak-chat`. Absent, or any text other than
/// `off` and `pages`, is `all`.
inline constexpr const char* speak_chat_key = "open-annihilation.access.speak-chat";

/// `open-annihilation.access.speak-lobby`. `0` is off. Absent, or any other
/// text, is on.
inline constexpr const char* speak_lobby_key = "open-annihilation.access.speak-lobby";

/// `open-annihilation.access.speak-ticker`. `1` is on. Absent, or any other
/// text, is off.
inline constexpr const char* speak_ticker_key = "open-annihilation.access.speak-ticker";

/// Reads speak-chat. Absent, or any text other than `off` and `pages`, is `all`.
///
/// @param text the stored value; empty when the key is absent
/// @return which chat lines are spoken
[[nodiscard]] SpeakChat speak_chat_of(std::string_view text);

/// Reads speak-lobby. `0` is off. Absent, or any other text, is on.
///
/// @param text the stored value; empty when the key is absent
/// @return true when lobby changes are spoken
[[nodiscard]] bool speak_lobby_of(std::string_view text);

/// Reads speak-ticker. `1` is on. Absent, or any other text, is off.
///
/// @param text the stored value; empty when the key is absent
/// @return true when the ticker is spoken
[[nodiscard]] bool speak_ticker_of(std::string_view text);

/// How long a polite chat line waits, in milliseconds, before it is spoken.
///
/// @return the hold
[[nodiscard]] uint64_t line_hold_ms();

/// Queues chat and lobby lines, drops a muted player, and collapses a burst
/// once the hold has passed.
class LineQueue {
  public:

    /// Mutes or hears a player. A muted player's line is dropped and does
    /// not count in a burst. The name is compared as it is written.
    ///
    /// @param name the player
    /// @param muted true to drop that player's lines
    void set_muted(std::string_view name, bool muted);

    /// Reports whether a player is muted.
    ///
    /// @param name the player
    /// @return true when that player's lines are dropped
    [[nodiscard]] bool muted(std::string_view name) const;

    /// Sets the clock that stamps the next line and releases a held burst.
    ///
    /// The game passes `SDL_GetTicks`. A test passes its own clock. A line
    /// is stamped with the clock at the moment it is pushed.
    ///
    /// @param now_ms milliseconds since the clock's start
    void set_now(uint64_t now_ms);

    /// Queues one line. A muted speaker is dropped.
    ///
    /// @param offer the line
    void push(SpokenOffer offer);

    /// Releases the lines the hold allows and keeps the rest.
    ///
    /// A line that is not collapsible, and an assertive line, leave on this
    /// call. Collapsible polite lines in one room wait until
    /// `line_hold_ms` has passed since the oldest of them. Those lines then
    /// become one burst, "{count} new lines in {room}", or the line itself
    /// when there is only one. A later line in that room stays for the next
    /// window. An assertive line does not end the window.
    ///
    /// @return the lines to speak, in order
    [[nodiscard]] std::vector<Announcement> flush();

  private:

    /// One line waiting for the hold.
    struct Held {
        SpokenOffer offer;
        uint64_t at{0}; ///< the clock when the line was pushed
    };

    std::vector<std::string> muted_;
    std::vector<Held> pending_;
    uint64_t now_{0};
};

/// The words the latest live node holds.
struct LiveRegion {
    bool shown{false}; ///< a line is waiting
    std::string text;  ///< the latest line
    Politeness politeness{Politeness::polite};
};

/// One live node. Each announcement has its own id, so a repeated line is
/// a new node.
struct LiveLine {
    uint64_t id{0}; ///< never 0, and never the window
    std::string text;
    Politeness politeness{Politeness::polite};
};

/// Reports the latest live line.
///
/// @return that line, or none when nothing is still shown
[[nodiscard]] LiveRegion live_region();

/// Every live line still shown, oldest first.
///
/// @return the lines
[[nodiscard]] std::vector<LiveLine> live_lines();

/// Forgets queued announcements, the live lines and the next live id. A
/// test calls this so a later tree is not changed by an earlier line. The
/// game does not.
void testing_reset_announcements();

/// Keeps one announcement for speech and adds a new live node for it.
///
/// A repeated sentence is a new node. A node is dropped once a few seconds
/// have passed, or once newer lines have replaced it. Does nothing when no
/// screen reader is running and the screen-reader preference is not `on`:
/// nothing is recorded and the live lines are left as they were. Where
/// Windows speaks because the bridge cannot run, the line is spoken too.
/// The caller posts each line `take_announcements` returns.
///
/// @param text the sentence
/// @param politeness how soon it is spoken
void announce(std::string text, Politeness politeness);

/// Takes the announcements recorded since the last take.
///
/// The live region keeps the latest line. The list is empty afterwards.
///
/// @return the lines, in the order they were announced
[[nodiscard]] std::vector<Announcement> take_announcements();

} // namespace oa::platform::screen_reader

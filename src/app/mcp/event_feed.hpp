// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The event feed (event_feed.cpp): events.subscribe, events.read and the
// resource oa://events. Notes the match watch hands on, and the screen and
// match events the automation watch already learns, share one list of the
// last notes. A client that has subscribed is sent each new note of a kind
// it asked for. Nothing here waits, and nothing here reads the clock on
// the wall: a note's tick is the match's.
#pragma once

#include "events.hpp"
#include "oa/app/match_view.hpp"
#include "oa/app/mcp/session.hpp"

#include <cstdint>
#include <deque>
#include <string>
#include <string_view>
#include <vector>

namespace oa::app {
struct AutomationHost;
struct CheckHost;
} // namespace oa::app

namespace oa::app::mcp {

/// The resource that holds the last notes.
inline constexpr std::string_view events_uri = "oa://events";

/// The notes both sessions share, and what each session has asked to be sent.
class EventFeed {
  public:

    /// One session's subscription. The tools keep this address.
    struct Slot {
        EventFeed* feed{};              ///< the feed
        Session* session{};             ///< the session; null when it has gone
        bool all{};                     ///< every kind
        std::vector<std::string> kinds; ///< the kinds, when all is false
    };

    /// An empty feed.
    EventFeed() = default;

    EventFeed(const EventFeed&) = delete;
    EventFeed& operator=(const EventFeed&) = delete;

    /// The session on standard input.
    ///
    /// @return the slot
    [[nodiscard]] Slot& stdio_slot() noexcept { return stdio_; }

    /// The HTTP session.
    ///
    /// @return the slot
    [[nodiscard]] Slot& http_slot() noexcept { return http_; }

    /// Appends notes the match watch handed on, oldest first.
    ///
    /// @param notes the notes
    void take_notes(const std::vector<MatchNote>& notes);

    /// Learns the screen and match events the automation watch learns, and
    /// appends each one that changed.
    ///
    /// @param check the check host
    /// @param automation the automation host
    /// @param frame the frame being served
    /// @param tick the match's tick, or 0 outside a match
    void gather_screen(
        const CheckHost& check, const AutomationHost& automation, uint64_t frame, uint32_t tick
    );

    /// Tells subscribed clients that oa://events changed, once per batch of notes.
    void flush_resource();

    /// The notes after a seq, as one JSON object: events, oldest and next.
    ///
    /// @param after notes at or before this seq are left out; 0 keeps every note still held
    /// @return the object
    [[nodiscard]] std::string backlog(uint64_t after) const;

    /// The seq of the newest note, or 0 when there are none.
    ///
    /// @return the seq
    [[nodiscard]] uint64_t tail() const noexcept { return next_seq_; }

  private:

    /// One kept note.
    struct Entry {
        uint64_t seq{};   ///< its seq, from 1
        std::string kind; ///< its kind
        std::string json; ///< the object, including seq
    };

    /// Appends one note and sends it to a session that asked for its kind.
    ///
    /// @param kind the kind
    /// @param object the object, without seq
    void append(std::string kind, std::string object);

    /// Sends one note to the sessions that asked for it.
    ///
    /// @param entry the note
    void notify(const Entry& entry);

    Slot stdio_{}; ///< the standard-input session
    Slot http_{};  ///< the HTTP session
    std::deque<Entry> entries_;
    uint64_t next_seq_{};
    bool changed_{};
    automation::EventWatch screen_watch_{};
    uint64_t current_frame_{};
    uint32_t current_tick_{};
    std::string pending_kind_;
};

/// Adds events.subscribe, events.read and oa://events.
///
/// @param[in,out] session the session
/// @param[in,out] feed the feed; it outlives the session's calls
/// @param http true for the HTTP session's subscription
void add_event_tools(Session& session, EventFeed& feed, bool http);

} // namespace oa::app::mcp

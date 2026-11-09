// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The spoken-update queue (announce.hpp).
#include "oa/platform/screen_reader/announce.hpp"

#include "oa/platform/screen_reader.hpp"

#if !defined(OA_SCREEN_READER_COMPILED_OUT)
#include <SDL3/SDL.h>
#endif

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace oa::platform::screen_reader {
namespace {

constexpr uint64_t kLineHoldMs = 1500;
constexpr uint64_t kLiveMs = 4000;
constexpr std::size_t kLiveKeep = 8;

struct ShownLine {
    uint64_t id{0};
    std::string text;
    Politeness politeness{Politeness::polite};
    uint64_t at{0};
};

std::vector<ShownLine> shown;
std::vector<Announcement> recorded;
uint64_t next_live_id = live_region_node;

/// The clock a live line is stamped with.
///
/// @return milliseconds, or 0 when the bridge is compiled out
uint64_t live_clock() {
#if !defined(OA_SCREEN_READER_COMPILED_OUT)
    return SDL_GetTicks();
#else
    return 0;
#endif
}

/// The next live id. It is never 0 and never the window.
///
/// @return the id
uint64_t take_live_id() {
    uint64_t id = next_live_id;
    if (next_live_id == UINT64_MAX)
        next_live_id = live_region_node;
    else
        ++next_live_id;
    if (id == 0 || id == window_node)
        return take_live_id();
    return id;
}

/// Drops a live line that has been shown for a few seconds, and drops the
/// oldest once newer lines have replaced it.
void prune_live() {
    const uint64_t now = live_clock();
    std::vector<ShownLine> kept;
    kept.reserve(shown.size());
    for (ShownLine& line : shown) {
        if (now >= line.at && now - line.at >= kLiveMs)
            continue;
        kept.push_back(std::move(line));
    }
    while (kept.size() > kLiveKeep)
        kept.erase(kept.begin());
    shown = std::move(kept);
}

/// The English sentence a burst of chat lines becomes.
///
/// @param count how many lines
/// @param room the room
/// @return the sentence
std::string burst_sentence(int count, const std::string& room) {
    return std::to_string(count) + " new lines in " + room;
}

} // namespace

uint64_t line_hold_ms() {
    return kLineHoldMs;
}

SpeakChat speak_chat_of(std::string_view text) {
    if (text == "off")
        return SpeakChat::off;
    if (text == "pages")
        return SpeakChat::pages;
    return SpeakChat::all;
}

bool speak_lobby_of(std::string_view text) {
    return text != "0";
}

bool speak_ticker_of(std::string_view text) {
    return text == "1";
}

void LineQueue::set_muted(std::string_view name, bool mute_on) {
    if (name.empty())
        return;
    for (std::string& kept : muted_) {
        if (kept == name) {
            if (!mute_on)
                kept.clear();
            return;
        }
    }
    if (mute_on)
        muted_.emplace_back(name);
}

bool LineQueue::muted(std::string_view name) const {
    for (const std::string& kept : muted_) {
        if (kept == name)
            return true;
    }
    return false;
}

void LineQueue::set_now(uint64_t now_ms) {
    now_ = now_ms;
}

void LineQueue::push(SpokenOffer offer) {
    if (!offer.speaker.empty() && muted(offer.speaker))
        return;
    Held held;
    held.offer = std::move(offer);
    held.at = now_;
    pending_.push_back(std::move(held));
}

std::vector<Announcement> LineQueue::flush() {
    const uint64_t hold = line_hold_ms();
    std::vector<int> group(pending_.size(), -1);
    std::vector<char> emit(pending_.size(), 0);
    for (std::size_t index = 0; index < pending_.size(); ++index) {
        const SpokenOffer& offer = pending_[index].offer;
        if (!offer.collapsible || offer.politeness == Politeness::assertive)
            emit[index] = 1;
    }
    std::vector<std::string> rooms;
    for (const Held& held : pending_) {
        if (!held.offer.collapsible || held.offer.politeness == Politeness::assertive)
            continue;
        bool seen = false;
        for (const std::string& room : rooms) {
            if (room == held.offer.room)
                seen = true;
        }
        if (!seen)
            rooms.push_back(held.offer.room);
    }
    int next_group = 0;
    for (const std::string& room : rooms) {
        bool have_oldest = false;
        uint64_t oldest = 0;
        for (const Held& held : pending_) {
            if (!held.offer.collapsible || held.offer.politeness == Politeness::assertive)
                continue;
            if (held.offer.room != room)
                continue;
            if (!have_oldest || held.at < oldest) {
                oldest = held.at;
                have_oldest = true;
            }
        }
        if (!have_oldest || now_ < oldest || now_ - oldest < hold)
            continue;
        const uint64_t until = oldest + hold;
        const int id = next_group++;
        for (std::size_t index = 0; index < pending_.size(); ++index) {
            const Held& held = pending_[index];
            if (!held.offer.collapsible || held.offer.politeness == Politeness::assertive)
                continue;
            if (held.offer.room == room && held.at < until) {
                emit[index] = 1;
                group[index] = id;
            }
        }
    }
    std::vector<Announcement> spoken;
    std::vector<char> group_done(static_cast<std::size_t>(next_group), 0);
    std::vector<Held> stay;
    for (std::size_t index = 0; index < pending_.size(); ++index) {
        if (emit[index] == 0) {
            stay.push_back(std::move(pending_[index]));
            continue;
        }
        const int id = group[index];
        if (id >= 0) {
            if (group_done[static_cast<std::size_t>(id)] != 0)
                continue;
            group_done[static_cast<std::size_t>(id)] = 1;
            int count = 0;
            for (std::size_t other = 0; other < pending_.size(); ++other) {
                if (group[other] == id)
                    ++count;
            }
            if (count <= 1) {
                Announcement one;
                one.text = pending_[index].offer.text;
                one.politeness = pending_[index].offer.politeness;
                spoken.push_back(std::move(one));
            } else {
                Announcement burst;
                burst.burst = true;
                burst.count = count;
                burst.room = pending_[index].offer.room;
                burst.text = burst_sentence(burst.count, burst.room);
                burst.politeness = Politeness::polite;
                spoken.push_back(std::move(burst));
            }
            continue;
        }
        Announcement one;
        one.text = pending_[index].offer.text;
        one.politeness = pending_[index].offer.politeness;
        spoken.push_back(std::move(one));
    }
    pending_ = std::move(stay);
    std::vector<std::string> still;
    for (std::string& name : muted_) {
        if (!name.empty())
            still.push_back(std::move(name));
    }
    muted_ = std::move(still);
    return spoken;
}

LiveRegion live_region() {
    if (shown.empty())
        return {};
    LiveRegion latest;
    latest.shown = true;
    latest.text = shown.back().text;
    latest.politeness = shown.back().politeness;
    return latest;
}

std::vector<LiveLine> live_lines() {
    prune_live();
    std::vector<LiveLine> lines;
    lines.reserve(shown.size());
    for (const ShownLine& line : shown) {
        LiveLine copy;
        copy.id = line.id;
        copy.text = line.text;
        copy.politeness = line.politeness;
        lines.push_back(std::move(copy));
    }
    return lines;
}

void testing_reset_announcements() {
    shown.clear();
    recorded.clear();
    next_live_id = live_region_node;
}

void announce(std::string text, Politeness politeness) {
    if (text.empty() || !announcements_open())
        return;
    prune_live();
    ShownLine line;
    line.id = take_live_id();
    line.text = text;
    line.politeness = politeness;
    line.at = live_clock();
    shown.push_back(std::move(line));
    while (shown.size() > kLiveKeep)
        shown.erase(shown.begin());
    Announcement kept;
    kept.text = std::move(text);
    kept.politeness = politeness;
    recorded.push_back(std::move(kept));
}

std::vector<Announcement> take_announcements() {
    std::vector<Announcement> taken = std::move(recorded);
    recorded.clear();
    return taken;
}

} // namespace oa::platform::screen_reader

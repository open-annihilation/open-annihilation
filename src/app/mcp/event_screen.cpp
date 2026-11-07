// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The screen and match notes the event feed takes from the automation watch
// (event_feed.hpp). Kept apart from the feed so a test of the list does not
// link the watch.
#include "event_feed.hpp"

#include "oa/app/automation_host.hpp"
#include "oa/app/check_host.hpp"
#include "oa/app/extension.hpp"
#include "oa/formats/json.hpp"

#include "oa/app/automation/screen.hpp"
#include "oa/app/automation/view.hpp"

#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app::mcp {
namespace json = oa::formats::json;

namespace {

/// The notes an extension posts, which the match watch does not learn.
///
/// @param name the kind
/// @return true when events.subscribe accepts it
bool known_extension_kind(std::string_view name) {
    return name == "said" || name == "page" || name == "ticker" || name == "alert" ||
           name == "session" || name == "console";
}

} // namespace

void EventFeed::gather_screen(
    const CheckHost& check, const AutomationHost& automation, uint64_t frame, uint32_t tick
) {
    if (check.screen == nullptr)
        return;
    current_frame_ = frame;
    current_tick_ = tick;
    const std::string shown = automation::screen_name(check.screen(check.context));
    const bool dialogs = automation::screen_event_due(screen_watch_, shown);
    automation::Observed now;
    // events.read keeps every kind, so the feed always reads the focus.
    automation::fill_observed(check, automation, now, dialogs, true);
    automation::EventHooks hooks;
    hooks.context = this;
    hooks.wanted = [](void*, uint32_t) { return true; };
    hooks.begin = [](void* context, std::string_view kind) {
        auto* feed = static_cast<EventFeed*>(context);
        feed->pending_kind_ = std::string(kind);
        json::JsonWriter writer;
        writer.begin_object();
        writer.key("event");
        writer.string(kind);
        writer.key("tick");
        writer.integer(static_cast<int64_t>(feed->current_tick_));
        writer.key("frame");
        writer.integer(static_cast<int64_t>(feed->current_frame_));
        return writer;
    };
    hooks.send = [](void* context, json::JsonWriter& opened) {
        opened.end_object();
        auto* feed = static_cast<EventFeed*>(context);
        feed->append(std::move(feed->pending_kind_), opened.text());
    };
    automation::watch_events(screen_watch_, now, hooks);
    if (automation.context == nullptr)
        return;
    std::vector<std::string> kinds;
    std::vector<std::string> texts;
    take_extension_notes(automation.context, kinds, texts);
    for (std::size_t index = 0; index < kinds.size(); ++index) {
        const std::string& kind = kinds[index];
        if (!known_extension_kind(kind))
            continue;
        json::JsonWriter writer;
        writer.begin_object();
        writer.key("event");
        writer.string(kind);
        writer.key("tick");
        writer.integer(static_cast<int64_t>(current_tick_));
        writer.key("frame");
        writer.integer(static_cast<int64_t>(current_frame_));
        writer.key("text");
        writer.string(index < texts.size() ? texts[index] : std::string{});
        writer.end_object();
        append(kind, writer.text());
    }
}

} // namespace oa::app::mcp

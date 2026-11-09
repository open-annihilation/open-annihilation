// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The event feed (event_feed.hpp).
#include "event_feed.hpp"

#include "oa/formats/json.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace oa::app::mcp {
namespace {

namespace json = oa::formats::json;

/// Every kind, in the order events.subscribe lists them.
///
/// The first twelve are the automation endpoint's. The next eight are the
/// match watch: units, orders and what the local player sees. The last seven
/// are notes an extension posts (a line, a page, the ticker, an alert, the
/// session, a console opening and a spoken update). `focus` is an automation
/// kind, not a match-watch kind and not an extension note.
constexpr std::string_view kKinds[] = {
    "screen",       "player",         "ready",     "chat",         "loaded",     "match",
    "pause",        "speed",          "alliance",  "transfer",     "game_over",  "focus",
    "unit_created", "unit_destroyed", "unit_idle", "unit_damaged", "order_done", "build_done",
    "enemy_seen",   "radar_contact",  "said",      "page",         "ticker",     "alert",
    "session",      "console",        "announce",
};

/// Tells whether a kind is one the feed knows.
///
/// @param name the kind
/// @return true when it is listed
bool known_kind(std::string_view name) {
    for (const std::string_view kind : kKinds)
        if (kind == name)
            return true;
    return false;
}

/// Tells whether a subscription includes a kind.
///
/// @param slot the subscription
/// @param kind the kind
/// @return true when a note of that kind is sent
bool wants(const EventFeed::Slot& slot, std::string_view kind) {
    if (slot.all)
        return true;
    for (const std::string& name : slot.kinds)
        if (name == kind)
            return true;
    return false;
}

/// Inserts a seq into an object that starts with '{'.
///
/// @param seq the seq
/// @param object the object
/// @return the object with seq
std::string with_seq(uint64_t seq, const std::string& object) {
    if (object.empty() || object.front() != '{')
        return object;
    std::string with = "{\"seq\":";
    with += std::to_string(seq);
    with += ',';
    with.append(object, 1, std::string::npos);
    return with;
}

/// A tool failure.
///
/// @param text the sentence
/// @return the answer
ToolReply failed(std::string text) {
    ToolReply reply;
    reply.failed = true;
    reply.text = std::move(text);
    return reply;
}

/// A finished tool.
///
/// @param text the object
/// @return the answer
ToolReply finished(std::string text) {
    ToolReply reply;
    reply.text = std::move(text);
    return reply;
}

/// Reads the subscription a call asked for.
///
/// @param events the events member
/// @param[in,out] slot the subscription; unchanged when the call is refused
/// @return the sentence, or empty when it was taken
std::string bind_kinds(const json::Json& events, EventFeed::Slot& slot) {
    std::vector<std::string> names;
    if (const std::string* text = events.string()) {
        names.push_back(*text);
    } else if (events.type() == json::JsonType::array) {
        for (const json::Json& item : events.elements()) {
            if (item.string() == nullptr)
                return "events are names";
            names.push_back(*item.string());
        }
    } else {
        return "events are names";
    }
    bool all = false;
    for (const std::string& name : names) {
        if (name == "all")
            all = true;
        else if (!known_kind(name))
            return "no such event " + name;
    }
    if (all && names.size() != 1)
        return "all is the only event in that list";
    slot.all = all;
    slot.kinds.clear();
    if (!all) {
        for (const std::string& name : names) {
            bool found = false;
            for (const std::string& kept : slot.kinds)
                if (kept == name)
                    found = true;
            if (!found)
                slot.kinds.push_back(name);
        }
    }
    return {};
}

/// Writes the kinds a subscription takes, in the feed's order.
///
/// @param writer the open object
/// @param slot the subscription
void write_kinds(json::JsonWriter& writer, const EventFeed::Slot& slot) {
    writer.key("events");
    writer.begin_array();
    for (const std::string_view kind : kKinds)
        if (wants(slot, kind))
            writer.string(kind);
    writer.end_array();
}

/// Answers events.subscribe.
///
/// @param context the session's slot
/// @param call the arguments
/// @return the kinds now taken, and the newest seq
ToolReply subscribe(void* context, const ToolCall& call) {
    auto* slot = static_cast<EventFeed::Slot*>(context);
    if (slot == nullptr || slot->feed == nullptr)
        return failed("the event feed is not running");
    const json::Json* events = call.arguments->find("events");
    if (events == nullptr)
        return failed("name the events");
    EventFeed::Slot taken = *slot;
    if (const std::string why = bind_kinds(*events, taken); !why.empty())
        return failed(why);
    slot->all = taken.all;
    slot->kinds = std::move(taken.kinds);
    json::JsonWriter writer;
    writer.begin_object();
    write_kinds(writer, *slot);
    writer.key("next");
    writer.integer(static_cast<int64_t>(slot->feed->tail()));
    writer.end_object();
    return finished(writer.text());
}

/// Answers events.read.
///
/// @param context the session's slot
/// @param call the arguments
/// @return the notes after the seq the call names, or every note still held
ToolReply read_events(void* context, const ToolCall& call) {
    auto* slot = static_cast<EventFeed::Slot*>(context);
    if (slot == nullptr || slot->feed == nullptr)
        return failed("the event feed is not running");
    uint64_t after = 0;
    if (const json::Json* member = call.arguments->find("after")) {
        const std::optional<int64_t> whole = member->integer();
        if (!whole || *whole < 0)
            return failed("after is out of range");
        after = static_cast<uint64_t>(*whole);
    }
    return finished(slot->feed->backlog(after));
}

/// Reads oa://events.
///
/// @param context the feed
/// @return the notes still held
std::string read_resource(void* context) {
    return static_cast<EventFeed*>(context)->backlog(0);
}

} // namespace

void EventFeed::take_notes(const std::vector<MatchNote>& notes) {
    for (const MatchNote& note : notes)
        append(note.kind, note.json);
}

void EventFeed::flush_resource() {
    if (!changed_)
        return;
    changed_ = false;
    if (stdio_.session != nullptr && stdio_.session->initialized())
        stdio_.session->note_updated(events_uri);
    if (http_.session != nullptr && http_.session != stdio_.session && http_.session->initialized())
        http_.session->note_updated(events_uri);
}

std::string EventFeed::backlog(uint64_t after) const {
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("events");
    writer.begin_array();
    for (const Entry& entry : entries_) {
        if (entry.seq > after)
            writer.raw(entry.json);
    }
    writer.end_array();
    writer.key("oldest");
    writer.integer(entries_.empty() ? 0 : static_cast<int64_t>(entries_.front().seq));
    writer.key("next");
    writer.integer(static_cast<int64_t>(next_seq_));
    writer.end_object();
    return writer.text();
}

void EventFeed::append(std::string kind, std::string object) {
    if (entries_.size() == match_note_keep)
        entries_.pop_front();
    ++next_seq_;
    Entry entry;
    entry.seq = next_seq_;
    entry.kind = std::move(kind);
    entry.json = with_seq(entry.seq, object);
    entries_.push_back(std::move(entry));
    changed_ = true;
    notify(entries_.back());
}

void EventFeed::notify(const Entry& entry) {
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("jsonrpc");
    writer.string("2.0");
    writer.key("method");
    writer.string("notifications/oa/event");
    writer.key("params");
    writer.raw(entry.json);
    writer.end_object();
    const std::string line = writer.text();
    const Slot* slots[] = {&stdio_, &http_};
    for (const Slot* slot : slots) {
        if (slot->session == nullptr || !slot->session->initialized() || !wants(*slot, entry.kind))
            continue;
        slot->session->enqueue(line);
    }
}

void add_event_tools(Session& session, EventFeed& feed, bool http) {
    EventFeed::Slot& slot = http ? feed.http_slot() : feed.stdio_slot();
    slot.feed = &feed;
    slot.session = &session;
    slot.all = false;
    slot.kinds.clear();

    Tool subscribe_tool;
    subscribe_tool.name = "events.subscribe";
    subscribe_tool.description =
        "Chooses the events this session is sent from now on. events is all, one kind, "
        "or a list of kinds. An empty list stops them. Notes already kept are not sent "
        "again; events.read returns them. A note's tick is the match's.";
    subscribe_tool.input_schema =
        "{\"type\":\"object\",\"additionalProperties\":false,\"required\":[\"events\"],"
        "\"properties\":{\"events\":{}}}";
    subscribe_tool.reads = true;
    subscribe_tool.context = &slot;
    subscribe_tool.call = subscribe;
    session.add_tool(std::move(subscribe_tool));

    Tool read_tool;
    read_tool.name = "events.read";
    read_tool.description =
        "Reads the events still kept, oldest first, the last 4096. after leaves out "
        "events at or before that seq. Each event carries its kind, its seq and the "
        "match's tick.";
    read_tool.input_schema = "{\"type\":\"object\",\"additionalProperties\":false,\"properties\":{"
                             "\"after\":{\"type\":\"integer\",\"minimum\":0}}}";
    read_tool.reads = true;
    read_tool.context = &slot;
    read_tool.call = read_events;
    session.add_tool(std::move(read_tool));

    Resource resource;
    resource.uri = std::string(events_uri);
    resource.name = "events";
    resource.description = "The last 4096 events, oldest first.";
    resource.mime_type = "application/json";
    resource.context = &feed;
    resource.read = read_resource;
    session.add_resource(std::move(resource));
}

} // namespace oa::app::mcp

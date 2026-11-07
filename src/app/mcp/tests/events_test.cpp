// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// events.subscribe, events.read and oa://events, with notes handed in and no
// running game. A subscription sends only the kinds it names, and only notes
// that arrive after it. The list keeps the last 4096.
#include "event_feed.hpp"

#include "oa/formats/json.hpp"
#include "oa/test/check.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace {

namespace json = oa::formats::json;
namespace mcp = oa::app::mcp;

/// Parses one JSON text. A failure is a failed check and an empty result.
///
/// @param text the text
/// @return the value
std::optional<json::Json> parsed(std::string_view text) {
    json::JsonError failure;
    std::optional<json::Json> value = json::parse_json(text, failure);
    OA_CHECK(value.has_value());
    return value;
}

/// One request.
///
/// @param id the id, as JSON
/// @param method the method
/// @param params the params object, as JSON; empty omits params
/// @return the request
std::string request(std::string_view id, std::string_view method, std::string_view params = {}) {
    std::string text = "{\"jsonrpc\":\"2.0\",\"id\":";
    text.append(id);
    text.append(",\"method\":\"");
    text.append(method);
    text.push_back('"');
    if (!params.empty()) {
        text.append(",\"params\":");
        text.append(params);
    }
    text.push_back('}');
    return text;
}

/// Finishes initialize on a session and installs the event tools.
///
/// The session's address is the one the feed keeps, so the session is not
/// moved afterwards.
///
/// @param[in,out] session the session
/// @param[in,out] feed the feed the tools read; it outlives the session
void open_session(mcp::Session& session, mcp::EventFeed& feed) {
    mcp::add_event_tools(session, feed, false);
    (void)session.receive(request("1", "initialize"));
    (void)session.receive("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
}

/// The one answer a message produced, parsed.
///
/// @param session the session
/// @param message the message
/// @return the answer, or nothing when it was not one JSON object
std::optional<json::Json> only(mcp::Session& session, std::string_view message) {
    const std::vector<std::string> lines = session.receive(message);
    OA_CHECK(lines.size() == 1);
    if (lines.size() != 1)
        return std::nullopt;
    return parsed(lines[0]);
}

/// The text of a tool result.
///
/// @param answer the tools/call answer
/// @return the text, or nothing
std::optional<std::string> tool_text(const json::Json& answer) {
    const json::Json* result = answer.find("result");
    OA_CHECK(result != nullptr);
    if (result == nullptr)
        return std::nullopt;
    const json::Json* content = result->find("content");
    OA_CHECK(content != nullptr && !content->elements().empty());
    if (content == nullptr || content->elements().empty())
        return std::nullopt;
    const json::Json* text = content->elements().front().find("text");
    OA_CHECK(text != nullptr && text->string() != nullptr);
    if (text == nullptr || text->string() == nullptr)
        return std::nullopt;
    return *text->string();
}

/// The text of a successful tool result, parsed.
///
/// @param answer the tools/call answer
/// @return the text's JSON, or nothing
std::optional<json::Json> tool_body(const json::Json& answer) {
    const std::optional<std::string> text = tool_text(answer);
    if (!text)
        return std::nullopt;
    return parsed(*text);
}

/// Tells whether a tool result is a failure.
///
/// @param answer the tools/call answer
/// @return true when isError is set
bool tool_failed(const json::Json& answer) {
    const json::Json* result = answer.find("result");
    if (result == nullptr)
        return false;
    const json::Json* flag = result->find("isError");
    return flag != nullptr && flag->boolean().value_or(false);
}

/// One note, as the match watch would hand it on.
///
/// @param kind the kind
/// @param unit the unit
/// @param tick the match's tick
/// @return the note
oa::app::MatchNote make_note(std::string_view kind, int64_t unit, int64_t tick) {
    json::JsonWriter writer;
    writer.begin_object();
    writer.key("event");
    writer.string(kind);
    writer.key("tick");
    writer.integer(tick);
    writer.key("unit");
    writer.integer(unit);
    writer.key("owner");
    writer.integer(0);
    writer.end_object();
    oa::app::MatchNote note;
    note.tick = static_cast<uint32_t>(tick);
    note.kind = std::string(kind);
    note.json = writer.text();
    return note;
}

/// Hands one note to the feed.
///
/// @param feed the feed
/// @param kind the kind
/// @param unit the unit
/// @param tick the match's tick
void hand(mcp::EventFeed& feed, std::string_view kind, int64_t unit, int64_t tick) {
    std::vector<oa::app::MatchNote> notes;
    notes.push_back(make_note(kind, unit, tick));
    feed.take_notes(notes);
}

/// The events array of a backlog object.
///
/// @param body the object
/// @return the array, or null
const json::Json* events_of(const json::Json& body) {
    return body.find("events");
}

/// A subscription sends later notes of its kinds, and events.read keeps every kind.
void check_subscribe() {
    mcp::EventFeed feed;
    mcp::Session session(mcp::SessionConfig{});
    open_session(session, feed);

    if (const std::optional<json::Json> answer = only(
            session,
            request(
                "2",
                "tools/call",
                "{\"name\":\"events.subscribe\",\"arguments\":{\"events\":[\"nope\"]}}"
            )
        )) {
        OA_CHECK(tool_failed(*answer));
        if (const std::optional<std::string> text = tool_text(*answer))
            OA_CHECK(text->find("no such event nope") != std::string::npos);
    }

    if (const std::optional<json::Json> answer = only(
            session,
            request(
                "8",
                "tools/call",
                "{\"name\":\"events.subscribe\",\"arguments\":{\"events\":[\"focus\"]}}"
            )
        ))
        OA_CHECK(!tool_failed(*answer));

    hand(feed, "unit_idle", 7, 4);
    OA_CHECK(session.pump().empty());

    if (const std::optional<json::Json> answer = only(
            session,
            request(
                "3",
                "tools/call",
                "{\"name\":\"events.subscribe\",\"arguments\":{\"events\":[\"unit_idle\"]}}"
            )
        )) {
        OA_CHECK(!tool_failed(*answer));
        if (const std::optional<json::Json> body = tool_body(*answer)) {
            const json::Json* kinds = events_of(*body);
            OA_CHECK(kinds != nullptr && kinds->elements().size() == 1);
            if (kinds != nullptr && !kinds->elements().empty()) {
                const json::Json& kind = kinds->elements().front();
                OA_CHECK(kind.string() != nullptr && *kind.string() == "unit_idle");
            }
            const json::Json* next = body->find("next");
            OA_CHECK(next != nullptr && next->integer() && *next->integer() == 1);
        }
    }
    OA_CHECK(session.pump().empty());

    hand(feed, "order_done", 7, 9);
    hand(feed, "unit_idle", 7, 10);
    const std::vector<std::string> notes = session.pump();
    OA_CHECK(notes.size() == 1);
    if (notes.size() == 1) {
        OA_CHECK(notes[0].find("notifications/oa/event") != std::string::npos);
        if (const std::optional<json::Json> sent = parsed(notes[0])) {
            const json::Json* params = sent->find("params");
            OA_CHECK(params != nullptr);
            if (params != nullptr) {
                const json::Json* kind = params->find("event");
                const json::Json* unit = params->find("unit");
                const json::Json* tick = params->find("tick");
                const json::Json* seq = params->find("seq");
                OA_CHECK(kind != nullptr && kind->string() && *kind->string() == "unit_idle");
                OA_CHECK(unit != nullptr && unit->integer() && *unit->integer() == 7);
                OA_CHECK(tick != nullptr && tick->integer() && *tick->integer() == 10);
                OA_CHECK(seq != nullptr && seq->integer() && *seq->integer() == 3);
            }
        }
    }

    if (const std::optional<json::Json> answer =
            only(session, request("4", "tools/call", "{\"name\":\"events.read\"}"))) {
        OA_CHECK(!tool_failed(*answer));
        if (const std::optional<json::Json> body = tool_body(*answer)) {
            const json::Json* listed = events_of(*body);
            OA_CHECK(listed != nullptr && listed->elements().size() == 3);
            if (listed != nullptr && listed->elements().size() == 3) {
                const json::Json* first = listed->elements()[0].find("event");
                const json::Json* second = listed->elements()[1].find("event");
                const json::Json* third = listed->elements()[2].find("event");
                OA_CHECK(first != nullptr && first->string() && *first->string() == "unit_idle");
                OA_CHECK(
                    second != nullptr && second->string() && *second->string() == "order_done"
                );
                OA_CHECK(third != nullptr && third->string() && *third->string() == "unit_idle");
            }
            const json::Json* oldest = body->find("oldest");
            const json::Json* next = body->find("next");
            OA_CHECK(oldest != nullptr && oldest->integer() && *oldest->integer() == 1);
            OA_CHECK(next != nullptr && next->integer() && *next->integer() == 3);
        }
    }

    if (const std::optional<json::Json> answer =
            only(session, request("5", "resources/read", "{\"uri\":\"oa://events\"}"))) {
        const json::Json* result = answer->find("result");
        const json::Json* contents = result != nullptr ? result->find("contents") : nullptr;
        OA_CHECK(contents != nullptr && !contents->elements().empty());
        if (contents != nullptr && !contents->elements().empty()) {
            const json::Json* text = contents->elements().front().find("text");
            OA_CHECK(text != nullptr && text->string() != nullptr);
            if (text != nullptr && text->string() != nullptr) {
                if (const std::optional<json::Json> body = parsed(*text->string())) {
                    const json::Json* listed = events_of(*body);
                    OA_CHECK(listed != nullptr && listed->elements().size() == 3);
                }
            }
        }
    }

    (void)only(session, request("6", "resources/subscribe", "{\"uri\":\"oa://events\"}"));
    hand(feed, "order_done", 7, 11);
    feed.flush_resource();
    const std::vector<std::string> updated = session.pump();
    OA_CHECK(updated.size() == 1);
    if (!updated.empty())
        OA_CHECK(updated[0].find("notifications/resources/updated") != std::string::npos);

    if (const std::optional<json::Json> answer = only(
            session,
            request("7", "tools/call", "{\"name\":\"events.read\",\"arguments\":{\"after\":3}}")
        )) {
        OA_CHECK(!tool_failed(*answer));
        if (const std::optional<json::Json> body = tool_body(*answer)) {
            const json::Json* listed = events_of(*body);
            OA_CHECK(listed != nullptr && listed->elements().size() == 1);
            if (listed != nullptr && !listed->elements().empty()) {
                const json::Json* kind = listed->elements().front().find("event");
                OA_CHECK(kind != nullptr && kind->string() && *kind->string() == "order_done");
            }
        }
    }

    if (const std::optional<json::Json> answer = only(
            session,
            request(
                "8",
                "tools/call",
                "{\"name\":\"events.subscribe\",\"arguments\":{\"events\":[\"all\",\"unit_idle\"]}}"
            )
        )) {
        OA_CHECK(tool_failed(*answer));
        if (const std::optional<std::string> text = tool_text(*answer))
            OA_CHECK(text->find("all is the only event") != std::string::npos);
    }

    if (const std::optional<json::Json> answer = only(
            session,
            request(
                "9", "tools/call", "{\"name\":\"events.subscribe\",\"arguments\":{\"events\":[]}}"
            )
        )) {
        OA_CHECK(!tool_failed(*answer));
        if (const std::optional<json::Json> body = tool_body(*answer)) {
            const json::Json* kinds = events_of(*body);
            OA_CHECK(kinds != nullptr && kinds->elements().empty());
        }
    }
    hand(feed, "unit_idle", 7, 12);
    OA_CHECK(session.pump().empty());
}

/// The 4097th note drops the oldest. The resource is that same list.
void check_ring() {
    mcp::EventFeed feed;
    mcp::Session session(mcp::SessionConfig{});
    open_session(session, feed);
    std::vector<oa::app::MatchNote> notes;
    notes.reserve(oa::app::match_note_keep + 1);
    for (int64_t unit = 1; unit <= static_cast<int64_t>(oa::app::match_note_keep) + 1; ++unit)
        notes.push_back(make_note("unit_created", unit, 1));
    feed.take_notes(notes);
    OA_CHECK(session.pump().empty());

    if (const std::optional<json::Json> answer =
            only(session, request("2", "tools/call", "{\"name\":\"events.read\"}"))) {
        OA_CHECK(!tool_failed(*answer));
        if (const std::optional<json::Json> body = tool_body(*answer)) {
            const json::Json* listed = events_of(*body);
            OA_CHECK(listed != nullptr && listed->elements().size() == oa::app::match_note_keep);
            if (listed != nullptr && listed->elements().size() == oa::app::match_note_keep) {
                const json::Json* first = listed->elements().front().find("unit");
                const json::Json* last = listed->elements().back().find("unit");
                const json::Json* seq = listed->elements().front().find("seq");
                OA_CHECK(first != nullptr && first->integer() && *first->integer() == 2);
                OA_CHECK(
                    last != nullptr && last->integer() &&
                    *last->integer() == static_cast<int64_t>(oa::app::match_note_keep) + 1
                );
                OA_CHECK(seq != nullptr && seq->integer() && *seq->integer() == 2);
            }
            const json::Json* oldest = body->find("oldest");
            const json::Json* next = body->find("next");
            OA_CHECK(oldest != nullptr && oldest->integer() && *oldest->integer() == 2);
            OA_CHECK(
                next != nullptr && next->integer() &&
                *next->integer() == static_cast<int64_t>(oa::app::match_note_keep) + 1
            );
        }
    }
}

/// The notes an extension posts are kinds events.subscribe accepts.
void check_extension_kinds() {
    mcp::EventFeed feed;
    mcp::Session session(mcp::SessionConfig{});
    open_session(session, feed);
    if (const std::optional<json::Json> answer = only(
            session,
            request(
                "11",
                "tools/call",
                "{\"name\":\"events.subscribe\",\"arguments\":{\"events\":["
                "\"said\",\"page\",\"ticker\",\"alert\",\"session\",\"console\"]}}"
            )
        )) {
        OA_CHECK(!tool_failed(*answer));
        if (const std::optional<json::Json> body = tool_body(*answer)) {
            const json::Json* kinds = events_of(*body);
            OA_CHECK(kinds != nullptr && kinds->elements().size() == 6);
        }
    }
}

} // namespace

int main() {
    check_subscribe();
    check_extension_kinds();
    check_ring();
    return oa::test::check_exit_status();
}

// SPDX-FileCopyrightText: The Open Annihilation Authors; see COPYRIGHT
// SPDX-License-Identifier: GPL-3.0-only

// The MCP session with no game and no stream: framing, the lifecycle,
// tools/list, tools/call, a schema, a held call's progress, a resource,
// and a tool result that carries a PNG.
#include "oa/app/mcp/session.hpp"
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

/// Checks an error answer's code and message.
///
/// @param answer the answer
/// @param code the code
/// @param message the message
void check_error(const json::Json& answer, int64_t code, std::string_view message) {
    const json::Json* error = answer.find("error");
    OA_CHECK(error != nullptr);
    if (error == nullptr)
        return;
    const json::Json* got = error->find("code");
    OA_CHECK(got != nullptr && got->integer() && *got->integer() == code);
    const json::Json* why = error->find("message");
    OA_CHECK(why != nullptr && why->string() && *why->string() == message);
}

/// The second poll of a steady hold sends no further progress notification.
///
/// @param session the session whose call is held
/// @return true when the next pump is empty and the one after answers
bool session_progress_once(mcp::Session& session);

/// A tool that reads, and one that acts, for the list and for observe.
mcp::ToolReply echo_tool(void* /*context*/, const mcp::ToolCall& call) {
    mcp::ToolReply reply;
    const json::Json* name = call.arguments->find("name");
    reply.text = name != nullptr && name->string() != nullptr ? *name->string() : "";
    return reply;
}

/// Holds until poll reaches the limit, reporting that poll as its progress.
mcp::ToolReply hold_tool(void* context, const mcp::ToolCall& call) {
    const auto* limit = static_cast<const int64_t*>(context);
    if (limit != nullptr && call.poll < *limit) {
        mcp::ToolReply reply;
        reply.held = true;
        reply.progress = call.poll;
        reply.total = *limit;
        reply.progress_message = "working";
        return reply;
    }
    mcp::ToolReply reply;
    reply.text = "done";
    return reply;
}

/// Holds twice with the same progress, then finishes.
mcp::ToolReply steady_tool(void* /*context*/, const mcp::ToolCall& call) {
    if (call.poll < 2) {
        mcp::ToolReply reply;
        reply.held = true;
        reply.progress = 0;
        reply.total = 2;
        return reply;
    }
    mcp::ToolReply reply;
    reply.text = "done";
    return reply;
}

std::string resource_body(void* /*context*/) {
    return "body";
}

/// A picture: the bytes 1, 2, 3, or no PNG when empty is set.
mcp::ToolReply picture_tool(void* /*context*/, const mcp::ToolCall& call) {
    mcp::ToolReply reply;
    reply.text = "picture";
    const json::Json* empty = call.arguments->find("empty");
    if (empty == nullptr || !empty->boolean().value_or(false))
        reply.png = {1, 2, 3};
    return reply;
}

/// Lines, including a line split across appends and one that is too long.
void check_framing() {
    mcp::LineBuffer lines(4);
    mcp::FramedLine line;
    lines.append("ab");
    OA_CHECK(!lines.take(line));
    lines.append("c\r\nok\n");
    OA_CHECK(lines.take(line));
    OA_CHECK(!line.too_long);
    OA_CHECK(line.text == "abc");
    OA_CHECK(lines.take(line));
    OA_CHECK(line.text == "ok");

    mcp::LineBuffer huge(4);
    huge.append("12345\nnext\n");
    OA_CHECK(huge.take(line));
    OA_CHECK(line.too_long);
    OA_CHECK(huge.take(line));
    OA_CHECK(!line.too_long);
    OA_CHECK(line.text == "next");

    mcp::LineBuffer split(4);
    split.append("12345");
    OA_CHECK(split.take(line));
    OA_CHECK(line.too_long);
    split.append("678\ngood\n");
    OA_CHECK(split.take(line));
    OA_CHECK(!line.too_long);
    OA_CHECK(line.text == "good");
}

/// Bad JSON, a batch, an unknown method, and a request before the lifecycle.
void check_rejects() {
    mcp::Session session({});
    const std::vector<std::string> empty = session.receive("   ");
    OA_CHECK(empty.empty());

    if (const std::optional<json::Json> answer = only(session, "{")) {
        check_error(*answer, mcp::parse_error, "not JSON");
        const json::Json* id = answer->find("id");
        OA_CHECK(id != nullptr && id->type() == json::JsonType::null);
    }
    if (const std::optional<json::Json> answer = only(session, "[{\"jsonrpc\":\"2.0\"}]"))
        check_error(*answer, mcp::invalid_request, "JSON-RPC batches are not accepted");
    if (const std::optional<json::Json> answer = only(session, "1"))
        check_error(*answer, mcp::invalid_request, "not a JSON-RPC 2.0 request");
    if (const std::optional<json::Json> answer = only(session, request("1", "nope")))
        check_error(
            *answer, mcp::invalid_request, "notifications/initialized has not been received"
        );
    if (const std::optional<json::Json> answer = only(session, request("9", "tools/list")))
        check_error(
            *answer, mcp::invalid_request, "notifications/initialized has not been received"
        );

    const std::vector<std::string> ping = session.receive(request("3", "ping"));
    OA_CHECK(ping.size() == 1);
    OA_CHECK(ping[0].find("\"id\":3") != std::string::npos);
    if (const std::optional<json::Json> answer = parsed(ping[0])) {
        const json::Json* result = answer->find("result");
        OA_CHECK(result != nullptr && result->type() == json::JsonType::object);
    }
}

/// initialize, the initialized notification, a second initialize, and the id's shape.
void check_lifecycle() {
    mcp::Session session({});
    const std::vector<std::string> started =
        session.receive(request("1", "initialize", "{\"protocolVersion\":\"2025-03-26\"}"));
    OA_CHECK(started.size() == 1);
    OA_CHECK(started[0].find("\"id\":1") != std::string::npos);
    OA_CHECK(started[0].find("\"id\":\"1\"") == std::string::npos);
    OA_CHECK(!session.initialized());
    OA_CHECK(session.negotiated_version() == mcp::protocol_2025_03_26);
    if (const std::optional<json::Json> answer = parsed(started[0])) {
        const json::Json* result = answer->find("result");
        OA_CHECK(result != nullptr);
        if (result != nullptr) {
            const json::Json* version = result->find("protocolVersion");
            OA_CHECK(version != nullptr && version->string() && *version->string() == "2025-03-26");
            const json::Json* info = result->find("serverInfo");
            OA_CHECK(
                info != nullptr && info->find("name") != nullptr && info->find("name")->string() &&
                *info->find("name")->string() == "open-annihilation"
            );
        }
    }

    OA_CHECK(
        session.receive("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}").empty()
    );
    OA_CHECK(session.initialized());

    if (const std::optional<json::Json> answer = only(session, request("2", "initialize")))
        check_error(*answer, mcp::invalid_request, "initialize has already been answered");

    mcp::Session newest({});
    if (const std::optional<json::Json> answer = only(
            newest, request("\"abc\"", "initialize", "{\"protocolVersion\":\"1999-01-01\"}")
        )) {
        const json::Json* result = answer->find("result");
        OA_CHECK(
            result != nullptr && result->find("protocolVersion") != nullptr &&
            result->find("protocolVersion")->string() &&
            *result->find("protocolVersion")->string() == mcp::protocol_2025_06_18
        );
        const json::Json* id = answer->find("id");
        OA_CHECK(id != nullptr && id->string() && *id->string() == "abc");
    }
}

/// tools/list, an unknown tool, a schema, and observe mode.
void check_tools() {
    mcp::Session session({});
    mcp::Tool reads;
    reads.name = "game.status";
    reads.description = "reads";
    reads.input_schema = "{\"type\":\"object\",\"properties\":{\"n\":{\"type\":\"integer\","
                         "\"minimum\":0,\"maximum\":3}},\"required\":[\"n\"],"
                         "\"additionalProperties\":false}";
    reads.reads = true;
    reads.call = echo_tool;
    session.add_tool(std::move(reads));
    mcp::Tool acts;
    acts.name = "game.quit";
    acts.description = "leaves";
    acts.input_schema = "{\"type\":\"object\",\"additionalProperties\":false}";
    acts.reads = false;
    acts.destructive = true;
    acts.call = echo_tool;
    session.add_tool(std::move(acts));

    (void)session.receive(request("1", "initialize"));
    (void)session.receive("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");

    if (const std::optional<json::Json> answer = only(session, request("2", "tools/list"))) {
        const json::Json* result = answer->find("result");
        OA_CHECK(result != nullptr);
        if (result != nullptr) {
            const json::Json* tools = result->find("tools");
            OA_CHECK(tools != nullptr && tools->elements().size() == 2);
        }
    }
    if (const std::optional<json::Json> answer =
            only(session, request("3", "tools/list", "{\"cursor\":\"more\"}")))
        check_error(*answer, mcp::invalid_params, "unknown cursor");
    if (const std::optional<json::Json> answer =
            only(session, request("4", "tools/call", "{\"name\":\"missing\"}")))
        check_error(*answer, mcp::invalid_params, "unknown tool missing");
    if (const std::optional<json::Json> answer =
            only(session, request("5", "tools/call", "{\"name\":\"game.status\"}")))
        check_error(
            *answer, mcp::invalid_params, "arguments do not match the tool's schema: missing n"
        );
    if (const std::optional<json::Json> answer = only(
            session,
            request("6", "tools/call", "{\"name\":\"game.status\",\"arguments\":{\"n\":\"x\"}}")
        ))
        check_error(
            *answer,
            mcp::invalid_params,
            "arguments do not match the tool's schema: n must be a integer"
        );
    if (const std::optional<json::Json> answer = only(
            session,
            request("7", "tools/call", "{\"name\":\"game.status\",\"arguments\":{\"n\":4}}")
        ))
        check_error(
            *answer, mcp::invalid_params, "arguments do not match the tool's schema: n is above 3"
        );
    if (const std::optional<json::Json> answer = only(
            session,
            request(
                "8",
                "tools/call",
                "{\"name\":\"game.status\",\"arguments\":{\"n\":1,\"extra\":true}}"
            )
        ))
        check_error(
            *answer,
            mcp::invalid_params,
            "arguments do not match the tool's schema: unknown argument extra"
        );

    mcp::SessionConfig observe;
    observe.observe = true;
    mcp::Session watching(observe);
    mcp::Tool again = {};
    again.name = "game.status";
    again.input_schema = "{\"type\":\"object\"}";
    again.reads = true;
    again.call = echo_tool;
    watching.add_tool(std::move(again));
    mcp::Tool hidden;
    hidden.name = "game.quit";
    hidden.input_schema = "{\"type\":\"object\"}";
    hidden.reads = false;
    hidden.call = echo_tool;
    watching.add_tool(std::move(hidden));
    (void)watching.receive(request("1", "initialize"));
    (void)watching.receive("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
    if (const std::optional<json::Json> answer = only(watching, request("2", "tools/list"))) {
        const json::Json* result = answer->find("result");
        const json::Json* tools = result != nullptr ? result->find("tools") : nullptr;
        OA_CHECK(tools != nullptr && tools->elements().size() == 1);
        if (tools != nullptr && !tools->elements().empty()) {
            const json::Json* name = tools->elements().front().find("name");
            OA_CHECK(name != nullptr && name->string() && *name->string() == "game.status");
        }
    }
    if (const std::optional<json::Json> answer =
            only(watching, request("3", "tools/call", "{\"name\":\"game.quit\"}")))
        check_error(*answer, mcp::invalid_params, "unknown tool game.quit");
}

/// A held call reports progress, then its result. A steady progress is sent once.
void check_progress() {
    int64_t limit = 2;
    mcp::Session session({});
    mcp::Tool tool;
    tool.name = "hold";
    tool.input_schema = "{\"type\":\"object\"}";
    tool.reads = true;
    tool.context = &limit;
    tool.call = hold_tool;
    session.add_tool(std::move(tool));
    (void)session.receive(request("1", "initialize"));
    (void)session.receive("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");

    const std::vector<std::string> held = session.receive(
        request("2", "tools/call", "{\"name\":\"hold\",\"_meta\":{\"progressToken\":7}}")
    );
    OA_CHECK(held.size() == 1);
    if (held.size() == 1) {
        if (const std::optional<json::Json> note = parsed(held[0])) {
            const json::Json* method = note->find("method");
            OA_CHECK(
                method != nullptr && method->string() &&
                *method->string() == "notifications/progress"
            );
            const json::Json* params = note->find("params");
            OA_CHECK(
                params != nullptr && params->find("progressToken") != nullptr &&
                params->find("progressToken")->integer() &&
                *params->find("progressToken")->integer() == 7
            );
            OA_CHECK(
                params->find("progress") != nullptr && params->find("progress")->integer() &&
                *params->find("progress")->integer() == 0
            );
        }
    }
    const std::vector<std::string> next = session.pump();
    OA_CHECK(next.size() == 1);
    const std::vector<std::string> done = session.pump();
    OA_CHECK(done.size() == 1);
    if (done.size() == 1)
        OA_CHECK(done[0].find("done") != std::string::npos);

    mcp::Session steady_session({});
    mcp::Tool steady;
    steady.name = "steady";
    steady.input_schema = "{\"type\":\"object\"}";
    steady.reads = true;
    steady.call = steady_tool;
    steady_session.add_tool(std::move(steady));
    (void)steady_session.receive(request("1", "initialize"));
    (void)steady_session.receive("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
    const std::vector<std::string> first = steady_session.receive(
        request("2", "tools/call", "{\"name\":\"steady\",\"_meta\":{\"progressToken\":\"tok\"}}")
    );
    OA_CHECK(first.size() == 1);
    OA_CHECK(session_progress_once(steady_session));
}

/// The second poll of a steady hold sends no further progress notification.
///
/// @param session the session whose call is held
/// @return true when the next pump is empty and the one after answers
bool session_progress_once(mcp::Session& session) {
    const std::vector<std::string> quiet = session.pump();
    OA_CHECK(quiet.empty());
    const std::vector<std::string> done = session.pump();
    OA_CHECK(done.size() == 1);
    return quiet.empty() && done.size() == 1;
}

/// resources/list, read, subscribe and a notification when the resource changes.
void check_resources() {
    mcp::Session session({});
    mcp::Resource resource;
    resource.uri = "oa://game/status";
    resource.name = "status";
    resource.description = "the match";
    resource.mime_type = "application/json";
    resource.read = resource_body;
    session.add_resource(std::move(resource));
    (void)session.receive(request("1", "initialize"));
    (void)session.receive("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
    if (const std::optional<json::Json> answer = only(session, request("2", "resources/list"))) {
        const json::Json* result = answer->find("result");
        const json::Json* resources = result != nullptr ? result->find("resources") : nullptr;
        OA_CHECK(resources != nullptr && resources->elements().size() == 1);
    }
    if (const std::optional<json::Json> answer =
            only(session, request("3", "resources/read", "{\"uri\":\"oa://game/status\"}"))) {
        const json::Json* result = answer->find("result");
        const json::Json* contents = result != nullptr ? result->find("contents") : nullptr;
        OA_CHECK(contents != nullptr && !contents->elements().empty());
        if (contents != nullptr && !contents->elements().empty()) {
            const json::Json* text = contents->elements().front().find("text");
            OA_CHECK(text != nullptr && text->string() && *text->string() == "body");
        }
    }
    (void)only(session, request("4", "resources/subscribe", "{\"uri\":\"oa://game/status\"}"));
    session.note_updated("oa://game/status");
    const std::vector<std::string> notes = session.pump();
    OA_CHECK(notes.size() == 1);
    if (notes.size() == 1)
        OA_CHECK(notes[0].find("notifications/resources/updated") != std::string::npos);
    (void)only(session, request("5", "resources/unsubscribe", "{\"uri\":\"oa://game/status\"}"));
    session.note_updated("oa://game/status");
    OA_CHECK(session.pump().empty());
    if (const std::optional<json::Json> answer =
            only(session, request("6", "resources/read", "{\"uri\":\"missing\"}")))
        check_error(*answer, mcp::invalid_params, "unknown resource missing");
}

/// A tool result's text block, and an image block when the tool returns a PNG.
void check_image_content() {
    mcp::Session session({});
    mcp::Tool picture;
    picture.name = "capture.screenshot";
    picture.description = "a picture";
    picture.input_schema = "{\"type\":\"object\",\"properties\":{\"empty\":{\"type\":\"boolean\"}},"
                           "\"additionalProperties\":false}";
    picture.reads = true;
    picture.call = picture_tool;
    session.add_tool(std::move(picture));
    (void)session.receive(request("1", "initialize"));
    (void)session.receive("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}");
    if (const std::optional<json::Json> answer =
            only(session, request("2", "tools/call", "{\"name\":\"capture.screenshot\"}"))) {
        const json::Json* result = answer->find("result");
        const json::Json* content = result != nullptr ? result->find("content") : nullptr;
        OA_CHECK(content != nullptr && content->elements().size() == 2);
        if (content != nullptr && content->elements().size() == 2) {
            const json::Json& text = content->elements()[0];
            const json::Json* text_type = text.find("type");
            const json::Json* text_body = text.find("text");
            OA_CHECK(text_type != nullptr && text_type->string() && *text_type->string() == "text");
            OA_CHECK(
                text_body != nullptr && text_body->string() && *text_body->string() == "picture"
            );
            const json::Json& image = content->elements()[1];
            const json::Json* kind = image.find("type");
            const json::Json* data = image.find("data");
            const json::Json* mime = image.find("mimeType");
            OA_CHECK(kind != nullptr && kind->string() && *kind->string() == "image");
            OA_CHECK(data != nullptr && data->string() && *data->string() == "AQID");
            OA_CHECK(mime != nullptr && mime->string() && *mime->string() == "image/png");
        }
    }
    if (const std::optional<json::Json> answer = only(
            session,
            request(
                "3",
                "tools/call",
                "{\"name\":\"capture.screenshot\",\"arguments\":{\"empty\":true}}"
            )
        )) {
        const json::Json* result = answer->find("result");
        const json::Json* content = result != nullptr ? result->find("content") : nullptr;
        OA_CHECK(content != nullptr && content->elements().size() == 1);
        if (content != nullptr && content->elements().size() == 1) {
            const json::Json* kind = content->elements()[0].find("type");
            OA_CHECK(kind != nullptr && kind->string() && *kind->string() == "text");
        }
    }
}

} // namespace

int main() {
    check_framing();
    check_rejects();
    check_lifecycle();
    check_tools();
    check_progress();
    check_resources();
    check_image_content();
    return oa::test::check_exit_status();
}
